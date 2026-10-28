#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
# Assembles the combined Linux + Windows package, dist/OpsRemote/, from
# the single-platform bundles built by make-bundle-docker.sh and
# make-bundle-windows-docker.sh. Both platforms share one settings/ folder,
# so the same sessions, vault, and preferences work on either OS.
#
# Each platform's program is stored as <platform>.tar.zst plus <platform>.id
# (part of the archive's hash). The launchers extract it once into
# runtime/<platform>-<id>/ next to themselves and reuse that copy until the
# archive changes; settings/ and data/ always stay outside. --uncompressed
# keeps plain <platform>/ folders instead (quicker while developing); the
# launchers handle both layouts.
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
DIST_DIR="${DIST_DIR:-${PROJECT_DIR}/dist}"
# The platform bundles and tools the package is assembled from.
STAGE_DIR="${STAGE_DIR:-${PROJECT_DIR}/build/stage}"
PACKAGE_NAME="OpsRemote"
PACKAGE="${DIST_DIR}/${PACKAGE_NAME}"
LINUX_BUNDLE="${STAGE_DIR}/OpsRemote-linux-x86_64"
WINDOWS_BUNDLE="${STAGE_DIR}/OpsRemote-windows-x86_64"
STATE_FILES=(sessions.yaml settings.yaml credentials.vault known_hosts color-schemes)
# User state kept from an earlier package, next to settings/.
KEPT_FOLDERS=(settings backups)

make_archive=1
compress=1
for argument in "$@"; do
    case "${argument}" in
        --no-archive) make_archive=0 ;;
        --uncompressed) compress=0 ;;
        *) echo "Usage: $0 [--no-archive] [--uncompressed]" >&2; exit 2 ;;
    esac
done

