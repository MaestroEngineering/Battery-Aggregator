# Blue Nova + KNY in parallel: findings (13 Nov 2026, 14:37 to 18:55)

Source: receiver-DB export `analysis/data/frames_since_20261113_1400.csv.gz` (91,689 rows) and `events_since_20261113_1400.csv.gz` (13 rows), read only. They were copied unchanged from `~/Desktop`; the originals are still there. Times are SAST (UTC+2), as exported.

All numbers come from `analysis/2026-11-13/results.json`, `frame_quality.csv`, `grid_1s.csv.gz` and `engine_*.csv`, which `analyse_parallel.py` writes. Plots are in the same folder. To reproduce (about 30 s):

```sh
g++ -std=c++17 -O2 -I components/agg/include analysis/2026-11-13/replay_engine.cpp components/agg/src/engine.cpp -o /tmp/re
uv run --no-project --with pandas --with matplotlib python -I analysis/2026-11-13/analyse_parallel.py --data analysis/data --replay /tmp/re
```

Decode layouts:
- **Blue Nova (port A):** as verified in `analysis/2026-11-05` (0x351, 0x355 with 0.01 % SOC in bytes 4–5, 0x356, 0x373, 0x378, 0x379, pack 2 on 0x211–0x217).
- **KNY (port B):** Pylontech layout, every frame DLC 8. 0x373 is cells in mV and temperatures in K. 0x379 is `2C01…` = 300 Ah. 0x35E is `"PYLON   "`, and 0x359 bytes 4–7 are `01 70 4E 00`.

## Summary

- **The export does not contain the aggregator's output.** The INV rows are frames the device *received* on the inverter bus: 0x305 (all zero), plus unknown all-zero 0x71F, 0x15D and 0x1FD, and an all-ones extended remote frame. The firmware never puts its own transmitted frames into the ring buffer that Verbose Diagnostics uploads. So no 0x351/0x355/0x356/0x359/0x35C/0x35E sent to the inverter appears anywhere. **Question 3 is answered by replaying the firmware's engine over the recorded battery frames** (`04_output_reconstructed.png`). That is a reconstruction, not a measurement.
- **Every upload was change-only.** A frame only appears when its data changed, plus a snapshot of every ID about every 60 s (median 60.0 s, maximum 61 s for an unchanging frame). **Frame periods, late frames and staleness cannot be measured from this export**, and neither can 0x305 gaps (0x305 is constant, so it appears once a minute). Gaps longer than about 61 s mean the device or the upload missed data, not the battery.
- **Data:** four stretches: 14:50:13–14:58:24, 15:01:07–15:41:39 and 16:44:16–18:54:57, plus a 10 s fragment at 14:38:54 that falls entirely inside a corruption burst.
  - Gaps: 11.0 min (14:39–14:50), 2.7 min (14:58–15:01) and **62.6 min (15:41–16:44), with no data on any port**.
  - Three device boots: `b67b1f48`, `a4b3eb30` (from about 14:51) and `50c07891` (the 16:44 stretch).
- **The DC bus joined at 16:44:51** (first Blue Nova current of 1 A or more). It was sustained from 16:48:13, when the evening load rose. **Voltage difference at the join was 0.01–0.02 V** (BN bank 53.23 V, pack 2 53.22 V, KNY 53.24 V at 16:44:50). **There was no surge.** In the first 10 min the largest currents were BN −9.7 A, KNY −11.4 A, pack 1 −5.2 A and pack 2 −4.6 A, all following the load.
- **After the join there is only discharge** (7,609 s with the total at −5 A or beyond, 0 s of charging). Median shares of the total: **KNY 46.8 %, BN pack 1 28.1 %, pack 2 25.0 %**.
  - The KNY holds 60 % of the capacity (300 of 500 Ah) but gave less than half the current. The Blue Nova gave 53 % from 40 % of the capacity.
  - From 16:44 to 18:55 the BN delivered 29.7 Ah (−15 % SOC) and the KNY 26.9 Ah (−9 %). **The SOCs drift apart: by 18:55 the KNY reads 89 % and the BN 83 %.**
  - **No pack exceeded its own discharge limit.** The largest event was a −88 A load at 17:20:53 for about 58 s, split KNY −39.7 A, pack 1 −26.8 A, pack 2 −23.2 A, against limits of 195, 50 and 100 A.
