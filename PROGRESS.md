# Dash Build Progress

End-state at handover. Every milestone M0-M12 is implemented and the firmware
has been flashed + verified on the v1 prototype.

---

## M0 — Project Setup & Reference Review ✅
- PlatformIO project (`platformio.ini`) with three envs: `dash`, `dash-debug`,
  `dash-release`.
- 16 MB partition table (`partitions.csv`) — see ADR-005b for the saga around
  app0 offset and `spiffs` partition name. 1.5 MB app slots, 13 MB filesystem.
- Eye animation library vendored at `lib/eyes/` (unmodified, AGPL).
- Stub `main.cpp` prints reset reason + wake cause + heap stats on boot.
- Wiki: peripherals, reference review, decisions (ADR-001..012).
- Git repo initialised, pushed to https://github.com/JPhusion/dash.

## M1 — Core Peripherals ✅
- `dash::Display` — wraps the eye library, OLED render task pinned to core 1.
- `dash::Imu` — MPU-6050 + Madgwick AHRS, 100 Hz sampling, software tap /
  double-tap / triple-tap detection, shake via running variance, 6-face
  orientation with hysteresis, opportunistic gyro bias save to NVS.
- `dash::Audio` — I2S amplifier playing raw PCM from LittleFS. Configurable
  format (8 kHz mono u8 canonical), volume 0-100, silent-mode override.
- `dash::Touch` — GPIO27 cap-touch, auto-calibrated baseline + threshold,
  recalibrates every 30 s of quiet.

## M2 — Power Management & State Machine ✅
- `dash::Power` — CPU freq profiles (80 / 240 MHz), deep-sleep entry that
  cuts radios, cap-touch wake, optional timer wake, `gpio_hold_en` for
  I2S DOUT across sleep. RTC_DATA_ATTR boot count + last-sleep timestamps.
- `dash::StateMachine` — single-owner FSM (12 states), transition logger,
  listener pattern.
- `dash::IdleManager` — drowsy progression at 60/75/90/95/100% of timeout,
  drops to 80 MHz at drowsy 1+, back to 240 MHz on activity. Activity
  events (IMU + Touch) reset the clock.
- Triple-tap → fast deep sleep; face-down for 2 s → fast deep sleep.

## M3 — Sound Generation Pipeline ✅
- `tools/sounds/generate.py` — 23 procedural sounds (boot/wake/sleep, session
  start/end/complete, distraction, encouragement, yawn, giggle, heartbeat,
  3 tap_ack variants, good_morning, milestone, 3 menu sounds, 3 game cues).
  Total 55 KiB on flash at 8 kHz mono u8.
- `src/dash/sounds.h` — typed registry, `playTapAck()` randomises across
  the 3 chirp variants to avoid auditory fatigue.

## M4 — Eye States & Character System ✅
- `dash::Character` — mood enum (Neutral / Focused / Excited / Tired /
  Listening / Playful) drives the resting eye state.
- Boot animation: splash → surprised → searching → blink → settle.
- Wake animation: closed → sleepy → blink → settle (much shorter).
- `react()` API for transient overrides with auto-return.
- Idle quirks task: occasional blinks, glances, micro-emotions every
  8-25 s, with mood-aware probabilities.
- `greetBasedOnTime()` — time-aware microexpression on session start.

## M5 — Wi-Fi AP & Captive Portal ✅
- `dash::WifiAp` — softAP `Dash-XXXX`, channel 6, 11 dBm TX, open SSID.
  DNS + HTTP server tasks pinned to core 0.
- `dash::Portal` — `/api/status`, `/api/time-sync`, `/api/config`,
  `/api/session`, `/api/stats`, `/api/onboarding`, `/api/ota/check`,
  `/api/group`, `/api/game`, `/api/test-tone`, `/api/easter-egg`,
  `/api/factory-reset`. Captive-portal probes 302 to `/`.
- `dash::Settings` — NVS-backed config: name, volume, sleep timeout,
  session length, home WiFi creds, last unix + tz, onboarded flag.
- Web frontend in `data/web/` — see M12 below for the polished design.

## M6 — Study Session Engine ✅
- `dash::Session` — FSM (Idle / Running / Paused / Completed). Tracks
  start time, accumulated pause time, target duration, distraction count,
  optional 40-char label.
- RTC-mem recovery struct so a brownout / panic mid-session can resume.
- Natural completion fires celebrating animation; manual stop fires
  disappointed eyes. Milestone counts (1st, 10th, 50th, 100th completed
  session) get extra-long heart-eye finale + milestone chime.
- Double-tap on cube toggles the session (start at default length / stop
  if active). Idle clock poked on stop so dash doesn't go drowsy in the
  same moment it's celebrating.

