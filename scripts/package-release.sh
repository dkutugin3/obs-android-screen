#!/usr/bin/env bash
# Build the plugin and package a distributable zip for GitHub Releases.
#   ./scripts/package-release.sh            # build + package into release/
#   ARCHS="arm64;x86_64" ./scripts/package-release.sh   # universal build
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(sed -n 's/^  VERSION \(.*\)$/\1/p' "${ROOT}/CMakeLists.txt" | head -1 | tr -d ' ')"
ARCHS="${ARCHS:-arm64}"
BUILD_DIR="${ROOT}/build"
DIST_DIR="${ROOT}/release"
ZIP_NAME="obs-android-adb-${VERSION}-macos-$(echo "${ARCHS}" | tr ';' '+').zip"

echo "==> Building obs-android-adb v${VERSION} (${ARCHS})"
cmake -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="${ARCHS}" \
  ${NINJA:+-G Ninja} >/dev/null
cmake --build "${BUILD_DIR}" >/dev/null

if [[ ! -d "${BUILD_DIR}/obs-android-adb.plugin/Contents/MacOS" ]]; then
	echo "Bundle missing after build" >&2
	exit 1
fi

echo "==> Packaging ${ZIP_NAME}"
rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}/staging"
ditto "${BUILD_DIR}/obs-android-adb.plugin" "${DIST_DIR}/staging/obs-android-adb.plugin"

cat > "${DIST_DIR}/staging/INSTALL.txt" <<EOF
obs-android-adb v${VERSION} — Android Screen (ADB) source for OBS Studio (macOS)

INSTALL
  1. Quit OBS Studio.
  2. Copy obs-android-adb.plugin to:
       ~/Library/Application Support/obs-studio/plugins/
     (create the "plugins" folder if it does not exist; the full path is
      "obs-studio/plugins/obs-android-adb.plugin")
     In Finder: press Cmd+Shift+G and paste the path above.
  3. Start OBS Studio. Add a source: Sources → + → Android Screen (ADB).

REQUIREMENTS
  - macOS 13+ on ${ARCHS}
  - OBS Studio 30+ (https://obsproject.com)
  - Android platform tools (adb): brew install --cask android-platform-tools
  - Phone: Developer options → USB debugging enabled, connected via USB-C

USAGE & TROUBLESHOOTING
  See the project README on GitHub.

UNINSTALL
  rm -rf ~/Library/Application\ Support/obs-studio/plugins/obs-android-adb.plugin
EOF

(cd "${DIST_DIR}/staging" && zip -qry "../${ZIP_NAME}" .)
rm -rf "${DIST_DIR}/staging"
shasum -a 256 "${DIST_DIR}/${ZIP_NAME}" | tee "${DIST_DIR}/${ZIP_NAME}.sha256"
echo "==> Done: release/${ZIP_NAME}"
