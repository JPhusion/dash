// wifi_ap.h — Dash's captive-portal AP.
//
// On bring-up: SSID Dash-XXXX (XXXX = last 4 chars of MAC), open by default
// (since the cube is right next to you), TX power capped at 11 dBm, locked to
// channel 6 so future ESP-NOW group sessions can coexist. WiFi modem-sleep
// disabled while a station is connected to keep the portal snappy.
//
// DNS server forces every name lookup back to the AP IP — this is the standard
// "captive portal" trigger that pops the auth-page on phones.
//
// HTTP routing lives in the Portal module so this module stays focused on
// radio plumbing.

#ifndef DASH_WIFI_AP_H
#define DASH_WIFI_AP_H

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>

namespace dash {

class WifiAp {
 public:
  WifiAp();

  // Brings up the AP + DNS server + HTTP server. Returns false if any stage
  // fails. Use stop() to tear everything down (for OTA STA mode).
  bool start();
  void stop();
  bool running() const { return running_; }

  // SSID + IP for display on the OLED.
  String ssid() const { return ssid_; }
  IPAddress apIp() const { return apIp_; }

  // Number of currently associated stations.
  uint8_t stationCount() const;

  // Expose the web server so Portal can register routes.
  WebServer* server() { return server_; }

 private:
  static void dnsTaskTrampoline(void* arg);
  static void httpTaskTrampoline(void* arg);
  void dnsLoop();
  void httpLoop();

  String ssid_;
  IPAddress apIp_;
  DNSServer dns_;
  WebServer* server_;
  TaskHandle_t dnsTask_;
  TaskHandle_t httpTask_;
  volatile bool running_;
};

WifiAp& wifiAp();

}  // namespace dash

#endif
