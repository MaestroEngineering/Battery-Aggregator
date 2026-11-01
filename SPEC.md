# BMS Aggregator: Open-Source Spec (draft v0.1)

Oct 31, 2026 · @Adagio

## Overview and goals

This is an open-source CAN gateway that lets batteries of different brands and capacities, with the same cell count and chemistry, share one inverter as a single virtual battery. It sits between the batteries and the inverter, decodes each battery's CAN messages, combines them, and presents one battery to the inverter. The default input and output protocol is Pylontech-style CAN, the common lithium protocol that most inverters, including Sunsynk (protocol "BMS:0"), accept.

The first real use is Iwan's Blue Nova bank, which is discontinued, so a new battery can be added behind the same dual-Sunsynk system. The design is general on purpose, so other people can profile and combine their own batteries.

**Goals**

- Combine dissimilar batteries, chosen per field: min, max, mean, weighted mean, sum, pass A, pass B or failover.
- Default to Pylontech on both sides, with other input and output profiles added as YAML.
- OTA firmware updates first, so the device never needs a programmer once installed.
- A web UI for settings, YAML import and export, MQTT setup and a live register map.
- An optional transparent sniffer mode so anyone can profile a battery without extra hardware.
- A safe failure mode: when in doubt, send nothing.

## Scope and non-goals

The gateway only translates and combines CAN data. It never does the BMS's protection job.

- **In scope:** CAN decode, combine, re-encode, monitoring, logging, MQTT, profile sharing.
- **Not in scope:** balancing or equalising between batteries, generating alarms the batteries did not raise, correcting for voltage differences before paralleling, or replacing fusing and the batteries' own protection.
- **Same cell count and chemistry** are required for the batteries behind one gateway. The gateway does not check this.

## Hardware platform

The reference build is an ESP32 with three CAN ports, each an MCP2515 SPI module. One chip then gives WiFi, OTA and a web server, and the MCP2515 supports listen-only mode.

| Port | Connects to | Controller (proposed) |
| --- | --- | --- |
| INV | Inverter | MCP2515 #1 |
| A | Battery A | MCP2515 #2 |
| B | Battery B | MCP2515 #3 |

- **MCP2515 modules:** the common 5 V boards use a TJA1050 transceiver, so level-shift SCK, MOSI, MISO and the chip selects (an 8-channel shifter covers all six lines: three chip selects plus SCK, MOSI and MISO). Give each INT line its own GPIO through a divider; INT is push-pull, so never wire-OR them. Start SPI at 1-2 MHz.
- **Bitrate:** set per port. The Blue Nova bus runs at 250 kbit/s.
- **Power:** from the 48 V DC bus through a 48 V to 12 V buck, then onboard 5 V and 3.3 V. The buck's input rating must sit well above the bank's maximum voltage: at least 60 V, ideally 75-100 V. Fuse the input at the source (1-2 A DC) and route the supply away from the CAN pair. The actual buck's rating is not confirmed yet.
- **Flash:** two OTA slots plus a filesystem for config and profiles. 4 MB is the minimum; 8 MB is comfortable.
- **Isolation:** whether to isolate the CAN ports from each other (isolated transceivers or digital isolators) is open.
- **Bypass relay (optional):** a relay that connects Battery A straight to the inverter if the gateway dies. It is only safe if Battery B is also disconnected, because the inverter would then see Battery A's limits alone while both batteries are still on the DC bus. The default failure behaviour is the fail-safe action, not bypass.

## Architecture

Each battery is decoded by its own input profile into one canonical model, the combine engine merges the batteries field by field, and an output profile encodes the result for the inverter.

&#91;embedded content: gateway architecture · decode, combine, encode\]

Batteries enter at the top and the combined result leaves at the bottom; the side blocks watch over or configure the gateway.

## Combine engine

Every output field has its own mode, set in the web UI and stored in the config. The defaults below are the safe choices for dissimilar batteries sharing one DC bus.

