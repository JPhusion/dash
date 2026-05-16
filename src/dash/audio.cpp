#include "dash/audio.h"

#include <LittleFS.h>
#include <driver/i2s.h>

#include "dash/log.h"
#include "dash/pins.h"

namespace dash {

namespace {
constexpr const char* kTag = "Audio";
constexpr i2s_port_t kI2sPort = I2S_NUM_0;

struct PlayRequest {
  char path[64];
  AudioFormat fmt;
};

Audio* g_singleton = nullptr;

bool littlefsMounted = false;
bool ensureFs() {
  if (littlefsMounted) return true;
  if (!LittleFS.begin(true)) {
    log::error(kTag, "LittleFS mount failed");
    return false;
  }
  littlefsMounted = true;
  return true;
}

uint32_t formatSampleRate(AudioFormat f) {
  switch (f) {
    case AudioFormat::Pcm8kHzMono8:   return 8000;
    case AudioFormat::Pcm16kHzMono8:  return 16000;
    case AudioFormat::Pcm22kHzMono8:  return 22050;
    case AudioFormat::Pcm44kHzMono16: return 44100;
  }
  return 8000;
}

bool formatIs16Bit(AudioFormat f) {
  return f == AudioFormat::Pcm44kHzMono16;
}
}  // namespace

Audio::Audio()
    : task_(nullptr),
      requestQueue_(xQueueCreate(2, sizeof(PlayRequest))),
      running_(false),
      playing_(false),
      cancel_(false),
      volume_(60),     // medium default
      silent_(false) {}

bool Audio::begin() {
  if (!ensureFs()) return false;

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = 8000;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = true;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = pins::I2S_BCLK;
  pins.ws_io_num = pins::I2S_LRCLK;
  pins.data_out_num = pins::I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  esp_err_t e = i2s_driver_install(kI2sPort, &cfg, 0, nullptr);
  if (e != ESP_OK) {
    log::error(kTag, "i2s_driver_install: %d", e);
    return false;
  }
  i2s_set_pin(kI2sPort, &pins);
  i2s_zero_dma_buffer(kI2sPort);

  log::info(kTag, "i2s up @ 8kHz default, volume=%u silent=%d", volume_, (int)silent_);
  return true;
}

void Audio::start() {
  if (running_) return;
  running_ = true;
  xTaskCreatePinnedToCore(&Audio::playbackTaskTrampoline, "audio", 6144, this,
                          1, &task_, 1 /*core 1*/);
}

void Audio::stop() {
  running_ = false;
  cancel_ = true;
}

void Audio::setVolume(uint8_t v) {
  if (v > 100) v = 100;
  volume_ = v;
}

void Audio::setI2sClock(uint32_t sampleRate, uint8_t bps) {
  i2s_set_clk(kI2sPort, sampleRate,
              bps == 16 ? I2S_BITS_PER_SAMPLE_16BIT : I2S_BITS_PER_SAMPLE_16BIT,
              I2S_CHANNEL_STEREO);
}

bool Audio::play(const char* path, AudioFormat fmt, bool exclusive) {
  if (!path) return false;
  if (playing_ && !exclusive) {
    log::debug(kTag, "play(%s) rejected — busy", path);
    return false;
  }
  if (exclusive) stopPlayback();

  PlayRequest r{};
  strncpy(r.path, path, sizeof(r.path) - 1);
  r.fmt = fmt;
  if (xQueueSend(requestQueue_, &r, pdMS_TO_TICKS(50)) != pdTRUE) {
    log::warn(kTag, "play queue full");
    return false;
  }
  return true;
}

void Audio::stopPlayback() {
  cancel_ = true;
  for (int i = 0; i < 50 && playing_; i++) vTaskDelay(pdMS_TO_TICKS(5));
  cancel_ = false;
}

void Audio::playbackTaskTrampoline(void* arg) {
  static_cast<Audio*>(arg)->playbackLoop();
}

void Audio::writeWithGain(const uint8_t* buf, size_t n, AudioFormat fmt) {
  // Convert input format to stereo 16-bit and apply gain.
  uint8_t gain = silent_ ? 0 : volume_;
  static int16_t outBuf[512];  // 256 stereo frames at a time

  if (formatIs16Bit(fmt)) {
    const int16_t* in = reinterpret_cast<const int16_t*>(buf);
    size_t samples = n / 2;
    while (samples) {
      size_t batch = samples > 256 ? 256 : samples;
      for (size_t i = 0; i < batch; i++) {
        int32_t v = (int32_t)in[i] * gain / 100;
        outBuf[2*i + 0] = (int16_t)v;
        outBuf[2*i + 1] = (int16_t)v;
      }
      size_t written = 0;
      i2s_write(kI2sPort, outBuf, batch * 4, &written, portMAX_DELAY);
      in += batch;
      samples -= batch;
      if (cancel_) return;
      taskYIELD();
    }
  } else {
    // 8-bit unsigned PCM: center at 128 and scale to int16.
    while (n) {
      size_t batch = n > 256 ? 256 : n;
      for (size_t i = 0; i < batch; i++) {
        int16_t v = ((int16_t)buf[i] - 128) << 8;  // -32768..32512
        int32_t scaled = (int32_t)v * gain / 100;
        outBuf[2*i + 0] = (int16_t)scaled;
        outBuf[2*i + 1] = (int16_t)scaled;
      }
      size_t written = 0;
      i2s_write(kI2sPort, outBuf, batch * 4, &written, portMAX_DELAY);
      buf += batch;
      n -= batch;
      if (cancel_) return;
      taskYIELD();
    }
  }
}

void Audio::playbackLoop() {
  PlayRequest r;
  while (running_) {
    if (xQueueReceive(requestQueue_, &r, pdMS_TO_TICKS(100)) != pdTRUE) continue;
    if (cancel_) { cancel_ = false; continue; }

    File f = LittleFS.open(r.path, "r");
    if (!f) {
      log::warn(kTag, "open failed: %s", r.path);
      continue;
    }
    playing_ = true;
    uint32_t rate = formatSampleRate(r.fmt);
    setI2sClock(rate, formatIs16Bit(r.fmt) ? 16 : 8);
    log::info(kTag, "play %s (%u Hz, %s)%s", r.path, (unsigned)rate,
              formatIs16Bit(r.fmt) ? "s16" : "u8",
              silent_ ? " [silent]" : "");

    static uint8_t buf[1024];
    while (!cancel_ && f.available()) {
      size_t got = f.read(buf, sizeof(buf));
      if (!got) break;
      writeWithGain(buf, got, r.fmt);
    }
    f.close();
    playing_ = false;
    cancel_ = false;
  }
  task_ = nullptr;
  vTaskDelete(nullptr);
}

Audio& audio() {
  if (!g_singleton) g_singleton = new Audio();
  return *g_singleton;
}

}  // namespace dash
