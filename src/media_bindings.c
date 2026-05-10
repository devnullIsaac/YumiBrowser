/*
    Unified WASM-Callable Image / Video / Audio Decoder Bindings
    Copyright (C) 2026  DevNullIsaac

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/*
 * media_bindings.c
 *
 * Unified WASM-callable image / video / audio decoder bindings.
 *
 * Backend stack:
 *   - FFmpeg (libavformat / libavcodec / libswscale / libswresample) for
 *     all common containers and codecs.
 *   - Optional LibRaw for camera RAW images.
 *   - SDL3 audio for playback + post-mix spectrum.
 *   - WGPU (BGRA) textures for image / video frames.
 *
 * Single buffer-only entry point: media_open(buf, len). Detects whether
 * the buffer is an image, video or audio asset by magic bytes and routes
 * to the appropriate decode path.
 */

#include "media_bindings.h"
#include "ffmpeg_loader.h"

#include <SDL3/SDL_audio.h>
#ifdef YUMI_HAS_LIBRAW
#include <libraw/libraw.h>
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/*  Platform HW pixel format                                           */
/* ================================================================== */

#if defined(__linux__) && !defined(__APPLE__)
#  define MB_HW_PIX_FMT  AV_PIX_FMT_VAAPI
#elif defined(__APPLE__)
#  define MB_HW_PIX_FMT  AV_PIX_FMT_VIDEOTOOLBOX
#elif defined(_WIN32)
#  define MB_HW_PIX_FMT  AV_PIX_FMT_D3D11
#else
#  define MB_HW_PIX_FMT  AV_PIX_FMT_NONE
#endif

#define MB_SPECTRUM_BANDS 12

/* ================================================================== */
/*  Image-frame record (eager decode path)                             */
/* ================================================================== */

typedef struct {
    WGPUTexture     texture;
    WGPUTextureView view;
    uint32_t        view_handle;   /* in wgpu->ht_texture_view */
    float           duration_ms;   /* per-frame display duration (animated) */
} MediaFrame;

/* ================================================================== */
/*  MediaContext — one per open buffer                                 */
/* ================================================================== */

typedef struct MediaContext {
    MediaKind kind;

    /* Common cached info */
    int       width;
    int       height;
    int       has_alpha;
    int64_t   duration_ms;

    /* ---------- IMAGE path: eagerly decoded frames ---------- */
    MediaFrame *frames;
    int         frame_count;
    int         frame_cap;

    /* ---------- VIDEO / AUDIO streaming path ---------- */
    AVFormatContext *fmt_ctx;
    AVIOContext     *avio_ctx;
    uint8_t         *avio_buf;   /* AVIO's internal scratch */
    uint8_t         *src_data;   /* copied source bytes */
    int64_t          src_size;
    int64_t          src_pos;

    /* Video stream */
    int              video_stream_idx;
    AVCodecContext  *codec_ctx;
    AVPacket        *pkt;
    AVFrame         *frame;
    AVRational       time_base;
    int64_t          position_pts;
    bool             is_hw;
    struct SwsContext *sws_ctx;
    uint8_t         *rgba_buf;
    int              rgba_linesize;
    WGPUTexture      sw_texture;
    WGPUTextureView  sw_view;
    uint32_t         tex_handle;     /* current frame in wgpu->ht_texture */
    uint32_t         view_handle;    /* current frame in wgpu->ht_texture_view */
    WFFPixelLayout   pixel_layout;
    uint32_t         plane_count;
    WGPUTextureFormat tex_format;

    /* Audio stream */
    int              audio_stream_idx;
    AVCodecContext  *audio_codec_ctx;
    SwrContext      *swr_ctx;
    SDL_AudioStream *sdl_stream;
    float            audio_volume;
    bool             audio_muted;
    int64_t          audio_clock_pts;

    /* Per-context spectrum (legacy; postmix is preferred) */
    float            spectrum[MB_SPECTRUM_BANDS];
    float            spectrum_decay[MB_SPECTRUM_BANDS];
} MediaContext;

/* ================================================================== */
/*  WASM callback macros                                               */
/* ================================================================== */

#define MB         ((MediaBindings *)env)
#define ARG_I32(n) (args->data[(n)].of.i32)
#define ARG_F32(n) (args->data[(n)].of.f32)
#define RET_I32(v) do { res->data[0] = (wasm_val_t){.kind=WASM_I32,.of.i32=(v)}; } while(0)
#define RET_F32(v) do { res->data[0] = (wasm_val_t){.kind=WASM_F32,.of.f32=(v)}; } while(0)

static inline WGPUDevice mb_dev(MediaBindings *mb) { return mb->wgpu->gpu->device; }
static inline WGPUQueue  mb_que(MediaBindings *mb) { return mb->wgpu->gpu->queue; }

static uint8_t *mb_mem_base(MediaBindings *mb) {
    return mb->memory ? (uint8_t *)wasm_memory_data(mb->memory) : NULL;
}
static size_t mb_mem_size(MediaBindings *mb) {
    return mb->memory ? wasm_memory_data_size(mb->memory) : 0;
}

/* ================================================================== */
/*  Magic-byte classification                                          */
/* ================================================================== */

typedef enum {
    HINT_UNKNOWN = 0,
    HINT_IMAGE,        /* still / animated still — try eager image decode */
    HINT_CONTAINER,    /* video or muxed — open as stream */
    HINT_AUDIO,        /* audio-only — open as stream */
} MagicHint;

static bool is_ftyp_brand(const uint8_t *p, const char *brand)
{
    return p[0] == (uint8_t)brand[0] && p[1] == (uint8_t)brand[1] &&
           p[2] == (uint8_t)brand[2] && p[3] == (uint8_t)brand[3];
}

