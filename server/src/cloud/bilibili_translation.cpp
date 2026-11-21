#include "bilibili_translation.h"

#include "ai/chat_completions.h"
#include <algorithm>
#include <cctype>
#include <chrono>

namespace
{
// 单个词的请求超时。2B 模型通常几百毫秒就答完，35B-A3B 慢一些；整批另有时间预算兜底。
constexpr long kRequestTimeoutMs = 5000;
constexpr auto kBatchBudget = std::chrono::seconds(6);
// 候选窗只显示简短释义，限制生成长度，避免模型偶尔复读时拖慢整批。
constexpr int kMaxTokens = 128;

// 语种代码到中文名，取值与官方客户端的 LANG_NAMES 一致。
std::string LanguageName(const std::string &code)
{
    if (code == "zh")
        return "中文";
    if (code == "en")
        return "英语";
    if (code == "fr")
        return "法语";
    if (code == "ja")
        return "日语";
    if (code == "es")
        return "西班牙语";
    if (code == "ru")
        return "俄语";
    if (code == "de")
        return "德语";
    if (code == "ko")
        return "韩语";
    if (code == "th")
        return "泰语";
    if (code == "vi")
        return "越南语";
    if (code == "it")
        return "意大利语";
    return code;
}

std::string Trim(std::string value)
{
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    const auto begin = std::find_if(value.begin(), value.end(), not_space);
    const auto end = std::find_if(value.rbegin(), value.rend(), not_space).base();
    if (begin >= end)
        return {};
    return std::string(begin, end);
}
} // namespace

namespace BilibiliTranslation
{
namespace detail
{
std::string BuildPrompt(const std::string &text, const std::string &source, const std::string &target)
{
    return "请将以下" + LanguageName(source) + "文本翻译为" + LanguageName(target) +
           "，直接输出翻译结果，不要进行任何解释。\n\n" + text;
}

nlohmann::json BuildRequestBody(const Config &config, const std::string &text, const std::string &source,
                                const std::string &target)
{
    // 关掉思考，免得思维链挤掉译文，参数与官方客户端一致。
    return {{"model", NormalizeModel(config.model)},
            {"messages", {{{"role", "user"}, {"content", BuildPrompt(text, source, target)}}}},
            {"temperature", 0.0},
            {"max_tokens", kMaxTokens},
            {"stream", false},
            {"chat_template_kwargs", {{"enable_thinking", false}}}};
}
} // namespace detail

std::vector<std::string> TextTranslateBatch(const Config &config, const std::vector<std::string> &texts,
                                            const std::string &source, const std::string &target,
                                            std::string *failure_detail)
{
    std::vector<std::string> results(texts.size());
    if (failure_detail != nullptr)
        failure_detail->clear();
    if (texts.empty() || source.empty() || target.empty())
        return results;

    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < texts.size(); ++i)
    {
        if (std::chrono::steady_clock::now() - started >= kBatchBudget)
        {
            results.resize(i);
            break;
        }
        const auto body = detail::BuildRequestBody(config, texts[i], source, target);
        // 接口不鉴权，token 留空就不带 Authorization 头。
        const auto fetched = ChatCompletions::FetchContent(kEndpoint, std::string(), body.dump(), kRequestTimeoutMs);
        if (fetched.content)
        {
            results[i] = Trim(*fetched.content);
        }
        else if (failure_detail != nullptr && failure_detail->empty())
        {
            *failure_detail = ChatCompletions::DescribeFailure(fetched.failure, fetched.detail);
        }
    }
    return results;
}
} // namespace BilibiliTranslation
