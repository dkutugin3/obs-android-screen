#!/usr/bin/env bash
# Download the official scrcpy-server binary from the Genymobile scrcpy
# releases (Apache License 2.0) into third_party/scrcpy-server/.
set -euo pipefail

VERSION="${SCRCPY_SERVER_VERSION:-4.1}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST_DIR="${ROOT}/third_party/scrcpy-server"
DEST="${DEST_DIR}/scrcpy-server-${VERSION}"

mkdir -p "${DEST_DIR}"

if [[ -f "${DEST}" && $(stat -f%z "${DEST}") -gt 100000 ]]; then
	echo "scrcpy-server ${VERSION} already present: ${DEST}"
	exit 0
fi

URL="https://github.com/Genymobile/scrcpy/releases/download/v${VERSION}/scrcpy-server-v${VERSION}"
echo "Downloading ${URL}"
curl -fSL --retry 3 -o "${DEST}.tmp" "${URL}"

# sanity check: it is a zip (jar) containing classes.dex
if ! unzip -l "${DEST}.tmp" >/dev/null 2>&1 || ! unzip -l "${DEST}.tmp" | grep -q classes.dex; then
	echo "Downloaded file does not look like a valid scrcpy-server jar" >&2
	rm -f "${DEST}.tmp"
	exit 1
fi

mv "${DEST}.tmp" "${DEST}"
echo "OK: ${DEST}"
