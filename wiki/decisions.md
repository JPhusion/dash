# Architectural Decisions

Running log of choices made while building Dash. Each entry: what was decided,
why, alternatives considered, and revisit conditions.

---

## ADR-001 — Arduino framework over ESP-IDF

**Decision.** Use the Arduino framework on PlatformIO.

**Why.** The reference code (3117) is Arduino-style; the eye library uses U8g2
which is well-supported in Arduino; bring-up time matters more than the last
5 % of power efficiency for an overnight v1 build. Where ESP-IDF features are
needed (deep-sleep config, ESP-NOW, OTA partitions), the Arduino framework
exposes the underlying ESP-IDF APIs anyway via `<esp_*.h>`.

**Revisit if.** We hit a wall on a feature ESP-IDF supports but Arduino
doesn't, or if power budget needs aggressive sdkconfig tuning.

---

## ADR-002 — OLED driver: SH1106, not SSD1306

**Decision.** Use `U8G2_SH1106_128X64_NONAME_F_HW_I2C` and full-frame buffer.

**Why.** The reference firmware uses SH1106. The two controllers are pin- and
I2C-compatible but SH1106 has 2-column RAM offsets that show as a 2-pixel band
on the left of the display if you address it as SSD1306. The eye library is
written against the SH1106 quirks. Confirmed visually on prototype.

**Revisit if.** A future hardware rev uses SSD1306; would only need a different
U8g2 constructor in `lib/eyes/src/Face.cpp`.

---

## ADR-003 — Eye library left unmodified

**Decision.** Vendor the eye library at `lib/eyes/` as-is; do not edit
`Face.cpp`, `Eye*.cpp`, etc.

**Why.** Reduces risk for the overnight build. The library is GPL/AGPL — kept
its license headers. Two new tiny files (`Common.h`, `Constants.h`) bridge it
to the library boundary without touching the original logic.

**Revisit if.** We need richer eye states (heart eyes, search eyes etc.) than
the existing emotion presets cover — M4 may layer a Dash-side wrapper that
draws additional overlay graphics on top of the eye buffer.

---

## ADR-004 — Partition table layout

**Decision.** Use the prompt-supplied table:

```
nvs       0x9000   20K
otadata   0xe000    8K
nvs_keys  0x10000   4K   (encrypted flag)
coredump  0x11000  64K
app0      0x30000 1.5M
app1      0x1B0000 1.5M
littlefs  0x330000 13M
```

**Why.** 1.5 MB app partitions are generous for the foreseeable feature set
(current bring-up is ~600 KB). 13 MB of LittleFS leaves plenty for sounds,
cosmetics, captive portal assets, and rolling session logs. The encrypted
`nvs_keys` partition is required by `nvs_flash` for AES-XTS encryption of the
main `nvs` partition.

**Gap.** ~60 KB between `coredump` end (0x21000) and `app0` start (0x30000).
Required because app partitions must be 64-KB aligned. Accepted.

**Revisit if.** App size approaches 1.4 MB (shrink LittleFS, grow app).

---

## ADR-005 — NVS encryption (deferred to M8)

**Decision.** Reserve the `nvs_keys` partition now but defer wiring up
encryption until M8 (onboarding), when Wi-Fi credentials first land in NVS.

**Why.** Empty NVS doesn't need encryption. Encrypting the partition requires
flashing keys via `espsecure.py generate_flash_encryption_key` and a couple
extra build steps that are not worth blocking M0 on.

**Revisit at.** M8 — flesh out `tools/keys/` workflow and document in
`wiki/development.md`.

---

## ADR-006 — Coredump-to-flash flag

**Decision.** Set `-DCONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=1` and reserve a
coredump partition. Treat as best-effort under Arduino framework.

**Why.** The Arduino-ESP32 framework is built from a fixed sdkconfig where the
coredump library may or may not have been enabled depending on the platform
version. The build flag signals intent; if the underlying lib is included, we
get post-mortem stack traces via `espcoredump.py info_corefile`. If not, the
partition is harmless.

**Revisit if.** We need guaranteed coredumps — switch to a custom
`platform_packages` build with `pioarduino-build/sdkconfig` overrides.

---

