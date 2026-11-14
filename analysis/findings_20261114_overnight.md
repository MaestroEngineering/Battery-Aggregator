# Overnight 13/14 Nov 2026: Blue Nova + KNY in parallel (17:00 to 07:14)

Source: receiver-DB export `analysis/data/frames_since_20261113_1700.csv.gz` (268,940 rows) and `events_since_20261113_1700.csv.gz` (1 row), read only. They were copied unchanged from `~/Desktop`; the originals are still there. Times are SAST.

The DC bus has been joined since 16:44:51 on 13 Nov. All numbers come from `analysis/2026-11-14/results.json`, `hourly.csv`, `steps.csv` and `guard_events.csv`, which `analyse_overnight.py` writes using the validated decoders of `analysis/2026-11-13/analyse_parallel.py`. To reproduce (about 1 min):

```sh
uv run --no-project --with pandas --with matplotlib python -I analysis/2026-11-14/analyse_overnight.py --data analysis/data
```

**Assumptions:**
- **Pack 1 current = A (0x356) − pack 2 (0x211).**
- **Pack 1 SOC = 2 × bank SOC − pack 2 SOC.** This holds only if both packs are 100 Ah, as the 200 Ah in 0x379 suggests.
- **The INV rows don't contain the gateway's transmitted frames**, so the output itself isn't in this data.

## Summary

- **Clean data:** 14.2 h, one continuous stretch, no gap longer than 61 s on any port, 0 frames dropped on the device, and **0 corrupt records out of 264,614** (after removing 4,326 duplicate snapshots). There were also no extended or remote frames.
- **Mostly discharge:** the bank discharged all night, at a total of 15–32 A (30-min means), then started charging at 06:20.
  - KNY: 153.9 Ah out, 97 → 46 % SOC.
  - Blue Nova: 117.0 Ah out (pack 1 56.8, pack 2 60.2), 97 → 40 %.
  - Over the whole night the KNY carried 57 % of the energy, close to its 60 % capacity share. The split was very uneven in between.
- **The split is driven by SOC, through the LFP voltage curve, not by current level or a fixed resistance.**
  - From 20:00 to 22:00 the Blue Nova crossed the LFP voltage step at about 70–64 % SOC, pack 1 a little before pack 2.
  - In that window, pack 2 carried up to 71 % of the Blue Nova's current (30-min mean), the Blue Nova cell spread rose from 7 to about 20 mV, and the **KNY took up to 89 % of the total** while it was still at 72–80 % SOC.
  - After the step, from 23:00 to 04:00, the shares were steady: **KNY 54–56 %, pack 1 22–23 %, pack 2 22–23 %, pack 2 / Blue Nova 0.50**.
- **Pack 2 ended 2–4 points below pack 1** (39 against 41 % at 07:00). It lost 4 points to pack 1 during the 20:00–22:00 step. Afterwards both carried exactly the same current, so nothing pulled them back together.
- **The dynamic (load-step) shares are KNY 40 %, pack 1 32 %, pack 2 28 %** (12 clean steps of 5 A or more). Translated into resistances: KNY ≈ 5.1 mΩ, pack 1 ≈ 6.3 mΩ, **pack 2 ≈ 7.2 mΩ**, with about 2.0 mΩ for all three in parallel.
  - Pack 2's path is about 1 mΩ (15 %) stiffer than pack 1's, which fits an extra lug or cable in the daisy chain.
  - That difference explains the step response but **not the steady split** (50/50 at the plateau) or the evening swing. The swing comes from the voltage curve.
- **No limit changed and no alarm appeared as SOC fell to 37–45 %.**
  - Blue Nova: 0x351 stayed 54.6 V / 100 A / 200 A / 48.0 V; pack 2 0x215/0x216 stayed 50 / 100 A; 0x35A stayed constant; 0x379 stayed 200 Ah.
  - KNY: 57.6 V / 195 A / 195 A / 44.8 V and 0x359 = 0 all night. The only change was CCL 0 → 195 A and 0x35C 0x40 → 0xC0 at 17:20:52/53, the release after the previous afternoon's cut-off.
