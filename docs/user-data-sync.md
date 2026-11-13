# 用户数据同步

把配置、用户词条和输入统计备份到用户自己的 WebDAV 或 S3 兼容存储。默认关闭，开启后数据只发往用户填写的地址，不经过官方服务器。

## 为什么要分「备份」和「同步」

设置 → 关于里的「导出用户数据」是一个完整的 ZIP 快照，导出时输入法继续运行，导入时先整包校验再停 Server。它解决的是换机迁移和手动留底，每次都是整体覆盖。

同步面对的问题不一样：两台机器都在用，各自都在写。如果还是整包覆盖，后同步的那台会把另一台的词库、统计和配置全部冲掉。所以同步需要按数据域定合并规则，而不是搬运文件。

## 数据域与合并规则

| 数据 | 位置 | 规则 |
|---|---|---|
| 用户词条 | `msime_user.db` 的 `user_dictionary_operations` | 按 `(dictionary,key,value)` 取 `updated_at` 较新的一条 |
| 删除墓碑 | 同上，`operation='delete'` | 保留。不保留就会把用户删掉的词从别的设备复活 |
| 亲手定的权重 | 同上，`weight_edited=1` | 跟着记录一起走，换词库时不被降级成自动调频 |
| 手动加的词 | 同上，`user_inserted=1` | 粘性标记，合并取两边的较大值，远端较新的自动调频行不能把它降级 |
| 字母词 | `user_latin_words` | 没有墓碑，按并集合并，两边都有的取较新的；本机删掉、远端还在的会在恢复后回来 |
| 词库量级 | `dictionary_state` 的 `replayed_dictionary_sha256` | 远端日志按另一份出厂词库记的，就清掉本机这一行，下次回放走 rebase |
| 配置 | `config.toml` | 只采纳远端用户真正改过的键，出厂默认值不覆盖本地 |
| 设备本地配置 | `[sync]`、`[network]` | 不合并。各机常用各自的 IAM 密钥或 WebDAV 账号，代理也随所在网络而定 |
| 凭据 | `config.toml` 的 token / secret | 永不参与同步，上传前剥离；恢复时保留本机值 |
| 统计 | `stats.db` | 同步恢复不动它，见下文；普通导入仍整份恢复 |
| 皮肤 | `skins/` | 随快照整包传输 |
| 模型 | `models/` | 默认不带，勾选后才进归档 |

调频状态表（`fixed_candidate_positions`、`quick_phrase_slots`、`english_slots`、`date_time_format_order`）是每台设备自己的使用习惯，跨机合并没有确定语义，这一版不动它们。

## 远端布局

```
<根>/devices/<设备标识>.json       设备元信息：主机名、最近快照、上传时间、大小
<根>/devices/<设备标识>/<revision>.zip  快照，保留最近 N 份
```

每台设备只写自己那一个命名空间，多设备并发不会互相覆盖。读取端先列举 `devices` 拿到全部设备，再决定拉哪一份。设备标识存在数据目录的 `sync_state.json`，跨升级保留；它进了安装器的保留清单，覆盖安装和换目录都不会丢。

`revision` 是 `<秒级时间戳>-<进程号>`。清理旧快照时按 (时间戳, 进程号) 数值比先后，不按字符串比（进程号位数不同时字符串序是错的），保留最近 N 份；刚上传、设备信息正指向的那一份无条件保留，系统时钟曾经超前过也不会被删。设备信息指向的快照不存在时，恢复退回到这台设备目录下实际存在的最新一份。

## 传输层

`server/src/settings/remote_store.h` 定义 `RemoteStore`：`Put`、`Get`、`Stat`、`List`、`Delete`、`EnsureDirectory`、`Probe`。两个实现：

- **WebDAV**：`PROPFIND` 列举，`PUT` 上传，`GET` 下载，`HEAD` 取大小，`DELETE` 删除，`MKCOL` 逐级建集合（对不存在集合的 PUT 会 409）。Basic 认证。
- **S3 兼容**：自己实现 SigV4，不引入 aws-sdk-cpp。canonical request 两百行，而 SDK 会带来整套凭证链、CMake 集成和平台依赖。哈希与 HMAC 用 BCrypt，和 `cloud/tencent_tmt.cpp` 一致。支持路径风格与虚拟主机风格，MinIO、R2、B2、OSS 都能用。

两条实现都走 `utils/network_proxy.cpp` 的代理配置，和云输入、模型下载共用同一套开关。TLS 校验始终开启，不提供跳过校验的开关：自签证书的服务端应当把证书导入系统信任库。

