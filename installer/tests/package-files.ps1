$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('msime-package-' + [Guid]::NewGuid())
function Write-Fixture([string]$Relative, [string]$Text = 'fixture') {
    $path = Join-Path $fixture $Relative
    New-Item -ItemType Directory -Force -Path (Split-Path $path) | Out-Null
    [IO.File]::WriteAllText($path, $Text)
}
try {
    $installer = Join-Path $fixture 'installer'
    New-Item -ItemType Directory -Force -Path $installer | Out-Null
    Copy-Item (Join-Path $PSScriptRoot '../Prepare-PackageFiles.ps1') $installer
    Copy-Item (Join-Path $PSScriptRoot '../msime_setup.iss') $installer
    Copy-Item (Join-Path $PSScriptRoot '../default_config') $installer -Recurse
    foreach ($file in @(
        'server/build-release/bin/Release/MetasequoiaImeServer.exe',
        'server/build-release/bin/Release/MetasequoiaImeServer.pdb',
        'server/build-release/bin/Release/MetasequoiaImeDictionaryReplay.exe',
        'server/build-release/bin/Release/MetasequoiaImeDictionaryReplay.pdb',
        'server/build-release/bin/Release/MetasequoiaImeDictionaryBuild.exe',
        'server/build-release/bin/Release/MetasequoiaImeDictionaryBuild.pdb',
        'server/build-release/bin/Release/MetasequoiaImeServerTests.exe',
        'server/build-release/bin/Release/MetasequoiaImeServerTests.pdb',
        'server/build-release/bin/Release/test_webview_contract.exe',
        'server/build-release/bin/Release/test_webview_contract.pdb',
        'windows/build32-release/Release/MetasequoiaImeTsf.dll',
        'windows/build32-release/Release/MetasequoiaImeTsf.pdb',
        'windows/build64-release/Release/MetasequoiaImeTsf.dll',
        'windows/build64-release/Release/MetasequoiaImeTsf.pdb',
        'windows/buildarm64x-release/Release/MetasequoiaImeTsf.dll',
        'windows/buildarm64x-release/Release/MetasequoiaImeTsf.pdb',
        'THIRD_PARTY_NOTICES.txt',
        'LICENSE',
        'server/assets/config/config.toml',
        'server/src/resource/MetasequoiaIME.ico',
        'windows/image/msime.ico',
        'engine/helpcode/helpcodes/helpcode.txt',
        'engine/radical/decomposition.txt',
        'engine/googlepinyinime-rev/data/dict_pinyin.dat',
        'MetasequoiaImeDict/source/mozc_dictionary_oss/README.txt',
        # 仓库里有 sc.lm 也不能进安装包，见下面的断言。
        'language-model/sc.lm',
        'language-model/NOTICE.md',
        'neural-model/sentence-model-desktop.safetensors',
        'neural-model/sentence-model.safetensors',
        'neural-model/NOTICE.md',
        'server/assets/settings/arrow.svg',
        'server/assets/settings/sidebar/about.svg',
        'skins/default/fluent/skin.toml',
        'skins/default/canvas/skin.toml'
    )) { Write-Fixture $file }
    Write-Fixture 'skins/demo-skin/skin.toml' "schema_version = 1`nid = `"demo-skin`"`n"
    Write-Fixture 'skins/demo-skin/assets/background.png' 'png'
    Write-Fixture 'skins/unverified/skin.toml' "schema_version = 1`nid = `"unverified`"`n[license]`nassets = `"UNVERIFIED-DEMO-ONLY`"`n"
    Write-Fixture 'server/assets/tables/pinyin.txt' 'xing'
    Write-Fixture 'MetasequoiaImeDict/out/dictionary-manifest.json' '{"manifest_version":1}'
    # 词库以文本源进包，导出脚本要读真正的 SQLite 文件和 MSJPDT1 模型，所以这几个夹具得是真的。
    Copy-Item (Join-Path $PSScriptRoot '../../scripts/pack-dictionary-source.py') (New-Item -ItemType Directory -Force -Path (Join-Path $fixture 'scripts'))
    python -c @'
import sqlite3, struct, sys
from pathlib import Path
out = Path(sys.argv[1])
with sqlite3.connect(out / "english.db") as db:
    db.execute("CREATE TABLE english_words(word TEXT,display TEXT,weight INTEGER,PRIMARY KEY(word,display))")
with sqlite3.connect(out / "msime.db") as db:
    db.execute("CREATE TABLE tbl_1_n(key TEXT, jp TEXT, value TEXT, weight INTEGER DEFAULT 0)")
    db.execute("INSERT INTO tbl_1_n VALUES ('ni', 'n', '你', 100)")
with sqlite3.connect(out / "others.db") as db:
    db.execute("CREATE TABLE emoji(emoji TEXT NOT NULL, category TEXT NOT NULL)")
strings = "あ亜".encode()
(out / "dict_japanese.dat").write_bytes(b"MSJPDT1\0" + struct.pack("<IIII", 1, 1, 1, 0)
    + struct.pack("<QQQQ", 56, 76, 78, len(strings)) + struct.pack("<IHIHHHi", 0, 3, 3, 3, 0, 0, 10)
    + struct.pack("<h", 0) + strings)
'@ (Join-Path $fixture 'MetasequoiaImeDict/out')
    if ($LASTEXITCODE -ne 0) { throw 'Failed to create packaging fixture' }
    & (Join-Path $installer 'Prepare-PackageFiles.ps1') -RepoRoot $fixture -TargetVersion '2026.9.1' -IncludeSymbols
    foreach ($file in @('app_data/assets/settings/arrow.svg', 'app_data/assets/settings/sidebar/about.svg',
                         'app_data/dictionary-manifest.json',
                         'app_data/dict_pinyin.dat',
                         'app_data/sentence-model.safetensors', 'app_data/chinese-ime-lm-NOTICE.md',
                         'tsf_dll/32/MetasequoiaImeTsf.dll', 'tsf_dll/32/MetasequoiaImeTsf.pdb',
                         'tsf_dll/64/MetasequoiaImeTsf.dll', 'tsf_dll/64/MetasequoiaImeTsf.pdb',
                         'tsf_dll/arm64/MetasequoiaImeTsf.dll', 'tsf_dll/arm64/MetasequoiaImeTsf.pdb',
                         'server_exe/MetasequoiaImeServer.pdb',
                         'server_exe/MetasequoiaImeDictionaryReplay.pdb',
                         'app_data/helpcodes/helpcode.txt', 'app_data/radical/decomposition.txt',
                         'default_skins/fluent/skin.toml',
                         'THIRD_PARTY_NOTICES.txt', 'LICENSE.txt', 'MetasequoiaImeTip.ico')) {
        if (-not (Test-Path (Join-Path $installer $file))) { throw "Missing packaged file: $file" }
    }
    # 词库以文本源进包，安装时由 MetasequoiaImeDictionaryBuild.exe 还原；SQLite 文件本身不能再进 app_data。
    foreach ($file in @('dictionary_source/msime.dump', 'dictionary_source/english.dump', 'dictionary_source/others.dump',
                         'dictionary_source/dict_japanese.tokens', 'dictionary_source/dict_japanese.connection',
                         'server_exe/MetasequoiaImeDictionaryBuild.exe')) {
        if (-not (Test-Path (Join-Path $installer $file))) { throw "Missing packaged file: $file" }
    }
    foreach ($file in @('msime.db', 'english.db', 'others.db', 'dict_japanese.dat')) {
        if (Test-Path (Join-Path $installer "app_data/$file")) { throw "Packaged the built dictionary file $file instead of its text source" }
    }
    $issForBuild = [IO.File]::ReadAllText((Join-Path $installer 'msime_setup.iss'))
    if ($issForBuild -notmatch 'Source: "\{#MySourceRoot\}\\dictionary_source\\\*";\s*DestDir: "\{tmp\}\\dictionary_source"') {
        throw 'dictionary_source must be extracted to {tmp}\dictionary_source'
    }
    # 回放往新词库里写，必须排在还原之后。
    if ($issForBuild -notmatch '(?s)BuildDictionaries;\s*ReplayUserDictionary;') {
        throw 'BuildDictionaries must run right before ReplayUserDictionary in ssPostInstall'
    }
    # 语言配置图标必须在 TSF DLL 的 regserver 之前装好：DLL 注册时才看得到它，否则又会把图标
    # 注册成随版本目录变化的 DLL 路径。32 位、64 位 DLL 各自找自己的上一级目录，两份都要有。
    $issForIcon = [IO.File]::ReadAllText((Join-Path $installer 'msime_setup.iss'))
    $dllEntry = $issForIcon.IndexOf('Source: "{#MySourceRoot}\tsf_dll\')
    foreach ($root in @('{#MyTsfRoot32}', '{#MyTsfRoot64}')) {
        $iconEntry = [regex]::Match($issForIcon,
            'Source: "\{#MySourceRoot\}\\MetasequoiaImeTip\.ico";\s*\\\s*DestDir: "' + [regex]::Escape($root) + '";')
        if (-not $iconEntry.Success) { throw "MetasequoiaImeTip.ico is not installed to $root" }
        if ($dllEntry -lt 0 -or $iconEntry.Index -gt $dllEntry) {
            throw "MetasequoiaImeTip.ico for $root must be installed before the TSF DLL entries"
        }
    }
    # CS2 等 Trusted Mode 游戏只放行系统目录下的签名 DLL，TSF DLL 装回 Program Files 就进不了游戏。
    foreach ($arch in @('32', '64')) {
        if ($issForIcon -notmatch ('Source: "\{#MySourceRoot\}\\tsf_dll\\' + $arch + '\\\*\.dll";\s*\\\s*DestDir: "\{#MyTsfRoot' + $arch + '\}\\')) {
            throw "The $arch-bit TSF DLL must be installed under MyTsfRoot$arch"
        }
    }
    # 64 位视图里只能有一个文件：Arm64 宿主与 x64 仿真宿主读的是同一份 InProcServer32。
    # 所以两份 64 位 DLL 必须按架构互斥，而 Arm64X 必须落在 MyTsfRoot64。
    if ($issForIcon -notmatch ('Source: "\{#MySourceRoot\}\\tsf_dll\\64\\\*\.dll";\s*\\\s*DestDir: "\{#MyTsfRoot64\}\\\{code:GetVersionDir\}";\s*\\\s*Flags: ignoreversion regserver; Check: not IsArm64')) {
        throw 'The x64 TSF DLL must be skipped on Arm64, which registers the Arm64X file instead'
    }
    if ($issForIcon -notmatch ('Source: "\{#MySourceRoot\}\\tsf_dll\\arm64\\\*\.dll";\s*\\\s*DestDir: "\{#MyTsfRoot64\}\\\{code:GetVersionDir\}";\s*\\\s*Flags: ignoreversion regserver; Check: IsArm64')) {
        throw 'The Arm64X TSF DLL must be installed into MyTsfRoot64 on Arm64'
    }
    if ($issForIcon -notmatch '#define MyTsfRoot64\s+"\{sys\}\\IME\\' -or $issForIcon -notmatch '#define MyTsfRoot32\s+"\{syswow64\}\\IME\\') {
        throw 'MyTsfRoot32/MyTsfRoot64 must point into the system directories'
    }
    # 词格语言模型 sc.lm 不随包发布（约 24 MB，几乎压不动），本地生成过也不能进安装包。
    foreach ($file in @('app_data/sc.lm', 'app_data/libime-lm-NOTICE.md')) {
        if (Test-Path (Join-Path $installer $file)) { throw "Packaged the retired language model file $file" }
    }
    # 神经整句效果档模型由设置页按需下载，仓库的 neural-model/ 里有它也不能进安装包。
    if (Test-Path (Join-Path $installer 'app_data/sentence-model-desktop.safetensors')) {
        throw 'The on-demand desktop neural model was packaged'
    }
    # 内置皮肤清单落在数据目录的 skins 下，那是用户目录（外部皮肤也在里面），卸载时不能删；
    # 它若经 app_data 暂存，就得进 IsShippedAppDataItem 名单，卸载会把整个 skins 带走。
    if (Test-Path (Join-Path $installer 'app_data/skins')) { throw 'Default skin settings were staged under app_data' }
    $issText = [IO.File]::ReadAllText((Join-Path $installer 'msime_setup.iss'))
    # 内置皮肤清单承载出厂外观，每次安装都要整份覆盖，否则老用户永远拿不到外观修正。
    if ($issText -notmatch '(?s)Source: "\{#MySourceRoot\}\\default_skins\\\*";[^\r\n]*\\\s*DestDir: "\{code:GetDataDir\}\\skins\\default";[^\r\n]*\\\s*Flags: ignoreversion') {
        throw 'Default skin manifests must be installed to skins\default with ignoreversion (overwritten on every install)'
    }
    if (-not (Test-Path (Join-Path $installer 'default_skins/canvas/skin.toml'))) { throw 'Missing packaged file: default_skins/canvas/skin.toml' }
    # 仓库 skins\ 下的外部皮肤随包分发；素材授权未核实的不进包，skins\default 走上面那条。
    foreach ($file in @('bundled_skins/demo-skin/skin.toml', 'bundled_skins/demo-skin/assets/background.png')) {
        if (-not (Test-Path (Join-Path $installer $file))) { throw "Missing bundled skin file: $file" }
    }
    foreach ($excluded in @('bundled_skins/unverified', 'bundled_skins/default')) {
        if (Test-Path (Join-Path $installer $excluded)) { throw "Bundled a skin that must not ship: $excluded" }
    }
    $expectedManifest = @(
        "demo-skin|assets\background.png|$((Get-FileHash (Join-Path $fixture 'skins/demo-skin/assets/background.png') -Algorithm SHA256).Hash.ToLowerInvariant())",
        "demo-skin|skin.toml|$((Get-FileHash (Join-Path $fixture 'skins/demo-skin/skin.toml') -Algorithm SHA256).Hash.ToLowerInvariant())"
    )
    $actualManifest = @(Get-Content -LiteralPath (Join-Path $installer 'bundled_skins.manifest'))
    if (($actualManifest -join "`n") -cne ($expectedManifest -join "`n")) {
        throw "Unexpected bundled skin manifest:`n$($actualManifest -join "`n")"
    }
    if ($issText -notmatch '(?s)Source: "\{#MySourceRoot\}\\bundled_skins\\\*";[^\r\n]*\\\s*DestDir: "\{code:GetDataDir\}\\skins";[^\r\n]*\\\s*Flags: ignoreversion') {
        throw 'Bundled skins must overwrite skins\<id> in the data directory'
    }
    # 安装期间占住的 Server 启动锁必须和 TSF DLL 用的是同一个名字，否则 DLL 照样能在词库生成前拉起 Server，
    # 引擎建出空的 msime.db 并占着，生成程序替换时拒绝访问。
    $dllSource = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '../../windows/src/IME/MetasequoiaIME.cpp'))
    $dllMutex = [regex]::Match($dllSource, 'SERVER_LAUNCH_MUTEX_NAME\[\]\s*=\s*L"([^"]+)"').Groups[1].Value.Replace('\\', '\')
    $issMutex = [regex]::Match($issText, '(?m)^#define\s+MyServerLaunchMutex\s+"([^"]+)"').Groups[1].Value
    if (-not $dllMutex -or $dllMutex -cne $issMutex) { throw "Installer launch mutex '$issMutex' does not match the TSF DLL's '$dllMutex'" }
    if ($issText -notmatch "(?s)CreateMutex\('\{#MyServerLaunchMutex\}'\);\s*StopImeProcesses;") {
        throw 'PrepareToInstall must take the Server launch mutex before stopping IME processes'
    }
    # 数据目录不整个归输入法时（旧版安装器把标记写进了用户原有的文件夹，#537），卸载只按
    # IsShippedAppDataItem 等名单删。包里新增了顶层条目而名单没跟上，卸载就会把它漏在用户目录里。
    $iss = [IO.File]::ReadAllText((Join-Path $installer 'msime_setup.iss'))
    $shippedBody = [regex]::Match($iss, '(?s)function IsShippedAppDataItem\(.*?\r?\nend;').Value
    if (-not $shippedBody) { throw 'msime_setup.iss is missing IsShippedAppDataItem' }
    $shipped = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($literal in [regex]::Matches($shippedBody, "CompareText\(\s*FileName\s*,\s*'([^']+)'")) {
        [void]$shipped.Add($literal.Groups[1].Value)
    }
    foreach ($entry in Get-ChildItem -LiteralPath (Join-Path $installer 'app_data') -Force) {
        if (-not $shipped.Contains($entry.Name)) {
            throw "app_data\$($entry.Name) 不在 msime_setup.iss 的 IsShippedAppDataItem 名单里"
        }
    }
    # user_dict.dat 是 role=user 的可写文件，清单里 profiles 为空。装进资源目录会把用户
    # 自造词顶掉，而且每次升级顶一次，所以它必须由引擎在用户数据目录下自建。
    if (Test-Path (Join-Path $installer 'app_data/user_dict.dat')) { throw 'Packaged the writable user dictionary' }
    foreach ($testFile in @(
        'server_exe/MetasequoiaImeServerTests.exe',
        'server_exe/MetasequoiaImeServerTests.pdb',
        'server_exe/test_webview_contract.exe',
        'server_exe/test_webview_contract.pdb'
    )) {
        if (Test-Path (Join-Path $installer $testFile)) { throw "Packaged a test file: $testFile" }
    }
    # 默认不带符号：PDB 是安装包体积和打包耗时的大头，只有显式 -IncludeSymbols 才进包。
    & (Join-Path $installer 'Prepare-PackageFiles.ps1') -RepoRoot $fixture -TargetVersion '2026.9.1'
    foreach ($file in @('app_data/assets/settings/sidebar/about.svg',
                         'tsf_dll/32/MetasequoiaImeTsf.dll', 'tsf_dll/64/MetasequoiaImeTsf.dll',
                         'tsf_dll/arm64/MetasequoiaImeTsf.dll',
                         'server_exe/MetasequoiaImeServer.exe')) {
        if (-not (Test-Path (Join-Path $installer $file))) { throw "Missing packaged file: $file" }
    }
    $stagedSymbols = @(
        Get-ChildItem -LiteralPath (Join-Path $installer 'server_exe'), (Join-Path $installer 'tsf_dll') `
            -Recurse -File -Filter '*.pdb'
    )
    if ($stagedSymbols.Count -gt 0) {
        throw "Packaged symbols without -IncludeSymbols: $($stagedSymbols.Name -join ', ')"
    }
    $serverPdbFixture = Join-Path $fixture 'server/build-release/bin/Release/MetasequoiaImeServer.pdb'
    Remove-Item $serverPdbFixture -Force
    $rejected = $false
    try { & (Join-Path $installer 'Prepare-PackageFiles.ps1') -RepoRoot $fixture } catch { $rejected = $_.Exception.Message -match 'PDB' }
    if (-not $rejected) { throw 'Missing production PDB was accepted' }
    [IO.File]::WriteAllText($serverPdbFixture, 'fixture')
    $database = Join-Path $installer 'app_data/msime.db'
    [IO.File]::WriteAllText($database, 'preserved user data')
    # 轻量包不清空 app_data：旧版本留下的 html\（WebView2 页面）必须被清掉，否则会被打进包里。
    $legacyHtml = Join-Path $installer 'app_data/html/webview2/shared/runtime.js'
    New-Item -ItemType Directory -Force -Path (Split-Path $legacyHtml) | Out-Null
    [IO.File]::WriteAllText($legacyHtml, 'stale')
    & (Join-Path $installer 'Prepare-PackageFiles.ps1') -RepoRoot $fixture -TsfDirectory windows -ServerDirectory server -NoticesDirectory . -Light
    if ([IO.File]::ReadAllText($database) -ne 'preserved user data') { throw 'Light package replaced dictionary data' }
    if (-not (Test-Path (Join-Path $installer 'app_data/assets/settings/sidebar/about.svg'))) { throw 'Light package lost settings assets' }
    if (Test-Path (Join-Path $installer 'app_data/html')) { throw 'Light package kept the retired html directory' }
    # dict_pinyin.dat 是静默失败：缺了它 im_open_decoder 返回 false，Google 那条 Fallback 整句
    # 候选悄无声息地消失。它长期没被打包进去，测试却是绿的，因为测试用的是
    # server/assets/tables 那份开发副本 —— 所以这条断言要盯的是安装包，不是数据目录。
    $pinyinModelFixture = Join-Path $fixture 'engine/googlepinyinime-rev/data/dict_pinyin.dat'
    Remove-Item $pinyinModelFixture -Force
    $rejected = $false
    try { & (Join-Path $installer 'Prepare-PackageFiles.ps1') -RepoRoot $fixture } catch { $rejected = $_.Exception.Message -match 'dict_pinyin\.dat' }
    if (-not $rejected) { throw 'Missing pinyin decoder model was accepted' }
    [IO.File]::WriteAllText($pinyinModelFixture, 'fixture')
    # 随包的神经整句速度档模型同理：缺文件时 shared_sentence_model 返回 nullptr，设置页开关能开但不出候选。
    $neuralModelFixture = Join-Path $fixture 'neural-model/sentence-model.safetensors'
    Remove-Item $neuralModelFixture -Force
    $rejected = $false
    try { & (Join-Path $installer 'Prepare-PackageFiles.ps1') -RepoRoot $fixture } catch { $rejected = $_.Exception.Message -match 'safetensors' }
    if (-not $rejected) { throw 'Missing neural sentence model was accepted' }
    [IO.File]::WriteAllText($neuralModelFixture, 'fixture')
    Remove-Item (Join-Path $fixture 'server/assets/settings') -Recurse -Force
    $rejected = $false
    try { & (Join-Path $installer 'Prepare-PackageFiles.ps1') -RepoRoot $fixture -TsfDirectory windows -ServerDirectory server -NoticesDirectory . } catch { $rejected = $_.Exception.Message -match '设置窗口图片资源' }
    if (-not $rejected) { throw 'Missing settings assets were accepted' }
    if ([IO.File]::ReadAllText($database) -ne 'preserved user data') { throw 'Rejected package damaged previous staging' }
    Write-Host 'Full/light package contracts, provenance, exclusions and failure staging passed'
} finally {
    if (Test-Path $fixture) { Remove-Item $fixture -Recurse -Force }
}
