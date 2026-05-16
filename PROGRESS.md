# Dash Build Progress

## M0 — Project Setup & Reference Review ✅

**Done**
- Read entire `/Users/josh/Developer/UNSW/ELEC3117/` source tree.
- Documented findings in `wiki/peripherals.md` and `wiki/reference_code_review.md`.
- Copied eye animation library to `lib/eyes/src/` with self-contained `Common.h`
  + `Constants.h` shims. Library kept unmodified.
- Initialised PlatformIO project (`platformio.ini`) with three envs: `dash`,
  `dash-debug` (silent audio, no deep sleep, verbose logs), `dash-release`.
- Custom partition table — see ADR-005b for the saga; final layout:
  nvs/otadata/app0 (1.5M) /app1 (1.5M) /spiffs (13M) /coredump (64K).
- 16 MB flash declared via `board_build.flash_size` overrides.
- Build flags: `FIRMWARE_VERSION`, core-dump-to-flash, debug level 3.
- Stub `src/main.cpp` prints reset reason + wake cause + heap stats on boot.
- Git init, `main` branch, remote = github.com/JPhusion/dash. Pushed.
- Wiki: `peripherals.md`, `reference_code_review.md`, `decisions.md`.

**Tested**
- `pio run -e dash-debug` clean build, 7.0% RAM / 18.8% flash.
- Hardware boot verified (post-M1 testing exposed the partition issues).

---

## M1 — Core Peripherals ✅

**Done**
- `dash::Display` — wraps the eye library, owns the OLED render task pinned to
  core 1 at 30 fps. Eye states map to library emotions + look directions.
  Overlay system for boot splash / text / progress bars / QR placeholders.
- `dash::Imu` — MPU-6050 over I2C, Madgwick AHRS, 100 Hz sampling task pinned
  to core 1. Software tap/double-tap/triple-tap detection, shake (running
  variance over 16-sample window), 6-face orientation with hysteresis,
  opportunistic gyro bias recal when device is stationary >5 s, bias persisted
  to NVS. Event dispatch worker on core 1 with bounded queue.
- `dash::Audio` — I2S amplifier, plays raw PCM from LittleFS. Supports the
  canonical 8 kHz mono u8 format plus 22 kHz / 44 kHz / s16 for compatibility.
  Volume 0-100, silent-mode override. Playback in a dedicated task on core 1.
- `dash::Touch` — GPIO27 cap-touch with auto-calibrated baseline + threshold.
  Touch / double-touch / long-press events. Recalibrates every 30 s of quiet.
- Shared helpers added: `src/dash/pins.h`, `src/dash/log.h`,
  `src/dash/build_info.h`, `src/dash/reset_reason.h`.
- `main.cpp` wires all four modules and logs every event.

**Tested on hardware**
- Cold boot from POWERON: all four init steps complete, free heap settles to
  ~234 KB, IMU reports face=Up, pitch=-3°, roll=-29° (cube on its side).
- IMU sampling stable; no events fired while stationary (correct).
- Touch baseline=82, threshold=57 (USB-powered; lower on battery — recal task
  handles drift).
- Audio I2S brings up at 8 kHz with silent mode active under DASH_SILENT_AUDIO.
- LittleFS auto-formats on first boot — first attempt logs `Corrupted dir
  pair` then format-on-fail kicks in; subsequent boots are silent.

**Open issues / deferred**
- IMU motion-wake interrupt: INT pin not wired on prototype (deferred to v2
  hardware). Deep sleep wake currently relies on touch + timer.
- USB 5 V sense not implemented. `dash::Power` will assume USB attached.
- The eye library's `RandomBehavior=false` is set; we drive emotions
  explicitly. Eye states for `Heart`, `Celebrating`, `SideEye` etc. map to the
  closest available built-in emotion — richer custom states deferred to M4.
- Audio sample-rate switching uses 16-bit-stereo I2S even for u8 input (we
  upconvert to int16 internally). Acceptable but slightly wasteful.

**Tested but needs physical action**
- Single tap / double-tap / triple-tap detection wired but unverified — the
  cube is stationary on a desk. User can physically test in the morning.
- Shake detection same — sustained shake needed to trigger.
- Face change events partly verified (cube was on its side at boot, so Up was
  detected) but other faces unverified.

---

## M2 — Power Management & State Machine ✅

