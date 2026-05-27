# FFmpeg Multimedia Decoding and Encoding

Yumi Browser uses FFmpeg for multimedia decoding in image and video bindings, and for encoding when transcoding uploaded content. This page documents the project's policy on codec support, patent compliance, and dynamic library loading.

## Philosophy

Yumi Browser respects software patent law where applicable. The project does not distribute, encourage, or provide instructions for obtaining implementations of patented media codecs.

## Bundled FFmpeg Build: Patent-Free Codecs Only

The FFmpeg libraries bundled with Yumi Browser (built via `build.sh` from `deps/ffmpeg/`) are compiled with **only royalty-free, patent-free codecs** enabled. The bundled build explicitly excludes all patented codecs, including but not limited to:

- H.264 (MPEG-AVC)
- H.265 / HEVC
- AAC
- MP3
- AC-3 and E-AC-3
- MPEG-2 Video and MPEG-4 Visual
- VC-1
- WMV3 / WMA

### Codecs Included in the Bundled Build

The bundled FFmpeg enables only the following decoders:

- **Video**: AV1 (via libaom and libdav1d), VP8, VP9, Theora, WebP, PNG, APNG, GIF, BMP, TIFF, Targa, MJPEG, raw video
- **Audio**: Opus, Vorbis, FLAC, raw PCM variants
- **Image**: All image formats listed above, plus JPEG (via libjpeg-turbo)

All **patented** encoders are disabled. Only patent-free encoders are enabled: AV1 (libaom), Opus, Vorbis, FLAC, VP8, VP9, Theora, and common image formats (PNG, MJPEG, GIF, BMP, WebP, TIFF, Targa). Yumi Browser encodes media **only** to these royalty-free formats.

The bundled build is licensed under **GPL version 3** (via `--enable-gpl --enable-version3`), which is compatible with Yumi Browser's AGPL-3.0 license.

## System FFmpeg: User's Responsibility

Yumi Browser's dynamic loader (`src/ffmpeg_loader.c`) attempts to load the **system-provided FFmpeg libraries first** before falling back to the bundled build. If a system FFmpeg is present and contains additional codecs (including patented ones), Yumi Browser may load those libraries.

**Important:** When a system FFmpeg is loaded, Yumi Browser uses it for **decoding only**. Encoding and transcoding within Yumi Browser always uses the bundled patent-free encoder set (AV1, Opus, Vorbis, FLAC, and image formats). The project operates under the presumption that users who have a system FFmpeg with patented codecs installed have obtained the necessary licenses to use those decoders on their system in their jurisdiction. This is the same presumption that every Linux distribution, media player, and browser operates under when dynamically loading system libraries.

## Transcoding Policy: AV1 + Opus Only

Yumi Browser's architecture is designed to always transcode uploaded video content to **AV1** for video and **Opus** for audio. This is not a legal maneuver to evade patent enforcement; it is a deliberate technical choice with two independent goals.

### Goal 1: Patent-Free Content in the Group Ecosystem

When a user uploads a video file:

1. The original file is decoded using whatever decoder is available (bundled or system).
2. The decoded content is re-encoded to AV1 video + Opus audio.
3. The re-encoded content is what is stored and shared within the group.

This design means that even if a system FFmpeg with patented decoders is used to read an incoming file, the output that persists in the group ecosystem is always patent-free.

### Goal 2: No Asymmetry of Access Between Group Members

Groups are made of people with different system configurations. One member may have a system FFmpeg that includes patented decoders; another member may be running Yumi Browser on a system with only the bundled patent-free build. If the group stored and shared the original H.264 or AAC file, the second member would be unable to play it.

By coalescing every uploaded video to AV1 + Opus — the most widely supported royalty-free codecs — Yumi Browser is designed so that **every member of the group can access every piece of shared media**, regardless of what decoders their local system happens to have installed. The transcoding step is a common-denominator operation: it is intended to remove the decoder-availability lottery so that a file uploaded by one member is playable by every other member whose installation is functioning normally.

This is not about restricting choice. It is about ensuring that shared content is actually shared — that no member is silently locked out of a video, a song, or a picture because their operating system shipped a different set of libraries than the uploader's.

## Recommendations

- **For end users:** The bundled patent-free FFmpeg build is recommended. It provides full functionality for all Yumi Browser features without any patent concerns.
- **For packagers and distributions:** The `YUMI_FFMPEG_LIBDIR` environment variable allows overriding the FFmpeg library search path. Distributions may point this to their own FFmpeg builds if desired.
- **For users with patented codecs:** If you choose to install a system FFmpeg that includes patented codecs, you are responsible for ensuring you have the appropriate licenses for your jurisdiction. Yumi Browser will respect your system configuration but does not encourage or facilitate the use of patented codecs.

## Technical Implementation

The dynamic loader (`src/ffmpeg_loader.c`) resolves FFmpeg symbols at runtime using `dlopen`/`dlsym` (POSIX) or `LoadLibrary`/`GetProcAddress` (Windows). The loading order is:

1. System libraries (via the OS dynamic linker search path)
2. `$YUMI_FFMPEG_LIBDIR` override directory
3. Bundled libraries next to the executable
4. Bundled libraries in the source tree (`../deps/ffmpeg/install/lib`)

This allows maximum flexibility while keeping the default bundled build patent-free.
