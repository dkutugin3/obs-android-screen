#!/usr/bin/env bash
# Windows packaging (run from an MSYS2/MinGW shell after a build):
# zips build/win-dist into the OBS drop-in layout.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${1:?version}"
BUILD_DIR="${BUILD_DIR:-${ROOT}/build}"
DIST_DIR="${ROOT}/release"
ZIP_NAME="obs-android-screen-${VERSION}-windows-x64.zip"

if [[ ! -f "${BUILD_DIR}/win-dist/obs-plugins/64bit/obs-android-adb.dll" ]]; then
	echo "==> Building obs-android-adb v${VERSION} (Windows x64)"
	cmake -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
	cmake --build "${BUILD_DIR}" >/dev/null
fi

if [[ ! -f "${BUILD_DIR}/win-dist/obs-plugins/64bit/obs-android-adb.dll" ]]; then
	echo "win-dist missing after build" >&2
	exit 1
fi

echo "==> Packaging ${ZIP_NAME}"
rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}/staging"
cp -R "${BUILD_DIR}/win-dist/obs-plugins" "${DIST_DIR}/staging/"
cp -R "${BUILD_DIR}/win-dist/data" "${DIST_DIR}/staging/"

# INSTALL.txt — written with printf to avoid heredoc delimiters, which are
# fragile when a Windows checkout rewrites line endings.
printf '%s\n' \
	"obs-android-adb v${VERSION} — Android Screen (ADB) source for OBS Studio (Windows x64)" \
	"" \
	"INSTALL" \
	"  1. Quit OBS Studio." \
	'  2. Copy the "obs-plugins" and "data" folders from this archive into:' \
	'       %APPDATA%\obs-studio\' \
	"     merging them with the existing folders (create them if they do not" \
	"     exist). Easiest: Win+R, type  %APPDATA%\\obs-studio  and press Enter." \
	"     Result:" \
	'       %APPDATA%\obs-studio\obs-plugins\64bit\obs-android-adb.dll' \
	'       %APPDATA%\obs-studio\data\obs-plugins\obs-android-adb\...' \
	"  3. Start OBS Studio. Add a source: Sources -> + -> Android Screen (ADB)." \
	"" \
	"REQUIREMENTS" \
	"  - Windows 10/11 x64" \
	"  - OBS Studio 30+ (https://obsproject.com)" \
	"  - Phone: Developer options -> USB debugging enabled, connected via a" \
	"    USB-C data cable" \
	"  That is all — adb is bundled inside the plugin. (If a phone is not" \
	"  detected, install the Google USB driver; see the project README.)" \
	"" \
	"USAGE & TROUBLESHOOTING" \
	"  See the project README on GitHub." \
	"" \
	"UNINSTALL" \
	'  Delete obs-android-adb.dll from %APPDATA%\obs-studio\obs-plugins\64bit\' \
	'  and the folder %APPDATA%\obs-studio\data\obs-plugins\obs-android-adb\' \
	> "${DIST_DIR}/staging/INSTALL.txt"

(cd "${DIST_DIR}/staging" && zip -qry "../${ZIP_NAME}" .)
rm -rf "${DIST_DIR}/staging"
if command -v sha256sum >/dev/null 2>&1; then
	sha256sum "${DIST_DIR}/${ZIP_NAME}" > "${DIST_DIR}/${ZIP_NAME}.sha256"
else
	shasum -a 256 "${DIST_DIR}/${ZIP_NAME}" > "${DIST_DIR}/${ZIP_NAME}.sha256"
fi
echo "==> Done: release/${ZIP_NAME}"