**Done**
- `dash::Power` — CPU profile (80 / 240 MHz), deep-sleep entry that cuts radios
  (WiFi off, esp_wifi_stop, btStop), enables cap-touch wake on T7, optional
  timer wake, and `gpio_hold_en()` for the I2S DOUT pin across sleep. Build
  flag `DASH_NO_DEEP_SLEEP` short-circuits to a logged no-op.
- RTC_DATA_ATTR variables for `rtcBootCount`, `rtcLastOtaCheckUnix`,
  `rtcLastSleepUnix`, `rtcLastSleepMillis` — survive deep sleep without flash
  writes.
- `dash::StateMachine` — single-owner FSM with logger and listener pattern.
  States: Booting, Onboarding, Idle, Drowsy, Asleep, AwakeForSession,
  InSession, InMenu, InGame, GroupSessionWaiting, GroupSessionActive,
  OtaChecking. Helpers `isInteractive()` / `wantsPerformance()`.
- `dash::IdleManager` — ticks every 500 ms, walks Idle -> Drowsy1..5 -> Asleep
  using configurable timeout (default 180 s). Any IMU event, touch event, or
  state transition out of Idle resets the clock. Switches CPU to 80 MHz at
  drowsy 1+, back to 240 MHz on return to Idle.
- Main loop adds two extra gestures: triple-tap → immediate deep sleep,
  face-down for > 2 s → fast-path deep sleep.
- Boot path logs `Power::begin()` first so `boot_count` and `wake_cause`
  appear before any other module touches state.

**Tested on hardware**
- Cold boot: `boot #1 wake_cause=0`, state transitions to Idle within ~1.5 s,
  240 MHz CPU, ~230 KB free heap.
- State machine logs every transition (e.g. `[State] Booting -> Idle`).
- Idle progression not yet observed (would take 3 min; deferred to morning).
- Deep sleep paths flagged off by `DASH_NO_DEEP_SLEEP`; logs `deep sleep
  skipped` when triple-tap / face-down gestures fire.

**Open issues / deferred**
- Wake-on-motion IMU INT not wired on v1 hardware → only touch + timer wake
  available. The face-down fast-path mitigates this for "putting Dash to bed".
- IdleManager calls `Display::setEyeState()` directly; the Drowsy1..5 states
  currently all map to library's `Sleepy` preset. M4 adds intermediate
  emotion intensities.

---

## M3 — Sound Generation Pipeline ✅

**Done**
- `tools/sounds/generate.py` — procedural sound generator. 18 sounds covering
  boot, wake/sleep, session start/end/complete, menu blip/confirm/back,
  distraction, encouragement, yawn, giggle, heartbeat, game start/correct/
  wrong, tap acknowledgement. Output: 8 kHz mono u8 PCM `.raw` files in
  `data/sounds/`. Total 46.1 KiB.
- DSP primitives: sine/square/triangle/noise oscillators, pitch sweeps,
  linear ADSR envelope, file concat.
- `src/dash/sounds.h` — central registry mapping every sound to its on-flash
  path so application code uses `dash::sounds::kBoot` instead of literal
  strings. Convenience `dash::sounds::play()` defaults to the canonical 8 kHz
  u8 format.
- LittleFS image built via `pio run -t uploadfs` — sounds flashed to the
  spiffs partition.
- Boot chime wired into `main.cpp` (silent under `DASH_SILENT_AUDIO`).
- Tap-ack chirp on every IMU tap event.

**Tested**
- Hardware: serial logs confirm `[Audio] play /sounds/boot.raw (8000 Hz, u8)
  [silent]` after `setup complete` — file is found, opened, format detected,
  written to I2S with gain=0. Volume restoration in production build is a
  one-line flip.
- Generator is deterministic via `np.random.seed(hash(name))` so identical
  re-runs produce identical bytes.

**Open issues / deferred**
- No way for the user to preview the sounds on host machine — generator
  outputs `.raw` not `.wav`. Could add a `--wav` flag later for QA.

## M4 — Eye States & Character System ✅

**Done**
- `dash::Character` — personality layer that drives the eye library above the
  raw `Display::setEyeState()` API.
- Boot animation: splash → surprised → searching → blink → settle to resting
  state.
- Mood enum: Neutral / Focused / Excited / Tired / Listening / Playful.
  Resting eye state derives from mood; other modules set the mood (Session
  module will set Focused; Game module will set Playful; etc.).
