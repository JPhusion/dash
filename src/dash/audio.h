// audio.h — I2S amplifier driver for Dash. Plays raw PCM streams from
// LittleFS files in a dedicated FreeRTOS task. Designed for the canonical Dash
// sound format (8 kHz mono 8-bit unsigned), but also accepts 16-bit signed PCM
// raw files for legacy compatibility.
//
// Silent mode: setSilent(true) forces volume to 0 across the I2S DMA without
// changing application logic. Useful during overnight bring-up where the user
// is asleep — every "playSound" call still logs and consumes time as normal.

#ifndef DASH_AUDIO_H
#define DASH_AUDIO_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace dash {

enum class AudioFormat : uint8_t {
  Pcm8kHzMono8 = 0,    // canonical Dash format: 8000 Hz, 1 channel, uint8 unsigned
  Pcm16kHzMono8,
  Pcm22kHzMono8,
  Pcm44kHzMono16,      // legacy 16-bit signed for reference WAVs (raw payload only)
};

class Audio {
 public:
  Audio();

  // Initialise the I2S peripheral and mount LittleFS if not already mounted.
  bool begin();

  // Spawn the playback task. Idempotent.
  void start();
  void stop();

  // Play a sound file from LittleFS (e.g. "/sounds/boot.raw"). If a sound is
  // already playing, this call is rejected unless exclusive=true (in which
  // case the current sound is cooperatively cancelled first).
  bool play(const char* path, AudioFormat fmt = AudioFormat::Pcm8kHzMono8,
            bool exclusive = false);

  // Stop any currently-playing sound. Cooperative — returns once the task has
  // exited its inner write loop (up to ~250 ms).
  void stopPlayback();

  bool isPlaying() const { return playing_; }

  // 0..100. Linear gain applied to each PCM sample before I2S write.
  void setVolume(uint8_t v);
  uint8_t volume() const { return volume_; }

  // Force volume to zero regardless of setVolume() value.
  void setSilent(bool on) { silent_ = on; }
  bool silent() const { return silent_; }

 private:
  static void playbackTaskTrampoline(void* arg);
  void playbackLoop();

  void setI2sClock(uint32_t sampleRate, uint8_t bitsPerSample);
  void writeWithGain(const uint8_t* buf, size_t n, AudioFormat fmt);

  TaskHandle_t task_;
  QueueHandle_t requestQueue_;
  volatile bool running_;
  volatile bool playing_;
  volatile bool cancel_;
  volatile uint8_t volume_;
  volatile bool silent_;
};

Audio& audio();

}  // namespace dash

#endif
