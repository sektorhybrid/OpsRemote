#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
#
# Makes dist/OpsRemote-<version>-source.tar.xz, the Corresponding Source of a
# release, to upload next to its packages (GPLv3 section 6):
#   opsremote/  this repository at the given commit (with the modified
#               QTermWidget and Kvantum in third_party/);
#   upstream/   the exact source archives the Docker images build from
#               (versions and SHA-256 checksums from packaging/*/Dockerfile).
# Libraries taken from AlmaLinux/Fedora packages are listed with their source
# packages in each package's licenses/DISTRIBUTION-PACKAGES.txt.
#
#   make-source-archive.sh [commit or tag, default HEAD]
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
REF="${1:-HEAD}"
VERSION="$(sed -n 's/^project(OpsRemote VERSION \([0-9.]*\).*/\1/p' "${PROJECT_DIR}/CMakeLists.txt")"
NAME="OpsRemote-${VERSION}-source"
OUT="${PROJECT_DIR}/dist/${NAME}.tar.xz"

if [[ "${REF}" == HEAD ]] && [[ -n "$(git -C "${PROJECT_DIR}" status --porcelain)" ]]; then
    echo "warning: uncommitted changes are not included; the archive has ${REF} as committed" >&2
fi

# An ARG value from the Dockerfiles (the same in both when shared).
arg() {
    sed -n "s/^ARG $1=//p" "${PROJECT_DIR}"/packaging/*/Dockerfile | head -n 1
}

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
mkdir -p "${work}/${NAME}/upstream"

echo "==> OpsRemote at ${REF}"
git -C "${PROJECT_DIR}" archive --format=tar --prefix="${NAME}/opsremote/" "${REF}" | tar -C "${work}" -x
commit="$(git -C "${PROJECT_DIR}" rev-parse "${REF}")"

# Downloads an upstream archive and checks it against the Dockerfile's checksum.
fetch() {
    local url="$1" sha256="$2" file="${work}/${NAME}/upstream/$3"
    echo "==> ${3}"
    curl -fsSL -o "${file}" "${url}"
    echo "${sha256}  ${file}" | sha256sum -c --quiet -
}

freerdp="$(arg FREERDP_VERSION)"
zstd="$(arg ZSTD_VERSION)"
libvnc="$(arg LIBVNC_VERSION)"
libssh="$(arg LIBSSH_VERSION)"
utf8proc="$(arg UTF8PROC_VERSION)"
fetch "https://github.com/FreeRDP/FreeRDP/releases/download/${freerdp}/freerdp-${freerdp}.tar.gz" \
    "$(arg FREERDP_SHA256)" "freerdp-${freerdp}.tar.gz"
fetch "https://github.com/facebook/zstd/releases/download/v${zstd}/zstd-${zstd}.tar.gz" \
    "$(arg ZSTD_SHA256)" "zstd-${zstd}.tar.gz"
fetch "https://github.com/LibVNC/libvncserver/archive/refs/tags/LibVNCServer-${libvnc}.tar.gz" \
    "$(arg LIBVNC_SHA256)" "libvncserver-${libvnc}.tar.gz"
fetch "https://www.libssh.org/files/${libssh%.*}/libssh-${libssh}.tar.xz" \
    "$(arg LIBSSH_SHA256)" "libssh-${libssh}.tar.xz"
fetch "https://github.com/JuliaStrings/utf8proc/archive/refs/tags/v${utf8proc}.tar.gz" \
    "$(arg UTF8PROC_SHA256)" "utf8proc-${utf8proc}.tar.gz"

cat > "${work}/${NAME}/README.txt" <<TEXT
OpsRemote ${VERSION}: Corresponding Source

opsremote/   OpsRemote at commit ${commit},
             including the modified QTermWidget and Kvantum in third_party/
             (their changes are described in OPSREMOTE-CHANGES.md and
             OPSREMOTE-CHANGES.patch there). Build instructions: README.md.
upstream/    The source archives of the libraries built for the packages,
             exactly as downloaded by packaging/linux/Dockerfile and
             packaging/windows/Dockerfile (checksums verified):
               FreeRDP ${freerdp}, zstd ${zstd}, LibVNCServer ${libvnc},
               libssh ${libssh} (Windows), utf8proc ${utf8proc} (Windows)

The other libraries in the packages come from AlmaLinux 9 (Linux) and
Fedora's MinGW packages (Windows). Each package lists them with their exact
source packages in licenses/DISTRIBUTION-PACKAGES.txt; see SOURCES.txt in
the package for where to get those sources.
TEXT

mkdir -p "${PROJECT_DIR}/dist"
echo "==> Writing ${OUT}"
tar -C "${work}" -cJf "${OUT}" "${NAME}"
echo "==> Source archive ready: ${OUT} ($(du -h "${OUT}" | cut -f1))"
