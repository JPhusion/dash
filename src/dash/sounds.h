// sounds.h — central registry of every Dash sound. One C string per sound so
// callers don't sprinkle "/sounds/boot.raw" string literals across the codebase.
// Filenames map 1:1 to the generators in tools/sounds/generate.py.

#ifndef DASH_SOUNDS_H
#define DASH_SOUNDS_H

#include <esp_random.h>

#include "dash/audio.h"

namespace dash::sounds {

inline constexpr const char* kBoot             = "/sounds/boot.raw";
inline constexpr const char* kWake             = "/sounds/wake.raw";
inline constexpr const char* kSleep            = "/sounds/sleep.raw";
inline constexpr const char* kSessionStart     = "/sounds/session_start.raw";
inline constexpr const char* kSessionEnd       = "/sounds/session_end.raw";
inline constexpr const char* kSessionComplete  = "/sounds/session_complete.raw";
inline constexpr const char* kTapAck           = "/sounds/tap_ack.raw";
inline constexpr const char* kTapAck2          = "/sounds/tap_ack_2.raw";
inline constexpr const char* kTapAck3          = "/sounds/tap_ack_3.raw";
inline constexpr const char* kDoubleTapAck     = "/sounds/double_tap_ack.raw";
inline constexpr const char* kTripleTapAck     = "/sounds/triple_tap_ack.raw";
inline constexpr const char* kTestTone         = "/sounds/test_tone.raw";
inline constexpr const char* kWhoa             = "/sounds/whoa.raw";
inline constexpr const char* kTilt             = "/sounds/tilt.raw";
inline constexpr const char* kBoop             = "/sounds/boop.raw";
inline constexpr const char* kSurprised        = "/sounds/surprised.raw";
inline constexpr const char* kAnnoyed          = "/sounds/annoyed.raw";
inline constexpr const char* kConfused         = "/sounds/confused.raw";
inline constexpr const char* kDizzy            = "/sounds/dizzy.raw";
inline constexpr const char* kCurious          = "/sounds/curious.raw";
inline constexpr const char* kGoodMorning      = "/sounds/good_morning.raw";
inline constexpr const char* kMilestone        = "/sounds/milestone.raw";
inline constexpr const char* kMenuBlip         = "/sounds/menu_blip.raw";
inline constexpr const char* kMenuConfirm      = "/sounds/menu_confirm.raw";
inline constexpr const char* kMenuBack         = "/sounds/menu_back.raw";
inline constexpr const char* kDistraction      = "/sounds/distraction.raw";
inline constexpr const char* kEncouragement    = "/sounds/encouragement.raw";
inline constexpr const char* kYawn             = "/sounds/yawn.raw";
inline constexpr const char* kGiggle           = "/sounds/giggle.raw";
inline constexpr const char* kHeartbeat        = "/sounds/heartbeat.raw";
inline constexpr const char* kGameCorrect      = "/sounds/game_correct.raw";
inline constexpr const char* kGameWrong        = "/sounds/game_wrong.raw";
inline constexpr const char* kGameStart        = "/sounds/game_start.raw";

inline bool play(const char* path, bool exclusive = false) {
  return dash::audio().play(path, dash::AudioFormat::Pcm8kHzMono8, exclusive);
}

// Consistent touch acknowledgement — always the same chirp so users can
// learn the sound and trust it. (We used to randomise across three
// variants, but consistency turned out to matter more than variety here.)
inline bool playTapAck() {
  return play(kTapAck);
}

}  // namespace dash::sounds

#endif
