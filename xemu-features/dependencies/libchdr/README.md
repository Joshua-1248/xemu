# Pinned libchdr dependency

Xemu Features #12 uses **libchdr v0.3.0** as a private/static CHD decoder.
The exact release/tag/commit and archive SHA-512 are recorded in
`UPSTREAM_VERSION.txt`.

The normal `build.sh` automatically runs `vendor-libchdr.sh` when the pinned
source has not already been materialized. The helper tries the upstream GitHub
release archive and a byte-identical Gentoo distfiles mirror, verifies the
pinned SHA-512, and only then installs the complete upstream tree at
`upstream/`. Meson itself never performs a network fetch.

No system libchdr package and no separate CMake invocation are required.
Libchdr is built privately by this directory's Meson file. To avoid duplicate
codec implementations/symbols in the xemu process, the private build reuses
QEMU's existing zlib dependency and reuses QEMU's zstd dependency when it is
available. If QEMU was configured without zstd, libchdr's pinned single-file
zstd decoder is used as the fallback. Libchdr's pinned LZMA decoder remains
feature-owned here.

The complete upstream source tree and its original notices remain under this
feature directory for provenance and source redistribution.

Upstream project: https://github.com/rtissera/libchdr
License: BSD-3-Clause (see `LICENSE.libchdr.txt` and upstream license files).


## Embedded codec licensing

The pinned libchdr source distribution also contains third-party codec material
that retains its own terms:

- LZMA SDK 25.01 — public domain
- dr_flac / dr_libs — Public Domain or MIT No Attribution
- miniz 3.1.1 — MIT; retained for source provenance but not compiled by Xemu's
  `CHDR_SYSTEM_ZLIB` build
- zstd 1.5.7 fallback decoder — BSD-style or GPLv2; compiled only when QEMU's
  zstd dependency is unavailable

The authoritative notices remain in the vendored source. Supplemental copies
for outbound packaging live under `licenses/` where applicable. The
Xemu-specific build glue and CHD adapter remain GPL-2.0-or-later and do not
relicense any of these third-party components.
