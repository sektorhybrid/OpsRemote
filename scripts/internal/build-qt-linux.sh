#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
# Rebuilds AlmaLinux's Qt base package with Qt's security fixes
# (packaging/qt-linux/patches) and keeps the libraries they change in
# third_party/qt-linux/<version-release>/lib, where the Linux bundle takes
# them from. AlmaLinux's own source package and configuration are used, so
# the result fits the rest of its Qt exactly; only the patches differ. Runs
# as root inside the Linux image (build-qt-linux-docker.sh).
set -euo pipefail

SRC=/src
# shellcheck source=lib/errors.sh
source "${SRC}/scripts/internal/lib/errors.sh"
OWNER="${OWNER:-0:0}"
version="$(rpm -q --qf '%{VERSION}-%{RELEASE}' qt6-qtbase)"
out="${SRC}/third_party/qt-linux/${version}"
# The libraries the patches change: QXmlStreamReader is in Qt Core; the
# Cache-Control, NTLM and HTTP/2 fixes are in Qt Network.
changed=(libQt6Core.so.6 libQt6Network.so.6)

echo "==> Qt base ${version}: fetching AlmaLinux's source package"
dnf -q -y install rpm-build dnf-plugins-core > /dev/null
work="$(mktemp -d)"
on_exit_remove "${work}"
cd "${work}"
dnf -q download --source "qt6-qtbase-${version}"
rpm -i --define "_topdir ${work}/rpmbuild" qt6-qtbase-*.src.rpm
spec="${work}/rpmbuild/SPECS/qt6-qtbase.spec"

echo "==> Adding the patches"
number=900
while IFS= read -r patch; do
    cp "${patch}" "${work}/rpmbuild/SOURCES/"
    # After AlmaLinux's own patches; %autosetup -p1 applies them all in
    # order, with no fuzz: a patch applies exactly or the build stops.
    last="$(grep -n '^Patch[0-9]*:' "${spec}" | tail -n 1 | cut -d: -f1)"
    sed -i "${last}a Patch${number}:  $(basename "${patch}")" "${spec}"
    echo "    ${number}: $(basename "${patch}")"
    number=$((number + 1))
done < <(find "${SRC}/packaging/qt-linux/patches" -name '*.patch' | sort)

echo "==> Installing what the build needs"
dnf -q -y builddep "${spec}" > /dev/null

echo "==> Building (this takes a while)"
# Prepare and build only (-bc): the libraries come from the build folder.
if ! rpmbuild -bc --nocheck --define "_topdir ${work}/rpmbuild" "${spec}" > "${work}/build.log" 2>&1; then
    tail -n 60 "${work}/build.log" >&2
    die "the Qt build failed (the end of its log is above)"
fi

echo "==> Keeping the changed libraries in third_party/qt-linux/${version}"
mkdir -p "${out}/lib"
for library in "${changed[@]}"; do
    # The real file (libQt6Core.so.6.10.3), kept under its soname.
    found="$(find "${work}/rpmbuild/BUILD" -name "${library}.*" -type f -not -name '*.debug' | sort | awk 'NR == 1')"
    [[ -n "${found}" ]] || die "${library} is missing from the build"
    install -m 0755 "${found}" "${out}/lib/${library}"
    strip --strip-unneeded "${out}/lib/${library}"
    # The build folder's search path (rpm's install step would remove it):
    # it would make the loader look past the package's lib/.
    patchelf --remove-rpath "${out}/lib/${library}"
done
# What these libraries were built from, to tell when they are out of date.
{
    echo "qt: qt6-qtbase ${version}"
    (cd "${SRC}" && sha256sum packaging/qt-linux/patches/*.patch)
} > "${out}/BUILT-FROM.txt"
chown -R "${OWNER}" "${SRC}/third_party/qt-linux"
echo "==> Done: ${out}"