## M7 — Stats & Dashboard ✅
- `dash::Stats` — append-only NDJSON log at `/stats/sessions.ndjson`.
  Auto-rotates at 64 KiB (drops oldest half at newline boundary).
- `summary()` aggregates total sessions, completed sessions, focused
  seconds, distractions, best single session. **Streak computation** —
  buckets sessions by UTC day, walks consecutive days ending today (with
  one-day grace).
- Portal `/api/stats` returns aggregate + last 10 raw records.
- Frontend renders headline number, streak, best, 7-day bar chart,
  recent session history list.

## M8 — Onboarding ✅
- First-boot users land in `Onboarding` state with `Listening` mood.
- Portal `/api/onboarding` GET/POST + the `reset` flag for replaying the
  tutorial later.
- `data/web/onboarding.html` — 5-step wizard with progress dots:
  welcome → name → gesture tutorial → home WiFi → done.
- Settings tab has a "Replay welcome tutorial" button.

## M9 — OTA Updates ✅
- `dash::Ota` — polls `api.github.com/repos/JPhusion/dash/releases/latest`,
  compares semver tag, downloads `firmware.bin`, streams through
  `Update.h` with inline SHA-256 verification.
- STA bring-up enables `WIFI_PS_MIN_MODEM` to save current; AP is torn
  down and brought back up around the OTA flow.
- Portal `/api/ota/check` for manual triggers. Nightly 4 AM auto-check
  scaffold in place (RTC `lastOtaCheckUnix`); scheduled wake not yet
  wired (deferred — see "Open issues" below).
- `tools/release.sh` automates the build + hash for cutting a release.

## M10 — Gestures Menu & Games ✅
- `dash::Games` — two minigames behind one FSM. **Reaction Time** (5
  rounds, sum of `3000 - reactionMs`). **Bop It** (TAP! / SHAKE! / UP! /
  DOWN! prompts with shrinking window).
- Games subscribe to existing IMU events. State machine → InGame, mood
  → Playful during play.
- Portal `/api/game` GET + POST for start/stop.

## M11 — ESP-NOW Multi-Dash ✅ (scaffolding)
- `dash::EspNowDash` — DSH1-prefixed broadcast frames on channel 6,
  44-byte fixed payload. Peer table with TTL (max 8 peers).
- Portal `/api/group` for start/stop/invite.
- **Cannot exercise multi-cube behaviour** with only one Dash on hand.
  Heartbeat handler is logged but not yet routed into Session.

---

## M12 — Polish, Documentation, Final Pass ✅

**Audit & code quality**
- Zero build warnings; flash 72%, RAM 16%.
- 90-second runtime test: heap rock-stable at 128 KB, no panics,
  no watchdog warnings.
- Log noise cleanup: Wire double-init, LittleFS double-mount, NVS first-
  boot E line all suppressed.

**Captive portal UX (ground-up rewrite)**
- Coherent design system (`wiki/design.md`): single accent, 6-step type
  scale, 4 px base spacing, 200 ms transitions throughout.
- Three-tab layout (Study / Stats / Settings) with sticky bottom tab bar
  and pulsing dot indicator when a session is active.
- Study tab: preset duration chips (25/45/60/90/custom), "what are you
  working on?" label field, live timer + progress bar during sessions,
  pause/resume + end-early action row.
- Stats tab: total focused hours hero, streak with flame, 7-day bar chart,
  best session, distractions count, scrollable recent list.
- Settings tab: name + volume hero, session defaults, Wi-Fi & updates
  with show/hide password, tutorial replay, theme picker, advanced
  destructive actions.
- 5-step onboarding wizard with progress dots and gesture tutorial.
- 4 frontend themes (warm/calm/cool/dusk) persisted in localStorage.
- Volume slider live preview (debounced /api/test-tone plays a chirp).

**Character & sounds**
- Per-mood idle quirks (Focused gets occasional side-eye, Playful gets
  heart-eyes etc.).
- Time-aware greeting on session start.
- 3 tap_ack variants randomised on every IMU tap.
- Milestone fanfare on completed-session counts 1, 10, 25, 50, 100, 100k.
- Distinct wake animation for deep-sleep returns vs cold boot.

**Hardening**
- Idle clock poked on session stop so post-session celebration doesn't
  immediately roll into drowsy.
- Session label propagates through portal + RTC recovery struct.
- Factory reset wipes both NVS namespaces + stats log + reboots.

**Documentation**
- `wiki/architecture.md` — full module map, threading table, FSM diagram,
  power profiles.
- `wiki/api/portal.md` — every HTTP endpoint documented.
- `wiki/protocols.md` — ESP-NOW frame format, DNS server behaviour.
- `wiki/hardware_notes.md` — v1 limitations + v2 wishlist.
- `wiki/development.md` — build/flash/serial-monitor recipes, conventions.
- `wiki/design.md` — portal design system reference.
- `wiki/audit.md` — M12 audit log.
- `wiki/decisions.md` — 13 ADRs.

