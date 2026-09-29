#!/usr/bin/env bash
# Source-based coverage (Clang) of the shipped library over the whole test
# suite, including the golden end-to-end runs of the real binary. Fails if
# line or branch coverage is below the design's thresholds, or if any branch
# of the request parser is unexercised.
#
# Usage: scripts/coverage.sh        (report in build/coverage/report/index.html)
# Exit codes: 0 ok, 1 below threshold, 3 tools unavailable.
set -euo pipefail
cd "$(dirname "$0")/.."

MIN_LINES=95
MIN_BRANCHES=90

find_tool() {  # first available of the given names; empty (not an error) if none
    for t in "$@"; do command -v "$t" >/dev/null && { echo "$t"; return 0; }; done
    return 0
}
CLANGXX="$(find_tool clang++-18 clang++)"
PROFDATA="$(find_tool llvm-profdata-18 llvm-profdata)"
COV="$(find_tool llvm-cov-18 llvm-cov)"
if [[ -z "$PROFDATA" && "$(uname)" == Darwin ]]; then PROFDATA="xcrun llvm-profdata"; COV="xcrun llvm-cov"; fi
if [[ -z "$CLANGXX" || -z "$PROFDATA" || -z "$COV" ]]; then
    echo "coverage: clang++, llvm-profdata and llvm-cov are required"
    exit 3
fi

JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
BUILD=build/coverage
rm -rf "$BUILD/profiles"
# NDEBUG: an assert()'s failing side is unreachable by construction, so it
# would only ever count as a "missed" branch.
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=-DNDEBUG -DCMAKE_CXX_COMPILER="$CLANGXX" \
    -DMATCHER_COVERAGE=ON -DMATCHER_BUILD_BENCH=OFF -DMATCHER_DIFF_REQUESTS=20000 >/dev/null
cmake --build "$BUILD" -j "$JOBS"
LLVM_PROFILE_FILE="$PWD/$BUILD/profiles/%p.profraw" ctest --test-dir "$BUILD" -j "$JOBS" --timeout 3000 >/dev/null

$PROFDATA merge -sparse -o "$BUILD/merged.profdata" "$BUILD"/profiles/*.profraw
# The first binary is positional; the rest are passed with -object.
OBJECTS=("$BUILD/tests/matcher_tests" -object "$BUILD/matcher" -object "$BUILD/tests/no_alloc_test")
SOURCES=(src include/matcher)

$COV report -instr-profile="$BUILD/merged.profdata" "${OBJECTS[@]}" "${SOURCES[@]}" | tee "$BUILD/summary.txt"
$COV show -format=html -output-dir="$BUILD/report" -instr-profile="$BUILD/merged.profdata" \
    "${OBJECTS[@]}" "${SOURCES[@]}" >/dev/null

# TOTAL row: ... Lines Missed Cover Branches Missed Cover
read -r lines branches < <(awk '/^TOTAL/ {gsub("%", ""); print $10, $13}' "$BUILD/summary.txt")
parser_branches="$(awk '/request_parser.cpp/ {gsub("%", ""); print $13}' "$BUILD/summary.txt")"
echo
echo "library line coverage:    ${lines}% (minimum ${MIN_LINES}%)"
echo "library branch coverage:  ${branches}% (minimum ${MIN_BRANCHES}%)"
echo "request parser branches:  ${parser_branches}% (required 100%)"

awk -v l="$lines" -v b="$branches" -v p="$parser_branches" -v ml="$MIN_LINES" -v mb="$MIN_BRANCHES" \
    'BEGIN { exit !(l >= ml && b >= mb && p >= 100) }' || { echo "coverage below threshold"; exit 1; }
echo "coverage: OK"
