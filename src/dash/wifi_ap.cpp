#include "dash/wifi_ap.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "dash/log.h"
#include "dash/settings.h"

namespace dash {

namespace {
constexpr const char* kTag = "WifiAp";
constexpr uint8_t kChannel = 6;
constexpr int8_t  kTxPowerQdBm = 60;  // 60/4 = 15 dBm — was 11 dBm but the
                                       // AP sometimes didn't appear in some
                                       // phone Wi-Fi lists at the lower power.
WifiAp* g_singleton = nullptr;
}

WifiAp::WifiAp() : server_(nullptr), dnsTask_(nullptr), httpTask_(nullptr),
                   running_(false) {}

bool WifiAp::start() {
  if (running_) return true;

  WiFi.mode(WIFI_AP);

  // SSID = "{user}'s Dash" if the user provided a name during onboarding;
  // otherwise fall back to a MAC-suffixed Dash-XXXX so multiple cubes in
  // the same room don't collide.
  String user = settings().userName();
  if (user.length() > 0) {
    ssid_ = user + "'s Dash";
  } else {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[32];
    snprintf(buf, sizeof(buf), "Dash-%02X%02X", mac[4], mac[5]);
    ssid_ = String(buf);
  }

  apIp_ = IPAddress(192, 168, 4, 1);
  IPAddress gateway = apIp_;
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIp_, gateway, subnet);

  bool ok = WiFi.softAP(ssid_.c_str(), nullptr /*open*/, kChannel,
                        false /*not hidden*/, 4 /*max clients*/);
  if (!ok) {
    log::error(kTag, "softAP failed");
    return false;
  }

  // TX power: 11 dBm to save current on battery and reduce brownout risk.
  esp_wifi_set_max_tx_power(kTxPowerQdBm);

  // Snappy portal: don't let the modem nap while a phone is on the AP.
  WiFi.setSleep(false);

  // DNS server: redirect every query to our IP (captive portal trigger).
  dns_.setErrorReplyCode(DNSReplyCode::NoError);
  if (!dns_.start(53, "*", apIp_)) {
    log::error(kTag, "DNS start failed");
    WiFi.softAPdisconnect(true);
    return false;
  }

  // HTTP server — port 80.
  server_ = new WebServer(80);
  // Routes are registered by Portal::begin(); kick the server here so it's
  // serving 404s while Portal wires up.
  server_->begin();

  // mDNS responder so users can type http://dash.local in their browser
  // instead of remembering 192.168.4.1. Works automatically on iOS/macOS
  // (Bonjour) and most Android browsers (Chrome adds .local resolution).
  // Advertises an http service so phones can also discover via Bonjour.
  if (MDNS.begin("dash")) {
    MDNS.addService("http", "tcp", 80);
    log::info(kTag, "mDNS up: http://dash.local/");
  } else {
    log::warn(kTag, "mDNS start failed");
  }

  running_ = true;
  xTaskCreatePinnedToCore(&WifiAp::dnsTaskTrampoline, "dns", 4096, this, 1,
                          &dnsTask_, 0);
  xTaskCreatePinnedToCore(&WifiAp::httpTaskTrampoline, "http", 6144, this, 1,
                          &httpTask_, 0);

  log::info(kTag, "AP up: %s @ %s (ch %u, 11 dBm)", ssid_.c_str(),
            apIp_.toString().c_str(), kChannel);
  return true;
}

void WifiAp::stop() {
  if (!running_) return;
  running_ = false;
  MDNS.end();
  dns_.stop();
  if (server_) {
    server_->close();
    delete server_;
    server_ = nullptr;
  }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  log::info(kTag, "AP down");
}

uint8_t WifiAp::stationCount() const {
  return WiFi.softAPgetStationNum();
}

void WifiAp::dnsTaskTrampoline(void* arg) {
  static_cast<WifiAp*>(arg)->dnsLoop();
}

void WifiAp::httpTaskTrampoline(void* arg) {
  static_cast<WifiAp*>(arg)->httpLoop();
}

void WifiAp::dnsLoop() {
  while (running_) {
    dns_.processNextRequest();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  dnsTask_ = nullptr;
  vTaskDelete(nullptr);
}

void WifiAp::httpLoop() {
  while (running_) {
    if (server_) server_->handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  httpTask_ = nullptr;
  vTaskDelete(nullptr);
}

WifiAp& wifiAp() {
  if (!g_singleton) g_singleton = new WifiAp();
  return *g_singleton;
}

}  // namespace dash