| Field | Default | Why |
| --- | --- | --- |
| CVL (charge voltage limit) | min | The lowest limit protects every battery |
| CCL (charge current limit) | sum, clamped to a user maximum | Parallel limits add; clamp at the inverter or cable limit |
| DCL (discharge current limit) | sum, clamped to a user maximum | Same as CCL |
| DVL (discharge voltage limit) | max | The highest cutoff protects every battery |
| SOC | weighted mean by capacity | A small battery should not swing the bank's SOC |
| SOH | weighted mean by capacity | min is the conservative alternative |
| Voltage | mean | Batteries on one bus sit at nearly the same voltage |
| Current | sum | Total flow into or out of the bank |
| Temperature | max | Worst case |
| Alarm and protection flags | any (OR) | One battery's alarm raises the bank's alarm |
| Capacity (Ah) | sum | Total bank capacity |
| Energy counters | sum | Total throughput of all packs |

Any field can use any of these modes: min, max, mean, weighted mean, sum, pass A, pass B, failover A to B, failover B to A, or a fixed value. After the mode, optional steps run in order: clamp (min and max), scale, offset, slew limit (maximum change per second) and hold time.

**Missing data.** If a profile does not provide a field, that battery is skipped for it. A limit field (CVL, CCL, DCL, DVL) that a profile cannot provide must be given a fixed value in the profile, otherwise the gateway refuses to start: summing with a missing limit would overstate what the bank can take.

**Rate limiting.** CVL changes are slew-limited and given hysteresis, because the Blue Nova steps its CVL between 54.6, 55.2 and 56.0 V as its charge current crosses about 20 A and 50 A, and chatters when the current sits on a threshold. With a second battery sharing the charge current, the Blue Nova's own current falls, so its CVL steps at different bank currents. It also derates its CCL (100 A down to 60 A at a cell maximum of about 3.45 V) and restores it only once the bank discharges, so every limit is recomputed from each frame.

**Safety confirmations.** Setting CVL to anything but min, DVL to anything but max, or the limits to pass through one battery while both are online needs a typed confirmation, and a warning banner shows on the dashboard while it is active.

## Fail-safe behaviour

When a battery stops talking or reports a fault, the gateway falls back to fail-safe values that you set, or goes silent if you choose that instead. Either way it never invents battery data: fail-safe values are limits and flags you configured, not made-up readings. How the Sunsynk reacts to each action has not been tested yet.

**Always on (cannot be disabled)**

1. **No invented data.** Every value sent to the inverter is computed from live battery frames or comes from the configured fail-safe values.
2. **Frame timeout per battery.** If an enabled battery's frames stop for longer than the timeout (proposed default 5 s, adjustable), the gateway applies the fail-safe action.
3. **Alarm trip.** A battery alarm or protection flag, as mapped by its profile, sets CCL and DCL to zero and raises the alarm flags in the output. So does any change in a frame the profile marks with a baseline, such as the Blue Nova's 0x35A. Trips on unknown flags latch until cleared in the web UI or by a power cycle.
4. **Plausibility check.** A value outside the profile's valid range counts as missing.
5. **Start-up hold.** Until every enabled battery has sent valid frames for a minimum time, the gateway applies the fail-safe action.
6. **Watchdog.** A hardware watchdog restarts the device if the CAN task stops, and the output is silent while it reboots.
7. **Firmware update.** The output is silent during an OTA update.

**Fail-safe action**

What the gateway does when a battery times out, trips, or has not yet been seen is a setting:

- **Fail-safe values (default):** keep talking to the inverter but send the values below. The link stays up, and the inverter is told explicitly to stop charging and discharging.
- **Silent:** stop sending, so the inverter's own comms-loss handling takes over.

| Field | Fail-safe value |
| --- | --- |
| CCL, DCL | 0 A (factory default) |
| CVL, DVL | Entered by the user. There is no factory default, and the gateway refuses to start until they are set |
| SOC, SOH, voltage, current, temperature | Last valid value, or a fixed value if preferred |
| Alarm flags | Set, where the output profile defines them |

