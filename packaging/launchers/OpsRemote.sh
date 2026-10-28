#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
# Starts OpsRemote on Linux. Sessions, settings, and the credential
# vault are shared with the Windows version in the settings/ folder next to
# this script.
#
# The program is stored compressed (linux-x86_64.tar.zst). The first start
# of a version extracts it into runtime/ next to this script; later starts
# reuse that copy, and copies of older versions are removed.
here="$(dirname -- "$(readlink -f -- "$0")")"
platform=linux-x86_64

fail() {
    echo "OpsRemote: $1" >&2
    exit 1
}

if [ -f "${here}/${platform}.tar.zst" ]; then
    id="$(cat "${here}/${platform}.id" 2>/dev/null)"
    [ -n "${id}" ] || fail "${platform}.id is missing. Extract the whole archive again."
    runtime="${here}/runtime/${platform}-${id}"
    if [ ! -x "${runtime}/opsremote.sh" ]; then
        mkdir -p "${here}/runtime" 2>/dev/null \
            || fail "cannot write to ${here}. OpsRemote unpacks itself next to this script, so the folder must be writable."
        zstd="${here}/tools/zstd-${platform}"
        if [ ! -x "${zstd}" ]; then
            # A copy without execute permission (some file systems), or none.
            zstd="$(command -v zstd)" \
                || fail "tools/zstd-${platform} is missing or not executable. Extract the whole archive again."
        fi
        echo "OpsRemote: preparing this version (first start only)..." >&2
        partial="${runtime}.partial.$$"
        rm -rf "${partial}"
        mkdir -p "${partial}" || fail "cannot create ${partial}."
        if ! "${zstd}" -dcq "${here}/${platform}.tar.zst" | tar -xf - -C "${partial}"; then
            rm -rf "${partial}"
            fail "could not unpack ${platform}.tar.zst (is the disk full?)."
        fi
        # Another start may have finished first; either copy is complete.
        mv "${partial}" "${runtime}" 2>/dev/null || rm -rf "${partial}"
        [ -x "${runtime}/opsremote.sh" ] || fail "unpacking ${platform}.tar.zst failed."
    fi
    # Running programs keep their open files on Linux, so older copies can go.
    for old in "${here}/runtime/${platform}-"*; do
        [ "${old}" = "${runtime}" ] || rm -rf "${old}"
    done
    # Earlier packages unpacked into a hidden .runtime folder.
    rm -rf "${here}/.runtime/${platform}-"*
    rmdir "${here}/.runtime" 2>/dev/null || true
    app="${runtime}"
else
    # An uncompressed package (make-package.sh --uncompressed).
    app="${here}/${platform}"
    [ -x "${app}/opsremote.sh" ] \
        || fail "the Linux program (${platform}.tar.zst) is missing from this folder."
fi

OPSREMOTE_HOME="${here}"
export OPSREMOTE_HOME
# The platform launcher checks the system's libraries before starting.
exec "${app}/opsremote.sh" "$@"
