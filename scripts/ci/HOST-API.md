# Host API（Rust 账号与云同步）

设置窗口的账号功能通过 C ABI 调用 msime 仓（metasequoiaime/msime）共享的 Rust 实现
`msime-host-api`。本仓不重写会话管理、令牌刷新与同步编排，只做 JSON 进出的绑定。

## 组成

| 路径 | 内容 |
|---|---|
| `server/src/settings/native/msime_client.h` | 上游 C ABI 头文件（随构建更新，勿手改） |
| `server/src/settings/account/msime_account_client.{h,cpp}` | C++ 封装：信封构造、JSON 解析、目录解析 |
| `server/src/settings/native/native_page_account.cpp` | 账号设置页（验证码登录、状态、退出） |
| `scripts/ci/build-host-api.ps1` | 从 msime 源码构建 `msime_host_api.dll` 与导入库 |
| `scripts/ci/host-api-pin.txt` | 构建的 msime commit；写 `latest` 跟随 develop |

## CI 链路（ci.yml 的 Server job）

1. `Resolve the host-api source commit`：读 pin 文件解析出 msime commit
2. `Restore/Save the host-api build`：以 `host-api-x64-<commit>` 为 key 缓存
3. `Build the Rust host-api from msime`：浅克隆 msime、按上游 rust-toolchain.toml
   装工具链、`cargo build --locked --release --target x86_64-pc-windows-msvc -p msime-host-api`
4. `Stage ...`：把 `msime_host_api.dll(.lib)` 与 `msime_client.h` 摆进
   `server/build-release/host-api/{lib,include}`，CMake 从这两个路径取

本地没有 `host-api` 目录时链接失败，错误信息会指向本文档。

## 更新上游库

`host-api-pin.txt` 写 `latest` 时每次 CI 都取 msime develop 的 head（构建产物随 commit
缓存，只有 head 变化才重建）。想钉住就换成完整 40 位 commit。上游改了 ABI（头文件变了）
时 CI 编译会直接暴露。

## 运行时

`msime_host_api.dll` 由 Settings 的 POST_BUILD 拷到可执行文件旁，打包脚本整目录收集
`bin/Release`，随安装包分发。会话目录与水杉共享应用、输入法共用
`<数据目录>\account`，旧 Tauri 会话（`%LOCALAPPDATA%\app.msime.windows`）首次使用时
由 Rust 侧迁移。所有阻塞网络的调用走 `RunInBackground`，结果以 posted message 回
UI 线程。
