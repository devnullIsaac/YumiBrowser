/*
 * ffmpeg_loader.c
 *
 * Runtime dynamic loader for FFmpeg libraries.
 *
 * Loading order:
 *   1. System libraries  (e.g. /usr/lib/libavcodec.so)
 *   2. $YUMI_FFMPEG_LIBDIR override
 *   3. Bundled libraries next to the executable
 *   4. Bundled libraries in the source tree (dev builds)
 *
 * Uses dlopen/dlsym on POSIX and LoadLibrary/GetProcAddress on Windows.
 */

#include "ffmpeg_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <unistd.h>
#  include <limits.h>
#endif

#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif

/* ================================================================== */
/*  Platform helpers                                                   */
/* ================================================================== */

#ifdef _WIN32
static void *dyn_open(const char *path) { return (void *)LoadLibraryA(path); }
static void *dyn_sym(void *handle, const char *name)
{
    return (void *)GetProcAddress((HMODULE)handle, name);
}
static void dyn_close(void *handle) { if (handle) FreeLibrary((HMODULE)handle); }
#else
static void *dyn_open(const char *path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
static void *dyn_sym(void *handle, const char *name) { return dlsym(handle, name); }
static void dyn_close(void *handle) { if (handle) dlclose(handle); }
#endif

/* ================================================================== */
/*  Discover the directory containing the current executable          */
/* ================================================================== */

static int get_exe_dir(char *out, size_t out_len)
{
#if defined(_WIN32)
    wchar_t wpath[MAX_PATH];
    if (!GetModuleFileNameW(NULL, wpath, MAX_PATH))
        return -1;
    char path[MAX_PATH];
    if (!WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, MAX_PATH, NULL, NULL))
        return -1;
    char *sep = strrchr(path, '\\');
    if (!sep) sep = strrchr(path, '/');
    if (sep) *sep = '\0';
    snprintf(out, out_len, "%s", path);
    return 0;
#elif defined(__linux__)
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    path[n] = '\0';
    char *sep = strrchr(path, '/');
    if (sep) *sep = '\0';
    snprintf(out, out_len, "%s", path);
    return 0;
#elif defined(__APPLE__)
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return -1;
    char *sep = strrchr(path, '/');
    if (sep) *sep = '\0';
    snprintf(out, out_len, "%s", path);
    return 0;
#else
    (void)out; (void)out_len;
    return -1;
#endif
}

/* ================================================================== */
/*  Library base names (no path)                                       */
/* ================================================================== */

#ifdef _WIN32
static const char *NAME_AVUTIL      = "avutil.dll";
static const char *NAME_AVCODEC     = "avcodec.dll";
static const char *NAME_AVFORMAT    = "avformat.dll";
static const char *NAME_SWSCALE     = "swscale.dll";
static const char *NAME_SWRESAMPLE  = "swresample.dll";
#elif defined(__APPLE__)
static const char *NAME_AVUTIL      = "libavutil.dylib";
static const char *NAME_AVCODEC     = "libavcodec.dylib";
static const char *NAME_AVFORMAT    = "libavformat.dylib";
static const char *NAME_SWSCALE     = "libswscale.dylib";
static const char *NAME_SWRESAMPLE  = "libswresample.dylib";
#else
static const char *NAME_AVUTIL      = "libavutil.so";
static const char *NAME_AVCODEC     = "libavcodec.so";
static const char *NAME_AVFORMAT    = "libavformat.so";
static const char *NAME_SWSCALE     = "libswscale.so";
static const char *NAME_SWRESAMPLE  = "libswresample.so";
#endif

/* ================================================================== */
/*  Try to open a library from a list of candidate paths              */
/* ================================================================== */

