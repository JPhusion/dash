#include "dash/ota.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#include <mbedtls/sha256.h>

#include "dash/build_info.h"
#include "dash/log.h"
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

  // Tear AP down so we can become STA. Channel won't carry over to home AP.
  if (wifiAp().running()) wifiAp().stop();

  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   // power-save during OTA
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
  log::info(kTag, "home WiFi connected, IP=%s", WiFi.localIP().toString().c_str());
  return true;
}

void Ota::teardownStation() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool Ota::fetchLatestTag(String& tagOut, String& assetUrl) {
  WiFiClientSecure client;
  client.setInsecure();   // GitHub uses Let's Encrypt; embedding the root CA
                          // bundle is a future M12 hardening task.
  HTTPClient http;
  http.setUserAgent("dash-firmware");
  if (!http.begin(client, kReleasesUrl)) return false;
  int code = http.GET();
  if (code != 200) {
    log::warn(kTag, "GitHub releases HTTP %d", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  // Very light parse — just look for "tag_name" and the first firmware.bin asset URL.
  int t = body.indexOf("\"tag_name\":\"");
  if (t < 0) return false;
  int e = body.indexOf("\"", t + 12);
  if (e < 0) return false;
  tagOut = body.substring(t + 12, e);

  int asset = body.indexOf("firmware.bin");
  if (asset < 0) return false;
  // Walk backwards to find the most recent "browser_download_url":"<url-ending-in-firmware.bin>"
  int urlStart = body.lastIndexOf("\"browser_download_url\":\"", asset);
  if (urlStart < 0) return false;
  urlStart += 24;
  int urlEnd = body.indexOf("\"", urlStart);
  if (urlEnd < 0) return false;
  assetUrl = body.substring(urlStart, urlEnd);
  return true;
}

bool Ota::downloadAndFlash(const String& url, const String& expectedHash) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setUserAgent("dash-firmware");
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

  if (!Update.begin(len)) {
    log::error(kTag, "Update.begin failed");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  size_t total = 0;
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

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
      }
    } else {
      delay(1);
    }
  }
  http.end();
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

  if (!Update.end(true)) {
    log::error(kTag, "Update.end: %u", Update.getError());
    return false;
  }
  log::info(kTag, "OTA wrote %u bytes", (unsigned)total);
  return true;
}

OtaResult Ota::checkAndApply() {
  if (settings().homeWifiSsid().length() == 0) return OtaResult::NoCredentials;
  if (!ensureStation()) return OtaResult::ConnectFailed;

  String tag, assetUrl;
  if (!fetchLatestTag(tag, assetUrl)) {
    teardownStation();
    return OtaResult::CheckFailed;
  }
  log::info(kTag, "remote tag %s, local %s", tag.c_str(), kFirmwareVersion);
  if (!isNewer(tag, kFirmwareVersion)) {
    teardownStation();
    return OtaResult::UpToDate;
  }

  String hash = "";  // could be fetched from a sibling firmware.bin.sha256 asset
  if (!downloadAndFlash(assetUrl, hash)) {
    teardownStation();
    return OtaResult::WriteFailed;
  }
  teardownStation();
  log::info(kTag, "rebooting into new firmware");
  delay(500);
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
