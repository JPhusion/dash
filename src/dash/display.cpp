#include "dash/display.h"

#include <Wire.h>

#include "Common.h"
#include "Face.h"
#include "FaceEmotions.hpp"
#include "dash/imu.h"
#include "dash/log.h"
#include "dash/pins.h"
#include "dash/session.h"
#include "dash/state_machine.h"
#include "dash/wifi_ap.h"

namespace dash {

namespace {
constexpr const char* kTag = "Display";

constexpr uint16_t kScreenWidth  = 128;
constexpr uint16_t kScreenHeight = 64;
constexpr uint16_t kEyeSize      = 38;

// `::Face` (global namespace) is the eye-library class. `dash::Face` is the
// IMU's orientation enum — they collide inside `namespace dash` so every
// reference to the eye-library type below must be fully qualified.
::Face* g_face = nullptr;
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
    // Dizzy uses Sleepy with eyes drifted off-center — combined with the
    // periodic look-shuffle in playDizzyAnimation() this reads as "woozy".
    case EyeState::Dizzy:        return {eEmotions::Sleepy,    -1, -1};
    case EyeState::Annoyed:      return {eEmotions::Unimpressed, 0,  0};
  }
  return {eEmotions::Normal, 0, 0};
}

void applyLook(::Face* face, int8_t x, int8_t y) {
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
      autoLook_(true),
      arrowDir_(ArrowDir::None),
      menuBarPct_(0) {
  text1_[0] = '\0';
  text2_[0] = '\0';
  qrPayload_[0] = '\0';
  menuTitle_[0] = '\0';
  menuPrev_[0]  = '\0';
  menuItem_[0]  = '\0';
  menuNext_[0]  = '\0';
  menuValue_[0] = '\0';
}

bool Display::begin() {
  if (g_face != nullptr) {
    log::warn(kTag, "begin() called twice");
    return true;
  }
  // u8g2's constructor in Face.cpp already supplies SCL/SDA pins; calling
  // Face's constructor invokes u8g2.begin() which sets up Wire.
  g_face = new (std::nothrow) ::Face(kScreenWidth, kScreenHeight, kEyeSize);
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

void Display::showBig(const char* word) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  overlay_ = Overlay::Big;
  strncpy(text1_, word ? word : "", sizeof(text1_) - 1);
  text1_[sizeof(text1_) - 1] = '\0';
  text2_[0] = '\0';
  xSemaphoreGive(mutex_);
}

void Display::showInverted(const char* word) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  overlay_ = Overlay::Inverted;
  strncpy(text1_, word ? word : "", sizeof(text1_) - 1);
  text1_[sizeof(text1_) - 1] = '\0';
  text2_[0] = '\0';
  xSemaphoreGive(mutex_);
}

void Display::showArrow(ArrowDir dir) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  overlay_ = Overlay::Arrow;
  arrowDir_ = dir;
  xSemaphoreGive(mutex_);
}

void Display::showGravityBall() {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  overlay_ = Overlay::GravityBall;
  xSemaphoreGive(mutex_);
}

void Display::showMenuList(const char* title,
                           const char* prev, const char* item, const char* next) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  overlay_ = Overlay::MenuList;
  strlcpy(menuTitle_, title ? title : "", sizeof(menuTitle_));
  strlcpy(menuPrev_,  prev  ? prev  : "", sizeof(menuPrev_));
  strlcpy(menuItem_,  item  ? item  : "", sizeof(menuItem_));
  strlcpy(menuNext_,  next  ? next  : "", sizeof(menuNext_));
  xSemaphoreGive(mutex_);
}