- **The KNY cut-off happened at 14:58:19**, before the Blue Nova was on the bus, while the KNY alone charged at about 11 A.
  - At about 3.5 V per cell on average, **one KNY cell reached 3,651 mV while the lowest was 3,393 mV (258 mV spread)**. The pack was at 56.0 V, well below its own CVL of 57.6 V.
  - The sequence: CCL 195 → 97.5 A at 14:57:48, then 0x35C 0x40 (charge disabled) at 14:58:18 and CCL 0 with 0x359 = 0x02 at 14:58:19. 0x359 cleared after 4 s (14:58:23), but **CCL 0 and charge disabled stayed for 2 h 22 min, until 17:20:52**, exactly when the KNY's SOC stepped from 96 to 95 %.
  - **The min-CVL scheme was never tested at the top of charge:** there was no charging after the join.
- **The Blue Nova reported a single-pack bank from 15:12:57 to 15:19:18**: 0x379 = 100 Ah and 0x351 CCL 50 / DCL 100 A. From 15:19:18 it reported 200 Ah, 100 / 200 A.
  - **The firmware treats that 100 → 200 Ah change as a latched trip** ("bank capacity changed … pack lost?"). The replay shows fail-safe from 15:19:18 until the next boot.
  - From 16:44 onwards the BN reported 200 Ah throughout. If the page now shows 100 Ah, it isn't from this data.

### Surprises

1. **About 40 % of port B's records in the first two boots are corrupt** (12,000 frames per minute at peak). This is very likely the SPI read, not the CAN bus.
   - **Correction (14 Nov, 0.1.8):** probably wrong. The MCP2515s ran with RXM=11, which also stores fragments of frames that had a bus error (datasheet 4.2), so bus errors from the swapped CAN-H/CAN-L would produce exactly these stored garbage frames. The 0x71F / 00×8 frames on INV are such fragments too (one per bus error, then repeated by the once-a-minute snapshots). See `findings_20261114_overnight.md` Q5; 0.1.8 stores valid frames only.
   - Evidence: bursts of up to 1,818 records per second; random extended IDs (15,561) and remote frames (608); standard IDs that are bit-flipped KNY IDs; and data from one ID under another, such as 0x379 with "PYLON" text and A's 0x211 with 0x374's "01--01" text.
   - The corrupt IDs are mostly 1-bits: 0.71 (standard) and 0.77 (extended), against 0.53 for valid IDs. An all-ones remote frame 0x1FFFFFFF also shows up on INV. Both fit a MISO line that reads high or a marginal SPI read.
   - The MCP2515 never stores a frame that fails its CRC, so bus noise can't produce well-formed but wrong frames.
   - **After the 62-minute gap (boot 3, 16:44 onwards) A and B have 0 corrupt records.** INV has 15, all 0x71F with zero data, which may be a real frame. Was something changed in that hour?
   - **On the device these frames go into the aggregator too.** In the device-like replay, corrupted 0x359 frames latched KNY trips ("undefined bits") at 14:38:55 and 14:50:04, a corrupted 0x351 tripped the plausibility check, and 0x379 "changed" from 300 to 36, 44 and 508 Ah.
2. **The KNY charged while reporting CCL 0 and charge disabled**: 19 s in 7 runs, up to 7.6 A (15:13:15 for 5 s at +5.8 A; 15:24:32 for 3 s at +7.6 A), while the Blue Nova was not yet on the bus.
   - The 0x356 frames in those runs are a clean, consistent series, so this is real.
   - Whatever controlled the inverter then did not enforce the KNY's CCL 0. The export can't say whether that was the aggregator (the replay says it would have sent CCL 0) or another link.
3. **The device dropped 2,522 frames before upload**: 2,076 in boot `b67b1f48` (four batches with 192 or 500 dropped) and 446 in `a4b3eb30`. These coincide with the corruption floods.
4. **Device and server clocks disagree in boots 1 and 2.**
   - Boot `a4b3eb30` stamped frames 9–11 min after the server's receive time (frame 15:21:54, received 15:11:11).
   - Boot `b67b1f48` has batches 53–61 "received" at 10:23, which is before batch 27 (received 14:38:49).
   - **Times before 16:44 may be off by up to about 10 min.** Boot 3 is consistent: frame 17:19:50, received 17:20:19.