static MagicHint classify_magic(const uint8_t *b, size_t n)
{
    if (n < 4) return HINT_UNKNOWN;

    /* PNG */
    if (n >= 8 && b[0]==0x89 && b[1]==0x50 && b[2]==0x4E && b[3]==0x47 &&
        b[4]==0x0D && b[5]==0x0A && b[6]==0x1A && b[7]==0x0A)
        return HINT_IMAGE;

    /* JPEG */
    if (n >= 3 && b[0]==0xFF && b[1]==0xD8 && b[2]==0xFF) return HINT_IMAGE;

    /* GIF */
    if (n >= 6 && b[0]=='G' && b[1]=='I' && b[2]=='F' && b[3]=='8' &&
        (b[4]=='7' || b[4]=='9') && b[5]=='a') return HINT_IMAGE;

    /* BMP */
    if (b[0]=='B' && b[1]=='M') return HINT_IMAGE;

    /* TIFF */
    if ((b[0]==0x49 && b[1]==0x49 && b[2]==0x2A && b[3]==0x00) ||
        (b[0]==0x4D && b[1]==0x4D && b[2]==0x00 && b[3]==0x2A))
        return HINT_IMAGE;

    /* PSD */
    if (b[0]=='8' && b[1]=='B' && b[2]=='P' && b[3]=='S') return HINT_IMAGE;

    /* OpenEXR */
    if (b[0]==0x76 && b[1]==0x2F && b[2]==0x31 && b[3]==0x01) return HINT_IMAGE;

    /* DPX */
    if ((b[0]=='S' && b[1]=='D' && b[2]=='P' && b[3]=='X') ||
        (b[0]=='X' && b[1]=='P' && b[2]=='D' && b[3]=='S'))
        return HINT_IMAGE;

    /* ICO */
    if (n >= 4 && b[0]==0x00 && b[1]==0x00 && b[2]==0x01 && b[3]==0x00)
        return HINT_IMAGE;

    /* JPEG-XL: naked codestream FF 0A, or ISOBMFF box 00 00 00 0C 'JXL ' */
    if (b[0]==0xFF && b[1]==0x0A) return HINT_IMAGE;
    if (n >= 12 && b[0]==0x00 && b[1]==0x00 && b[2]==0x00 && b[3]==0x0C &&
        b[4]=='J' && b[5]=='X' && b[6]=='L' && b[7]==' ')
        return HINT_IMAGE;

    /* JPEG 2000 codestream: FF 4F FF 51 — or jp2 box 0000000C 6A50 2020 */
    if (b[0]==0xFF && b[1]==0x4F && n>=4 && b[2]==0xFF && b[3]==0x51)
        return HINT_IMAGE;
    if (n >= 12 && b[0]==0x00 && b[1]==0x00 && b[2]==0x00 && b[3]==0x0C &&
        b[4]=='j' && b[5]=='P' && b[6]==' ' && b[7]==' ')
        return HINT_IMAGE;

    /* RIFF: WebP / WAV / AVI */
    if (n >= 12 && b[0]=='R' && b[1]=='I' && b[2]=='F' && b[3]=='F') {
        if (b[8]=='W' && b[9]=='E' && b[10]=='B' && b[11]=='P') return HINT_IMAGE;
        if (b[8]=='W' && b[9]=='A' && b[10]=='V' && b[11]=='E') return HINT_AUDIO;
        if (b[8]=='A' && b[9]=='V' && b[10]=='I' && b[11]==' ') return HINT_CONTAINER;
        return HINT_CONTAINER;
    }

    /* ISOBMFF: ftyp at offset 4, brand at offset 8 */
    if (n >= 12 && b[4]=='f' && b[5]=='t' && b[6]=='y' && b[7]=='p') {
        const uint8_t *brand = b + 8;
        /* HEIF / AVIF still-image brands */
        static const char *img_brands[] = {
            "avif","avis","heic","heix","heim","heis","hevc","hevx",
            "mif1","msf1","mif2","jxl ","jxlc",
        };
        for (size_t i = 0; i < sizeof(img_brands)/sizeof(img_brands[0]); i++)
            if (is_ftyp_brand(brand, img_brands[i])) return HINT_IMAGE;
        /* Audio-only brands */
        static const char *audio_brands[] = {
            "M4A ","M4B ","M4P ","mp41","mp42",
        };
        /* mp41/mp42/M4A overlap with video — let FFmpeg decide; treat as container */
        (void)audio_brands;
        return HINT_CONTAINER;
    }

    /* Matroska / WebM */
    if (n >= 4 && b[0]==0x1A && b[1]==0x45 && b[2]==0xDF && b[3]==0xA3)
        return HINT_CONTAINER;

    /* FLV */
    if (b[0]=='F' && b[1]=='L' && b[2]=='V' && b[3]==0x01) return HINT_CONTAINER;

    /* MPEG-TS */
    if (b[0]==0x47 && n >= 188 && b[188]==0x47) return HINT_CONTAINER;

    /* MP3 ID3 / sync */
    if (b[0]=='I' && b[1]=='D' && b[2]=='3') return HINT_AUDIO;
    if (b[0]==0xFF && (b[1]&0xE0)==0xE0) return HINT_AUDIO;

    /* FLAC */
    if (b[0]=='f' && b[1]=='L' && b[2]=='a' && b[3]=='C') return HINT_AUDIO;

    /* Ogg */
    if (b[0]=='O' && b[1]=='g' && b[2]=='g' && b[3]=='S') return HINT_AUDIO;

    return HINT_UNKNOWN;
}

/* ================================================================== */
/*  AVIO callbacks for buffer-based open                               */
/* ================================================================== */

static int avio_read_cb(void *opaque, uint8_t *buf, int buf_size)
{
    MediaContext *mc = (MediaContext *)opaque;
    int64_t remaining = mc->src_size - mc->src_pos;
    if (remaining <= 0) return AVERROR_EOF;
    int to_read = buf_size < (int)remaining ? buf_size : (int)remaining;
    memcpy(buf, mc->src_data + mc->src_pos, (size_t)to_read);
    mc->src_pos += to_read;
    return to_read;
}

static int64_t avio_seek_cb(void *opaque, int64_t offset, int whence)
{
    MediaContext *mc = (MediaContext *)opaque;
    switch (whence) {
    case SEEK_SET: mc->src_pos = offset; break;
    case SEEK_CUR: mc->src_pos += offset; break;
    case SEEK_END: mc->src_pos = mc->src_size + offset; break;
    case AVSEEK_SIZE: return mc->src_size;
    default: return AVERROR(EINVAL);
    }
    if (mc->src_pos < 0) mc->src_pos = 0;
    if (mc->src_pos > mc->src_size) mc->src_pos = mc->src_size;
    return mc->src_pos;
}

/* ================================================================== */
/*  GPU upload helper (BGRA frame → WGPU texture + view)                */
/* ================================================================== */

static MediaFrame upload_bgra_frame(MediaBindings *mb, int w, int h,
                                    const uint8_t *bgra, int linesize,
                                    float duration_ms)
{
    MediaFrame f = {0};
    f.duration_ms = duration_ms;

    WGPUTextureDescriptor td = {
        .usage     = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
        .dimension = WGPUTextureDimension_2D,
        .size      = { (uint32_t)w, (uint32_t)h, 1 },
        .format    = WGPUTextureFormat_BGRA8Unorm,
        .mipLevelCount = 1,
        .sampleCount   = 1,
    };
    f.texture = wgpuDeviceCreateTexture(mb_dev(mb), &td);
    if (!f.texture) return f;

    f.view = wgpuTextureCreateView(f.texture, NULL);
    if (!f.view) {
        wgpuTextureDestroy(f.texture);
        wgpuTextureRelease(f.texture);
        f.texture = NULL;
        return f;
    }

    int aligned_bpr = (w * 4 + 255) & ~255;
    uint8_t *aligned_buf = NULL;
    const uint8_t *upload = bgra;
    int upload_bpr = linesize;

    if (linesize != aligned_bpr) {
        aligned_buf = (uint8_t *)malloc((size_t)aligned_bpr * h);
        if (aligned_buf) {
            for (int row = 0; row < h; row++)
                memcpy(aligned_buf + row * aligned_bpr,
                       bgra + row * linesize, (size_t)w * 4);
            upload = aligned_buf;
            upload_bpr = aligned_bpr;
        }
    }

    WGPUTexelCopyTextureInfo dst_info = {
        .texture = f.texture, .mipLevel = 0, .origin = {0,0,0},
    };
    WGPUTexelCopyBufferLayout layout = {
        .offset = 0, .bytesPerRow = (uint32_t)upload_bpr,
        .rowsPerImage = (uint32_t)h,
    };
    WGPUExtent3D extent = { (uint32_t)w, (uint32_t)h, 1 };
    wgpuQueueWriteTexture(mb_que(mb), &dst_info, upload,
                          (size_t)upload_bpr * h, &layout, &extent);

    free(aligned_buf);
    f.view_handle = htable_insert(&mb->wgpu->ht_texture_view, f.view);
    return f;
}

static int mc_add_frame(MediaContext *mc, MediaFrame f)
{
    if (mc->frame_count >= mc->frame_cap) {
        int nc = mc->frame_cap ? mc->frame_cap * 2 : 8;
        MediaFrame *nf = (MediaFrame *)realloc(mc->frames,
                                               (size_t)nc * sizeof(MediaFrame));
        if (!nf) return -1;
        mc->frames = nf;
        mc->frame_cap = nc;
    }
    mc->frames[mc->frame_count++] = f;
    return 0;
}

