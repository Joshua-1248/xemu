# Xbox DVD CHD support

This feature adds a read-only QEMU block-format layer for Xbox DVD/XISO images
compressed with the standard MAME `chdman createdvd` workflow.

## User workflow

```sh
chdman createdvd -i game.iso -o game.chd
```

Then use Xemu's normal **Machine -> Load Disc...** action and choose `game.chd`.
No extraction, hunk/codec setting, command-line switch, or CHD-specific runtime
configuration is required.

## Architectural contract

`game.chd -> libchdr -> exact logical DVD bytes -> existing Xemu DVD/ATAPI path`

The block driver intentionally knows nothing about XDVDFS, Xbox partitions,
individual games, or mod overrides. It does not emulate CD tracks/FAD/pregaps,
does not cook 2352-byte sectors, does not extract a temporary ISO, and does not
add CHD-specific timing behavior.

V1 accepts standalone CHDv5 DVD images with 2048-byte units, 2048-aligned
logical length/hunks, and the normal `DVD ` metadata tag produced by
`chdman createdvd`. Parent/delta CHDs are rejected clearly for now.

The backing file stays owned by QEMU. libchdr is opened through its callback API
and reads through the format node's normal `BdrvChild`, so it never reopens a
host pathname behind QEMU's block graph.

## Runtime cache

Decoded hunks use an adaptive, bounded 4-way set-associative cache with a
16 MiB target budget. The cache scales down automatically for small images and
falls back as far as a single hunk if memory is constrained, so the optimization
cannot turn a previously loadable CHD into an avoidable allocation failure.
Sequential streams still decode each unique hunk once; locality-heavy access can
reuse recently decoded hunks instead of repeating compressed-file I/O and codec
work. No speculative read-ahead is performed, so guest-visible access semantics
and existing DVD timing remain unchanged.

## Disc Files & Mods integration

The filesystem browser, title detection, extraction, and per-title override
logic consume the already-mounted logical QEMU medium. They never reopen the
`.chd` container as a host file. A feature-owned read-only BlockBackend pins
the exact mounted BDS while it is in use, and media refresh/change is
serialized against background extraction so an extraction can never cross
from one disc generation into another.

Background extraction submits reads on the pinned backend's own AioContext; it
does not call coroutine-backed block I/O directly from the host `std::thread`.
The pin is explicitly released on eject, failed load refresh, and before QEMU
block teardown at shutdown.

## Validation status

Static/standalone validation covers the frontend feature gate, CHD architecture
contract, XDVDFS logical-reader equivalence, deterministic cross-hunk read
slicing, and cache replacement/locality behavior.

Linux Mint runtime acceptance has additionally confirmed normal Load Disc use,
lower/uppercase `.chd`, boot and gameplay across multiple Xbox titles, XDVDFS
browsing, individual/full-filesystem extraction, media replacement/eject parity,
and live per-title file overrides from a CHD-backed game. A `chdman createdvd`
-> `extractdvd` round trip was also SHA-256 identical to the source logical ISO.
