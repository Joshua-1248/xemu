# Shared optional-tool services

`guest-memory.c/.h` is a fork-local service used by cheats, TAS/TAStudio, scripting and custom debug tools. It is not an independent user-facing feature. Root `meson.build` links it only if at least one dependent feature is enabled.

The service owns cached guest-address translation/RAM-size helpers and read/write/code-invalidation helpers so individual tools do not duplicate CPU-memory plumbing. With all dependent tools disabled, this object is physically absent from the build.
