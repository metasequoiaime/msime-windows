[CmdletBinding()]
param(
    [string]$TargetVersion = '0.0.1',
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    # Component paths are relative to RepoRoot and default to the consolidated layout.
    # Historical or custom layouts remain available through explicit overrides.
    [string]$TsfDirectory = 'windows',
    [string]$ServerDirectory = 'server',
    [string]$HelpCodeDirectory = 'engine/helpcode',
    # U 模式的部件拆字反查表（decomposition.txt）随包发布到数据目录的 radical\ 下。
    [string]$RadicalDirectory = 'engine/radical',
    # Google 解码器的系统词典。仓库里有两份完全相同的副本：这一份是 engine/contracts/assets
    # 的清单声明的来源，server/assets/tables 下那份是跑测试时拷进数据目录的开发副本。打包
    # 取清单声明的那份，免得哪天两份不同步了装出来的和测出来的不是一个东西。
    [string]$PinyinModelDirectory = 'engine/googlepinyinime-rev/data',
    [string]$DictionaryDirectory = 'MetasequoiaImeDict',
    # 词格打分用的语言模型不随词库发布下发，而是由 scripts/build-language-model.ps1
    # 从钉住的上游语料现场转换到这个目录。
    [string]$LanguageModelDirectory = 'language-model',
    # 神经整句速度档模型（chinese-ime-lm 的 safetensors）从这个目录下发，与 sc.lm 同为资源根下的
    # 只读文件。缺文件时引擎静默不出神经整句，但打包仍然必须带上，故这里断言存在。效果档模型
    # 不随包分发，由设置页按需下载到 <数据目录>\models\neural-desktop\。
    [string]$NeuralModelDirectory = 'neural-model',
    # THIRD_PARTY_NOTICES.txt used to sit next to the tip's sources. In the consolidated repository
    # the notice covers the whole product and lives at the root, one level above windows/, so where
    # to read it is no longer answered by where the tip is.
    [string]$NoticesDirectory = '.',
    [switch]$Light,
    # PDBs are ~140 MB against ~20 MB of actual binaries, and Inno Setup compresses them with solid
    # LZMA2, so staging them is what makes a local packaging run take minutes. Symbols are therefore
    # opt-in: the release job and test-symbols.ps1 pass this, everyday local runs do not. The
    # assertions above still require the build to have produced matching PDBs either way.
    [switch]$IncludeSymbols
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-PathExists {
    param([Parameter(Mandatory)][string]$LiteralPath, [Parameter(Mandatory)][string]$Description)
    if (-not (Test-Path -LiteralPath $LiteralPath)) {
        throw "$Description 不存在：$LiteralPath"
    }
}

function Copy-DirectoryContents {
    param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Destination)
    Assert-PathExists -LiteralPath $Source -Description '源目录'
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Get-ChildItem -LiteralPath $Source -Force | Copy-Item -Destination $Destination -Recurse -Force
}

function Reset-Directory {
    param([Parameter(Mandatory)][string]$LiteralPath)
    if (Test-Path -LiteralPath $LiteralPath) {
        Remove-Item -LiteralPath $LiteralPath -Recurse -Force
    }
    New-Item -ItemType Directory -Path $LiteralPath -Force | Out-Null
}

