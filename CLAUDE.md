# CLAUDE.md

BMS CAN aggregator gateway on an ESP32. The design is in `SPEC.md`, and the user-facing docs are in `README.md`.
The project is built in phases (SPEC.md "Build order"). Done: provisioning, web UI, OTA with rollback (Phase 1), listen-only tap and diagnostics (Phase 2), in 0.1.5 receive on all three ports plus the aggregator (decode, combine, fail-safe, events, MQTT), in 0.1.6 the owner-approved transmit path on port INV and ACK mode on port B (see the SPI guard rule below), and in 0.1.7 the per-source over-current guard and caps, capacity-step hold, frame validation, pack 1, KNY telemetry, energy counters and TX upload.

## Hard rules

- **No git actions at all.** That includes read-only ones: no init, add, commit, branch, push, log, status, diff or rev-parse. The user handles git.
- **The fail-safe behaviour in SPEC.md must never be made optional.** The "Always on (cannot be disabled)" rules have no setting, flag, build option or debug switch that turns them off: no invented data, frame timeout, alarm trip, plausibility check, start-up hold, watchdog, and silent output during OTA. New settings may only choose between the safe behaviours the spec allows, such as fail-safe values or silent.
- **The SPI guard is the single gate for every MCP2515 command.** `mcp2515::Guard` (`components/can/include/can/mcp2515_regs.h`) checks every transaction; its policy is fixed per port by `kPortPolicy` (never a setting). The owner approved exactly these exceptions (0.1.6) and nothing broader:
  - **Port A (Blue Nova tap): none.** Listen-only, hardware-silent: no normal mode, no LOAD TX, no RTS, no 0x305. The tap never transmits.
  - **Port B (KNY): normal mode only**, so the controller acknowledges frames (setting `can_b_ack`, default off; falls back to listen-only by itself on bus errors). LOAD TX and RTS stay refused: B never sends a data frame.
  - **Port INV (inverter): normal mode with one-shot (OSM) only; LOAD TX only for the allow-list 0x351/8, 0x355/4, 0x356/6, 0x359/7, 0x35C/2, 0x35E/8 (standard data frames, the DLC fixed per ID, full frame from the buffer start); RTS only for a single buffer just loaded that way, at most once per ID per 1000 ms.** Transmit is off by default (`agg_tx_enabled`), switched on only by `POST /api/agg/transmit` with the typed confirmation `TRANSMIT` (Aggregator page dialog), never by the settings sections or an import; disabling port INV or changing its bitrate switches it off. The start-up hold (zero limits) and every fail-safe rule apply to what is sent; OTA silences it.
  - Everywhere: TX buffer registers and TXRTSCTRL pin triggers are never writable, CANCTRL/TXRTSCTRL never bit-modified, loopback never allowed.
  Do not widen any of this, add IDs, add ports, or add a transmit path elsewhere without the owner's explicit approval. `tools/test_mcp2515.cpp` (every port, every CANCTRL value, every register, every LOAD TX/RTS form, every ID x DLC, the rate limit) must keep passing. Bitrate detection runs in listen-only mode only.
- Leave `LICENSE` untouched and do not add another licence file.
- No default password, ever. Secrets stay in NVS only, and the admin password is stored only as a salted hash. Exports never contain secrets.
- The git hash is the constant `"unknown"` (`BMS_GIT_HASH` in `components/webserver/src/api.cpp`). Do not add build steps that call git.

## Build commands

```sh
pio run                              # build (pio may be ~/.platformio/penv/bin/pio)
pio run -t upload                    # USB flash (first install only)
pio device monitor                   # serial log
scripts/release.sh                   # release/bms-agg-<VERSION>.bin
scripts/release.sh --fail-selftest   # TEST image whose self-test fails (rollback test)
tools/make_bad_images.py <bin>       # broken images for the OTA rejection test
g++ -std=c++17 -I components/ota/include tools/test_image_check.cpp \
    components/ota/src/image_check.cpp -o /tmp/tic && /tmp/tic ok <bin>   # host test
```

