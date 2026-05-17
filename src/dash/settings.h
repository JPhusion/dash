// settings.h — NVS-backed runtime configuration.
//
// Keys live in the "dash.cfg" namespace; default values are returned if a key
// isn't present yet. Volatile state goes through Session/Stats modules, not
// here. WiFi credentials in M8 will get encryption on top of this.

#ifndef DASH_SETTINGS_H
#define DASH_SETTINGS_H

#include <Arduino.h>
#include <Preferences.h>

namespace dash {

class Settings {
 public:
  Settings();

  bool begin();

  // Dash device name — for marketing reasons this stays "Dash" always.
  // Kept as a getter so callers don't have to know that.
  String deviceName() { return String("Dash"); }
  void setDeviceName(const String& /*name*/) { /* no-op, see ADR */ }

  // The user's name — used to personalise the AP SSID ("Josh's Dash") and
  // optionally the portal greeting. Empty string means "not yet provided"
  // and the SSID falls back to "Dash-XXXX".
  String userName();
  void setUserName(const String& name);

  // Audio volume 0-100. Default 60.
  uint8_t audioVolume();
  void setAudioVolume(uint8_t v);

  // Sleep timeout in seconds. Default 180.
  uint16_t sleepTimeoutSec();
  void setSleepTimeoutSec(uint16_t s);

  // Default session length in minutes. Default 25 (one pomodoro).
  uint16_t sessionLengthMin();
  void setSessionLengthMin(uint16_t m);

  // Tap sensitivity threshold in g of linear accel. Default 0.5 — lower =
  // more sensitive. Range clamped to 0.2..2.0.
  float tapSensitivityG();
  void setTapSensitivityG(float g);

  // Home Wi-Fi credentials for OTA. Empty string = unset.
  String homeWifiSsid();
  void setHomeWifiSsid(const String& s);
  String homeWifiPassword();
  void setHomeWifiPassword(const String& s);

  // Last reported epoch (UTC seconds). Used for OTA scheduling + log
  // timestamps. Synced from phone on every portal page load.
  uint32_t lastUnix();
  void setLastUnix(uint32_t t);
  int16_t tzOffsetMin();    // signed minutes from UTC
  void setTzOffsetMin(int16_t m);

  // Has the user completed the onboarding wizard?
  bool onboarded();
  void setOnboarded(bool o);

 private:
  Preferences p_;
  bool open_;
};

Settings& settings();

}  // namespace dash

#endif
