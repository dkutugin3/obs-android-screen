#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Renders a small status placeholder as an NV12 frame (grayscale: text is
// drawn into the Y plane, UV plane is neutral). Used for "Reconnecting…",
// "Device disconnected", "Unauthorized" states on the OBS canvas.
namespace placeholder {

struct Frame {
	std::vector<uint8_t> data; // Y plane + interleaved UV plane
	uint32_t width = 0;
	uint32_t height = 0;

	bool valid() const { return width > 0 && height > 0 && !data.empty(); }
};

// `line1` is drawn large, `line2` smaller below it.
Frame render(const std::string &line1, const std::string &line2, uint32_t width, uint32_t height);

} // namespace placeholder
