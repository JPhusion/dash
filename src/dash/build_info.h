// build_info.h — compile-time constants accessible to the rest of the firmware.
// The version string flows in from platformio.ini via -DFIRMWARE_VERSION=...

#ifndef DASH_BUILD_INFO_H
#define DASH_BUILD_INFO_H

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0-dev"
#endif

namespace dash {

inline constexpr const char* kFirmwareVersion = FIRMWARE_VERSION;

}

#endif
