// libavcodec decoder backend (Windows). The plugin-facing interface lives in
// decoder.hpp. Software decode (the stock H.264 decoder ships with every
// Windows; HEVC works everywhere via libavcodec regardless of the OS "HEVC
// Video Extensions").

#include "decoder.hpp"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
}

#include <util/base.h>

namespace video {

static enum AVPixelFormat pickPixelFormat(AVCodecContext *, const enum AVPixelFormat *fmts)
{
	// NV12 keeps the frame path identical to the VideoToolbox backend.
	for (const enum AVPixelFormat *f = fmts; *f != AV_PIX_FMT_NONE; f++)
		if (*f == AV_PIX_FMT_NV12)
			return AV_PIX_FMT_NV12;
	return fmts[0];
}

struct Decoder::Impl {
	OutputFn output;
	scrcpy::VideoCodecId codec;

	const AVCodec *decoder = nullptr;
	AVCodecContext *ctx = nullptr;
	AVPacket *pkt = nullptr;
	AVFrame *frame = nullptr;
	std::vector<uint8_t> extradata; // Annex-B parameter sets
	bool open = false;
	bool loggedDecoder = false;

	Impl(OutputFn out, scrcpy::VideoCodecId c) : output(std::move(out)), codec(c)
	{
		AVCodecID id = c == scrcpy::VideoCodecId::H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
		decoder = avcodec_find_decoder(id);
		if (!decoder) {
			blog(LOG_ERROR, "[obs-android-adb] libavcodec has no %s decoder", scrcpy::videoCodecName(c));
			return;
		}
		pkt = av_packet_alloc();
		frame = av_frame_alloc();
	}

	~Impl()
	{
		if (ctx)
			avcodec_free_context(&ctx);
		if (pkt)
			av_packet_free(&pkt);
		if (frame)
			av_frame_free(&frame);
	}

	bool openCodec()
	{
		if (open || !decoder)
			return open;
		if (ctx)
			avcodec_free_context(&ctx);

		ctx = avcodec_alloc_context3(decoder);
		if (!ctx)
			return false;

		ctx->get_format = pickPixelFormat;
		// Low-latency screen content: slice threading only (frame threading
		// adds delay), let libavcodec pick the thread count.
		ctx->thread_count = 0;
		ctx->thread_type = FF_THREAD_SLICE;
		ctx->flags2 |= AV_CODEC_FLAG2_FAST;
		if (!extradata.empty()) {
			ctx->extradata = (uint8_t *)av_memdup(extradata.data(), extradata.size());
			ctx->extradata_size = (int)extradata.size();
		}

		if (avcodec_open2(ctx, decoder, nullptr) < 0) {
			blog(LOG_ERROR, "[obs-android-adb] avcodec_open2(%s) failed", scrcpy::videoCodecName(codec));
			avcodec_free_context(&ctx);
			return false;
		}
		open = true;
		if (!loggedDecoder) {
			blog(LOG_INFO, "[obs-android-adb] decoder: libavcodec %s (%s, %d thread(s))", decoder->name,
				decoder->capabilities & AV_CODEC_CAP_HARDWARE ? "hardware" : "software", ctx->thread_count);
			loggedDecoder = true;
		}
		return true;
	}

	void deliver(AVFrame *f)
	{
		if (f->format != AV_PIX_FMT_NV12 || !f->data[0] || !f->data[1])
			return;
		DecodedFrame out;
		out.y = f->data[0];
		out.uv = f->data[1];
		out.yStride = (uint32_t)f->linesize[0];
		out.uvStride = (uint32_t)f->linesize[1];
		out.width = (uint32_t)f->width;
		out.height = (uint32_t)f->height;
		if (output)
			output(out, (uint64_t)f->pts, f->flags & AV_FRAME_FLAG_KEY);
	}
};

Decoder::Decoder(OutputFn output, scrcpy::VideoCodecId codec) : impl_(new Impl(std::move(output), codec)) {}

Decoder::~Decoder()
{
	delete impl_;
}

bool Decoder::ready() const
{
	return impl_ && impl_->open;
}

bool Decoder::feedConfig(const uint8_t *data, size_t len)
{
	if (!impl_ || !impl_->decoder)
		return false;

	if (impl_->open) {
		// Parameter sets changed mid-stream (e.g. after rotation): reopen
		// the context with the new extradata.
		blog(LOG_INFO, "[obs-android-adb] new %s parameter sets, reopening decoder",
			scrcpy::videoCodecName(impl_->codec));
		impl_->open = false;
	}

	impl_->extradata.assign(data, data + len);
	return impl_->openCodec();
}

bool Decoder::decode(const uint8_t *data, size_t len, uint64_t ptsNs, bool keyFrame)
{
	(void)keyFrame;
	if (!impl_ || !impl_->decoder)
		return false;
	if (!impl_->open && !impl_->openCodec())
		return false;

	impl_->pkt->data = const_cast<uint8_t *>(data);
	impl_->pkt->size = (int)len;
	impl_->pkt->pts = (int64_t)ptsNs;
	impl_->pkt->dts = (int64_t)ptsNs;

	int ret = avcodec_send_packet(impl_->ctx, impl_->pkt);
	impl_->pkt->data = nullptr;
	impl_->pkt->size = 0;
	if (ret < 0 && ret != AVERROR(EAGAIN)) {
		char err[AV_ERROR_MAX_STRING_SIZE] = {0};
		av_strerror(ret, err, sizeof(err));
		blog(LOG_WARNING, "[obs-android-adb] avcodec_send_packet: %s", err);
		return false;
	}

	while (true) {
		ret = avcodec_receive_frame(impl_->ctx, impl_->frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0) {
			char err[AV_ERROR_MAX_STRING_SIZE] = {0};
			av_strerror(ret, err, sizeof(err));
			blog(LOG_WARNING, "[obs-android-adb] avcodec_receive_frame: %s", err);
			break;
		}
		impl_->deliver(impl_->frame);
		av_frame_unref(impl_->frame);
	}
	return true;
}

void Decoder::flush()
{
	if (impl_ && impl_->open)
		avcodec_flush_buffers(impl_->ctx);
}

} // namespace video