/* ================================================================== */
/*  Image path: decode every video-stream frame to BGRA, eagerly       */
/* ================================================================== */

static int decode_all_image_frames(MediaBindings *mb, AVFormatContext *fmt,
                                   MediaContext *mc)
{
    int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (si < 0) return -1;

    AVStream *st = fmt->streams[si];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return -1;

    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (!dec) return -1;
    if (avcodec_parameters_to_context(dec, st->codecpar) < 0 ||
        avcodec_open2(dec, codec, NULL) < 0) {
        avcodec_free_context(&dec);
        return -1;
    }

    mc->width  = dec->width;
    mc->height = dec->height;
    const AVPixFmtDescriptor *pfd = av_pix_fmt_desc_get(dec->pix_fmt);
    mc->has_alpha = pfd && (pfd->flags & AV_PIX_FMT_FLAG_ALPHA);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm = av_frame_alloc();
    struct SwsContext *sws = NULL;
    int bgra_ls = (mc->width * 4 + 255) & ~255;
    uint8_t *bgra = (uint8_t *)malloc((size_t)bgra_ls * mc->height);
    if (!pkt || !frm || !bgra) goto fail;

    int rc;
    while ((rc = av_read_frame(fmt, pkt)) >= 0) {
        if (pkt->stream_index != si) { av_packet_unref(pkt); continue; }
        int sret = avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
        if (sret < 0) continue;
        while (avcodec_receive_frame(dec, frm) == 0) {
            if (!sws) {
                sws = sws_getContext(frm->width, frm->height,
                                     (enum AVPixelFormat)frm->format,
                                     mc->width, mc->height, AV_PIX_FMT_BGRA,
                                     SWS_BILINEAR, NULL, NULL, NULL);
                if (!sws) { av_frame_unref(frm); goto fail; }
            }
            uint8_t *dd[1] = { bgra }; int ds[1] = { bgra_ls };
            sws_scale(sws, (const uint8_t *const *)frm->data,
                      frm->linesize, 0, frm->height, dd, ds);

            float dur = 100.0f;
            if (frm->duration > 0)
                dur = (float)av_rescale_q(frm->duration, st->time_base,
                                          (AVRational){1,1000});
            else if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
                dur = 1000.0f * (float)st->avg_frame_rate.den /
                      (float)st->avg_frame_rate.num;
            if (dur < 10.0f) dur = 100.0f;

            MediaFrame f = upload_bgra_frame(mb, mc->width, mc->height,
                                             bgra, bgra_ls, dur);
            av_frame_unref(frm);
            if (!f.texture) goto fail;
            if (mc_add_frame(mc, f) < 0) goto fail;
        }
    }
    avcodec_send_packet(dec, NULL);
    while (avcodec_receive_frame(dec, frm) == 0) {
        if (!sws) {
            sws = sws_getContext(frm->width, frm->height,
                                 (enum AVPixelFormat)frm->format,
                                 mc->width, mc->height, AV_PIX_FMT_BGRA,
                                 SWS_BILINEAR, NULL, NULL, NULL);
        }
        if (sws) {
            uint8_t *dd[1] = { bgra }; int ds[1] = { bgra_ls };
            sws_scale(sws, (const uint8_t *const *)frm->data,
                      frm->linesize, 0, frm->height, dd, ds);
            float dur = 100.0f;
            if (frm->duration > 0)
                dur = (float)av_rescale_q(frm->duration, st->time_base,
                                          (AVRational){1,1000});
            if (dur < 10.0f) dur = 100.0f;
            MediaFrame f = upload_bgra_frame(mb, mc->width, mc->height,
                                             bgra, bgra_ls, dur);
            if (f.texture) mc_add_frame(mc, f);
        }
        av_frame_unref(frm);
    }

    free(bgra);
    if (sws) sws_freeContext(sws);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    return mc->frame_count > 0 ? 0 : -1;

fail:
    free(bgra);
    if (sws) sws_freeContext(sws);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    return -1;
}

/* ================================================================== */
/*  LibRaw fallback for camera RAW                                     */
/* ================================================================== */

#ifdef YUMI_HAS_LIBRAW
static int try_libraw(MediaBindings *mb, const void *buf, int len, MediaContext *mc)
{
    libraw_data_t *raw = libraw_init(0);
    if (!raw) return -1;
    if (libraw_open_buffer(raw, (void *)buf, (size_t)len) != LIBRAW_SUCCESS ||
        libraw_unpack(raw) != LIBRAW_SUCCESS ||
        libraw_dcraw_process(raw) != LIBRAW_SUCCESS) {
        libraw_close(raw); return -1;
    }
    int err = 0;
    libraw_processed_image_t *img = libraw_dcraw_make_mem_image(raw, &err);
    if (!img || err != LIBRAW_SUCCESS) { libraw_close(raw); return -1; }
    int w = img->width, h = img->height;
    int bgra_ls = (w * 4 + 255) & ~255;
    uint8_t *bgra = (uint8_t *)malloc((size_t)bgra_ls * h);
    if (!bgra) { libraw_dcraw_clear_mem(img); libraw_close(raw); return -1; }

    if (img->colors == 3 && img->bits == 8) {
        for (int y = 0; y < h; y++) {
            const uint8_t *s = img->data + y * w * 3;
            uint8_t *d = bgra + y * bgra_ls;
            for (int x = 0; x < w; x++) {
                d[x*4+0]=s[x*3+2]; d[x*4+1]=s[x*3+1];
                d[x*4+2]=s[x*3+0]; d[x*4+3]=255;
            }
        }
    } else if (img->colors == 3 && img->bits == 16) {
        for (int y = 0; y < h; y++) {
            const uint16_t *s = (const uint16_t *)(img->data + y * w * 6);
            uint8_t *d = bgra + y * bgra_ls;
            for (int x = 0; x < w; x++) {
                d[x*4+0]=(uint8_t)(s[x*3+2]>>8);
                d[x*4+1]=(uint8_t)(s[x*3+1]>>8);
                d[x*4+2]=(uint8_t)(s[x*3+0]>>8);
                d[x*4+3]=255;
            }
        }
    } else {
        free(bgra); libraw_dcraw_clear_mem(img); libraw_close(raw); return -1;
    }

    mc->width = w; mc->height = h; mc->has_alpha = 0;
    MediaFrame f = upload_bgra_frame(mb, w, h, bgra, bgra_ls, 0.0f);
    free(bgra); libraw_dcraw_clear_mem(img); libraw_close(raw);
    if (!f.texture) return -1;
    return mc_add_frame(mc, f);
}
#endif

/* ================================================================== */
/*  Streaming path setup (video / audio)                               */
/* ================================================================== */

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *fmts)
{
    (void)ctx;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == MB_HW_PIX_FMT) return *p;
    return fmts[0];
}

static void release_current_video_frame(MediaBindings *mb, MediaContext *mc)
{
    WgpuBindings *wb = mb->wgpu;
    if (mc->view_handle) { htable_remove(&wb->ht_texture_view, mc->view_handle); mc->view_handle = 0; }
    if (mc->tex_handle)  { htable_remove(&wb->ht_texture,      mc->tex_handle);  mc->tex_handle  = 0; }
}

