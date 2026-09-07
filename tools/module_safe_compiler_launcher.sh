#!/usr/bin/env bash
set -euo pipefail

if (($# == 0)); then
    printf 'mmltk compiler launcher: missing compiler command\n' >&2
    exit 2
fi

compiler="$1"
shift

for argument in "$@"; do
    case "${argument}" in
        -fmodules|-fmodules=*|-fmodules-ts|-fmodule-mapper=*)
            exec "${compiler}" "$@"
            ;;
    esac
done

exec ccache "${compiler}" "$@"
