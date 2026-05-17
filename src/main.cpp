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
#include "dash/character.h"
#include "dash/debug_cli.h"
#include "dash/display.h"
#include "dash/games.h"
#include "dash/idle_manager.h"
#include "dash/imu.h"
#include "dash/log.h"
#include "dash/portal.h"
#include "dash/power.h"
#include "dash/reset_reason.h"
#include "dash/session.h"
#include "dash/settings.h"
#include "dash/sounds.h"
#include "dash/state_machine.h"
#include "dash/stats.h"
#include "dash/touch.h"
#include "dash/wifi_ap.h"

static uint32_t g_lastHeartbeatMs = 0;
static uint32_t g_faceDownSinceMs = 0;
static uint8_t  g_lastStationCount = 0;
static uint32_t g_lastLookMs = 0;

// ---- Gesture-driven games menu --------------------------------------------
//
// Long-press the cap pad to open. Shake the cube to cycle items, tap to
// select, triple-tap (or 10 s of no input) to exit.
namespace menu {

constexpr const char* kItems[] = { "Reaction", "Bop It", "Exit" };
constexpr uint8_t kItemCount = sizeof(kItems) / sizeof(kItems[0]);
constexpr uint32_t kAutoExitMs = 10000;

uint8_t  index = 0;
uint32_t lastActivityMs = 0;
bool     active = false;

void showCurrent() {
  dash::display().showText("Tap to play", kItems[index]);
}
void enter() {
  if (active) return;
  active = true; index = 0; lastActivityMs = millis();
  dash::log::info("Menu", "open: %s", kItems[index]);
  dash::stateMachine().transitionTo(dash::DeviceState::InMenu);
  dash::character().setMood(dash::Mood::Playful);
  dash::sounds::play(dash::sounds::kMenuBlip, true);
  showCurrent();
}
void exit_(bool playSound = true) {
  if (!active) return;
  active = false;
  dash::log::info("Menu", "close");
  if (playSound) dash::sounds::play(dash::sounds::kMenuBack, true);
  dash::display().clearOverlay();
  dash::character().setMood(dash::Mood::Neutral);
  dash::stateMachine().transitionTo(dash::DeviceState::Idle);
}
void next() {
  lastActivityMs = millis();
  index = (index + 1) % kItemCount;
  dash::log::info("Menu", "→ %s", kItems[index]);
  dash::sounds::play(dash::sounds::kMenuBlip, true);
  showCurrent();
}
void select() {
  lastActivityMs = millis();
  dash::log::info("Menu", "select %s", kItems[index]);
  dash::sounds::play(dash::sounds::kMenuConfirm, true);
  active = false;
  dash::display().clearOverlay();
  const char* item = kItems[index];
  if (strcmp(item, "Exit") == 0) {
    dash::character().setMood(dash::Mood::Neutral);
    dash::stateMachine().transitionTo(dash::DeviceState::Idle);
  } else if (strcmp(item, "Reaction") == 0) {
    dash::games().startGame(dash::GameId::Reaction);
  } else if (strcmp(item, "Bop It") == 0) {
    dash::games().startGame(dash::GameId::BopIt);
  }
}
void tickAutoExit() {
  if (active && millis() - lastActivityMs > kAutoExitMs) exit_(true);
}

}  // namespace menu

