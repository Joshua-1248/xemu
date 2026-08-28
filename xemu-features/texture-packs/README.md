# Texture Packs

## Purpose

Texture dumping/replacement for NV2A with GL and Vulkan adapters, animated GIF/WebP replacements, procedural shader replacements, dump color conversion and feature-owned renderer sidecars.

## Build gate

- Meson: `xemu_feature_texture_packs`
- Config macro: `CONFIG_XEMU_FEATURE_TEXTURE_PACKS`
- Default in this custom fork: ON

## Public API

`texture-packs.h` is renderer-neutral. `texture-packs-gl.h` and `texture-packs-vk.h` expose backend lifecycle/upload/dump/sidecar hooks. `frontend.hh` owns settings, hotkeys and render/frame synchronization.

## Files owned

- `frontend.cc`
- `frontend.hh`
- `texture-packs-gl.c`
- `texture-packs-gl.h`
- `texture-packs-vk.c`
- `texture-packs-vk.h`
- `texture-packs.c`
- `texture-packs.h`

## Exact Xemu hook sites

- `hw/xbox/nv2a/pgraph/gl/texture.c` — binding lifecycle, sampler override, replacement upload, dumps, dynamic refresh and backend sync.
- `hw/xbox/nv2a/pgraph/vk/texture.c` — creation plan, binding lifecycle, replacement upload, dumps, dynamic refresh and backend sync.
- `ui/xui/main.cc` / `ui/xui/main-menu.cc` — frame/hotkey/render-commit and settings hooks.
- `hw/xbox/nv2a/pgraph/{meson.build,gl/meson.build,vk/meson.build}` and `ui/xui/meson.build` — conditional source inclusion.

## Dependencies

Uses existing NV2A texture metadata and host GL/Vulkan APIs plus Xemu settings/file helpers. The renderer-neutral implementation is compiled once from `pgraph/meson.build`; GL and Vulkan compile only their adapters.

## Threading model

No feature-specific permanent worker thread is introduced. Filesystem/index work is feature-owned; renderer hot paths consume prepared lookup state. GL/Vulkan sidecars are keyed by ordinary `TextureBinding *` identity.

## Hot-path behavior

With the feature off, every renderer hook is an inline neutral operation and no sidecar is allocated. With it on, ordinary textures that need no custom state do not receive sidecars.

## Build-disabled behavior

No texture-pack implementation or backend adapter objects are linked. Core `TextureBinding` structures remain upstream-shaped and dump/replace hooks compile away.

## Porting only this feature

Copy `xemu-features/texture-packs/`, add the option/config flag, compile the generic source once plus the selected renderer adapter(s), then add the documented texture lifecycle/query hooks.

The neutral public-header contract should be retained when porting so unrelated core code does not need `#ifdef` forests.
