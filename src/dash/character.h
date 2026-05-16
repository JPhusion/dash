// character.h — Dash's personality.
//
// On top of the display/eye library, the Character module adds:
// - Boot animation (eye sequence + splash overlay)
// - Idle quirks: occasional look-around, blinks, micro-emotions
// - Mood biasing based on device state (session = focused; idle = neutral)
// - Reaction primitives: react(Surprised | Happy | etc.) holds a face for N
//   ms then returns to the resting eye state.
//
// Single FreeRTOS task on core 1 (low priority) handles quirks; reactions are
// triggered synchronously by other modules via react().

#ifndef DASH_CHARACTER_H
#define DASH_CHARACTER_H

#include <Arduino.h>

#include "dash/display.h"

namespace dash {

enum class Mood : uint8_t {
  Neutral,     // resting Idle state
  Focused,     // during sessions
  Excited,     // celebration / encouragement
  Tired,       // drowsy progression
  Listening,   // during onboarding / settings input
  Playful,     // during games
};

class Character {
 public:
  Character();

  void begin();
  void start();
  void stop();

  // Animated boot sequence: splash, blink, look around, settle.
  void playBootAnimation();

  // Time-aware greeting (good morning / hello / good evening) — call after a
  // session starts or after a deep-sleep wake. Falls through cleanly if the
  // wall clock hasn't been synced yet.
  void greetBasedOnTime();

  // Fire a brief reaction (overrides resting state for hold_ms then returns).
  void react(EyeState state, uint32_t hold_ms = 1200);

  // Set the underlying mood — modulates the resting eye state and idle quirks.
  void setMood(Mood m);
  Mood mood() const { return mood_; }

  // Resting eye state for the current mood.
  EyeState restingEyeState() const;

 private:
  static void taskTrampoline(void* arg);
  void loop();

  TaskHandle_t task_;
  volatile bool running_;
  volatile Mood mood_;
  volatile uint32_t reactUntilMs_;
  uint32_t lastQuirkMs_;
};

Character& character();

}  // namespace dash

#endif
