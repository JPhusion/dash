// state_machine.h — top-level Dash device state.
//
// Single owner of "what is Dash doing right now". All transitions funnel
// through transitionTo() which logs the change. Other modules subscribe via
// onTransition() to react to state changes (e.g. Display switches eye state,
// Power adjusts CPU frequency).
//
// This is a finite-state-machine but intentionally not over-engineered: states
// are an enum, transitions are validated only with log warnings, and any
// module can request a transition. The simplicity is the point — invariants
// are enforced by ownership (only Session calls InSession, only Sleep calls
// Asleep, etc.) rather than a transition table.

#ifndef DASH_STATE_MACHINE_H
#define DASH_STATE_MACHINE_H

#include <Arduino.h>
#include <functional>

namespace dash {

enum class DeviceState : uint8_t {
  Booting,
  Onboarding,
  Idle,
  Drowsy,             // mid-progression toward sleep
  Asleep,             // a marker state — actual deep sleep happens after this
  AwakeForSession,    // session UI on portal, not yet started
  InSession,
  InMenu,
  InGame,
  GroupSessionWaiting,
  GroupSessionActive,
  OtaChecking,
};

const char* deviceStateName(DeviceState s);

using TransitionListener = std::function<void(DeviceState from, DeviceState to)>;

class StateMachine {
 public:
  StateMachine();

  // Apply a state transition. Logs the change and notifies listeners. Always
  // succeeds (no transition table guard) — modules opt into validation.
  void transitionTo(DeviceState s);

  DeviceState state() const { return current_; }

  // Subscribe to transitions. Up to 8 listeners.
  void onTransition(TransitionListener l);

  // Tap event helpers — convenience wrappers used widely from main.
  bool isInteractive() const;            // true if user can affect state via gestures
  bool wantsPerformance() const;         // true if state needs 240 MHz CPU

 private:
  DeviceState current_;
  TransitionListener listeners_[8];
  uint8_t listenerCount_;
};

StateMachine& stateMachine();

}  // namespace dash

#endif
