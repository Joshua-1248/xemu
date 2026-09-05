#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

vp_path="hw/xbox/mcpx/apu/vp/vp.c"
patch_path="xemu-features/audio-packs/STREAM_REPLACEMENT_NATIVE_BRIDGE.patch"

if [[ ! -f "$vp_path" || ! -f "$patch_path" ]]; then
    echo "Run this from the Xemu repository root." >&2
    exit 1
fi

fix_shadow_warning() {
    if ! grep -q 'int count = xemu_audio_packs_voice_get_samples' "$vp_path"; then
        return 0
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        echo "Audio bridge is applied; python3 not found, leaving a harmless -Wshadow warning." >&2
        return 0
    fi
    python3 - "$vp_path" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
old = '''    if (!stream && xemu_audio_packs_voice_has_replacement(v)) {\n        int count = xemu_audio_packs_voice_get_samples(v, samples,\n                                                     num_samples_requested);\n'''
new = '''    if (!stream && xemu_audio_packs_voice_has_replacement(v)) {\n        int replacement_count = xemu_audio_packs_voice_get_samples(\n            v, samples, num_samples_requested);\n'''
if old in s:
    s = s.replace(old, new, 1)
    old_ret = '''        return count > 0 ? count : -1;\n    }\n\n    if (stream) {'''
    new_ret = '''        return replacement_count > 0 ? replacement_count : -1;\n    }\n\n    if (stream) {'''
    if old_ret not in s:
        raise SystemExit("Found old replacement declaration but not its return site")
    s = s.replace(old_ret, new_ret, 1)
    p.write_text(s)
PY
}

if grep -q 'xemu_audio_packs_apu_override_stream_samples' "$vp_path"; then
    fix_shadow_warning
    echo "Audio stream replacement bridge is already applied."
    exit 0
fi

if command -v git >/dev/null 2>&1 && git apply --check "$patch_path" 2>/dev/null; then
    git apply "$patch_path"
elif command -v patch >/dev/null 2>&1; then
    patch -p1 --forward < "$patch_path"
else
    echo "Neither git nor patch is available to apply the native bridge." >&2
    exit 1
fi

fix_shadow_warning
echo "Applied the minimal audio stream replacement bridge to $vp_path."
