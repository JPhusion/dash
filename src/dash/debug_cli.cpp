#include "dash/debug_cli.h"

#include <LittleFS.h>
#include <esp_heap_caps.h>

#include "dash/audio.h"
#include "dash/character.h"
#include "dash/display.h"
#include "dash/games.h"
#include "dash/idle_manager.h"
#include "dash/imu.h"
#include "dash/log.h"
#include "dash/power.h"
#include "dash/self_test.h"
#include "dash/session.h"
#include "dash/settings.h"
#include "dash/sounds.h"
#include "dash/state_machine.h"
#include "dash/stats.h"
#include "dash/touch.h"
#include "dash/wifi_ap.h"

namespace dash {

namespace {
constexpr const char* kTag = "CLI";
DebugCli* g_singleton = nullptr;

void printHelp() {
  Serial.println();
  Serial.println(F("=== Dash debug CLI ==="));
  Serial.println(F("help                 — this list"));
  Serial.println(F("status               — state / mood / heap / face"));
  Serial.println(F("tap [g]              — inject IMU tap (default 1.0g)"));
  Serial.println(F("dt | tt              — double / triple tap"));
  Serial.println(F("shake                — IMU shake event"));
  Serial.println(F("tilt up|down|left|right|front|back|unknown"));
  Serial.println(F("touch | dtouch | lptouch — cap-touch events"));
  Serial.println(F("eye <0..19>          — force EyeState"));
  Serial.println(F("mood <0..5>          — set Character mood"));
  Serial.println(F("sound <name>         — play a sound by short name"));
  Serial.println(F("vol <0..100>         — set audio volume"));
  Serial.println(F("silent on|off        — silent mode"));
  Serial.println(F("session start [min]  — start a session"));
  Serial.println(F("session stop|pause|resume|snap"));
  Serial.println(F("state <name>         — force state transition"));
  Serial.println(F("sleep                — enter deep sleep"));
  Serial.println(F("stats [fake N | reset]"));
  Serial.println(F("heap                 — heap snapshot"));
  Serial.println(F("ap                   — wifi info"));
  Serial.println(F("selftest             — run the automated suite"));
  Serial.println();
}

const char* moodName(Mood m) {
  switch (m) {
    case Mood::Neutral:   return "Neutral";
    case Mood::Focused:   return "Focused";
    case Mood::Excited:   return "Excited";
    case Mood::Tired:     return "Tired";
    case Mood::Listening: return "Listening";
    case Mood::Playful:   return "Playful";
  }
  return "?";
}

Face parseFace(const String& s) {
  if (s == "up")      return Face::Up;
  if (s == "down")    return Face::Down;
  if (s == "left")    return Face::Left;
  if (s == "right")   return Face::Right;
  if (s == "front")   return Face::Front;
  if (s == "back")    return Face::Back;
  return Face::Unknown;
}

const char* soundByName(const String& n) {
  if (n == "boot")             return sounds::kBoot;
  if (n == "wake")             return sounds::kWake;
  if (n == "sleep")            return sounds::kSleep;
  if (n == "session_start")    return sounds::kSessionStart;
  if (n == "session_end")      return sounds::kSessionEnd;
  if (n == "session_complete") return sounds::kSessionComplete;
  if (n == "tap_ack")          return sounds::kTapAck;
  if (n == "double_tap_ack")   return sounds::kDoubleTapAck;
  if (n == "triple_tap_ack")   return sounds::kTripleTapAck;
  if (n == "test_tone")        return sounds::kTestTone;
  if (n == "whoa")             return sounds::kWhoa;
  if (n == "tilt")             return sounds::kTilt;
  if (n == "boop")             return sounds::kBoop;
  if (n == "menu_blip")        return sounds::kMenuBlip;
  if (n == "menu_confirm")     return sounds::kMenuConfirm;
  if (n == "menu_back")        return sounds::kMenuBack;
  if (n == "distraction")      return sounds::kDistraction;
  if (n == "encouragement")    return sounds::kEncouragement;
  if (n == "yawn")             return sounds::kYawn;
  if (n == "giggle")           return sounds::kGiggle;
  if (n == "heartbeat")        return sounds::kHeartbeat;
  if (n == "good_morning")     return sounds::kGoodMorning;
  if (n == "milestone")        return sounds::kMilestone;
  if (n == "game_correct")     return sounds::kGameCorrect;
  if (n == "game_wrong")       return sounds::kGameWrong;
  if (n == "game_start")       return sounds::kGameStart;
  return nullptr;
}

DeviceState parseState(const String& s) {
  if (s == "booting")             return DeviceState::Booting;
  if (s == "onboarding")          return DeviceState::Onboarding;
  if (s == "idle")                return DeviceState::Idle;
  if (s == "drowsy")              return DeviceState::Drowsy;
  if (s == "asleep")              return DeviceState::Asleep;
  if (s == "awakeforsession")     return DeviceState::AwakeForSession;
  if (s == "insession")           return DeviceState::InSession;
  if (s == "inmenu")              return DeviceState::InMenu;
  if (s == "ingame")              return DeviceState::InGame;
  if (s == "groupwait")           return DeviceState::GroupSessionWaiting;
  if (s == "groupactive")         return DeviceState::GroupSessionActive;
  if (s == "ota")                 return DeviceState::OtaChecking;
  return DeviceState::Idle;
}

// Tokenise "session start 5" into ["session", "start", "5"].
void tokenize(const String& line, String tokens[], int max, int& count) {
  count = 0;
  int i = 0, n = line.length();
  while (i < n && count < max) {
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= n) break;
    int start = i;
    while (i < n && line[i] != ' ' && line[i] != '\t') i++;
    tokens[count++] = line.substring(start, i);
  }
}

}  // namespace

