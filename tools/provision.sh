#!/usr/bin/env bash
# Provision a fresh Dash cube with the current production-released firmware.
# Checks out the latest GitHub release tag, then flashes both firmware.bin
# and the LittleFS image (sounds + web assets).
#
# Usage:
#   bash tools/provision.sh
#
# Optional:
#   bash tools/provision.sh v2.3.0    # pin a specific release
set -euo pipefail

cd "$(dirname "$0")/.."

if ! command -v pio >/dev/null; then
  echo "error: pio (PlatformIO) is not on PATH"; exit 1
fi

TAG="${1:-}"
if [ -z "$TAG" ]; then
  if command -v gh >/dev/null; then
    TAG="$(gh release view --json tagName -q .tagName 2>/dev/null || true)"
  fi
  if [ -z "$TAG" ]; then
    git fetch --tags --quiet
    TAG="$(git describe --tags --abbrev=0)"
  fi
fi

echo "provisioning Dash with $TAG ..."
git fetch --tags --quiet
git checkout --quiet "$TAG"

pio run -e dash-release -t upload -t uploadfs

echo
echo "done — cube is on $TAG"
