# Credits

## This fork

Custom-fork maintenance and feature development by **Joshua-1248**
(<https://github.com/Joshua-1248/xemu>), including texture/audio replacement
and dumping, material enhancements, cheat/patch tooling, debugger/memory tools,
TAS/scripting, Fast Forward and presentation controls, Geometry Dumper,
experimental Free Camera, Disc Files & Mods/XDVDFS tooling, and standard
read-only Xbox DVD CHDv5 support.

## Upstream

This is a fork of [xemu](https://github.com/xemu-project/xemu), an original Xbox
emulator by **Matt Borgerson** and contributors, which is itself based on
[XQEMU](https://github.com/xqemu/xqemu) and
[QEMU](https://www.qemu.org/).

The NV2A graphics implementation this fork extends is the work of **espes**,
**Jannik Vogel**, **Matt Borgerson**, and other xemu and XQEMU contributors.

See `LICENSE` and `COPYING` for the licensing of the base project.

## Third-party libraries

Some features in this fork use host-provided libraries, while feature-owned
bundled dependencies are kept under `xemu-features/dependencies/` and retain
their own upstream licenses and notices.

Host-provided optional libraries currently include:

| Library | Used for | License |
| --- | --- | --- |
| [libwebp](https://developers.google.com/speed/webp) (`libwebp`, `libwebpdemux`) | Decoding `.webp` still and animated replacement textures | BSD-3-Clause |

Bundled custom-feature dependencies include:

| Library | Used for | License | Pinned version |
| --- | --- | --- | --- |
| [Capstone](https://www.capstone-engine.org/) | x86 disassembly | BSD-3-Clause | 5.0.9 |
| [libchdr](https://github.com/rtissera/libchdr) | CHDv5 DVD decompression | BSD-3-Clause | 0.3.0 |

The following are vendored in-tree and used by this fork's texture code.
`stb_image.h` ships with upstream xemu; `stb_image_write.h` is added by this
fork, and retains its own dual MIT / public-domain licence notice in the file:

| Library | Used for | License | Origin |
| --- | --- | --- | --- |
| [stb_image](https://github.com/nothings/stb) | Decoding `.png` and `.gif` replacement textures | MIT or Public Domain | upstream xemu |
| [stb_image_write](https://github.com/nothings/stb) | Writing dumped textures as `.png` | MIT or Public Domain | added by this fork |
| [glslang](https://github.com/KhronosGroup/glslang) | Compiling `.shader` files to SPIR-V (Vulkan backend) | BSD-3-Clause and others |

## Licensing of changes

This fork is distributed under the same terms as upstream xemu. Modifications to
existing files retain the license of the file they are made in:

- `hw/xbox/nv2a/pgraph/gl/texture.c`, `hw/xbox/nv2a/pgraph/gl/renderer.h`,
  `hw/xbox/nv2a/pgraph/vk/texture.c`, `hw/xbox/nv2a/pgraph/vk/renderer.h` —
  GNU Lesser General Public License, version 2.1 or later
- `hw/xbox/nv2a/pgraph/gl/texture-io.c`, `hw/xbox/nv2a/pgraph/gl/texture-io.h`
  (new files) — GNU General Public License, version 2 or later

Texture packs, replacement images, and `.shader` files are user-authored data
and are not covered by this project's license. Their authors retain their own
rights in them.

<!-- BEGIN JOSHUA-1248 CUSTOM FORK CREDITS -->

---

## Joshua-1248 custom fork additions

This section supplements the existing project credits. It does not replace
upstream authorship, Git history, per-file copyright notices, or third-party
attribution already present in the repository.

### Fork maintenance and integration

**Joshua-1248**  
https://github.com/Joshua-1248

Custom-fork maintenance, integration, testing, publication, feature direction,
and game-by-game validation.

The expanded custom feature work includes areas such as:

- texture dumping/replacement and animated replacements
- procedural texture shaders
- normal/specular/AO/displacement material maps
- camera-reactive material lighting
- geometry dumping and DCC-oriented export support
- audio dumping and replacement
- packetized/SSL stream replacement
- resident/ring-buffer replacement
- consumed-source-window matching
- logical intro/loop/outro audio extraction
- cheat and runtime-patch tooling
- debugger/disassembler expansion
- physical breakpoints/watchpoints and memory tools
- function indexing
- Fast Forward enhancements
- TAS / TAStudio tooling
- Python/Lua scripting APIs and overlays
- detachable custom tool windows
- experimental Free Camera functionality
- custom hotkey/notification/UI improvements
- feature-wide optimization and correctness work
- XDVDFS/disc-file modding and filesystem-browser development
- standard CHDv5 Xbox DVD support and libchdr/QEMU block integration
- CHD-backed extraction and per-title disc-file overrides
- feature-owned third-party dependency integration and portability work

### Development assistance

**OpenAI ChatGPT** has been used as a development assistant for portions of
feature design, implementation, debugging, code review, optimization,
documentation, and validation planning.

The fork maintainer performs the final integration decisions, local
compilation/runtime testing, repository maintenance, and publication.

### Upstream projects and contributors

The custom fork depends fundamentally on the work of the upstream projects and
their contributors:

- **xemu — Original Xbox Emulator**  
  https://github.com/xemu-project/xemu  
  https://xemu.app/

- **XQEMU**  
  https://github.com/xqemu/xqemu

- **QEMU**  
  https://www.qemu.org/  
  https://gitlab.com/qemu-project/qemu

Existing Git history, source headers, `MAINTAINERS`, and upstream documentation
remain the authoritative detailed authorship record.

The custom fork does not claim authorship of inherited Xbox hardware
emulation, NV2A emulation, MCPX/APU emulation, QEMU infrastructure, UI/platform
infrastructure, or other upstream work.

### Third-party components relevant to custom features

Custom features use or interface with existing in-tree or optional components
such as:

- **Capstone 5.0.9** — bundled x86 disassembly engine under
  `xemu-features/dependencies/capstone/upstream/`; official Capstone source,
  BSD-licensed, with custom build integration kept outside the upstream tree
- **libchdr 0.3.0** — pinned CHD decoder under
  `xemu-features/dependencies/libchdr/upstream/`; BSD-licensed upstream source,
  with the QEMU/Xemu adapter kept in `xemu-features/chd/`
- **LZMA SDK 25.01** — public-domain decoder source embedded in the pinned
  libchdr dependency and used by the CHD decoder
- **dr_flac / dr_libs** — embedded libchdr FLAC decoder, offered upstream as
  Public Domain or MIT No Attribution
- **miniz 3.1.1** — retained in the pinned libchdr source distribution under
  its MIT notice; Xemu's CHD build uses QEMU/system zlib instead
- **zstd 1.5.7 fallback decoder** — retained in the pinned libchdr source;
  dual BSD/GPLv2 upstream terms, compiled only when QEMU has no zstd dependency
- **libwebp** — WebP texture replacement/animation support
- **stb_image / stb_image_write** — image decode/write paths
- **xxHash / XXH3** — source identities and fingerprints used by asset tooling
- **glslang** and existing renderer/shader infrastructure where applicable

Each component's own copyright and license notice remains authoritative.
Nothing in this section changes those licenses.

### User-created packs and scripts

Texture packs, audio packs, replacement images/audio, material maps,
procedural `.shader` files, Lua/Python scripts, and other user-created mod data
are separate from the emulator source. Their authors retain rights to their
original work and are responsible for the rights attached to source material
they use.

### Preservation of attribution

When modifying this fork, do not remove existing upstream or third-party
copyright/license notices. Moving, wrapping, or extending code does not erase
the authorship or licensing of the original work.

See [`NOTICE.md`](NOTICE.md) for the fork-specific provenance and licensing
notice.

<!-- END JOSHUA-1248 CUSTOM FORK CREDITS -->
