// power.h — CPU frequency scaling and deep-sleep entry/exit.
//
// Decisions baked in:
// - 80 MHz when Idle / Drowsy / OTA-check; 240 MHz when InSession / InGame / InMenu.
// - Wake sources on v1 hardware: capacitive touch (T7) + timer. IMU INT pin
//   isn't wired on the prototype, so motion-wake falls back to "tap the cube
//   anywhere — the wire is sensitive to the body cap shift". Documented in
//   wiki/peripherals.md.
// - DASH_NO_DEEP_SLEEP build flag short-circuits enterDeepSleep() to a logged
//   no-op for overnight bring-up.
//
// RTC variables this module is responsible for live here as extern RTC_DATA_ATTR
// so any module can read them after wake without going through Power's API.

#ifndef DASH_POWER_H
#define DASH_POWER_H

#include <Arduino.h>
#include <esp_sleep.h>

namespace dash {

extern RTC_DATA_ATTR uint32_t rtcBootCount;
extern RTC_DATA_ATTR uint32_t rtcLastOtaCheckUnix;
extern RTC_DATA_ATTR uint32_t rtcLastSleepUnix;
extern RTC_DATA_ATTR uint32_t rtcLastSleepMillis;

enum class CpuProfile : uint8_t {
  LowPower,    // 80 MHz
  Performance, // 240 MHz
};

class Power {
 public:
  Power();

  // Capture the wake cause and bump rtcBootCount.
  void begin();

  // Set CPU profile. Idempotent if already at the requested speed.
  void setCpuProfile(CpuProfile p);

  // Configure wake sources and call esp_deep_sleep_start(). Never returns
  // unless DASH_NO_DEEP_SLEEP is defined.
  //
  // wakeAfterMs: 0 disables the timer wake.
  void enterDeepSleep(uint32_t wakeAfterMs = 0);

  esp_sleep_wakeup_cause_t lastWakeCause() const { return lastWake_; }
  uint32_t bootCount() const { return rtcBootCount; }

 private:
  esp_sleep_wakeup_cause_t lastWake_;
  CpuProfile current_;
};

Power& power();

}  // namespace dash

#endif
