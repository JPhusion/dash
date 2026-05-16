#include "dash/power.h"

#include <WiFi.h>
#include <driver/rtc_io.h>
#include <esp_bt.h>
#include <esp_wifi.h>
#include <soc/rtc.h>

#include "dash/log.h"
#include "dash/pins.h"

namespace dash {

RTC_DATA_ATTR uint32_t rtcBootCount = 0;
RTC_DATA_ATTR uint32_t rtcLastOtaCheckUnix = 0;
RTC_DATA_ATTR uint32_t rtcLastSleepUnix = 0;
RTC_DATA_ATTR uint32_t rtcLastSleepMillis = 0;

namespace {
constexpr const char* kTag = "Power";
Power* g_singleton = nullptr;

constexpr uint16_t kTouchWakeThreshold = 40;  // pad reading at which we wake
}

Power::Power() : lastWake_(ESP_SLEEP_WAKEUP_UNDEFINED), current_(CpuProfile::Performance) {}

void Power::begin() {
  lastWake_ = esp_sleep_get_wakeup_cause();
  rtcBootCount++;
  log::info(kTag, "boot #%u wake_cause=%d", (unsigned)rtcBootCount, (int)lastWake_);
}

void Power::setCpuProfile(CpuProfile p) {
  if (p == current_) return;
  uint32_t mhz = (p == CpuProfile::Performance) ? 240 : 80;
  setCpuFrequencyMhz(mhz);
  current_ = p;
  log::info(kTag, "cpu -> %u MHz", (unsigned)mhz);
}

void Power::enterDeepSleep(uint32_t wakeAfterMs) {
#ifdef DASH_NO_DEEP_SLEEP
  log::warn(kTag, "deep sleep skipped (DASH_NO_DEEP_SLEEP)");
  return;
#else
  log::info(kTag, "entering deep sleep, timer=%ums", (unsigned)wakeAfterMs);
  rtcLastSleepUnix = 0;  // wall clock filled in by Session module when known
  rtcLastSleepMillis = millis();

  // Cut radios before sleeping — saves ~80 mA peak.
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  btStop();

  // Configure cap-touch wake on T7.
  touchSleepWakeUpEnable(pins::TOUCH, kTouchWakeThreshold);
  esp_sleep_enable_touchpad_wakeup();

  if (wakeAfterMs > 0) {
    esp_sleep_enable_timer_wakeup((uint64_t)wakeAfterMs * 1000ULL);
  }

  // Hold the I2S enable line low across sleep (when wired in v2). For v1 the
  // amp has no enable pin so this is a no-op safeguard.
  if (pins::I2S_DOUT >= 0) {
    gpio_hold_en((gpio_num_t)pins::I2S_DOUT);
    gpio_deep_sleep_hold_en();
  }

  Serial.flush();
  esp_deep_sleep_start();
#endif
}

Power& power() {
  if (!g_singleton) g_singleton = new Power();
  return *g_singleton;
}

}  // namespace dash
