# Credits and attribution

This file documents attribution for the Joshua-1248 xemu fork. It supplements,
and does not replace, the copyright and license notices carried by individual
source files or the repository's `LICENSE`, `COPYING`, `COPYING.LIB`, and
`licenses/` files.

## This fork

Fork-specific feature development, integration, packaging, and maintenance by
**Joshua-1248** (<https://github.com/Joshua-1248/xemu>), including the current
custom feature set:

- texture dumping/replacement, animated replacements, and procedural texture shaders;
- source-level Xbox audio dumping and WAV replacement;
- cheats/patches and the in-emulator codes editor;
- fast forward and pitch-preservation work;
- TAS/TAStudio tools;
- Lua/Python scripting consoles and bridge APIs;
- custom x86 disassembler/debugger integration;
- the optional 0-200% output-volume extension;
- feature-isolation/portability work under `xemu-features/`;
- fork-specific stability and quality-of-life fixes.

Git history remains the authoritative record for individual changes. Attribution
here does not imply sole authorship of code moved or derived from upstream
xemu/XQEMU/QEMU files; those origins are called out below and in file headers.

## Upstream xemu, XQEMU, and QEMU

This repository is a fork of [xemu](https://github.com/xemu-project/xemu), the
Original Xbox emulator created and maintained by **Matt Borgerson** and xemu
contributors.

xemu is based on [XQEMU](https://github.com/xqemu/xqemu) and
[QEMU](https://www.qemu.org/). QEMU was originally created by **Fabrice
Bellard** and is maintained by the QEMU project and its contributors.

All upstream copyright and per-file license notices are to be retained when
upstream files are modified or code is moved from them.

### NV2A graphics lineage

The NV2A implementation extended by this fork includes work by, among others:

- **espes**;
- **Jannik Vogel**;
- **Matt Borgerson**;
- other XQEMU and xemu contributors.

The OpenGL and Vulkan renderer files in this lineage are generally
LGPL-2.1-or-later as stated in their own headers. The feature-owned GL/Vulkan
texture-pack adapters retain that lineage and are explicitly marked
LGPL-2.1-or-later.

### MCPX APU lineage

The MCPX Audio Processing Unit implementation used by the Audio Packs and Fast
Forward integration includes work by:

- **espes**;
- **Jannik Vogel**;
- **Matt Borgerson**;
- other xemu/XQEMU contributors.

The feature-owned APU audio-pack bridge contains code derived from the MCPX
voice-processing implementation and therefore retains LGPL-2.1-or-later
licensing and upstream attribution in its header.

### Guest-memory helper lineage

`xemu-features/shared/guest-memory.*` contains functionality moved from xemu XBE
helpers. The retained source notices credit **Matt Borgerson** for the original
xemu virtual-to-physical helper work.

## Cheat-engine source lineage

The native C++ cheat interpreter/parser in `xemu-features/cheats/` is a port of
Joshua-1248's earlier Python project:

- **Xemu Cheat Engine and Trainer** —
  <https://github.com/Joshua-1248/Xemu-Cheat-Engine-and-Trainer>
- source modules used as the principal behavioral/implementation reference:
  `xemu_trainer_lib/codes.py` and `xemu_trainer_lib/cheatfiles.py`;
- source-project license: **MIT**.

The Python project is not bundled with this xemu fork. The native C++ files are
distributed here under GPL-2.0-or-later, while the MIT source provenance and
notice for the earlier implementation are retained in
`THIRD_PARTY_NOTICES.md` and `licenses/xemu_trainer_lib.license.txt`.

## FATX helper lineage

`ui/thirdparty/fatx/fatx.c` contains FATX image-creation definitions derived
from **libfatx**, part of Matt Borgerson's FATX project:
<https://github.com/mborgerson/fatx>. The source project is
GPL-2.0-or-later. The xemu helper now carries an explicit provenance comment
and GPL-2.0-or-later SPDX identifier instead of only the historical
`This is from libfatx` comment.

## SDL3

Upstream xemu uses **SDL3 / Simple DirectMedia Layer** for host platform,
windowing, input, audio, and related integration. SDL3 is licensed under the
**zlib license** and credits **Sam Lantinga and SDL contributors**. Its retained
license is `licenses/SDL3.license.txt` and the SDL source distribution also
contains its own `LICENSE.txt`.

## Third-party libraries and tools

The fork uses or exposes optional integration with the following components.
Their own licenses remain controlling for those components.

| Component | Use in this fork | License / notice |
| --- | --- | --- |
| [stb_image](https://github.com/nothings/stb) | PNG/GIF replacement decoding | MIT or Public Domain; upstream xemu copy retains its notice |
| [stb_image_write](https://github.com/nothings/stb) | PNG texture dumping | MIT or Public Domain; bundled header retains its notice |
| [libwebp](https://developers.google.com/speed/webp) | WebP still/animated replacement decoding | BSD-3-Clause + retained WebM patent grant; system library, not bundled by this fork |
| [Capstone](https://www.capstone-engine.org/) | x86 disassembly | BSD-3-Clause; system library, not bundled by this fork |
| [Xemu Cheat Engine and Trainer](https://github.com/Joshua-1248/Xemu-Cheat-Engine-and-Trainer) | Source/reference implementation for native cheat engine | MIT; source project not bundled |
| [libfatx](https://github.com/mborgerson/fatx) | Source provenance for FATX image helper | GPL-2.0-or-later |
| [glslang](https://github.com/KhronosGroup/glslang) | GLSL-to-SPIR-V compilation used by Vulkan shader replacements | Upstream subproject; see `licenses/glslang.license.txt` |
| [SDL3](https://github.com/libsdl-org/SDL) | Host windowing/input/audio/platform infrastructure used by xemu and fork hooks | zlib; see `licenses/SDL3.license.txt` |
| [RenderDoc](https://renderdoc.org/) in-application API | Graphics capture API header compiled into xemu integration | MIT; Baldur Karlsson; see `licenses/renderdoc.license.txt` |
| gloffscreen | Offscreen OpenGL abstraction used by NV2A | MIT; Intel/Collabora/Wayo/Matt Borgerson notices; see `licenses/gloffscreen.license.txt` |
| Lua interpreters | Optional scripting-console runtime | External system executable; not bundled by this fork |
| Python interpreters | Optional scripting-console runtime | External system executable; not bundled by this fork |

The repository contains many additional dependencies inherited from upstream
xemu/QEMU. Their notices are preserved under `licenses/`, their source trees,
and/or their individual file headers.

## User-authored content

Texture packs, replacement images, replacement WAV files, `.shader` files,
cheat/patch files, TAS movies, scripts, and other user-created data are not
relicensed by this repository. Their authors retain their own rights and remain
responsible for the rights to content they distribute.

## Fork relationship

This fork is not affiliated with or endorsed by the upstream xemu project.
Fork-specific issues should be reported to this repository unless they can be
reproduced on an unmodified upstream xemu build.
