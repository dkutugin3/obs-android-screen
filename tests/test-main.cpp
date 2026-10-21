// Offline test harness: exercises the adb output parser, the Annex-B splitter,
// the placeholder renderer and the VideoToolbox decoder against a synthetic
// H.264 elementary stream. No device required.
//
// Provides a local `blog` implementation so the plugin sources link without
// libobs.

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "adb/adb-client.hpp"
#include "placeholder.hpp"
#include "scrcpy/protocol.hpp"
#include "video/decoder.hpp"

// --- libobs stubs ----------------------------------------------------------
extern "C" void blog(int log_level, const char *format, ...)
{
	(void)log_level;
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

static int gFailures = 0;
#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                \
			gFailures++;                                                                                  \
		}                                                                                                      \
	} while (0)

static void testParseDevices()
{
	// adb 37 pads the serial with spaces; older adb used a tab
	const char *sample = "List of devices attached\n"
			     "R3CW30ABCDEF            device usb:3-1 product:o1sxxx model:SM-S931B device:o1s transport_id:4\n"
			     "ABC123\tunauthorized usb:3-2 transport_id:5\n"
			     "999XYZ   offline usb:3-3 transport_id:6\n"
			     "* daemon not running; starting now at tcp:5037\n"
			     "adb server version (41) doesn't match this client (39); killing...\n";

	auto devices = adb::parseDevicesOutput(sample);
	CHECK(devices.size() == 3);
	if (devices.size() == 3) {
		CHECK(devices[0].serial == "R3CW30ABCDEF");
		CHECK(devices[0].state == adb::DeviceState::Ok);
		CHECK(devices[0].model == "SM-S931B");
		CHECK(devices[1].serial == "ABC123");
		CHECK(devices[1].state == adb::DeviceState::Unauthorized);
		CHECK(devices[2].state == adb::DeviceState::Offline);
	}
}

static void testSplitAnnexB()
{
	// [00 00 00 01] SPS [00 00 00 01] PPS [00 00 01] IDR
	uint8_t data[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0xAA, 0x00, 0x00, 0x00, 0x01, 0x68,
		0xBB, 0x00, 0x00, 0x01, 0x65, 0xCC, 0xCC};

	auto nals = scrcpy::splitAnnexB(data, sizeof(data));
	CHECK(nals.size() == 3);
	if (nals.size() == 3) {
		CHECK(nals[0].size == 2 && nals[0].data[0] == 0x67);
		CHECK(nals[1].size == 2 && nals[1].data[0] == 0x68);
		CHECK(nals[2].size == 3 && nals[2].data[0] == 0x65);
	}
}

static void testPlaceholder()
{
	auto f = placeholder::render("Device disconnected", "Reconnecting...", 1280, 720);
	CHECK(f.valid());
	CHECK(f.data.size() == (size_t)1280 * 720 * 3 / 2);
	// text must have produced brighter pixels than the background
	int bright = 0;
	for (size_t i = 0; i < (size_t)1280 * 720; i++)
		if (f.data[i] > 0x40)
			bright++;
	CHECK(bright > 1000);

	// Glyph orientation regression (bug #7): a huge 'L' must be upright in
	// the buffer — its ink mass must sit in the LOWER half of its band (the
	// foot), not the upper half. A CTM flip in the renderer mirrors glyphs.
	// Scanned in the LEFT half only: the corner credit lives bottom-right.
	auto g = placeholder::render("L", "", 640, 360);
	CHECK(g.valid());
	auto inkAt = [&g](uint32_t row) {
		long ink = 0;
		for (uint32_t c = 0; c < g.width / 2; c++)
			if (g.data[(size_t)row * g.width + c] > 0x40)
				ink++;
		return ink;
	};
	uint32_t r0 = 0, r1 = 0;
	bool found = false;
	for (uint32_t r = 0; r < g.height && !found; r++)
		if (inkAt(r) > 0) { r0 = r; found = true; }
	CHECK(found);
	if (found) {
		for (uint32_t r = g.height; r-- > 0;)
			if (inkAt(r) > 0) { r1 = r; break; }
		uint32_t mid = (r0 + r1) / 2;
		long upper = 0, lower = 0;
		for (uint32_t r = r0; r <= r1; r++)
			(r <= mid ? upper : lower) += inkAt(r);
		CHECK(lower > upper); // upright: foot-heavy
	}

	// Layout: first line above second line, both in the upper-middle band
	// (left half — the credit caption sits bottom-right).
	auto t = placeholder::render("BIGLINE", "smallline", 640, 360);
	CHECK(t.valid());
	int firstRow = -1, prevInk = -10, lines = 0;
	for (uint32_t r = 0; r < t.height; r++) {
		long ink = 0;
		for (uint32_t c = 0; c < t.width / 2; c++)
			if (t.data[(size_t)r * t.width + c] > 0x40)
				ink++;
		if (ink > 3) {
			if (prevInk < (int)r - 1) {
				lines++;
				if (firstRow < 0)
					firstRow = (int)r;
			}
			prevInk = (int)r;
		}
	}
	CHECK(lines == 2);
	CHECK(firstRow > 0 && firstRow < (int)t.height / 2); // starts in the top half

	// Credit caption: dim ink in the bottom-right quadrant, above background.
	long credit = 0;
	for (uint32_t r = t.height * 3 / 4; r < t.height; r++)
		for (uint32_t c = t.width * 3 / 5; c < t.width; c++)
			if (t.data[(size_t)r * t.width + c] > 0x20 && t.data[(size_t)r * t.width + c] < 0xC0)
				credit++;
	CHECK(credit > 20);
}

