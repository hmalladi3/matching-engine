#!/usr/bin/env bash
# Traceability between the EARS specs (docs/specs) and the code:
#   - every behavioral spec is cited by at least one @spec annotation in tests/
#   - every deliverable spec (DLV-*) is cited by the artifact that satisfies it
#   - every cited ID exists in docs/specs
# @spec DLV-TEST-008
set -euo pipefail
cd "$(dirname "$0")/.."

ID='[A-Z]+(-[A-Z]+)+-[0-9]{3}'

defined="$(grep -hoE "\*\*${ID}\*\*" docs/specs/*.md | tr -d '*' | sort -u)"

# IDs in @spec annotations (including continuation lines of multi-line
# annotations) in files git would ship: tracked or untracked-but-not-ignored.
cited_in() {
    LC_ALL=C awk '
        /@spec/ { on = 1 }
        on && !/^[[:space:]]*(\/\/|#)/ && !/@spec/ { on = 0 }
        on { print }
        /@spec/ && !/,[[:space:]]*$/ { on = 0 }
    ' $(shipped_files "$@" | grep -v '^docs/specs/') |
        grep -oE "$ID" | sort -u
}

# Files that ship: tracked or untracked-but-not-ignored when this is a git
# checkout; otherwise (Docker image, unpacked zip) everything outside build output.
shipped_files() {
    if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        git ls-files --cached --others --exclude-standard -- "$@"
    else
        find "$@" -type f ! -path '*/build/*' ! -path '*/dist/*' ! -path '*/.cache/*' |
            sed 's|^\./||'
    fi
}

tests_cite="$(cited_in tests)"
# Deliverables may be satisfied anywhere in the project's own files.
anywhere_cite="$(cited_in app bench data docs fuzz include scripts src tests tools \
    CMakeLists.txt Dockerfile README.md PERFORMANCE.md)"

status=0
missing_tests="$(comm -23 <(echo "$defined" | grep -v '^DLV-') <(echo "$tests_cite"))"
missing_artifacts="$(comm -23 <(echo "$defined" | grep '^DLV-') <(echo "$anywhere_cite"))"
unknown="$(comm -13 <(echo "$defined") <(echo "$anywhere_cite"))"

if [[ -n "$missing_tests" ]]; then
    echo "Behavioral specs with no test citing them:"; echo "$missing_tests" | sed 's/^/  /'; status=1
fi
if [[ -n "$missing_artifacts" ]]; then
    echo "Deliverable specs with no artifact citing them:"; echo "$missing_artifacts" | sed 's/^/  /'; status=1
fi
if [[ -n "$unknown" ]]; then
    echo "Cited IDs that do not exist in docs/specs:"; echo "$unknown" | sed 's/^/  /'; status=1
fi

total="$(echo "$defined" | wc -l | tr -d ' ')"
[[ $status == 0 ]] && echo "spec traceability: all $total specs cited; no unknown IDs"
exit $status
