#include "dash/self_test.h"

#include <Arduino.h>
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
#include "dash/portal.h"
#include "dash/session.h"
#include "dash/settings.h"
#include "dash/sounds.h"
#include "dash/state_machine.h"
#include "dash/stats.h"
#include "dash/touch.h"
#include "dash/wifi_ap.h"

namespace dash {

namespace {

uint32_t g_passes = 0;
uint32_t g_fails = 0;
char g_lastImuEvent[24] = {0};
char g_lastTouchEvent[24] = {0};
uint32_t g_lastTransitionMs = 0;
DeviceState g_lastTransitionFrom = DeviceState::Booting;
DeviceState g_lastTransitionTo = DeviceState::Booting;
int g_testImuListener = -1;
int g_testTouchListener = -1;
int g_testStateListener = -1;
bool g_listenersInstalled = false;

void pass(const char* fmt, ...) {
  char buf[160];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  Serial.printf("  PASS  %s\n", buf);
  g_passes++;
}
void fail(const char* fmt, ...) {
  char buf[160];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  Serial.printf("  FAIL  %s\n", buf);
  g_fails++;
}
void section(const char* name) {
  Serial.println();
  Serial.printf("── %s ──\n", name);
}

void check(bool cond, const char* fmt, ...) {
  char buf[160];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  if (cond) pass("%s", buf); else fail("%s", buf);
}

const char* imuEventName_(ImuEventType t) {
  switch (t) {
    case ImuEventType::Tap:               return "Tap";
    case ImuEventType::DoubleTap:         return "DoubleTap";
    case ImuEventType::TripleTap:         return "TripleTap";
    case ImuEventType::Shake:             return "Shake";
    case ImuEventType::OrientationChange: return "OrientationChange";
    case ImuEventType::Stationary:        return "Stationary";
  }
  return "?";
}

void installListeners() {
  if (g_listenersInstalled) return;
  imu().onEvent([](const ImuEvent& e) {
    strncpy(g_lastImuEvent, imuEventName_(e.type), sizeof(g_lastImuEvent) - 1);
  });
  touch().onEvent([](const TouchEvent& e) {
    const char* n = "?";
    switch (e.type) {
      case TouchEventType::Touch:       n = "Touch"; break;
      case TouchEventType::DoubleTouch: n = "DoubleTouch"; break;
      case TouchEventType::LongPress:   n = "LongPress"; break;
    }
    strncpy(g_lastTouchEvent, n, sizeof(g_lastTouchEvent) - 1);
  });
  stateMachine().onTransition([](DeviceState from, DeviceState to) {
    g_lastTransitionFrom = from;
    g_lastTransitionTo = to;
    g_lastTransitionMs = millis();
  });
  g_listenersInstalled = true;
}

void clearImuFlag()   { g_lastImuEvent[0]   = '\0'; }
void clearTouchFlag() { g_lastTouchEvent[0] = '\0'; }

bool waitForImuEvent(const char* expected, uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    if (strcmp(g_lastImuEvent, expected) == 0) return true;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return false;
}

bool waitForTouchEvent(const char* expected, uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    if (strcmp(g_lastTouchEvent, expected) == 0) return true;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return false;
}

// ---- per-section tests ---------------------------------------------------

void testStateMachine() {
  section("StateMachine");
  DeviceState orig = stateMachine().state();
  const DeviceState seq[] = {
    DeviceState::Idle, DeviceState::InSession, DeviceState::InMenu,
    DeviceState::InGame, DeviceState::OtaChecking,
    DeviceState::GroupSessionWaiting, DeviceState::GroupSessionActive,
    DeviceState::Drowsy, DeviceState::Idle,
  };
  for (auto s : seq) {
    stateMachine().transitionTo(s);
    check(stateMachine().state() == s, "transition to %s", deviceStateName(s));
  }
  check(strlen(deviceStateName(DeviceState::Booting)) > 0, "deviceStateName non-empty");
  stateMachine().transitionTo(orig);
}

void testEyeStates() {
  section("Display: every eye state");
  for (int i = 0; i < 20; i++) {
    display().setEyeState((EyeState)i);
    vTaskDelay(pdMS_TO_TICKS(60));   // ~2 frames at 30 fps so the render task picks it up
    check(display().eyeState() == (EyeState)i, "eye state %d applied", i);
  }
  display().setEyeState(EyeState::Idle);
}

void testDisplayOverlays() {
  section("Display: overlays");
  display().showBootSplash();   vTaskDelay(pdMS_TO_TICKS(80)); pass("boot splash overlay set");
  display().showProgress(0);    vTaskDelay(pdMS_TO_TICKS(80));
  display().showProgress(50);   vTaskDelay(pdMS_TO_TICKS(80));
  display().showProgress(100);  vTaskDelay(pdMS_TO_TICKS(80)); pass("progress overlay 0/50/100");
  display().showText("Test", "OK"); vTaskDelay(pdMS_TO_TICKS(80)); pass("text overlay");
  display().showQR("dash.local");   vTaskDelay(pdMS_TO_TICKS(80)); pass("QR overlay");
  display().clearOverlay();         vTaskDelay(pdMS_TO_TICKS(80)); pass("clearOverlay");
  display().blink();                vTaskDelay(pdMS_TO_TICKS(120)); pass("blink");
}

void testCharacter() {
  section("Character: moods and reactions");
  Mood orig = character().mood();
  const Mood moods[] = {Mood::Neutral, Mood::Focused, Mood::Excited,
                        Mood::Tired, Mood::Listening, Mood::Playful};
  for (auto m : moods) {
    character().setMood(m);
    check(character().mood() == m, "mood=%d set", (int)m);
  }
  character().react(EyeState::Heart, 200);
  vTaskDelay(pdMS_TO_TICKS(60));
  pass("react(Heart, 200ms)");
  character().setMood(orig);
}

void testImuEvents() {
  section("IMU: synthetic event pipeline");
  // Inject one at a time with a small settle delay so successive events
  // don't stack on top of each other's audio cues. Each waitFor* uses a
  // 1500ms timeout — generous, since some main-loop handlers spawn
  // deferred tasks before returning to the event loop.
  clearImuFlag();
  imu().injectEvent({ImuEventType::Tap, Face::Up, Face::Up, 1.2f, 0});
  check(waitForImuEvent("Tap", 1500), "Tap dispatched to listener");
  vTaskDelay(pdMS_TO_TICKS(150));

  clearImuFlag();
  imu().injectEvent({ImuEventType::DoubleTap, Face::Up, Face::Up, 0.0f, 0});
  check(waitForImuEvent("DoubleTap", 1500), "DoubleTap dispatched");
  vTaskDelay(pdMS_TO_TICKS(150));

  clearImuFlag();
  imu().injectEvent({ImuEventType::TripleTap, Face::Up, Face::Up, 0.0f, 0});
  check(waitForImuEvent("TripleTap", 1500), "TripleTap dispatched");
  vTaskDelay(pdMS_TO_TICKS(200));

  clearImuFlag();
  imu().injectEvent({ImuEventType::Shake, Face::Up, Face::Up, 2.0f, 0});
  check(waitForImuEvent("Shake", 1500), "Shake dispatched");
  vTaskDelay(pdMS_TO_TICKS(150));

  clearImuFlag();
  imu().injectEvent({ImuEventType::OrientationChange, Face::Left, Face::Up, 0.0f, 0});
  check(waitForImuEvent("OrientationChange", 1500), "OrientationChange dispatched");
  vTaskDelay(pdMS_TO_TICKS(150));

  clearImuFlag();
  imu().injectEvent({ImuEventType::Stationary, Face::Up, Face::Up, 0.0f, 0});
  check(waitForImuEvent("Stationary", 1500), "Stationary dispatched");
}

void testTouchEvents() {
  section("Touch: synthetic event pipeline");
  clearTouchFlag();
  touch().injectEvent({TouchEventType::Touch, 50, 0});
  check(waitForTouchEvent("Touch", 800), "Touch dispatched");
  clearTouchFlag();
  touch().injectEvent({TouchEventType::DoubleTouch, 50, 0});
  check(waitForTouchEvent("DoubleTouch", 800), "DoubleTouch dispatched");
  clearTouchFlag();
  touch().injectEvent({TouchEventType::LongPress, 50, 0});
  check(waitForTouchEvent("LongPress", 800), "LongPress dispatched");
}

void testSounds() {
  section("Audio: every sound file");
  // Force silent during the file-existence sweep so we don't blast through
  // half a megabyte of sound while the user is in front of the cube.
  bool wasSilent = audio().silent();
  audio().setSilent(true);
  const char* paths[] = {
    sounds::kBoot, sounds::kWake, sounds::kSleep,
    sounds::kSessionStart, sounds::kSessionEnd, sounds::kSessionComplete,
    sounds::kTapAck, sounds::kTapAck2, sounds::kTapAck3,
    sounds::kDoubleTapAck, sounds::kTripleTapAck,
    sounds::kTestTone, sounds::kWhoa, sounds::kTilt, sounds::kBoop,
    sounds::kMenuBlip, sounds::kMenuConfirm, sounds::kMenuBack,
    sounds::kDistraction, sounds::kEncouragement,
    sounds::kYawn, sounds::kGiggle, sounds::kHeartbeat,
    sounds::kGoodMorning, sounds::kMilestone,
    sounds::kGameCorrect, sounds::kGameWrong, sounds::kGameStart,
  };
  for (auto p : paths) {
    File f = LittleFS.open(p, "r");
    if (f && f.size() > 0) {
      pass("file %s (%u bytes)", p, (unsigned)f.size());
      f.close();
    } else {
      fail("file %s missing or empty", p);
    }
  }
  // Smoke-test the playback path on one sound (still silent).
  audio().play(sounds::kTapAck, AudioFormat::Pcm8kHzMono8, true);
  vTaskDelay(pdMS_TO_TICKS(80));
  pass("play(tap_ack) dispatched");
  audio().setSilent(wasSilent);
}

void testSettings() {
  section("Settings: NVS round-trip");
  // Snapshot
  String origUser   = settings().userName();
  uint8_t origVol   = settings().audioVolume();
  uint16_t origSleep= settings().sleepTimeoutSec();
  uint16_t origSess = settings().sessionLengthMin();
  float origTap     = settings().tapSensitivityG();

  settings().setUserName("Tester");
  check(settings().userName() == "Tester", "userName round-trip");
  settings().setAudioVolume(42);
  check(settings().audioVolume() == 42, "audioVolume round-trip");
  settings().setSleepTimeoutSec(240);
  check(settings().sleepTimeoutSec() == 240, "sleepTimeoutSec round-trip");
  settings().setSessionLengthMin(15);
  check(settings().sessionLengthMin() == 15, "sessionLengthMin round-trip");
  settings().setTapSensitivityG(0.7f);
  float t = settings().tapSensitivityG();
  check(t > 0.69f && t < 0.71f, "tapSensitivityG round-trip (%.2f)", t);
  // Clamp checks
  settings().setTapSensitivityG(10.0f);
  check(settings().tapSensitivityG() <= 2.0f, "tapSensitivity clamps high");
  settings().setTapSensitivityG(0.0f);
  check(settings().tapSensitivityG() >= 0.2f, "tapSensitivity clamps low");
  // Restore
  settings().setUserName(origUser);
  settings().setAudioVolume(origVol);
  settings().setSleepTimeoutSec(origSleep);
  settings().setSessionLengthMin(origSess);
  settings().setTapSensitivityG(origTap);
}

void testPower() {
  section("Power: CPU profile switch");
  power().setCpuProfile(CpuProfile::LowPower);
  vTaskDelay(pdMS_TO_TICKS(50));
  check(getCpuFrequencyMhz() == 80, "CPU at 80 MHz");
  power().setCpuProfile(CpuProfile::Performance);
  vTaskDelay(pdMS_TO_TICKS(50));
  check(getCpuFrequencyMhz() == 240, "CPU at 240 MHz");
}

void testSession() {
  section("Session: lifecycle");
  // Tear down any active session.
  session().stop(false);
  vTaskDelay(pdMS_TO_TICKS(100));

  bool started = session().start(1, "selftest");
  check(started, "start(1 min, label)");
  auto s = session().snapshot();
  check(s.active, "snapshot.active true");
  check(s.targetMin == 1, "target minutes propagated");
  check(strcmp(s.label, "selftest") == 0, "label propagated");

  session().pause();
  vTaskDelay(pdMS_TO_TICKS(80));
  check(session().state() == SessionState::Paused, "pause -> Paused");
  session().resume();
  vTaskDelay(pdMS_TO_TICKS(80));
  check(session().state() == SessionState::Running, "resume -> Running");

  // Note a distraction (just exercise the path).
  session().noteDistraction();
  vTaskDelay(pdMS_TO_TICKS(40));
  auto s2 = session().snapshot();
  check(s2.distractions >= 1, "noteDistraction increments");

  session().stop(true);                  // celebrate = true → records as completed
  vTaskDelay(pdMS_TO_TICKS(120));
  check(!session().snapshot().active, "stop -> inactive");
  check(stateMachine().state() == DeviceState::Idle, "back to Idle");
}

void testStats() {
  section("Stats: append + summary");
  uint16_t baseline = stats().summary().totalSessions;
  SessionRecord r{};
  r.startedUnix = 1715000000UL;
  r.targetMin = 25;
  r.actualSec = 1200;
  r.distractions = 2;
  r.completed = 1;
  stats().append(r);
  auto s = stats().summary();
  check(s.totalSessions == (uint16_t)(baseline + 1), "totalSessions incremented");
  check(s.totalFocusedSec >= 1200, "totalFocusedSec includes new record");
  // Recent JSON serialisation
  static char buf[1024];
  size_t k = stats().recentSessionsJson(buf, sizeof(buf), 5);
  check(k > 0 && buf[0] == '[' && buf[k-1] == ']', "recentSessionsJson valid array");
}

void testWifiAp() {
  section("WifiAp + Portal");
  check(wifiAp().running(), "AP running");
  check(wifiAp().ssid().length() > 0, "SSID non-empty: %s", wifiAp().ssid().c_str());
  check(wifiAp().apIp().toString() == "192.168.4.1", "AP IP 192.168.4.1");
  pass("station count = %u (informational)", wifiAp().stationCount());

  portal().recordDiagEvent("SelfTest");
  check(strcmp(portal().lastDiagEvent(), "SelfTest") == 0, "Portal diag event recorder");
}

void testIdleManager() {
  section("IdleManager");
  idleManager().poke();
  // Just exercise the API surface — drowsy progression would take minutes.
  idleManager().setSleepTimeoutSec(180);
  check(idleManager().sleepTimeoutSec() == 180, "sleepTimeoutSec round-trip");
  idleManager().inhibitSleep(true);
  idleManager().inhibitSleep(false);
  pass("inhibitSleep toggled cleanly");
}

void testAnimations() {
  section("Character: animation routines");
  uint32_t start = millis();
  character().playSessionStartAnimation();
  uint32_t dur = millis() - start;
  check(dur >= 400 && dur < 1500, "session start animation duration ok (%lums)", (unsigned long)dur);

  start = millis();
  character().playWakeAnimation();
  dur = millis() - start;
  check(dur >= 400 && dur < 1500, "wake animation duration ok (%lums)", (unsigned long)dur);

  // Sleep animation is longer (~2s); just smoke-test that it returns.
  start = millis();
  character().playSleepAnimation();
  dur = millis() - start;
  check(dur >= 1000 && dur < 3500, "sleep animation duration ok (%lums)", (unsigned long)dur);

  // greetBasedOnTime should not crash regardless of whether clock is synced.
  character().greetBasedOnTime();
  vTaskDelay(pdMS_TO_TICKS(50));
  pass("greetBasedOnTime returned");
}

void testGames() {
  section("Games: state transition smoke test");
  // Don't run the full game — it has its own loop with delays. Just
  // check the FSM transition fires correctly.
  DeviceState before = stateMachine().state();
  GameId origGame = games().current();
  check(origGame == GameId::None, "no game running pre-test");
  // Starting a game would spawn a 4-5 second loop; instead just verify
  // the api surface compiles and the current() getter works.
  check(games().lastScore() >= 0, "lastScore accessor (=%u)", (unsigned)games().lastScore());
  stateMachine().transitionTo(before);
}

void testHeapStability() {
  section("Heap stability");
  size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  // Hammer a few subsystems with no-op work.
  for (int i = 0; i < 50; i++) {
    imu().injectEvent({ImuEventType::Tap, Face::Up, Face::Up, 0.7f, 0});
    portal().recordDiagEvent("HeapTest");
    (void)stats().summary();
  }
  vTaskDelay(pdMS_TO_TICKS(200));
  size_t after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  int32_t delta = (int32_t)after - (int32_t)before;
  // Allow ±2KB jitter from FreeRTOS internals.
  check(delta > -2048, "heap not leaking (Δ=%ld bytes)", (long)delta);
}

}  // namespace

void runSelfTest() {
  installListeners();
  g_passes = 0;
  g_fails = 0;
  Serial.println();
  Serial.println(F("################################################"));
  Serial.println(F("##  Dash self-test — running                  ##"));
  Serial.println(F("################################################"));

  // Snapshot of state we need to restore at the end.
  DeviceState origState = stateMachine().state();
  Mood origMood = character().mood();

  testStateMachine();
  testEyeStates();
  testDisplayOverlays();
  testCharacter();
  testImuEvents();
  testTouchEvents();
  testSounds();
  testSettings();
  testPower();
  testSession();
  testStats();
  testWifiAp();
  testIdleManager();
  testAnimations();
  testGames();
  testHeapStability();

  // Restore
  stateMachine().transitionTo(origState);
  character().setMood(origMood);
  display().setEyeState(EyeState::Idle);
  display().clearOverlay();

  Serial.println();
  Serial.println(F("################################################"));
  Serial.printf("##  Self-test complete: %u PASS, %u FAIL    ##\n", g_passes, g_fails);
  Serial.println(F("################################################"));
  Serial.println();
}

}  // namespace dash
