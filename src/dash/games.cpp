#include "dash/games.h"

#include "dash/audio.h"
#include "dash/character.h"
#include "dash/display.h"
#include "dash/imu.h"
#include "dash/log.h"
#include "dash/sounds.h"
#include "dash/state_machine.h"

namespace dash {

namespace {
constexpr const char* kTag = "Games";
Games* g_singleton = nullptr;

// Bop It actions — 3 prompts that reliably distinguish on this cube's
// IMU (per second calibration round):
//   0 = TAP    body-Z dominant (laz ~ +0.9g on a clean tap)
//   1 = FLICK  -Y dominant (lay ~ -0.4g on any sideways jerk)
//   2 = SHAKE  sustained +Y motion at high magnitude (linMag ~ 3g)
//
// We previously had left/right as separate prompts but the user's
// physical flick-left and flick-right both registered as -Y dominant
// — the IMU was capturing the deceleration phase which has the same
// sign regardless of initial direction. Collapsed to one "FLICK"
// prompt to keep the game reliable.
constexpr uint8_t kActionCount = 3;

}  // namespace

Games::Games()
    : task_(nullptr), current_(GameId::None), lastScore_(0),
      actionMs_(0), actionConsumed_(true), expectedAction_(0) {}

void Games::begin() {
  // Event-level matching catches the clean cases:
  //   TAP prompt   ← Tap event
  //   SHAKE prompt ← Shake event
  // FLICK prompts are matched by the per-frame raw-IMU polling inside
  // runBopIt() — the firmware doesn't fire a "Flick" event anymore.
  imu().onEvent([this](const ImuEvent& e) {
    if (current_ == GameId::None) return;
    // Bop It FLICK prompts are matched by the per-sample raw-IMU polling
    // in runBopIt() (looser thresholds, direction-aware) since flick
    // discrimination at the firmware level was unreliable on this cube.
    // Here we only catch the clean cases at the event layer.
    if (e.type == ImuEventType::Tap && expectedAction_ == 0)         actionConsumed_ = true;
    else if (e.type == ImuEventType::Shake && expectedAction_ == 2)  actionConsumed_ = true;
  });
}

void Games::startGame(GameId id) {
  if (current_ != GameId::None) return;
  current_ = id;
  stateMachine().transitionTo(DeviceState::InGame);
  character().setMood(Mood::Playful);
  audio().play(sounds::kGameStart);
  xTaskCreatePinnedToCore(&Games::taskTrampoline, "game", 4096, this, 1, &task_, 1);
}

void Games::stopGame() {
  current_ = GameId::None;
}

void Games::taskTrampoline(void* arg) { static_cast<Games*>(arg)->loop(); }

void Games::loop() {
  if (current_ == GameId::Reaction) runReaction();
  else if (current_ == GameId::BopIt) runBopIt();
  // Return to Idle. Clear any in-progress tap-chain so a tap landed
  // during the score-display screen doesn't get classified as the
  // first tap of a double-tap once we're back in Idle (which would
  // accidentally start a session).
  display().clearOverlay();
  imu().resetTapState();
  character().setMood(Mood::Neutral);
  stateMachine().transitionTo(DeviceState::Idle);
  current_ = GameId::None;
  task_ = nullptr;
  vTaskDelete(nullptr);
}

void Games::runReaction() {
  log::info(kTag, "Reaction time game");
  uint32_t totalScore = 0;
  const uint8_t kRounds = 5;
  for (uint8_t round = 0; round < kRounds && current_ != GameId::None; round++) {
    // "Get ready" prompt while we wait a random interval.
    char buf[24];
    snprintf(buf, sizeof(buf), "READY %u/%u", round + 1, kRounds);
    display().showBig(buf);
    uint32_t wait = 1500 + (esp_random() % 3000);
    delay(wait);
    if (current_ == GameId::None) return;

    // GO! — full-screen inverted prompt. Visually unmissable.
    expectedAction_ = 0;
    actionConsumed_ = false;
    actionMs_ = millis();
    display().showInverted("GO!");

    uint32_t deadline = millis() + 3000;
    while (!actionConsumed_ && millis() < deadline) delay(5);
    uint32_t reactionMs = millis() - actionMs_;
    if (actionConsumed_) {
      totalScore += (3000 - reactionMs);
      audio().play(sounds::kGameCorrect);
      char rt[24];
      snprintf(rt, sizeof(rt), "%lu ms", (unsigned long)reactionMs);
      display().showBig(rt);
    } else {
      audio().play(sounds::kGameWrong);
      display().showBig("MISS");
    }
    delay(900);
  }
  lastScore_ = totalScore;
  log::info(kTag, "Reaction score=%u", (unsigned)totalScore);
  audio().play(sounds::kEncouragement);
  // Final score on its own screen.
  char score[24];
  snprintf(score, sizeof(score), "%u", (unsigned)totalScore);
  display().showText("Reaction", score);
  delay(2500);
  display().clearOverlay();
}

void Games::runBopIt() {
  log::info(kTag, "Bop It");
  uint32_t score = 0;
  uint32_t window = 1800;
  while (current_ != GameId::None && score < 20) {
    uint8_t action = (uint8_t)(esp_random() % kActionCount);
    expectedAction_ = action;
    actionConsumed_ = false;
    actionMs_ = millis();
    // Each prompt is a full-screen visual cue. Tap uses big text "TAP!";
    // the four directional flicks use a large drawn arrow.
    switch (action) {
      case 0: display().showBig("TAP!");    break;
      case 1: display().showBig("FLICK!");  break;
      case 2: display().showBig("SHAKE!");  break;
    }
    audio().play(sounds::kMenuBlip);

    // Per-frame raw IMU polling with TWO outcomes:
    //   - Correct motion → action consumed (pass)
    //   - Clearly wrong motion → wrongMotion (miss, ends the game)
    //   - No motion before deadline → timeout (miss)
    //
    // Thresholds are tuned to require a deliberate motion (not just
    // ambient hand-shake) AND to distinguish direction so continuous
    // shaking can't cheat TAP / ← / → prompts.
    constexpr float kMoveThreshold     = 0.40f;  // |linear accel| g — clear this to consider
    constexpr float kDirComponentMin   = 0.30f;  // axis-of-interest g — match expected direction
    constexpr float kWrongComponentMin = 0.50f;  // axis-of-OTHER g — miss-out-loud threshold
    constexpr float kShakeIntentMin    = 1.2f;   // |linMag| for SHAKE prompt
    constexpr uint32_t kSettleMs       = 180;    // grace period after prompt change

    uint32_t promptShownMs = millis();
    uint32_t deadline = promptShownMs + window;
    bool wrongMotion = false;
    while (!actionConsumed_ && !wrongMotion && millis() < deadline) {
      if (millis() - promptShownMs < kSettleMs) { delay(6); continue; }

      auto s = imu().latest();
      float linMag = sqrtf(s.ax * s.ax + s.ay * s.ay + s.az * s.az);
      if (linMag < kMoveThreshold) { delay(6); continue; }

      // Component checks. Per calibration on this cube:
      //   TAP   → +laz (body-Z up) is the dominant axis (~+0.9g).
      //   FLICK → -lay (body-Y negative) regardless of physical direction.
      //   SHAKE → linMag stays very high (~3g) with sustained motion.
      bool zCorrect    = s.az >  kDirComponentMin;
      bool flickCorrect= s.ay < -kDirComponentMin;
      bool shakeStrong = linMag > kShakeIntentMin;
      // "Strongly wrong" — clear motion on a non-expected axis.
      bool zStrong     = s.az >  kWrongComponentMin;
      bool flickStrong = s.ay < -kWrongComponentMin;

      switch (action) {
        case 0:  // TAP
          if (zCorrect)              actionConsumed_ = true;
          else if (flickStrong)      wrongMotion = true;
          break;
        case 1:  // FLICK
          if (flickCorrect)          actionConsumed_ = true;
          else if (zStrong)          wrongMotion = true;
          break;
        case 2:  // SHAKE — any strong motion qualifies; no wrong-direction.
          if (shakeStrong)           actionConsumed_ = true;
          break;
      }
      delay(6);
    }
    if (wrongMotion) {
      // Treat wrong-direction motion the same as a miss — same path below.
      actionConsumed_ = false;
    }
    if (!actionConsumed_) {
      audio().play(sounds::kGameWrong);
      display().showInverted("MISS");
      delay(900);
      break;
    }
    score++;
    audio().play(sounds::kGameCorrect);
    // Brief "NICE" between prompts so the player feels the streak.
    display().showInverted("NICE");
    delay(220);
    if (window > 600) window -= 60;
  }
  lastScore_ = score;
  log::info(kTag, "Bop It score=%u", (unsigned)score);
  audio().play(sounds::kEncouragement);
  char str[24];
  snprintf(str, sizeof(str), "%u", (unsigned)score);
  display().showText("Bop It", str);
  delay(2500);
  display().clearOverlay();
}

Games& games() {
  if (!g_singleton) g_singleton = new Games();
  return *g_singleton;
}

}  // namespace dash
