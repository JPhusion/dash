# Dash Architecture

## What Dash is

A cube-shaped focus companion. ESP32-WROOM at the core, MPU-6050 IMU, SH1106
OLED, I2S amplifier, single capacitive-touch pin, USB-C charging.

The cube hosts a Wi-Fi AP + captive portal so the user pairs it to their phone
without an account or app install. Sessions are timed focus blocks; Dash
animates its eyes in response to gestures and session state, and (optionally)
talks to other nearby Dashes over ESP-NOW for group study.

## Top-level module map

All Dash code lives under `src/dash/` in the `dash::` namespace. One `.h` +
one `.cpp` per module, exposing a `&moduleName()` singleton getter.

```
                       ┌────────────────────────────┐
                       │     dash::StateMachine     │   FSM owner; emits
                       │  (Booting / Idle / etc.)   │   transition events.
                       └────────────────────────────┘
                                    ▲ subscribes
            ┌───────────────────────┼───────────────────────┐
            │                       │                       │
   ┌────────────────┐   ┌────────────────────┐    ┌────────────────┐
   │ dash::Power    │   │ dash::IdleManager  │    │ dash::Session  │
   │ cpu freq + deep│   │ drowsy clock + 80  │    │ timer, distract│
   │ sleep gateway  │   │ MHz / 240 MHz swap │    │ count, RTC mem │
   └────────────────┘   └────────────────────┘    └────────────────┘

   ┌────────────────┐   ┌────────────────────┐    ┌────────────────┐
   │ dash::Imu      │   │ dash::Touch        │    │ dash::Audio    │
   │ MPU-6050 + Mad │   │ T7 cap-touch +     │    │ I2S amp +      │
   │ wick + tap FSM │   │ baseline calibr.   │    │ littlefs files │
   └────────────────┘   └────────────────────┘    └────────────────┘
            │                       │                       │
            └────────────┬──────────┴───────────┬───────────┘
                         │ events                │ playback
                ┌────────▼─────────┐    ┌────────▼────────┐
                │ dash::Character  │    │ dash::Display   │
                │ mood + reactions │    │ wraps lib/eyes/ │
                │ idle quirks      │    │ overlays/text   │
                └────────┬─────────┘    └────────┬────────┘
                         │ drives                │ render
                         └────────────┬──────────┘
                                      ▼
                              physical OLED + speaker

   ┌──────────────────┐  ┌────────────────┐   ┌────────────────────┐
   │ dash::WifiAp     │  │ dash::Portal   │   │ dash::EspNowDash   │
   │ softAP ch 6,     │  │ HTTP routes    │   │ peer discovery,    │
   │ DNS captive      │  │ static + api/* │   │ session sync       │
   └──────────────────┘  └────────────────┘   └────────────────────┘

   ┌────────────────┐  ┌──────────────────┐  ┌────────────────────┐
   │ dash::Settings │  │ dash::Stats      │  │ dash::Ota          │
   │ NVS-backed     │  │ ndjson rolling   │  │ github releases    │
   │ config getters │  │ session log      │  │ sha256 verified    │
   └────────────────┘  └──────────────────┘  └────────────────────┘

   ┌────────────────┐
   │ dash::Games    │  reaction-time, bop-it
   └────────────────┘
```

## Threading

| Task            | Core | Priority | Owner            |
|-----------------|------|----------|------------------|
| `display`       | 1    | 1        | dash::Display    |
| `imu-sample`    | 1    | 3        | dash::Imu        |
| `imu-event`     | 1    | 2        | dash::Imu        |
| `audio`         | 1    | 1        | dash::Audio      |
| `touch`         | 0    | 1        | dash::Touch      |
| `char`          | 1    | 1        | dash::Character  |
| `session`       | 0    | 1        | dash::Session    |
| `idle`          | 0    | 1        | dash::IdleManager|
| `dns`           | 0    | 1        | dash::WifiAp     |
| `http`          | 0    | 1        | dash::WifiAp     |
| `espnow worker` | 0    | 1        | dash::EspNowDash |
| `game`          | 1    | 1        | dash::Games (transient) |

Core 1 hosts rendering + audio + character + IMU; core 0 hosts Wi-Fi, DNS,
HTTP, ESP-NOW. The Wi-Fi stack itself runs on core 0 (default in
Arduino-ESP32), so we keep latency-sensitive things off core 0 where possible.

## State machine

`dash::DeviceState` covers the top-level mode. Transitions log every change.
Listener pattern lets any module react to state changes.

```
Booting ─▶ Onboarding ─▶ Idle ◀──▶ Drowsy ─▶ Asleep (deep sleep)
            │
            └──▶ Idle
                 ▲ ▲ ▲ ▲ ▲
                 │ │ │ │ │
                 │ │ │ │ └ InGame   (gesture trigger / portal)
                 │ │ │ └─── InMenu  (touch long-press)
                 │ │ └───── OtaChecking (4 AM wake or portal action)
                 │ └─────── InSession  (portal action / double-tap)
                 └───────── GroupSession{Waiting,Active}
```

## Power

- **240 MHz** — Performance. Active states, sessions, games, OTA.
- **80 MHz** — Low-power. Drowsy 1+, Asleep entry, post-session quiet.
- **Deep sleep** — disabled in `dash-debug`. In `dash-release`, triggered by
  IdleManager hitting 100% of timeout, by triple-tap gesture, or by face-down
  for >2 s.

Wake sources: capacitive touch (T7), timer (for nightly OTA), and IMU motion
INT (when wired on v2 hardware — not v1).

## Persistence

| Storage              | Used for                                    |
|----------------------|---------------------------------------------|
| **RTC slow memory**  | boot count, last sleep unix, in-progress session recovery, last OTA check time |
| **NVS** (`dash.cfg`) | settings: device name, audio volume, sleep timeout, home Wi-Fi credentials, last unix, tz offset, onboarded flag |
| **NVS** (`dash.imu`) | gyro bias (X, Y, Z) saved opportunistically when stationary |
| **LittleFS**         | `/sounds/*.raw`, `/web/*` (portal assets), `/stats/sessions.ndjson` (rolling session log) |

Encryption is **deferred** — see ADR-005. Home Wi-Fi password is currently
stored in plaintext NVS.

## Wi-Fi & RF

- AP locked to channel 6, 11 dBm TX, open SSID `Dash-XXXX`.
- DNS server redirects every query to the AP IP (192.168.4.1) → captive
  portal trigger on every modern phone OS.
- STA mode (for OTA) tears the AP down briefly. Returns to AP after.
- ESP-NOW also on channel 6, broadcast-only discovery, up to 8 paired peers.

## What's intentionally simple

- Single Wire instance shared between IMU and OLED (no separate buses).
- No RTOS queues for inter-module events beyond what each module needs.
- No JSON parser in the hot path of `/api/status` — `snprintf` straight to a
  char buffer. Larger handlers (`/api/config`, `/api/onboarding`) use
  ArduinoJson because they have nested fields.
- No HTTPS on the AP — the phone is in arm's reach; the portal carries no
  secrets in transit. OTA over STA uses HTTPS via `WiFiClientSecure`.
- One state machine, not one per module — keeps invariants simple.

## What's left for v2

- Real ECDSA signature verification on OTA images
- NVS encryption (with `nvs_keys` partition + custom bootloader)
- IMU motion-wake INT pin wired to a RTC GPIO
- Battery voltage divider for fuel-gauge estimation
- A 32 kHz crystal for accurate deep-sleep clocking (today's RTC drifts ±5%)
- Real ESP-NOW peer encryption with paired LMK
