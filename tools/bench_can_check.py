#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["python-can>=4.3", "pyserial>=3.5", "requests>=2.31"]
# ///
"""Bench test of the aggregator's three MCP2515 modules with known traffic from a CANable (slcan).

    AGG_PASSWORD=... uv run tools/bench_can_check.py --bench [--port /dev/ttyACM0] [--bitrate 250000]
    AGG_PASSWORD=... uv run tools/bench_can_check.py --bench --tests 4      # map the INT lines
    AGG_PASSWORD=... uv run tools/bench_can_check.py --bench --tests 5,6    # B ACK, INV transmit
    uv run tools/bench_can_check.py --quiet-only [--port ...] [--bitrate ...]   # never transmits

THIS SCRIPT TRANSMITS. Run it only on an isolated bench bus: the CANable plus aggregator ports
and nothing else (tests 1-3: one port; test 4: all three modules on one bus). Never with a
battery or inverter connected. Without --bench it refuses to start.

Safety (enforced in code, see Slcan):
  - Before every transmit session (each port, each bitrate) the CANable is opened listen-only
    ("M1" then "O") for 2 s. If any frame is seen: "bus is not quiet, refusing to transmit".
  - Transmit IDs are limited to 0x100-0x1FF (standard frames). The BN/Pylontech IDs 0x305,
    0x351-0x379 and 0x211-0x217 are refused as well, even though they lie outside that range.
  - At most 100 frames/s, by a single rate-limited send function.

Bench wiring: the bus needs termination at both ends. The CANable's 120 ohm jumper is on, so put
the 120 ohm jumper on the MCP2515 module too (about 60 ohm across CAN-H/CAN-L, power off).

Tests 5 and 6 (0.1.6 guard exceptions; all three modules and the CANable on ONE bench bus):
  5  Port B "acknowledge" mode. Control run with B listen-only (CANable must see ACK errors), then
     B in ACK mode: every frame acknowledged (CANable error register 0, each frame stored exactly
     once on port B, TEC/REC 0), and the CANable receives NO frame at all from the device. Then B
     in ACK mode at a wrong bitrate must fall back to listen-only by itself. Settings restored.
  6  Port INV transmit (the device sends; the CANable only listens or ACKs, it sends nothing).
     Enabling without the typed confirmation must be refused. With the CANable ACKing: only the
     allow-listed IDs with their fixed DLCs, at most 1 Hz per ID, start-up hold (CCL = DCL = 0,
     fail-safe CVL/DVL, because no battery is on the bench). With nothing ACKing: each frame
     appears once (one-shot, no retransmit storm). After disabling: no frames. Settings restored.
  The device's frames in test 6 are the only non-0x1xx IDs on the bench; the script's own
  transmit gate is unchanged.

No-ACK behaviour (tests 1-4): the aggregator never ACKs (listen-only), and there is no other node, so every
frame the CANable sends ends in an ACK error. The script switches automatic retransmission off
("A0"). Then the first ~16 frames end in active error frames until the CANable is error passive,
and after that each frame is seen exactly once. The script sends those warm-up frames (ID 0x1FF)
before every test window. If A0 does not take effect, the CANable retransmits its first frame
forever and frames repeat. Every check therefore uses set membership of unique IDs and payloads,
never exact counts.

CANable firmware (canable2 slcan, github.com/normaldotcom/canable2, checked 2026-11-05 on
version "16e7497-dirty"):
  - Acknowledges NOTHING: O, C, S<n>, M<n>, A<n> and t frames get no "\r", no BEL, no "z".
  - Answers only V (version line) and E ("CANable Error Register: <hex>", no trailing CR).
    The script uses V as the liveness check and E as the only state readback.
  - There is no "L" command (ignored). Listen-only should be M1, set before O. No command reports
    open/closed, mode or bitrate.
  - MEASURED 2026-11-10 (bench, 0.1.6): after C, S5, M1, O the CANable still ACKNOWLEDGES frames
    (the device's port INV sent 30 frames, 0 failed, TEC 0; with the CANable closed: 30 failed,
    TEC 128). So M1 is NOT silent on this firmware: the quiet check sends no data frame but does
    drive ACK bits, and E stayed 0 after unacknowledged transmits too (it does not show ACK
    errors). Tests 5 and 6 therefore use the device's own counters and the tap (port A, a real
    listen-only MCP2515) as observers, and close the CANable (C) when nothing may ACK.
  - A bare "\r" re-runs the previous command (the line buffer is not cleared), so a stray CR
    after a transmit would send that frame again. The script never sends an empty line or "\n",
    sends only whitelisted commands (C O S4-S6 M0 M1 A0 V E), and always sends 8 data bytes (the
    parser would read stale bytes past a short frame). "X" falls through to a transmit: never sent.
  Consequence: the CAN state cannot be verified on the CANable itself. The script prints at the
  end what was verified and what was assumed. The quiet check (listen-only, 2 s: no frames and
  error register 0) cannot tell a quiet bus from a channel that never opened. The end-to-end
  evidence is test 1: frames that the aggregator receives prove the CANable opened, at that
  bitrate.

Payload of a test frame: [tag hi, tag lo, test no, counter (4 bytes, big-endian), 0xA5]. The tag
is random per run; ID = 0x100 + counter % 0xFF (0x100-0x1FE).

Aggregator endpoints used (all JSON unless noted; every POST carries "X-BMS-Request: 1"):
  POST /api/login            {"password": ...} -> session cookie (bms_sid; never printed)
  GET  /api/can/status       tap state/bitrate, probe {running, done, bitrate, age_s, ports[]},
                             detect {running, results[], suggestion, note}
  POST /api/can/probe        "Check modules": all three ports, 1 s listen-only each (~4 s)
  POST /api/can/detect       "Detect bitrate": 125k/250k/500k, 1.5 s each (tap must be enabled)
  POST /api/can/intmap       "Map INT pins" (test 4): each module alone in listen-only, INT GPIO
                             found from test frames 0x100-0x1FF; status under "intmap"
  GET  /api/can/buffer       the RAM ring of recent frames, binary BMSF (no CSV endpoint exists:
                             the web UI builds CSV in the browser from this same data)
  POST /api/logout

Env: AGG_HOST (default 192.168.0.59), AGG_PASSWORD (required; environment only).
Exit code: 0 if every selected test passed, 1 on any FAIL, 2 on a usage or safety abort.
"""
from __future__ import annotations

