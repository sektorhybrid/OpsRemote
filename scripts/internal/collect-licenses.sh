#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
#
# Fills <bundle>/licenses/ with the licence texts of everything the bundle
# ships, as their licences require for binary distribution:
#   - libraries from the build image's packages: the package's licence files
#     (found through rpm, from /usr/share/licenses or /usr/share/doc);
#   - libraries built from source in the image: /opt/licenses/<name>/;
#   - the bundled QTermWidget.
# The icon themes and colour schemes carry their own licence files in data/.
# Runs inside the Docker build images.
#
#   collect-licenses.sh <bundle folder> <linux|windows> [sysroot]
set -euo pipefail

BUNDLE="$1"
PLATFORM="$2"
SYSROOT="${3:-}"
PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=lib/errors.sh
source "${PROJECT_DIR}/scripts/internal/lib/errors.sh"
OUT="${BUNDLE}/licenses"
DISTRIBUTION="$( . /etc/os-release && echo "${PRETTY_NAME:-}")"

rm -rf "${OUT}"
mkdir -p "${OUT}"

# Copies a package's licence files into licenses/<package>/: the files the
# package declares as licences, else those in its licence or doc folder, or
# (for sub-packages such as qt6-qtbase-gui) its source package's.
copy_package() {
    local package="$1" folder file source
    if [[ -d "${OUT}/${package}" ]]; then
        return 0
    fi
    # The exact source package, for SOURCES.txt (see the written offer there).
    rpm -q --queryformat '%{NAME}-%{VERSION}-%{RELEASE}  (source: %{SOURCERPM})\n' "${package}" \
        >> "${OUT}/.packages"
    local declared listed
    declared="$(rpm -qL "${package}")"
    # 1: the package declares no licence files.
    capture listed 1 -- grep -v '^(contains no files)$' <<< "${declared}"
    while IFS= read -r file; do
        [[ -f "${file}" ]] || continue
        mkdir -p "${OUT}/${package}"
        cp "${file}" "${OUT}/${package}/"
    done <<< "${listed}"
    if [[ -d "${OUT}/${package}" ]]; then
        return 0
    fi
    source="$(source_package_of "${package}")"
    source="$(sed -E 's/-[^-]+-[^-]+\.src\.rpm$//' <<< "${source}")"
    for folder in "/usr/share/licenses/${package}" "/usr/share/doc/${package}" \
                  ${source:+"/usr/share/licenses/${source}" "/usr/share/doc/${source}"}; do
        [[ -d "${folder}" ]] || continue
        while IFS= read -r -d '' file; do
            mkdir -p "${OUT}/${package}"
            cp "${file}" "${OUT}/${package}/"
        done < <(find "${folder}" -maxdepth 1 -type f \( -iname '*licen[cs]e*' -o -iname 'copying*' \
                    -o -iname 'notice*' -o -iname 'copyright*' -o -iname 'lgpl*' -o -iname 'gpl*' \) -print0)
    done
    if [[ ! -d "${OUT}/${package}" ]]; then
        from_source_package "${package}"
    fi
    if [[ ! -d "${OUT}/${package}" ]]; then
        # Some projects keep their licence only in source file headers: record
        # the package's declared licence and where its source is.
        echo "warning: no licence file for ${package}; writing its package information" >&2
        mkdir -p "${OUT}/${package}"
        rpm -q --queryformat 'Name:    %{NAME}\nVersion: %{VERSION}-%{RELEASE}\nLicence: %{LICENSE}\nURL:     %{URL}\nSource:  %{SOURCERPM}\n' \
            "${package}" > "${OUT}/${package}/PACKAGE-INFO.txt"
    fi
}

# The source package (name-version-release.src.rpm) a package was built from.
source_package_of() {
    local sources
    sources="$(rpm -q --queryformat '%{SOURCERPM}\n' "$1")"
    echo "${sources%%$'\n'*}"
}

