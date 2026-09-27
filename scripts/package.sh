#!/usr/bin/env bash
# Builds the submission zip from the committed tree: source, project, test,
# dataset, README and PERFORMANCE files; no build output, nothing untracked.
#
# The design documents (docs/) and the tooling that only serves them are
# development material and stay out (export-ignore in .gitattributes). So that
# nothing in the zip points at them, the zipped copy has its spec annotations
# removed (`@spec` comment lines and inline "(ID)" citations), along with any
# block marked "# >>> development tree only" ... "# <<<". The script refuses to
# write the zip if any spec ID or reference to the excluded files survives.
#
# Usage: scripts/package.sh            -> submission/order-matcher.zip
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

staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT
git archive --format=tar --prefix=order-matcher/ HEAD | tar -x -C "$staging"

mkdir -p submission
python3 - "$staging" submission/order-matcher.zip <<'EOF'
import os, re, sys, time, zipfile

root, out = sys.argv[1], sys.argv[2]
ID = r"[A-Z]+-[A-Z]+-[0-9]{3}"
ANNOTATION = re.compile(r"^\s*(//|#|<!--)\s*@spec\b")
CONTINUATION = re.compile(rf"^\s*(//|#)\s*({ID}\s*,?\s*)+$")
INLINE = [
    re.compile(rf" ?\(({ID}(, )?)+\)"),  # "... best-effort (OUT-ERR-005)"
    re.compile(rf"\b{ID}:? "),           # "// BOOK-MEM-001 default", "MATCH-EVT-003: each ..."
]

DEV_BEGIN, DEV_END = "# >>> development tree only", "# <<<"
EXCLUDED = re.compile(
    r"docs/|spec_coverage|package\.sh|development tree only|"
    r"\b(high-level-design|order-book|matching-engine|price|protocol|output|verification|deliverables|TODO)\.md\b")

def strip(text):
    lines, out, skipping, dev = text.split("\n"), [], False, False
    for i, line in enumerate(lines):
        if line is None:
            continue
        if line.startswith(DEV_BEGIN):
            dev = True
        if dev:
            dev = not line.startswith(DEV_END)
            continue
        if ANNOTATION.match(line):
            skipping = line.rstrip().endswith(",")
            # A markdown annotation sits on its own line followed by a blank one.
            if line.lstrip().startswith("<!--") and i + 1 < len(lines) and not lines[i + 1].strip():
                lines[i + 1] = None
            continue
        if skipping and CONTINUATION.match(line):
            skipping = line.rstrip().endswith(",")
            continue
        skipping = False
        for pattern in INLINE:
            line = pattern.sub("", line)
        out.append(line)
    return "\n".join(out)

left, count = [], 0
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            path = os.path.join(dirpath, name)
            arc = os.path.relpath(path, root)
            data = open(path, "rb").read()
            try:
                text = data.decode("utf-8")
            except UnicodeDecodeError:
                text = None
            if text is not None and not arc.startswith("order-matcher/data/"):
                text = strip(text)
                left += [f"{arc}: {m}" for m in re.findall(rf"@spec|\b{ID}\b", text)]
                left += [f"{arc}: {m.group(0)}" for m in EXCLUDED.finditer(text)]
                data = text.encode("utf-8")
            info = zipfile.ZipInfo(arc, date_time=time.localtime(os.stat(path).st_mtime)[:6])  # commit time
            info.external_attr = (os.stat(path).st_mode & 0o777 | 0o100000) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
            count += 1
if left:
    os.remove(out)
    sys.exit("package: spec references left after stripping:\n  " + "\n  ".join(left))
print(f"{count} files")
EOF
echo "wrote submission/order-matcher.zip (commit $(git rev-parse --short HEAD))"
