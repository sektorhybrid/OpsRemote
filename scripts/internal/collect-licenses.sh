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
OUT="${BUNDLE}/licenses"

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
    while IFS= read -r file; do
        [[ -f "${file}" ]] || continue
        mkdir -p "${OUT}/${package}"
        cp "${file}" "${OUT}/${package}/"
    done < <(rpm -qL "${package}" 2>/dev/null | grep -v '^(contains no files)$' || true)
    if [[ -d "${OUT}/${package}" ]]; then
        return 0
    fi
    source="$(rpm -q --queryformat '%{SOURCERPM}\n' "${package}" 2>/dev/null | head -n 1 \
                | sed -E 's/-[^-]+-[^-]+\.src\.rpm$//' || true)"
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

# The build images leave out documentation, and some packages declare no
# licence files; their source package (downloaded from the distribution)
# has them.
from_source_package() {
    local package="$1" source work archive
    source="$(rpm -q --queryformat '%{SOURCERPM}\n' "${package}" 2>/dev/null | head -n 1 || true)"
    [[ -n "${source}" ]] || return 0
    work="$(mktemp -d)"
    # The build runs as the calling user, so dnf gets a writable home for its
    # cache; </dev/null keeps it off the file list this runs inside of.
    if (cd "${work}" && HOME="${work}" dnf -q download --source "${source%.src.rpm}" \
            </dev/null >/dev/null 2>&1); then
        (cd "${work}" && rpm2cpio ./*.src.rpm | cpio -idm --quiet 2>/dev/null) || true
        mkdir -p "${work}/unpacked"
        for archive in "${work}"/*.tar.* "${work}"/*.tgz; do
            [[ -f "${archive}" ]] && tar -C "${work}/unpacked" -xf "${archive}" 2>/dev/null || true
        done
        while IFS= read -r -d '' file; do
            mkdir -p "${OUT}/${package}"
            cp -n "${file}" "${OUT}/${package}/" || true
        done < <(find "${work}" -maxdepth 3 -type f \( -iname '*licen[cs]e*' -o -iname 'copying*' \
                    -o -iname 'notice*' -o -iname 'copyright*' \) ! -name '*.spec' -print0)
    fi
    rm -rf "${work}"
}

# The package (rpm) a bundled file came from, if any.
owner_of() {
    # rpm reports "not owned" on standard output, with a failure status.
    local owner
    if owner="$(rpm -qf --queryformat '%{NAME}\n' "$1" 2>/dev/null)"; then
        echo "${owner}" | head -n 1
    fi
    return 0
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
# Windows libssh and utf8proc).
if [[ -d /opt/licenses ]]; then
    cp -r /opt/licenses/. "${OUT}/"
fi

# QTermWidget, built from third_party/ with OpsRemote's changes.
mkdir -p "${OUT}/qtermwidget"
cp "${PROJECT_DIR}/third_party/qtermwidget/"{LICENSE,LICENSE.LGPL2+,LICENSE.BSD-3-clause,AUTHORS} \
    "${PROJECT_DIR}/third_party/qtermwidget/OPSREMOTE-CHANGES.md" "${OUT}/qtermwidget/"

echo "==> Licence texts: $(find "${OUT}" -mindepth 1 -maxdepth 1 -type d | wc -l) components in ${OUT}"