static int open_video_decoder(MediaBindings *mb, MediaContext *mc)
{
    int si = av_find_best_stream(mc->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (si < 0) { mc->video_stream_idx = -1; return 0; }
    mc->video_stream_idx = si;
    AVStream *st = mc->fmt_ctx->streams[si];
    mc->time_base = st->time_base;

    if (st->duration != AV_NOPTS_VALUE)
        mc->duration_ms = av_rescale_q(st->duration, st->time_base, (AVRational){1,1000});
    else if (mc->fmt_ctx->duration != AV_NOPTS_VALUE)
        mc->duration_ms = mc->fmt_ctx->duration / 1000;

    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return -1;
    mc->codec_ctx = avcodec_alloc_context3(codec);
    if (!mc->codec_ctx) return -1;
    if (avcodec_parameters_to_context(mc->codec_ctx, st->codecpar) < 0) return -1;

    mc->is_hw = false;
    if (mb->bridge && MB_HW_PIX_FMT != AV_PIX_FMT_NONE) {
        if (wff_bridge_init_hw_decoder(mb->bridge, mc->codec_ctx, NULL) >= 0) {
            mc->codec_ctx->get_format = get_hw_format;
            mc->is_hw = true;
        }
    }
    if (avcodec_open2(mc->codec_ctx, codec, NULL) < 0) return -1;

    mc->width  = mc->codec_ctx->width;
    mc->height = mc->codec_ctx->height;
    mc->pkt   = av_packet_alloc();
    mc->frame = av_frame_alloc();
    if (!mc->pkt || !mc->frame) return -1;
    return 0;
}

static int open_audio_decoder(MediaBindings *mb, MediaContext *mc)
{
    int si = av_find_best_stream(mc->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1,
                                 mc->video_stream_idx, NULL, 0);
    if (si < 0) { mc->audio_stream_idx = -1; return 0; }
    mc->audio_stream_idx = si;
    AVStream *st = mc->fmt_ctx->streams[si];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) { mc->audio_stream_idx = -1; return 0; }

    mc->audio_codec_ctx = avcodec_alloc_context3(codec);
    if (!mc->audio_codec_ctx) { mc->audio_stream_idx = -1; return 0; }
    if (avcodec_parameters_to_context(mc->audio_codec_ctx, st->codecpar) < 0 ||
        avcodec_open2(mc->audio_codec_ctx, codec, NULL) < 0) {
        avcodec_free_context(&mc->audio_codec_ctx);
        mc->audio_stream_idx = -1; return 0;
    }

    int out_rate = 48000;
    AVChannelLayout out_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout in_layout  = mc->audio_codec_ctx->ch_layout;

    if (swr_alloc_set_opts2(&mc->swr_ctx,
            &out_layout, AV_SAMPLE_FMT_S16, out_rate,
            &in_layout,  mc->audio_codec_ctx->sample_fmt,
                         mc->audio_codec_ctx->sample_rate,
            0, NULL) < 0 || swr_init(mc->swr_ctx) < 0) {
        swr_free(&mc->swr_ctx);
        avcodec_free_context(&mc->audio_codec_ctx);
        mc->audio_stream_idx = -1; return 0;
    }

    SDL_AudioSpec spec = {
        .format = SDL_AUDIO_S16LE, .channels = 2, .freq = out_rate,
    };
    mc->sdl_stream = SDL_CreateAudioStream(&spec, &spec);
    if (!mc->sdl_stream) {
        swr_free(&mc->swr_ctx);
        avcodec_free_context(&mc->audio_codec_ctx);
        mc->audio_stream_idx = -1; return 0;
    }
    if (mb->audio_device) SDL_BindAudioStream(mb->audio_device, mc->sdl_stream);

    /* Default: video → muted (visualisation); audio-only → audible. */
    bool start_muted = (mc->kind == MEDIA_KIND_VIDEO);
    mc->audio_muted  = start_muted;
    mc->audio_volume = 1.0f;
    SDL_SetAudioStreamGain(mc->sdl_stream, start_muted ? 0.0f : 1.0f);
    return 0;
}

static int ensure_sw_video_texture(MediaBindings *mb, MediaContext *mc)
{
    if (mc->sw_texture && mc->rgba_buf) return 0;

    enum AVPixelFormat src_fmt = mc->codec_ctx->pix_fmt;
    if (mc->frame->hw_frames_ctx) {
        AVHWFramesContext *hwf = (AVHWFramesContext *)mc->frame->hw_frames_ctx->data;
        src_fmt = hwf->sw_format;
    }
    mc->sws_ctx = sws_getContext(mc->width, mc->height, src_fmt,
                                 mc->width, mc->height, AV_PIX_FMT_BGRA,
                                 SWS_BILINEAR, NULL, NULL, NULL);
    if (!mc->sws_ctx) return -1;

    mc->rgba_linesize = (mc->width * 4 + 255) & ~255;
    mc->rgba_buf = (uint8_t *)malloc((size_t)mc->rgba_linesize * mc->height);
    if (!mc->rgba_buf) return -1;

    WGPUTextureDescriptor td = {
        .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
        .dimension = WGPUTextureDimension_2D,
        .size = { (uint32_t)mc->width, (uint32_t)mc->height, 1 },
        .format = WGPUTextureFormat_BGRA8Unorm,
        .mipLevelCount = 1, .sampleCount = 1,
    };
    mc->sw_texture = wgpuDeviceCreateTexture(mb_dev(mb), &td);
    if (!mc->sw_texture) return -1;
    mc->sw_view = wgpuTextureCreateView(mc->sw_texture, NULL);
    if (!mc->sw_view) return -1;

    mc->pixel_layout = WFF_PIXEL_LAYOUT_BGRA;
    mc->plane_count  = 1;
    mc->tex_format   = WGPUTextureFormat_BGRA8Unorm;
    return 0;
}

/* ================================================================== */
/*  Audio decode + spectrum                                            */
/* ================================================================== */

static void decode_audio_packet(MediaBindings *mb, MediaContext *mc, AVPacket *pkt)
{
    (void)mb;
    if (mc->audio_stream_idx < 0 || !mc->audio_codec_ctx) return;
    if (avcodec_send_packet(mc->audio_codec_ctx, pkt) < 0) return;

    AVFrame *af = av_frame_alloc();
    if (!af) return;
    while (avcodec_receive_frame(mc->audio_codec_ctx, af) == 0) {
        if (af->pts != AV_NOPTS_VALUE) {
            AVRational tb = mc->fmt_ctx->streams[mc->audio_stream_idx]->time_base;
            mc->audio_clock_pts = av_rescale_q(af->pts, tb, (AVRational){1,1000});
        }
        int out_samples = swr_get_out_samples(mc->swr_ctx, af->nb_samples);
        if (out_samples <= 0) { av_frame_unref(af); continue; }
        int buf_size = out_samples * 2 * 2;
        uint8_t *buf = (uint8_t *)malloc((size_t)buf_size);
        if (!buf) { av_frame_unref(af); continue; }
        uint8_t *out_bufs[1] = { buf };
        int conv = swr_convert(mc->swr_ctx, out_bufs, out_samples,
                               (const uint8_t **)af->extended_data, af->nb_samples);
        if (conv > 0 && mc->sdl_stream)
            SDL_PutAudioStreamData(mc->sdl_stream, buf, conv * 2 * 2);
        free(buf);
        av_frame_unref(af);
    }
    av_frame_free(&af);
}

static void pump_audio_buffer(MediaBindings *mb, MediaContext *mc)
{
    if (!mc->sdl_stream) return;
    const int target = 48000 * 2 * 2 / 5; /* ~200ms */
    for (int i = 0; i < 500; i++) {
        if (SDL_GetAudioStreamQueued(mc->sdl_stream) >= target) return;
        int rc = av_read_frame(mc->fmt_ctx, mc->pkt);
        if (rc < 0) return;
        if (mc->pkt->stream_index == mc->audio_stream_idx) {
            decode_audio_packet(mb, mc, mc->pkt);
        } else if (mc->codec_ctx && mc->pkt->stream_index == mc->video_stream_idx) {
            avcodec_send_packet(mc->codec_ctx, mc->pkt);
            while (avcodec_receive_frame(mc->codec_ctx, mc->frame) == 0) {
                if (mc->frame->pts != AV_NOPTS_VALUE)
                    mc->position_pts = mc->frame->pts;
                av_frame_unref(mc->frame);
            }
        }
        av_packet_unref(mc->pkt);
    }
}

