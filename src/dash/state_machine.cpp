#include "dash/state_machine.h"

#include "dash/log.h"

namespace dash {

namespace {
constexpr const char* kTag = "State";
StateMachine* g_singleton = nullptr;

const char* kNames[] = {
  "Booting", "Onboarding", "Idle", "Drowsy", "Asleep",
  "AwakeForSession", "InSession", "InMenu", "InGame",
  "GroupSessionWaiting", "GroupSessionActive", "OtaChecking",
};
}  // namespace

const char* deviceStateName(DeviceState s) {
  uint8_t i = static_cast<uint8_t>(s);
  if (i >= sizeof(kNames) / sizeof(kNames[0])) return "?";
  return kNames[i];
}

StateMachine::StateMachine() : current_(DeviceState::Booting), listenerCount_(0) {}

void StateMachine::transitionTo(DeviceState s) {
  if (s == current_) return;
  DeviceState from = current_;
  current_ = s;
  log::info(kTag, "%s -> %s", deviceStateName(from), deviceStateName(s));
  for (uint8_t i = 0; i < listenerCount_; i++) listeners_[i](from, s);
}

void StateMachine::onTransition(TransitionListener l) {
  if (listenerCount_ >= 8) {
    log::warn(kTag, "listener slot full");
    return;
  }
  listeners_[listenerCount_++] = std::move(l);
}

bool StateMachine::isInteractive() const {
  switch (current_) {
    case DeviceState::Idle:
    case DeviceState::InSession:
    case DeviceState::InMenu:
    case DeviceState::InGame:
    case DeviceState::AwakeForSession:
    case DeviceState::GroupSessionWaiting:
    case DeviceState::GroupSessionActive:
      return true;
    default: return false;
  }
}

bool StateMachine::wantsPerformance() const {
  switch (current_) {
    case DeviceState::InSession:
    case DeviceState::InMenu:
    case DeviceState::InGame:
    case DeviceState::AwakeForSession:
    case DeviceState::GroupSessionActive:
    case DeviceState::OtaChecking:
      return true;
    default: return false;
  }
}

StateMachine& stateMachine() {
  if (!g_singleton) g_singleton = new StateMachine();
  return *g_singleton;
}

}  // namespace dash
