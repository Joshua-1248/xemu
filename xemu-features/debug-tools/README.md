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

Uses the bundled **Capstone 5.0.9** x86 decoder under
`xemu-features/dependencies/capstone/`, plus Xemu CPU/debug primitives and
shared guest-memory. A normal build does not require a system Capstone package
or runtime library. The feature deliberately does not absorb native
`ui/xui/debug.cc`, monitor windows, or other upstream debugger facilities.

## Threading model

No dedicated debug worker is created. Debug requests use the existing CPU/BQL synchronization paths.

## Hot-path behavior

There is no normal emulation polling when the debugger is closed. Expensive memory/disassembly work is performed on demand by the feature UI.

## Build-disabled behavior

Custom debugger/disassembler/API objects are not linked. Menu/window calls are inline no-ops; native Xemu debugging functionality remains available as upstream provides it.

## Porting only this feature

Copy `xemu-features/debug-tools/` plus shared guest-memory, gate the API/UI sources, and add only the custom menu/window hooks.

The neutral public-header contract should be retained when porting so unrelated core code does not need `#ifdef` forests.

## Features #10 debugger / memory workflow refresh

The debugger UI now includes the later Features #10 interaction work in addition
to the original Features #5 layout:

- disassembly supports single, toggle, and range/multi-selection, with copy/NOP
  actions operating on the current instruction selection;
- keyboard debugger controls include breakpoint toggling and stepping shortcuts;
- the Memory viewer is a direct editor surface with hex and text input,
  keyboard navigation, mouse/drag selection, and typed selected-value
  interpretation (`u8/u16/u32/u64/float32/float64`);
- Memory Search results can be handed directly to the Memory viewer,
  Disassembler, Globals, watchpoints, or Saved Addresses;
- **Saved Addresses** is a per-title Cheat-Engine-style table with groups,
  descriptions, physical/virtual addressing, typed values, freeze/unfreeze
  state, set-value actions, multi-selection, and verified persistent storage;
- saved-address writes use temporary-file verification and preserve a backup of
  the previous table before replacing it.

These workflows are implemented in-process and continue to use the same
feature-owned guest-memory/debug boundaries.

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

## Integrated Memory Search

The Disassembler's lower tool area includes a **Memory Search** tab ported from
the useful scanning workflow of the user's standalone Xemu Cheat Engine. Unlike
the external tool, this implementation runs in-process and reads Xbox guest
memory through the feature-owned debugger/guest-memory APIs rather than
`/proc`, host process handles, or host-address discovery.

Supported value types:

- `int8`, `int16`, `int32` (unsigned little-endian, matching the standalone tool)
- `float32`, `float64`
- UTF-8 strings
- arrays of bytes, including `??` wildcards

Supported narrowing modes include Equal/Not Equal, Less/Greater, Between,
Increased/Decreased, Increased/Decreased By, Changed, Unchanged, and **Unknown
Value Search**.

Scans can target physical RAM, common virtual Xbox regions, the detected XBE
image, or a custom physical/virtual range. The value type, item size, alignment,
and address space are locked after First Scan so subsequent scans cannot
silently reinterpret the candidate set.

Large scans are time-sliced on the UI thread instead of issuing unsafe guest
memory reads from a host worker thread. Candidate membership is stored as a
bitset, keeping an unknown `int8` scan across 128 MiB to roughly 16 MiB of
candidate metadata rather than hundreds of megabytes of address objects.
Two scan snapshots are reused transactionally; cancelling a Next Scan preserves
the previous baseline and result set.

Results are paged and only visible rows perform live reads. A selected result
can be copied, opened in the Memory tab, sent to the Disassembler, added as a
Globals watch, or used to create read/write watchpoints. Physical-to-virtual
translation is performed only for explicit result actions, never once per row
per frame.
