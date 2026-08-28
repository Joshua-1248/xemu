# Cheats / Patches

## Purpose

Cheat/patch database parsing, UI, and optimized runtime execution, including the
custom Type-6 pointer format with 1–255 offsets.

## Source lineage and license

The native C++ interpreter/parser in this directory is a port of Joshua-1248's
earlier Python project:

- <https://github.com/Joshua-1248/Xemu-Cheat-Engine-and-Trainer>
- `xemu_trainer_lib/codes.py` is the principal source/reference for
  `codes-engine.cc`;
- `xemu_trainer_lib/cheatfiles.py` is the principal source/reference for
  `cheatfile.cc`;
- the source project is licensed under the **MIT License**.

The Python package itself is not bundled with this xemu fork. The native C++
implementation is distributed here under **GPL-2.0-or-later**, while the MIT
source provenance and notice are retained in `licenses/xemu_trainer_lib.license.txt`,
`THIRD_PARTY_NOTICES.md`, `CREDITS.md`, and the source comments of the ported
files.

## Build gate

- Meson: `xemu_feature_cheats`
- Config macro: `CONFIG_XEMU_FEATURE_CHEATS`
- Default in this custom fork: ON

## Public API

`runtime.hh` is the generic frontend tick boundary. `codes.hh`,
`codes-engine.hh`, and `cheatfile.hh` contain the feature implementation and
editor/parser interfaces.

## Files owned

- `cheatfile.cc`
- `cheatfile.hh`
- `codes-engine.cc`
- `codes-engine.hh`
- `codes.cc`
- `codes.hh`
- `runtime.cc`
- `runtime.hh`

## Exact xemu hook sites

- `ui/xui/main.cc` — one `FeatureCodesTick()` runtime hook.
- `ui/xui/meson.build` — conditional frontend/engine source inclusion.
- root `meson.build` — includes `xemu-features/shared/guest-memory.c` only when
  tooling that needs guest memory is enabled.

## Dependencies

Uses the shared guest-memory service and existing XBE/title/settings/UI helpers.
It does not require TAS, scripting, debug tools, audio, texture, or fast forward.

## Threading model

Runs through the existing frontend/BQL integration. No dedicated feature worker
thread is created.

## Hot-path behavior

Compiled enabled blocks, page caching, generation invalidation, and lightweight
title polling keep the active path bounded. If codes are disabled in settings,
the runtime returns immediately.

## Build-disabled behavior

Cheat engine/editor/runtime translation units are omitted. The UI tick hook is
an inline no-op; shared guest-memory is also omitted unless another enabled tool
requires it.

## Porting only this feature

Copy `xemu-features/cheats/` and `xemu-features/shared/guest-memory.*`, wire the
single UI tick and Meson option, retain the settings schema used by the feature,
and preserve the `xemu_trainer_lib` MIT provenance above.

The neutral public-header contract should be retained when porting so unrelated
core code does not need `#ifdef` forests.
