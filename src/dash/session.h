// session.h — Dash study session controller.
//
// A session is a fixed-duration focused-work block. While it's running:
// - State machine reads InSession (inhibits idle → sleep progression)
// - Character mood is Focused
// - DNS server captures every name lookup from connected phones; when
//   non-whitelisted domains are hit too frequently, Dash reacts (sad eyes,
//   distraction sound) but does NOT block — Dash is a companion, not a nanny.
// - On completion: celebration animation + completion chime + stats entry.
// - On manual end: short acknowledgement + partial-credit stats entry.
//
// Crash recovery: at session start we write a small struct to RTC memory so
// that a brownout / panic mid-session can resume after reboot.

#ifndef DASH_SESSION_H
#define DASH_SESSION_H

#include <Arduino.h>

namespace dash {

enum class SessionState : uint8_t {
  Idle,
  Running,
  Paused,
  Completed,
};

struct SessionSnapshot {
  bool active;
  SessionState state;
  uint32_t startedAtMs;     // millis() when started
  uint32_t pausedMs;        // total time spent paused, ms
  uint16_t targetMin;       // session length goal in minutes
  uint16_t distractions;
};

extern RTC_DATA_ATTR uint32_t rtcSessionTargetMin;
extern RTC_DATA_ATTR uint32_t rtcSessionStartedUnix;
extern RTC_DATA_ATTR uint8_t  rtcSessionDirty;   // 1 = active when last sleep happened

class Session {
 public:
  Session();

  void begin();

  // Start a session of N minutes. If a session is already running, this is a
  // no-op (the portal returns 409 — caller handles).
  bool start(uint16_t minutes);

  // Cooperative pause / resume.
  void pause();
  void resume();

  // Stop the session early. Records distractions and partial time to stats.
  void stop(bool celebrate = false);

  SessionSnapshot snapshot() const;
  SessionState state() const { return state_; }

  // Called by DNS server every time a phone resolves a hostname during a
  // session. Distraction-domain heuristic lives in distractions.cpp; this
  // call just notes the event.
  void noteDistraction();

  // Tick: called from a session task every second; checks completion and
  // posts UI updates.
  void tick();

  // RTC recovery: returns true if a session was interrupted by an unexpected
  // restart and the caller (Portal) should offer recovery.
  bool hasRecoverableSession() const;
  void clearRecoverableSession();

 private:
  void enterState(SessionState s);

  SessionState state_;
  uint32_t startedAtMs_;
  uint32_t pausedAtMs_;     // millis when current pause began
  uint32_t accumulatedPauseMs_;
  uint16_t targetMin_;
  uint16_t distractions_;

  TaskHandle_t task_;
  static void taskTrampoline(void* arg);
  void loop();
};

Session& session();

}  // namespace dash

#endif
