# Capstone integration for the Joshua-1248 xemu fork

## Pinned upstream

- Project: Capstone Engine — https://github.com/capstone-engine/capstone
- Release: **5.0.9**
- Official tag commit: `022575848782a4801fd150fdbc927effcbca0864`
- Official release archive: `capstone-5.0.9.tar.xz`
- Archive SHA-256: `1b70351879f6998998ebcbe09bd5f3c5e27127e985af14722cbe52c11c35178e`
- License: BSD 3-Clause-style Capstone license; see `upstream/LICENSE.TXT`

The source under `upstream/` is the verified official Capstone release tree and
must remain unmodified. Custom Xemu integration lives beside it in this directory.

## Build policy

Xemu builds Capstone as a private static library. Only Capstone's x86 backend is
compiled, because Original Xbox guest code is x86 regardless of the host CPU or
host operating system. Full x86 instruction support is retained; diet mode and
`CAPSTONE_X86_REDUCE` are deliberately not used. Both Intel and AT&T printers are
kept because Xemu/QEMU code can request either syntax.

Bundled Capstone defaults to **enabled**. This is intentional: QEMU's configure
wrapper applies `-Dauto_features=disabled` to clean builds, so leaving this
project option at Meson's `auto` state would silently disable the disassembler in
fresh builds even though the bundled source is present. This was observed in the
Windows cross-build path and applies equally to any fresh `build.sh` configure.

The custom `xemu_feature_debug_tools` build gate therefore requires Capstone. A
configuration that enables Debug Tools while explicitly disabling Capstone is
rejected at configure time instead of producing a runtime debugger window with
no disassembler. Developers who intentionally want neither may use both:

```sh
-Dxemu_feature_debug_tools=false -Dcapstone=disabled
```

Normal Xemu builds do not require a system `libcapstone`, a Capstone DLL/dylib/so,
Homebrew Capstone, or `libcapstone-dev` package.

## Windows CI guard

The Windows cross-build explicitly passes `--enable-capstone` and, before
publishing an artifact, verifies that:

- the committed Capstone source is present;
- `CONFIG_CAPSTONE` is defined in `build/config-host.h`;
- `CONFIG_XEMU_FEATURE_DEBUG_TOOLS` is defined; and
- the private `libxemu-capstone-5.0.9.a` archive was produced.

This turns the previous silent runtime failure into a build/CI failure if the
configuration ever regresses.

## Maintenance / recovery vendoring

The current repository already contains the verified `upstream/` tree, so
ordinary builders do not run a download/bootstrap step. If that directory is
ever absent during repository maintenance or recovery, the maintainer runs:

```sh
./xemu-features/dependencies/vendor-capstone.sh
# or, with an already downloaded official archive:
./xemu-features/dependencies/vendor-capstone.sh ~/Downloads/capstone-5.0.9.tar.xz
```

The script downloads only the official 5.0.9 release archive (or accepts that
exact archive as an argument), verifies the published SHA-256 before extraction,
validates the expected source layout, and then populates `upstream/`. The
resulting `upstream/` directory is intended to be committed to this fork so
ordinary builders and binary users require no download.

## Local modifications

None to the Capstone source. Build selection and compiler definitions are owned
by the adjacent `meson.build` file.