static void *try_open_lib(const char *base_name,
                          const char *env_dir,
                          const char *exe_dir)
{
    char path[4096];
    void *h;

    /* 1. System library (ld.so resolution) */
    h = dyn_open(base_name);
    if (h) {
        printf("[ffmpeg-loader] Loaded system %s\n", base_name);
        return h;
    }

    /* 2. $YUMI_FFMPEG_LIBDIR override */
    if (env_dir && env_dir[0]) {
        snprintf(path, sizeof(path), "%s/%s", env_dir, base_name);
        h = dyn_open(path);
        if (h) {
            printf("[ffmpeg-loader] Loaded %s from YUMI_FFMPEG_LIBDIR\n", base_name);
            return h;
        }
    }

    /* 3. Next to the executable (release bundles) */
    if (exe_dir && exe_dir[0]) {
        snprintf(path, sizeof(path), "%s/%s", exe_dir, base_name);
        h = dyn_open(path);
        if (h) {
            printf("[ffmpeg-loader] Loaded bundled %s (exe dir)\n", base_name);
            return h;
        }

        /* 4. Source-tree dev build: ../deps/ffmpeg/install/lib */
        snprintf(path, sizeof(path), "%s/../deps/ffmpeg/install/lib/%s",
                 exe_dir, base_name);
        h = dyn_open(path);
        if (h) {
            printf("[ffmpeg-loader] Loaded bundled %s (dev path)\n", base_name);
            return h;
        }
    }

    return NULL;
}

/* ================================================================== */
/*  Handle storage                                                     */
/* ================================================================== */

static struct {
    void *avutil;
    void *avcodec;
    void *avformat;
    void *swscale;
    void *swresample;
} handles;

/* ================================================================== */
/*  Function pointer definitions — libavutil                          */
/* ================================================================== */

void  (*dyn_av_free)(void *ptr) = NULL;
void  (*dyn_av_freep)(void *arg) = NULL;
void *(*dyn_av_malloc)(size_t size) = NULL;
int64_t (*dyn_av_rescale_q)(int64_t a, AVRational bq, AVRational cq) = NULL;

AVBufferRef *(*dyn_av_buffer_ref)(AVBufferRef *buf) = NULL;
void         (*dyn_av_buffer_unref)(AVBufferRef **buf) = NULL;

AVFrame *(*dyn_av_frame_alloc)(void) = NULL;
void     (*dyn_av_frame_free)(AVFrame **frame) = NULL;
void     (*dyn_av_frame_unref)(AVFrame *frame) = NULL;

const AVPixFmtDescriptor *(*dyn_av_pix_fmt_desc_get)(enum AVPixelFormat pix_fmt) = NULL;

int (*dyn_av_hwdevice_ctx_create)(AVBufferRef **device_ctx,
                                   enum AVHWDeviceType type,
                                   const char *device,
                                   AVDictionary *opts,
                                   int flags) = NULL;
AVBufferRef *(*dyn_av_hwdevice_ctx_alloc)(enum AVHWDeviceType type) = NULL;
int          (*dyn_av_hwdevice_ctx_init)(AVBufferRef *ref) = NULL;
int          (*dyn_av_hwframe_map)(AVFrame *dst,
                                    const AVFrame *src,
                                    int flags) = NULL;
int          (*dyn_av_hwframe_transfer_data)(AVFrame *dst,
                                              const AVFrame *src,
                                              int flags) = NULL;

/* ================================================================== */
/*  Function pointer definitions — libavformat                        */
/* ================================================================== */

AVFormatContext *(*dyn_avformat_alloc_context)(void) = NULL;
int (*dyn_avformat_open_input)(AVFormatContext **ps,
                                const char *url,
                                AVInputFormat *fmt,
                                AVDictionary **options) = NULL;
int (*dyn_avformat_find_stream_info)(AVFormatContext *ic,
                                      AVDictionary **options) = NULL;
void (*dyn_avformat_close_input)(AVFormatContext **s) = NULL;

int (*dyn_av_find_best_stream)(AVFormatContext *ic,
                                enum AVMediaType type,
                                int wanted_stream_nb,
                                int related_stream,
                                AVCodec **decoder_ret,
                                int flags) = NULL;

int (*dyn_av_read_frame)(AVFormatContext *s, AVPacket *pkt) = NULL;
int (*dyn_av_seek_frame)(AVFormatContext *s,
                          int stream_index,
                          int64_t timestamp,
                          int flags) = NULL;

