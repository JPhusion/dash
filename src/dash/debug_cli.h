// debug_cli.h — line-based serial command interpreter for stand-in user
// input during development. Reads bytes from Serial, parses commands when
// a newline arrives, and dispatches to the rest of the firmware.
//
// Commands:
//   help                          — print this list
//   status                        — current state, mood, heap, face
//   tap [magnitude]               — inject an IMU Tap event
//   dt                            — inject DoubleTap
//   tt                            — inject TripleTap
//   shake                         — inject Shake
//   tilt up|down|left|right|front|back|unknown — inject OrientationChange
//   touch | dtouch | lptouch      — inject Touch / DoubleTouch / LongPress
//   eye <0..19>                   — set eye state by index
//   mood <0..5>                   — set Character mood
//   sound <name>                  — play a sound (e.g. boot, tilt, whoa)
//   vol <0..100>                  — set volume (also re-applies to audio)
//   silent on|off                 — toggle silent mode
//   session start [minutes]       — start a session (default minutes)
//   session stop                  — stop the active session
//   session pause|resume          — pause/resume
//   session snap                  — print current session snapshot
//   state <name>                  — force state-machine transition
//   sleep                         — call enterDeepSleep (no-op in debug build)
//   stats                         — print stats summary
//   stats fake [n]                — inject N synthetic completed-session records
//   stats reset                   — wipe the stats log
//   heap                          — heap snapshot
//   ap                            — wifi ap info (ssid, ip, stations)
//   diag-event                    — record / read diag event
//   selftest                      — run the full automated self-test suite
//
// Lines starting with `#` are echoed and ignored (comments in scripts).

#ifndef DASH_DEBUG_CLI_H
#define DASH_DEBUG_CLI_H

#include <Arduino.h>

namespace dash {

class DebugCli {
 public:
  DebugCli();
  void begin();
  void start();
  void stop();

  // Run a single command line directly (used by selftest harness too).
  void dispatch(const String& line);

 private:
  static void taskTrampoline(void* arg);
  void loop();

  TaskHandle_t task_;
  volatile bool running_;
  String buf_;
};

DebugCli& debugCli();

}  // namespace dash

#endif
