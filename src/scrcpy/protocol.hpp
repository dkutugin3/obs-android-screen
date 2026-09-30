#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scrcpy {

// 4-byte codec ids on the wire (big-endian ASCII), see scrcpy Codec.java
enum class VideoCodecId : uint32_t {
	H264 = 0x68323634, // "h264"
	H265 = 0x68323635, // "h265"
	AV1 = 0x00617631,  // "av1"
};

enum class AudioCodecId : uint32_t {
	Opus = 0x6f707573,
	Aac = 0x00616163,
	Flac = 0x666c6163,
	Raw = 0x00726177,
};

// 12-byte frame header flags (scrcpy Streamer.java)
constexpr uint64_t PACKET_FLAG_SESSION = UINT64_C(1) << 63;
constexpr uint64_t PACKET_FLAG_CONFIG = UINT64_C(1) << 62;
constexpr uint64_t PACKET_FLAG_KEY_FRAME = UINT64_C(1) << 61;
constexpr uint64_t PACKET_PTS_MASK = PACKET_FLAG_KEY_FRAME - 1;

// Control message types (scrcpy ControlMessage.java)
constexpr uint8_t CONTROL_MSG_TYPE_SET_DISPLAY_POWER = 10;

struct PacketHeader {
	uint64_t ptsAndFlags = 0;
	uint32_t size = 0;

	bool isConfig() const { return ptsAndFlags & PACKET_FLAG_CONFIG; }
	bool isKeyFrame() const { return ptsAndFlags & PACKET_FLAG_KEY_FRAME; }
	uint64_t ptsUs() const { return ptsAndFlags & PACKET_PTS_MASK; }
};

// A "session" pseudo-packet announcing (width, height); occurs at stream start
// and on every resolution/orientation change.
struct SessionMeta {
	uint32_t width = 0;
	uint32_t height = 0;
	bool clientResized = false;
};

uint16_t read16be(const uint8_t *p);
uint32_t read32be(const uint8_t *p);
uint64_t read64be(const uint8_t *p);

const char *videoCodecName(VideoCodecId id);

// Split an Annex-B buffer into individual NAL units (without start codes).
// Returns offsets+lengths into the input buffer.
struct NalUnit {
	const uint8_t *data;
	size_t size;
};
std::vector<NalUnit> splitAnnexB(const uint8_t *data, size_t size);

} // namespace scrcpy
