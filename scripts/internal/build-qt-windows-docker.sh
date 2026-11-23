#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
# Rebuilds the patched Qt DLLs for Windows (see build-qt-windows.sh) in the
# Windows bundle's image. Needed only when packaging/qt/patches or the
# image's Qt version changes; the Windows bundle says so when it is.
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=lib/errors.sh
source "${PROJECT_DIR}/scripts/internal/lib/errors.sh"
IMAGE="${IMAGE:-opsremote-bundle-windows}"

# packaging/ is the context: the images share files (packaging/libvncserver).
docker build -t "${IMAGE}" -f "${PROJECT_DIR}/packaging/windows/Dockerfile" "${PROJECT_DIR}/packaging"
# As root: it installs the build's dependencies; the result is given back to
# the user who runs this.
docker run --rm -e OWNER="$(id -u):$(id -g)" -v "${PROJECT_DIR}:/src" -w /src \
    "${IMAGE}" ./scripts/internal/build-qt-windows.sh
