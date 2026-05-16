// main.cpp — Dash firmware entry point.
//
// Through M2: peripherals + state machine + drowsy progression + deep sleep.
// IMU events drive both interaction (taps/shake change eye state) and the
// idle clock. Face-down for >2s is a fast-path to deep sleep, mirroring the
// reference firmware's gesture.

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "dash/audio.h"
#include "dash/build_info.h"
#include "dash/display.h"
#include "dash/idle_manager.h"
#include "dash/imu.h"
#include "dash/log.h"
#include "dash/power.h"
#include "dash/reset_reason.h"
#include "dash/sounds.h"
#include "dash/state_machine.h"
#include "dash/touch.h"

static uint32_t g_lastHeartbeatMs = 0;
static uint32_t g_faceDownSinceMs = 0;

namespace {

void onImuEvent(const dash::ImuEvent& e) {
  using dash::ImuEventType;
  switch (e.type) {
    case ImuEventType::Tap:
      dash::log::info("Main", "tap (mag=%.2fg)", e.magnitude);
      dash::display().blink();
      dash::sounds::play(dash::sounds::kTapAck);
      break;
    case ImuEventType::DoubleTap:
      dash::log::info("Main", "double-tap");
      dash::display().setEyeState(dash::EyeState::Surprised);
      break;
    case ImuEventType::TripleTap:
      dash::log::info("Main", "triple-tap (deep-sleep gesture)");
      dash::display().setEyeState(dash::EyeState::Sleepy);
      delay(500);
      dash::power().enterDeepSleep(0);
      break;
    case ImuEventType::Shake:
      dash::log::info("Main", "shake (mag=%.2f)", e.magnitude);
      dash::display().setEyeState(dash::EyeState::Confused);
      break;
    case ImuEventType::OrientationChange:
      dash::log::info("Main", "face: %s -> %s",
                      dash::faceToString(e.oldFace),
                      dash::faceToString(e.newFace));
      if (e.newFace == dash::Face::Down) {
        g_faceDownSinceMs = millis();
      } else {
        g_faceDownSinceMs = 0;
      }
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
      dash::log::info("Main", "double-touch -> toggle session placeholder");
      break;
    case dash::TouchEventType::LongPress:
      dash::log::info("Main", "long-press -> menu placeholder");
      break;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 40 && !Serial; i++) delay(50);
  delay(200);
  Serial.println();

  dash::power().begin();   // captures wake cause + bumps boot count

  const esp_reset_reason_t reset = esp_reset_reason();
  Serial.println(F("================ Dash boot ================"));
  Serial.printf("firmware   : %s\n", dash::kFirmwareVersion);
  Serial.printf("boot count : %u\n", (unsigned)dash::power().bootCount());
  Serial.printf("reset      : %s (%d)\n", dash::resetReasonString(reset), reset);
  Serial.printf("wake cause : %s (%d)\n",
                dash::wakeCauseString(dash::power().lastWakeCause()),
                dash::power().lastWakeCause());
  Serial.printf("cpu freq   : %u MHz\n", (unsigned)getCpuFrequencyMhz());
  Serial.printf("free heap  : %u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
  Serial.println(F("==========================================="));

  // Display first — it owns u8g2 / I2C init.
  if (dash::display().begin()) {
    dash::display().showBootSplash();
    dash::display().start();
    delay(800);
    dash::display().clearOverlay();
    dash::display().setEyeState(dash::EyeState::Idle);
  }

  if (dash::imu().begin()) {
    dash::imu().onEvent(onImuEvent);
    dash::imu().start();
  }

  if (dash::audio().begin()) {
#ifdef DASH_SILENT_AUDIO
    dash::audio().setSilent(true);
    dash::log::info("Main", "audio silent mode active (debug build)");
#endif
    dash::audio().start();
  }

  dash::touch().begin();
  dash::touch().onEvent(onTouchEvent);
  dash::touch().start();

  // State machine + idle manager.
  dash::stateMachine().transitionTo(dash::DeviceState::Idle);
  dash::idleManager().begin();
  dash::idleManager().start();

  // Boot chime (silent under DASH_SILENT_AUDIO).
  dash::sounds::play(dash::sounds::kBoot);

  dash::log::info("Main", "setup complete");
}

void loop() {
  const uint32_t now = millis();

  // Face-down fast-path to sleep (cube flipped face-down for >2s).
  if (g_faceDownSinceMs != 0 && (now - g_faceDownSinceMs) > 2000) {
    dash::log::info("Main", "face-down > 2s -> deep sleep");
    g_faceDownSinceMs = 0;
    dash::display().setEyeState(dash::EyeState::Sleepy);
    delay(500);
    dash::power().enterDeepSleep(0);
  }

  if (now - g_lastHeartbeatMs >= 10000) {
    g_lastHeartbeatMs = now;
    auto s = dash::imu().latest();
    dash::log::info("Main", "uptime=%lus heap=%u state=%s face=%s pitch=%.1f roll=%.1f",
                    (unsigned long)(now / 1000),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                    dash::deviceStateName(dash::stateMachine().state()),
                    dash::faceToString(dash::imu().currentFace()),
                    s.pitch, s.roll);
  }
  delay(50);
}
