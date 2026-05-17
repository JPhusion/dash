// touch.h — capacitive touch pad on GPIO27 (T7).
//
// Calibrates a baseline at boot from N samples, derives a threshold as 70% of
// baseline. Events: onTouch (rising edge), onLongPress (held > 1s), and
// onDoubleTouch. Polled in a low-priority FreeRTOS task on core 0 (the cap-
// touch peripheral is core-agnostic so this just keeps it off core 1 which is
// busy with rendering and audio).
//
// Reliability caveat: on battery (no chassis ground reference), the readings
// drift heavily; threshold is recomputed every 30 s when no touch is observed
// to compensate.

#ifndef DASH_TOUCH_H
#define DASH_TOUCH_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <functional>

namespace dash {

enum class TouchEventType : uint8_t {
  Touch,
  DoubleTouch,
  LongPress,
};

struct TouchEvent {
  TouchEventType type;
  uint16_t rawValue;       // raw touchRead value at event time
  uint32_t millis_ts;
};

using TouchListener = std::function<void(const TouchEvent&)>;

class Touch {
 public:
  Touch();

  // Calibrate baseline + threshold from N=16 samples. Returns false if the
  // baseline is implausibly low (touch pad shorted to ground at boot).
  bool begin();

  void start();
  void stop();

  void onEvent(TouchListener l);

  // Tune knobs.
  void setLongPressMs(uint32_t ms) { longPressMs_ = ms; }
  void setDoubleTouchWindowMs(uint32_t ms) { doubleTouchMs_ = ms; }

  uint16_t baseline() const { return baseline_; }
  uint16_t threshold() const { return threshold_; }

  // Is the pad currently being touched? Polled by other modules (e.g. Imu
  // ramps up tap sensitivity when this returns true so a finger contacting
  // the pad while tapping the cube counts as a deliberate input).
  bool isTouched() const { return wasTouched_; }

  // Test hook: synthesise a touch event without the cap pad. Used by the
  // serial CLI / self-test runner.
  void injectEvent(TouchEvent e);

 private:
  static void taskTrampoline(void* arg);
  void loop();

  void recalibrate();
  void emit(TouchEvent e);

  TaskHandle_t task_;
  volatile bool running_;
  TouchListener listeners_[3];
  uint8_t listenerCount_;

  uint16_t baseline_;
  uint16_t threshold_;
  uint32_t longPressMs_;
  uint32_t doubleTouchMs_;

  bool wasTouched_;
  uint32_t touchStartMs_;
  uint32_t lastReleaseMs_;
  uint8_t consecutiveTouches_;
  bool longPressFired_;
  uint32_t lastQuietMs_;     // last time the pad was "untouched" — for recal cadence
};

Touch& touch();

}  // namespace dash

#endif