- The version is read from `VERSION`. Bump it by hand before an OTA test build. `tools/pio_prebuild.py` forces a CMake re-configure when it changes; PlatformIO would not do that on its own.
- Chip and flash size live only in `platformio.ini [target]`. Run `pio run -t fullclean` after changing them.
- ESP-IDF 6.x with mbedTLS 4: the legacy mbedtls md/pkcs5 APIs are gone, so use PSA (`psa/crypto.h`). The `json` component is gone too, so use `espressif/cjson`. Managed components are named `espressif__mdns`, `joltwallet__littlefs`, `espressif__cjson` and `espressif__mqtt` (esp-mqtt left IDF core in 6.x) in `REQUIRES`.
- After changing `sdkconfig.defaults`, delete `sdkconfig.bms-agg*`. Otherwise options that are already present in the generated file (even as "is not set") keep their old values.
- Host tests (run all of them after touching these areas):
  - `g++ -std=c++17 -I components/can/include tools/test_mcp2515.cpp -o /tmp/tm && /tmp/tm` checks the bit timing and the per-port SPI guard.
  - `g++ -std=c++17 -Wall -Wextra -Werror -I components/agg/include tools/test_agg.cpp components/agg/src/engine.cpp -o /tmp/ta && /tmp/ta` is the aggregator simulator (every fail-safe path; both sources, BN only, KNY only, none). `tools/sim_agg.cpp` replays `analysis/data` captures (see README; feed the files one by one, one is truncated). `tools/replay_asc.cpp <file.asc> --kny-checks` replays the owner's KNY capture (Vector .asc) against the owner's expectations. `tools/test_replay_exports.py --replay /tmp/re` (build `tools/replay_engine.cpp`) replays the 13/14 Nov receiver exports, corrupt frames included.
  - `tools/test_output_independent.cpp` compiles the real `glue.cpp` against `tools/hoststub/` and proves the INV output (`agg::tx_frames`) equals an independent reference engine at every 100 ms tick while the reporting snapshot succeeds, fails, is empty or is overwritten, and that the control path never allocates (inputs: `tools/test_replay_exports.py --write-input DIR`; synthetic without arguments). `tools/test_status_json.cpp` checks the streamed `/api/agg/status` and `/api/can/status` replies and measures the settings tree. `tools/heap_model.cpp` is the 10-minute heap model (three pages open, MQTT, diag uploads). Build lines are in the file headers.
  - `tools/test_batch.cpp` writes batches with the firmware's encoder for `server/receiver.py` (see the README).
- Warnings are errors (for example `-Werror=format-truncation`).
- PlatformIO compiles with SCons from the CMake code model, so CMake `add_custom_command` steps do not run. Generate files in `tools/pio_prebuild.py` instead.

## Architecture

- `main/main.cpp` runs, in order: `applog::init` → `cfg::init` (NVS, LittleFS) → `webserver::sync_assets` → `net::start` → `webserver::start` → `selftest::start` → `button::start`.
- **config**: NVS namespace `bms`. Settings are hostname, NTP server, timezone and require-signed. Secrets are the WiFi password, the recovery AP password, and the admin PBKDF2 salt, hash and iteration count. The `prov` flag is written last during provisioning.
- **net**: a single task owns the WiFi state machine. Modes are Setup (open AP plus captive DNS), Station, and FallbackAP (WPA2, after 5 failures or 30 s without an IP, which is below the 60 s self-test window). Provisioning credentials are only saved after the station gets an IP.
- **webserver**: an esp_http_server route table with flags (`R_PUBLIC`, `R_AUTH`, `R_SETUP_ONLY`). Every POST needs `X-BMS-Request: 1`. Sessions live in RAM (4 slots). The login rate limit is per IP plus a global limit. The web UI is embedded (`generated/www_assets.c`), mirrored to `/littlefs/www/*.gz` and served gzipped.
- **ota**: `ota::Upload` buffers the first 288 bytes and runs `check_image_header` (magic, chip id and revision, app descriptor, project name) before `esp_ota_begin`. `esp_ota_end` verifies the SHA-256, then `verify_signature()` runs (the **signature hook**, fail-closed if required), then the boot partition is set. `bms_ota_activity(bool)` is a weak hook. The CAN component must implement it so the output goes silent during OTA (fail-safe rule 7).
- **self-test** (`main/selftest.cpp`): for a `PENDING_VERIFY` image it marks the image valid only when the config has loaded, HTTP answers over loopback, and STA or AP is up. Otherwise it rolls back at `CONFIG_BMS_SELFTEST_TIMEOUT_S` (60 s). When CAN exists, add "CAN controllers initialised" to the checks (SPEC.md).
- **applog**: a hook on `esp_log_set_vprintf`, with a 16 KB / 200-line ring buffer.
- **can**: pins in `include/can/pins.h`; port A (CS 26, INT 34) is the tap. Ports INV and B are only touched by the hardware check (`run_probe` in `tap.cpp`, at boot and via `POST /api/can/probe`): reset values, CNF1 read-back, CS isolation, INT line (forced WAKIF), 1 s listen-only. Afterwards they are left in reset (configuration mode). The `can` task runs on core 1 at priority 18 and is subscribed to the task watchdog (fail-safe rule 6).
  - Frames go to a 1536-frame ring (WebSocket capture, buffer download, pre-trigger), to a per-ID table (192 IDs) and to `diag::on_frame`.
  - Status strings are fixed `char` buffers, because nothing may allocate inside a `portENTER_CRITICAL` section.
  - It holds the strong definition of `bms_ota_activity`: the aggregator output goes silent and `tx_cycle()` also checks the OTA flag itself.
  - Receive set-up (`Mcp2515::start`): RXM=00 with all masks 0 and filters RXF0/2/4 standard, RXF1/3/5 extended (every valid frame). Never RXM=11: it stores error fragments (phantom 0x71F/00x8 frames on INV in 0.1.7).
  - Acknowledged INV transmits go to the ring and the diag sink with `kFlagTx` (`deliver_tx`), never to the aggregator hook or the receive table.
  - The tap is off by default. While enabled, the self-test requires listen-only mode.
  - Bench script `tools/bench_can_check.py`: tests 5 (B ACK) and 6 (INV transmit) exist. The canable2 slcan `M1` is NOT silent (it ACKs); see the script header.
  - INT mapping (`run_intmap`, bench only; `POST /api/can/intmap`, serial `intmap` via `main/serial_cmd.cpp`, bench script `--tests 4`): each module alone in listen-only, test frames 0x100-0x1FF only, samples candidate GPIOs high/low/high over 5 rounds. Only reports; never edits pins.h.
