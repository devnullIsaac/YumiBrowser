project('yumibrowser', 'c', 'cpp')

cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')

# wasmer.h references `enum wasmer_parser_operator_t` unconditionally at the
# `wasmer_metering_cost_function_t` typedef, but the enum body itself is
# gated behind WASMER_COMPILER_ENABLED. C accepts the dangling tag; C++
# (e.g. src/slang_bindings.cpp) does not. Define it project-wide so the
# header is self-consistent in every translation unit.
add_project_arguments('-DWASMER_COMPILER_ENABLED', language: ['c', 'cpp'])

# ── Build mode option ────────────────────────────────────────────
# Set via: meson setup -Drelease_build=true
# or from build.sh via --release flag

option_release = get_option('release_build')

if option_release
  native_c_args = ['-O3', '-DNDEBUG']
  native_link_args = ['-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-Os']
else
  native_c_args = []
  native_link_args = []
endif

# ── Dependencies ─────────────────────────────────────────────────

wasmer_dep = declare_dependency(
  include_directories: include_directories('deps/wasmer/package/include'),
  dependencies: cc.find_library('wasmer',
    dirs: [meson.project_source_root() / 'deps/wasmer/package/lib']
  ),
)

sdl_dep = declare_dependency(
  include_directories: [
    include_directories('deps/sdl/include'),
    include_directories('deps/sdl/include/SDL3'),
  ],
  dependencies: cc.find_library('SDL3',
    dirs: [meson.project_source_root() / 'deps/sdl/build']
  ),
)

# Dawn — check lib64 first (Fedora/RHEL), then lib
dawn_libdir = meson.project_source_root() / 'deps/dawn/install/lib64'
if not cc.find_library('webgpu_dawn', dirs: [dawn_libdir], required: false).found()
  dawn_libdir = meson.project_source_root() / 'deps/dawn/install/lib'
endif

dawn_dep = declare_dependency(
  include_directories: [
    include_directories('deps/dawn/install/include'),
    include_directories('deps/dawn/install/include/dawn'),
  ],
  dependencies: [
    cc.find_library('webgpu_dawn',
      dirs: [dawn_libdir]
    ),
  ],
)

duckdb_dep = declare_dependency(
  include_directories: [
    include_directories('deps/duckdb/src/include'),
  ],
  dependencies: cc.find_library('duckdb',
    dirs: [meson.project_source_root() / 'deps/duckdb/build/release/src/']
  ),
)

freetype_dep = declare_dependency(
  include_directories: include_directories('deps/freetype/install/include/freetype2'),
  dependencies: cc.find_library('freetype',
    dirs: [meson.project_source_root() / 'deps/freetype/install/lib']
  ),
)

# ── OpenSSL + oqs-provider (post-quantum crypto) ────────────────
openssl_dep = declare_dependency(
  include_directories: include_directories('deps/openssl/install/include'),
  dependencies: [
    cc.find_library('crypto',
      dirs: [meson.project_source_root() / 'deps/openssl/install/lib']
    ),
    cc.find_library('ssl',
      dirs: [meson.project_source_root() / 'deps/openssl/install/lib']
    ),
  ],
)

# ── Crypto abstraction layer sources ─────────────────────────────
crypto_src = [
  'src/crypto/crypto.c',
  'src/crypto/skein.c',
  'src/crypto/skein_block.c',
]

crypto_test_env = environment()
crypto_test_env.set('OPENSSL_MODULES',
  meson.project_source_root() / 'deps/openssl/install/lib/ossl-modules')
crypto_test_env.prepend('LD_LIBRARY_PATH',
  meson.project_source_root() / 'deps/openssl/install/lib')

# ICU — Unicode services (bidi, line/grapheme breaking)
# Built from deps/icu via build.sh, installed to deps/icu/install
icu_common_dep = declare_dependency(
  include_directories: include_directories('deps/icu/install/include'),
  dependencies: cc.find_library('icuuc',
    dirs: [meson.project_source_root() / 'deps/icu/install/lib']
  ),
)

icu_i18n_dep = declare_dependency(
  dependencies: cc.find_library('icui18n',
    dirs: [meson.project_source_root() / 'deps/icu/install/lib']
  ),
)

icu_data_dep = declare_dependency(
  dependencies: cc.find_library('icudata',
    dirs: [meson.project_source_root() / 'deps/icu/install/lib']
  ),
)

