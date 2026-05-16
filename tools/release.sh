#!/usr/bin/env bash
# Dash release helper. Builds the release env, hashes the firmware binary,
# and emits a release/ folder ready to upload as a GitHub release asset.
#
# Usage: tools/release.sh <semver>
# Example: tools/release.sh 0.2.0
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <semver>" >&2
  exit 1
fi
VERSION="$1"
case "$VERSION" in
  v*) VERSION="${VERSION#v}";;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Bump version in platformio.ini.
echo ">> bumping version to $VERSION"
python3 - <<PY
from pathlib import Path
p = Path("platformio.ini")
text = p.read_text()
import re
text = re.sub(r"^firmware_version = .*$",
              f"firmware_version = $VERSION",
              text, flags=re.M)
p.write_text(text)
PY

echo ">> building dash-release"
pio run -e dash-release

OUT="$ROOT/release/$VERSION"
mkdir -p "$OUT"
cp ".pio/build/dash-release/firmware.bin" "$OUT/firmware.bin"

echo ">> hashing"
( cd "$OUT" && shasum -a 256 firmware.bin > firmware.bin.sha256 )

echo ">> done"
echo "release/$VERSION/firmware.bin"
echo "release/$VERSION/firmware.bin.sha256"
echo
echo "next: 'gh release create v$VERSION release/$VERSION/firmware.bin release/$VERSION/firmware.bin.sha256 --notes \"...\"'"
