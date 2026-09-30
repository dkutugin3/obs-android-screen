#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <CoreMedia/CMFormatDescription.h>
#include <VideoToolbox/VTDecompressionSession.h>

#include "scrcpy/protocol.hpp"

namespace video {

// Hardware H.264 / HEVC decoder built directly on VideoToolbox.
// Input: Annex-B elementary stream (scrcpy wire format).
// Output: NV12 CVPixelBuffer frames delivered on a VideoToolbox worker thread.
class Decoder {
public:
	// Receives a retained CVPixelBuffer (NV12) plus the presentation timestamp
	// in nanoseconds. Called synchronously from the VT output callback.
	using OutputFn = std::function<void(CVPixelBufferRef pixelBuffer, uint64_t ptsNs, bool keyFrame)>;

	explicit Decoder(OutputFn output, scrcpy::VideoCodecId codec);
	~Decoder();

	Decoder(const Decoder &) = delete;
	Decoder &operator=(const Decoder &) = delete;

	// Feed a codec config packet (Annex-B parameter sets). Accumulates
	// SPS/PPS(/VPS) across packets and (re)creates the session once a complete
	// set is available. Returns false on a fatal setup error.
	bool feedConfig(const uint8_t *data, size_t len);

	// Submit a media packet. Returns false if it could not be submitted
	// (decoder not ready yet, or fatal VT error).
	bool decode(const uint8_t *data, size_t len, uint64_t ptsUs, bool keyFrame);

	bool ready() const { return session_ != nullptr; }

	void flush();

	// Passed as the VT callback refcon.
	struct OutputContext {
		OutputFn output;
	};

private:
	bool createSession();
	void destroySession();
	bool buildFormatDescription();

	OutputFn output_;
	OutputContext outputCtx_{};

	scrcpy::VideoCodecId codec_ = scrcpy::VideoCodecId::H264;
	std::vector<uint8_t> sps_, pps_, vps_;
	bool haveSps_ = false, havePps_ = false, haveVps_ = false;

	CMFormatDescriptionRef formatDesc_ = nullptr;
	VTDecompressionSessionRef session_ = nullptr;
	std::vector<uint8_t> avccBuffer_; // Annex B -> length-prefixed scratch
};

} // namespace video
