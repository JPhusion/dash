#include "dash/session.h"

#include "dash/audio.h"
#include "dash/character.h"
#include "dash/display.h"
#include "dash/idle_manager.h"
#include "dash/log.h"
#include "dash/settings.h"
#include "dash/sounds.h"
#include "dash/state_machine.h"
#include "dash/stats.h"
#include <esp_random.h>

namespace dash {

RTC_DATA_ATTR uint32_t rtcSessionTargetMin = 0;
RTC_DATA_ATTR uint32_t rtcSessionStartedUnix = 0;
RTC_DATA_ATTR uint8_t  rtcSessionDirty = 0;

namespace {
constexpr const char* kTag = "Session";
Session* g_singleton = nullptr;
}

Session::Session()
    : state_(SessionState::Idle),
      startedAtMs_(0), pausedAtMs_(0), accumulatedPauseMs_(0),
      targetMin_(0), distractions_(0),
      task_(nullptr) { label_[0] = '\0'; }

void Session::begin() {
  // Spin up a tick task pinned to core 0 (next to WiFi).
  xTaskCreatePinnedToCore(&Session::taskTrampoline, "session", 3072, this,
                          1, &task_, 0);
}

bool Session::start(uint16_t minutes, const char* label) {
  if (state_ == SessionState::Running || state_ == SessionState::Paused) {
    log::warn(kTag, "start rejected: already %d", (int)state_);
    return false;
  }
  if (minutes < 1) minutes = 1;
  if (minutes > 240) minutes = 240;
  targetMin_ = minutes;
  startedAtMs_ = millis();
  accumulatedPauseMs_ = 0;
  pausedAtMs_ = 0;
  distractions_ = 0;
  if (label && label[0]) {
    strncpy(label_, label, sizeof(label_) - 1);
    label_[sizeof(label_) - 1] = '\0';
  } else {
    label_[0] = '\0';
  }
  enterState(SessionState::Running);

  rtcSessionTargetMin = minutes;
  rtcSessionStartedUnix = 0;  // wall clock set when phone syncs time
  rtcSessionDirty = 1;

  log::info(kTag, "session start, target=%u min", (unsigned)minutes);
  character().setMood(Mood::Focused);
  stateMachine().transitionTo(DeviceState::InSession);
  idleManager().inhibitSleep(true);
  display().setEyeState(EyeState::Focused);
  // Time-aware microexpression overrides the focused face for a moment so
  // the user feels acknowledged before settling in.
  character().greetBasedOnTime();
  audio().play(sounds::kSessionStart);
  return true;
}

void Session::pause() {
  if (state_ != SessionState::Running) return;
  pausedAtMs_ = millis();
  enterState(SessionState::Paused);
  display().setEyeState(EyeState::Sleepy);
  log::info(kTag, "paused");
}

void Session::resume() {
  if (state_ != SessionState::Paused) return;
  accumulatedPauseMs_ += millis() - pausedAtMs_;
  pausedAtMs_ = 0;
  enterState(SessionState::Running);
  display().setEyeState(EyeState::Focused);
  log::info(kTag, "resumed");
}

void Session::stop(bool celebrate) {
  if (state_ == SessionState::Idle) return;
  log::info(kTag, "stop (celebrate=%d distractions=%u)", (int)celebrate, distractions_);

  // Record stats before tearing down state.
  uint32_t elapsedMs = (millis() - startedAtMs_) - accumulatedPauseMs_;
  SessionRecord r{};
  r.startedUnix    = settings().lastUnix();
  r.targetMin      = targetMin_;
  r.actualSec      = (uint16_t)(elapsedMs / 1000UL);
  r.distractions   = distractions_;
  r.completed      = celebrate ? 1 : 0;
  stats().append(r);

  enterState(SessionState::Idle);
  rtcSessionDirty = 0;
  character().setMood(Mood::Neutral);
  stateMachine().transitionTo(DeviceState::Idle);
  idleManager().inhibitSleep(false);
  // Reset the idle clock so dash doesn't immediately go drowsy if the user
  // happened to be still during the session — they just earned a moment of
  // celebration, not the sleepy face.
  idleManager().poke();
  if (celebrate) {
    // Milestone celebrations: total session count crosses round numbers gets
    // an extra-long heart-eye finale. The completed bit is what we just wrote
    // so this fires inside the same stop() call.
    auto summary = stats().summary();
    uint16_t n = summary.completedSessions;
    if (n == 1 || n == 10 || n == 25 || n == 50 || n == 100 || (n > 100 && n % 100 == 0)) {
      log::info(kTag, "milestone %u completed sessions!", n);
      character().react(EyeState::Heart, 4000);
      audio().play(sounds::kMilestone);
    } else {
      character().react(EyeState::Celebrating, 3000);
      audio().play(sounds::kSessionComplete);
    }
  } else {
    character().react(EyeState::Disappointed, 1500);
    audio().play(sounds::kSessionEnd);
  }
}

SessionSnapshot Session::snapshot() const {
  SessionSnapshot s{};
  s.active = (state_ == SessionState::Running || state_ == SessionState::Paused);
  s.state = state_;
  s.startedAtMs = startedAtMs_;
  s.pausedMs = accumulatedPauseMs_ +
               (state_ == SessionState::Paused ? (millis() - pausedAtMs_) : 0);
  s.targetMin = targetMin_;
  s.distractions = distractions_;
  memcpy(s.label, label_, sizeof(s.label));
  return s;
}

void Session::noteDistraction() {
  if (state_ != SessionState::Running) return;
  distractions_++;
  // Throttle the reaction sound so we don't carpet-bomb the user.
  static uint32_t lastReactMs = 0;
  uint32_t now = millis();
  if (now - lastReactMs < 8000) return;
  lastReactMs = now;
  log::info(kTag, "distraction (#%u)", distractions_);
  character().react(EyeState::Disappointed, 1500);
  audio().play(sounds::kDistraction);
}

void Session::enterState(SessionState s) { state_ = s; }

bool Session::hasRecoverableSession() const {
  return rtcSessionDirty == 1 && rtcSessionTargetMin > 0;
}

void Session::clearRecoverableSession() {
  rtcSessionDirty = 0;
  rtcSessionTargetMin = 0;
}

void Session::taskTrampoline(void* arg) { static_cast<Session*>(arg)->loop(); }

void Session::loop() {
  while (true) {
    tick();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void Session::tick() {
  if (state_ != SessionState::Running) return;
  uint32_t elapsedMs = (millis() - startedAtMs_) - accumulatedPauseMs_;
  uint32_t totalMs = (uint32_t)targetMin_ * 60UL * 1000UL;
  if (elapsedMs >= totalMs) {
    stop(true);   // celebrate completion
    return;
  }

  // Mid-session encouragement at the halfway mark: brief happy reaction,
  // then back to focused. Fires once per session.
  static bool firedHalfway = false;
  if (!firedHalfway && elapsedMs >= totalMs / 2 && elapsedMs < totalMs / 2 + 2000) {
    log::info(kTag, "halfway! brief encouragement");
    character().react(EyeState::Happy, 1500);
    firedHalfway = true;
  }
  // Reset the halfway flag any time elapsed is back near zero — protects
  // against the static persisting across distinct sessions in the same boot.
  if (elapsedMs < 5000) firedHalfway = false;

  // Update progress overlay every 5 seconds to avoid display thrash.
  static uint32_t lastUiMs = 0;
  uint32_t now = millis();
  if (now - lastUiMs >= 5000) {
    lastUiMs = now;
    uint8_t pct = (uint8_t)((uint64_t)elapsedMs * 100 / totalMs);
    display().showProgress(pct);
  }
}

Session& session() {
  if (!g_singleton) g_singleton = new Session();
  return *g_singleton;
}

}  // namespace dash
