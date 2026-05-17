#include "dash/calibration.h"

#include <Arduino.h>
#include <math.h>

#include "dash/audio.h"
#include "dash/character.h"
#include "dash/display.h"
#include "dash/imu.h"
#include "dash/log.h"
#include "dash/sounds.h"
#include "dash/state_machine.h"

namespace dash {

namespace {
constexpr const char* kTag = "Calib";

struct Step {
  const char* prompt;     // shown on OLED
  const char* shortName;  // used in summary table
};

constexpr Step kSteps[] = {
  {"TAP TOP",      "tap-top"   },
  {"FLICK <-",     "flick-left"},
  {"FLICK ->",     "flick-rite"},
  {"FLICK ^",      "flick-up"  },
  {"FLICK v",      "flick-down"},
  {"SHAKE",        "shake"     },
};
constexpr uint8_t kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);
constexpr uint8_t kRepsPerStep = 3;
constexpr uint32_t kCaptureMs = 3000;
constexpr uint32_t kSettleMs  = 800;

struct Sample {
  float peakMag;
  float laxAtPeak, layAtPeak, lazAtPeak;
  float grxAtPeak, gryAtPeak, grzAtPeak;
};

void captureOne(Sample& out) {
  out.peakMag = 0;
  out.laxAtPeak = out.layAtPeak = out.lazAtPeak = 0;
  out.grxAtPeak = out.gryAtPeak = out.grzAtPeak = 0;
  uint32_t deadline = millis() + kCaptureMs;
  while (millis() < deadline) {
    auto s = imu().latest();
    float mag = sqrtf(s.ax * s.ax + s.ay * s.ay + s.az * s.az);
    if (mag > out.peakMag) {
      out.peakMag    = mag;
      out.laxAtPeak  = s.ax;
      out.layAtPeak  = s.ay;
      out.lazAtPeak  = s.az;
      out.grxAtPeak  = s.gravityX;
      out.gryAtPeak  = s.gravityY;
      out.grzAtPeak  = s.gravityZ;
    }
    vTaskDelay(pdMS_TO_TICKS(8));
  }
}

const char* dominantAxisOf(const Sample& s) {
  float ax = fabsf(s.laxAtPeak), ay = fabsf(s.layAtPeak), az = fabsf(s.lazAtPeak);
  if (ax >= ay && ax >= az) return s.laxAtPeak > 0 ? "+X" : "-X";
  if (ay >= ax && ay >= az) return s.layAtPeak > 0 ? "+Y" : "-Y";
  return s.lazAtPeak > 0 ? "+Z" : "-Z";
}

}  // namespace

