// main.cpp — M1 bring-up with stepwise init for crash isolation.
//
// Each peripheral begin() is fenced with a log line so a boot-loop tells us
// which step is failing. Once all init succeeds we drop into the event loop.

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "dash/audio.h"
#include "dash/build_info.h"
#include "dash/display.h"
#include "dash/imu.h"
#include "dash/log.h"
#include "dash/reset_reason.h"
#include "dash/touch.h"

static uint32_t g_lastHeartbeatMs = 0;

namespace {

void onImuEvent(const dash::ImuEvent& e) {
  using dash::ImuEventType;
  switch (e.type) {
    case ImuEventType::Tap:
      dash::log::info("Main", "tap (mag=%.2fg)", e.magnitude);
      dash::display().blink();
      break;
    case ImuEventType::DoubleTap:
      dash::log::info("Main", "double-tap");
      dash::display().setEyeState(dash::EyeState::Surprised);
      break;
    case ImuEventType::TripleTap:
      dash::log::info("Main", "triple-tap");
      dash::display().setEyeState(dash::EyeState::Happy);
      break;
    case ImuEventType::Shake:
      dash::log::info("Main", "shake (mag=%.2f)", e.magnitude);
      dash::display().setEyeState(dash::EyeState::Confused);
      break;
    case ImuEventType::OrientationChange:
      dash::log::info("Main", "face: %s -> %s",
                      dash::faceToString(e.oldFace),
                      dash::faceToString(e.newFace));
      break;
    case ImuEventType::Stationary:
      dash::log::info("Main", "stationary; gyro bias updated");
      break;
  }
}

void onTouchEvent(const dash::TouchEvent& e) {
  switch (e.type) {
    case dash::TouchEventType::Touch:
      dash::log::info("Main", "touch raw=%u", e.rawValue);
      break;
    case dash::TouchEventType::DoubleTouch:
      dash::log::info("Main", "double-touch");
      dash::display().setEyeState(dash::EyeState::Happy);
      break;
    case dash::TouchEventType::LongPress:
      dash::log::info("Main", "long-press");
      dash::display().setEyeState(dash::EyeState::Sleepy);
      break;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // Give USB-UART time to enumerate AND for any prior dump to drain.
  for (int i = 0; i < 40 && !Serial; i++) delay(50);
  delay(500);
  Serial.println();
  Serial.println(F(">>>>>> Dash setup() begin <<<<<<"));

  const esp_reset_reason_t reset = esp_reset_reason();
  const esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();

  Serial.println(F("================ Dash boot ================"));
  Serial.printf("firmware   : %s\n", dash::kFirmwareVersion);
  Serial.printf("reset      : %s (%d)\n", dash::resetReasonString(reset), reset);
  Serial.printf("wake cause : %s (%d)\n", dash::wakeCauseString(wake), wake);
  Serial.printf("cpu freq   : %u MHz\n", (unsigned)getCpuFrequencyMhz());
  Serial.printf("free heap  : %u (largest %u)\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  Serial.println(F("==========================================="));

  Serial.println("STEP 1: Display::begin");
  Serial.flush();
  if (!dash::display().begin()) {
    dash::log::error("Main", "display init failed — continuing without it");
  } else {
    Serial.println("STEP 1: OK -> showBootSplash + start");
    Serial.flush();
    dash::display().showBootSplash();
    dash::display().start();
    delay(800);
    dash::display().clearOverlay();
    dash::display().setEyeState(dash::EyeState::Idle);
  }

  Serial.println("STEP 2: Imu::begin");
  Serial.flush();
  if (!dash::imu().begin()) {
    dash::log::error("Main", "IMU init failed");
  } else {
    Serial.println("STEP 2: OK -> imu.start");
    Serial.flush();
    dash::imu().onEvent(onImuEvent);
    dash::imu().start();
  }

  Serial.println("STEP 3: Audio::begin");
  Serial.flush();
  if (!dash::audio().begin()) {
    dash::log::error("Main", "audio init failed");
  } else {
    Serial.println("STEP 3: OK -> audio.start");
    Serial.flush();
#ifdef DASH_SILENT_AUDIO
    dash::audio().setSilent(true);
    dash::log::info("Main", "audio silent mode active (debug build)");
#endif
    dash::audio().start();
  }

  Serial.println("STEP 4: Touch::begin");
  Serial.flush();
  if (!dash::touch().begin()) {
    dash::log::warn("Main", "touch baseline unstable");
  }
  Serial.println("STEP 4: OK -> touch.start");
  Serial.flush();
  dash::touch().onEvent(onTouchEvent);
  dash::touch().start();

  dash::log::info("Main", "setup complete");
}

void loop() {
  const uint32_t now = millis();
  if (now - g_lastHeartbeatMs >= 10000) {
    g_lastHeartbeatMs = now;
    auto s = dash::imu().latest();
    dash::log::info("Main", "uptime=%lus heap=%u face=%s pitch=%.1f roll=%.1f",
                    (unsigned long)(now / 1000),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                    dash::faceToString(dash::imu().currentFace()),
                    s.pitch, s.roll);
  }
  delay(50);
}
