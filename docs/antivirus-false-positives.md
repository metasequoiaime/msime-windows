# 杀毒软件误报：自查与加白

终端安全产品（SentinelOne、Defender 等）把水杉输入法的组件判成恶意文件时，先按本文确认「这份文件是不是官方构建」，再决定加白还是提交误报。本文不讨论运行时行为，只讲文件级判定：静态 ML 看的是签名、版本资源、文件信誉这些二进制元数据。

## 为什么会被判成恶意文件

文件级静态检测在文件落地或执行前就给出结论，不观察进程行为。它主要看这些特征：

| 特征 | 水杉输入法的实际情况 |
|---|---|
| 版本资源 | 2026-11 之前的构建里，8 个自研 EXE 只有图标资源，`CompanyName`、`FileDescription`、`FileVersion` 等全空。空版本信息是打包器、加载器和恶意样本的常见形态，静态模型会加分。现在所有自研 EXE 都带完整 `VERSIONINFO`。 |
| 发布者签名 | 正式版用真实证书或 SignPath；nightly 用一张固定的自签名证书；dev build 每次现场生成自签名证书。自签名本身不等于恶意，但在没有导入信任的机器上，它和「无签名」在信誉系统里同样陌生。 |
| 文件信誉 | 每个版本、每次 nightly、每个 dev build 都是新的 SHA256。安全厂商的云端信誉库没有历史记录，新哈希在「未知」状态下容易被保守拦截。 |
| 出现位置与频率 | 输入法组件会写系统目录、注册 COM/TSF、随登录启动。这些是正常安装行为，但对只看静态特征的模型来说，和持久化恶意软件的表面特征接近。 |

误报是**文件级判定**，所以改剪贴板轮询之类的运行时逻辑不会降低这类拦截；能降低的是签名、版本资源、发布渠道和信誉积累。

## 三条通道的签名差异

