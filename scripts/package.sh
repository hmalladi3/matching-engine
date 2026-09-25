#!/usr/bin/env bash
# Builds the submission zip from the committed tree: all source, project,
# test, dataset and documentation files; no build output, nothing untracked.
#
# Usage: scripts/package.sh            -> dist/order-matcher.zip
# @spec DLV-BUILD-005
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ -n "$(git status --porcelain)" ]]; then
    echo "package: working tree has uncommitted changes; commit them first" >&2
    git status --short >&2
    exit 1
fi
if git ls-files | grep -qiE '\.(pdf|docx?)$'; then
    echo "package: refusing to include document files (the assignment brief must not be shipped)" >&2
    exit 1
fi

mkdir -p dist
git archive --format=zip --prefix=order-matcher/ -o dist/order-matcher.zip HEAD
echo "wrote dist/order-matcher.zip ($(git ls-files | wc -l | tr -d ' ') files, commit $(git rev-parse --short HEAD))"
