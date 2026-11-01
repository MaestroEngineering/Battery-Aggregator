#!/usr/bin/env python3
"""BMS aggregator reference server: receives Verbose Diagnostics batches.

Minimal by design: Python standard library only, SQLite storage.

Environment:
  BMS_TOKEN   shared secret; the device sends "Authorization: Bearer <token>" (required)
  BMS_DB      SQLite file (default /data/bms.db)
  BMS_PORT    listen port (default 8080)

Endpoints (all except /api/v1/health need the token):
  POST /api/v1/batches   body = batch (components/diag/include/diag/batch_format.h)
                         200 {"ok":true,"frames":n,"duplicate":false}; duplicates also get 200
  GET  /api/v1/health
  GET  /api/v1/stats     per-device totals
  GET  /api/v1/batches?limit=50[&kind=event]
  GET  /api/v1/export?format=csv|candump|asc[&device=..][&kind=periodic|event|all]
                         [&since=unix_s][&until=unix_s][&batch=<id>]
"""
import datetime
import hmac
import json
import os
import sqlite3
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

TOKEN = os.environ.get("BMS_TOKEN", "")
DB_PATH = os.environ.get("BMS_DB", "/data/bms.db")
PORT = int(os.environ.get("BMS_PORT", "8080"))
MAX_BODY = 2 * 1024 * 1024

HEADER = struct.Struct("<4sHH6sHIIIIqII8s8s")  # 64 bytes
RECORD = struct.Struct("<QIBBBB8s")             # 24 bytes
assert HEADER.size == 64 and RECORD.size == 24

F_SYNCED, F_CHANGE_ONLY, F_EVENT, F_TRUNCATED = 1, 2, 4, 8
ID_EXT, ID_RTR, ID_MASK = 0x80000000, 0x40000000, 0x1FFFFFFF

SCHEMA = """
CREATE TABLE IF NOT EXISTS batches (
  id INTEGER PRIMARY KEY,
  device TEXT NOT NULL, boot_id INTEGER NOT NULL, seq INTEGER NOT NULL,
  flags INTEGER NOT NULL, n_frames INTEGER NOT NULL, dropped INTEGER NOT NULL,
  epoch_offset_us INTEGER NOT NULL, bitrate INTEGER, event_id INTEGER, event_data BLOB,
  fw TEXT, received_at REAL NOT NULL,
  UNIQUE (device, boot_id, seq));
CREATE TABLE IF NOT EXISTS frames (
  batch_id INTEGER NOT NULL REFERENCES batches(id),
  ts_us INTEGER NOT NULL, unix_us INTEGER,
  can_id INTEGER NOT NULL, ext INTEGER NOT NULL, rtr INTEGER NOT NULL,
  dlc INTEGER NOT NULL, port INTEGER NOT NULL, flags INTEGER NOT NULL, data BLOB NOT NULL);
CREATE INDEX IF NOT EXISTS frames_unix ON frames(unix_us);
CREATE INDEX IF NOT EXISTS frames_batch ON frames(batch_id);
CREATE INDEX IF NOT EXISTS frames_id ON frames(can_id);
"""

db_lock = threading.Lock()
db = None


def open_db():
    global db
    os.makedirs(os.path.dirname(os.path.abspath(DB_PATH)), exist_ok=True)
    db = sqlite3.connect(DB_PATH, check_same_thread=False)
    db.executescript(SCHEMA)
    db.execute("PRAGMA journal_mode=WAL")
    db.commit()


class BadBatch(ValueError):
    pass


def parse_batch(body):
    if len(body) < HEADER.size:
        raise BadBatch("body shorter than the 64-byte header")
    (magic, version, hlen, mac, flags, boot_id, seq, n, dropped, offset, bitrate,
     event_id, event_data, fw) = HEADER.unpack_from(body)
    if magic != b"BMSB":
        raise BadBatch("bad magic")
    if version != 1 or hlen != 64:
        raise BadBatch(f"unsupported version {version} / header length {hlen}")
    if len(body) != hlen + n * RECORD.size:
        raise BadBatch(f"length {len(body)} does not match {n} frames")
    hdr = dict(device=mac.hex(":"), flags=flags, boot_id=boot_id, seq=seq, n_frames=n,
               dropped=dropped, epoch_offset_us=offset, bitrate=bitrate,
               event_id=event_id if flags & F_EVENT else None,
               event_data=event_data if flags & F_EVENT else None,
               fw=fw.rstrip(b"\0").decode("ascii", "replace"))
    frames = []
    for i in range(n):
        ts, cid, dlc, port, fflags, _, data = RECORD.unpack_from(body, hlen + i * RECORD.size)
        dlc = min(dlc, 8)
        frames.append((ts, ts + offset if offset else None, cid & ID_MASK, 1 if cid & ID_EXT else 0,
                       1 if cid & ID_RTR else 0, dlc, port, fflags, data[:dlc]))
    return hdr, frames


