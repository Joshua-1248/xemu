# Lua / Python Scripting

## Purpose

Custom Lua/Python scripting consoles and related frontend integration.

## Build gate

- Meson: `xemu_feature_scripting`
- Config macro: `CONFIG_XEMU_FEATURE_SCRIPTING`
- Default in this custom fork: ON

## Public API

`frontend.hh` exposes menu/window composition and the open-window query. `script-console.hh` contains console-facing APIs.

## Files owned

- `frontend.cc`
- `frontend.hh`
- `script-console.cc`
- `script-console.hh`

## Exact Xemu hook sites

- `ui/xui/main.cc` — show windows and include open-window state in UI wakeup.
- `ui/xui/menubar.cc` — scripting menu hook.
- `ui/xui/meson.build` and root `meson.build` — conditional feature/shared-memory sources.

## Dependencies

Uses shared guest-memory and the TAS public header for optional TAS-facing script integration. TAS can be compiled out because its public header supplies neutral stubs.

## Threading model

No permanently running feature thread is introduced by the isolation layer. Existing subprocess/console behavior remains owned inside the scripting module.

## Hot-path behavior

No generic emulation hot-path hook exists. When no scripting window/process activity is present, generic frontend work is limited to neutral window/menu queries.

## Build-disabled behavior

All scripting translation units are omitted and the menu/window hooks compile to false/no-op. Shared guest-memory is omitted if no other enabled tool needs it.

## Porting only this feature

Copy `xemu-features/scripting/` plus shared guest-memory, add the two frontend hooks and conditional Meson entries.

The neutral public-header contract should be retained when porting so unrelated core code does not need `#ifdef` forests.
