// main.cpp — Dash firmware entry point.
//
// M0 bring-up: print reset/wake reason on boot, then heartbeat heap stats on
// loop. Subsequent milestones layer in IMU, display, audio, touch, power
// management, Wi-Fi AP, sessions, etc.

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "dash/build_info.h"
#include "dash/reset_reason.h"

static uint32_t g_lastHeartbeatMs = 0;

void setup() {
  Serial.begin(115200);
  delay(50);  // settle USB-CDC enumeration on cold boot

  const esp_reset_reason_t reset = esp_reset_reason();
  const esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();

  Serial.println();
  Serial.println(F("================ Dash boot ================"));
  Serial.printf("firmware   : %s\n", dash::kFirmwareVersion);
  Serial.printf("reset      : %s (%d)\n", dash::resetReasonString(reset), reset);
  Serial.printf("wake cause : %s (%d)\n", dash::wakeCauseString(wake), wake);
  Serial.printf("cpu freq   : %u MHz\n", (unsigned)getCpuFrequencyMhz());
  Serial.printf("free heap  : %u bytes (largest block %u)\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  Serial.println(F("==========================================="));
}

void loop() {
  const uint32_t now = millis();
  if (now - g_lastHeartbeatMs >= 10000) {
    g_lastHeartbeatMs = now;
    Serial.printf("[heartbeat] up=%lus  free=%u  largest=%u\n",
                  (unsigned long)(now / 1000),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  }
  delay(50);
}