$serverRelease = Join-Path $RepoRoot (Join-Path $ServerDirectory 'build-release\bin\Release')
$dictionaryReplayRelease = Join-Path $serverRelease 'MetasequoiaImeDictionaryReplay.exe'
$tsf32Release = Join-Path $RepoRoot (Join-Path $TsfDirectory 'build32-release\Release\MetasequoiaImeTsf.dll')
$tsf64Release = Join-Path $RepoRoot (Join-Path $TsfDirectory 'build64-release\Release\MetasequoiaImeTsf.dll')
$tsf32Pdb = Join-Path $RepoRoot (Join-Path $TsfDirectory 'build32-release\Release\MetasequoiaImeTsf.pdb')
$tsf64Pdb = Join-Path $RepoRoot (Join-Path $TsfDirectory 'build64-release\Release\MetasequoiaImeTsf.pdb')
# 原生（Direct2D）设置窗口用到的图标与图片，按原相对路径装到数据目录的 assets\settings\。
$settingsAssets = Join-Path $RepoRoot (Join-Path $ServerDirectory 'assets\settings')
$defaultSkinsSource = Join-Path $RepoRoot 'skins\default'
$bundledSkinsSource = Join-Path $RepoRoot 'skins'
$serverConfig = Join-Path $RepoRoot (Join-Path $ServerDirectory 'assets\config\config.toml')
$factoryConfig = Join-Path $PSScriptRoot 'default_config\config.default.toml'
$pinyinTable = Join-Path $RepoRoot (Join-Path $ServerDirectory 'assets\tables\pinyin.txt')
$pinyinModel = Join-Path $RepoRoot (Join-Path $PinyinModelDirectory 'dict_pinyin.dat')
$helpcodeSource = Join-Path $RepoRoot (Join-Path $HelpCodeDirectory 'helpcodes')
$radicalTable = Join-Path $RepoRoot (Join-Path $RadicalDirectory 'decomposition.txt')
$appIcon = Join-Path $RepoRoot (Join-Path $ServerDirectory 'src\resource\MetasequoiaIME.ico')
# 输入法语言配置的图标，与 TSF DLL 里的 IDIS_METASEQUOIAIME 是同一个文件。安装器把它放到固定路径，
# DLL 注册时优先用它，免得图标路径随版本目录变化（见 msime_setup.iss）。
$tipIcon = Join-Path $RepoRoot (Join-Path $TsfDirectory 'image\msime.ico')
$thirdPartyNotices = Join-Path $RepoRoot (Join-Path $NoticesDirectory 'THIRD_PARTY_NOTICES.txt')
$license = Join-Path $RepoRoot 'LICENSE'
$dictionaryDb = Join-Path $RepoRoot (Join-Path $DictionaryDirectory 'out\msime.db')
$dictionaryManifest = Join-Path $RepoRoot (Join-Path $DictionaryDirectory 'out\dictionary-manifest.json')
$japaneseModel = Join-Path $RepoRoot (Join-Path $DictionaryDirectory 'out\dict_japanese.dat')
$japaneseModelLicense = Join-Path $RepoRoot (Join-Path $DictionaryDirectory 'source\mozc_dictionary_oss\README.txt')
$englishDb = Join-Path $RepoRoot (Join-Path $DictionaryDirectory 'out\english.db')
$othersDb = Join-Path $RepoRoot (Join-Path $DictionaryDirectory 'out\others.db')
$languageModel = Join-Path $RepoRoot (Join-Path $LanguageModelDirectory 'sc.lm')
$languageModelNotice = Join-Path $RepoRoot (Join-Path $LanguageModelDirectory 'NOTICE.md')
$neuralModelKeyboard = Join-Path $RepoRoot (Join-Path $NeuralModelDirectory 'sentence-model.safetensors')
$neuralModelNotice = Join-Path $RepoRoot (Join-Path $NeuralModelDirectory 'NOTICE.md')

