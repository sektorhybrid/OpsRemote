#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
# Cross-builds the portable Windows bundle inside a Fedora MinGW-w64 container.
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=lib/errors.sh
source "${PROJECT_DIR}/scripts/internal/lib/errors.sh"
IMAGE="${IMAGE:-opsremote-bundle-windows}"

# packaging/ is the context: the images share files (packaging/libvncserver).
docker build -t "${IMAGE}" -f "${PROJECT_DIR}/packaging/windows/Dockerfile" "${PROJECT_DIR}/packaging"
# Fedora's toolchain file names /usr/bin tools; keep /usr/bin first so CMake
# does not detect the same tools under /usr/sbin and reset its cache.
docker run --rm --user "$(id -u):$(id -g)" -e PATH=/usr/bin:/usr/sbin:/bin \
    -v "${PROJECT_DIR}:/src" -w /src \
    "${IMAGE}" ./scripts/internal/make-bundle-windows.sh "$@"
