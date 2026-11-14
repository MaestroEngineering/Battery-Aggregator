#!/usr/bin/env python3
"""Replay test: the real receiver exports through the firmware engine (0.1.7 behaviour).

    g++ -std=c++17 -O2 -I components/agg/include tools/replay_engine.cpp components/agg/src/engine.cpp -o /tmp/re
    uv run --no-project --with pandas --with matplotlib python -I tools/test_replay_exports.py --replay /tmp/re

Inputs (read only): analysis/data/frames_since_20261113_1400.csv.gz (13 Nov afternoon, with the
corruption bursts) and frames_since_20261113_1700.csv.gz (the night of 13/14 Nov). Every recorded
standard-ID frame on ports A and B is fed in order, INCLUDING the corrupt ones (as the device saw
them), plus a 1 Hz rebuild of each ID from its last valid payload (the uploads were change-only).
Settings: sources A + B, user maximum 100/100 A, fail-safe 53.0/47.0 V, caps A 100, A pack 2 100, B 100.
Exit code 0 when every check passes.

--write-input DIR only writes the engine input lists (DIR/in_<stem>.csv) for
tools/test_output_independent.cpp and exits.
"""
import argparse, os, sys, tempfile

import pandas as pd

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "analysis", "2026-11-13"))
import analyse_parallel as P  # noqa: E402  (validated decoders, segments, engine input)

FAIL = 0


def check(cond, msg):
    global FAIL
    print(("  PASS " if cond else "  FAIL ") + msg)
    FAIL += 0 if cond else 1


def replay(replay_bin, stem, data):
    df, _ = P.load(data, stem)
    segs = P.segments(df)
    rows = P.engine_input(df, segs, True)
    tmp = tempfile.mkdtemp(prefix="replay-")
    f = os.path.join(tmp, "in.csv")
    with open(f, "w") as fh:
        fh.write("t_ms,src,id,dlc,hex,seg\n")
        for t, s, i, d, h, k in rows:
            fh.write(f"{t},{s},{i:X},{d},{h if h else '0'},{k}\n")
    import subprocess
    subprocess.run([replay_bin, f, os.path.join(tmp, "o"), "100", "100", "53.0", "47.0", "100", "100", "100"], check=True)
    o = pd.read_csv(os.path.join(tmp, "o_out.csv"))
    e = pd.read_csv(os.path.join(tmp, "o_events.csv"))
    o["t"], e["t"] = o.t_ms / 1000, e.t_ms / 1000
    o["hm"] = [P.fmt(t) for t in o.t]
    e["hm"] = [P.fmt(t) for t in e.t]
    return df, segs, o, e


def show(e, types):
    for _, x in e[e.type.isin(types)].iterrows():
        print(f"      {x.hm} {x.type:<19} {x.source:>2} {x.text}")


def afternoon(rb, data):
    print("13 Nov 14:37-18:55 (corruption bursts, capacity step, KNY at CCL 0)")
    df, segs, o, e = replay(rb, "20261113_1400", data)
    show(e, ["trip", "capacity_step", "overcurrent_derate", "frame_rejected"])
    check(not (e.type == "trip").any() or not e[e.type == "trip"].text.str.contains("latched").any(),
          "no latched trip (0.1.6: three, from corrupt 0x359 x2 and the 100 -> 200 Ah step)")
    cs = e[(e.type == "capacity_step") & e.text.str.contains("100 -> 200")]
    check(len(cs) == 1 and cs.hm.iloc[0] in ("15:19:18", "15:19:19"),
          f"capacity step 100 -> 200 Ah at {cs.hm.tolist()} (BMS changed at 15:19:18; counts on the 2nd frame)")
    if len(cs):
        t0 = cs.t.iloc[0]
        st = o[(o.t > t0 + 1) & (o.t < t0 + 9)].state.unique().tolist()
        after = o[(o.t > t0 + 11) & (o.t < t0 + 60)].state.unique().tolist()
        check(st == ["start-up hold"] and after == ["normal"], f"recovery hold {st}, then {after}")
    rej_b = int(o.kny_rej.max())
    check(rej_b > 100, f"B rejected {rej_b} standard frames with IDs/DLCs the KNY does not send (the extended/remote "
          "ones are dropped and counted before the engine)")
    one = e[(e.type == "capacity_step") & ~e.text.str.contains("100 -> 200")]
    check(len(one) == 0, f"no capacity step from single corrupt 0x379 frames ({one.text.tolist()})")
    # the 15:13 KNY charge against its own CCL 0 is logged, not derated
    z = e[(e.type == "overcurrent_derate") & e.text.str.contains("against its own limit 0")]
    check(len(z) >= 1 and (o.g_b_c == 0).all(), f"KNY charging against CCL 0 logged ({z.hm.tolist()}), no derate state")
    s3 = o[o.t >= P.ts("16:45")]
    check((s3.state == "normal").mean() > 0.999, f"16:45-18:55 normal {100 * (s3.state == 'normal').mean():.2f} %")
    check(((s3[[c for c in o.columns if c.startswith('g_')]] == 0).all().all()), "no guard derate after the join")