void media_bindings_pump_audio(MediaBindings *mb)
{
    for (uint32_t h = 1; h <= mb->ht_media.capacity; h++) {
        MediaContext *mc = (MediaContext *)htable_get(&mb->ht_media, h);
        if (!mc) continue;
        if (mc->kind == MEDIA_KIND_VIDEO || mc->kind == MEDIA_KIND_AUDIO)
            pump_audio_buffer(mb, mc);
    }
}

/* ================================================================== */
/*  Video frame stepping                                               */
/* ================================================================== */

static int decode_next_video_frame(MediaBindings *mb, MediaContext *mc)
{
    if (mc->video_stream_idx < 0) return -1;
    release_current_video_frame(mb, mc);
    int rc;
    for (;;) {
        rc = av_read_frame(mc->fmt_ctx, mc->pkt);
        if (rc < 0) {
            if (rc == AVERROR_EOF || avio_feof(mc->fmt_ctx->pb)) return 0;
            return -1;
        }
        if (mc->pkt->stream_index == mc->audio_stream_idx) {
            decode_audio_packet(mb, mc, mc->pkt);
            av_packet_unref(mc->pkt); continue;
        }
        if (mc->pkt->stream_index != mc->video_stream_idx) {
            av_packet_unref(mc->pkt); continue;
        }
        rc = avcodec_send_packet(mc->codec_ctx, mc->pkt);
        av_packet_unref(mc->pkt);
        if (rc < 0) return -1;
        rc = avcodec_receive_frame(mc->codec_ctx, mc->frame);
        if (rc == AVERROR(EAGAIN)) continue;
        if (rc == AVERROR_EOF) return 0;
        if (rc < 0) return -1;
        break;
    }
    if (mc->frame->pts != AV_NOPTS_VALUE) mc->position_pts = mc->frame->pts;
    mc->width  = mc->frame->width;
    mc->height = mc->frame->height;

    AVFrame *src = mc->frame;
    AVFrame *tmp = NULL;
    if (mc->frame->format == MB_HW_PIX_FMT) {
        tmp = av_frame_alloc();
        if (!tmp) { av_frame_unref(mc->frame); return -1; }
        if (av_hwframe_transfer_data(tmp, mc->frame, 0) < 0) {
            av_frame_free(&tmp); av_frame_unref(mc->frame); return -1;
        }
        src = tmp;
    }

    if (ensure_sw_video_texture(mb, mc) < 0) {
        if (tmp) av_frame_free(&tmp);
        av_frame_unref(mc->frame);
        return -1;
    }
    uint8_t *dd[1] = { mc->rgba_buf };
    int      ds[1] = { mc->rgba_linesize };
    if (!mc->sws_ctx) {
        mc->sws_ctx = sws_getContext(
            mc->width, mc->height, (enum AVPixelFormat)src->format,
            mc->width, mc->height, AV_PIX_FMT_BGRA,
            SWS_BILINEAR, NULL, NULL, NULL);
    }
    if (mc->sws_ctx)
        sws_scale(mc->sws_ctx, (const uint8_t *const *)src->data,
                  src->linesize, 0, mc->height, dd, ds);

    WGPUTexelCopyTextureInfo dst = {
        .texture = mc->sw_texture, .mipLevel = 0, .origin = {0,0,0},
    };
    WGPUTexelCopyBufferLayout layout = {
        .offset = 0, .bytesPerRow = (uint32_t)mc->rgba_linesize,
        .rowsPerImage = (uint32_t)mc->height,
    };
    WGPUExtent3D ext = { (uint32_t)mc->width, (uint32_t)mc->height, 1 };
    wgpuQueueWriteTexture(mb_que(mb), &dst, mc->rgba_buf,
                          (size_t)mc->rgba_linesize * mc->height,
                          &layout, &ext);

    mc->tex_handle  = htable_insert(&mb->wgpu->ht_texture,      mc->sw_texture);
    mc->view_handle = htable_insert(&mb->wgpu->ht_texture_view, mc->sw_view);

    if (tmp) av_frame_free(&tmp);
    av_frame_unref(mc->frame);
    pump_audio_buffer(mb, mc);
    return 1;
}

/* ================================================================== */
/*  Open from buffer (universal)                                       */
/* ================================================================== */

static MediaContext *try_open_image(MediaBindings *mb,
                                    const uint8_t *buf, int len)
{
    MediaContext *mc = (MediaContext *)calloc(1, sizeof(*mc));
    if (!mc) return NULL;
    mc->kind = MEDIA_KIND_IMAGE;
    mc->video_stream_idx = -1;
    mc->audio_stream_idx = -1;

    /* Build an AVIO over a private copy. */
    uint8_t *src = (uint8_t *)malloc(len);
    if (!src) { free(mc); return NULL; }
    memcpy(src, buf, len);
    mc->src_data = src; mc->src_size = len; mc->src_pos = 0;

    static const int AVIO_BUF_SIZE = 32 * 1024;
    mc->avio_buf = (uint8_t *)av_malloc(AVIO_BUF_SIZE);
    if (!mc->avio_buf) goto fail;
    mc->avio_ctx = avio_alloc_context(mc->avio_buf, AVIO_BUF_SIZE, 0,
                                      mc, avio_read_cb, NULL, avio_seek_cb);
    if (!mc->avio_ctx) goto fail;

    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) goto fail;
    fmt->pb = mc->avio_ctx;

    if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0) {
        /* avformat_open_input frees fmt on failure but leaves avio. */
        goto fail;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0 ||
        decode_all_image_frames(mb, fmt, mc) < 0) {
        avformat_close_input(&fmt);
        goto fail_freed_avio;
    }
    avformat_close_input(&fmt);
    /* avio buffer is released by avformat_close_input. */
    mc->avio_ctx = NULL; mc->avio_buf = NULL;
    free(mc->src_data); mc->src_data = NULL;
    return mc;

fail:
    if (mc->avio_ctx) { av_freep(&mc->avio_ctx->buffer); avio_context_free(&mc->avio_ctx); }
    else if (mc->avio_buf) av_free(mc->avio_buf);
fail_freed_avio:
    free(mc->src_data);
    /* Note: frames may have been partially uploaded; leak avoidance done
       only on success path. Caller frees mc on failure. */
    if (mc->frame_count > 0) {
        for (int i = 0; i < mc->frame_count; i++) {
            MediaFrame *f = &mc->frames[i];
            if (f->view_handle) htable_remove(&mb->wgpu->ht_texture_view, f->view_handle);
            if (f->view) wgpuTextureViewRelease(f->view);
            if (f->texture) { wgpuTextureDestroy(f->texture); wgpuTextureRelease(f->texture); }
        }
        free(mc->frames);
    }
    free(mc);
    return NULL;
}

