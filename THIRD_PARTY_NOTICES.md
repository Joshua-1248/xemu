# Third-party notices and provenance

This file is an attribution/provenance index for the Joshua-1248 xemu fork. It
supplements, and does not replace, the full license texts, copyright notices,
SPDX identifiers, Git history, or notices retained beside individual source
files. When a summary here differs from an authoritative source-file or license
notice, the authoritative notice controls.

## Base-project lineage

This repository is derived from:

1. **xemu** — <https://github.com/xemu-project/xemu>
2. **XQEMU** — <https://github.com/xqemu/xqemu>
3. **QEMU** — <https://www.qemu.org/>

The repository preserves upstream `LICENSE`, `COPYING`, and `COPYING.LIB`.
QEMU's root `LICENSE` explains that the emulator as a whole is GPL version 2,
while individual source files may carry other GPL-compatible licenses. Source
files with no licensing information fall back to GPL version 2 or, at the
recipient's option, any later version.

Do not remove or replace upstream copyright/license notices when modifying or
moving upstream code.

## Fork-specific or fork-relevant provenance

### Xemu Cheat Engine and Trainer / `xemu_trainer_lib`

Project: <https://github.com/Joshua-1248/Xemu-Cheat-Engine-and-Trainer>

License: **MIT**.

The native C++ cheat parser/interpreter in `xemu-features/cheats/` was ported
from and validated against the earlier Python implementation, principally:

- `xemu_trainer_lib/codes.py`;
- `xemu_trainer_lib/cheatfiles.py`.

The Python project is not bundled with this xemu fork. The native C++ files are
distributed here under GPL-2.0-or-later while retaining the source-project MIT
provenance and notice. See `licenses/xemu_trainer_lib.license.txt`.

### libfatx / FATX helper

Project: <https://github.com/mborgerson/fatx>

License: **GPL-2.0-or-later**.

`ui/thirdparty/fatx/fatx.c` contains FATX image-creation definitions derived
from libfatx. The helper now carries an explicit provenance notice and
GPL-2.0-or-later SPDX identifier. The GPLv2 text is already distributed in this
repository as `COPYING`.

### SDL3 / Simple DirectMedia Layer

Project: <https://github.com/libsdl-org/SDL>

License: **zlib**.

SDL3 is inherited from upstream xemu and provides host windowing, input, audio,
and related platform integration. The retained license is
`licenses/SDL3.license.txt`; the SDL source distribution also carries its own
`LICENSE.txt`. Do not relabel SDL3 source as GPL merely because xemu as a whole
is GPL-licensed.

### stb_image / stb_image_write

Project: <https://github.com/nothings/stb>

License: **MIT or Public Domain**, as stated by stb.

- `stb_image.h` is inherited from upstream xemu.
- `stb_image_write.h` is used by the fork for PNG texture dumping.
- Both bundled headers retain their original notices.
- `licenses/stb_image_write.license.txt` mirrors the retained stb MIT notice for
  outbound-license generation.

### libwebp

Project: <https://developers.google.com/speed/webp>

License: **BSD-3-Clause**, with the additional WebM patent grant retained with the notice.

Use: still and animated WebP texture replacement decoding. The fork normally
links to a system-provided copy rather than vendoring libwebp. If a distributor
ships or statically links libwebp, its notice must accompany that build. A copy
of the BSD notice used for outbound-license generation is retained as
`licenses/libwebp.license.txt`.

### Capstone

Project: <https://www.capstone-engine.org/>

License: **BSD-3-Clause**.

Use: guest x86 disassembly. The fork normally links to a system-provided copy.
If a distributor ships or statically links Capstone, its notice must accompany
that build. A retained BSD notice is provided as `licenses/capstone.license.txt`.

### Upstream-derived feature adapters

The Phase 4 isolation work moved fork-specific functionality into
`xemu-features/`. Some feature-owned adapters contain or are materially derived
from upstream LGPL renderer/APU code and therefore retain that lineage:

