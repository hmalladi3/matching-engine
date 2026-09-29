#!/usr/bin/env bash
# Stress suite: the real binaries under large, sustained and hostile load.
#
#   S1 volume          10^7 requests x 3 profiles; output identical across GCC, Clang and a rerun
#   S2 huge book       5x10^6 orders over 10^5 levels per side; cancel half; sweep both sides
#   S3 soak            3x10^7 streamed requests; memory must stay flat
#   S4 pathological    1 GiB line; 10^7 blank/comment, garbage, and unknown-cancel lines
#   S5 slow consumer   stdout through a throttled reader must match an unthrottled run
#   S6 long fuzz       (--long only) each libFuzzer target for FUZZ_SECONDS (default 1800)
#
# Usage: scripts/stress.sh [--long] [--only S1,S4]
# Sizes can be reduced for small machines with STRESS_S1_COUNT, STRESS_S2_ORDERS,
# STRESS_S2_LEVELS, STRESS_S3_COUNT, STRESS_S4_LINES, STRESS_S5_COUNT.
set -euo pipefail
cd "$(dirname "$0")/.."

LONG=0
ONLY=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --long) LONG=1 ;;
        --only) ONLY="$2"; shift ;;
        *) echo "usage: scripts/stress.sh [--long] [--only S1,S2,...]" >&2; exit 2 ;;
    esac
    shift
done

S1_COUNT="${STRESS_S1_COUNT:-10000000}"
S2_ORDERS="${STRESS_S2_ORDERS:-5000000}"
S2_LEVELS="${STRESS_S2_LEVELS:-100000}"
S3_COUNT="${STRESS_S3_COUNT:-30000000}"
S4_LINES="${STRESS_S4_LINES:-10000000}"
S5_COUNT="${STRESS_S5_COUNT:-1000000}"
FUZZ_SECONDS="${FUZZ_SECONDS:-1800}"

JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
GENERATOR=()
command -v ninja >/dev/null && GENERATOR=(-G Ninja)
find_tool() {  # first available of the given names; empty (not an error) if none
    for t in "$@"; do command -v "$t" >/dev/null && { echo "$t"; return 0; }; done
    return 0
}
hash_of() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
rss_of_pid() {  # current RSS in KiB
    if [[ -r "/proc/$1/status" ]]; then awk '/^VmRSS/ {print $2}' "/proc/$1/status"; else ps -o rss= -p "$1" | tr -d ' '; fi
}
wanted() { [[ -z "$ONLY" || ",$ONLY," == *",$1,"* ]]; }

SUMMARY=()
FAILED=0
record() {  # test result detail
    SUMMARY+=("| $1 | $2 | $3 |")
    if [[ "$2" == FAIL ]]; then FAILED=1; fi
    printf '%-4s %s  %s\n' "$2" "$1" "$3"
}
check() {  # test condition-description condition...
    local name="$1" what="$2"; shift 2
    if "$@"; then return 0; fi
    record "$name" FAIL "$what"
    return 1
}

# ---- builds -------------------------------------------------------------------------
GXX="$(find_tool g++-13 g++-14 g++-12 g++)"
if [[ -n "$GXX" ]] && ! "$GXX" --version | grep -qi 'gcc\|free software'; then GXX=""; fi
CLANGXX="$(find_tool clang++-18 clang++)"
BUILDS=()
for compiler in clang gcc; do
    cxx="$([[ $compiler == gcc ]] && echo "$GXX" || echo "$CLANGXX")"
    [[ -z "$cxx" ]] && continue
    dir="build/stress/$compiler"
    echo "building $compiler release binaries in $dir"
    cmake -S . -B "$dir" ${GENERATOR[@]+"${GENERATOR[@]}"} -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$cxx" \
        -DMATCHER_BUILD_BENCH=OFF >/dev/null
    cmake --build "$dir" -j "$JOBS" --target matcher gen_orders slow_reader measure >/dev/null
    BUILDS+=("$dir")