void runCalibration() {
  Serial.println();
  Serial.println(F("################################################"));
  Serial.println(F("##  Dash gesture calibration                   ##"));
  Serial.println(F("##  Follow the prompts on the OLED.            ##"));
  Serial.println(F("################################################"));

  DeviceState origState = stateMachine().state();
  stateMachine().transitionTo(DeviceState::InMenu);   // suppresses gaze tracking + idle

  Sample results[kStepCount * kRepsPerStep];

  for (uint8_t s = 0; s < kStepCount; s++) {
    for (uint8_t r = 0; r < kRepsPerStep; r++) {
      char prompt[24];
      snprintf(prompt, sizeof(prompt), "%s %u/%u", kSteps[s].prompt, r + 1, kRepsPerStep);
      Serial.printf("\n[CALIB] %s — get ready…\n", prompt);
      display().showBig(prompt);
      vTaskDelay(pdMS_TO_TICKS(kSettleMs));

      // Go cue: 200ms inverted flash
      display().showInverted(prompt);
      audio().play(sounds::kMenuBlip, AudioFormat::Pcm8kHzMono8, true);

      Sample& smp = results[s * kRepsPerStep + r];
      captureOne(smp);

      display().clearOverlay();
      audio().play(sounds::kGameCorrect, AudioFormat::Pcm8kHzMono8, true);
      Serial.printf("[CALIB] %-12s rep%u  peak=%.2fg  dom=%s  "
                    "lax=%.2f lay=%.2f laz=%.2f  gx=%.2f gy=%.2f gz=%.2f\n",
                    kSteps[s].shortName, r + 1, smp.peakMag, dominantAxisOf(smp),
                    smp.laxAtPeak, smp.layAtPeak, smp.lazAtPeak,
                    smp.grxAtPeak, smp.gryAtPeak, smp.grzAtPeak);
      vTaskDelay(pdMS_TO_TICKS(400));
    }
  }

  // Summary table: averaged peak + dominant-axis vote per gesture.
  Serial.println();
  Serial.println(F("================ Calibration summary ================"));
  Serial.println(F("gesture       avg_peak   dominant axis      mean lin-accel (lax,lay,laz)"));
  for (uint8_t s = 0; s < kStepCount; s++) {
    float avgPeak = 0;
    float meanLax = 0, meanLay = 0, meanLaz = 0;
    int axisVotes[6] = {0,0,0,0,0,0};  // +X -X +Y -Y +Z -Z
    for (uint8_t r = 0; r < kRepsPerStep; r++) {
      const Sample& smp = results[s * kRepsPerStep + r];
      avgPeak += smp.peakMag;
      meanLax += smp.laxAtPeak;
      meanLay += smp.layAtPeak;
      meanLaz += smp.lazAtPeak;
      const char* d = dominantAxisOf(smp);
      if      (!strcmp(d, "+X")) axisVotes[0]++;
      else if (!strcmp(d, "-X")) axisVotes[1]++;
      else if (!strcmp(d, "+Y")) axisVotes[2]++;
      else if (!strcmp(d, "-Y")) axisVotes[3]++;
      else if (!strcmp(d, "+Z")) axisVotes[4]++;
      else if (!strcmp(d, "-Z")) axisVotes[5]++;
    }
    avgPeak /= kRepsPerStep;
    meanLax /= kRepsPerStep; meanLay /= kRepsPerStep; meanLaz /= kRepsPerStep;
    int bestIdx = 0;
    for (int i = 1; i < 6; i++) if (axisVotes[i] > axisVotes[bestIdx]) bestIdx = i;
    static const char* kAxisNames[] = {"+X","-X","+Y","-Y","+Z","-Z"};
    Serial.printf("%-12s  %5.2fg     %-3s  (%u/%u votes)   (%.2f, %.2f, %.2f)\n",
                  kSteps[s].shortName, avgPeak,
                  kAxisNames[bestIdx], axisVotes[bestIdx], kRepsPerStep,
                  meanLax, meanLay, meanLaz);
  }
  Serial.println(F("======================================================"));
  Serial.println(F("Tip: 'tap-top' should land on a Z axis; flicks left/right"));
  Serial.println(F("should land on opposite X signs; flicks up/down on Y."));
  Serial.println(F("If your physical 'left' lands on +Y not -X, the IMU is"));
  Serial.println(F("rotated relative to firmware assumptions — share this"));
  Serial.println(F("table and I'll flip the mapping in imu.cpp."));

  display().clearOverlay();
  stateMachine().transitionTo(origState);
}