The Blue Nova clone output profile has no alarm flags (the 0x35A bits are unknown), so with it the zero limits do the work. The voltage settings in the inverter for a lost-comms battery, which the Blue Novas also needed, stay in place as the last line of defence. They are all that remains if the gateway itself dies or goes silent.

**Settable**

- **Degraded mode (off by default):** continue on the surviving battery's limits when the other one times out. Needs a typed confirmation.
- **Bank-size alert:** the Blue Nova master only counts its slave pack at power-up, so a CAN glitch can silently halve the bank. If 0x379 falls from 200 Ah to 100 Ah, or the limits halve, the gateway raises an alert in the web UI and MQTT, and can optionally derate.

The gateway does not replace the batteries' own BMS protection or the DC fusing.

## Profiles and YAML

There are two kinds of profile, both YAML files that can be imported and exported in the web UI. An **input profile** maps one battery's CAN frames onto the canonical model (CVL, CCL, DCL, DVL, SOC, SOH, voltage, current, temperature, alarms, energy). An **output profile** does the reverse: it encodes the combined model as an inverter protocol.

The example below is the Blue Nova input profile, limited to frames whose layout is verified against Home Assistant and the battery screen. The valid range on voltage is illustrative.

```yaml
schema_version: 1
profile:
  name: Blue Nova BN52V-100-5.2K BP
  kind: battery            # battery | inverter
  bitrate: 250000
  id_format: standard      # standard | extended
forward_from_inverter:
  - id: 0x305              # keep-alive, passed on to the battery
frames:
  - id: 0x351
    period_ms: 1000
    fields:
      - {name: cvl, offset: 0, type: u16, endian: little, scale: 0.1, unit: V, maps_to: cvl}
      - {name: ccl, offset: 2, type: s16, endian: little, scale: 0.1, unit: A, maps_to: ccl}
      - {name: dcl, offset: 4, type: s16, endian: little, scale: 0.1, unit: A, maps_to: dcl}
      - {name: dvl, offset: 6, type: u16, endian: little, scale: 0.1, unit: V, maps_to: dvl}
  - id: 0x355
    fields:
      - {name: soc, offset: 0, type: u16, endian: little, unit: '%', maps_to: soc}
      - {name: soh, offset: 2, type: u16, endian: little, unit: '%', maps_to: soh}
  - id: 0x356
    fields:
      - {name: voltage, offset: 0, type: u16, endian: little, scale: 0.01, unit: V, maps_to: voltage, valid: [40.0, 60.0]}
      - {name: current, offset: 2, type: s16, endian: little, scale: 0.1, unit: A, maps_to: current}   # positive = charging
      - {name: temperature, offset: 4, type: s16, endian: little, scale: 0.1, unit: C, maps_to: temperature}
  - id: 0x35A
    baseline: [0xA8, 0xAA, 0x82, 0x02, 0xA8, 0xAA, 0x02, 0x02]
    on_change: trip        # bit meanings are not known
  - id: 0x378
    fields:
      - {name: e_charged, offset: 0, type: u32, endian: little, scale: 0.1, unit: kWh, maps_to: energy_charged}
      - {name: e_discharged, offset: 4, type: u32, endian: little, scale: 0.1, unit: kWh, maps_to: energy_discharged}
```

The schema also needs bit fields (`bit` and `bits`), enumerations and a per-frame timeout. The browser converts YAML to JSON before upload, and the device validates it against the schema and rejects unknown keys.

**Output profiles.** The default is Pylontech, using the frames defined in the public V1.3 spec (0x351, 0x355, 0x356, 0x359, 0x35C, 0x35E, every second). Only the Blue Nova's own frame set, which has no 0x359 or 0x35C, has been seen working with the Sunsynk. So a second built-in output profile, "Blue Nova clone", reproduces that frame set as the known-good reference, and which one the Sunsynk accepts needs bench confirmation.

