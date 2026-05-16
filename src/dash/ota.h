// ota.h — OTA firmware updater.
//
// Two trigger paths:
//   1. Scheduled overnight check: wake from deep sleep at the 4 AM window,
//      bring up STA, query the GitHub releases API, and flash if a newer tag
//      is available. Falls asleep again afterwards.
//   2. Manual via portal: /api/ota/check (interactive, while phone is on
//      the AP). Same logic, but stays online for the user to watch progress.
//
// Version comparison: SemVer-ish, "MAJOR.MINOR.PATCH" → integer tuple, with
// optional leading `v`. Anything that doesn't parse is treated as older.
//
// Signature: SHA-256 hash check against a hash file alongside the release
// (firmware.bin.sha256). Stronger ECDSA verification is staged in
// tools/keys/ but not enforced this build — see ADR-006-ota.

#ifndef DASH_OTA_H
#define DASH_OTA_H

#include <Arduino.h>

namespace dash {

enum class OtaResult : uint8_t {
  UpToDate,
  Updated,
  NoCredentials,
  ConnectFailed,
  CheckFailed,
  DownloadFailed,
  HashMismatch,
  WriteFailed,
};

const char* otaResultString(OtaResult r);

class Ota {
 public:
  Ota();

  // Run the full check + update if needed. Caller is responsible for state
  // transition + display. Blocks for the duration (typ. 5-30 s on success,
  // <2 s on UpToDate).
  OtaResult checkAndApply();

  // Convenience: returns the latest tag string (or empty on error). Used by
  // the portal to show "new version available" without downloading.
  String latestRemoteTag();

 private:
  bool ensureStation();
  void teardownStation();
  bool fetchLatestTag(String& tagOut, String& assetUrl);
  bool downloadAndFlash(const String& url, const String& expectedHash);
};

Ota& ota();

}  // namespace dash

#endif