static MediaContext *try_open_stream(MediaBindings *mb,
                                     const uint8_t *buf, int len)
{
    MediaContext *mc = (MediaContext *)calloc(1, sizeof(*mc));
    if (!mc) return NULL;
    mc->video_stream_idx = -1;
    mc->audio_stream_idx = -1;

    mc->src_data = (uint8_t *)malloc(len);
    if (!mc->src_data) { free(mc); return NULL; }
    memcpy(mc->src_data, buf, len);
    mc->src_size = len; mc->src_pos = 0;

    static const int AVIO_BUF_SIZE = 32 * 1024;
    mc->avio_buf = (uint8_t *)av_malloc(AVIO_BUF_SIZE);
    if (!mc->avio_buf) goto fail;
    mc->avio_ctx = avio_alloc_context(mc->avio_buf, AVIO_BUF_SIZE, 0,
                                      mc, avio_read_cb, NULL, avio_seek_cb);
    if (!mc->avio_ctx) goto fail;

    mc->fmt_ctx = avformat_alloc_context();
    if (!mc->fmt_ctx) goto fail;
    mc->fmt_ctx->pb = mc->avio_ctx;

    if (avformat_open_input(&mc->fmt_ctx, NULL, NULL, NULL) < 0) {
        mc->fmt_ctx = NULL; goto fail;
    }
    if (avformat_find_stream_info(mc->fmt_ctx, NULL) < 0) goto fail;

    /* Provisional kind: video if there's a video stream, else audio. */
    int has_video = av_find_best_stream(mc->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1,-1,NULL,0) >= 0;
    int has_audio = av_find_best_stream(mc->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1,-1,NULL,0) >= 0;
    if (!has_video && !has_audio) goto fail;
    mc->kind = has_video ? MEDIA_KIND_VIDEO : MEDIA_KIND_AUDIO;

    if (has_video && open_video_decoder(mb, mc) < 0) goto fail;
    open_audio_decoder(mb, mc);

    /* Need at least one usable stream for the chosen kind. */
    if (mc->kind == MEDIA_KIND_VIDEO && !mc->codec_ctx) goto fail;
    if (mc->kind == MEDIA_KIND_AUDIO && mc->audio_stream_idx < 0) goto fail;

    /* Audio-only needs duration too. */
    if (mc->kind == MEDIA_KIND_AUDIO && mc->fmt_ctx->duration != AV_NOPTS_VALUE)
        mc->duration_ms = mc->fmt_ctx->duration / 1000;

    return mc;

fail:
    if (mc->sdl_stream)      { SDL_UnbindAudioStream(mc->sdl_stream); SDL_DestroyAudioStream(mc->sdl_stream); }
    if (mc->swr_ctx)         swr_free(&mc->swr_ctx);
    if (mc->audio_codec_ctx) avcodec_free_context(&mc->audio_codec_ctx);
    if (mc->frame)           av_frame_free(&mc->frame);
    if (mc->pkt)             av_packet_free(&mc->pkt);
    if (mc->codec_ctx)       avcodec_free_context(&mc->codec_ctx);
    if (mc->fmt_ctx)         avformat_close_input(&mc->fmt_ctx);
    else if (mc->avio_ctx)   { av_freep(&mc->avio_ctx->buffer); avio_context_free(&mc->avio_ctx); }
    else if (mc->avio_buf)   av_free(mc->avio_buf);
    free(mc->src_data);
    free(mc);
    return NULL;
}

static MediaContext *media_open_buffer(MediaBindings *mb,
                                       const uint8_t *buf, int len)
{
    if (len <= 0) return NULL;
    MagicHint h = classify_magic(buf, (size_t)len);

    MediaContext *mc = NULL;
    switch (h) {
    case HINT_IMAGE:
        mc = try_open_image(mb, buf, len);
        break;
    case HINT_AUDIO:
    case HINT_CONTAINER:
        mc = try_open_stream(mb, buf, len);
        break;
    default: break;
    }
    if (mc) return mc;

    /* Fallback chain: if first guess failed, try the other path. */
    if (h == HINT_IMAGE)
        mc = try_open_stream(mb, buf, len);
    else
        mc = try_open_image(mb, buf, len);
    if (mc) return mc;

#ifdef YUMI_HAS_LIBRAW
    {
        MediaContext *raw = (MediaContext *)calloc(1, sizeof(*raw));
        if (raw) {
            raw->kind = MEDIA_KIND_IMAGE;
            raw->video_stream_idx = -1;
            raw->audio_stream_idx = -1;
            if (try_libraw(mb, buf, len, raw) == 0) return raw;
            free(raw->frames);
            free(raw);
        }
    }
#endif
    return NULL;
}

/* ================================================================== */
/*  Destroy                                                            */
/* ================================================================== */

static void media_ctx_destroy(MediaBindings *mb, MediaContext *mc)
{
    if (!mc) return;
    /* Image frames */
    for (int i = 0; i < mc->frame_count; i++) {
        MediaFrame *f = &mc->frames[i];
        if (f->view_handle) htable_remove(&mb->wgpu->ht_texture_view, f->view_handle);
        if (f->view)        wgpuTextureViewRelease(f->view);
        if (f->texture)     { wgpuTextureDestroy(f->texture); wgpuTextureRelease(f->texture); }
    }
    free(mc->frames);

    /* Streaming */
    release_current_video_frame(mb, mc);
    if (mc->sw_view)    { wgpuTextureViewRelease(mc->sw_view); }
    if (mc->sw_texture) { wgpuTextureDestroy(mc->sw_texture); wgpuTextureRelease(mc->sw_texture); }
    if (mc->sws_ctx)    sws_freeContext(mc->sws_ctx);
    free(mc->rgba_buf);

    if (mc->sdl_stream)      { SDL_UnbindAudioStream(mc->sdl_stream); SDL_DestroyAudioStream(mc->sdl_stream); }
    if (mc->swr_ctx)         swr_free(&mc->swr_ctx);
    if (mc->audio_codec_ctx) avcodec_free_context(&mc->audio_codec_ctx);
    if (mc->frame)           av_frame_free(&mc->frame);
    if (mc->pkt)             av_packet_free(&mc->pkt);
    if (mc->codec_ctx)       avcodec_free_context(&mc->codec_ctx);
    if (mc->fmt_ctx)         avformat_close_input(&mc->fmt_ctx);
    else if (mc->avio_ctx)   { av_freep(&mc->avio_ctx->buffer); avio_context_free(&mc->avio_ctx); }
    free(mc->src_data);
    free(mc);
}

/* ================================================================== */
/*  WASM-callable functions                                            */
/* ================================================================== */

#define GET_MC()                                                          \
    MediaContext *mc = (MediaContext *)htable_get(&MB->ht_media,          \
                                                  (uint32_t)ARG_I32(0))

/* media_open(buf_ptr, buf_len) → handle */
static wasm_trap_t *fn_media_open(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    uint32_t bp = (uint32_t)ARG_I32(0);
    uint32_t bl = (uint32_t)ARG_I32(1);
    if (bl == 0 || (size_t)bp + bl > mb_mem_size(MB)) { RET_I32(0); return NULL; }
    /* Snapshot — guest may mutate / grow memory afterwards. */
    uint8_t *snap = (uint8_t *)malloc(bl);
    if (!snap) { RET_I32(0); return NULL; }
    memcpy(snap, mb_mem_base(MB) + bp, bl);
    MediaContext *mc = media_open_buffer(MB, snap, (int)bl);
    free(snap);
    if (!mc) { RET_I32(0); return NULL; }
    RET_I32(htable_insert(&MB->ht_media, mc));
    return NULL;
}

/* media_close(handle) */
static wasm_trap_t *fn_media_close(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    (void)res;
    uint32_t h = (uint32_t)ARG_I32(0);
    MediaContext *mc = (MediaContext *)htable_get(&MB->ht_media, h);
    if (mc) { media_ctx_destroy(MB, mc); htable_remove(&MB->ht_media, h); }
    return NULL;
}

