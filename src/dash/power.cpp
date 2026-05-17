#include "dash/power.h"

#include <WiFi.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_bt.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <soc/rtc.h>

#include "dash/imu.h"
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
  // Despite the function name, this is now a LIGHT-sleep entry — the
  // MPU-6050 INT pin is on GPIO 19 which isn't RTC-capable, so deep
  // sleep can't wake on motion. Light sleep with GPIO wake gives the
  // shake-to-wake feature at the cost of ~0.8 mA standby (vs ~10 µA in
  // deep sleep). Function name kept for caller compatibility — all the
  // existing flip-to-sleep + idle-manager + CLI callers just work.
  //
  // Wake sources (any one of them resumes execution):
  //   - GPIO 19 high level  → IMU motion (shake to wake)
  //   - Touchpad on GPIO 27 → cap-touch on the pad
  //   - Timer               → only when caller requested wakeAfterMs > 0
  //                           (used by OTA scheduling).
  //
  // The MPU-6050 is reconfigured to its low-power Wake-on-Motion mode
  // before sleeping (drops to ~10 µA itself) and restored to normal
  // sampling on wake.
  log::info(kTag, "entering light sleep, motion-wake on GPIO%d, timer=%ums",
            pins::IMU_INT, (unsigned)wakeAfterMs);
  rtcLastSleepUnix = 0;  // wall clock filled in by Session module when known
  rtcLastSleepMillis = millis();

  // Cut radios — saves ~80 mA peak even in light sleep.
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  btStop();

  // MPU-6050 enters low-power WoM mode and pulses INT on motion.
  imu().enableWakeOnMotion();

  // GPIO 19 has no external pull resistor — enable internal pull-down so
  // the pin idles LOW (MPU INT is push-pull active-HIGH after WoM setup)
  // and the wake fires when motion pulses it high.
  gpio_num_t intPin = (gpio_num_t)pins::IMU_INT;
  gpio_pulldown_en(intPin);
  gpio_pullup_dis(intPin);
  gpio_wakeup_enable(intPin, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  // Cap-touch wake on T7.
  touchSleepWakeUpEnable(pins::TOUCH, kTouchWakeThreshold);
  esp_sleep_enable_touchpad_wakeup();

  if (wakeAfterMs > 0) {
    esp_sleep_enable_timer_wakeup((uint64_t)wakeAfterMs * 1000ULL);
  }

  Serial.flush();
  esp_light_sleep_start();

  // We're awake again. Identify the wake source and tidy up.
  lastWake_ = esp_sleep_get_wakeup_cause();
  log::info(kTag, "light-sleep wake cause=%d", (int)lastWake_);

  // Disarm light-sleep-specific wake sources so they don't fire on the
  // next enterDeepSleep call with different intent.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  gpio_wakeup_disable(intPin);

  // Restore the IMU to its normal sampling mode.
  imu().disableWakeOnMotion();
#endif
}

Power& power() {
  if (!g_singleton) g_singleton = new Power();
  return *g_singleton;
}

}  // namespace dash