// --- Decoder over a synthetic stream (VideoToolbox on macOS, libavcodec on
// Windows) ------------------------------------------------------------------

static bool fileReadable(const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return false;
	fclose(fp);
	return true;
}

static std::vector<uint8_t> readFile(const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return {};
	std::vector<uint8_t> out;
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
		out.insert(out.end(), buf, buf + n);
	fclose(fp);
	return out;
}

static bool isH264Slice(uint8_t type) { return type == 1 || type == 5; }
static bool isHevcVcl(uint8_t type) { return type < 32; } // 0..31 = VCL NALs

static void testDecoderStream(const char *path, scrcpy::VideoCodecId codec, uint32_t expectW, uint32_t expectH)
{
	bool h264 = codec == scrcpy::VideoCodecId::H264;
	auto es = readFile(path);
	CHECK(!es.empty());
	if (es.empty())
		return;

	// Split to NALs; build a config packet from SPS/PPS and group slices into
	// access units, mirroring what the scrcpy server sends.
	auto nals = scrcpy::splitAnnexB(es.data(), es.size());
	CHECK(nals.size() > 4);
	if (nals.size() <= 4)
		return;

	std::vector<uint8_t> config;
	std::vector<std::vector<uint8_t>> accessUnits;
	std::vector<uint8_t> current;
	bool currentHasSlice = false;

	auto flushAu = [&]() {
		if (!current.empty()) {
			accessUnits.push_back(current);
			current.clear();
		}
		currentHasSlice = false;
	};

	for (auto &nal : nals) {
		if (nal.size < 1)
			continue;
		uint8_t type = h264 ? (nal.data[0] & 0x1f) : ((nal.data[0] >> 1) & 0x3f);
		bool isConfig = h264 ? (type == 7 || type == 8) : (type == 32 || type == 33 || type == 34);
		bool isVcl = h264 ? isH264Slice(type) : isHevcVcl(type);
		if (isConfig) { // parameter sets -> config packet
			static const uint8_t sc4[4] = {0, 0, 0, 1};
			config.insert(config.end(), sc4, sc4 + 4);
			config.insert(config.end(), nal.data, nal.data + nal.size);
			continue;
		}
		if (isVcl) {
			if (currentHasSlice)
				flushAu(); // two consecutive VCL NALs: new access unit
			currentHasSlice = true;
		}
		static const uint8_t sc4[4] = {0, 0, 0, 1};
		current.insert(current.end(), sc4, sc4 + 4);
		current.insert(current.end(), nal.data, nal.data + nal.size);
	}
	flushAu();

	fprintf(stderr, "stream: %zu NALs, %zu access units, config %zu bytes\n", nals.size(), accessUnits.size(),
		config.size());
	CHECK(accessUnits.size() >= 100); // 2s at 60fps minus a few

	std::atomic<int> decodedFrames(0);
	std::atomic<uint32_t> width(0), height(0);
	video::Decoder decoder(
		[&](const video::DecodedFrame &f, uint64_t ptsNs, bool kf) {
			(void)ptsNs;
			(void)kf;
			width = f.width;
			height = f.height;
			decodedFrames++;
		},
		codec);

	CHECK(decoder.feedConfig(config.data(), config.size()));
	CHECK(decoder.ready());

	uint64_t pts = 0;
	for (auto &au : accessUnits) {
		decoder.decode(au.data(), au.size(), pts, false);
		pts += 16667000; // ns (~60 fps)
	}
	decoder.flush();

	fprintf(stderr, "decoded frames: %d, size %ux%u\n", decodedFrames.load(), width.load(), height.load());
	CHECK(decodedFrames.load() >= (int)accessUnits.size() - 3);
	CHECK(width == expectW && height == expectH);
}