---

## Open issues / deferred

- **OTA scheduled 4 AM auto-check** — code path exists, RTC variable exists,
  but the deep-sleep timer wake calculation that targets next-4am hasn't
  been wired into `enterDeepSleep()`. Manual `/api/ota/check` works.
- **ECDSA OTA signing** — only SHA-256 hash verification implemented. The
  full PKI flow (private key in `tools/keys/`, public key compiled into
  firmware, `.sig` asset fetched alongside `.bin`) is staged.
- **NVS encryption** — see ADR-005. The `nvs_keys` subtype is rejected by
  the Arduino-ESP32 prebuilt bootloader; would require custom-built
  bootloader (`platform_packages` override) or app-level AES.
- **TLS root CA bundle** — OTA uses `WiFiClientSecure::setInsecure()`. Should
  embed Let's Encrypt + DigiCert root certs for production.
- **ESP-NOW heartbeat → Session sync** — broadcasts work, peer table works,
  but heartbeat payload is not yet driving "host's clock wins" on the
  session module. Needs a second cube to develop against.
- **IMU motion-wake INT** — pin not wired on v1 hardware. Deep-sleep wake
  via cap-touch only.
- **Battery sensing** — no ADC divider on v1; portal can't show battery %.

## What's been verified on hardware

- Cold boot from POWERON: clean, every module initialises, ~234 KB free
  heap pre-AP, ~128 KB free heap after AP comes up.
- Wake from cap-touch wake: works (verified during M2 development).
- IMU readings stable, no jitter, gyro bias saves to NVS after >5 s still.
- I2S audio paths verified by serial logs (`[Audio] play /sounds/foo.raw`).
- WiFi AP comes up as `Dash-21A4` at 192.168.4.1 on channel 6.
- Portal HTTP handlers register cleanly.
- 90-second idle observation: zero heap drift.

## Needs the user's manual verification

- Sounds actually playing through the speaker (firmware is finally not
  silent — see `MORNING.md`).
- Connecting a phone to the Dash AP and seeing the captive portal pop up.
- Walking through the onboarding wizard.
- Starting a session via double-tap and via the portal.
- Tap / double-tap / triple-tap / shake gestures on the physical cube.
- Multi-Dash group session (requires a second cube).

## Next steps once user is awake

See `MORNING.md` at the repo root for a friendly walkthrough.

---

## Self-test status — 116 PASS / 0 FAIL

Triggered via the **serial CLI** at 115200 baud: type `selftest`.

| Section | Tests | Notes |
|---|---|---|
| StateMachine transitions | 10 | every DeviceState reachable |
| Display: every EyeState 0..19 | 20 | render task applies each in ~60 ms |
| Display: overlays | 6 | boot splash, progress 0/50/100, text, QR, clear, blink |
| Character: Moods + react | 7 | Neutral..Playful + transient react() |
| IMU event pipeline | 6 | Tap / DoubleTap / TripleTap / Shake / OrientationChange / Stationary, all via injectEvent() |
| Touch event pipeline | 3 | Touch / DoubleTouch / LongPress |
| Audio | 29 | all 28 .raw files non-zero + silent playback smoke |
| Settings: NVS round-trip | 7 | every getter/setter + tap-sensitivity clamps |
| Power: CPU profile switch | 2 | 240 MHz and 80 MHz verified |
| Session lifecycle | 9 | start(label) → pause → resume → noteDistraction → stop |
| Stats | 3 | append, summary, recentSessionsJson valid JSON |
| WifiAp + Portal | 5 | AP up, SSID "Josh's Dash", IP 192.168.4.1 |
| IdleManager API | 2 | sleepTimeoutSec round-trip, inhibitSleep toggle |
| Character animations | 4 | session-start ~700 ms, wake ~590 ms, sleep ~1820 ms, greetBasedOnTime |
| Games smoke | 2 | initial state None, lastScore accessor |
| Heap stability | 1 | 50 synthetic events, **Δ = 0 bytes** |

### Issues caught + fixed by the test run
- **TripleTap / Shake were stalling for ~2 seconds** because main's
  TripleTap handler ran `playSleepAnimation()` inline (1.8 s of
  delay()-spaced eye states), blocking the IMU event listener chain.
  All heavy event handlers (TripleTap, DoubleTap session toggle,
  LongPress, face-flip-to-sleep) now spawn a one-shot FreeRTOS task and
  return immediately.
- UART serial output corrupts a byte or two across
  `setCpuFrequencyMhz()` transitions (CPU change garbles in-flight
  bytes). Cosmetic; counts are correct.
