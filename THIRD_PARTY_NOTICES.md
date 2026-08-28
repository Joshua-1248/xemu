# Third-party notices and provenance

This file is an attribution/provenance index for the Joshua-1248 xemu fork. It
is not a substitute for the full license texts or copyright notices in the
repository. When this file and an individual source-file notice differ, the
source-file notice and applicable license text control.

## Base-project licensing

This repository is derived from:

1. **xemu** — <https://github.com/xemu-project/xemu>
2. **XQEMU** — <https://github.com/xqemu/xqemu>
3. **QEMU** — <https://www.qemu.org/>

The repository preserves upstream `LICENSE`, `COPYING`, and `COPYING.LIB`.
QEMU's root `LICENSE` explains that the emulator as a whole is GPL version 2,
while individual source files may carry compatible licenses of their own. It
also specifies that source files with no licensing information are GPL version
2 or, at the recipient's option, any later version.

Do not remove or replace upstream per-file copyright/license notices when
modifying or moving upstream code.

## Upstream-derived feature adapters

The Phase 4 isolation work moved fork-specific functionality into
`xemu-features/`. Some feature-owned adapters necessarily contain or are derived
from code in upstream LGPL renderer/APU files. Those files carry explicit
LGPL-2.1-or-later SPDX identifiers and preserve the relevant lineage:

| Fork path | Upstream lineage | License |
| --- | --- | --- |
| `xemu-features/audio-packs/audio-packs-apu.c` | `hw/xbox/mcpx/apu/vp/vp.c`; espes, Jannik Vogel, Matt Borgerson, contributors | LGPL-2.1-or-later |
| `xemu-features/audio-packs/audio-packs-apu.h` | MCPX/APU feature boundary associated with the above bridge | LGPL-2.1-or-later |
| `xemu-features/texture-packs/texture-packs-gl.c` | NV2A OpenGL renderer lineage; espes, Jannik Vogel, Matt Borgerson, contributors | LGPL-2.1-or-later |
| `xemu-features/texture-packs/texture-packs-gl.h` | NV2A OpenGL feature boundary | LGPL-2.1-or-later |
| `xemu-features/texture-packs/texture-packs-vk.c` | NV2A Vulkan/OpenGL renderer lineage; Matt Borgerson, espes, Jannik Vogel, contributors | LGPL-2.1-or-later |
| `xemu-features/texture-packs/texture-packs-vk.h` | NV2A Vulkan feature boundary | LGPL-2.1-or-later |

Other fork-specific `xemu-features/` C/C++ source files are explicitly marked
GPL-2.0-or-later unless an individual file states otherwise. Existing upstream
files outside `xemu-features/` keep their existing license headers regardless of
which fork feature calls them.

## stb_image and stb_image_write

Project: <https://github.com/nothings/stb>

- `stb_image.h` is already present in upstream xemu.
- `stb_image_write.h` is used by this fork for PNG texture dumping.
- The bundled headers retain their original stb notices.
- stb publishes these single-header libraries under its dual public-domain / MIT
  terms as stated in the headers/project.

No upstream stb notice should be removed from those files.

## libwebp

Project: <https://developers.google.com/speed/webp>

Use: still and animated WebP texture replacement decoding.

License: BSD-3-Clause.

The fork does not vendor libwebp; it links to a system-provided copy when WebP
support is enabled.

## Capstone

Project: <https://www.capstone-engine.org/>

Use: guest x86 disassembly.

License: BSD-3-Clause.

The fork does not vendor Capstone; it links to a system-provided copy when the
disassembler is enabled/available.

## glslang

Project: <https://github.com/KhronosGroup/glslang>

Use: GLSL-to-SPIR-V compilation for Vulkan procedural texture shaders.

This is inherited through the xemu build/subproject structure. Preserve the
upstream notices and `licenses/glslang.license.txt`.

## SDL and other inherited dependencies

The fork calls the same SDL and other host libraries already used by upstream
xemu. The fork does not replace their licenses. See the existing `licenses/`
directory and dependency source trees for the complete inherited dependency
inventory.

## Lua and Python interpreters

The scripting consoles launch an installed system Lua or Python interpreter.
No Lua or Python runtime is bundled by this fork. Users/distributors remain
subject to the license of the interpreter they install separately.

## `xemu_trainer_lib` development reference

The cheat interpreter/parser comments refer to a Python development reference
named `xemu_trainer_lib` (`codes.py` and `cheatfiles.py`). The Python package is
not included in this repository and is not a runtime/build dependency.

No separate public upstream URL or third-party license for that Python reference
is recorded in the source materials currently present in the fork. The C++
implementation is distributed under the GPL-2.0-or-later notices in its own
files. If the Python reference is later identified as originating from a
separate external project, add that project's copyright, URL, and license here
and in `CREDITS.md` before redistributing the external Python source.

## User-provided game/mod assets

This repository does not grant rights to Xbox game assets or other copyrighted
material supplied by users. Texture/audio packs, shaders, TAS movies, scripts,
and cheat data remain separate user content and must be distributed only when
the distributor has the necessary rights.
