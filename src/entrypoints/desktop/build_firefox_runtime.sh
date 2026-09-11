#!/usr/bin/env bash
set -euo pipefail

: "${MMLTK_FIREFOX_CACHE_ROOT:?MMLTK_FIREFOX_CACHE_ROOT is required}"
: "${MMLTK_FIREFOX_BUILD_INPUT:?MMLTK_FIREFOX_BUILD_INPUT is required}"
: "${MMLTK_FIREFOX_BUILT_INPUT:?MMLTK_FIREFOX_BUILT_INPUT is required}"
: "${MMLTK_FIREFOX_MACH:?MMLTK_FIREFOX_MACH is required}"
: "${MMLTK_TRACED_COMMAND:?MMLTK_TRACED_COMMAND is required}"
: "${MMLTK_JOBS:?MMLTK_JOBS is required}"

: "${MMLTK_WORKSPACE_GRAPHICS_ABI:?MMLTK_WORKSPACE_GRAPHICS_ABI is required}"
export MMLTK_WORKSPACE_GRAPHICS_ABI

export CARGO_PROFILE_RELEASE_CODEGEN_UNITS="${MMLTK_JOBS}"

runtime_root="${MMLTK_FIREFOX_CACHE_ROOT}/obj-minimal-opt/dist/firefox"
runtime_complete() {
    local required_path
    for required_path in \
        firefox \
        firefox-bin \
        libxul.so \
        libmozgtk.so \
        libmozwayland.so \
        application.ini \
        platform.ini \
        omni.ja \
        browser/omni.ja; do
        [[ -e "${runtime_root}/${required_path}" ]] || return 1
    done
}

next_built_input="${MMLTK_FIREFOX_BUILT_INPUT}.next.$$"
cleanup_next_built_input() {
    rm -f -- "${next_built_input}"
}
trap cleanup_next_built_input EXIT
{
    cat -- "${MMLTK_FIREFOX_BUILD_INPUT}"
    sha256sum -- "${MMLTK_WORKSPACE_GRAPHICS_ABI}" | cut -d' ' -f1
} > "${next_built_input}"

if [[ -f "${MMLTK_FIREFOX_BUILT_INPUT}" ]] \
    && cmp -s -- "${next_built_input}" \
        "${MMLTK_FIREFOX_BUILT_INPUT}" \
    && runtime_complete; then
    exit 0
fi

"${MMLTK_TRACED_COMMAND}" firefox_build \
    "${MMLTK_FIREFOX_MACH}" build \
        --jobs "${MMLTK_JOBS}" \
        --priority normal
"${MMLTK_FIREFOX_MACH}" mmltk-stage-runtime

mv -f -- "${next_built_input}" "${MMLTK_FIREFOX_BUILT_INPUT}"
trap - EXIT
