#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
# Builds a self-contained Linux bundle: the application, its shared libraries,
# and the Qt plugins it needs, runnable from any directory without installing
# Qt, libssh, yaml-cpp, libsodium, or ICU on the target machine.
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build/linux-host}"
DIST_DIR="${DIST_DIR:-${PROJECT_DIR}/build/stage}"
BUNDLE_NAME="OpsRemote-linux-$(uname -m)"
BUNDLE="${DIST_DIR}/${BUNDLE_NAME}"
mkdir -p "${DIST_DIR}"

usage() {
    cat <<'EOF'
Usage: ./scripts/internal/make-bundle.sh [--no-archive]

Builds a Release configuration in ./build/linux-host and assembles
./build/stage/OpsRemote-linux-<arch>/ plus a .tar.gz of it.

Environment overrides:
  BUILD_DIR  Build directory (default: ./build/linux-host)
  DIST_DIR   Output directory (default: ./build/stage)
EOF
}

make_archive=1
case "${1:-}" in
    "") ;;
    --no-archive) make_archive=0 ;;
    -h|--help|help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac

# Qt plugins the application can load at runtime. Missing ones are skipped
# with a warning, since not every Qt installation ships all of them.
QT_PLUGINS=(
    platforms/libqxcb.so
    platforms/libqwayland.so
    platforms/libqoffscreen.so
    platforminputcontexts/libcomposeplatforminputcontextplugin.so
    platforminputcontexts/libibusplatforminputcontextplugin.so
    xcbglintegrations/libqxcb-glx-integration.so
    xcbglintegrations/libqxcb-egl-integration.so
    wayland-shell-integration/libxdg-shell.so
    wayland-decoration-client/libbradient.so
    wayland-decoration-client/libadwaita.so
    wayland-graphics-integration-client/libqt-plugin-wayland-egl.so
    platformthemes/libqxdgdesktopportal.so
    iconengines/libqsvgicon.so
    imageformats/libqsvg.so
    imageformats/libqico.so
    imageformats/libqgif.so
    imageformats/libqjpeg.so
)

# Libraries that must come from the target system and are never bundled:
# glibc, which must match the kernel ABI and the dynamic loader; libgcc_s,
# the backward-compatible GCC runtime every system has (a copy from a patched
# glibc distribution can need newer glibc symbols than its base version); the GPU
# driver stack; the X11 and Wayland client libraries that speak the display
# server's protocol; and the font stack, which must read the system's font
# configuration.
SYSTEM_LIBRARY_PATTERN='^(ld-linux.*|libc|libm|libdl|libpthread|librt|libresolv|libutil|libanl|libgcc_s'
SYSTEM_LIBRARY_PATTERN+='|libGL|libGLX|libEGL|libGLdispatch|libOpenGL|libGLESv2|libdrm|libgbm|libvulkan'
SYSTEM_LIBRARY_PATTERN+='|libX11|libX11-xcb|libXau|libXdmcp|libXext|libXrender|libXi|libXfixes|libxcb'
SYSTEM_LIBRARY_PATTERN+='|libxcb-(composite|damage|dbe|dpms|dri2|dri3|glx|present|randr|record|render'
SYSTEM_LIBRARY_PATTERN+='|res|screensaver|shape|shm|sync|xf86dri|xfixes|xinerama|xinput|xkb|xtest|xv|xvmc)'
SYSTEM_LIBRARY_PATTERN+='|libwayland-(client|cursor|egl|server)'
SYSTEM_LIBRARY_PATTERN+='|libfontconfig|libfreetype|libharfbuzz|libgraphite2)\.so'

# Prints a Qt build setting such as QT_INSTALL_PLUGINS or QT_VERSION.
query_qt() {
    local qtpaths
    for qtpaths in qtpaths6 /usr/lib/qt6/bin/qtpaths6 /usr/lib/qt6/bin/qtpaths qtpaths; do
        if command -v "${qtpaths}" >/dev/null 2>&1; then
            "${qtpaths}" --query "$1" && return
        fi
    done
    if command -v qmake6 >/dev/null 2>&1; then
        qmake6 -query "$1" && return
    fi
    echo "Could not find qtpaths6 or qmake6 to query Qt's $1." >&2
    exit 1
}

is_system_library() {
    [[ "$1" =~ ${SYSTEM_LIBRARY_PATTERN} ]]
}

