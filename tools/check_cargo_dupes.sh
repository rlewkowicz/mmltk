#!/usr/bin/env bash
set -euo pipefail

project_dir="${1:-.}"
cd "${project_dir}"

# The browser action/runtime contract is installed from the native reflected
# bootstrap, so every Rust source is handwritten presentation/runtime logic
# and participates in the duplication audit.
readonly dupes_args=(--format json --min-lines 5)
dupes_stats="$(cargo dupes "${dupes_args[@]}" stats)"
readonly dupes_stats
printf '%s\n' "${dupes_stats}"

if ! DUPES_STATS="${dupes_stats}" python3 -c '
import json
import os

stats = json.loads(os.environ["DUPES_STATS"])
raise SystemExit(
    int(stats["exact_duplicate_groups"] != 0 or stats["near_duplicate_groups"] != 0)
)
'; then
    cargo dupes "${dupes_args[@]}" report
    exit 1
fi
