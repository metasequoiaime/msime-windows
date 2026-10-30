#!/usr/bin/env bash
# The review gate that fits inside a Codespace.
#
# A reviewer's real question is "will this PR's CI pass", and the honest answer needs a Windows
# compiler. This script does not try to answer it. It answers the questions that a Linux container
# can answer without guessing, so that by the time somebody opens the Dev build artifact they know
# the diff is not going to fail on something cheap:
#
#   1. Which files changed, and whether the change goes anywhere near the Windows-only trees. A PR
#      that touches server/, ui/ or windows/ cannot be compiled here; the script says so up front
#      instead of letting a green result be misread as a build check.
#   2. The changed-file format check (clang-format 18.1.8, the version CI pins).
#   3. The Linux-side checks in check-linux.sh.
#
# It needs gh authenticated, and a token with pull-request read access. In a Codespace, gh is
# already logged in. Outside one: `gh auth login` first.
#
# The workflow is a gate on evidence, not a second CI. The job is to hand the reviewer the facts a
# Linux container can actually establish, and to stop a green result from being mistaken for "the
# Windows build will pass" -- it will not tell you that, and the closing lines say so.
#
# Usage: bash .devcontainer/check-pr.sh <PR number> [--no-format]
#   The PR number, not a branch name: review decisions are made about a revision, and a branch may
#   have moved between the fetch and the read.
set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1

# The repository root, saved before anything changes directory. The Linux-side checks run inside the
# temporary worktree, but this script itself lives in the main checkout's .devcontainer/.
runtime_root=$PWD

if [[ $# -lt 1 || ! $1 =~ ^[0-9]+$ ]]; then
    echo "usage: ${BASH_SOURCE[0]} <PR number> [--no-format]" >&2
    exit 2
fi
pr=$1
shift

if ! command -v gh >/dev/null 2>&1; then
    echo 'gh is not installed. Rebuild the container, or use the Dev build workflow instead.' >&2
    exit 2
fi
if ! gh auth status >/dev/null 2>&1; then
    echo 'gh is installed but not authenticated. Run "gh auth login" first.' >&2
    exit 2
fi

repo=$(gh repo view --json nameWithOwner --jq .nameWithOwner)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "==> PR #$pr on $repo"
meta=$(gh pr view "$pr" --repo "$repo" --json state,isDraft,baseRefName,headRefName,headRefOid,title,additions,deletions,changedFiles) || exit 1

report() {
    python3 - "$meta" <<'PY'
import json, sys

meta = json.loads(sys.argv[1])
print(f"    title:  {meta['title']}")
print(f"    state:  {meta['state']}{' (draft)' if meta['isDraft'] else ''}")
print(f"    base:   {meta['baseRefName']} <- {meta['headRefName']}")
print(f"    head:   {meta['headRefOid'][:12]}")
print(f"    size:   +{meta['additions']} -{meta['deletions']} across {meta['changedFiles']} files")
PY
}
report
head=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["headRefOid"])' "$meta")

# The PR's own diff, restricted to file names. Not the full patch: the point here is routing, and a
# large PR would otherwise fill the terminal before anything useful is printed.
gh api "repos/$repo/pulls/$pr/files" --paginate --jq '.[].filename' >"$tmp/files"
if [[ ! -s "$tmp/files" ]]; then
    echo '    (no changed files reported; the PR may be empty)'
fi

echo
echo '==> where the change lands'
python3 - "$tmp/files" <<'PY'
import sys

groups = {
    'Windows-only, cannot compile here': ('windows/', 'server/', 'ui/', 'log/'),
    'engine/ (mostly portable, but built by the Server)': ('engine/',),
    'docs and config': ('docs/', '.github/', 'scripts/', 'installer/', 'skins/', 'examples/'),
}
counts = {name: 0 for name in groups}
other = []
for path in open(sys.argv[1], encoding='utf-8').read().splitlines():
    for name, prefixes in groups.items():
        if path.startswith(prefixes):
            counts[name] += 1
            break
    else:
        other.append(path)

for name, count in counts.items():
    print(f'    {count:4d}  {name}')
if other:
    print(f'    {len(other):4d}  top-level or unclassified files')
    for path in other[:10]:
        print(f'          {path}')
    if len(other) > 10:
        print(f'          ... and {len(other) - 10} more')
PY

echo
echo '==> fetching the PR head into a temporary worktree'
git fetch --quiet --depth=1 "https://github.com/$repo.git" "$head" || exit 1
wt="$tmp/tree"
git worktree add --quiet --detach "$wt" "$head" || exit 1
# Submodules are not needed for the checks below; skipping them keeps the fetch small.
cd "$wt" || exit 1

failures=()

echo
echo "==> formatting the files this PR changed"
if [[ " $* " == *' --no-format '* ]]; then
    echo '    skipped: --no-format was passed'
else
    # Only the changed files, and only those the project's own gate covers. Formatting an untouched
    # file would blame this PR for a mistake it did not make.
    changed=$(python3 - "$tmp/files" <<'PY'
import sys
covered = ('server/', 'windows/', 'ui/', 'log/', 'engine/')
excluded = ('engine/googlepinyinime-rev/', 'engine/ngram/kenlm/', 'engine/utfcpp/', '/third_party/', '/vendor/')
generated = {
    'engine/contracts/assets/assets.h',
    'engine/contracts/dictionary/format.h',
    'engine/contracts/punctuation/policy.h',
    'engine/contracts/webview/schema.h',
    'engine/ngram/octagram/darts.h',
}
for path in open(sys.argv[1], encoding='utf-8').read().splitlines():
    if not path.endswith(('.cpp', '.h', '.hpp')):
        continue
    if not path.startswith(covered):
        continue
    if path.startswith(excluded) or path in generated:
        continue
    print(path)
PY
)
    if [[ -z "$changed" ]]; then
        echo '    no C++ files this gate covers were changed'
    else
        # shellcheck disable=SC2086 # word splitting is the point; the paths contain no spaces
        if bash scripts/format.sh --check $changed; then
            echo '    ok'
        else
            failures+=('formatting of changed files')
        fi
    fi
fi

echo
echo '==> Linux-side checks (runs inside the temporary worktree; the script is taken from the main checkout)'
linux_args=()
if [[ " $* " == *' --no-format '* ]]; then
    linux_args+=(--no-format)
fi
if ! bash "$runtime_root/.devcontainer/check-linux.sh" "${linux_args[@]}"; then
    failures+=('Linux-side checks')
fi

echo
if [[ ${#failures[@]} -eq 0 ]]; then
    echo "Linux-side review of PR #$pr is clean."
else
    echo 'Problems:'
    printf '  %s\n' "${failures[@]}"
fi
echo
echo 'This is not a build check. If the PR touches windows/, server/, ui/ or engine/, read the Dev'
echo 'build artifact for it (CONTRIBUTING.md, "没有本地工具链时：dev-build workflow") before approving.'
[[ ${#failures[@]} -eq 0 ]]
