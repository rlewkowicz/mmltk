#!/usr/bin/env bash
# set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
readonly RUNS=1

readonly PROMPT="$(cat <<'EOF'
Execute actionplan.md. If all phases are complete, exit.
EOF
)"

for ((run = 1; run <= RUNS; ++run)); do
    printf 'codex actionplan run %d/%d\n' "${run}" "${RUNS}" >&2
    (codex --sandbox danger-full-access --ask-for-approval never  exec --cd "${REPO_ROOT}" "${PROMPT}")
    printf 'codex actionplan run %d of %d done\n' "${run}" "${RUNS}" >&2
done

