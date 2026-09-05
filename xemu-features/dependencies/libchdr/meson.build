# SPDX-License-Identifier: GPL-2.0-or-later
# Feature-owned, pinned libchdr build for xemu's CHD block format.
#
# Keep libchdr itself pinned under xemu-features, but deliberately reuse QEMU's
# already-established zlib/libzstd dependencies when available. This avoids
# duplicate miniz/zstd global symbols in the final xemu executable and mirrors
# libchdr's own WITH_SYSTEM_ZLIB/WITH_SYSTEM_ZSTD supported configuration.
libchdr_root = meson.current_source_dir() / 'upstream'
libchdr_inc = include_directories('upstream/include')
libchdr_private_inc = [
    include_directories('upstream/src'),
    include_directories('upstream/deps/lzma-25.01/include'),
]

libchdr_sources = files(
    'upstream/deps/lzma-25.01/src/LzmaDec.c',
    'upstream/src/libchdr_bitstream.c',
    'upstream/src/libchdr_cdrom.c',
    'upstream/src/libchdr_chd.c',
    'upstream/src/libchdr_codec_cdfl.c',
    'upstream/src/libchdr_codec_cdlz.c',
    'upstream/src/libchdr_codec_cdzl.c',
    'upstream/src/libchdr_codec_cdzs.c',
    'upstream/src/libchdr_codec_flac.c',
    'upstream/src/libchdr_codec_huff.c',
    'upstream/src/libchdr_codec_lzma.c',
    'upstream/src/libchdr_codec_zlib.c',
    'upstream/src/libchdr_codec_zstd.c',
    'upstream/src/libchdr_flac.c',
    'upstream/src/libchdr_huffman.c',
)

libchdr_cargs = [
    '-DWANT_RAW_DATA_SECTOR=1',
    '-DWANT_SUBCODE=1',
    '-DVERIFY_BLOCK_CRC=1',
    '-DCHDR_SYSTEM_ZLIB=1',
]
libchdr_external_deps = [zlib]

# QEMU's libzstd support is optional. Reuse it when present; otherwise use
# libchdr's pinned single-file decoder. The bundled fallback is only compiled
# when QEMU is not already bringing libzstd into the process.
if zstd.found()
    libchdr_cargs += ['-DCHDR_SYSTEM_ZSTD=1']
    libchdr_external_deps += [zstd]
else
    libchdr_private_inc += [include_directories('upstream/deps/zstd-1.5.7')]
    libchdr_sources += files('upstream/deps/zstd-1.5.7/zstddeclib.c')
endif

if host_os == 'windows'
    libchdr_cargs += ['-D_CRT_SECURE_NO_WARNINGS']
endif

xemu_bundled_libchdr = static_library(
    'xemu-libchdr',
    libchdr_sources,
    include_directories: [libchdr_inc] + libchdr_private_inc,
    dependencies: libchdr_external_deps,
    c_args: libchdr_cargs,
    pic: true,
    build_by_default: true,
)

xemu_bundled_libchdr_dep = declare_dependency(
    link_with: xemu_bundled_libchdr,
    dependencies: libchdr_external_deps,
    include_directories: libchdr_inc,
)