- **KNY cells:** 3,253–3,318 mV. Spread median 4 mV, p95 6 mV, at rest (|I| < 1 A) median 2 mV and at most 8 mV. The largest was 26 mV at 17:21:13, during the −88 A load. **The 258 mV spread at the 13 Nov cut-off is a top-of-charge effect only.**
- **The per-source over-current guard (Phase 2 parameters) would not have triggered once overnight.** The highest |I|/limit was A 0.50, pack 2 0.23 and KNY 0.40. The firmware engine, replayed over the same data, agrees.

## Q1: pack 1 against pack 2, and the share per battery

`01_currents_shares.png`, `03_split_drivers.png`, `hourly.csv`

| Time | Total (A) | KNY | Pack 1 | Pack 2 | Pack 2 / BN | SOC KNY / P1 / P2 |
|---|---|---|---|---|---|---|
| 17:00 | −26.0 | 50.0 % | 27.2 % | 22.8 % | 0.46 | 95 / 94 / 94 |
| 19:00 | −30.4 | 44.0 % | 27.2 % | 28.8 % | 0.51 | 86 / 77 / 79 |
| 20:30 | −27.4 | 62.6 % | 11.0 % | 26.5 % | **0.71** | 79 / 69 / 67 |
| 21:30 | −23.1 | **89.0 %** | 4.8 % | 6.2 % | 0.57 | 72 / 68 / 64 |
| 23:00 | −18.4 | 62.9 % | 18.5 % | 18.6 % | 0.50 | 64 / 64 / 60 |
| 01:00 | −15.3 | 54.3 % | 22.8 % | 22.9 % | 0.50 | 58 / 56 / 52 |
| 03:00 | −15.4 | 53.8 % | 23.3 % | 22.9 % | 0.50 | 52 / 49 / 45 |
| 05:00 | −15.3 | 51.1 % | 26.2 % | 22.7 % | 0.46 | 47 / 40 / 38 |

(30-min means. SOC is the last value in each window.)

What the pack-2 fraction tracks, as Spearman rank correlation over 47,500 discharge seconds:

| Driver | ρ |
|---|---|
| current level | −0.003 |
| Blue Nova SOC | 0.29 |
| pack 2 − pack 1 temperature | −0.62 |
| time | −0.29 |

Binned by current, the pack-2 fraction is 0.49–0.50 from 3 to 60 A. Binned by SOC, it rises from 0.48 (above 80 %) to 0.55 (70–80 %) and falls back to 0.50 (50–60 %) and 0.46 (30–40 %).

**Reading:**
- It doesn't track the current level, so it isn't a fixed series resistance. A pure cable/lug difference would give the same split at every current, at every SOC.
- It does track SOC: the swing is centred on the LFP voltage step at about 70–64 % (`03_split_drivers.png`, middle panel).
- The temperature correlation is a side effect. Pack 2 is 0.1–0.7 °C warmer than the Blue Nova sensor, and the difference shrinks over the evening as the step passes. That co-moves with time, so I can't separate the two, but the size of the effect (±20 points of share for 0.6 °C) is far too large for temperature.
- **There is a small resistance difference:** at load steps, pack 2 takes 28 % against pack 1's 32 % (Q2). It doesn't show in the steady split because, at the plateau, the voltage curve sets the split.

## Q2: SOC against voltage, and internal resistance

`02_soc_voltage.png`, `04_voltage_soc_steps.png`, `steps.csv`

- **Why the smaller packs fell faster, then stabilised:**
  - All three batteries sit on one bus at one voltage (the three voltages agree within 10–30 mV all night).
  - Each one supplies whatever current puts its own OCV − I·R at that bus voltage.
  - From 17:00 to 20:00 all three were on the upper plateau, and the Blue Nova packs (lower resistance per Ah) took 50–56 % of the load from 40 % of the capacity. Their SOC fell faster: 97 → 70 % against the KNY's 97 → 82 %.
  - At about 70 % the Blue Nova cells dropped onto the lower plateau (cell max falls from about 3,312 to 3,290 mV). The KNY, still at about 80 %, was then the higher-OCV source and took up to 89 % until its SOC came down to the same voltage at about 64 % (23:00).
  - From then on the two curves run parallel. This is self-balancing by the voltage curve, not by the aggregator.
