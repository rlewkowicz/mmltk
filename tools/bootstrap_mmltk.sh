#!/usr/bin/env bash
# Bootstrap only: all wrapper operations belong to tools/mmltk.cpp.
set -euo pipefail
operation="${1:?bootstrap operation required}"
checkout="${2:?checkout required}"
launch=false
launch_arguments=("${@:3}")
case "${operation}" in --launch) launch=true; operation=--ensure;; --ensure|--build|--image) ;; *) exit 2 ;; esac
checkout="$(realpath -e -- "${checkout}")"
cache="${MMLTK_CACHE_ROOT:-${checkout}/.cache}"
[[ "${cache}" == /* ]] || cache="${checkout}/${cache}"
cache="$(realpath -m -- "${cache}")"
case "${cache}" in "${checkout}/.cache"|"${checkout}/.cache/"*) ;; *) printf '%s\n' 'mmltk bootstrap: cache must stay below the checkout .cache' >&2; exit 1 ;; esac
# Exec keeps the checkout entry PID and working directory across cold bootstrap
# and ordinary startup. Release build-only descriptors before handing it over.
finish() {
    if "${launch}"; then
        if [[ -n "${tools_lock:-}" ]]; then flock -u "${tools_lock}"; exec {tools_lock}>&-; fi
        if [[ -n "${original_stdout:-}" ]]; then
            exec 1>&"${original_stdout}" 2>&"${original_stderr}"
            exec {original_stdout}>&- {original_stderr}>&-
        fi
        trap - EXIT HUP INT TERM
        exec "${checkout}/build/tools/mmltk" "${launch_arguments[@]}"
    fi
}
# Hash bytes and names, including untracked edits, without any compiler on the host.
inputs() {
    cd -- "${checkout}"
    printf '%s\0' mmltk docker/Dockerfile.gcc docker/Dockerfile.tools docker/nvidia-payload.json
    find tools third_party/json/include -type f ! -path '*/__pycache__/*' ! -path '*/build/*' ! -path '*/generated/*' -print0
}
fingerprint="$({ printf 'native-tools-v1\n'; (cd -- "${checkout}"; inputs | LC_ALL=C sort -zu | xargs -0 -r sha256sum --); } | sha256sum)"
fingerprint="${fingerprint%% *}"
if [[ "${operation}" == --ensure && -x "${checkout}/build/tools/mmltk" && -f "${checkout}/build/tools/.inputs.sha256" && "$(<"${checkout}/build/tools/.inputs.sha256")" == "${fingerprint}" ]]; then finish; exit 0; fi
# Read-only operations retain their no-build/no-pull admission on a cold checkout.
if [[ "${operation}" == --ensure ]]; then
    case "${3:-}:${4:-}" in
        --logs:*|--diagnose-processes:*|--diagnose-native-symbols:*|--diagnose-native-link:*|--diagnose-gpu-program:*|--diagnose-gpu-environment:*|--diagnose-io:*|--diagnose-benchmark-image:*|--diagnose-nvidia-payload:development|--test:log-query-tool|--test:headless-compositor-tool|--test=log-query-tool:*|--test=headless-compositor-tool:*)
            printf '%s\n' 'mmltk: native tools are missing or stale; run ./mmltk --build-tools first (diagnostic did not build or pull)' >&2
            exit 1;;
    esac
fi
mkdir -p -- "${checkout}/.cache/locks" "${checkout}/build/.tools-generations" "${cache}/cmake"
exec {tools_lock}>"${checkout}/.cache/locks/tools.lock"
flock -x "${tools_lock}"
if [[ "${operation}" == --ensure && -x "${checkout}/build/tools/mmltk" && -f "${checkout}/build/tools/.inputs.sha256" && "$(<"${checkout}/build/tools/.inputs.sha256")" == "${fingerprint}" ]]; then finish; exit 0; fi
# Bootstrap failures retain evidence even before the native transcript owner exists.
if [[ "${MMLTK_BOOTSTRAP_CAPTURED:-0}" != 1 ]]; then
    mkdir -p -- "${checkout}/build/logs"
    transcript="${checkout}/build/logs/mmltk-tools-bootstrap-$(date +%Y%m%d-%H%M%S)-$$.log"
    printf 'mmltk: tools bootstrap log: %s\n' "${transcript}" >&2
    exec {original_stdout}>&1 {original_stderr}>&2
    exec > >(tee -a -- "${transcript}" >&2) 2> >(tee -a -- "${transcript}" >&2)
fi
child=''
container=''
stage=''
cleanup() {
    status=$?
    trap - EXIT HUP INT TERM
    if [[ -n "${child}" ]]; then
        kill -TERM -- "-${child}" 2>/dev/null || true
        timeout --signal=KILL 5s pidwait -F /dev/stdin <<<"${child}" >/dev/null 2>&1 || true
        kill -KILL -- "-${child}" 2>/dev/null || true
        wait "${child}" 2>/dev/null || true
    fi
    if [[ -n "${container}" ]]; then timeout --signal=KILL 5s docker rm -f "${container}" >/dev/null 2>&1 || true; fi
    if [[ -n "${stage}" && ! -f "${stage}/.inputs.sha256" ]]; then rm -rf -- "${stage}"; fi
    rm -f -- "${checkout}/build/.tools-next-$$"
    if (( status != 0 )); then printf 'mmltk bootstrap: native tools failed (exit %s)\n' "${status}" >&2; fi
    exit "${status}"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
# Keep the active Docker client cancellable, including image provisioning.
# setsid gives its subprocess tree one bounded cancellation boundary.
docker() {
    setsid --wait docker "$@" <&0 &
    child=$!
    local status=0
    wait "${child}" || status=$?
    child=''
    return "${status}"
}
type -P docker >/dev/null || { printf '%s\n' 'mmltk bootstrap: docker is not installed' >&2; exit 1; }
if ! docker version --format '{{.Server.Version}}' >/dev/null 2>&1; then
    for attempt in 'systemctl --user start docker' 'systemctl start docker' 'service docker start' 'sudo -n systemctl --user start docker' 'sudo -n systemctl start docker' 'sudo -n service docker start'; do
        read -r -a command <<<"${attempt}"
        if "${command[@]}" >/dev/null 2>&1; then break; fi
    done
    docker version --format '{{.Server.Version}}' >/dev/null || { printf '%s\n' 'mmltk bootstrap: docker is unavailable' >&2; exit 1; }
fi
# Docker resolves sources on its daemon host. Translate the current container's
# longest matching mount destination; a host invocation already uses host paths.
host_checkout="${checkout}"
if [[ -f /.dockerenv && "${MMLTK_DOCKER_CONTAINER:-}" != host ]]; then
    mount_rows="$(docker inspect --format '{{range .Mounts}}{{printf "%s\t%s\n" .Destination .Source}}{{end}}' "${MMLTK_DOCKER_CONTAINER:-${HOSTNAME}}" 2>/dev/null)" || {
        printf '%s\n' 'mmltk bootstrap: cannot resolve this container mount mapping' >&2; exit 1;
    }
    longest=0
    while IFS=$'\t' read -r destination source; do
        [[ -n "${destination}" && -n "${source}" ]] || continue
        if [[ "${checkout}" == "${destination}" || "${checkout}" == "${destination}/"* ]]; then
            if (( ${#destination} > longest )); then
                host_checkout="${source}${checkout#"${destination}"}"
                longest=${#destination}
            fi
        fi
    done <<<"${mount_rows}"
    (( longest > 0 )) || { printf '%s\n' 'mmltk bootstrap: checkout is not a daemon-visible bind mount' >&2; exit 1; }
fi
[[ "${host_checkout}" != *:* ]] || { printf '%s\n' 'mmltk bootstrap: checkout mount path cannot contain a colon' >&2; exit 1; }
gcc_image="${MMLTK_GCC_IMAGE:-mmltk-gcc16:latest}"
gcc_fingerprint="$({ printf 'gcc16\nubuntu:24.04\n'; sha256sum -- "${checkout}/docker/Dockerfile.gcc"; } | sha256sum)"
gcc_fingerprint="${gcc_fingerprint%% *}"
if [[ "$(docker image inspect "${gcc_image}" --format '{{index .Config.Labels "com.mmltk.input-fingerprint"}}' 2>/dev/null || true)" != "${gcc_fingerprint}" ]]; then
    docker buildx build --builder default --progress=plain --load --target gcc16 --tag "${gcc_image}" --file "${checkout}/docker/Dockerfile.gcc" --build-arg MMLTK_GCC_BUILD_IMAGE=ubuntu:24.04 --build-arg "MMLTK_IMAGE_FINGERPRINT=${gcc_fingerprint}" "${checkout}"
fi
gcc_identity="$(docker image inspect "${gcc_image}" --format '{{.Id}}')"
gcc_alias="mmltk-local-gcc16:${gcc_identity#sha256:}"
docker tag "${gcc_identity}" "${gcc_alias}"
donor="$(sed -n 's/.*"donor": "\([^"]*\)".*/\1/p' "${checkout}/docker/nvidia-payload.json")"
cuda_source="$(sed -n 's/.*"cuda_source": "\([^"]*\)".*/\1/p' "${checkout}/docker/nvidia-payload.json")"
[[ "${cuda_source}" == /* ]] || { printf '%s\n' 'mmltk bootstrap: missing canonical CUDA source' >&2; exit 1; }
[[ "${donor}" == *@sha256:* ]] || { printf '%s\n' 'mmltk bootstrap: invalid pinned NVIDIA donor' >&2; exit 1; }
tools_image="${MMLTK_TOOLS_IMAGE:-mmltk-tools:latest}"
image_fingerprint="$({ printf '%s\n' "${gcc_identity}" "${donor}" "${cuda_source}"; sha256sum -- "${checkout}/docker/Dockerfile.tools"; } | sha256sum)"
image_fingerprint="${image_fingerprint%% *}"
if [[ "$(docker image inspect "${tools_image}" --format '{{index .Config.Labels "com.mmltk.input-fingerprint"}}' 2>/dev/null || true)" != "${image_fingerprint}" ]]; then
    docker buildx build --builder default --progress=plain --load --tag "${tools_image}" --file "${checkout}/docker/Dockerfile.tools" --build-context "mmltk_gcc16=docker-image://${gcc_alias}" --build-arg "MMLTK_NVIDIA_DONOR_IMAGE=${donor}" --build-arg "MMLTK_CUDA_SOURCE=${cuda_source}" --build-arg "MMLTK_IMAGE_FINGERPRINT=${image_fingerprint}" "${checkout}"
fi
[[ "${operation}" != --image ]] || exit 0
generation="${fingerprint}-$$"
stage="${checkout}/build/.tools-generations/${generation}"
container="${MMLTK_BOOTSTRAP_CONTAINER:-mmltk-tools-$(printf %s "${checkout}" | sha256sum | cut -c1-12)-$$}"
owner_pid="${MMLTK_BOOTSTRAP_OWNER_PID:-$$}"
owner_stat="$(<"/proc/${owner_pid}/stat")"
owner_stat="${owner_stat##*) }"
read -r -a owner_fields <<<"${owner_stat}"
owner_start="${owner_fields[19]}"
mkdir -p -- "${stage}"
container_cache="/workspace${cache#"${checkout}"}/cmake/tools"
docker run --rm --init --name "${container}" --label com.mmltk.wrapper=1 --label "com.mmltk.repo=${checkout}" --label "com.mmltk.owner-pid=${owner_pid}" --label "com.mmltk.owner-start=${owner_start}" --label com.mmltk.mode=build-tools --user "$(id -u):$(id -g)" --volume "${host_checkout}:/workspace:rw" --workdir /workspace --env HOME=/tmp --env "MMLTK_TOOLS_BUILD=${container_cache}" --env "MMLTK_TOOLS_STAGE=/workspace/build/.tools-generations/${generation}" --env "MMLTK_JOBS=$(getconf _NPROCESSORS_ONLN)" --entrypoint /bin/bash "${tools_image}" -c 'set -euo pipefail
cmake -S /workspace/tools -B "${MMLTK_TOOLS_BUILD}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMMLTK_TOOLS_BUILD_DRIVER=ON
cmake --build "${MMLTK_TOOLS_BUILD}" --parallel "${MMLTK_JOBS}" --target mmltk
cmake --install "${MMLTK_TOOLS_BUILD}" --prefix "${MMLTK_TOOLS_STAGE}" --component Runtime
'
# The loader check runs the new native command itself; it performs no workload.
"${stage}/mmltk" --tool runtime-package -- inventory >/dev/null
printf '%s\n' "${fingerprint}" >"${stage}/.inputs.sha256"
ln -s -- ".tools-generations/${generation}" "${checkout}/build/.tools-next-$$"
# A directory at build/tools indicates an older manually populated closure.
[[ ! -d "${checkout}/build/tools" || -L "${checkout}/build/tools" ]] || { printf '%s\n' 'mmltk bootstrap: build/tools must be a published symlink' >&2; exit 1; }
mv -Tf -- "${checkout}/build/.tools-next-$$" "${checkout}/build/tools"
printf '%s\n' 'mmltk: native tools published' >&2
finish
