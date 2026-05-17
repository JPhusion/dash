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

// Bop It actions:
//   0 = TAP   (tap on top of cube)
//   1 = ←    (flick cube left)
//   2 = →    (flick cube right)
//   3 = ↑    (flick cube up / away from user)
//   4 = ↓    (flick cube down / toward user)
constexpr uint8_t kActionCount = 5;

}  // namespace

Games::Games()
    : task_(nullptr), current_(GameId::None), lastScore_(0),
      actionMs_(0), actionConsumed_(true), expectedAction_(0) {}

void Games::begin() {
  imu().onEvent([this](const ImuEvent& e) {
    if (current_ == GameId::None) return;
    // Match the right kind of event for whichever prompt is currently
    // displayed. Tap is body-Z; flicks are X/Y with the direction in
    // newFace (Left/Right/Front/Back).
    if (e.type == ImuEventType::Tap && expectedAction_ == 0) {
      actionConsumed_ = true;
    } else if (e.type == ImuEventType::Flick) {
      if (expectedAction_ == 1 && e.newFace == Face::Left)  actionConsumed_ = true;
      if (expectedAction_ == 2 && e.newFace == Face::Right) actionConsumed_ = true;
      if (expectedAction_ == 3 && e.newFace == Face::Front) actionConsumed_ = true;  // "↑"
      if (expectedAction_ == 4 && e.newFace == Face::Back)  actionConsumed_ = true;  // "↓"
    }
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
  // Return to Idle.
  display().clearOverlay();
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
      case 0: display().showBig("TAP!");           break;
      case 1: display().showArrow(ArrowDir::Left); break;
      case 2: display().showArrow(ArrowDir::Right);break;
      case 3: display().showArrow(ArrowDir::Up);   break;
      case 4: display().showArrow(ArrowDir::Down); break;
    }
    audio().play(sounds::kMenuBlip);

    uint32_t deadline = millis() + window;
    while (!actionConsumed_ && millis() < deadline) delay(5);
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
