$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# 安装器在覆盖安装时按白名单清空数据目录，换数据目录时按另一份清单搬文件。两份清单都是
# 纯手写列表，新增用户数据文件漏改任何一处都是静默丢数据：覆盖安装直接删掉，换目录则留在
# 旧目录并随 TryDeleteTree 一起没了。Server 侧 `Store::Open` 用 SQLITE_OPEN_CREATE，被删后
# 会立刻重建一个空库，用户看到的是"统计数据被清零"而不是报错。
# 这里从 msime_setup.iss 源码反解两份清单，锁住它们的并集，也锁住 Server 侧库名。
# 仓库没有跑 Inno Setup 的测试基建（[Code] 段在安装器进程里才执行），所以只能静态校验清单；
# 见 .agents/notes/implemented/bug-fix/2026-09-28-installer-stats-db-preservation.md。

$issPath = Join-Path $PSScriptRoot '../msime_setup.iss'
$iss = [IO.File]::ReadAllText((Resolve-Path $issPath))

# 必须活过覆盖安装的用户数据。主库 + 三个 sidecar：安装器会强杀 Server，WAL 里可能还有尚未
# checkpoint 的写入，只搬主库会丢掉最后一批。
$required = @(
    'msime_user.db', 'msime_user.db-wal', 'msime_user.db-shm', 'msime_user.db-journal',
    'stats.db', 'stats.db-wal', 'stats.db-shm', 'stats.db-journal',
    'config.toml', 'config.base.toml'
)

# 目录类用户数据由 MigrateUserDataDir 单独 robocopy，迁移清单按文件解析取不到它们。
$preservedDirectories = @('skins', 'helpcodes\custom', 'shuangpin', 'models')

function Get-PascalBody([string]$Text, [string]$Signature) {
    $start = [regex]::Match($Text, [regex]::Escape($Signature) + '\s*\(')
    if (-not $start.Success) { throw "msime_setup.iss is missing: $Signature" }
    $end = [regex]::Match($Text.Substring($start.Index), '(?m)^end;\s*$')
    if (-not $end.Success) { throw "msime_setup.iss has an unterminated body: $Signature" }
    return $Text.Substring($start.Index, $end.Index)
}

function Get-PreservedItems([string]$Text) {
    # IsPreservedAppDataItem 组合若干判定函数，名单散在它们各自体内：顺着调用关系收集，
    # 免得有人把某个名字挪进新函数就被这里漏掉。
    $pending = [Collections.Generic.Stack[string]]::new()
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $items = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $pending.Push('function IsPreservedAppDataItem')
    while ($pending.Count -gt 0) {
        $name = $pending.Pop()
        if (-not $seen.Add($name)) { continue }
        $body = Get-PascalBody $Text $name
        foreach ($call in [regex]::Matches($body, '\b(Is[A-Za-z0-9]+)\s*\(\s*FileName\s*\)')) {
            $pending.Push("function $($call.Groups[1].Value)")
        }
        foreach ($literal in [regex]::Matches($body, "CompareText\(\s*FileName\s*,\s*'([^']+)'")) {
            [void]$items.Add($literal.Groups[1].Value)
        }
    }
    return $items
}

function Get-MigratedFiles([string]$Text) {
    # robocopy 的文件清单是 Pascal 字符串拼接。带双引号的是路径片段（含旧目录根、旧新子目录
    # 拼接），以 / 开头的是 robocopy 开关，没有扩展名的是日志文本——三类都剔掉，剩下的才是
    # 被搬走的文件。要求扩展名同时把 skins、helpcodes\custom 这类目录排除在外。
    $body = Get-PascalBody $Text 'function MigrateUserDataDir'
    $files = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($literal in [regex]::Matches($body, "'([^']*)'")) {
        $value = $literal.Groups[1].Value
        if ($value.Contains('"')) { continue }
        foreach ($token in ($value -split '\s+')) {
            if ($token -match '^[\w.\-]+\.[A-Za-z0-9\-]+$') { [void]$files.Add($token) }
        }
    }
    return $files
}

$preserved = Get-PreservedItems $iss
$migrated = Get-MigratedFiles $iss

if ($preserved.Count -lt $required.Count) {
    throw "Preservation list collapsed to $($preserved.Count) items; the parser is broken, not the installer"
}

foreach ($name in $required) {
    if (-not $preserved.Contains($name)) { throw "覆盖安装会删除用户数据：$name 不在 IsPreservedAppDataItem 名单里" }
    if (-not $migrated.Contains($name)) { throw "换数据目录会丢失用户数据：$name 不在 MigrateUserDataDir 的 robocopy 清单里" }
}

# 两份清单必须同步：白名单里保住的文件，换目录时也得搬走，反之亦然。这正是 stats.db 当初
# 两边都没有的原因，而两边各写一遍、只能靠人记，所以在这里交叉断言。
foreach ($name in $preserved) {
    if ($preservedDirectories -contains $name) { continue }
    if (-not $migrated.Contains($name)) { throw "清单不同步：$name 覆盖安装时保留，换数据目录时却不会被搬走" }
}
foreach ($name in $migrated) {
    if ($preservedDirectories -contains $name) { continue }
    if (-not $preserved.Contains($name)) { throw "清单不同步：$name 换数据目录时会搬走，覆盖安装时却会被删掉" }
}

# Server 侧库名是这份清单的源头。改了 stats.db 之类的名字而不同步安装器，升级又会清零一次。
# 库名在 EnsureStatsStoreOpen 里，随首词命中率改动从 stats_pipe.cpp 挪到了 stats_candidate_capture.cpp。
$statsSource = Join-Path $PSScriptRoot '../../server/src/statistics/stats_candidate_capture.cpp'
$storeName = [regex]::Match([IO.File]::ReadAllText($statsSource), 'L"([\w.-]+\.db)"').Groups[1].Value
if (-not $storeName) { throw "stats_candidate_capture.cpp no longer names a database file; update this test and the installer" }
if (-not $preserved.Contains($storeName)) { throw "统计库 $storeName 不在 IsPreservedAppDataItem 名单里，覆盖安装会清空统计数据" }
if (-not $migrated.Contains($storeName)) { throw "统计库 $storeName 不在 MigrateUserDataDir 的 robocopy 清单里，换数据目录会丢统计数据" }

Write-Host "Installer user-data preservation lists are in sync ($($preserved.Count) preserved, $($migrated.Count) migrated)"