DebugCli::DebugCli() : task_(nullptr), running_(false) { buf_.reserve(96); }

void DebugCli::begin() { /* nothing to init */ }

void DebugCli::start() {
  if (running_) return;
  running_ = true;
  xTaskCreatePinnedToCore(&DebugCli::taskTrampoline, "debug-cli", 6144, this,
                          1, &task_, 0);
}

void DebugCli::stop() { running_ = false; }

void DebugCli::taskTrampoline(void* arg) { static_cast<DebugCli*>(arg)->loop(); }

void DebugCli::loop() {
  Serial.println();
  Serial.println(F("[CLI] ready — type 'help'"));
  while (running_) {
    while (Serial.available() > 0) {
      char c = (char)Serial.read();
      if (c == '\r') continue;
      if (c == '\n') {
        String line = buf_;
        buf_ = "";
        line.trim();
        if (line.length() > 0) dispatch(line);
      } else if (buf_.length() < 90) {
        buf_ += c;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  task_ = nullptr;
  vTaskDelete(nullptr);
}

void DebugCli::dispatch(const String& line) {
  if (line.startsWith("#")) {
    Serial.printf("[CLI] %s\n", line.c_str());
    return;
  }
  String t[6];
  int n = 0;
  tokenize(line, t, 6, n);
  if (n == 0) return;
  String cmd = t[0];
  cmd.toLowerCase();
  Serial.printf("[CLI] > %s\n", line.c_str());

  if (cmd == "help" || cmd == "?") { printHelp(); return; }

  if (cmd == "status") {
    auto s = imu().latest();
    Serial.printf("state=%s  mood=%s  face=%s  pitch=%.1f roll=%.1f\n",
                  deviceStateName(stateMachine().state()),
                  moodName(character().mood()),
                  faceToString(imu().currentFace()),
                  s.pitch, s.roll);
    Serial.printf("heap=%u  ssid=%s  onboarded=%d\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  wifiAp().ssid().c_str(),
                  settings().onboarded() ? 1 : 0);
    return;
  }

  if (cmd == "tap") {
    float mag = (n >= 2) ? t[1].toFloat() : 1.0f;
    imu().injectEvent({ImuEventType::Tap, imu().currentFace(), imu().currentFace(), mag, 0});
    return;
  }
  if (cmd == "dt") {
    imu().injectEvent({ImuEventType::DoubleTap, imu().currentFace(), imu().currentFace(), 0.0f, 0});
    return;
  }
  if (cmd == "tt") {
    imu().injectEvent({ImuEventType::TripleTap, imu().currentFace(), imu().currentFace(), 0.0f, 0});
    return;
  }
  if (cmd == "shake") {
    imu().injectEvent({ImuEventType::Shake, imu().currentFace(), imu().currentFace(), 1.5f, 0});
    return;
  }
  if (cmd == "tilt" && n >= 2) {
    Face newFace = parseFace(t[1]);
    Face old = imu().currentFace();
    imu().injectEvent({ImuEventType::OrientationChange, newFace, old, 0.0f, 0});
    return;
  }
  if (cmd == "touch")  { touch().injectEvent({TouchEventType::Touch,       0, 0}); return; }
  if (cmd == "dtouch") { touch().injectEvent({TouchEventType::DoubleTouch, 0, 0}); return; }
  if (cmd == "lptouch"){ touch().injectEvent({TouchEventType::LongPress,   0, 0}); return; }

  if (cmd == "eye" && n >= 2) {
    int v = t[1].toInt();
    if (v < 0 || v > 19) { Serial.println("eye index 0..19"); return; }
    display().setEyeState((EyeState)v);
    return;
  }
  if (cmd == "mood" && n >= 2) {
    int v = t[1].toInt();
    if (v < 0 || v > 5) { Serial.println("mood 0..5"); return; }
    character().setMood((Mood)v);
    return;
  }
  if (cmd == "sound" && n >= 2) {
    const char* path = soundByName(t[1]);
    if (!path) { Serial.printf("unknown sound '%s'\n", t[1].c_str()); return; }
    audio().play(path, AudioFormat::Pcm8kHzMono8, true);
    return;
  }
  if (cmd == "vol" && n >= 2) {
    uint8_t v = (uint8_t)t[1].toInt();
    audio().setVolume(v);
    settings().setAudioVolume(v);
    Serial.printf("vol=%u\n", audio().volume());
    return;
  }
  if (cmd == "silent" && n >= 2) {
    audio().setSilent(t[1] == "on" || t[1] == "1");
    Serial.printf("silent=%d\n", (int)audio().silent());
    return;
  }

  if (cmd == "session" && n >= 2) {
    if (t[1] == "start") {
      uint16_t minutes = n >= 3 ? (uint16_t)t[2].toInt() : settings().sessionLengthMin();
      bool ok = session().start(minutes);
      Serial.printf("session.start(%u) → %d\n", minutes, ok);
    } else if (t[1] == "stop") {
      session().stop(false);
    } else if (t[1] == "pause") {
      session().pause();
    } else if (t[1] == "resume") {
      session().resume();
    } else if (t[1] == "snap") {
      auto s = session().snapshot();
      Serial.printf("active=%d state=%d target=%u distractions=%u label=%s\n",
                    (int)s.active, (int)s.state, s.targetMin, s.distractions, s.label);
    }
    return;
  }

  if (cmd == "state" && n >= 2) {
    String s = t[1]; s.toLowerCase();
    stateMachine().transitionTo(parseState(s));
    return;
  }
  if (cmd == "sleep") { power().enterDeepSleep(0); return; }
  if (cmd == "heap") {
    Serial.printf("free=%u largest=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return;
  }
  if (cmd == "ap") {
    Serial.printf("ssid=%s ip=%s stations=%u running=%d\n",
                  wifiAp().ssid().c_str(),
                  wifiAp().apIp().toString().c_str(),
                  wifiAp().stationCount(), (int)wifiAp().running());
    return;
  }

  if (cmd == "stats") {
    if (n >= 2 && t[1] == "reset") {
      LittleFS.remove("/stats/sessions.ndjson");
      Serial.println("stats wiped");
      return;
    }
    if (n >= 2 && t[1] == "fake") {
      int count = (n >= 3) ? t[2].toInt() : 5;
      for (int i = 0; i < count; i++) {
        SessionRecord r{};
        r.startedUnix = settings().lastUnix() + i * 60;
        r.targetMin = 25;
        r.actualSec = 25 * 60;
        r.distractions = (uint16_t)(i % 4);
        r.completed = 1;
        stats().append(r);
      }
      Serial.printf("injected %d fake records\n", count);
      return;
    }
    auto s = stats().summary();
    Serial.printf("sessions=%u completed=%u focused_s=%u distractions=%u "
                  "streak=%u best=%u\n",
                  s.totalSessions, s.completedSessions,
                  (unsigned)s.totalFocusedSec, s.totalDistractions,
                  s.streakDays, (unsigned)s.bestSingleSec);
    return;
  }

  if (cmd == "selftest") { runSelfTest(); return; }

  Serial.printf("[CLI] unknown command: %s\n", cmd.c_str());
}

DebugCli& debugCli() {
  if (!g_singleton) g_singleton = new DebugCli();
  return *g_singleton;
}

}  // namespace dash
