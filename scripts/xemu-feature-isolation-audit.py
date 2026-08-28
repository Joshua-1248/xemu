#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Structural audit for xemu custom-fork Feature Isolation Phase 4."""
from pathlib import Path
import re, sys, tempfile, subprocess

ROOT = Path(__file__).resolve().parents[1]
errors=[]

def read(p):
    q=ROOT/p
    if not q.exists():
        errors.append(f"missing: {p}")
        return ""
    return q.read_text(errors='replace')

def require(cond,msg):
    if not cond: errors.append(msg)

features={
    'audio_packs':'audio-packs',
    'texture_packs':'texture-packs',
    'cheats':'cheats',
    'tas':'tas',
    'scripting':'scripting',
    'debug_tools':'debug-tools',
    'fast_forward':'fast-forward',
    'volume_amplifier':'volume-amplifier',
}
for opt,dirname in features.items():
    require((ROOT/'xemu-features'/dirname).is_dir(), f'missing feature directory: {dirname}')
    require((ROOT/'xemu-features'/dirname/'README.md').is_file(), f'missing per-feature README: {dirname}')
    require((ROOT/'xemu-features'/dirname/'EXPORT_MANIFEST.txt').is_file(), f'missing export manifest: {dirname}')
    require(f"option('xemu_feature_{opt}'" in read('meson_options.txt'), f'missing Meson option: {opt}')
    require(f"CONFIG_XEMU_FEATURE_{opt.upper()}" in read('meson.build'), f'missing config-host flag: {opt}')

require((ROOT/'xemu-features/README.md').is_file(), 'missing top-level xemu-features README')
require((ROOT/'xemu-features/shared/README.md').is_file(), 'missing shared service README')
require((ROOT/'docs/custom-fork/FEATURE_ISOLATION_PHASE4_2026-08-28.md').is_file(), 'missing Phase 4 documentation')
require((ROOT/'docs/custom-fork/PHASE4_CORE_TOUCHPOINTS.txt').is_file(), 'missing Phase 4 core touchpoint inventory')

require(not (ROOT/'ui/xui/features').exists(), 'legacy ui/xui/features still exists')
require(not (ROOT/'hw/xbox/features').exists(), 'legacy hw/xbox/features still exists')
for stale in ['xemu-guestmem.c','xemu-guestmem.h','xemu-dbg.c','xemu-dbg.h']:
    require(not (ROOT/stale).exists(), f'custom helper still at root: {stale}')

# No stale ownership paths in live source/build files.
scan_roots=['meson.build','meson_options.txt','ui','hw','xemu-features']
stale_patterns=['ui/xui/features','hw/xbox/features','include/xemu-tas.h','include/xemu-fast-forward.h',
                'xemu-guestmem.h','xemu-dbg.h']
for sr in scan_roots:
    p=ROOT/sr
    files=[p] if p.is_file() else [x for x in p.rglob('*') if x.suffix in {'.c','.cc','.h','.hh','.build'}]
    for f in files:
        if '.before-' in f.name: continue
        text=f.read_text(errors='replace')
        for pat in stale_patterns:
            if pat in text:
                errors.append(f'stale path {pat}: {f.relative_to(ROOT)}')

# Core must not read texture/audio pack settings directly.
core_files=[]
for base in [ROOT/'hw/xbox/nv2a', ROOT/'hw/xbox/mcpx']:
    core_files += [p for p in base.rglob('*') if p.suffix in {'.c','.h','.cc','.hh'} and 'features' not in p.parts]
for f in core_files:
    t=f.read_text(errors='replace')
    if re.search(r'g_config\.general\.texture_(dump|replace)',t):
        errors.append(f'texture pack config leak in core: {f.relative_to(ROOT)}')
    if re.search(r'g_config\.audio\.(dump|replacement|skip)',t):
        errors.append(f'audio pack config leak in core: {f.relative_to(ROOT)}')

monitor=read('hw/xbox/mcpx/apu/monitor.c')
for marker in ['FF_PITCH_XFADE_FRAMES','ff_pitch_prepare_block','fast_forward_preserve_pitch','2.0))','applied_frequency_ratio','gain_source_volume']:
    require(marker not in monitor, f'APU monitor still owns custom algorithm/state: {marker}')
require('xemu_fast_forward_audio_submit' in monitor, 'APU monitor missing FF audio hook')
require('xemu_volume_amplifier_apply' in monitor, 'APU monitor missing volume hook')
apuint=read('hw/xbox/mcpx/apu/apu_int.h')
for marker in ['gain_source_volume','applied_gain','applied_frequency_ratio']:
    require(marker not in apuint, f'MCPXAPUState still enlarged by custom field: {marker}')

uix=read('ui/xemu.c')
for marker in ['render_divider','last_multiplier','last_unlimited','fast_forward_advance_guest_clock(void)']:
    require(marker not in uix, f'ui/xemu.c still owns FF policy/state: {marker}')