Assert-PathExists -LiteralPath $RepoRoot -Description '源码仓库根目录'
Assert-PathExists -LiteralPath $serverRelease -Description 'Server Release 输出目录'
Assert-PathExists -LiteralPath $dictionaryReplayRelease -Description '用户词库回放程序 Release EXE'
Assert-PathExists -LiteralPath $tsf32Release -Description '32 位 TSF Release DLL'
Assert-PathExists -LiteralPath $tsf64Release -Description '64 位 TSF Release DLL'
Assert-PathExists -LiteralPath $tsf32Pdb -Description '32 位 TSF Release PDB'
Assert-PathExists -LiteralPath $tsf64Pdb -Description '64 位 TSF Release PDB'
$serverExecutables = @(
    Get-ChildItem -LiteralPath $serverRelease -Recurse -File -Filter '*.exe' |
        Where-Object { $_.BaseName -notlike '*Tests' -and $_.BaseName -notlike 'test_*' }
)
$missingServerPdb = @(
    $serverExecutables |
        Where-Object {
            -not (Test-Path -LiteralPath (Join-Path $_.DirectoryName "$($_.BaseName).pdb"))
        } |
        ForEach-Object { Join-Path $_.DirectoryName "$($_.BaseName).pdb" }
)
if ($missingServerPdb.Count -gt 0) {
    throw "Server Release 缺少同名 PDB：$($missingServerPdb -join ', ')"
}
Assert-PathExists -LiteralPath $serverConfig -Description 'Server config.toml'
Assert-PathExists -LiteralPath $appIcon -Description '应用图标'
Assert-PathExists -LiteralPath $tipIcon -Description '输入法语言配置图标 msime.ico'
Assert-PathExists -LiteralPath $thirdPartyNotices -Description '第三方声明 THIRD_PARTY_NOTICES.txt'
Assert-PathExists -LiteralPath $license -Description '许可证 LICENSE'
Assert-PathExists -LiteralPath $settingsAssets -Description '设置窗口图片资源目录'
Assert-PathExists -LiteralPath $defaultSkinsSource -Description '内置皮肤设置清单 skins\default'

