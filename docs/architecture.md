# obs-android-adb — Architecture

Native OBS Studio source plugin that mirrors an Android device (Samsung Galaxy
S26, Android 16) screen **and system audio** over a plain USB-C cable, using the
official **scrcpy** server and protocol. No Window Capture, no macOS Screen
Capture, no scrcpy GUI, no virtual camera, no HDMI capture card.

```
Samsung Galaxy S26 (Android 16)
        │  USB-C
        ▼
   adb (platform-tools)  ──► scrcpy-server v4.1 (app_process)
        │                          │
        │   localabstract:scrcpy_XXXXXXXX  (adb forward)
        ▼                          ▼
  ┌─────────────────────────────────────────────┐
  │  obs-android-adb.plugin (arm64)             │
  │                                             │
  │  video socket ─► demux ─► VideoToolbox ─► obs_source_output_video
  │  audio socket ─► demux ─► (raw PCM) ────► obs_source_output_audio
  │  control socket (optional)                 │
  └─────────────────────────────────────────────┘
```

## 1. Why scrcpy-server (and which version)

The Android side (MediaProjection-free `SurfaceControl` display capture +
`MediaCodec` H.264/HEVC encoding + `AudioPlaybackCapture`) is complex and
device-quirky. Genymobile's **scrcpy** (Apache-2.0) has solved it over 10 years
of device workarounds. We reuse the **upstream, unmodified `scrcpy-server`
binary v4.1** (release asset `scrcpy-server-v4.1`) and speak the documented
client-side wire protocol implemented in `app/src/demuxer.c` of scrcpy.

The server jar is bundled inside the plugin bundle
(`Contents/Resources/scrcpy-server-v4.1`) and pushed to
`/data/local/tmp/scrcpy-server.jar` by the plugin at connect time. We do not
build or modify the Java server, which keeps the Android side exactly as
battle-tested upstream.

## 2. ADB transport

**Decision: spawn the `adb` binary as a subprocess** (exactly what scrcpy's own
"adb library" does — see `app/src/adb/adb.c` in scrcpy). Implementing the ADB
wire protocol (RSA key auth, transport multiplexing) from scratch would add
thousands of lines and legal/auth surface for zero benefit.

`adb` lookup order:
1. plugin setting `adb_path` (advanced property)
2. `<plugin bundle>/Contents/Resources/bin/adb` (optional user-supplied)
3. `/opt/homebrew/bin/adb`, `/usr/local/bin/adb`, `/usr/bin/adb`
4. `PATH`

ADB commands used:
- `adb devices -l` — enumerate devices (serial, state, `model:` property)
- `adb -s SERIAL shell getprop ro.product.marketname` — friendly name cache
- `adb -s SERIAL push scrcpy-server /data/local/tmp/scrcpy-server.jar`
- `adb -s SERIAL forward tcp:PORT localabstract:scrcpy_%08x` (and `--remove`)
- `adb -s SERIAL shell CLASSPATH=/data/local/tmp/scrcpy-server.jar app_process / com.genymobile.scrcpy.Server 4.1 <key=value args...>`
  (this process is kept alive for the whole session; server stderr is captured
  for diagnostics and logged through OBS `blog()`)

Tunnel mode: **`tunnel_forward=true`** (client connects through a localhost TCP
port). We bind no listening sockets; each source picks a free ephemeral port
and a random 31-bit `scid`, so multiple sources/devices can run concurrently.

## 3. scrcpy v4.1 wire protocol (verified against sources)

Server args we pass (all others default):

```
scid=<8 hex digits>            # abstract socket name scrcpy_<scid>
log_level=info
video=true
audio=true|false
audio_codec=raw                # PCM S16LE 48 kHz stereo, no decoder needed
audio_source=output            # system audio only, NOT the microphone
video_codec=h264|h265|av1
video_bit_rate=<bps>
max_fps=<fps>
max_size=<0|1080|1440>         # 0 = native resolution
tunnel_forward=true
control=true|false             # true only when "screen off" is requested
clipboard_autosync=false
stay_awake=<bool>
power_off_on_close=<bool>
send_device_meta=true
send_frame_meta=true
send_dummy_byte=true
send_stream_meta=true
```

