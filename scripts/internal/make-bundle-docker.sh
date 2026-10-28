#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
# Builds the portable Linux bundle inside an AlmaLinux 9 container, so the
# result needs only glibc 2.34 instead of the host's glibc.
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="${IMAGE:-opsremote-bundle-el9}"

docker build -t "${IMAGE}" "${PROJECT_DIR}/packaging/linux"
# Run as the calling user so build and dist files are not owned by root.
docker run --rm --user "$(id -u):$(id -g)" \
    -v "${PROJECT_DIR}:/src" -w /src \
    -e BUILD_DIR=/src/build/linux \
    "${IMAGE}" ./scripts/internal/make-bundle.sh "$@"
