# xemu — Joshua-1248 fork

A custom fork of [xemu](https://github.com/xemu-project/xemu), the Original Xbox
emulator, focused on modding, debugging, TAS/tool-assisted workflows, asset
replacement, and emulator-side development tools.

The fork currently adds:

- texture dumping and replacement on OpenGL and Vulkan;
- animated GIF/WebP texture replacements and procedural texture shaders;
- source-level Xbox audio dumping and WAV replacement;
- a cheat and patch engine with an in-emulator editor;
- 2x–5x and unlimited fast forward, with optional pitch preservation;
- TAS mode and a TAStudio-style piano-roll/movie editor;
- Lua and Python scripting consoles with an emulator API bridge;
- an x86 guest disassembler/debugger;
- a 0–200% host-output volume range;
- several stability, controller, save-state, and quality-of-life fixes.

The custom systems are isolated under [`xemu-features/`](xemu-features/) and are
independently build-gated so they can be disabled or ported without dragging the
rest of the fork-specific code with them.

For the emulator itself — what xemu is, BIOS/HDD setup, compatibility, and
normal usage — see **[xemu.app](https://xemu.app)**. This README documents the
features added by this fork.

---

## Features

### Texture dumping and replacement

Dump the textures a game uses, edit them, and have xemu load your versions in
their place. The implementation supports both the OpenGL and Vulkan backends.

Textures are matched by a content hash, so a replacement follows the texture
wherever it appears. Files are separated by Xbox title ID so packs for different
games do not collide.

The controls are available under **Settings → Display → Texture Packs**.

| Default key | Action |
| --- | --- |
| `,` | Toggle texture replacements on/off |
| `.` | Reload replacements from disk |
| `/` | Toggle texture dumping on/off |

The hotkeys are configurable in the UI.

Default Linux paths:

```text
~/.local/share/xemu/xemu/textures/<TITLEID>/dumps/
~/.local/share/xemu/xemu/textures/<TITLEID>/replacements/
```

Both roots can be pointed elsewhere from the settings UI. A per-title directory
is still added automatically. Replacement subdirectories are scanned
recursively, so packs can be organized however you like.

Replacement files are named from the texture hash:

```text
<16 hex digits>.png            plain 2D texture
<16 hex digits>_posx.png       cubemap face
<16 hex digits>_mip1.png       explicit mip level
```

Cubemap suffixes are `posx`, `negx`, `posy`, `negy`, `posz`, and `negz`.

`.png`, `.gif`, and `.webp` replacements are supported. If more than one image
format exists for the same replacement key, WebP takes precedence over GIF,
which takes precedence over PNG.

The dumper also contains Xbox-format color conversion fixes so dumped images
are written in the intended channel/color layout rather than applying a global
format hack to unrelated textures.

### Animated textures

GIF and WebP replacements can animate in place at their own frame timing.
Animated WebP is preferred when available because it supports lossless full
alpha without GIF's 256-color palette and 1-bit transparency limits.

Animated WebP requires xemu to be built with libwebp. If libwebp is unavailable,
WebP files are ignored and GIF/PNG support continues to work.

### Procedural texture shaders

A replacement can be generated or transformed by a GLSL fragment shader. Put a
`<hash>.shader` file alongside the image replacements and the fork renders the
shader output into that texture.

Available uniforms:

| Uniform | Meaning |
| --- | --- |
| `iTime` | Seconds since start |
| `iResolution` | Target texture size in pixels |
| `iFrame` | Frame counter |
| `iChannel0` | Image replacement sampler, if an image exists |
| `iHasChannel0` | Whether `iChannel0` is bound |

If both an image and a `.shader` exist for a hash, the image is exposed as
`iChannel0`, allowing the shader to distort, animate, recolor, or otherwise
process an ordinary replacement.

Shader files are re-read while xemu is running, so shader edits can be previewed
without restarting the emulator.

### Audio dumping and replacement

**Settings → Audio → Audio Packs.** The fork can dump decoded Xbox APU source
audio to WAV and replace matching sounds before normal guest-controlled voice
processing.

Default Linux paths:

```text
~/.local/share/xemu/xemu/audio/<TITLEID>/dumps/
~/.local/share/xemu/xemu/audio/<TITLEID>/replacements/
```

The roots are configurable from the Audio settings page.

A normal replacement uses:

```text
<16 hex digits>.wav
```

Randomized variants use:

```text
<16 hex digits>_1.wav
<16 hex digits>_2.wav
<16 hex digits>_3.wav
...
```

When numbered variants are present, one is selected when the sound starts.
Replacement WAVs are preloaded when the pack is indexed so file I/O and WAV
decoding do not block APU voice workers during gameplay.

Replacement audio may use a different sample rate and may be shorter or longer
than the original. The replacement's natural duration controls completion while
Xbox-side pitch, volume, envelopes, filters, HRTF, DSP routing, looping, and
retrigger behavior remain in the guest audio path.

Current limitation: this pass targets static hardware/APU voices. Streaming SSL
voices remain on the original path until streaming reconstruction support is
implemented.

### Cheats and patches

**Settings → Codes.** Codes are grouped into cheats, which can remain active
while playing, and patches, which are applied as patches. Both use plain-text
files that can also be edited from the in-emulator editor.

Default Linux paths:

```text
~/.local/share/xemu/xemu/codes/cheats/<SERIAL_TITLEID>.txt
~/.local/share/xemu/xemu/codes/patches/<SERIAL_TITLEID>.txt
```

The filename stem is derived from the XBE title information without requiring an
external game database.

The interpreter is a C++ port of the earlier Python implementation and was
validated differentially against that reference across fuzz seeds, directed edge
cases, the reference game database, and parser files, including sanitizer runs.

The custom Type-6 pointer format supports **1–255 offsets**. `NN=00` retains the
legacy one-offset form for compatibility.

### Fast forward

**Settings → General → Fast Forward.** The fork provides host-side fast-forward
modes at:

```text
2x
3x
4x
5x
Unlimited
```

The default hotkey is **Tab**. It can operate as either a hold-to-fast-forward
key or a toggle, and the hotkey itself is configurable.

Fast forward includes timing, VBLANK/render-throttle, guest-clock, and APU pacing
handling rather than simply speeding one frontend loop. Audio remains enabled
while fast-forwarding.

**Preserve audio pitch** enables experimental granular time compression so audio
stays closer to its normal pitch instead of rising with emulation speed.

### TAS / TAStudio tools

The fork contains an experimental TAS core and a TAStudio-style editor built
around the Xbox guest VBLANK boundary and exact Xbox XID controller reports.

Use the top-level **TAS** menu or **Settings → General → TAS Tools**.

Current tooling includes:

- exact controller-report capture and injection;
- frame advance and skip-lag advance;
- movie recording and playback using `.xmt` files;
- recording from power-on or the current state;
- read-only movies and playback from a selected frame;
- a TAS Studio / piano-roll editor;
- input display and TAS HUD overlay;
- lag and frame tracking;
- bookmarks, markers, movie comments, and properties;
- rewind and greenzone caches;
- state hashing and movie compatibility checks;
- recovery autosaves and backup history;
- CSV movie import/export and movie comparison;
- RAM watch/search and RNG-oriented tools;
- optional experimental deterministic TAS mode.

TAS is additive: ordinary xemu functionality, including Fast Forward, remains
available. Host controller input is bypassed on a port only when TAS playback or
an explicit override injects an Xbox controller report.

### Lua and Python scripting

The top-level **Misc** menu exposes **Lua Console** and **Python Console**
windows.

The consoles launch a system Lua or Python interpreter and provide an
auto-generated xemu API bridge. The bridge currently exposes functionality such
as:

- guest physical-memory reads and writes;
- TAS frame and lag information;
- frame advance and frame waits;
- Xbox controller/XID access;
- snapshot operations;
- xemu notifications;
- overlay text.

The fork searches for common interpreter names such as `lua`, `lua5.4`,
`lua5.3`, `luajit`, `python3`, and `python`. The interpreters are not bundled
with xemu.

### Disassembler and debugger

**Debug → Disassembler.** The custom debugger provides x86 guest disassembly,
register inspection, breakpoints, watchpoints, single-stepping, run-to-cursor,
and a memory viewer that can operate on guest virtual or physical addresses.

Capstone is optional at build time. Without it the emulator still builds and
runs, but disassembly is unavailable.

> The memory viewer and some higher-level stepping helpers are less heavily
> tested than basic disassembly/breakpoint functionality. Step Out obtains a
> return address from `[ebp+4]`, so code compiled without a conventional frame
> pointer can legitimately make that operation unavailable.

Native upstream xemu Monitor, Audio, and Video debug windows remain native xemu
features; Phase 4 deliberately does not reclassify them as part of the custom
debugger.

### 0–200% output volume

The normal output-volume control is extended from 0–100% to **0–200%**.

Up to 100%, the fork preserves xemu's existing perceptual volume curve. Above
100%, the extension provides additional host-output gain up to 2.0x at 200%.
The feature is independent of Fast Forward and Audio Packs.

---

## Fixes and quality-of-life changes

- **Save-state freeze:** restores main-loop locking required during snapshot
  loads so a load cannot leave the machine running without advancing.
- **Fast-forward/save-state interaction:** fast-forward state is handled around
  snapshot operations rather than leaving timing state stuck across a load.
- **Controllers with identical names:** bindings also use the device path so
  multi-pad adapters exposing identical USB names do not arbitrarily swap player
  assignments between sessions.
- **Settings persistence:** settings are written when changed rather than only on
  a clean emulator exit.
- **PCI bus reset crash:** fixes reset behavior with pending interrupts.
- **Texture-dump color handling:** Xbox texture layouts are converted at the dump
  boundary instead of using broad format-specific channel swaps that can corrupt
  unrelated textures.
- **Notification responsiveness:** repeated feature hotkeys do not unnecessarily
  serialize transient notifications.

---

## Feature-isolated architecture

Fork-specific implementations live under:

```text
xemu-features/
├── audio-packs/
├── cheats/
├── debug-tools/
├── fast-forward/
├── scripting/
├── shared/
├── tas/
├── texture-packs/
└── volume-amplifier/
```

Ordinary xemu/QEMU and Xbox hardware files keep only narrow integration hooks.
Each user-facing feature has its own Meson build gate and neutral no-op API when
disabled, allowing implementation objects to be physically omitted from the
build.

Shared guest-memory helpers are linked only when an enabled tool requires them.
Native xemu systems such as SnapshotManager, NotificationManager, XBE handling,
the ordinary Monitor/Audio/Video debug windows, and `fast_hash` remain owned by
their upstream subsystems.

Architecture and integration notes live in [`docs/custom-fork/`](docs/custom-fork/)
and in each feature directory's `README.md` and `EXPORT_MANIFEST.txt`.

---

## Building

The normal build process is the same as upstream xemu. See the
[xemu build documentation](https://xemu.app/docs/download/#building).

Two useful optional development packages are:

```sh
sudo apt install libwebp-dev libcapstone-dev   # Debian/Ubuntu/Mint
brew install webp capstone                     # macOS
```

- Without libwebp, WebP texture replacements are unavailable; PNG/GIF continue
  to work.
- Without Capstone, the custom disassembler cannot decode instructions; the rest
  of the emulator continues to build.

Lua and Python are runtime dependencies only if you want to use their respective
scripting consoles.

Build normally with:

```sh
./build.sh
```

### Custom feature build switches

All custom feature gates default to `true` in this fork:

| Feature | Meson option |
| --- | --- |
| Audio Packs | `xemu_feature_audio_packs` |
| Texture Packs | `xemu_feature_texture_packs` |
| Cheats / Patches | `xemu_feature_cheats` |
| TAS / TAStudio | `xemu_feature_tas` |
| Lua / Python Scripting | `xemu_feature_scripting` |
| Debug Tools | `xemu_feature_debug_tools` |
| Fast Forward | `xemu_feature_fast_forward` |
| 0–200% Volume Amplifier | `xemu_feature_volume_amplifier` |

For example, to build without TAS and scripting:

```sh
cd build
./pyvenv/bin/meson configure \
  -Dxemu_feature_tas=false \
  -Dxemu_feature_scripting=false
ninja qemu-system-i386
```

To compile out every custom feature implementation:

```sh
cd build
./pyvenv/bin/meson configure \
  -Dxemu_feature_audio_packs=false \
  -Dxemu_feature_texture_packs=false \
  -Dxemu_feature_cheats=false \
  -Dxemu_feature_tas=false \
  -Dxemu_feature_scripting=false \
  -Dxemu_feature_debug_tools=false \
  -Dxemu_feature_fast_forward=false \
  -Dxemu_feature_volume_amplifier=false
ninja qemu-system-i386
```

Capstone and WebP can still be controlled using the ordinary Meson options:

```sh
cd build
./pyvenv/bin/meson configure -Dcapstone=enabled -Dwebp=enabled
ninja qemu-system-i386 && cp qemu-system-i386 ../dist/xemu
```

---

## Configuration

Most options are exposed through the xemu UI. The underlying config also
contains the following fork-specific keys:

| Key | Default | Meaning |
| --- | --- | --- |
| `general.texture_dump_enabled` | `false` | Dump textures as they are used |
| `general.texture_replace_enabled` | `false` | Load texture replacements |
| `general.texture_dump_skip_replaced` | `true` | Do not re-dump already replaced textures |
| `general.texture_dump_mipmaps` | `false` | Dump mip levels separately |
| `general.texture_dump_dir` | *(empty)* | Override texture dump root |
| `general.texture_replace_dir` | *(empty)* | Override texture replacement root |
| `general.texture_dump_toggle_key` | `/` | Texture-dump hotkey |
| `general.texture_replace_toggle_key` | `,` | Texture-replacement toggle hotkey |
| `general.texture_replace_reload_key` | `.` | Texture-replacement reload hotkey |
| `general.fast_forward_multiplier` | `3` | `2`–`5`, or `0` for Unlimited |
| `general.fast_forward_toggle_mode` | `false` | Toggle instead of hold-to-fast-forward |
| `general.fast_forward_preserve_pitch` | `false` | Experimental pitch-preserving fast-forward audio |
| `general.fast_forward_hotkey` | `Tab` | Fast-forward hotkey |
| `audio.dump_enabled` | `false` | Dump supported source audio to WAV |
| `audio.dump_skip_replaced` | `true` | Skip audio already covered by a replacement |
| `audio.replace_enabled` | `false` | Enable source-audio replacement |
| `audio.dump_dir` | *(empty)* | Override audio dump root |
| `audio.replace_dir` | *(empty)* | Override audio replacement root |
| `audio.volume_limit` | `1.0` | Host output volume; custom build supports up to `2.0` |
| `codes.enable` | `true` | Master switch for cheats and patches |
| `codes.cheats_dir` | *(empty)* | Override cheats directory |
| `codes.patches_dir` | *(empty)* | Override patches directory |
| `codes.interval_ms` | `16` | Re-application interval for enabled codes |

Texture hotkey values are stored as ImGui key codes even though the UI presents
them as named keys.

---

## Licensing

This fork is distributed under the same overall licensing structure as upstream
xemu, which is based on [QEMU](https://www.qemu.org/). The emulator as a whole is
under the **GNU General Public License, version 2**. See `LICENSE`, `COPYING`, and
`COPYING.LIB` in the repository.

Each source file retains its own licensing information. New fork-specific source
files are GPL-2.0-or-later. Modifications to existing files retain the license of
the file being modified; for example, the NV2A renderer code extended by this
fork includes LGPL-2.1-or-later files.

Third-party components used by the custom features include:

| Component | Use | License |
| --- | --- | --- |
| [stb_image](https://github.com/nothings/stb) | PNG/GIF replacement decoding | MIT or Public Domain |
| [stb_image_write](https://github.com/nothings/stb) | PNG texture dumping | MIT or Public Domain |
| [libwebp](https://developers.google.com/speed/webp) | WebP replacement decoding | BSD-3-Clause |
| [Capstone](https://www.capstone-engine.org/) | x86 disassembly | BSD-3-Clause |

`stb_image.h` already ships with upstream xemu. `stb_image_write.h` is included
by this fork and retains its own license notice. libwebp and Capstone are not
bundled; they are linked from the system when enabled/available.

The Lua and Python consoles launch external system interpreters and do not bundle
Lua or Python runtimes.

Texture packs, replacement images, replacement WAVs, shader files, scripts, TAS
movies, cheat files, and other user-authored content are not relicensed by this
repository; their authors retain their own rights in that content.

See `CREDITS.md` for attribution.

---

## Credits

xemu is by **Matt Borgerson** and contributors, built on
[XQEMU](https://github.com/xqemu/xqemu) and [QEMU](https://www.qemu.org/). The
NV2A graphics implementation extended by this fork includes work by **espes**,
**Jannik Vogel**, **Matt Borgerson**, and other xemu/XQEMU contributors.

This fork is not affiliated with the upstream xemu project. Please report
fork-specific issues to this repository rather than upstream xemu unless the
same issue can be reproduced on an unmodified upstream build.