- **Load-step analysis:** 12 steps of 5 A or more in the total, with flat current 3 s before and after.

| | Share of a step (median, IQR) | R from its own V/I (mΩ) | R from share × R_parallel (mΩ) |
|---|---|---|---|
| KNY | 39.7 % (38.5–40.9) | 5.3 (4.1–6.2), n = 11 | 5.1 |
| Pack 1 | 32.1 % (31.1–33.2) | — (no separate voltage) | 6.3 |
| Pack 2 | 27.8 % (27.6–28.2) | 9.2 (8.7–9.9), n = 9 | 7.2 |
| BN bank | 59.7 % | 4.8 (4.5–5.0), n = 12 | — |

  - R_parallel ≈ 2.0 mΩ (ΔV_bus / ΔI_total).
  - **Precision:** the BMS voltages have 10 mV resolution, against ΔV of 10–30 mV per step, so the direct V/I values are rough. The share-based values are more robust.
  - Pack 2's direct value (9.2) is above the share-based one (7.2). Its 0x211 voltage is measured inside pack 2, so it also includes the pack's own resistance.
- **Do resistances explain the shares?**
  - **For fast changes, yes:** the KNY takes 40 % of any load step, less than its 60 % capacity share. It is the stiffest source per amp but the softest per Ah.
  - **For the steady split, no.** At the plateau the KNY carried 54 %, more than its 40 % step share. The rest is the OCV difference between the batteries' SOCs.
- **Can't show:** a true OCV curve. There were only 784 s with every |I| < 1 A, mostly at 06:00, at 37–46 % SOC.

## Q3: limits at low SOC

- **The lowest SOCs** reached were Blue Nova 37 %, pack 2 36 %, pack 1 38 % and KNY 45 %, with the lowest voltage 52.23 V (both, at about 04:47).
- **No limit, alarm or capacity changed** on either battery (full list under Summary). The DVLs (48.0 / 44.8 V) were 4 V or more below the lowest voltage.
- **The only change:** KNY CCL 0 → 195 A and 0x35C 0x40 → 0xC0 at 17:20:52–53, as SOC stepped from 96 to 95 %.
- **Can't show:** derating below 37 %. Neither battery has been near its DVL.

## Q4: KNY cells (0x373, port B)

`05_kny_cells.png`

| | mV |
|---|---|
| Cell min / max all night | 3,253 / 3,318 |
| Spread median / p95 / max | 4 / 6 / 26 (17:21:13, −88 A load) |
| Spread at rest (|I| < 1 A, 784 s) | median 2, max 8 |
| Spread by SOC (10 % bins, 40–90 %) | 3–6 |
| Spread by KNY current | 2 (rest) → 4 (−5 to −20 A) → 6 (below −20 A) |
| Temperatures | 26.9–31.9 °C |

- **At mid SOC the KNY is well balanced.** The 258 mV spread at the 13 Nov cut-off (3,651 against 3,393 mV) is a top-of-charge divergence of one cell.
- **For comparison,** the Blue Nova's spread rose to about 20 mV while it crossed the voltage step (20:00–22:00) and was 5–8 mV otherwise.

## Q5: events, gaps, corruption

- **Stale sources, trips, recoveries:** none can be measured. Uploads were change-only again, with a snapshot of every ID every 60 s, so a frame gap of up to 61 s just means "unchanged".
  - The longest gaps were A 2.0 s, B 49.6 s (23:09:06), INV 61.2 s, and every required ID at most 61.2 s. **No gap exceeds the snapshot cadence, so nothing in the data contradicts "never stale".**
  - The device's own event log isn't in the export, so trips and recoveries can't be read directly.