**Inverter frames.** Frames the inverter sends, such as the 0x305 keep-alive, are forwarded to every battery port listed in `forward_from_inverter`.

**Internal frames.** A profile can mark frame IDs as `scope: internal`: traffic between batteries on the same bus, such as the Blue Nova's 0x211-0x217 pack-2 reports and 0x374-0x377 position codes, which appear to be master-slave traffic. In passthrough mode they are forwarded unchanged. In merge mode they are not forwarded to the inverter, but the gateway can still decode them for per-pack telemetry.

**Sharing.** Community profiles live in a `profiles/` directory in the repository.

## Web UI

The web UI is served by the device itself, with no cloud, and covers everything below.

| Page | What it does |
| --- | --- |
| Dashboard | Merged and per-battery live values, gateway state, trip log |
| Batteries | The register map: every decoded field next to its raw frame, and the age of each battery's last frame |
| Combine | Per-field mode and post-steps, with warnings on unsafe choices |
| Profiles | Library of input and output profiles, YAML import and export, editor with a byte picker |
| Sniffer | Live frame table and capture download |
| MQTT | Broker settings, topics, Home Assistant discovery |
| System | WiFi, password, time, config backup and restore, Verbose Diagnostics (data upload) |
| Firmware | OTA upload, active slot, rollback |

**YAML import and export.** Whole configs and single profiles can be exported and imported. The browser converts YAML to JSON, the device stores the JSON on its flash filesystem, and export converts back to YAML. An import is validated first and only takes effect when applied, with a confirmation if it contains unsafe settings. The previous config is kept, so one click rolls back. Exports leave out passwords unless asked.

## MQTT and Home Assistant

MQTT is configured entirely in the web UI, and the topic layout below is a proposal.

- **Settings:** broker host and port, username and password, TLS on or off with an optional CA certificate, client ID, base topic, retain flag and publish interval.
- **Topics:** `<base>/merged/<field>` for the combined values, `<base>/battery_a/<field>` and `<base>/battery_b/<field>` for each battery, `<base>/state` for the gateway state, and `<base>/events` for trips and alerts.
- **Availability:** a last-will message on `<base>/availability`, so Home Assistant shows the gateway as offline when it drops.
- **Discovery:** an on/off switch for Home Assistant MQTT discovery, so every value appears as a sensor without YAML.
- **Read-only by default:** configuration changes are made in the web UI, not over MQTT.

## Sniffer mode and register-map editor

Sniffer mode lets anyone profile a battery with no extra hardware, and it never transmits a frame of its own.

There are two ways to use it:

- **Bridge sniffer:** the gateway forwards every frame between Battery A and the inverter unchanged and logs them. Port B is unused. This is how to profile a bank on a live system.
- **Listen-only:** the gateway listens on one port without acknowledging frames. This is for profiling a battery on its own. A wrong bitrate with acknowledging switched on disturbs the bus (it took the Blue Nova off the inverter during testing), so bitrate detection tries the common rates (125, 250, 500 and 1000 kbit/s) in listen-only mode and only then suggests one.

What the user gets:

- **Live table:** ID, count, rate, last data, changed bytes highlighted, and the minimum and maximum of each byte.
- **Capture:** a WebSocket stream to the browser, saved as CSV, `.asc` or `candump`. The ESP32's RAM holds only a few thousand frames, so a ring buffer covers the recent past and the browser file is the long record.
- **Register-map editor:** click bytes in a live frame to define a field (offset, length, endianness, sign, scale, unit) and watch it plot live. Typing the value shown on the battery's screen lets the editor suggest the scale. The result exports as an input profile in YAML.

## Verbose Diagnostics: data logging and upload

Verbose Diagnostics is a switch in the Settings page, off by default. When it is on, the gateway records CAN traffic and uploads it to a server you run, so there is a long-term record without a separate logger in the CAN circuit.

