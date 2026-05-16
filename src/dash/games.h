// games.h — minigames accessed via gesture from the Idle state.
//
// Trigger: shake-while-touching the cube (or via the portal). Two games for
// v1: Reaction Time (face shows a colour change, user taps cube as fast as
// possible) and Bop It (cube says "tap me / shake me / flip me", user obeys
// before a shrinking timer expires).

#ifndef DASH_GAMES_H
#define DASH_GAMES_H

#include <Arduino.h>

namespace dash {

enum class GameId : uint8_t {
  None,
  Reaction,
  BopIt,
};

class Games {
 public:
  Games();

  void begin();      // subscribes to IMU + Touch events
  void startGame(GameId id);
  void stopGame();
  GameId current() const { return current_; }

  // Surface latest score for the portal / display.
  uint32_t lastScore() const { return lastScore_; }

 private:
  static void taskTrampoline(void* arg);
  void loop();
  void runReaction();
  void runBopIt();

  TaskHandle_t task_;
  volatile GameId current_;
  volatile uint32_t lastScore_;
  volatile uint32_t actionMs_;       // millis() when the current prompt fired
  volatile bool actionConsumed_;
  volatile uint8_t expectedAction_;  // 0=tap, 1=shake, 2=flip-up, 3=flip-down
};

Games& games();

}  // namespace dash

#endif