## ADR-007 — Sound format: 8 kHz mono 8-bit unsigned `.raw`

**Decision.** Standardise on 8000 Hz, 1 channel, 8-bit unsigned PCM `.raw`
files for all Dash sounds.

**Why.** 8 KB/s on disk, easy to stream off LittleFS, zero WAV parsing in the
playback path. The reference WAVs (16-bit 44.1 kHz) are ~10× the size and
overkill for a tiny cube speaker. The `tools/sounds/` generator outputs this
format directly.

**Revisit if.** Audio quality is unacceptably bad — bump to 16 kHz 8-bit or
8 kHz 16-bit, both still tiny.

---

## ADR-008 — Wi-Fi channel 6 locked across all radios

**Decision.** AP, STA (for OTA), and ESP-NOW all operate on channel 6.

**Why.** ESP-NOW does not retune the radio per peer; switching channels for STA
mode would tear down active ESP-NOW peers. Channel 6 is a 2.4 GHz mid-band
choice that minimises overlap with typical 1/11 home routers. Documented up
front so M11 (multi-Dash) doesn't surprise M9 (OTA).

**Revisit if.** OTA fails on home networks that use channel 6 exclusively for
the home router — at that point implement pause/resume of ESP-NOW around STA
operations.

---

## ADR-009 — Wi-Fi TX power 11 dBm

**Decision.** `esp_wifi_set_max_tx_power(44)` (= 11 dBm) for the AP.

**Why.** Phone is in arm's reach during onboarding/session control. Default
20 dBm wastes current and can cause brownouts on the 1000 mAh cell during
TX bursts. 11 dBm is the lowest sane setting that maintains a stable BSSID
across the room.

**Revisit if.** Users report flaky connections from across a room.

---

## ADR-010 — Tap detection in software

**Decision.** Detect single/double/triple tap purely from MPU-6050 accel
samples; do not use the chip's built-in motion/tap interrupts for v1.

**Why.** The MPU-6050's tap detection requires careful sensitivity tuning and
fires false positives easily. A software detector on the high-pass filtered
linear-accel magnitude is more controllable and avoids burning an INT pin we
haven't wired yet. We can swap to the hardware interrupt in v2 hardware when
the INT pin is exposed for deep-sleep wake.

**Revisit at.** Hardware v2 — the wake-on-motion path NEEDS a hardware INT.

---

## ADR-011 — Module/file layout

**Decision.** All Dash code lives under `src/dash/` in the `dash::` namespace.
Module boundaries:

```
dash::Imu       — IMU sampling, orientation, tap detection
dash::Display   — wrapper around lib/eyes/ + ad-hoc UI overlays
dash::Audio     — I2S playback of .raw files from LittleFS
dash::Touch     — cap-touch calibration + events
dash::Power     — CPU freq scaling, deep sleep, wake reason routing
dash::StateMachine — top-level mode (Idle / InSession / InGame / ...)
dash::WifiAp    — AP + captive portal HTTP server
dash::Portal    — request handlers wired to dash::Session + dash::Stats
dash::Session   — study session timer, progress, DNS distraction monitor
dash::Stats     — rolling session log (LittleFS)
dash::Settings  — NVS config (Wi-Fi creds, name, prefs)
dash::Ota       — GitHub release polling, signature verify, flash
dash::EspNow    — peer discovery + group session messaging
```

Each module is one `.h` + one `.cpp`. Inter-module dependencies are one-way
where possible (Session depends on Audio + Display, not the reverse).

**Revisit if.** Modules grow beyond ~500 LOC each — split before merging.

---

## ADR-012 — `Wire.begin` shared bus

**Decision.** A single `Wire` instance (default I2C peripheral 0) is shared
between the IMU and the OLED. The eye library's `Face` constructor calls
`u8g2.begin()` which internally calls `Wire.begin()`. The Dash IMU init must
either run after `Face` is constructed, or both must use the same SCL/SDA pins
and clock rate.

**Why.** Saves a peripheral. Reference firmware ran this way successfully.

**Order in `setup()`.** `Display::begin()` first (it sets up Wire), then
`Imu::begin()` which reuses the existing Wire instance with `Wire.setClock()`
if needed.
