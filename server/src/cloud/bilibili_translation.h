#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <string>
#include <vector>

// 哔哩哔哩 Index-Translate 免费翻译接口（OpenAI 兼容 Chat Completions，不需要 API Key）。
// 参照官方客户端 bilibili/Index-Translate inference/llm/call_api.py。
namespace BilibiliTranslation
{
constexpr const char *kEndpoint = "https://index-translate.bilibili.com/v1/chat/completions";
constexpr const char *kDefaultModel = "Index-Translate-2B";

struct ModelInfo
{
    const char *id;
    const wchar_t *label;
};

// 接口支持的文本翻译模型，第一项是默认值。设置页下拉框与配置校验共用这一份；放在头文件里，
// 配置层不必为了校验模型名再链接网络请求的实现。
inline const std::vector<ModelInfo> &Models()
{
    static const std::vector<ModelInfo> kModels = {
        {"Index-Translate-2B", L"Index-Translate-2B（最快）"},
        {"Index-Translate-9B", L"Index-Translate-9B"},
        {"Index-Translate-35B-A3B", L"Index-Translate-35B-A3B（预览）"},
    };
    return kModels;
}

inline bool IsSupportedModel(const std::string &model)
{
    const auto &models = Models();
    return std::any_of(models.begin(), models.end(), [&model](const ModelInfo &info) { return model == info.id; });
}

// 不认识的模型名回落到默认模型，免得手改坏的配置让翻译整个失效。
inline std::string NormalizeModel(const std::string &model)
{
    return IsSupportedModel(model) ? model : std::string(kDefaultModel);
}

struct Config
{
    std::string model = kDefaultModel;
};

namespace detail
{
// 与官方客户端相同的纯文本翻译提示词。暴露给测试。
std::string BuildPrompt(const std::string &text, const std::string &source, const std::string &target);
nlohmann::json BuildRequestBody(const Config &config, const std::string &text, const std::string &source,
                                const std::string &target);
} // namespace detail

// 每个词单独请求一次（翻译模型按整段原文翻译，把多个词拼成一段会被当成一句话）。空串表示该项
// 失败；批次时间预算用完时结果比 texts 短，缺的尾部根本没有发出。failure_detail 非空时写入
// 第一个失败请求的原因，供设置页的测试按钮展示。
std::vector<std::string> TextTranslateBatch(const Config &config, const std::vector<std::string> &texts,
                                            const std::string &source, const std::string &target,
                                            std::string *failure_detail = nullptr);
} // namespace BilibiliTranslation