`args[0]` must be exactly `4.1` (the server asserts the version matches).

### Socket order (client connects in this order over the forwarded port)
1. **video socket** — first connection. Server writes:
   - 1 dummy byte `0x00` (connectivity check)
   - 64 bytes UTF-8 device name (NUL-padded)
   - 4-byte BE codec id (`h264`=0x68323634, `h265`=0x68323635, `av1`=0x00617631)
   - 12-byte **session packet**: `0x80 00 00 0R | width u32be | height u32be`
     (R=client-resized flag; a new session packet is emitted mid-stream on
     every resolution/orientation change — this is how rotation is signalled)
   - frames
2. **audio socket** (only if `audio=true`) — second connection. Server writes:
   - 4-byte BE codec id; `raw`=0x00726177, `0` = stream disabled (non-fatal,
     e.g. Android < 11 or capture start failed → video continues),
     `1` = fatal configuration error
   - frames
3. **control socket** (only if `control=true`) — third connection. Server →
   client *device messages* (clipboard, etc.) which we drain and discard;
   client → server *control messages*. "Screen off while mirroring" is one
   2-byte message: `0x0A 0x00` (`TYPE_SET_DISPLAY_POWER`, off).

### Frame format (both sockets, 12-byte header, big-endian)
```
u64  PTS | flags        # bit63: session-only, bit62: CONFIG, bit61: KEY_FRAME,
                        # lower 61 bits: presentation timestamp in µs
                        # (device CLOCK_MONOTONIC — common timebase for A/V)
u32  payload size
...  payload            # video: Annex-B H.264/H.265 NAL units (MediaCodec CSD
                        #        for config packets: SPS/PPS[/VPS])
                        # audio: raw PCM S16LE interleaved stereo 48 kHz
```

Config packets (bit62) have no PTS. For H.26x the upstream client merges a
config packet into the next media packet before decoding; instead of merging we
hand SPS/PPS/VPS to VideoToolbox as a `CMVideoFormatDescription`, which is the
natural VT input.

## 4. Decoding

### Video — VideoToolbox (hardware, zero external deps)
- Parse config packet NALs → `CMVideoFormatDescriptionCreateFromH264ParameterSets`
  / `...FromHEVCParameterSets` (VPS+SPS+PPS) / AV1 TBD (experimental).
- Media packets: convert Annex-B start codes (`00 00 00 01`) to 4-byte
  big-endian length prefixes in one pass, wrap in `CMBlockBuffer`,
  submit via `VTDecompressionSessionDecodeFrame`.
- Session configured with `kVTDecompressionSessionPropertyID_RealTime = true`,
  output `kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange` (NV12).
  VT may fall back internally but on Apple Silicon it uses the hardware decoder.
- Format change (rotation, resolution): session packet announces new size, then
  a config packet with new SPS arrives. When the parameter sets differ from the
  current ones we recreate the format description and (if
  `VTDecompressionSessionCanAcceptFormatDescription` is false) the session.
  Frame dimensions in OBS follow the decoded frames — async sources handle
  mid-stream size changes natively.
- Output callback (VT thread): copy CVPixelBuffer NV12 planes into a pooled
  `obs_source_frame` (Y + interleaved UV) and call `obs_source_output_video()`.
  The copy is ~3 MB/frame at 1080p60 and keeps CVPixelBuffer lifetime trivially
  correct; it is not the latency bottleneck (<0.5 ms on M4).

No software-decoder fallback is shipped: VideoToolbox on Apple Silicon covers
H.264/HEVC in hardware; adding a bundled FFmpeg would balloon the dependency
surface. The "hardware decoding" toggle is therefore not exposed (documented
deviation from the spec).

### Audio — raw PCM passthrough
`audio_codec=raw` makes the server send **unencoded** PCM (48 kHz stereo S16LE
interleaved — ~192 kB/s over USB). We wrap each packet in `obs_source_audio`
(`AUDIO_FORMAT_16BIT`, `SPEAKERS_STEREO`, 48000) and call
`obs_source_output_audio()`. No decoder, no drift, no resampling.

