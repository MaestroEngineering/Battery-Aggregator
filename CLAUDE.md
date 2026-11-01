# CLAUDE.md

BMS CAN aggregator gateway on an ESP32. The design is in `SPEC.md`, and the user-facing docs are in `README.md`.
The project is built in phases (SPEC.md "Build order"). Phase 1 is done: provisioning, web UI, and OTA with rollback. There is no CAN code yet.

## Hard rules

- **No git actions at all.** That includes read-only ones: no init, add, commit, branch, push, log, status, diff or rev-parse. The user handles git.
- **The fail-safe behaviour in SPEC.md must never be made optional.** The "Always on (cannot be disabled)" rules have no setting, flag, build option or debug switch that turns them off: no invented data, frame timeout, alarm trip, plausibility check, start-up hold, watchdog, and silent output during OTA. New settings may only choose between the safe behaviours the spec allows, such as fail-safe values or silent.
- **The CAN tap never transmits.** Do not add transmit functions to the listen-only path. Do not weaken `mcp2515::spi_command_allowed()` (`components/can/include/can/mcp2515_regs.h`): it is the single gate for every SPI command, and `tools/test_mcp2515.cpp` must keep passing. Bitrate detection runs in listen-only mode only.
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
- ESP-IDF 6.x with mbedTLS 4: the legacy mbedtls md/pkcs5 APIs are gone, so use PSA (`psa/crypto.h`). The `json` component is gone too, so use `espressif/cjson`. Managed components are named `espressif__mdns`, `joltwallet__littlefs` and `espressif__cjson` in `REQUIRES`.
- After changing `sdkconfig.defaults`, delete `sdkconfig.bms-agg*`. Otherwise options that are already present in the generated file (even as "is not set") keep their old values.
- Host tests (run all of them after touching these areas):
  - `g++ -std=c++17 -I components/can/include tools/test_mcp2515.cpp -o /tmp/tm && /tmp/tm` checks the bit timing and the no-transmit guard.
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
- **can**: pins in `include/can/pins.h`; only port A (CS 26, INT 35) is used. The `can` task runs on core 1 at priority 18 and is subscribed to the task watchdog (fail-safe rule 6).
  - Frames go to a 1536-frame ring (WebSocket capture, buffer download, pre-trigger), to a per-ID table (192 IDs) and to `diag::on_frame`.
  - Status strings are fixed `char` buffers, because nothing may allocate inside a `portENTER_CRITICAL` section.
  - It holds the strong definition of `bms_ota_activity`. The tap has no output, so there is nothing to silence yet.
  - The tap is off by default. While enabled, the self-test requires listen-only mode.
- **diag**: three 500-frame batch buffers, allocated on first enable. The `diag` task runs at priority 2. A batch is freed only on HTTP 200; when all buffers are full the oldest waiting batch is dropped and counted. An event takes its pre-trigger window from the CAN ring and is uploaded first.
- **server/**: `receiver.py`, Python standard library only. Dedup key is (mac, boot_id, seq), and it back-fills wall-clock time per boot.
- The BOOT button (GPIO0) held for 10 s does a factory reset: erase NVS and format LittleFS.