import argparse
import os
import random
import re
import struct
import sys
import time

import can  # python-can: Message type only (its SlcanBus can't send M1 or A0)
import requests
import serial

ALLOWED_IDS = range(0x100, 0x200)
FORBIDDEN_IDS = {0x305, *range(0x351, 0x37A), *range(0x211, 0x218)}
MAX_FPS = 100
SLCAN_RATE = {125000: "S4", 250000: "S5", 500000: "S6"}
PORT_NAMES = ["INV", "A", "B"]
WARMUP_ID = 0x1FF
WARMUP_FRAMES = 24  # >= 16 ACK errors to reach error passive (TEC 128)


class Abort(Exception):
    """Safety or usage abort: exit code 2."""


# ---- slcan -----------------------------------------------------------------------------

class Slcan:
    """canable2 slcan firmware (github.com/normaldotcom/canable2): see "CANable firmware" in the
    header. Commands are fire-and-forget; only V and E answer."""

    COMMANDS = {"C", "O", "M0", "M1", "A0", "V", "E", *SLCAN_RATE.values()}  # nothing else, ever

    def __init__(self, port: str, checks: "Checks"):
        self.ser = serial.Serial(port, 115200, timeout=0)  # non-blocking reads
        self.rx = b""
        self.checks = checks
        self.open_mode = None  # None | "listen" | "normal"
        self.bitrate = None
        self.quiet_ok = False  # set by quiet_check(); cleared on close
        self.last_tx = 0.0
        self.version = self.alive()
        checks.verified(f"CANable answers V: {self.version}")
        self.close()

    def _write(self, line: str):
        # Never a bare "\r" (the firmware re-runs the previous command, e.g. the last transmit),
        # never "\r\n" ("\n" would be stored at the start of the next command).
        if not line or "\r" in line or "\n" in line:
            raise Abort(f"internal: refused serial line {line!r}")
        self.ser.write(line.encode() + b"\r")
        self.ser.flush()

    def cmd(self, c: str):
        """Fire-and-forget command (the firmware sends no ACK/NACK). Input is flushed before, and
        after every command except O: after O, everything that arrives is bus traffic and must be
        seen by the quiet check."""
        if c not in self.COMMANDS:
            raise Abort(f"internal: refused CANable command {c!r}")
        self.ser.reset_input_buffer()
        self.rx = b""
        self._write(c)
        if c != "O":
            time.sleep(0.02)
            self.ser.reset_input_buffer()

    def _query(self, c: str, wait: float = 0.3) -> bytes:
        self._write(c)
        end, buf = time.monotonic() + wait, b""
        while time.monotonic() < end:
            buf += self.ser.read(4096)
            time.sleep(0.01)
        return buf

    def alive(self) -> str:
        """Liveness: V is the one command that always answers."""
        self.ser.reset_input_buffer()
        b = self._query("V")
        txt = b.split(b"\r")[0].decode(errors="replace").strip()
        if not txt:
            raise Abort("no answer from CANable to 'V' (wrong port, or not slcan firmware?)")
        return txt

    def error_register(self) -> tuple[int | None, list[can.Message]]:
        """E -> "CANable Error Register: <hex>" (no trailing CR). Also returns any frames that
        arrived in the same window, so none are lost while open."""
        b = self._query("E")
        m = re.search(rb"CANable Error Register: ([0-9A-Fa-f]+)", b)
        rest = b[: m.start()] + b[m.end():] if m else b
        frames = [f for f in (parse_slcan(x) for x in rest.split(b"\r")) if f]
        return (int(m.group(1), 16) if m else None), frames

    def read_frames(self, duration: float) -> list[can.Message]:
        out, end = [], time.monotonic() + duration
        while True:
            self.rx += self.ser.read(4096)
            *lines, self.rx = self.rx.split(b"\r")
            out += [f for f in (parse_slcan(x) for x in lines) if f]
            if time.monotonic() >= end:
                return out
            time.sleep(0.005)

    def close(self):
        self.cmd("C")
        self.open_mode = None
        self.quiet_ok = False

    def quiet_check(self, bitrate: int):
        """Listen-only for 2 s. Any frame -> Abort. Required before any transmit."""
        self.close()
        self.alive()
        # Configuration before O (firmware README). M1 = silent; there is no "L" command.
        self.cmd(SLCAN_RATE[bitrate])
        self.cmd("M1")
        self.cmd("O")
        self.open_mode, self.bitrate = "listen", bitrate
        seen = self.read_frames(2.0)
        err, more = self.error_register()
        seen += more
        self.close()
        self.alive()  # still responsive after the session
        if seen:
            raise Abort(f"bus is not quiet, refusing to transmit ({len(seen)} frames in 2 s, "
                        f"first id 0x{seen[0].arbitration_id:X})")
        if err is None:
            raise Abort("CANable did not answer E after the listen-only window")
        if err != 0:
            raise Abort(f"CANable error register 0x{err:X} after listening (traffic at another "
                        "bitrate, or wiring/termination fault): refusing to transmit")
        self.checks.verified("quiet check: 0 frames and error register 0 in listen-only")
        self.checks.assumed("S/M1/O were applied; a closed channel would also read as quiet")
        self.checks.verified("M1 is NOT silent on this firmware (measured 2026-11-10): the CANable ACKs while 'listen-only'")
        self.quiet_ok = True
        self.bitrate = bitrate

    def open_tx(self, bitrate: int):
        """Opens for transmit at `bitrate`, straight after a passing quiet check at that rate."""
        if not self.quiet_ok or self.bitrate != bitrate:
            raise Abort("internal: transmit without a quiet check at this bitrate")
        self.cmd(SLCAN_RATE[bitrate])
        self.cmd("M0")
        self.cmd("A0")  # auto-retransmit off: one attempt per frame
        self.cmd("O")
        self.open_mode = "normal"
        self.checks.assumed("S/M0/A0/O were applied (no ACK); confirmed only if the aggregator sees the frames")

    def open_listen(self, bitrate: int):
        """Listen-only (M1: no ACK, no transmit; needs no quiet check because it cannot transmit)."""
        self.close()
        self.cmd(SLCAN_RATE[bitrate])
        self.cmd("M1")
        self.cmd("O")
        self.open_mode = "listen"

    def read_stamped(self, duration: float) -> list[tuple[float, can.Message]]:
        out, end = [], time.monotonic() + duration
        while time.monotonic() < end:
            t = time.monotonic()
            out += [(t, m) for m in self.read_frames(0.02)]
        return out

    def send(self, msg: can.Message):
        """The single transmit gate."""
        aid = msg.arbitration_id
        if (msg.is_extended_id or msg.is_remote_frame or aid not in ALLOWED_IDS or aid in FORBIDDEN_IDS
                or len(msg.data) != 8):  # full length: the parser reads past short frames
            raise Abort(f"internal: refused transmit on id 0x{aid:X}")
        if self.open_mode != "normal" or not self.quiet_ok:
            raise Abort("internal: transmit while not opened after a quiet check")
        wait = self.last_tx + 1.05 / MAX_FPS - time.monotonic()  # 5% margin under the cap
        if wait > 0:
            time.sleep(wait)
        self.last_tx = time.monotonic()
        self._write(f"t{aid:03X}8{bytes(msg.data).hex().upper()}")

    def end_tx(self) -> int | None:
        """Closes after a transmit session; returns the error register read just before closing."""
        err, _ = self.error_register() if self.open_mode == "normal" else (None, [])
        self.close()
        return err


