# Captive Portal HTTP API

Base URL: `http://192.168.4.1` (Dash's AP IP).

All responses are `application/json` unless otherwise noted. Requests with a
body must send `content-type: application/json`. Errors use plain text with a
sensible HTTP status.

## Status

### `GET /api/status`

Returns the current device state, mood, current orientation, uptime, etc.

```json
{
  "firmware": "0.1.0",
  "state": "Idle",
  "mood": 0,
  "face": "Up",
  "uptime_ms": 245890,
  "boot_count": 3,
  "name": "Dash"
}
```

`state` values: `Booting | Onboarding | Idle | Drowsy | Asleep |
AwakeForSession | InSession | InMenu | InGame | GroupSessionWaiting |
GroupSessionActive | OtaChecking`.

`mood` is the integer value of `dash::Mood` (0=Neutral, 1=Focused, 2=Excited,
3=Tired, 4=Listening, 5=Playful).

## Time sync

### `POST /api/time-sync`

Body: `{ "unix_ms": <number>, "tz_min": <number> }`.

The phone JS calls this on every page load with `Date.now()` + the local tz
offset. Dash stores both in NVS for stats timestamps and OTA scheduling.

```json
200 { "ok": true }
```

## Config

### `GET /api/config`

```json
{
  "name": "Dash",
  "volume": 60,
  "sleep_timeout_s": 180,
  "session_minutes": 25,
  "onboarded": true
}
```

### `POST /api/config`

Partial update. Any subset of these keys may be provided:
`name | volume | sleep_timeout_s | session_minutes | home_ssid | home_password`.

Values are persisted to NVS immediately.

```json
200 { "ok": true }
```

## Session

### `GET /api/session`

```json
{
  "active": true,
  "state": 1,
  "elapsed_s": 312,
  "total_s": 1500,
  "distractions": 0,
  "label": "Drafting the proposal"
}
```

`state` matches `dash::SessionState`: 0=Idle, 1=Running, 2=Paused, 3=Completed.

### `POST /api/session`

Body: `{ "action": "start|pause|resume|stop", "minutes": <N>, "label": "..." }`.

`minutes` defaults to the saved session length when omitted. Returns `409` if
you try to start while one is already running.

## Stats

### `GET /api/stats`

```json
{
  "total_sessions": 12,
  "completed_sessions": 11,
  "total_focused_sec": 16320,
  "total_distractions": 5,
  "best_single_sec": 2700,
  "streak_days": 3,
  "recent": [
    {"u": 1715864400, "tm": 25, "as": 1480, "d": 0, "c": 1}
  ]
}
```

`recent` is the last 10 raw session records. Field keys are short to fit more
records under the 64 KiB rolling cap:
- `u` = startedUnix
- `tm` = targetMin
- `as` = actualSec
- `d` = distractions
- `c` = completed (1 = ran to natural completion, 0 = manual stop)

### `DELETE /api/stats`

Wipes `/stats/sessions.ndjson`. Returns `{"ok": true}`.

## Onboarding

### `GET /api/onboarding`

```json
{ "onboarded": true, "name": "Dash", "home_wifi_set": true }
```

### `POST /api/onboarding`

Partial update. Any subset of:
- `name`: device name string
- `home_ssid`, `home_password`: STA-mode credentials for OTA
- `complete`: bool, `true` flips the `onboarded` flag and transitions to Idle
- `reset`: bool, `true` clears `onboarded` and goes back to Onboarding state

## OTA

### `POST /api/ota/check`

Triggers the OTA flow synchronously. Returns `202` immediately, then runs
`fetch_latest_tag → compare → download → flash → restart` in the HTTP task.
If the update succeeds, Dash reboots and the phone loses the AP briefly.
On `UpToDate` / failure, the AP is brought back up automatically.

## Group study (ESP-NOW)

### `GET /api/group`

```json
{
  "running": true,
  "peers": [
    {"id": "f221a4", "last_seen_ms": 1430}
  ]
}
```

### `POST /api/group`

Body: `{ "action": "start|stop|invite" }`.

`start` brings up ESP-NOW alongside the AP, sends one presence beacon, and
begins listening. `stop` tears it down. `invite` broadcasts a RoomInvite
frame.

## Games

### `POST /api/game`

Body: `{ "action": "start|stop", "game": "reaction|bopit" }`.

### `GET /api/game`

```json
{ "current": 1, "last_score": 4200 }
```

`current` matches `dash::GameId`: 0=None, 1=Reaction, 2=BopIt.

## Factory reset

### `POST /api/factory-reset`

Wipes all NVS namespaces + stats log, then reboots. Returns
`{"ok":true,"rebooting":true}`. Use this to recover from a wedged onboarding
state.

## Easter egg

### `POST /api/easter-egg`

The Konami code in the portal triggers this. Dash flashes heart-eyes for 2.5 s.
Returns `{"ok":true}`.

## Captive-portal probes

Common phone-OS captive-portal probe paths (`/generate_204`, `/gen_204`,
`/hotspot-detect.html`, `/library/test/success.html`, `/connecttest.txt`,
`/ncsi.txt`, `/redirect`, `/success.txt`) all `302` to `/` so the phone reliably
surfaces the portal page.

Any other unknown hostname is also redirected — this is what triggers most
phones' "this Wi-Fi requires a login" sheet.
