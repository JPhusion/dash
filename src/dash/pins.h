// pins.h — Dash hardware pin assignments.
// Source: ELEC3117 reference + wiki/peripherals.md. Any change must update the
// wiki and be flagged in wiki/decisions.md.

#ifndef DASH_PINS_H
#define DASH_PINS_H

#include <stdint.h>

namespace dash::pins {

// I2C bus shared between IMU (MPU-6050 @ 0x68) and OLED (SH1106 @ 0x3C).
inline constexpr int I2C_SCL = 17;
inline constexpr int I2C_SDA = 18;
inline constexpr uint32_t I2C_FREQ_HZ = 400000;  // u8g2 likes 400 kHz; safe with MPU6050.

// I2S audio amplifier (PCM5102 / MAX98357 family).
inline constexpr int I2S_DOUT  = 14;
inline constexpr int I2S_BCLK  = 25;
inline constexpr int I2S_LRCLK = 26;

// Capacitive touch (T7).
inline constexpr int TOUCH = 27;

// MPU-6050 motion interrupt — not wired on v1 prototype. Reserved RTC GPIO for v2.
inline constexpr int IMU_INT = -1;

// USB 5V sense — not wired; software assumes USB-attached during bring-up.
inline constexpr int USB_VBUS = -1;

}  // namespace dash::pins

#endif
