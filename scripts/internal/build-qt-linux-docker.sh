#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
# Rebuilds the patched Qt libraries for Linux (see build-qt-linux.sh) in the
# Linux bundle's image. Needed only when packaging/qt-linux/patches or the
# image's Qt version changes; the Linux bundle says so when it is.
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=lib/errors.sh
source "${PROJECT_DIR}/scripts/internal/lib/errors.sh"
IMAGE="${IMAGE:-opsremote-bundle-el9}"

# packaging/ is the context: the images share files (packaging/libvncserver).
docker build -t "${IMAGE}" -f "${PROJECT_DIR}/packaging/linux/Dockerfile" "${PROJECT_DIR}/packaging"
# As root: it installs the build's dependencies; the result is given back to
# the user who runs this.
docker run --rm -e OWNER="$(id -u):$(id -g)" -v "${PROJECT_DIR}:/src" -w /src \
    "${IMAGE}" ./scripts/internal/build-qt-linux.sh
