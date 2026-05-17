#include "dash/imu.h"

#include <Preferences.h>
#include <Wire.h>
#include <math.h>

#include "dash/log.h"
#include "dash/pins.h"
#include "dash/touch.h"

namespace dash {

namespace {
constexpr const char* kTag = "Imu";

constexpr uint8_t kMpuAddr      = 0x68;
constexpr uint8_t kRegPwrMgmt1  = 0x6B;
constexpr uint8_t kRegPwrMgmt2  = 0x6C;
constexpr uint8_t kRegWhoAmI    = 0x75;
constexpr uint8_t kRegAccelXout = 0x3B;
constexpr uint8_t kRegSignalPathReset = 0x68;
constexpr uint8_t kRegAccelConfig = 0x1C;
constexpr uint8_t kRegMotDetectCtrl = 0x69;
constexpr uint8_t kRegMotThr    = 0x1F;
constexpr uint8_t kRegMotDur    = 0x20;
constexpr uint8_t kRegIntPinCfg = 0x37;
constexpr uint8_t kRegIntEnable = 0x38;

constexpr float kAccScale  = 16384.0f;   // ±2g
constexpr float kGyroScale = 131.0f;     // ±250 deg/s
constexpr float kMadgwickBeta = 0.2f;
constexpr float kDeg2Rad = 0.017453292519943295f;
constexpr float kRad2Deg = 57.29577951308232f;

constexpr TickType_t kSamplePeriod = pdMS_TO_TICKS(10);   // 100 Hz
constexpr float kFaceGravityThreshold = 0.7f;             // unit-gravity
constexpr uint32_t kFaceHoldMs = 250;                      // hysteresis on face change
constexpr float kStationaryGyroThresh = 1.5f;             // deg/s
constexpr float kStationaryAccelThresh = 0.05f;           // g (linear accel)
constexpr uint32_t kStationaryHoldMs = 5000;
// Tap timing tuned to feel mouse-like:
// - Refractory 25ms: still rejects post-impact ringing but doesn't gate
//   a deliberate fast 2nd tap.
// - Commit-after-quiet 220ms: how long after the LAST tap we wait before
//   declaring the chain final. A 2-tap chain → DoubleTap at +220ms. A
//   3rd tap before that resets the timer and we commit as TripleTap
//   at +220ms after the 3rd.
constexpr uint32_t kTapRefractoryMs     = 25;
constexpr uint32_t kTapChainCommitMs    = 180;   // tighter: 220 → 180
constexpr uint32_t kShakeRefractoryMs   = 1500;

Imu* g_singleton = nullptr;

const char* kFaceNames[] = {
  "Unknown", "Up", "Down", "Left", "Right", "Front", "Back",
};
}  // namespace

const char* faceToString(Face f) {
  return kFaceNames[static_cast<uint8_t>(f) % (sizeof(kFaceNames) / sizeof(kFaceNames[0]))];
}

Imu::Imu()
    : mutex_(xSemaphoreCreateMutex()),
      eventQueue_(xQueueCreate(8, sizeof(ImuEvent))),
      sampleTask_(nullptr),
      eventTask_(nullptr),
      running_(false),
      q0_(1.0f), q1_(0.0f), q2_(0.0f), q3_(0.0f),
      biasGx_(0.0f), biasGy_(0.0f), biasGz_(0.0f),
      biasValid_(false),
      lastSampleUs_(0),
      latest_{},
      tapThreshold_(1.5f),
      doubleTapWindowMs_(300),
      tripleTapWindowMs_(550),
      lastTapMs_(0), tapCount_(0), firstTapMs_(0), tapCooldownUntilMs_(0),
      lastLinMag_(0.0f),
      shakeVariance_(0.6f),
      varianceIndex_(0),
      lastShakeMs_(0),
      currentFace_(Face::Unknown),
      candidateFace_(Face::Unknown),
      candidateFaceSinceMs_(0),
      spinAccumDeg_(0.0f),
      spinLastSampleMs_(0),
      lastSpinFiredMs_(0),
      tiltSnapshotValid_(false),
      tiltRefGx_(0), tiltRefGy_(0), tiltRefGz_(0),
      tiltInProgress_(false),
      tiltStartMs_(0),
      lastTiltFiredMs_(0),
      tiltPeakDevX_(0), tiltPeakDevY_(0), tiltPeakDevZ_(0),
      tiltPeakMagnitude_(0),
      stationarySinceMs_(0),
      stationaryFired_(false),
      listenerCount_(0) {
  for (auto& v : varianceWindow_) v = 0.0f;
  // Tap threshold defaults to 0.5g of *linear* accel (gravity-subtracted).
  // 1.5g (the original) required a firm bang; 0.5g registers a normal
  // finger tap on a ~50g cube. setTapThreshold() can re-tune at runtime.
  tapThreshold_ = 0.5f;
}

bool Imu::readMpu(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(kMpuAddr);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;
  int n = Wire.requestFrom((uint8_t)kMpuAddr, len, (uint8_t)true);
  if (n != (int)len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

void Imu::writeMpu(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(kMpuAddr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

bool Imu::begin() {
  // Wire is already brought up by Display::begin() via u8g2. We only call
  // Wire.begin() ourselves if Display didn't run first (e.g., a future code
  // path that uses the IMU headless). The Arduino Wire library warns when
  // begin() is called twice, so check before calling.
  if (Wire.getClock() == 0) {
    Wire.begin(pins::I2C_SDA, pins::I2C_SCL, pins::I2C_FREQ_HZ);
  }
  uint8_t who = 0;
  if (!readMpu(kRegWhoAmI, &who, 1)) {
    log::error(kTag, "WHO_AM_I read failed");
    return false;
  }
  log::info(kTag, "WHO_AM_I=0x%02X at 0x%02X", who, kMpuAddr);
  if (who != 0x68 && who != 0x98) {
    log::error(kTag, "unexpected WHO_AM_I value");
    return false;
  }
  writeMpu(kRegPwrMgmt1, 0x00);   // wake
  delay(50);
  loadBiasFromNvs();
  lastSampleUs_ = micros();
  return true;
}

void Imu::loadBiasFromNvs() {
  // Preferences::begin() with readonly=true logs an [E] when the namespace
  // doesn't exist yet (first boot). We suppress by checking, then opening
  // read-write to create the namespace silently.
  Preferences p;
  if (!p.begin("dash.imu", true)) {
    // Create empty namespace so subsequent reads are quiet.
    if (p.begin("dash.imu", false)) p.end();
    log::debug(kTag, "no NVS bias yet");
    return;
  }
  if (p.isKey("bx")) {
    biasGx_ = p.getFloat("bx", 0.0f);
    biasGy_ = p.getFloat("by", 0.0f);
    biasGz_ = p.getFloat("bz", 0.0f);
    biasValid_ = true;
    log::info(kTag, "loaded gyro bias from NVS: %.2f %.2f %.2f", biasGx_, biasGy_, biasGz_);
  }
  p.end();
}

void Imu::saveBiasToNvs() const {
  Preferences p;
  if (!p.begin("dash.imu", false)) return;
  p.putFloat("bx", biasGx_);
  p.putFloat("by", biasGy_);
  p.putFloat("bz", biasGz_);
  p.end();
}

void Imu::start() {
  if (running_) return;
  running_ = true;
  xTaskCreatePinnedToCore(&Imu::sampleTaskTrampoline, "imu-sample", 4096, this,
                          3 /*prio*/, &sampleTask_, 1 /*core*/);
  xTaskCreatePinnedToCore(&Imu::eventTaskTrampoline, "imu-event", 4096, this,
                          2, &eventTask_, 1);
}

void Imu::stop() {
  running_ = false;
}

ImuSample Imu::latest() const {
  ImuSample copy;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  copy = latest_;
  xSemaphoreGive(mutex_);
  return copy;
}

void Imu::onEvent(ImuListener l) {
  if (listenerCount_ >= (sizeof(listeners_) / sizeof(listeners_[0]))) {
    log::warn(kTag, "listener slot full, ignored");
    return;
  }
  listeners_[listenerCount_++] = std::move(l);
}

void Imu::updateMadgwick(float gx, float gy, float gz,
                         float ax, float ay, float az, float dt) {
  float recipNorm = sqrtf(ax * ax + ay * ay + az * az);
  if (recipNorm == 0.0f) return;
  recipNorm = 1.0f / recipNorm;
  ax *= recipNorm; ay *= recipNorm; az *= recipNorm;

  float _2q0 = 2.0f * q0_;
  float _2q1 = 2.0f * q1_;
  float _2q2 = 2.0f * q2_;
  float _2q3 = 2.0f * q3_;
  float _4q0 = 4.0f * q0_;
  float _4q1 = 4.0f * q1_;
  float _4q2 = 4.0f * q2_;
  float _8q1 = 8.0f * q1_;
  float _8q2 = 8.0f * q2_;
  float q0q0 = q0_ * q0_;
  float q1q1 = q1_ * q1_;
  float q2q2 = q2_ * q2_;
  float q3q3 = q3_ * q3_;

  float s0 = _4q0 * q2q2 + _2q2 * ax + _4q0 * q1q1 - _2q1 * ay;
  float s1 = _4q1 * q3q3 - _2q3 * ax + 4.0f * q0q0 * q1_ - _2q0 * ay - _4q1 + _8q1 * q1q1 + _8q1 * q2q2 + _4q1 * az;
  float s2 = 4.0f * q0q0 * q2_ + _2q0 * ax + _4q2 * q3q3 - _2q3 * ay - _4q2 + _8q2 * q1q1 + _8q2 * q2q2 + _4q2 * az;
  float s3 = 4.0f * q1q1 * q3_ - _2q1 * ax + 4.0f * q2q2 * q3_ - _2q2 * ay;

  float norm_s = sqrtf(s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3);
  if (norm_s == 0.0f) return;
  s0 /= norm_s; s1 /= norm_s; s2 /= norm_s; s3 /= norm_s;

  float qDot0 = 0.5f * (-q1_ * gx - q2_ * gy - q3_ * gz) - kMadgwickBeta * s0;
  float qDot1 = 0.5f * ( q0_ * gx + q2_ * gz - q3_ * gy) - kMadgwickBeta * s1;
  float qDot2 = 0.5f * ( q0_ * gy - q1_ * gz + q3_ * gx) - kMadgwickBeta * s2;
  float qDot3 = 0.5f * ( q0_ * gz + q1_ * gy - q2_ * gx) - kMadgwickBeta * s3;

  q0_ += qDot0 * dt;
  q1_ += qDot1 * dt;
  q2_ += qDot2 * dt;
  q3_ += qDot3 * dt;

  float recip = 1.0f / sqrtf(q0_*q0_ + q1_*q1_ + q2_*q2_ + q3_*q3_);
  q0_ *= recip; q1_ *= recip; q2_ *= recip; q3_ *= recip;
}

void Imu::quaternionToGravity(float& gxOut, float& gyOut, float& gzOut) const {
  gxOut = 2.0f * (q1_ * q3_ - q0_ * q2_);
  gyOut = 2.0f * (q0_ * q1_ + q2_ * q3_);
  gzOut = q0_ * q0_ - q1_ * q1_ - q2_ * q2_ + q3_ * q3_;
}

Face Imu::dominantFace(float gx, float gy, float gz) const {
  if (gz >  kFaceGravityThreshold) return Face::Up;
  if (gz < -kFaceGravityThreshold) return Face::Down;
  if (gx >  kFaceGravityThreshold) return Face::Right;
  if (gx < -kFaceGravityThreshold) return Face::Left;
  if (gy >  kFaceGravityThreshold) return Face::Back;
  if (gy < -kFaceGravityThreshold) return Face::Front;
  return Face::Unknown;
}

bool Imu::readSensors(float& ax, float& ay, float& az,
                      float& gx, float& gy, float& gz) {
  uint8_t buf[14];
  if (!readMpu(kRegAccelXout, buf, 14)) return false;
  int16_t accX = (buf[0] << 8) | buf[1];
  int16_t accY = (buf[2] << 8) | buf[3];
  int16_t accZ = (buf[4] << 8) | buf[5];
  int16_t gyroX_raw = (buf[8] << 8) | buf[9];
  int16_t gyroY_raw = (buf[10] << 8) | buf[11];
  int16_t gyroZ_raw = (buf[12] << 8) | buf[13];
  ax = accX / kAccScale;
  ay = accY / kAccScale;
  az = accZ / kAccScale;
  gx = ((gyroX_raw - biasGx_) / kGyroScale) * kDeg2Rad;
  gy = ((gyroY_raw - biasGy_) / kGyroScale) * kDeg2Rad;
  gz = ((gyroZ_raw - biasGz_) / kGyroScale) * kDeg2Rad;
  return true;
}

void Imu::injectEvent(ImuEvent e) {
  emit(e);
}

void Imu::resetTapState() {
  tapCount_ = 0;
  lastTapMs_ = 0;
  firstTapMs_ = 0;
  tapCooldownUntilMs_ = millis() + 250;   // small post-reset cooldown so
                                          // a tap that arrived JUST before
                                          // the reset doesn't immediately
                                          // re-arm the chain.
}

void Imu::enableWakeOnMotion(uint16_t thresholdMg) {
  // 1. Stop the sample loop so we own the I2C bus.
  bool wasRunning = running_;
  running_ = false;
  if (sampleTask_ != nullptr) {
    for (int i = 0; i < 50 && eTaskGetState(sampleTask_) != eDeleted; ++i) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  // Per MPU-6050 product spec § "Wake-on-Motion Interrupt Configuration":
  //   1. Reset signal paths.
  //   2. ACCEL_CONFIG = 0 (no DLPF, no HPF).
  //   3. INT_PIN_CFG: push-pull, active-high, latched until cleared.
  //   4. INT_ENABLE: MOT_EN = 1.
  //   5. Wait > 1 ms for accel to settle.
  //   6. ACCEL_CONFIG: DHPF = HOLD (0x07).
  //   7. MOT_DETECT_CTRL: ACCEL_ON_DELAY = 0, MOT_COUNT = 1.
  //   8. MOT_THR / MOT_DUR.
  //   9. PWR_MGMT_2: LP_WAKE_CTRL = 11 (40 Hz), all gyro standby.
  //  10. PWR_MGMT_1: CYCLE = 1, SLEEP = 0, TEMP_DIS = 1.
  writeMpu(kRegSignalPathReset, 0x07);   // gyro+accel+temp signal reset
  delay(2);
  writeMpu(kRegAccelConfig,   0x00);
  writeMpu(kRegIntPinCfg,     0xA0);     // INT_LEVEL=0 (active-high), latch, clear on any read
  writeMpu(kRegIntEnable,     0x40);     // MOT_EN
  delay(2);
  writeMpu(kRegAccelConfig,   0x07);     // DHPF = Hold
  writeMpu(kRegMotDetectCtrl, 0x15);     // accel power-on delay + mot count
  // Threshold register is in 2 mg units; clamp [1, 255].
  uint16_t thrReg = thresholdMg / 2;
  if (thrReg == 0) thrReg = 1;
  if (thrReg > 255) thrReg = 255;
  writeMpu(kRegMotThr, (uint8_t)thrReg);
  writeMpu(kRegMotDur, 1);                // 1 ms minimum
  writeMpu(kRegPwrMgmt2, 0x07);           // gyro X/Y/Z standby; accel on
  writeMpu(kRegPwrMgmt1, 0x28);           // CYCLE=1 + TEMP_DIS

  log::info(kTag, "WoM enabled, threshold ~%umg", (unsigned)(thrReg * 2));
  (void)wasRunning;
}

void Imu::disableWakeOnMotion() {
  // Bring the MPU-6050 back to its normal sampling configuration. Mirrors
  // the begin() init path so the sample loop restarts cleanly.
  writeMpu(kRegPwrMgmt1, 0x00);     // wake, no cycle
  delay(10);
  writeMpu(kRegPwrMgmt2, 0x00);     // all axes active
  writeMpu(kRegIntEnable, 0x00);    // disable motion interrupt
  writeMpu(kRegAccelConfig, 0x00);
  writeMpu(kRegMotDetectCtrl, 0x00);
  // Re-arm sampling task.
  running_ = true;
  xTaskCreatePinnedToCore(&Imu::sampleTaskTrampoline, "imu-sample", 4096, this,
                          3, &sampleTask_, 1);
  log::info(kTag, "WoM disabled, sample loop restarted");
}

void Imu::emit(ImuEvent e) {
  e.millis_ts = millis();
  // Non-blocking — drop oldest if full.
  if (xQueueSend(eventQueue_, &e, 0) != pdTRUE) {
    ImuEvent drop;
    xQueueReceive(eventQueue_, &drop, 0);
    xQueueSend(eventQueue_, &e, 0);
  }
}

void Imu::sampleTaskTrampoline(void* arg) {
  static_cast<Imu*>(arg)->sampleLoop();
}

void Imu::eventTaskTrampoline(void* arg) {
  static_cast<Imu*>(arg)->eventLoop();
}

void Imu::sampleLoop() {
  // Rolling gyro mean for opportunistic bias.
  float sumGx = 0, sumGy = 0, sumGz = 0;
  uint32_t calibrationSamples = 0;

  TickType_t lastWake = xTaskGetTickCount();
  while (running_) {
    float ax, ay, az, gx, gy, gz;
    if (readSensors(ax, ay, az, gx, gy, gz)) {
      unsigned long nowUs = micros();
      float dt = (nowUs - lastSampleUs_) / 1e6f;
      if (dt > 0.05f) dt = 0.05f;  // cap on first iteration
      lastSampleUs_ = nowUs;

      updateMadgwick(gx, gy, gz, ax, ay, az, dt);

      // Gravity-removed linear accel.
      float grx, gry, grz;
      quaternionToGravity(grx, gry, grz);
      float lax = ax - grx, lay = ay - gry, laz = az - grz;
      float linMag = sqrtf(lax * lax + lay * lay + laz * laz);

      // Euler angles for debug + Sleep module compatibility.
      float sinr_cosp = 2.0f * (q0_ * q1_ + q2_ * q3_);
      float cosr_cosp = 1.0f - 2.0f * (q1_ * q1_ + q2_ * q2_);
      float roll = atan2f(sinr_cosp, cosr_cosp) * kRad2Deg;
      float sinp = 2.0f * (q0_ * q2_ - q3_ * q1_);
      float pitch = (fabsf(sinp) >= 1.0f) ? copysignf(90.0f, sinp) * 1.0f
                                          : asinf(sinp) * kRad2Deg;
      float siny_cosp = 2.0f * (q0_ * q3_ + q1_ * q2_);
      float cosy_cosp = 1.0f - 2.0f * (q2_ * q2_ + q3_ * q3_);
      float yaw = atan2f(siny_cosp, cosy_cosp) * kRad2Deg;

      // Publish latest snapshot.
      ImuSample snap;
      snap.ax = lax; snap.ay = lay; snap.az = laz;
      snap.gx = gx * kRad2Deg; snap.gy = gy * kRad2Deg; snap.gz = gz * kRad2Deg;
      snap.pitch = pitch; snap.roll = roll; snap.yaw = yaw;
      snap.gravityX = grx; snap.gravityY = gry; snap.gravityZ = grz;
      snap.millis_ts = millis();
      // Block until the mutex is free — the readers (Imu::latest, the
      // calibration capture loop) hold it for nanoseconds at most, so the
      // sample task never stalls in practice. Was a non-blocking take +
      // unconditional give → mutex-not-held assert under contention.
      xSemaphoreTake(mutex_, portMAX_DELAY);
      latest_ = snap;
      xSemaphoreGive(mutex_);

      // ---- Shake detection (runs first so taps can be suppressed during
      // sustained motion / shaking). Running variance of linear-accel
      // magnitude over 16 samples (~160ms at 100Hz).
      uint32_t nowMs = millis();
      varianceWindow_[varianceIndex_ % 16] = linMag;
      varianceIndex_++;
      float runningVar = 0;
      if (varianceIndex_ >= 16) {
        float mean = 0;
        for (auto v : varianceWindow_) mean += v;
        mean /= 16.0f;
        for (auto v : varianceWindow_) runningVar += (v - mean) * (v - mean);
        runningVar /= 16.0f;
        if (runningVar > shakeVariance_ && nowMs - lastShakeMs_ > kShakeRefractoryMs) {
          lastShakeMs_ = nowMs;
          emit({ImuEventType::Shake, currentFace_, currentFace_, sqrtf(runningVar), 0});
        }
      }

      // ---- Tap detection. A real "tap on the head" has two properties
      // that distinguish it from a side-to-side shake:
      //
      //   1. Direction: the cap-touch pad lives on a fixed face of the
      //      cube. A tap on that face pushes the cube along ONE specific
      //      body axis (Z by IMU mounting convention). Side-to-side
      //      shaking puts the impulse in the X / Y plane. We measure
      //      alignment of the linear-accel vector with the Z axis:
      //      |laz| / |linMag| > 0.55 → along body-Z → tap on top/bottom.
      //
      //   2. Profile: a sharp spike preceded by a quiet sample
      //      (prevQuiet) rules out continuous motion. The FIRST tap of
      //      a chain enforces this strictly; follow-up taps within the
      //      triple-tap window get a relaxed rule so a hard first-tap's
      //      ring-down doesn't gate the second hit of a real double-tap.
      //
      // Cap-touch boost: while the user is touching the cap pad, drop
      // the threshold to 60% so soft taps on the pad-side register.
      float effThreshold = tapThreshold_;
      if (touch().isTouched()) effThreshold *= 0.6f;

      // (alongZ check removed — was rejecting valid taps on this cube
      // because the impulse dissipates across all 3 axes. Shake vs tap
      // is now decided by the post-spike variance check in stage 2.)

      const bool prevQuiet     = (lastLinMag_ < effThreshold * 0.5f);
      const bool inShakeWindow = (nowMs - lastShakeMs_) < 300;
      const bool inTapChain    = (tapCount_ > 0 &&
                                  nowMs - lastTapMs_ < tripleTapWindowMs_);

      // Gyro discriminator. A clean tap is a translation impulse — the
      // cube barely rotates. A flick, spin, or pickup-and-jiggle adds
      // rotational velocity. If gyro is high during the spike, reject
      // it. Threshold tuned per cube; 220 deg/s is well above hand
      // tremor / normal motion but below a deliberate flick (~400 deg/s).
      constexpr float kTapGyroMax = 220.0f;
      float gyroMagDeg = sqrtf(snap.gx*snap.gx + snap.gy*snap.gy + snap.gz*snap.gz);
      const bool tooMuchRotation = (gyroMagDeg > kTapGyroMax);

      // Magnitude window. A finger tap on a ~50 g cube tops out near 2 g
      // of linear accel. A vigorous shake easily clears 3 g and a bash
      // exceeds 4 g — those are NOT taps. Bound the candidate spike
      // between [effThreshold, kTapMaxMag] so out-of-range hits don't
      // sneak into the tap path.
      constexpr float kTapMaxMag = 4.0f;
      const bool tooHard = (linMag > kTapMaxMag);

      // Stage 1 — candidate spike. Magnitude in-window, low rotation,
      // and either inside an active tap chain or following a quiet
      // sample (so it's a transient — not the middle of sustained
      // motion).
      bool spikeFound = false;
      if (linMag > effThreshold &&
          !tooHard &&
          nowMs > tapCooldownUntilMs_ &&
          !inShakeWindow &&
          !tooMuchRotation &&
          (inTapChain || prevQuiet)) {
        spikeFound = true;
      }

      if (spikeFound) {
        if (inTapChain) {
          // Inside a chain we already know the user is tapping — commit
          // the follow-up tap right now to keep multi-tap snappy.
          tapCooldownUntilMs_ = nowMs + kTapRefractoryMs;
          tapCount_++;
          lastTapMs_ = nowMs;
          emit({ImuEventType::Tap, currentFace_, currentFace_, linMag, 0});
        } else {
          // First tap of a chain — DEFER emission. We'll commit (or drop)
          // ~80 ms later based on whether the variance climbs (= shake)
          // or stays low (= clean transient tap).
          tapPendingMs_   = nowMs;
          tapPendingMag_  = linMag;
          tapPendingFace_ = currentFace_;
          tapCooldownUntilMs_ = nowMs + kTapRefractoryMs;
        }
      }

      // Stage 2 — confirm or drop a pending tap after kTapConfirmMs.
      // A real tap is a brief impulse — by the confirm tick its
      // magnitude has decayed to baseline. Sustained motion (cube being
      // shaken up/down on Z) holds the magnitude near or above the
      // trigger threshold. Reject the candidate if EITHER:
      //   - running variance is too high (still shaking)   OR
      //   - linMag at confirm time is still > 0.6 * effThreshold (the
      //     impulse hasn't decayed — it's sustained motion, not a tap).
      constexpr uint32_t kTapConfirmMs   = 80;
      constexpr float kShakeConfirmRatio = 0.55f;
      constexpr float kTapDecayRatio     = 0.6f;   // of effThreshold
      if (tapPendingMs_ > 0 && (nowMs - tapPendingMs_) >= kTapConfirmMs) {
        const bool isShake     = (runningVar > shakeVariance_ * kShakeConfirmRatio);
        const bool stillMoving = (linMag    > effThreshold     * kTapDecayRatio);
        if (!isShake && !stillMoving) {
          if (tapCount_ == 0 || (nowMs - lastTapMs_) > tripleTapWindowMs_) {
            tapCount_ = 1;
            firstTapMs_ = tapPendingMs_;
          } else {
            tapCount_++;
          }
          lastTapMs_ = tapPendingMs_;
          emit({ImuEventType::Tap, tapPendingFace_, tapPendingFace_, tapPendingMag_, 0});
        }
        tapPendingMs_ = 0;
      }
      lastLinMag_ = linMag;

      // Face / orientation detection with hysteresis.
      Face f = dominantFace(grx, gry, grz);
      if (f != candidateFace_) {
        candidateFace_ = f;
        candidateFaceSinceMs_ = nowMs;
      } else if (f != currentFace_ && (nowMs - candidateFaceSinceMs_) > kFaceHoldMs) {
        Face old = currentFace_;
        currentFace_ = f;
        emit({ImuEventType::OrientationChange, f, old, 0.0f, 0});
        // Face change invalidates the tilt baseline + cancels any
        // tilt/spin in progress (those gestures are meaningful only when
        // the resting face is stable).
        tiltSnapshotValid_ = false;
        tiltInProgress_ = false;
        spinAccumDeg_ = 0.0f;
      }

      // ---- Spin detection. Integrate yaw rate (cube's body-Z gyro) over
      // time when the cube is flat on a face. A deliberate spin produces
      // sustained yaw rate; small drift or hand tremor stays below the
      // floor and decays toward zero. Fires once per spin with direction
      // encoded in newFace (Left/Right by signed accumulation — the
      // physical-to-user mapping is calibrated in main.cpp).
      constexpr float kSpinRateMin    = 80.0f;   // deg/s — engage threshold
      constexpr float kSpinFireDeg    = 90.0f;   // accumulated deg to fire
      constexpr uint32_t kSpinCooldownMs = 1200;
      constexpr float kSpinDecayPerSec = 4.0f;   // multiplier per second
      if (currentFace_ != Face::Unknown) {
        float yawDps = snap.gz;
        float dtSpin = (spinLastSampleMs_ == 0)
                       ? 0.0f
                       : (nowMs - spinLastSampleMs_) * 0.001f;
        spinLastSampleMs_ = nowMs;
        if (dtSpin > 0.0f && dtSpin < 0.1f) {
          if (fabsf(yawDps) > kSpinRateMin) {
            spinAccumDeg_ += yawDps * dtSpin;
          } else {
            // Decay so a tiny continuous drift doesn't accumulate, but a
            // brief plateau mid-spin doesn't reset us.
            float decay = expf(-kSpinDecayPerSec * dtSpin);
            spinAccumDeg_ *= decay;
          }
        }
        if (fabsf(spinAccumDeg_) > kSpinFireDeg &&
            (nowMs - lastSpinFiredMs_) > kSpinCooldownMs) {
          // Sign → direction. Map both choices to Face::Left/Right so
          // listeners have a stable encoding; the physical interpretation
          // is done at the consumer.
          Face dir = (spinAccumDeg_ > 0) ? Face::Left : Face::Right;
          emit({ImuEventType::Spin, dir, currentFace_,
                fabsf(spinAccumDeg_), 0});
          lastSpinFiredMs_ = nowMs;
          spinAccumDeg_ = 0.0f;
        }
      } else {
        spinAccumDeg_ = 0.0f;
        spinLastSampleMs_ = nowMs;
      }

      // ---- Tilt detection. Snapshot gravity when the cube has been
      // stable on a face for ~200 ms. Each sample, measure the deviation
      // of the live gravity vector from the snapshot. When |deviation|
      // crosses kTiltEngage, mark as tilting and track the peak. When
      // |deviation| drops back below kTiltDisengage AND the face hasn't
      // changed, fire Tilt with direction encoded by the peak deviation.
      constexpr float kTiltEngage      = 0.30f;   // unit-gravity (~17°)
      constexpr float kTiltDisengage   = 0.15f;
      constexpr uint32_t kTiltSettleMs = 250;
      constexpr uint32_t kTiltMaxMs    = 1500;    // tilt must return within
      constexpr uint32_t kTiltCooldownMs = 700;
      if (currentFace_ != Face::Unknown &&
          (nowMs - candidateFaceSinceMs_) > kTiltSettleMs) {
        if (!tiltSnapshotValid_) {
          // First settled sample on this face — capture the rest pose.
          tiltRefGx_ = grx; tiltRefGy_ = gry; tiltRefGz_ = grz;
          tiltSnapshotValid_ = true;
          tiltInProgress_ = false;
        }
        float devX = grx - tiltRefGx_;
        float devY = gry - tiltRefGy_;
        float devZ = grz - tiltRefGz_;
        float devMag = sqrtf(devX*devX + devY*devY + devZ*devZ);
        if (!tiltInProgress_) {
          if (devMag > kTiltEngage &&
              (nowMs - lastTiltFiredMs_) > kTiltCooldownMs) {
            tiltInProgress_ = true;
            tiltStartMs_ = nowMs;
            tiltPeakDevX_ = devX;
            tiltPeakDevY_ = devY;
            tiltPeakDevZ_ = devZ;
            tiltPeakMagnitude_ = devMag;
          }
        } else {
          // Track the peak deviation.
          if (devMag > tiltPeakMagnitude_) {
            tiltPeakDevX_ = devX;
            tiltPeakDevY_ = devY;
            tiltPeakDevZ_ = devZ;
            tiltPeakMagnitude_ = devMag;
          }
          const bool tooLong = (nowMs - tiltStartMs_) > kTiltMaxMs;
          if (devMag < kTiltDisengage) {
            // Returned to neutral — fire Tilt with the peak direction.
            // Direction is encoded in newFace using whichever of the two
            // non-face axes had the largest peak deviation, signed:
            //   +X → Face::Right    -X → Face::Left
            //   +Y → Face::Back     -Y → Face::Front
            //   +Z → Face::Up       -Z → Face::Down
            // The face-normal axis is masked out so deviations along the
            // resting axis don't bias direction selection.
            float refAx = fabsf(tiltRefGx_);
            float refAy = fabsf(tiltRefGy_);
            float refAz = fabsf(tiltRefGz_);
            float dXabs = fabsf(tiltPeakDevX_);
            float dYabs = fabsf(tiltPeakDevY_);
            float dZabs = fabsf(tiltPeakDevZ_);
            if (refAx >= refAy && refAx >= refAz) dXabs = 0.0f;
            else if (refAy >= refAz)              dYabs = 0.0f;
            else                                   dZabs = 0.0f;
            Face dir;
            if (dXabs >= dYabs && dXabs >= dZabs) {
              dir = (tiltPeakDevX_ > 0) ? Face::Right : Face::Left;
            } else if (dYabs >= dZabs) {
              dir = (tiltPeakDevY_ > 0) ? Face::Back : Face::Front;
            } else {
              dir = (tiltPeakDevZ_ > 0) ? Face::Up   : Face::Down;
            }
            emit({ImuEventType::Tilt, dir, currentFace_,
                  tiltPeakMagnitude_, 0});
            tiltInProgress_ = false;
            lastTiltFiredMs_ = nowMs;
          } else if (tooLong) {
            // Held too long without returning — abandon, don't fire.
            // The cube probably completed a face change, which the
            // OrientationChange path handles.
            tiltInProgress_ = false;
          }
        }
      } else {
        tiltSnapshotValid_ = false;
        tiltInProgress_ = false;
      }

      // Opportunistic stationary detect: gyro near zero AND linear accel small.
      // gyroMagDeg was already computed earlier (for the tap discriminator).
      if (gyroMagDeg < kStationaryGyroThresh && linMag < kStationaryAccelThresh) {
        if (stationarySinceMs_ == 0) stationarySinceMs_ = nowMs;
        sumGx += (gx / kDeg2Rad) * kGyroScale + biasGx_;  // raw counts (revert correction)
        sumGy += (gy / kDeg2Rad) * kGyroScale + biasGy_;
        sumGz += (gz / kDeg2Rad) * kGyroScale + biasGz_;
        calibrationSamples++;
        if (!stationaryFired_ && (nowMs - stationarySinceMs_) > kStationaryHoldMs && calibrationSamples > 200) {
          biasGx_ = sumGx / calibrationSamples;
          biasGy_ = sumGy / calibrationSamples;
          biasGz_ = sumGz / calibrationSamples;
          biasValid_ = true;
          saveBiasToNvs();
          stationaryFired_ = true;
          emit({ImuEventType::Stationary, currentFace_, currentFace_, 0.0f, 0});
        }
      } else {
        stationarySinceMs_ = 0;
        stationaryFired_ = false;
        calibrationSamples = 0;
        sumGx = sumGy = sumGz = 0;
      }
    }
    vTaskDelayUntil(&lastWake, kSamplePeriod);
  }
  sampleTask_ = nullptr;
  vTaskDelete(nullptr);
}

void Imu::eventLoop() {
  ImuEvent e;
  while (running_) {
    // Short poll so the commit check below runs ~50× per second — that's
    // what lets us fire DoubleTap 220 ms after the 2nd tap instead of
    // 600 ms.
    if (xQueueReceive(eventQueue_, &e, pdMS_TO_TICKS(15)) != pdTRUE) {
      // Idle tick — see if the tap chain is ready to commit. We commit
      // when no further taps have arrived for kTapChainCommitMs. This
      // mimics a mouse double-click: the moment we're confident no
      // 3rd / 4th tap is coming, fire the multi-tap event.
      uint32_t nowMs = millis();
      if (tapCount_ > 0 && (nowMs - lastTapMs_) > kTapChainCommitMs) {
        if (tapCount_ == 2) {
          ImuEvent de{ImuEventType::DoubleTap, currentFace_, currentFace_, 0, nowMs};
          for (uint8_t i = 0; i < listenerCount_; i++) listeners_[i](de);
        } else if (tapCount_ >= 3) {
          ImuEvent te{ImuEventType::TripleTap, currentFace_, currentFace_, 0, nowMs};
          for (uint8_t i = 0; i < listenerCount_; i++) listeners_[i](te);
        }
        // count == 1 → already fired single Tap when it happened; nothing
        // else to emit on commit.
        tapCount_ = 0;
      }
      continue;
    }
    for (uint8_t i = 0; i < listenerCount_; i++) listeners_[i](e);
  }
  eventTask_ = nullptr;
  vTaskDelete(nullptr);
}

Imu& imu() {
  if (!g_singleton) g_singleton = new Imu();
  return *g_singleton;
}

}  // namespace dash
