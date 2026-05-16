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

const char* actionName(uint8_t a) {
  switch (a) {
    case 0: return "TAP!";
    case 1: return "SHAKE!";
    case 2: return "UP!";
    case 3: return "DOWN!";
  }
  return "";
}
}  // namespace

Games::Games()
    : task_(nullptr), current_(GameId::None), lastScore_(0),
      actionMs_(0), actionConsumed_(true), expectedAction_(0) {}

void Games::begin() {
  imu().onEvent([this](const ImuEvent& e) {
    if (current_ == GameId::None) return;
    if (e.type == ImuEventType::Tap && expectedAction_ == 0)            actionConsumed_ = true;
    else if (e.type == ImuEventType::Shake && expectedAction_ == 1)     actionConsumed_ = true;
    else if (e.type == ImuEventType::OrientationChange) {
      if (expectedAction_ == 2 && e.newFace == Face::Up)   actionConsumed_ = true;
      if (expectedAction_ == 3 && e.newFace == Face::Down) actionConsumed_ = true;
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
    display().setEyeState(EyeState::Idle);
    uint32_t wait = 1500 + (esp_random() % 3000);
    delay(wait);
    if (current_ == GameId::None) return;

    expectedAction_ = 0;
    actionConsumed_ = false;
    actionMs_ = millis();
    display().setEyeState(EyeState::Surprised);

    uint32_t deadline = millis() + 3000;
    while (!actionConsumed_ && millis() < deadline) delay(5);
    uint32_t reactionMs = millis() - actionMs_;
    if (actionConsumed_) {
      totalScore += (3000 - reactionMs);
      audio().play(sounds::kGameCorrect);
      display().setEyeState(EyeState::Happy);
    } else {
      audio().play(sounds::kGameWrong);
      display().setEyeState(EyeState::Sad);
    }
    delay(800);
  }
  lastScore_ = totalScore;
  log::info(kTag, "Reaction score=%u", (unsigned)totalScore);
  audio().play(sounds::kEncouragement);
  display().showText("score", String(totalScore).c_str());
  delay(2500);
  display().clearOverlay();
}

void Games::runBopIt() {
  log::info(kTag, "Bop It");
  uint32_t score = 0;
  uint32_t window = 1800;
  while (current_ != GameId::None && score < 20) {
    uint8_t action = (uint8_t)(esp_random() % 4);
    expectedAction_ = action;
    actionConsumed_ = false;
    actionMs_ = millis();
    display().showText(actionName(action));
    audio().play(sounds::kMenuBlip);

    uint32_t deadline = millis() + window;
    while (!actionConsumed_ && millis() < deadline) delay(5);
    if (!actionConsumed_) {
      audio().play(sounds::kGameWrong);
      display().setEyeState(EyeState::Sad);
      display().clearOverlay();
      delay(800);
      break;
    }
    score++;
    audio().play(sounds::kGameCorrect);
    display().setEyeState(EyeState::Happy);
    delay(250);
    display().clearOverlay();
    // Speed up.
    if (window > 600) window -= 60;
  }
  lastScore_ = score;
  log::info(kTag, "Bop It score=%u", (unsigned)score);
  audio().play(sounds::kEncouragement);
  display().showText("rounds", String(score).c_str());
  delay(2500);
  display().clearOverlay();
}

Games& games() {
  if (!g_singleton) g_singleton = new Games();
  return *g_singleton;
}

}  // namespace dash
