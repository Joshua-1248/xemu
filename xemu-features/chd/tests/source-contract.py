#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Static architecture guardrails for the xemu CHD feature."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[3]

def text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")

def require(cond: bool, message: str) -> None:
    if not cond:
        raise SystemExit(f"FAIL: {message}")

chd = text("xemu-features/chd/chd-block.c")
core = text("xemu-features/disc-modding/core.cc")
bridge = text("xemu-features/disc-modding/mounted-disc-bridge.c")
xui_meson = text("ui/xui/meson.build")
xui = text("ui/xemu.c")
actions = text("ui/xui/actions.cc")
popup = text("ui/xui/popup-menu.cc")
meson = text("meson.build")
dep_meson = text("xemu-features/dependencies/libchdr/meson.build")

# CHD must remain a raw logical DVD format layer, not a CD cooker.
require("chd_open_core_file_callbacks" in chd,
        "libchdr must use QEMU-backed callbacks")
require(not re.search(r"\bchd_open\s*\(", chd),
        "block driver must not reopen a host path with chd_open()")
require("chd_precache" not in chd,
        "CHD support must not turn loading a disc into whole-image precaching")
for forbidden in ("CDROM_TRACK_METADATA", "CD_FRAME_SIZE", "start_fad",
                  "2352", "pregap"):
    require(forbidden not in chd,
            f"CD-oriented concept leaked into Xbox DVD CHD driver: {forbidden}")
require("DVD_METADATA_TAG" in chd, "DVD metadata validation is required")
require("XEMU_CHD_DVD_SECTOR_SIZE 2048" in chd,
        "DVD unit size must stay 2048 bytes")
require("XEMU_CHD_MAX_HUNK_SIZE" in chd,
        "driver must bound its extra hunk allocation")
require("XEMU_CHD_CACHE_TARGET_BYTES" in chd and
        "XEMU_CHD_CACHE_WAYS" in chd and
        "xemu_chd_get_hunk" in chd,
        "adaptive bounded multi-hunk cache is missing")
require("for (size_t capacity = wanted; capacity != 0; capacity /= 2)" in chd,
        "CHD cache must fall back under memory pressure")
require("cached_hunk" not in chd,
        "single-hunk cache regression detected")
require("Parent/delta CHDs are not supported yet" in chd,
        "v1 parent handling must fail explicitly")
require('#include "qapi/error.h"' in chd,
        "CHD driver must include the QAPI Error declarations it uses")

# Normal UI/load path: .chd chooses the CHD format, raw ISO stays raw.
require("xemu_chd_block_format_for_path(path)" in xui,
        "Load Disc must select CHD format by path")
require("iso;xiso;chd" in actions, "Load Disc picker must expose CHD")
require('".chd"' in popup, "Games list must scan CHD")
require("xemu_feature_chd" in meson and "xemu-features/chd/chd-block.c" in meson,
        "Meson CHD feature gate/driver registration missing")

# Disc Files & Mods must consume the mounted logical medium, including worker I/O.
require("ParseXdvdfsReader" in core and "MountedLogicalDiscReader" in core,
        "XDVDFS browser must parse mounted logical bytes")
require("mounted-disc-bridge.h" in core and "qemu/osdep.h" not in core,
        "Disc Files C++ must use the feature-owned C bridge, not QEMU C-only headers")
require("xemu_mounted_disc_pread_worker" in core and
        "aio_bh_schedule_oneshot(ctx, xemu_mounted_disc_read_start" in bridge,
        "background extraction must marshal reads to the BlockBackend AioContext")
require("blk_inc_in_flight(backend->blk)" in bridge and
        "blk_dec_in_flight(wait->backend->blk)" in bridge,
        "async extraction must pin the BlockBackend AioContext")
require("AIO_WAIT_WHILE" in bridge,
        "disc-change cancellation must pump pending AIO to avoid join deadlock")
require("mounted-disc-bridge.c" in xui_meson,
        "feature-owned C bridge must be part of the disc-modding build")
require("g_backend_refreshing" in core and
        "g_extraction.CancelAndWait();" in core,
        "backend replacement must exclude new extraction and cancel the old one")
require("Register the worker while holding the same control mutex" in core,
        "extraction registration/backend lifetime race guard is missing")
require("next_offset_valid" in core,
        "sequential override reads should avoid redundant host seeks")
require("FindVirtualFileIndex" in core,
        "virtual override range walking should use one indexed lookup then scan")
require("xemu_disc_overlay_schedule_refresh();" in xui,
        "load/eject paths must refresh or release the mounted logical backend")
require("xemu_disc_overlay_notify_disc_path(NULL);" in xui and
        "xemu_disc_overlay_refresh_mounted_backend();" in xui,
        "shutdown must release the feature-owned BlockBackend before QEMU cleanup")

# Avoid duplicate compression libraries when QEMU already provides them.
require("CHDR_SYSTEM_ZLIB=1" in dep_meson and "dependencies: libchdr_external_deps" in dep_meson,
        "libchdr must reuse QEMU's zlib dependency")
require("CHDR_SYSTEM_ZSTD=1" in dep_meson,
        "libchdr should reuse QEMU zstd when available")
require("VERIFY_BLOCK_CRC=1" in dep_meson,
        "performance work must not disable libchdr block integrity checks")
require("miniz.c" not in dep_meson,
        "do not compile libchdr's bundled miniz into xemu")

print("PASS: CHD source architecture contract")