# Copies every non-system shared library that the given ELF files depend on
# into lib/, keeping the soname as the file name.
bundle_dependencies() {
    local elf name path
    for elf in "$@"; do
        while read -r name path; do
            if [[ "${path}" == "not" ]]; then
                echo "Missing dependency ${name} of ${elf}" >&2
                exit 1
            fi
            if is_system_library "${name}" || [[ -e "${BUNDLE}/lib/${name}" ]]; then
                continue
            fi
            cp -L "${path}" "${BUNDLE}/lib/${name}"
            chmod 0755 "${BUNDLE}/lib/${name}"
        done < <(ldd "${elf}" | awk '/=>/ { sub(".*/", "", $1); print $1, $3 }')
    done
}

echo "==> Building Release configuration in ${BUILD_DIR}"
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DOPSREMOTE_BUNDLE=ON
cmake --build "${BUILD_DIR}" --parallel

echo "==> Assembling ${BUNDLE}"
# Keep the user's state from an earlier build of this bundle: settings/
# (sessions, preferences, vault, known hosts), and anything an older version
# left in data/ for the application to migrate. Everything else is rebuilt.
saved_state="$(mktemp -d "${DIST_DIR}/.state.XXXXXX")"
for folder in settings data; do
    if [[ -d "${BUNDLE}/${folder}" ]]; then
        mv "${BUNDLE}/${folder}" "${saved_state}/${folder}"
    fi
done
rm -rf "${saved_state}/data/qtermwidget" "${saved_state}/data/color-schemes" \
    "${saved_state}/data/icons"
rm -rf "${BUNDLE}"
mkdir -p "${BUNDLE}/lib"
for folder in settings data; do
    if [[ -d "${saved_state}/${folder}" ]]; then
        mv "${saved_state}/${folder}" "${BUNDLE}/${folder}"
    fi
done
rmdir "${saved_state}"
mkdir -p "${BUNDLE}/data"
install -m 0755 "${BUILD_DIR}/opsremote" "${BUNDLE}/opsremote"
cp -L "${BUILD_DIR}/third_party/qtermwidget/libqtermwidget6.so.2" "${BUNDLE}/lib/"
# Only the shipped terminal resources; never sessions, settings, or vaults.
cp -r "${BUILD_DIR}/data/qtermwidget" "${BUILD_DIR}/data/color-schemes" "${BUILD_DIR}/data/icons" \
    "${BUNDLE}/data/"

qt_plugin_dir="$(query_qt QT_INSTALL_PLUGINS)"
# Plugins keep their RUNPATH of $ORIGIN/../../../, so they must sit three
# levels below lib/.
plugin_root="${BUNDLE}/lib/qt6/plugins"
plugin_files=()
for plugin in "${QT_PLUGINS[@]}"; do
    if [[ ! -f "${qt_plugin_dir}/${plugin}" ]]; then
        echo "warning: Qt plugin ${plugin} not found; skipping" >&2
        continue
    fi
    mkdir -p "${plugin_root}/$(dirname "${plugin}")"
    cp -L "${qt_plugin_dir}/${plugin}" "${plugin_root}/${plugin}"
    plugin_files+=("${plugin_root}/${plugin}")
done

# OpenSSL's legacy provider holds MD4 and RC4, which NTLM (the usual RDP
# password log-on) needs. main() points OPENSSL_MODULES at this folder.
openssl_modules="$(openssl version -m | sed -E 's/^MODULESDIR: "(.*)"$/\1/')"
mkdir -p "${BUNDLE}/lib/ossl-modules"
cp -L "${openssl_modules}/legacy.so" "${BUNDLE}/lib/ossl-modules/"
plugin_files+=("${BUNDLE}/lib/ossl-modules/legacy.so")

bundle_dependencies "${BUNDLE}/opsremote" "${BUNDLE}/lib/libqtermwidget6.so.2" \
    "${plugin_files[@]}"
strip --strip-unneeded "${BUNDLE}/opsremote" "${BUNDLE}/lib/libqtermwidget6.so.2"
"${PROJECT_DIR}/scripts/internal/collect-licenses.sh" "${BUNDLE}" linux

# The combined package's launcher decompresses with this zstd (built in the
# Docker image), so it needs none on the host.
if [[ -x /opt/portable-tools/zstd ]]; then
    install -D -m 0755 /opt/portable-tools/zstd "${DIST_DIR}/tools/zstd-linux-x86_64"
fi

cat > "${BUNDLE}/qt.conf" <<'EOF'
[Paths]
Prefix = .
Plugins = lib/qt6/plugins
EOF

