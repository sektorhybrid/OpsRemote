#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
# Copies the complete combined package (dist/OpsRemote: both launchers,
# both program archives, tools, data, and settings) to a Windows test machine
# over SSH, then starts it once without a window to check that it unpacks and
# runs. The machine's previous settings are first copied to
# settings-backups\ next to the package (outside it, so deleting the
# package folder keeps them).
#
#   VM_HOST   the test VM's address (required)
#   VM_USER   the Windows account to log in as (required)
#   VM_DIR    folder under the user's profile (default Documents\terminal-project)
#   SSHPASS   the account's password, for sshpass (not stored anywhere)
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
PACKAGE="${PROJECT_DIR}/dist/OpsRemote"
VM_HOST="${VM_HOST:?Set VM_HOST to the address of the test VM.}"
VM_USER="${VM_USER:?Set VM_USER to the Windows account to log in as.}"
VM_DIR="${VM_DIR:-Documents\\terminal-project}"
SSH_OPTIONS=(-o PubkeyAuthentication=no -o LogLevel=ERROR -o ConnectTimeout=10)

if [[ -z "${SSHPASS:-}" ]]; then
    echo "Set SSHPASS to ${VM_USER}'s password." >&2
    exit 2
fi
export SSHPASS
# A package build rewrites dist/OpsRemote; copying it meanwhile would
# send a half-written package.
if pgrep -f "scripts/internal/make-package.sh" > /dev/null; then
    echo "A package build is running; deploy again when it has finished." >&2
    exit 1
fi
for file in legacy/OpsRemote.sh legacy/OpsRemote.bat compressed/linux-x86_64.tar.zst \
    compressed/windows-x86_64.tar.zst; do
    [[ -f "${PACKAGE}/${file}" ]] || { echo "${PACKAGE}/${file} is missing; run ./scripts/build-and-run.sh package-all." >&2; exit 1; }
done

remote() {
    sshpass -e ssh "${SSH_OPTIONS[@]}" "${VM_USER}@${VM_HOST}" "$1"
}
# PowerShell commands go through -EncodedCommand, which avoids cmd quoting.
remote_ps() {
    local encoded
    encoded="$(printf '%s' "\$ProgressPreference = 'SilentlyContinue'; $1" | iconv -t UTF-16LE | base64 -w0)"
    remote "powershell -NoProfile -NonInteractive -EncodedCommand ${encoded}"
}

if [[ "$(remote_ps 'if (Get-Process OpsRemote -ErrorAction SilentlyContinue) { "running" } else { "free" }')" == *running* ]]; then
    echo "OpsRemote is running on ${VM_HOST}; close it first." >&2
    exit 1
fi

archive="$(mktemp --suffix=.tar.gz)"
trap 'rm -f "${archive}"' EXIT
echo "==> Packing ${PACKAGE}"
tar -C "${PACKAGE}/.." -czf "${archive}" \
    --exclude=OpsRemote/runtime --exclude=OpsRemote/backups OpsRemote

echo "==> Copying to ${VM_USER}@${VM_HOST}"
remote_ps "New-Item -ItemType Directory -Force \"\$env:USERPROFILE\\${VM_DIR}\" | Out-Null"
sshpass -e scp "${SSH_OPTIONS[@]}" "${archive}" \
    "${VM_USER}@${VM_HOST}:${VM_DIR//\\//}/pt-deploy.tar.gz"

echo "==> Installing (previous settings go to settings-backups\\<time>)"
remote_ps "
\$ErrorActionPreference = 'Stop'
\$root = \"\$env:USERPROFILE\\${VM_DIR}\"
\$package = Join-Path \$root 'OpsRemote'
if (Test-Path (Join-Path \$package 'settings')) {
    \$backup = Join-Path \$root ('settings-backups\\' + (Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'))
    New-Item -ItemType Directory -Force (Split-Path \$backup) | Out-Null
    Copy-Item -Recurse (Join-Path \$package 'settings') \$backup
}
# Everything but backups and the unpacked runtime is replaced.
if (Test-Path \$package) {
    Get-ChildItem -Force \$package | Where-Object { \$_.Name -notin @('backups', 'runtime') } |
        Remove-Item -Recurse -Force
}
tar -C \$root -xzf (Join-Path \$root 'pt-deploy.tar.gz')
Remove-Item (Join-Path \$root 'pt-deploy.tar.gz')
"

echo "==> Checking the deployed package"
remote_ps "
\$package = \"\$env:USERPROFILE\\${VM_DIR}\\OpsRemote\"
'linux id:    ' + (Get-Content (Join-Path \$package 'compressed\\linux-x86_64.id'))
'windows id:  ' + (Get-Content (Join-Path \$package 'compressed\\windows-x86_64.id'))
'settings:    ' + ((Get-ChildItem (Join-Path \$package 'settings') | ForEach-Object Name) -join ', ')
\$env:QT_QPA_PLATFORM = 'offscreen'
\$env:OPSREMOTE_TEST_CLOSE_MS = '5000'
\$id = Get-Content (Join-Path \$package 'compressed\\windows-x86_64.id')
# Check while it runs: with 'remove the unpacked program when closing' on,
# the copy is gone again after exit.
\$launcher = Start-Process -FilePath cmd.exe -ArgumentList '/c', ([char]34 + (Join-Path \$package 'legacy\\OpsRemote.bat') + [char]34) -WindowStyle Hidden -PassThru
\$process = \$null
for (\$i = 0; \$i -lt 60 -and -not \$process; \$i++) {
    Start-Sleep -Milliseconds 500
    \$process = Get-Process OpsRemote -ErrorAction SilentlyContinue |
        Where-Object { \$_.Path -like ('*windows-x86_64-' + \$id + '*') }
}
if (\$process) { 'start test:  started from runtime (windows-x86_64-' + \$id + ')' } else { 'start test:  FAILED (the program did not start)' }
if (\$process) { \$process | ForEach-Object { if (-not \$_.WaitForExit(30000)) { 'WARNING: it did not close' } } }
Start-Sleep 5
if (Get-Process OpsRemote -ErrorAction SilentlyContinue) { 'WARNING: a OpsRemote process is still running' }
"
echo "    local ids:   linux $(cat "${PACKAGE}/compressed/linux-x86_64.id"), windows $(cat "${PACKAGE}/compressed/windows-x86_64.id")"