# HarfBuzz — text shaping
# Built from deps/harfbuzz via build.sh, installed to deps/harfbuzz/install
harfbuzz_dep = declare_dependency(
  include_directories: include_directories('deps/harfbuzz/install/include/harfbuzz'),
  dependencies: cc.find_library('harfbuzz',
    dirs: [meson.project_source_root() / 'deps/harfbuzz/install/lib']
  ),
)

# FFmpeg — multimedia decoding/encoding (AGPL-3.0 build)
# Built from deps/ffmpeg via build.sh, installed to deps/ffmpeg/install.
# Libraries are now loaded dynamically at runtime via src/ffmpeg_loader.c.
# We still need the headers for struct / enum / macro definitions.
ffmpeg_inc = include_directories('deps/ffmpeg/install/include')
ffmpeg_libdir = meson.project_source_root() / 'deps/ffmpeg/install/lib'

fontconfig_dep = dependency('fontconfig')

# libxml2 — XML parser (system dep), used by contrib/svg-to-slang
libxml2_dep = dependency('libxml-2.0', required: false)

m_dep = cc.find_library('m', required: false)
cpp_stdlib = cxx.find_library('stdc++', required: true)

# pthreads — required by libjuice and other threading consumers
threads_dep = dependency('threads')

# dl — required by the FFmpeg dynamic loader on POSIX
dl_dep = cc.find_library('dl', required: false)

# ── libjuice — ICE/STUN/TURN (compiled from source) ─────────────
libjuice_src = [
  'deps/libjuice/src/addr.c',
  'deps/libjuice/src/agent.c',
  'deps/libjuice/src/base64.c',
  'deps/libjuice/src/conn.c',
  'deps/libjuice/src/conn_mux.c',
  'deps/libjuice/src/conn_poll.c',
  'deps/libjuice/src/conn_thread.c',
  'deps/libjuice/src/const_time.c',
  'deps/libjuice/src/crc32.c',
  'deps/libjuice/src/hash.c',
  'deps/libjuice/src/hmac.c',
  'deps/libjuice/src/ice.c',
  'deps/libjuice/src/juice.c',
  'deps/libjuice/src/log.c',
  'deps/libjuice/src/random.c',
  'deps/libjuice/src/server.c',
  'deps/libjuice/src/stun.c',
  'deps/libjuice/src/tcp.c',
  'deps/libjuice/src/timestamp.c',
  'deps/libjuice/src/turn.c',
  'deps/libjuice/src/udp.c',
]

libjuice_inc = [
  include_directories('deps/libjuice/include'),
  include_directories('deps/libjuice/include/juice'),
  include_directories('deps/libjuice/src'),
]

libjuice_c_args = ['-DUSE_NETTLE=0']

# Slang shader compiler — built via cmake presets, installed to deps/slang/install.
# libslang.so is a backward-compat symlink to libslang-compiler.so.
# Additional runtime libs (slang-rt, slang-llvm, slang-glslang, gfx) are
# collected into release/ for deployment but not linked directly.
slang_dep = declare_dependency(
  include_directories: include_directories('deps/slang/install/include'),
  dependencies: cc.find_library('slang',
    dirs: [meson.project_source_root() / 'deps/slang/install/lib']
  ),
)

# ── Deps array (must be declared before conditional appends) ─────
lzma_dep = dependency('liblzma')
deps = [wasmer_dep, sdl_dep, dawn_dep, duckdb_dep, freetype_dep,
        openssl_dep, icu_common_dep, icu_i18n_dep, icu_data_dep,
        harfbuzz_dep, m_dep, cpp_stdlib, fontconfig_dep, lzma_dep, slang_dep]
if dl_dep.found()
  deps += dl_dep
endif

# LibRaw — Camera RAW decoding (optional)
# Built from deps/libRaw via build.sh, installed to deps/libRaw/install
libraw_libdir = meson.project_source_root() / 'deps/libRaw/install/lib'
libraw_lib = cc.find_library('raw', dirs: [libraw_libdir], required: false)
if libraw_lib.found()
  libraw_dep = declare_dependency(
    include_directories: include_directories('deps/libRaw/install/include'),
    dependencies: libraw_lib,
  )
  add_project_arguments('-DYUMI_HAS_LIBRAW', language: 'c')
  deps += libraw_dep
