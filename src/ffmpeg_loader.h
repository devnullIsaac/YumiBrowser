/*
 * ffmpeg_loader.h - Runtime dynamic loader for FFmpeg: replaces static linking by dlopen/LoadLibrary-resolving every needed symbol from libavformat/codec/util/swscale/swresample.
 * Copyright (C) 2026 DevNullIsaac
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * ffmpeg_loader.h
 *
 * Runtime dynamic loader for FFmpeg libraries.
 *
 * Replaces static linking of libavformat, libavcodec, libavutil,
 * libswscale and libswresample by loading them via dlopen / LoadLibrary
 * and resolving every symbol the YumiBrowser codebase needs.
 *
 * Usage:
 *   1. Call ffmpeg_loader_init() before any FFmpeg operation.
 *   2. Include this header instead of the usual <libav...> headers.
 *   3. Call ffmpeg_loader_deinit() on shutdown.
 */

#ifndef FFMPEG_LOADER_H
#define FFMPEG_LOADER_H

#include <stddef.h>
#include <stdint.h>

/* Pull in all FFmpeg type definitions / macros / enums.
 * We still need the headers for structs, enums and inline macros,
 * but we redirect every *function call* to our dynamically loaded
 * function pointers so no static library linkage is required.       */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Function pointer declarations — libavutil                         */
/* ================================================================== */

extern void  (*dyn_av_free)(void *ptr);
extern void  (*dyn_av_freep)(void *arg);
extern void *(*dyn_av_malloc)(size_t size);
extern int64_t (*dyn_av_rescale_q)(int64_t a, AVRational bq, AVRational cq);

extern AVBufferRef *(*dyn_av_buffer_ref)(AVBufferRef *buf);
extern void         (*dyn_av_buffer_unref)(AVBufferRef **buf);

extern AVFrame *(*dyn_av_frame_alloc)(void);
extern void     (*dyn_av_frame_free)(AVFrame **frame);
extern void     (*dyn_av_frame_unref)(AVFrame *frame);

extern const AVPixFmtDescriptor *(*dyn_av_pix_fmt_desc_get)(enum AVPixelFormat pix_fmt);

extern int (*dyn_av_hwdevice_ctx_create)(AVBufferRef **device_ctx,
                                          enum AVHWDeviceType type,
                                          const char *device,
                                          AVDictionary *opts,
                                          int flags);
extern AVBufferRef *(*dyn_av_hwdevice_ctx_alloc)(enum AVHWDeviceType type);
extern int          (*dyn_av_hwdevice_ctx_init)(AVBufferRef *ref);
extern int          (*dyn_av_hwframe_map)(AVFrame *dst,
                                           const AVFrame *src,
                                           int flags);
extern int          (*dyn_av_hwframe_transfer_data)(AVFrame *dst,
                                                     const AVFrame *src,
                                                     int flags);

/* ================================================================== */
/*  Function pointer declarations — libavformat                       */
/* ================================================================== */

extern AVFormatContext *(*dyn_avformat_alloc_context)(void);
extern int (*dyn_avformat_open_input)(AVFormatContext **ps,
                                       const char *url,
                                       AVInputFormat *fmt,
                                       AVDictionary **options);
extern int (*dyn_avformat_find_stream_info)(AVFormatContext *ic,
                                             AVDictionary **options);
extern void (*dyn_avformat_close_input)(AVFormatContext **s);

extern int (*dyn_av_find_best_stream)(AVFormatContext *ic,
                                       enum AVMediaType type,
                                       int wanted_stream_nb,
                                       int related_stream,
                                       AVCodec **decoder_ret,
                                       int flags);

extern int (*dyn_av_read_frame)(AVFormatContext *s, AVPacket *pkt);
extern int (*dyn_av_seek_frame)(AVFormatContext *s,
                                 int stream_index,
                                 int64_t timestamp,
                                 int flags);

extern int (*dyn_avio_feof)(AVIOContext *s);
extern AVIOContext *(*dyn_avio_alloc_context)(
    unsigned char *buffer,
    int buffer_size,
    int write_flag,
    void *opaque,
    int (*read_packet)(void *opaque, uint8_t *buf, int buf_size),
    int (*write_packet)(void *opaque, uint8_t *buf, int buf_size),
    int64_t (*seek)(void *opaque, int64_t offset, int whence));
extern void (*dyn_avio_context_free)(AVIOContext **s);

/* ================================================================== */
/*  Function pointer declarations — libavcodec                        */
/* ================================================================== */

extern AVCodec *(*dyn_avcodec_find_decoder)(enum AVCodecID id);
extern AVCodecContext *(*dyn_avcodec_alloc_context3)(const AVCodec *codec);
extern int (*dyn_avcodec_parameters_to_context)(AVCodecContext *codec,
                                                 const AVCodecParameters *par);
extern int (*dyn_avcodec_open2)(AVCodecContext *avctx,
                                 const AVCodec *codec,
                                 AVDictionary **options);
extern int (*dyn_avcodec_send_packet)(AVCodecContext *avctx,
                                       const AVPacket *avpkt);
extern int (*dyn_avcodec_receive_frame)(AVCodecContext *avctx,
                                         AVFrame *frame);
