# Good morning ☀️

Dash is on your desk and ready. Here's what's there to play with.

## Try this first

1. **Tap the cube once.** Eyes should blink and you should hear a tiny chirp.
   (If you hear nothing — that's a problem, ping me. If you do — sounds are
   wired and the volume default of 60% is what's on right now.)
2. **Tap the cube twice in quick succession.** Dash should start a 25-minute
   focus session. Eyes go focused. Session-start chime plays. The cube
   is now running a timer.
3. **Pull out your phone, open Wi-Fi settings, join "Dash-XXXX"** (the
   exact SSID is logged in serial; check your terminal or the OLED — Dash's
   MAC ends in `21A4` so the AP is `Dash-21A4`). It's an open network. Your
   phone should pop the captive-portal sheet automatically.
4. **You'll land on the welcome tutorial.** 5 steps: name, gesture tutorial,
   optional home Wi-Fi (for OTA — skip if you want), done.
5. **You're now on the main portal.** Three tabs: Study, Stats, Settings.

## What I built

Everything from the master prompt. M0 through M12. The big things:

- 📦 PlatformIO project, 16 MB partition table, dual OTA slots, LittleFS for assets.
- 🎯 IMU with software tap / double-tap / triple-tap / shake / face-down detection.
- 👀 Animated eyes via the vendored eye library, with a mood/character layer on top.
- 🔊 Procedural sound bank (23 samples, 8 kHz mono u8 raw).
- 🌐 Captive portal with a three-tab phone-first UI and four colour themes.
- ⏱️ Sessions with pause/resume, distraction count, RTC-mem crash recovery,
  streak computation, milestone celebrations at 1/10/25/50/100/etc.
- 🎮 Two minigames: Reaction Time and Bop It (start via `/api/game` POST or
  shake-while-idle — actually the gesture trigger isn't wired yet, sorry,
  use the portal).
- 📡 ESP-NOW peer discovery scaffold (single-cube; needs a second to test).
- 🔄 OTA updater that hits the GitHub Releases API.

The full status is in `PROGRESS.md`. Architecture is in `wiki/architecture.md`.
Every API endpoint is in `wiki/api/portal.md`.

## Specific gestures to try

- **One tap**: blink + tap-ack chirp (randomised across 3 variants).
- **Two taps**: toggles a session (start at default length / end if active).
- **Three taps**: deep sleep gesture — Dash plays the sleep chime then sleeps.
- **Shake**: confused face for a beat.
- **Face-down for >2 seconds**: another fast path to sleep.
- **Long-press the touch pad**: shows sleepy face (will trigger menu in v2).

## Specific things to try in the portal

- **Settings → Theme**: four accent colours. They persist in localStorage so
  each phone remembers its own pick.
- **Settings → Volume**: drag the slider. Dash plays a test tick at the new
  level after a 700 ms debounce.
- **Settings → Replay welcome tutorial**: re-runs the onboarding wizard.
- **Settings → Factory reset**: nukes everything (NVS + stats log) and reboots.
- **Konami code (↑↑↓↓←→←→ b a)** while focused on the portal: Dash gets
  heart eyes for 2.5 seconds.

## Sleep behaviour

The cube is set up to *not* deep-sleep on this build (`DASH_NO_DEEP_SLEEP`
build flag) so you can interact with it as soon as you wake up without
having to tap to wake it first. If you want real deep-sleep:

```
pio run -e dash-release -t upload --upload-port /dev/cu.usbserial-110
```

That env has audio on AND deep sleep on. Cube will doze off after 3 minutes
of idle and wake on cap-touch.

## Knowns I'd flag

- **The cap-touch pad is finicky on battery** (no chassis ground). It works
  reliably on USB. Documented in `wiki/hardware_notes.md`.
- **The IMU motion-wake interrupt isn't wired on v1.** Deep sleep can only
  be woken by cap-touch or timer. Documented same place.
- **Multi-Dash group sessions** need a second cube to develop against. The
  protocol is in place (`wiki/protocols.md`) but the heartbeat→session
  sync isn't wired through.
- **OTA's TLS verification is currently `setInsecure()`** because I didn't
  embed a root CA bundle. Fine for v1 since the GitHub releases API is
  public, but worth a hardening pass before any production rev.

## Code layout

```
src/main.cpp               — entry, wires everything together
src/dash/                  — all modules, dash:: namespace
lib/eyes/                  — vendored eye library (unmodified, AGPL)
data/web/                  — captive portal frontend
data/sounds/               — generated audio (.raw 8 kHz u8)
tools/sounds/generate.py   — procedural sound generator
tools/release.sh           — release-cut helper
wiki/                      — architecture, decisions, api, hardware, design
partitions.csv             — 16 MB layout
platformio.ini             — three envs
PROGRESS.md                — milestone status
```

## If something's broken

1. **Cube boot-looping**: check serial at 115200 baud. If it's a partition
   issue (rare now), the easiest reset is `pio run -e dash-debug -t erase`
   then re-upload firmware + fs.
2. **No serial output**: device is in download mode. Press the EN button on
   the dev board to reset.
3. **Portal doesn't load**: check serial — Dash logs every HTTP request and
   AP event with `[WifiAp]` / `[Portal]` tags.
4. **Sounds inaudible**: confirm `DASH_SILENT_AUDIO` isn't in build_flags.
   Check `[Audio]` lines in serial — the log says "[silent]" when gain is 0.

git remote is `https://github.com/JPhusion/dash`. Every commit pushed.

— Josh