int (*dyn_avio_feof)(AVIOContext *s) = NULL;
AVIOContext *(*dyn_avio_alloc_context)(
    unsigned char *buffer,
    int buffer_size,
    int write_flag,
    void *opaque,
    int (*read_packet)(void *opaque, uint8_t *buf, int buf_size),
    int (*write_packet)(void *opaque, uint8_t *buf, int buf_size),
    int64_t (*seek)(void *opaque, int64_t offset, int whence)) = NULL;
void (*dyn_avio_context_free)(AVIOContext **s) = NULL;

/* ================================================================== */
/*  Function pointer definitions — libavcodec                         */
/* ================================================================== */

AVCodec *(*dyn_avcodec_find_decoder)(enum AVCodecID id) = NULL;
AVCodecContext *(*dyn_avcodec_alloc_context3)(const AVCodec *codec) = NULL;
int (*dyn_avcodec_parameters_to_context)(AVCodecContext *codec,
                                          const AVCodecParameters *par) = NULL;
int (*dyn_avcodec_open2)(AVCodecContext *avctx,
                          const AVCodec *codec,
                          AVDictionary **options) = NULL;
int (*dyn_avcodec_send_packet)(AVCodecContext *avctx,
                                const AVPacket *avpkt) = NULL;
int (*dyn_avcodec_receive_frame)(AVCodecContext *avctx,
                                  AVFrame *frame) = NULL;
void (*dyn_avcodec_flush_buffers)(AVCodecContext *avctx) = NULL;
void (*dyn_avcodec_free_context)(AVCodecContext **avctx) = NULL;

AVPacket *(*dyn_av_packet_alloc)(void) = NULL;
void      (*dyn_av_packet_free)(AVPacket **pkt) = NULL;
void      (*dyn_av_packet_unref)(AVPacket *pkt) = NULL;
void      (*dyn_av_packet_move_ref)(AVPacket *dst, AVPacket *src) = NULL;

/* ================================================================== */
/*  Function pointer definitions — libswscale                         */
/* ================================================================== */

struct SwsContext *(*dyn_sws_getContext)(
    int srcW, int srcH, enum AVPixelFormat srcFormat,
    int dstW, int dstH, enum AVPixelFormat dstFormat,
    int flags, SwsFilter *srcFilter,
    SwsFilter *dstFilter, const double *param) = NULL;

int (*dyn_sws_scale)(struct SwsContext *c,
                      const uint8_t *const srcSlice[],
                      const int srcStride[],
                      int srcSliceY, int srcSliceH,
                      uint8_t *const dst[],
                      const int dstStride[]) = NULL;

void (*dyn_sws_freeContext)(struct SwsContext *swsContext) = NULL;

/* ================================================================== */
/*  Function pointer definitions — libswresample                      */
/* ================================================================== */

int (*dyn_swr_alloc_set_opts2)(SwrContext **ps,
                                const AVChannelLayout *out_ch_layout,
                                enum AVSampleFormat out_sample_fmt,
                                int out_sample_rate,
                                const AVChannelLayout *in_ch_layout,
                                enum AVSampleFormat in_sample_fmt,
                                int in_sample_rate,
                                int log_offset, void *log_ctx) = NULL;

int  (*dyn_swr_init)(SwrContext *s) = NULL;
void (*dyn_swr_free)(SwrContext **s) = NULL;
int  (*dyn_swr_get_out_samples)(SwrContext *s, int in_samples) = NULL;
int  (*dyn_swr_convert)(SwrContext *s,
                         uint8_t **out, int out_count,
                         const uint8_t **in, int in_count) = NULL;

/* ================================================================== */
/*  Helper: load a single symbol from a specific library handle       */
/* ================================================================== */

