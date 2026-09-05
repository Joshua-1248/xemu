<!-- BEGIN JOSHUA-1248 CUSTOM FORK NOTICE -->

# NOTICE — Joshua-1248 xemu custom fork

This repository is an unofficial custom fork of the **xemu Original Xbox
Emulator**.

## Upstream provenance

Primary upstream project:

- xemu: https://github.com/xemu-project/xemu
- xemu website/documentation: https://xemu.app/

Major inherited technical lineage:

- XQEMU: https://github.com/xqemu/xqemu
- QEMU: https://www.qemu.org/

A substantial majority of the emulator foundation is inherited upstream work,
including QEMU infrastructure, Xbox hardware emulation, NV2A graphics
emulation, MCPX/APU emulation, UI/platform infrastructure, and the accumulated
work of their contributors.

The custom fork must not be presented as the sole or original author of that
work.

## Custom feature layer

Additional modding, replacement, dumping, debugger, scripting, TAS,
free-camera, Xbox DVD CHD, XDVDFS filesystem-browser/override, and related
functionality is implemented primarily under `xemu-features/`.

Small integration changes can exist outside `xemu-features/` when access to a
native emulator, renderer, APU, or UI path is necessary. Such modifications do
not change the ownership or license of the upstream files in which they appear.

### Feature-owned dependencies

When this fork vendors a third-party library specifically for custom features,
its source belongs under `xemu-features/dependencies/` where practical. Vendored
code remains third-party code: its original copyright, license, disclaimer,
provenance, and any required redistribution notices must be preserved. The
location of a dependency inside `xemu-features/` does not make it fork-authored
or relicense it under the fork's default custom-source terms.

## Licensing

This repository contains code under multiple licenses.

The authoritative terms are the licenses and notices already present in the
repository, including:

- `LICENSE`
- `COPYING`
- `COPYING.LIB`
- `licenses/`
- individual source-file copyright/license headers
- third-party component notices
- upstream Git history

**This NOTICE does not relicense any code.**

Modified upstream files retain their applicable license terms. Custom files are
governed by their own explicit headers and any applicable repository licensing
requirements when no more specific notice exists.

## Third-party software

Third-party dependencies and vendored components retain their original
copyright and license notices. Custom features use or interface with components
including Capstone, libchdr, the LZMA SDK and dr_flac embedded by libchdr,
libwebp, stb libraries, xxHash/XXH3, glslang, and other libraries present in or
supported by the wider xemu/QEMU tree.

See `CREDITS.md` and the corresponding in-tree component notices for
attribution.

## Game, firmware, and dumped content

No Microsoft Xbox firmware, MCPX/BIOS data, commercial game content, or
copyrighted game assets are licensed to users by this repository.

The emulator's replacement-pack, dumping, scripting, debugging, and modding
features do not grant rights to redistribute third-party game assets. Users and
pack authors are responsible for obtaining, using, modifying, and
redistributing firmware, games, textures, audio, models, scripts, dumps, and
other content lawfully.

## Trademarks and affiliation

Xbox and Microsoft are trademarks of their respective owners.

This custom fork is not affiliated with, sponsored by, or endorsed by
Microsoft or by the upstream xemu, XQEMU, or QEMU projects.

## Redistribution

When redistributing this fork, preserve all notices and materials required by
the applicable licenses. In particular:

1. keep applicable upstream copyright/license notices intact;
2. keep third-party notices intact;
3. retain source-file license headers;
4. distinguish custom-fork changes from upstream xemu where practical;
5. provide corresponding source and other materials when required by the
   applicable license.

For additional attribution, see `CREDITS.md`.

<!-- END JOSHUA-1248 CUSTOM FORK NOTICE -->
