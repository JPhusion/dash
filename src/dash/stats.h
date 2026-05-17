// stats.h — rolling session log on LittleFS.
//
// Schema: append-only JSON-lines file at /stats/sessions.ndjson, one record per
// session completion (natural or manual). Old records age out via a soft cap;
// when the file exceeds 64 KiB the oldest ~half is dropped so we never run
// out of flash.
//
// Stats consumer (Portal `/api/stats`) walks the file once on request and
// returns an aggregated summary.

#ifndef DASH_STATS_H
#define DASH_STATS_H

#include <Arduino.h>

namespace dash {

struct SessionRecord {
  uint32_t startedUnix;
  uint16_t targetMin;
  uint16_t actualSec;      // wall seconds focused (not paused)
  uint16_t distractions;
  uint8_t  completed;      // 1 = ran to natural completion, 0 = manual stop
};

struct StatsSummary {
  uint16_t totalSessions;
  uint16_t completedSessions;
  uint32_t totalFocusedSec;
  uint16_t totalDistractions;
  uint16_t streakDays;
  uint32_t bestSingleSec;
};

class Stats {
 public:
  Stats();

  bool begin();
  void append(const SessionRecord& r);
  StatsSummary summary();

  // Sum of actualSec over sessions whose startedUnix falls on "today" in
  // the user's local timezone (tzOffsetMin = signed minutes from UTC).
  // Returns 0 if no records or the system clock hasn't been synced.
  uint32_t todayFocusedSec(int16_t tzOffsetMin);

  // For Portal /api/stats — serialise the full log (or last N entries) as a
  // JSON-lines string. Buffer size matters.
  size_t recentSessionsJson(char* buf, size_t cap, uint8_t limit = 20);

 private:
  void rotateIfNeeded();
};

Stats& stats();

}  // namespace dash

#endif
