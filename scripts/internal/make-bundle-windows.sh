#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
# Builds the portable Windows bundle with the MinGW-w64 cross toolchain. Run it
# inside the image from packaging/windows (see make-bundle-windows-docker.sh).
# The result runs on 64-bit Windows 10 (1809) or newer without installation.
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build/windows}"
DIST_DIR="${DIST_DIR:-${PROJECT_DIR}/build/stage}"
BUNDLE_NAME="OpsRemote-windows-x86_64"
BUNDLE="${DIST_DIR}/${BUNDLE_NAME}"
mkdir -p "${DIST_DIR}"
SYSROOT="/usr/x86_64-w64-mingw32/sys-root/mingw"
OBJDUMP="x86_64-w64-mingw32-objdump"

make_archive=1
case "${1:-}" in
    "") ;;
    --no-archive) make_archive=0 ;;
    *) echo "Usage: $0 [--no-archive]" >&2; exit 2 ;;
esac

# Qt plugins the application can load at runtime.
QT_PLUGINS=(
    platforms/qwindows.dll
    platforms/qoffscreen.dll
    styles/qmodernwindowsstyle.dll
    iconengines/qsvgicon.dll
    imageformats/qsvg.dll
    imageformats/qico.dll
    imageformats/qgif.dll
    imageformats/qjpeg.dll
    tls/qopensslbackend.dll
    tls/qschannelbackend.dll
)

# Prints the DLLs a PE file imports.
imported_dlls() {
    "${OBJDUMP}" -p "$1" | sed -n 's/^\s*DLL Name: //p'
}

