#include "dash/idle_manager.h"

#include "dash/display.h"
#include "dash/imu.h"
#include "dash/log.h"
#include "dash/power.h"
#include "dash/state_machine.h"
#include "dash/touch.h"

namespace dash {

namespace {
constexpr const char* kTag = "Idle";
IdleManager* g_singleton = nullptr;

EyeState eyeForDrowsyLevel(uint8_t l) {
  switch (l) {
    case 1: return EyeState::Drowsy1;
    case 2: return EyeState::Drowsy2;
    case 3: return EyeState::Drowsy3;
    case 4: return EyeState::Drowsy4;
    case 5: return EyeState::Drowsy5;
    default: return EyeState::Idle;
  }
}
}  // namespace

IdleManager::IdleManager()
    : task_(nullptr), running_(false),
      lastActivityMs_(0), sleepInhibited_(false),
      sleepTimeoutSec_(180), lastDrowsyLevel_(0) {}

void IdleManager::begin() {
  lastActivityMs_ = millis();
  // Any IMU event counts as activity.
  imu().onEvent([this](const ImuEvent& /*e*/) { poke(); });
  touch().onEvent([this](const TouchEvent& /*e*/) { poke(); });
  // Activity also resets drowsy state on state-machine transitions out of Idle/Drowsy.
  stateMachine().onTransition([this](DeviceState /*from*/, DeviceState to) {
    if (to != DeviceState::Idle && to != DeviceState::Drowsy &&
        to != DeviceState::Asleep) {
      inhibitSleep(true);
    } else {
      inhibitSleep(false);
    }
  });
}

void IdleManager::start() {
  if (running_) return;
  running_ = true;
  xTaskCreatePinnedToCore(&IdleManager::taskTrampoline, "idle", 3072, this, 1, &task_, 0);
}

void IdleManager::stop() { running_ = false; }

void IdleManager::poke() { lastActivityMs_ = millis(); }

void IdleManager::setSleepTimeoutSec(uint16_t sec) {
  if (sec < 60) sec = 60;
  if (sec > 600) sec = 600;
  sleepTimeoutSec_ = sec;
}

void IdleManager::taskTrampoline(void* arg) {
  static_cast<IdleManager*>(arg)->loop();
}

void IdleManager::loop() {
  const TickType_t period = pdMS_TO_TICKS(500);
  TickType_t lastWake = xTaskGetTickCount();
  while (running_) {
    if (!sleepInhibited_ && stateMachine().state() == DeviceState::Idle) {
      uint32_t elapsedMs = millis() - lastActivityMs_;
      uint32_t totalMs = (uint32_t)sleepTimeoutSec_ * 1000UL;
      // 60% -> drowsy1, 75% -> 2, 90% -> 3, 95% -> 4, 100% -> 5 (and sleep)
      uint8_t level = 0;
      if (elapsedMs >= totalMs * 60 / 100) level = 1;
      if (elapsedMs >= totalMs * 75 / 100) level = 2;
      if (elapsedMs >= totalMs * 90 / 100) level = 3;
      if (elapsedMs >= totalMs * 95 / 100) level = 4;
      if (elapsedMs >= totalMs) level = 5;

      if (level != lastDrowsyLevel_) {
        log::info(kTag, "drowsy level %u (elapsed %lus)", level,
                  (unsigned long)(elapsedMs / 1000));
        if (level == 0) {
          stateMachine().transitionTo(DeviceState::Idle);
          display().setEyeState(EyeState::Idle);
          power().setCpuProfile(CpuProfile::Performance);
        } else if (level >= 5) {
          stateMachine().transitionTo(DeviceState::Asleep);
          display().setEyeState(EyeState::Asleep);
          // enterDeepSleep is light-sleep under the hood now — returns
          // when the cube wakes (motion / touch). Restore state +
          // animation on wake.
          power().enterDeepSleep(0);
          display().setEyeState(EyeState::Idle);
          stateMachine().transitionTo(DeviceState::Idle);
          power().setCpuProfile(CpuProfile::Performance);
          lastActivityMs_ = millis();   // reset drowsy progression
          lastDrowsyLevel_ = 0;
        } else {
          stateMachine().transitionTo(DeviceState::Drowsy);
          display().setEyeState(eyeForDrowsyLevel(level));
          // Low-power CPU from drowsy 1 onward.
          power().setCpuProfile(CpuProfile::LowPower);
        }
        lastDrowsyLevel_ = level;
      }
    } else if (lastDrowsyLevel_ != 0) {
      // Activity returned — back to Idle at full speed.
      lastDrowsyLevel_ = 0;
      if (stateMachine().state() == DeviceState::Drowsy ||
          stateMachine().state() == DeviceState::Asleep) {
        stateMachine().transitionTo(DeviceState::Idle);
        display().setEyeState(EyeState::Idle);
      }
      power().setCpuProfile(CpuProfile::Performance);
    }
    vTaskDelayUntil(&lastWake, period);
  }
  task_ = nullptr;
  vTaskDelete(nullptr);
}

IdleManager& idleManager() {
  if (!g_singleton) g_singleton = new IdleManager();
  return *g_singleton;
}

}  // namespace dash
