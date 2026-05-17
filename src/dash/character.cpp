#include "dash/character.h"

#include "dash/log.h"
#include "dash/settings.h"

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
  // Eyes opening: asleep → sleepy (slits) → blink twice → look around
  // → settle. Skips the splash text — the eyes themselves are the brand.
  display().clearOverlay();
  display().setEyeState(EyeState::Asleep);
  delay(450);
  display().setEyeState(EyeState::Sleepy);
  delay(300);
  display().blink();
  delay(250);
  display().setEyeState(EyeState::Surprised);
  delay(280);
  display().setEyeState(EyeState::Searching);
  delay(420);
  display().blink();
  delay(180);
  display().setEyeState(restingEyeState());
  log::info(kTag, "boot animation done");
}

void Character::playWakeAnimation() {
  // Wake-from-deep-sleep is shorter than boot but uses the same opening
  // motion so the user sees the same "eyes coming open" feel.
  display().clearOverlay();
  display().setEyeState(EyeState::Asleep);
  delay(220);
  display().setEyeState(EyeState::Sleepy);
  delay(220);
  display().blink();
  delay(150);
  display().setEyeState(restingEyeState());
  log::info(kTag, "wake animation done");
}

void Character::playSleepAnimation() {
  // Going-to-sleep sequence — yawn (look up briefly), then down, then
  // eyes close. Holds the closed-eyes state for a moment so the user
  // feels the transition land before the deep-sleep cut.
  display().clearOverlay();
  display().setEyeState(EyeState::Surprised);
  delay(300);                                  // yawn — eyes open wide briefly
  display().setEyeState(EyeState::Sleepy);
  delay(500);
  display().blink();
  delay(220);
  display().setEyeState(EyeState::Sleepy);
  delay(300);
  display().setEyeState(EyeState::Asleep);
  delay(500);                                   // hold "asleep" before sleep entry
  log::info(kTag, "sleep animation done");
}

void Character::playSessionStartAnimation() {
  // Quick "alright, let's go" — wide eyes, glance, settle to focused.
  display().clearOverlay();
  display().setEyeState(EyeState::Surprised);
  delay(250);
  display().setEyeState(EyeState::Searching);
  delay(300);
  display().blink();
  delay(150);
  display().setEyeState(EyeState::Focused);
  log::info(kTag, "session start animation done");
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

    // Idle quirks: stochastic micro-behaviors during long stretches without
    // user input. Probabilities tuned per mood — Focused stays calm (Dash is
    // working with you, not at you), Tired blinks a lot, Playful is busy.
    if (reactUntilMs_ == 0 && (now - lastQuirkMs_) > 8000) {
      uint32_t interval = 8000 + (uint32_t)(esp_random() % 17000);
      if (now - lastQuirkMs_ > interval) {
        lastQuirkMs_ = now;
        uint32_t r = esp_random() % 100;
        if (mood_ == Mood::Neutral) {
          if (r < 35)      display().blink();
          else if (r < 50) react(EyeState::Searching, 800);
          else if (r < 60) react(EyeState::Surprised, 600);
          else if (r < 68) react(EyeState::Confused, 700);
          else if (r < 73) react(EyeState::SideEye, 1200);  // suspicious glance
          // else: stay idle
        } else if (mood_ == Mood::Focused) {
          // Mostly blinks during focus; very rare side-eye to remind you Dash is here.
          if (r < 70)       display().blink();
          else if (r < 80)  react(EyeState::SideEye, 1500);  // "are you still working?"
          else if (r < 85)  react(EyeState::Attentive, 1200);
        } else if (mood_ == Mood::Tired) {
          if (r < 85) display().blink();
        } else if (mood_ == Mood::Playful) {
          if (r < 35)      react(EyeState::Heart, 700);
          else if (r < 65) react(EyeState::Celebrating, 700);
          else             display().blink();
        } else if (mood_ == Mood::Listening) {
          if (r < 60)      display().blink();
          else             react(EyeState::Attentive, 900);
        } else if (mood_ == Mood::Excited) {
          if (r < 50) react(EyeState::Heart, 600);
          else        react(EyeState::Celebrating, 800);
        }
      }
    }
    vTaskDelayUntil(&lastWake, period);
  }
  task_ = nullptr;
  vTaskDelete(nullptr);
}

void Character::greetBasedOnTime() {
  uint32_t unix = settings().lastUnix();
  if (unix == 0) {
    // Phone hasn't synced yet — generic wave.
    react(EyeState::Happy, 1200);
    return;
  }
  // Convert to local-time hour using tz offset.
  int16_t tzMin = settings().tzOffsetMin();
  uint32_t local = unix + (int32_t)tzMin * 60;
  uint32_t hour = (local / 3600UL) % 24;
  if (hour >= 5 && hour < 11) {
    log::info(kTag, "morning greet");
    react(EyeState::Happy, 1500);
  } else if (hour >= 11 && hour < 17) {
    react(EyeState::Attentive, 1200);
  } else if (hour >= 17 && hour < 22) {
    react(EyeState::Focused, 1200);
  } else {
    // Late night / very early — gentle.
    react(EyeState::Sleepy, 1200);
  }
}

Character& character() {
  if (!g_singleton) g_singleton = new Character();
  return *g_singleton;
}

}  // namespace dash
