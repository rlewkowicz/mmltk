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

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd -- "${script_dir}/.." && pwd -P)"
cmake_configuration_inputs=(
    CMakePresets.json
    CMakeLists.txt
    docker/nvidia-payload.json
    docker/record_compiler_evidence.py
    tools/check_toolchain_invariants.py
    tools/configure_cmake_graph.sh
)
while IFS= read -r -d '' cmake_configuration_input; do
    cmake_configuration_inputs+=("${cmake_configuration_input}")
done < <(
    cd -- "${repo_root}"
    find cmake src -type f \( -name '*.cmake' -o -name CMakeLists.txt \) -print0 | LC_ALL=C sort -z
)

for cmake_configuration_input in "${cmake_configuration_inputs[@]}"; do
    if [[ ! -f "${repo_root}/${cmake_configuration_input}" ]]; then
        printf 'missing CMake configuration input: %s\n' \
            "${repo_root}/${cmake_configuration_input}" >&2
        exit 2
    fi
done

cmake_configuration_identity="$({
    for cmake_configuration_input in "${cmake_configuration_inputs[@]}"; do
        printf '%s\0' "${cmake_configuration_input}"
        sha256sum <"${repo_root}/${cmake_configuration_input}" | cut -d' ' -f1
        printf '\0'
    done
} | sha256sum | cut -d' ' -f1)"

identity_file="${build_dir}/.mmltk-toolchain-identity"
configure_identity_file="${build_dir}/.mmltk-configure-identity"
configure_identity="$({
    printf '%s\0%s\0%s\0' \
        "${preset}" "${MMLTK_TOOLCHAIN_ID}" "${cmake_configuration_identity}"
    printf '%s\0' "$@"
} | sha256sum | cut -d' ' -f1)"
configure_state=skipped
if [[ -e "${build_dir}" ]] && {
    [[ ! -f "${identity_file}" ]] \
        || [[ "$(<"${identity_file}")" != "${MMLTK_TOOLCHAIN_ID}" ]]
}; then
    # An incompatible compiler/dependency image cannot reuse native artifacts.
    cmake -E rm -rf "${build_dir}"
fi
if [[ ! -f "${build_dir}/build.ninja" ]] \
    || [[ ! -f "${build_dir}/CMakeCache.txt" ]] \
    || [[ ! -f "${identity_file}" ]] \
    || [[ ! -f "${configure_identity_file}" ]] \
    || [[ "$(<"${configure_identity_file}")" != "${configure_identity}" ]]; then
    configure_state=executed
    # Reapply defaults, preset values, and command-line overrides from scratch,
    # including removal of old options. Keep CMakeFiles (compiler detection,
    # objects and modules), generated outputs, and Ninja's build history.
    # Invalidate the successful configuration marker before mutating the graph
    # so a failed attempt cannot make an older invocation appear current.
    cmake -E rm -f "${build_dir}/CMakeCache.txt" "${configure_identity_file}"
    cmake --preset "${preset}" -B "${build_dir}" "$@"
fi

python3 "${script_dir}/check_toolchain_invariants.py" \
    --repo-root "${repo_root}" --build-dir "${build_dir}"

if [[ "${configure_state}" == executed ]]; then
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