if (-not $Light) {
    Assert-PathExists -LiteralPath $factoryConfig -Description '出厂配置 default_config\config.default.toml'
    Assert-PathExists -LiteralPath $pinyinTable -Description '完整拼音音节表 pinyin.txt'
    Assert-PathExists -LiteralPath $pinyinModel -Description 'Google 解码器系统词典 dict_pinyin.dat'
    Assert-PathExists -LiteralPath $helpcodeSource -Description '辅助码目录'
    Assert-PathExists -LiteralPath $radicalTable -Description 'U 模式部件拆字表 decomposition.txt'
    Assert-PathExists -LiteralPath $dictionaryDb -Description '词库数据库 msime.db'
    Assert-PathExists -LiteralPath $japaneseModel -Description '日语整句模型 dict_japanese.dat'
    Assert-PathExists -LiteralPath $japaneseModelLicense -Description 'Mozc 日语词典授权声明'
    Assert-PathExists -LiteralPath $englishDb -Description '英文词库数据库 english.db'
    python -c @"
import sqlite3, sys
cols = list(sqlite3.connect(sys.argv[1]).execute('PRAGMA table_info(english_words)'))
names = {row[1] for row in cols}
pk = [row[1] for row in cols if row[5] > 0]
if 'weight' not in names or pk != ['word', 'display']:
    raise SystemExit('english.db schema is stale; rebuild with weight and PRIMARY KEY(word, display)')
"@ $englishDb
    if ($LASTEXITCODE -ne 0) {
        throw "英文词库数据库 schema 检查失败：$englishDb"
    }
    Assert-PathExists -LiteralPath $othersDb -Description '杂项数据库 others.db'
    # 摘要校验留在 build-language-model.ps1 里：它既有 lock.json 又刚生成完文件。
    # 这里只确认文件在，与 msime.db 一样 —— 词库的摘要也是由 product_lock.py 校验的。
    Assert-PathExists -LiteralPath $languageModel `
        -Description '词格整句语言模型 sc.lm（用 scripts\build-language-model.ps1 生成）'
    Assert-PathExists -LiteralPath $languageModelNotice -Description 'libime 语言模型授权声明 NOTICE.md'
    Assert-PathExists -LiteralPath $neuralModelKeyboard -Description '神经整句模型 sentence-model.safetensors'
    Assert-PathExists -LiteralPath $neuralModelNotice -Description 'chinese-ime-lm 授权声明 NOTICE.md'
}

$targetAppData = Join-Path $PSScriptRoot 'app_data'
$targetServer = Join-Path $PSScriptRoot 'server_exe'
$targetTsf = Join-Path $PSScriptRoot 'tsf_dll'

if ($Light) {
    Write-Host '轻量模式：跳过词库、辅助码、拼音表和出厂配置，只刷新 TSF、Server 和设置窗口图片资源。'
    New-Item -ItemType Directory -Path $targetAppData -Force | Out-Null
}
else {
    Reset-Directory -LiteralPath $targetAppData
    if (-not (Get-Content -LiteralPath $pinyinTable | Where-Object { $_.Trim() -eq 'xing' })) {
        throw "完整拼音音节表缺少 xing：$pinyinTable"
    }
    Copy-Item -LiteralPath $pinyinTable -Destination (Join-Path $targetAppData 'pinyin.txt') -Force
    # Google 解码器的整句候选（CandidateSource::Fallback）没有这份词典就完全出不来，而且和
    # sc.lm 一样是静默失败：im_open_decoder 返回 false，解码器直接不出候选，不报任何错。
    # 与它配对的 user_dict.dat 是 role=user 的可写文件，profiles 为空，故意不进包 ——
    # 引擎首次使用时由 UserDict::reset 在用户数据目录下自行建出来。
    Copy-Item -LiteralPath $pinyinModel -Destination (Join-Path $targetAppData 'dict_pinyin.dat') -Force
    Copy-Item -LiteralPath $dictionaryDb -Destination (Join-Path $targetAppData 'msime.db') -Force
    if (Test-Path -LiteralPath $dictionaryManifest) {
        Copy-Item -LiteralPath $dictionaryManifest -Destination (Join-Path $targetAppData 'dictionary-manifest.json') -Force
    }
    Copy-Item -LiteralPath $japaneseModel -Destination (Join-Path $targetAppData 'dict_japanese.dat') -Force
    Copy-Item -LiteralPath $japaneseModelLicense -Destination (Join-Path $targetAppData 'MOZC_DICTIONARY_LICENSE.txt') -Force
    Copy-Item -LiteralPath $englishDb -Destination (Join-Path $targetAppData 'english.db') -Force
    Copy-Item -LiteralPath $othersDb -Destination (Join-Path $targetAppData 'others.db') -Force
    # sc.lm 落在资源目录根下，与 msime.db 同级：engine/contracts/assets 的清单就是这么
    # 声明这个资源的路径的，引擎按该清单里的文件名去找。
    Copy-Item -LiteralPath $languageModel -Destination (Join-Path $targetAppData 'sc.lm') -Force
    # 模型数据是 LGPL-2.1-or-later 的第三方作品，声明必须跟着二进制到用户磁盘上，
    # 理由同下面 rime-ice 那段。
    Copy-Item -LiteralPath $languageModelNotice -Destination (Join-Path $targetAppData 'libime-lm-NOTICE.md') -Force
    # 神经整句速度档模型与 sc.lm 一样落在资源目录根下，文件名由 engine/contracts/assets 清单声明。
    # 缺文件时引擎静默不出神经整句候选（shared_sentence_model 返回 nullptr），不影响其它候选。
    Copy-Item -LiteralPath $neuralModelKeyboard -Destination (Join-Path $targetAppData 'sentence-model.safetensors') -Force
    # 权重是 Apache-2.0 的第三方作品，授权声明必须随二进制到用户磁盘上。
    Copy-Item -LiteralPath $neuralModelNotice -Destination (Join-Path $targetAppData 'chinese-ime-lm-NOTICE.md') -Force

    $defaultConfigPath = Join-Path $targetAppData 'config.default.toml'
    # 出厂配置来自本仓库的 default_config，不依赖本机是否已安装输入法。
    # 安装脚本用 onlyifdoesntexist 生成用户 config.toml，升级不会覆盖已有方案/主题。
    Copy-Item -LiteralPath $factoryConfig -Destination $defaultConfigPath -Force
    $defaultConfig = Get-Content -LiteralPath $defaultConfigPath -Raw
    if ($defaultConfig -notmatch '(?m)^schema\s*=\s*"quanpin"\s*$') {
        throw '出厂配置的 input.schema 必须是 quanpin。'
    }
    if ($defaultConfig -notmatch '(?m)^theme_mode\s*=\s*"system"\s*$') {
        throw '出厂配置的 appearance.theme_mode 必须是 system。'
    }
    if ($defaultConfig -match '(?m)^diagnostic_log\s*=\s*true\s*$') {
        throw '出厂配置不应默认打开 diagnostic_log。'
    }
    if ($defaultConfig -notmatch '(?m)^settings_window_linger\s*=\s*"off"\s*$') {
        throw '出厂配置的 appearance.settings_window_linger 必须是 off（关闭后立即退出）。'
    }
    foreach ($sentenceSwitch in @('sentence_wordlattice', 'sentence_google', 'sentence_neural_keyboard')) {
        if ($defaultConfig -notmatch "(?m)^$sentenceSwitch\s*=\s*true\s*$") {
            throw "出厂配置的 association.$sentenceSwitch 必须默认开启。"
        }
    }
    $defaultConfig = $defaultConfig.TrimEnd("`r", "`n") + "`r`n"
    Set-Content -LiteralPath $defaultConfigPath -Value $defaultConfig -Encoding utf8NoBOM -NoNewline
    foreach ($stagedUserConfig in @('config.toml', 'config.base.toml')) {
        $stagedPath = Join-Path $targetAppData $stagedUserConfig
        if (Test-Path -LiteralPath $stagedPath) {
            Remove-Item -LiteralPath $stagedPath -Force
        }
    }

    $targetHelpcodes = Join-Path $targetAppData 'helpcodes'
    Reset-Directory -LiteralPath $targetHelpcodes
    Copy-DirectoryContents -Source $helpcodeSource -Destination $targetHelpcodes

    # U 模式的拆字表落在数据目录的 radical\ 下，路径由 engine/contracts/assets 清单声明。
    $targetRadical = Join-Path $targetAppData 'radical'
    Reset-Directory -LiteralPath $targetRadical
    Copy-Item -LiteralPath $radicalTable -Destination (Join-Path $targetRadical 'decomposition.txt') -Force
}

# 轻量模式不清空 app_data，旧版本留下的 html\（WebView2 页面）要在这里删掉，免得被打进包里。
$legacyHtml = Join-Path $targetAppData 'html'
if (Test-Path -LiteralPath $legacyHtml) {
    Remove-Item -LiteralPath $legacyHtml -Recurse -Force
}
$targetAssets = Join-Path $targetAppData 'assets'
$targetSettingsAssets = Join-Path $targetAssets 'settings'
if (Test-Path -LiteralPath $targetAssets) {
    Remove-Item -LiteralPath $targetAssets -Recurse -Force
}
Copy-DirectoryContents -Source $settingsAssets -Destination $targetSettingsAssets
Copy-DirectoryContents -Source (Join-Path $RepoRoot "$ServerDirectory/assets/dictionaries") -Destination (Join-Path $targetAssets 'dictionaries')

# 内置皮肤的出厂清单（开关与完整外观，含 canvas 基底）。安装脚本每次都整份覆盖到数据目录的
# skins\default，用户在那里的改动升级后不保留。
# 不放进 app_data：app_data 的顶层条目都在卸载时按 IsShippedAppDataItem 删除，而 skins
# 整个目录归用户（外部皮肤也在里面），不能进那份名单。
$targetDefaultSkins = Join-Path $PSScriptRoot 'default_skins'
Reset-Directory -LiteralPath $targetDefaultSkins
Copy-DirectoryContents -Source $defaultSkinsSource -Destination $targetDefaultSkins

# 仓库 skins\ 下的外部皮肤随包装到数据目录的 skins\<id>，同名的直接覆盖。覆盖前安装程序拿
# bundled_skins.manifest 核对用户机器上的同名目录：内容与随包的一模一样就不动，否则先改名成
# <id>.bak 备份（Server 扫描时跳过 .bak 目录）。清单每行是「id|相对路径|sha256」，
# 安装程序按字节比较，所以路径限定 ASCII、不能含 |。
# 素材授权没核实的皮肤（[license] assets = "UNVERIFIED..."）只能放在仓库里演示，不进安装包。
$targetBundledSkins = Join-Path $PSScriptRoot 'bundled_skins'
$bundledSkinsManifest = Join-Path $PSScriptRoot 'bundled_skins.manifest'
Reset-Directory -LiteralPath $targetBundledSkins
$manifestLines = [Collections.Generic.List[string]]::new()
$bundledSkinIds = [Collections.Generic.List[string]]::new()
foreach ($skinDir in @(Get-ChildItem -LiteralPath $bundledSkinsSource -Directory | Sort-Object Name)) {
    $id = $skinDir.Name
    if ($id -ceq 'default') { continue }
    if ($id -cnotmatch '^[a-z0-9][a-z0-9._-]{0,63}$' -or $id -match '\.bak$') {
        throw "皮肤目录名不是有效的外部皮肤 ID：$($skinDir.FullName)"
    }
    $skinManifest = Join-Path $skinDir.FullName 'skin.toml'
    Assert-PathExists -LiteralPath $skinManifest -Description "皮肤 $id 的 skin.toml"
    $skinText = [IO.File]::ReadAllText($skinManifest)
    if ($skinText -notmatch "(?m)^\s*id\s*=\s*`"$([regex]::Escape($id))`"\s*(#.*)?$") {
        throw "皮肤 $id 的 skin.toml 里 id 与目录名不一致，Server 不会加载它：$skinManifest"
    }
    if ($skinText -match '(?m)^\s*assets\s*=\s*"UNVERIFIED') {
        Write-Host "跳过皮肤 ${id}：素材授权未核实，不随安装包分发。"
        continue
    }
    $destination = Join-Path $targetBundledSkins $id
    Copy-DirectoryContents -Source $skinDir.FullName -Destination $destination
    foreach ($file in @(Get-ChildItem -LiteralPath $destination -Recurse -File -Force | Sort-Object FullName)) {
        $relative = $file.FullName.Substring($destination.Length + 1)
        if ($relative -notmatch '^[\x20-\x7E]+$' -or $relative.Contains('|')) {
            throw "皮肤文件名只能用 ASCII 且不能含 |（安装程序按清单逐字节比对）：$($file.FullName)"
        }
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $manifestLines.Add("$id|$relative|$hash")
    }
    $bundledSkinIds.Add($id)
}
if ($bundledSkinIds.Count -eq 0) {
    throw "没有可随包分发的外部皮肤：$bundledSkinsSource"
}
[IO.File]::WriteAllLines($bundledSkinsManifest, $manifestLines, [Text.UTF8Encoding]::new($false))
Write-Host "随包皮肤：$($bundledSkinIds -join ', ')"