namespace {

struct OrientStep {
  const char* prompt;     // OLED prompt
  const char* shortName;  // serial summary key
};

// 10 poses total: 6 resting orientations + 4 tilts from screen-up.
// The user holds each pose; the cube auto-captures when gyro is low.
constexpr OrientStep kOrientSteps[] = {
  // Resting orientations — six unique gravity vectors.
  {"SCREEN FORWARD",      "screen-fwd"  },
  {"SCREEN UP",           "screen-up"   },
  {"SCREEN DOWN",         "screen-down" },
  {"SCREEN ROT RIGHT",    "screen-rotR" },
  {"SCREEN ROT LEFT",     "screen-rotL" },
  {"UPSIDE DOWN",         "upside-down" },
  // Tilts FROM screen-up (cube still nominally face-up, just tipped).
  {"FACE-UP, TILT AWAY",  "tilt-fwd"    },
  {"FACE-UP, TILT TOWARD","tilt-back"   },
  {"FACE-UP, TILT RIGHT", "tilt-right"  },
  {"FACE-UP, TILT LEFT",  "tilt-left"   },
};
constexpr uint8_t kOrientStepCount = sizeof(kOrientSteps) / sizeof(kOrientSteps[0]);

struct OrientSample {
  float gx, gy, gz;   // mean gravity vector during the capture window
  float lax, lay, laz;
  float gyroMag;      // mean |gyro| during capture (should be ~0)
};

// Wait until the cube has been stationary (low gyro) for kStableMs, then
// average gravity over a 400 ms window. If the cube never stabilises
// within kTimeoutMs we still capture what we have so the walkthrough
// doesn't stall.
void captureOrient(OrientSample& out) {
  constexpr uint32_t kStableMs    = 600;
  constexpr uint32_t kCaptureMs   = 400;
  constexpr uint32_t kTimeoutMs   = 6000;
  constexpr float    kGyroMaxDeg  = 5.0f;
  uint32_t stableSince = 0;
  uint32_t deadline = millis() + kTimeoutMs;
  while (millis() < deadline) {
    auto s = imu().latest();
    float gMag = sqrtf(s.gx*s.gx + s.gy*s.gy + s.gz*s.gz);
    if (gMag < kGyroMaxDeg) {
      if (stableSince == 0) stableSince = millis();
      if (millis() - stableSince > kStableMs) break;
    } else {
      stableSince = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  // Now capture an average across kCaptureMs.
  float gx = 0, gy = 0, gz = 0, lax = 0, lay = 0, laz = 0, gyroSum = 0;
  int n = 0;
  uint32_t end = millis() + kCaptureMs;
  while (millis() < end) {
    auto s = imu().latest();
    gx += s.gravityX; gy += s.gravityY; gz += s.gravityZ;
    lax += s.ax; lay += s.ay; laz += s.az;
    gyroSum += sqrtf(s.gx*s.gx + s.gy*s.gy + s.gz*s.gz);
    n++;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (n == 0) n = 1;
  out.gx = gx / n; out.gy = gy / n; out.gz = gz / n;
  out.lax = lax / n; out.lay = lay / n; out.laz = laz / n;
  out.gyroMag = gyroSum / n;
}

const char* gravityDomAxis(const OrientSample& s) {
  float ax = fabsf(s.gx), ay = fabsf(s.gy), az = fabsf(s.gz);
  if (ax >= ay && ax >= az) return s.gx > 0 ? "+X" : "-X";
  if (ay >= ax && ay >= az) return s.gy > 0 ? "+Y" : "-Y";
  return s.gz > 0 ? "+Z" : "-Z";
}

}  // namespace

void runOrientCalibration() {
  Serial.println();
  Serial.println(F("################################################"));
  Serial.println(F("##  Dash orientation calibration               ##"));
  Serial.println(F("##  Follow the prompts on the OLED.            ##"));
  Serial.println(F("##  Hold each pose still; we auto-capture when ##"));
  Serial.println(F("##  the gyro is quiet.                         ##"));
  Serial.println(F("################################################"));

  DeviceState origState = stateMachine().state();
  stateMachine().transitionTo(DeviceState::InMenu);

  OrientSample results[kOrientStepCount];

  for (uint8_t i = 0; i < kOrientStepCount; i++) {
    Serial.printf("\n[ORIENT] %s — get in position…\n", kOrientSteps[i].prompt);
    display().showBig(kOrientSteps[i].prompt);
    vTaskDelay(pdMS_TO_TICKS(1500));
    display().showInverted(kOrientSteps[i].prompt);
    audio().play(sounds::kMenuBlip, AudioFormat::Pcm8kHzMono8, true);
    captureOrient(results[i]);
    display().clearOverlay();
    audio().play(sounds::kGameCorrect, AudioFormat::Pcm8kHzMono8, true);
    Serial.printf("[ORIENT] %-12s  gx=%+.2f gy=%+.2f gz=%+.2f  "
                  "lax=%+.2f lay=%+.2f laz=%+.2f  gyro=%.1fdps  dom=%s\n",
                  kOrientSteps[i].shortName,
                  results[i].gx, results[i].gy, results[i].gz,
                  results[i].lax, results[i].lay, results[i].laz,
                  results[i].gyroMag,
                  gravityDomAxis(results[i]));
    vTaskDelay(pdMS_TO_TICKS(400));
  }

  // Summary table — easy to copy/paste back.
  Serial.println();
  Serial.println(F("=========== Orientation calibration summary ============"));
  Serial.println(F("pose            gx     gy     gz     dom"));
  for (uint8_t i = 0; i < kOrientStepCount; i++) {
    Serial.printf("%-13s  %+.2f  %+.2f  %+.2f   %s\n",
                  kOrientSteps[i].shortName,
                  results[i].gx, results[i].gy, results[i].gz,
                  gravityDomAxis(results[i]));
  }
  Serial.println(F("---------------------------------------------------------"));
  Serial.println(F("Send this table back so the menu trigger face + tilt"));
  Serial.println(F("direction mapping can be fixed."));
  Serial.println(F("========================================================="));

  display().clearOverlay();
  stateMachine().transitionTo(origState);
}

}  // namespace dash
