# Battery-Aggregator

A device that allows multiple incompatible batteries on different CAN networks to be used in a single inverter.

An open-source BMS CAN aggregator gateway on an ESP32. The full design is in [SPEC.md](SPEC.md).

**Status: 0.1.7 (built, not yet flashed). Receive, decode, combine, fail-safe logic, the per-source over-current guard and the (off by default) transmit to the inverter are done. 0.1.6 has run on the real batteries since 13 Nov; transmit to a real inverter is untested.**

- **Phase 1:** the device is flashed once over USB and from then on updated over WiFi. It has WiFi provisioning, a web UI, and OTA with automatic rollback.
- **Phase 2:** one MCP2515 (port A) runs in **listen-only** mode. It provides a live frame table, capture download, listen-only bitrate detection, and Verbose Diagnostics upload to a minimal reference server.
- **0.1.5:** all three ports receive (listen-only, bitrate per port). The aggregator decodes the Blue Nova (port A) and the KNY-P51300 (port B), combines them into one Pylontech-style battery, applies every always-on fail-safe rule, and computes the inverter frames every second. Events, MQTT and Home Assistant discovery are included.

- **0.1.6:** port INV can transmit the output to the inverter (off by default; enabled only through a typed confirmation), and port B can acknowledge the KNY's frames. Port A stays listen-only and hardware-silent. The SPI guard allows exactly these exceptions; see [SPI guard](#spi-guard-what-each-port-may-do). Either battery alone, or both, can be the source.
- **0.1.7:** per-source over-current guard and user caps; a capacity step (a Blue Nova pack joining or leaving) is a warning plus a recovery hold, not a latched trip; per-source frame validation; Blue Nova pack 1 shown explicitly; KNY cells, capacity and enable flags; integrated energy counters per battery and pack; fail-safe CVL/DVL follow the last good pack voltage; the frames sent on INV are uploaded to Verbose Diagnostics, tagged TX.

## Contents

