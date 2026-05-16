// esp_now_dash.h — ESP-NOW peer discovery + group session sync.
//
// All Dashes broadcast a small Presence beacon every 2s while in Group Study
// mode. The receive callback queues frames into a worker task — never blocks.
//
// Wire protocol (little-endian on the ESP32):
//   uint8_t  magic[4]    = 'D','S','H','1'
//   uint8_t  type         (PresenceBeacon | RoomInvite | SessionStart |
//                          Heartbeat | SessionEnd)
//   uint8_t  flags
//   uint16_t seq
//   uint32_t deviceId      (last 4 bytes of MAC)
//   uint8_t  payload[]
//
// Channel: locked at 6 to coexist with the dash AP. If STA mode is needed
// (OTA), the caller pauses ESP-NOW.

#ifndef DASH_ESP_NOW_DASH_H
#define DASH_ESP_NOW_DASH_H

#include <Arduino.h>
#include <freertos/queue.h>

namespace dash {

enum class EnMsgType : uint8_t {
  Presence = 1,
  RoomInvite,
  SessionStart,
  Heartbeat,
  SessionEnd,
};

struct EnMessage {
  uint8_t  src[6];
  EnMsgType type;
  uint32_t deviceId;
  uint8_t  payload[32];
  uint8_t  payloadLen;
};

struct DashPeer {
  uint32_t deviceId;
  uint8_t  mac[6];
  uint32_t lastSeenMs;
};

class EspNowDash {
 public:
  EspNowDash();

  bool begin();
  void stop();
  bool running() const { return running_; }

  // Broadcast a presence beacon. Returns true if queued for TX.
  bool sendPresence();
  bool sendRoomInvite();
  bool sendHeartbeat(uint16_t elapsedSec, uint16_t targetSec);

  uint8_t peerCount() const { return peerCount_; }
  const DashPeer& peer(uint8_t i) const { return peers_[i]; }

 private:
  static void onRecvStatic(const uint8_t* mac, const uint8_t* data, int len);
  void onRecv(const uint8_t* mac, const uint8_t* data, int len);
  void prunePeers();

  static void workerTrampoline(void* arg);
  void worker();

  QueueHandle_t inbox_;
  TaskHandle_t workerTask_;
  volatile bool running_;

  DashPeer peers_[8];
  uint8_t peerCount_;
};

EspNowDash& espNow();

}  // namespace dash

#endif
