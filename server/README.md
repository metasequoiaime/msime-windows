# Metasequoia IME Server(水杉输入法 Server 端)

This is the server end of [MSIME-Windows](https://github.com/metasequoiaime/MSIME-Windows).

## How to build

### Prerequisites

- Visual Studio 2026
- CMake
- vcpkg
- Python3.10+
- Boost
- Node.js and pnpm 10 (settings page build)

Make sure vcpkg and Boost are installed by **Scoop**.

### Build steps(For Dev)

Clone the product and provision the reviewed dictionary release from its root:

```powershell
git clone --recursive https://github.com/metasequoiaime/MSIME-Windows.git
cd MSIME-Windows
python scripts/product_lock.py fetch-dictionaries --staging-root .
if ($LASTEXITCODE -ne 0) { throw 'Dictionary verification failed' }
$devLocalData = Join-Path $PWD 'build/dev-local'
$devData = Join-Path $devLocalData 'metasequoiaime'
New-Item -ItemType Directory -Force $devData | Out-Null
Copy-Item MetasequoiaImeDict/out/* $devData -Force
Copy-Item engine/helpcode/helpcodes $devData -Recurse -Force
Copy-Item server/assets/tables/* $devData -Force
Copy-Item server/assets/config/config.toml $devData -Force
Push-Location ui-html/webview2/settings/ime-settings
try {
    pnpm install --frozen-lockfile
    if ($LASTEXITCODE -ne 0) { throw 'Settings dependencies failed' }
    pnpm build
    if ($LASTEXITCODE -ne 0) { throw 'Settings page build failed' }
} finally { Pop-Location }
$devWebview = Join-Path $devData 'html/webview2'
New-Item -ItemType Directory -Force $devWebview | Out-Null
foreach ($part in @('shared', 'candwnd', 'ftb', 'menu')) {
    Copy-Item "ui-html/webview2/$part" $devWebview -Recurse -Force
}
$devSettings = Join-Path $devWebview 'settings/ime-settings'
New-Item -ItemType Directory -Force $devSettings | Out-Null
Copy-Item ui-html/webview2/settings/ime-settings/dist $devSettings -Recurse -Force
python server/scripts/prepare_env.py
if ($LASTEXITCODE -ne 0) { throw 'Server environment setup failed' }
$env:LOCALAPPDATA = $devLocalData
$env:METASEQUOIA_IME_DATA_DIR = $devData
cd server
```

Use a dedicated PowerShell terminal. These steps stage the same locked data as CI
and the current product pages in a separate development directory. Both
`LOCALAPPDATA` (Windows config/pages) and `METASEQUOIA_IME_DATA_DIR` (Engine data)
are set for that terminal; setting only the latter would still use installed
Windows configuration and pages. Close the terminal after development.
Dictionary source changes belong in Engine's `dictionary/`; use its root `build_profile.py`
to build data instead of calling internal stages in the archived Dict repository.

Then, build and run,

```powershell
.\scripts\lcompile.ps1
.\scripts\lrun.ps1
```

If you want to build and run in **one step**, run the following command,

```powershell
.\scripts\llaunch.ps1
```

## Codex CLI backend integration

The AI assistant accepts `provider = "codex"` as a separate backend. It launches
the Codex CLI using its existing `codex login` session rather than a Chat
Completions API token. In `[ai_assistant]`, `codex_executable` defaults to `codex`
from PATH and also accepts the full executable path. `model` is the active model;
`model_codex` stores its provider-specific value. An empty Codex model leaves the
choice to the CLI's built-in default model. The subprocess ignores user config
and profiles. Switching to an API provider restores that
provider's model and credentials.

Codex inference remains online, sends the composition and preceding context to
Codex, and consumes Codex allowance. The settings connection probe also makes
one online request with synthetic input. Token and endpoint fields are unused
for this backend; model-list fetching remains an API-provider operation.

Both settings hosts expose `ai_assistant.codex_executable` in `configSnapshot`
and accept it through the existing `configUpdate` contract. The setting and the
Codex model slot must stay in both configuration templates so installer upgrades
preserve them. Configuration isolation and provider-switch round trips are
covered in `test_config_non_ascii_path.cpp`; settings visibility and probe
payloads are covered by `ai-settings.test.ts`.

## Watchdog

`MetasequoiaImeWatchdog.exe` is built next to the server. Starting the server directly also starts the watchdog,
which monitors the server from the same directory and restarts it after an unexpected exit. Repeated early crashes use
an exponential restart delay (up to 30 seconds) to avoid a restart storm.

The developer terminate shortcut stops both processes. The restart shortcut asks the watchdog to start a fresh server
after the current process exits.

## English prefix candidates

Place `english.db` next to `msime.db` in `%LOCALAPPDATA%\metasequoiaime`. Set
`general.cn_en_mixed_input = true` in `config.toml` to enable asynchronous English prefix candidates for Quanpin and
Shuangpin. The master switch `utility.mixed_candidates` must also be on; English, emoji, kaomoji and date/time mixed
candidates are queried together on the single `MixedCandidates` worker thread (`src/mixed/`).

## Developer Shortcuts

The server process includes a few built-in global shortcuts that are useful during development:

- `Ctrl + Shift + Alt + T`
  - terminate `MetasequoiaImeServer` immediately
- `Ctrl + Shift + Alt + R`
  - restart `MetasequoiaImeServer`
  - Settings exposes the same restart under 外观 → 重启输入法进程; it reaches the
    server over the Aux pipe because Settings runs in its own process
- `Ctrl + Shift + Alt + C`
  - clear IME engine cache
- `Ctrl + Shift + Alt + 1` to `Ctrl + Shift + Alt + 8`
  - delete candidate `1` to `8` from the current candidate window
  - this only works while the candidate window is visible

## Screenshots

![](https://i.postimg.cc/c402J3KR/image.png)

![](https://i.postimg.cc/v8Bpx6Gf/image.png)

![](https://i.postimg.cc/ssBgtM5M/image.png)

![](https://i.postimg.cc/ryDqXH0B/image.png)

![](https://i.postimg.cc/2m9WJTgR/image.png)

![](https://i.postimg.cc/L96qQZT8/image.png)

![](https://i.postimg.cc/FNcz9QTv/image.png)

## Shared voice providers

The server links Engine VoiceCapture and Voice. WAV encoding and batch transcription/polish codecs come from the public library; Windows keeps provider configuration, SiliconFlow padding/retry behavior and Doubao streaming. Batch uploads are bounded to 20 MiB and responses to 1 MiB. Failed or empty polish responses retain the original transcript.

本地 Release 构建脚本可通过 `-ManifestTool <mt.exe 路径>` 使用其他 Windows SDK；未指定时沿用现有 SDK 路径。配置、编译或 manifest 嵌入失败都会中止，避免安装器收集旧产物。
