#pragma once

// Windows-only: the av*/avcodec* symbols are resolved at runtime (see
// ffmpeg-loader.cpp). Include this AFTER the libav* headers; the macros
// below keep ordinary call sites unchanged.

namespace ff {

extern AVPacket *(*av_packet_alloc_p)(void);
extern void (*av_packet_free_p)(AVPacket **);
extern AVFrame *(*av_frame_alloc_p)(void);
extern void (*av_frame_free_p)(AVFrame **);
extern void (*av_frame_unref_p)(AVFrame *);
extern void (*av_log_set_level_p)(int);
extern int (*av_strerror_p)(int, char *, size_t);
extern const AVPixFmtDescriptor *(*av_pix_fmt_desc_get_p)(enum AVPixelFormat);
extern void *(*av_memdup_p)(const void *, size_t);
extern const AVCodec *(*avcodec_find_decoder_p)(enum AVCodecID);
extern AVCodecContext *(*avcodec_alloc_context3_p)(const AVCodec *);
extern int (*avcodec_open2_p)(AVCodecContext *, const AVCodec *, AVDictionary **);
extern void (*avcodec_free_context_p)(AVCodecContext **);
extern int (*avcodec_send_packet_p)(AVCodecContext *, const AVPacket *);
extern int (*avcodec_receive_frame_p)(AVCodecContext *, AVFrame *);
extern void (*avcodec_flush_buffers_p)(AVCodecContext *);

bool load(std::string *err);

} // namespace ff

#define av_packet_alloc (ff::av_packet_alloc_p)
#define av_packet_free (ff::av_packet_free_p)
#define av_frame_alloc (ff::av_frame_alloc_p)
#define av_frame_free (ff::av_frame_free_p)
#define av_frame_unref (ff::av_frame_unref_p)
#define av_log_set_level (ff::av_log_set_level_p)
#define av_strerror (ff::av_strerror_p)
#define av_pix_fmt_desc_get (ff::av_pix_fmt_desc_get_p)
#define av_memdup (ff::av_memdup_p)
#define avcodec_find_decoder (ff::avcodec_find_decoder_p)
#define avcodec_alloc_context3 (ff::avcodec_alloc_context3_p)
#define avcodec_open2 (ff::avcodec_open2_p)
#define avcodec_free_context (ff::avcodec_free_context_p)
#define avcodec_send_packet (ff::avcodec_send_packet_p)
#define avcodec_receive_frame (ff::avcodec_receive_frame_p)
#define avcodec_flush_buffers (ff::avcodec_flush_buffers_p)
