# Hardware Notes

Things to remember when designing v2 hardware, or when debugging the v1
prototype.

## v1 — known limitations

### IMU motion-wake interrupt
The MPU-6050's INT pin is not connected to a RTC GPIO on the prototype.
Consequence: deep sleep can only be woken by capacitive touch or by the
nightly OTA timer. There's no "shake to wake me" gesture in v1.

**v2 ask**: route MPU-6050 INT to any of GPIO 32–39 (RTC-capable). 36/39 are
also input-only on the classic ESP32, which works for an INT line.

### Battery voltage sensing
There's no ADC divider on the LiPo line. The portal cannot show a real
battery % — only "USB connected" or "running on battery" (and not even that
reliably in v1, since USB 5V isn't sensed either).

**v2 ask**: a 100k/100k divider from BAT+ to a free GPIO. Use an ADC1 pin
(e.g., 32 or 33) so it works while Wi-Fi is on. Also expose USB 5V via a
similar divider to a different ADC1 pin.

### 32 kHz crystal
WROOM module has no 32k crystal. RTC slow clock falls back to the internal
150 kHz RC, ~5% drift over deep sleep. The nightly 4 AM OTA window has to
be ±30 min generous.

**v2 ask**: add a 32.768 kHz crystal across the chip's XTAL_32K pins, set
`CONFIG_ESP32_RTC_CLK_SRC_EXT_CRYS` in sdkconfig.

### Speaker drive
I2S DOUT (GPIO14) goes directly to the amp's data line. There's no
shutdown/enable pin wired, so the amp draws current continuously. Also,
GPIO14 is a strapping pin — the boot ROM reads it during reset, so adding a
hard pull-up here would break boot.

**v2 ask**: bring out the amp's `SD` pin to a non-strapping GPIO (e.g., 13).
Drive it low for shutdown across deep sleep using `gpio_hold_en()`.

### Capacitive touch on battery
Cap-touch readings drift heavily when the cube is on battery and not USB-grounded.
The Touch module auto-recalibrates every 30 s of "not touched" to compensate,
but the readings still aren't great.

**v2 ask**: a physical button as the primary input. Keep the cap-touch as a
secondary "long-press to wake" path.

### Brownout during AP startup
Bringing up the Wi-Fi AP at default TX power (20 dBm) can pull the 1000 mAh
LiPo low enough to brownout-reset the chip. We mitigate by setting TX power
to 11 dBm via `esp_wifi_set_max_tx_power(44)` in `WifiAp::start()`.

**v2 ask**: a bulk cap (e.g., 470 µF tantalum) on the 3.3 V rail near the
Wi-Fi chip. Belt and braces.

### ESD on USB-C
The prototype has no TVS diode on the USB lines. Plugging into a static-y
cable can wedge the chip until power-cycle.

**v2 ask**: SP3010-04 or USBLC6-2 across D+/D- and 5V.

## v1 — pin map

See `wiki/peripherals.md` for the authoritative table.

```
SCL = 17, SDA = 18    (I2C @ 400 kHz, shared by MPU-6050 @ 0x68 and SH1106 @ 0x3C)
I2S DOUT  = 14        (strapping pin; do not add pull-ups)
I2S BCLK  = 25
I2S LRCLK = 26
TOUCH     = 27        (T7)
IMU INT   = (not wired in v1)
USB 5V    = (not sensed in v1)
```

## Notes for the housing

- The 1000 mAh pouch cell sits inside the cube near the speaker. Heat from
  charging is minimal but worth thermal-checking once the housing is sealed.
- Keep the OLED ribbon away from the IMU — the FPC carries fast I2C edges
  that show up on the accelerometer as low-frequency noise if they couple
  through the chassis.
- Cap-touch pad area on the housing should be a single conductive face
  (silver ink, copper foil, or a metal cap) connected to GPIO27 via a short
  trace. Don't run the trace next to the OLED or I2S lines.
