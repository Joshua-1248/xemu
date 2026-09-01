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

## Script text / display API (Features #5)

The scripting bridge supports two kinds of script-owned text output in addition
to the Lua/Python console itself:

1. **Internal overlay text** drawn over Xemu's main ImGui foreground.
2. **External script display** rendered in a feature-owned ImGui tool window
   which automatically detaches into a native OS window by default and can be
   dragged back into Xemu like the other detachable custom tools.

The older `overlay_text(x, y, text)` call remains available for compatibility,
but it is append-only.  For live counters, watches, HUDs, and continuously
changing values, use the ID-based `overlay_set(...)` API instead. Reusing the
same ID updates the existing text in place instead of adding a new draw item on
every frame.

Colors use `#RRGGBB` or `#RRGGBBAA`. Coordinates are logical window/client
pixels. `screen_size()` and `display_size()` are available when a script wants
to calculate dynamic placement.

### Python example

```python
import xemu

# Internal HUD text over the main Xemu window.
xemu.overlay_set("fps_note", 20, 50, "Hello from Python",
                 color="#FFFFFFFF", scale=1.25,
                 background="#000000A0")

# A separate native script display.
xemu.display_open("My Python HUD", 720, 360, detached=True)
xemu.display_text("title", 20, 20, "External display",
                  color="#FFD060FF", scale=1.5)

# Efficient live update: the same IDs are replaced in place.
def update(frame):
    xemu.overlay_set("frame", 20, 80, f"Frame: {frame}")
    xemu.display_text("frame", 20, 60, f"Frame: {frame}")

xemu.on_frame(update, 300)

xemu.overlay_remove("fps_note")
xemu.display_clear()
```

### Lua example

```lua
local xemu = require("xemu")

xemu.overlay_set("hello", 20, 50, "Hello from Lua",
                 "#FFFFFFFF", 1.25, "#000000A0")

xemu.display_open("My Lua HUD", 720, 360, true)
xemu.display_text("status", 20, 20, "External display",
                  "#80E0FFFF", 1.5)

xemu.on_frame(function(frame)
  xemu.overlay_set("frame", 20, 80, "Frame: " .. frame)
  xemu.display_text("frame", 20, 60, "Frame: " .. frame)
end, 300)
```

### Added helpers

Python and Lua expose equivalent helpers:

- `screen_size()`
- `overlay_set(id, x, y, text, color, scale, background)`
- `overlay_remove(id)`
- `overlay_clear()`
- legacy `overlay_text(x, y, text)`
- `display_open(title, width, height, detached)`
- `display_close()`
- `display_size()`
- `display_text(id, x, y, text, color, scale, background)`
- `display_remove(id)`
- `display_clear()`

Script displays and overlay contents remain owned by
`xemu-features/scripting/`; no native Xemu/QEMU source changes are required.

## Advanced scripting API (Features #5)

The scripting bridge is now intended to be usable as an in-emulator reverse-
engineering and automation environment rather than only a memory read/write
console.  Lua and Python expose matching APIs where practical.

### Drawing / HUD API

Both the internal Xemu foreground and the detachable external script canvas can
render persistent ID-based objects. Reusing an ID updates the object in place.

Supported objects:

- text with RGBA color, scale, and optional background;
- lines;
- outlined or filled rectangles;
- outlined or filled circles;
- horizontal or vertical progress/value bars;
- crosshairs;
- images loaded through Xemu's existing `stb_image` support;
- live Xbox controller visualizers.

The drawing functions accept `target="overlay"` / `"display"` in Python and the
same final target argument in Lua. The external display continues to use the
feature-owned detachable-window implementation.

Images are loaded lazily into the frontend OpenGL resource-sharing context.
Relative image paths are resolved against the script's directory. No native
Xemu renderer source is modified.

### Memory API

Legacy `read_u8/u16/u32` and `write_u8/u16/u32` retain their original physical
RAM semantics for compatibility. New helpers make the address space explicit:

- physical and virtual 8/16/32-bit reads/writes;
- arbitrary `read_bytes` / `write_bytes` up to 1 MiB per request;
- little-endian float32/float64 helpers;
- C-string helpers;
- Python pointer-chain helper;
- direct memory-to-HUD bindings.

`watch_text()` and `watch_bar()` are updated inside Xemu itself, so a script can
bind a health value, timer, coordinate, flags word, etc. directly to an overlay
without doing a Python/Lua IPC round-trip every frame. Supported watch types are
`u8`, `i8`, `hex8`, `u16`, `i16`, `hex16`, `u32`, `i32`, `hex32`, `f32`, and
`f64`.

### Controller API

In addition to raw 20-byte XID report get/set support, scripts can now:

- request a decoded controller state;
- release one or all TAS controller overrides;
- receive controller-change callbacks in the helper layer;
- draw a live visual controller widget internally or externally.

The visualizer shows digital buttons, analog A/B/X/Y/Black/White, triggers, and
both thumbsticks from the actual guest-visible XID report.

### Event API

The subprocess bridge remains synchronous by design, but event waits are
serviced by Xemu's frontend even while emulation is paused. Helpers include:

- frame callbacks;
- run-state change callbacks;
- pause/resume callbacks;
- title-change callbacks;
- controller-change callbacks;
- memory-change callbacks;
- debugger stop callbacks;
- breakpoint callbacks;
- watchpoint callbacks.

Debugger callbacks use a feature-owned stop-event record in
`xemu-features/debug-tools/debug-api.*`; they do not require a remote GDB
connection and do not modify native QEMU/Xemu debugger source.

### Debugger / disassembler API

When `xemu_feature_debug_tools` is enabled, scripts can use:

- `debug_available()`;
- general register reads and register writes;
- Capstone-backed disassembly;
- virtual or physical execution breakpoints;
- virtual or physical read/write/access watchpoints;
- Step Into, Step Over, Step Out, and Run To;
- virtual-to-physical and physical-to-virtual address translation;
- current/next debugger stop events.

When Debug Tools is compiled out, the scripting feature still builds and runs;
`debug_available()` returns false and debug-only calls report that the optional
feature is disabled.

### API capability discovery

`api_version()` currently returns `2`. `capabilities()` reports the compiled
feature set so reusable scripts can degrade gracefully.

### Examples

`examples/` now includes:

- `text_display_demo.py/.lua` - basic named text output;
- `advanced_hud_demo.py/.lua` - primitives, controller visualization, and an
  external detached HUD;
- `memory_watch_hud.py` - direct guest-memory bindings to text and bars;
- `debug_callbacks_demo.py` - breakpoint/watchpoint event handling;
- `image_overlay_demo.py` - image overlay loading.

All of this functionality remains owned by `xemu-features/`. The only companion
changes in this milestone are to the already-custom `debug-tools` public API so
scripting can consume breakpoint/watchpoint stop events.