| Fork path | Upstream lineage | License |
| --- | --- | --- |
| `xemu-features/audio-packs/audio-packs-apu.c` | MCPX/APU voice processing; espes, Jannik Vogel, Matt Borgerson, contributors | LGPL-2.1-or-later |
| `xemu-features/audio-packs/audio-packs-apu.h` | MCPX/APU bridge boundary | LGPL-2.1-or-later |
| `xemu-features/texture-packs/texture-packs-gl.c` | NV2A OpenGL renderer lineage | LGPL-2.1-or-later |
| `xemu-features/texture-packs/texture-packs-gl.h` | NV2A OpenGL bridge boundary | LGPL-2.1-or-later |
| `xemu-features/texture-packs/texture-packs-vk.c` | NV2A Vulkan/OpenGL renderer lineage | LGPL-2.1-or-later |
| `xemu-features/texture-packs/texture-packs-vk.h` | NV2A Vulkan bridge boundary | LGPL-2.1-or-later |

Other fork-owned `xemu-features/` C/C++ source is normally marked
GPL-2.0-or-later unless a file states otherwise. Existing upstream files outside
`xemu-features/` keep their existing licenses regardless of which fork feature
calls them.

## Source-level third-party material and retained in-tree notices

The following material retains notices in its own source tree and must not have
those notices removed:

| Component | Location | License / notice |
| --- | --- | --- |
| RenderDoc in-application API header | `thirdparty/renderdoc_app.h` | MIT; copyright Baldur Karlsson; notice retained in header and `licenses/renderdoc.license.txt` |
| NVIDIA NVAPI headers | `thirdparty/nvapi/` | MIT; see `thirdparty/nvapi/nvapi_defs.LICENSE.txt` |
| gloffscreen | `hw/xbox/nv2a/pgraph/thirdparty/gloffscreen/` | MIT notices retained in source and `licenses/gloffscreen.license.txt`; Intel, Collabora contributors, Wayo, Matt Borgerson |
| Berkeley SoftFloat 3e | `subprojects/berkeley-softfloat-3/` | BSD-3-Clause; see `COPYING.txt`; test-only subproject usage in this tree |
| Berkeley TestFloat 3e | `subprojects/berkeley-testfloat-3/` | BSD-3-Clause; see `COPYING.txt`; test-only subproject usage in this tree |
| genconfig | `subprojects/genconfig/` | MIT; copyright Matt Borgerson; linked into xemu |
| nlohmann/json | `subprojects/json-*` | MIT; copyright Niels Lohmann; header-only code compiled into xemu |
| keycodemapdb | `subprojects/keycodemapdb/` | Dual GPL-2.0-or-later / BSD-3-Clause; generated keymap output may use either; xemu uses BSD-compatible generated output |

## Retained `licenses/` inventory

The repository already contains a broad inherited license inventory. The
license-family column below is a navigation summary only; the named file is the
authoritative retained text, including any component-specific exceptions or
multi-license terms.