class Checks:
    """What was verified and what had to be assumed (the firmware acknowledges nothing)."""

    def __init__(self):
        self.v: dict[str, None] = {}
        self.a: dict[str, None] = {}

    def verified(self, s):
        self.v.setdefault(s)

    def assumed(self, s):
        self.a.setdefault(s)

    def report(self):
        print("\nCANable checks:")
        for s in self.v:
            print(f"  verified: {s}")
        for s in self.a:
            print(f"  assumed:  {s}")


def parse_slcan(line: bytes) -> can.Message | None:
    try:
        s = line.decode()
        if not s:
            return None
        kind = s[0]
        if kind in "tr":
            aid, n, rest = int(s[1:4], 16), int(s[4], 16), s[5:]
            ext = False
        elif kind in "TR":
            aid, n, rest = int(s[1:9], 16), int(s[9], 16), s[10:]
            ext = True
        else:
            return None
        data = bytes.fromhex(rest[: 2 * n]) if kind in "tT" else b""
        return can.Message(arbitration_id=aid, is_extended_id=ext, is_remote_frame=kind in "rR",
                           dlc=n, data=data)
    except (ValueError, IndexError):
        return None


# ---- aggregator API -----------------------------------------------------------------------

class Agg:
    def __init__(self, host: str, password: str):
        self.base = f"http://{host}"
        self._pw = password
        self.s = requests.Session()
        self.s.headers["X-BMS-Request"] = "1"
        self.login()

    def login(self):
        r = self.s.post(self.base + "/api/login", json={"password": self._pw}, timeout=10)
        if r.status_code != 200:
            raise Abort(f"login failed: HTTP {r.status_code} {self._msg(r)}")

    @staticmethod
    def _msg(r) -> str:
        try:
            return r.json().get("error", "")
        except ValueError:
            return ""

    def req(self, method: str, path: str, **kw):
        for attempt in range(2):
            r = self.s.request(method, self.base + path, timeout=15, **kw)
            if r.status_code == 401 and attempt == 0:
                self.login()
                continue
            return r
        return r

    def get(self, path):
        r = self.req("GET", path)
        r.raise_for_status()
        return r.json()

    def post(self, path, body=None):
        r = self.req("POST", path, json=body if body is not None else {})
        if r.status_code != 200:
            raise RuntimeError(f"POST {path}: HTTP {r.status_code} {self._msg(r)}")

    def post_status(self, path, body) -> tuple[int, str]:
        r = self.req("POST", path, json=body)
        return r.status_code, self._msg(r)

    def port(self, name: str) -> dict:
        return next(p for p in self.get("/api/can/status")["ports"] if p["name"] == name)

    def wait_port(self, name: str, states: set, timeout: float = 5.0) -> dict:
        end = time.monotonic() + timeout
        while True:
            p = self.port(name)
            if p["state"] in states or time.monotonic() > end:
                return p
            time.sleep(0.3)

    def buffer(self) -> tuple[int, list[tuple]]:
        r = self.req("GET", "/api/can/buffer")
        r.raise_for_status()
        b = r.content
        if len(b) < 24 or b[:4] != b"BMSF":
            raise RuntimeError("buffer: not a BMSF file")
        epoch_off = struct.unpack_from("<q", b, 16)[0]
        recs = []
        for off in range(24, len(b) - 23, 24):
            ts, aid, dlc, port, flags, _r, data = struct.unpack_from("<QIBBBB8s", b, off)
            recs.append((ts, aid, dlc, port, flags, data[:dlc]))
        return epoch_off, recs

    def logout(self):
        try:
            self.req("POST", "/api/logout", json={})
        except requests.RequestException:
            pass


# ---- traffic -------------------------------------------------------------------------------

class Traffic:
    def __init__(self, test_no: int):
        self.tag = random.getrandbits(16)
        self.test_no = test_no
        self.n = 0
        self.sent: list[tuple[int, bytes, float]] = []  # (id, data, host unix time)

    def next(self) -> can.Message:
        c = self.n
        self.n += 1
        data = struct.pack(">HBIB", self.tag, self.test_no, c, 0xA5)
        return can.Message(arbitration_id=0x100 + c % 0xFF, is_extended_id=False, data=data)

    def mine(self, data: bytes) -> bool:
        return len(data) == 8 and struct.unpack(">H", data[:2])[0] == self.tag and data[2] == self.test_no


def warm_up(dev: Slcan):
    for i in range(WARMUP_FRAMES):
        dev.send(can.Message(arbitration_id=WARMUP_ID, is_extended_id=False,
                             data=bytes([0x57, 0x41, 0x52, 0x4D, 0, 0, 0, i])))
    time.sleep(0.1)


def send_while(dev: Slcan, tr: Traffic, keep_going, timeout: float):
    end = time.monotonic() + timeout
    while keep_going() and time.monotonic() < end:
        m = tr.next()
        dev.send(m)
        tr.sent.append((m.arbitration_id, bytes(m.data), time.time()))


def fmt_err(e: int | None) -> str:
    return "no answer" if e is None else f"0x{e:X}"


