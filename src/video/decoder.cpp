#include "decoder.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CMSampleBuffer.h>
#include <CoreVideo/CVPixelBuffer.h>

#include <cmath>
#include <cstring>

#include <util/base.h>

namespace video {

// NAL unit type extraction (first byte of the NAL, after the start code)
static uint8_t h264NalType(const uint8_t *nal)
{
	return nal[0] & 0x1f;
}
static uint8_t hevcNalType(const uint8_t *nal)
{
	return (nal[0] >> 1) & 0x3f;
}

// ---------------------------------------------------------------------------
// VT output callback

namespace {

void vtOutputCallback(void *decompressionOutputRefCon, void *sourceFrameRefCon, OSStatus status,
	VTDecodeInfoFlags infoFlags, CVImageBufferRef imageBuffer, CMTime presentationTimeStamp,
	CMTime presentationDuration)
{
	(void)sourceFrameRefCon;
	(void)infoFlags;
	(void)presentationDuration;

	auto *ctx = static_cast<Decoder::OutputContext *>(decompressionOutputRefCon);
	if (status != noErr || !imageBuffer) {
		blog(LOG_DEBUG, "[obs-android-adb] VT decode status %d", (int)status);
		return;
	}

	uint64_t ptsNs = (uint64_t)llround(CMTimeGetSeconds(presentationTimeStamp) * 1e9);
	// The buffer is only valid for the duration of the callback; the consumer
	// copies it synchronously.
	if (ctx && ctx->output)
		ctx->output(static_cast<CVPixelBufferRef>(imageBuffer), ptsNs, false);
}

} // namespace

// ---------------------------------------------------------------------------
// Decoder

Decoder::Decoder(OutputFn output, scrcpy::VideoCodecId codec)
	: output_(std::move(output)),
	  codec_(codec)
{
	outputCtx_.output = output_;
}

Decoder::~Decoder()
{
	destroySession();
	if (formatDesc_) {
		CFRelease(formatDesc_);
		formatDesc_ = nullptr;
	}
}

void Decoder::destroySession()
{
	// Only tears down the VT session; the format description is owned by
	// buildFormatDescription() (and released there or in the destructor).
	if (session_) {
		VTDecompressionSessionInvalidate(session_);
		CFRelease(session_);
		session_ = nullptr;
	}
}

bool Decoder::buildFormatDescription()
{
	if (formatDesc_) {
		CFRelease(formatDesc_);
		formatDesc_ = nullptr;
	}

	OSStatus err = noErr;
	if (codec_ == scrcpy::VideoCodecId::H264) {
		if (!haveSps_ || !havePps_)
			return false;
		const uint8_t *paramSets[2] = {sps_.data(), pps_.data()};
		size_t paramSetSizes[2] = {sps_.size(), pps_.size()};
		err = CMVideoFormatDescriptionCreateFromH264ParameterSets(
			kCFAllocatorDefault, 2, paramSets, paramSetSizes, 4 /* nalLengthHeaderSize */, &formatDesc_);
	} else if (codec_ == scrcpy::VideoCodecId::H265) {
		if (!haveVps_ || !haveSps_ || !havePps_)
			return false;
		const uint8_t *paramSets[3] = {vps_.data(), sps_.data(), pps_.data()};
		size_t paramSetSizes[3] = {vps_.size(), sps_.size(), pps_.size()};
		err = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
			kCFAllocatorDefault, 3, paramSets, paramSetSizes, 4, nullptr, &formatDesc_);
	} else {
		blog(LOG_ERROR, "[obs-android-adb] codec not supported by the VideoToolbox decoder yet");
		return false;
	}

	if (err != noErr || !formatDesc_) {
		blog(LOG_ERROR, "[obs-android-adb] could not create CMVideoFormatDescription (%d)", (int)err);
		formatDesc_ = nullptr;
		return false;
	}
	return true;
}

