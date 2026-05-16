// idle_manager.h — drowsy progression and deep-sleep gating.
//
// Tracks "time since last user activity" and walks Dash through Idle ->
// Drowsy1..5 -> Asleep based on configurable thresholds. Any IMU motion,
// touch, or state-machine transition out of Idle resets the timer.
//
// Single-task design: a low-priority FreeRTOS task ticks every 500 ms,
// applies the drowsy progression, and delegates eye-state updates to the
// Display + cpu profile to Power.

#ifndef DASH_IDLE_MANAGER_H
#define DASH_IDLE_MANAGER_H

#include <Arduino.h>

namespace dash {

class IdleManager {
 public:
  IdleManager();

  void begin();           // wires up to Imu + Touch + StateMachine.
  void start();
  void stop();

  // Reset the idle clock — called when any user activity is observed.
  void poke();

  // Total time-to-sleep, in seconds. Default 180 (3 min). Clamped 60..600.
  void setSleepTimeoutSec(uint16_t sec);
  uint16_t sleepTimeoutSec() const { return sleepTimeoutSec_; }

  // Disable sleep entirely (e.g. while a session is running).
  void inhibitSleep(bool on) { sleepInhibited_ = on; if (on) poke(); }

 private:
  static void taskTrampoline(void* arg);
  void loop();

  TaskHandle_t task_;
  volatile bool running_;
  volatile uint32_t lastActivityMs_;
  volatile bool sleepInhibited_;
  uint16_t sleepTimeoutSec_;
  uint8_t lastDrowsyLevel_;     // 0=Idle, 1..5=Drowsy progression
};

IdleManager& idleManager();

}  // namespace dash

#endif
