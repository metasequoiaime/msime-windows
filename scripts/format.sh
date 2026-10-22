#!/usr/bin/env bash
set -euo pipefail

# Formatting has to be reproducible across contributor machines and CI, so the
# formatter version is pinned here and installed from PyPI instead of being
# taken from whatever the host toolchain happens to ship. Apple, Debian and
# LLVM upstream all disagree about clang-format defaults between releases.
#
# Ported from MSIME-Linux, which has run this in CI since before the
# consolidation. Each component keeps its own .clang-format; --style=file picks
# up the nearest one, so server/, windows/, ui/, log/ and engine/ stay
# independent -- engine/voice/.clang-format widens the column limit for that
# subtree alone, and the nearest-file rule is what keeps it that way.
clang_format_version=18.1.8

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
venv_root=${METASEQUOIA_CLANG_FORMAT_VENV:-"${TMPDIR:-/tmp}/metasequoia-clang-format-$clang_format_version"}
clang_format="$venv_root/bin/clang-format"

if [[ ! -x "$clang_format" ]]; then
    python3 -m venv "$venv_root"
    "$venv_root/bin/pip" install --quiet "clang-format==$clang_format_version"
fi

mode=${1:---write}
case "$mode" in
    --check) clang_format_arguments=(--dry-run --Werror) ;;
    --write) clang_format_arguments=(-i) ;;
    *)
        echo "Usage: ${BASH_SOURCE[0]} [--check|--write]" >&2
        exit 2
        ;;
esac

cd "$project_root"
# --others --exclude-standard so a newly written file that has not been staged
# yet is still formatted. Without it the script reports a clean tree while
# skipping exactly the file being worked on.
#
# vendor/ and experiments/ are excluded: the first is third-party, the second is
# a scratch project that is not part of the shipped product.
#
# engine/ is covered here rather than by a script of its own. It used to be a
# separate repository with its own formatting gate; now that it is a directory
# of this one, a Windows-specific fix to it has to be held to the same standard
# as the code that calls it, and one entry point is what makes that automatic.
# The exclusions below are the ones that gate carried:
#
#   - googlepinyinime-rev keeps its AOSP formatting, and ngram/kenlm and utfcpp
#     are upstream copies, so none of them is ours to reformat; same for
#     */third_party/ (miniaudio).
#   - The four generated headers are emitted from JSON by
#     contracts/{assets,dictionary,punctuation,webview}/generate.py. No CI job
#     re-runs those generators; after editing the JSON, run each generate.py
#     yourself (all of them take --check). Reformatting the checked-in copy
#     would only make it disagree with what the generator produces.
#   - ngram/octagram/darts.h is darts-clone itself, listed by exact path rather
#     than by directory the way the other copies are: its siblings in that
#     directory are ours (octagram_gram.{h,cpp}) and stay under the gate.
#   - server/src/settings/native/msime_client.h is the upstream host-api C ABI
#     header (scripts/ci/HOST-API.md): scripts/ci/build-host-api.ps1 refreshes
#     it from metasequoiaime/msime, and reformatting it here would break that
#     byte-for-byte update the next time it is copied over.
#   - server/src/settings/native/msime_client.h is the upstream host-api C ABI
#     header (scripts/ci/HOST-API.md): scripts/ci/build-host-api.ps1 refreshes
#     it from metasequoiaime/msime, and reformatting it here would break that
#     byte-for-byte update the next time it is copied over.
git ls-files --cached --others --exclude-standard \
        'server/*.cpp' 'server/*.h' \
        'windows/*.cpp' 'windows/*.h' \
        'ui/*.cpp' 'ui/*.h' \
        'log/*.cpp' 'log/*.h' \
        'engine/*.cpp' 'engine/*.h' 'engine/*.hpp' \
    | grep -v '/vendor/' \
    | grep -vE '^engine/(googlepinyinime-rev|ngram/kenlm|utfcpp)/' \
    | grep -v '/third_party/' \
    | grep -vxF \
        -e server/src/settings/native/msime_client.h \
        -e engine/contracts/assets/assets.h \
        -e engine/contracts/dictionary/format.h \
        -e engine/contracts/punctuation/policy.h \
        -e engine/contracts/webview/schema.h \
        -e engine/ngram/octagram/darts.h \
    | sort -u \
    | xargs "$clang_format" "${clang_format_arguments[@]}" --style=file
