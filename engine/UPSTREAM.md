# Upstream provenance

This directory was `vendor/MetasequoiaImeEngine`, a submodule of
<https://github.com/metasequoiaime/MSIME-Engine>. It is now a component of this repository, vendored
in-tree at `engine/` alongside `server/`, `ui/`, `windows/` and `installer/`.

| | |
| --- | --- |
| Upstream repository | `metasequoiaime/MSIME-Engine` |
| Imported commit | `c810d201f549b337ae0c4a65a9d694103f1c1754` |
| Engine version at import | 0.9.0 (`version.txt`) |

That commit is the one `product-lock.json` pinned before the import, so this is a relocation and not
an engine upgrade: every source file here was verified against that commit's blob after the move.

The engine's own nested submodules were expanded in place at the commits it pinned:

| Path | Upstream | Commit |
| --- | --- | --- |
| `googlepinyinime-rev/` | `metasequoiaime/Google-PinyinIME-Rev` | `12db5237adfcecb79b8ac602d80f3576639ea219` |
| `utfcpp/` | `nemtrif/utfcpp` | `2d8e20b22dcb3e9b3c4f52103182ebda949c6089` |
| `ngram/kenlm/` | `kpu/kenlm` | `4cb443e60b7bf2c0ddf3c745378f76cb59e254e5` |
| `ngram/octagram/darts.h` | `s-yata/darts-clone` | `87b71afd6cf784953e3c08f24c64203397f3b724` (tag `v0.32h`) |
| `voice/third_party/miniaudio/` | `mackron/miniaudio` | `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d` |

`voice/third_party/sherpa-onnx/` was added after the import and is not part of the imported tree: it
is the C API header (`sherpa-onnx/c-api/c-api.h`) of `k2-fsa/sherpa-onnx` v1.13.8 (commit
`11afbd009a7f8c08f4bcf2fc1b265d0df4670fbf`), copied unmodified with its Apache-2.0 `LICENSE`, the
same file `metasequoiaime/msime` vendors. The Server's on-device recognition compiles against it and
loads the matching prebuilt runtime at run time; see `server/src/voice-input/local_asr_catalog.h`.

`ngram/octagram/darts.h` is the only file of that copy in the tree — upstream ships it as
`include/darts.h` next to `COPYING.md`, both of which are renamed on import to sit beside the
`octagram_gram` reader that is ours. It is a header-only double-array trie: `octagram_gram` uses
`set_array` to adopt the mmap of a `.gram` file, then `traverse` and `commonPrefixSearch`. None of
that is registered with darts-clone upstream, so it is tracked by tag rather than by branch.

**This copy is patched, not verbatim.** Replaying an upgrade means re-applying all of these against
the new tag:

- `DoubleArrayImpl::open()` drops upstream's 256-unit header check — the `size < 256 || (size & 0xFF)`
  guard, the `units[0]` validation, and the two-stage read that copies those units into `buf` before
  reading the remainder — and reads the array in a single `fread`.
- `DoubleArrayImpl::save()` drops its `offset` parameter and the `fseek` that used it, becoming
  `save(const char *, const char *, std::size_t) const`.
- `commonLongestPrefixSearch` is deleted: both overloads declared on the class and the out-of-line
  template definition. Only `commonPrefixSearch` and `traverse` are called by `octagram_gram`.
- `DoubleArrayBuilder`'s `BLOCK_SIZE` / `NUM_EXTRA_BLOCKS` / `NUM_EXTRAS` change from an unnamed `enum`
  to `static const std::size_t`.
- A `label()` comment typo is fixed (`associted` → `associated`).

The `open()` and `save()` rewrites follow from the same decision as the rest of this reader: loading
goes through Win32 mmap, so the file-opening and file-writing entry points are dead here. Verify the
current state against the tag before assuming any of them still applies.

`ngram/kenlm/` did not arrive with the engine import; it was added later, at the commit libime pins
for its own `src/libime/core/kenlm` submodule, so the query code and the shipped `sc.lm` come from
the same generation of kenlm. Only the query subset was copied: `lm/*.{hh,cc}`, `util/` and
`util/double-conversion/`. `lm/builder`, `lm/filter`, `lm/interpolate` and `lm/wrappers` are not
here, and no file that is here includes them. The sources are unmodified except for the one local
patch below; the two Windows adaptations it needs — a wide-character `util::_open` and the
`_HAS_AUTO_PTR_ETC` definition — live outside the subtree, in `ngram/kenlm_file_io.h` and
`ngram/CMakeLists.txt`.

