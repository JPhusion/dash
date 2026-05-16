# Dash

A cube-shaped ESP32 study companion. Animated eyes, IMU gestures, captive-portal
session control, on-device minigames, and group study over ESP-NOW.

## Build

```
pio run -e dash-debug          # build (silent audio, no deep sleep)
pio run -e dash-debug -t upload
pio device monitor             # 115200 baud
```

Three environments:

| Env            | Purpose                                       |
|----------------|-----------------------------------------------|
| `dash`         | Default release build.                        |
| `dash-debug`   | Silent audio, deep sleep disabled, verbose.   |
| `dash-release` | Production firmware (sounds on, sleep on).    |

## Project layout

```
src/             — Dash firmware (all under dash:: namespace)
lib/eyes/        — Animated-eye rendering library (vendored, AGPL)
data/            — LittleFS image (sounds, web/captive portal assets)
tools/           — Build scripts, sound generators, OTA signing
wiki/            — Architecture, peripherals, decisions, protocols
partitions.csv   — Custom 16 MB partition table (dual OTA + LittleFS)
```

## Documentation

See `wiki/`:

- `peripherals.md`  — pin map, peripheral notes
- `reference_code_review.md` — what was inherited from the ELEC3117 prototype
- `decisions.md`    — running log of architectural decisions (ADRs)
- `architecture.md` — module overview (added in M12)
- `protocols.md`    — ESP-NOW + captive-portal API (added in M11/M12)
- `development.md`  — dev workflow, signing keys, testing tips (added in M12)
