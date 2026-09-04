# Disc Files & Mods

Feature-owned XDVDFS browsing, extraction, and per-title virtual disc-file
replacement for the Joshua-1248 xemu fork.

## UI location

Open **Misc → Disc Files & Mods**. The window participates in the shared
detachable-window system.

The window has two primary tabs:

- **Disc Files** — browse the mounted Xbox filesystem, inspect file metadata,
  extract content, and create/copy override material.
- **Mods / Settings** — enable/disable overrides, choose the mod-library root,
  inspect the detected title/disc and override counts, select extraction
  collision policy, and view safety/runtime messages.

## Build gate

- Meson: `xemu_feature_disc_modding`
- Config macro: `CONFIG_XEMU_FEATURE_DISC_MODDING`
- Default in this custom fork: ON

## XDVDFS browser

The feature parses the mounted Xbox disc image directly. It supports the common
trimmed/full-disc XDVDFS layouts used by the fork and exposes the filesystem as
a directory tree without first extracting the XISO.

The browser displays:

- internal Xbox path/name;
- directory/file type;
- original file size;
- start sector / LBA;
- whether an override is present and whether it is active.

Context/selection actions include:

- extract one file;
- extract a directory recursively;
- extract the entire disc;
- create the corresponding override folder;
- copy the original file into the override tree;
- copy internal or host override paths.

Extraction runs as a background job with progress, current path, byte/file
counts, and cancellation. Existing destination files can be skipped,
overwritten, or auto-renamed.

## Per-title disc-file overrides

The default mod-library base is:

```text
<xemu data>/mods/
```

The effective title tree is:

```text
<xemu data>/mods/<TITLEID>/disc/<original Xbox path>
```

A custom mod-library base can be selected from the Mods / Settings tab.

Overrides are matched using normalized Xbox-style case-insensitive paths. The
source XISO is never rewritten or repacked. Instead, the feature patches the
guest-visible XDVDFS file metadata in memory and maps replacement payloads into
feature-owned virtual sectors after the physical backing image.

Replacement files may be smaller, equal-size, larger, or empty, subject to the
format/runtime bounds enforced by the feature.

## Safety

The host mod tree is treated as untrusted input. The feature rejects or
fail-closes on conditions including:

- `.` / `..` path traversal;
- absolute/escaped host paths;
- symlink traversal/escape;
- unsafe or ambiguous host/Xbox path collisions;
- non-regular replacement files;
- replacement sizes/virtual-sector mappings that cannot be represented safely.

Replacement files are containment/symlink-checked again before their first
runtime open.

Override state is published immutably. Retired generations are retained for a
bounded period so previously queued/cached disc reads cannot accidentally refer
to freshly replaced state.

## Reload behavior

Changing a replacement file on disk requires **Reload Disc / Overrides** before
the new mapping is published. Restarting the title after a replacement change is
the safest workflow because an Xbox game may cache directory entries or file
sizes internally.

## Settings

Feature settings are stored separately from the normal xemu config at:

```text
<xemu data>/disc-modding/settings.txt
```

The file currently stores the enabled state and optional custom mod-library
base.

## Files owned

- `core.cc` / `core.hh` — immutable snapshots, override mapping, extraction,
  settings, path safety, and virtual-file state.
- `xdvdfs.cc` / `xdvdfs.hh` — XDVDFS image parsing.
- `disc-overlay.h` — narrow C bridge used by the DVD read path/frontend.
- `frontend.cc` / `frontend.hh` — detachable Misc UI.

## Exact Xemu integration surface

Small native hooks remain limited to the facts the feature cannot obtain
otherwise:

- `hw/ide/atapi.c` — lets the feature satisfy/patch DVD sectors when an active
  virtual overlay mapping applies.
- `system/vl.c` — supplies the initially configured disc path before the guest
  can issue its first DVD read.
- `ui/xemu.c` — keeps the feature synchronized with runtime disc
  load/eject/change operations.
- root `meson.build` / `meson_options.txt` and `ui/xui/meson.build` — build gate
  and conditional feature sources.
- `xemu-features/shared/misc-menu.hh` — feature-owned Misc menu/window
  aggregation.

The XDVDFS parser, mod policy, extraction implementation, filesystem work, and
virtual replacement state remain under `xemu-features/disc-modding/`.

## Build-disabled behavior

When `xemu_feature_disc_modding=false`, the feature implementation sources are
not linked and `disc-overlay.h` provides neutral behavior to the small native
hook sites. Ordinary Xemu disc reads continue without the custom overlay layer.
