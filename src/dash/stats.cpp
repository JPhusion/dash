#include "dash/stats.h"

#include <LittleFS.h>

#include "dash/log.h"

namespace dash {

namespace {
constexpr const char* kTag = "Stats";
constexpr const char* kPath = "/stats/sessions.ndjson";
constexpr size_t kMaxBytes = 64 * 1024;
Stats* g_singleton = nullptr;
}

Stats::Stats() {}

bool Stats::begin() {
  if (!LittleFS.begin(true)) {
    log::error(kTag, "fs mount failed");
    return false;
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

  // Parse simple JSON-lines without a full JSON lib; pull numeric values by
  // scanning for "as":N and similar.
  String line;
  line.reserve(160);
  while (f.available()) {
    char c = f.read();
    if (c == '\n') {
      // tiny scan
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
      // Streak math is approximate without timezone info; approximated by
      // counting consecutive 24h-apart records.
      (void)startedUnix;
      (void)targetMin;
      line = "";
    } else if (line.length() < 200) {
      line += c;
    }
  }
  f.close();
  return s;
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
