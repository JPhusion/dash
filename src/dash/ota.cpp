#include "dash/ota.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#include <mbedtls/sha256.h>

#include <ESPmDNS.h>

#include "dash/audio.h"
#include "dash/build_info.h"
#include "dash/display.h"
#include "dash/idle_manager.h"
#include "dash/imu.h"
#include "dash/log.h"
#include "dash/power.h"
#include "dash/settings.h"
#include "dash/wifi_ap.h"

namespace dash {

namespace {
constexpr const char* kTag = "Ota";
constexpr const char* kReleasesUrl =
    "https://api.github.com/repos/JPhusion/dash/releases/latest";
Ota* g_singleton = nullptr;

bool parseSemver(const String& s, uint16_t out[3]) {
  String t = s;
  if (t.startsWith("v")) t = t.substring(1);
  int a = t.indexOf('.');
  int b = t.indexOf('.', a + 1);
  if (a <= 0 || b <= 0) return false;
  out[0] = (uint16_t)t.substring(0, a).toInt();
  out[1] = (uint16_t)t.substring(a + 1, b).toInt();
  out[2] = (uint16_t)t.substring(b + 1).toInt();
  return true;
}

bool isNewer(const String& remote, const String& local) {
  uint16_t r[3], l[3];
  if (!parseSemver(remote, r) || !parseSemver(local, l)) return false;
  for (int i = 0; i < 3; i++) {
    if (r[i] > l[i]) return true;
    if (r[i] < l[i]) return false;
  }
  return false;
}
}  // namespace

const char* otaResultString(OtaResult r) {
  switch (r) {
    case OtaResult::UpToDate:       return "up-to-date";
    case OtaResult::Updated:        return "updated";
    case OtaResult::NoCredentials:  return "no-credentials";
    case OtaResult::ConnectFailed:  return "connect-failed";
    case OtaResult::CheckFailed:    return "check-failed";
    case OtaResult::DownloadFailed: return "download-failed";
    case OtaResult::HashMismatch:   return "hash-mismatch";
    case OtaResult::WriteFailed:    return "write-failed";
  }
  return "?";
}

Ota::Ota() {}

bool Ota::ensureStation() {
  String ssid = settings().homeWifiSsid();
  String pass = settings().homeWifiPassword();
  if (ssid.length() == 0) return false;

  // Tear AP + mDNS down so we can become STA. mDNS was advertising on
  // the AP interface; if it stays running it tries to multicast on the
  // gone interface and the WiFi stack can spuriously disconnect.
  MDNS.end();
  if (wifiAp().running()) wifiAp().stop();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                  // disable WiFi sleep up-front
  WiFi.begin(ssid.c_str(), pass.c_str());
  log::info(kTag, "connecting to home WiFi %s", ssid.c_str());

  uint32_t deadline = millis() + 20000;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) {
    log::warn(kTag, "home WiFi connect timeout");
    return false;
  }
  // Re-assert PS_NONE after association — IDF resets power-save state
  // during association so a pre-begin call doesn't stick. Without this
  // the AP times out our association mid-TLS handshake (ASSOC_LEAVE
  // reason 8 ~1-2 s after the IP is assigned).
  esp_wifi_set_ps(WIFI_PS_NONE);
  // 500 ms settle window — DHCP/EAPOL handshake races finish, and TLS
  // doesn't immediately compete with link-layer chatter.
  delay(500);
  log::info(kTag, "home WiFi connected, IP=%s rssi=%d ps=off heap=%u",
            WiFi.localIP().toString().c_str(), WiFi.RSSI(),
            (unsigned)ESP.getFreeHeap());
  return true;
}

