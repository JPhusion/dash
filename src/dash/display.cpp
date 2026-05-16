#include "dash/display.h"

#include <Wire.h>

#include "Common.h"
#include "Face.h"
#include "FaceEmotions.hpp"
#include "dash/log.h"
#include "dash/pins.h"

namespace dash {

namespace {
constexpr const char* kTag = "Display";

constexpr uint16_t kScreenWidth  = 128;
constexpr uint16_t kScreenHeight = 64;
constexpr uint16_t kEyeSize      = 38;

Face* g_face = nullptr;
Display* g_singleton = nullptr;

// Map a Dash EyeState to the underlying Face/Eye library's emotion + look.
// Each entry sets a primary emotion and an optional look direction. The eye
// library handles transitions/blinking on its own.
struct EyeStateMapping {
  eEmotions emotion;
  int8_t lookX;    // -1 left, 0 center, 1 right
  int8_t lookY;    // -1 bottom, 0 center, 1 top
};

EyeStateMapping mapEyeState(EyeState s) {
  switch (s) {
    case EyeState::Idle:         return {eEmotions::Normal,    0,  0};
    case EyeState::Attentive:    return {eEmotions::Surprised, 0,  0};
    case EyeState::Focused:      return {eEmotions::Focused,   0,  0};
    case EyeState::Surprised:    return {eEmotions::Surprised, 0,  1};
    case EyeState::Sleepy:       return {eEmotions::Sleepy,    0,  0};
    case EyeState::Drowsy1:      return {eEmotions::Sleepy,    0,  0};
    case EyeState::Drowsy2:      return {eEmotions::Sleepy,    0, -1};
    case EyeState::Drowsy3:      return {eEmotions::Sleepy,    0, -1};
    case EyeState::Drowsy4:      return {eEmotions::Sleepy,    0, -1};
    case EyeState::Drowsy5:      return {eEmotions::Sleepy,    0, -1};
    case EyeState::Asleep:       return {eEmotions::Sleepy,    0, -1};
    case EyeState::Sad:          return {eEmotions::Sad,       0, -1};
    case EyeState::Happy:        return {eEmotions::Happy,     0,  0};
    case EyeState::Confused:     return {eEmotions::Skeptic,   0,  0};
    case EyeState::SideEye:      return {eEmotions::Suspicious, 1, 0};
    case EyeState::Angry:        return {eEmotions::Angry,     0,  0};
    case EyeState::Disappointed: return {eEmotions::Unimpressed, 0, -1};
    case EyeState::Heart:        return {eEmotions::Glee,      0,  0};
    case EyeState::Celebrating:  return {eEmotions::Glee,      0,  1};
    case EyeState::Searching:    return {eEmotions::Suspicious, 0, 0};
  }
  return {eEmotions::Normal, 0, 0};
}

void applyLook(Face* face, int8_t x, int8_t y) {
  // Map int8 (-1,0,1) to library's float (-1.0..1.0).
  face->Look.LookAt(static_cast<float>(x), static_cast<float>(y));
}

}  // namespace

Display::Display()
    : mutex_(xSemaphoreCreateMutex()),
      task_(nullptr),
      running_(false),
      paused_(false),
      targetEye_(EyeState::Idle),
      appliedEye_(EyeState::Idle),
      overlay_(Overlay::None),
      progressPct_(0),
      autoLook_(true) {
  text1_[0] = '\0';
  text2_[0] = '\0';
  qrPayload_[0] = '\0';
}

bool Display::begin() {
  if (g_face != nullptr) {
    log::warn(kTag, "begin() called twice");
    return true;
  }
  // u8g2's constructor in Face.cpp already supplies SCL/SDA pins; calling
  // Face's constructor invokes u8g2.begin() which sets up Wire.
  g_face = new (std::nothrow) Face(kScreenWidth, kScreenHeight, kEyeSize);
  if (!g_face) {
    log::error(kTag, "Face alloc failed");
    return false;
  }
  g_face->RandomBehavior = false;     // we drive emotions explicitly
  g_face->RandomLook = autoLook_;
  g_face->RandomBlink = true;
  g_face->Expression.GoTo_Normal();
  g_face->LookFront();
  log::info(kTag, "init OK (SH1106 128x64, eye size %u)", kEyeSize);
  return true;
}

void Display::start() {
  if (running_) return;
  running_ = true;
  // 4 KB stack — Face::Draw uses U8g2 full-frame buffer (1 KB) plus call frames.
  xTaskCreatePinnedToCore(&Display::renderTaskTrampoline, "display", 4096, this,
                          1 /*prio*/, &task_, 1 /*core*/);
}

void Display::stop() {
  running_ = false;
  // Render task self-deletes on its next loop iteration.
  if (task_ != nullptr) {
    for (int i = 0; i < 50 && eTaskGetState(task_) != eDeleted; ++i) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    task_ = nullptr;
  }
}

void Display::setEyeState(EyeState s) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  targetEye_ = s;
  xSemaphoreGive(mutex_);
}

EyeState Display::eyeState() const {
  return targetEye_;
}

void Display::showBootSplash() {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  overlay_ = Overlay::BootSplash;
  xSemaphoreGive(mutex_);
}

void Display::showProgress(uint8_t pct) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  overlay_ = Overlay::Progress;
  progressPct_ = pct > 100 ? 100 : pct;
  xSemaphoreGive(mutex_);
}

void Display::showText(const char* line1, const char* line2) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  overlay_ = Overlay::Text;
  strncpy(text1_, line1 ? line1 : "", sizeof(text1_) - 1);
  text1_[sizeof(text1_) - 1] = '\0';
  strncpy(text2_, line2 ? line2 : "", sizeof(text2_) - 1);
  text2_[sizeof(text2_) - 1] = '\0';
  xSemaphoreGive(mutex_);
}

