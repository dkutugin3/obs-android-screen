// VideoToolbox decoder backend (macOS). The plugin-facing interface lives in
// decoder.hpp; this file compiles only on Apple platforms.

#include "decoder.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CMSampleBuffer.h>
#include <CoreVideo/CVPixelBuffer.h>
#include <VideoToolbox/VTDecompressionSession.h>

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

struct Decoder::Impl {
	OutputFn output;
	scrcpy::VideoCodecId codec;

	std::vector<uint8_t> sps, pps, vps;
	bool haveSps = false, havePps = false, haveVps = false;

	CMFormatDescriptionRef formatDesc = nullptr;
	VTDecompressionSessionRef session = nullptr;
	std::vector<uint8_t> avccBuffer; // Annex B -> length-prefixed scratch

	Impl(OutputFn out, scrcpy::VideoCodecId c) : output(std::move(out)), codec(c) {}

	void destroySession()
	{
		// Only tears down the VT session; the format description is owned
		// here and released separately.
		if (session) {
			VTDecompressionSessionInvalidate(session);
			CFRelease(session);
			session = nullptr;
		}
	}

	bool buildFormatDescription()
	{
		if (formatDesc) {
			CFRelease(formatDesc);
			formatDesc = nullptr;
		}

		OSStatus err = noErr;
		if (codec == scrcpy::VideoCodecId::H264) {
			if (!haveSps || !havePps)
				return false;
			const uint8_t *paramSets[2] = {sps.data(), pps.data()};
			size_t paramSetSizes[2] = {sps.size(), pps.size()};
			err = CMVideoFormatDescriptionCreateFromH264ParameterSets(
				kCFAllocatorDefault, 2, paramSets, paramSetSizes, 4, &formatDesc);
		} else if (codec == scrcpy::VideoCodecId::H265) {
			if (!haveVps || !haveSps || !havePps)
				return false;
			const uint8_t *paramSets[3] = {vps.data(), sps.data(), pps.data()};
			size_t paramSetSizes[3] = {vps.size(), sps.size(), pps.size()};
			err = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
				kCFAllocatorDefault, 3, paramSets, paramSetSizes, 4, nullptr, &formatDesc);
		} else {
			blog(LOG_ERROR, "[obs-android-adb] codec not supported by the VideoToolbox decoder");
			return false;
		}

		if (err != noErr || !formatDesc) {
			blog(LOG_ERROR, "[obs-android-adb] could not create CMVideoFormatDescription (%d)", (int)err);
			formatDesc = nullptr;
			return false;
		}
		return true;
	}

	bool createSession()
	{
		destroySession();
		if (!formatDesc)
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
		callback.decompressionOutputRefCon = this;

		OSStatus err = VTDecompressionSessionCreate(
			kCFAllocatorDefault, formatDesc, decoderSpec, destAttrs, &callback, &session);
		CFRelease(decoderSpec);
		CFRelease(destAttrs);

		if (err != noErr) {
			blog(LOG_ERROR, "[obs-android-adb] VTDecompressionSessionCreate failed: %d", (int)err);
			session = nullptr;
			return false;
		}

		VTSessionSetProperty(session, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);

		blog(LOG_INFO, "[obs-android-adb] decoder: VideoToolbox %s (hardware)", scrcpy::videoCodecName(codec));
		return true;
	}

	static void vtOutputCallback(void *refCon, void *sourceFrameRefCon, OSStatus status, VTDecodeInfoFlags infoFlags,
		CVImageBufferRef imageBuffer, CMTime presentationTimeStamp, CMTime presentationDuration)
	{
		(void)sourceFrameRefCon;
		(void)infoFlags;
		(void)presentationDuration;

		auto *impl = static_cast<Impl *>(refCon);
		if (status != noErr || !imageBuffer) {
			blog(LOG_DEBUG, "[obs-android-adb] VT decode status %d", (int)status);
			return;
		}

		uint64_t ptsNs = (uint64_t)llround(CMTimeGetSeconds(presentationTimeStamp) * 1e9);

		// Wrap the pixel buffer into plane pointers; the consumer copies
		// synchronously before the callback returns.
		CVPixelBufferRef pb = static_cast<CVPixelBufferRef>(imageBuffer);
		CVPixelBufferLockBaseAddress(pb, 0);
		DecodedFrame f;
		f.y = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
		f.uv = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 1);
		f.yStride = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
		f.uvStride = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
		f.width = (uint32_t)CVPixelBufferGetWidth(pb);
		f.height = (uint32_t)CVPixelBufferGetHeight(pb);
		if (f.y && f.uv && impl->output)
			impl->output(f, ptsNs, false);
		CVPixelBufferUnlockBaseAddress(pb, 0);
	}
};

