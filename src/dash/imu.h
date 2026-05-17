// imu.h — MPU-6050 driver with software tap/double-tap/triple-tap, shake, and
// orientation (face up/down/etc) detection. Runs in a dedicated FreeRTOS task
// pinned to core 1 sampling at 100 Hz. Events fire via callbacks dispatched
// from a worker task — callbacks never run in ISR or sampling-task context, so
// you can do anything in them (Serial, mutex, file I/O).

#ifndef DASH_IMU_H
#define DASH_IMU_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <functional>

namespace dash {

enum class Face : uint8_t {
  Unknown = 0,
  Up,        // +Z gravity dominant
  Down,      // -Z gravity dominant
  Left,      // -X
  Right,     // +X
  Front,     // -Y (display facing user)
  Back,      // +Y
};

const char* faceToString(Face f);

struct ImuSample {
  float ax, ay, az;       // gravity-removed linear accel, in g
  float gx, gy, gz;       // gyro, deg/s
  float pitch, roll, yaw; // degrees
  float gravityX, gravityY, gravityZ;  // unit vector
  uint32_t millis_ts;
};

enum class ImuEventType : uint8_t {
  Tap,
  DoubleTap,
  TripleTap,
  Shake,
  OrientationChange,
  Stationary,            // device has been still long enough to recalibrate gyro
  Flick,                 // directional flick — direction in newFace
                         // (Left / Right / Front / Back). Tap = body-Z;
                         // flicks happen in the body X/Y plane.
};

struct ImuEvent {
  ImuEventType type;
  Face newFace;           // for OrientationChange
  Face oldFace;
  float magnitude;        // peak |linear accel| for Tap/Shake events
  uint32_t millis_ts;
};

using ImuListener = std::function<void(const ImuEvent&)>;

class Imu {
 public:
  Imu();

  // Initialise the MPU-6050. Returns false if the WHO_AM_I check fails.
  bool begin();

  // Start sampling + event dispatch tasks (pinned to core 1).
  void start();
  void stop();                              // for clean deep-sleep entry

  // Get the most-recent computed sample. Thread-safe (returns a copy).
  ImuSample latest() const;
  Face currentFace() const { return currentFace_; }

  // Subscribe to events. Multiple listeners supported; called from a worker
  // task, never from the sampling task.
  void onEvent(ImuListener l);

  // Tuning knobs. Defaults are reasonable for a small cube; tweak per device.
  void setTapThreshold(float g)        { tapThreshold_ = g; }
  float tapThreshold() const           { return tapThreshold_; }
  void setShakeThresholdVariance(float v) { shakeVariance_ = v; }
  void setDoubleTapWindowMs(uint32_t ms)  { doubleTapWindowMs_ = ms; }
  void setTripleTapWindowMs(uint32_t ms)  { tripleTapWindowMs_ = ms; }

  // Persist/restore gyro bias to NVS. Called automatically once the device
  // appears stationary for > 5 seconds.
  void loadBiasFromNvs();
  void saveBiasToNvs() const;

  // Test hook: inject a synthetic event into the dispatch queue, bypassing
  // the sampling loop. Used by the serial CLI / self-test runner so the
  // full event pipeline (listeners, double-tap state machine, etc.) can be
  // exercised without physical motion.
  void injectEvent(ImuEvent e);

 private:
  static void sampleTaskTrampoline(void* arg);
  static void eventTaskTrampoline(void* arg);
  void sampleLoop();
  void eventLoop();

  bool readMpu(uint8_t reg, uint8_t* buf, uint8_t len);
  void writeMpu(uint8_t reg, uint8_t val);
  bool readSensors(float& ax, float& ay, float& az, float& gx, float& gy, float& gz);
  void updateMadgwick(float gx, float gy, float gz, float ax, float ay, float az, float dt);
  void quaternionToGravity(float& gxOut, float& gyOut, float& gzOut) const;
  Face dominantFace(float gx, float gy, float gz) const;
  void emit(ImuEvent e);

  mutable SemaphoreHandle_t mutex_;
  QueueHandle_t eventQueue_;
  TaskHandle_t sampleTask_;
  TaskHandle_t eventTask_;
  volatile bool running_;

  // Madgwick state.
  float q0_, q1_, q2_, q3_;
  float biasGx_, biasGy_, biasGz_;
  bool biasValid_;
  unsigned long lastSampleUs_;

  // Latest sample (mutex-protected on writes; readers copy).
  ImuSample latest_;

  // Tap state machine.
  float tapThreshold_;          // g — peak |linear accel|
  uint32_t doubleTapWindowMs_;
  uint32_t tripleTapWindowMs_;
  uint32_t lastTapMs_;
  uint8_t tapCount_;
  uint32_t firstTapMs_;
  uint32_t tapCooldownUntilMs_;
  float lastLinMag_;            // previous-sample magnitude for quiet-before-spike test

  // Shake state.
  float shakeVariance_;
  float varianceWindow_[16];
  uint8_t varianceIndex_;
  uint32_t lastShakeMs_;

  // Orientation.
  Face currentFace_;
  Face candidateFace_;
  uint32_t candidateFaceSinceMs_;

  // Stationary detection for opportunistic gyro recal.
  uint32_t stationarySinceMs_;
  bool stationaryFired_;

  ImuListener listeners_[4];
  uint8_t listenerCount_;
};

Imu& imu();

}  // namespace dash

#endif
