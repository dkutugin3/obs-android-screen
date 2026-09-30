#!/usr/bin/env bash
# Configure + build the plugin bundle.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

# Ensure the scrcpy-server binary is present
./scripts/fetch-scrcpy-server.sh

cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja "$@"
cmake --build build
echo
echo "Bundle: ${ROOT}/build/obs-android-adb.plugin"
