#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Populate the pinned, official Capstone source used by the Joshua-1248 xemu fork.
set -eu

VERSION='5.0.9'
TAG_COMMIT='022575848782a4801fd150fdbc927effcbca0864'
ARCHIVE_SHA256='1b70351879f6998998ebcbe09bd5f3c5e27127e985af14722cbe52c11c35178e'
URL="https://github.com/capstone-engine/capstone/releases/download/${VERSION}/capstone-${VERSION}.tar.xz"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$SCRIPT_DIR/capstone/upstream"
TMP_BASE="${TMPDIR:-/tmp}/xemu-capstone-vendor.$$"
ARCHIVE="$TMP_BASE/capstone-${VERSION}.tar.xz"
EXTRACT="$TMP_BASE/extract"
NEW="$TMP_BASE/upstream.new"
trap 'rm -rf "$TMP_BASE"' EXIT HUP INT TERM
mkdir -p "$TMP_BASE" "$EXTRACT"

if [ "$#" -gt 1 ]; then
    echo "usage: $0 [capstone-5.0.9.tar.xz]" >&2
    exit 2
fi
LOCAL_ARCHIVE=${1:-}

verify_sha256() {
    file=$1
    if command -v sha256sum >/dev/null 2>&1; then
        actual=$(sha256sum "$file" | awk '{print $1}')
    elif command -v shasum >/dev/null 2>&1; then
        actual=$(shasum -a 256 "$file" | awk '{print $1}')
    else
        echo 'error: sha256sum or shasum is required to verify Capstone' >&2
        exit 1
    fi
    if [ "$actual" != "$ARCHIVE_SHA256" ]; then
        echo "error: Capstone archive SHA-256 mismatch" >&2
        echo " expected: $ARCHIVE_SHA256" >&2
        echo "      got: $actual" >&2
        exit 1
    fi
}

validate_tree() {
    tree=$1
    [ -f "$tree/LICENSE.TXT" ] || return 1
    [ -f "$tree/cs.c" ] || return 1
    [ -f "$tree/include/capstone/capstone.h" ] || return 1
    [ -f "$tree/arch/X86/X86Module.c" ] || return 1
    grep -q '^#define CS_VERSION_MAJOR CS_API_MAJOR' "$tree/include/capstone/capstone.h" || return 1
    grep -q '^#define CS_VERSION_EXTRA 9' "$tree/include/capstone/capstone.h" || return 1
    return 0
}

if [ -d "$DEST" ] && validate_tree "$DEST"; then
    echo "Capstone ${VERSION} source is already populated at:"
    echo "  $DEST"
    exit 0
fi

if [ -n "$LOCAL_ARCHIVE" ]; then
    [ -f "$LOCAL_ARCHIVE" ] || { echo "error: archive not found: $LOCAL_ARCHIVE" >&2; exit 1; }
    cp "$LOCAL_ARCHIVE" "$ARCHIVE"
elif command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 --retry-delay 1 -o "$ARCHIVE" "$URL"
elif command -v wget >/dev/null 2>&1; then
    wget -O "$ARCHIVE" "$URL"
else
    echo 'error: curl or wget is required, or pass a pre-downloaded official archive' >&2
    exit 1
fi

verify_sha256 "$ARCHIVE"
tar -xJf "$ARCHIVE" -C "$EXTRACT"
SRC="$EXTRACT/capstone-${VERSION}"
if ! validate_tree "$SRC"; then
    echo 'error: verified archive did not contain the expected Capstone 5.0.9 source layout' >&2
    exit 1
fi

mkdir -p "$NEW"
cp -a "$SRC"/. "$NEW"/
# Commit the exact official release source; do not patch files under upstream/.
rm -rf "$DEST.old"
if [ -e "$DEST" ]; then
    mv "$DEST" "$DEST.old"
fi
mkdir -p "$(dirname "$DEST")"
mv "$NEW" "$DEST"
rm -rf "$DEST.old"

echo "Vendored official Capstone ${VERSION}."
echo "Tag commit: $TAG_COMMIT"
echo "Archive SHA-256: $ARCHIVE_SHA256"
echo "Destination: $DEST"
echo
echo 'Next: git add xemu-features/dependencies/capstone/upstream'