# Copies every DLL the given files need from the MinGW sysroot next to the
# executable, recursively. DLLs not in the sysroot are Windows system DLLs.
bundle_dependencies() {
    local pending=("$@") file dll source
    while (( ${#pending[@]} > 0 )); do
        file="${pending[0]}"
        pending=("${pending[@]:1}")
        while read -r dll; do
            [[ -e "${BUNDLE}/${dll}" ]] && continue
            source="$(find "${SYSROOT}/bin" -maxdepth 1 -iname "${dll}" -print -quit)"
            [[ -n "${source}" ]] || continue
            cp "${source}" "${BUNDLE}/$(basename "${source}")"
            pending+=("${BUNDLE}/$(basename "${source}")")
        done < <(imported_dlls "${file}")
    done
}

echo "==> Building Release configuration in ${BUILD_DIR}"
mingw64-cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}"

echo "==> Assembling ${BUNDLE}"
# Keep the user's state from an earlier build of this bundle.
saved_state=""
if [[ -d "${BUNDLE}/settings" ]]; then
    saved_state="$(mktemp -d "${DIST_DIR}/.state.XXXXXX")"
    mv "${BUNDLE}/settings" "${saved_state}/settings"
fi
rm -rf "${BUNDLE}"
mkdir -p "${BUNDLE}/data" "${BUNDLE}/plugins"
if [[ -n "${saved_state}" ]]; then
    mv "${saved_state}/settings" "${BUNDLE}/settings"
    rmdir "${saved_state}"
fi

cp "${BUILD_DIR}/opsremote.exe" "${BUNDLE}/OpsRemote.exe"
cp "${BUILD_DIR}/third_party/qtermwidget/libqtermwidget6.dll" "${BUNDLE}/"
cp -r "${BUILD_DIR}/data/qtermwidget" "${BUILD_DIR}/data/color-schemes" "${BUILD_DIR}/data/icons" \
    "${BUILD_DIR}/data/Kvantum" \
    "${BUNDLE}/data/"

plugin_files=()
for plugin in "${QT_PLUGINS[@]}"; do
    if [[ ! -f "${SYSROOT}/lib/qt6/plugins/${plugin}" ]]; then
        echo "warning: Qt plugin ${plugin} not found; skipping" >&2
        continue
    fi
    mkdir -p "${BUNDLE}/plugins/$(dirname "${plugin}")"
    cp "${SYSROOT}/lib/qt6/plugins/${plugin}" "${BUNDLE}/plugins/${plugin}"
    plugin_files+=("${BUNDLE}/plugins/${plugin}")
done
# OpenSSL's legacy provider holds MD4 and RC4, which NTLM (the usual RDP
# password log-on) needs. main() points OPENSSL_MODULES at this folder.
mkdir -p "${BUNDLE}/ossl-modules"
cp "${SYSROOT}/lib/ossl-modules/legacy.dll" "${BUNDLE}/ossl-modules/"
plugin_files+=("${BUNDLE}/ossl-modules/legacy.dll")
bundle_dependencies "${BUNDLE}/OpsRemote.exe" "${BUNDLE}/libqtermwidget6.dll" "${plugin_files[@]}"
x86_64-w64-mingw32-strip --strip-unneeded "${BUNDLE}"/*.exe "${BUNDLE}"/*.dll \
    "${BUNDLE}"/plugins/*/*.dll "${BUNDLE}"/ossl-modules/*.dll
"${PROJECT_DIR}/scripts/internal/collect-licenses.sh" "${BUNDLE}" windows "${SYSROOT}"

printf '[Paths]\r\nPrefix = .\r\nPlugins = plugins\r\n' > "${BUNDLE}/qt.conf"

# The combined package's launcher decompresses with this zstd (built in the
# Docker image); Windows has no zstd of its own.
if [[ -f /opt/portable-tools/zstd.exe ]]; then
    install -D -m 0755 /opt/portable-tools/zstd.exe "${DIST_DIR}/tools/zstd-windows-x86_64.exe"
    x86_64-w64-mingw32-strip --strip-unneeded "${DIST_DIR}/tools/zstd-windows-x86_64.exe"
fi

# The package's native launcher (OpsRemote.exe, with the icon and without a
# console window; legacy\OpsRemote.bat stays as a fallback). Linked statically, so
# it needs no DLLs of its own.
echo "==> Building the Windows launcher"
mkdir -p "${DIST_DIR}/tools"
launcher_dir="${PROJECT_DIR}/packaging/launchers/native"
x86_64-w64-mingw32-windres "${launcher_dir}/launcher-windows.rc" \
    -O coff -o "${BUILD_DIR}/launcher-windows-res.o"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -municode -mwindows -static \
    -o "${DIST_DIR}/tools/launcher-windows-x86_64.exe" \
    "${launcher_dir}/launcher-windows.c" "${BUILD_DIR}/launcher-windows-res.o" -lshell32
x86_64-w64-mingw32-strip --strip-unneeded "${DIST_DIR}/tools/launcher-windows-x86_64.exe"

echo "==> Checking that every DLL is bundled or a Windows system DLL"
problems=0
while IFS= read -r -d '' file; do
    while read -r dll; do
        if [[ -z "$(find "${BUNDLE}" -maxdepth 1 -iname "${dll}" -print -quit)" ]] \
            && [[ -n "$(find "${SYSROOT}/bin" -maxdepth 1 -iname "${dll}" -print -quit)" ]]; then
            echo "  not bundled: ${dll} (needed by ${file#"${BUNDLE}/"})" >&2
            problems=1
        fi
    done < <(imported_dlls "${file}")
done < <(find "${BUNDLE}" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0)
(( problems == 0 )) || { echo "Bundle check failed." >&2; exit 1; }

system_dlls="$(find "${BUNDLE}" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0 \
    | while IFS= read -r -d '' file; do imported_dlls "${file}"; done \
    | while read -r dll; do
          [[ -n "$(find "${BUNDLE}" -maxdepth 1 -iname "${dll}" -print -quit)" ]] || echo "${dll}"
      done | sort -fu | tr '\n' ' ')"
qt_version="$(grep -ho 'set(PACKAGE_VERSION "[0-9.]*")' "${SYSROOT}"/lib/cmake/Qt6/Qt6ConfigVersion*.cmake \
    2>/dev/null | head -n 1 | grep -o '[0-9][0-9.]*' || true)"

sed 's/$/\r/' > "${BUNDLE}/README.txt" <<EOF
OpsRemote for Windows (x86_64)

Run OpsRemote.exe from this folder. No installation or administrator
rights are needed. Settings, sessions, known SSH hosts, and the encrypted
credential vault are stored in the settings\ folder, so the folder can be
moved or copied as a whole.

Requirements: 64-bit Windows 10 version 1809 or newer, or Windows 11.

Local terminals are not available on Windows yet; SSH sessions, SFTP, and
the credential vault are.

Qt ${qt_version:-6}, libssh, OpenSSL, yaml-cpp, libsodium, utf8proc, and their
dependencies are bundled next to the executable. Windows provides:
  ${system_dlls}
EOF

size="$(du -sh "${BUNDLE}" | cut -f1)"
echo "==> Bundle ready: ${BUNDLE} (${size})"
if (( make_archive )); then
    # Ship only the program, never this machine's settings.
    rm -f "${DIST_DIR}/${BUNDLE_NAME}.zip"
    (cd "${DIST_DIR}" && zip -qr -9 "${BUNDLE_NAME}.zip" "${BUNDLE_NAME}" -x "${BUNDLE_NAME}/settings/*")
    echo "==> Archive: ${DIST_DIR}/${BUNDLE_NAME}.zip ($(du -sh "${DIST_DIR}/${BUNDLE_NAME}.zip" | cut -f1))"
fi
