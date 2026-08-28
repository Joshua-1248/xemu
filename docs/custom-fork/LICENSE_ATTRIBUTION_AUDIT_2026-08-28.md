# License and attribution review — 2026-08-28

This document records the corrective pass performed on the Joshua-1248 xemu
fork after Feature Isolation Phase 4. It is a provenance/packaging checklist,
not legal advice. Authoritative terms remain the individual source notices and
license texts distributed with the repository and its dependencies.

## Status after this correction

The review found no reason to replace or remove xemu/QEMU's existing licensing
structure. The corrective work instead closes attribution and packaging gaps
introduced or exposed by the custom feature set.

Resolved in this package:

- `xemu_trainer_lib` provenance is no longer unknown. The source/reference
  project is Joshua-1248's MIT-licensed
  `https://github.com/Joshua-1248/Xemu-Cheat-Engine-and-Trainer`.
  `xemu_trainer_lib/codes.py` is the principal source/reference for the native
  cheat interpreter and `xemu_trainer_lib/cheatfiles.py` for the native parser.
- SDL is documented explicitly as **SDL3 / Simple DirectMedia Layer**, under
  the zlib license, and the outbound license generator obtains its version from
  `subprojects/sdl3.wrap` instead of retaining a stale hard-coded version.
- The outbound license generator obtains the QEMU version from `QEMU_VERSION`
  instead of the obsolete hard-coded `6.0.0` value, and derives the bundled
  stb_image/stb_image_write versions from their actual headers.
- The generator's missing-license cache path is repaired (`self.license_path`
  replaces the undefined `fname`).
- `stb_image_write`, nlohmann/json, genconfig, generated keycodemapdb, RenderDoc
  API, gloffscreen, and the trainer-source MIT notice are represented in the
  generated outbound license inventory.
- Optional libwebp and Capstone notices are retained and included by the
  generator when those dependencies are present on a platform where they are
  linked/shipped.
- libwebp's retained notice also records the WebM additional patent grant.
- `ui/thirdparty/fatx/fatx.c` gets explicit provenance for Matt Borgerson's
  GPL-2.0-or-later `mborgerson/fatx` project instead of only the ambiguous
  historical comment `This is from libfatx`.
- every C/C++ source/header under `xemu-features/` is required to carry an SPDX
  license identifier; the MCPX/NV2A derived bridge files retain
  LGPL-2.1-or-later and the ordinary fork-owned feature files use
  GPL-2.0-or-later.
- `scripts/xemu-feature-isolation-audit.py` carries a GPL SPDX marker.
- `THIRD_PARTY_NOTICES.md` indexes all existing repository-level
  `licenses/*.license.txt` files plus the notices added by this package.
- the README and CREDITS now describe the full current custom feature set and
  preserve upstream/project/dependency lineage rather than implying that all
  code in the combined repository has one per-file license.

## Source-tree third-party material checked

The review also identified source/subproject notices that should remain intact
through future upstream syncs, including:

- SDL3;
- RenderDoc's in-application API header;
- NVIDIA NVAPI headers;
- gloffscreen;
- Berkeley SoftFloat/TestFloat test subprojects;
- genconfig;
- nlohmann/json;
- keycodemapdb generated tables;
- stb_image/stb_image_write;
- SPIR-V/graphics subprojects already represented in `licenses/`;
- the inherited QEMU/xemu/XQEMU source notices.

These components are indexed in `THIRD_PARTY_NOTICES.md`; their own license
files/source headers remain authoritative.

## Ongoing rules

For future custom features and upstream merges:

1. Do not remove upstream copyright or license notices.
2. Do not relabel LGPL/MIT/BSD/zlib source as GPL merely because it is linked
   into the GPL-covered xemu program.
3. When meaningful code is extracted from an upstream file into a new custom
   feature file, carry forward the applicable notice/license lineage.
4. Document non-obvious reference implementations such as the Python trainer
   port directly in source comments and the fork's provenance documents.
5. Add newly shipped or statically linked third-party libraries to the outbound
   license generator and retain their redistribution notices.
6. Keep `scripts/xemu-license-audit.py` passing when dependencies, versions,
   or feature-owned source files change.
7. Keep user-authored texture/audio packs, scripts, TAS movies, and copyrighted
   Xbox game assets legally separate from the emulator source/distribution.

## Known distinction: source inventory vs packaged-binary inventory

`THIRD_PARTY_NOTICES.md` is intentionally broad: it documents material present
in the source tree as well as fork-relevant dependencies. `scripts/gen-license.py`
is narrower and emits notices applicable to a packaged binary on the selected
platform. A component being listed in one but not emitted in a particular
binary's generated license file does not by itself mean its source-tree notice
is missing.