5. **The one event in the events file** is a trigger on 0x35E at 17:19:50: `426C75654E6F7661` = "BlueNova", 768 frames, truncated. The trigger's configuration isn't in the export.

---

## Step 0: data and quality

| Port | Valid | Corrupt: extended ID | Remote frame | Unknown standard ID | Known ID, bad content | In a burst (well-formed but distrusted) |
|---|---|---|---|---|---|---|
| A (BN) | 40,699 | 7 | 4 | 12 | 9 | 38 |
| B (KNY) | 13,111 | 15,561 | 608 | 1,521 | 239 | 17,677 |
| INV | 192 | 26 | — | 37 | 4 | 3 |

- 1,941 rows were duplicate snapshots (same port, ID, time and data) and were removed first.
- **"In a burst"** means a well-formed frame within ±2 s of a second that had 3 or more corrupt records on the same port. Inside the bursts even well-formed frames carried flipped bits (KNY CCL 143.8 or 92.6 A, CVL 59.1 V, a pack voltage of 59.06 V while the cells summed to about 54.3 V). They are excluded. **Assumption:** this rule drops some good frames too (B 14:50–14:51), but none in the 16:44–18:55 stretch.
- DLCs match the earlier capture exactly. BN: 0x355/0x356 DLC 6, 0x379 DLC 2, 0x35F DLC 4, the rest 8. KNY: all 8.
- Within boot 3, the longest gap in valid frames is 1.8 s on A, 29.8 s on B (16:44:21, at the start) and 61.2 s on INV (0x305 snapshot cadence). Change-only uploads explain all three.
- The 62.6-min gap has no data and no events-file record. Boot `50c07891` starts in it, so it's a reboot, a power cut or an upload outage.

## Q1: the DC bus join (`02_join.png`)

| | Value |
|---|---|
| First BN current ≥ 1 A | 16:44:51 |
| Sustained (≥ 80 % of the next 60 s) | 16:48:13 |
| Just before (16:44:50): BN bank / pack 2 / KNY | 53.23 / 53.22 / 53.24 V; BN 0.0 A, KNY −2.2 A |
| ΔV BN − KNY / pack 2 − KNY | −0.01 V / −0.02 V |
| Peaks, first 10 min | BN −9.7 A (16:48:45), pack 1 −5.2 A, pack 2 −4.6 A, KNY −11.4 A (16:52:48) |

- **There was no inrush** at the 1 Hz BMS resolution. The currents follow the load step at 16:48 (total 0 → −18 A). Each battery stayed above half its peak for about 370 s because it was a load, not a surge.
- Before the gap (15:12–15:41) the Blue Nova sat at 53.20 V and 0 A while the KNY was charging and discharging on its own (53.0–53.6 V), so the two were not yet joined.
- **Can't show:** a sub-second inrush. The BMS reports at 1 Hz, and a breaker closing inside the 62-minute gap would be invisible.

## Q2: sharing and own limits (`03_sharing.png`)

| Discharge, 16:44–18:55 (7,609 s) | Median share | Range |
|---|---|---|
| KNY | 46.8 % (p5–p95 44.0–59.1 %) | −2.7 to −39.7 A |
| BN pack 1 | 28.1 % | −1.3 to −26.8 A |
| BN pack 2 | 25.0 % | 0 to −23.2 A |

- **Over time:** the KNY's share started at about 60 % at the join and fell to about 44 % by 18:55. The BN packs rose from about 20 % to 28 % each. A higher-capacity, higher-SOC battery would normally carry more, so this points to a higher internal resistance or a different OCV curve in the KNY at this SOC.
- **Limits seen:**
  - pack 2: 0x215 CCL 50 A, 0x216 DCL 100 A (no 30 A derate at this SOC);
  - BN 0x351: 100 / 200 A (50 / 100 A during the single-pack minutes);
  - KNY: 195 / 195 A, 97.5 / 97.5 A for 31 s, CCL 0 from 14:58:19 to 17:20:52.
- **Exceedances:** none by pack 1, pack 2 or the BN bank, in either direction. **The KNY charged above its own CCL (0 A) for 19 s in 7 runs, up to 7.6 A** (Surprise 2). The KNY never exceeded its DCL.
- **Can't show:** sharing while charging. There was no charging after the join.