endif
# ── Group Registrar sources ──────────────────────────────────────

group_registrar_src = [
  'src/group_registrar/buf.c',
  'src/group_registrar/db.c',
  'src/group_registrar/identity.c',
  'src/group_registrar/lifecycle.c',
  'src/group_registrar/peer.c',
  'src/group_registrar/role.c',
  'src/group_registrar/webapp.c',
  'src/group_registrar/server.c',
  'src/group_registrar/epoch.c',
  'src/group_registrar/retention.c',
  'src/group_registrar/crypto.c',
  'src/group_registrar/invite.c',
  'src/group_registrar/audit.c',
  'src/group_registrar/serialize.c',
  'src/group_registrar/util.c',
  'src/group_registrar/join.c',
  'src/group_registrar/icon.c',
  'src/group_registrar/behavior.c',
  'src/group_registrar/blocklist.c'
]

# ── Network sources ──────────────────────────────────────────────

network_src = [
  'src/network/bbr.c',
  'src/network/yumi_udp.c',
  'src/network/yumi_udp_client.c',
  'src/network/yumi_sudp_client.c',
]

# ── Yumi Client (high-level abstraction) ─────────────────────────

yumi_client_src = [
  'src/yumi_client.c',
]

# ── Main executable ──────────────────────────────────────────────

src = [
  'src/main.c',
  'src/dashboard_runtime.c',
  'src/webapp_runtime.c',
  'src/gpu.c',
  'src/wgpu_bindings.c',
  'src/duckdb_bindings.c',
  'src/sdl_bindings.c',
  'src/log_bindings.c',
  'src/text_bindings.c',
  'src/input_bindings.c',
  'src/clipboard_bindings.c',
  'src/font_bindings.c',
  'src/ffmpeg_loader.c',
  'src/wgpu_ffmpeg.c',
  'src/media_bindings.c',
  'src/slang_bindings.cpp',
  'src/network_bindings.c',
  'src/memory/static_heap.c',
] + group_registrar_src + crypto_src + network_src + yumi_client_src

# (above the executable() call)
build_rpath_dirs = [
    meson.project_source_root() / 'deps/wasmer/package/lib',
    meson.project_source_root() / 'deps/sdl/build',
    dawn_libdir,
    meson.project_source_root() / 'deps/duckdb/build/release/src',
    meson.project_source_root() / 'deps/freetype/install/lib',
    meson.project_source_root() / 'deps/openssl/install/lib',
    meson.project_source_root() / 'deps/icu/install/lib',
    meson.project_source_root() / 'deps/harfbuzz/install/lib',
    meson.project_source_root() / 'deps/ffmpeg/install/lib',
    meson.project_source_root() / 'deps/slang/install/lib',
]
if libraw_lib.found()
  build_rpath_dirs += libraw_libdir
endif

executable('yumibrowser',
  src + libjuice_src,
  dependencies: deps + [threads_dep],
  include_directories: [
    'include/',
    'src/',
    'src/group_registrar/',
    'src/crypto/',
    ffmpeg_inc,
  ] + libjuice_inc,
  c_args: native_c_args + libjuice_c_args,
  link_args: native_link_args,
  build_rpath: ':'.join(build_rpath_dirs),
  install_rpath: '$ORIGIN',
)

# =====================================================================
#  Test suite
# =====================================================================

test_buf = executable('test_buf',
  [
    'tests/test_buf.c',
  ] + group_registrar_src + crypto_src,
  dependencies: [duckdb_dep, openssl_dep, m_dep],
  include_directories: [
    'include/',
    'src/group_registrar/',
    'src/crypto/',
  ],
  c_args: native_c_args,
  link_args: native_link_args,
  build_rpath: ':'.join([
    meson.project_source_root() / 'deps/duckdb/build/release/src',
    meson.project_source_root() / 'deps/openssl/install/lib',
  ]),
  install: false,
)

test('buf', test_buf, env: crypto_test_env)

test_db = executable('test_db',
  [
    'tests/test_db.c',
  ] + group_registrar_src + crypto_src,
  dependencies: [duckdb_dep, openssl_dep, m_dep],
  include_directories: [
    'include/',
    'src/group_registrar/',
    'src/crypto/',
  ],
  c_args: native_c_args,
  link_args: native_link_args,
  build_rpath: ':'.join([
    meson.project_source_root() / 'deps/duckdb/build/release/src',
    meson.project_source_root() / 'deps/openssl/install/lib',
  ]),
  install: false,
)