Decoder::Decoder(OutputFn output, scrcpy::VideoCodecId codec) : impl_(new Impl(std::move(output), codec)) {}

Decoder::~Decoder()
{
	delete impl_;
}

bool Decoder::ready() const
{
	return impl_ && impl_->session != nullptr;
}

bool Decoder::feedConfig(const uint8_t *data, size_t len)
{
	if (!impl_)
		return false;

	auto nals = scrcpy::splitAnnexB(data, len);
	bool sawNewSets = false;

	for (auto &nal : nals) {
		if (nal.size < 2)
			continue;
		if (impl_->codec == scrcpy::VideoCodecId::H264) {
			uint8_t type = h264NalType(nal.data);
			if (type == 7 && impl_->sps != std::vector<uint8_t>(nal.data, nal.data + nal.size)) {
				impl_->sps.assign(nal.data, nal.data + nal.size);
				impl_->haveSps = true;
				sawNewSets = true;
			} else if (type == 8 && impl_->pps != std::vector<uint8_t>(nal.data, nal.data + nal.size)) {
				impl_->pps.assign(nal.data, nal.data + nal.size);
				impl_->havePps = true;
				sawNewSets = true;
			}
		} else if (impl_->codec == scrcpy::VideoCodecId::H265) {
			uint8_t type = hevcNalType(nal.data);
			auto set = [&](std::vector<uint8_t> &dst, bool &have) {
				if (dst != std::vector<uint8_t>(nal.data, nal.data + nal.size)) {
					dst.assign(nal.data, nal.data + nal.size);
					have = true;
					sawNewSets = true;
				}
			};
			if (type == 32)
				set(impl_->vps, impl_->haveVps);
			else if (type == 33)
				set(impl_->sps, impl_->haveSps);
			else if (type == 34)
				set(impl_->pps, impl_->havePps);
		}
	}

	if (!sawNewSets && impl_->session)
		return true; // same parameter sets as before

	if (!impl_->buildFormatDescription())
		return false;

	if (impl_->session && VTDecompressionSessionCanAcceptFormatDescription(impl_->session, impl_->formatDesc)) {
		blog(LOG_INFO, "[obs-android-adb] decoder accepted new format description");
		return true;
	}
	return impl_->createSession();
}

bool Decoder::decode(const uint8_t *data, size_t len, uint64_t ptsNs, bool keyFrame)
{
	(void)keyFrame;
	if (!impl_ || !impl_->session)
		return false;

	// Annex B -> AVCC (4-byte big-endian length prefixes)
	auto nals = scrcpy::splitAnnexB(data, len);
	size_t total = 0;
	for (auto &nal : nals)
		total += 4 + nal.size;
	if (total == 0)
		return true; // nothing to decode (e.g. AUD-only packet)

	std::vector<uint8_t> &avcc = impl_->avccBuffer;
	if (avcc.size() < total)
		avcc.resize(total);

	uint8_t *out = avcc.data();
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
	err = CMBlockBufferReplaceDataBytes(avcc.data(), block, 0, total);
	if (err != noErr) {
		CFRelease(block);
		blog(LOG_ERROR, "[obs-android-adb] CMBlockBufferReplaceDataBytes failed: %d", (int)err);
		return false;
	}

	CMSampleTimingInfo timing{};
	timing.presentationTimeStamp = CMTimeMake((int64_t)ptsNs, 1000000000);

	CMSampleBufferRef sample = nullptr;
	err = CMSampleBufferCreate(kCFAllocatorDefault, block, true, nullptr, nullptr, impl_->formatDesc, 1, 1,
		&timing, 1, &total, &sample);
	if (err != noErr || !sample) {
		CFRelease(block);
		blog(LOG_ERROR, "[obs-android-adb] CMSampleBufferCreate failed: %d", (int)err);
		return false;
	}

	err = VTDecompressionSessionDecodeFrame(impl_->session, sample, 0, nullptr, nullptr);
	CFRelease(sample);
	CFRelease(block);

	if (err == kVTVideoDecoderMalfunctionErr) {
		blog(LOG_WARNING, "[obs-android-adb] decoder malfunction, resetting session");
		impl_->destroySession();
		impl_->createSession();
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
	if (impl_ && impl_->session)
		VTDecompressionSessionWaitForAsynchronousFrames(impl_->session);
}

} // namespace video
