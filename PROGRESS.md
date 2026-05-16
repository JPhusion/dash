# Dash Build Progress

## M0 — Project Setup & Reference Review ✅

**Done**
- Read entire `/Users/josh/Developer/UNSW/ELEC3117/` source tree.
- Documented findings in `wiki/peripherals.md` and `wiki/reference_code_review.md`.
- Copied eye animation library to `lib/eyes/src/` with self-contained `Common.h`
  + `Constants.h` shims. Library kept unmodified.
- Initialised PlatformIO project (`platformio.ini`) with three envs: `dash`
  (default), `dash-debug` (silent audio, no deep sleep, verbose logs), and
  `dash-release` (production).
- Custom partition table (`partitions.csv`) with dual OTA slots, encrypted
  `nvs_keys`, coredump region, 13 MB LittleFS.
- Build flags: `FIRMWARE_VERSION`, core-dump-to-flash, debug level 3.
- Stub `src/main.cpp` prints reset reason + wake cause + heap stats on every
  boot; heartbeat heap log every 10 s.
- Git init, `main` branch, remote `origin = https://github.com/JPhusion/dash`.
- Author configured locally as Josh Chans <joshua.hy.chans@gmail.com>.
- Wiki: `peripherals.md`, `reference_code_review.md`, `decisions.md` (ADR-001..012).

**Tested**
- (Pending) build verification: `pio run -e dash-debug` — running next.

**Open issues / deferred**
- NVS encryption keys not yet generated. Reserved partition; wiring deferred to
  M8 per ADR-005.
- IMU motion-wake interrupt not yet usable (INT pin not wired on prototype) —
  documented in `wiki/peripherals.md`. M2 falls back to touch + timer wake.

---

## M1 — Core Peripherals ⏳

(Pending)

## M2..M12

(Pending — see master prompt in build log.)