def start_session(dev: Slcan, bitrate: int):
    print(f"  listen-only quiet check at {bitrate // 1000} kbit/s (2 s)...")
    dev.quiet_check(bitrate)
    dev.open_tx(bitrate)
    warm_up(dev)


# ---- tests ---------------------------------------------------------------------------------

class Results:
    def __init__(self):
        self.items = []

    def add(self, name, ok, why):
        self.items.append((name, ok, why))
        print(f"{'PASS' if ok else 'FAIL'}  {name}: {why}")


def wait_status(agg: Agg, key: str, started: float, timeout: float):
    """Waits until status[key] has run and finished after `started` (monotonic)."""
    seen_running = False
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        st = agg.get("/api/can/status")
        part = st[key]
        seen_running |= bool(part["running"])
        if not part["running"]:
            el = time.monotonic() - started
            if key == "probe" and el > 1.0 and part["done"] and part["age_s"] <= el + 1:
                return st
            if key == "detect" and (seen_running or len(part["results"]) == 3):
                return st
        time.sleep(0.3)
    raise RuntimeError(f"{key} did not finish within {timeout:.0f} s")


def test_ports(dev: Slcan, agg: Agg, bitrate: int, res: Results):
    configured = agg.get("/api/settings")["can"]["bitrate"]
    if configured != bitrate:
        for p in PORT_NAMES:
            res.add(f"1 RX port {p}", False, f"Check modules listens at the configured {configured} bit/s; "
                    f"run with --bitrate {configured} or change the CAN bitrate setting")
        return
    for p in PORT_NAMES:
        input(f"\n>>> move CANable to port {p}, press Enter ")
        name = f"1 RX port {p}"
        try:
            start_session(dev, bitrate)
            tr = Traffic(1)
            send_while(dev, tr, lambda: True, 0.5)  # pre-roll: traffic already flowing when listening starts
            t0 = time.monotonic()
            agg.post("/api/can/probe")
            state = {"done": False, "st": None}

            def keep():
                if time.monotonic() - keep.last >= 0.3:
                    keep.last = time.monotonic()
                    pr = agg.get("/api/can/status")["probe"]
                    el = time.monotonic() - t0
                    if el > 1.0 and not pr["running"] and pr["done"] and pr["age_s"] <= el + 1:
                        state["done"] = True
                return not state["done"]
            keep.last = 0.0
            send_while(dev, tr, keep, 12)
            err = dev.end_tx()
            st = wait_status(agg, "probe", t0, 5)
            port = next(x for x in st["probe"]["ports"] if x["name"] == p)
            others = ", ".join(f"{x['name']} {x['frames']}" for x in st["probe"]["ports"] if x["name"] != p)
            if port["note"]:
                res.add(name, False, f"module check failed: {port['note']}")
            elif port["frames"] > 0 and port["errors"] == 0:
                res.add(name, True, f"{port['frames']} frames, 0 errors in 1 s ({len(tr.sent)} sent; "
                        f"other ports: {others}; CANable error register {fmt_err(err)})")
                dev.checks.verified("normal-mode open, bitrate and transmit: the aggregator received the frames")
            else:
                res.add(name, False, f"{port['frames']} frames, {port['errors']} errors "
                        f"(want frames > 0, errors 0; {len(tr.sent)} sent; CANable error register "
                        f"{fmt_err(err)}{'; 0 frames can also mean the CANable never opened' if not port['frames'] else ''})")
        except (RuntimeError, requests.RequestException) as e:
            dev.close()
            res.add(name, False, str(e))


def test_content(dev: Slcan, agg: Agg, bitrate: int, n: int, res: Results):
    name = "2 content on tap port A"
    st = agg.get("/api/can/status")
    if st["state"] != "listen-only":
        return res.add(name, False, f"tap is '{st['state']}' (enable the tap on the CAN page first)")
    if st["bitrate"] != bitrate:
        return res.add(name, False, f"tap runs at {st['bitrate']} bit/s, CANable at {bitrate}")
    input("\n>>> move CANable to port A (the tap), press Enter ")
    try:
        start_session(dev, bitrate)
        tr = Traffic(2)
        while tr.n < n:
            m = tr.next()
            dev.send(m)
            tr.sent.append((m.arbitration_id, bytes(m.data), time.time()))
        time.sleep(0.5)
        err = dev.end_tx()
        print(f"  CANable error register after sending: {fmt_err(err)}")
        epoch_off, recs = agg.buffer()
    except (RuntimeError, requests.RequestException) as e:
        dev.close()
        return res.add(name, False, str(e))
    got = [r for r in recs if r[3] == 1 and tr.mine(r[5])]
    first = {}
    for i, r in enumerate(got):
        first.setdefault((r[1] & 0x1FFFFFFF, r[5]), (i, r[0]))
    want = [(aid, d) for aid, d, _ in tr.sent]
    missing = [w for w in want if w not in first]
    dups = len(got) - len(first)
    print(f"  {len(want)} sent, {len(got)} matching records on port A, {len(first)} unique, {dups} repeats "
          "(repeats are normal: nothing ACKs on this bench)")
    if missing:
        hint = (" (if only the first frames arrived, repeated: A0 did not take effect and the CANable kept"
                " retransmitting; if none arrived: the CANable did not open)") if len(first) < len(want) // 2 else ""
        return res.add(name, False, f"{len(missing)} of {len(want)} frames missing, first: id 0x{missing[0][0]:X} "
                       f"{missing[0][1].hex()}{hint}")
    order = [first[w][0] for w in want]
    if order != sorted(order):
        bad = next(k for k in range(1, len(order)) if order[k] < order[k - 1])
        return res.add(name, False, f"out of order at sent frame #{bad}")
    ts = [first[w][1] for w in want]
    if any(b <= a for a, b in zip(ts, ts[1:])):
        return res.add(name, False, "device timestamps not strictly increasing")
    dev_span, host_span = (ts[-1] - ts[0]) / 1e6, tr.sent[-1][2] - tr.sent[0][2]
    if abs(dev_span - host_span) > 0.1 * host_span + 0.1:
        return res.add(name, False, f"timestamp span {dev_span:.3f} s on the device vs {host_span:.3f} s sent")
    clock = ""
    if epoch_off:
        skew = (ts[0] + epoch_off) / 1e6 - tr.sent[0][2]
        if abs(skew) > 2.0:
            return res.add(name, False, f"device wall clock {skew:+.2f} s off the host (NTP?)")
        clock = f", wall clock {skew * 1000:+.0f} ms vs host"
    else:
        clock = ", device time not NTP-synced (wall clock not checked)"
    res.add(name, True, f"all {len(want)} frames present in order; span {dev_span:.3f} s vs {host_span:.3f} s"
            f"{clock}")


