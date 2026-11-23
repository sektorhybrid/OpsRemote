# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
# OpsRemote's version as CMakeLists.txt sets it, for the scripts (sourced):
# project(OpsRemote VERSION X.Y.Z) and, for a pre-release,
# set(OPSREMOTE_VERSION_LABEL "beta.1"), giving X.Y.Z-beta.1.

# The version pattern scripts accept: X.Y.Z, or X.Y.Z-beta.N / X.Y.Z-rc.N.
OPSREMOTE_VERSION_PATTERN='^([0-9]+)\.([0-9]+)\.([0-9]+)(-(beta|rc)\.([1-9][0-9]*))?$'

# The full version from CMakeLists.txt in folder $1 (default: the current one).
opsremote_version() {
    local cmake="${1:-.}/CMakeLists.txt" number label
    number="$(sed -n 's/^project(OpsRemote VERSION \([0-9.]*\)[ )].*/\1/p' "${cmake}")"
    label="$(sed -n 's/^set(OPSREMOTE_VERSION_LABEL "\([^"]*\)")$/\1/p' "${cmake}")"
    echo "${number}${label:+-${label}}"
}

# A version as a sortable key "major minor patch stage number": a
# pre-release sorts before its release (beta < rc < release).
opsremote_version_key() {
    [[ "$1" =~ ${OPSREMOTE_VERSION_PATTERN} ]] || return 1
    local stage=2 number=0
    case "${BASH_REMATCH[5]}" in
        beta) stage=0 number="${BASH_REMATCH[6]}" ;;
        rc) stage=1 number="${BASH_REMATCH[6]}" ;;
    esac
    printf '%d %d %d %d %d\n' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${BASH_REMATCH[3]}" "${stage}" "${number}"
}
