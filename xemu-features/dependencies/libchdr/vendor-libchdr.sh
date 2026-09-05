#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

VERSION="0.3.0"
TAG="v${VERSION}"
COMMIT="93d8c239ff0d4e8d7722985992649fce12d2463b"
SHA512="db86bd9d604bc8623231cfed7f6eb21b035f70c7a8748a4f80ce7e9a3610f8898f7db5fb75a707a357857970a2104d0e6e71d6953bf359001eb763851e248ac4"
URLS=(
    "https://codeload.github.com/rtissera/libchdr/tar.gz/${TAG}"
    "https://distfiles.gentoo.org/distfiles/36/libchdr-${VERSION}.tar.gz"
)

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="${SELF_DIR}/upstream"

if [[ -f "${DEST}/src/libchdr_chd.c" && -f "${DEST}/CMakeLists.txt" ]]; then
    if grep -q 'project(chdr VERSION 0.3.0' "${DEST}/CMakeLists.txt"; then
        printf 'Pinned libchdr %s is already materialized.\n' "${TAG}"
        exit 0
    fi
fi

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
archive="${tmp}/libchdr-${TAG}.tar.gz"

printf 'Fetching libchdr %s (%s)...\n' "${TAG}" "${COMMIT}"
if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
    echo 'error: curl or wget is required to materialize the pinned libchdr source.' >&2
    exit 1
fi

fetched=0
for url in "${URLS[@]}"; do
    printf '  trying %s\n' "${url}"
    rm -f "${archive}"
    if command -v curl >/dev/null 2>&1; then
        if curl -fL --retry 3 --connect-timeout 20 -o "${archive}" "${url}"; then
            fetched=1
            break
        fi
    elif wget -O "${archive}" "${url}"; then
        fetched=1
        break
    fi
done
if [[ "${fetched}" != 1 ]]; then
    echo 'error: could not fetch the pinned libchdr archive from any verified source.' >&2
    exit 1
fi

if command -v sha512sum >/dev/null 2>&1; then
    actual="$(sha512sum "${archive}" | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
    actual="$(shasum -a 512 "${archive}" | awk '{print $1}')"
else
    echo 'error: sha512sum or shasum is required to verify libchdr.' >&2
    exit 1
fi
if [[ "${actual}" != "${SHA512}" ]]; then
    echo 'error: libchdr archive SHA-512 mismatch.' >&2
    echo "expected: ${SHA512}" >&2
    echo "actual:   ${actual}" >&2
    exit 1
fi

tar -xzf "${archive}" -C "${tmp}"
src="$(find "${tmp}" -mindepth 1 -maxdepth 1 -type d -name 'libchdr-*' | head -n 1)"
if [[ -z "${src}" || ! -f "${src}/src/libchdr_chd.c" ]]; then
    echo 'error: verified archive did not contain the expected libchdr source tree.' >&2
    exit 1
fi

rm -rf "${DEST}.new"
mv "${src}" "${DEST}.new"
rm -rf "${DEST}"
mv "${DEST}.new" "${DEST}"
printf '%s\n' "version=${VERSION}" "tag=${TAG}" "commit=${COMMIT}" "archive_sha512=${SHA512}" > "${SELF_DIR}/UPSTREAM_VERSION.txt"
printf 'Materialized verified libchdr %s under %s\n' "${TAG}" "${DEST}"
