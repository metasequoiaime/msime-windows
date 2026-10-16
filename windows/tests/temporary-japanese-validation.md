# Issue #786 validation

Base: `develop` at `53649d9d12f3d71929f47f3a5fd43c7b3b70a83b`.

## Automated coverage

`temporary_japanese_input` includes the same production TSF key adapter used by
normal and deferred classification. It checks the resulting composing/input or
punctuation actions, full and temporary Japanese eligibility, repeated tests,
Ctrl/Alt, fresh compositions, raw strings containing consecutive long vowels,
and the shared insertion/backspace/caret boundaries. It also exercises policy
updates and exact one-character Worker payload validation.

The test does not slice source files, mock a TIP or instantiate a host. It does
not test the full deferred FIFO, TSF edit sessions, actual pipe delivery or stale
Worker/disconnect lifecycle. Those paths still require host/integration testing.

Server tests directly call production Japanese routing, word-to-character and
caret recovery functions. A separate integration case feeds the classified keys
through a real `EngineInputSession` and `JapaneseCandidateProvider`, requires
`ko-hi-` / `コーヒー`, and verifies that the original Chinese session remains `ni`.
Existing IPC contract tests cover optional-capability negotiation and old peers.

## Reproduction and host acceptance

User-reported environment: Windows 11 Home 26H2, build 26300.9711, Windows
Feature Experience Pack 1000.26100.372.0; Chrome 155.0.8059.82, official 64-bit
build. Notepad's application version and the loaded module paths were not
independently recorded. Restart hosts after replacing DLLs.

| Check | Steps | Expected result | Evidence |
| --- | --- | --- | --- |
| Notepad | Chinese temporary R mode, `Rko-hi-`, select the first candidate | `コーヒー`; no R prefix or duplicate commit | User reports Notepad passed; application version not recorded |
| Current test application | `Rko-hi-`; repeat with minus/equal paging on/off | First candidate `コーヒー`; additional minus keys remain input; equals follows Japanese punctuation handling | User screenshots and report confirm these candidate/key behaviors |
| Chrome | In an ordinary webpage textarea, `Rko-hi-`, Space; both paging settings | Exactly one `コーヒー` commit; no R prefix | User reports this test group passed in Chrome |
| Caret insertion | `Rkohi`, Left twice, `-`, Backspace | `Rko-hi`, then `Rkohi`; caret remains at the edit position | User reports the editing group passed in Chrome |
| Prefix boundary | With a nonempty R-mode body, move to its beginning and press Backspace; then Esc | R is retained at the body boundary; Esc clears composition | Included in the editing group reported passed |
| Other modes | Full Japanese `ko-hi-`; ordinary Chinese; disable/re-enable R; dedicated English candidates | Existing behavior preserved; R policy enabled only in eligible Chinese mode | User reports the mode-switching group passed in Chrome |
| Focus/lifecycle | Switch applications; switch to another IME and back during/after composition | No stale candidate window, duplicate commit or stuck input | IME-switch test included in the reported mode-switching group; Server restart not verified |

The detailed host matrix above was first reported with the second local test
installer. After receiving the third installer (0.0.3), built from the current
base and refactored source, the user reported no issues in testing. A real Server
restart and mixed old/new DLL/Server combinations have not been separately verified.

## Commands

Run both TSF architectures and Server Release builds using the repository
scripts. After building test targets:

```powershell
ctest --test-dir .build/tsf-x64 -C Release --output-on-failure
ctest --test-dir .build/tsf-x86 -C Release --output-on-failure
ctest --test-dir .build/server -C Release --output-on-failure --timeout 120
```

Server tests require the product-lock dictionaries plus repository tables,
helpcodes and test configuration. Point `LOCALAPPDATA`,
`METASEQUOIA_IME_DATA_DIR` and `METASEQUOIA_IME_CONFIG_DIR` at isolated staging
directories; do not run against installed user data. Follow the repository's
clang-format 18.1.8 and changed-file checks before submitting.

## Current automated results

- x64 and x86 TSF Release DLLs rebuilt; CTest 16/16 passed on each architecture.
- Server tests rebuilt on the current base; CTest 3/3 passed with isolated data.
- clang-format 18.1.8, diff whitespace and developer-path checks passed.
- Root Python suite: 17/19 passed with Python UTF-8 mode and working python3/Git
  Bash selected. The two `test_release_pr_ci` cases fail before assertions when
  the mocked CLI call log is absent; both fail identically from a clean archive
  of the base commit on this Windows machine. Linux CI verification is pending.
- Full Server/Settings Release product build passed via
  `server/scripts/lcompile-release.ps1`, including Server manifest embedding.
- Five isolated negative variants (full-Japanese-only classification, missing
  prefix clamp, unprotected prefix Backspace, overlong policy frames and Ctrl/Alt
  input) all failed the new direct-production test as expected. Only copied test
  modules under the build directory were mutated; production sources were not.
- Build tools: MSVC 19.44 / Visual Studio 2022, Windows SDK 10.0.26100.


## Third local installer

The complete third test installer uses version 0.0.3, with both TSF DLLs stamped
0.0.3.0. TSF, Server and settings-page build scripts completed. Source version
templates were restored after packaging. The user subsequently reported no issues
with the third installer. Local signing, certificate setup and taskbar icon-refresh
packaging are excluded from this source change. Icon restoration was not separately
confirmed and is not claimed as an effect of the long-vowel fix.
