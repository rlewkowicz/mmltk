#!/usr/bin/env bash
set -euo pipefail

if (($# < 2)); then
    printf 'usage: %s <preset> <build-dir> [cmake-options...]\n' "$0" >&2
    exit 2
fi

preset="$1"
build_dir="$2"
shift 2

: "${MMLTK_CONTAINER_CACHE_ROOT:?MMLTK_CONTAINER_CACHE_ROOT is required}"
: "${MMLTK_TOOLCHAIN_ID:?MMLTK_TOOLCHAIN_ID is required}"

case "${build_dir}" in
    "${MMLTK_CONTAINER_CACHE_ROOT}/cmake/"*)
        ;;
    *)
        printf 'refusing CMake build directory outside the cache root: %s\n' \
            "${build_dir}" >&2
        exit 2
        ;;
esac

identity_file="${build_dir}/.mmltk-toolchain-identity"
configure_identity_file="${build_dir}/.mmltk-configure-identity"
configure_identity="$({
    printf '%s\0%s\0' "${preset}" "${MMLTK_TOOLCHAIN_ID}"
    printf '%s\0' "$@"
} | sha256sum | cut -d' ' -f1)"
configure_state=skipped
if [[ ! -f "${build_dir}/build.ninja" ]] \
    || [[ ! -f "${identity_file}" ]] \
    || [[ "$(<"${identity_file}")" != "${MMLTK_TOOLCHAIN_ID}" ]] \
    || [[ ! -f "${configure_identity_file}" ]] \
    || [[ "$(<"${configure_identity_file}")" != "${configure_identity}" ]]; then
    configure_state=executed
    if [[ -e "${build_dir}" ]]; then
        cmake -E rm -rf "${build_dir}"
    fi
    cmake --preset "${preset}" -B "${build_dir}" "$@"
    identity_next="${identity_file}.next.$$"
    configure_identity_next="${configure_identity_file}.next.$$"
    printf '%s\n' "${MMLTK_TOOLCHAIN_ID}" >"${identity_next}"
    printf '%s\n' "${configure_identity}" >"${configure_identity_next}"
    mv -f "${identity_next}" "${identity_file}"
    mv -f "${configure_identity_next}" "${configure_identity_file}"
fi

printf 'mmltk: CMake configure %s for %s\n' "${configure_state}" "${preset}" >&2
if [[ -n "${MMLTK_BUILD_TRACE_FILE:-}" ]]; then
    printf '{"timestamp_ns":%s,"pid":%d,"phase":"configure:%s","event":"finish","result":"%s"}\n' \
        "$(date +%s%N)" "${BASHPID}" "${preset}" "${configure_state}" \
        >>"${MMLTK_BUILD_TRACE_FILE}"
fi
