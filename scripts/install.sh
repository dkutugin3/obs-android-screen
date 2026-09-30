#!/usr/bin/env bash
# Install (or uninstall) the plugin bundle into the OBS user plugins directory.
#   ./scripts/install.sh          install
#   ./scripts/install.sh remove   uninstall
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE="${ROOT}/build/obs-android-adb.plugin"
DEST_DIR="${HOME}/Library/Application Support/obs-studio/plugins"
DEST="${DEST_DIR}/obs-android-adb.plugin"

if [[ "${1:-install}" == "remove" ]]; then
	rm -rf "${DEST}"
	echo "Removed ${DEST}"
	exit 0
fi

if [[ ! -d "${BUNDLE}/Contents/MacOS" ]]; then
	echo "Bundle not found at ${BUNDLE} — run ./scripts/build.sh first" >&2
	exit 1
fi

mkdir -p "${DEST_DIR}"
rm -rf "${DEST}"
ditto "${BUNDLE}" "${DEST}"
codesign --force --sign - --timestamp=none "${DEST}" >/dev/null 2>&1 || true

echo "Installed: ${DEST}"
echo "Restart OBS Studio to load the plugin."