def overnight(rb, data):
    print("13/14 Nov 17:00-07:14 (overnight discharge, then charge)")
    df, segs, o, e = replay(rb, "20261113_1700", data)
    show(e, ["trip", "capacity_step", "overcurrent_derate", "frame_rejected", "stale", "plausibility"])
    check(not (e.type.isin(["trip", "capacity_step", "stale", "plausibility", "frame_rejected"])).any(),
          "no trip, capacity step, stale, implausible or rejected frame")
    check(not (e.type == "overcurrent_derate").any() and (o[[c for c in o.columns if c.startswith('g_')]] == 0).all().all(),
          "over-current guard never triggered")
    first = e[e.type == "recovered"].t.max()  # the export starts mid-stream: first snapshots arrive within 15 s
    n = o[o.t >= first + 1]
    check((n.state == "normal").all(), f"normal from {P.fmt(first)} (first hold) on: {n.state.value_counts().to_dict()}")
    check(n.ccl.max() <= 100 and n.dcl.max() <= 100, f"CCL <= 100 ({n.ccl.max()}), DCL <= 100 ({n.dcl.max()})")
    check(abs(n.cvl.mode().iloc[0] - 54.6) < 0.05 and abs(n.dvl.mode().iloc[0] - 48.0) < 0.05,
          f"CVL {n.cvl.mode().iloc[0]} (BN), DVL {n.dvl.mode().iloc[0]} (BN)")
    # KNY CCL 0 until 17:20:52: output CCL 0 then, 100 afterwards
    a = n[n.t < P.ts("17:20")]
    b = n[n.t > P.ts("17:22")]
    check((a.ccl == 0).all() and (b.ccl == 100).all(), "CCL 0 while the KNY has charge disabled, then 100")


def write_input(data, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for stem in ("20261113_1400", "20261113_1700"):
        df, _ = P.load(data, stem)
        rows = P.engine_input(df, P.segments(df), True)
        f = os.path.join(out_dir, f"in_{stem}.csv")
        with open(f, "w") as fh:
            fh.write("t_ms,src,id,dlc,hex,seg\n")
            for t, s, i, d, h, k in rows:
                fh.write(f"{t},{s},{i:X},{d},{h if h else '0'},{k}\n")
        print(f"{len(rows)} frames -> {f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--replay", default="/tmp/re")
    ap.add_argument("--data", default=os.path.join(ROOT, "analysis", "data"))
    ap.add_argument("--write-input", metavar="DIR", help="only write the engine input lists to DIR")
    a = ap.parse_args()
    if a.write_input:
        write_input(a.data, a.write_input)
        return 0
    afternoon(a.replay, a.data)
    overnight(a.replay, a.data)
    print(f"\n{'all replay checks passed' if not FAIL else str(FAIL) + ' FAILED'}")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