## 5. OBS integration

- One input source: id `android_adb_screen`, name **"Android Screen (ADB)"**.
- `output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO` — video via
  `obs_source_output_video()` (async frames, any thread), audio via
  `obs_source_output_audio()` (shows up in the Audio Mixer as a normal source).
- Properties UI: device combo (friendly names), codec, resolution, FPS,
  bitrate, capture-audio / screen-off / stay-awake / auto-reconnect checkboxes,
  Refresh devices button. Status line reflects connection state.
- Device enumeration: a process-wide `DeviceMonitor` singleton keeps one
  long-lived `adb track-devices -l` process (spawned only while at least one
  source is active or a properties pane is open — zero processes while idle),
  caches serial → {state, model, market name}, and fans out change
  notifications to sources (used by auto-reconnect) and properties — but only
  when the device set actually changes. `adb track-devices` frames each
  message as a 4-hex-digit length prefix + the full current device list
  (empty list = `0000`), so a connect/disconnect is committed the moment adb
  emits it, with no polling interval. If the framing is ever unexpected or
  the spawn fails, the monitor degrades to one-shot `adb devices -l` polls.
- Friendly names: `Galaxy S26 (R3CW...)` — market name from
  `getprop ro.product.marketname`, fallback to `model:` from `adb devices -l`,
  fallback to serial only.
- Logging via `blog()`; per-frame events are never logged.
- A "disconnected/reconnecting/unauthorized" placeholder frame is rendered once
  (CoreText → grayscale NV12 frame) so the user sees state on the canvas.

## 6. Threading model

```
OBS main thread        create/destroy/update/properties (all OBS API),
                       starts/stops the session
DeviceMonitor thread   adb track-devices event reader, name cache
                       (one long-lived process, no polling)
session thread (1/src) state machine: push → forward → exec server → connect
                       sockets → start demuxers; on EOS: teardown + reconnect
video thread (1/src)   reads video socket, feeds VT
audio thread (1/src)   reads audio socket, emits obs audio
control thread (1/src) drains control socket device messages
VT callback threads    frame copy → obs_source_output_video
```

All cross-thread state is mutex/atomic guarded; OBS API calls that require the
graphics thread are not used (async source needs none). Shutdown ordering:
cancel → join worker threads → close sockets (which unblocks readers) → kill
`adb shell` process → remove forward → free decoder.

## 7. Source lifetime & reconnect

States: `Idle → Connecting → Streaming ⇄ Reconnecting → (Destroyed)`.

- `update()` while streaming: changes that need a server restart (codec,
  resolution, fps, bitrate, audio on/off, device) trigger a **same-scid-less
  clean restart** (teardown + connect, no user interaction); cosmetic changes
  apply immediately.
- Socket EOF / adb death → `Reconnecting`: placeholder frame "Reconnecting…",
  then retry loop (1 s → 2 s → 5 s, capped). Before each attempt the device
  list is checked; if the serial is gone we wait for the `DeviceMonitor`
  "device appeared" event instead of hammering adb. USB re-plug of the *same*
  phone therefore resumes automatically; the OBS source is never removed.
- Unauthorized device → immediate, clear error (see §9); retry continues so
  accepting the prompt on the phone recovers the stream.

## 8. A/V synchronization & latency

- Video and audio PTS share the device monotonic clock (verified:
  `AudioRecordReader` uses `getTimestamp(TIMEBASE_MONOTONIC)`; MediaCodec
  surface-input PTS derive from the same clock).
- Per stream, on first packet: `offset_ns = os_gettime_ns() − pts*1000`.
  Every frame: `obs_ts = pts*1000 + offset_ns`. Because both offsets are
  anchored at (nearly) the same wall moment, A/V stay in sync without any
  drift correction; OBS aligns audio and video on these timestamps.
- Latency budget (target < 100 ms end-to-end): encoder on device
  (~10–30 ms, I-frame interval left at scrcpy defaults) + USB transport
  (~1–3 ms) + demux (~0) + VT hardware decode (~4–10 ms in realtime mode) +
  copy (<1 ms) + OBS composite (1 frame). No frame queues: demux thread hands
  each packet straight to the decoder; VT is the only queue and runs realtime.
  If decoding falls behind, VideoToolbox in realtime mode drops late frames
  rather than accumulating delay.

