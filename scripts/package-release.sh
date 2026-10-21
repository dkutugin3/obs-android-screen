#!/usr/bin/env bash
# Build the plugin and package distributables for GitHub Releases:
#   - .dmg (recommended install path; files inside a DMG carry no Gatekeeper
#     quarantine, so the bundled adb runs without any prompts)
#   - .zip (same content, for scripted installs)
#   ./scripts/package-release.sh                          # arm64 .so
#   ARCHS="arm64;x86_64" ./scripts/package-release.sh     # universal .so
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(sed -n 's/^  VERSION \(.*\)$/\1/p' "${ROOT}/CMakeLists.txt" | head -1 | tr -d ' ')"
ARCHS="${ARCHS:-arm64}"
BUILD_DIR="${ROOT}/build"
DIST_DIR="${ROOT}/release"
ZIP_NAME="obs-android-adb-${VERSION}-macos-$(echo "${ARCHS}" | tr ';' '+').zip"
DMG_NAME="obs-android-screen-${VERSION}-macos.dmg"

echo "==> Building obs-android-adb v${VERSION} (${ARCHS})"
cmake -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="${ARCHS}" \
  ${NINJA:+-G Ninja} >/dev/null
cmake --build "${BUILD_DIR}" >/dev/null

if [[ ! -d "${BUILD_DIR}/obs-android-adb.plugin/Contents/MacOS" ]]; then
	echo "Bundle missing after build" >&2
	exit 1
fi
if [[ ! -x "${BUILD_DIR}/obs-android-adb.plugin/Contents/Resources/adb/adb" ]]; then
	echo "Bundled adb missing — configure with BUNDLE_ADB=ON (default)" >&2
	exit 1
fi

echo "==> Packaging ${ZIP_NAME} + ${DMG_NAME}"
rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}/staging"
ditto "${BUILD_DIR}/obs-android-adb.plugin" "${DIST_DIR}/staging/obs-android-adb.plugin"

cat > "${DIST_DIR}/staging/INSTALL.txt" <<EOF
obs-android-adb v${VERSION} — Android Screen (ADB) source for OBS Studio (macOS)

INSTALL (DMG, recommended)
  1. Open the .dmg.
  2. Quit OBS Studio.
  3. Copy obs-android-adb.plugin to:
       ~/Library/Application Support/obs-studio/plugins/
     (create the "plugins" folder if it does not exist)
     Easiest way: in Finder press Cmd+Shift+G, paste the path above,
     then drag the plugin from the DMG into the folder.
  4. Start OBS Studio. Add a source: Sources -> + -> Android Screen (ADB).

REQUIREMENTS
  - macOS 13+ (universal: arm64 + x86_64)
  - OBS Studio 30+ (https://obsproject.com)
  - Phone: Developer options -> USB debugging enabled, connected via a
    USB-C data cable
  That is all — adb is bundled inside the plugin. (If you have your own
  platform-tools install, the plugin can use it via Advanced settings.)

USAGE & TROUBLESHOOTING
  See the project README on GitHub.

UNINSTALL
  rm -rf ~/Library/Application\ Support/obs-studio/plugins/obs-android-adb.plugin
EOF

(cd "${DIST_DIR}/staging" && zip -qry "../${ZIP_NAME}" .)
hdiutil create -volname "obs-android-screen ${VERSION}" -srcfolder "${DIST_DIR}/staging" \
  -ov -format UDZO "${DIST_DIR}/${DMG_NAME}" >/dev/null
rm -rf "${DIST_DIR}/staging"
shasum -a 256 "${DIST_DIR}/${ZIP_NAME}" "${DIST_DIR}/${DMG_NAME}" > "${DIST_DIR}/checksums.sha256"
cat "${DIST_DIR}/checksums.sha256"
echo "==> Done: release/${ZIP_NAME}, release/${DMG_NAME}"