- **Buffering:** frames are timestamped into a RAM buffer of 500 frames. Two buffers alternate, so receiving never waits on the network.
- **When it uploads:** when 500 frames are buffered or 30 s have passed (both adjustable), and at once on an alarm event. At the Blue Nova's roughly 1,300 frames a minute, 500 frames is about 23 s.
- **Upload:** an HTTP POST of the batch to the server's API with a token. Each batch has a sequence number and says whether the clock was synced by NTP. A buffer is freed only after the server answers 200.
- **Server down:** the last few batches stay in RAM and the oldest are dropped. The number dropped is reported when the connection returns.
- **Change-only mode (optional):** only frames whose data changed, plus a full snapshot every minute. A batch then covers minutes instead of seconds.
- **Pre-trigger capture:** the last 60 s of frames stay in RAM and are uploaded with every alarm event, such as a change in a baseline-marked frame like 0x35A. The event is also published to MQTT directly, so the HA flag does not depend on the server.
- **Priority:** logging runs in its own low-priority task and can never block CAN reception or the fail-safe logic.
- **Settings:** server URL, token, batch size, flush interval, change-only on or off, and pre-trigger length.

**Reference server.** The repository includes a `docker-compose.yml` for anyone profiling their own battery. It runs a small receiver API that accepts batches (ignoring duplicates by sequence number), a database for the frames and decoded values, and optionally Grafana with a ready dashboard. Stored frames are decoded with the same YAML input profile the device uses, and any capture can be exported as CSV or `.asc`. The database choice (SQLite by default, or PostgreSQL) is still to be decided.

## OTA updates and security

OTA is built first, so every later feature ships over WiFi.

- **Two app slots with rollback:** a new image boots once and must pass a self-test (WiFi up, CAN controllers initialised, config loaded) and mark itself valid. Otherwise the bootloader returns to the old slot on the next reset.
- **How to update:** upload a `.bin` in the web UI. An optional, off-by-default check against the project's GitHub releases can come later.
- **Signed images:** images are verified before they boot. Unsigned images are accepted only in a developer mode that the user turns on.
- **During an update:** the output is silent (fail-safe rule 7), and the web UI says so before it starts.
- **First boot:** there is no default password. The device starts a setup access point, the user sets a password and WiFi, and the access point then switches off.
- **Web access:** HTTP on the LAN by default with optional TLS, rate-limited login attempts, and no cloud connection of any kind.
- **Secrets:** MQTT and WiFi passwords are stored on the device and left out of exports unless the user includes them.

## Build order

1. **Platform bring-up:** WiFi setup, web UI skeleton, OTA with two slots and rollback.
2. **Listen-only tap and Verbose Diagnostics:** a listen-only MCP2515 on the Blue Nova slave's free port, the live frame table and capture download, bitrate detection, and batch upload to the reference server, so the long-term dataset starts early. It cannot affect the inverter link.
3. **CAN bridge:** the three ports, a transparent bridge between the inverter and Battery A, forwarding of the 0x305 keep-alive, and error counters. Passthrough test: replay the captured Blue Nova logs on the bench, then run live for a full charge and discharge cycle and compare against the 31 Oct capture (same IDs, rates and HA values, and the inverter still shows the battery). Pull the ESP's power to see what the inverter does. Each bus segment needs its own 120 ohm termination.
4. **Profile engine:** YAML to JSON import and export, the canonical model, the Blue Nova input profile, the register-map editor.
5. **Combine engine and fail-safe logic:** per-field modes, the Pylontech and Blue Nova clone output profiles, and every always-on safety rule.
6. **MQTT and Home Assistant discovery.**
7. **Bench validation:** first with the two Blue Nova packs set up as independent masters on ports A and B, if the role can be set. The merged output must then equal what the real master reports today (CCL 100 A, DCL 200 A, 200 Ah). Then repeat with the new battery. Use a bench bus with a second CAN logger watching the output before the inverter is connected, and clamp CCL and DCL low for the first live runs.
8. **Live trial:** connected to the inverter with the logger running in parallel, exercising the trip and timeout paths.
9. **Release:** licence, documentation, profile library and a first tagged release.

