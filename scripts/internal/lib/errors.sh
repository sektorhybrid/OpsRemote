# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
# Safe error handling for OpsRemote's scripts.
# Source it after `set -euo pipefail`:
#   source "$(dirname -- "${BASH_SOURCE[0]}")/internal/lib/errors.sh"
# Never "continue on errors" (`|| true`, `set +e`): an outcome a command may
# have besides success is accepted by its exit code, and anything else stops
# the script, naming the command.

# Where a script stopped: the failed command (named by tolerate/capture,
# or by bash) and the chain of functions to it. -E: functions and $(…)
# subshells report too. Like set -e, it does not fire inside if conditions
# or && / || lists: use `if` only for real yes/no checks.
set -E
_on_error() {
    local status="$1" i
    echo "stopped (exit status ${status}) at:" >&2
    for ((i = 1; i < ${#FUNCNAME[@]}; i++)); do
        echo "  ${FUNCNAME[$i]} (${BASH_SOURCE[$i]}:${BASH_LINENO[$((i - 1))]})" >&2
    done
}
trap '_on_error $?' ERR

# Temporary files and folders removed when the script ends, however it
# ends (an error, Ctrl+C). Register them here (and other clean-up with
# on_exit_run); don't set an EXIT trap of your own, it would replace this
# one. ($(…) subshells don't run it.)
_remove_on_exit=()
on_exit_remove() {
    _remove_on_exit+=("$@")
}
# Functions run when the script ends, however it ends, before the removals
# (a clean-up that needs more than rm, e.g. deleting an unfinished draft).
_run_on_exit=()
on_exit_run() {
    _run_on_exit+=("$@")
}
_on_exit() {
    local status=$? step
    for step in "${_run_on_exit[@]}"; do
        "${step}" || echo "warning: clean-up step ${step} failed" >&2
    done
    if ((${#_remove_on_exit[@]})); then
        rm -rf -- "${_remove_on_exit[@]}"
    fi
    exit "${status}"
}
trap _on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Stops the script with a message.
die() {
    echo "error: $*" >&2
    exit 1
}

# tolerate CODE… -- COMMAND…
# Runs COMMAND. Success and the listed exit codes are expected outcomes (for
# example grep's 1, "no match", or diff's 1, "different"): the function
# returns 0 and leaves the real code in TOLERATED_STATUS. Any other code is
# a failure: it is reported with the command and returned, so `set -e` (or
# pipefail, in a pipeline) stops the script.
TOLERATED_STATUS=0
tolerate() {
    local allowed=() status=0 code
    while (($#)) && [[ "$1" != -- ]]; do
        allowed+=("$1")
        shift
    done
    [[ "${1:-}" == -- ]] || die "tolerate: missing -- before the command"
    shift
    "$@" || status=$?
    TOLERATED_STATUS="${status}"
    for code in 0 "${allowed[@]}"; do
        [[ "${status}" == "${code}" ]] && return 0
    done
    echo "error: '$*' failed (exit status ${status})" >&2
    return "${status}"
}

# capture VARIABLE CODE… -- COMMAND…
# Like tolerate, but keeps COMMAND's output in VARIABLE and its exit code in
# TOLERATED_STATUS, so the caller branches on the exact outcome. (tolerate
# inside "$(…)" runs in a subshell, where TOLERATED_STATUS is lost; and an
# `if` would take every failure for "no".)
capture() {
    local -n _captured="$1"
    shift
    local allowed=() status=0 code
    while (($#)) && [[ "$1" != -- ]]; do
        allowed+=("$1")
        shift
    done
    [[ "${1:-}" == -- ]] || die "capture: missing -- before the command"
    shift
    _captured="$("$@")" || status=$?
    TOLERATED_STATUS="${status}"
    for code in 0 "${allowed[@]}"; do
        [[ "${status}" == "${code}" ]] && return 0
    done
    echo "error: '$*' failed (exit status ${status})" >&2
    return "${status}"
}

# available WHAT CHECK…: whether an optional resource is there (CHECK is a
# command, e.g. `command -v docker`). If not, says "skipped: WHAT" with the
# reason, so a missing optional step is reported, never silently ignored.
available() {
    local what="$1"
    shift
    if "$@" > /dev/null 2>&1; then
        return 0
    fi
    echo "skipped: ${what} (not available: $*)" >&2
    return 1
}
