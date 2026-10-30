#!/usr/bin/env bash
# What this container can verify, in one command.
#
# Runs the subset of CONTRIBUTING.md's "提交前" list that does not need Windows. Compilation is not
# in that subset and never will be: server/, ui/ and windows/ include windows.h. Compile
# verification goes to the Dev build workflow.
#
# Exit status is non-zero if any step fails. Every step runs even after one fails, so a single call
# shows everything that is wrong with the checkout instead of stopping at the first problem.
#
# Usage: bash .devcontainer/check-linux.sh [--no-format]
#   --no-format   skip scripts/format.sh --check; it downloads clang-format 18.1.8 on first use and
#                 needs network, which a sandboxed session may not have.
set -uo pipefail

# The script lives in .devcontainer/ but the repository root is one level up; the path below is
# relative to .devcontainer/ now that the scripts moved out of scripts/.
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1

run_format=1
for arg in "$@"; do
    case "$arg" in
        --no-format) run_format=0 ;;
        *)
            echo "unknown argument: $arg" >&2
            exit 2
            ;;
    esac
done

failures=()
skips=()

step() {
    local name=$1
    shift
    echo
    echo "==> $name"
    if "$@"; then
        return 0
    fi
    failures+=("$name")
    return 1
}

skip() {
    local name=$1 reason=$2
    echo
    echo "==> $name"
    echo "    skipped: $reason"
    skips+=("$name: $reason")
}

# Developer paths first: it is a two-second grep and it catches the most embarrassing class of
# mistake, a commit that hardcodes somebody's home directory.
step 'developer paths in build inputs' bash scripts/ci/check-developer-paths.sh

# The product-lock and dictionary contracts, same as ci.yml's "Product inputs" job. They are pure
# Python and need no data, so the sandbox can hold them to the same bar.
step 'dictionary contract' python3 scripts/check-dictionary-contract.py
step 'product-lock contract' python3 scripts/check-product-lock-contract.py

# The autocorrect table must stay byte-identical to the generator's output; quality.yml runs this
# on the same Ubuntu base image.
step 'autocorrect table' python3 server/scripts/generate_quanpin_autocorrect.py --check

# The product-level combination tests. They must not require Windows: if this grows a dependency on
# a Windows-only helper, the failure belongs here rather than in a surprise at review time.
step 'product tests' python3 -m unittest discover -s tests

if [[ $run_format -eq 1 ]]; then
    # format.sh pins clang-format 18.1.8 and installs it with pip into a repository-local venv.
    # That needs PyPI on first use; sandboxed sessions may have no network. CI remains the
    # authority on formatting, so no network is reported as a skip rather than a failure.
    step 'formatting' bash scripts/format.sh --check
else
    skip 'formatting' '--no-format was passed'
fi

echo
if [[ ${#failures[@]} -eq 0 ]]; then
    echo 'All Linux-side checks passed.'
    if [[ ${#skips[@]} -gt 0 ]]; then
        echo 'Skipped:'
        printf '  %s\n' "${skips[@]}"
    fi
    echo
    echo 'Reminder: nothing here compiles the TSF DLL, the Server or msimeui. To verify a build,'
    echo 'push the branch and let the Dev build workflow produce an installer (CONTRIBUTING.md).'
    exit 0
fi

echo 'Failed:'
printf '  %s\n' "${failures[@]}"
if [[ ${#skips[@]} -gt 0 ]]; then
    echo 'Skipped:'
    printf '  %s\n' "${skips[@]}"
fi
exit 1