// Rotation: feed a portrait stream, then a landscape stream with new
// parameter sets into the SAME decoder (what happens when the phone rotates).
static void testRotation(const char *portrait, const char *landscape)
{
	struct Parsed {
		std::vector<uint8_t> config;
		std::vector<std::vector<uint8_t>> aus;
	};
	auto parse = [](const char *path) {
		Parsed p;
		auto es = readFile(path);
		auto nals = scrcpy::splitAnnexB(es.data(), es.size());
		std::vector<uint8_t> cur;
		bool hasVcl = false;
		static const uint8_t sc4[4] = {0, 0, 0, 1};
		for (auto &nal : nals) {
			if (nal.size < 1)
				continue;
			uint8_t t = nal.data[0] & 0x1f;
			if (t == 7 || t == 8) {
				p.config.insert(p.config.end(), sc4, sc4 + 4);
				p.config.insert(p.config.end(), nal.data, nal.data + nal.size);
				continue;
			}
			if ((t == 1 || t == 5) && hasVcl) {
				p.aus.push_back(cur);
				cur.clear();
				hasVcl = false;
			}
			if (t == 1 || t == 5)
				hasVcl = true;
			cur.insert(cur.end(), sc4, sc4 + 4);
			cur.insert(cur.end(), nal.data, nal.data + nal.size);
		}
		if (!cur.empty())
			p.aus.push_back(cur);
		return p;
	};

	Parsed a = parse(portrait);
	Parsed b = parse(landscape);

	std::atomic<int> frames(0);
	std::atomic<uint32_t> lastW(0), lastH(0);
	video::Decoder dec(
		[&](const video::DecodedFrame &f, uint64_t, bool) {
			lastW = f.width;
			lastH = f.height;
			frames++;
		},
		scrcpy::VideoCodecId::H264);

	CHECK(dec.feedConfig(a.config.data(), a.config.size()));
	uint64_t pts = 0;
	for (auto &au : a.aus)
		dec.decode(au.data(), au.size(), pts += 16667000, false);
	dec.flush();
	CHECK(frames.load() > 50);
	CHECK(lastW == 1080 && lastH == 2340);

	// rotate: new parameter sets + frames
	CHECK(dec.feedConfig(b.config.data(), b.config.size()));
	int before = frames.load();
	for (auto &au : b.aus)
		dec.decode(au.data(), au.size(), pts += 16667000, false);
	dec.flush();
	CHECK(frames.load() > before + 40);
	fprintf(stderr, "rotation: %d frames, final size %ux%u\n", frames.load(), lastW.load(), lastH.load());
	CHECK(lastW == 2340 && lastH == 1080);
}

int main(int argc, char **argv)
{
	testParseDevices();
	testSplitAnnexB();
	testPlaceholder();

	const char *h264 = argc > 1 ? argv[1] : "/tmp/obs-android-adb-test.h264";
	const char *h265 = argc > 2 ? argv[2] : "/tmp/obs-android-adb-test.h265";
	const char *rot = argc > 3 ? argv[3] : "/tmp/obs-android-adb-test-rot.h264";
	testDecoderStream(h264, scrcpy::VideoCodecId::H264, 1080, 2340);
	if (fileReadable(h265))
		testDecoderStream(h265, scrcpy::VideoCodecId::H265, 1080, 2340);
	else
		fprintf(stderr, "skipping HEVC test (no %s)\n", h265);

	if (fileReadable(rot))
		testRotation(h264, rot);
	else
		fprintf(stderr, "skipping rotation test (no %s)\n", rot);

	if (gFailures == 0) {
		fprintf(stderr, "ALL TESTS PASSED\n");
		return 0;
	}
	fprintf(stderr, "%d FAILURES\n", gFailures);
	return 1;
}