platforms=()
[[ -x "${LINUX_BUNDLE}/opsremote" ]] && platforms+=(linux)
[[ -f "${WINDOWS_BUNDLE}/OpsRemote.exe" ]] && platforms+=(windows)
if (( ${#platforms[@]} == 0 )); then
    echo "No platform bundle found in ${STAGE_DIR}; build one first." >&2
    exit 1
fi
if (( compress )) && ! command -v zstd > /dev/null; then
    echo "zstd is needed to compress the package (or use --uncompressed)." >&2
    exit 1
fi

echo "==> Assembling ${PACKAGE} (${platforms[*]})"
# Keep the shared user state (and the unpacked runtimes, which are reused
# when unchanged) from an earlier package.
saved_state="$(mktemp -d "${DIST_DIR}/.state.XXXXXX")"
for item in "${KEPT_FOLDERS[@]}" runtime; do
    [[ -d "${PACKAGE}/${item}" ]] && mv "${PACKAGE}/${item}" "${saved_state}/${item}"
done
rm -rf "${PACKAGE}"
mkdir -p "${PACKAGE}"
for item in "${KEPT_FOLDERS[@]}" runtime; do
    [[ -d "${saved_state}/${item}" ]] && mv "${saved_state}/${item}" "${PACKAGE}/${item}"
done
rmdir "${saved_state}"
if [[ ! -d "${PACKAGE}/settings" ]]; then
    # First package: start from the Linux bundle's state, then the Windows
    # bundle's for anything missing; host keys trusted on either are merged.
    mkdir -p "${PACKAGE}/settings"
    for bundle in "${LINUX_BUNDLE}" "${WINDOWS_BUNDLE}"; do
        for item in "${STATE_FILES[@]}"; do
            [[ -e "${bundle}/settings/${item}" && ! -e "${PACKAGE}/settings/${item}" ]] \
                && cp -rp "${bundle}/settings/${item}" "${PACKAGE}/settings/"
        done
        if [[ -f "${bundle}/settings/known_hosts" ]]; then
            while IFS= read -r line; do
                grep -qxF -- "${line}" "${PACKAGE}/settings/known_hosts" \
                    || printf '%s\n' "${line}" >> "${PACKAGE}/settings/known_hosts"
            done < <(tr -d '\r' < "${bundle}/settings/known_hosts")
        fi
    done
    [[ -f "${PACKAGE}/settings/known_hosts" ]] && chmod 0600 "${PACKAGE}/settings/known_hosts"
    [[ -f "${PACKAGE}/settings/credentials.vault" ]] && chmod 0600 "${PACKAGE}/settings/credentials.vault"
fi

# Shared resources; both bundles ship the same data/.
source_data="${LINUX_BUNDLE}/data"
[[ -d "${source_data}" ]] || source_data="${WINDOWS_BUNDLE}/data"
cp -r "${source_data}" "${PACKAGE}/data"

# Licences: OpsRemote's own, and each platform's bundled components (their
# licence texts also travel inside the program archives).
for file in LICENSE AUTHORS THIRD_PARTY.md; do
    install -m 0644 "${PROJECT_DIR}/${file}" "${PACKAGE}/${file}"
done

add_platform() {
    local platform="$1" bundle="$2" tool="$3"
    local staging="${DIST_DIR}/.package-${platform}"
    rm -rf "${staging}"
    mkdir -p "${staging}"
    (cd "${bundle}" && tar --exclude=./data --exclude=./settings -cf - .) | tar -C "${staging}" -xf -
    if [[ -d "${bundle}/licenses" ]]; then
        mkdir -p "${PACKAGE}/licenses"
        cp -r "${bundle}/licenses" "${PACKAGE}/licenses/${platform}"
    fi
    if (( ! compress )); then
        mv "${staging}" "${PACKAGE}/${platform}"
        return
    fi
    if [[ ! -x "${STAGE_DIR}/tools/${tool}" ]]; then
        echo "Missing ${STAGE_DIR}/tools/${tool}; rebuild the ${platform} bundle with Docker." >&2
        exit 1
    fi
    echo "==> Compressing ${platform}"
    # Sorted and owner-neutral, so the same files give the same archive (and
    # the same .id, which keeps an unpacked copy valid).
    tar -C "${staging}" --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 -cf - . \
        | zstd -q -19 -T0 -o "${PACKAGE}/${platform}.tar.zst"
    sha256sum "${PACKAGE}/${platform}.tar.zst" | cut -c1-16 > "${PACKAGE}/${platform}.id"
    install -D -m 0755 "${STAGE_DIR}/tools/${tool}" "${PACKAGE}/tools/${tool}"
    rm -rf "${staging}"
}

if [[ " ${platforms[*]} " == *" linux "* ]]; then
    add_platform linux-x86_64 "${LINUX_BUNDLE}" zstd-linux-x86_64
    install -m 0755 "${PROJECT_DIR}/packaging/launchers/OpsRemote.sh" "${PACKAGE}/"
fi

if [[ " ${platforms[*]} " == *" windows "* ]]; then
    add_platform windows-x86_64 "${WINDOWS_BUNDLE}" zstd-windows-x86_64.exe
    sed 's/$/\r/' "${PROJECT_DIR}/packaging/launchers/OpsRemote.bat" \
        > "${PACKAGE}/OpsRemote.bat"
fi

sed 's/$/\r/' > "${PACKAGE}/README.txt" <<EOF
OpsRemote for Linux and Windows (x86_64)

Start it from this folder, without installation or administrator rights:
  Linux:    ./OpsRemote.sh
  Windows:  OpsRemote.bat

Both platforms share the settings/ folder (sessions, preferences, known SSH
hosts, and the encrypted credential vault), so the whole folder can be
carried on a USB stick and used on either system. data/ holds shared
resources (color schemes, icon themes, keyboard layouts).

The programs are stored compressed (*.tar.zst). The first start of each
version unpacks it into the runtime folder here, which takes a few
seconds; later starts reuse it. Keep this folder writable.

OpsRemote is free software by Thanos Athanasopoulos, under the GNU GPL
version 3 or later (LICENSE). It includes third-party components under their
own licences: see THIRD_PARTY.md and the licenses/ folder. Source code:
https://github.com/sektorhybrid/OpsRemote

Included: ${platforms[*]}
  Linux:    glibc 2.34 or newer, an X11 or Wayland desktop
  Windows:  64-bit Windows 10 version 1809 or newer, or Windows 11
EOF

size="$(du -sh --exclude=runtime "${PACKAGE}" | cut -f1)"
echo "==> Package ready: ${PACKAGE} (${size})"
if (( make_archive )); then
    rm -f "${DIST_DIR}/${PACKAGE_NAME}.zip"
    # Never ship this machine's settings, backups, or unpacked runtimes.
    (cd "${DIST_DIR}" && zip -qr -9 -y "${PACKAGE_NAME}.zip" "${PACKAGE_NAME}" \
        -x "${PACKAGE_NAME}/settings/*" "${PACKAGE_NAME}/backups/*" "${PACKAGE_NAME}/runtime/*")
    echo "==> Archive: ${DIST_DIR}/${PACKAGE_NAME}.zip ($(du -sh "${DIST_DIR}/${PACKAGE_NAME}.zip" | cut -f1))"
fi