- `react()` API: temporarily overrides resting state for a hold time, then
  returns. IMU events (double-tap, shake) now go through this so reactions
  don't get stuck.
- Idle quirks: low-priority task on core 1 fires occasional blinks, glances,
  micro-emotions every 8-25 s. Quirk probability depends on mood (Focused has
  fewer interruptions; Playful gets heart-eyes and celebrations).
- Eye state mapping in `Display::applyEyeState()` covers all 20 enum values,
  using closest emotion preset from the eye library.

**Tested**
- Build clean. Will verify boot animation on hardware after the next upload.

## M5 — WiFi AP & Captive Portal ✅

**Done**
- `dash::WifiAp` — softAP on channel 6 at 11 dBm TX, open SSID `Dash-XXXX`
  (last 4 MAC hex). `WiFi.setSleep(false)` for snappy portal once a phone is
  associated. DNS server task + HTTP server task both pinned to core 0 so
  rendering on core 1 stays untouched.
- `dash::Portal` — request handlers for `/api/status`, `/api/time-sync`,
  `/api/config` (GET/POST). Captive-portal probe paths
  (`/generate_204`, `/hotspot-detect.html`, etc.) 302 to `/`. Static fallback
  serves `/web/*` from LittleFS.
- `dash::Settings` — NVS-backed config: device name, audio volume, sleep
  timeout, session length, home Wi-Fi credentials, last unix + tz, onboarded
  flag. Module owns its `Preferences` instance and serialises through it.
- Web frontend (`data/web/`): vanilla HTML/CSS/JS, dark-mode, mobile-first.
  3 cards (Home, Settings, Stats), session start/end buttons, progress bar.
  8.5 KiB total, well under the 500 KiB budget.
- Main loop boots the AP after settings; portal routes register against the
  WebServer that WifiAp owns.

**Tested**
- Hardware: AP comes up; serial logs `AP up: Dash-21A4 @ 192.168.4.1 (ch 6,
  11 dBm)` and `Portal routes registered`. Free heap drops from 248 KB to
  136 KB after WiFi stack init — expected.
- Web assets served via `python3 -m http.server 8765` locally:
  `index.html` 1.8 KB, `styles.css` 2.5 KB, `app.js` 4.3 KB — all 200 OK.
- Did NOT connect to Dash's own AP (would lose internet); will verify on
  hardware in the morning.

**Open issues / deferred**
- `/api/session` / `/api/stats` endpoints registered in `app.js` but the
  C++ handler returns 404 until M6/M7 land.
- No QR code on OLED yet — `Display::showQR` is an ASCII placeholder. A real
  QR library will be pulled in during M8 (onboarding).

## M6 — Study Session Engine ✅

**Done**
- `dash::Session` — finite-state controller (Idle / Running / Paused /
  Completed). Tracks start time, accumulated pause time, target duration,
  distraction count.
- `start(minutes)`: writes RTC recovery struct, transitions device state to
  InSession, inhibits idle sleep, sets character mood to Focused, plays the
  session_start chime. `pause/resume` does the obvious.
- `stop(celebrate=true)`: on natural completion, fires the celebrating eye
  state + session_complete chime. On manual stop, disappointed eyes +
  session_end chime.
- `noteDistraction()`: 8-second cooldown on the disappointed reaction so a
  burst of social-app DNS queries doesn't carpet-bomb the user.
- Session tick task on core 0 polls every 1 s, redraws the OLED progress
  overlay every 5 s.
- RTC_DATA_ATTR recovery state (`rtcSessionDirty`, `rtcSessionTargetMin`,
  `rtcSessionStartedUnix`) — survives deep sleep / crash, lets the portal
  offer "resume in-progress session" after an unexpected reboot.
- Portal `/api/session` GET returns snapshot JSON; POST handles
  `start|pause|resume|stop` actions.

**Tested**
- Build clean. Hardware boot validates the session task starts (no panic, no
  watchdog). Full start/stop UX needs the phone-AP interaction the user runs
  in the morning.

**Open issues / deferred**
- DNS distraction-domain heuristic NOT yet wired — DNS server currently just
  redirects everything to the captive-portal IP. The proper distraction
  detector (whitelist of focus-safe domains, increment counter on other
  hostnames during a session) is staged for M7 since it integrates tightly
  with stats logging.
- Crash-recovery prompt not yet rendered in the portal UI.

## M7..M12

(Pending.)
