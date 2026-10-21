#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "scrcpy/protocol.hpp"

namespace video {

// A decoded frame in NV12: a Y plane and an interleaved UV plane. The
// pointers stay valid until the decoder processes the next packet — the
// buffers are owned by the backend.
struct DecodedFrame {
	const uint8_t *y = nullptr;
	const uint8_t *uv = nullptr;
	uint32_t yStride = 0;
	uint32_t uvStride = 0;
	uint32_t width = 0;
	uint32_t height = 0;
};

// H.264 / HEVC decoder over the Annex-B elementary stream (scrcpy wire
// format). Backends: VideoToolbox (macOS, hardware) and libavcodec
// (Windows, software). Output: NV12 frames on a decoder worker thread.
class Decoder {
public:
	using OutputFn = std::function<void(const DecodedFrame &, uint64_t ptsNs, bool keyFrame)>;

	explicit Decoder(OutputFn output, scrcpy::VideoCodecId codec);
	~Decoder();

	Decoder(const Decoder &) = delete;
	Decoder &operator=(const Decoder &) = delete;

	// Feed a codec config packet (Annex-B parameter sets). Returns false on
	// a fatal setup error.
	bool feedConfig(const uint8_t *data, size_t len);

	// Submit a media packet (Annex-B access unit). Returns false if it
	// could not be submitted.
	bool decode(const uint8_t *data, size_t len, uint64_t ptsNs, bool keyFrame);

	bool ready() const;

	// Drop all pending/delayed frames (called on rotation and reconnect).
	void flush();

	struct Impl;

private:
	Impl *impl_;
};

} // namespace video
