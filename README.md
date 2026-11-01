# Battery-Aggregator

A device that allows multiple incompatible batteries on different CAN networks to be used in a single inverter.

An open-source BMS CAN aggregator gateway on an ESP32. The full design is in [SPEC.md](SPEC.md).

**Status: Phase 2 (listen-only tap and Verbose Diagnostics).**

- **Phase 1:** the device is flashed once over USB and from then on updated over WiFi. It has WiFi provisioning, a web UI, and OTA with automatic rollback.
- **Phase 2:** one MCP2515 (port A) runs in **listen-only** mode. It provides a live frame table, capture download, listen-only bitrate detection, and Verbose Diagnostics upload to a minimal reference server.

Nothing transmits on the CAN bus yet. The inverter output and the combine engine come in later phases.

## Contents

- [Build](#build)
- [First flash over USB](#first-flash-over-usb)
- [First boot: setup](#first-boot-setup)
- [OTA updates](#ota-updates)
- [Testing rollback and rejection](#testing-rollback-and-rejection)
- [Factory reset](#factory-reset)
- [CAN tap (Phase 2)](#can-tap-phase-2)
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

### Wiring (classic ESP32, approved pin map)

| Signal | GPIO | | Signal | GPIO |
| --- | --- | --- | --- | --- |
| SPI SCK | 18 | | CS #1 INV | 25 |
| SPI MOSI | 23 | | CS #2 **A (tap)** | **26** |
| SPI MISO | 19 | | CS #3 B | 27 |
| INT #1 INV | 34 | | INT #2 **A (tap)** | **35** |
| INT #3 B | 36 | | BOOT button | 0 |

The pin map lives in `components/can/include/can/pins.h`. Phase 2 uses only **module #2** (port A).

- The common 5 V MCP2515 boards need a **level shifter** on SCK, MOSI, MISO and the CS lines. Each INT line needs a **divider** (for example 10k/20k).
- Add **10k pull-ups** to 3.3 V on the CS lines.
- The crystal is **8 MHz**, so the supported bitrates are 125, 250 and 500 kbit/s. 1 Mbit/s is impossible at 8 MHz.
- SPI runs at 1 MHz.
- Optional extra safety: on the tap module, tie the TJA1050's pin 8 (S) to VCC. That puts the transceiver in silent mode, so it cannot drive the bus whatever the firmware does.

### Never transmits

- The driver has no transmit function.
- Every SPI command goes through `mcp2515::spi_command_allowed()`. It refuses:
  - the transmit-related instructions,
  - writes to the TX buffers (including TXREQ),
  - TXnRTS pin modes,
  - any operating mode other than configuration, listen-only and sleep. Normal mode would acknowledge frames.
- After configuring the chip, the driver reads the mode back. It checks again every second, and if the chip ever leaves listen-only mode it is forced back to configuration mode.
- *Blocked TX cmds* on the CAN page counts refused commands. It must stay 0.

### Using it

The tap is **off by default**. While it is enabled, an OTA image only passes its self-test if the MCP2515 is up in listen-only mode. To use it, open **CAN**, tick *Enable the tap*, choose the bitrate and save.

- **Detect bitrate** tries 125, 250 and 500 kbit/s, about 1.5 s each, in listen-only mode. A rate with clean frames is offered as a suggestion; nothing is changed until you apply it.
- **Live frames** lists each ID with its count, period, age, DLC and data, plus the per-byte min/max since the last reset. Bytes that changed since the previous refresh are highlighted.
- **Capture** streams every frame to the browser over a WebSocket. *Save capture* writes CSV, Vector `.asc` or a candump log. *Save device buffer* downloads the last ~1500 frames the device holds in RAM. Timestamps are wall-clock once NTP has synced, otherwise time since boot.

## Verbose Diagnostics and the reference server

Verbose Diagnostics is a switch on the System page and is off by default. When it is on, the device posts CAN traffic in batches to a server you run.

- **Batching:** a batch is sent when it reaches the batch size (default 500 frames) or the flush interval passes (default 30 s), and immediately on an event.
- **When the server is down:** each batch stays in RAM until the server answers HTTP 200. Three batch buffers hold about 1500 frames. Once they are full, the oldest waiting batch is dropped. The next batch that gets through reports how many frames were dropped.
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
- It exports CSV, candump or `.asc`:

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
| POST `/api/can/detect`, `/api/can/table/reset` | login | bitrate detection, reset the table |
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
  can/                  MCP2515 listen-only driver + no-transmit guard, tap task, ring buffer, ID table
  diag/                 Verbose Diagnostics batches and uploader
web/                    web UI sources
tools/                  asset packer, PlatformIO pre-build hook, bad-image generator, host tests
server/                 reference server: receiver.py (SQLite), Dockerfile, docker-compose.yml
scripts/release.sh      release build named from VERSION
```

`dependencies.lock` pins the managed component versions. Keep it with the sources. `.pio/`, `managed_components/`, `sdkconfig.<env>`, `components/webserver/generated/` and `release/` are generated.