#define LOAD(lib, name)                                                   \
    do {                                                                  \
        dyn_##name = (typeof(dyn_##name))dyn_sym(lib, #name);            \
        if (!dyn_##name) {                                                \
            fprintf(stderr, "[ffmpeg-loader] failed to resolve %s\n", #name); \
            return -1;                                                    \
        }                                                                 \
    } while(0)

/* ================================================================== */
/*  Loader entry point                                                 */
/* ================================================================== */

int ffmpeg_loader_init(void)
{
    memset(&handles, 0, sizeof(handles));

    /* Resolve environment override and executable directory once. */
    const char *env_dir = getenv("YUMI_FFMPEG_LIBDIR");
    char exe_dir[4096];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) != 0)
        exe_dir[0] = '\0';

    /* -------------------------------------------------------------- */
    /*  1. libavutil  — must load first (others depend on it)         */
    /* -------------------------------------------------------------- */
    handles.avutil = try_open_lib(NAME_AVUTIL, env_dir, exe_dir);
    if (!handles.avutil) {
        fprintf(stderr, "[ffmpeg-loader] failed to open %s (tried system, "
                        "YUMI_FFMPEG_LIBDIR, bundled)\n", NAME_AVUTIL);
        return -1;
    }
    LOAD(handles.avutil, av_free);
    LOAD(handles.avutil, av_freep);
    LOAD(handles.avutil, av_malloc);
    LOAD(handles.avutil, av_rescale_q);
    LOAD(handles.avutil, av_buffer_ref);
    LOAD(handles.avutil, av_buffer_unref);
    LOAD(handles.avutil, av_frame_alloc);
    LOAD(handles.avutil, av_frame_free);
    LOAD(handles.avutil, av_frame_unref);
    LOAD(handles.avutil, av_pix_fmt_desc_get);
    LOAD(handles.avutil, av_hwdevice_ctx_create);
    LOAD(handles.avutil, av_hwdevice_ctx_alloc);
    LOAD(handles.avutil, av_hwdevice_ctx_init);
    LOAD(handles.avutil, av_hwframe_map);
    LOAD(handles.avutil, av_hwframe_transfer_data);

    /* -------------------------------------------------------------- */
    /*  2. libavformat                                                 */
    /* -------------------------------------------------------------- */
    handles.avformat = try_open_lib(NAME_AVFORMAT, env_dir, exe_dir);
    if (!handles.avformat) {
        fprintf(stderr, "[ffmpeg-loader] failed to open %s\n", NAME_AVFORMAT);
        return -1;
    }
    LOAD(handles.avformat, avformat_alloc_context);
    LOAD(handles.avformat, avformat_open_input);
    LOAD(handles.avformat, avformat_find_stream_info);
    LOAD(handles.avformat, avformat_close_input);
    LOAD(handles.avformat, av_find_best_stream);
    LOAD(handles.avformat, av_read_frame);
    LOAD(handles.avformat, av_seek_frame);
    LOAD(handles.avformat, avio_feof);
    LOAD(handles.avformat, avio_alloc_context);
    LOAD(handles.avformat, avio_context_free);

    /* -------------------------------------------------------------- */
    /*  3. libavcodec                                                  */
    /* -------------------------------------------------------------- */
    handles.avcodec = try_open_lib(NAME_AVCODEC, env_dir, exe_dir);
    if (!handles.avcodec) {
        fprintf(stderr, "[ffmpeg-loader] failed to open %s\n", NAME_AVCODEC);
        return -1;
    }
    LOAD(handles.avcodec, avcodec_find_decoder);
    LOAD(handles.avcodec, avcodec_alloc_context3);
    LOAD(handles.avcodec, avcodec_parameters_to_context);
    LOAD(handles.avcodec, avcodec_open2);
    LOAD(handles.avcodec, avcodec_send_packet);
    LOAD(handles.avcodec, avcodec_receive_frame);
    LOAD(handles.avcodec, avcodec_flush_buffers);
    LOAD(handles.avcodec, avcodec_free_context);
    LOAD(handles.avcodec, av_packet_alloc);
    LOAD(handles.avcodec, av_packet_free);
    LOAD(handles.avcodec, av_packet_unref);
    LOAD(handles.avcodec, av_packet_move_ref);

    /* -------------------------------------------------------------- */
    /*  4. libswscale                                                  */
    /* -------------------------------------------------------------- */
    handles.swscale = try_open_lib(NAME_SWSCALE, env_dir, exe_dir);
    if (!handles.swscale) {
        fprintf(stderr, "[ffmpeg-loader] failed to open %s\n", NAME_SWSCALE);
        return -1;
    }
    LOAD(handles.swscale, sws_getContext);
    LOAD(handles.swscale, sws_scale);
    LOAD(handles.swscale, sws_freeContext);

    /* -------------------------------------------------------------- */
    /*  5. libswresample                                               */
    /* -------------------------------------------------------------- */
    handles.swresample = try_open_lib(NAME_SWRESAMPLE, env_dir, exe_dir);
    if (!handles.swresample) {
        fprintf(stderr, "[ffmpeg-loader] failed to open %s\n", NAME_SWRESAMPLE);
        return -1;
    }
    LOAD(handles.swresample, swr_alloc_set_opts2);
    LOAD(handles.swresample, swr_init);
    LOAD(handles.swresample, swr_free);
    LOAD(handles.swresample, swr_get_out_samples);
    LOAD(handles.swresample, swr_convert);

    printf("[ffmpeg-loader] All FFmpeg libraries loaded successfully.\n");
    return 0;
}