## 9. Error handling (user-visible messages)

| Situation | Behavior |
|---|---|
| adb not found | "ADB was not found. Install Android Platform Tools (`brew install --cask android-platform-tools`) or set the path in Advanced." |
| device unauthorized | "Device is not authorized. Unlock your phone and accept the USB debugging prompt." — retries until accepted |
| device offline | waits, retries when `adb devices` reports `device` |
| server exec failure | logged with captured server stderr |
| audio disabled by device (id 0) | warning; video continues |
| audio config error (id 1) | error; video continues |
| decoder init failure | error + placeholder frame; reconnect suppressed until settings change |
| socket EOF / USB unplug | placeholder "Reconnecting…" + auto-reconnect |

## 10. Dependencies & licensing

| Component | Source | License | Linked |
|---|---|---|---|
| libobs API | OBS Studio 32.x headers (fetched at configure) | GPL-2.0 (API usage) | unresolved at build, resolved from OBS.app at load (`-undefined dynamic_lookup`) |
| VideoToolbox / CoreMedia / CoreVideo / CoreText / CoreGraphics | macOS system | Apple SDK | system frameworks |
| scrcpy-server v4.1 binary | Genymobile release | Apache-2.0 | pushed to device |
| adb binary | user-installed platform-tools (optional bundled copy) | Apache-2.0 | subprocess |

Plugin code: **GPL-2.0-or-later**. libobs is GPL-2.0 and the OBS project
treats plugins linking libobs as derivative works, so the plugin carries a
GPL-compatible license; the "or later" form keeps the bundled Apache-2.0
scrcpy-server compatible (via the GPLv3 path). Nothing from GPL-licensed
*client* code of other projects is copied; the wire protocol follows the
behavior of scrcpy's Apache-2.0 sources. See `THIRD_PARTY_NOTICES.md`.

## 11. Risks

- **Protocol drift**: scrcpy may change the protocol in v4.2+. Mitigation:
  server binary + version string are pinned (`4.1`) in one place; the server
  itself rejects version mismatch loudly.
- **OEM encoder quirks** (Samsung Exynos/Snapdragon): scrcpy server handles
  the known ones; `downsize_on_error` stays enabled.
- **Android < 11 devices**: no raw audio (server sends disable code 0) — video
  still works. Primary target (S26/Android 16) unaffected.
- **AV1**: Samsung encoder support varies; offered as experimental only.
- **adb server not running**: first command auto-starts it; macOS may ask for
  permission to run adb — surfaced via clear error text.
- **OBS load-order**: plugin uses only late-bound libobs symbols; safe with
  `-undefined dynamic_lookup` because OBS loads libobs before plugin modules.

## 12. Milestones

1. Plugin skeleton, CMake, source registration, device list in properties.
2. Full server bring-up + H.264 elementary stream dump to `/tmp/android.h264`
   (debug env `OBS_ANDROID_ADB_DUMP=<path>`) — proves transport without decoding.
3. VideoToolbox decode → live video in OBS (MVP).
4. Settings: codec/resolution/fps/bitrate; rotation via session packets.
5. Raw audio capture → OBS audio.
6. Auto-reconnect, placeholder states, packaging, install script, README.

## 13. Verification status

| Area | Method | Status |
|---|---|---|
| Plugin loads in OBS | OBS 32.2.2 `--verbose` run, log check | ✅ loads, registers source, resolves bundled server, clean unload |
| `adb devices -l` parsing | Unit tests (device / unauthorized / offline / garbage lines) | ✅ |
| Annex-B splitting | Unit tests (3/4-byte start codes, trailing zeros) | ✅ |
| VideoToolbox H.264 decode | Synthetic 1080×2340@60 stream (ffmpeg) → 120/120 frames, NV12 | ✅ hardware |
| VideoToolbox HEVC decode | Synthetic stream → 120/120 frames | ✅ hardware |
| Rotation (session change) | Portrait stream → new SPS/PPS → landscape stream in one decoder: 180 frames, final 2340×1080 | ✅ |
| Placeholder renderer | Non-empty NV12 with rendered text | ✅ |
| Live device transport | Samsung SM-S942B (Android 16), adb 37.0.1 | ✅ see §14.1 |

