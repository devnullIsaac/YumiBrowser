#!/usr/bin/env bash
# Patent-free lite FFmpeg build (AGPL-3.0, royalty-free codecs only).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"

step "Building FFmpeg (patent-free lite build)"

if [ -f "${FFMPEG_INSTALL}/lib/libavcodec.so" ] || \
   [ -f "${FFMPEG_INSTALL}/lib/libavcodec.dylib" ]; then
    warn "FFmpeg already built — skipping (delete ${FFMPEG_INSTALL} to rebuild)"
    exit 0
fi

if [ ! -f "${FFMPEG_DIR}/configure" ]; then
    fail "FFmpeg configure script not found at ${FFMPEG_DIR}/configure"
fi

mkdir -p "${FFMPEG_BUILD}"
pushd "${FFMPEG_BUILD}" >/dev/null

"${FFMPEG_DIR}/configure" \
    --prefix="${FFMPEG_INSTALL}" \
    --enable-shared \
    --disable-static \
    --enable-gpl \
    --enable-version3 \
    --disable-encoders \
    --disable-decoders \
    --enable-encoder=libaom_av1 \
    --enable-encoder=libopus \
    --enable-encoder=libvorbis \
    --enable-encoder=flac \
    --enable-encoder=vp8 \
    --enable-encoder=vp9 \
    --enable-encoder=libtheora \
    --enable-encoder=png \
    --enable-encoder=mjpeg \
    --enable-encoder=gif \
    --enable-encoder=bmp \
    --enable-encoder=tiff \
    --enable-encoder=targa \
    --enable-encoder=webp \
    --enable-encoder=apng \
    --enable-encoder=pcm_s16le \
    --enable-encoder=pcm_s16be \
    --enable-encoder=pcm_u8 \
    --enable-encoder=pcm_f32le \
    --enable-encoder=pcm_s24le \
    --enable-encoder=pcm_s32le \
    --enable-encoder=rawvideo \
    --enable-decoder=libaom_av1 \
    --enable-decoder=libdav1d \
    --enable-decoder=vp8 \
    --enable-decoder=vp9 \
    --enable-decoder=flac \
    --enable-decoder=opus \
    --enable-decoder=libopus \
    --enable-decoder=vorbis \
    --enable-decoder=libvorbis \
    --enable-decoder=theora \
    --enable-decoder=libtheora \
    --enable-decoder=png \
    --enable-decoder=mjpeg \
    --enable-decoder=gif \
    --enable-decoder=bmp \
    --enable-decoder=tiff \
    --enable-decoder=targa \
    --enable-decoder=webp \
    --enable-decoder=apng \
    --enable-decoder=pcm_s16le \
    --enable-decoder=pcm_s16be \
    --enable-decoder=pcm_u8 \
    --enable-decoder=pcm_f32le \
    --enable-decoder=pcm_s24le \
    --enable-decoder=pcm_s32le \
    --enable-decoder=rawvideo \
    --enable-libaom \
    --enable-libdav1d \
    --enable-libwebp \
    --disable-doc \
    --disable-programs \
    --disable-debug

make -j "${JOBS}"
make install
ok "FFmpeg (patent-free lite) installed to ${FFMPEG_INSTALL}"

popd >/dev/null