cat > "${BUNDLE}/opsremote.sh" <<'EOF'
#!/bin/sh
# Starts OpsRemote from this folder. Settings, sessions, and the
# credential vault are kept in the settings/ folder next to this script.
#
# Before starting, it checks with the system's dynamic loader that every
# library the application needs can be loaded, and stops with an explanation
# if not (for example on a distribution whose glibc is too old). Set
# OPSREMOTE_SKIP_CHECK=1 to skip the check.
here="$(dirname -- "$(readlink -f -- "$0")")"

fail() {
    message="OpsRemote cannot start on this system.

$1"
    printf '%s\n' "${message}" >&2
    # Started from a file manager there is no terminal to show the message.
    if [ ! -t 2 ] && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; }; then
        if command -v zenity >/dev/null 2>&1; then
            zenity --error --title="OpsRemote" --no-wrap --text="${message}" 2>/dev/null
        elif command -v kdialog >/dev/null 2>&1; then
            kdialog --title "OpsRemote" --error "${message}" 2>/dev/null
        elif command -v xmessage >/dev/null 2>&1; then
            xmessage -center "${message}" 2>/dev/null
        fi
    fi
    exit 1
}

# Prints the problems the dynamic loader reports for one ELF file: missing
# libraries and missing (too new) symbol versions. The bundle's lib/ is on the
# search path only for this check, standing in for the executable's RPATH.
loader_problems() {
    # Trace mode, as used by ldd, keeps going after the first missing library.
    LD_TRACE_LOADED_OBJECTS=1 LD_WARN=yes LD_LIBRARY_PATH="${here}/lib" "${loader}" "$1" 2>&1 \
        | sed -n -e 's/^[[:space:]]*\([^[:space:]]*\) => not found.*/missing library: \1/p' \
                 -e 's/.*error while loading shared libraries: \([^:]*\): cannot open.*/missing library: \1/p' \
                 -e "s/.*version \`\([^']*\)' not found.*/missing symbol version: \1/p" \
        | sort -u
}

describe_problems() {
    printf '%s\n' "$1" | grep '^missing library:' | sed 's/^missing library: /  - /'
    required="$(printf '%s\n' "$1" | sed -n 's/^missing symbol version: GLIBC_//p' \
        | sort -t. -k1,1n -k2,2n | tail -n 1)"
    if [ -n "${required}" ]; then
        system="$(getconf GNU_LIBC_VERSION 2>/dev/null | cut -d' ' -f2)"
        echo "  - the GNU C library (glibc) is too old: version ${required} or newer is needed, this system has ${system:-an older one}"
    fi
    printf '%s\n' "$1" | grep '^missing symbol version:' | grep -v 'GLIBC_' \
        | sed 's/^missing symbol version: /  - a system library is too old (needs symbol version /; s/$/)/'
}

check_system() {
    loader=""
    for candidate in /lib64/ld-linux-x86-64.so.2 /lib/ld-linux-aarch64.so.1 /lib/ld-linux-armhf.so.3; do
        if [ -x "${candidate}" ]; then
            loader="${candidate}"
            break
        fi
    done
    [ -n "${loader}" ] || fail "This system has no GNU C library (glibc) dynamic loader.
OpsRemote needs a glibc-based Linux distribution; musl-based ones
such as Alpine Linux are not supported."

    problems="$(loader_problems "${here}/opsremote")"
    [ -z "${problems}" ] || fail "These requirements are not met:
$(describe_problems "${problems}")

Install the missing libraries with your distribution's package manager,
or use a newer distribution (see README.txt for the requirements)."

    # A display plugin is loaded only at runtime: at least one that matches
    # the session must load. An explicitly chosen platform is left to Qt.
    [ -z "${QT_QPA_PLATFORM:-}" ] || return 0
    plugins="${here}/lib/qt6/plugins/platforms"
    candidates=""
    [ -n "${WAYLAND_DISPLAY:-}" ] && candidates="${candidates} libqwayland.so"
    [ -n "${DISPLAY:-}" ] && candidates="${candidates} libqxcb.so"
    [ -n "${candidates}" ] || return 0
    details=""
    for plugin in ${candidates}; do
        problems="$(loader_problems "${plugins}/${plugin}")"
        [ -z "${problems}" ] && return 0
        case "${plugin}" in
            libqwayland.so) name="Wayland" ;;
            *) name="X11" ;;
        esac
        details="${details}${name} display support needs:
$(describe_problems "${problems}")
"
    done
    fail "No display backend can be loaded for this desktop session.
${details}
Install the missing libraries with your distribution's package manager."
}

[ -n "${OPSREMOTE_SKIP_CHECK:-}" ] || check_system