def test_detect(dev: Slcan, agg: Agg, res: Results):
    st = agg.get("/api/can/status")
    if not st["enabled"]:
        for br in SLCAN_RATE:
            res.add(f"3 detect {br // 1000}k", False, "the tap must be enabled for Detect bitrate")
        return
    input("\n>>> CANable on port A (the tap), press Enter ")
    for br in SLCAN_RATE:
        name = f"3 detect {br // 1000}k"
        try:
            print(f"  sending at {br // 1000} kbit/s")
            start_session(dev, br)
            tr = Traffic(3)
            send_while(dev, tr, lambda: True, 0.3)
            t0 = time.monotonic()
            agg.post("/api/can/detect")
            flag = {"running": True}

            def keep():
                if time.monotonic() - keep.last >= 0.3:
                    keep.last = time.monotonic()
                    d = agg.get("/api/can/status")["detect"]
                    flag["running"] = d["running"] or time.monotonic() - t0 < 1.0
                return flag["running"]
            keep.last = 0.0
            send_while(dev, tr, keep, 15)
            dev.end_tx()
            d = wait_status(agg, "detect", t0, 5)["detect"]
            table = ", ".join(f"{r['bitrate'] // 1000}k {r['frames']}/{r['errors']}" for r in d["results"])
            ok = d["suggestion"] == br
            res.add(name, ok, f"suggested {d['suggestion'] // 1000 if d['suggestion'] else 'none'}"
                    f" (frames/errors: {table}; {d['note']})")
        except (RuntimeError, requests.RequestException) as e:
            dev.close()
            res.add(name, False, str(e))


def test_intmap(dev: Slcan, agg: Agg, bitrate: int, res: Results):
    """Test 4: INT-line mapping. Bench: CANable + INV, A and B on ONE bus, nothing else. The device
    never transmits (every module is listen-only or in reset), so nothing ACKs: same as tests 1-3."""
    configured = agg.get("/api/settings")["can"]["bitrate"]
    if configured != bitrate:
        return res.add("4 INT map", False, f"the mapping listens at the configured {configured} bit/s; "
                       f"run with --bitrate {configured}")
    input("\n>>> ONE bench bus: CANable + modules INV, A and B, NO battery or inverter. Press Enter ")
    try:
        start_session(dev, bitrate)  # 2 s quiet check first, then warm-up
        tr = Traffic(4)
        t0 = time.monotonic()
        joined = False
        r = agg.req("POST", "/api/can/intmap", json={})
        if r.status_code == 409:
            if not agg.get("/api/can/status")["intmap"]["running"]:
                dev.end_tx()
                return res.add("4 INT map", False, f"device busy: {agg._msg(r)}")
            joined = True  # started from the web UI or the serial console: just supply frames
            print("  mapping already running on the device: sending test frames for it")
        elif r.status_code != 200:
            dev.end_tx()
            return res.add("4 INT map", False, f"POST /api/can/intmap: HTTP {r.status_code} {agg._msg(r)}")
        state = {"done": False, "last": 0.0, "port": ""}
        while not state["done"] and time.monotonic() - t0 < 90:
            m = tr.next()
            dev.send(m)  # the single transmit gate: 0x100-0x1FF, <= 100 frames/s
            tr.sent.append((m.arbitration_id, bytes(m.data), time.time()))
            time.sleep(0.03)  # ~25 frames/s is plenty: one frame per round
            if time.monotonic() - state["last"] >= 0.5:
                state["last"] = time.monotonic()
                im = agg.get("/api/can/status")["intmap"]
                if im["running"] and im["port"] != state["port"]:
                    state["port"] = im["port"]
                    print(f"  mapping module {im['port']}...")
                el = time.monotonic() - t0
                if not im["running"] and im["done"] and (joined or im["age_s"] <= el + 1):
                    state["done"] = True
        err = dev.end_tx()
        if not state["done"]:
            return res.add("4 INT map", False, "mapping did not finish within 90 s")
        im = agg.get("/api/can/status")["intmap"]
    except (RuntimeError, requests.RequestException) as e:
        dev.close()
        return res.add("4 INT map", False, str(e))
    print(f"  {len(tr.sent)} test frames sent, CANable error register {fmt_err(err)}")
    print(f"  candidates scanned: GPIO {', '.join(map(str, im['candidates']))}")
    print(f"\n  {'module':<7}{'CS':>3}  {'INT found':>9}  {'INT pins.h':>10}  rounds")
    fr = lambda g: str(g) if g >= 0 else ("none" if g == -1 else "several")
    for p in im["ports"]:
        found = str(p["found"]) if p["result"] == "ok" else "-"
        rounds = ",".join(fr(g) for g in p["per_round"])
        print(f"  {p['name']:<7}{p['cs']:>3}  {found:>9}  {p['int_pins_h']:>10}  "
              f"{len(p['per_round'])}/{im['rounds_wanted']} [{rounds}]")
    print()
    differ = []
    for p in im["ports"]:
        name = f"4 INT map {p['name']}"
        if p["result"] == "ok" and p["found"] == p["int_pins_h"]:
            res.add(name, True, f"INT on GPIO{p['found']} in every round, matches pins.h")
        elif p["result"] == "ok":
            differ.append(p)
            res.add(name, False, f"INT on GPIO{p['found']} in every round, pins.h says GPIO{p['int_pins_h']}")
        else:
            res.add(name, False, f"{p['result_text']}: {p['note']}")
            print(f"      {INTMAP_CHECKS.get(p['result'], '')}")
    if differ:
        print("\n  INT wiring differs from pins.h. Proposed mapping (NOT applied; pins.h unchanged):")
        for p in im["ports"]:
            if p["result"] == "ok":
                print(f"    #define BMS_CAN_{p['name']}_INT {p['found']}   // now {p['int_pins_h']}")


