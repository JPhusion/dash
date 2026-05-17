#include "dash/settings.h"

#include "dash/log.h"

namespace dash {

namespace {
constexpr const char* kTag = "Settings";
constexpr const char* kNs  = "dash.cfg";
Settings* g_singleton = nullptr;
}

Settings::Settings() : open_(false) {}

bool Settings::begin() {
  // Open read-only first to check. NVS opens for read-write lazily per setter.
  if (!p_.begin(kNs, true)) {
    log::warn(kTag, "first-boot NVS init");
    if (!p_.begin(kNs, false)) {
      log::error(kTag, "NVS init failed");
      return false;
    }
    open_ = true;
    p_.end();
    return true;
  }
  open_ = true;
  p_.end();
  return true;
}

String Settings::deviceName() {
  p_.begin(kNs, true);
  String s = p_.getString("name", "Dash");
  p_.end();
  return s;
}
void Settings::setDeviceName(const String& s) {
  p_.begin(kNs, false); p_.putString("name", s); p_.end();
}

uint8_t Settings::audioVolume() {
  p_.begin(kNs, true);
  uint8_t v = p_.getUChar("vol", 60);
  p_.end();
  return v;
}
void Settings::setAudioVolume(uint8_t v) {
  p_.begin(kNs, false); p_.putUChar("vol", v); p_.end();
}

uint16_t Settings::sleepTimeoutSec() {
  p_.begin(kNs, true);
  uint16_t v = p_.getUShort("sleep_to", 180);
  p_.end();
  return v;
}
void Settings::setSleepTimeoutSec(uint16_t s) {
  p_.begin(kNs, false); p_.putUShort("sleep_to", s); p_.end();
}

uint16_t Settings::sessionLengthMin() {
  p_.begin(kNs, true);
  uint16_t v = p_.getUShort("sess_min", 25);
  p_.end();
  return v;
}
void Settings::setSessionLengthMin(uint16_t m) {
  p_.begin(kNs, false); p_.putUShort("sess_min", m); p_.end();
}

float Settings::tapSensitivityG() {
  p_.begin(kNs, true);
  float v = p_.getFloat("tap_g", 0.5f);
  p_.end();
  if (v < 0.2f) v = 0.2f;
  if (v > 2.0f) v = 2.0f;
  return v;
}
void Settings::setTapSensitivityG(float g) {
  if (g < 0.2f) g = 0.2f;
  if (g > 2.0f) g = 2.0f;
  p_.begin(kNs, false); p_.putFloat("tap_g", g); p_.end();
}

String Settings::homeWifiSsid() {
  p_.begin(kNs, true);
  String s = p_.getString("hwifi_s", "");
  p_.end();
  return s;
}
void Settings::setHomeWifiSsid(const String& s) {
  p_.begin(kNs, false); p_.putString("hwifi_s", s); p_.end();
}
String Settings::homeWifiPassword() {
  p_.begin(kNs, true);
  String s = p_.getString("hwifi_p", "");
  p_.end();
  return s;
}
void Settings::setHomeWifiPassword(const String& s) {
  p_.begin(kNs, false); p_.putString("hwifi_p", s); p_.end();
}

uint32_t Settings::lastUnix() {
  p_.begin(kNs, true);
  uint32_t v = p_.getULong("unix", 0);
  p_.end();
  return v;
}
void Settings::setLastUnix(uint32_t t) {
  p_.begin(kNs, false); p_.putULong("unix", t); p_.end();
}

int16_t Settings::tzOffsetMin() {
  p_.begin(kNs, true);
  int16_t v = p_.getShort("tz_min", 0);
  p_.end();
  return v;
}
void Settings::setTzOffsetMin(int16_t m) {
  p_.begin(kNs, false); p_.putShort("tz_min", m); p_.end();
}

bool Settings::onboarded() {
  p_.begin(kNs, true);
  bool v = p_.getBool("onb", false);
  p_.end();
  return v;
}
void Settings::setOnboarded(bool o) {
  p_.begin(kNs, false); p_.putBool("onb", o); p_.end();
}

Settings& settings() {
  if (!g_singleton) g_singleton = new Settings();
  return *g_singleton;
}

}  // namespace dash
