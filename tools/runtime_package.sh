#!/usr/bin/env bash

# Each row owns a staged directory, its image destination, its pre-install
# source, and required entries. A ? prefix marks an optional installed tool.
# Source trees are hashed whole; release/repository files are hashed selectively.
runtime_package_inventory() {
    "$@" bin /opt/mmltk/bin release . \
        mmltk mmltk-browser-bundle-contract mmltk-browser-host \
        '?mmltk-rfdetr-onnx-info' '?mmltk-rfdetr-onnx-simplify' || return
    "$@" lib/gcc-16.2 /opt/gcc-16.2/lib64 image . \
        libstdc++.so.6 libgcc_s.so.1 || return
    "$@" lib/mmltk/firefox /opt/mmltk/lib/mmltk/firefox firefox . \
        firefox firefox-bin libmozgtk.so libxul.so || return
    "$@" share/mmltk/licenses/gdrcopy /opt/mmltk/share/mmltk/licenses/gdrcopy repository \
        third_party/gdrcopy LICENSE UPSTREAM.md || return
    "$@" models /opt/mmltk/models repository src/backend/imaging/upscale/assets \
        ShiftLUT_fp32.onnx RealPLKSR_fp16.onnx || return
    "$@" share/mmltk/python /opt/mmltk/share/mmltk/python repository \
        src/backend/models/rfdetr/training rfdetr_checkpoint_bridge.py || return
    "$@" share/mmltk/browser-app /opt/mmltk/share/mmltk/browser-app browser \
        generated/frontend/iced/browser_protocol.marker \
        index.html browser_protocol.marker
}

runtime_package_check_directory() {
    local root="$1" layout="$2" path="$3" destination="$4" package_path="$3"
    shift 6
    if [[ "${layout}" == image ]]; then path="${destination#/}"; fi
    [[ -d "${root}/${path}" ]] || return 1
    local entry
    for entry in "$@"; do
        [[ "${entry}" == \?* ]] && continue
        [[ -e "${root}/${path}/${entry}" ]] || return 1
    done
    if [[ "${package_path}" == bin ]]; then
        local test_executable
        test_executable="$(find -L "${root}/${path}" -maxdepth 1 -type f -executable \
            \( -name 'mmltk_acceptance' -o -name 'mmltk_*_tests' \) -print -quit)" \
            || return 1
        [[ -z "${test_executable}" ]] || return 1
    fi
}

runtime_package_complete() {
    local root="$1" layout="${2:-stage}"
    runtime_package_inventory runtime_package_check_directory "${root}" "${layout}"
}

runtime_package_path_fingerprint() {
    local root="$1"
    if [[ ! -e "${root}" && ! -L "${root}" ]]; then
        printf 'missing\n' | sha256sum | cut -d' ' -f1
        return
    fi
    # One sorted stream retains names, types, modes, symlink targets and bytes,
    # including empty directories. Ignore timestamps and host ownership, as
    # image assembly does. Normalize the top-level name across .next/release.
    {
        local directory member
        if [[ -d "${root}" ]]; then
            if [[ -L "${root}" ]]; then
                printf 'root-link\0%s\0' "$(readlink -- "${root}")"
            fi
            directory="${root}"
            member=.
        else
            directory="$(dirname -- "${root}")"
            member="$(basename -- "${root}")"
        fi
        tar --format=gnu --sort=name --mtime=@0 --owner=0 --group=0 \
            --numeric-owner --hard-dereference --transform='flags=r;s,^[^/]*,entry,' \
            --create --file=- --directory="${directory}" -- "${member}"
    } | sha256sum | cut -d' ' -f1
}

runtime_package_hash_directory() {
    local root="$1" path="$2" fingerprint
    fingerprint="$(runtime_package_path_fingerprint "${root}/${path}")" || return
    printf '%s %s\n' "${path}" "${fingerprint}"
}

runtime_package_fingerprint() {
    runtime_package_inventory runtime_package_hash_directory "$1" \
        | sha256sum | cut -d' ' -f1
}

runtime_package_copy_directory() {
    local root="$1" path="$2" destination="$3"
    mkdir -p -- "${destination}" || return
    cp -a --no-preserve=ownership -- "${root}/${path}/." "${destination}/"
}

runtime_package_install() {
    local root="$1"
    runtime_package_complete "${root}" || {
        echo "Packaged release is incomplete or contains test executables" >&2
        return 1
    }
    runtime_package_inventory runtime_package_copy_directory "${root}" || return
    runtime_package_complete / image
}
