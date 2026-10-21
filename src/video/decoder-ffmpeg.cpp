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
#include <libavutil/pixdesc.h>
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
	std::vector<uint8_t> nv12;     // YUV420P -> NV12 conversion scratch
	bool open = false;
	bool loggedDecoder = false;

	Impl(OutputFn out, scrcpy::VideoCodecId c) : output(std::move(out)), codec(c)
	{
		av_log_set_level(AV_LOG_ERROR);
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
		if (!f->data[0])
			return;
		DecodedFrame out;
		out.width = (uint32_t)f->width;
		out.height = (uint32_t)f->height;
		if (f->format == AV_PIX_FMT_NV12 && f->data[1]) {
			out.y = f->data[0];
			out.uv = f->data[1];
			out.yStride = (uint32_t)f->linesize[0];
			out.uvStride = (uint32_t)f->linesize[1];
		} else if (f->format == AV_PIX_FMT_YUV420P && f->data[1] && f->data[2]) {
			// Interleave the planar U/V planes into NV12.
			size_t ySize = (size_t)f->width * f->height;
			nv12.resize(ySize + ySize / 2);
			for (int r = 0; r < f->height; r++)
				memcpy(nv12.data() + (size_t)r * f->width, f->data[0] + (size_t)r * f->linesize[0], f->width);
			uint8_t *uv = nv12.data() + ySize;
			for (int r = 0; r < (f->height + 1) / 2; r++) {
				const uint8_t *u = f->data[1] + (size_t)r * f->linesize[1];
				const uint8_t *v = f->data[2] + (size_t)r * f->linesize[2];
				for (int c = 0; c < (f->width + 1) / 2; c++) {
					uv[2 * c] = u[c];
					uv[2 * c + 1] = v[c];
				}
				uv += f->width;
			}
			out.y = nv12.data();
			out.uv = nv12.data() + ySize;
			out.yStride = (uint32_t)f->width;
			out.uvStride = (uint32_t)f->width;
		} else {
			return; // exotic pixel format — drop
		}
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
	if (!impl_ || !impl_->open)
		return;
	// Drain everything the decoder still holds (send EOF, then collect), and
	// reset for the next segment afterwards.
	avcodec_send_packet(impl_->ctx, nullptr);
	while (true) {
		int ret = avcodec_receive_frame(impl_->ctx, impl_->frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0)
			break;
		impl_->deliver(impl_->frame);
		av_frame_unref(impl_->frame);
	}
	avcodec_flush_buffers(impl_->ctx);
}

} // namespace video
