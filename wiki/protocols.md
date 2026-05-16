# Wire Protocols

## Captive portal (HTTP)

See `wiki/api/portal.md` for the full HTTP API. TL;DR:

- Soft AP `Dash-XXXX` on channel 6, open SSID, 11 dBm TX.
- DNS server (port 53) redirects every name lookup to `192.168.4.1`.
- HTTP server (port 80) serves `/web/*` from LittleFS and exposes `/api/*`.
- All bodies are JSON. Responses are JSON unless explicitly otherwise.

## ESP-NOW frame format

Every Dash → Dash frame is fixed-size (44 bytes payload), little-endian on
ESP32:

```
offset  size  field         description
0       4     magic         literal "DSH1"
4       1     type          EnMsgType (1=Presence, 2=RoomInvite,
                                       3=SessionStart, 4=Heartbeat, 5=SessionEnd)
5       1     flags         reserved
6       2     seq           per-sender monotonic sequence
8       4     deviceId      last 4 bytes of sender MAC, big-endian uint32
12      32    payload       type-specific
44      1     payloadLen    bytes of payload actually used
```

**All frames are broadcast to FF:FF:FF:FF:FF:FF** — no peer pairing required
for v1. Each Dash ignores its own deviceId on receive.

### Payloads by type

- `Presence` — empty (`payloadLen = 0`). Sent every 2 s while in Group Study
  mode.
- `RoomInvite` — empty for v1.
- `SessionStart` — `payload[0..1]` = target session minutes, `payload[2..5]` =
  unix start time as little-endian uint32. (Not yet emitted; format reserved.)
- `Heartbeat` — `payload[0..1]` = elapsed seconds, `payload[2..3]` = target
  seconds. Sent every 5 s during an active group session.
- `SessionEnd` — empty.

### Channel coordination

All Dashes use channel 6. STA-mode operations (M9 OTA) pause ESP-NOW via
`EspNowDash::stop()` and resume it after the STA flow completes.

If a Dash is hosting an AP and a user device is connected, the AP/STA radio
state is on channel 6 and ESP-NOW can coexist without retuning.

## DNS server

Single-bind UDP listener on port 53. Every query is answered with an A record
pointing at 192.168.4.1, regardless of qname. Reply code is `NoError` so the
phone OS treats the response as authoritative and triggers the captive-portal
sheet.

The server task yields every 10 ms (`vTaskDelay`) to keep the watchdog happy
between bursts of queries.

## Time / clock

The cube has no RTC and no 32 kHz crystal — RTC uses the internal 150 kHz RC
oscillator (~5% accuracy). For session timestamps, the phone sends the current
wall clock via `POST /api/time-sync` on every portal page load. Dash persists
the last known unix + tz offset in NVS.

The same persisted unix is used to schedule the nightly OTA wake.