static wasm_trap_t *fn_media_kind(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{ GET_MC(); RET_I32(mc ? (int32_t)mc->kind : (int32_t)MEDIA_KIND_UNKNOWN); return NULL; }

static wasm_trap_t *fn_media_width(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{ GET_MC(); RET_I32(mc ? mc->width : 0); return NULL; }

static wasm_trap_t *fn_media_height(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{ GET_MC(); RET_I32(mc ? mc->height : 0); return NULL; }

static wasm_trap_t *fn_media_has_alpha(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{ GET_MC(); RET_I32(mc ? mc->has_alpha : 0); return NULL; }

static wasm_trap_t *fn_media_duration_ms(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{ GET_MC(); RET_I32(mc ? (int32_t)mc->duration_ms : 0); return NULL; }

/* ---------------- IMAGE accessors ---------------- */

static wasm_trap_t *fn_media_frame_count(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{ GET_MC(); RET_I32(mc ? mc->frame_count : 0); return NULL; }

static wasm_trap_t *fn_media_frame_duration_ms(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    GET_MC();
    int idx = ARG_I32(1);
    if (!mc || idx < 0 || idx >= mc->frame_count) { RET_F32(0.0f); return NULL; }
    RET_F32(mc->frames[idx].duration_ms);
    return NULL;
}

static wasm_trap_t *fn_media_frame_view(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    GET_MC();
    int idx = ARG_I32(1);
    if (!mc || idx < 0 || idx >= mc->frame_count) { RET_I32(0); return NULL; }
    RET_I32((int32_t)mc->frames[idx].view_handle);
    return NULL;
}

/* ---------------- VIDEO streaming accessors ---------------- */

static wasm_trap_t *fn_media_decode_next(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    GET_MC();
    if (!mc || mc->kind != MEDIA_KIND_VIDEO) { RET_I32(-1); return NULL; }
    RET_I32(decode_next_video_frame(MB, mc));
    return NULL;
}

static wasm_trap_t *fn_media_seek(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    GET_MC();
    int32_t ms = ARG_I32(1);
    if (!mc || !mc->fmt_ctx) { RET_I32(-1); return NULL; }
    int stream_idx = mc->video_stream_idx >= 0 ? mc->video_stream_idx
                                               : mc->audio_stream_idx;
    if (stream_idx < 0) { RET_I32(-1); return NULL; }
    AVRational tb = mc->video_stream_idx >= 0
        ? mc->time_base
        : mc->fmt_ctx->streams[mc->audio_stream_idx]->time_base;
    int64_t ts = av_rescale_q((int64_t)ms, (AVRational){1,1000}, tb);
    if (av_seek_frame(mc->fmt_ctx, stream_idx, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        RET_I32(-1); return NULL;
    }
    if (mc->codec_ctx)       avcodec_flush_buffers(mc->codec_ctx);
    if (mc->audio_codec_ctx) avcodec_flush_buffers(mc->audio_codec_ctx);
    if (mc->sdl_stream)      SDL_ClearAudioStream(mc->sdl_stream);
    /* Frame-accurate forward decode for video. */
    if (mc->video_stream_idx >= 0) {
        for (;;) {
            int rc = av_read_frame(mc->fmt_ctx, mc->pkt);
            if (rc < 0) break;
            if (mc->pkt->stream_index != mc->video_stream_idx) {
                av_packet_unref(mc->pkt); continue;
            }
            rc = avcodec_send_packet(mc->codec_ctx, mc->pkt);
            av_packet_unref(mc->pkt);
            if (rc < 0) break;
            rc = avcodec_receive_frame(mc->codec_ctx, mc->frame);
            if (rc == AVERROR(EAGAIN)) continue;
            if (rc < 0) break;
            mc->position_pts = mc->frame->pts;
            av_frame_unref(mc->frame);
            if (mc->position_pts >= ts) break;
        }
    }
    RET_I32(0);
    return NULL;
}

static wasm_trap_t *fn_media_position_ms(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    GET_MC();
    if (!mc) { RET_I32(0); return NULL; }
    int64_t vid_ms = 0;
    if (mc->position_pts != AV_NOPTS_VALUE && mc->time_base.den > 0)
        vid_ms = av_rescale_q(mc->position_pts, mc->time_base, (AVRational){1,1000});
    int64_t ms = (mc->audio_clock_pts > vid_ms) ? mc->audio_clock_pts : vid_ms;
    RET_I32((int32_t)ms);
    return NULL;
}

static wasm_trap_t *fn_media_texture(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{ GET_MC(); RET_I32(mc ? (int32_t)mc->tex_handle : 0); return NULL; }

static wasm_trap_t *fn_media_texture_view(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{ GET_MC(); RET_I32(mc ? (int32_t)mc->view_handle : 0); return NULL; }

static wasm_trap_t *fn_media_pixel_layout(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{ GET_MC(); RET_I32(mc ? (int32_t)mc->pixel_layout : (int32_t)WFF_PIXEL_LAYOUT_UNKNOWN); return NULL; }

static wasm_trap_t *fn_media_plane_count(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{ GET_MC(); RET_I32(mc ? (int32_t)mc->plane_count : 0); return NULL; }

static wasm_trap_t *fn_media_is_hw(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{ GET_MC(); RET_I32(mc ? (int32_t)mc->is_hw : 0); return NULL; }

static wasm_trap_t *fn_media_format(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{ GET_MC(); RET_I32(mc ? (int32_t)mc->tex_format : (int32_t)WGPUTextureFormat_Undefined); return NULL; }

/* ---------------- AUDIO controls ---------------- */

static wasm_trap_t *fn_media_audio_set_muted(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    GET_MC();
    if (!mc || !mc->sdl_stream) { RET_I32(-1); return NULL; }
    mc->audio_muted = ARG_I32(1) ? true : false;
    SDL_SetAudioStreamGain(mc->sdl_stream, mc->audio_muted ? 0.0f : mc->audio_volume);
    RET_I32(0); return NULL;
}

static wasm_trap_t *fn_media_audio_is_muted(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{ GET_MC(); RET_I32(mc ? (int32_t)mc->audio_muted : 1); return NULL; }

static wasm_trap_t *fn_media_audio_set_volume(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    GET_MC();
    if (!mc || !mc->sdl_stream) { RET_I32(-1); return NULL; }
    float v = ARG_F32(1);
    if (v < 0.0f) v = 0.0f;
    mc->audio_volume = v;
    if (!mc->audio_muted) SDL_SetAudioStreamGain(mc->sdl_stream, v);
    RET_I32(0); return NULL;
}

static wasm_trap_t *fn_media_audio_get_volume(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{ GET_MC(); RET_F32(mc ? mc->audio_volume : 0.0f); return NULL; }

static wasm_trap_t *fn_media_audio_get_spectrum(void *env,
    const wasm_val_vec_t *args, wasm_val_vec_t *res)
{
    GET_MC();
    if (!mc) { RET_I32(0); return NULL; }
    int32_t out_off = ARG_I32(1);
    int32_t count   = ARG_I32(2);
    if (count > MB_SPECTRUM_BANDS) count = MB_SPECTRUM_BANDS;
    if (count <= 0) { RET_I32(0); return NULL; }
    size_t mem_sz = mb_mem_size(MB);
    uint8_t *mem = mb_mem_base(MB);
    size_t need = (size_t)out_off + (size_t)count * sizeof(float);
    if (!mem || need > mem_sz) { RET_I32(-1); return NULL; }
    memcpy(mem + out_off, MB->postmix_spectrum_decay,
           (size_t)count * sizeof(float));
    RET_I32(count);
    return NULL;
}

/* ================================================================== */
/*  Binding table                                                      */
/* ================================================================== */

typedef struct {
    const char *name;
    wasm_func_callback_with_env_t cb;
    uint32_t np; wasm_valkind_t params[4];
    uint32_t nr; wasm_valkind_t results[1];
} MBindingEntry;

#define I WASM_I32
#define F WASM_F32

static const MBindingEntry MEDIA_BINDINGS[] = {
    /* universal */
    {"media_open",                fn_media_open,              2, {I,I},   1, {I}},
    {"media_close",               fn_media_close,             1, {I},     0, {0}},
    {"media_kind",                fn_media_kind,              1, {I},     1, {I}},
    {"media_width",               fn_media_width,             1, {I},     1, {I}},
    {"media_height",              fn_media_height,            1, {I},     1, {I}},
    {"media_has_alpha",           fn_media_has_alpha,         1, {I},     1, {I}},
    {"media_duration_ms",         fn_media_duration_ms,       1, {I},     1, {I}},
    /* image */
    {"media_frame_count",         fn_media_frame_count,       1, {I},     1, {I}},
    {"media_frame_duration_ms",   fn_media_frame_duration_ms, 2, {I,I},   1, {F}},
    {"media_frame_view",          fn_media_frame_view,        2, {I,I},   1, {I}},
    /* video streaming */
    {"media_decode_next",         fn_media_decode_next,       1, {I},     1, {I}},
    {"media_seek",                fn_media_seek,              2, {I,I},   1, {I}},
    {"media_position_ms",         fn_media_position_ms,       1, {I},     1, {I}},
    {"media_texture",             fn_media_texture,           1, {I},     1, {I}},
    {"media_texture_view",        fn_media_texture_view,      1, {I},     1, {I}},
    {"media_pixel_layout",        fn_media_pixel_layout,      1, {I},     1, {I}},
    {"media_plane_count",         fn_media_plane_count,       1, {I},     1, {I}},
    {"media_is_hw",               fn_media_is_hw,             1, {I},     1, {I}},
    {"media_format",              fn_media_format,            1, {I},     1, {I}},
    /* audio */
    {"media_audio_set_muted",     fn_media_audio_set_muted,   2, {I,I},   1, {I}},
    {"media_audio_is_muted",      fn_media_audio_is_muted,    1, {I},     1, {I}},
    {"media_audio_set_volume",    fn_media_audio_set_volume,  2, {I,F},   1, {I}},
    {"media_audio_get_volume",    fn_media_audio_get_volume,  1, {I},     1, {F}},
    {"media_audio_get_spectrum",  fn_media_audio_get_spectrum,3, {I,I,I}, 1, {I}},
};

#undef I
#undef F

#define NUM_MEDIA_BINDINGS (sizeof(MEDIA_BINDINGS)/sizeof(MEDIA_BINDINGS[0]))

static wasm_functype_t *mmake_ft(uint32_t np, const wasm_valkind_t p[],
                                 uint32_t nr, const wasm_valkind_t r[])
{
    wasm_valtype_vec_t params, results;
    if (np > 0) {
        wasm_valtype_t *pt[4];
        for (uint32_t i = 0; i < np; i++) pt[i] = wasm_valtype_new(p[i]);
        wasm_valtype_vec_new(&params, np, pt);
    } else wasm_valtype_vec_new_empty(&params);
    if (nr > 0) {
        wasm_valtype_t *rt[1] = { wasm_valtype_new(r[0]) };
        wasm_valtype_vec_new(&results, nr, rt);
    } else wasm_valtype_vec_new_empty(&results);
    return wasm_functype_new(&params, &results);
}

/* ================================================================== */
/*  Postmix spectrum                                                   */
/* ================================================================== */

static void postmix_cb(void *userdata, const SDL_AudioSpec *spec,
                       float *buffer, int buflen)
{
    MediaBindings *mb = (MediaBindings *)userdata;
    int channels = spec->channels;
    int samples  = buflen / (int)sizeof(float) / channels;
    if (samples <= 0) return;

    int bands = MB_SPECTRUM_BANDS;
    float variance[MB_SPECTRUM_BANDS];
    for (int b = 0; b < bands; b++) {
        int stride = 1 << (bands - 1 - b);
        if (stride > samples / 2) stride = samples / 2;
        if (stride < 1) stride = 1;
        float sum = 0.0f; int cnt = 0;
        for (int j = stride; j < samples; j++) {
            float cur  = buffer[j * channels];
            float prev = buffer[(j - stride) * channels];
            if (channels > 1) {
                cur  = (cur  + buffer[j * channels + 1]) * 0.5f;
                prev = (prev + buffer[(j - stride) * channels + 1]) * 0.5f;
            }
            float diff = cur - prev;
            sum += diff * diff; cnt++;
        }
        variance[b] = cnt > 0 ? sum / (float)cnt : 0.0f;
    }
    for (int b = 0; b < bands; b++) {
        float energy = (b < bands - 1) ? variance[b] - variance[b+1] : variance[b];
        if (energy < 0.0f) energy = 0.0f;
        float rms = sqrtf(energy) * 3.0f;
        float cur = mb->postmix_spectrum_decay[b];
        mb->postmix_spectrum_decay[b] =
            (rms > cur) ? cur + (rms - cur) * 0.15f
                        : cur + (rms - cur) * 0.08f;
    }
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

void media_bindings_init(MediaBindings *mb, WgpuBindings *wgpu, WFFBridge *bridge)
{
    memset(mb, 0, sizeof(*mb));
    mb->wgpu   = wgpu;
    mb->bridge = bridge;
    htable_init(&mb->ht_media, 16);

    mb->audio_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
    if (!mb->audio_device) {
        fprintf(stderr, "[media] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    } else {
        SDL_ResumeAudioDevice(mb->audio_device);
        SDL_SetAudioPostmixCallback(mb->audio_device, postmix_cb, mb);
    }
    printf("[media] Initialized media bindings (%zu imports, HW=%s, audio=%s)\n",
           NUM_MEDIA_BINDINGS, bridge ? "yes" : "no",
           mb->audio_device ? "yes" : "no");
}

void media_bindings_destroy(MediaBindings *mb)
{
    for (uint32_t h = 1; h <= mb->ht_media.capacity; h++) {
        MediaContext *mc = (MediaContext *)htable_get(&mb->ht_media, h);
        if (mc) media_ctx_destroy(mb, mc);
    }
    htable_destroy(&mb->ht_media);
    if (mb->audio_device) {
        SDL_CloseAudioDevice(mb->audio_device);
        mb->audio_device = 0;
    }
}

void media_bindings_set_memory(MediaBindings *mb, wasm_memory_t *mem)
{
    mb->memory = mem;
}

size_t media_bindings_get_imports(MediaBindings *mb,
                                  wasm_store_t *store,
                                  const char ***out_names,
                                  wasm_func_t ***out_funcs)
{
    static const char *names[NUM_MEDIA_BINDINGS];
    static wasm_func_t *funcs[NUM_MEDIA_BINDINGS];
    for (size_t i = 0; i < NUM_MEDIA_BINDINGS; i++) {
        names[i] = MEDIA_BINDINGS[i].name;
        wasm_functype_t *ft = mmake_ft(MEDIA_BINDINGS[i].np,
                                       MEDIA_BINDINGS[i].params,
                                       MEDIA_BINDINGS[i].nr,
                                       MEDIA_BINDINGS[i].results);
        funcs[i] = wasm_func_new_with_env(store, ft, MEDIA_BINDINGS[i].cb, mb, NULL);
        wasm_functype_delete(ft);
    }
    *out_names = names;
    *out_funcs = funcs;
    return NUM_MEDIA_BINDINGS;
}
