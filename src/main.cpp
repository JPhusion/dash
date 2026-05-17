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

// ---- Face-up tilt menu ----------------------------------------------------
//
// Opens when the cube has been "screen up" (display facing the ceiling) for
// >= kOpenHoldMs. Top level is a vertical list (Games | Volume | Session min
// | Sleep timeout | Tap sens). Tilting the cube forward/back (away/toward
// the user) cycles items; tap selects.
//
// Selecting Games opens a vertical games submenu (Reaction | Bop It); tap
// launches the game.
//
// Selecting a setting enters Edit mode for that setting: a horizontal bar
// + value readout; tilt left/right decrement/increment in fixed steps;
// tap returns to the top level.
//
// Exit: when the cube has been NOT face-up for >= kCloseHoldMs the menu
// closes and the device returns to Idle. (No "Back" item — putting the
// cube down is always the exit.)
//
// The Face value that counts as "screen up" is calibrated per cube — set
// kMenuFace once the ball-overlay testing reveals which Face enum value
// corresponds to the user's screen-up orientation.
namespace menu {

// Calibrated 2026-05-17 via `orient` walkthrough on this cube. "Screen
// up" puts -X dominant in the IMU's gravity vector → Face::Left.
constexpr dash::Face kMenuFace = dash::Face::Left;
constexpr uint32_t kOpenHoldMs  = 2000;
constexpr uint32_t kCloseHoldMs = 2000;

enum class Mode : uint8_t {
  Closed,
  TopLevel,
  GamesSubmenu,
  Editing,
};

enum TopItem : uint8_t {
  kTopGames = 0,
  kTopVolume,
  kTopSessionMin,
  kTopSleepSec,
  kTopTapSens,
  kTopCount,
};

constexpr const char* kTopLabels[kTopCount] = {
  "Games", "Volume", "Session min", "Sleep timeout", "Tap sens"
};

constexpr const char* kGamesLabels[] = { "Reaction", "Bop It" };
constexpr uint8_t     kGamesCount    = 2;

Mode    mode      = Mode::Closed;
uint8_t topIndex  = 0;
uint8_t gamesIndex = 0;
TopItem editing   = kTopVolume;     // valid only while mode == Editing

// Polling-based tilt nav state. Baseline gravity is captured when the
// menu opens; each main-loop tick the cube's current gravity is sampled
// and compared to the baseline. When the dominant non-face-normal
// component crosses its engage threshold we fire one nav action; the
// pollEngaged gate then waits for |deviation| to drop below
// kPollDisengage before another nav action can fire.
//
// Per-direction thresholds — left/right is a smaller motion than
// up/down on this cube (tilting-left tops out at ~0.33 deviation while
// tilting-away reaches ~0.64) so we make left/right easier to trigger.
//   up/down       0.225  (75% of the original symmetric 0.30)
//   left/right    0.15   (50% of the original)
// Disengage sits below both so each direction has hysteresis.
//
// This bypasses the IMU Tilt event entirely: those events get cancelled
// when the cube's gravity crosses the face hysteresis threshold mid-tilt,
// which is exactly the magnitude the user naturally tilts at. Polling
// the raw gravity vector here is robust to that.
constexpr float kPollEngageUpDown    = 0.225f;
constexpr float kPollEngageLeftRight = 0.15f;
constexpr float kPollDisengage       = 0.08f;
// Hold-to-repeat: while the cube is held tilted left or right past the
// engage threshold, re-fire the same nav action every kPollRepeatMs.
// Up/down doesn't repeat — scrolling a list past where you want with one
// tilt would feel awful — so this only kicks in for kTiltInc / kTiltDec.
constexpr uint32_t kPollRepeatMs     = 500;   // 2 Hz
float    baseGx = 0.0f, baseGy = 0.0f, baseGz = 0.0f;
bool     baseValid = false;
bool     pollEngaged = false;
uint32_t pollLastFireMs = 0;
dash::Face pollLastDir = dash::Face::Unknown;

bool isOpen() { return mode != Mode::Closed; }

// Render whichever screen matches current mode.
void render();

void close() {
  if (mode == Mode::Closed) return;
  dash::log::info("Menu", "close");
  mode = Mode::Closed;
  baseValid = false;
  pollEngaged = false;
  dash::sounds::play(dash::sounds::kMenuBack, true);
  dash::display().clearOverlay();
  dash::character().setMood(dash::Mood::Neutral);
  if (dash::stateMachine().state() == dash::DeviceState::InMenu) {
    dash::stateMachine().transitionTo(dash::DeviceState::Idle);
  }
}

void open() {
  if (mode != Mode::Closed) return;
  dash::log::info("Menu", "open");
  mode = Mode::TopLevel;
  topIndex = 0;
  // Snapshot the gravity vector NOW — used as the baseline for the
  // polling tilt detector below. The cube has been screen-up for >=
  // kOpenHoldMs by definition so this baseline should be stable.
  {
    auto s = dash::imu().latest();
    baseGx = s.gravityX;
    baseGy = s.gravityY;
    baseGz = s.gravityZ;
    baseValid = true;
    pollEngaged = false;
  }
  dash::sounds::play(dash::sounds::kMenuBlip, true);
  dash::stateMachine().transitionTo(dash::DeviceState::InMenu);
  dash::character().setMood(dash::Mood::Playful);
  render();
}

// Snapshot a setting's current value into a 0..100 bar percentage and a
// human-readable value string.
struct EditSnapshot {
  const char* name;
  uint8_t pct;
  char value[16];
};
EditSnapshot snapshot(TopItem t) {
  EditSnapshot s{};
  switch (t) {
    case kTopVolume: {
      uint8_t v = dash::settings().audioVolume();
      s.name = "Volume";
      s.pct = v;
      snprintf(s.value, sizeof(s.value), "%u", (unsigned)v);
      break;
    }
    case kTopSessionMin: {
      uint16_t m = dash::settings().sessionLengthMin();
      s.name = "Session min";
      // 5..90 → 0..100 pct
      int span = 90 - 5;
      int rel = (int)m - 5;
      if (rel < 0) rel = 0;
      s.pct = (uint8_t)((rel * 100) / span);
      snprintf(s.value, sizeof(s.value), "%u min", (unsigned)m);
      break;
    }
    case kTopSleepSec: {
      uint16_t sec = dash::settings().sleepTimeoutSec();
      s.name = "Sleep timeout";
      // 60..900 → 0..100
      int span = 900 - 60;
      int rel = (int)sec - 60;
      if (rel < 0) rel = 0;
      s.pct = (uint8_t)((rel * 100) / span);
      snprintf(s.value, sizeof(s.value), "%u s", (unsigned)sec);
      break;
    }
    case kTopTapSens: {
      float g = dash::settings().tapSensitivityG();
      s.name = "Tap sens";
      // 0.30..1.00g → 0..100
      float rel = (g - 0.30f) / 0.70f;
      if (rel < 0) rel = 0; if (rel > 1) rel = 1;
      s.pct = (uint8_t)(rel * 100.0f);
      snprintf(s.value, sizeof(s.value), "%.2fg", g);
      break;
    }
    default:
      s.name = "";
      s.pct = 0;
      s.value[0] = '\0';
      break;
  }
  return s;
}

// Adjust the editing setting by one step. delta is +1 or -1.
void adjust(int delta) {
  switch (editing) {
    case kTopVolume: {
      int v = (int)dash::settings().audioVolume() + delta * 5;
      if (v < 0) v = 0; if (v > 100) v = 100;
      dash::settings().setAudioVolume((uint8_t)v);
      dash::audio().setVolume((uint8_t)v);
      break;
    }
    case kTopSessionMin: {
      int v = (int)dash::settings().sessionLengthMin() + delta * 5;
      if (v < 5) v = 5; if (v > 90) v = 90;
      dash::settings().setSessionLengthMin((uint16_t)v);
      break;
    }
    case kTopSleepSec: {
      int v = (int)dash::settings().sleepTimeoutSec() + delta * 30;
      if (v < 60) v = 60; if (v > 900) v = 900;
      dash::settings().setSleepTimeoutSec((uint16_t)v);
      dash::idleManager().setSleepTimeoutSec((uint16_t)v);
      break;
    }
    case kTopTapSens: {
      float g = dash::settings().tapSensitivityG() + delta * 0.05f;
      if (g < 0.30f) g = 0.30f; if (g > 1.00f) g = 1.00f;
      dash::settings().setTapSensitivityG(g);
      dash::imu().setTapThreshold(g);
      break;
    }
    default: return;
  }
}

void render() {
  switch (mode) {
    case Mode::Closed:
      return;
    case Mode::TopLevel: {
      const char* prev = (topIndex == 0) ? "" : kTopLabels[topIndex - 1];
      const char* cur  = kTopLabels[topIndex];
      const char* next = (topIndex >= kTopCount - 1) ? "" : kTopLabels[topIndex + 1];
      dash::display().showMenuList("MENU", prev, cur, next);
      break;
    }
    case Mode::GamesSubmenu: {
      const char* prev = (gamesIndex == 0) ? "" : kGamesLabels[gamesIndex - 1];
      const char* cur  = kGamesLabels[gamesIndex];
      const char* next = (gamesIndex >= kGamesCount - 1) ? "" : kGamesLabels[gamesIndex + 1];
      dash::display().showMenuList("GAMES", prev, cur, next);
      break;
    }
    case Mode::Editing: {
      EditSnapshot s = snapshot(editing);
      dash::display().showMenuEdit(s.name, s.pct, s.value);
      break;
    }
  }
}

// Tilt nav. Cube is in screen-up (Face::Left) while the menu is open.
// Mappings calibrated 2026-05-17:
//   tilt away from user → gz deviation negative → Tilt newFace = Face::Down
//   tilt toward user    → gz deviation positive → Tilt newFace = Face::Up
//   tilt right          → gy deviation positive → Tilt newFace = Face::Back
//   tilt left           → gy deviation negative → Tilt newFace = Face::Front
constexpr dash::Face kTiltUp   = dash::Face::Down;   // away → scroll up
constexpr dash::Face kTiltDown = dash::Face::Up;     // toward → scroll down
constexpr dash::Face kTiltInc  = dash::Face::Back;   // right → bar +
constexpr dash::Face kTiltDec  = dash::Face::Front;  // left  → bar -

void onTiltDirection(dash::Face dir) {
  if (mode == Mode::Closed) return;
  if (mode == Mode::TopLevel) {
    if (dir == kTiltUp)   {
      if (topIndex > 0) topIndex--;
      dash::sounds::play(dash::sounds::kMenuBlip, true);
      render();
    } else if (dir == kTiltDown) {
      if (topIndex < kTopCount - 1) topIndex++;
      dash::sounds::play(dash::sounds::kMenuBlip, true);
      render();
    }
    return;
  }
  if (mode == Mode::GamesSubmenu) {
    if (dir == kTiltUp)   {
      if (gamesIndex > 0) gamesIndex--;
      dash::sounds::play(dash::sounds::kMenuBlip, true);
      render();
    } else if (dir == kTiltDown) {
      if (gamesIndex < kGamesCount - 1) gamesIndex++;
      dash::sounds::play(dash::sounds::kMenuBlip, true);
      render();
    }
    return;
  }
  if (mode == Mode::Editing) {
    if (dir == kTiltInc) {
      adjust(+1);
      dash::sounds::play(dash::sounds::kMenuBlip, true);
      render();
    } else if (dir == kTiltDec) {
      adjust(-1);
      dash::sounds::play(dash::sounds::kMenuBlip, true);
      render();
    }
    return;
  }
}

void onTap() {
  switch (mode) {
    case Mode::Closed:
      return;
    case Mode::TopLevel: {
      dash::sounds::play(dash::sounds::kMenuConfirm, true);
      if (topIndex == kTopGames) {
        mode = Mode::GamesSubmenu;
        gamesIndex = 0;
        render();
      } else {
        mode = Mode::Editing;
        editing = (TopItem)topIndex;
        render();
      }
      break;
    }
    case Mode::GamesSubmenu: {
      dash::sounds::play(dash::sounds::kMenuConfirm, true);
      mode = Mode::Closed;
      dash::display().clearOverlay();
      if (gamesIndex == 0)      dash::games().startGame(dash::GameId::Reaction);
      else if (gamesIndex == 1) dash::games().startGame(dash::GameId::BopIt);
      break;
    }
    case Mode::Editing: {
      dash::sounds::play(dash::sounds::kMenuConfirm, true);
      mode = Mode::TopLevel;
      render();
      break;
    }
  }
}

// Polling tilt detector — called from main loop() while the menu is
// open. Compares live gravity to the baseline captured at open(), masks
// the face-normal axis, fires one nav action on threshold crossing.
// State resets when gravity returns to within kPollDisengage of baseline.
void pollTilt() {
  if (mode == Mode::Closed) return;
  if (!baseValid) return;
  auto s = dash::imu().latest();
  float dX = s.gravityX - baseGx;
  float dY = s.gravityY - baseGy;
  float dZ = s.gravityZ - baseGz;
  // Mask the face-normal axis so a deviation along the resting axis
  // (which can drift from gravity-vector wobble) doesn't bias direction
  // selection.
  float refAx = fabsf(baseGx), refAy = fabsf(baseGy), refAz = fabsf(baseGz);
  if (refAx >= refAy && refAx >= refAz) dX = 0.0f;
  else if (refAy >= refAz)              dY = 0.0f;
  else                                  dZ = 0.0f;
  float devMag = sqrtf(dX*dX + dY*dY + dZ*dZ);

  // Always compute the candidate direction from current deviation.
  float absX = fabsf(dX), absY = fabsf(dY), absZ = fabsf(dZ);
  dash::Face dir;
  float componentMag;
  if (absX >= absY && absX >= absZ) {
    dir = (dX > 0) ? dash::Face::Right : dash::Face::Left;
    componentMag = absX;
  } else if (absY >= absZ) {
    dir = (dY > 0) ? dash::Face::Back  : dash::Face::Front;
    componentMag = absY;
  } else {
    dir = (dZ > 0) ? dash::Face::Up    : dash::Face::Down;
    componentMag = absZ;
  }
  const bool isUpDown = (dir == kTiltUp || dir == kTiltDown);
  const float engage = isUpDown ? kPollEngageUpDown : kPollEngageLeftRight;
  const uint32_t now = millis();

  if (!pollEngaged) {
    if (componentMag > engage) {
      dash::log::info("Menu", "poll tilt fire dir=%s comp=%.2f thr=%.2f",
                      dash::faceToString(dir), componentMag, engage);
      onTiltDirection(dir);
      pollEngaged = true;
      pollLastFireMs = now;
      pollLastDir = dir;
    }
    return;
  }

  // Engaged path. Drop back to neutral once gravity returns close to
  // baseline, otherwise check for hold-to-repeat (left/right only).
  if (devMag < kPollDisengage) {
    pollEngaged = false;
    return;
  }
  const bool isLR = (pollLastDir == kTiltInc || pollLastDir == kTiltDec);
  if (isLR &&
      dir == pollLastDir &&                       // still tilted same way
      componentMag > kPollEngageLeftRight &&      // still past threshold
      (now - pollLastFireMs) > kPollRepeatMs) {
    onTiltDirection(pollLastDir);
    pollLastFireMs = now;
  }
}

}  // namespace menu

