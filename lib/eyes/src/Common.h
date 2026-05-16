// Common.h — eye library shared display handle.
// The U8g2 instance is created inside Face.cpp at library compile time;
// other library files reference it via this extern declaration.

#ifndef DASH_EYES_COMMON_H
#define DASH_EYES_COMMON_H

#include <U8g2lib.h>

extern U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2;

#endif
