# Peripherals & Pin Map

Source of truth for Dash hardware wiring. Extracted from the ELEC3117 reference
firmware and confirmed against the working prototype.

## Pin map

| Function          | GPIO | Notes |
|-------------------|------|-------|
| I2C SCL           | 17   | Shared by IMU + OLED. 4 MHz fast-mode. |
| I2C SDA           | 18   | Shared bus. |
| I2S DOUT          | 14   | **Strapping pin.** Used as output to amplifier. See note below. |
| I2S BCLK          | 25   | DAC-capable, used as output. |
| I2S LRCLK / WS    | 26   | DAC-capable, used as output. |
| Capacitive touch  | 27   | T7 input. Unreliable on battery — secondary input only. |
| IMU INT (wake)    | TBD  | Not wired on current prototype; software wake-on-motion only. Reserve RTC GPIO (32–39) for v2 hardware. |
| USB 5V detect     | none | No divider on current prototype; assume USB-attached. |
| I2S amp SD/EN     | none | Not on current prototype. If added in v2, drive with `gpio_hold_en()` across deep sleep so the amp stays muted. |

## ADC2 caveat

The classic ESP32 cannot use ADC2 inputs while Wi-Fi is active. Not relevant for
v1 (no analog reads), but anything that lands on GPIO 0/2/4/12/13/14/15/25/26/27
in a future revision must avoid ADC2 functions during AP / STA / ESP-NOW use.

## Strapping pin usage

- **GPIO 14 (I2S DOUT)** — strapping pin. Default state at reset is "pulled
  down internally" on most ESP32 modules; that gives normal SPI boot. The I2S
  peripheral takes the pin over after boot, so the value during reset still
  determines boot mode. Verified: prototype boots reliably with the amplifier
  attached, so I2S input impedance is high enough not to flip the strap. **Do
  not add an external pull-up on this line.**
- **Other strapping pins (GPIO 0, 2, 4, 5, 12, 15)** are unused by current
  firmware. Future expansion should treat them carefully.

## Input-only pin check

None of the assigned output pins fall on GPIO 34/35/36/39 (input-only on the
classic ESP32). 

## Peripheral notes

### IMU — MPU-6050
- I2C address `0x68`. `WHO_AM_I` returns `0x68` (sometimes `0x98` on clones).
- Built-in DMP unused; we do Madgwick AHRS in software.
- No hardware tap detection — implemented entirely in `dash::Imu`.
- Gyro bias calibration runs opportunistically when device is stationary > 5 s
  and persisted to NVS. Boot-time calibration is skipped to keep startup snappy.
- Wake-on-motion ISR uses MPU-6050's motion interrupt (register `0x37/0x38`)
  once an INT pin is wired. Until then, deep-sleep wake relies on touch + timer.

### Display — SH1106 128×64 OLED
- Driver: U8g2 `U8G2_SH1106_128X64_NONAME_F_HW_I2C`. Full-frame buffer (1 KB).
- I2C address `0x3C`.
- The eye library at `lib/eyes/` creates the U8g2 instance internally; do not
  instantiate u8g2 again in application code.

### Audio — I2S amplifier
- Stereo PCM 16-bit, configurable sample rate per file.
- Sounds live on LittleFS at `/sounds/`. Filenames must be 8.3-friendly to keep
  flash listing cheap.
- Per the build prompt, raw PCM 8 kHz mono 8-bit is the canonical Dash format.
  M3 sound generation pipeline emits `.raw` files in that format. WAVs from the
  reference are kept for fallback during M1 bring-up only.

### Capacitive touch
- `touchRead(27)` returns lower numbers when touched. Baseline ~70, touched <30.
- Reference uses threshold 60; we recompute baseline at boot and use baseline ×
  0.7 as a per-device threshold.
- Note: capacitive touch readings shift dramatically when running on battery
  (no chassis ground reference). Treat as a hint, not a primary input.

## Power notes
- 1000 mAh single-cell LiPo, no fuel gauge IC, no ADC divider.
- USB-C charging via TP4056-class IC (typical for cube builds).
- No 32 kHz crystal → RTC uses the internal 150 kHz RC oscillator. Acceptable
  for short deep-sleep timer wakes (< 1 h); for the nightly 4 AM OTA wake we
  rely on a UTC-relative offset computed from the last phone sync time and
  budget ±30 min drift.