## Q3: output to the inverter, reconstructed (`04_output_reconstructed.png`)

**Not recorded** (see the Summary). The firmware engine (`components/agg/src/engine.cpp`) was replayed over the valid frames, rebuilt at 1 Hz from the last recorded value because of the change-only uploads. It restarts at each data stretch.

**Assumptions:**
- sources A and B both enabled;
- user maximum 100 / 100 A;
- fail-safe CVL / DVL 53.0 / 47.0 V (the values left on the device at the last bench session);
- **no source went stale.** The 1 Hz rebuild makes staleness invisible.

| Stretch | State | CVL | CCL | DCL | DVL | Set by |
|---|---|---|---|---|---|---|
| 14:50–15:13 | start-up hold, then fail-safe (KNY 0x359 trip 14:58:19) | 53.0 | 0 | 0 | 47.0 | BN not seen |
| 15:13:09–15:19:18 | normal | 53.0 → 54.6 (rise hold) | **0** | 100 | 48.0 | CVL/DVL: BN. CCL 0: KNY charge disabled. DCL: user max (sum 295) |
| 15:19:18–15:41 | **fail-safe (latched)** | 53.0 | 0 | 0 | 47.0 | BN 0x379 100 → 200 Ah trip |
| 16:44:28–17:20:52 | normal | 54.6 | **0** | 100 | 48.0 | CVL/DVL: BN (54.6 < 57.6; 48.0 > 44.8). CCL 0: KNY. DCL: user max (sum 395) |
| 17:20:54–18:55 | normal | 54.6 | 100 | 100 | 48.0 | CCL: user max (sum 295) |

- **The output never exceeds any single source's limit** (0 s above the BN or the KNY limit for CCL, DCL or CVL in normal).
- **Weighted SOC:** the engine's SOC matches the capacity-weighted mean of the BN's 0.01 % SOC and the KNY's SOC within 0.75 % (p99 0.0). During the 380 s with BN 100 Ah, the BN weighted at 100 Ah, as designed. Own SOCs: BN 98 → 83 %, KNY 98 → 89 %, output 98 → 86.6 %. KNY minus BN ranged from −1 to +6 points.
- **The device-like replay** (corrupted standard-ID frames included) has the same states after 15:01. Before that it adds latched KNY trips from corrupt 0x359 at 14:38:55 and 14:50:04.

## Q4: events and failures

- **KNY 0x359:** 0x00000002 from 14:58:19 to 14:58:23 is the only real alarm. Every other 0x359 value was in a corruption burst.
- **KNY 0x35C:** 0xC0, then 0x40 (charge disabled) from 14:58:18 to 17:20:53, then 0xC0.
- **Trips in the replay:**
  - 14:58:19, KNY protection 0x02. Not latched; it would have released after the 10 s hold, but CCL stayed 0 from the KNY's own limit.
  - 15:19:18, BN 0x379 capacity change, latched.
  - Device-like only: 14:38:55 and 14:50:04, latched "undefined bits" from corrupt frames.
- **Recoveries:** at each stretch start, after the 10 s hold. KNY from 15:01:54, BN from 15:13:09, both from 16:44:28.
- **Stale sources, failed frames, late frames:** not measurable. Staleness needs 1 Hz data (change-only hides it). Failed TX frames and the INV TEC are device counters that aren't uploaded.
- **Longest gaps:**
  - Boot 3: A 1.8 s, B 29.8 s, INV 61.2 s.
  - Boot 2 (15:19–15:22): about 92–180 s on A and B. The device dropped 370 frames in that boot at 15:21–15:22 (events file seq 62–64).

## Q5: top of charge (`05_cells.png`)

| | Value |
|---|---|
| KNY at the cut-off (14:58:14–14:58:19) | 56.08–56.11 V, +11.3 A, cell max 3,643–3,651 mV, min 3,385–3,393 mV, **spread 258 mV** |
| KNY 14:57:00 → 14:58:10 | cell max 3,550 → 3,634 mV while min stayed at about 3,385; CCL 195 → 97.5 A at 14:57:48 |
| BN (all data) | max 53.26 V, cell max 3,332 mV, spread ≤ 20 mV (at rest, 98 % SOC, never charging) |