test('db', test_db, env: crypto_test_env)

test_audit = executable('test_audit',
  [
    'tests/test_audit.c',
  ] + group_registrar_src + crypto_src,
  dependencies: [duckdb_dep, openssl_dep, m_dep],
  include_directories: [
    'include/',
    'src/group_registrar/',
    'src/crypto/',
  ],
  c_args: native_c_args,
  link_args: native_link_args,
  build_rpath: ':'.join([
    meson.project_source_root() / 'deps/duckdb/build/release/src',
    meson.project_source_root() / 'deps/openssl/install/lib',
  ]),
  install: false,
)

test('audit', test_audit, env: crypto_test_env)

test_crypto = executable('test_crypto',
  [
    'tests/test_crypto.c',
  ] + group_registrar_src + crypto_src,
  dependencies: [duckdb_dep, openssl_dep, m_dep],
  include_directories: [
    'include/',
    'src/group_registrar/',
    'src/crypto/',
  ],
  c_args: native_c_args,
  link_args: native_link_args,
  build_rpath: ':'.join([
    meson.project_source_root() / 'deps/duckdb/build/release/src',
    meson.project_source_root() / 'deps/openssl/install/lib',
  ]),
  install: false,
)

test('crypto', test_crypto, env: crypto_test_env)

test_join_verify = executable('test_join_verify',
  [
    'tests/test_join_verify.c',
  ] + group_registrar_src + crypto_src,
  dependencies: [duckdb_dep, openssl_dep, m_dep],
  include_directories: [
    'include/',
    'src/group_registrar/',
    'src/crypto/',
  ],
  c_args: native_c_args,
  link_args: native_link_args,
  build_rpath: ':'.join([
    meson.project_source_root() / 'deps/duckdb/build/release/src',
    meson.project_source_root() / 'deps/openssl/install/lib',
  ]),
  install: false,
)

test('join_verify', test_join_verify, env: crypto_test_env)

test_delta = executable('test_delta',
  [
    'tests/test_delta.c',
  ] + group_registrar_src + crypto_src,
  dependencies: [duckdb_dep, openssl_dep, m_dep],
  include_directories: [
    'include/',
    'src/group_registrar/',
    'src/crypto/',
  ],
  c_args: native_c_args,
  link_args: native_link_args,
  build_rpath: ':'.join([
    meson.project_source_root() / 'deps/duckdb/build/release/src',
    meson.project_source_root() / 'deps/openssl/install/lib',
  ]),
  install: false,
)

test('delta', test_delta, env: crypto_test_env)

# ── Crypto abstraction test ──────────────────────────────────────

test_crypto_abstract = executable('test_crypto_abstract',
  ['tests/test_crypto_abstract.c'] + crypto_src,
  dependencies: [openssl_dep, m_dep],
  include_directories: [
    'include/',
    'src/crypto/',
  ],
  c_args: native_c_args,
  link_args: native_link_args,
  install: false,
)

test('crypto_abstract', test_crypto_abstract, env: crypto_test_env)

# ── UDP client test ──────────────────────────────────────────────

test_udp_client = executable('test_udp_client',
  [
    'tests/test_udp_client.c',
    'src/network/bbr.c',
    'src/network/yumi_udp.c',
    'src/network/yumi_udp_client.c',
  ] + libjuice_src,
  dependencies: [m_dep, threads_dep],
  include_directories: [
    'include/',
  ] + libjuice_inc,
  c_args: native_c_args + libjuice_c_args,
  link_args: native_link_args,
  install: false,
)

test('udp_client', test_udp_client, timeout: 30)

# ── UDP session stress test (15 s) ──────────────────────────────

test_udp_session = executable('test_udp_session',
  [
    'tests/test_udp_session.c',
    'src/network/bbr.c',
    'src/network/yumi_udp.c',
    'src/network/yumi_udp_client.c',
  ] + libjuice_src + crypto_src,
  dependencies: [openssl_dep, m_dep, threads_dep],
  include_directories: [
    'include/',
    'src/crypto/',
  ] + libjuice_inc,
  c_args: native_c_args + libjuice_c_args,
  link_args: native_link_args,
  build_rpath: meson.project_source_root() / 'deps/openssl/install/lib',
  install: false,
)

