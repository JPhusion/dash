# Reference Code Review (ELEC3117)

Tour of `/Users/josh/Developer/UNSW/ELEC3117/`, the prior-art Arduino project
that Dash builds on. Captured here so future maintainers don't have to re-read
the whole tree.

## Salvaged for Dash

| Reference file(s)               | What it does                                  | Where it lives in Dash |
|---------------------------------|-----------------------------------------------|------------------------|
| `Eye*`, `Face*`, `Animations.h` | Animated eye/face rendering (U8g2 SH1106)     | `lib/eyes/` (copied, unmodified)|
| `Constants.h`                   | Pin defines + Madgwick filter constants       | Split: pins → `wiki/peripherals.md`, runtime constants → `src/dash/imu.cpp` |
| `IMU.cpp/.h`                    | MPU-6050 I2C driver + Madgwick AHRS           | Rewritten under `dash::Imu` for M1 with tap/double-tap/shake detection |
| `Speaker.cpp/.h`                | I2S WAV playback with cooperative-cancel task | Replaced by `dash::Audio` for raw PCM playback + silent-test mode |
| `Sleep.cpp/.h`                  | Pitch-hold + shake-to-sleep, touch wake       | Replaced by `dash::Power` (state-machine driven) for M2 |
| `CapacitiveTouch.cpp/.h`        | Tap/double-tap/hold via `touchRead`           | Replaced by `dash::Touch` with calibrated threshold |
| `AsyncManager.h/.cpp`           | FreeRTOS task spawner (`xTaskCreate*`)        | Reused directly under `src/dash/async.h` |

## Discarded / not pulled in

- `AsyncTimer.h/.cpp` — needed by the eye library, kept inside `lib/eyes/`.
- The "pull over" session-safety logic from `src.ino` — that was driving-game
  specific and doesn't apply to Dash.
- Raw `.wav` files in `data/` — Dash regenerates its own sound bank in M3 from
  generators in `tools/sounds/`. The reference WAVs are 16-bit PCM at variable
  sample rates; Dash standardises on 8 kHz mono 8-bit unsigned `.raw` for
  smaller flash footprint and simpler I2S setup.

## Notable patterns / gotchas

- I2C `Wire.begin(SDA, SCL, freq)` ordering matters: ESP32 Wire library takes
  SDA first, SCL second. Reference matches this; double-checked.
- `i2sWriteAll()` yields every ~8 KB to avoid starving other tasks. Pattern
  carried into `dash::Audio`.
- Eye library's `Face` constructor instantiates the U8g2 driver as a global.
  Compiling it pulls `extern u8g2` symbol from `Common.h`. Library kept
  self-contained by adding `lib/eyes/src/Common.h` + `Constants.h` shims.
- `touchSleepWakeUpEnable()` is the supported API for waking ESP32 from deep
  sleep via cap-touch. Used in M2.
- Madgwick gain `BETA=0.2` worked well for the reference. Keep unless we see
  drift during long sessions.

## Pin assignments lifted verbatim

```
I2C: SCL=17, SDA=18 (4 MHz)
IMU: MPU-6050 @ 0x68
OLED: SH1106 @ 0x3C (NOT SSD1306 as the master prompt assumed — corrected)
I2S: DOUT=14, BCLK=25, LRCLK=26
TOUCH: GPIO27
```

The master prompt suggested the OLED was "likely SSD1306". The reference code
is built around SH1106, which is electrically compatible at the I2C layer but
needs a different U8g2 constructor and has a different RAM addressing offset.
Dash uses SH1106 — see `wiki/decisions.md`.
