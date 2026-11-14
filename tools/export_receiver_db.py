#!/usr/bin/env python3
"""Export frames and event/diagnostic records from the reference server's SQLite database.

READ-ONLY: the database is opened with mode=ro. If that is impossible (WAL mode needs a
writable -shm next to the file), bms.db and bms.db-wal are COPIED to a temporary directory
and the copy is read. The original files and the running server are never modified.
Python standard library only (runs on the host or inside the receiver container).

    python3 export_receiver_db.py --db /path/to/bms.db --since "2026-11-12 17:00" --out-dir /tmp/bms-export

Writes:
  frames_<since>.csv.gz  time,port,source,id_hex,dlc,data_hex,kind,dir,copy
      time     local SAST (+02:00) ISO 8601 with microseconds
      port     INV / A / B (the device's port field; old firmware sent A only)
      source   inverter / BN (source A) / KNY (source B)
      id_hex   0x351 (standard) or 0x1ABCDEF0 (extended); RTR frames have data_hex "R"
      kind     periodic (Verbose Diagnostics stream) or event (pre-trigger window of a trigger)
      dir      rx, or tx for a frame the device transmitted on port INV (firmware 0.1.7+)
      copy     live, or snapshot: the once-a-minute re-send of an ID's LAST frame in change-only
               mode, with that frame's original timestamp (so one frame can appear several times,
               a few microseconds apart because each batch carries its own epoch offset)
  events_<since>.csv.gz  one row per event batch, and per batch with a diagnostic flag
      (frames dropped on the device, truncated, or sent before NTP sync)
Frames without wall-clock time (a boot that never synced NTP) cannot be placed after --since;
they are counted and reported, not exported.
"""
import argparse
import csv
import datetime
import gzip
import os
import shutil
import sqlite3
import sys
import tempfile

SAST = datetime.timezone(datetime.timedelta(hours=2), "SAST")
F_SYNCED, F_CHANGE_ONLY, F_EVENT, F_TRUNCATED = 1, 2, 4, 8
PORTS = {0: ("INV", "inverter"), 1: ("A", "BN (source A)"), 2: ("B", "KNY (source B)")}


def open_ro(path):
    if not os.path.exists(path):
        sys.exit(f"no database at {path}")
    try:
        db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        db.execute("SELECT count(*) FROM batches").fetchone()
        return db, None
    except sqlite3.Error as e:
        print(f"read-only open failed ({e}); reading a temporary copy instead", file=sys.stderr)
    tmp = tempfile.mkdtemp(prefix="bms-export-")
    for suffix in ("", "-wal"):
        if os.path.exists(path + suffix):
            shutil.copy2(path + suffix, os.path.join(tmp, "bms.db" + suffix))
    return sqlite3.connect(os.path.join(tmp, "bms.db")), tmp


def check_schema(db):
    need = {"batches": {"id", "device", "boot_id", "seq", "flags", "n_frames", "dropped", "epoch_offset_us",
                        "bitrate", "event_id", "event_data", "fw", "received_at"},
            "frames": {"batch_id", "ts_us", "unix_us", "can_id", "ext", "rtr", "dlc", "port", "flags", "data"}}
    for table, cols in need.items():
        have = {r[1] for r in db.execute(f"PRAGMA table_info({table})")}
        if not cols <= have:
            sys.exit(f"table {table}: missing columns {sorted(cols - have)} (not the repo's receiver.py schema)")