INTMAP_CHECKS = {
    "no_spi": "check by hand: module 5 V power; level shifter OE pin high; CS pull-up (10k to 3.3 V); "
              "SCK/MOSI/MISO through the shifter; crystal marking (8.000 MHz expected)",
    "no_frames": "check by hand: CAN-H/CAN-L to this module; termination (~60 ohm across the pair, power off); "
                 "crystal marking (8.000 MHz expected); bench script running",
    "no_int": "check by hand: INT pin to its divider (1k over 2k) to the GPIO; divider ground; "
              "which GPIO it lands on (only the candidates were scanned)",
    "multi_int": "check by hand: INT lines shorted together or dividers sharing a node",
    "inconsistent": "check by hand: loose INT wire or divider joint; run again",
}


INV_ALLOW = {0x351: 8, 0x355: 4, 0x356: 6, 0x359: 7, 0x35C: 2, 0x35E: 8}


def bench_window(agg: Agg, port: int, tr: Traffic) -> tuple[int, int, int]:
    """(sent, unique stored on `port`, repeats) for this run's frames in the device buffer."""
    _, recs = agg.buffer()
    got = [r for r in recs if r[3] == port and tr.mine(r[5])]
    uniq = {(r[1] & 0x1FFFFFFF, r[5]) for r in got}
    want = {(aid, d) for aid, d, _ in tr.sent}
    return len(want), len(uniq & want), len(got) - len(uniq)


def inv_burst(agg: Agg, seconds: float) -> tuple[int, int, int, list]:
    """Port INV transmits for `seconds` (the CANable must be CLOSED: it would ACK otherwise).
    Returns (sent ok, failed, TEC, port-A records of the allow-listed IDs in that window)."""
    p0 = agg.port("INV")
    agg.post("/api/agg/transmit", {"enable": True, "confirm": "TRANSMIT"})
    time.sleep(seconds)
    p1 = agg.port("INV")
    agg.post("/api/agg/transmit", {"enable": False})
    _, recs = agg.buffer()
    seen = [r for r in recs if r[3] == 1 and (r[1] & 0x1FFFFFFF) in INV_ALLOW]
    return p1["tx_frames"] - p0["tx_frames"], p1["tx_failed"] - p0["tx_failed"], p1["tec"], seen


def test_b_ack(dev: Slcan, agg: Agg, bitrate: int, n: int, res: Results):
    """Test 5: port B acknowledges (normal mode) but never sends a data frame."""
    if agg.get("/api/agg/status")["tx"]["enabled"]:
        return res.add("5 B ACK", False, "inverter transmit is on: turn it off first")
    s = agg.get("/api/settings")
    orig, orig_inv = s["can_b"], s["can_inv"]
    wrong = 500000 if bitrate != 500000 else 250000
    try:
        # 5a: B ACK proven with the CANable closed: port INV's frames need an acknowledgement.
        # Control first (B listen-only: nothing ACKs), then B in ACK mode.
        agg.post("/api/settings", {"can_inv": {"enabled": True, "bitrate": bitrate},
                                   "can_b": {"enabled": True, "bitrate": bitrate, "ack": False}})
        agg.wait_port("B", {"listen-only"})
        print(f"  listen-only quiet check at {bitrate // 1000} kbit/s (2 s)...")
        dev.quiet_check(bitrate)  # leaves the CANable closed
        ok_c, fail_c, tec_c, _ = inv_burst(agg, 4)
        agg.post("/api/settings", {"can_b": {"enabled": True, "bitrate": bitrate, "ack": True}})
        p0 = agg.wait_port("B", {"normal (ACK only)"})
        time.sleep(3)  # INV leaves error passive only through successful frames: wait a moment
        ok_a, fail_a, tec_a, _ = inv_burst(agg, 4)
        res.add("5a B acknowledges (INV as sender, CANable closed)",
                fail_c > 0 and ok_c == 0 and ok_a > 0 and fail_a == 0 and p0["state"] == "normal (ACK only)",
                f"B listen-only: {ok_c} acknowledged / {fail_c} not (TEC {tec_c}); "
                f"B ACK mode: {ok_a} acknowledged / {fail_a} not (TEC {tec_a})")
        # 5b: CANable sends 0x100-0x1FF; B ACKs; each frame stored exactly once, no retransmissions.
        p0 = agg.port("B")
        blocked0 = agg.get("/api/can/status")["blocked_cmds"]
        start_session(dev, bitrate)  # quiet check (B idle in normal mode must be silent), warm-up
        tr = Traffic(5)
        while tr.n < n:
            m = tr.next()
            dev.send(m)
            tr.sent.append((m.arbitration_id, bytes(m.data), time.time()))
        heard = dev.read_frames(3.0)  # anything the CANable receives now came from the device
        err, more = dev.error_register()
        heard += more
        dev.close()
        sent, uniq_b, reps_b = bench_window(agg, 2, tr)
        _, uniq_a, reps_a = bench_window(agg, 1, tr)
        p1 = agg.port("B")
        blocked1 = agg.get("/api/can/status")["blocked_cmds"]
        problems = []
        if uniq_b != sent:
            problems.append(f"{sent - uniq_b} of {sent} frames missing on port B")
        if reps_b or reps_a:
            problems.append(f"repeated frames: B {reps_b}, A {reps_a} (retransmissions)")
        if p1["tec"] or p1["rec"] or p1["bus_errors"] != p0["bus_errors"] or p1["fallback"]:
            problems.append(f"port B TEC {p1['tec']} REC {p1['rec']} errors +{p1['bus_errors'] - p0['bus_errors']}"
                            f" fallback {p1['fallback']}")
        if p1["state"] != "normal (ACK only)":
            problems.append(f"port B state '{p1['state']}'")
        res.add("5b CANable frames ACKed by B, no retransmit", not problems,
                "; ".join(problems) or f"{sent} sent (0x100-0x1FF), {uniq_b} stored once each on B and {uniq_a} on A, "
                f"0 repeats, B TEC 0 REC 0 (CANable error register {fmt_err(err)}, not meaningful on this firmware)")
        res.add("5c B sends no data frame", not heard and blocked1 == blocked0,
                f"{len(heard)} frames received by the CANable from the device in 3 s after sending"
                + (f" (first id 0x{heard[0].arbitration_id:X})" if heard else "")
                + f"; quiet check passed with B in ACK mode; SPI commands blocked {blocked0} -> {blocked1}")
        # 5d: ACK mode at a wrong bitrate must give up by itself.
        agg.post("/api/settings", {"can_b": {"enabled": True, "bitrate": wrong, "ack": True}})
        agg.wait_port("B", {"normal (ACK only)"})
        start_session(dev, bitrate)
        tr = Traffic(5)
        end = time.monotonic() + 8
        fell = None
        last = 0.0
        while time.monotonic() < end and not fell:
            m = tr.next()
            dev.send(m)
            if time.monotonic() - last > 0.5:
                last = time.monotonic()
                q = agg.port("B")
                if q["fallback"]:
                    fell = q
        dev.end_tx()
        q = fell or agg.port("B")
        res.add("5d B wrong bitrate -> listen-only", bool(fell) and q["state"] == "listen-only",
                f"B at {wrong // 1000}k on a {bitrate // 1000}k bus: state '{q['state']}', fallback {q['fallback']}"
                f" ({q['error']})")
    except Abort as e:
        res.add("5 B ACK", False, f"abort: {e}")
    except (RuntimeError, requests.RequestException) as e:
        res.add("5 B ACK", False, str(e))
    finally:
        dev.close()
        try:
            agg.post_status("/api/agg/transmit", {"enable": False})
            agg.post("/api/settings", {"can_inv": {"enabled": orig_inv["enabled"], "bitrate": orig_inv["bitrate"]},
                                       "can_b": {"enabled": orig["enabled"], "bitrate": orig["bitrate"],
                                                 "ack": orig.get("ack", False)}})
            print(f"  ports restored: B {agg.wait_port('B', {'listen-only', 'off'})['state']}, "
                  f"INV {agg.port('INV')['state']}")
        except (RuntimeError, requests.RequestException) as e:
            res.add("5 restore ports", False, str(e))