# Plugins from a system Qt of a different version must not be mixed in.
unset QT_PLUGIN_PATH QT_QPA_PLATFORM_PLUGIN_PATH
# On Wayland, QT_IM_MODULES (for example "wayland;ibus") makes Qt try the
# text-input-v3 protocol first. When the desktop names an input method the
# bundle ships, use that plugin directly over D-Bus instead (a precaution;
# see the design notes on input methods).
case "${QT_IM_MODULE:-}" in
    ibus|compose) unset QT_IM_MODULES ;;
esac
exec "${here}/opsremote" "$@"
EOF
chmod 0755 "${BUNDLE}/opsremote.sh"

echo "==> Checking that every dependency is bundled or provided by the system"
problems=0
# A non-PIE executable breaks Qt signal connections to QTermWidget, which is
# linked with -Bsymbolic-functions (typing into SSH terminals stops working).
if ! readelf -h "${BUNDLE}/opsremote" | grep -q 'Type:.*DYN'; then
    echo "  the executable is not position independent (PIE)" >&2
    problems=1
fi
# Every dependency of every bundled file must be either a system library or
# present in lib/ (ldd on a single library resolves through the system, so
# only names are compared here).
while IFS= read -r -d '' elf; do
    while read -r name; do
        if ! is_system_library "${name}" && [[ ! -e "${BUNDLE}/lib/${name}" ]]; then
            echo "  not bundled: ${name} (needed by ${elf#"${BUNDLE}/"})" >&2
            problems=1
        fi
    done < <(readelf -d "${elf}" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')
done < <(find "${BUNDLE}" -type f \( -name '*.so*' -o -name opsremote \) -print0)
# The executable's DT_RPATH covers its whole dependency tree, so in its
# context every non-system library must resolve inside the bundle.
while read -r name path; do
    if [[ "${path}" != "${BUNDLE}/"* ]] && ! is_system_library "${name}"; then
        echo "  resolves outside the bundle: ${name} -> ${path}" >&2
        problems=1
    fi
done < <(env -u LD_LIBRARY_PATH ldd "${BUNDLE}/opsremote" \
    | awk '/=>/ { sub(".*/", "", $1); print $1, $3 }')
if (( problems )); then
    echo "Bundle check failed." >&2
    exit 1
fi

# The oldest glibc the bundle can run on is the newest symbol version any
# bundled file requires.
glibc_required="$(find "${BUNDLE}" -type f \( -name '*.so*' -o -name opsremote \) \
    -exec objdump -T {} + 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -n 1 || true)"
system_libraries="$(find "${BUNDLE}" -type f \( -name '*.so*' -o -name opsremote \) \
    -exec readelf -d {} + 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | sort -u \
    | while read -r name; do if is_system_library "${name}"; then echo "${name}"; fi; done \
    | tr '\n' ' ')"

cat > "${BUNDLE}/README.txt" <<EOF
OpsRemote for Linux ($(uname -m))

Run ./opsremote.sh from this folder. No installation is needed.
Before starting, it checks that the system provides every library listed
below and explains what is missing if not (set OPSREMOTE_SKIP_CHECK=1
to skip the check).
Settings, sessions, known SSH hosts, and the encrypted credential vault are
stored in the settings/ folder, so the folder can be moved or copied as a whole.

The target system must provide:
  - glibc ${glibc_required#GLIBC_} or newer
  - an X11 or Wayland desktop session
  - fontconfig, FreeType, and HarfBuzz (present on any Linux desktop)
  - the graphics and display client libraries:
    ${system_libraries}

Everything else (Qt $(query_qt QT_VERSION), libssh, OpenSSL, yaml-cpp, libsodium, ICU,
and their dependencies) is bundled in lib/.
EOF

size="$(du -sh "${BUNDLE}" | cut -f1)"
echo "==> Bundle ready: ${BUNDLE} (${size}, needs ${glibc_required})"
if (( make_archive )); then
    # The archive is for other machines: ship only the terminal resources,
    # never this machine's sessions, settings, vault, or known hosts.
    tar -C "${DIST_DIR}" -czf "${DIST_DIR}/${BUNDLE_NAME}.tar.gz" \
        --exclude="${BUNDLE_NAME}/settings" \
        --exclude="${BUNDLE_NAME}/data/sessions.*" --exclude="${BUNDLE_NAME}/data/settings.yaml" \
        --exclude="${BUNDLE_NAME}/data/credentials.vault" --exclude="${BUNDLE_NAME}/data/known_hosts" \
        "${BUNDLE_NAME}"
    echo "==> Archive: ${DIST_DIR}/${BUNDLE_NAME}.tar.gz ($(du -sh "${DIST_DIR}/${BUNDLE_NAME}.tar.gz" | cut -f1))"
fi
