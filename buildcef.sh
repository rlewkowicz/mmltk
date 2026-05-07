#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_root="${script_dir}/.docker-cache/cef-build"
cache_dir="${script_dir}/.docker-cache/cef"
docker_image="${CEF_DOCKER_IMAGE:-ubuntu:22.04}"
host_build_jobs="${CEF_BUILD_JOBS:-$(nproc)}"

cef_branch="7680"
chromium_tag="refs/tags/146.0.7680.179"
archive_name="mmltk_cef_linux64_minimal_debug.tar.bz2"
archive_path="${cache_dir}/${archive_name}"
archive_sha_path="${archive_path}.sha256"
no_work_sentinel="${cache_dir}/.mmltk-cef-build-no-work"
deps_image_suffix="$(
    printf '%s-%s-%s' "${docker_image}" "${cef_branch}" "${chromium_tag}" |
        tr '/:' '--' |
        tr -c '[:alnum:]_.-' '-'
)"
deps_image="${CEF_DEPS_DOCKER_IMAGE:-mmltk-cef-builddeps:${deps_image_suffix}}"
rebuild_deps_image="${CEF_REBUILD_DEPS_IMAGE:-0}"

require_command() {
    local command_name="$1"
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        printf 'buildcef.sh: required command is missing: %s\n' "${command_name}" >&2
        exit 1
    fi
}

require_path() {
    local path="$1"
    if [[ ! -e "${path}" ]]; then
        printf 'buildcef.sh: missing required path: %s\n' "${path}" >&2
        exit 1
    fi
}

require_command docker
require_command flock
require_command python3
require_command sha256sum
require_path "${script_dir}/third_party/cef/automate-git.py"
require_path "${script_dir}/third_party/cef/patcher.py"
require_path "${script_dir}/third_party/cef/restore_siso_mtimes.py"
require_path "${script_dir}/third_party/cef/patch/patch.cfg"
require_path "${script_dir}/third_party/cef/patch/patches"

cef_env=(
    CEF_INSTALL_SYSROOT="x64"
    GN_ARGUMENTS=""
    GN_DEFINES="use_sysroot=true symbol_level=1 is_cfi=false use_thin_lto=false"
    MMLTK_CEF_BUILD_ARCH="x64"
    MMLTK_CEF_BUILD_TARGET="libcef"
    MMLTK_CEF_PATCH_ROOT="/repo/third_party/cef/patch"
)

if [[ -f "${archive_path}" && -f "${archive_sha_path}" \
      && -f "${build_root}/.mmltk-cef-build-state.json" ]]; then
    if env "${cef_env[@]}" python3 \
        "${script_dir}/third_party/cef/stock_authoring_checkout.py" \
        --check-cache-hit \
        --repo-root "${script_dir}" \
        --build-root "${build_root}" \
        --archive-path "${archive_path}"; then
        printf 'buildcef.sh: cache hit; reusing %s\n' "${archive_path}"
        ls -lh "${archive_path}" "${archive_sha_path}"
        exit 0
    fi
fi

mkdir -p "${build_root}" "${cache_dir}"
rm -f "${no_work_sentinel}"

build_lock_path="${build_root}/.buildcef.lock"
container_name_hash="$(printf '%s' "${build_root}" | sha256sum)"
container_name_hash="${container_name_hash%% *}"
container_name="mmltk-cef-build-${container_name_hash:0:16}"
siso_lock_path="${build_root}/code/chromium_git/chromium/src/out/Debug_GN_x64/.siso_lock"

container_uses_build_root() {
    local container_id="$1"
    local mount_source

    mount_source="$(
        docker inspect \
            --format '{{range .Mounts}}{{if eq .Destination "/work"}}{{.Source}}{{end}}{{end}}' \
            "${container_id}" 2>/dev/null || true
    )"
    [[ "${mount_source}" == "${build_root}" ]]
}

has_running_build_container() {
    local container_id

    while IFS= read -r container_id; do
        [[ -n "${container_id}" ]] || continue
        if container_uses_build_root "${container_id}"; then
            return 0
        fi
    done < <(docker ps -q 2>/dev/null || true)

    return 1
}