- **What it shows:** one KNY cell (or a group) runs away at the top while the rest sit at about 3.39 V. The cut-off is driven by that cell at a pack voltage well below the KNY's CVL of 57.6 V. After the cut-off, the spread fell to about 5–10 mV at rest.
- **The cut-off didn't recur in this data,** because nothing charged after 14:58 except the small runs in Surprise 2.
- **Can't show:** whether the min-CVL of 54.6 V (the BN's, about 3.41 V per cell average) would have kept that cell below 3.65 V. A min-CVL charge with both batteries joined hasn't happened yet.

## Q6: capacity and weighted SOC

- **BN 0x379:** 100 Ah from 15:12:57 to 15:19:18 (with single-pack limits 50 / 100 A), then 200 Ah to the end.
- **The aggregator used** 100 Ah for 380 s and 200 Ah for 9,182 s (replay). It is consistent with the BMS at every moment.
- **Inconsistencies:**
  1. The 100 → 200 Ah change latches a trip in the firmware. It was a pack joining, not a pack lost.
  2. If the page shows 100 Ah now, the BMS has sent 100 again after 18:55, or the device is in a state this export doesn't cover.
- **KNY 0x379:** 300 Ah (`2C01`) constantly, and it replaces the setting.

## Q7: anything odd

- **DLCs:** unchanged against the 30 Oct–5 Nov capture. KNY all 8.
- **Port A unknown IDs:** 0x090 (remote, DLC 0), 0x231, 0x23E, 0x278, 0x511, 0x002F0000, 0x1EEA721B. All within 15:18–15:22 and all corrupt.
- **Port INV:**
  - 0x305 all zero (195 valid), plus 4 × 0x305 = FF×8;
  - 0x71F (25), 0x15D (6), 0x1FD (6), all zero;
  - 26 × 0x1FFFFFFF remote frames.
  - The 0x71F/0x15D/0x1FD frames have no known meaning. They may be the second inverter or corruption.
  - In boot 3 (clean SPI on A and B) only 0x71F appears, 15 times, all zero, which suggests a real frame. 0x15D and 0x1FD appear only in boots 1–2. **I can't tell which they are.**
- **Duplicates:** 1,941 snapshot duplicates (expected with change-only).
- **Events file:** 12 "dropped" batches (2,522 frames), 1 truncated trigger event, and the clock inconsistencies in Surprise 4.

## What this data can't answer

- Anything about the frames the gateway sent: values, timing, failed frames, TEC. The export has no TX records, and Q3 is a reconstruction.
- Staleness, frame periods and 0x305 gaps (change-only uploads).
- Charging behaviour with both batteries joined, including the min-CVL top of charge.
- What happened from 15:41 to 16:44, and why the device rebooted twice.
- The exact absolute time before 16:44 (clocks).
- Whether the inverter followed the aggregator at 15:13–15:24, when the KNY charged at CCL 0.

## Suggestions (no changes made)

1. **Find the SPI problem before trusting the output again.** Boot 3 is clean, so check what changed in that hour.
   - Look at port B's MISO and the level shifter, and the grounding.
   - In firmware: count corrupt reads and refuse to feed frames to the aggregator when CANINTF reads 0xFF or a frame fails a sanity check. Today a corrupt 0x359 latches a trip and a corrupt 0x351 trips the plausibility check, which is safe but disruptive.
2. **Decide whether a Blue Nova 0x379 rise (100 → 200 Ah, a pack joining) should latch.** Today it does. A fall is the "pack lost" case.
3. **Upload the gateway's own TX frames, or at least its output values, with Verbose Diagnostics,** so the next analysis can measure the output instead of reconstructing it.
4. **Capture a charge with both batteries joined.** Watch the KNY cell spread at a CVL of 54.6 V and the share near full.

## Files

`analysis/2026-11-13/`:
- `analyse_parallel.py`, `replay_engine.cpp`
- `01_overview.png`, `02_join.png`, `03_sharing.png`, `04_output_reconstructed.png`, `05_cells.png`, `06_corruption.png`
- `results.json`, `frame_quality.csv`, `grid_1s.csv.gz` (1 s state table)
- `engine_valid_*.csv`, `engine_devicelike_*.csv` (replayed output and events)