extern void (*dyn_avcodec_flush_buffers)(AVCodecContext *avctx);
extern void (*dyn_avcodec_free_context)(AVCodecContext **avctx);

extern AVPacket *(*dyn_av_packet_alloc)(void);
extern void      (*dyn_av_packet_free)(AVPacket **pkt);
extern void      (*dyn_av_packet_unref)(AVPacket *pkt);
extern void      (*dyn_av_packet_move_ref)(AVPacket *dst, AVPacket *src);

/* ================================================================== */
/*  Function pointer declarations — libswscale                        */
/* ================================================================== */

extern struct SwsContext *(*dyn_sws_getContext)(
    int srcW, int srcH, enum AVPixelFormat srcFormat,
    int dstW, int dstH, enum AVPixelFormat dstFormat,
    int flags, SwsFilter *srcFilter,
    SwsFilter *dstFilter, const double *param);

extern int (*dyn_sws_scale)(struct SwsContext *c,
                             const uint8_t *const srcSlice[],
                             const int srcStride[],
                             int srcSliceY, int srcSliceH,
                             uint8_t *const dst[],
                             const int dstStride[]);

extern void (*dyn_sws_freeContext)(struct SwsContext *swsContext);

/* ================================================================== */
/*  Function pointer declarations — libswresample                     */
/* ================================================================== */

extern int (*dyn_swr_alloc_set_opts2)(SwrContext **ps,
                                       const AVChannelLayout *out_ch_layout,
                                       enum AVSampleFormat out_sample_fmt,
                                       int out_sample_rate,
                                       const AVChannelLayout *in_ch_layout,
                                       enum AVSampleFormat in_sample_fmt,
                                       int in_sample_rate,
                                       int log_offset, void *log_ctx);

extern int  (*dyn_swr_init)(SwrContext *s);
extern void (*dyn_swr_free)(SwrContext **s);
extern int  (*dyn_swr_get_out_samples)(SwrContext *s, int in_samples);
extern int  (*dyn_swr_convert)(SwrContext *s,
                                uint8_t **out, int out_count,
                                const uint8_t **in, int in_count);

/* ================================================================== */
/*  Loader lifecycle                                                   */
/* ================================================================== */

int  ffmpeg_loader_init(void);
void ffmpeg_loader_deinit(void);

#ifdef __cplusplus
}
#endif

/* ================================================================== */
/*  Redirect every FFmpeg function call to the dynamic pointer.       */
/*  This MUST come after the declarations above so existing code can  */
/*  keep calling the normal names transparently.                      */
/* ================================================================== */

#define av_free                     dyn_av_free
#define av_freep                    dyn_av_freep
#define av_malloc                   dyn_av_malloc
#define av_rescale_q                dyn_av_rescale_q
#define av_buffer_ref               dyn_av_buffer_ref
#define av_buffer_unref             dyn_av_buffer_unref
#define av_frame_alloc              dyn_av_frame_alloc
#define av_frame_free               dyn_av_frame_free
#define av_frame_unref              dyn_av_frame_unref
#define av_pix_fmt_desc_get         dyn_av_pix_fmt_desc_get
#define av_hwdevice_ctx_create      dyn_av_hwdevice_ctx_create
#define av_hwdevice_ctx_alloc       dyn_av_hwdevice_ctx_alloc
#define av_hwdevice_ctx_init        dyn_av_hwdevice_ctx_init
#define av_hwframe_map              dyn_av_hwframe_map
#define av_hwframe_transfer_data    dyn_av_hwframe_transfer_data

#define avformat_alloc_context      dyn_avformat_alloc_context
#define avformat_open_input         dyn_avformat_open_input
#define avformat_find_stream_info   dyn_avformat_find_stream_info
#define avformat_close_input        dyn_avformat_close_input
#define av_find_best_stream         dyn_av_find_best_stream
#define av_read_frame               dyn_av_read_frame
#define av_seek_frame               dyn_av_seek_frame
#define avio_feof                   dyn_avio_feof
#define avio_alloc_context          dyn_avio_alloc_context
#define avio_context_free           dyn_avio_context_free

#define avcodec_find_decoder        dyn_avcodec_find_decoder
#define avcodec_alloc_context3      dyn_avcodec_alloc_context3
#define avcodec_parameters_to_context dyn_avcodec_parameters_to_context
#define avcodec_open2               dyn_avcodec_open2
#define avcodec_send_packet         dyn_avcodec_send_packet
#define avcodec_receive_frame       dyn_avcodec_receive_frame
#define avcodec_flush_buffers       dyn_avcodec_flush_buffers
#define avcodec_free_context        dyn_avcodec_free_context
#define av_packet_alloc             dyn_av_packet_alloc
#define av_packet_free              dyn_av_packet_free
#define av_packet_unref             dyn_av_packet_unref
#define av_packet_move_ref          dyn_av_packet_move_ref

#define sws_getContext              dyn_sws_getContext
#define sws_scale                   dyn_sws_scale
#define sws_freeContext             dyn_sws_freeContext

#define swr_alloc_set_opts2         dyn_swr_alloc_set_opts2
#define swr_init                    dyn_swr_init
#define swr_free                    dyn_swr_free
#define swr_get_out_samples         dyn_swr_get_out_samples
#define swr_convert                 dyn_swr_convert

#endif /* FFMPEG_LOADER_H */
