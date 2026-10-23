#include "translate/translate_config.h"

#include "cloud/translation_gloss.h"

namespace Translate
{
namespace
{
TencentTmt::Credentials ResolveTencent(const TencentTmtConfig &config)
{
    TencentTmt::Credentials credentials;
    // provider 由设置页明确选成腾讯云时，加载配置会把 enabled 置为 true；只是从旧布尔推导出
    // 腾讯云的配置仍要求 enabled，和旧版一样，不会因为升级就开始向腾讯云发送候选词。
    if (!config.enabled)
        return credentials;
    credentials.region = config.region.empty() ? "ap-guangzhou" : config.region;
    credentials.secret_id = CloudTranslation::TrimSecret(config.secret_id);
    credentials.secret_key = CloudTranslation::TrimSecret(config.secret_key);
    if (!CloudTranslation::IsUsableSecret(credentials.secret_id) ||
        !CloudTranslation::IsUsableSecret(credentials.secret_key))
        return {};
    return credentials;
}
} // namespace

Vendor ActiveVendor(const TranslationConfig &config)
{
    if (config.provider == "ai")
        return Vendor::Ai;
    if (config.provider == "niutrans")
        return Vendor::NiuTrans;
    if (config.provider == "aliyun")
        return Vendor::Aliyun;
    if (config.provider == "custom")
        return Vendor::Custom;
    return Vendor::Tencent;
}

std::string VendorScope(const TranslationConfig &config)
{
    switch (ActiveVendor(config))
    {
    case Vendor::Ai:
        return "ai:" + config.ai.provider + ":" + config.ai.endpoint + ":" + config.ai.model;
    case Vendor::Aliyun:
        // 阿里云没有应用 id，缓存域按 AccessKey ID 区分。
        return "aliyun:" + config.aliyun.secret_id;
    case Vendor::NiuTrans:
        return "niutrans:" + config.niutrans.app_id;
    case Vendor::Custom:
        // 协议与模型一起进缓存域：同一地址换协议或换模型，旧译文不该继续命中。
        return "custom:" + config.custom.endpoint + ":" +
               (config.custom.format == "deeplx" ? std::string("deeplx") : config.custom.model);
    case Vendor::Tencent:
    default:
        return "tencent";
    }
}

std::string TargetLanguage(const TranslationConfig &config)
{
    const auto &language = config.tencent.target_language;
    return language.empty() ? "en" : language;
}

std::optional<RuntimeConfig> Resolve(const TranslationConfig &config)
{
    RuntimeConfig resolved;
    resolved.vendor = ActiveVendor(config);
    resolved.target_language = TargetLanguage(config);
    switch (resolved.vendor)
    {
    case Vendor::Ai:
        // 复用 AI 辅助当前服务商的接口地址、Token 与模型，与 ai_assistant.enabled 解耦：
        // 用户可能只想用翻译、不开 AI 联想。
        resolved.ai = AiTranslation::FromConfiguredTranslation(config);
        if (!AiTranslation::IsUsableConfig(resolved.ai))
            return std::nullopt;
        break;
    case Vendor::Aliyun:
        resolved.aliyun = {CloudTranslation::TrimSecret(config.aliyun.secret_id),
                           CloudTranslation::TrimSecret(config.aliyun.secret_key)};
        if (!AliyunMt::IsUsableCredentials(resolved.aliyun))
            return std::nullopt;
        break;
    case Vendor::NiuTrans:
        resolved.niutrans = {CloudTranslation::TrimSecret(config.niutrans.app_id),
                             CloudTranslation::TrimSecret(config.niutrans.apikey)};
        if (!NiuTransTranslation::IsUsableConfig(resolved.niutrans))
            return std::nullopt;
        break;
    case Vendor::Custom:
        // 四协议、模型、提示词槽和校对开关都在这里落地：只拿 endpoint + api_key 会让
        // 用户选的协议被默默换成 Chat Completions，模型也退回 gpt-4o-mini。
        resolved.custom.endpoint = CloudTranslation::TrimSecret(config.custom.endpoint);
        resolved.custom.api_key = CloudTranslation::TrimSecret(config.custom.api_key);
        resolved.custom.format = config.custom.format;
        resolved.custom.model = config.custom.model.empty() ? std::string("gpt-4o-mini") : config.custom.model;
        // 提示词槽已在配置加载时按 prompt_id 展开成 prompt，这里不再搬运槽本身：
        // CustomTranslation::Config 只带最终生效的那一份。
        resolved.custom.prompt = config.custom.prompt;
        resolved.custom.review = config.custom.review;
        resolved.custom.thinking = config.custom.thinking;
        if (!CustomTranslation::IsSupportedEndpoint(resolved.custom.endpoint))
            return std::nullopt;
        break;
    case Vendor::Tencent:
    default:
        resolved.tencent = ResolveTencent(config.tencent);
        if (!CloudTranslation::IsUsableSecret(resolved.tencent.secret_id))
            return std::nullopt;
        break;
    }
    return resolved;
}
} // namespace Translate