// Test hooks for the serial CLI — let `debug_cli` exercise the menu state
// machine end-to-end without needing physical tilt events. Bridges into
// the anonymous-namespace menu:: functions above (same translation unit
// so this works directly).
namespace dash::menutest {
  // When set, the loop()'s face-not-up auto-close logic is skipped so the
  // CLI can drive the menu over a longer span than 2 s without the cube
  // physically held face-up.
  bool autoCloseSuspended = false;

  bool isOpen()            { return menu::isOpen(); }
  void open()              { menu::open(); }
  void close()             { menu::close(); }
  void tap()               { menu::onTap(); }
  void tilt(dash::Face d)  { menu::onTiltDirection(d); }
  uint8_t mode()           { return (uint8_t)menu::mode; }
  uint8_t topIndex()       { return menu::topIndex; }
  uint8_t gamesIndex()     { return menu::gamesIndex; }
  const char* topLabel(uint8_t i) {
    return (i < menu::kTopCount) ? menu::kTopLabels[i] : "";
  }
  void setAutoClose(bool on) { autoCloseSuspended = !on; }
  bool autoCloseEnabled()    { return !autoCloseSuspended; }
  // Expose the per-cube nav mapping so the CLI can offer an
  // action-named nav command independent of face axes.
  dash::Face navUpFace()    { return menu::kTiltUp; }
  dash::Face navDownFace()  { return menu::kTiltDown; }
  dash::Face navIncFace()   { return menu::kTiltInc; }
  dash::Face navDecFace()   { return menu::kTiltDec; }
}