def store(hdr, frames):
    with db_lock:
        # The device's epoch offset is constant within one boot, so frames sent
        # before NTP synced get wall-clock time from any synced batch of that boot.
        offset = hdr["epoch_offset_us"]
        if not offset:
            row = db.execute("SELECT epoch_offset_us FROM batches WHERE device=? AND boot_id=? AND "
                             "epoch_offset_us != 0 ORDER BY seq DESC LIMIT 1",
                             (hdr["device"], hdr["boot_id"])).fetchone()
            if row:
                frames = [(f[0], f[0] + row[0], *f[2:]) for f in frames]
        cur = db.execute(
            "INSERT OR IGNORE INTO batches (device, boot_id, seq, flags, n_frames, dropped, "
            "epoch_offset_us, bitrate, event_id, event_data, fw, received_at) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
            (hdr["device"], hdr["boot_id"], hdr["seq"], hdr["flags"], hdr["n_frames"],
             hdr["dropped"], hdr["epoch_offset_us"], hdr["bitrate"], hdr["event_id"],
             hdr["event_data"], hdr["fw"], datetime.datetime.now().timestamp()))
        if cur.rowcount == 0:
            return True  # duplicate (device, boot_id, seq): already stored
        bid = cur.lastrowid
        db.executemany(
            "INSERT INTO frames (batch_id, ts_us, unix_us, can_id, ext, rtr, dlc, port, flags, data) "
            "VALUES (?,?,?,?,?,?,?,?,?,?)", [(bid, *f) for f in frames])
        if offset:
            db.execute("UPDATE frames SET unix_us = ts_us + ? WHERE unix_us IS NULL AND batch_id IN "
                       "(SELECT id FROM batches WHERE device=? AND boot_id=?)",
                       (offset, hdr["device"], hdr["boot_id"]))
        db.commit()
        return False


def fmt_id(cid, ext):
    return f"{cid:08X}" if ext else f"{cid:03X}"


def export_rows(q):
    sql = ("SELECT f.ts_us, f.unix_us, f.can_id, f.ext, f.rtr, f.dlc, f.data, b.device, b.boot_id, "
           "b.seq, b.flags FROM frames f JOIN batches b ON b.id = f.batch_id WHERE 1=1")
    args = []
    kind = q.get("kind", ["periodic"])[0]
    if kind == "periodic":
        sql += " AND (b.flags & 4) = 0"
    elif kind == "event":
        sql += " AND (b.flags & 4) != 0"
    if "device" in q:
        sql += " AND b.device = ?"
        args.append(q["device"][0])
    if "batch" in q:
        sql += " AND b.id = ?"
        args.append(int(q["batch"][0]))
    if "since" in q:
        sql += " AND f.unix_us >= ?"
        args.append(int(float(q["since"][0]) * 1e6))
    if "until" in q:
        sql += " AND f.unix_us < ?"
        args.append(int(float(q["until"][0]) * 1e6))
    sql += " ORDER BY b.device, COALESCE(f.unix_us, f.ts_us), b.seq"
    with db_lock:
        return db.execute(sql, args).fetchall()


def export_text(rows, fmt):
    out = []
    if fmt == "csv":
        out.append("unix_time,unix_us,ts_us,device,boot_id,seq,kind,id,ext,rtr,dlc,data")
        for ts, ux, cid, ext, rtr, dlc, data, dev, boot, seq, flags in rows:
            iso = (datetime.datetime.fromtimestamp(ux / 1e6, datetime.timezone.utc).isoformat()
                   if ux else "")
            out.append(f"{iso},{ux or ''},{ts},{dev},{boot},{seq},{'event' if flags & F_EVENT else 'periodic'},"
                       f"0x{fmt_id(cid, ext)},{ext},{rtr},{dlc},{data.hex().upper()}")
    elif fmt == "candump":
        for ts, ux, cid, ext, rtr, dlc, data, *_ in rows:
            t = (ux if ux else ts) / 1e6
            payload = "R" if rtr else data.hex().upper()
            out.append(f"({t:.6f}) can0 {fmt_id(cid, ext)}#{payload}")
    elif fmt == "asc":
        # One time base per file: wall-clock if any frame has it, else time since boot.
        wall = any(r[1] for r in rows)
        skipped = sum(1 for r in rows if wall and not r[1])
        rows = [r for r in rows if r[1]] if wall else rows
        start_us = (rows[0][1] if wall else rows[0][0]) if rows else 0
        start = (datetime.datetime.fromtimestamp(start_us / 1e6) if wall and rows
                 else datetime.datetime.now())
        stamp = start.strftime("%a %b %d %I:%M:%S.%f")[:-3] + start.strftime(" %p %Y").lower()
        out += [f"date {stamp}", "base hex  timestamps absolute", "internal events logged",
                "// version 9.0.0"]
        if skipped:
            out.append(f"// {skipped} frames without wall-clock time omitted")
        out.append(f"Begin Triggerblock {stamp}")
        for ts, ux, cid, ext, rtr, dlc, data, *_ in rows:
            t = ((ux if wall else ts) - start_us) / 1e6
            ident = (f"{cid:X}x" if ext else f"{cid:X}").ljust(15)
            body = "r" if rtr else f"d {dlc} " + " ".join(f"{b:02X}" for b in data)
            out.append(f"{t:11.6f} 1  {ident} Rx   {body}")
        out.append("End TriggerBlock")
    else:
        raise BadBatch("format must be csv, candump or asc")
    return "\n".join(out) + "\n"


