// display.h — wraps the vendored eye library and provides a unified rendering
// surface for both animated eyes and ad-hoc UI overlays (progress bars, text,
// QR codes, boot splash).
//
// Threading: eye animation and frame transfer run inside a dedicated FreeRTOS
// task pinned to core 1. All public methods are safe to call from other tasks;
// they update target state via a mutex-protected struct, the render task picks
// up the change on the next frame.

#ifndef DASH_DISPLAY_H
#define DASH_DISPLAY_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace dash {

enum class EyeState : uint8_t {
  Idle,
  Attentive,
  Focused,
  Surprised,
  Sleepy,
  Drowsy1,
  Drowsy2,
  Drowsy3,
  Drowsy4,
  Drowsy5,
  Asleep,
  Sad,
  Happy,
  Confused,
  SideEye,
  Angry,
  Disappointed,
  Heart,
  Celebrating,
  Searching,
};

enum class Overlay : uint8_t {
  None,
  BootSplash,
  Progress,         // shows EyeState plus a thin progress bar at the bottom
  Text,             // hides eyes, prints centered text instead
  QR,               // hides eyes, draws a QR code
  Big,              // hides eyes, prints one BIG word centred (game prompts)
  Inverted,         // big text but the whole screen inverted (reaction GO!)
};

class Display {
 public:
  Display();

  // Brings up I2C + the eye library. Idempotent.
  bool begin();

  // Render-thread control.
  void start();      // spawn render task (pinned to core 1)
  void stop();       // tear down render task (used before deep sleep)

  // High-level state.
  void setEyeState(EyeState s);
  EyeState eyeState() const;

  // Overlays — all replace the current draw mode until cleared.
  void showBootSplash();
  void showProgress(uint8_t percent);     // 0..100
  void showText(const char* line1, const char* line2 = nullptr);
  void showQR(const char* data);          // null-terminated payload

  // Game-prompt overlays: one big word centered. `Big` is normal
  // (black bg, white text); `Inverted` swaps to draw white bg + black
  // text, which is the "GO!" cue in Reaction Time.
  void showBig(const char* word);
  void showInverted(const char* word);

  void clearOverlay();

  // Force a one-shot blink (otherwise eye library blinks autonomously).
  void blink();

  // Pause/resume autonomous look-around (used during gameplay).
  void setAutoLook(bool on);

  // Aim the eyes at a continuous direction in [-1..1] x [-1..1].
  // (+x = right, +y = up). Each call animates the eyes toward the target
  // over ~200 ms so rapid calls produce a smooth gaze rather than jitter.
  // Disables autoLook for the duration; re-enable with setAutoLook(true)
  // if you stop driving lookAt yourself.
  void lookAt(float x, float y);

  // Low-level: pause the render task (e.g. before talking to I2C from another
  // task). Re-call with false to resume.
  void pause(bool on);

 private:
  static void renderTaskTrampoline(void* arg);
  void renderTaskLoop();

  void applyEyeState(EyeState s);     // touches Face — must run inside render task
  void drawOverlay();                  // ditto

  SemaphoreHandle_t mutex_;
  TaskHandle_t task_;
  volatile bool running_;
  volatile bool paused_;

  // Target state (written by other tasks, read by render task).
  EyeState targetEye_;
  EyeState appliedEye_;
  Overlay  overlay_;
  uint8_t  progressPct_;
  char     text1_[24];
  char     text2_[24];
  char     qrPayload_[128];
  bool     autoLook_;
};

// Singleton — there is only one OLED.
Display& display();

}  // namespace dash

#endif
