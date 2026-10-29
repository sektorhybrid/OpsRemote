#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build/dev}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
PACKAGE_DIR="${PROJECT_DIR}/dist/OpsRemote"

usage() {
    cat <<'EOF'
Usage: ./scripts/build-and-run.sh <command>

The portable package, dist/OpsRemote/ (the Linux and Windows programs
in one folder with a shared settings/ folder; built in Docker containers):
  package-linux              Build the Linux program and update the package.
                             The package keeps the last Windows build, if any.
  package-windows            Build the Windows program and update the package.
                             The package keeps the last Linux build, if any.
  package-all                Build both programs and update the package.
  package-zip                Zip the current package as dist/OpsRemote.zip,
                             for handing out (without your settings and backups).
  run-package                Start the package's Linux version
                             (dist/OpsRemote/OpsRemote, or legacy/OpsRemote.sh).
  package-linux-and-run      package-linux, then run-package (the default).
  package-all-and-run        package-all, then run-package.

A developer build in ./build/dev, against this computer's own libraries (quick to
rebuild, not portable):
  dev-build                  Configure and compile.
  dev-run                    Start the developer build.
  dev-build-and-run          dev-build, then dev-run.

The package keeps its settings/ folder (sessions, preferences, vault, known
hosts) across rebuilds.

Environment overrides (developer build):
  BUILD_DIR   Build directory (default: ./build/dev)
  BUILD_TYPE  CMake build type (default: Debug)
EOF
}

build_linux_program() {
    "${PROJECT_DIR}/scripts/internal/make-bundle-docker.sh" --no-archive
}

build_windows_program() {
    "${PROJECT_DIR}/scripts/internal/make-bundle-windows-docker.sh" --no-archive
}

zip_package() {
    "${PROJECT_DIR}/scripts/internal/make-package.sh"
}

update_package() {
    "${PROJECT_DIR}/scripts/internal/make-package.sh" --no-archive
}

run_package() {
    # The native launcher, or its fallback script.
    if [[ -x "${PACKAGE_DIR}/OpsRemote" ]]; then
        exec "${PACKAGE_DIR}/OpsRemote"
    fi
    if [[ ! -x "${PACKAGE_DIR}/legacy/OpsRemote.sh" ]]; then
        echo "Package not found at ${PACKAGE_DIR}. Build it first with: $0 package-linux" >&2
        exit 1
    fi
    exec "${PACKAGE_DIR}/legacy/OpsRemote.sh"
}

dev_build() {
    cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    cmake --build "${BUILD_DIR}" --parallel
}

dev_run() {
    local app="${BUILD_DIR}/opsremote"
    if [[ ! -x "${app}" ]]; then
        echo "Developer build not found at ${app}. Build it first with: $0 dev-build" >&2
        exit 1
    fi
    exec "${app}"
}

command="${1:-package-linux-and-run}"
case "${command}" in
    package-linux) build_linux_program; update_package ;;
    package-windows) build_windows_program; update_package ;;
    package-all) build_linux_program; build_windows_program; update_package ;;
    package-zip) zip_package ;;
    run-package) run_package ;;
    package-linux-and-run) build_linux_program; update_package; run_package ;;
    package-all-and-run) build_linux_program; build_windows_program; update_package; run_package ;;
    dev-build) dev_build ;;
    dev-run) dev_run ;;
    dev-build-and-run) dev_build; dev_run ;;
    -h|--help|help) usage ;;
    *)
        echo "Unknown command: ${command}" >&2
        usage >&2
        exit 2
        ;;
esac
