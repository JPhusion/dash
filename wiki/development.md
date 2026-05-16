# Development Notes

## Build

```
pio run -e dash-debug          # debug build (silent audio, no deep sleep, verbose logs)
pio run -e dash                # default build
pio run -e dash-release        # production (audio on, deep sleep on)

pio run -e dash-debug -t upload      # flash firmware
pio run -e dash-debug -t uploadfs    # flash LittleFS image (data/)
```

USB-UART upload speed is **460800** — the original 921600 was unreliable on
the prototype's CP2102. Documented as ADR-X (see `decisions.md`).

The dev cube enumerates as `/dev/cu.usbserial-110` on the build machine.

## Serial monitor

`pio device monitor` works in a terminal. For headless capture (e.g., from a
script), use pyserial directly:

```python
import serial, time
s = serial.Serial('/dev/cu.usbserial-110', 115200, timeout=1)
# trigger reset via DTR/RTS toggle:
s.setDTR(False); s.setRTS(True); time.sleep(0.1)
s.setDTR(False); s.setRTS(False)
while True: print(s.readline().decode('utf-8', 'replace'), end='')
```

## Generating sounds

```
python3 tools/sounds/generate.py
```

Emits 18 `.raw` files under `data/sounds/` (8 kHz mono u8 PCM). Deterministic
— `np.random.seed(hash(name))` ensures byte-identical re-runs. Re-upload the
LittleFS image after regenerating.

## Cutting a release

```
tools/release.sh 0.2.0     # bumps platformio.ini, builds, hashes
gh release create v0.2.0 release/0.2.0/firmware.bin release/0.2.0/firmware.bin.sha256 --notes "..."
```

The on-device OTA flow polls `api.github.com/repos/JPhusion/dash/releases/latest`,
compares the tag against `kFirmwareVersion`, and downloads the
`firmware.bin` asset. If a sibling `firmware.bin.sha256` is also published, it
gets verified inline.

## Resetting the cube

- **Factory reset via portal**: Settings tab → Factory reset (confirms twice).
- **Wipe entire flash**: `pio run -e dash-debug -t erase` (then re-upload firmware
  + LittleFS).
- **Replay onboarding without losing other settings**: Settings tab → Replay
  welcome tutorial.

## Hardware quirks worth remembering

- **GPIO14 (I2S DOUT) is a strapping pin**. Default boot mode of the
  ESP32-WROOM works fine with the amplifier attached, but adding an external
  pull-up here will brick boot.
- **Partition table needs `board_build.flash_size = 16MB`**. The `esp32dev`
  board defaults to 4 MB. Without the override, the Arduino-ESP32 bootloader
  is built for 4 MB and our 16 MB layout silently traps the chip in a boot
  loop. (See ADR-005b.)
- **App0 must start at offset `0x10000`** for the prebuilt Arduino-ESP32
  bootloader to find it. Anything else = silent boot loop with no
  second-stage bootloader output.
- **LittleFS partition must be named `spiffs`** (subtype spiffs). The
  Arduino-ESP32 `LittleFS.begin()` looks up the partition by name string. If
  named `littlefs`, mount returns error 261.

## Adding a new module

Convention is one `.h` + one `.cpp` per module, under `src/dash/`, in the
`dash::` namespace. Pattern:

```cpp
// src/dash/foo.h
namespace dash {
class Foo {
 public:
  Foo();
  bool begin();
  void start();        // spawn FreeRTOS task if needed
  void stop();
 private:
  static void taskTrampoline(void* arg);
  void loop();
};
Foo& foo();
}  // namespace dash

// src/dash/foo.cpp
#include "dash/foo.h"
namespace dash {
namespace { constexpr const char* kTag = "Foo"; Foo* g_singleton = nullptr; }
Foo::Foo() {}
bool Foo::begin() { /* ... */ return true; }
Foo& foo() { if (!g_singleton) g_singleton = new Foo(); return *g_singleton; }
}
```

Lazy singletons avoid static-init-order issues. Module owns its own FreeRTOS
task pinned to whichever core is appropriate (core 1 for latency-sensitive,
core 0 for network).

## Testing the portal locally

```
cd data/web && python3 -m http.server 8765
```

Open `http://localhost:8765/` in a browser. The frontend's API calls will
404, but layout/styles/behaviour are testable. Real API behaviour needs the
phone-to-Dash-AP flow.

## Logging conventions

All modules use `dash::log::{info,warn,error,debug}(kTag, fmt, ...)`. Tags
are short capitalised strings (e.g. `Imu`, `Audio`, `Portal`). `debug()` is
compiled out when `DASH_DEBUG` isn't defined.

State transitions go through `dash::stateMachine().transitionTo(s)` which
already logs.

## Open items / known issues

See `PROGRESS.md` per-milestone "Open issues / deferred" sections and
`wiki/audit.md` for the M12 audit log.
