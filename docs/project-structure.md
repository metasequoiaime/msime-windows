# 项目结构

Windows 端的全部一方源码都在本仓，一次 clone 就能拿到完整产品。DLL 与 Server 仍是两个进程，只是不再是两个仓库。

| 目录 | 职责 |
|---|---|
| [`windows/`](../windows/) | 核心 TSF 文本服务 DLL：按键预判、焦点、edit session、与 Server 的管道协议 |
| [`server/`](../server/) | Server 端：算法调度、候选窗与工具栏宿主、设置程序、词库管理 |
| [`engine/`](../engine/) | 输入引擎：输入会话、候选查询、辅助码、跨进程契约、语音模块 |
| [`ui/`](../ui/) | 自研原生 GUI 框架（`msimeui`）：Win32 宿主窗口，Direct2D / DirectWrite 渲染 |
| [`installer/`](../installer/) | 收集各组件产物、自签名、Inno Setup 打包 |
| [`log/`](../log/) | 各模块共用的日志采集库 |
| [`experiments/tsf-edit-control/`](../experiments/tsf-edit-control/) | 基于 Win32 TSF 的编辑控件实验工程，附最小宿主 demo |
| [`vendor/`](../vendor/) | submodule：opencc 与 cpp-pinyin 两个第三方依赖 |
| `scripts/`、`tests/`、`docs/`、`product-lock.json` | 产品级构建脚本、组合验证、文档与外部输入清单 |

## 引擎在仓内

`engine/` 源自 [MSIME-Engine](https://github.com/metasequoiaime/MSIME-Engine)，现在是本仓的一等组件，和 `server/`、`windows/` 平级，并按 Windows 做了专门化。来源提交和裁剪清单见 [`engine/UPSTREAM.md`](../engine/UPSTREAM.md)。要为 Windows 改引擎就直接改，引擎和调用它的代码在同一个 PR 里评审；`product-lock.json` 也不再锁引擎，只记录构建时下载的词库。

仍在仓外的部分：

- 词库：按固定 tag 和摘要在构建时从 `dict-*` release 取用。建库流水线不在本仓，已归档的 [MSIME-Dict](https://github.com/metasequoiaime/MSIME-Dict) 只读保留，其已发布的 release 仍可用。
- 语音输入的独立工具 [MetasequoiaVoiceInput](https://github.com/metasequoiaime/MetasequoiaVoiceInput) 已归档，其已发布的 release 仍可下载；录音、WAV 编码与识别/润色协议现在在 `engine/voice/`。
