#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${REPO_ROOT}/build/dev}"
COMPILE_DB="${BUILD_DIR}/compile_commands.json"
CLANG_TIDY_BIN="${FASTLOADER_STATIC_CLANG_TIDY_BIN:-$(command -v clang-tidy || true)}"
CPPCHECK_BIN="${FASTLOADER_STATIC_CPPCHECK_BIN:-$(command -v cppcheck || true)}"
VALGRIND_BIN="${FASTLOADER_STATIC_VALGRIND_BIN:-$(command -v valgrind || true)}"
VULTURE_BIN="${FASTLOADER_STATIC_VULTURE_BIN:-$(command -v vulture || true)}"
SUPPRESSIONS_FILE="${FASTLOADER_STATIC_CPPCHECK_SUPPRESSIONS:-${REPO_ROOT}/utilities/cppcheck.suppressions}"
ENABLE_ANALYZER_ALPHA="${FASTLOADER_STATIC_ENABLE_ANALYZER_ALPHA:-0}"
ENABLE_CHECK_PROFILE="${FASTLOADER_STATIC_ENABLE_CHECK_PROFILE:-0}"
INCLUDE_TESTS="${FASTLOADER_STATIC_INCLUDE_TESTS:-1}"
CLANG_TIDY_JOBS="${FASTLOADER_STATIC_CLANG_TIDY_JOBS:-1}"
CPPCHECK_JOBS="${FASTLOADER_STATIC_CPPCHECK_JOBS:-$(nproc)}"
CPPCHECK_BUILD_DIR="${FASTLOADER_STATIC_CPPCHECK_BUILD_DIR:-${BUILD_DIR}/cppcheck}"

