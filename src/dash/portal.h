// portal.h — HTTP request handlers for the captive portal.
//
// All routes are registered via WifiAp::server(). The web frontend lives on
// LittleFS at /web/* and is served verbatim; only the /api/* endpoints are
// dynamic.
//
// API surface (used by phone JS):
//   GET  /api/status         current device state, version, mood
//   POST /api/time-sync      body: {unix_ms, tz_min}
//   GET  /api/config         current settings JSON
//   POST /api/config         partial settings update (subset of keys)
//   POST /api/session        body: {action: start|pause|resume|stop, minutes?}
//   GET  /api/session        current session snapshot
//   GET  /api/stats          rolling stats summary
//
// Captive-portal probe paths (generate_204, hotspot-detect.html, etc.) all
// 302 to "/" so phones reliably surface the page.

#ifndef DASH_PORTAL_H
#define DASH_PORTAL_H

#include <Arduino.h>

namespace dash {

class Portal {
 public:
  Portal();
  void begin();           // registers routes against WifiAp's WebServer
  bool isClientConnected() const;

  // Diagnostic recorder — fed by IMU/Touch listeners in main. The /diag.html
  // walkthrough polls /api/diag-event to detect peripheral activity step
  // by step.
  void recordDiagEvent(const char* name);
  const char* lastDiagEvent() const { return lastDiagEvent_; }
  uint32_t lastDiagEventMs() const { return lastDiagEventMs_; }

 private:
  uint32_t lastClientMs_;
  char lastDiagEvent_[24];
  uint32_t lastDiagEventMs_;
};

Portal& portal();

}  // namespace dash

#endif