### 14.1 Live test results (Samsung SM-S942B / Android 16, adb 37.0.1)

A live run against the phone exposed **seven** real bugs, all fixed and
re-verified live:

1. **adb 37 output format**: `adb devices -l` separates the serial from the
   state with a *run of spaces*, not a tab (older adb used a tab; scrcpy's
   parser reads the line backwards for the same reason). The parser now splits
   on any whitespace run; unit test covers both formats.
2. **adb forward connect race**: a TCP connect to the forwarded port succeeds
   immediately (the forward accepts locally) long before the scrcpy server is
   listening on the device, so the first `recv` returns EOF. The client now
   retries the connect+dummy-byte pair (80 × 250 ms, 1 s receive timeout),
   exactly like scrcpy's `connect_and_read_byte()`.
3. **Socket-order deadlock (protocol subtlety)**: the server writes the dummy
   byte right after the video `accept()`, but the 64-byte device name only
   *after every enabled socket (video → audio → control) has been accepted*.
   Reading the device name before connecting the audio socket deadlocks both
   sides. All sockets are now connected before reading the name.
4. **Green picture (color conversion)**: for `full_range == false` frames the
   libobs conversion shader applies `frame->color_range_min/max[3]`; leaving
   them zeroed (only the matrix was filled) makes the shader divide by a zero
   range — the G channel saturates and R/B collapse, rendering the source as
   pure green. `video_format_get_parameters()` now fills the matrix AND both
   range vectors. Verified by recording the OBS canvas while the phone showed
   a solid red page: the recording contains `#ff0000`.
5. **Decoder/lock deadlock**: calling `VTDecompressionSessionDecodeFrame` while
   holding the lock that the VT output callback also needs deadlocks once
   VideoToolbox' internal queue fills (~2 frames in): the video demux thread
   blocks holding `frameMutex_`, VT callbacks cannot deliver, TCP backpressure
   freezes the server-side writer, and the session hangs silently while
   looking alive. The decoder is now owned exclusively by the video demux
   thread, which never holds `frameMutex_` across decoder calls; VT callbacks
   are the only ones taking the mutex. Verified by a continuous 20 Mbit/s
   stream (165 MB dumped over ~10 minutes) where the pre-fix build froze at
   151 KB.

6. **Exit crash (`DeviceMonitor` destructor)**: the singleton's defaulted
   destructor left a joinable `std::thread` at process exit → `std::terminate`
   → OBS aborted on *every* quit (crash report `OBS-2026-09-26-072930.ips`),
   which also left the "unclean shutdown" dialog for the next start. The
   destructor now sets the stop flag, kills the adb tracker process (its death
   closes the pipe and wakes the blocked `read()`) and joins the thread.
   Verified live: OBS quits cleanly, no new crash report is produced, and the
   log ends with `device event stream ended` → `device monitor thread stopped`.
7. **Placeholder text upside down (CoreText CTM)**: the placeholder renderer
   wrapped `CTLineDraw` in the "canonical" top-left flip
   (`TranslateCTM` + `ScaleCTM(1,-1)`) — but in a `CGBitmapContext` CoreText
   already renders upright glyphs with a bottom-left origin that maps directly
   onto the top-down buffer, so the flip mirrored every glyph (all placeholders
   — "Device disconnected", "unauthorized", "select a device" — showed upside
   down). Verified empirically: a fill at low CG y lands in the last buffer
   rows, and a huge `L` drawn with the flip is foot-at-top. The flip is
   removed; positions are converted from-the-top instead. Regression-tested in
   the offline suite (ink-mass distribution of an `L` + two-line layout
   order).

Live-verified end to end on the phone:
- device enumeration with model name (`SM-S942B (RFGL61W5MQL)`);
- continuous H.264 1080p-class capture at the full 20 Mbit/s (ffprobe-valid
  elementary stream, 1920x886 landscape);