void Display::showQR(const char* data) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  overlay_ = Overlay::QR;
  strncpy(qrPayload_, data ? data : "", sizeof(qrPayload_) - 1);
  qrPayload_[sizeof(qrPayload_) - 1] = '\0';
  xSemaphoreGive(mutex_);
}

void Display::clearOverlay() {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  overlay_ = Overlay::None;
  xSemaphoreGive(mutex_);
}

void Display::blink() {
  if (g_face) g_face->DoBlink();
}

void Display::setAutoLook(bool on) {
  autoLook_ = on;
  if (g_face) g_face->RandomLook = on;
}

void Display::pause(bool on) { paused_ = on; }

void Display::renderTaskTrampoline(void* arg) {
  static_cast<Display*>(arg)->renderTaskLoop();
}

void Display::renderTaskLoop() {
  const TickType_t framePeriod = pdMS_TO_TICKS(33);  // ~30 fps
  while (running_) {
    if (paused_) {
      vTaskDelay(framePeriod);
      continue;
    }

    // Sync target state.
    EyeState want;
    Overlay overlay;
    uint8_t pct;
    char l1[24], l2[24], qr[128];
    xSemaphoreTake(mutex_, portMAX_DELAY);
    want = targetEye_;
    overlay = overlay_;
    pct = progressPct_;
    memcpy(l1, text1_, sizeof(l1));
    memcpy(l2, text2_, sizeof(l2));
    memcpy(qr, qrPayload_, sizeof(qr));
    xSemaphoreGive(mutex_);

    if (want != appliedEye_) {
      applyEyeState(want);
      appliedEye_ = want;
    }

    // For overlays that hide the eyes (Text, QR, BootSplash) we draw entirely
    // ourselves. For Progress we let the eye library draw, then overlay a bar.
    if (overlay == Overlay::Text || overlay == Overlay::QR ||
        overlay == Overlay::BootSplash) {
      u8g2.clearBuffer();
      if (overlay == Overlay::BootSplash) {
        u8g2.setFont(u8g2_font_logisoso24_tr);
        u8g2.setCursor(20, 44);
        u8g2.print("Dash");
      } else if (overlay == Overlay::Text) {
        u8g2.setFont(u8g2_font_helvR12_tr);
        if (l1[0]) {
          int w = u8g2.getStrWidth(l1);
          u8g2.setCursor((kScreenWidth - w) / 2, l2[0] ? 26 : 38);
          u8g2.print(l1);
        }
        if (l2[0]) {
          int w = u8g2.getStrWidth(l2);
          u8g2.setCursor((kScreenWidth - w) / 2, 50);
          u8g2.print(l2);
        }
      } else {
        // Simple ASCII rendering for QR until we add a real QR code lib.
        u8g2.setFont(u8g2_font_5x7_tr);
        u8g2.setCursor(0, 8);
        u8g2.print("QR:");
        u8g2.setCursor(0, 20);
        u8g2.print(qr);
      }
      u8g2.sendBuffer();
    } else {
      // Normal eye render path.
      g_face->Update();
      if (overlay == Overlay::Progress) {
        // Draw a 4px bar at the bottom on top of the eye buffer. The eye lib
        // just called sendBuffer() so we redraw the bottom 4 rows.
        u8g2.setDrawColor(0);
        u8g2.drawBox(0, 60, kScreenWidth, 4);
        u8g2.setDrawColor(1);
        int filled = (pct * kScreenWidth) / 100;
        u8g2.drawBox(0, 60, filled, 4);
        u8g2.sendBuffer();
      }
    }

    vTaskDelay(framePeriod);
  }

  task_ = nullptr;
  vTaskDelete(nullptr);
}

void Display::applyEyeState(EyeState s) {
  auto map = mapEyeState(s);
  switch (map.emotion) {
    case eEmotions::Normal:     g_face->Expression.GoTo_Normal(); break;
    case eEmotions::Angry:      g_face->Expression.GoTo_Angry(); break;
    case eEmotions::Glee:       g_face->Expression.GoTo_Glee(); break;
    case eEmotions::Happy:      g_face->Expression.GoTo_Happy(); break;
    case eEmotions::Sad:        g_face->Expression.GoTo_Sad(); break;
    case eEmotions::Worried:    g_face->Expression.GoTo_Worried(); break;
    case eEmotions::Focused:    g_face->Expression.GoTo_Focused(); break;
    case eEmotions::Annoyed:    g_face->Expression.GoTo_Annoyed(); break;
    case eEmotions::Surprised:  g_face->Expression.GoTo_Surprised(); break;
    case eEmotions::Skeptic:    g_face->Expression.GoTo_Skeptic(); break;
    case eEmotions::Frustrated: g_face->Expression.GoTo_Frustrated(); break;
    case eEmotions::Unimpressed:g_face->Expression.GoTo_Unimpressed(); break;
    case eEmotions::Sleepy:     g_face->Expression.GoTo_Sleepy(); break;
    case eEmotions::Suspicious: g_face->Expression.GoTo_Suspicious(); break;
    case eEmotions::Squint:     g_face->Expression.GoTo_Squint(); break;
    case eEmotions::Furious:    g_face->Expression.GoTo_Furious(); break;
    case eEmotions::Scared:     g_face->Expression.GoTo_Scared(); break;
    case eEmotions::Awe:        g_face->Expression.GoTo_Awe(); break;
    default: g_face->Expression.GoTo_Normal(); break;
  }
  applyLook(g_face, map.lookX, map.lookY);
  log::debug(kTag, "eye -> %d", static_cast<int>(s));
}

Display& display() {
  if (g_singleton == nullptr) g_singleton = new Display();
  return *g_singleton;
}

}  // namespace dash
