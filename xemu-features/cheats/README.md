# Cheats / Patches

## Purpose

Cheat/patch database parsing, UI and optimized runtime execution, including the custom Type-6 pointer format with 1–255 offsets.

## Build gate

- Meson: `xemu_feature_cheats`
- Config macro: `CONFIG_XEMU_FEATURE_CHEATS`
- Default in this custom fork: ON

## Public API

`runtime.hh` is the generic frontend tick boundary. `codes.hh`, `codes-engine.hh` and `cheatfile.hh` contain the feature implementation and editor/parser interfaces.

## Files owned

- `cheatfile.cc`
- `cheatfile.hh`
- `codes-engine.cc`
- `codes-engine.hh`
- `codes.cc`
- `codes.hh`
- `debug-bridge.hh`
- `runtime.cc`
- `runtime.hh`

## Exact Xemu hook sites

- `ui/xui/main.cc` — one `FeatureCodesTick()` runtime hook.
- `ui/xui/meson.build` — conditional frontend/engine source inclusion.
- root `meson.build` — includes `xemu-features/shared/guest-memory.c` only when tooling that needs guest memory is enabled.

## Dependencies

Uses the shared guest-memory service and existing XBE/title/settings/UI helpers. It does not require TAS, scripting, debug tools, audio, texture or fast forward.

## Threading model

Runs through the existing frontend/BQL integration. No dedicated feature worker thread is created.

## Hot-path behavior

Compiled enabled blocks, page caching, generation invalidation and lightweight title polling keep the active path bounded. If codes are disabled in settings the runtime returns immediately.

## Build-disabled behavior

Cheat engine/editor/runtime translation units are omitted. The UI tick hook is an inline no-op; shared guest-memory is also omitted unless another enabled tool requires it.

## Porting only this feature

Copy `xemu-features/cheats/` and `xemu-features/shared/guest-memory.*`, wire the single UI tick and Meson option, and retain the settings schema used by the feature.

The neutral public-header contract should be retained when porting so unrelated core code does not need `#ifdef` forests.

## `[ASM]` patch ownership / debugger handoff (Features #5)

A cheat whose leaf name ends in `[ASM]` keeps using the existing cheat-code
interpreter and existing write types. It gains reversible patch semantics:

- Original guest physical bytes are captured once, immediately before that
  `[ASM]` block first writes them.
- Re-applying the block does not overwrite the saved originals.
- Disabling the individual `[ASM]` cheat restores those original bytes
  immediately.
- Disabling the entire Codes feature restores all active `[ASM]` journals.
- A same-title reload restores active patches before rebuilding node IDs.
- A transient XBE-identification miss stops applying codes but does not destroy
  restore data; a positively identified different title invalidates it.

`debug-bridge.hh` is the optional Debug Tools -> Cheats boundary used by the
in-Xemu debugger's **Save ASM Cheat** action. The debugger hands over ordinary
existing command/value pairs (currently virtual write types 8/9/A) only after
it has restored its temporary live patch, so this feature records the real
pre-patch bytes as the restore image.

**Reserved Type F is unchanged.** The bridge neither generates nor assigns any
new meaning to Type F.

## In-Xemu individual cheat/group editor (Features #5)

The Cheats and Patches panes now keep the existing raw **Edit entire .txt**
editor while also exposing tree-level editing directly in Xemu:

- Add a cheat/patch at the top level.
- Add a group at the top level.
- Right-click a group to add a cheat or nested group inside it.
- Edit a cheat/patch name, author, description, enabled state and code lines.
- Move a cheat/patch between groups by changing its group path.
- Rename groups.
- Duplicate cheats, patches or whole groups. Duplicates intentionally start
  disabled so a copy cannot immediately double-apply a freeze or `[ASM]` patch.
- Delete cheats, patches or groups (with confirmation); deleting a group also
  deletes its contents.

All edits are serialized immediately through the existing atomic `.txt`
writer. Individual code-line parsing uses the same two-hex-value shape as the
trainer (`COMMAND VALUE`) but reports invalid lines instead of silently dropping
them. Missing group paths entered in the individual editor are created on save.

Active `[ASM]` nodes are restored before an edit or deletion changes their node
identity/content, then an edited enabled `[ASM]` node is re-applied through the
normal feature-owned journal path. Reserved Type F remains unchanged.
