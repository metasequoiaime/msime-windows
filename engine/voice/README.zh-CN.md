# 水杉公共语音模块

VoiceInput 是引擎的一部分。公共 API、构建选项与接入方式见 [公共模块说明](README.md)。处理与录音分别由 `MetasequoiaIme::Voice`、`MetasequoiaIme::VoiceCapture` 提供，按需链接。

- 本产品的语音宿主是仓库里的 `server/`。
- 本地 Whisper 推理（`MetasequoiaIme::VoiceWhisper`）已随内嵌专门化移除，见 [UPSTREAM.md](../UPSTREAM.md)。本机离线识别改由宿主提供（`server/src/voice-input/local_asr.*`，运行时载入 sherpa-onnx，运行库与模型按需下载）；本库只带它编译所需的 sherpa-onnx C API 头文件（`third_party/sherpa-onnx/`）。
- 原独立 Windows 工具保留在 `platforms/windows/`，默认不构建；构建命令、配置和快捷键见 [独立工具说明](platforms/windows/README.md)。它使用云识别。

流式识别由 `server/` 实现，不在本模块内。`MetasequoiaIme::Voice` 提供的是有界的一次性 HTTP
识别，不提供流式传输或增量文本提交；不要指望它具备 server 的增量转写行为。把流式下沉到这里
是一次 API 和前端输入状态改造，不是简单替换 Provider。

构建命令从 **`engine/` 目录**执行。独立工具资源在 `voice/assets/`，用户配置仍位于 `%LOCALAPPDATA%\MetasequoiaVoiceInput\config.toml`。保留这个路径可继续使用已有配置。

旧环境生成器会覆盖公共 CMake 工程，现已移除；不再运行旧 `prepare_env.py` 或旧 build preset。历史脚本和提交仍保存在 Git 历史中。[旧仓库 Releases](https://github.com/metasequoiaime/MetasequoiaVoiceInput/releases) 保留已发布版本。

原项目 GPL-3.0 许可证与第三方声明继续保留，合仓不改变它们的授权。
