# Debug Tools

## Purpose

Custom disassembler/debugger extension and memory/register/breakpoint UI. Native Xemu Monitor/Audio/Video debug facilities remain in their original upstream-owned files.

## Build gate

- Meson: `xemu_feature_debug_tools`
- Config macro: `CONFIG_XEMU_FEATURE_DEBUG_TOOLS`
- Default in this custom fork: ON

## Public API

`debug-api.h` is the CPU/memory debugger service boundary. `disassembler.hh` owns the custom debugger UI implementation. `frontend.hh` owns menu/window composition.

## Files owned

- `debug-api.c`
- `debug-api.h`
- `disassembler.cc`
- `disassembler.hh`
- `function-index.cc`
- `function-index.hh`
- `frontend.cc`
- `frontend.hh`

## Exact Xemu hook sites

- `ui/xui/main.cc` — custom debug windows hook.
- `ui/xui/menubar.cc` — custom debug menu items.
- `ui/xui/meson.build` — conditional UI/disassembler sources.
- root `meson.build` — conditional `debug-api.c` and shared guest-memory.


## Features #5 ASM-cheat workflow refresh

The custom debugger now uses the same reverse-engineering shape as the user's external Xemu Cheat Engine: address/back navigation, a searchable function browser, call/prologue/RTTI discovery, symbol import, direct call-xref navigation, a table-oriented disassembly view, and lower Registers/Breakpoints/Stack/Threads/Locals/Parameters/Globals/Memory panels.

Live patch helpers preserve original bytes for reliable undo, support NOP and arbitrary byte patches, and can copy a tested patch as the existing virtual write code types 8/9/A. **Reserved code Type F is intentionally untouched.** The debugger does not repurpose, execute, or generate Type F.

## Dependencies

Uses Capstone and Xemu CPU/debug primitives plus shared guest-memory. It deliberately does not absorb native `ui/xui/debug.cc`, monitor windows or other upstream debugger facilities.

## Threading model

No dedicated debug worker is created. Debug requests use the existing CPU/BQL synchronization paths.

## Hot-path behavior

There is no normal emulation polling when the debugger is closed. Expensive memory/disassembly work is performed on demand by the feature UI.

## Build-disabled behavior

Custom debugger/disassembler/API objects are not linked. Menu/window calls are inline no-ops; native Xemu debugging functionality remains available as upstream provides it.

## Porting only this feature

Copy `xemu-features/debug-tools/` plus shared guest-memory, gate the API/UI sources, and add only the custom menu/window hooks.

The neutral public-header contract should be retained when porting so unrelated core code does not need `#ifdef` forests.

### Direct debugger -> Cheats handoff

When both `xemu_feature_debug_tools` and `xemu_feature_cheats` are enabled, a
live debugger byte patch can be saved directly as an `[ASM]` cheat. The
handoff is transactional: Debug Tools first restores its temporary patch,
Cheats captures the true original bytes while applying the generated 8/9/A
writes, and Debug Tools re-applies its temporary patch if the handoff fails.
Disabling the resulting `[ASM]` cheat restores the captured bytes.

The coupling is only through `xemu-features/cheats/debug-bridge.hh`; when
Cheats is disabled that header exposes a neutral no-op API, preserving the
independent Debug Tools build gate.

**This does not use or modify reserved Type F.**
