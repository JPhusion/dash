#include "dash/portal.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WebServer.h>

#include "dash/build_info.h"
#include "dash/character.h"
#include "dash/imu.h"
#include "dash/log.h"
#include "dash/power.h"
#include "dash/session.h"
#include "dash/settings.h"
#include "dash/state_machine.h"
#include "dash/stats.h"
#include "dash/wifi_ap.h"

namespace dash {

namespace {
constexpr const char* kTag = "Portal";
Portal* g_singleton = nullptr;

const char* contentTypeForPath(const String& p) {
  if (p.endsWith(".html"))  return "text/html";
  if (p.endsWith(".css"))   return "text/css";
  if (p.endsWith(".js"))    return "application/javascript";
  if (p.endsWith(".json"))  return "application/json";
  if (p.endsWith(".svg"))   return "image/svg+xml";
  if (p.endsWith(".png"))   return "image/png";
  if (p.endsWith(".ico"))   return "image/x-icon";
  return "application/octet-stream";
}

void redirectToRoot(WebServer& s) {
  s.sendHeader("Location", "/", true);
  s.send(302, "text/plain", "");
}

void serveFile(WebServer& s, const String& path) {
  String fsPath = String("/web") + path;
  if (fsPath.endsWith("/")) fsPath += "index.html";
  if (!LittleFS.exists(fsPath)) {
    s.send(404, "text/plain", "not found");
    return;
  }
  File f = LittleFS.open(fsPath, "r");
  if (!f) { s.send(500, "text/plain", "open failed"); return; }
  s.streamFile(f, contentTypeForPath(fsPath));
  f.close();
}

}  // namespace

Portal::Portal() : lastClientMs_(0) {}

void Portal::begin() {
  WebServer* sv = wifiAp().server();
  if (!sv) {
    log::error(kTag, "no server");
    return;
  }

  // --- Captive-portal probe redirects ---
  static const char* kProbes[] = {
    "/generate_204", "/gen_204", "/hotspot-detect.html",
    "/library/test/success.html", "/connecttest.txt", "/ncsi.txt",
    "/redirect", "/success.txt",
  };
  for (auto p : kProbes) {
    sv->on(p, HTTP_GET, [sv]() { redirectToRoot(*sv); });
  }

  // --- /api/status ---
  sv->on("/api/status", HTTP_GET, [this, sv]() {
    lastClientMs_ = millis();
    char buf[256];
    auto& sm = stateMachine();
    (void)imu().latest();   // touch the IMU so its sample task hasn't deadlocked
    snprintf(buf, sizeof(buf),
             "{\"firmware\":\"%s\",\"state\":\"%s\",\"mood\":%d,"
             "\"face\":\"%s\",\"uptime_ms\":%lu,\"boot_count\":%u,"
             "\"name\":\"%s\"}",
             kFirmwareVersion, deviceStateName(sm.state()),
             (int)character().mood(),
             faceToString(imu().currentFace()),
             (unsigned long)millis(), (unsigned)power().bootCount(),
             settings().deviceName().c_str());
    sv->send(200, "application/json", buf);
  });

  // --- /api/time-sync ---
  sv->on("/api/time-sync", HTTP_POST, [this, sv]() {
    lastClientMs_ = millis();
    if (!sv->hasArg("plain")) { sv->send(400, "text/plain", "no body"); return; }
    JsonDocument doc;
    auto err = deserializeJson(doc, sv->arg("plain"));
    if (err) { sv->send(400, "text/plain", "bad json"); return; }
    uint64_t unix_ms = doc["unix_ms"] | 0ULL;
    int16_t tz_min   = doc["tz_min"]  | 0;
    if (unix_ms > 0) {
      uint32_t unix_s = (uint32_t)(unix_ms / 1000ULL);
      settings().setLastUnix(unix_s);
      settings().setTzOffsetMin(tz_min);
      log::info(kTag, "time sync: unix=%u tz=%d", unix_s, tz_min);
    }
    sv->send(200, "application/json", "{\"ok\":true}");
  });

  // --- /api/config GET/POST ---
  sv->on("/api/config", HTTP_GET, [this, sv]() {
    lastClientMs_ = millis();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"name\":\"%s\",\"volume\":%u,\"sleep_timeout_s\":%u,"
             "\"session_minutes\":%u,\"onboarded\":%s}",
             settings().deviceName().c_str(),
             settings().audioVolume(),
             settings().sleepTimeoutSec(),
             settings().sessionLengthMin(),
             settings().onboarded() ? "true" : "false");
    sv->send(200, "application/json", buf);
  });