# Server Release 输出整体复制，但测试程序及其 PDB 绝不能进入安装包。
# 带 -IncludeSymbols 时其他 PDB 保留在对应 EXE 旁边，方便安装后直接进行崩溃分析。
Reset-Directory -LiteralPath $targetServer
Copy-DirectoryContents -Source $serverRelease -Destination $targetServer
Get-ChildItem -LiteralPath $targetServer -Recurse -File -Filter '*Tests.exe' |
    Remove-Item -Force
Get-ChildItem -LiteralPath $targetServer -Recurse -File -Filter 'test_*.exe' |
    Remove-Item -Force
if ($IncludeSymbols) {
    Get-ChildItem -LiteralPath $targetServer -Recurse -File -Filter '*Tests.pdb' |
        Remove-Item -Force
    Get-ChildItem -LiteralPath $targetServer -Recurse -File -Filter 'test_*.pdb' |
        Remove-Item -Force
}
else {
    Get-ChildItem -LiteralPath $targetServer -Recurse -File -Filter '*.pdb' |
        Remove-Item -Force
}

Reset-Directory -LiteralPath $targetTsf
$targetTsf32 = Join-Path $targetTsf '32'
$targetTsf64 = Join-Path $targetTsf '64'
New-Item -ItemType Directory -Path $targetTsf32, $targetTsf64 -Force | Out-Null
Copy-Item -LiteralPath $tsf32Release -Destination $targetTsf32 -Force
Copy-Item -LiteralPath $tsf64Release -Destination $targetTsf64 -Force
if ($IncludeSymbols) {
    Copy-Item -LiteralPath $tsf32Pdb -Destination $targetTsf32 -Force
    Copy-Item -LiteralPath $tsf64Pdb -Destination $targetTsf64 -Force
}
Copy-Item -LiteralPath $appIcon -Destination (Join-Path $PSScriptRoot 'MetasequoiaIME.ico') -Force
Copy-Item -LiteralPath $tipIcon -Destination (Join-Path $PSScriptRoot 'MetasequoiaImeTip.ico') -Force
# rime-ice is GPL-3.0 and requires attribution, and its content forms the bulk of msime.db, so the
# notice has to reach the user's disk rather than only exist in the source repository.
Copy-Item -LiteralPath $thirdPartyNotices -Destination (Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.txt') -Force
# GPLv3 sections 4 and 6 require a copy of the licence to reach whoever receives the program, and the
# packaged product includes third-party GPL-3.0 dictionary data. macOS and Linux already install the
# licence text (CMakeLists.txt in MSIME-Apple and MSIME-Linux); Windows is the platform that actually
# ships at volume and was the only one omitting it. THIRD_PARTY_NOTICES.txt does not cover this: it
# points at "the LICENSE file" without carrying the GPL text itself.
Copy-Item -LiteralPath $license -Destination (Join-Path $PSScriptRoot 'LICENSE.txt') -Force

$targetIss = Join-Path $PSScriptRoot 'msime_setup.iss'
Assert-PathExists -LiteralPath $targetIss -Description '安装脚本'
$issContent = Get-Content -LiteralPath $targetIss -Raw
if ($issContent -notmatch '(?m)^#define\s+MyAppVersion\s+"[^"]+"\s*$') {
    throw '未能在安装脚本中找到 MyAppVersion。'
}
$updatedIss = [regex]::Replace(
    $issContent,
    '(?m)^#define\s+MyAppVersion\s+"[^"]+"\s*$',
    "#define MyAppVersion   `"$TargetVersion`""
)
# .gitattributes pins this repo to eol=lf; emit LF so packaging does not leave the file as a
# CRLF-only diff that git flags as modified while `git diff` shows no textual change.
$updatedIss = $updatedIss.TrimEnd("`r", "`n") + "`n"
Set-Content -LiteralPath $targetIss -Value $updatedIss -Encoding utf8NoBOM -NoNewline

$serverBinaryCount = @(Get-ChildItem -LiteralPath $targetServer -Recurse -File -Include '*.exe', '*.dll').Count
$tsfBinaryCount = @(Get-ChildItem -LiteralPath $targetTsf -Recurse -File -Include '*.exe', '*.dll').Count
$symbolCount = @(
    Get-ChildItem -LiteralPath $targetServer, $targetTsf -Recurse -File -Filter '*.pdb'
).Count
$modeLabel = if ($Light) { '轻量' } else { '完整' }
if (-not $IncludeSymbols) { $modeLabel += '，不含符号' }
Write-Host "安装文件准备完成（$modeLabel）：$PSScriptRoot"
Write-Host "版本：$TargetVersion；Server EXE/DLL：$serverBinaryCount 个；TSF EXE/DLL：$tsfBinaryCount 个；PDB：$symbolCount 个。"
