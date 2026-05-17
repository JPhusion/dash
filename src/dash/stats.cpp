#include "dash/stats.h"

#include <LittleFS.h>
#include <time.h>

#include "dash/log.h"
#include "dash/settings.h"

namespace dash {

namespace {
constexpr const char* kTag = "Stats";
constexpr const char* kPath = "/stats/sessions.ndjson";
constexpr size_t kMaxBytes = 64 * 1024;
Stats* g_singleton = nullptr;
}

Stats::Stats() {}

bool Stats::begin() {
  // Audio module mounts LittleFS first; double-mount here is benign but the
  // Arduino-ESP32 LittleFS wrapper warns. Skip if already mounted by checking
  // an arbitrary path.
  if (!LittleFS.exists("/")) {
    if (!LittleFS.begin(true)) {
      log::error(kTag, "fs mount failed");
      return false;
    }
  }
  if (!LittleFS.exists("/stats")) LittleFS.mkdir("/stats");
  return true;
}

void Stats::rotateIfNeeded() {
  File f = LittleFS.open(kPath, "r");
  if (!f) return;
  size_t size = f.size();
  if (size < kMaxBytes) { f.close(); return; }
  // Drop the first half of the file by reading the back half and rewriting.
  size_t keep = size / 2;
  f.seek(size - keep);
  // Skip past the next newline so we don't start mid-record.
  while (f.available() && f.read() != '\n') {}
  String tail;
  tail.reserve(keep);
  while (f.available()) tail += (char)f.read();
  f.close();
  File w = LittleFS.open(kPath, "w");
  if (w) {
    w.print(tail);
    w.close();
    log::info(kTag, "rotated stats log: kept %u bytes", (unsigned)tail.length());
  }
}

void Stats::append(const SessionRecord& r) {
  rotateIfNeeded();
  File f = LittleFS.open(kPath, FILE_APPEND);
  if (!f) {
    log::warn(kTag, "append: cannot open");
    return;
  }
  char buf[160];
  snprintf(buf, sizeof(buf),
           "{\"u\":%u,\"tm\":%u,\"as\":%u,\"d\":%u,\"c\":%u}\n",
           (unsigned)r.startedUnix, (unsigned)r.targetMin,
           (unsigned)r.actualSec, (unsigned)r.distractions, (unsigned)r.completed);
  f.print(buf);
  f.close();
  log::info(kTag, "logged session: t=%us c=%u", (unsigned)r.actualSec, r.completed);
}

StatsSummary Stats::summary() {
  StatsSummary s{};
  File f = LittleFS.open(kPath, "r");
  if (!f) return s;

  // Build a sorted-by-unix list of session days for streak math (small enough
  // to fit in stack — we keep at most 64 distinct days from the rolling log).
  uint32_t distinctDays[64];
  uint8_t distinctDayCount = 0;

  String line;
  line.reserve(160);
  while (f.available()) {
    char c = f.read();
    if (c == '\n') {
      int idx;
      uint32_t startedUnix = 0;
      uint16_t targetMin = 0, actualSec = 0, distractions = 0;
      uint8_t completed = 0;
      idx = line.indexOf("\"u\":");
      if (idx >= 0) startedUnix = strtoul(line.c_str() + idx + 4, nullptr, 10);
      idx = line.indexOf("\"tm\":");
      if (idx >= 0) targetMin = (uint16_t)strtoul(line.c_str() + idx + 5, nullptr, 10);
      idx = line.indexOf("\"as\":");
      if (idx >= 0) actualSec = (uint16_t)strtoul(line.c_str() + idx + 5, nullptr, 10);
      idx = line.indexOf("\"d\":");
      if (idx >= 0) distractions = (uint16_t)strtoul(line.c_str() + idx + 4, nullptr, 10);
      idx = line.indexOf("\"c\":");
      if (idx >= 0) completed = (uint8_t)strtoul(line.c_str() + idx + 4, nullptr, 10);

      s.totalSessions++;
      if (completed) s.completedSessions++;
      s.totalFocusedSec += actualSec;
      s.totalDistractions += distractions;
      if (actualSec > s.bestSingleSec) s.bestSingleSec = actualSec;

      // Day bucketing for streak — use UTC days; we don't have tz info here
      // and a 1-day-window error on streak is acceptable.
      if (startedUnix > 0 && distinctDayCount < 64) {
        uint32_t dayBucket = startedUnix / 86400UL;
        bool seen = false;
        for (uint8_t i = 0; i < distinctDayCount; i++) {
          if (distinctDays[i] == dayBucket) { seen = true; break; }
        }
        if (!seen) distinctDays[distinctDayCount++] = dayBucket;
      }
      (void)targetMin;
      line = "";
    } else if (line.length() < 200) {
      line += c;
    }
  }
  f.close();

  // Streak: count consecutive day buckets ending today (or yesterday — gives
  // the user a one-day grace so an active streak doesn't break the moment
  // the clock rolls past midnight).
  if (distinctDayCount > 0) {
    // Sort distinctDays ascending (small N, bubble sort is fine).
    for (uint8_t i = 0; i < distinctDayCount; i++) {
      for (uint8_t j = i + 1; j < distinctDayCount; j++) {
        if (distinctDays[j] < distinctDays[i]) {
          uint32_t t = distinctDays[i]; distinctDays[i] = distinctDays[j]; distinctDays[j] = t;
        }
      }
    }
    uint32_t today = (uint32_t)time(nullptr) / 86400UL;
    // Walk backwards from today.
    uint16_t streak = 0;
    int32_t expected = (int32_t)today;
    for (int i = distinctDayCount - 1; i >= 0 && expected >= 0; i--) {
      if ((int32_t)distinctDays[i] == expected) {
        streak++;
        expected--;
      } else if ((int32_t)distinctDays[i] == expected - 1 && streak == 0) {
        // Grace: today missing but yesterday present still counts as 1.
        streak++;
        expected -= 2;
      } else if ((int32_t)distinctDays[i] < expected) {
        break;
      }
    }
    s.streakDays = streak;
  }
  return s;
}

uint32_t Stats::todayFocusedSec(int16_t tzOffsetMin) {
  // Use the last phone-synced unix as "now" — it's what session
  // records also stamp themselves with (see Session::stop), so this
  // keeps records from the same calendar day in the same bucket
  // even though we don't run a real RTC.
  uint32_t nowUnix = settings().lastUnix();
  if (nowUnix < 86400UL) return 0;   // clock not yet synced
  // Local-day bucket: shift by tz minutes, then floor to 24h.
  int32_t shift = (int32_t)tzOffsetMin * 60;
  uint32_t todayBucket = (uint32_t)(((int64_t)nowUnix + shift) / 86400);

  File f = LittleFS.open(kPath, "r");
  if (!f) return 0;
  uint32_t total = 0;
  String line;
  line.reserve(160);
  while (f.available()) {
    char c = f.read();
    if (c == '\n') {
      int idx;
      uint32_t startedUnix = 0;
      uint16_t actualSec = 0;
      idx = line.indexOf("\"u\":");
      if (idx >= 0) startedUnix = strtoul(line.c_str() + idx + 4, nullptr, 10);
      idx = line.indexOf("\"as\":");
      if (idx >= 0) actualSec = (uint16_t)strtoul(line.c_str() + idx + 5, nullptr, 10);
      if (startedUnix > 0) {
        uint32_t b = (uint32_t)(((int64_t)startedUnix + shift) / 86400);
        if (b == todayBucket) total += actualSec;
      }
      line = "";
    } else if (line.length() < 200) {
      line += c;
    }
  }
  f.close();
  return total;
}

size_t Stats::recentSessionsJson(char* buf, size_t cap, uint8_t limit) {
  // We walk the file to find the last `limit` lines, then build a JSON array.
  File f = LittleFS.open(kPath, "r");
  if (!f) { snprintf(buf, cap, "[]"); return strlen(buf); }
  String all;
  all.reserve(f.size() < 4096 ? f.size() : 4096);
  while (f.available() && all.length() < 4096) all += (char)f.read();
  f.close();

  // Split lines from the back.
  int start = all.length();
  uint8_t collected = 0;
  String pieces[20];
  while (start > 0 && collected < limit && collected < 20) {
    int nl = all.lastIndexOf('\n', start - 1);
    int from = nl + 1;
    if (from < start) {
      pieces[collected++] = all.substring(from, start);
    }
    start = nl;
  }
  // Build JSON array; oldest first within the recent window.
  size_t used = 0;
  used += snprintf(buf + used, cap - used, "[");
  for (int i = collected - 1; i >= 0 && used + 8 < cap; --i) {
    used += snprintf(buf + used, cap - used, "%s%s",
                     (i == collected - 1) ? "" : ",",
                     pieces[i].c_str());
  }
  used += snprintf(buf + used, cap - used, "]");
  return used;
}

Stats& stats() {
  if (!g_singleton) g_singleton = new Stats();
  return *g_singleton;
}

}  // namespace dash
