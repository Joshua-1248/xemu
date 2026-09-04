# Custom feature dependencies

This directory contains third-party dependencies that are vendored specifically
for the Joshua-1248 custom feature layer.

## Rules

- Use an official upstream release or pinned upstream commit as the source.
- Record the exact upstream project, version/commit, and retrieval date.
- Preserve the dependency's original copyright, license, disclaimer, and source
  notices verbatim.
- Document any local patches or build-only changes.
- Keep dependency-specific integration files here where practical.
- Update `THIRD_PARTY_NOTICES.md`, `CREDITS.md`, and outbound binary-license
  generation when a dependency is bundled or statically linked.
- Do not treat code in this directory as fork-authored merely because it is
  stored under `xemu-features/`.
- Do not move inherited upstream xemu/QEMU dependencies here solely for
  organizational consistency.

## Planned layout

```text
xemu-features/dependencies/
├── README.md
└── capstone/
    ├── <official pinned Capstone source>
    ├── LICENSES/
    └── XEMU_INTEGRATION.md
```

Capstone is **not yet bundled merely because this policy directory exists**.
Until the pinned source and build integration are committed, the repository's
existing system-detected Capstone behavior remains authoritative.