void Display::showMenuEdit(const char* name, uint8_t pct, const char* value) {
  if (pct > 100) pct = 100;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  overlay_ = Overlay::MenuEdit;
  strlcpy(menuTitle_, name  ? name  : "", sizeof(menuTitle_));
  strlcpy(menuValue_, value ? value : "", sizeof(menuValue_));
  menuBarPct_ = pct;
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

void Display::lookAt(float x, float y) {
  if (!g_face) return;
  // While we're driving the gaze explicitly, the autonomous look-around
  // task would just fight us. Stay disabled until setAutoLook(true).
  if (g_face->RandomLook) g_face->RandomLook = false;
  if (x < -1.0f) x = -1.0f; if (x > 1.0f) x = 1.0f;
  if (y < -1.0f) y = -1.0f; if (y > 1.0f) y = 1.0f;
  g_face->Look.LookAt(x, y);
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
    char mTitle[16], mPrev[20], mItem[20], mNext[20], mValue[16];
    uint8_t mBar;
    ArrowDir arrowDir;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    want = targetEye_;
    overlay = overlay_;
    pct = progressPct_;
    arrowDir = arrowDir_;
    memcpy(l1, text1_, sizeof(l1));
    memcpy(l2, text2_, sizeof(l2));
    memcpy(qr, qrPayload_, sizeof(qr));
    memcpy(mTitle, menuTitle_, sizeof(mTitle));
    memcpy(mPrev,  menuPrev_,  sizeof(mPrev));
    memcpy(mItem,  menuItem_,  sizeof(mItem));
    memcpy(mNext,  menuNext_,  sizeof(mNext));
    memcpy(mValue, menuValue_, sizeof(mValue));
    mBar = menuBarPct_;
    xSemaphoreGive(mutex_);

    if (want != appliedEye_) {
      applyEyeState(want);
      appliedEye_ = want;
    }

    // For overlays that hide the eyes we draw entirely ourselves. For
    // Progress we let the eye library draw, then overlay a bar.
    const bool isHidingOverlay =
        (overlay == Overlay::Text || overlay == Overlay::QR ||
         overlay == Overlay::BootSplash ||
         overlay == Overlay::Big || overlay == Overlay::Inverted ||
         overlay == Overlay::Arrow || overlay == Overlay::GravityBall ||
         overlay == Overlay::MenuList || overlay == Overlay::MenuEdit);

    if (isHidingOverlay) {
      u8g2.clearBuffer();
      if (overlay == Overlay::Inverted) {
        // White (filled) background, black (cutout) text. Done by drawing
        // a filled rect then setDrawColor(0) for the glyphs.
        u8g2.setDrawColor(1);
        u8g2.drawBox(0, 0, kScreenWidth, kScreenHeight);
        u8g2.setDrawColor(0);
      } else {
        u8g2.setDrawColor(1);
      }

      if (overlay == Overlay::BootSplash) {
        u8g2.setFont(u8g2_font_logisoso24_tr);
        u8g2.setCursor(20, 44);
        u8g2.print("Dash");
      } else if (overlay == Overlay::Big || overlay == Overlay::Inverted) {
        if (l1[0]) {
          // Pick the biggest font that fits the string in the 128 px width.
          // logisoso24 (24 px tall) is the marketing-sized cue; fall back to
          // logisoso18 / helvB18 / helvB12 as the string gets longer so
          // text never clips off the right edge.
          struct FontChoice { const uint8_t* font; int baselineY; };
          const FontChoice choices[] = {
            { u8g2_font_logisoso24_tr, 46 },
            { u8g2_font_logisoso18_tr, 42 },
            { u8g2_font_helvB14_tr,    40 },
            { u8g2_font_helvB12_tr,    38 },
          };
          int picked = 3;
          for (int i = 0; i < 4; i++) {
            u8g2.setFont(choices[i].font);
            int w = u8g2.getStrWidth(l1);
            if (w <= kScreenWidth - 4) { picked = i; break; }
          }
          u8g2.setFont(choices[picked].font);
          int w = u8g2.getStrWidth(l1);
          int x = (kScreenWidth - w) / 2;
          if (x < 2) x = 2;
          u8g2.setCursor(x, choices[picked].baselineY);
          u8g2.print(l1);
        }
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
      } else if (overlay == Overlay::Arrow) {
        // Filled triangle head + thick rectangular stem, centered on the
        // OLED (cx=64, cy=32). Roughly 48 px wide × 40 px tall — fills
        // most of the screen so it's readable across the desk.
        const int cx = kScreenWidth / 2;
        const int cy = kScreenHeight / 2;
        const int half = 22;           // half-size of the arrow body
        const int stemH = 12;
        switch (arrowDir) {
          case ArrowDir::Up:
            u8g2.drawTriangle(cx, cy - half,        // tip
                              cx - half, cy,        // bottom-left
                              cx + half, cy);       // bottom-right
            u8g2.drawBox(cx - stemH/2, cy, stemH, half);    // stem below tip
            break;
          case ArrowDir::Down:
            u8g2.drawTriangle(cx, cy + half,
                              cx - half, cy,
                              cx + half, cy);
            u8g2.drawBox(cx - stemH/2, cy - half, stemH, half);
            break;
          case ArrowDir::Left:
            u8g2.drawTriangle(cx - half, cy,
                              cx, cy - half,
                              cx, cy + half);
            u8g2.drawBox(cx, cy - stemH/2, half, stemH);
            break;
          case ArrowDir::Right:
            u8g2.drawTriangle(cx + half, cy,
                              cx, cy - half,
                              cx, cy + half);
            u8g2.drawBox(cx - half, cy - stemH/2, half, stemH);
            break;
          default: break;
        }
      } else if (overlay == Overlay::GravityBall) {
        // Debug visualisation: a ball that "falls" toward gravity. We try
        // both intuitive 2-axis mappings of the body-frame gravity onto
        // the screen, drawn side-by-side, so the user can see which one
        // matches their physical intuition.
        //
        // Left half  (cx=32, cy=32): ball at (gx, gy)
        // Right half (cx=96, cy=32): ball at (gx, gz)
        //
        // Labels and the live gx/gy/gz numbers are printed across the
        // top edge so the user can correlate "I tilted to the right" with
        // the actual gravity components.
        auto s = imu().latest();
        u8g2.setFont(u8g2_font_5x7_tr);
        char buf[40];
        snprintf(buf, sizeof(buf), "x%+.2f y%+.2f z%+.2f",
                 s.gravityX, s.gravityY, s.gravityZ);
        u8g2.setCursor(0, 7);
        u8g2.print(buf);
        // Two arenas, each 60 px wide, 50 px tall, side-by-side, with a
        // central separator.
        const int arenaR = 22;                      // arena radius
        const int leftCx = 32, rightCx = 96;
        const int cy = 38;
        u8g2.drawFrame(leftCx - arenaR,  cy - arenaR, arenaR*2, arenaR*2);
        u8g2.drawFrame(rightCx - arenaR, cy - arenaR, arenaR*2, arenaR*2);
        // Labels under each arena.
        u8g2.setCursor(leftCx - 12, 64);  u8g2.print("xy");
        u8g2.setCursor(rightCx - 12, 64); u8g2.print("xz");
        // Clamp the gravity components to [-1, 1] so out-of-range values
        // still show inside the arena.
        auto clamp = [](float v) {
          if (v >  1.0f) return  1.0f;
          if (v < -1.0f) return -1.0f;
          return v;
        };
        float gx = clamp(s.gravityX);
        float gy = clamp(s.gravityY);
        float gz = clamp(s.gravityZ);
        const int r = 5;   // ball radius
        // Mapping LEFT: x = gx (cube right → screen right);
        //              y = gy (cube +Y → screen down).
        int lx = leftCx  + (int)(gx * (arenaR - r));
        int ly = cy      + (int)(gy * (arenaR - r));
        u8g2.drawDisc(lx, ly, r);
        // Mapping RIGHT: x = gx, y = gz.
        int rx = rightCx + (int)(gx * (arenaR - r));
        int ry = cy      + (int)(gz * (arenaR - r));
        u8g2.drawDisc(rx, ry, r);
      } else if (overlay == Overlay::MenuList) {
        // Three-row list view:
        //   row 0 (top, small): previous item (greyed) or chevron
        //   row 1 (mid, big):   highlighted item, drawn inverted
        //   row 2 (bottom, small): next item (greyed)
        // Tiny title bar at the very top.
        u8g2.setFont(u8g2_font_5x7_tr);
        if (mTitle[0]) {
          int tw = u8g2.getStrWidth(mTitle);
          u8g2.setCursor((kScreenWidth - tw) / 2, 8);
          u8g2.print(mTitle);
        }
        // Previous item — dimmed (rendered as plain text at small size).
        u8g2.setFont(u8g2_font_helvR08_tr);
        if (mPrev[0]) {
          int w = u8g2.getStrWidth(mPrev);
          u8g2.setCursor((kScreenWidth - w) / 2, 22);
          u8g2.print(mPrev);
        } else {
          // Top-of-list — show an up chevron hint.
          u8g2.setCursor(kScreenWidth / 2 - 3, 22);
          u8g2.print("^");
        }
        // Current item — highlighted in an inverted bar.
        u8g2.drawBox(0, 27, kScreenWidth, 18);
        u8g2.setDrawColor(0);
        u8g2.setFont(u8g2_font_helvB12_tr);
        if (mItem[0]) {
          int w = u8g2.getStrWidth(mItem);
          int x = (kScreenWidth - w) / 2;
          if (x < 2) x = 2;
          u8g2.setCursor(x, 41);
          u8g2.print(mItem);
        }
        u8g2.setDrawColor(1);
        u8g2.setFont(u8g2_font_helvR08_tr);
        if (mNext[0]) {
          int w = u8g2.getStrWidth(mNext);
          u8g2.setCursor((kScreenWidth - w) / 2, 58);
          u8g2.print(mNext);
        } else {
          u8g2.setCursor(kScreenWidth / 2 - 3, 58);
          u8g2.print("v");
        }
      } else if (overlay == Overlay::MenuEdit) {
        // Single-setting view:
        //   top:    setting name
        //   middle: big numeric/text value
        //   bottom: horizontal bar with current pct
        u8g2.setFont(u8g2_font_helvB08_tr);
        if (mTitle[0]) {
          int w = u8g2.getStrWidth(mTitle);
          u8g2.setCursor((kScreenWidth - w) / 2, 12);
          u8g2.print(mTitle);
        }
        u8g2.setFont(u8g2_font_logisoso18_tr);
        if (mValue[0]) {
          int w = u8g2.getStrWidth(mValue);
          int x = (kScreenWidth - w) / 2;
          if (x < 2) x = 2;
          u8g2.setCursor(x, 38);
          u8g2.print(mValue);
        }
        // Horizontal bar across the bottom. Frame + filled portion.
        const int barX = 8, barY = 50, barW = kScreenWidth - 16, barH = 8;
        u8g2.drawFrame(barX, barY, barW, barH);
        int fill = (int)((barW - 2) * (int)mBar / 100);
        if (fill > 0) u8g2.drawBox(barX + 1, barY + 1, fill, barH - 2);
        // Tilt arrows as hints at the bar's left/right.
        u8g2.setFont(u8g2_font_5x7_tr);
        u8g2.setCursor(1, barY + 7);    u8g2.print("<");
        u8g2.setCursor(kScreenWidth - 6, barY + 7); u8g2.print(">");
      } else {
        // Simple ASCII rendering for QR until we add a real QR code lib.
        u8g2.setFont(u8g2_font_5x7_tr);
        u8g2.setCursor(0, 8);
        u8g2.print("QR:");
        u8g2.setCursor(0, 20);
        u8g2.print(qr);
      }
      u8g2.setDrawColor(1);    // restore for subsequent draws
      u8g2.sendBuffer();
    } else {
      // Normal eye render path. For Progress overlay we have the eye lib
      // skip its own sendBuffer (DeferSend=true), composite the progress
      // bar onto the same buffer, and send once. Without this the user
      // sees a flicker because the bar is drawn AFTER the eye lib already
      // sent a frame without it.
      //
      // We additionally composite a small MM:SS countdown in the
      // top-right corner when we're mid-session AND running standalone
      // (no phone associated with the AP) — so the user can glance at
      // the cube and see time-remaining without the portal open.
      const bool progressOverlay = (overlay == Overlay::Progress);
      bool drawCountdown = false;
      char countdownBuf[8] = {0};
      if (stateMachine().state() == DeviceState::InSession &&
          wifiAp().stationCount() == 0) {
        auto snap = session().snapshot();
        if (snap.active && snap.targetMin > 0) {
          uint32_t elapsedMs =
              (millis() - snap.startedAtMs) - snap.pausedMs;
          uint32_t totalSec = (uint32_t)snap.targetMin * 60UL;
          uint32_t elapsedSec = elapsedMs / 1000UL;
          uint32_t remaining =
              (elapsedSec >= totalSec) ? 0 : (totalSec - elapsedSec);
          uint32_t mm = remaining / 60UL;
          uint32_t ss = remaining % 60UL;
          if (mm > 999) mm = 999;
          snprintf(countdownBuf, sizeof(countdownBuf),
                   "%02u:%02u", (unsigned)mm, (unsigned)ss);
          drawCountdown = true;
        }
      }

      const bool defer = progressOverlay || drawCountdown;
      g_face->DeferSend = defer;
      g_face->Update();
      if (drawCountdown) {
        // Top-right corner. 5x7 glyphs => "MM:SS" is 25 px wide. Clear
        // the underlying pixels first so the eye animation doesn't
        // bleed through the digits.
        u8g2.setFont(u8g2_font_5x7_tr);
        int w = u8g2.getStrWidth(countdownBuf);
        int x = kScreenWidth - w - 2;
        if (x < 0) x = 0;
        u8g2.setDrawColor(0);
        u8g2.drawBox(x - 1, 0, w + 2, 9);
        u8g2.setDrawColor(1);
        u8g2.setCursor(x, 7);
        u8g2.print(countdownBuf);
      }
      if (progressOverlay) {
        u8g2.setDrawColor(0);
        u8g2.drawBox(0, 60, kScreenWidth, 4);
        u8g2.setDrawColor(1);
        // Outline + fill so the bar is visible at any percentage.
        u8g2.drawFrame(0, 60, kScreenWidth, 4);
        int filled = (pct * (kScreenWidth - 2)) / 100;
        u8g2.drawBox(1, 61, filled, 2);
      }
      if (defer) {
        u8g2.sendBuffer();
        g_face->DeferSend = false;
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