def check_inv_frames(frames, fs_cvl_dv, fs_dvl_dv) -> list[str]:
    problems = []
    by_id: dict[int, list[float]] = {}
    for t, m in frames:
        aid = m.arbitration_id
        if m.is_extended_id or m.is_remote_frame or aid not in INV_ALLOW or m.dlc != INV_ALLOW[aid]:
            problems.append(f"frame outside the allow-list: id 0x{aid:X} dlc {m.dlc} ext {m.is_extended_id} rtr {m.is_remote_frame}")
            continue
        by_id.setdefault(aid, []).append(t)
        if aid == 0x351:
            d = bytes(m.data)
            cvl, ccl, dcl, dvl = struct.unpack("<HhhH", d)
            if ccl or dcl or cvl != fs_cvl_dv or dvl != fs_dvl_dv:
                problems.append(f"0x351 not the start-up hold values: CVL {cvl} CCL {ccl} DCL {dcl} DVL {dvl}")
    for aid, ts in by_id.items():
        gaps = [b - a for a, b in zip(ts, ts[1:])]
        if gaps and min(gaps) < 0.85:  # host-side jitter margin; the device spaces them >= 1.05 s
            problems.append(f"0x{aid:X} twice within {min(gaps):.2f} s")
    missing = set(INV_ALLOW) - set(by_id)
    if missing:
        problems.append("never seen: " + ", ".join(f"0x{x:X}" for x in sorted(missing)))
    return problems


def test_inv_tx(dev: Slcan, agg: Agg, bitrate: int, res: Results):
    """Test 6: port INV transmits only the allow-list, 1 Hz, one-shot; off again afterwards."""
    s = agg.get("/api/settings")
    orig_inv, orig_b = s["can_inv"], s["can_b"]
    fs_cvl, fs_dvl = round(s["agg"]["failsafe_cvl"] * 10), round(s["agg"]["failsafe_dvl"] * 10)
    try:
        if not agg.get("/api/agg/status")["configured"]:
            return res.add("6 INV transmit", False, "aggregator not configured (fail-safe CVL/DVL, CCL/DCL max)")
        agg.post("/api/settings", {"can_inv": {"enabled": True, "bitrate": bitrate},
                                   "can_b": {"enabled": orig_b["enabled"], "bitrate": orig_b["bitrate"], "ack": False}})
        agg.wait_port("INV", {"listen-only"})
        # 6a: enabling without the typed confirmation is refused.
        c1, m1 = agg.post_status("/api/agg/transmit", {"enable": True})
        c2, m2 = agg.post_status("/api/agg/transmit", {"enable": True, "confirm": "transmit"})
        still_off = not agg.get("/api/agg/status")["tx"]["enabled"]
        res.add("6a transmit needs confirmation", c1 == 400 and c2 == 400 and still_off,
                f"no confirm: HTTP {c1} '{m1}'; wrong word: HTTP {c2} '{m2}'; transmit still off: {still_off}")
        # 6b: CANable ACKs (normal mode, sends nothing). Quiet check first, with INV listen-only.
        print(f"  listen-only quiet check at {bitrate // 1000} kbit/s (2 s)...")
        dev.quiet_check(bitrate)
        dev.open_tx(bitrate)  # M0: the CANable acknowledges; this test sends nothing from it
        inv0 = agg.port("INV")
        blocked0 = agg.get("/api/can/status")["blocked_cmds"]
        agg.post("/api/agg/transmit", {"enable": True, "confirm": "TRANSMIT"})
        frames = dev.read_stamped(8.0)
        st = agg.get("/api/agg/status")
        inv1 = agg.port("INV")
        err, _ = dev.error_register()
        problems = check_inv_frames(frames, fs_cvl, fs_dvl)
        if inv1["state"] != "transmitting":
            problems.append(f"INV state '{inv1['state']}'")
        if inv1["tx_failed"] != inv0["tx_failed"]:
            problems.append(f"{inv1['tx_failed'] - inv0['tx_failed']} frames failed with an ACKing node")
        if err != 0:
            problems.append(f"CANable error register {fmt_err(err)}")
        # No battery on the bench: start-up hold, or fail-safe when port A/B picked up the device's
        # own 0x35x frames on this shared bench bus (source then "stale"). Both mean CCL = DCL = 0.
        if st["state"] not in ("start-up hold", "fail-safe"):
            problems.append(f"aggregator state '{st['state']}' (want start-up hold or fail-safe: no battery on the bench)")
        ids = sorted({m.arbitration_id for _, m in frames})
        res.add("6b INV allow-list, 1 Hz, hold values", not problems,
                "; ".join(problems) or f"{len(frames)} frames in 8 s, IDs {', '.join(f'0x{x:X}' for x in ids)}, "
                f"each >= 0.85 s apart (host clock), CCL = DCL = 0, CVL/DVL = fail-safe ({st['state']}); device sent "
                f"{inv1['tx_frames'] - inv0['tx_frames']}, failed 0")
        dev.close()
        # 6c: nothing ACKs (CANable closed; A and B listen-only): one attempt per frame, no storm.
        ok_n, fail_n, tec_n, seen = inv_burst(agg, 6)
        attempts = ok_n + fail_n
        ok = ok_n == 0 and fail_n > 0 and attempts <= 6 * 7
        res.add("6c INV without ACK: one-shot", ok,
                f"{attempts} attempts in 6 s ({fail_n} unacknowledged, want <= 42 = 6 IDs x 7), TEC {tec_n}; "
                f"(the tap's buffer holds {len(seen)} allow-listed frames)")
        # 6d: off again.
        agg.post("/api/agg/transmit", {"enable": False})
        time.sleep(1.5)
        dev.quiet_check(bitrate)  # raises Abort if the device still transmits
        st = agg.get("/api/agg/status")
        p = agg.port("INV")
        blocked1 = agg.get("/api/can/status")["blocked_cmds"]
        res.add("6d transmit off", not st["tx"]["enabled"] and p["state"] == "listen-only" and blocked1 == blocked0,
                f"bus quiet 2 s after disabling; INV '{p['state']}'; SPI commands blocked {blocked0} -> {blocked1}")
    except Abort as e:
        res.add("6 INV transmit", False, f"abort: {e}")
    except (RuntimeError, requests.RequestException) as e:
        res.add("6 INV transmit", False, str(e))
    finally:
        dev.close()
        try:
            agg.post_status("/api/agg/transmit", {"enable": False})
            agg.post("/api/settings", {"can_inv": {"enabled": orig_inv["enabled"], "bitrate": orig_inv["bitrate"]},
                                       "can_b": {"enabled": orig_b["enabled"], "bitrate": orig_b["bitrate"],
                                                 "ack": orig_b.get("ack", False)}})
            print(f"  INV restored: {agg.port('INV')['state']}, transmit "
                  f"{agg.get('/api/agg/status')['tx']['enabled']}")
        except (RuntimeError, requests.RequestException) as e:
            res.add("6 restore", False, str(e))