void Ota::teardownStation() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool Ota::fetchLatestTag(String& tagOut, String& assetUrl) {
  // Up to 3 attempts at the HTTPS request, with a short delay between
  // them. Most home routers will drop a STA that's silent during the
  // TLS handshake (reason-8 ASSOC_LEAVE); on retry the cube re-associates
  // and the second/third attempt usually goes through.
  int code = 0;
  String body;
  for (int attempt = 1; attempt <= 3; ++attempt) {
    if (WiFi.status() != WL_CONNECTED) {
      log::warn(kTag, "WiFi dropped before HTTP — reconnecting (attempt %d)", attempt);
      WiFi.reconnect();
      uint32_t deadline = millis() + 10000;
      while (WiFi.status() != WL_CONNECTED && millis() < deadline) delay(150);
      if (WiFi.status() != WL_CONNECTED) {
        log::warn(kTag, "reconnect failed");
        continue;
      }
      esp_wifi_set_ps(WIFI_PS_NONE);
      delay(300);
    }
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15);                       // seconds; default is too short
    HTTPClient http;
    http.setUserAgent("dash-firmware");
    http.setReuse(false);
    http.setTimeout(15000);                      // ms
    log::info(kTag, "GET %s heap=%u (attempt %d)",
              kReleasesUrl, (unsigned)ESP.getFreeHeap(), attempt);
    if (!http.begin(client, kReleasesUrl)) {
      log::warn(kTag, "http.begin failed");
      delay(800);
      continue;
    }
    code = http.GET();
    log::info(kTag, "HTTP %d", code);
    if (code == 200) {
      body = http.getString();
      http.end();
      log::info(kTag, "body len=%u", (unsigned)body.length());
      // A truncated body (~few hundred bytes when AP drops mid-transfer)
      // means the headers arrived but the JSON didn't — retry.
      if (body.length() < 500) {
        log::warn(kTag, "body too short, retrying");
        code = 0;
        delay(800);
        continue;
      }
      break;
    }
    http.end();
    delay(800);
  }
  if (code != 200 || body.length() < 500) {
    log::warn(kTag, "GitHub releases final HTTP %d len=%u",
              code, (unsigned)body.length());
    return false;
  }
  // body already captured + http already ended inside the retry loop.

  // Very light parse — just look for "tag_name" and the first firmware.bin asset URL.
  int t = body.indexOf("\"tag_name\":\"");
  if (t < 0) { log::warn(kTag, "parse: no tag_name (head=%s)", body.substring(0, 80).c_str()); return false; }
  int e = body.indexOf("\"", t + 12);
  if (e < 0) { log::warn(kTag, "parse: tag_name unterminated"); return false; }
  tagOut = body.substring(t + 12, e);
  log::info(kTag, "parse: tag_name=%s", tagOut.c_str());

  // GitHub asset JSON puts browser_download_url AFTER the "name" field
  // — there's only one asset per release in our case, so we search
  // forward from the start of the body for the first occurrence.
  // Sanity-check that the URL ends in firmware.bin so we don't pick up
  // a stray URL pattern from elsewhere.
  int urlStart = body.indexOf("\"browser_download_url\":\"");
  if (urlStart < 0) {
    log::warn(kTag, "parse: no browser_download_url in body (len=%u)",
              (unsigned)body.length());
    return false;
  }
  urlStart += 24;
  int urlEnd = body.indexOf("\"", urlStart);
  if (urlEnd < 0) { log::warn(kTag, "parse: download_url unterminated"); return false; }
  assetUrl = body.substring(urlStart, urlEnd);
  if (!assetUrl.endsWith("firmware.bin")) {
    log::warn(kTag, "parse: asset_url=%s doesn't end in firmware.bin", assetUrl.c_str());
    return false;
  }
  log::info(kTag, "parse: asset_url=%s", assetUrl.c_str());
  return true;
}

bool Ota::downloadAndFlash(const String& url, const String& expectedHash) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(30);
  HTTPClient http;
  http.setUserAgent("dash-firmware");
  http.setTimeout(30000);
  // GitHub's browser_download_url 302-redirects to objects.githubusercontent.com.
  // Without follow-redirects, http.GET() returns 302 and the download fails.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return false;
  static const char* kCollectHeaders[] = {"Content-Length"};
  http.collectHeaders(kCollectHeaders, 1);
  int code = http.GET();
  if (code != 200) {
    log::warn(kTag, "asset HTTP %d", code);
    http.end();
    return false;
  }
  int len = http.getSize();
  if (len <= 0) {
    log::warn(kTag, "no content length");
    http.end();
    return false;
  }

  // Any leftover state from a previous failed attempt would make
  // Update.begin reject the new session. Force-abort first so retries
  // are clean.
  if (Update.isRunning()) Update.abort();
  if (!Update.begin(len)) {
    log::error(kTag, "Update.begin failed");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  // 4 KB read buffer (was 1 KB). Bigger buffer amortises TLS per-record
  // overhead — measured throughput ~3-4× faster on noisy links.
  uint8_t buf[4096];
  size_t total = 0;
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  display().showText("Pulling 0%", "");
  int lastShownPct = 0;
  // No-progress watchdog: if the stream is silent for kStallMs, bail out
  // so the user isn't staring at "Pulling X%" forever when the AP died
  // mid-download or the CDN slow-loris'd us.
  constexpr uint32_t kStallMs = 20000;
  uint32_t lastProgressMs = millis();
  while (http.connected() && (total < (size_t)len)) {
    size_t avail = stream->available();
    if (avail) {
      size_t got = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
      if (got > 0) {
        if (Update.write(buf, got) != got) {
          log::error(kTag, "Update.write short");
          Update.abort();
          mbedtls_sha256_free(&sha);
          http.end();
          return false;
        }
        mbedtls_sha256_update(&sha, buf, got);
        total += got;
        lastProgressMs = millis();
        int pct = (int)((total * 100) / (size_t)len);
        if (pct >= lastShownPct + 5 || pct == 100) {
          char line[16];
          snprintf(line, sizeof(line), "Pulling %d%%", pct);
          display().showText(line, "");
          log::info(kTag, "Pulling %d%% (%u/%d bytes)", pct,
                    (unsigned)total, len);
          lastShownPct = pct;
        }
      }
    } else {
      if (millis() - lastProgressMs > kStallMs) {
        log::error(kTag, "download stalled (%u/%d bytes, no data for %ums)",
                   (unsigned)total, len, (unsigned)kStallMs);
        Update.abort();
        mbedtls_sha256_free(&sha);
        http.end();
        return false;
      }
      delay(2);
    }
  }
  http.end();
  display().showText("Verifying", "");
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);

  if (expectedHash.length() == 64) {
    char actualHex[65];
    for (int i = 0; i < 32; i++) snprintf(actualHex + i * 2, 3, "%02x", digest[i]);
    actualHex[64] = '\0';
    if (expectedHash != actualHex) {
      log::error(kTag, "SHA256 mismatch (got %s want %s)", actualHex, expectedHash.c_str());
      Update.abort();
      return false;
    }
    log::info(kTag, "SHA256 verified");
  } else {
    log::warn(kTag, "no expected hash supplied — skipping verify");
  }

  display().showText("Installing", "");
  if (!Update.end(true)) {
    log::error(kTag, "Update.end: %u", Update.getError());
    return false;
  }
  log::info(kTag, "OTA wrote %u bytes", (unsigned)total);
  return true;
}

