#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "${ROOT}"

python3 xemu-features/chd/tests/source-contract.py
python3 xemu-features/chd/tests/hunk-slice-fuzz.py
python3 xemu-features/chd/tests/cache-policy-test.py
python3 -m py_compile \
    xemu-features/chd/tests/source-contract.py \
    xemu-features/chd/tests/hunk-slice-fuzz.py \
    xemu-features/chd/tests/cache-policy-test.py \
    xemu-features/chd/tests/verify-createdvd-roundtrip.py \
    scripts/gen-license.py
bash -n build.sh xemu-features/dependencies/libchdr/vendor-libchdr.sh

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

# core.cc deliberately stays C++-only and talks to QEMU through the narrow C
# bridge.  A tiny settings stub is enough to syntax-check the complete feature
# core with strict warnings, catching hot-path edits without needing a QEMU
# configure/build in this standalone suite.
mkdir -p "${tmp}/stub/ui"
cat > "${tmp}/stub/ui/xemu-settings.h" <<'SRC'
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
const char *xemu_settings_get_base_path(void);
#ifdef __cplusplus
}
#endif
SRC
cat > "${tmp}/stub/config-host.h" <<'SRC'
#define CONFIG_XEMU_FEATURE_DISC_MODDING 1
SRC
TERM=dumb "${CXX:-c++}" -std=gnu++17 -Wall -Wextra -Werror -fsyntax-only \
    -I"${tmp}/stub" -I. xemu-features/disc-modding/core.cc
echo "PASS: Disc Files & Mods core strict C++ syntax"

for enabled in 0 1; do
    mkdir -p "${tmp}/cfg-${enabled}"
    if [[ "${enabled}" == 1 ]]; then
        printf '#define CONFIG_XEMU_FEATURE_CHD 1\n' > "${tmp}/cfg-${enabled}/config-host.h"
    else
        : > "${tmp}/cfg-${enabled}/config-host.h"
    fi
    cat > "${tmp}/path-test.c" <<'SRC'
#include "xemu-features/chd/chd-path.h"
#include <assert.h>
#include <string.h>
int main(void)
{
#ifdef CONFIG_XEMU_FEATURE_CHD
    assert(xemu_chd_support_enabled());
    assert(xemu_chd_path_is_chd("game.chd"));
    assert(xemu_chd_path_is_chd("game.CHD"));
    assert(xemu_chd_path_is_chd("dir.with.dot/game.ChD"));
    assert(!xemu_chd_path_is_chd("game.iso"));
    assert(strcmp(xemu_chd_block_format_for_path("game.chd"), "chd") == 0);
#else
    assert(!xemu_chd_support_enabled());
    assert(!xemu_chd_path_is_chd("game.chd"));
    assert(strcmp(xemu_chd_block_format_for_path("game.chd"), "raw") == 0);
#endif
    return 0;
}
SRC
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -I"${tmp}/cfg-${enabled}" -I. "${tmp}/path-test.c" \
        -o "${tmp}/path-test-${enabled}"
    "${tmp}/path-test-${enabled}"
done

echo "PASS: CHD enabled/disabled frontend path helper"

"${CXX:-c++}" -std=gnu++17 -O2 -Wall -Wextra -Werror -I. \
    xemu-features/disc-modding/xdvdfs.cc \
    xemu-features/chd/tests/xdvdfs-logical-reader-test.cc \
    -o "${tmp}/xdvdfs-reader-test"
"${tmp}/xdvdfs-reader-test" "${tmp}/reader-test.xiso"

echo "PASS: all CHD static/standalone tests"
