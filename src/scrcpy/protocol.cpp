#include "protocol.hpp"

namespace scrcpy {

uint16_t read16be(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

uint32_t read32be(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

uint64_t read64be(const uint8_t *p)
{
	return (uint64_t)read32be(p) << 32 | read32be(p + 4);
}

const char *videoCodecName(VideoCodecId id)
{
	switch (id) {
	case VideoCodecId::H264:
		return "H264";
	case VideoCodecId::H265:
		return "H265";
	case VideoCodecId::AV1:
		return "AV1";
	}
	return "unknown";
}

// Find the next Annex-B start code ("00 00 01", optionally preceded by extra
// zero bytes) at or after `from`. On success sets *nalStart* to the first byte
// of the NAL payload and *codeEnd* to the first byte of the run of zeros that
// constitutes the start code (i.e. the previous NAL ends there).
static bool findStartCode(const uint8_t *data, size_t size, size_t from, size_t *codeStart, size_t *nalStart)
{
	for (size_t j = from; j + 3 <= size; j++) {
		if (data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1) {
			size_t begin = j;
			while (begin > from && data[begin - 1] == 0)
				begin--; // 4-byte start code / trailing zero bits
			*codeStart = begin;
			*nalStart = j + 3;
			return true;
		}
	}
	return false;
}

std::vector<NalUnit> splitAnnexB(const uint8_t *data, size_t size)
{
	std::vector<NalUnit> nals;
	size_t codeStart = 0, nalStart = 0;
	if (!findStartCode(data, size, 0, &codeStart, &nalStart))
		return {};
	while (nalStart < size) {
		size_t nextCodeStart = 0, nextNalStart = 0;
		if (!findStartCode(data, size, nalStart, &nextCodeStart, &nextNalStart)) {
			nals.push_back({data + nalStart, size - nalStart});
			break;
		}
		if (nextCodeStart > nalStart)
			nals.push_back({data + nalStart, nextCodeStart - nalStart});
		nalStart = nextNalStart;
	}
	return nals;
}

} // namespace scrcpy