- **Device-dropped frames:** 0. The only events-file row is the 17:19:50 trigger event from the previous export (0x35E, truncated).
- **Capacity steps:** none. BN 0x379 was 200 Ah throughout and KNY 300 Ah.
- **Corrupt records:** **0** of 264,614, against about 40 % of port B in the first two boots on 13 Nov. All IDs and DLCs match the verified layouts, and there are no extended or remote frames.
- **Is the swapped CAN-H/CAN-L ruled out as an ongoing problem?** The data can confirm that the corruption has stopped completely since the wiring change. It can't confirm that the swap was the cause.
  - On 13 Nov the corrupt records were well-formed frames with flipped bits (biased towards 1s) and with one ID's data under another ID.
  - A swapped pair normally shows up as receive errors (the controller discards frames that fail the CRC), not as stored frames with valid CRCs. That pattern fits a bad SPI read better.
  - Swapping the wires may have fixed a connector or ground at the same time. I'd keep an eye on it rather than call it closed. The lightweight validation in Phase 2 will count anything that comes back.
  - **Correction (14 Nov, firmware 0.1.8):** the argument above is wrong. Up to 0.1.7 all three MCP2515s ran with RXM=11 ("receive any message"). In that mode the controller also stores a message that had an error before EOF, as far as it was assembled (MCP2515 datasheet, section 4.2). So a swapped pair *does* produce stored garbage frames, without any CRC check. The 13 Nov bursts therefore fit the swap after all. The same mode produced the phantom INV frame 0x71F / 00×8 at the first transmit after the start-up hold (14 Nov 15:19:24 and 15:23:40). 0.1.8 receives valid frames only (RXM=00, filters open); bus errors are still counted.
- **KNY cadence:** its frames repeat on a 1.6 s grid (0x356/0x373 change intervals have a median of 1.6 s, and the snapshots land every 60.8 s = 38 × 1.6). **It probably sends every 1.6 s, not 1 Hz.** That is still well inside the 5 s timeout (three frames).

## Q6: the per-source over-current guard on this data

Parameters (the Phase 2 firmware values):
- limit = min(own reported limit, user cap), with the 0.1.7 default user caps A 100 A, A pack 2 100 A, B 100 A;
- more than 105 % for more than 3 s → that direction at 50 % (held at least 30 s; released after 30 s below 90 %);
- still over 15 s later, or above 120 % at any moment → that direction 0 for the recovery hold.

| Source | Limit used (charge / discharge) | Highest |I| / limit | Triggers |
|---|---|---|---|
| A (BN bank) | min(100, 100) / min(200, 100) | 0.50 | 0 |
| A pack 2 | min(50, 100) / min(100, 100) | 0.23 | 0 |
| B (KNY) | min(195, 100) / min(195, 100) | 0.40 | 0 |

- **Overnight: 0 derates, 0 seconds derated.**
- **For comparison, the 13 Nov afternoon:** one trigger, B charge at 15:13:19. The KNY charged at 5.6 A while its own CCL was 0. The output CCL was already 0 then (a source at CCL 0 blocks charging), so the derate changes nothing but records the event.
- **Cross-check:** this table comes from the analysis-side model (`guard_sim` in `analyse_overnight.py`). The 0.1.7 firmware engine, replayed over the same export by `tools/test_replay_exports.py`, gives the same result: 0 derates and 0 s derated overnight.
- On 13 Nov the firmware logs the KNY's 5.6 A against CCL 0 at 15:13:19 (and 5.1 A at 15:24:35) without a derate, because the output CCL is already 0. The analysis model counted it as a derate.
- **The 6-day Blue Nova capture (30 Oct–5 Nov)** has 4 trigger episodes, all pack 2 charging above its derated 30 A at the top of charge: 31 Oct 12:18:53 (36.0 A), 3 Nov 12:34:07 (41.4 A, the known overrun), 4 Nov 11:53:19 (35.7 A) and 5 Nov 11:52:09 (36.7 A).

## What this data can't show

- What the gateway sent (no TX in this export), and whether the inverter obeyed it.
- Staleness and true frame periods (change-only uploads).
- An OCV curve, or behaviour below 37 % SOC, at the top of charge with both joined, or at currents above 50 A (one −88 A event on 13 Nov, about 40 A overnight).
- Pack 1's own voltage (the Blue Nova reports the bank voltage), so pack 1's resistance is share-based only.

## Files

`analysis/2026-11-14/`:
- `analyse_overnight.py`
- `01_currents_shares.png`, `02_soc_voltage.png`, `03_split_drivers.png`, `04_voltage_soc_steps.png`, `05_kny_cells.png`
- `results.json`, `hourly.csv` (30 min), `steps.csv`, `guard_events.csv`, `grid_1s.csv.gz`