class Handler(BaseHTTPRequestHandler):
    server_version = "bms-agg-receiver/1"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    def send(self, code, obj=None, text=None, ctype="application/json", filename=None):
        body = (text if text is not None else json.dumps(obj)).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        if filename:
            self.send_header("Content-Disposition", f'attachment; filename="{filename}"')
        self.end_headers()
        self.wfile.write(body)

    def authorized(self):
        got = self.headers.get("Authorization", "")
        return bool(TOKEN) and hmac.compare_digest(got.encode(), f"Bearer {TOKEN}".encode())

    def do_POST(self):
        url = urlparse(self.path)
        if url.path != "/api/v1/batches":
            return self.send(404, {"ok": False, "error": "not found"})
        if not self.authorized():
            return self.send(401, {"ok": False, "error": "bad or missing token"})
        length = self.headers.get("Content-Length")
        if length is None:
            return self.send(411, {"ok": False, "error": "Content-Length required"})
        length = int(length)
        if length > MAX_BODY:
            return self.send(413, {"ok": False, "error": "batch too large"})
        body = self.rfile.read(length)
        try:
            hdr, frames = parse_batch(body)
        except BadBatch as e:
            return self.send(400, {"ok": False, "error": str(e)})
        dup = store(hdr, frames)
        kind = "event" if hdr["flags"] & F_EVENT else "periodic"
        self.log_message("batch %s boot %08x seq %d: %d frames (%s)%s%s", hdr["device"], hdr["boot_id"],
                         hdr["seq"], len(frames), kind, " DUPLICATE" if dup else "",
                         f", {hdr['dropped']} dropped" if hdr["dropped"] else "")
        self.send(200, {"ok": True, "frames": len(frames), "duplicate": dup})

    def do_GET(self):
        url = urlparse(self.path)
        q = parse_qs(url.query)
        if url.path == "/api/v1/health":
            return self.send(200, {"ok": True})
        if not self.authorized():
            return self.send(401, {"ok": False, "error": "bad or missing token"})
        if url.path == "/api/v1/stats":
            with db_lock:
                rows = db.execute(
                    "SELECT device, COUNT(*), SUM(n_frames), SUM(dropped), SUM((flags & 4) != 0), "
                    "MAX(received_at) FROM batches GROUP BY device").fetchall()
            return self.send(200, [dict(device=d, batches=b, frames=f, dropped=dr, events=ev,
                                        last_received=datetime.datetime.fromtimestamp(t).isoformat())
                                   for d, b, f, dr, ev, t in rows])
        if url.path == "/api/v1/batches":
            limit = min(int(q.get("limit", ["50"])[0]), 1000)
            sql = ("SELECT id, device, boot_id, seq, flags, n_frames, dropped, epoch_offset_us, bitrate, "
                   "event_id, event_data, fw, received_at FROM batches")
            if q.get("kind", [""])[0] == "event":
                sql += " WHERE (flags & 4) != 0"
            sql += " ORDER BY id DESC LIMIT ?"
            with db_lock:
                rows = db.execute(sql, (limit,)).fetchall()
            out = []
            for (i, dev, boot, seq, flags, n, dropped, off, br, eid, edata, fw, rx) in rows:
                out.append(dict(id=i, device=dev, boot_id=f"{boot:08x}", seq=seq, frames=n, dropped=dropped,
                                kind="event" if flags & F_EVENT else "periodic",
                                time_synced=bool(flags & F_SYNCED), change_only=bool(flags & F_CHANGE_ONLY),
                                truncated=bool(flags & F_TRUNCATED), bitrate=br, fw=fw,
                                event_id=f"0x{eid & ID_MASK:X}" if eid is not None else None,
                                event_data=edata.hex().upper() if edata else None,
                                received_at=datetime.datetime.fromtimestamp(rx).isoformat()))
            return self.send(200, out)
        if url.path == "/api/v1/export":
            fmt = q.get("format", ["csv"])[0]
            try:
                text = export_text(export_rows(q), fmt)
            except (BadBatch, ValueError) as e:
                return self.send(400, {"ok": False, "error": str(e)})
            ext = {"csv": "csv", "candump": "log", "asc": "asc"}[fmt]
            return self.send(200, text=text, ctype="text/plain; charset=utf-8",
                             filename=f"bms-agg-export.{ext}")
        self.send(404, {"ok": False, "error": "not found"})


def main():
    if not TOKEN:
        sys.exit("set BMS_TOKEN (the device must send the same token)")
    open_db()
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(f"bms-agg receiver on :{PORT}, db {DB_PATH}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