  sv->on("/api/config", HTTP_POST, [this, sv]() {
    lastClientMs_ = millis();
    if (!sv->hasArg("plain")) { sv->send(400, "text/plain", "no body"); return; }
    JsonDocument doc;
    auto err = deserializeJson(doc, sv->arg("plain"));
    if (err) { sv->send(400, "text/plain", "bad json"); return; }
    if (doc["name"].is<const char*>()) {
      settings().setDeviceName(doc["name"].as<String>());
    }
    if (doc["volume"].is<unsigned>()) {
      settings().setAudioVolume((uint8_t)(doc["volume"].as<unsigned>()));
    }
    if (doc["sleep_timeout_s"].is<unsigned>()) {
      settings().setSleepTimeoutSec((uint16_t)(doc["sleep_timeout_s"].as<unsigned>()));
    }
    if (doc["session_minutes"].is<unsigned>()) {
      settings().setSessionLengthMin((uint16_t)(doc["session_minutes"].as<unsigned>()));
    }
    log::info(kTag, "config updated");
    sv->send(200, "application/json", "{\"ok\":true}");
  });

  // --- /api/session ---
  sv->on("/api/session", HTTP_GET, [this, sv]() {
    lastClientMs_ = millis();
    auto snap = session().snapshot();
    char buf[256];
    uint32_t elapsed_s = 0;
    if (snap.active) {
      uint32_t elapsedMs = (millis() - snap.startedAtMs) - snap.pausedMs;
      elapsed_s = elapsedMs / 1000UL;
    }
    snprintf(buf, sizeof(buf),
             "{\"active\":%s,\"state\":%d,\"elapsed_s\":%u,"
             "\"total_s\":%u,\"distractions\":%u}",
             snap.active ? "true" : "false", (int)snap.state,
             (unsigned)elapsed_s,
             (unsigned)snap.targetMin * 60u,
             (unsigned)snap.distractions);
    sv->send(200, "application/json", buf);
  });

  sv->on("/api/session", HTTP_POST, [this, sv]() {
    lastClientMs_ = millis();
    if (!sv->hasArg("plain")) { sv->send(400, "text/plain", "no body"); return; }
    JsonDocument doc;
    auto err = deserializeJson(doc, sv->arg("plain"));
    if (err) { sv->send(400, "text/plain", "bad json"); return; }
    String action = doc["action"].as<String>();
    if (action == "start") {
      uint16_t minutes = doc["minutes"].is<unsigned>()
                           ? (uint16_t)doc["minutes"].as<unsigned>()
                           : settings().sessionLengthMin();
      bool ok = session().start(minutes);
      sv->send(ok ? 200 : 409, "application/json",
               ok ? "{\"ok\":true}" : "{\"error\":\"already running\"}");
    } else if (action == "pause") {
      session().pause();
      sv->send(200, "application/json", "{\"ok\":true}");
    } else if (action == "resume") {
      session().resume();
      sv->send(200, "application/json", "{\"ok\":true}");
    } else if (action == "stop") {
      session().stop(false);
      sv->send(200, "application/json", "{\"ok\":true}");
    } else {
      sv->send(400, "text/plain", "unknown action");
    }
  });

  // --- /api/stats ---
  sv->on("/api/stats", HTTP_GET, [this, sv]() {
    lastClientMs_ = millis();
    auto s = stats().summary();
    char buf[2048];
    int n = snprintf(buf, sizeof(buf),
        "{\"total_sessions\":%u,\"completed_sessions\":%u,"
        "\"total_focused_sec\":%u,\"total_distractions\":%u,"
        "\"best_single_sec\":%u,\"recent\":",
        s.totalSessions, s.completedSessions,
        (unsigned)s.totalFocusedSec, s.totalDistractions,
        (unsigned)s.bestSingleSec);
    n += stats().recentSessionsJson(buf + n, sizeof(buf) - n - 2, 10);
    snprintf(buf + n, sizeof(buf) - n, "}");
    sv->send(200, "application/json", buf);
  });

  // --- Static file fallback / root ---
  sv->onNotFound([this, sv]() {
    lastClientMs_ = millis();
    String p = sv->uri();
    // Treat unknown hostnames (captive portal probes) as redirect to /.
    if (sv->hostHeader() != wifiAp().apIp().toString() &&
        sv->hostHeader() != wifiAp().ssid() + ".local" &&
        sv->hostHeader() != "dash.local") {
      redirectToRoot(*sv);
      return;
    }
    serveFile(*sv, p);
  });

  sv->on("/", HTTP_GET, [this, sv]() {
    lastClientMs_ = millis();
    serveFile(*sv, "/index.html");
  });

  log::info(kTag, "routes registered");
}

bool Portal::isClientConnected() const {
  return (millis() - lastClientMs_) < 30000;
}

Portal& portal() {
  if (!g_singleton) g_singleton = new Portal();
  return *g_singleton;
}

}  // namespace dash