三条通道的证书由 [`installer/README.md`](../installer/README.md#三个通道怎么选) 维护，签名策略见 [`code-signing-policy.md`](code-signing-policy.md)。

| | 正式发布 | Nightly | Dev build |
|---|---|---|---|
| 版本串 | `0.7.1` | `0.7.1-nightly.<日期>.<commit>` | `0.7.1-dev.<日期>.<commit>` |
| 签名证书 | 真实代码签名证书或 SignPath Foundation 证书 | 固定自签名证书 | 每次运行现场生成的自签名证书 |
| 证书 Subject | 随发布渠道变化，以实际文件签名为准 | `CN=Metasequoia IME Windows Nightly Build Code Signing` | `CN=Metasequoia IME Dev Build Code Signing` |
| 证书有效期 | 由证书决定 | 3 年，到期换发 | 90 天，每次构建换新 |
| 用户要不要导入信任 | 不要 | 导入一次 `nightly-signing.cer`，之后所有 nightly 通用 | 每个新构建都要导入随包的 `.cer` |
| 未导入信任时的签名状态 | `Valid` | `NotTrusted` / `UnknownError` | `NotTrusted` / `UnknownError` |
| 更新检查 | 提示升级 | 不提示 | 不提示 |

两个推论：

- **nightly / dev 显示「不受信任的发布者」是预期结果，不是文件被篡改。** 这两条通道的证书不在 Windows 受信任根里，除非用户主动导入。
- **同一台机器上的 dev 包换一次构建就换一张证书。** 按证书给 dev 包加白会很快失效，dev 通道应优先按路径加白。

## 用户自查

默认安装位置：程序在 `C:\Program Files\metasequoiaime`（`server\` 子目录放 8 个自研 EXE），TSF DLL 在 `System32\IME\MetasequoiaIME\` 与 `SysWOW64\IME\MetasequoiaIME\` 的版本目录下。以下命令在普通 PowerShell 里即可运行。

### 1. 看签名状态和签名者

```powershell
$projectPaths = @(
    'C:\Program Files\metasequoiaime',
    "$env:SystemRoot\System32\IME\MetasequoiaIME",
    "$env:SystemRoot\SysWOW64\IME\MetasequoiaIME"
)
Get-ChildItem $projectPaths -Recurse -File `
        -Include 'MetasequoiaIme*.exe', 'MetasequoiaIme*.dll' -ErrorAction SilentlyContinue |
    Get-AuthenticodeSignature |
    Select-Object Status, StatusMessage,
        @{ Name = 'Signer'; Expression = { $_.SignerCertificate.Subject } },
        Path |
    Format-Table -AutoSize -Wrap
```

`MetasequoiaIme*.exe` / `MetasequoiaIme*.dll` 只匹配本项目自己的二进制。`server\` 里同时装有 `fmt.dll`、`sqlite3.dll` 等第三方运行库，它们不由本项目签名，出现 `NotSigned` 是正常的，不在下表范围内。

`Status` 的含义：

| Status | 含义 | 该怎么处理 |
|---|---|---|
| `Valid` | 签名有效，证书链受信任，文件自签名后未被改动 | 正常 |
| `NotSigned` | 没有 Authenticode 签名 | 正式版不应出现；核对下载来源 |
| `HashMismatch` | 签名存在，但文件内容与签名时不一致 | **不要运行**，按安全事件处理 |
| `NotTrusted` / `UnknownError` | 签名存在，但证书不在本机受信任根（nightly / dev 未导入信任时） | 按通道导入 `.cer` 后复查；导入后应变成 `Valid` |
| `Expired` | 证书已过期 | nightly 证书需换发；dev 证书 90 天到期 |

只想看某个被告警的文件：

```powershell
Get-AuthenticodeSignature 'C:\Program Files\metasequoiaime\server\MetasequoiaImeEmojiPanel.exe' |
    Format-List Status, StatusMessage, @{ Name = 'Signer'; Expression = { $_.SignerCertificate.Subject } },
        @{ Name = 'Thumbprint'; Expression = { $_.SignerCertificate.Thumbprint } },
        @{ Name = 'NotAfter'; Expression = { $_.SignerCertificate.NotAfter } }
```

### 2. 看版本资源

```powershell
(Get-Item 'C:\Program Files\metasequoiaime\server\MetasequoiaImeEmojiPanel.exe').VersionInfo |
    Format-List CompanyName, FileDescription, FileVersion, ProductVersion,
        InternalName, OriginalFilename, LegalCopyright
```

8 个自研 EXE（Server、Watchdog、DictionaryReplay、DictionaryBuild、EmojiPanel、KeyboardPanel、HandwritingPanel、Settings）的预期字段：

| 字段 | 预期值 |
|---|---|
| `CompanyName` | `Metasequoia` |
| `ProductName` | `Metasequoia IME` |
| `LegalCopyright` | `Copyright (C) 2026 Metasequoia IME Team` |
| `FileDescription` | 该组件的中文/英文用途描述，非空 |
| `InternalName` / `OriginalFilename` | 目标名 / `<目标名>.exe` |
| `FileVersion` | 四位数字，如 `0.7.1.0`；dev / nightly 的末位是构建日期的 `YYMMDD`，如 `0.7.1.261104` |
| `ProductVersion` | 展示版本，如 `0.7.1`、`0.7.1-nightly.20261104.abcdef12` |

`FileVersion` 的四个数字与 TSF DLL 的 `FILEVERSION` 同源（`version.txt` / `MSIME_PRODUCT_VERSION_OVERRIDE`），所以同一次构建里 8 个 EXE 与 `MetasequoiaImeTsf.dll` 的主体版本一致。`ProductVersion` 保留 `-dev` / `-nightly` 后缀，用来区分通道。

### 3. 核对 SHA256

```powershell
Get-FileHash 'C:\Program Files\metasequoiaime\server\MetasequoiaImeEmojiPanel.exe' -Algorithm SHA256 |
    Format-List Algorithm, Hash, Path
```

正式发布的校验值写在 [Releases](https://github.com/metasequoiaime/MSIME-Windows/releases) 说明和官网 `update.json` 里；nightly 的校验值写在 `nightly` tag 的说明里。**哈希每次发版都会变**，它的用途是核对「下载到的这份是不是官方打的那份」，不是长期加白依据。

## 企业 IT 加白

### 不要按哈希加白

哈希（无论 SHA256 还是 SHA1）只在同一个二进制上有效。正式版每次发版、nightly 每次构建、dev build 每次运行都会产生新哈希，按哈希的排除项在下一个版本就失效——这也解释了为什么一个文件可能「昨天被管理员批准，今天又被拦截」。

### 按签名者 + 路径加白

在 SentinelOne 里把两类排除项配合使用：

1. **路径排除**：`C:\Program Files\metasequoiaime\`（含 `server\` 子目录）以及 `%SystemRoot%\System32\IME\MetasequoiaIME\`、`%SystemRoot%\SysWOW64\IME\MetasequoiaIME\`。范围不要放大到整个 `Program Files` 或整个盘。
2. **证书签名者身份排除（Certificate Signer Identity）**：从一份已核对来源的官方安装包上读取签名者 Subject 与 Thumbprint，只放行这一个签名者，不要放行「所有自签名证书」。

```powershell
# 从官方安装目录里取一个已签名的文件，读出签名者身份
Get-AuthenticodeSignature 'C:\Program Files\metasequoiaime\server\MetasequoiaImeServer.exe' |
    Select-Object @{ Name = 'Subject'; Expression = { $_.SignerCertificate.Subject } },
        @{ Name = 'Thumbprint'; Expression = { $_.SignerCertificate.Thumbprint } },
        @{ Name = 'NotAfter'; Expression = { $_.SignerCertificate.NotAfter } }
```

按通道的取舍：

| 通道 | 建议 |
|---|---|
| 正式发布 | 路径 + 签名者身份都合适。签名者可能随发布渠道切换（例如迁到 SignPath），换证书后需要更新排除项，以 [`code-signing-policy.md`](code-signing-policy.md) 和 Release 说明为准。 |
| Nightly | 路径 + 固定自签名证书的签名者身份。证书 3 年有效，换发时才需要更新。 |
| Dev build | 只用路径。证书每次构建都换，签名者身份排除很快失效，不值得维护。 |

### 加白前后都保留证据

加白只解决本机拦截，不解决厂商的检测规则。把被告警文件的 SHA256、签名信息、检测名称和发生时间记录下来；如果同一个版本在多台机器上被拦截，应按下一节提交误报，而不是逐台加白。

## 向 SentinelOne 提交误报

文件级误报最终要由厂商更新检测规则或信誉库。企业管理员在 SentinelOne 管理控制台对告警执行 **Report False Positive**（部分版本叫 **Mark as False Positive**），或通过支持门户提交工单；个人用户没有控制台时，把材料交给组织的安全联系人，由对方走企业渠道提交。**不要**把样本先上传到公开的多引擎扫描站点——那等于把文件公开，且不会进入 SentinelOne 的判定流程。

提交时附上这些材料，能让检测团队直接定位到版本资源与签名：

| 材料 | 怎么取 |
|---|---|
| 文件名与完整路径 | 告警详情里的路径 |
| SHA256 | `Get-FileHash <文件> -Algorithm SHA256` |
| `FileVersion` 与 `ProductVersion` | `(Get-Item <文件>).VersionInfo` 的对应字段 |
| 签名者 Subject / Thumbprint / 有效期 | `Get-AuthenticodeSignature <文件>` 的 `SignerCertificate` |
| 检测信息 | 告警里的威胁名、检测引擎（Static AI / ML）、检测时间、控制台告警 ID |
| 下载来源 | 对应的 GitHub Release / `nightly` tag / Actions run 的 URL，或企业分发包的部署记录 |
| 版本与通道 | 正式版 / nightly / dev，以及版本串 |
| 误报理由 | 官方构建、签名与版本资源完整、SHA256 与发布页一致、公司已按路径或签名者加白后仍被云端判定 |
| 复现方式 | 安装包版本、安装路径、触发拦截的操作 |

提交后按厂商回复更新排除项或升级客户端签名库，再用同一份文件复测。厂商修正的是服务端规则，不会改动本地文件。

如果你是安全问题报告者，而不是误报处理者——例如发现签名文件与官方构建不符——请按组织的 [安全策略](https://github.com/metasequoiaime/.github/blob/main/SECURITY.md) 私下报告，不要开公开 issue。