/* ================================================================== */
/*  Unloader                                                           */
/* ================================================================== */

void ffmpeg_loader_deinit(void)
{
    dyn_close(handles.swresample); handles.swresample = NULL;
    dyn_close(handles.swscale);    handles.swscale    = NULL;
    dyn_close(handles.avcodec);    handles.avcodec    = NULL;
    dyn_close(handles.avformat);   handles.avformat   = NULL;
    dyn_close(handles.avutil);     handles.avutil     = NULL;

    dyn_av_free                 = NULL;
    dyn_av_freep                = NULL;
    dyn_av_malloc               = NULL;
    dyn_av_rescale_q            = NULL;
    dyn_av_buffer_ref           = NULL;
    dyn_av_buffer_unref         = NULL;
    dyn_av_frame_alloc          = NULL;
    dyn_av_frame_free           = NULL;
    dyn_av_frame_unref          = NULL;
    dyn_av_pix_fmt_desc_get     = NULL;
    dyn_av_hwdevice_ctx_create  = NULL;
    dyn_av_hwdevice_ctx_alloc   = NULL;
    dyn_av_hwdevice_ctx_init    = NULL;
    dyn_av_hwframe_map          = NULL;
    dyn_av_hwframe_transfer_data = NULL;

    dyn_avformat_alloc_context     = NULL;
    dyn_avformat_open_input        = NULL;
    dyn_avformat_find_stream_info  = NULL;
    dyn_avformat_close_input       = NULL;
    dyn_av_find_best_stream        = NULL;
    dyn_av_read_frame              = NULL;
    dyn_av_seek_frame              = NULL;
    dyn_avio_feof                  = NULL;
    dyn_avio_alloc_context         = NULL;
    dyn_avio_context_free          = NULL;

    dyn_avcodec_find_decoder          = NULL;
    dyn_avcodec_alloc_context3        = NULL;
    dyn_avcodec_parameters_to_context = NULL;
    dyn_avcodec_open2                 = NULL;
    dyn_avcodec_send_packet           = NULL;
    dyn_avcodec_receive_frame         = NULL;
    dyn_avcodec_flush_buffers         = NULL;
    dyn_avcodec_free_context          = NULL;
    dyn_av_packet_alloc               = NULL;
    dyn_av_packet_free                = NULL;
    dyn_av_packet_unref               = NULL;
    dyn_av_packet_move_ref            = NULL;

    dyn_sws_getContext   = NULL;
    dyn_sws_scale        = NULL;
    dyn_sws_freeContext  = NULL;

    dyn_swr_alloc_set_opts2 = NULL;
    dyn_swr_init            = NULL;
    dyn_swr_free            = NULL;
    dyn_swr_get_out_samples = NULL;
    dyn_swr_convert         = NULL;
}