- **ports INV/B** (`tap.cpp`, `Aux`): receivers with their own bitrate and mode (`can_inv_*`, `can_b_*`, `can_b_ack`, `agg_tx_enabled`), applied on change; serviced in the `can` task; the operating mode is re-checked every second; restored after probe/INT map. INV transmit: `tx_cycle()` every 1.05 s asks `agg::tx_frames()` and sends through TX buffer 0 one frame at a time (one-shot). Frame table and ring are per (port, ID).
- **agg**: `engine.cpp` is pure C++ (time passed in, host-tested); `glue.cpp` wraps it in a mutex and is fed from the `can` task via `can::set_agg_hooks` (frames, a tick every loop, OTA). Source A = BN on port A, source B = KNY on port B. Either source alone or both. Output states: refused / silent / start-up hold / fail-safe / normal. Per-source frame validation (`frame_allowed`), latches only after 3 identical frames, capacity step = event + recovery hold (2 identical frames), per-source caps `agg_cap_*` and the over-current guard (`guard_update`, fixed parameters in engine.h, always on), fail-safe CVL/DVL bounded by the last good pack voltage, energy integrated in the engine and saved by `glue.cpp` to NVS namespace `aggE` every 15 min. Frames are sent on INV only through the guard exceptions above. Settings `agg_*` (fail-safe CVL/DVL and CCL/DCL max have no default: output refused until set). Timeouts 5 s / 6 s are constants.
- **heapmon**: heap watchdog (esp_timer, 1 s): lows, checkpoints (`heapmon::mark`), warnings; `low()` makes optional work wait (MQTT discovery). Never reboots. Free heap is tight (~36 KB with Verbose Diagnostics' 36 KB batch buffers): **polled and large replies must stream** (`JsonOut`, `components/webserver/src/json_stream.h`; `/api/status`, `/api/agg/status`, `/api/can/status`, `/api/diag/status`, `/api/log`, `/api/can/table` do), never build a big cJSON tree (the 0.1.7 `/api/agg/status` tree needed ~49 KB and failed on the device). Settings (page load only) stays a cJSON tree, measured at <= 9.5 KB. Verbose Diagnostics buffers are 3 x `diag_batch` x 24 B; event batches keep >= 16 KB largest block free. Reporting (web, MQTT) reads `agg::snapshot` copies; the INV output reads the engine directly in `tx_frames` and must stay that way.
- **hamqtt**: esp-mqtt publisher + HA discovery; its task is created only once MQTT is enabled (RAM). Password is an NVS secret (`mq_pass`), never exported.
- **diag**: three 500-frame batch buffers, allocated on first enable. The `diag` task runs at priority 2. A batch is freed only on HTTP 200; when all buffers are full the oldest waiting batch is dropped and counted. An event takes its pre-trigger window from the CAN ring and is uploaded first.
- **server/**: `receiver.py`, Python standard library only. Dedup key is (mac, boot_id, seq), and it back-fills wall-clock time per boot.
- The BOOT button (GPIO0) held for 10 s does a factory reset: erase NVS and format LittleFS.