namespace {

// IMU event handlers that involve long animations or sound sequences run
// the heavy work on a one-shot FreeRTOS task so the IMU event listener
// chain returns quickly. Otherwise a TripleTap (which plays a 2-second
// sleep sequence) blocks every subsequent event handler — including
// the next Tap or Shake — for the duration of the animation.
namespace deferred {

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
    case ImuEventType::Spin:             return "Spin";
    case ImuEventType::Tilt:             return "Tilt";
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
  if (menu::isOpen()) return true;
  if (dash::portal().diagModeActive()) return true;
  if (dash::display().overlay() == dash::Overlay::GravityBall) return true;
  return false;
}

void onImuEvent(const dash::ImuEvent& e) {
  using dash::ImuEventType;
  dash::portal().recordDiagEvent(imuEventName(e.type));
  const bool silenced = ambientReactionsSilenced();
  switch (e.type) {
    case ImuEventType::Tap:
      dash::log::info("Main", "tap (mag=%.2fg)", e.magnitude);
      if (menu::isOpen()) { menu::onTap(); break; }
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
      if (silenced) break;
      // Sleep is now driven only by holding the cube face-down for ~3s
      // (see the flip-to-sleep block in loop()). Triple-tap here just
      // plays the acknowledgement chirp + a celebrating reaction so the
      // user gets feedback the gesture was detected but doesn't
      // accidentally sleep the device.
      dash::sounds::play(dash::sounds::kTripleTapAck, true);
      dash::character().react(dash::EyeState::Happy, 1200);
      break;
    case ImuEventType::Shake:
      dash::log::info("Main", "shake (mag=%.2f)", e.magnitude);
      if (silenced) break;
      // A really hard shake makes Dash dizzy; lighter shakes just confuse.
      // Magnitude reflects running-variance sqrt — a normal shake comes in
      // around 0.8–1.2, a sustained vigorous one tops 1.6.
      if (e.magnitude > 1.3f) {
        dash::sounds::play(dash::sounds::kDizzy, true);
        dash::character().playDizzyAnimation(1400);
      } else {
        dash::sounds::play(dash::sounds::kConfused, true);
        dash::character().react(dash::EyeState::Confused, 1500);
      }
      break;
    case ImuEventType::OrientationChange:
      dash::log::info("Main", "face: %s -> %s",
                      dash::faceToString(e.oldFace),
                      dash::faceToString(e.newFace));
      // Per the 2026-05-17 orient calibration on this cube:
      //   Face::Up    = screen-forward (neutral resting pose)  — no reaction
      //   Face::Down  = upside-down, cap-touch face on table   — "Surprised!"
      //   Face::Left  = screen up    (menu trigger)            — Curious
      //   Face::Right = screen down  (sleep trigger)           — Curious
      //   Face::Back  = screen rotated right                   — Curious
      //   Face::Front = screen rotated left                    — Curious
      if (!silenced &&
          dash::stateMachine().state() != dash::DeviceState::InSession &&
          e.newFace != dash::Face::Up &&
          e.newFace != dash::Face::Unknown) {
        const bool flipped = (e.newFace == dash::Face::Down);
        dash::sounds::play(flipped ? dash::sounds::kSurprised
                                   : dash::sounds::kCurious, true);
        dash::character().react(
            flipped ? dash::EyeState::Surprised : dash::EyeState::SideEye,
            1400);
        dash::character().lookAtGravity(1200);
      }
      // Screen-down to sleep: cube screen facing the table = +X dominant
      // gravity = Face::Right. Held >3s triggers deep sleep.
      if (e.newFace == dash::Face::Right) {
        g_faceDownSinceMs = millis();
      } else {
        g_faceDownSinceMs = 0;
      }
      break;
    case ImuEventType::Stationary:
      dash::log::info("Main", "stationary; gyro bias updated");
      break;
    case ImuEventType::Spin:
      dash::log::info("Main", "spin %s (deg=%.0f)",
                      e.newFace == dash::Face::Right ? "right" : "left",
                      e.magnitude);
      if (silenced) break;
      dash::sounds::play(dash::sounds::kDizzy, true);
      dash::character().playDizzyAnimation(1500);
      break;
    case ImuEventType::Tilt:
      dash::log::info("Main", "tilt dir=%s (mag=%.2f)",
                      dash::faceToString(e.newFace), e.magnitude);
      // Menu nav is driven by the polling tilt detector in menu::pollTilt
      // (called from loop()), NOT by Tilt events. The IMU Tilt detector
      // can't see a tilt that crosses the face hysteresis threshold —
      // exactly the magnitude users naturally tilt at — so it's
      // unreliable for menu nav. Tilt events here are purely for
      // ambient reactions when no consumer owns the input stream.
      if (silenced) break;
      // Ambient reaction: chirp + side-eye in roughly the tilt direction.
      dash::sounds::play(dash::sounds::kCurious, true);
      dash::character().react(dash::EyeState::SideEye, 1100);
      {
        // Map the tilt direction to a screen-look (x, y). Signs are
        // best-effort until cube calibration is complete.
        float lx = 0.0f, ly = 0.0f;
        switch (e.newFace) {
          case dash::Face::Right: lx =  0.9f; break;
          case dash::Face::Left:  lx = -0.9f; break;
          case dash::Face::Front: ly = -0.9f; break;
          case dash::Face::Back:  ly =  0.9f; break;
          default: break;
        }
        dash::display().setAutoLook(false);
        dash::display().lookAt(lx, ly);
        struct Ret { static void task(void*) {
          vTaskDelay(pdMS_TO_TICKS(1100));
          dash::display().lookAt(0.0f, 0.0f);
          dash::display().setAutoLook(true);
          vTaskDelete(nullptr);
        }};
        xTaskCreate(&Ret::task, "tilt-look-ret", 2048, nullptr, 1, nullptr);
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
      // Cap-touch is unreliable on this cube, and the new menu is opened
      // by holding the cube face-up for 2s (see loop() face-up trigger).
      // LongPress is now a no-op apart from logging — kept around so the
      // touch event still gets recorded in diag traces.
      dash::log::info("Main", "long-press (no-op)");
      dash::portal().recordDiagEvent("TouchLong");
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
  // Poll tilt-based menu nav. Bypasses the IMU Tilt event pipeline so
  // deep tilts (which cross the IMU's face hysteresis and would
  // otherwise cancel the Tilt state machine) still drive the menu.
  menu::pollTilt();

  // Face-up trigger: cube screen-up for >= kOpenHoldMs from Idle/Drowsy
  // opens the menu; cube not-screen-up for >= kCloseHoldMs while open
  // closes it.
  {
    static uint32_t faceUpSinceMs = 0;
    static uint32_t faceNotUpSinceMs = 0;
    const dash::Face curFace = dash::imu().currentFace();
    const dash::DeviceState st = dash::stateMachine().state();
    const bool canOpen =
        !menu::isOpen() &&
        dash::games().current() == dash::GameId::None &&
        (st == dash::DeviceState::Idle || st == dash::DeviceState::Drowsy);
    if (curFace == menu::kMenuFace) {
      if (faceUpSinceMs == 0) faceUpSinceMs = now;
      faceNotUpSinceMs = 0;
      if (canOpen && (now - faceUpSinceMs) >= menu::kOpenHoldMs) {
        menu::open();
        faceUpSinceMs = 0;
      }
    } else {
      faceUpSinceMs = 0;
      if (menu::isOpen() && dash::menutest::autoCloseEnabled()) {
        if (faceNotUpSinceMs == 0) faceNotUpSinceMs = now;
        if ((now - faceNotUpSinceMs) >= menu::kCloseHoldMs) {
          menu::close();
          faceNotUpSinceMs = 0;
        }
      } else {
        faceNotUpSinceMs = 0;
      }
    }
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