cleanup_build_containers() {
    local container_id
    local container_name_for_log

    if docker container inspect "${container_name}" >/dev/null 2>&1; then
        printf 'buildcef.sh: removing previous CEF build container: %s\n' "${container_name}" >&2
        docker rm -f "${container_name}" >/dev/null 2>&1 || true
    fi

    while IFS= read -r container_id; do
        [[ -n "${container_id}" ]] || continue
        if container_uses_build_root "${container_id}"; then
            container_name_for_log="$(docker inspect --format '{{.Name}}' "${container_id}" 2>/dev/null || true)"
            container_name_for_log="${container_name_for_log#/}"
            [[ -n "${container_name_for_log}" ]] || container_name_for_log="${container_id}"
            printf 'buildcef.sh: removing previous CEF build container using %s: %s\n' \
                "${build_root}" "${container_name_for_log}" >&2
            docker rm -f "${container_id}" >/dev/null 2>&1 || true
        fi
    done < <(docker ps -q 2>/dev/null || true)
}

cleanup_stale_siso_lock() {
    local lock_contents

    if [[ ! -f "${siso_lock_path}" ]] || has_running_build_container; then
        return
    fi

    lock_contents=""
    IFS= read -r lock_contents <"${siso_lock_path}" || true
    printf 'buildcef.sh: removing stale Siso lock: %s (%s)\n' \
        "${siso_lock_path}" "${lock_contents:-unknown holder}" >&2
    rm -f "${siso_lock_path}"
}

cleanup_after_signal() {
    local signal_name="$1"

    printf 'buildcef.sh: caught %s; cleaning up interrupted CEF build.\n' "${signal_name}" >&2
    cleanup_build_containers
    cleanup_stale_siso_lock
    exit 130
}

exec 9>"${build_lock_path}"
if ! flock -n 9; then
    printf 'buildcef.sh: another buildcef.sh process is already using %s\n' "${build_root}" >&2
    exit 1
fi

trap 'cleanup_after_signal INT' INT
trap 'cleanup_after_signal TERM' TERM
trap 'cleanup_after_signal HUP' HUP

cleanup_build_containers
cleanup_stale_siso_lock

if [[ "${rebuild_deps_image}" == "1" ]] || ! docker image inspect "${deps_image}" >/dev/null 2>&1; then
    printf 'buildcef.sh: building CEF dependency image: %s\n' "${deps_image}"
    docker build \
        --build-arg "BASE_IMAGE=${docker_image}" \
        --build-arg "CHROMIUM_TAG=${chromium_tag}" \
        -t "${deps_image}" \
        - <<'DOCKERFILE'
ARG BASE_IMAGE=ubuntu:22.04
FROM ${BASE_IMAGE}

ARG CHROMIUM_TAG
ENV CHROMIUM_TAG="${CHROMIUM_TAG}" \
    DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        bzip2 \
        ca-certificates \
        file \
        git \
        lsb-release \
        procps \
        python3 \
        python3-pip \
        software-properties-common \
        sudo \
        xz-utils \
    && add-apt-repository -y universe \
    && apt-get update

RUN python3 - <<'PY'
import base64
import os
import urllib.request

chromium_tag = os.environ['CHROMIUM_TAG']
url = (
    'https://chromium.googlesource.com/chromium/src/+/'
    f'{chromium_tag}/build/install-build-deps.py?format=TEXT'
)
with urllib.request.urlopen(url) as response:
    encoded = response.read()
with open('/tmp/mmltk-install-build-deps.py', 'wb') as output:
    output.write(base64.b64decode(encoded))
PY

