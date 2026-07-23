#!/usr/bin/env bash
set -euo pipefail

build_dir="$(realpath -m "${1:?build directory is required}")"
output_dir="$(realpath -m "${2:?output directory is required}")"

mkdir -p "${output_dir}"

if [[ ! -d "${build_dir}" ]]; then
    exit 0
fi

find "${build_dir}" -type f -name '*.json' -print0 \
    | LC_ALL=C sort -z \
    | while IFS= read -r -d '' trace_path; do
        object_stem="${trace_path%.json}"
        if [[ ! -f "${object_stem}.o" && ! -f "${object_stem}.obj" ]]; then
            continue
        fi

        relative_path="${trace_path#${build_dir}/}"
        install -D -m 0644 "${trace_path}" "${output_dir}/${relative_path}"
    done
