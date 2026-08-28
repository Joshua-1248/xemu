# Fork licensing and provenance policy

This document records the licensing rules used when isolating custom fork
features from upstream xemu/QEMU code. It is intended to prevent future feature
moves from accidentally dropping attribution or changing a file's license.

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
   documented as dependencies but are not copied into the repository.
6. Git history and individual source headers remain authoritative for detailed
   authorship; summary documents must not erase upstream contributors.

## Current `xemu-features/` mapping

| Area | Default fork license | Important provenance notes |
| --- | --- | --- |
| `audio-packs/` | GPL-2.0-or-later | `audio-packs-apu.c/.h` are LGPL-2.1-or-later because the bridge is derived from MCPX/APU voice-processing code |
| `cheats/` | GPL-2.0-or-later | C++ parser/interpreter were developed from/validated against the non-bundled `xemu_trainer_lib` Python reference |
| `debug-tools/` | GPL-2.0-or-later | Built from fork debugger work and existing xemu/QEMU debug interfaces; retain notices in moved files |
| `fast-forward/` | GPL-2.0-or-later | Calls upstream timing/APU hooks; upstream files themselves retain their original licenses |
| `scripting/` | GPL-2.0-or-later | Lua/Python interpreters are external system programs, not vendored runtimes |
| `shared/` | GPL-2.0-or-later | `guest-memory.*` retains Matt Borgerson attribution for the xemu XBE helper lineage |
| `tas/` | GPL-2.0-or-later | Fork-owned TAS core/editor/tooling |
| `texture-packs/` | GPL-2.0-or-later | GL/Vulkan adapter pairs are LGPL-2.1-or-later and retain NV2A renderer lineage |
| `volume-amplifier/` | GPL-2.0-or-later | Fork-owned host-output gain extension using upstream SDL audio infrastructure |

The individual file header always wins over this summary.

## Upstream-derived adapter attribution

### MCPX APU

`xemu-features/audio-packs/audio-packs-apu.c` includes source/decode logic derived
from `hw/xbox/mcpx/apu/vp/vp.c`. The upstream MCPX implementation credits:

- espes;
- Jannik Vogel;
- Matt Borgerson;
- other xemu/XQEMU contributors.

The adapter is therefore marked LGPL-2.1-or-later and records that lineage.

### NV2A GL/Vulkan

The texture-pack backend adapters are coupled to and partially derived from the
NV2A renderer implementation. Relevant upstream lineage includes:

- espes;
- Jannik Vogel;
- Matt Borgerson;
- other xemu/XQEMU contributors.

The GL/Vulkan adapter files are marked LGPL-2.1-or-later. Generic texture-pack
policy/asset code remains separately GPL-2.0-or-later.

## Non-bundled development references

The repository contains comments referring to `xemu_trainer_lib/codes.py` and
`xemu_trainer_lib/cheatfiles.py`. Those Python files are not shipped here. The
available fork source does not name a separate public upstream URL/license for
them. Do not add or redistribute that Python source unless its provenance and
license are established. If an external upstream is identified, update both
`CREDITS.md` and `THIRD_PARTY_NOTICES.md`.

## Third-party dependency inventory

Use `THIRD_PARTY_NOTICES.md` for fork-relevant dependency/provenance notes and
the repository's existing `licenses/` directory for the wider upstream xemu
runtime/build dependency inventory.

## Future contribution checklist

When adding or moving code:

- retain original copyright lines;
- retain or add the correct SPDX identifier;
- do not convert an LGPL/MIT/BSD upstream file to GPL merely because a fork hook
  was added to it;
- when extracting code into a new feature-owned file, carry the original notice
  if meaningful code came from the upstream file;
- add any new third-party library or code source to `THIRD_PARTY_NOTICES.md`;
- identify non-obvious reference implementations and their licenses in
  `CREDITS.md`;
- never bundle copyrighted game assets as part of the emulator source tree.
