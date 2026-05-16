# M12 Audit Log

Running record of findings from the M0-M11 audit + polish pass.

## Build state (start of M12)

- `pio run -e dash-debug` clean. **Zero warnings, zero errors.**
- RAM: 15.9% (52 KB / 320 KB)
- Flash: 72.4% (1.14 MB / 1.57 MB app partition)

## 60-second runtime observation (post-flash)

Captured `/tmp/dash-audit.log` over 60 s with the cube idle on the desk.

- No panics, no `abort()`, no watchdog warnings, no backtraces.
- All modules initialised cleanly.
- State machine transitioned `Booting -> Onboarding` (correct — onboarded=false).
- Heap progression: 245 KB at boot announce → 129 KB once WiFi AP + portal +
  ESP-NOW tasks all run → **stable at 128 KB free** for 60 s. No leak.
- IMU streams pitch/roll cleanly, no glitch frames.

## Issues found & resolved

(none from initial audit — code came out clean.)

## Issues found & deferred / accepted

- The audit was a 60 s static observation. The cube was face-up the whole
  time, so no orientation-change events, no taps, no shakes. Morning-side
  manual verification of gesture handlers is still needed.
- No second Dash on hand to exercise ESP-NOW peer discovery.
- OTA path can't be tested without a real GitHub release.

## Final 3-minute stability test (post-M12)

Captured `/tmp/dash-180s.log` over 170+ seconds with the cube idle on the
desk and the final M12 build flashed (audio enabled, no deep sleep).

- Free heap: **127868 bytes constant** for the entire run. Zero drift across
  all the modules running concurrently: render task at 30 fps, IMU at 100
  Hz, audio task idle, touch task at 33 Hz, Wi-Fi AP up, DNS server, HTTP
  server, ESP-NOW worker, character idle quirks, session tick, idle
  manager tick.
- Touch baseline drift between 81 and 82 (1 LSB noise — expected on USB
  power) — auto-recalibration fires every 30 s as designed.
- IMU pitch holds at -2.7 to -3.4°, roll at -29.5 to -29.9° (cube on its
  side, stable readings).
- Zero panics, zero watchdog warnings, zero stack canary trips.
- All three build envs (`dash`, `dash-debug`, `dash-release`) build clean
  with zero warnings.

Verdict: **stable for handover.**

## Style / consistency observations

- Module structure is consistent: every dash:: module is one `.h` + one `.cpp`,
  exposes a `&moduleName()` singleton getter, has a constexpr `kTag` for log
  prefixing, and owns its own FreeRTOS task(s) with task_/running_ vars.
- Log tags are consistent: short, capitalised first letter.
- `dash::pins`, `dash::log`, `dash::sounds`, `dash::build_info` are header-only.
- ADRs in `wiki/decisions.md` cover the load-bearing decisions (partition
  layout, audio format, WiFi channel, NVS strategy, etc.).