RUN python3 /tmp/mmltk-install-build-deps.py --no-prompt --no-arm --no-chromeos-fonts --no-nacl \
    && apt-get install -y --no-install-recommends libx11-xcb-dev \
    && python3 -m pip install dataclasses importlib_metadata \
    && rm -f /tmp/mmltk-install-build-deps.py \
    && rm -rf /var/lib/apt/lists/*
DOCKERFILE
else
    printf 'buildcef.sh: reusing CEF dependency image: %s\n' "${deps_image}"
fi

docker run --rm -i \
    --name "${container_name}" \
    --label "mmltk.cef.build=1" \
    --label "mmltk.cef.build_root=${build_root}" \
    -e CEF_INSTALL_SYSROOT="x64" \
    -e GN_DEFINES="use_sysroot=true symbol_level=1 is_cfi=false use_thin_lto=false" \
    -e MMLTK_CEF_BUILD_ARCH="x64" \
    -e MMLTK_CEF_BUILD_TARGET="libcef" \
    -e MMLTK_CEF_NO_WORK_SENTINEL="/artifacts/$(basename "${no_work_sentinel}")" \
    -e MMLTK_CEF_PATCH_ROOT="/repo/third_party/cef/patch" \
    -v "${build_root}:/work" \
    -v "${cache_dir}:/artifacts" \
    -v "${script_dir}:/repo:ro" \
    -w /work \
    "${deps_image}" \
    bash -lc "
        set -euo pipefail
        for safe_dir in \
            /work/code/depot_tools \
            /work/code/chromium_git/cef \
            /work/code/chromium_git/chromium/src \
            /work/code/chromium_git/chromium/src/cef \
            /work/code/chromium_git/chromium/src/third_party/angle \
            /work/code/chromium_git/chromium/src/third_party/depot_tools \
            /work/code/chromium_git/chromium/src/v8; do
            if [[ -d \${safe_dir} ]]; then
                git config --global --add safe.directory \"\${safe_dir}\"
            fi
        done
        state_output=/work/.mmltk-cef-build.env
        MMLTK_CEF_AUTOMATE_NO_UPDATE=0
        MMLTK_CEF_SKIP_GCLIENT_HOOK=0
        MMLTK_CEF_BUILD_CHECKOUT_MODE=cold
        rm -f \"\${state_output}\"
        if [[ -e /work/code/chromium_git/chromium/src/.git && \
              -e /work/code/chromium_git/cef/.git && \
              -e /work/code/chromium_git/chromium/src/cef/.git ]]; then
            python3 /repo/third_party/cef/stock_authoring_checkout.py \
                --repo-root /repo \
                --build-root /work \
                --state-output \"\${state_output}\" \
                --ensure-patched-build-checkout
            if [[ -f \"\${state_output}\" ]]; then
                source \"\${state_output}\"
            fi
            export MMLTK_CEF_PATCHES_ALREADY_APPLIED=1
        else
            printf 'buildcef.sh: CEF source cache is incomplete; automate-git will create it before applying patches.\n'
        fi
        printf 'buildcef.sh: CEF build checkout mode: %s\n' \"\${MMLTK_CEF_BUILD_CHECKOUT_MODE}\"
        if [[ \"\${MMLTK_CEF_BUILD_CHECKOUT_MODE}\" == \"hot\" || \
              \"\${MMLTK_CEF_BUILD_CHECKOUT_MODE}\" == \"patch-suffix\" || \
              \"\${MMLTK_CEF_BUILD_CHECKOUT_MODE}\" == \"patch-targets\" ]]; then
            mtime_jobs=\"\$(nproc)\"
            printf 'buildcef.sh: restoring Siso mtimes with %s workers.\n' \"\${mtime_jobs}\"
            python3 /repo/third_party/cef/restore_siso_mtimes.py \
                --src-root /work/code/chromium_git/chromium/src \
                --build-dir /work/code/chromium_git/chromium/src/out/Debug_GN_x64 \
                --jobs \"\${mtime_jobs}\" \
                --require-state
        else
            printf 'buildcef.sh: skipping Siso mtime restore for %s checkout.\n' \
                \"\${MMLTK_CEF_BUILD_CHECKOUT_MODE}\"
        fi
        automate_args=(
            python3 /repo/third_party/cef/automate-git.py
            --download-dir=/work/code/chromium_git
            --depot-tools-dir=/work/code/depot_tools
            --no-depot-tools-update
            --branch='${cef_branch}'
            --chromium-checkout='${chromium_tag}'
            --x64-build
            --build-target=libcef
            --force-build
            --no-release-build
            --no-distrib
            --mmltk-patcher-source=/repo/third_party/cef/patcher.py
            --mmltk-build-jobs='${host_build_jobs}'
            --mmltk-output-dir=/artifacts
            --mmltk-output-archive-name='${archive_name}'
        )
        if [[ \"\${MMLTK_CEF_AUTOMATE_NO_UPDATE}\" == \"1\" ]]; then
            automate_args+=(--no-update)
        fi
        if [[ \"\${MMLTK_CEF_SKIP_GCLIENT_HOOK}\" == \"1\" ]]; then
            automate_args+=(--mmltk-skip-gclient-hook)
        fi
        \"\${automate_args[@]}\"
        python3 /repo/third_party/cef/stock_authoring_checkout.py \
            --repo-root /repo \
            --build-root /work \
            --state-output \"\${state_output}\" \
            --record-patched-build-checkout
    "

if [[ -f "${no_work_sentinel}" ]]; then
    rm -f "${no_work_sentinel}"
    printf 'buildcef.sh: ninja reported no work; artifact copy and packaging skipped.\n'
    exit 0
fi

if [[ ! -f "${archive_path}" ]]; then
    printf 'buildcef.sh: expected archive was not created: %s\n' "${archive_path}" >&2
    exit 1
fi

if [[ ! -f "${archive_sha_path}" ]]; then
    printf 'buildcef.sh: expected archive checksum was not created: %s\n' "${archive_sha_path}" >&2
    exit 1
fi

printf '\nSelf-contained CEF build complete.\n'
ls -lh "${archive_path}" "${archive_sha_path}"
