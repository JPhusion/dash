#include "dash/character.h"

#include "dash/log.h"

namespace dash {

namespace {
constexpr const char* kTag = "Char";
Character* g_singleton = nullptr;
}  // namespace

Character::Character()
    : task_(nullptr), running_(false), mood_(Mood::Neutral),
      reactUntilMs_(0), lastQuirkMs_(0) {}

void Character::begin() { lastQuirkMs_ = millis(); }

void Character::start() {
  if (running_) return;
  running_ = true;
  xTaskCreatePinnedToCore(&Character::taskTrampoline, "char", 3072, this, 1, &task_, 1);
}

void Character::stop() { running_ = false; }

void Character::playBootAnimation() {
  // Splash → blink → look around → settle.
  display().showBootSplash();
  delay(700);
  display().clearOverlay();
  display().setEyeState(EyeState::Surprised);
  delay(350);
  display().setEyeState(EyeState::Searching);
  delay(450);
  display().blink();
  delay(200);
  display().setEyeState(restingEyeState());
  log::info(kTag, "boot animation done");
}

void Character::react(EyeState state, uint32_t hold_ms) {
  display().setEyeState(state);
  reactUntilMs_ = millis() + hold_ms;
}

void Character::setMood(Mood m) {
  if (mood_ == m) return;
  mood_ = m;
  log::info(kTag, "mood -> %d", (int)m);
  if (millis() >= reactUntilMs_) {
    display().setEyeState(restingEyeState());
  }
}

EyeState Character::restingEyeState() const {
  switch (mood_) {
    case Mood::Neutral:   return EyeState::Idle;
    case Mood::Focused:   return EyeState::Focused;
    case Mood::Excited:   return EyeState::Happy;
    case Mood::Tired:     return EyeState::Sleepy;
    case Mood::Listening: return EyeState::Attentive;
    case Mood::Playful:   return EyeState::Surprised;
  }
  return EyeState::Idle;
}

void Character::taskTrampoline(void* arg) { static_cast<Character*>(arg)->loop(); }

void Character::loop() {
  const TickType_t period = pdMS_TO_TICKS(1000);
  TickType_t lastWake = xTaskGetTickCount();
  while (running_) {
    uint32_t now = millis();
    // If a reaction just finished, return to resting state.
    if (reactUntilMs_ != 0 && now >= reactUntilMs_) {
      display().setEyeState(restingEyeState());
      reactUntilMs_ = 0;
    }

    // Idle quirks: every 8-25 s while truly idle and not reacting.
    if (reactUntilMs_ == 0 && (now - lastQuirkMs_) > 8000) {
      uint32_t interval = 8000 + (uint32_t)(esp_random() % 17000);
      if (now - lastQuirkMs_ > interval) {
        lastQuirkMs_ = now;
        uint32_t r = esp_random() % 100;
        if (mood_ == Mood::Neutral) {
          if (r < 30)      display().blink();
          else if (r < 50) react(EyeState::Searching, 800);
          else if (r < 65) react(EyeState::Surprised, 600);
          else if (r < 75) react(EyeState::Confused, 700);
          // else: stay idle — not every quirk window has to fire
        } else if (mood_ == Mood::Focused) {
          // Focused sessions get fewer interruptions.
          if (r < 60) display().blink();
        } else if (mood_ == Mood::Tired) {
          if (r < 80) display().blink();
        } else if (mood_ == Mood::Playful) {
          if (r < 40) react(EyeState::Heart, 700);
          else if (r < 70) react(EyeState::Celebrating, 700);
          else display().blink();
        }
      }
    }
    vTaskDelayUntil(&lastWake, period);
  }
  task_ = nullptr;
  vTaskDelete(nullptr);
}

Character& character() {
  if (!g_singleton) g_singleton = new Character();
  return *g_singleton;
}

}  // namespace dash