def iso(us):
    return datetime.datetime.fromtimestamp(us / 1e6, SAST).isoformat(timespec="microseconds")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--db", required=True, help="path to bms.db (the receiver's BMS_DB)")
    ap.add_argument("--since", default="2026-11-12 17:00", help="local SAST time, 'YYYY-MM-DD HH:MM[:SS]'")
    ap.add_argument("--until", help="optional end, same format")
    ap.add_argument("--out-dir", default=".")
    a = ap.parse_args()
    since = datetime.datetime.fromisoformat(a.since).replace(tzinfo=SAST)
    until = datetime.datetime.fromisoformat(a.until).replace(tzinfo=SAST) if a.until else None
    s_us = int(since.timestamp() * 1e6)
    u_us = int(until.timestamp() * 1e6) if until else 2**62

    db, tmp = open_ro(a.db)
    try:
        check_schema(db)
        os.makedirs(a.out_dir, exist_ok=True)
        tag = since.strftime("%Y%m%d_%H%M")
        f_frames = os.path.join(a.out_dir, f"frames_since_{tag}.csv.gz")
        f_events = os.path.join(a.out_dir, f"events_since_{tag}.csv.gz")

        n = 0
        by_port = {}
        first = last = None
        with gzip.open(f_frames, "wt", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["time", "port", "source", "id_hex", "dlc", "data_hex", "kind", "dir", "copy"])
            cur = db.execute(
                "SELECT f.unix_us, f.port, f.can_id, f.ext, f.rtr, f.dlc, f.data, b.flags, f.flags "
                "FROM frames f JOIN batches b ON b.id = f.batch_id "
                "WHERE f.unix_us >= ? AND f.unix_us < ? ORDER BY f.unix_us, b.seq", (s_us, u_us))
            for ux, port, cid, ext, rtr, dlc, data, bflags, fflags in cur:
                pname, src = PORTS.get(port, (str(port), "?"))
                tx = bool(fflags & 0x02)
                w.writerow([iso(ux), pname, "gateway output" if tx else src,
                            f"0x{cid:08X}" if ext else f"0x{cid:03X}", dlc,
                            "R" if rtr else bytes(data).hex().upper(),
                            "event" if bflags & F_EVENT else "periodic", "tx" if tx else "rx",
                            "snapshot" if fflags & 0x01 else "live"])
                n += 1
                by_port[pname] = by_port.get(pname, 0) + 1
                first = first or ux
                last = ux

        unsynced = db.execute(
            "SELECT count(*) FROM frames f JOIN batches b ON b.id = f.batch_id "
            "WHERE f.unix_us IS NULL AND b.received_at >= ?", (since.timestamp(),)).fetchone()[0]

        ne = 0
        with gzip.open(f_events, "wt", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["time", "received_at", "record", "device", "boot_id", "seq", "fw", "event_id",
                        "event_data", "frames", "dropped", "time_synced", "change_only", "truncated", "bitrate"])
            cur = db.execute(
                "SELECT b.id, b.device, b.boot_id, b.seq, b.flags, b.n_frames, b.dropped, b.bitrate, b.event_id, "
                "b.event_data, b.fw, b.received_at, MIN(f.unix_us) FROM batches b "
                "LEFT JOIN frames f ON f.batch_id = b.id "
                "WHERE ((b.flags & 4) != 0 OR b.dropped > 0 OR (b.flags & 8) != 0 OR (b.flags & 1) = 0) "
                "GROUP BY b.id HAVING COALESCE(MIN(f.unix_us), CAST(b.received_at * 1e6 AS INTEGER)) BETWEEN ? AND ? "
                "ORDER BY b.received_at", (s_us, u_us))
            for (_, dev, boot, seq, flags, nfr, dropped, br, eid, edata, fw, rx, fux) in cur:
                kinds = []
                if flags & F_EVENT:
                    kinds.append("event")
                if dropped:
                    kinds.append("dropped")
                if flags & F_TRUNCATED:
                    kinds.append("truncated")
                if not flags & F_SYNCED:
                    kinds.append("unsynced")
                w.writerow([iso(fux) if fux else "", iso(rx * 1e6), "+".join(kinds), dev, f"{boot:08x}", seq, fw,
                            f"0x{eid & 0x1FFFFFFF:X}" if eid is not None else "",
                            bytes(edata).hex().upper() if edata else "", nfr, dropped,
                            int(bool(flags & F_SYNCED)), int(bool(flags & F_CHANGE_ONLY)),
                            int(bool(flags & F_TRUNCATED)), br if br is not None else ""])
                ne += 1
    finally:
        db.close()
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)

    print(f"{n} frames {by_port} -> {f_frames}")
    if n:
        print(f"  first {iso(first)}, last {iso(last)}")
    print(f"{ne} event/diagnostic records -> {f_events}")
    if unsynced:
        print(f"note: {unsynced} frames received since then have no wall-clock time (not exported)")


if __name__ == "__main__":
    main()