## Open questions

- **New battery protocol:** the KNY-P51300's CAN bitrate and frame map have not been seen. Ask the supplier for the CAN documentation.
- **Sunsynk behaviour:** how it reacts to a silent gateway and to fail-safe values (zero limits), and whether it accepts the Pylontech V1.3 frame set or only the Blue Nova's.
- **Alarm bits:** the meaning of the Blue Nova's 0x35A is unknown, and it never left its baseline value across a full day (19 % to 100 % to 20 %), even during a CCL derate at a 3.457 V cell. Whether it changes on a real protection event is untested.
- **When one battery drops out:** fail-safe values (the default), silent, or degraded mode.
- **Buck regulator:** its input voltage rating.
- **Isolation:** whether to isolate the CAN ports from each other.
- **Licence:** which open-source licence to use.
- **Home bank:** buying two identical batteries would make the aggregator optional there, and the project could still go ahead for other users.
- **ESP32 variant:** classic ESP32 or S3, decided by how much RAM the web UI needs.
- **Project name.**

## Appendix: verified Blue Nova CAN decode

Source: Blue Nova CAN captures of 30 and 31 October 2026, compared with Home Assistant and the battery screen. The bus runs at 250 kbit/s.

| ID | Content | Decode | Status |
| --- | --- | --- | --- |
| 0x305 | Inverter keep-alive, 1 Hz | 8 bytes of 0x00 | Seen |
| 0x351 | CVL, CCL, DCL, DVL | Four little-endian 16-bit values in 0.1 V or 0.1 A (CCL and DCL signed) | Verified against the screen. Over a full day CVL stepped 54.6 / 55.2 / 56.0 V with charge current, CCL derated 100 to 60 A at a cell maximum of about 3.45 V, and DCL (200 A) and DVL (48 V) never changed |
| 0x355 | SOC, SOH | Two 16-bit percentages; bytes 4-5 hold SOC in 0.01 % | Verified against HA |
| 0x356 | Voltage, current, temperature | 0.01 V, 0.1 A, 0.1 °C; positive current is charging | Verified against HA (current scale 0.997, offset 0.05 A) |
| 0x35A | Alarm bitfield | Constant A8 AA 82 02 A8 AA 02 02 | Bit meanings unknown |
| 0x35E, 0x370 | Manufacturer string | ASCII "BlueNova" | Seen |
| 0x35F | Unknown | Constant 00 00 00 07 | Unknown |
| 0x372 | Unknown | All FF | Unknown |
| 0x373 | Cell min and max, temperature min and max | Two unsigned 16-bit in mV, two signed 16-bit in kelvin | Temperatures corrected from °C to K |
| 0x374-0x377 | Position codes | ASCII | Seen |
| 0x378 | Energy counters | Two little-endian 32-bit counters (charged, discharged), 0.1 kWh per count, sum of both packs | Verified against HA |
| 0x379 | Capacity | 100 Ah = master alone, 200 Ah = both packs | Seen; 200 Ah throughout the capture |
| 0x210, 0x211 | Placeholders | AA FF ... at 1 Hz | Seen |
| 0x211-0x217 (every 2 s) | Pack-2 data, big-endian | 0x211 frames starting 0x02: V (0.01 V), current (0.01 A, signed), SOC, SOH; 0x212 temperatures in K (not rechecked); 0x213 bytes 1-2 temperature in 0.1 °C and byte 5 a charge/discharge state byte; 0x214 byte 4 = 0x80 while a pack-2 cell is above about 3.40 V (probably balancing); 0x215 and 0x216 energy counters and pack limits; 0x217 cell min and max | Partly verified |