- **decoded frames delivered to OBS** (`first decoded frame 1920x886 -> OBS`
  log; the audio/video dumps below are gated by `OBS_ANDROID_ADB_DUMP` /
  `OBS_ANDROID_ADB_DUMP_AUDIO`);
- **system audio capture at the exact expected rate**: the raw PCM dump grows
  at precisely 192,000 B/s = 48 kHz × 2ch × 16-bit;
- **rotation portrait ↔ landscape mid-stream** (session packets, decoder
  rebuilt, both directions observed live);
- hardware VideoToolbox decoding;
- **auto-reconnect**: killing the on-device server (equivalent to a dropped
  tunnel) → the source re-established the session and resumed the full-bitrate
  stream within seconds, without user interaction;
- event-driven device tracking live (`tracking device events (adb
  track-devices)`, device list populated instantly) and **a clean quit** with
  the full shutdown log sequence and no new crash report;
- OBS 32.2.2 stayed alive and stable throughout (~15 min, no crash).


### 14.2 CPU footprint (measured 2026-09-26, MacBook Pro M4, 1080p-class
### landscape @ 60 fps, H.264 20 Mbit/s + raw PCM audio, preview rendered)

- Steady state, measured as `ps` cputime deltas: **≈ 2.0–2.2 s CPU per 10 s
  wall ≈ 20 % of one P-core ≈ 2 % of the whole machine** (OBS's own stats
  overlay agrees: "CPU: 2.0 %", 60 fps). That figure covers the whole
  pipeline: socket demux, VideoToolbox hardware decode, the NV12 copy into
  obs frames (~2.9 MB/frame ≈ 2–3 % of a core) *and* OBS rendering the
  preview canvas — hiding the preview removes the largest single share.
- Device tracking costs nothing measurable: one long-lived
  `adb track-devices -l` process replaces ~1800 `adb devices -l` process
  spawns per hour of the old 2 s polling, and observers fire only on actual
  device-set changes. A zero-copy IOSurface texture path was considered and
  deliberately skipped: the copy is a few percent of one core and the async
  source API would need bypassing for it.

Debugging note: OBS buffers stdout when piped — launch the OBS binary under a
PTY (`script -q log.txt …/OBS --verbose`) for live plugin logs, and remove
`~/Library/Application Support/obs-studio/.sentinel` if OBS was force-killed,
otherwise it blocks startup with the "unclean shutdown" dialog.

## 14. Implementation notes (deviations & gotchas found during coding)

- **AV1 is not exposed in the UI**: VideoToolbox's public API has no
  `CMVideoFormatDescriptionCreateFrom*` entry for AV1 parameter sets in the
  supported deployment target, so AV1 was cut instead of shipping a broken
  option. H.264/H.265 are fully supported.
- **No software-decoder fallback / hardware toggle**: VideoToolbox is the only
  decoder shipped (see §4); a "hardware decoding" checkbox would be a no-op.
- **Header-only libobs usage**: the plugin is linked with
  `-undefined dynamic_lookup`; no libobs binary is needed at build time, and
  `otool -L` shows only system frameworks.
- **obs-studio tarball gaps**: the release tarball lacks submodules and
  generated headers, so the build downloads `simde` separately and generates
  `obsconfig.h` from `obsconfig.h.in`.
- **CMBlockBuffer ownership**: frame data is copied into a CoreMedia-owned
  block (`CMBlockBufferCreateWithMemoryBlock(NULL, …)` +
  `CMBlockBufferReplaceDataBytes`); passing our own pointer with an allocator
  would make CoreMedia `free()` plugin memory.
- **Session/description lifetime**: `VTDecompressionSession` and
  `CMVideoFormatDescription` have separate ownership in `video::Decoder`; the
  session is recreated (after `flush()`) whenever the parameter sets change
  and the running session cannot accept the new description.
- **Locale**: `obs_module_text()` works only in the module TU; the rest of the
  plugin calls the exported `aadText()` helper defined in `plugin.cpp`.
- **Bundle data**: bundle assembly is a dedicated always-running CMake target
  (a POST_BUILD step would miss data-only changes).
