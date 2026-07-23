#!/usr/bin/env bash
set -euo pipefail

project_dir="${1:-.}"
cd "${project_dir}"

dupes_stats="$(cargo dupes --format json --min-lines 5 stats)"
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
    cargo dupes --format json --min-lines 5 report
    exit 1
fi
