# obs-android-screen — Android Screen (ADB) for OBS Studio

A native OBS Studio source plugin for macOS (Apple Silicon) that mirrors an
Android phone — video **and system audio** — into OBS over a plain USB-C cable,
using the official [scrcpy](https://github.com/Genymobile/scrcpy) server and
wire protocol over ADB.

No Window Capture. No macOS Screen Capture. No scrcpy GUI window. No virtual
camera. No HDMI capture card. No AirPlay. No macOS *Screen Recording*
permission required.

```
Samsung Galaxy S26                Mac (M4, arm64)
┌────────────────┐   USB-C    ┌──────────────────────────────┐
│ scrcpy-server  │◄──────────►│ adb forward (localhost TCP)  │
│  ├ H.264/HEVC  │            │ obs-android-adb.plugin       │
│  └ system audio│            │  ├ VideoToolbox decode ──► OBS video
└────────────────┘            │  └ raw PCM 48 kHz ──────► OBS audio
                              └──────────────────────────────┘
```

The Android side is the **unmodified upstream `scrcpy-server` v4.1** binary
(Apache-2.0) bundled inside the plugin; the plugin implements the client side
of the scrcpy protocol exactly as `app/src/demuxer.c` in scrcpy does.

## Requirements

| Component | Version |
|---|---|
| macOS | 15+ (Sequoia), Apple Silicon (arm64) |
| OBS Studio | 30.0+ (tested with 32.2.2) |
| Android device | Android 11+ for audio; any scrcpy-supported Android for video (**live-tested**: Galaxy S26-class SM-S942B / Android 16) |
| Android Platform Tools (adb) | any recent (`brew install --cask android-platform-tools`) |
| On the phone | Developer options → USB debugging enabled |
| Build toolchain | Xcode Command Line Tools, CMake ≥ 3.25, Ninja (optional) |

The plugin links only system frameworks (VideoToolbox/CoreMedia/CoreVideo/
CoreText/CoreGraphics) — no FFmpeg, no bundled dylibs. libobs symbols resolve
from OBS itself at load time.

## Build

```bash
./scripts/build.sh
# or manually:
#   ./scripts/fetch-scrcpy-server.sh
#   cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
#   cmake --build build
```

The first configure downloads the obs-studio sources (for libobs headers), the
[simde](https://github.com/simd-everywhere/simde) headers and bundles
`scrcpy-server-v4.1` — a network connection is needed once; everything is
cached in `build/deps/`.

Result: `build/obs-android-adb.plugin`.

Offline tests (adb output parser, Annex-B splitter, placeholder renderer and
the VideoToolbox decoder against synthetic H.264/HEVC streams, including a
resolution-change/rotation case) are built as `build/obs-android-adb-tests`:

```bash
ffmpeg -f lavfi -i testsrc2=size=1080x2340:rate=60 -t 2 -c:v libx264 \
  -profile:v baseline -pix_fmt yuv420p -bsf:v h264_mp4toannexb -f h264 /tmp/obs-android-adb-test.h264
ffmpeg -f lavfi -i testsrc2=size=1080x2340:rate=60 -t 2 -c:v libx265 \
  -pix_fmt yuv420p -f hevc /tmp/obs-android-adb-test.h265
ffmpeg -f lavfi -i testsrc2=size=2340x1080:rate=60 -t 1 -c:v libx264 \
  -profile:v baseline -pix_fmt yuv420p -bsf:v h264_mp4toannexb -f h264 /tmp/obs-android-adb-test-rot.h264
./build/obs-android-adb-tests   # expect: ALL TESTS PASSED
```

## Install

### From a release (recommended)

1. Download the latest `obs-android-adb-<version>-macos-arm64.zip` from
   [Releases](../../releases).
2. Unzip it and copy `obs-android-adb.plugin` to
   `~/Library/Application Support/obs-studio/plugins/` (create the `plugins`
   folder if needed — an `INSTALL.txt` with step-by-step instructions is
   inside the zip).

   ```bash
   unzip obs-android-adb-*-macos-arm64.zip
   mkdir -p ~/Library/Application\ Support/obs-studio/plugins
   cp -R obs-android-adb.plugin ~/Library/Application\ Support/obs-studio/plugins/
   ```
3. Restart OBS Studio.

### From source

```bash
git clone https://github.com/dkutugin3/obs-android-screen.git
cd obs-android-screen

# (the built plugin bundle keeps the module name `obs-android-adb.plugin`)
./scripts/build.sh
./scripts/install.sh            # copies to ~/Library/Application Support/obs-studio/plugins/
./scripts/install.sh remove     # uninstalls
```

Then restart OBS Studio. (Requires [Xcode Command Line
Tools](https://developer.apple.com/xcode/resources/) and CMake ≥ 3.25; Intel
Macs can build a universal binary with
`ARCHS="arm64;x86_64" ./scripts/package-release.sh`.)

## Usage

1. On the phone: **Settings → About phone → tap "Build number" 7×** to enable
   Developer options.
2. **Settings → Developer options → enable USB debugging.**
3. Connect the phone to the Mac with a USB-C cable.
4. Verify it is visible and authorized:

   ```bash
   adb devices -l
   # R3CW30ABCDEF   device usb:3-1 product:... model:SM-S931B ...
   ```

   The state must be `device` (not `unauthorized`/`offline`).
5. In OBS: **Sources → + → Android Screen (ADB)**.
6. Pick your device (shown as `Galaxy S26 (R3CW30ABCDEF)`), press **Refresh
   devices** if the list is stale, then **OK**.
7. The phone screen appears in OBS; system audio appears as a regular audio
   source in the Audio Mixer ("Capture device audio" is on by default).

Settings: codec (H.264 default / H.265), resolution (Native / 1080p / 1440p /
720p — the value caps the long edge, matching scrcpy's `--max-size`), FPS
(15–120, default 60), bitrate (4–30 Mbps, default 20), capture audio, turn
device screen off while mirroring, keep device awake, auto reconnect.
Under **Advanced** you can point to a specific `adb` executable.

If the cable is unplugged the source shows a **Device disconnected /
Reconnecting…** placeholder and automatically resumes when the same phone is
plugged back in — the source must not be deleted and re-added.

The source keeps streaming while OBS is minimized or covered by other windows
(it is a normal active OBS source, not a screen capture).

### Performance

The plugin is deliberately light on the Mac (measured on an M4, 1080p-class
60 fps H.264 @ 20 Mbit/s + system audio, preview rendered):

- **≈ 2 % of total CPU** (≈ 20 % of one performance core) — decoding runs on
  the VideoToolbox hardware decoder, not the CPU.
- Device tracking is event-driven: one long-lived `adb track-devices` process
  replaces per-second polling; no extra processes are spawned while idle, and
  a reconnect is detected the instant it happens.
- OBS exits cleanly with the source active (no crash on quit).
- To lower CPU further, hide the OBS preview (right-click the canvas →
  **Preview → Disable preview**) — canvas rendering, not the plugin, is the
  largest remaining share.

### Debug dump of the elementary stream

For transport debugging you can dump the raw H.264/H.265 elementary stream the
plugin receives (before decoding):

```bash
OBS_ANDROID_ADB_DUMP=/tmp/android.h264 open -a OBS
# reproduce, then:  ffplay /tmp/android.h264
```

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| **Device is not authorized** | Unlock the phone, watch for the "Allow USB debugging?" prompt, tap Allow. The source retries automatically. If it never appears: `adb kill-server && adb start-server`, re-plug the cable. |
| **device offline** | `adb devices` shows `offline`: bad cable/port or adb daemon hiccup — `adb kill-server`, reconnect. The source recovers by itself. |
| **adb not found** | Install platform tools: `brew install --cask android-platform-tools`, or set the explicit path in the source's **Advanced** section. macOS may also ask to allow the `adb` binary on first run. |
| **Black screen** | 1) Check the OBS log (`Help → Log Files → View Current Log`) for `[obs-android-adb]` lines. 2) Set `OBS_ANDROID_ADB_DUMP=/tmp/android.h264`, reproduce and try `ffplay /tmp/android.h264` — if it plays, decoding is the issue (switch codec H.265 ↔ H.264); if not, transport/device is the issue. 3) Some devices fail to start an encoder at exotic sizes — try resolution Native or 1080p. |
| **No audio** | Audio capture needs Android 11+ and is blocked for DRM-protected content (Netflix etc.) — this is an Android `AudioPlaybackCapture` restriction. Also check "Capture device audio" in the source properties and that the source is not muted in the OBS mixer. The OBS log shows `audio capture started` or `audio disabled: ...`. |
| **decoder initialization failed** | The device produced parameter sets VideoToolbox rejected (rare OEM encoder quirk). Switch codec, or file an issue with the log excerpt. |
| **Device disconnected / Reconnecting…** | USB unplugged or the phone went to sleep and dropped adb. Re-plug / unlock the phone; auto reconnect handles the rest. Disable "keep device awake" only if you want the phone to sleep. |
| **High latency** | Lower the resolution/FPS/bitrate; prefer H.264; use a good USB-C data cable (charge-only cables don't carry data at all). |

## Logging

The plugin logs through OBS (`[obs-android-adb]` prefix): device connects,
server start with parameters, codec/resolution, decoder creation, audio start,
disconnects and reconnects. Nothing is logged per frame.

## License

- Plugin code: **GPL-2.0-or-later** (see [`LICENSE`](LICENSE)). libobs is
  GPL-2.0, and plugins linking against it are expected to carry a
  GPL-compatible license; the "or later" form additionally keeps the bundle
  compatible with the Apache-2.0 scrcpy-server.
- Bundled `scrcpy-server` and the implemented wire protocol are from
  [Genymobile/scrcpy](https://github.com/Genymobile/scrcpy) — **Apache-2.0**
  (license text shipped in `third_party/scrcpy-server/LICENSE`).
- Full component attribution: [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## Architecture

See [docs/architecture.md](docs/architecture.md) for the full design document
(data flow, threading, A/V synchronization, reconnect logic, protocol details,
risks).
