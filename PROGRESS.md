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

## M2 — Power Management & State Machine ⏳

(Pending — module headers/code drafted, not yet committed.)

## M3..M12

(Pending.)
