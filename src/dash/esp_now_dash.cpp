#include "dash/esp_now_dash.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "dash/log.h"

namespace dash {

namespace {
constexpr const char* kTag = "EspNow";
constexpr uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr uint8_t kMagic[4] = {'D', 'S', 'H', '1'};
constexpr uint8_t kChannel = 6;
EspNowDash* g_singleton = nullptr;

#pragma pack(push, 1)
struct Frame {
  uint8_t magic[4];
  uint8_t type;
  uint8_t flags;
  uint16_t seq;
  uint32_t deviceId;
  uint8_t payload[32];
  uint8_t payloadLen;
};
#pragma pack(pop)

uint32_t computeDeviceId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  return ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
         ((uint32_t)mac[4] << 8) | mac[5];
}

uint16_t g_seq = 0;
uint32_t g_deviceId = 0;
}  // namespace

EspNowDash::EspNowDash()
    : inbox_(xQueueCreate(8, sizeof(EnMessage))),
      workerTask_(nullptr), running_(false), peerCount_(0) {}

bool EspNowDash::begin() {
  if (running_) return true;

  // Initialise WiFi in STA mode so ESP-NOW can use the underlying radio.
  // AP mode also works but we coordinate with WifiAp for channel selection.
  if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
  // Lock channel to 6 for cross-Dash coexistence.
  esp_wifi_set_channel(kChannel, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    log::error(kTag, "esp_now_init failed");
    return false;
  }
  esp_now_register_recv_cb(&EspNowDash::onRecvStatic);

  // Add the broadcast pseudo-peer so esp_now_send to FFFFFF works.
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kBroadcast, 6);
  peer.channel = kChannel;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  g_deviceId = computeDeviceId();
  running_ = true;
  xTaskCreatePinnedToCore(&EspNowDash::workerTrampoline, "espnow", 4096, this,
                          1, &workerTask_, 0);
  log::info(kTag, "up on channel %u, deviceId=%08x", kChannel, (unsigned)g_deviceId);
  return true;
}

void EspNowDash::stop() {
  if (!running_) return;
  running_ = false;
  esp_now_unregister_recv_cb();
  esp_now_deinit();
  log::info(kTag, "down");
}

bool EspNowDash::sendPresence() {
  if (!running_) return false;
  Frame f{};
  memcpy(f.magic, kMagic, 4);
  f.type = (uint8_t)EnMsgType::Presence;
  f.seq = g_seq++;
  f.deviceId = g_deviceId;
  f.payloadLen = 0;
  return esp_now_send(kBroadcast, (const uint8_t*)&f, sizeof(f)) == ESP_OK;
}

bool EspNowDash::sendRoomInvite() {
  if (!running_) return false;
  Frame f{};
  memcpy(f.magic, kMagic, 4);
  f.type = (uint8_t)EnMsgType::RoomInvite;
  f.seq = g_seq++;
  f.deviceId = g_deviceId;
  f.payloadLen = 0;
  return esp_now_send(kBroadcast, (const uint8_t*)&f, sizeof(f)) == ESP_OK;
}

bool EspNowDash::sendHeartbeat(uint16_t elapsedSec, uint16_t targetSec) {
  if (!running_) return false;
  Frame f{};
  memcpy(f.magic, kMagic, 4);
  f.type = (uint8_t)EnMsgType::Heartbeat;
  f.seq = g_seq++;
  f.deviceId = g_deviceId;
  memcpy(f.payload + 0, &elapsedSec, 2);
  memcpy(f.payload + 2, &targetSec, 2);
  f.payloadLen = 4;
  return esp_now_send(kBroadcast, (const uint8_t*)&f, sizeof(f)) == ESP_OK;
}

void EspNowDash::onRecvStatic(const uint8_t* mac, const uint8_t* data, int len) {
  if (g_singleton) g_singleton->onRecv(mac, data, len);
}

void EspNowDash::onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len < (int)sizeof(Frame) - 32) return;
  const Frame* f = (const Frame*)data;
  if (memcmp(f->magic, kMagic, 4) != 0) return;
  if (f->deviceId == g_deviceId) return;   // ignore our own broadcasts

  EnMessage msg{};
  memcpy(msg.src, mac, 6);
  msg.type = (EnMsgType)f->type;
  msg.deviceId = f->deviceId;
  msg.payloadLen = f->payloadLen <= 32 ? f->payloadLen : 32;
  memcpy(msg.payload, f->payload, msg.payloadLen);
  // Non-blocking enqueue — drop oldest if full.
  if (xQueueSend(inbox_, &msg, 0) != pdTRUE) {
    EnMessage drop;
    xQueueReceive(inbox_, &drop, 0);
    xQueueSend(inbox_, &msg, 0);
  }
}

void EspNowDash::prunePeers() {
  uint32_t now = millis();
  uint8_t kept = 0;
  for (uint8_t i = 0; i < peerCount_; i++) {
    if (now - peers_[i].lastSeenMs < 10000) {
      if (i != kept) peers_[kept] = peers_[i];
      kept++;
    }
  }
  peerCount_ = kept;
}

void EspNowDash::workerTrampoline(void* arg) {
  static_cast<EspNowDash*>(arg)->worker();
}

void EspNowDash::worker() {
  EnMessage msg;
  while (running_) {
    if (xQueueReceive(inbox_, &msg, pdMS_TO_TICKS(500)) == pdTRUE) {
      // Find or insert peer.
      uint32_t now = millis();
      bool found = false;
      for (uint8_t i = 0; i < peerCount_; i++) {
        if (peers_[i].deviceId == msg.deviceId) {
          peers_[i].lastSeenMs = now;
          found = true;
          break;
        }
      }
      if (!found && peerCount_ < 8) {
        peers_[peerCount_].deviceId = msg.deviceId;
        memcpy(peers_[peerCount_].mac, msg.src, 6);
        peers_[peerCount_].lastSeenMs = now;
        peerCount_++;
        log::info(kTag, "new peer %08x", (unsigned)msg.deviceId);
      }
    }
    prunePeers();
  }
  workerTask_ = nullptr;
  vTaskDelete(nullptr);
}

EspNowDash& espNow() {
  if (!g_singleton) g_singleton = new EspNowDash();
  return *g_singleton;
}

}  // namespace dash
