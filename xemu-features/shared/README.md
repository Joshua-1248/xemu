# Shared optional-tool services

`guest-memory.c/.h` is a fork-local service used by cheats, TAS/TAStudio,
scripting and custom debug tools. It is not an independent user-facing
feature. Root `meson.build` links it only if at least one dependent feature is
enabled.

The service owns cached guest-address translation/RAM-size helpers and
read/write/code-invalidation helpers so individual tools do not duplicate
CPU-memory plumbing. With all dependent tools disabled, this object is
physically absent from the build.

## Detachable feature windows

`detachable-windows.hh` is a header-only, feature-owned native-window host for
custom ImGui tools. It exists specifically so detachable custom tools do **not**
require changes to Xemu's native `ui/`, SDL event loop, QEMU display code, or
the bundled Dear ImGui sources.

It creates secondary SDL3 + OpenGL windows with independent ImGui contexts,
queues only secondary-window SDL events through `SDL_AddEventWatch()`, and
restores Xemu's original window/GL/ImGui context after each tool render.

Supported interaction:

- drag an opted-in ImGui tool's title bar outside the main Xemu window to
  detach it;
- right-click the title bar and choose **Detach to native window** as a
  compositor-safe fallback;
- drag the native tool window back so its center is over Xemu to reattach it;
- press **Ctrl+Shift+D** while a detached tool is focused to reattach it;
- closing a detached OS window closes the corresponding tool normally.

The host is header-only so the Phase-4 feature isolation remains intact and no
new native Meson integration hook is required.