test('udp_session', test_udp_session, timeout: 120, env: crypto_test_env)

# ── Secure UDP client test ──────────────────────────────────────

test_sudp_client = executable('test_sudp_client',
  [
    'tests/test_sudp_client.c',
    'src/network/bbr.c',
    'src/network/yumi_udp.c',
    'src/network/yumi_udp_client.c',
    'src/network/yumi_sudp_client.c',
  ] + libjuice_src + group_registrar_src + crypto_src,
  dependencies: [duckdb_dep, openssl_dep, m_dep, threads_dep],
  include_directories: [
    'include/',
    'src/group_registrar/',
    'src/crypto/',
  ] + libjuice_inc,
  c_args: native_c_args + libjuice_c_args,
  link_args: native_link_args,
  build_rpath: ':'.join([
    meson.project_source_root() / 'deps/duckdb/build/release/src',
    meson.project_source_root() / 'deps/openssl/install/lib',
  ]),
  install: false,
)

test('sudp_client', test_sudp_client, timeout: 120, env: crypto_test_env)

# ── Yumi Client unit tests ──────────────────────────────────────

test_yumi_client = executable('test_yumi_client',
  [
    'tests/test_yumi_client.c',
  ] + yumi_client_src + group_registrar_src + crypto_src,
  dependencies: [duckdb_dep, openssl_dep, m_dep, threads_dep],
  include_directories: [
    'include/',
    'src/group_registrar/',
    'src/crypto/',
  ],
  c_args: native_c_args,
  link_args: native_link_args,
  build_rpath: ':'.join([
    meson.project_source_root() / 'deps/duckdb/build/release/src',
    meson.project_source_root() / 'deps/openssl/install/lib',
  ]),
  install: false,
)

test('yumi_client', test_yumi_client, timeout: 60, env: crypto_test_env)

# ── Comprehensive registrar module tests ────────────────────────

test_registrar_full = executable('test_registrar_full',
  ['tests/test_registrar_full.c'] + group_registrar_src + crypto_src,
  dependencies: [duckdb_dep, openssl_dep, m_dep, threads_dep],
  include_directories: [
    'include/',
    'src/group_registrar/',
    'src/crypto/',
  ],
  c_args: native_c_args,
  link_args: native_link_args,
  build_rpath: ':'.join([
    meson.project_source_root() / 'deps/duckdb/build/release/src',
    meson.project_source_root() / 'deps/openssl/install/lib',
  ]),
  install: false,
)

test('registrar_full', test_registrar_full, timeout: 120, env: crypto_test_env)

# ── Dashboard runtime unit tests ───────────────────────────────

dashboard_test_src = [
  'src/dashboard_runtime.c',
  'src/webapp_runtime.c',
  'src/gpu.c',
  'src/wgpu_bindings.c',
  'src/duckdb_bindings.c',
  'src/sdl_bindings.c',
  'src/log_bindings.c',
  'src/text_bindings.c',
  'src/input_bindings.c',
  'src/clipboard_bindings.c',
  'src/font_bindings.c',
  'src/ffmpeg_loader.c',
  'src/wgpu_ffmpeg.c',
  'src/media_bindings.c',
  'src/slang_bindings.cpp',
  'src/network_bindings.c',
] + group_registrar_src + crypto_src + network_src + yumi_client_src

test_dashboard = executable('test_dashboard',
  ['tests/test_dashboard.c', 'src/memory/static_heap.c'] + dashboard_test_src + libjuice_src,
  dependencies: deps + [threads_dep],
  include_directories: [
    'include/',
    'src/',
    'src/group_registrar/',
    'src/crypto/',
    ffmpeg_inc,
  ] + libjuice_inc,
  c_args: native_c_args + libjuice_c_args,
  link_args: native_link_args,
  build_rpath: ':'.join(build_rpath_dirs),
  install: false,
)

dashboard_test_env = environment()
dashboard_test_env.set('OPENSSL_MODULES',
  meson.project_source_root() / 'deps/openssl/install/lib/ossl-modules')
dashboard_test_env.prepend('LD_LIBRARY_PATH',
  ':'.join(build_rpath_dirs))

test('dashboard', test_dashboard, timeout: 60, env: dashboard_test_env)
