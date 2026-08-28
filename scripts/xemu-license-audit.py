#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""License/provenance consistency checks for the Joshua-1248 xemu fork."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []


def fail(message):
    errors.append(message)


def text(rel):
    p = ROOT / rel
    if not p.is_file():
        fail(f"missing required file: {rel}")
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


required_docs = [
    "README.md",
    "CREDITS.md",
    "THIRD_PARTY_NOTICES.md",
    "docs/custom-fork/LICENSING_AND_PROVENANCE.md",
]
for rel in required_docs:
    text(rel)

required_license_files = [
    "licenses/SDL3.license.txt",
    "licenses/stb_image_write.license.txt",
    "licenses/libwebp.license.txt",
    "licenses/capstone.license.txt",
    "licenses/xemu_trainer_lib.license.txt",
]
for rel in required_license_files:
    text(rel)

# Every fork-owned/feature-isolated C/C++ translation unit and header must carry
# a machine-readable license marker.  The renderer/APU bridge exceptions retain
# upstream LGPL lineage; the rest are GPL-2.0-or-later unless a future file
# documents a different compatible license deliberately.
feature_root = ROOT / "xemu-features"
if not feature_root.is_dir():
    fail("missing xemu-features directory")
else:
    lgpl = {
        "audio-packs/audio-packs-apu.c",
        "audio-packs/audio-packs-apu.h",
        "texture-packs/texture-packs-gl.c",
        "texture-packs/texture-packs-gl.h",
        "texture-packs/texture-packs-vk.c",
        "texture-packs/texture-packs-vk.h",
    }
    for p in sorted(feature_root.rglob("*")):
        if p.suffix not in {".c", ".cc", ".h", ".hh"}:
            continue
        rel = p.relative_to(feature_root).as_posix()
        src = p.read_text(encoding="utf-8", errors="replace")
        expected = "LGPL-2.1-or-later" if rel in lgpl else "GPL-2.0-or-later"
        if f"SPDX-License-Identifier: {expected}" not in src[:2048]:
            fail(f"{p.relative_to(ROOT)}: expected SPDX {expected}")

# Human-readable lineage should stay beside the machine-readable LGPL markers
# for bridge files derived from upstream renderer/APU code.
for rel, sentinel in {
    "xemu-features/audio-packs/audio-packs-apu.c": "Portions of this bridge are derived from the xemu/QEMU MCPX Audio",
    "xemu-features/audio-packs/audio-packs-apu.h": "MCPX/APU feature boundary associated",
    "xemu-features/texture-packs/texture-packs-gl.c": "Adapter for the xemu NV2A OpenGL renderer",
    "xemu-features/texture-packs/texture-packs-gl.h": "NV2A OpenGL texture-pack boundary",
    "xemu-features/texture-packs/texture-packs-vk.c": "Portions are derived from the xemu NV2A Vulkan renderer",
    "xemu-features/texture-packs/texture-packs-vk.h": "NV2A Vulkan texture-pack boundary",
}.items():
    src = text(rel)
    if src and sentinel not in src[:8192]:
        fail(f"{rel}: missing upstream renderer/APU lineage comment")

# The Python feature-isolation checker is fork-owned code too.
phase4_audit = text("scripts/xemu-feature-isolation-audit.py")
if phase4_audit and "SPDX-License-Identifier: GPL-2.0-or-later" not in phase4_audit[:1024]:
    fail("scripts/xemu-feature-isolation-audit.py: missing GPL SPDX marker")

trainer_url = "https://github.com/Joshua-1248/Xemu-Cheat-Engine-and-Trainer"
for rel, source_name in [
    ("xemu-features/cheats/codes-engine.cc", "xemu_trainer_lib/codes.py"),
    ("xemu-features/cheats/cheatfile.cc", "xemu_trainer_lib/cheatfiles.py"),
]:
    src = text(rel)
    if src:
        if trainer_url not in src[:4096]:
            fail(f"{rel}: missing Xemu Cheat Engine and Trainer source URL")
        if source_name not in src[:4096]:
            fail(f"{rel}: missing source-module provenance ({source_name})")
        if "MIT" not in src[:4096]:
            fail(f"{rel}: missing MIT source-project provenance")

fatx = text("ui/thirdparty/fatx/fatx.c")
if fatx:
    if "SPDX-License-Identifier: GPL-2.0-or-later" not in fatx[:2048]:
        fail("ui/thirdparty/fatx/fatx.c: missing GPL-2.0-or-later SPDX marker")
    if "https://github.com/mborgerson/fatx" not in fatx[:4096]:
        fail("ui/thirdparty/fatx/fatx.c: missing libfatx provenance URL")
fatx_h = text("ui/thirdparty/fatx/fatx.h")
if fatx_h and "SPDX-License-Identifier: GPL-2.0-or-later" not in fatx_h[:1024]:
    fail("ui/thirdparty/fatx/fatx.h: missing GPL-2.0-or-later SPDX marker")

# The retained-license index should account for every repository-level license
# cache entry, so adding a new dependency cannot silently leave the human index
# stale.
notices = text("THIRD_PARTY_NOTICES.md")
license_dir = ROOT / "licenses"
if notices and license_dir.is_dir():
    for p in sorted(license_dir.glob("*.license.txt")):
        rel = p.relative_to(ROOT).as_posix()
        if rel not in notices:
            fail(f"THIRD_PARTY_NOTICES.md does not index {rel}")

# Guard the known outbound-license-generator regressions found in the 2026-08-28
# review.
gen = text("scripts/gen-license.py")
if gen:
    if 'version="6.0.0"' in gen:
        fail("scripts/gen-license.py still hard-codes QEMU 6.0.0")
    if 'version="3.4.8"' in gen:
        fail("scripts/gen-license.py still hard-codes SDL3 3.4.8")
    if 'version="2.25"' in gen:
        fail("scripts/gen-license.py still hard-codes stale stb_image 2.25")
    if 'open(fname,' in gen:
        fail("scripts/gen-license.py still writes an undefined fname")
    for token in [
        'open("QEMU_VERSION"',
        'Submodule("subprojects/sdl3.wrap")',
        'version_from_file(',
        '"stb_image_write"',
        '"nlohmann_json"',
        '"genconfig"',
        '"keycodemapdb-generated"',
        '"libwebp"',
        '"Capstone"',
        '"RenderDoc in-application API"',
        '"gloffscreen"',
        '"Xemu-Cheat-Engine-and-Trainer"',
    ]:
        if token not in gen:
            fail(f"scripts/gen-license.py missing expected dependency/version handling: {token}")

# Confirm the actual SDL wrap version is discoverable rather than relying on a
# version string duplicated in the generator.
sdl_wrap = text("subprojects/sdl3.wrap")
if sdl_wrap and not re.search(r"^directory\s*=\s*SDL3-\d+(?:\.\d+)+\s*$", sdl_wrap, re.MULTILINE):
    fail("subprojects/sdl3.wrap: could not identify SDL3 release directory/version")

if errors:
    print("Xemu licensing/provenance consistency check: FAIL", file=sys.stderr)
    for e in errors:
        print(f"  - {e}", file=sys.stderr)
    raise SystemExit(1)

print("Xemu licensing/provenance consistency check: PASS")
