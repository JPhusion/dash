#include "dash/portal.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>

#include "dash/audio.h"
#include "dash/build_info.h"
#include "dash/character.h"
#include "dash/display.h"
#include "dash/esp_now_dash.h"
#include "dash/games.h"
#include "dash/imu.h"
#include "dash/log.h"
#include "dash/ota.h"
#include "dash/power.h"
#include "dash/session.h"
#include "dash/settings.h"
#include "dash/sounds.h"
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

Portal::Portal() : lastClientMs_(0), lastDiagEventMs_(0) {
  lastDiagEvent_[0] = '\0';
}

void Portal::recordDiagEvent(const char* name) {
  if (!name) return;
  strncpy(lastDiagEvent_, name, sizeof(lastDiagEvent_) - 1);
  lastDiagEvent_[sizeof(lastDiagEvent_) - 1] = '\0';
  lastDiagEventMs_ = millis();
}

void Portal::begin() {
  WebServer* sv = wifiAp().server();
  if (!sv) {
    log::error(kTag, "no server");
    return;
  }

  // --- Captive-portal probe redirects ---
  //
  // Different OSes expect different responses to detect a captive portal:
  //   iOS / macOS:  GET hotspot-detect.html, expects body == "Success" for
  //                 no-portal. Anything else => trigger the captive sheet.
  //   Android:      GET /generate_204, expects HTTP 204 (no content) for
  //                 no-portal. Anything else => trigger.
  //   Windows:      GET /connecttest.txt or /ncsi.txt, expects specific text.
  //
  // We return a tiny HTML page (not a 302) for all probes. The HTML triggers
  // the captive sheet on every OS we care about and gives the user a tap
  // target if the sheet doesn't auto-render.
  static const char* kCaptiveHtml =
      "<!doctype html><html><head>"
      "<meta charset=utf-8>"
      "<meta http-equiv=refresh content=\"0; url=http://192.168.4.1/\">"
      "<title>Dash</title>"
      "</head><body>"
      "<a href=\"http://192.168.4.1/\">Open Dash</a>"
      "</body></html>";

  static const char* kProbes[] = {
    "/generate_204", "/gen_204", "/hotspot-detect.html",
    "/library/test/success.html", "/connecttest.txt", "/ncsi.txt",
    "/redirect", "/success.txt", "/captiveportal.html", "/canonical.html",
    "/check_network_status.txt", "/fwlink/", "/mobile/status.php",
  };
  for (auto p : kProbes) {
    sv->on(p, HTTP_GET, [this, sv, p]() {
      log::info(kTag, "probe %s host=%s", p, sv->hostHeader().c_str());
      lastClientMs_ = millis();
      sv->send(200, "text/html", kCaptiveHtml);
    });
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
             "\"name\":\"Dash\",\"user_name\":\"%s\",\"ssid\":\"%s\"}",
             kFirmwareVersion, deviceStateName(sm.state()),
             (int)character().mood(),
             faceToString(imu().currentFace()),
             (unsigned long)millis(), (unsigned)power().bootCount(),
             settings().userName().c_str(),
             wifiAp().ssid().c_str());
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
    char buf[360];
    snprintf(buf, sizeof(buf),
             "{\"user_name\":\"%s\",\"volume\":%u,\"sleep_timeout_s\":%u,"
             "\"session_minutes\":%u,\"tap_g\":%.2f,"
             "\"ssid\":\"%s\",\"onboarded\":%s}",
             settings().userName().c_str(),
             settings().audioVolume(),
             settings().sleepTimeoutSec(),
             settings().sessionLengthMin(),
             settings().tapSensitivityG(),
             wifiAp().ssid().c_str(),
             settings().onboarded() ? "true" : "false");
    sv->send(200, "application/json", buf);
  });

  sv->on("/api/config", HTTP_POST, [this, sv]() {
    lastClientMs_ = millis();
    if (!sv->hasArg("plain")) { sv->send(400, "text/plain", "no body"); return; }
    JsonDocument doc;
    auto err = deserializeJson(doc, sv->arg("plain"));
    if (err) { sv->send(400, "text/plain", "bad json"); return; }
    if (doc["user_name"].is<const char*>()) {
      settings().setUserName(doc["user_name"].as<String>());
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
    if (doc["tap_g"].is<float>() || doc["tap_g"].is<double>()) {
      float g = doc["tap_g"].as<float>();
      settings().setTapSensitivityG(g);
      imu().setTapThreshold(settings().tapSensitivityG());
      log::info(kTag, "tap sensitivity -> %.2fg", settings().tapSensitivityG());
    }
    // Home Wi-Fi credentials piped through here too so the settings card can
    // edit them after onboarding. Empty string is treated as "no change" so
    // the user doesn't accidentally clear creds by leaving the field blank.
    if (doc["home_ssid"].is<const char*>()) {
      String s = doc["home_ssid"].as<String>();
      if (s.length() > 0) settings().setHomeWifiSsid(s);
    }
    if (doc["home_password"].is<const char*>()) {
      String p = doc["home_password"].as<String>();
      if (p.length() > 0) settings().setHomeWifiPassword(p);
    }
    log::info(kTag, "config updated");
    sv->send(200, "application/json", "{\"ok\":true}");
  });

  // --- /api/session ---
  sv->on("/api/session", HTTP_GET, [this, sv]() {
    lastClientMs_ = millis();
    auto snap = session().snapshot();
    char buf[320];
    uint32_t elapsed_s = 0;
    if (snap.active) {
      uint32_t elapsedMs = (millis() - snap.startedAtMs) - snap.pausedMs;
      elapsed_s = elapsedMs / 1000UL;
    }
    // Escape user-supplied label minimally (replace " with ' for json).
    char safeLabel[40];
    size_t k = 0;
    for (size_t i = 0; snap.label[i] && k < sizeof(safeLabel) - 1; i++) {
      char c = snap.label[i];
      if (c == '"' || c == '\\' || c == '\n' || c == '\r') c = ' ';
      safeLabel[k++] = c;
    }
    safeLabel[k] = '\0';
    snprintf(buf, sizeof(buf),
             "{\"active\":%s,\"state\":%d,\"elapsed_s\":%u,"
             "\"total_s\":%u,\"distractions\":%u,\"label\":\"%s\"}",
             snap.active ? "true" : "false", (int)snap.state,
             (unsigned)elapsed_s,
             (unsigned)snap.targetMin * 60u,
             (unsigned)snap.distractions,
             safeLabel);
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
      const char* label = doc["label"].is<const char*>() ? doc["label"].as<const char*>() : nullptr;
      bool ok = session().start(minutes, label);
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

  // --- /api/onboarding ---
  sv->on("/api/onboarding", HTTP_GET, [this, sv]() {
    lastClientMs_ = millis();
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"onboarded\":%s,\"user_name\":\"%s\",\"home_wifi_set\":%s}",
             settings().onboarded() ? "true" : "false",
             settings().userName().c_str(),
             settings().homeWifiSsid().length() > 0 ? "true" : "false");
    sv->send(200, "application/json", buf);
  });

  sv->on("/api/onboarding", HTTP_POST, [this, sv]() {
    lastClientMs_ = millis();
    if (!sv->hasArg("plain")) { sv->send(400, "text/plain", "no body"); return; }
    JsonDocument doc;
    auto err = deserializeJson(doc, sv->arg("plain"));
    if (err) { sv->send(400, "text/plain", "bad json"); return; }
    if (doc["user_name"].is<const char*>()) {
      settings().setUserName(doc["user_name"].as<String>());
    }
    if (doc["home_ssid"].is<const char*>()) {
      settings().setHomeWifiSsid(doc["home_ssid"].as<String>());
    }
    if (doc["home_password"].is<const char*>()) {
      settings().setHomeWifiPassword(doc["home_password"].as<String>());
    }
    if (doc["complete"].is<bool>() && doc["complete"].as<bool>()) {
      settings().setOnboarded(true);
      stateMachine().transitionTo(DeviceState::Idle);
      character().setMood(Mood::Neutral);
      // Clear the "Connect to: Dash-XXXX" text overlay we showed during
      // onboarding so the eyes return on the OLED.
      display().clearOverlay();
      character().react(EyeState::Happy, 1500);
      log::info(kTag, "onboarding complete");
    }
    if (doc["reset"].is<bool>() && doc["reset"].as<bool>()) {
      // Replay the tutorial from settings — clears the onboarded flag so the
      // app.js redirect sends the user back to /onboarding.html.
      settings().setOnboarded(false);
      stateMachine().transitionTo(DeviceState::Onboarding);
      character().setMood(Mood::Listening);
      display().showText("Connect to:", wifiAp().ssid().c_str());
      log::info(kTag, "onboarding reset by user");
    }
    sv->send(200, "application/json", "{\"ok\":true}");
  });

  // --- /api/stats DELETE (reset) ---
  sv->on("/api/stats", HTTP_DELETE, [this, sv]() {
    lastClientMs_ = millis();
    LittleFS.remove("/stats/sessions.ndjson");
    log::info(kTag, "stats reset by user");
    sv->send(200, "application/json", "{\"ok\":true}");
  });

  // --- /api/factory-reset ---
  sv->on("/api/factory-reset", HTTP_POST, [this, sv]() {
    lastClientMs_ = millis();
    log::info(kTag, "factory reset requested");
    // Wipe NVS namespace + stats log + flag onboarding back to false.
    Preferences p;
    p.begin("dash.cfg", false);
    p.clear();
    p.end();
    p.begin("dash.imu", false);
    p.clear();
    p.end();
    LittleFS.remove("/stats/sessions.ndjson");
    sv->send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    delay(500);
    ESP.restart();
  });

  // --- /api/diag-event (last imu/touch event seen, used by diag.html) ---
  sv->on("/api/diag-event", HTTP_GET, [this, sv]() {
    lastClientMs_ = millis();
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"last_event\":\"%s\",\"age_ms\":%lu}",
             lastDiagEvent_,
             (unsigned long)(millis() - lastDiagEventMs_));
    sv->send(200, "application/json", buf);
  });
  // POST clears the stored last_event so the diag page can wait for fresh
  // input at each step boundary (otherwise a tap from before the step
  // started would match immediately).
  sv->on("/api/diag-event", HTTP_POST, [this, sv]() {
    lastClientMs_ = millis();
    lastDiagEvent_[0] = '\0';
    lastDiagEventMs_ = millis();
    sv->send(200, "application/json", "{\"ok\":true}");
  });

  // diag.html toggles this to suppress main's normal tap / shake / flick
  // reactions so they don't interfere with what the diagnostic is
  // measuring. Auto-clears after 60s of no refresh.
  sv->on("/api/diag-mode", HTTP_POST, [this, sv]() {
    lastClientMs_ = millis();
    if (!sv->hasArg("plain")) { sv->send(400, "text/plain", "no body"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, sv->arg("plain"))) {
      sv->send(400, "text/plain", "bad json"); return;
    }
    setDiagMode(doc["active"].as<bool>());
    log::info(kTag, "diag mode %s", diagModeActive() ? "ON" : "OFF");
    sv->send(200, "application/json", "{\"ok\":true}");
  });

  // --- /api/test-tone (volume preview, diagnostic speaker test) ---
  sv->on("/api/test-tone", HTTP_POST, [this, sv]() {
    lastClientMs_ = millis();
    audio().setVolume(settings().audioVolume());
    audio().play(sounds::kTestTone, dash::AudioFormat::Pcm8kHzMono8, true);
    sv->send(200, "application/json", "{\"ok\":true}");
  });

  // --- /api/easter-egg (konami / fun extras) ---
  sv->on("/api/easter-egg", HTTP_POST, [this, sv]() {
    lastClientMs_ = millis();
    log::info(kTag, "konami code");
    character().react(EyeState::Heart, 2500);
    sv->send(200, "application/json", "{\"ok\":true}");
  });

  // --- /api/stats ---
  sv->on("/api/stats", HTTP_GET, [this, sv]() {
    lastClientMs_ = millis();
    auto s = stats().summary();
    char buf[2048];
    int n = snprintf(buf, sizeof(buf),
        "{\"total_sessions\":%u,\"completed_sessions\":%u,"
        "\"total_focused_sec\":%u,\"total_distractions\":%u,"
        "\"best_single_sec\":%u,\"streak_days\":%u,\"recent\":",
        s.totalSessions, s.completedSessions,
        (unsigned)s.totalFocusedSec, s.totalDistractions,
        (unsigned)s.bestSingleSec, s.streakDays);
    n += stats().recentSessionsJson(buf + n, sizeof(buf) - n - 2, 10);
    snprintf(buf + n, sizeof(buf) - n, "}");
    sv->send(200, "application/json", buf);
  });

  // --- /api/group ---
  sv->on("/api/group", HTTP_GET, [this, sv]() {
    lastClientMs_ = millis();
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "{\"running\":%s,\"peers\":[",
                     espNow().running() ? "true" : "false");
    for (uint8_t i = 0; i < espNow().peerCount(); i++) {
      n += snprintf(buf + n, sizeof(buf) - n,
                    "%s{\"id\":\"%08x\",\"last_seen_ms\":%lu}",
                    i ? "," : "", (unsigned)espNow().peer(i).deviceId,
                    (unsigned long)(millis() - espNow().peer(i).lastSeenMs));
    }
    snprintf(buf + n, sizeof(buf) - n, "]}");
    sv->send(200, "application/json", buf);
  });

  sv->on("/api/group", HTTP_POST, [this, sv]() {
    lastClientMs_ = millis();
    if (!sv->hasArg("plain")) { sv->send(400, "text/plain", "no body"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, sv->arg("plain"))) {
      sv->send(400, "text/plain", "bad json"); return;
    }
    String action = doc["action"].as<String>();
    if (action == "start") {
      espNow().begin();
      espNow().sendPresence();
      sv->send(200, "application/json", "{\"ok\":true}");
    } else if (action == "stop") {
      espNow().stop();
      sv->send(200, "application/json", "{\"ok\":true}");
    } else if (action == "invite") {
      espNow().sendRoomInvite();
      sv->send(200, "application/json", "{\"ok\":true}");
    } else {
      sv->send(400, "text/plain", "unknown action");
    }
  });

  // --- /api/game ---
  sv->on("/api/game", HTTP_POST, [this, sv]() {
    lastClientMs_ = millis();
    if (!sv->hasArg("plain")) { sv->send(400, "text/plain", "no body"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, sv->arg("plain"))) {
      sv->send(400, "text/plain", "bad json"); return;
    }
    String action = doc["action"].as<String>();
    if (action == "start") {
      String which = doc["game"].as<String>();
      if (which == "reaction")  games().startGame(GameId::Reaction);
      else if (which == "bopit") games().startGame(GameId::BopIt);
      else { sv->send(400, "text/plain", "unknown game"); return; }
      sv->send(200, "application/json", "{\"ok\":true}");
    } else if (action == "stop") {
      games().stopGame();
      sv->send(200, "application/json", "{\"ok\":true}");
    } else {
      sv->send(400, "text/plain", "unknown action");
    }
  });

  sv->on("/api/game", HTTP_GET, [this, sv]() {
    lastClientMs_ = millis();
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"current\":%d,\"last_score\":%u}",
             (int)games().current(), (unsigned)games().lastScore());
    sv->send(200, "application/json", buf);
  });

  // --- /api/ota/check ---
  sv->on("/api/ota/check", HTTP_POST, [this, sv]() {
    lastClientMs_ = millis();
    log::info(kTag, "manual OTA check");
    sv->send(202, "application/json", "{\"started\":true}");
    // Run synchronously after the response (server task is on core 0).
    // checkAndApply() will reboot on success; otherwise we just log.
    OtaResult r = ota().checkAndApply();
    log::info(kTag, "OTA result: %s", otaResultString(r));
    // Bring the AP back up if STA mode left it down.
    if (!wifiAp().running()) {
      wifiAp().start();
      portal().begin();
    }
  });

  // --- Static file fallback / root ---
  sv->onNotFound([this, sv]() {
    lastClientMs_ = millis();
    String p = sv->uri();
    String host = sv->hostHeader();
    log::info(kTag, "404? uri=%s host=%s", p.c_str(), host.c_str());
    // Any request hitting an external hostname (i.e. a captive-portal probe
    // from the phone OS) gets the captive HTML — sometimes phones probe
    // unusual paths we haven't enumerated.
    if (host.length() > 0 &&
        host != wifiAp().apIp().toString() &&
        host != wifiAp().ssid() + ".local" &&
        host != "dash.local" &&
        host != "dash") {
      sv->send(200, "text/html", kCaptiveHtml);
      return;
    }
    serveFile(*sv, p);
  });

  sv->on("/", HTTP_GET, [this, sv]() {
    lastClientMs_ = millis();
    log::info(kTag, "GET / host=%s", sv->hostHeader().c_str());
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
