#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
# Rebuilds Fedora's MinGW Qt base package with OpsRemote's patches
# (packaging/qt/patches) and keeps the DLLs they change in
# third_party/qt-windows/<version-release>/bin, where the Windows bundle
# takes them from. Fedora's own source package and configuration are used,
# so the result fits the rest of Fedora's Qt exactly; only the patches
# differ. Runs as root inside the Windows image (build-qt-windows-docker.sh).
set -euo pipefail

SRC=/src
OWNER="${OWNER:-0:0}"
version="$(rpm -q --qf '%{VERSION}-%{RELEASE}' mingw64-qt6-qtbase)"
out="${SRC}/third_party/qt-windows/${version}"
# The DLLs the patches change.
changed=(Qt6Core.dll)

echo "==> Qt base ${version}: fetching Fedora's source package"
dnf -q -y install rpm-build dnf-plugins-core >/dev/null
work="$(mktemp -d)"
cd "${work}"
dnf -q download --source "mingw-qt6-qtbase-${version}"
rpm -i --define "_topdir ${work}/rpmbuild" mingw-qt6-qtbase-*.src.rpm
spec="${work}/rpmbuild/SPECS/mingw-qt6-qtbase.spec"

echo "==> Adding the patches"
number=900
while IFS= read -r patch; do
    cp "${patch}" "${work}/rpmbuild/SOURCES/"
    # After Fedora's own patches; %autosetup -p1 applies them all in order.
    last="$(grep -n '^Patch[0-9]*:' "${spec}" | tail -n 1 | cut -d: -f1)"
    sed -i "${last}a Patch${number}:       $(basename "${patch}")" "${spec}"
    echo "    ${number}: $(basename "${patch}")"
    number=$((number + 1))
done < <(find "${SRC}/packaging/qt/patches" -name '*.patch' | sort)

echo "==> Installing what the build needs"
dnf -q -y builddep "${spec}" >/dev/null

echo "==> Building (64-bit only; this takes a while)"
# Prepare and build only (-bc): the DLLs come from the build folder, so
# Fedora's install and packaging steps (which expect the 32-bit build too)
# are not needed.
rpmbuild -bc --nocheck --define "_topdir ${work}/rpmbuild" --define 'mingw_build_win32 0' \
    "${spec}" > "${work}/build.log" 2>&1 || { tail -n 60 "${work}/build.log"; exit 1; }

echo "==> Keeping the changed DLLs in third_party/qt-windows/${version}"
built="$(find "${work}/rpmbuild/BUILD" -type d -name build_win64 | head -n 1)"
[[ -n "${built}" ]] || { echo "The 64-bit build folder is missing." >&2; exit 1; }
mkdir -p "${out}/bin"
for dll in "${changed[@]}"; do
    found="$(find "${built}" -name "${dll}" | head -n 1)"
    [[ -n "${found}" ]] || { echo "${dll} is missing from the build." >&2; exit 1; }
    install -m 0644 "${found}" "${out}/bin/${dll}"
    x86_64-w64-mingw32-strip --strip-unneeded "${out}/bin/${dll}"
done
# What these DLLs were built from, to tell when they are out of date.
{
    echo "qt: mingw64-qt6-qtbase ${version}"
    (cd "${SRC}" && sha256sum packaging/qt/patches/*.patch)
} > "${out}/BUILT-FROM.txt"
chown -R "${OWNER}" "${SRC}/third_party/qt-windows"
rm -rf "${work}"
echo "==> Done: ${out}"
