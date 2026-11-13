#pragma once

// 本地语音识别（离线 ASR）的内置目录与磁盘布局。设置进程按它下载、校验、解压，Server 按它找文件
// 加载模型，两边共用这一份，不另设清单文件。条目与 ../msime 的 resources/local-asr-models.json、
// 运行库与 resources/voice-runtime.lock.json 对齐（sherpa-onnx 1.13.8）。
//
// 全部按需下载，安装包里一样都不带：
//   <DataDir>/models/voice-asr/runtime/  sherpa-onnx C API 与 onnxruntime 三个 DLL（各模型共用）
//   <DataDir>/models/voice-asr/vad/      silero_vad.onnx（整句模型切段用，各模型共用）
//   <DataDir>/models/voice-asr/<id>/     模型文件，去掉压缩包顶层目录
// 每个组件的目录里最后写入 kInstalledMarker，有它才算装好；落位都在校验之后，所以「标记在 = 曾通过校验」。
// models/ 整目录在安装器里是「升级保留、卸载删除」。

#include "engine/core/data_path.h"

#include <filesystem>
#include <string>
#include <vector>

namespace local_asr
{

inline constexpr char kInstalledMarker[] = ".installed";

// 一份钉死的远端文件。host/path 是 HTTPS 直链（GitHub release 资产经 302 跳到 CDN）。
struct RemoteFile
{
    const char *name; // 文件名，用于导入时提示与 NOTICE
    const wchar_t *host;
    const wchar_t *path;
    long long size;
    const char *sha256;
};

enum class ModelKind
{
    OnlineTransducer,  // 流式 zipformer transducer
    OfflineTransducer, // 整段 zipformer transducer，经 VAD 切段
    OfflineSenseVoice, // SenseVoice，经 VAD 切段
    OfflineFunAsrNano, // Fun-ASR-Nano（LLM 解码），经 VAD 切段
};

// 模型目录里的一项：sherpa 配置的键与相对路径。tokenizer 是目录，其余是文件。
struct ModelFileEntry
{
    const char *key;
    const char *path;
};

struct ModelEntry
{
    const char *id;
    const char *title;
    const char *description;
    ModelKind kind;
    const char *model_type; // 交给 sherpa 的 model_type，可为空串
    RemoteFile archive;     // tar.bz2
    const char *root;       // 压缩包顶层目录
    std::vector<ModelFileEntry> files;
    const char *size_hint;
    const char *memory_hint;
    const char *license;
    const char *notice;
};

struct RuntimeLibrary
{
    const char *name;
    long long size;
    const char *sha256;
};

// 每个整句模型都要 VAD；流式模型不用。
inline bool NeedsVad(const ModelEntry &model)
{
    return model.kind != ModelKind::OnlineTransducer;
}

inline const RemoteFile &RuntimeArchive()
{
    static const RemoteFile archive{
        "sherpa-onnx-v1.13.8-win-x64-shared-MD-Release-no-tts-lib.tar.bz2",
        L"github.com",
        L"/k2-fsa/sherpa-onnx/releases/download/v1.13.8/"
        L"sherpa-onnx-v1.13.8-win-x64-shared-MD-Release-no-tts-lib.tar.bz2",
        6907798,
        "b90992b888710715d613a4fedcd75d5b4db294ef64f3194e4f77ea0a8b387441",
    };
    return archive;
}

// 压缩包内 DLL 所在目录（解压时整段剥掉）。
inline constexpr char kRuntimeArchiveLibDirectory[] = "sherpa-onnx-v1.13.8-win-x64-shared-MD-Release-no-tts-lib/lib";

// 解压后逐个钉死：Server 加载前再按这里校验一遍，数据目录里的 DLL 被换掉就拒绝加载。
// 顺序即加载顺序：先载依赖，sherpa-onnx-c-api.dll 的导入表就直接解析到这里载入的 onnxruntime。
inline const std::vector<RuntimeLibrary> &RuntimeLibraries()
{
    static const std::vector<RuntimeLibrary> libraries{
        {"onnxruntime_providers_shared.dll", 10752, "0190137dee4933261c065c5d030c3568a68c7aa43cad942b4726a07011738bc2"},
        {"onnxruntime.dll", 17136128, "422d776ab0e3218260f7f628fcb84606aaa5c21116f720c8619a6da5e0b2e0f9"},
        {"sherpa-onnx-c-api.dll", 2882048, "5616605dc07ffbcf1cd6a001c2c76e826b9e1710e0fe17dfc3412de3a28df6a5"},
    };
    return libraries;
}

inline const RemoteFile &VadModel()
{
    static const RemoteFile vad{
        "silero_vad.onnx",
        L"github.com",
        L"/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad.onnx",
        643854,
        "9e2449e1087496d8d4caba907f23e0bd3f78d91fa552479bb9c23ac09cbb1fd6",
    };
    return vad;
}

inline const std::vector<ModelEntry> &Catalog()
{
    static const std::vector<ModelEntry> catalog{
        {
            "x-asr-zh-en-streaming",
            "中英流式（X-ASR）",
            "边说边出字，自带标点，中英夹杂识别好；内存占用最小",
            ModelKind::OnlineTransducer,
            "zipformer2",
            {"sherpa-onnx-x-asr-480ms-streaming-zipformer-transducer-zh-en-punct-int8-2026-06-05.tar.bz2",
             L"github.com",
             L"/k2-fsa/sherpa-onnx/releases/download/asr-models/"
             L"sherpa-onnx-x-asr-480ms-streaming-zipformer-transducer-zh-en-punct-int8-2026-06-05.tar.bz2",
             133895136, "fa5f63d618e5a01526e275a358bb7772e403f84808a4769fba52cffd8160bf74"},
            "sherpa-onnx-x-asr-480ms-streaming-zipformer-transducer-zh-en-punct-int8-2026-06-05",
            {{"encoder", "encoder.int8.onnx"},
             {"decoder", "decoder.onnx"},
             {"joiner", "joiner.int8.onnx"},
             {"tokens", "tokens.txt"}},
            "约 134 MB",
            "约 0.5 GB",
            "Apache-2.0",
            "X-ASR (https://github.com/Gilgamesh-J/X-ASR), Apache License 2.0. Converted to ONNX by the "
            "k2-fsa/sherpa-onnx project.",
        },
        {
            "x-asr-zh-en-offline",
            "中英整句（X-ASR）",
            "逐句识别：说完一句出一句，正在说的这句先显示草稿；比中英流式更准，自带标点，中英夹杂",
            ModelKind::OfflineTransducer,
            "transducer",
            {"sherpa-onnx-x-asr-zipformer-transducer-zh-en-punct-int8-2026-06-03.tar.bz2", L"github.com",
             L"/k2-fsa/sherpa-onnx/releases/download/asr-models/"
             L"sherpa-onnx-x-asr-zipformer-transducer-zh-en-punct-int8-2026-06-03.tar.bz2",
             136396739, "5d02c36d7b44e886b7c8f0d8e051f8713acab96c264bb6ef9e718be39a6a2224"},
            "sherpa-onnx-x-asr-zipformer-transducer-zh-en-punct-int8-2026-06-03",
            {{"encoder", "encoder-epoch-99-avg-1.int8.onnx"},
             {"decoder", "decoder-epoch-99-avg-1.onnx"},
             {"joiner", "joiner-epoch-99-avg-1.int8.onnx"},
             {"tokens", "tokens.txt"}},
            "约 137 MB",
            "约 0.6 GB",
            "Apache-2.0",
            "X-ASR (https://github.com/Gilgamesh-J/X-ASR), Apache License 2.0. Converted to ONNX by the "
            "k2-fsa/sherpa-onnx project.",
        },
        {
            "sense-voice-small",
            "快速整句（SenseVoice）",
            "逐句识别：说完一句出一句，正在说的这句先显示草稿；速度最快，支持中、英、日、韩、粤语",
            ModelKind::OfflineSenseVoice,
            "",
            {"sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17.tar.bz2", L"github.com",
             L"/k2-fsa/sherpa-onnx/releases/download/asr-models/"
             L"sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17.tar.bz2",
             163002883, "7d1efa2138a65b0b488df37f8b89e3d91a60676e416f515b952358d83dfd347e"},
            "sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17",
            {{"model", "model.int8.onnx"}, {"tokens", "tokens.txt"}},
            "约 163 MB",
            "约 0.9 GB",
            "FunASR Model License 1.1",
            "SenseVoice-Small by Alibaba Tongyi Lab (FunAudioLLM), used under the FunASR Model License v1.1 "
            "(https://github.com/modelscope/FunASR/blob/main/MODEL_LICENSE). Converted to ONNX by the "
            "k2-fsa/sherpa-onnx project. The model name and this attribution must be kept; it is downloaded on "
            "request and not shipped with MetasequoiaIME.",
        },
        {
            "fun-asr-nano",
            "高精度（Fun-ASR-Nano）",
            "大模型解码，专有名词与中英夹杂最准，速度较慢，说完后才整段出字，建议 16 GB 内存以上的电脑使用",
            ModelKind::OfflineFunAsrNano,
            "",
            {"sherpa-onnx-funasr-nano-int8-2025-12-30.tar.bz2", L"github.com",
             L"/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-funasr-nano-int8-2025-12-30.tar.bz2",
             841730611, "eb43d7ccc2e86b243f6a03b7df361033dda66db9523d1a92bf6aca2b50c9476b"},
            "sherpa-onnx-funasr-nano-int8-2025-12-30",
            {{"encoder_adaptor", "encoder_adaptor.int8.onnx"},
             {"llm", "llm.int8.onnx"},
             {"embedding", "embedding.int8.onnx"},
             {"tokenizer", "Qwen3-0.6B"}},
            "约 842 MB",
            "约 1.5 GB",
            "Apache-2.0",
            "Fun-ASR-Nano-2512 by Alibaba Tongyi Lab (FunAudioLLM), Apache License 2.0; its decoder is Qwen3-0.6B "
            "(Apache-2.0). Converted to ONNX by the k2-fsa/sherpa-onnx project.",
        },
    };
    return catalog;
}

inline const ModelEntry *FindModel(const std::string &id)
{
    for (const ModelEntry &entry : Catalog())
    {
        if (id == entry.id)
            return &entry;
    }
    return nullptr;
}

inline std::filesystem::path Root()
{
    return metasequoia::data_directory() / "models" / "voice-asr";
}

inline std::filesystem::path RuntimeDirectory()
{
    return Root() / "runtime";
}

inline std::filesystem::path VadDirectory()
{
    return Root() / "vad";
}

inline std::filesystem::path VadFile()
{
    return VadDirectory() / VadModel().name;
}

inline std::filesystem::path ModelDirectory(const ModelEntry &model)
{
    return Root() / model.id;
}

inline bool IsInstalled(const std::filesystem::path &directory)
{
    std::error_code error;
    return std::filesystem::is_regular_file(directory / kInstalledMarker, error) && !error;
}

inline bool RuntimeInstalled()
{
    return IsInstalled(RuntimeDirectory());
}

inline bool VadInstalled()
{
    return IsInstalled(VadDirectory());
}

inline bool ModelFilesInstalled(const ModelEntry &model)
{
    return IsInstalled(ModelDirectory(model));
}

// 识别所需的全部组件都在：模型、运行库，整句模型还要 VAD。
inline bool ModelReady(const ModelEntry &model)
{
    return ModelFilesInstalled(model) && RuntimeInstalled() && (!NeedsVad(model) || VadInstalled());
}

inline std::string SourceUrl(const RemoteFile &file)
{
    std::string url = "https://";
    for (const wchar_t *p = file.host; *p; ++p)
        url.push_back(static_cast<char>(*p));
    for (const wchar_t *p = file.path; *p; ++p)
        url.push_back(static_cast<char>(*p));
    return url;
}

} // namespace local_asr