OtaResult Ota::checkAndApply() {
  if (settings().homeWifiSsid().length() == 0) {
    display().showText("OTA", "No WiFi set");
    delay(1500);
    display().clearOverlay();
    return OtaResult::NoCredentials;
  }
  // RAII guard: while OTA is running, keep the cube out of drowsy
  // (idle manager would otherwise drop CPU to 80 MHz halfway through
  // the TLS handshake and tear the WiFi association down) and pin CPU
  // at full speed. Destructor restores defaults regardless of return path.
  struct OtaScope {
    OtaScope() {
      idleManager().inhibitSleep(true);
      power().setCpuProfile(CpuProfile::Performance);
    }
    ~OtaScope() {
      power().setCpuProfile(CpuProfile::Performance);
      idleManager().inhibitSleep(false);
    }
  } scope;

  display().showText("OTA", "Connecting…");
  if (!ensureStation()) {
    display().showText("OTA", "Connect fail");
    delay(2000);
    display().clearOverlay();
    return OtaResult::ConnectFailed;
  }

  display().showText("OTA", "Checking…");
  String tag, assetUrl;
  if (!fetchLatestTag(tag, assetUrl)) {
    display().showText("OTA", "Check fail");
    delay(2000);
    display().clearOverlay();
    teardownStation();
    return OtaResult::CheckFailed;
  }
  log::info(kTag, "remote tag %s, local %s", tag.c_str(), kFirmwareVersion);
  if (!isNewer(tag, kFirmwareVersion)) {
    display().showText("OTA", "Up to date");
    delay(1500);
    display().clearOverlay();
    teardownStation();
    return OtaResult::UpToDate;
  }

  char foundLine[24];
  snprintf(foundLine, sizeof(foundLine), "%s", tag.c_str());
  display().showText("New version", foundLine);
  delay(900);

  String hash = "";  // could be fetched from a sibling firmware.bin.sha256 asset
  // Up to 3 attempts at the full download+flash pipeline. TLS handshake
  // / CDN slow-loris / mid-stream WiFi drops all manifest as a returned
  // false from downloadAndFlash; rather than bail the user out on the
  // first transient, we retry. Each attempt: reset Update state (handled
  // inside downloadAndFlash), reconnect WiFi if dropped, try again.
  constexpr int kMaxAttempts = 3;
  bool downloadOk = false;
  for (int attempt = 1; attempt <= kMaxAttempts && !downloadOk; ++attempt) {
    if (attempt > 1) {
      char retryLine[24];
      snprintf(retryLine, sizeof(retryLine), "Retry %d/%d", attempt, kMaxAttempts);
      log::warn(kTag, "downloadAndFlash failed, %s", retryLine);
      display().showText(retryLine, "");
      delay(1500);
      if (WiFi.status() != WL_CONNECTED) {
        log::warn(kTag, "WiFi dropped, reconnecting before retry");
        WiFi.reconnect();
        uint32_t deadline = millis() + 15000;
        while (WiFi.status() != WL_CONNECTED && millis() < deadline) delay(200);
        if (WiFi.status() != WL_CONNECTED) {
          log::warn(kTag, "reconnect failed, giving up");
          break;
        }
        esp_wifi_set_ps(WIFI_PS_NONE);
        delay(500);
      }
    }
    downloadOk = downloadAndFlash(assetUrl, hash);
  }
  if (!downloadOk) {
    display().showText("OTA", "Write fail");
    delay(2500);
    display().clearOverlay();
    teardownStation();
    return OtaResult::WriteFailed;
  }
  teardownStation();
  display().showText("OTA", "Rebooting…");
  log::info(kTag, "rebooting into new firmware");
  delay(800);
  ESP.restart();
  return OtaResult::Updated;
}

String Ota::latestRemoteTag() {
  if (!ensureStation()) return "";
  String tag, asset;
  bool ok = fetchLatestTag(tag, asset);
  teardownStation();
  return ok ? tag : String();
}

Ota& ota() {
  if (!g_singleton) g_singleton = new Ota();
  return *g_singleton;
}

}  // namespace dash
