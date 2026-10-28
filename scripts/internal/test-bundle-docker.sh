#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
# Smoke-tests the portable bundle on other distributions: in each container it
# installs only what a desktop system provides (X11/xcb, GL, fonts) plus Xvfb,
# confirms that no Qt is installed, starts the bundle on a virtual display, and
# checks that the main window appears and that no library the bundle ships
# was loaded from the system instead.
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
ARCHIVE="${PROJECT_DIR}/build/stage/OpsRemote-linux-$(uname -m).tar.gz"
IMAGES=("$@")
if (( ${#IMAGES[@]} == 0 )); then
    IMAGES=(ubuntu:22.04 debian:12 fedora:42 almalinux:9 archlinux:latest)
fi
[[ -f "${ARCHIVE}" ]] || { echo "Build the bundle first: ${ARCHIVE} not found" >&2; exit 1; }
# The same list of system libraries that make-bundle.sh leaves out.
SYSTEM_PATTERN="$(grep -o "SYSTEM_LIBRARY_PATTERN+\?='[^']*'" "${PROJECT_DIR}/scripts/internal/make-bundle.sh" \
    | sed "s/^[^']*'//; s/'$//" | tr -d '\n')"

DEB_PACKAGES="xvfb x11-utils libfontconfig1 libfreetype6 libharfbuzz0b libegl1 libgl1 libopengl0
    libx11-6 libx11-xcb1 libxcb1 libxcb-glx0 libxcb-randr0 libxcb-render0 libxcb-shape0
    libxcb-shm0 libxcb-sync1 libxcb-xfixes0 libxcb-xinput0 libxcb-xkb1 libwayland-client0
    libwayland-cursor0 libwayland-egl1 fonts-dejavu-core xkb-data"
RPM_PACKAGES="xorg-x11-server-Xvfb xorg-x11-utils xwininfo fontconfig freetype harfbuzz libglvnd-egl
    libglvnd-glx libglvnd-opengl libX11 libX11-xcb libxcb libwayland-client libwayland-cursor
    libwayland-egl dejavu-sans-mono-fonts xkeyboard-config"
ARCH_PACKAGES="xorg-server-xvfb xorg-xwininfo fontconfig freetype2 harfbuzz libglvnd libx11
    libxcb wayland ttf-dejavu xkeyboard-config"

# Runs inside the container: installs the desktop runtime, then starts the bundle.
read -r -d '' CONTAINER_SCRIPT <<'EOF' || true
set -eu
. /etc/os-release
if command -v apt-get >/dev/null; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null && apt-get install -y -qq --no-install-recommends $DEB_PACKAGES >/dev/null
    installed() { dpkg -l | awk '/^ii/ { print $2 }'; }
elif command -v dnf >/dev/null; then
    # Package names differ between releases; skip the ones a release lacks
    # (dnf5 and dnf4 spell that differently).
    dnf -y -q install --skip-unavailable $RPM_PACKAGES >/dev/null 2>&1 \
        || dnf -y -q install --setopt=strict=0 $RPM_PACKAGES >/dev/null
    installed() { rpm -qa --qf '%{NAME}\n'; }
else
    pacman -Sy --noconfirm --needed -q $ARCH_PACKAGES >/dev/null 2>&1
    installed() { pacman -Qq; }
fi
if installed | grep -Ei '^(lib)?qt6|qt6-'; then
    echo "FAIL: the image has Qt installed"; exit 1
fi
glibc="$(ldd --version | head -n 1 | grep -o '[0-9]\+\.[0-9]\+$')"
tar -C /tmp -xzf /bundle.tar.gz
cd /tmp/OpsRemote-linux-*
Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
sleep 2
DISPLAY=:99 QT_QPA_PLATFORM=xcb LD_DEBUG=libs LD_DEBUG_OUTPUT=/tmp/ld \
    ./opsremote.sh >/tmp/app.log 2>&1 &
app=$!
for _ in $(seq 1 20); do
    sleep 0.5
    if DISPLAY=:99 xwininfo -root -tree 2>/dev/null | grep -q '"OpsRemote"'; then
        bundle="$(pwd -P)"
        kill $app; sleep 1
        # glibc's loader log names each requested soname followed by the files
        # it tried; the last one tried is the one loaded. A defect is a soname
        # the bundle ships that was resolved to a system file instead.
        resolved="$(awk '
            /find library=/ { if (name != "") print name, path
                              name = $0; sub(/.*find library=/, "", name); sub(/ .*/, "", name); path = "" }
            /trying file=/  { path = $0; sub(/.*trying file=/, "", path) }
            END             { if (name != "") print name, path }' /tmp/ld.$app)"
        shadowed="$(echo "$resolved" | while read -r name path; do
            [ -e "$bundle/lib/$name" ] && case "$path" in "$bundle"/*) ;; *) echo "$name -> $path" ;; esac
        done)"
        if [ -n "$shadowed" ]; then
            echo "FAIL: $PRETTY_NAME: bundled libraries were loaded from the system:"; echo "$shadowed"; exit 1
        fi
        from_bundle="$(echo "$resolved" | awk -v b="$bundle/" 'index($2, b) == 1' | wc -l)"
        from_system="$(echo "$resolved" | awk -v b="$bundle/" 'index($2, b) != 1 { print $1 }' | sort -u | tr '\n' ' ')"
        echo "PASS: $PRETTY_NAME (glibc $glibc): window shown; $from_bundle libraries from the bundle, none shadowed"
        echo "      from the system: $from_system"
        exit 0
    fi
    kill -0 $app 2>/dev/null || break
done
echo "FAIL: $PRETTY_NAME (glibc $glibc): no window (xwininfo: $(command -v xwininfo || echo missing))"
grep -v propagateSizeHints /tmp/app.log | head -n 20
exit 1
EOF

failures=0
for image in "${IMAGES[@]}"; do
    echo "==> ${image}"
    if ! docker run --rm \
        -v "${ARCHIVE}:/bundle.tar.gz:ro" \
        -e DEB_PACKAGES="${DEB_PACKAGES//$'\n'/ }" \
        -e RPM_PACKAGES="${RPM_PACKAGES//$'\n'/ }" \
        -e ARCH_PACKAGES="${ARCH_PACKAGES//$'\n'/ }" \
        -e SYSTEM_PATTERN="${SYSTEM_PATTERN}" \
        "${image}" sh -c "${CONTAINER_SCRIPT}"; then
        failures=$((failures + 1))
    fi
done
(( failures == 0 )) || { echo "${failures} image(s) failed" >&2; exit 1; }