**Local patch:** `util/string_stream.hh` `StringStream::AdvanceTo` — `&*out_.end()` and
`&*out_.begin()` dereference string iterators, which the MSVC checked iterators reject (a
"Debug Error!" assert under any `_DEBUG` build, on every formatted kenlm message). `out_.data() +
out_.size()` computes the same past-the-end pointer without touching iterators; release behaviour
is unchanged because the asserts were compiled out there anyway.

Two of those files, `googlepinyinime-rev/src/{include/userdict.h,share/userdict.cpp}`, are committed
upstream with CRLF and arrive here with LF. That is this repository's `.gitattributes` (`* text=auto
eol=lf`) doing what it does to every other file; the content is unchanged.

`googlepinyinime-rev/` is no longer tracked against its upstream and is edited here directly, in its
original formatting and under its original licence. Local changes so far: a per-syllable Hanzi
constraint for the shuangpin mid-sentence helpcode — `MatrixSearch::set_char_constraints`,
`MatrixSearch::filter_lpis_by_constraints` (called from `add_char_qwerty` before
`extend_mtrx_nd`), `im_set_char_constraints` and the `CharConstraintFn` typedef in `dictdef.h`;
and in the multi-syllable branch of `MatrixSearch::get_lpis`, items equal to the full sentence
candidate are dropped before deduplication, so a better-scored duplicate of a dropped item no longer
writes `lma_buf[-1]` (crashed the Server on shuangpin `zjle`, i.e. `zan'l`).

## Why this file and not `product-lock.json`

The engine used to be pinned by a gitlink, and `product-lock.json` recorded the commit so CI could
check that the checked-out submodule matched the reviewed one. There is no gitlink any more, so
there is nothing left for CI to compare against: the first Windows-specific fix to `engine/` makes
"this tree is upstream commit X" false, and nothing would notice. Recording the provenance in prose
says what is true — where the code came from — without claiming an equality that no longer holds.

Treat `engine/` as first-party code from here on. Changes to it are reviewed, formatted and tested
like any other directory of this repository (`scripts/format.sh` covers it), not merged from
upstream. Upstream is history, not a remote to track.

## What was removed during the Windows specialization

Everything below was deleted from the imported tree. No engine source file was otherwise modified.
`voice/CMakeLists.txt` lost the option blocks belonging to the deleted sources; the rest of the
changes are documentation (`README*.md`, `AGENTS.md`, `NOTICE.md`, the `contracts/` and `docs/`
prose) rewritten to describe a directory rather than a repository.

**Non-Windows platform code**

- `voice/third_party/whisper.cpp/` and the whisper provider (worker, header, test) — roughly 36 MB of
  local-inference ASR. This product's voice input uses cloud ASR; nothing here built or shipped it.
- `voice/examples/macos/` — a standalone macOS host application.

Portable `if(APPLE)` / `elseif(WIN32)` / `else()` guards inside the surviving sources were left
alone. Stripping them would be a large, untested rewrite of working code for no build-output gain.

**The dictionary build pipeline**

- `dictionary/` in full — about 228 MB of raw corpora (`cn`, `en`, `source`, `makecikudb`) and the
  scripts that turn them into a dictionary release. No CMake target in this repository consumes
  them, and the workflow that built them lived in the engine's own CI. The built dictionaries are
  still fetched at build time from their release, as `product-lock.json` records.
- `build_assets.py` and `build_profile.py`, which import `dictionary.build_profile`.

`contracts/assets/` was kept: `assets.h` is a generated header included by roughly twenty engine
translation units, and it is unrelated to the corpora despite the name.

**Upstream governance and tooling**

- `.github/` (the engine's own CI), `.vscode/`, `.gitmodules`, `.mailmap`,
  `.git-blame-ignore-revs`, `CHANGELOG.md`, `release-please-config.json`,
  `.release-please-manifest.json` — all of these describe a standalone repository with its own
  release train, which this directory no longer is.
- `scripts/format.sh` — this repository formats the engine from `scripts/format.sh` at the root, so
  a second entry point with its own exclusion list would only be a way for the two to disagree. The
  exclusions it carried were folded into the root script.
- Unused portions of the vendored `miniaudio`, `googlepinyinime-rev` and `utfcpp` copies. What
  remains of those three is still third-party and still under its own licence; see `NOTICE.md`.