# ---- main ----------------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--bench", action="store_true", help="confirm this is an isolated bench bus (required)")
    ap.add_argument("--port", default="/dev/ttyACM0", help="CANable serial port")
    ap.add_argument("--bitrate", type=int, default=250000, choices=sorted(SLCAN_RATE),
                    help="bitrate for tests 1 and 2 (must match the aggregator's CAN setting)")
    ap.add_argument("--tests", default="1,2,3",
                    help="comma-separated subset of 1-6 (4 = map INT pins, 5 = B ACK, 6 = INV transmit; "
                         "4-6: all modules on one bench bus)")
    ap.add_argument("--frames", type=int, default=200, help="frames for test 2 (max 1000)")
    ap.add_argument("--quiet-only", action="store_true",
                    help="never transmit: only V (liveness) and the 2 s listen-only quiet check, no login")
    a = ap.parse_args()
    sys.stdout.reconfigure(line_buffering=True)  # keep prompts and results in order when piped
    if a.quiet_only:
        checks = Checks()
        dev = None
        try:
            dev = Slcan(a.port, checks)
            print(f"listen-only quiet check at {a.bitrate // 1000} kbit/s (2 s), no transmit...")
            dev.quiet_check(a.bitrate)
            print("quiet: no frames, error register 0")
            return 0
        except (Abort, serial.SerialException) as e:
            print(f"ABORT: {e}", file=sys.stderr)
            return 2
        finally:
            if dev:
                dev.close()
            checks.report()
    if not a.bench:
        print("refusing to run: this script transmits. Use --bench only on an isolated bench bus "
              "(CANable + one aggregator port, terminated at both ends).", file=sys.stderr)
        return 2
    pw = os.environ.get("AGG_PASSWORD")
    if not pw:
        print("set AGG_PASSWORD in the environment (it is never printed or logged)", file=sys.stderr)
        return 2
    tests = {t.strip() for t in a.tests.split(",") if t.strip()}
    if not tests <= {"1", "2", "3", "4", "5", "6"} or not 1 <= a.frames <= 1000:
        print("bad --tests or --frames", file=sys.stderr)
        return 2
    host = os.environ.get("AGG_HOST", "192.168.0.59")
    print(f"aggregator {host}, CANable {a.port}, {a.bitrate // 1000} kbit/s, tests {','.join(sorted(tests))}")
    print("bench bus: 120 ohm on the CANable AND on the MCP2515 module (about 60 ohm across the pair)")
    res = Results()
    checks = Checks()
    dev = agg = None
    try:
        agg = Agg(host, pw)
        del pw
        dev = Slcan(a.port, checks)
        if "1" in tests:
            print("\n== Test 1: per-port RX (Check modules while sending)")
            test_ports(dev, agg, a.bitrate, res)
        if "2" in tests:
            print("\n== Test 2: content on the tap (port A)")
            test_content(dev, agg, a.bitrate, a.frames, res)
        if "3" in tests:
            print("\n== Test 3: bitrate detection on port A")
            test_detect(dev, agg, res)
        if "4" in tests:
            print("\n== Test 4: map the INT lines (all three modules on one bench bus)")
            test_intmap(dev, agg, a.bitrate, res)
        if "5" in tests:
            print("\n== Test 5: port B acknowledges, never transmits (all modules on one bench bus)")
            test_b_ack(dev, agg, a.bitrate, a.frames, res)
        if "6" in tests:
            print("\n== Test 6: port INV transmit (all modules on one bench bus, no battery/inverter)")
            test_inv_tx(dev, agg, a.bitrate, res)
    except Abort as e:
        print(f"ABORT: {e}", file=sys.stderr)
        return 2
    except (serial.SerialException, requests.RequestException) as e:
        print(f"ABORT: {type(e).__name__}: {e}", file=sys.stderr)
        return 2
    except (KeyboardInterrupt, EOFError):
        print("\ninterrupted", file=sys.stderr)
        return 2
    finally:
        if dev:
            dev.close()
        if agg:
            agg.logout()
        checks.report()
    failed = [n for n, ok, _ in res.items if not ok]
    print(f"\n{len(res.items) - len(failed)} passed, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