上传用 libcurl 的 `CURLOPT_UPLOAD` 流式发送，下载写到本地临时文件，不会把几百 MB 的归档整个读进内存。上传总是带上 `Content-Length`（空文件也是 0），不让 curl 退回 chunked 编码。传输没走完（断网、磁盘写满）一律判失败，不看响应头里的 200。归档传输不设总时长上限，改为连续 60 秒低于 1 KB/s 才算卡死。

WebDAV 的 base 地址先解码再统一编码一次，服务端回的 href 也先解码再比，路径里有中文（坚果云默认目录「我的坚果云」）时两边才对得上。S3 请求不跟随重定向：签名绑定了 host，跳过去必然验签失败；3xx 会提示区域不对，并带上服务端给的 `x-amz-bucket-region`。S3 的 `List` 带 `delimiter=/`，与 WebDAV 一样只列一层。

## 凭据

同步用的密码与密钥写进 `config.toml` 的 `[sync]` 段，按 `IsCredentialKey` 走 DPAPI 当前用户范围加密，和现有 token 同一条链路。

`webdav_password` 与 `s3_secret_key` 在凭据名单里；`s3_access_key` 不在，它是标识符不是密钥，S3 的签名里它明文参与 canonical request，加密存储只会让设置页读不出来。

归档上传前，`config.toml` 会先过一遍 `StripCredentials`，把凭据键的值清空。远端永远不会出现 DPAPI 密文：那密文跨机本来就用不了，留着只是多一份泄露面。

## 使用

设置 → 数据同步：

1. 选择 WebDAV 或 S3 兼容，填地址与凭据。
2. 点「测试连接」，确认目录存在、凭据可用。
3. 打开「启用同步」。
4. 「立即上传」把当前数据打包传上去。
5. 换台电脑，同样配好后点「刷新设备」，在列表里点「恢复…」。

本机那一行也可以恢复：设备标识跨重装保留，本机数据坏了时云端最近的就是它。恢复是合并而不是覆盖，从本机的旧快照恢复只会把丢掉的东西补回来。

恢复会先停 Server 和 Watchdog，把将被覆盖的文件与会被 replay 改动的 `msime.db`、`english.db` 一起备份到 `.backup-before-import`，落位后再把 Watchdog 拉起来。词库按日志合并并回放，不会整份替换现有数据库。

## 这一版没做的

- **统计合并**：`stats.db` 按设备分账需要给表加 `device` 维度并做 schema 2→3 迁移。这一版快照里仍然带着 `stats.db`，但同步恢复不落位它（整份替换就是拿别的设备的统计抹掉本机的），只有普通导入会恢复统计。真正的按设备汇总留给单独一个 PR，连同迁移测试一起评审。
- **自动上传**：`sync.auto_upload_minutes` 已经写进配置，但后台轮询还没接，设置页也暂不露出这一项，免得用户以为有了定期备份。上传需要构建归档，而归档构建目前在设置进程里；要放进常驻的 Server，得先把 `user_data_backup.cpp` 和 `zip_archive.cpp` 挪到一个共用的静态库。设置窗口一关就停的上传不是自动上传。
- **配置三路合并**：现在是「远端改过的键覆盖本地」，没有共同祖先基线。两台机器同时改同一个键时，后同步的一方胜出，没有冲突提示。
- **归档加密**：归档里有个人词条和统计。放在公共桶或共享盘时值得加一层口令加密，BCrypt 有 `BCryptDeriveKeyPBKDF2` 和 AES-GCM，信封头存 salt、迭代次数和 nonce，约一百行。

## 验证

`server/tests/src/test_remote_store.cpp`、`test_sync_merge.cpp`、`test_sync_service.cpp` 与 `test_user_data_backup.cpp` 覆盖：

- SigV4 用 AWS 官方文档里的两个测试向量，离线验证签名逐字节正确
- URI 编码、查询串规范化、URL 拆分（含 IPv6 与端口）
- 设备与快照 key 的往返，以及拒绝 `..` 与多层路径
- 设备标识的生成与稳定性
- 内存假 store 驱动的上传与列举
- 凭据剥离、配置合并（远端默认值不覆盖、凭据不导入、非法 TOML 原样返回）
- 词库合并的较新者胜出、删除墓碑保留、本地较新时不被覆盖、`user_inserted` 不被降级、字母词并集合并、词库不同时强制 rebase
- 配置合并跳过 `[sync]` 与 `[network]`；同步恢复不替换 `stats.db`
- WebDAV href 两边解码后比对、XML 实体单遍解码、百分号解码
- 快照清理保留刚上传的那份、按数值比 revision 先后

真实 WebDAV 与 S3 服务端没有自动化测试，需要手工验证：改完传输层后，用坚果云或 MinIO 跑一遍上传、列举、恢复。