namespace {

// IMU event handlers that involve long animations or sound sequences run
// the heavy work on a one-shot FreeRTOS task so the IMU event listener
// chain returns quickly. Otherwise a TripleTap (which plays a 2-second
// sleep sequence) blocks every subsequent event handler — including
// the next Tap or Shake — for the duration of the animation.
namespace deferred {

void sleepSequenceTask(void* /*arg*/) {
  dash::sounds::play(dash::sounds::kTripleTapAck, true);
  vTaskDelay(pdMS_TO_TICKS(280));
  dash::sounds::play(dash::sounds::kSleep, true);
  dash::character().playSleepAnimation();
  dash::power().enterDeepSleep(0);
  vTaskDelete(nullptr);
}

void sessionStartTask(void* arg) {
  uint16_t minutes = (uint16_t)(uintptr_t)arg;
  dash::session().start(minutes);
  vTaskDelete(nullptr);
}

void sessionStopTask(void* /*arg*/) {
  dash::session().stop(false);
  vTaskDelete(nullptr);
}

void faceFlipSleepTask(void* /*arg*/) {
  dash::sounds::play(dash::sounds::kSleep);
  dash::character().playSleepAnimation();
  dash::power().enterDeepSleep(0);
  vTaskDelete(nullptr);
}

}  // namespace deferred

const char* imuEventName(dash::ImuEventType t) {
  using dash::ImuEventType;
  switch (t) {
    case ImuEventType::Tap:              return "Tap";
    case ImuEventType::DoubleTap:        return "DoubleTap";
    case ImuEventType::TripleTap:        return "TripleTap";
    case ImuEventType::Shake:            return "Shake";
    case ImuEventType::OrientationChange:return "OrientationChange";
    case ImuEventType::Stationary:       return "Stationary";
    case ImuEventType::Flick:            return "Flick";
  }
  return "?";
}

// Silence the cube's ambient reactions when something else owns the
// input stream — during games (games.cpp consumes), in the menu
// (menu:: consumes nav), and while the diag walkthrough is open on
// the phone. Without this guard the user sees Dash reacting + state
// transitions happening underneath the test/game they're running.
bool ambientReactionsSilenced() {
  if (dash::games().current() != dash::GameId::None) return true;
  if (menu::active) return true;
  if (dash::portal().diagModeActive()) return true;
  return false;
}

void onImuEvent(const dash::ImuEvent& e) {
  using dash::ImuEventType;
  dash::portal().recordDiagEvent(imuEventName(e.type));
  const bool silenced = ambientReactionsSilenced();
  switch (e.type) {
    case ImuEventType::Tap:
      dash::log::info("Main", "tap (mag=%.2fg)", e.magnitude);
      if (menu::active) { menu::select(); break; }
      if (silenced) break;
      dash::display().blink();
      dash::sounds::playTapAck();
      break;
    case ImuEventType::DoubleTap:
      dash::log::info("Main", "double-tap");
      if (silenced) break;
      // Distinct two-note chime so the user hears "double-tap detected"
      // separately from the per-tap chirps.
      dash::sounds::play(dash::sounds::kDoubleTapAck, true);
      // Session toggle on a one-shot task so we don't block the imu event
      // queue with the session-start animation (~700ms of delays).
      if (dash::settings().onboarded()) {
        auto snap = dash::session().snapshot();
        if (snap.active) {
          xTaskCreate(&deferred::sessionStopTask, "ses-stop", 4096,
                      nullptr, 1, nullptr);
        } else {
          xTaskCreate(&deferred::sessionStartTask, "ses-start", 4096,
                      (void*)(uintptr_t)dash::settings().sessionLengthMin(),
                      1, nullptr);
        }
      } else {
        dash::character().react(dash::EyeState::Surprised, 1200);
      }
      break;
    case ImuEventType::TripleTap:
      dash::log::info("Main", "triple-tap");
      if (menu::active) { menu::exit_(true); break; }
      if (silenced) break;
      // Otherwise: deep-sleep gesture, deferred onto its own task.
      xTaskCreate(&deferred::sleepSequenceTask, "sleep-seq", 4096,
                  nullptr, 1, nullptr);
      break;
    case ImuEventType::Shake:
      dash::log::info("Main", "shake (mag=%.2f)", e.magnitude);
      if (menu::active) { menu::next(); break; }
      if (silenced) break;
      dash::sounds::play(dash::sounds::kWhoa, true);
      dash::character().react(dash::EyeState::Confused, 1500);
      break;
    case ImuEventType::OrientationChange:
      dash::log::info("Main", "face: %s -> %s",
                      dash::faceToString(e.oldFace),
                      dash::faceToString(e.newFace));
      // Curious "huh?" + surprised face whenever the cube gets rotated to
      // a new face. Skip during sessions / games / menus / diagnostic.
      if (!silenced &&
          dash::stateMachine().state() != dash::DeviceState::InSession) {
        dash::sounds::play(dash::sounds::kTilt, true);
        dash::character().react(dash::EyeState::Surprised, 1000);
      }
      // "Flipped to sleep" gesture: any orientation other than Up held
      // for >3s triggers sleep (robust to whichever body axis the IMU
      // calls 'down' — we just detect 'not in the usual orientation').
      if (e.newFace != dash::Face::Up && e.newFace != dash::Face::Unknown) {
        g_faceDownSinceMs = millis();
      } else {
        g_faceDownSinceMs = 0;
      }
      break;
    case ImuEventType::Stationary:
      dash::log::info("Main", "stationary; gyro bias updated");
      break;
    case ImuEventType::Flick:
      dash::log::info("Main", "flick %s (mag=%.2fg)",
                      dash::faceToString(e.newFace), e.magnitude);
      // Flick "forward" (away from user) opens the games menu when in
      // Idle — touchless alternative to long-press. The cap-pad
      // long-press still works.
      if (!menu::active &&
          dash::stateMachine().state() == dash::DeviceState::Idle &&
          (e.newFace == dash::Face::Front || e.newFace == dash::Face::Back)) {
        menu::enter();
        break;
      }
      // Inside a game, games::begin() listener picks the flick up to
      // satisfy bop-it prompts. Skip the ambient boop when something
      // else is using the input stream.
      if (!silenced) {
        dash::sounds::play(dash::sounds::kBoop, true);
        dash::character().react(dash::EyeState::Surprised, 800);
      }
      break;
  }
}

// Cap-touch is treated as a redundant tap input — same UX as an IMU tap.
// Touch        → tap_ack chirp + blink (just like onImuEvent::Tap).
// DoubleTouch  → session toggle (just like onImuEvent::DoubleTap).
// LongPress    → deep-sleep gesture (parallel to triple-tap).
void onTouchEvent(const dash::TouchEvent& e) {
  const bool silenced = ambientReactionsSilenced();
  switch (e.type) {
    case dash::TouchEventType::Touch:
      dash::log::info("Main", "touch raw=%u → tap", e.rawValue);
      dash::portal().recordDiagEvent("Touch");
      if (silenced) break;
      dash::display().blink();
      dash::sounds::playTapAck();
      break;
    case dash::TouchEventType::DoubleTouch:
      dash::log::info("Main", "double-touch → session toggle");
      dash::portal().recordDiagEvent("TouchDouble");
      if (silenced) break;
      if (dash::settings().onboarded()) {
        auto snap = dash::session().snapshot();
        if (snap.active) {
          xTaskCreate(&deferred::sessionStopTask, "ses-stop", 4096,
                      nullptr, 1, nullptr);
        } else {
          xTaskCreate(&deferred::sessionStartTask, "ses-start", 4096,
                      (void*)(uintptr_t)dash::settings().sessionLengthMin(),
                      1, nullptr);
        }
      } else {
        dash::character().react(dash::EyeState::Surprised, 1200);
      }
      break;
    case dash::TouchEventType::LongPress:
      dash::log::info("Main", "long-press → menu");
      dash::portal().recordDiagEvent("TouchLong");
      if (menu::active) {
        menu::exit_(true);
      } else if (dash::stateMachine().state() == dash::DeviceState::Idle) {
        menu::enter();
      }
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
    dash::display().start();
  }

  if (dash::imu().begin()) {
    dash::imu().setTapThreshold(dash::settings().tapSensitivityG());
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

  // Character first so it can drive the boot animation.
  dash::character().begin();
  dash::character().start();

  // Settings (NVS) up front so other modules can read.
  dash::settings().begin();

  // Audio volume from settings (silent override stays under DASH_SILENT_AUDIO).
  dash::audio().setVolume(dash::settings().audioVolume());

  // State machine + idle manager.
  // First-boot users land in Onboarding; portal wizard transitions them to
  // Idle once they finish.
  if (dash::settings().onboarded()) {
    dash::stateMachine().transitionTo(dash::DeviceState::Idle);
  } else {
    dash::stateMachine().transitionTo(dash::DeviceState::Onboarding);
    dash::character().setMood(dash::Mood::Listening);
  }
  dash::idleManager().setSleepTimeoutSec(dash::settings().sleepTimeoutSec());
  dash::idleManager().begin();
  dash::idleManager().start();

  // Stats + session controller + games.
  dash::stats().begin();
  dash::session().begin();
  dash::games().begin();

  // WiFi AP + captive portal.
  if (dash::wifiAp().start()) {
    dash::portal().begin();
  }

  // Boot vs wake animation. If we came from deep sleep, skip the splash and
  // run a shorter wake sequence; otherwise the full Dash boot.
  if (dash::power().lastWakeCause() == ESP_SLEEP_WAKEUP_TOUCHPAD ||
      dash::power().lastWakeCause() == ESP_SLEEP_WAKEUP_EXT0 ||
      dash::power().lastWakeCause() == ESP_SLEEP_WAKEUP_TIMER) {
    dash::sounds::play(dash::sounds::kWake);
    dash::character().playWakeAnimation();
  } else {
    dash::sounds::play(dash::sounds::kBoot);
    dash::character().playBootAnimation();
  }

  // First-time users haven't seen the portal yet — show the SSID on the
  // OLED so they know which network to join. Stays on screen until they
  // connect their phone (the loop swaps it to a "open dash.local" hint
  // once a station associates).
  if (!dash::settings().onboarded()) {
    dash::display().showText("Connect to:", dash::wifiAp().ssid().c_str());
  }

  // Debug CLI on serial. Type 'help' for the command list, 'selftest' to
  // run the automated walkthrough.
  dash::debugCli().begin();
  dash::debugCli().start();

  dash::log::info("Main", "setup complete");
}

void loop() {
  const uint32_t now = millis();

  // Flip-to-sleep gesture: cube held in any non-Up orientation for >3s.
  // Skip while a session is running so the user can pick the cube up
  // without accidentally sleeping mid-focus.
  if (g_faceDownSinceMs != 0 && (now - g_faceDownSinceMs) > 3000 &&
      dash::stateMachine().state() != dash::DeviceState::InSession) {
    dash::log::info("Main", "flip-to-sleep gesture -> sleep");
    g_faceDownSinceMs = 0;
    xTaskCreate(&deferred::faceFlipSleepTask, "flip-sleep", 4096, nullptr, 1, nullptr);
  }

  // Show a URL hint on the OLED the moment a phone associates with the AP
  // (transition from 0 → 1+ stations). Clears the hint when they disconnect.
  const uint8_t stationCount = dash::wifiAp().stationCount();
  if (stationCount != g_lastStationCount) {
    if (g_lastStationCount == 0 && stationCount > 0) {
      dash::log::info("Main", "phone connected — showing portal hint");
      dash::display().showText("Open in browser", "dash.local");
      dash::character().react(dash::EyeState::Happy, 1500);
    } else if (stationCount == 0) {
      dash::log::info("Main", "phone disconnected");
      dash::display().clearOverlay();
    }
    g_lastStationCount = stationCount;
  }

  // Menu auto-exits after 10s of inactivity so it gets out of the way.
  menu::tickAutoExit();

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
