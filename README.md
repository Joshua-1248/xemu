# xemu — Joshua-1248 fork

A fork of [xemu](https://github.com/xemu-project/xemu), the Original Xbox
emulator, adding texture replacement, procedural texture shaders, a cheat and
patch engine, and an in-emulator disassembler.

For the emulator itself — what it is, how to set up a BIOS and hard disk image,
compatibility — see **[xemu.app](https://xemu.app)**. This README covers only
what this fork adds.

---

## Features

### Texture dumping and replacement

Dump the textures a game uses, edit them, and have the emulator load your
versions in their place. Works on both the OpenGL and Vulkan backends.

Textures are matched by a content hash, so a replacement follows the texture
wherever it appears, and files are per-title so packs for different games do
not collide.

| Key | Action |
| --- | --- |
| `,` | Toggle texture replacements on/off |
| `.` | Reload replacements from disk |
| `/` | Toggle texture dumping on/off |

Files live under:

```
~/.local/share/xemu/xemu/textures/<TITLEID>/dumps/
~/.local/share/xemu/xemu/textures/<TITLEID>/replacements/
```

Both directories can be pointed elsewhere in **Settings → General**. Subfolders
inside the replacement directory are scanned, so a pack can be organised however
you like.

Replacement files are named by the texture's hash:

```
<16 hex digits>.png            plain 2D texture
<16 hex digits>_posx.png       cubemap face (posx negx posy negy posz negz)
<16 hex digits>_mip1.png       explicit mip level
```

`.png`, `.gif` and `.webp` are accepted. When several files share a hash, `.webp`
wins, then `.gif`, then `.png`.

### Animated textures

`.gif` and `.webp` replacements animate in place at their own frame rate. WebP
is preferred where you have the choice — it is lossless with full alpha, where
GIF is stuck with a 256-colour palette and 1-bit transparency.

Animated WebP requires the emulator to have been built with libwebp. If it was
not, `.webp` files are ignored and a message says so on first use; `.gif` still
works, since it is decoded by the bundled stb_image and needs no library.

### Procedural texture shaders

A texture can be driven by a GLSL fragment shader instead of an image. Drop a
`<hash>.shader` file next to the replacements and the emulator renders it to
that texture every frame.

Available uniforms:

| Uniform | Meaning |
| --- | --- |
| `iTime` | Seconds since start, as a float |
| `iResolution` | Target texture size in pixels |
| `iFrame` | Frame counter |
| `iChannel0` | Sampler for the image replacement, if one exists |
| `iHasChannel0` | Whether `iChannel0` is bound |

If a hash has both a `.shader` and an image, the image becomes `iChannel0` and
the shader output becomes the texture — so a shader can distort, animate or
tint an existing replacement rather than generating everything from scratch.

Shaders are re-read from disk while the emulator runs, so you can edit and see
the result without restarting.

### Cheats and patches

**Settings → Codes.** Codes are grouped into cheats (toggled while playing) and
patches (applied once), both in a plain-text format you can edit by hand.

```
~/.local/share/xemu/xemu/codes/cheats/<SERIAL_TITLEID>.txt
~/.local/share/xemu/xemu/codes/patches/<SERIAL_TITLEID>.txt
```

The filename stem is derived arithmetically from the XBE title id, so no
external game database is needed to work out which file belongs to which game.

The interpreter is a port of an existing Python implementation and was verified
against it differentially: 250 fuzz seeds, 39 directed edge cases, all 736
games in the reference database, and all 1,472 parser files, clean under
AddressSanitizer and UndefinedBehaviorSanitizer.

### Disassembler and debugger

**Debug → Disassembler.** x86 disassembly of guest code with registers,
breakpoints, watchpoints, single-stepping, and a memory viewer that can be
addressed in either the virtual or physical address space.

Requires capstone at build time. Without it the emulator still builds and runs;
the window reports that disassembly is unavailable.

> The memory viewer and the Step Over / Step Out / Run-to-cursor buttons are
> lightly tested. Step Out reads the return address from `[ebp+4]`, so it will
> report an error on code compiled without a frame pointer — that is expected
> rather than a fault.

### Fixes

- **Save-state freeze.** Loading a save state could leave the machine running
  but not advancing, needing a second load to unstick. Restores
  `qemu_mutex_lock_main_loop()` in `xemu_main_loop_lock()`, without which the
  UI thread could run a snapshot load while the main loop was mid-iteration.
  This is an upstream bug, not one specific to this fork.
- **Controllers with identical names.** Adapters exposing several pads under
  one USB identity — dual PS2-to-USB adapters, for instance — no longer swap
  player assignments between sessions. Bindings now key on the device path as
  well as the GUID.
- **Settings lost on a crash.** Settings are written when changed rather than
  only at a clean exit.
- **PCI bus reset crash** on reset with pending interrupts.

---

## Building

Same as upstream — see the
[xemu build docs](https://xemu.app/docs/download/#building) — with two optional
extra packages.

```sh
sudo apt install libwebp-dev libcapstone-dev   # Debian/Ubuntu/Mint
brew install webp capstone                     # macOS
```

Both are optional. Without libwebp, `.webp` replacements are skipped. Without
capstone, the disassembler window reports no disassembly. Everything else works
either way, and the tree builds on Linux, macOS and Windows with neither
installed.

```sh
./build.sh
```

To turn the features on explicitly:

```sh
cd build
./pyvenv/bin/meson configure -Dcapstone=enabled -Dwebp=enabled
ninja && cp qemu-system-i386 ../dist/xemu
```

---

## Configuration

Beyond the UI, these keys are available in the config file:

| Key | Default | Meaning |
| --- | --- | --- |
| `general.texture_dump_enabled` | `false` | Dump textures as they are used |
| `general.texture_replace_enabled` | `false` | Load replacements |
| `general.texture_dump_skip_replaced` | `true` | Don't re-dump replaced textures |
| `general.texture_dump_mipmaps` | `false` | Dump mip levels as separate files |
| `general.texture_dump_dir` | *(empty)* | Override the dump directory |
| `general.texture_replace_dir` | *(empty)* | Override the replacement directory |
| `codes.enable` | `true` | Master switch for cheats and patches |
| `codes.cheats_dir` | *(empty)* | Override the cheats directory |
| `codes.patches_dir` | *(empty)* | Override the patches directory |
| `codes.interval_ms` | `16` | How often enabled codes are re-applied |

The three hotkeys are configurable as `general.texture_*_key`, using ImGui key
codes.

---

## Licensing

This fork is distributed under the same terms as upstream xemu, which is itself
based on [QEMU](https://www.qemu.org/): the emulator as a whole is under the
**GNU General Public License, version 2**. See `LICENSE`, `COPYING` and
`COPYING.LIB` in this repository.

Each source file carries its own licensing information. Files added by this
fork are GPL-2.0-or-later. Modifications to existing files keep the licence of
the file they are made in — the NV2A renderer files this fork extends are
LGPL-2.1-or-later.

Third-party code:

| Component | Use | Licence |
| --- | --- | --- |
| [stb_image](https://github.com/nothings/stb) | Decoding PNG and GIF replacements | MIT or Public Domain |
| [stb_image_write](https://github.com/nothings/stb) | Writing dumped textures as PNG | MIT or Public Domain |
| [libwebp](https://developers.google.com/speed/webp) | Decoding WebP replacements | BSD-3-Clause |
| [capstone](https://www.capstone-engine.org/) | x86 disassembly | BSD-3-Clause |

`stb_image.h` ships with upstream xemu; `stb_image_write.h` is added by this
fork and keeps its own licence notice in the file. libwebp and capstone are not
bundled — they are linked from the system if present.

Texture packs, replacement images and `.shader` files are user-authored data
and are not covered by this project's licence. Their authors keep their own
rights in them.

See `CREDITS.md` for attribution.

---

## Credits

xemu is by **Matt Borgerson** and contributors, built on
[XQEMU](https://github.com/xqemu/xqemu) and [QEMU](https://www.qemu.org/). The
NV2A graphics implementation this fork extends is the work of **espes**,
**Jannik Vogel**, **Matt Borgerson**, and other xemu and XQEMU contributors.

This fork is not affiliated with the xemu project. Please report issues here
rather than to upstream xemu, unless you can reproduce them on an unmodified
upstream build.
