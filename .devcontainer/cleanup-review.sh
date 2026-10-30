#!/usr/bin/env bash
# Clean up after check-pr.sh.
#
# check-pr.sh registers its temporary worktree under $TMPDIR and removes it when it exits. A run
# that was killed (Ctrl-C during a long format check, a closed terminal) skips that trap and leaves
# both the directory and the .git/worktrees entry behind, which makes `git worktree list` in the
# main checkout look wrong. This is safe to run any time.
#
# It only removes directories matching the shape check-pr.sh creates (a mktemp -d directory holding
# a `files` listing and a `tree` worktree), so it cannot delete unrelated temporary files.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

shopt -s nullglob
for dir in "${TMPDIR:-/tmp}"/tmp.*; do
    if [[ -f "$dir/files" && -d "$dir/tree" ]]; then
        # Deregister first, then delete: rm -rf followed by prune also works, but this way the
        # verbose output names the worktree before its directory disappears.
        git worktree remove --force "$dir/tree" >/dev/null 2>&1 || true
        echo "removing leftover review worktree: $dir"
        rm -rf "$dir"
    fi
done

git worktree prune --verbose