require('xemu_fast_forward_should_render_vblank' in uix, 'ui/xemu.c missing narrow FF timing hook')

# Phase-3 invariant: no custom replacement/animation state in renderer bindings.
for rel in ['hw/xbox/nv2a/pgraph/gl/renderer.h','hw/xbox/nv2a/pgraph/vk/renderer.h']:
    t=read(rel)
    m=re.search(r'typedef struct TextureBinding \{(.*?)\}\s*TextureBinding;',t,re.S)
    require(bool(m), f'cannot locate TextureBinding in {rel}')
    if m:
        body=m.group(1)
        for marker in ['replacement','animated','anim_','shader_program','hot_reload','texture_pack']:
            require(marker not in body, f'custom TextureBinding field marker {marker} in {rel}')

# Generic texture pack implementation must not be owned by GL.
pgraph=read('hw/xbox/nv2a/pgraph/meson.build')
glmes=read('hw/xbox/nv2a/pgraph/gl/meson.build')
require('texture-packs/texture-packs.c' in pgraph, 'renderer-neutral texture pack source not owned by pgraph')
require('texture-packs/texture-packs.c' not in glmes, 'renderer-neutral texture pack source still owned by GL')

# Root custom helpers must be conditional.
rootmes=read('meson.build')
require("specific_ss.add(files('xemu-xbe.c', 'xemu-version.c'))" in rootmes,
        'native xbe/version root ownership changed unexpectedly')
require("xemu-features/shared/guest-memory.c" in rootmes, 'shared guest-memory conditional missing')
require("xemu-features/debug-tools/debug-api.c" in rootmes, 'debug API conditional missing')

# Old phase smoke scripts are invalid after physical consolidation.
for old in ['scripts/xemu-feature-isolation-smoke.py','scripts/xemu-feature-isolation-phase2-smoke.py','scripts/xemu-feature-isolation-phase3-smoke.py']:
    require(not (ROOT/old).exists(), f'stale pre-Phase4 smoke script remains: {old}')

# Syntax-check compile-time-neutral public boundaries with all OFF, all ON,
# and every representative single-feature configuration.
def header_check(enabled, label):
    with tempfile.TemporaryDirectory(prefix='xemu-feature-audit-') as d:
        d=Path(d)
        cfg=d/'config-host.h'
        cfg.write_text(''.join(
            f'#define CONFIG_XEMU_FEATURE_{k.upper()} 1\n'
            for k in features if k in enabled))
        c=d/'test.c'; cc=d/'test.cc'
        c.write_text('''\
#include "xemu-features/audio-packs/audio-packs.h"\n#include "xemu-features/audio-packs/audio-packs-apu.h"\n#include "xemu-features/texture-packs/texture-packs.h"\n#include "xemu-features/texture-packs/texture-packs-gl.h"\n#include "xemu-features/tas/tas.h"\n#include "xemu-features/fast-forward/fast-forward.h"\n#include "xemu-features/fast-forward/timing.h"\n#include "xemu-features/fast-forward/audio.h"\n#include "xemu-features/volume-amplifier/volume.h"\nint main(void){return xemu_fast_forward_mode()==123;}\n''')
        cc.write_text('''\
#include "xemu-features/audio-packs/frontend.hh"\n#include "xemu-features/texture-packs/frontend.hh"\n#include "xemu-features/cheats/runtime.hh"\n#include "xemu-features/tas/studio.hh"\n#include "xemu-features/scripting/frontend.hh"\n#include "xemu-features/debug-tools/frontend.hh"\n#include "xemu-features/fast-forward/frontend.hh"\nint main(){return TasWindowsOpen() || FeatureScriptToolsWindowsOpen();}\n''')
        for cmd in [
            ['cc','-std=gnu11','-fsyntax-only',f'-I{d}',f'-I{ROOT}',str(c)],
            ['c++','-std=gnu++17','-fsyntax-only',f'-I{d}',f'-I{ROOT}',str(cc)],
        ]:
            r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            if r.returncode:
                errors.append(label+' public-header syntax failed:\n'+r.stderr)

header_check(set(), 'ALL-OFF')
header_check(set(features), 'ALL-ON')
for one in features:
    header_check({one}, 'ONLY-'+one.upper())

if errors:
    print('FEATURE ISOLATION PHASE 4 AUDIT: FAIL')
    for e in errors: print(' -',e)
    sys.exit(1)
print('FEATURE ISOLATION PHASE 4 AUDIT: PASS')
print(' - top-level feature ownership present')
print(' - eight independent Meson options/config flags present')
print(' - legacy ownership trees absent')
print(' - FF audio/timing + volume algorithms out of core')
print(' - renderer sidecar invariant preserved')
print(' - pack config reads absent from NV2A/MCPX core')
print(' - shared custom helpers conditionally linked')
print(' - public headers compile all-OFF, all-ON, and each single-feature mode')