if [[ "${BUILD_DIR}" != /* ]]; then
    BUILD_DIR="${REPO_ROOT}/${BUILD_DIR#./}"
    COMPILE_DB="${BUILD_DIR}/compile_commands.json"
fi

cd "${REPO_ROOT}"

require_tool() {
    local tool_name="$1"
    local tool_path="$2"
    if [[ -z "${tool_path}" ]]; then
        echo "[static] Missing required tool: ${tool_name}" >&2
        exit 1
    fi
}

if [[ ! -f "${COMPILE_DB}" ]]; then
    echo "[static] Missing compilation database: ${COMPILE_DB}" >&2
    exit 1
fi

require_tool "clang-tidy" "${CLANG_TIDY_BIN}"
require_tool "cppcheck" "${CPPCHECK_BIN}"
require_tool "valgrind" "${VALGRIND_BIN}"

if [[ ! -f "${SUPPRESSIONS_FILE}" ]]; then
    echo "[static] Missing cppcheck suppressions file: ${SUPPRESSIONS_FILE}" >&2
    exit 1
fi

mapfile -t translation_units < <(
    sed -n 's/^[[:space:]]*"file":[[:space:]]*"\(.*\)"[[:space:]]*,\{0,1\}[[:space:]]*$/\1/p' "${COMPILE_DB}" | sort -u
)

analysis_roots='src'
if [[ "${INCLUDE_TESTS}" != "0" ]]; then
    analysis_roots='src|tests'
fi

mapfile -t analysis_units < <(
    printf '%s\n' "${translation_units[@]}" | grep -E "^${REPO_ROOT}/(${analysis_roots})/.*\\.cpp$" || true
)

if [[ ${#analysis_units[@]} -eq 0 ]]; then
    echo "[static] No translation units found in ${COMPILE_DB}" >&2
    exit 1
fi

CLANG_TIDY_CHECKS='-*,clang-analyzer-*,bugprone-*,performance-*,misc-use-internal-linkage,-bugprone-easily-swappable-parameters,-bugprone-exception-escape'
if [[ "${ENABLE_ANALYZER_ALPHA}" != "0" ]]; then
    CLANG_TIDY_CHECKS+=",clang-analyzer-alpha.clone.*,clang-analyzer-alpha.core.*,clang-analyzer-alpha.cplusplus.*,clang-analyzer-alpha.deadcode.*,clang-analyzer-alpha.security.*,clang-analyzer-alpha.unix.*,-clang-analyzer-alpha.webkit.*"
fi
readonly CLANG_TIDY_WARNINGS_AS_ERRORS='clang-analyzer-*,bugprone-*,performance-*'
readonly HEADER_FILTER='(include|src|tests)/'
readonly TEST_ROUNDTRIP="${BUILD_DIR}/test_roundtrip"
readonly CLANG_TIDY_PROFILE_PREFIX="${BUILD_DIR}/clang-tidy-profile"

clang_tidy_common_args=(
    -p "${BUILD_DIR}"
    --quiet
    --checks="${CLANG_TIDY_CHECKS}"
    --warnings-as-errors="${CLANG_TIDY_WARNINGS_AS_ERRORS}"
    --header-filter="${HEADER_FILTER}"
)

if [[ "${ENABLE_ANALYZER_ALPHA}" != "0" ]]; then
    clang_tidy_common_args+=(-allow-enabling-analyzer-alpha-checkers)
fi

if [[ "${ENABLE_CHECK_PROFILE}" != "0" ]]; then
    mkdir -p "${BUILD_DIR}"
    clang_tidy_common_args+=(--enable-check-profile "--store-check-profile=${CLANG_TIDY_PROFILE_PREFIX}")
fi

echo "[static][1/4] Running clang-tidy on ${#analysis_units[@]} translation units"
for index in "${!analysis_units[@]}"; do
    unit="${analysis_units[index]}"
    printf '[static][clang-tidy] %d/%d %s\n' \
        "$((index + 1))" \
        "${#analysis_units[@]}" \
        "${unit#${REPO_ROOT}/}"
    "${CLANG_TIDY_BIN}" "${clang_tidy_common_args[@]}" "${unit}"
done

echo "[static][2/4] Running cppcheck"
mkdir -p "${CPPCHECK_BUILD_DIR}"
"${CPPCHECK_BIN}" \
    --project="${COMPILE_DB}" \
    --cppcheck-build-dir="${CPPCHECK_BUILD_DIR}" \
    --enable=all \
    --inconclusive \
    --check-level=exhaustive \
    --library=posix \
    --platform=unix64 \
    -j "${CPPCHECK_JOBS}" \
    --error-exitcode=2 \
    --inline-suppr \
    --suppressions-list="${SUPPRESSIONS_FILE}" \
    -ithird_party

if [[ ! -x "${TEST_ROUNDTRIP}" ]]; then
    echo "[static] Missing roundtrip test binary: ${TEST_ROUNDTRIP}" >&2
    exit 1
fi

echo "[static][3/4] Running valgrind on ${TEST_ROUNDTRIP}"
"${VALGRIND_BIN}" \
    --tool=memcheck \
    --quiet \
    --leak-check=full \
    --show-leak-kinds=definite,indirect \
    --errors-for-leak-kinds=definite,indirect \
    --track-origins=yes \
    --error-exitcode=3 \
    "${TEST_ROUNDTRIP}"

python_roots=(
    tests
    utilities
)

mapfile -t python_units < <(
    find "${python_roots[@]}" \
        -type f \
        -name '*.py' \
        -not -path '*/__pycache__/*' \
        2>/dev/null | sort
)

if [[ ${#python_units[@]} -gt 0 ]]; then
    require_tool "vulture" "${VULTURE_BIN}"
    echo "[static][4/4] Running vulture on ${#python_units[@]} Python files"
    "${VULTURE_BIN}" \
        --min-confidence 100 \
        --exclude "build,third_party,__pycache__,rf-detr,scipy,pytorch" \
        "${python_units[@]}"
else
    echo "[static][4/4] No Python files to scan with vulture"
fi

echo "[static] Static analysis passed"