# The build images leave out documentation, and some packages declare no
# licence files; their source package (downloaded from the distribution)
# has them.
from_source_package() {
    local package="$1" source work archive
    source="$(source_package_of "${package}")"
    [[ -n "${source}" ]] || return 0
    work="$(mktemp -d)"
    on_exit_remove "${work}"
    # The build runs as the calling user, so dnf gets a writable home for its
    # cache; </dev/null keeps it off the file list this runs inside of.
    # The download is optional (it needs the network): its failure is
    # reported, and the package's information is written instead.
    if (cd "${work}" && HOME="${work}" dnf -q download --source "${source%.src.rpm}" \
            </dev/null >"${work}/download.log" 2>&1); then
        (cd "${work}" && rpm2cpio ./*.src.rpm | cpio -idm --quiet)
        mkdir -p "${work}/unpacked"
        for archive in "${work}"/*; do
            # Source archives only, not their signatures (.asc, .sig).
            if [[ "${archive}" =~ \.(tar\.(gz|xz|bz2|zst)|tgz)$ ]]; then
                tar -C "${work}/unpacked" -xf "${archive}"
            fi
        done
        while IFS= read -r -d '' file; do
            mkdir -p "${OUT}/${package}"
            # The first of several files with one name is kept.
            [[ -e "${OUT}/${package}/$(basename "${file}")" ]] || cp "${file}" "${OUT}/${package}/"
        done < <(find "${work}" -maxdepth 3 -type f \( -iname '*licen[cs]e*' -o -iname 'copying*' \
                    -o -iname 'notice*' -o -iname 'copyright*' \) ! -name '*.spec' -print0)
    else
        echo "skipped: the source package of ${package} (dnf download failed: $(tail -n 1 "${work}/download.log"))" >&2
    fi
}

# The package (rpm) a bundled file came from, if any. A packaged file our
# own build replaced (OpenSSL on Windows) is not that package's: its digest
# no longer matches, and its licence comes from /opt/licenses instead.
owner_of() {
    local owner changes
    # rpm -qf: 1 is "not owned by any package": built from source, or
    # OpsRemote's own (any other failure stops the script).
    capture owner 1 -- rpm -qf --queryformat '%{NAME}\n' "$1"
    if ((TOLERATED_STATUS == 1)); then
        return 0
    fi
    # rpm -V: 1 is "changes found". A file our own build replaced (OpenSSL on
    # Windows) is not the package's: its licence comes from /opt/licenses.
    capture changes 1 -- rpm -Vf "$1"
    if awk -v file="$1" '$1 ~ /^..5/ && $NF == file { found = 1 } END { exit !found }' <<< "${changes}"; then
        return 0
    fi
    echo "${owner%%$'\n'*}"
}

# Where a bundled file came from in the build image.
source_of() {
    local relative="$1" name candidate
    name="$(basename "${relative}")"
    if [[ "${PLATFORM}" == windows ]]; then
        for candidate in "${SYSROOT}/bin/${name}" "${SYSROOT}/lib/qt6/plugins/${relative#plugins/}" \
                         "${SYSROOT}/lib/ossl-modules/${name}"; do
            if [[ -f "${candidate}" ]]; then
                echo "${candidate}"
                return 0
            fi
        done
    else
        for candidate in "/usr/lib64/${name}" "/lib64/${name}" \
                         "/usr/lib64/qt6/plugins/${relative#lib/qt6/plugins/}" \
                         "/usr/lib64/ossl-modules/${name}"; do
            if [[ -e "${candidate}" ]]; then
                readlink -f "${candidate}"
                return 0
            fi
        done
    fi
    # Not from a system package (built from source, or OpsRemote's own).
    return 0
}

while IFS= read -r -d '' file; do
    relative="${file#"${BUNDLE}/"}"
    source="$(source_of "${relative}")"
    [[ -n "${source}" ]] || continue
    package="$(owner_of "${source}")"
    if [[ -n "${package}" ]]; then
        copy_package "${package}"
    fi
done < <(find "${BUNDLE}" -type f \( -name '*.so*' -o -iname '*.dll' \) -print0)

# Built from source in the image (FreeRDP, libvncserver, zstd, and on
# Windows OpenSSL, libssh and utf8proc).
if [[ -d /opt/licenses ]]; then
    cp -r /opt/licenses/. "${OUT}/"
fi

# LibVNCServer carries upstream fixes applied by OpsRemote: its change note.
mkdir -p "${OUT}/libvncserver"
cp "${PROJECT_DIR}/packaging/libvncserver/OPSREMOTE-CHANGES.md" "${OUT}/libvncserver/"

# QTermWidget, built from third_party/ with OpsRemote's changes.
mkdir -p "${OUT}/qtermwidget"
cp "${PROJECT_DIR}/third_party/qtermwidget/"{LICENSE,LICENSE.LGPL2+,LICENSE.BSD-3-clause,AUTHORS} \
    "${PROJECT_DIR}/third_party/qtermwidget/OPSREMOTE-CHANGES.md" "${OUT}/qtermwidget/"

# The distribution packages the bundle's libraries come from, with their
# source packages.
if [[ -f "${OUT}/.packages" ]]; then
    {
        echo "Libraries in this package taken from ${DISTRIBUTION:-the build image},"
        echo "and the source packages they were built from:"
        echo
        sort -u "${OUT}/.packages"
    } > "${OUT}/DISTRIBUTION-PACKAGES.txt"
    rm -f "${OUT}/.packages"
fi

# Kvantum, compiled into OpsRemote with OpsRemote's changes; its themes are in
# data/Kvantum/.
mkdir -p "${OUT}/kvantum"
cp "${PROJECT_DIR}/third_party/kvantum/COPYING" \
    "${PROJECT_DIR}/third_party/kvantum/OPSREMOTE-CHANGES.md" "${OUT}/kvantum/"

# Colour schemes in data/color-schemes/ (the icon themes keep their licence
# files in their own folders under data/icons/).
mkdir -p "${OUT}/iterm2-color-schemes"
cp "${PROJECT_DIR}/third_party/iterm2-color-schemes/LICENSE" "${OUT}/iterm2-color-schemes/"

echo "==> Licence texts: $(find "${OUT}" -mindepth 1 -maxdepth 1 -type d | wc -l) components in ${OUT}"
