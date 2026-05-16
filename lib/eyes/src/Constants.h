// Constants.h — pin defines required by the eye library.
// Kept self-contained so the library compiles without depending on the host
// project's pin map. If you wire the OLED to different I2C pins, override
// these defines via -DSCL=... -DSDA=... build flags.

#ifndef DASH_EYES_CONSTANTS_H
#define DASH_EYES_CONSTANTS_H

#ifndef SCL
#define SCL 17
#endif

#ifndef SDA
#define SDA 18
#endif

#endif
