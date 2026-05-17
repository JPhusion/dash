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

  // Faster, quieter wake animation for deep-sleep wake-up. Doesn't show the
  // splash; just blinks awake and settles.
  void playWakeAnimation();

  // Deliberate "going to sleep" animation — yawn, look down, eyes close,
  // hold. Used before deep sleep entry so the user sees Dash settle.
  void playSleepAnimation();

  // Session-start celebration: brief look-around then settle into focused.
  void playSessionStartAnimation();

  // Time-aware greeting (good morning / hello / good evening) — call after a
  // session starts or after a deep-sleep wake. Falls through cleanly if the
  // wall clock hasn't been synced yet.
  void greetBasedOnTime();

  // Fire a brief reaction (overrides resting state for hold_ms then returns).
  void react(EyeState state, uint32_t hold_ms = 1200);

  // Spinning-eye "dizzy" animation — drives lookAt() in a small circle for
  // ~1.4s, then settles into the Dizzy eye state for another hold_ms,
  // then returns to the resting state. Spawned on its own task so the
  // caller doesn't block.
  void playDizzyAnimation(uint32_t hold_ms = 1500);

  // Aim the eyes in the direction of the current gravity vector. Reads
  // imu().latest() once, projects the gravity vector onto the screen
  // plane, calls display().lookAt(x, y). The projection assumes the
  // screen-forward (neutral) face — when the cube is in some other
  // resting orientation, this points the eyes toward where the floor
  // is from the cube's perspective. Pass returnMs > 0 to schedule a
  // return to center (lookAt 0,0) after that long.
  void lookAtGravity(uint32_t returnMs = 0);

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