bool Decoder::createSession()
{
	destroySession();
	if (!formatDesc_)
		return false;

	// Hardware-accelerated decoder preferred (always available on Apple Silicon)
	CFMutableDictionaryRef decoderSpec = CFDictionaryCreateMutable(
		kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(decoderSpec, kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
		kCFBooleanTrue);

	CFMutableDictionaryRef destAttrs = CFDictionaryCreateMutable(
		kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	int32_t pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange; // NV12
	CFNumberRef pixelFormatNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixelFormat);
	CFDictionarySetValue(destAttrs, kCVPixelBufferPixelFormatTypeKey, pixelFormatNum);
	CFRelease(pixelFormatNum);
	CFMutableDictionaryRef iosurfaceProps = CFDictionaryCreateMutable(
		kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(destAttrs, kCVPixelBufferIOSurfacePropertiesKey, iosurfaceProps);
	CFRelease(iosurfaceProps);

	VTDecompressionOutputCallbackRecord callback{};
	callback.decompressionOutputCallback = vtOutputCallback;
	callback.decompressionOutputRefCon = &outputCtx_;

	OSStatus err = VTDecompressionSessionCreate(
		kCFAllocatorDefault, formatDesc_, decoderSpec, destAttrs, &callback, &session_);
	CFRelease(decoderSpec);
	CFRelease(destAttrs);

	if (err != noErr) {
		blog(LOG_ERROR, "[obs-android-adb] VTDecompressionSessionCreate failed: %d", (int)err);
		session_ = nullptr;
		return false;
	}

	VTSessionSetProperty(session_, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);

	blog(LOG_INFO, "[obs-android-adb] decoder: VideoToolbox %s (hardware)",
		scrcpy::videoCodecName(codec_));
	return true;
}

bool Decoder::feedConfig(const uint8_t *data, size_t len)
{
	auto nals = scrcpy::splitAnnexB(data, len);

	uint32_t newCodecSetHash = 0;
	bool sawNewSets = false;

	for (auto &nal : nals) {
		if (nal.size < 2)
			continue;
		if (codec_ == scrcpy::VideoCodecId::H264) {
			uint8_t type = h264NalType(nal.data);
			if (type == 7 && sps_ != std::vector<uint8_t>(nal.data, nal.data + nal.size)) {
				sps_.assign(nal.data, nal.data + nal.size);
				haveSps_ = true;
				sawNewSets = true;
			} else if (type == 8 && pps_ != std::vector<uint8_t>(nal.data, nal.data + nal.size)) {
				pps_.assign(nal.data, nal.data + nal.size);
				havePps_ = true;
				sawNewSets = true;
			}
		} else if (codec_ == scrcpy::VideoCodecId::H265) {
			uint8_t type = hevcNalType(nal.data);
			auto set = [&](std::vector<uint8_t> &dst, bool &have) {
				if (dst != std::vector<uint8_t>(nal.data, nal.data + nal.size)) {
					dst.assign(nal.data, nal.data + nal.size);
					have = true;
					sawNewSets = true;
				}
			};
			if (type == 32)
				set(vps_, haveVps_);
			else if (type == 33)
				set(sps_, haveSps_);
			else if (type == 34)
				set(pps_, havePps_);
		}
	}
	(void)newCodecSetHash;

	if (!sawNewSets && session_)
		return true; // same parameter sets as before

	if (!buildFormatDescription())
		return false;

	if (session_ && VTDecompressionSessionCanAcceptFormatDescription(session_, formatDesc_)) {
		blog(LOG_INFO, "[obs-android-adb] decoder accepted new format description");
		return true;
	}
	return createSession();
}

bool Decoder::decode(const uint8_t *data, size_t len, uint64_t ptsUs, bool keyFrame)
{
	(void)keyFrame;
	if (!session_)
		return false;

	// Annex B -> AVCC (4-byte big-endian length prefixes)
	auto nals = scrcpy::splitAnnexB(data, len);
	size_t total = 0;
	for (auto &nal : nals)
		total += 4 + nal.size;
	if (total == 0)
		return true; // nothing to decode (e.g. AUD-only packet)

	if (avccBuffer_.size() < total)
		avccBuffer_.resize(total);

	uint8_t *out = avccBuffer_.data();
	for (auto &nal : nals) {
		uint32_t sz = (uint32_t)nal.size;
		out[0] = (uint8_t)(sz >> 24);
		out[1] = (uint8_t)(sz >> 16);
		out[2] = (uint8_t)(sz >> 8);
		out[3] = (uint8_t)sz;
		memcpy(out + 4, nal.data, sz);
		out += 4 + sz;
	}

	// Let CoreMedia own the memory (passing our own pointer with an allocator
	// would make it free() our std::vector buffer on release).
	CMBlockBufferRef block = nullptr;
	OSStatus err = CMBlockBufferCreateWithMemoryBlock(
		kCFAllocatorDefault, nullptr, total, nullptr, nullptr, 0, total, kCMBlockBufferAssureMemoryNowFlag, &block);
	if (err != noErr || !block) {
		blog(LOG_ERROR, "[obs-android-adb] CMBlockBufferCreate failed: %d", (int)err);
		return false;
	}
	err = CMBlockBufferReplaceDataBytes(avccBuffer_.data(), block, 0, total);
	if (err != noErr) {
		CFRelease(block);
		blog(LOG_ERROR, "[obs-android-adb] CMBlockBufferReplaceDataBytes failed: %d", (int)err);
		return false;
	}

	CMSampleTimingInfo timing{};
	timing.presentationTimeStamp = CMTimeMake((int64_t)ptsUs, 1000000);

	CMSampleBufferRef sample = nullptr;
	err = CMSampleBufferCreate(kCFAllocatorDefault, block, true, nullptr, nullptr, formatDesc_, 1, 1, &timing,
		1, &total, &sample);
	if (err != noErr || !sample) {
		CFRelease(block);
		blog(LOG_ERROR, "[obs-android-adb] CMSampleBufferCreate failed: %d", (int)err);
		return false;
	}

	err = VTDecompressionSessionDecodeFrame(session_, sample, 0, nullptr, nullptr);
	CFRelease(sample);
	CFRelease(block);

	if (err == kVTVideoDecoderMalfunctionErr) {
		blog(LOG_WARNING, "[obs-android-adb] decoder malfunction, resetting session");
		destroySession();
		createSession();
		return false;
	}
	if (err != noErr) {
		blog(LOG_WARNING, "[obs-android-adb] VTDecompressionSessionDecodeFrame: %d", (int)err);
		return false;
	}
	return true;
}

void Decoder::flush()
{
	if (session_)
		VTDecompressionSessionWaitForAsynchronousFrames(session_);
}

} // namespace video