done
[[ ${#BUILDS[@]} -gt 0 ]] || { echo "no compiler found"; exit 1; }
PRIMARY="${BUILDS[0]}"
MATCHER="$PRIMARY/matcher"
GEN="$PRIMARY/gen_orders"
MEASURE="$PRIMARY/measure"
SLOW="$PRIMARY/slow_reader"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/matcher-stress.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
STATS="$TMP/stats"
secs() { cut -d' ' -f1 "$STATS"; }
rss_mib() { awk '{printf "%.0f", $2 / 1024}' "$STATS"; }
per_sec() { awk -v n="$1" -v s="$(secs)" 'BEGIN { printf "%.2fM msg/s", n / s / 1e6 }'; }

# ---- S1 volume ------------------------------------------------------------------------
if wanted S1; then
    for profile in tight mixed sweep; do
        name="S1 volume ($profile, $S1_COUNT)"
        "$GEN" --profile "$profile" --count "$S1_COUNT" --seed 7 > "$TMP/in.csv"
        hashes=()
        detail=""
        ok=1
        runs=("${BUILDS[@]}" "$PRIMARY")  # every compiler, then a repeat of the first
        for dir in "${runs[@]}"; do
            if ! "$MEASURE" "$STATS" "$dir/matcher" < "$TMP/in.csv" > "$TMP/out" 2> "$TMP/err"; then
                record "$name" FAIL "$dir exited non-zero"; ok=0; break
            fi
            hashes+=("$(hash_of "$TMP/out"):$(hash_of "$TMP/err")")
            [[ -z "$detail" ]] && detail="$(per_sec "$S1_COUNT"), peak $(rss_mib) MiB, $(grep -c '^2,' "$TMP/out") trades"
        done
        if [[ $ok == 1 ]]; then
            if [[ "$(printf '%s\n' "${hashes[@]}" | sort -u | wc -l | tr -d ' ')" == 1 ]]; then
                record "$name" PASS "$detail; identical output across ${#runs[@]} runs (${BUILDS[*]##*/} + repeat)"
            else
                record "$name" FAIL "outputs differ between runs"
            fi
        fi
    done
    rm -f "$TMP/in.csv" "$TMP/out" "$TMP/err"
fi

# ---- S2 huge book ---------------------------------------------------------------------
if wanted S2; then
    name="S2 huge book ($S2_ORDERS orders, $S2_LEVELS levels/side)"
    "$GEN" --scenario huge_book --orders "$S2_ORDERS" --levels "$S2_LEVELS" > "$TMP/in.csv" 2> "$TMP/expected"
    expected="$(sed -n 's/^expected_trades=//p' "$TMP/expected")"
    if "$MEASURE" "$STATS" "$MATCHER" < "$TMP/in.csv" > "$TMP/out" 2> "$TMP/err"; then
        trades="$(grep -c '^2,' "$TMP/out" || true)"
        check "$name" "expected $expected trades, got $trades" [ "$trades" == "$expected" ] &&
        check "$name" "unexpected diagnostics: $(head -c 200 "$TMP/err")" [ ! -s "$TMP/err" ] &&
        check "$name" "peak RSS $(rss_mib) MiB >= 1536 MiB" [ "$(rss_mib)" -lt 1536 ] &&
        record "$name" PASS "$trades trades as expected, book empty, $(secs)s, peak $(rss_mib) MiB"
    else
        record "$name" FAIL "exited non-zero"
    fi
    rm -f "$TMP/in.csv" "$TMP/out" "$TMP/err"
fi

# ---- S3 soak --------------------------------------------------------------------------
if wanted S3; then
    name="S3 soak ($S3_COUNT cancel_heavy requests)"
    samples=()
    start=$(date +%s)
    "$GEN" --profile cancel_heavy --count "$S3_COUNT" --seed 11 | "$MATCHER" > /dev/null 2>&1 &
    pid=$!
    while kill -0 "$pid" 2>/dev/null; do
        rss="$(rss_of_pid "$pid" 2>/dev/null || true)"
        [[ -n "$rss" ]] && samples+=("$rss")
        sleep 0.05
    done
    status=0
    wait "$pid" || status=$?
    elapsed=$(( $(date +%s) - start ))
    n=${#samples[@]}
    if [[ $status != 0 ]]; then
        record "$name" FAIL "exited with status $status"
    elif (( n < 8 )); then
        record "$name" FAIL "run too short to judge memory ($n samples); raise STRESS_S3_COUNT"
    else
        early=${samples[$((n / 4))]}
        late=${samples[$((n - 1))]}
        if awk -v e="$early" -v l="$late" 'BEGIN { exit !(l <= e * 1.10) }'; then
            record "$name" PASS "RSS $((early / 1024)) MiB at 25% -> $((late / 1024)) MiB at end ($n samples, ~${elapsed}s)"
        else
            record "$name" FAIL "RSS grew from $((early / 1024)) MiB to $((late / 1024)) MiB"
        fi
    fi
fi

# ---- S4 pathological input --------------------------------------------------------------
if wanted S4; then
    trade=$'0,1,1,1,10\n0,2,0,1,10\n'
    trade_out=$'2,1,10\n3,2\n3,1\n'
    # Inputs come through process substitution, so only the matcher's exit status is
    # judged (with pipefail, `yes | head` would report yes's SIGPIPE as a failure).
    run_small() { "$MEASURE" "$STATS" "$MATCHER" --reserve 1024 > "$TMP/out" 2> "$TMP/err"; }

    name="S4a one 1 GiB line"
    if run_small < <(set +eo pipefail; head -c 1073741824 /dev/zero | tr '\0' 'x'; printf '\n%s' "$trade"); then
        check "$name" "expected exactly one diagnostic" [ "$(wc -l < "$TMP/err" | tr -d ' ')" == 1 ] &&
        check "$name" "wrong diagnostic: $(head -c 80 "$TMP/err")" grep -q '^line 1: line exceeds 4096 bytes: x' "$TMP/err" &&
        check "$name" "trailing trade not printed" [ "$(cat "$TMP/out")" == "${trade_out%$'\n'}" ] &&
        check "$name" "peak RSS $(rss_mib) MiB >= 64 MiB" [ "$(rss_mib)" -lt 64 ] &&
        record "$name" PASS "1 diagnostic, trade intact, peak $(rss_mib) MiB, $(secs)s"
    else
        record "$name" FAIL "exited non-zero"
    fi

    name="S4b $S4_LINES blank and comment lines"
    half=$((S4_LINES / 2))
    if run_small < <(set +eo pipefail; yes '' | head -n "$half"; yes '// just a comment' | head -n "$half"; printf '%s' "$trade"); then
        check "$name" "expected no diagnostics" [ ! -s "$TMP/err" ] &&
        check "$name" "trailing trade not printed" [ "$(cat "$TMP/out")" == "${trade_out%$'\n'}" ] &&
        record "$name" PASS "no diagnostics, trade intact, peak $(rss_mib) MiB, $(secs)s"
    else
        record "$name" FAIL "exited non-zero"
    fi

    name="S4c $S4_LINES garbage lines"
    if run_small < <(set +eo pipefail; yes 'garbage,not,a,message' | head -n "$S4_LINES"); then
        check "$name" "expected $S4_LINES diagnostics" [ "$(wc -l < "$TMP/err" | tr -d ' ')" == "$S4_LINES" ] &&
        check "$name" "unexpected stdout" [ ! -s "$TMP/out" ] &&
        record "$name" PASS "$S4_LINES diagnostics, peak $(rss_mib) MiB, $(secs)s"
    else
        record "$name" FAIL "exited non-zero"
    fi

    name="S4d $S4_LINES unknown cancels"
    if run_small < <(set +eo pipefail; seq 1 "$S4_LINES" | sed 's/^/1,/'); then
        check "$name" "expected $S4_LINES diagnostics" [ "$(wc -l < "$TMP/err" | tr -d ' ')" == "$S4_LINES" ] &&
        check "$name" "wrong diagnostic" grep -q "^line $S4_LINES: cannot cancel orderid $S4_LINES: " "$TMP/err" &&
        record "$name" PASS "$S4_LINES diagnostics, peak $(rss_mib) MiB, $(secs)s"
    else
        record "$name" FAIL "exited non-zero"
    fi
    rm -f "$TMP/out" "$TMP/err"
fi

# ---- S5 slow consumer -----------------------------------------------------------------
if wanted S5; then
    name="S5 slow consumer ($S5_COUNT requests)"
    "$GEN" --profile tight --count "$S5_COUNT" --seed 5 > "$TMP/in.csv"
    "$MATCHER" < "$TMP/in.csv" > "$TMP/fast" 2> /dev/null
    set +o pipefail
    "$MATCHER" < "$TMP/in.csv" 2> /dev/null | "$SLOW" 4096 8 2000 > "$TMP/slow"
    status=("${PIPESTATUS[@]}")
    set -o pipefail
    if [[ "${status[0]}" != 0 || "${status[1]}" != 0 ]]; then
        record "$name" FAIL "exit statuses ${status[*]}"
    elif cmp -s "$TMP/fast" "$TMP/slow"; then
        record "$name" PASS "$(wc -c < "$TMP/slow" | tr -d ' ') bytes identical to unthrottled run"
    else
        record "$name" FAIL "throttled output differs"
    fi
    rm -f "$TMP/in.csv" "$TMP/fast" "$TMP/slow"
fi

# ---- S6 long fuzz ---------------------------------------------------------------------
if [[ $LONG == 1 ]] && wanted S6; then
    if [[ -n "$CLANGXX" ]] && echo 'extern "C" int LLVMFuzzerTestOneInput(const char*, unsigned long){return 0;}' |
        "$CLANGXX" -x c++ -fsanitize=fuzzer - -o /dev/null 2>/dev/null; then
        cmake -S . -B build/stress/fuzz ${GENERATOR[@]+"${GENERATOR[@]}"} -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_CXX_COMPILER="$CLANGXX" -DMATCHER_BUILD_FUZZ=ON -DMATCHER_BUILD_TESTS=OFF \
            -DMATCHER_BUILD_BENCH=OFF >/dev/null
        cmake --build build/stress/fuzz -j "$JOBS" >/dev/null
        for target in fuzz_pipeline fuzz_price; do
            corpus="build/stress/fuzz/corpus-$target"
            mkdir -p "$corpus"
            cp data/golden/*.in "$corpus"/ 2>/dev/null || true
            if "build/stress/fuzz/fuzz/$target" -max_total_time="$FUZZ_SECONDS" -print_final_stats=1 \
                -artifact_prefix="build/stress/fuzz/$target-" "$corpus" > "$TMP/fuzz.log" 2>&1; then
                runs="$(sed -n 's/^stat::number_of_executed_units: *//p' "$TMP/fuzz.log")"
                record "S6 fuzz $target (${FUZZ_SECONDS}s)" PASS "$runs inputs, no findings"
            else
                record "S6 fuzz $target (${FUZZ_SECONDS}s)" FAIL "see build/stress/fuzz/$target-*"
            fi
        done
    else
        record "S6 long fuzz" SKIP "clang with libFuzzer not available (use Docker)"
    fi
fi

# ---- summary --------------------------------------------------------------------------
{
    echo "### Stress results"
    echo
    echo "- Binary: $MATCHER ($(${CLANGXX:-$GXX} --version | head -1))"
    echo "- Host: $(uname -srm), $JOBS CPUs"
    echo
    echo "| Test | Result | Detail |"
    echo "|---|---|---|"
    printf '%s\n' "${SUMMARY[@]}"
} | tee build/stress/summary.md
echo
[[ $FAILED == 0 ]] && echo "STRESS: ALL PASSED" || { echo "STRESS: FAILURES"; exit 1; }
