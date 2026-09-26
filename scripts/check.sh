#!/usr/bin/env bash
# Runs every quality gate and exits non-zero on the first failure:
#   1. build matrix {GCC, Clang} x {Debug, Release, ASan+UBSan}, each with its full test suite
#   2. libFuzzer smoke runs (Clang)
#   3. coverage thresholds            (scripts/coverage.sh)
#   4. clang-tidy and clang-format
#   5. spec traceability              (scripts/spec_coverage.sh)
#   6. a short benchmark
#
# Usage: scripts/check.sh [--quick] [--stress]
#   --quick   shorter fuzzing; skips the Debug builds
#   --stress  also runs scripts/stress.sh (S1-S5; several minutes, a few GB of disk)
# Missing tools are skipped with a warning locally; set CHECK_STRICT=1 to make them fatal
# (the Docker image has everything).
# @spec DLV-TEST-005, DLV-TEST-009, DLV-TEST-010, DLV-BUILD-002
set -euo pipefail
cd "$(dirname "$0")/.."

QUICK=0
STRESS=0
for arg in "$@"; do
    case "$arg" in
        --quick) QUICK=1 ;;
        --stress) STRESS=1 ;;
        *) echo "usage: scripts/check.sh [--quick] [--stress]" >&2; exit 2 ;;
    esac
done
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
FUZZ_SECONDS="${FUZZ_SECONDS:-$([[ $QUICK == 1 ]] && echo 15 || echo 60)}"
GENERATOR=()
command -v ninja >/dev/null && GENERATOR=(-G Ninja)

step() { printf '\n\033[1m==== %s ====\033[0m\n' "$*"; }
skip() {
    printf 'SKIPPED: %s\n' "$*"
    [[ "${CHECK_STRICT:-0}" == 1 ]] && { echo "CHECK_STRICT=1: missing tools are fatal"; exit 1; }
    return 0
}
find_tool() {  # first available of the given names; empty (not an error) if none
    for t in "$@"; do command -v "$t" >/dev/null && { echo "$t"; return 0; }; done
    return 0
}

GXX="$(find_tool g++-13 g++-14 g++-12 g++)"
# On macOS, "g++" is Apple Clang; only use it if it is really GCC.
if [[ -n "$GXX" ]] && ! "$GXX" --version | grep -qi 'gcc\|free software'; then GXX=""; fi
CLANGXX="$(find_tool clang++-18 clang++)"

build_and_test() {  # name compiler build-type [extra cmake args...]
    local name="$1" cxx="$2" type="$3"; shift 3
    step "build + test: $name"
    cmake -S . -B "build/check/$name" ${GENERATOR[@]+"${GENERATOR[@]}"} -DCMAKE_BUILD_TYPE="$type" \
        -DCMAKE_CXX_COMPILER="$cxx" "$@" >/dev/null
    cmake --build "build/check/$name" -j "$JOBS"
    ctest --test-dir "build/check/$name" -j "$JOBS" --output-on-failure --timeout 3000
}

# ---- 1. build matrix -------------------------------------------------------------------
for compiler in gcc clang; do
    cxx="$([[ $compiler == gcc ]] && echo "$GXX" || echo "$CLANGXX")"
    if [[ -z "$cxx" ]]; then skip "$compiler not found"; continue; fi
    [[ $QUICK == 0 ]] && build_and_test "$compiler-debug" "$cxx" Debug
    build_and_test "$compiler-release" "$cxx" Release
    build_and_test "$compiler-asan" "$cxx" Debug -DMATCHER_SANITIZE=ON -DMATCHER_BUILD_BENCH=OFF
done

# ---- 2. fuzzing ------------------------------------------------------------------------
step "fuzz smoke runs (${FUZZ_SECONDS}s per target)"
if [[ -n "$CLANGXX" ]] && echo 'extern "C" int LLVMFuzzerTestOneInput(const char*, unsigned long){return 0;}' |
    "$CLANGXX" -x c++ -fsanitize=fuzzer - -o /dev/null 2>/dev/null; then
    cmake -S . -B build/check/fuzz ${GENERATOR[@]+"${GENERATOR[@]}"} -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_CXX_COMPILER="$CLANGXX" -DMATCHER_BUILD_FUZZ=ON -DMATCHER_BUILD_TESTS=OFF \
        -DMATCHER_BUILD_BENCH=OFF >/dev/null
    cmake --build build/check/fuzz -j "$JOBS"
    for target in fuzz_pipeline fuzz_price; do
        corpus="build/check/fuzz/corpus-$target"
        mkdir -p "$corpus"
        cp data/golden/*.in fuzz/corpus/* "$corpus"/ 2>/dev/null || true
        "build/check/fuzz/fuzz/$target" -max_total_time="$FUZZ_SECONDS" -print_final_stats=1 \
            -artifact_prefix="build/check/fuzz/$target-" "$corpus" 2>&1 | tail -4
    done
else
    skip "clang with libFuzzer not available (Apple Clang lacks it; use Docker)"
fi

# ---- 3. coverage -----------------------------------------------------------------------
step "coverage"
# (Explicit status handling: a failure on the left of || would not trigger set -e.)
coverage_status=0
scripts/coverage.sh || coverage_status=$?
if [[ $coverage_status == 3 ]]; then
    skip "coverage tools not available"
elif [[ $coverage_status != 0 ]]; then
    echo "coverage check failed"; exit 1
fi

# ---- 4. static checks ------------------------------------------------------------------
step "clang-tidy"
TIDY="$(find_tool clang-tidy-18 clang-tidy)"
if [[ -n "$TIDY" && -n "$CLANGXX" ]]; then
    cmake -S . -B build/check/tidy ${GENERATOR[@]+"${GENERATOR[@]}"} -DCMAKE_CXX_COMPILER="$CLANGXX" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
    "$TIDY" -p build/check/tidy --quiet src/*.cpp app/*.cpp
    echo "clang-tidy: clean"
else
    skip "clang-tidy not found"
fi

step "clang-format"
FORMAT="$(find_tool clang-format-18 clang-format)"
if [[ -n "$FORMAT" ]]; then
    "$FORMAT" --dry-run --Werror include/matcher/*.h src/*.cpp app/*.cpp tests/*/*.cpp tests/*/*.h \
        bench/*.cpp bench/*.h fuzz/*.cpp tools/*.cpp
    echo "clang-format: clean"
else
    skip "clang-format not found"
fi

# ---- 5. traceability -------------------------------------------------------------------
step "spec traceability"
scripts/spec_coverage.sh

# ---- 6. benchmark ----------------------------------------------------------------------
step "benchmark (quick)"
bench="build/check/clang-release/bench/matcher_bench"
[[ -x "$bench" ]] || bench="build/check/gcc-release/bench/matcher_bench"
"$bench" --quick

# ---- 7. stress (optional) --------------------------------------------------------------
if [[ $STRESS == 1 ]]; then
    step "stress suite"
    scripts/stress.sh
fi

step "ALL CHECKS PASSED"
