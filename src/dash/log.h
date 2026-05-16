// log.h — thin Serial.print() wrapper with module tags.
//
// Goals: a single place to grep for "[Imu]"-style tags, automatic flushing
// before deep sleep, and a compile-time off-switch for release builds that
// strip debug-only logs.

#ifndef DASH_LOG_H
#define DASH_LOG_H

#include <Arduino.h>

namespace dash::log {

inline void info(const char* tag, const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.printf("[%s] %s\n", tag, buf);
}

inline void warn(const char* tag, const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.printf("[%s] WARN %s\n", tag, buf);
}

inline void error(const char* tag, const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.printf("[%s] ERROR %s\n", tag, buf);
}

#ifdef DASH_DEBUG
inline void debug(const char* tag, const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.printf("[%s] DBG %s\n", tag, buf);
}
#else
inline void debug(const char*, const char*, ...) {}
#endif

}  // namespace dash::log

#endif
