#!/usr/bin/env bash
set -euo pipefail

if (($# < 2)); then
    printf 'usage: %s <phase> <command> [args...]\n' "$0" >&2
    exit 2
fi

phase="$1"
shift
[[ "${phase}" =~ ^[a-z0-9_.:-]+$ ]] || {
    printf 'invalid trace phase: %s\n' "${phase}" >&2
    exit 2
}

trace_event() {
    [[ -n "${MMLTK_BUILD_TRACE_FILE:-}" ]] || return 0
    local event="$1"
    local result="${2:-}"
    printf '{"timestamp_ns":%s,"pid":%d,"phase":"%s","event":"%s","result":"%s","jobs":%d}\n' \
        "$(date +%s%N)" "${BASHPID}" "${phase}" "${event}" "${result}" \
        "${MMLTK_JOBS:-0}" >>"${MMLTK_BUILD_TRACE_FILE}"
}

finish_traced_command() {
    local exit_status=$?
    trap - EXIT
    if (( exit_status == 0 )); then
        trace_event finish success
    else
        trace_event finish "failure:${exit_status}"
    fi
    exit "${exit_status}"
}

trace_event start
trap finish_traced_command EXIT
"$@"