- [Build](#build)
- [First flash over USB](#first-flash-over-usb)
- [First boot: setup](#first-boot-setup)
- [OTA updates](#ota-updates)
- [Testing rollback and rejection](#testing-rollback-and-rejection)
- [Factory reset](#factory-reset)
- [CAN tap (Phase 2)](#can-tap-phase-2)
- [Aggregator](#aggregator)
- [Bench test with a CANable](#bench-test-with-a-canable)
- [Verbose Diagnostics and the reference server](#verbose-diagnostics-and-the-reference-server)
- [Partition layout](#partition-layout)
- [Changing chip or flash size](#changing-chip-or-flash-size)
- [Web UI and API](#web-ui-and-api)
- [Project layout](#project-layout)

## Build

The firmware uses ESP-IDF (not Arduino), built with [PlatformIO](https://platformio.org/). PlatformIO downloads ESP-IDF, the toolchain and the managed components (mdns, LittleFS, cJSON) on the first build.

```sh
pio run                     # build firmware   -> .pio/build/bms-agg/firmware.bin
scripts/release.sh          # release build    -> release/bms-agg-<VERSION>.bin (+ .sha256)
```

The version comes from the `VERSION` file in the project root (for example `0.1.0`). The build puts it into the app descriptor (`PROJECT_VER`), and the release script names the `.bin` after it and checks that the image really contains that version. The build date and time shown in the UI come from the app descriptor's own date and time fields. The git hash is a fixed constant, so the UI shows `git: unknown`. The build never runs git.

If `pio` is not on your PATH, use `~/.platformio/penv/bin/pio`.

## First flash over USB

You only need to do this once. Connect the ESP32 over USB, then:

```sh
pio run -t erase            # optional: wipe everything (recommended for the very first flash)
pio run -t upload           # bootloader + partition table + otadata + app
pio device monitor          # serial log, 115200 baud
```

You do not need a separate filesystem upload. The web UI is embedded in the firmware and copied into LittleFS at boot.

## First boot: setup

There is no default password.

1. On first boot (or after a factory reset) the device starts an **open** setup access point named `BMS-Agg-XXXX`, where XXXX comes from the MAC address.
2. Join it with a phone or laptop. The setup page should open by itself (captive portal). If it does not, browse to `http://192.168.4.1/`.
3. Enter:
   - **WiFi network and password.** The *Scan* button lists nearby networks.
   - **Admin password**, at least 8 characters. It is stored salted and hashed (PBKDF2-HMAC-SHA256), never in plain text.
   - **Recovery AP password**, at least 8 characters. See below.
4. The device tries the WiFi while the setup AP stays up, and the page shows the result.
   - **On success** the settings are saved, the page shows the new IP address, and the setup AP turns off after 20 s.
   - **On failure** (wrong password, network not found) nothing is saved and you can try again.
5. Rejoin your normal WiFi and open **http://bms-agg.local/**, or the IP address shown. Log in with the admin password.

**If WiFi later fails** (5 failed attempts in a row, or 30 s without an IP address), the access point comes back as a **WPA2** network protected by the recovery AP password. The UI there still needs a login. The device keeps retrying the WiFi in the background and turns the AP off again once it reconnects.

The hostname (default `bms-agg`, so `bms-agg.local`), NTP server (default `pool.ntp.org`) and POSIX timezone (default `SAST-2`) can be changed on the System page.

## OTA updates

> **Bump `VERSION` by hand before every OTA test build.** Otherwise the new image reports the same version as the old one, and you cannot see that the update happened.

1. Edit `VERSION`, for example `0.1.0` to `0.1.1`.
2. Run `scripts/release.sh`. This creates `release/bms-agg-0.1.1.bin`.
3. In the web UI open **Firmware**, choose the `.bin` and click **Upload and install**. A progress bar shows the upload.
4. The device checks the image, switches to it and reboots. The page waits for the device to come back. Log in again (sessions do not survive a reboot) and check the version on the Dashboard.

What happens on the device:

- The upload is streamed straight into the **inactive** slot. The running image is never touched.
- After the first 288 bytes it checks the **image magic**, **chip type**, **chip revision**, the **app descriptor** and the **project name** (`bms-agg`). A bad image is rejected at that point.
- At the end, `esp_ota_end` verifies every segment and the image's SHA-256, so corrupt or truncated files are rejected. Only then is the new slot selected for boot.
- Only one update can run at a time. A second upload gets HTTP 409.
- The upload endpoint requires a logged-in session and the `X-BMS-Request` header.
- **Self-test:** a freshly installed image boots in *pending verify* state. It marks itself valid only once the config has loaded, the HTTP server answers a real request over loopback, and WiFi or the AP is up. If that has not happened within **60 s**, it marks itself invalid and the bootloader boots the previous image. A crash, watchdog reset or power loss before that point also returns to the previous image.
- **Rollback button:** the Firmware page can boot the image in the other slot. Both images are kept.
- **Signed images:** not part of Phase 1. There is a clearly marked hook (`verify_signature()` in `components/ota/src/ota.cpp`) and a setting, *Require signed images*. Because Phase 1 cannot verify signatures, turning the setting on **rejects every upload**: it fails closed. It is off by default in Phase 1, which is the spec's developer mode.

Upload from the command line, for example for scripted tests:

```sh
H=http://bms-agg.local
curl -s -c /tmp/bms.jar -H 'X-BMS-Request: 1' -H 'Content-Type: application/json' \
     -d '{"password":"YOUR-ADMIN-PASSWORD"}' $H/api/login
curl -s -b /tmp/bms.jar -H 'X-BMS-Request: 1' -H 'Content-Type: application/octet-stream' \
     --data-binary @release/bms-agg-0.1.1.bin $H/api/ota/upload
```

## Testing rollback and rejection

**Automatic rollback.** Build a test image whose self-test always fails, then install it like a normal update:

```sh
echo 0.1.2 > VERSION
scripts/release.sh --fail-selftest   # -> release/bms-agg-0.1.2-selftest-fail.bin
```

After you upload it, the device reboots into 0.1.2. The log shows `self-test FAILED ... rolling back`, and about 60 s later the device is back on the previous version. The Firmware page then lists the 0.1.2 slot as *aborted* or *invalid*. Never ship this image.

**Rejected images.** Make broken copies of a good image and upload each one:

```sh
tools/make_bad_images.py release/bms-agg-0.1.1.bin     # -> release/bad/*.bin
```

- `wrong-chip.bin`, `wrong-project.bin`, `bad-magic.bin` and `random.bin` are rejected by the header check.
- `corrupt.bin` and `truncated.bin` are rejected by the full verification at the end.

In every case the UI says *Rejected: … The running firmware was not changed*.

The header check also runs on a PC:

```sh
g++ -std=c++17 -I components/ota/include tools/test_image_check.cpp \
    components/ota/src/image_check.cpp -o /tmp/test_image_check
/tmp/test_image_check ok release/bms-agg-0.1.0.bin
/tmp/test_image_check reject release/bad/wrong-chip.bin release/bad/wrong-project.bin
```

## Factory reset

There are two ways:

- **System → Factory reset…** in the web UI. You must type `RESET` to confirm.
- Hold the **BOOT** button (GPIO0) for **10 s** while the device is running. The serial log counts down. Releasing the button early cancels the reset.

Both erase the whole NVS partition (WiFi credentials, admin password hash, recovery AP password, all settings) and format LittleFS, then reboot into the setup AP. The firmware in both OTA slots is kept.

## CAN tap (Phase 2)

### Wiring (classic ESP32)

| Signal | GPIO | | Signal | GPIO |
| --- | --- | --- | --- | --- |
| SPI SCK | 18 | | CS #1 INV | 25 |
| SPI MOSI | 23 | | CS #2 **A (tap)** | **26** |
| SPI MISO | 19 | | CS #3 B | 27 |
| INT #1 INV | 36 | | INT #2 **A (tap)** | **34** |
| INT #3 B | 35 | | BOOT button | 0 |

The pin map lives in `components/can/include/can/pins.h`. The INT lines were changed on 2026-11-05 to match the bench wiring; the originally approved map had INV 34, A 35, B 36. Phase 2 uses only **module #2** (port A) as the tap.

- The common 5 V MCP2515 boards need a **level shifter** on SCK, MOSI, MISO and the CS lines. Each INT line needs a **divider** (for example 10k/20k).
- Add **10k pull-ups** to 3.3 V on the CS lines.
- The crystal is **8 MHz**, so the supported bitrates are 125, 250 and 500 kbit/s. 1 Mbit/s is impossible at 8 MHz.
- SPI runs at 1 MHz.
- Optional extra safety: on the tap module, tie the TJA1050's pin 8 (S) to VCC. That puts the transceiver in silent mode, so it cannot drive the bus whatever the firmware does.

### SPI guard: what each port may do

Every SPI command to an MCP2515 goes through that module's `mcp2515::Guard`. Its policy is fixed by the port in the code, not by a setting.

| | Port A (Blue Nova tap) | Port B (KNY) | Port INV (inverter) |
| --- | --- | --- | --- |
| Modes | configuration, listen-only, sleep | also **normal** (acknowledges frames) | also **normal with one-shot (OSM)**: a frame that isn't acknowledged is never retried |
| LOAD TX / RTS | refused | refused: B never sends a data frame | only the allow-list below |
| TX buffer registers, TXnRTS pins | refused | refused | refused (frames are loaded only through LOAD TX) |

**INV allow-list:** standard data frames 0x351 (DLC 8), 0x355 (4), 0x356 (6), 0x359 (7), 0x35C (2), 0x35E (8). Each LOAD TX must be a complete frame from the start of a buffer. Each RTS must name one buffer that was just loaded that way. Each ID may be requested at most once per 1000 ms (the firmware sends every 1.05 s).

- Loopback mode, and bit-modify of CANCTRL or TXRTSCTRL, are refused on every port.
- After configuring a chip, the driver reads its mode back, and checks it again every second. A chip in any other mode is forced back to configuration mode.
- *Blocked TX cmds* on the CAN page counts refused commands. It must stay 0.
- `tools/test_mcp2515.cpp` checks all of this exhaustively: every CANCTRL value on every port, every register write, every LOAD TX and RTS form, every ID × DLC, and the rate limit.

### Using it

The tap is **off by default**. While it is enabled, an OTA image only passes its self-test if the MCP2515 is up in listen-only mode. To use it, open **CAN**, tick *Enable the tap*, choose the bitrate and save.

- **Check modules** (CAN page; also runs at every boot and is written to the log) tests all three MCP2515 modules: reset values over SPI, register read-back, CS isolation, the INT line, and a 1 s listen-only receive at the configured bitrate. It never transmits. The tap pauses for about 4 s.
- **Map INT pins** (CAN page, Modules card; serial console: type `intmap` and Enter in `pio device monitor`) is **bench only**. It finds the GPIO each module's INT line is really wired to. Each module in turn is put alone in listen-only mode with a catch-all mask; every other module stays in reset. When a test frame (ID 0x100–0x1FF, from `tools/bench_can_check.py --bench --tests 4`) has arrived, every candidate GPIO is sampled before, while and after the RX flag is pending. A pin counts only if it goes high → low → high. This repeats 5 times per module. Candidates: the pins.h INT pins, 34, 35, 36, 39 and the free pins 4, 13, 14, 16, 17, 21, 22, 32, 33 (read only, never driven). The result is a table (module, CS, INT found, INT in pins.h, per-round pins). If the wiring differs, a proposed `pins.h` mapping is shown; **pins.h is never changed automatically**. Modules with no SPI answer, no test frame or no INT get a list of things to check by hand. The device never transmits; frames outside 0x100–0x1FF are ignored and counted. Up to about 20 s per module; the tap pauses meanwhile. Serial console also accepts `probe` and `help`.
- **Receive filter:** every port accepts all *valid* standard and extended frames (RXM=00 with open masks and filters). Firmware up to 0.1.7 used RXM=11, which also stores the fragments of frames that had a bus error. Those fragments showed up as phantom frames, for example 0x71F with eight zero bytes on port INV at the first transmit. Bus errors are counted, not stored.
- **Detect bitrate** tries 125, 250 and 500 kbit/s, about 1.5 s each, in listen-only mode. A rate with clean frames is offered as a suggestion; nothing is changed until you apply it.
- **Live frames** lists each ID with its count, period, age, DLC and data, plus the per-byte min/max since the last reset. Bytes that changed since the previous refresh are highlighted.
- **Capture** streams every frame to the browser over a WebSocket. *Save capture* writes CSV, Vector `.asc` or a candump log. *Save device buffer* downloads the last ~1500 frames the device holds in RAM. Timestamps are wall-clock once NTP has synced, otherwise time since boot.

## Aggregator

Open **Aggregator** in the web UI. Ports and sources:

| Port | Connects to | Bitrate (default) | Mode in 0.1.6 |
| --- | --- | --- | --- |
| INV | Sunsynk inverters | 250k, settable | listen-only; transmits the output only when *Inverter transmit* is on |
| A | Blue Nova bus (source A) | 250k | listen-only tap; TJA1050 pin 8 to VCC (hardware-silent) |
| B | KNY-P51300 (source B) | 500k, settable | listen-only; with *Acknowledge on B* it ACKs but never sends a data frame |

**Per-source caps** (Aggregator settings; defaults Blue Nova 100 A, Blue Nova pack 2 100 A, KNY 100 A, both directions): each source contributes at most min(own limit, cap) to the summed CCL/DCL, and the cap sets its over-current guard limit.

**Pack 1** of the Blue Nova is shown explicitly: current = bank − pack 2, limits = bank − pack 2, SOC = 2 × bank − pack 2 (only for a two-pack bank, at least 150 Ah, assuming two equal packs).

**Sources.** Enable source A, source B or both. With one source, the output is that battery's values with the user maximums and every fail-safe rule applied (simulator-tested for each case).

**Acknowledge on B** (CAN page, Ports card; off by default). A CAN frame needs at least one other node to acknowledge it. If the KNY is alone on its bus, tick this. If B then sees bus errors (TEC or REC at 96, or more than 20 receive errors a second, for example at a wrong bitrate), it drops back to listen-only by itself and says so, until the port settings change.

Each port's bitrate is set on the **CAN** page (*Ports* card) and applied at once, without a reboot. Valid rates are 125k, 250k and 500k. 1 Mbit/s is rejected: an MCP2515 with an 8 MHz crystal cannot run it.

### Decoding

- **Blue Nova (A):** 0x351 limits, 0x355 SOC/SOH (0.01 % SOC from bytes 4-5), 0x356 V/I/T, 0x35A (baseline), 0x35F, 0x373 cells and temperatures (K), 0x374-0x377, 0x378 energy, 0x379 capacity, and pack 2 (0x211 starting 0x02, 0x212-0x217: V, I, SOC, limits, energy, cells), as verified in `analysis/2026-11-05/findings.md`. Pack 1's own limit is the master's limit minus pack 2's.
- **KNY (B), Pylontech V1.3 layout (assumed, not yet seen on a real KNY):** 0x351 limits, 0x355 SOC/SOH (%), 0x356 V (0.01 V) / I (0.1 A, + = charging) / T (0.1 °C), 0x359 protection and alarm bits, 0x35C charge/discharge enable and force-charge request, 0x35E name. The KNY sends no capacity, so it is a setting (default 300 Ah).

### Combining

| Field | Rule |
| --- | --- |
| CVL | min of the sources. A fall passes at once; a rise only after the sources have stayed above the output for the **CVL rise hold** (default 60 s), and then only to the lowest value seen in that window |
| DVL | max |
| CCL, DCL | sum of each source's own limit, then: 0 if any source has 0 or has charge/discharge disabled (one DC bus: the inverter cannot charge one battery alone); then the **pack-share cap**; then the user maximum |
| SOC | capacity-weighted mean |
| SOH | min |
| Voltage / current / temperature | mean / sum / max |
| Alarms | OR; capacity and energy counters: sum |

**Pack-share cap.** A summed limit lets a pack that carries more than its share exceed its own limit: in the capture, Blue Nova pack 2 ran at 33 A against its own 30 A for 104 s while the bank was inside its summed 60 A. The gateway learns each pack's share of the bank current (Blue Nova pack 1, pack 2, KNY) at 20 A or more and limits CCL/DCL to `min(own limit / share)`. It can only lower the output, never raise it. Replayed on the 3 Nov capture, it holds the bank at about 46.6 A, which keeps pack 2 at about 29.8 A, inside its own 30 A.

### Fail-safe (always on, not settable)

- **Frame timeout:** each Blue Nova 1 Hz frame 5 s, each pack-2 frame 6 s, each KNY frame 5 s. The 0x305 keep-alive is not watched. A stale source puts the output into **fail-safe**: CCL = DCL = 0, CVL/DVL = the fail-safe values, the comms-fail alarm bit set, and frames still computed every second, so the inverter is told to stop rather than left to time out.
- **Start-up and recovery hold:** limits stay at 0 until every enabled source has delivered valid frames for the hold time (default 10 s). This also applies after a stale period or a cleared trip.
- **Alarm trip:** any KNY 0x359 bit trips while set. Undefined 0x359 bits and a Blue Nova 0x35A change **latch** until cleared on the Aggregator page (type CLEAR) or by a power cycle, but only after **3 consecutive identical frames**: one corrupt frame never latches.
- **Capacity step** (0x379, both batteries, e.g. 100 → 200 Ah when a Blue Nova pack joins): a `capacity_step` warning event and a recovery hold (the start-up hold time, zero limits), then the new capacity is used. A new value counts only when it arrives in two consecutive frames. A zero or implausible capacity (> 2000 Ah) makes the source invalid (fail-safe, not latched) until a sane value arrives.
- **Frame validation:** each source accepts only the IDs and DLCs that battery sends (Blue Nova: the 0x35x/0x37x set, 0x210–0x217; KNY: 0x351/355/356/359/35C/35E/373/379 with DLC 8 or the Pylontech lengths; 0x305 on either bus is ignored). Anything else is counted (*rejected*), as are extended and remote frames (*malformed*). Both show on the Aggregator page and over MQTT; a `frame_rejected` event is logged at most once a minute per source.
- **Over-current guard** (per source: Blue Nova bank, Blue Nova pack 2, KNY; per direction): limit = min(own reported limit, user cap). Above 105 % for more than 3 s, that direction of the output is halved (`overcurrent_derate` event), held at least 30 s and released after 30 s below 90 %. Still above 15 s later, or above 120 % at any time: that direction is 0 for the recovery hold, then back to 50 %. Below a 1 A limit the output is already 0 for that direction; current against it is only logged. On the 6-day Blue Nova capture it triggers 4 times, each time pack 2 charging above its derated 30 A at the top of charge; on the 13/14 Nov captures never.
- **Fail-safe voltages follow the pack:** in fail-safe and start-up hold the CVL is never below, and the DVL never above, the last good pack voltage (the configured values otherwise).
- **Plausibility:** an implausible limit (CVL/DVL outside 40-60 / 36-60 V, negative or > 1000 A, DVL ≥ CVL) trips fail-safe at once. An implausible measurement is ignored, so its frame goes stale after the timeout.
- **Refused:** until the fail-safe CVL and DVL and the CCL and DCL maximums are set, the output is refused (silent).
- **OTA:** the output is silent during a firmware update. **Watchdog:** the CAN task, which also runs the aggregator, is on the task watchdog.
- **Fail-safe action** (setting): *fail-safe values* (default) or *silent*. Silent asks for a confirmation.

### Events and MQTT

Events (state changes, stale, recovered, trips, alarms, limit clamps, CVL changes, rejected frames, capacity steps, over-current derates) are shown on the Aggregator page, written to the log, and published to `<base>/events`.

**MQTT** (System page): broker host, port, user, password (stored in NVS, never exported), base topic, interval, retain, and Home Assistant discovery. Topics are `<base>/availability` (last will), `<base>/state`, `<base>/reason`, `<base>/merged/*`, `<base>/battery_a/*` (including `pack1_*` and `pack2_*`) and `<base>/battery_b/*` (including cells, capacity, `charge_enable`/`discharge_enable`). Each battery and Blue Nova pack has `energy_in`/`energy_out` counters integrated from V × I (kWh, saved to NVS every 15 min, so a reboot loses at most 15 min), plus `guard_charge`/`guard_discharge` (ok, derated 50%, zero) and `rejected_frames`. Energy counters use `state_class: total`, because the Blue Nova's charged counter steps back at full SOC. Values are only published once a source has delivered them. Plain MQTT only: there is no TLS in this build.

### Transmit to the inverter

Off by default. On the Aggregator page, tick *Transmit the output to the inverter*. A dialog explains what happens, and you must type `TRANSMIT` to confirm. The setting is kept across reboots. It needs port INV enabled and a complete aggregator configuration.

- Port INV then runs in normal, one-shot mode. Every 1.05 s it sends the six frames in the allow-list, one at a time through one TX buffer.
- Whatever the output state says is what is sent: start-up hold (CCL = DCL = 0) until every enabled source has been valid for the hold time, fail-safe values when a source is stale or tripped, nothing when the output is refused or *silent*, and nothing during OTA.
- Disabling port INV or changing its bitrate switches transmit off. The settings import and the settings sections can't switch it on.
- The Aggregator page shows frames sent and failed and port INV's TEC. With no inverter acknowledging, each frame fails once and isn't retried.
- Only one device may act as the battery on the inverter bus. Disconnect the battery's own link to the inverter first.
- The frames sent (acknowledged) are uploaded with Verbose Diagnostics, tagged TX (`can::kFlagTx`); the receiver's exports mark them (`dir` = tx in CSV, `Tx` in .asc, channel `can0tx` in candump). What is transmitted does not change.

### Simulator tests

```sh
g++ -std=c++17 -Wall -Wextra -Werror -I components/agg/include tools/test_agg.cpp components/agg/src/engine.cpp -o /tmp/ta && /tmp/ta
g++ -std=c++17 -O2 -I components/agg/include tools/sim_agg.cpp components/agg/src/engine.cpp -o /tmp/sa
for f in analysis/data/can_*_p*.csv.gz; do zcat "$f"; done | /tmp/sa      # replay 6 days of Blue Nova capture
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I components/agg/include tools/replay_asc.cpp components/agg/src/engine.cpp -o /tmp/ra
/tmp/ra <kny.asc> --kny-checks      # KNY capture (Vector .asc): first 300 s and Fri 13:14-13:44, KNY only
g++ -std=c++17 -O2 -I components/agg/include tools/replay_engine.cpp components/agg/src/engine.cpp -o /tmp/re
uv run --no-project --with pandas --with matplotlib python -I tools/test_replay_exports.py --replay /tmp/re
```

`test_replay_exports` replays the two real receiver exports of 13/14 Nov (both batteries, the 13 Nov corruption bursts and capacity step, the night) through the engine, corrupt frames included, and checks the 0.1.7 behaviour: no latched trip, one capacity-step hold, no guard derate, the expected limits.

Two more host tests (build lines in the file headers):

- `tools/test_output_independent.cpp` runs the real aggregator glue against host stubs. It checks that the frames port INV would send equal an independent reference engine at every 100 ms tick, while the web/MQTT snapshot works, fails to allocate, is empty, is cut off or its copy is overwritten. It also checks that the control path never allocates heap. Inputs: `tools/test_replay_exports.py --write-input DIR` (the real exports), or a built-in synthetic scenario.
- `tools/test_status_json.cpp` checks the streamed `/api/agg/status` and `/api/can/status` replies: valid JSON, the fields the pages read, escaping, 256-byte chunks, heap per request (one ~5 KB allocation for the aggregator, none for CAN). It also measures the settings reply (a cJSON tree, page load only): at most 9.5 KB peak with every string at its maximum length.
- `tools/heap_model.cpp` models free heap and the largest block over 10 minutes, with the Dashboard, Aggregator and CAN pages open, MQTT on and Verbose Diagnostics uploading. It covers 0.1.7 and 0.1.8 and the Diagnostics buffer sizes. Our own allocations are measured by running the firmware code; WiFi, lwIP, httpd and the HTTP client are estimates (listed in the file).

**Heap watchdog.** The device samples free heap and the largest free block every second. It logs lows and checkpoints (start, boot done, Verbose Diagnostics buffers, MQTT discovery, first aggregator status, 60 s, 10 min), and shows them on the Dashboard and in `/api/status` (`heap_min_largest`, `heap_low_episodes`, `heap_marks`). MQTT publishes `heap_free` and `heap_largest`. While the largest block is below 12 KB, MQTT discovery waits. The watchdog never reboots the device, because a reboot silences the inverter output.

`replay_asc` reads a Vector ASCII log (`date` header, absolute or relative timestamps, hex IDs), replays it with the KNY as the only source, and prints an ID inventory (count, period, largest gap, DLCs). `--kny-checks` runs the checks for the 20 h KNY capture: decoded 195/195 A and 57.6 V, the output at 1000 A and at a 100 A user maximum, CCL 97.5 A at 13:15:55, CCL 0 from the 13:16:09 frame to 13:44, no stale event unless a required frame really pauses for more than 5 s, and 0x305 gaps having no effect. Free replay: `--from`/`--to` (seconds or `Fri 13:14`), `--cap`, `-v`.

`test_agg` simulates both batteries in 100 ms steps, and each battery alone. It checks decoding, boot with a missing source, stale sources (5 s / 6 s), the CVL rise hold and chatter, the limit clamps and the share cap, and every failure mode at the inverter output: refused, OTA, silent, 0x35A trip, 0x379 trip, KNY protection and warning bits, undefined bits, implausible values, and the frame layout. For KNY only and BN only it checks that the output is that battery's values (the other port's frames are ignored), the clamps, the CVL hold, stale, recovery, boot and OTA. With no source enabled the output is refused. `sim_agg` replays the capture. Its stale events all coincide with logger gaps, and it makes 37 CVL output changes per day, against the analysis's 36 for a 60 s hold.

## Bench test with a CANable

`tools/bench_can_check.py` sends known traffic from a CANable (slcan firmware) to check each MCP2515 module end to end. **It transmits**, so use it only on an isolated bench bus: the CANable plus aggregator ports, and nothing else (tests 1–3: one port at a time; tests 4–6: all three modules on one bus). Never with a battery or inverter connected.

```sh
uv run tools/bench_can_check.py --quiet-only [--port /dev/ttyACM0] [--bitrate 250000]   # never transmits, no login
AGG_PASSWORD='...' uv run tools/bench_can_check.py --bench [--port /dev/ttyACM0] [--bitrate 250000] [--tests 1,2,3]
AGG_PASSWORD='...' uv run tools/bench_can_check.py --bench --tests 5,6   # B acknowledges; INV transmit
```

- `AGG_HOST` (default `192.168.0.59`) and `AGG_PASSWORD` are read from the environment only. The password and the session cookie are never printed.
- **Termination:** the CANable's 120 ohm jumper is on, so put the module's 120 ohm jumper on as well (about 60 ohm across CAN-H/CAN-L, measured with the power off).
- **Safety:**
  - Before every transmit session, the script listens for 2 s with `M1`, then `O`. **`M1` is not silent on this firmware:** measured on 2026-11-10, the CANable still acknowledged frames. It sends no data frame in that window, but it does drive ACK bits. If it sees any frame, or the CANable's error register is not 0, it aborts with "bus is not quiet, refusing to transmit".
  - It sends only on IDs 0x100–0x1FF, at most 100 frames/s, and never on the BN/Pylontech IDs.
- **Tests:**
  1. **Per-port RX.** The script asks you to move the CANable to INV, then A, then B. For each port it runs *Check modules* while sending. It passes if that port reports frames > 0 and 0 errors. *Check modules* listens at the configured CAN bitrate, so `--bitrate` must match that setting.
  2. **Content on the tap (port A).** Enable the tap first. The script sends N unique frames, downloads the device buffer (`/api/can/buffer`), and checks that every frame is there, in order, with plausible timestamps.
  3. **Bitrate detection on port A.** For each of 125k, 250k and 500k it sends at that rate, runs *Detect bitrate*, and passes if the suggestion matches. The tap must be enabled.
  4. **Map INT pins** (`--tests 4`, not in the default set). The CANable and all three modules on **one** bench bus. After the quiet check and warm-up it starts *Map INT pins* (or, if that is already running from the web UI or serial console, just supplies frames), sends test frames at about 25 frames/s until the mapping ends, and prints the table and any proposed pins.h change. A module passes only if one GPIO matched in all 5 rounds and it equals pins.h.
  5. **Port B acknowledges** (`--tests 5`). (a) With the CANable closed, port INV sends a few frames: none is acknowledged while B is listen-only, and every one is once B is in ACK mode. (b) The CANable sends 200 frames (0x100–0x1FF): each is stored exactly once on B and on A, and B's TEC and REC stay 0. (c) The CANable receives no frame from the device. (d) B in ACK mode at a wrong bitrate must drop back to listen-only by itself. The port settings are restored afterwards.
  6. **Port INV transmit** (`--tests 6`). The device sends; the CANable only acknowledges or is closed. (a) Enabling without the typed word is refused. (b) With the CANable acknowledging, for 8 s: only the six allow-listed IDs with their DLCs, each at least 0.85 s apart, CCL = DCL = 0 and the fail-safe CVL/DVL (there is no battery on the bench), and no failed frames. (c) With nothing acknowledging, there is one attempt per frame. (d) After disabling, the bus is quiet. Transmit is left off. On the shared bench bus, ports A and B hear the device's own frames, so the aggregator may show source A or B as stale afterwards.
- **What the CANable firmware does and doesn't acknowledge.** This was checked on the canable2 slcan firmware `16e7497-dirty` (github.com/normaldotcom/canable2) on 2026-11-05.
  - **No acknowledgements:** it never answers `O`, `C`, `S<n>`, `M<n>`, `A<n>` or frames. There's no `\r`, no BEL and no `z`.
  - **Only `V` and `E` answer.** `V` gives the version line, and the script uses it as the liveness check. `E` gives `CANable Error Register: <hex>` without a trailing CR, and it's the only state readback.
  - **No `L` command.** It's silently ignored. Listen-only is `M1`, and it must be sent before `O`.
  - **No command reports** open/closed, mode or bitrate.
  - **A bare `\r` re-runs the previous command.** After a transmit, that would send the frame again. The script never sends an empty line or `\n`, sends only `C O S4-S6 M0 M1 A0 V E` and 8-byte frames, and never sends `X` (it falls through to a transmit).
  - **Consequence:** the CANable's state can't be verified directly, so the script prints at the end which checks were *verified* and which were *assumed*. A channel that never opened would also pass the quiet check. The end-to-end proof is test 1: the aggregator receiving the frames shows the CANable opened at that bitrate.
- **Nothing ACKs on this bench (tests 1–4).** The aggregator is listen-only and there is no other node. The script turns auto-retransmit off (`A0`; this can't be confirmed on the CANable), then sends ~24 warm-up frames on 0x1FF so the CANable becomes error passive. After that, each frame reaches the bus once. If `A0` doesn't take effect, the CANable resends its first frame forever, and test 2 fails. All checks use unique ID and payload matching, never exact counts.
- The exit code is 0 if everything passes, 1 if any test fails, and 2 for a safety or usage abort.

## Verbose Diagnostics and the reference server

Verbose Diagnostics is a switch on the System page and is off by default. When it is on, the device posts CAN traffic in batches to a server you run.

- **Batching:** a batch is sent when it reaches the batch size (default 500 frames) or the flush interval passes (default 30 s), and immediately on an event.
- **RAM:** the three batch buffers are sized by the batch size when uploads are first enabled: 3 × batch × 24 bytes. That is 36 KB at 500 (the default), 18 KB at 250 and 9 KB at 125. Free heap is tight with 500, so 250 is a good choice when MQTT and the web UI are in use (see `tools/heap_model.cpp`). Lowering the batch size takes effect at once (the buffers stay as allocated); a larger one takes effect after a reboot.
- **Event batches** copy the pre-trigger window from the CAN ring (up to 36 KB). The copy never takes the largest free heap block below 16 KB. If it would, only the newest frames are kept and the batch is marked truncated.
- **When the server is down:** each batch stays in RAM until the server answers HTTP 200. Three batch buffers hold about 1500 frames (at batch size 500). Once they are full, the oldest waiting batch is dropped. The next batch that gets through reports how many frames were dropped.
- **Batch contents:** each batch carries a sequence number (unique per device and boot) and says whether the clock was NTP-synced. The format is in `components/diag/include/diag/batch_format.h`.
- **Change-only mode:** only frames whose data changed, plus a full snapshot of every ID once a minute.
- **Event trigger:** fires when a chosen frame leaves its baseline. The default is the Blue Nova 0x35A pattern `A8AA8202A8AA0202`, but the trigger itself is off by default.
  - The event is logged, and the last N seconds of frames (pre-trigger, default 60 s) are uploaded at once as an event batch.
  - The trigger latches until the frame returns to its baseline.
  - *Send test event* raises an event by hand.
- **Task priority:** the uploader runs in its own low-priority task and never blocks CAN reception.
- **Token:** the upload token is a secret. It is never shown or exported.

**Reference server** (`server/`): a receiver API with SQLite storage, Python standard library only.

```sh
cd server
cp .env.example .env              # set BMS_TOKEN
docker compose up -d --build      # listens on :8080, data in server/data/
# without Docker:  BMS_TOKEN=... BMS_DB=./data/bms.db python3 receiver.py
```

On the device, set the URL to `http://<server>:8080/api/v1/batches` and use the same token.

- The server ignores duplicate batches.
- It back-fills wall-clock time for frames that were sent before the device's clock synced.
- It exports CSV, candump or `.asc`. `tools/export_receiver_db.py` exports straight from the database. Its `copy` column says whether a row is a live frame or a once-a-minute **snapshot** re-send of an ID's last frame (change-only mode). A snapshot keeps the frame's original timestamp, give or take a few µs, so one frame can appear several times.
- Aggregator events (trips, stale, guard, capacity steps) are **not** uploaded yet; they are in the device log, on the Aggregator page and on MQTT. Upload is planned for 0.1.9 (`analysis/plan_0.1.9_event_upload.md`).

```sh
curl -H "Authorization: Bearer $BMS_TOKEN" "http://<server>:8080/api/v1/stats"
curl -H "Authorization: Bearer $BMS_TOKEN" "http://<server>:8080/api/v1/batches?kind=event"
curl -H "Authorization: Bearer $BMS_TOKEN" -o cap.asc "http://<server>:8080/api/v1/export?format=asc"
```

## Partition layout

The partition tables are `partitions/4MB.csv` (default) and `partitions/8MB.csv`. The 4 MB layout:

| Region | Offset | Size | Notes |
| --- | --- | --- | --- |
| bootloader | 0x1000 | up to 28 KB | about 26 KB with rollback support (ESP32-S3: at 0x0) |
| partition table | 0x8000 | 4 KB | |
| `nvs` | 0x9000 | 20 KB | settings and secrets; enough for many hundreds of small entries |
| `otadata` | 0xE000 | 8 KB | which slot boots, and each image's verify state |
| `ota_0` | 0x10000 | 1792 KB (0x1C0000) | app slot A |
| `ota_1` | 0x1D0000 | 1792 KB (0x1C0000) | app slot B |
| `littlefs` | 0x390000 | 448 KB (0x70000) | web UI (gzipped), later config and profiles |
| **end** | 0x400000 | | exactly 4 MB |

How the sizes were chosen:

- App slots must start on a 64 KB boundary. Two equal slots are needed so any image can go into either slot.
- The Phase 1 app is about 0.9 MB, which is 49% of a slot. That leaves roughly 0.9 MB of headroom for CAN, MQTT with TLS, WebSockets and the profile engine in later phases.
- LittleFS gets what is left. The gzipped web UI is about 10 KB today, and profiles and configs are small JSON files, so 448 KB is plenty.
- There is no factory partition. The two OTA slots plus rollback replace it, and after a USB flash the image runs from `ota_0`.
- There is no coredump partition yet. One could take 64 KB from LittleFS later.

The 8 MB layout uses the same small partitions, two 3 MB app slots (`0x10000` and `0x310000`) and about 1.9 MB of LittleFS.

## Changing chip or flash size

Chip and flash size live in **one place**, the `[target]` section of `platformio.ini`:

```ini
[target]
board = esp32dev          ; esp32-s3-devkitc-1 for an ESP32-S3
flash_size = 4MB          ; 8MB uses partitions/8MB.csv + config/sdkconfig.flash_8MB
```

After you change either value, run `pio run -t fullclean` once, because the generated `sdkconfig.<env>` and the CMake cache are specific to the chip. Then flash over USB again, since the partition table changes. A new flash size needs a matching `partitions/<size>.csv` and `config/sdkconfig.flash_<size>`.

## Web UI and API

The UI is plain HTML, CSS and JS in `web/`. There is no build step and no CDN. It works on mobile and follows the system light or dark theme; the ◐ button switches it.

- The files are gzipped and embedded in the firmware by `tools/pack_www.py`. At boot they are copied to LittleFS (`/littlefs/www`) whenever they changed, and they are served from there. A single OTA `.bin` therefore updates firmware and UI together. If LittleFS is unusable, the embedded copy is served instead.
- **Pages:** Dashboard, System, Firmware, Log (the last ~200 log lines).
- **Security:** a login is required for everything except the setup page while the device is not provisioned. Logins use a session cookie (HttpOnly, SameSite=Strict, 30 min idle, 12 h maximum). Failed logins are rate limited per IP: after 5 failures, a lockout that doubles each time up to 15 min, plus a global limit. Every POST must carry `X-BMS-Request: 1`.

API (all JSON):

| Method and path | Auth | Purpose |
| --- | --- | --- |
| GET `/api/health` | none | `{ok, boot_id}` for reboot detection and the self-test |
| GET `/api/session` | none | `{setup, authenticated}` |
| POST `/api/login`, `/api/logout` | none | session cookie |
| POST `/api/setup`, GET `/api/setup/status`, `/api/setup/scan` | setup mode only | first-boot provisioning |
| GET `/api/status` | login | dashboard data |
| GET, POST `/api/settings` | login | hostname, NTP, timezone, require-signed setting |
| GET `/api/config/export`, POST `/api/config/import` | login | config as JSON, without secrets |
| POST `/api/wifi`, GET `/api/wifi/scan` | login | change WiFi |
| POST `/api/password` | login | change admin password |
| POST `/api/reboot`, `/api/factory-reset` | login | factory reset needs `{"confirm":"RESET"}` |
| GET `/api/ota/info`, POST `/api/ota/upload`, `/api/ota/rollback` | login | firmware |
| GET `/api/log` | login | recent log lines (text) |
| GET `/api/can/status`, `/api/can/table`, `/api/can/buffer` | login | tap status, live table, RAM buffer (binary) |
| POST `/api/can/detect`, `/api/can/probe`, `/api/can/table/reset` | login | bitrate detection, module hardware check, reset the table |
| WebSocket `/ws/can` | login | live capture stream (binary) |
| GET `/api/diag/status`, POST `/api/diag/token`, `/api/diag/test-event`, `/api/diag/clear-trigger` | login | Verbose Diagnostics |

The CAN, diagnostics and trigger settings are part of `/api/settings` and of the config export. The token is never included.

## Project layout

```
VERSION                 version, the single source of truth
platformio.ini          build config; [target] = chip + flash size
CMakeLists.txt          reads VERSION into PROJECT_VER, picks sdkconfig for the flash size
sdkconfig.defaults      common ESP-IDF settings (rollback, watchdog, logging, ...)
config/                 per-flash-size sdkconfig fragments
partitions/             partition tables (4MB.csv, 8MB.csv)
main/                   app_main, post-OTA self-test, BOOT-button factory reset
components/
  config/               NVS settings and secrets, password hashing, LittleFS mount
  net/                  WiFi state machine (setup AP / station / recovery AP), captive DNS, mDNS, NTP
  webserver/            HTTP server, sessions, rate limiting, JSON API, static files
  ota/                  streamed OTA, image checks, rollback, signature hook
  applog/               ring buffer of recent log lines (named applog: "log" would replace ESP-IDF's own)
  can/                  MCP2515 driver + per-port SPI guard, CAN task (tap, INV/B, INV transmit), ring buffer, ID table
  diag/                 Verbose Diagnostics batches and uploader
web/                    web UI sources
tools/                  asset packer, PlatformIO pre-build hook, bad-image generator, host tests
server/                 reference server: receiver.py (SQLite), Dockerfile, docker-compose.yml
scripts/release.sh      release build named from VERSION
```

`dependencies.lock` pins the managed component versions. Keep it with the sources. `.pio/`, `managed_components/`, `sdkconfig.<env>`, `components/webserver/generated/` and `release/` are generated.
