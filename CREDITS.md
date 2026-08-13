# Credits

## This fork

Texture dumping, texture replacement, animated texture replacement, procedural
texture shaders, cheat/patch engine, and disassembler/debugger by
**Joshua-1248** (<https://github.com/Joshua-1248/xemu>).

## Upstream

This is a fork of [xemu](https://github.com/xemu-project/xemu), an original Xbox
emulator by **Matt Borgerson** and contributors, which is itself based on
[XQEMU](https://github.com/xqemu/xqemu) and
[QEMU](https://www.qemu.org/).

The NV2A graphics implementation this fork extends is the work of **espes**,
**Jannik Vogel**, **Matt Borgerson**, and other xemu and XQEMU contributors.

See `LICENSE` and `COPYING` for the licensing of the base project.

## Third-party libraries

Features in this fork depend on the following libraries, which are **not**
bundled and must be installed to build:

| Library | Used for | License |
| --- | --- | --- |
| [libwebp](https://developers.google.com/speed/webp) (`libwebp`, `libwebpdemux`) | Decoding `.webp` still and animated replacement textures | BSD-3-Clause |

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
