# Fork licensing and provenance policy

This document records the licensing rules used when isolating and maintaining
custom fork features alongside upstream xemu/QEMU code. It is intended to
prevent future feature moves, ports, or upstream syncs from accidentally
dropping attribution or changing a component's license.

## Authoritative rules

1. Preserve the repository's upstream `LICENSE`, `COPYING`, and `COPYING.LIB`.
2. Preserve every existing per-file copyright and license notice in upstream
   code that is modified or moved.
3. QEMU's root `LICENSE` states that source files without their own licensing
   information are GPL-2.0-or-later. New fork-owned source should normally state
   that explicitly with an SPDX identifier rather than relying on the fallback.
4. Code moved or materially derived from an upstream file with another
   compatible license keeps that upstream licensing/attribution unless the
   legal basis for relicensing is explicit and documented.
5. Vendored third-party code keeps its original notice. System libraries are
   documented as dependencies; if a binary distribution ships or statically
   links one of them, the applicable binary-redistribution notice must be
   included with that distribution.
6. Git history and individual source headers remain authoritative for detailed
   authorship; summary documents must not erase upstream contributors.
7. Do not claim that the GPL license of the combined emulator changes the
   separate license of SDL3, MIT/BSD code, or other third-party components.

## Current `xemu-features/` mapping

| Area | Default fork license | Important provenance notes |
| --- | --- | --- |
| `audio-packs/` | GPL-2.0-or-later | `audio-packs-apu.c/.h` are LGPL-2.1-or-later because the bridge is derived from MCPX/APU voice-processing code |
| `cheats/` | GPL-2.0-or-later | Native port of Joshua-1248's MIT `xemu_trainer_lib` implementation; provenance retained below |
| `debug-tools/` | GPL-2.0-or-later | Built from fork debugger work and existing xemu/QEMU debug interfaces; retain notices in moved files |
| `fast-forward/` | GPL-2.0-or-later | Calls upstream timing/APU hooks; upstream files themselves retain their original licenses |
| `scripting/` | GPL-2.0-or-later | Lua/Python interpreters are external system programs, not vendored runtimes |
| `shared/` | GPL-2.0-or-later | `guest-memory.*` retains Matt Borgerson attribution for the xemu XBE helper lineage |
| `tas/` | GPL-2.0-or-later | Fork-owned TAS core/editor/tooling |
| `texture-packs/` | GPL-2.0-or-later | GL/Vulkan adapter pairs are LGPL-2.1-or-later and retain NV2A renderer lineage |
| `volume-amplifier/` | GPL-2.0-or-later | Fork-owned host-output gain extension using upstream SDL3 audio infrastructure |

The individual file header always wins over this summary.

## Resolved cheat-engine provenance

The previous unresolved `xemu_trainer_lib` provenance question is closed.

Source project:
<https://github.com/Joshua-1248/Xemu-Cheat-Engine-and-Trainer>

Project owner: **Joshua-1248**.

License: **MIT**.

The native xemu cheat implementation principally ports behavior/implementation
from:

- `xemu_trainer_lib/codes.py` -> `xemu-features/cheats/codes-engine.cc`;
- `xemu_trainer_lib/cheatfiles.py` -> `xemu-features/cheats/cheatfile.cc`.

The Python project is not bundled with this fork. The native files are
GPL-2.0-or-later in this repository while preserving the source-project MIT
notice and provenance in `licenses/xemu_trainer_lib.license.txt`,
`THIRD_PARTY_NOTICES.md`, `CREDITS.md`, and source comments.

## Upstream-derived adapter attribution

### MCPX APU

`xemu-features/audio-packs/audio-packs-apu.c` contains source/decode logic
derived from `hw/xbox/mcpx/apu/vp/vp.c`. The upstream MCPX implementation
credits espes, Jannik Vogel, Matt Borgerson, and other xemu/XQEMU contributors.
The adapter is therefore marked LGPL-2.1-or-later and records that lineage.

### NV2A GL/Vulkan

The texture-pack backend adapters are coupled to and partially derived from the
NV2A renderer implementation. Relevant upstream lineage includes espes, Jannik
Vogel, Matt Borgerson, and other xemu/XQEMU contributors. The GL/Vulkan adapter
files are marked LGPL-2.1-or-later. Generic texture-pack policy/asset code
remains separately GPL-2.0-or-later.

### Shared guest-memory helper

`xemu-features/shared/guest-memory.*` contains functionality moved from xemu XBE
helpers and retains Matt Borgerson's original attribution for the virtual to
physical helper lineage.

## FATX provenance

`ui/thirdparty/fatx/fatx.c` historically contained only the comment `This is
from libfatx`. The source project is Matt Borgerson's FATX project:
<https://github.com/mborgerson/fatx>, licensed GPL-2.0-or-later. The helper now
carries an explicit SPDX/provenance notice. The full GPLv2 text is already
shipped as `COPYING`.

## SDL3 handling

SDL3 is an inherited upstream xemu dependency and is licensed under the zlib
license. Preserve `licenses/SDL3.license.txt` and SDL's own source-tree
`LICENSE.txt`. If SDL source is modified directly, the zlib license requires
altered source versions to be plainly marked as altered; do not remove or
rewrite SDL's notice.

The outbound-license generator derives the SDL3 version from
`subprojects/sdl3.wrap` so the notice metadata follows future SDL3 version
bumps instead of silently remaining stale.

## Optional system libraries

libwebp and Capstone are optional system libraries for this fork. Their source
is not vendored by the fork. If a release build ships or statically links them,
that release must reproduce the applicable BSD-3-Clause notice. Retained copies
are stored as `licenses/libwebp.license.txt` and
`licenses/capstone.license.txt`; `scripts/gen-license.py` includes them when
present on a platform where xemu packaging ships the library.

## Inherited dependencies and generated code

The wider upstream dependency inventory is indexed by
`THIRD_PARTY_NOTICES.md`, the repository's `licenses/` directory, subproject
license files, and individual third-party source headers. In particular,
license/provenance must remain visible for generated or header-only code that is
compiled into the emulator, including nlohmann/json, genconfig, and generated
keycodemapdb tables.

## Future contribution checklist

When adding, moving, or syncing code:

- retain original copyright lines;
- retain or add the correct SPDX identifier;
- do not convert an LGPL/MIT/BSD/zlib upstream file to GPL merely because a fork
  hook was added to it;
- when extracting code into a new feature-owned file, carry the original notice
  if meaningful code came from the upstream file;
- add any new third-party library or code source to `THIRD_PARTY_NOTICES.md`;
- add binary-shipped dependencies to `scripts/gen-license.py` and retain a
  local license text when practical;
- identify non-obvious reference implementations and their licenses in
  `CREDITS.md` and the relevant feature README;
- update the license audit when a dependency version or subproject changes;
- never bundle copyrighted game assets as part of the emulator source tree.
