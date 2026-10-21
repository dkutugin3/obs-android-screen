# Third-party notices

This project (the `obs-android-adb` OBS Studio plugin) is licensed under the
**GPL-2.0-or-later** (see [`LICENSE`](LICENSE)). It builds on, bundles or
references the following third-party components:

## scrcpy-server (bundled)

- **What:** the unmodified upstream `scrcpy-server` binary (v4.1), pushed to
  the Android device at runtime. Bundled as
  `third_party/scrcpy-server/scrcpy-server-4.1` inside the plugin bundle's
  `Contents/Resources/`.
- **Source:** <https://github.com/Genymobile/scrcpy> (server module)
- **Copyright:** © Genymobile <https://www.genymobile.com>, Romain Vimont
- **License:** Apache License 2.0 — full text in
  [`third_party/scrcpy-server/LICENSE`](third_party/scrcpy-server/LICENSE)
- The client side of the scrcpy wire protocol implemented in this plugin
  follows scrcpy's app sources (`app/src/demuxer.c`, `app/src/server.c`,
  `server/src/.../DesktopConnection.java`, `Streamer.java`).

## Android platform tools — adb binary (bundled)

- **What:** the official `adb` executable from Google's Android SDK Platform
  Tools, downloaded at build time from
  <https://dl.google.com/android/repository/platform-tools-latest-darwin.zip>
  (universal Mach-O: arm64 + x86_64) and shipped inside the plugin bundle at
  `Contents/Resources/adb/adb`.
- **Copyright:** © Google LLC and the Android Open Source Project
  contributors
- **License:** Apache License 2.0 /
  <https://developer.android.com/tools/releases/platform-tools>
- The upstream `NOTICE.txt` is reproduced at
  [`third_party/adb/NOTICE.txt`](third_party/adb/NOTICE.txt) and shipped
  alongside the binary in the bundle.

## FFmpeg / libavcodec (bundled on Windows)

- **What:** `avcodec-62.dll` and `avutil-60.dll` from the BtbN win64 LGPL
  **shared** build of FFmpeg 8.1 (<https://github.com/BtbN/FFmpeg-Builds>),
  used for software H.264/HEVC decoding on Windows. Shipped next to the
  plugin DLL; also required at build time for the Windows target.
- **Copyright:** © FFmpeg developers
- **License:** LGPL-2.1-or-later — <https://ffmpeg.org/legal.html>
  (source: <https://github.com/BtbN/FFmpeg-Builds> / <https://ffmpeg.org>)
- The MinGW runtime DLLs (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`,
  `libwinpthread-1.dll`, GCC runtime library exception) are shipped next to
  the plugin as well.

## simde (build-time dependency)

- **What:** [simde](https://github.com/simd-everywhere/simde)
  (SIMD Everywhere) headers, amalgamated release 0.8.2. Required because the
  libobs headers include `<simde/x86/sse2.h>` on non-x86 platforms. Downloaded
  automatically at configure time (not stored in this repository).
- **Copyright:** © 2017 Evan Nemerson, © 2017–2024 simde contributors
- **License:** MIT:

  > MIT License
  >
  > Copyright (c) 2017 Evan Nemerson, (c) 2017-2024 simde contributors
  >
  > Permission is hereby granted, free of charge, to any person obtaining a copy
  > of this software and associated documentation files (the "Software"), to deal
  > in the Software without restriction, including without limitation the rights
  > to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  > copies of the Software, and to permit persons to whom the Software is
  > furnished to do so, subject to the following conditions:
  >
  > The above copyright notice and this permission notice shall be included in all
  > copies or substantial portions of the Software.
  >
  > THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  > IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  > FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  > AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  > LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  > OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  > SOFTWARE.

## obs-studio (libobs headers, runtime host)

- **What:** the OBS Studio sources (release tarball) are downloaded at
  configure time to compile against the `libobs` C API headers. At runtime the
  plugin is hosted by OBS.app and resolves libobs symbols dynamically
  (`-undefined dynamic_lookup`; nothing from obs-studio is redistributed in
  the plugin binary).
- **Source:** <https://github.com/obsproject/obs-studio>
- **License:** GPL-2.0 — <https://github.com/obsproject/obs-studio/blob/master/COPYING>

## Android

- The `adb` executable is **not** bundled; the plugin discovers the user's
  installation (Homebrew, system or a custom path in the source properties).
