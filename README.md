# xemu — Joshua-1248 fork

A feature-focused fork of [xemu](https://github.com/xemu-project/xemu), the Original Xbox
emulator, adding integrated modding, asset-replacement, debugging, memory,
cheat/patch, TAS, scripting, geometry/audio tooling, presentation controls, and
other development-oriented features while retaining upstream xemu as the base.

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

Both directories can be pointed elsewhere from **Misc → Texture Packs**. Subfolders
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

**Misc → Cheats/Patches.** Codes are grouped into cheats (toggled while playing) and
patches (applied once), both in a plain-text format you can edit by hand. The old
custom Settings → Codes page is intentionally hidden by the feature layer.

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

### Disc Files & Mods

**Misc → Disc Files & Mods.** Browse the mounted Xbox disc's XDVDFS filesystem
directly without extracting the XISO first. The browser shows directories,
files, sizes, type, sector/LBA information, and whether a per-title override is
present or active.

Files, directories, or the entire disc can be extracted to a chosen host
directory. Extraction runs in the background with progress/cancel support and
offers skip, overwrite, or auto-rename collision handling.

Per-title disc-file overrides mirror the original Xbox disc path under:

```text
<xemu data>/mods/<TITLEID>/disc/...
```

A custom mod-library root can be selected in the tool. The original XISO is
never rewritten or repacked: replacement data is exposed to the guest through
feature-owned virtual sectors while XDVDFS metadata is adjusted in memory.
Unsafe traversal/symlink escapes, ambiguous case collisions, and invalid
replacement files are rejected rather than guessed.

Changing replacement files requires **Reload Disc / Overrides**. Restarting the
title after a replacement change is safest because games may cache filesystem
metadata.

See [`xemu-features/disc-modding/README.md`](xemu-features/disc-modding/README.md).

### Disassembler and debugger

**Debug → Disassembler.** x86 disassembly of guest code with registers,
breakpoints, watchpoints, single-stepping, and a memory viewer that can be
addressed in either the virtual or physical address space.

Capstone 5.0.9 provides the x86 decoder used by the disassembler and is bundled
as feature-owned third-party source under
`xemu-features/dependencies/capstone/`. It is built as a private static x86-only
library on Linux, Windows, and macOS, so a normal build no longer requires a
separately installed Capstone package or runtime library.

> Step Out uses the conventional `[ebp+4]` return-address path when a frame
> pointer is available. Code compiled without a frame pointer may not support
> that operation reliably; the other debugger and memory tools remain usable.

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
[xemu build docs](https://xemu.app/docs/download/#building) — with optional
feature dependencies where applicable.

Capstone is bundled and does **not** need to be installed separately. libwebp
remains an optional host dependency for WebP texture replacement:

```sh
sudo apt install libwebp-dev   # Debian/Ubuntu/Mint
brew install webp              # macOS
```

Feature-owned bundled dependencies live under `xemu-features/dependencies/`.
Each keeps its own upstream license and provenance; bundling does not change the
license or authorship of a third-party component.

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

Disc-file override settings are kept by the feature in
`<xemu data>/disc-modding/settings.txt`. The default per-title mod tree is
`<xemu data>/mods/<TITLEID>/disc/`.

---

## Licensing

This fork is distributed under the same terms as upstream xemu, which is itself
based on [QEMU](https://www.qemu.org/): the emulator as a whole is under the
**GNU General Public License, version 2**. See `LICENSE`, `COPYING` and
`COPYING.LIB` in this repository.

Each source file carries its own licensing information. Files added by this
fork are GPL-2.0-or-later unless a more specific file notice applies.
Modifications to existing files keep the licence of the file they are made in —
for example, inherited renderer/APU code may remain LGPL-2.1-or-later.

Third-party code retains its own license and copyright notices regardless of
where it is located in this repository. A dependency placed under
`xemu-features/dependencies/` does **not** become fork-authored or GPL merely by
being stored there.

See `THIRD_PARTY_NOTICES.md`, `NOTICE.md`, `CREDITS.md`, `licenses/`, and the
individual source/component notices for attribution and redistribution terms.

Texture packs, replacement images, audio packs, scripts, dumps, and `.shader`
files are user-authored data and are not automatically covered by this project's
license. Their authors keep their own rights and remain responsible for the
rights to any third-party material they use.

---

## Credits

xemu is by **Matt Borgerson** and contributors, built on
[XQEMU](https://github.com/xqemu/xqemu) and [QEMU](https://www.qemu.org/). The
NV2A graphics implementation this fork extends is the work of **espes**,
**Jannik Vogel**, **Matt Borgerson**, and other xemu and XQEMU contributors.

This fork is not affiliated with the xemu project. Please report issues here
rather than to upstream xemu, unless you can reproduce them on an unmodified
upstream build.

<!-- BEGIN JOSHUA-1248 EXPANDED FEATURE SUITE -->

---

## Joshua-1248 custom fork — expanded feature suite

> **Unofficial community fork.** This repository remains based on the
> [upstream xemu project](https://github.com/xemu-project/xemu). The additions
> described below are custom-fork functionality and are not part of the
> official upstream xemu release unless separately merged upstream.

The custom work is intentionally concentrated under [`xemu-features/`](xemu-features/)
where practical. Small integration hooks outside that directory are kept
minimal when native renderer, APU, UI, or emulator access is required.

### Texture packs and advanced materials

The texture replacement system supports runtime dumping/replacement on the
supported renderer paths, including content-hash matching and live reload.

The optional material-enhancement path adds sidecar maps:

```text
<hash>_n.png    tangent-space normal map
<hash>_s.png    specular map/mask
<hash>_d.png    displacement/parallax height map
<hash>_ao.png   ambient-occlusion map
```

Material controls include normal strength, ambient/diffuse/specular response,
gloss, AO, parallax/displacement controls, normal-Y flipping, and
camera-reactive/directional lighting behavior.

See [`xemu-features/texture-packs/README.md`](xemu-features/texture-packs/README.md).

### Audio packs

Audio dumping/replacement is designed to be similarly approachable to a
texture pack: dump a source, keep its stable identity, provide a replacement
WAV, and reload replacements from the emulator.

The current matching/extraction paths cover multiple Xbox playback styles,
including:

- ordinary resident/static hardware voices
- packetized/SSL streams
- resident buffers that are refilled or reused
- software-managed/circular source buffers
- arbitrary-offset consumed-source matching
- PCM and Xbox ADPCM source handling
- replacement variants and live reload

The dumper also includes conservative logical-loop extraction. When exact
runtime evidence proves a loop, a recording can be reduced from repeated
runtime playback to the logical asset form:

```text
[intro] [one complete loop traversal] [outro]
```

Loop-capable dumps can store standard RIFF `smpl` loop metadata plus JSON
sidecar information describing loop points and extraction provenance.

See [`xemu-features/audio-packs/README.md`](xemu-features/audio-packs/README.md).

### Geometry Dumper

The Geometry Dumper captures rendered Xbox geometry for modding, asset
research, and reverse engineering.

The exporter keeps raw/native evidence separate from DCC-oriented conversion
where possible and includes material/texture/render-state information needed
to make exported scenes more useful outside the emulator.

See [`xemu-features/geometry-dumper/README.md`](xemu-features/geometry-dumper/README.md).

### Debugger, disassembler, and memory tools

The custom debugger tooling expands guest-code and guest-memory inspection with
features such as:

- x86 disassembly
- guest register inspection
- breakpoints and physical watchpoints
- memory viewing/editing
- stepping and run-to-cursor workflows
- stack/disassembly helpers
- function indexing
- debugger callbacks exposed to scripting

See [`xemu-features/debug-tools/README.md`](xemu-features/debug-tools/README.md).

### Cheats and runtime patches

The built-in custom cheat system provides per-game runtime cheats/patches,
editable code files, in-emulator controls, and integration with the custom
debugging layer.

See [`xemu-features/cheats/README.md`](xemu-features/cheats/README.md).

### Disc Files & Mods

The fork includes an XDVDFS filesystem browser/extractor and a per-title
disc-file override layer under
[`xemu-features/disc-modding/`](xemu-features/disc-modding/). It can inspect a
mounted XISO, extract one file/directory or the entire disc, and mirror
replacement files into a per-title mod tree without modifying the source image.

Overrides are resolved as Xbox-style case-insensitive paths and are exposed to
the guest through virtual sectors. Path traversal, symlink escapes, ambiguous
case collisions, non-regular files, and other unsafe mappings are rejected.

See [`xemu-features/disc-modding/README.md`](xemu-features/disc-modding/README.md).

### TAS / TAStudio

The fork includes tool-assisted-play functionality under
[`xemu-features/tas/`](xemu-features/tas/), including controller-input
recording/playback, timeline-oriented editing, controller-poll tracking, and
supporting TAS controls.

### Python/Lua scripting

The custom scripting layer is intended for game-specific automation,
debugging, overlays, and reverse-engineering tools.

Available functionality includes guest-memory access, memory watches,
controller automation/visualization, text and image overlays, lines,
rectangles/bars, debugger events, breakpoint/watchpoint callbacks, and
emulator-control helpers.

Example scripts are included under
[`xemu-features/scripting/examples/`](xemu-features/scripting/examples/).

See [`xemu-features/scripting/README.md`](xemu-features/scripting/README.md).

### Experimental Free Camera

An experimental game-agnostic free-camera framework is available under
[`xemu-features/freecam/`](xemu-features/freecam/). It contains multiple
fixed-function/programmed-vertex-path approaches and 6-DOF camera controls.

The default toggle is:

```text
F10
```

Because Xbox games can construct cameras and render scenes in very different
ways, compatibility is intentionally described as experimental.

See [`xemu-features/freecam/README.md`](xemu-features/freecam/README.md).

### Fast Forward and presentation controls

The fork extends Fast Forward with an unlimited mode and optional
pitch-preserving audio behavior. The separate Volume Amplifier feature extends
host output from the normal 0–100% range up to 200%. Additional custom UI/hotkey
work includes faster notifications, detachable tool-window infrastructure, and
related quality-of-life improvements.

See [`xemu-features/fast-forward/README.md`](xemu-features/fast-forward/README.md)
and [`xemu-features/volume-amplifier/README.md`](xemu-features/volume-amplifier/README.md).

### Custom feature layout

Current major feature directories include:

```text
xemu-features/
├── audio-packs/
├── cheats/
├── debug-tools/
├── dependencies/      feature-owned third-party dependencies
├── disc-modding/
├── fast-forward/
├── freecam/
├── geometry-dumper/
├── scripting/
├── shared/
├── tas/
├── texture-packs/
└── volume-amplifier/
```

### Bundled dependency policy

Custom-fork dependencies that need to be shipped in-tree belong under
[`xemu-features/dependencies/`](xemu-features/dependencies/). Each bundled
component must retain its original license/copyright notices, record its exact
upstream project and version/commit, document any local changes, and be included
in the repository's third-party notices and outbound binary-license bundle when
required.

This directory is a containment rule for custom-fork dependencies; it does not
move, rename, or relicense dependencies inherited from upstream xemu/QEMU.

### Builds and support

The normal upstream xemu build system is retained. For a configured Linux
build tree, the development binary can be rebuilt with:

```sh
ninja -C build qemu-system-i386
cp -f build/qemu-system-i386 dist/xemu
```

Upstream build documentation and requirements remain authoritative:
https://xemu.app/docs/download/#building

Problems caused specifically by the custom features should be reported against
this fork rather than upstream xemu. An issue should only be reported upstream
when it can also be reproduced on an unmodified upstream build.

### Game assets and replacement packs

This repository does not grant rights to redistribute Microsoft Xbox firmware,
commercial games, or copyrighted game assets. Texture packs, material maps,
audio packs, geometry exports, scripts, and other user-created mod content are
separate works whose authors are responsible for the rights to the material
they use and redistribute.

### Licensing and attribution for the custom fork

The custom feature work does **not** replace or override upstream licensing.

The authoritative licensing information remains in:

- [`LICENSE`](LICENSE)
- [`COPYING`](COPYING)
- [`COPYING.LIB`](COPYING.LIB)
- [`licenses/`](licenses/)
- individual source-file copyright/license headers
- [`CREDITS.md`](CREDITS.md)
- [`NOTICE.md`](NOTICE.md)
- [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)

Modified upstream files retain their applicable upstream license terms.
Third-party components retain their own licenses and notices.

This fork is independently maintained and is not affiliated with or endorsed
by Microsoft or the upstream xemu, XQEMU, or QEMU projects.

<!-- END JOSHUA-1248 EXPANDED FEATURE SUITE -->