| Component | Retained notice | License family / summary |
| --- | --- | --- |
| SDL3 | `licenses/SDL3.license.txt` | zlib |
| SPIRV-Reflect | `licenses/SPIRV-Reflect.license.txt` | Apache-2.0 |
| SPIRV-Tools | `licenses/SPIRV-Tools.license.txt` | Apache-2.0 |
| VulkanMemoryAllocator | `licenses/VulkanMemoryAllocator.license.txt` | MIT |
| dsp56300 | `licenses/dsp56300.license.txt` | MIT plus retained dependency notices |
| Font Awesome | `licenses/fontawesome.license.txt` | Multi-license; read retained file |
| fpng | `licenses/fpng.license.txt` | Unlicense / public-domain dedication |
| gettext runtime | `licenses/gettext.license.txt` | LGPL-2.1 |
| GLib | `licenses/glib-2.0.license.txt` | LGPL-2.1 |
| glslang | `licenses/glslang.license.txt` | Multi-notice/BSD-compatible; read retained file |
| GTK | `licenses/gtk.license.txt` | GNU Library/Lesser GPL terms in retained file |
| GNU libiconv | `licenses/iconv.license.txt` | LGPL-2.1 |
| Dear ImGui | `licenses/imgui.license.txt` | MIT |
| ImPlot | `licenses/implot.license.txt` | MIT |
| inih | `licenses/inih.license.txt` | BSD-3-Clause |
| libcurl | `licenses/libcurl.license.txt` | curl license / MIT-style |
| libepoxy | `licenses/libepoxy.license.txt` | MIT plus generated-material notices |
| MinGW-w64 runtime | `licenses/libmingw32.license.txt` | Multi-notice; read retained file |
| libsamplerate | `licenses/libsamplerate.license.txt` | BSD-2-Clause |
| miniz | `licenses/miniz.license.txt` | MIT |
| noc_file_dialog | `licenses/noc.license.txt` | MIT |
| nv2a_vsh_cpu | `licenses/nv2a_vsh_cpu.license.txt` | Unlicense / public-domain dedication |
| PCRE | `licenses/pcre.license.txt` | BSD-style; read retained file |
| PCRE2 | `licenses/pcre2.license.txt` | BSD-style with retained exception text |
| pixman | `licenses/pixman.license.txt` | MIT-family / retained contributor notices |
| QEMU | `licenses/qemu.license.txt` | GPL-2.0 overall structure; per-file compatible licenses |
| Roboto | `licenses/roboto.license.txt` | Apache-2.0 |
| libslirp | `licenses/slirp.license.txt` | BSD-3-Clause |
| stb_image | `licenses/stb_image.license.txt` | MIT notice |
| toml++ | `licenses/tomlplusplus.license.txt` | MIT |
| volk | `licenses/volk.license.txt` | MIT |
| xxHash | `licenses/xxHash.license.txt` | BSD-2-Clause |
| zlib | `licenses/zlib.license.txt` | zlib |
| stb_image_write | `licenses/stb_image_write.license.txt` | MIT notice / stb dual terms |
| libwebp | `licenses/libwebp.license.txt` | BSD-3-Clause plus retained WebM patent grant |
| Capstone | `licenses/capstone.license.txt` | BSD-3-Clause |
| Xemu Cheat Engine and Trainer / `xemu_trainer_lib` | `licenses/xemu_trainer_lib.license.txt` | MIT |
| RenderDoc in-application API | `licenses/renderdoc.license.txt` | MIT |
| gloffscreen | `licenses/gloffscreen.license.txt` | MIT |

## Generated outbound license bundle

`scripts/gen-license.py` is used by xemu packaging to generate an outbound
`LICENSE.txt` for binary distributions. This fork updates that generator to:

- read the QEMU version from `QEMU_VERSION` instead of carrying the stale
  hard-coded `6.0.0` value;
- derive the SDL3 version from `subprojects/sdl3.wrap` rather than hard-coding
  a version that silently becomes stale;
- derive bundled stb_image/stb_image_write versions from their headers instead of carrying stale manual values;
- include `stb_image_write`, nlohmann/json, genconfig, keycodemapdb, RenderDoc API, gloffscreen, and the MIT trainer-source provenance notice;
- include libwebp and Capstone when those optional libraries are present on a
  platform where the build ships them;
- fix the fallback license-cache write path to use `self.license_path` instead
  of an undefined variable.

The generated bundle is supplemental to the source-tree notices and does not
supersede them.

## Lua and Python interpreters

The scripting consoles launch installed system Lua or Python interpreters. No
Lua or Python runtime is bundled by this fork, so their own runtime licenses are
not copied into the xemu repository merely because a user has them installed.

## User-provided game/mod assets

This repository does not grant rights to Xbox game assets or other copyrighted
material supplied by users. Texture/audio packs, replacement images, shaders,
TAS movies, scripts, and cheat data remain separate user content and must be
distributed only when the distributor has the necessary rights.
