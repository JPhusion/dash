#include "dash/imu.h"

#include <Preferences.h>
#include <Wire.h>
#include <math.h>

#include "dash/log.h"
#include "dash/pins.h"

namespace dash {

namespace {
constexpr const char* kTag = "Imu";

constexpr uint8_t kMpuAddr      = 0x68;
constexpr uint8_t kRegPwrMgmt1  = 0x6B;
constexpr uint8_t kRegWhoAmI    = 0x75;
constexpr uint8_t kRegAccelXout = 0x3B;

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
constexpr uint32_t kTapRefractoryMs = 120;
constexpr uint32_t kShakeRefractoryMs = 1500;

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
      doubleTapWindowMs_(400),
      tripleTapWindowMs_(600),
      lastTapMs_(0), tapCount_(0), firstTapMs_(0), tapCooldownUntilMs_(0),
      lastLinMag_(0.0f),
      shakeVariance_(0.6f),
      varianceIndex_(0),
      lastShakeMs_(0),
      currentFace_(Face::Unknown),
      candidateFace_(Face::Unknown),
      candidateFaceSinceMs_(0),
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
      xSemaphoreTake(mutex_, 0);
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

      // ---- Tap detection. A real finger tap has a distinctive signature:
      // a brief quiet period followed by a sharp spike. Sustained motion
      // (shake / hand-held / sliding the cube) keeps linMag elevated and
      // therefore lacks the preceding quiet — those should NOT be taps.
      //
      // Rules:
      //   1. Previous sample (10ms ago) was below tapThreshold * 0.4 — proof
      //      we were actually quiet immediately before this spike.
      //   2. Variance over the last ~160ms is also low (variance < shake/3) —
      //      proof there isn't a shake-burst around this spike.
      //   3. Refractory window of kTapRefractoryMs after a fire.
      const bool prevQuiet      = (lastLinMag_ < tapThreshold_ * 0.4f);
      const bool varianceQuiet  = (runningVar < shakeVariance_ * 0.33f);
      const bool inShakeWindow  = (nowMs - lastShakeMs_) < 500;
      if (linMag > tapThreshold_ &&
          nowMs > tapCooldownUntilMs_ &&
          prevQuiet && varianceQuiet && !inShakeWindow) {
        tapCooldownUntilMs_ = nowMs + kTapRefractoryMs;
        if (tapCount_ == 0 || (nowMs - lastTapMs_) > tripleTapWindowMs_) {
          tapCount_ = 1;
          firstTapMs_ = nowMs;
        } else {
          tapCount_++;
        }
        lastTapMs_ = nowMs;
        emit({ImuEventType::Tap, currentFace_, currentFace_, linMag, 0});
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
      }

      // Opportunistic stationary detect: gyro near zero AND linear accel small.
      float gyroMagDeg = sqrtf(snap.gx*snap.gx + snap.gy*snap.gy + snap.gz*snap.gz);
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
    if (xQueueReceive(eventQueue_, &e, pdMS_TO_TICKS(50)) != pdTRUE) {
      // Window-based emit: if the tap sequence has expired, classify.
      uint32_t nowMs = millis();
      if (tapCount_ > 0 && (nowMs - lastTapMs_) > tripleTapWindowMs_) {
        if (tapCount_ == 2 && (nowMs - firstTapMs_) <= doubleTapWindowMs_ * 2) {
          ImuEvent de{ImuEventType::DoubleTap, currentFace_, currentFace_, 0, nowMs};
          for (uint8_t i = 0; i < listenerCount_; i++) listeners_[i](de);
        } else if (tapCount_ >= 3) {
          ImuEvent te{ImuEventType::TripleTap, currentFace_, currentFace_, 0, nowMs};
          for (uint8_t i = 0; i < listenerCount_; i++) listeners_[i](te);
        }
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
