#include "dash/touch.h"

#include "dash/log.h"
#include "dash/pins.h"

namespace dash {

namespace {
constexpr const char* kTag = "Touch";
Touch* g_singleton = nullptr;

uint16_t medianReading(int samples) {
  // Use a small bubble sort (samples is tiny) and return the median.
  uint16_t vals[32];
  if (samples > 32) samples = 32;
  for (int i = 0; i < samples; i++) {
    vals[i] = touchRead(pins::TOUCH);
    delay(5);
  }
  for (int i = 0; i < samples; i++) {
    for (int j = i + 1; j < samples; j++) {
      if (vals[j] < vals[i]) {
        uint16_t t = vals[i]; vals[i] = vals[j]; vals[j] = t;
      }
    }
  }
  return vals[samples / 2];
}
}  // namespace

Touch::Touch()
    : task_(nullptr),
      running_(false),
      listenerCount_(0),
      baseline_(0),
      threshold_(0),
      longPressMs_(1000),
      doubleTouchMs_(400),
      wasTouched_(false),
      touchStartMs_(0),
      lastReleaseMs_(0),
      consecutiveTouches_(0),
      longPressFired_(false),
      lastQuietMs_(0) {}

bool Touch::begin() {
  baseline_ = medianReading(16);
  if (baseline_ < 5) {
    log::warn(kTag, "baseline %u suspiciously low — pin shorted?", baseline_);
    return false;
  }
  threshold_ = (baseline_ * 70) / 100;  // touched < 70% of baseline
  log::info(kTag, "baseline=%u threshold=%u", baseline_, threshold_);
  return true;
}

void Touch::start() {
  if (running_) return;
  running_ = true;
  xTaskCreatePinnedToCore(&Touch::taskTrampoline, "touch", 3072, this, 1, &task_, 0);
}

void Touch::stop() { running_ = false; }

void Touch::onEvent(TouchListener l) {
  if (listenerCount_ >= 3) {
    log::warn(kTag, "listener slots full");
    return;
  }
  listeners_[listenerCount_++] = std::move(l);
}

void Touch::recalibrate() {
  uint16_t b = medianReading(8);
  if (b > baseline_ * 0.8 && b < baseline_ * 1.2) {
    baseline_ = b;
    threshold_ = (b * 70) / 100;
    log::debug(kTag, "recal baseline=%u threshold=%u", baseline_, threshold_);
  }
}

void Touch::injectEvent(TouchEvent e) {
  emit(e);
}

void Touch::emit(TouchEvent e) {
  e.millis_ts = millis();
  for (uint8_t i = 0; i < listenerCount_; i++) listeners_[i](e);
}

void Touch::taskTrampoline(void* arg) { static_cast<Touch*>(arg)->loop(); }

void Touch::loop() {
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(30);   // ~33 Hz polling
  while (running_) {
    uint16_t v = touchRead(pins::TOUCH);
    uint32_t now = millis();
    bool touchedNow = (v < threshold_);

    if (touchedNow) {
      if (!wasTouched_) {
        touchStartMs_ = now;
        longPressFired_ = false;
        // Track double-touch sequence.
        if ((now - lastReleaseMs_) < doubleTouchMs_) {
          consecutiveTouches_++;
        } else {
          consecutiveTouches_ = 1;
        }
        emit({TouchEventType::Touch, v, 0});
        if (consecutiveTouches_ == 2) {
          emit({TouchEventType::DoubleTouch, v, 0});
          consecutiveTouches_ = 0;
        }
      } else if (!longPressFired_ && (now - touchStartMs_) > longPressMs_) {
        longPressFired_ = true;
        emit({TouchEventType::LongPress, v, 0});
      }
    } else {
      if (wasTouched_) lastReleaseMs_ = now;
      lastQuietMs_ = now;
      // Recal every 30 s of continuous "not touched".
      static uint32_t lastRecal = 0;
      if (now - lastRecal > 30000) {
        recalibrate();
        lastRecal = now;
      }
    }
    wasTouched_ = touchedNow;
    vTaskDelayUntil(&lastWake, period);
  }
  task_ = nullptr;
  vTaskDelete(nullptr);
}

Touch& touch() {
  if (!g_singleton) g_singleton = new Touch();
  return *g_singleton;
}

}  // namespace dash
