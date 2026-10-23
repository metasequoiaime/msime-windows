// 在线服务配置：语音输入（识别与润色的提供商、凭证槽位、热键）、AI 助手、翻译服务（腾讯 TMT、自定义翻译、小牛翻译）
// 和这些服务共用的网络代理。
#include "config/ime_config_internal.h"
#include <algorithm>
#include <cctype>
#include <mutex>
#include <shared_mutex>
#include <string>
#include "config/ai_provider.h"
#include "voice-input/voice_providers.h"

using namespace ime_config_detail;

VoiceInputConfig GetConfiguredVoiceInput()
{
    return SnapshotVoiceInput();
}

bool SetConfiguredVoiceInputString(const std::string &key, const std::string &value)
{
    if (key == "language" && value != "zh-cn" && value != "en" && value != "auto")
        return false;
    if (key == "commit_mode" && value != "tsf" && value != "sendinput" && value != "ctrl_v")
        return false;
    if (key == "polish_prompt_id" && value != "cleanup" && value != "faithful" && value != "zh2en" &&
        value != "casual" && value != "custom_1" && value != "custom_2" && value != "custom_3")
        return false;

    // Fields are addressed by pointer-to-member rather than by a bare reference because the store has to happen under
    // the writer lock, while WriteConfiguredValue does file I/O and a cross-process wait that must never run with that
    // lock held.
    const auto persist = [](const std::string &toml_key, const std::string &toml_value,
                            std::string VoiceInputConfig::*field) {
        if (!WriteConfiguredValue("voice_input", toml_key, EscapeTomlBasicString(toml_value)))
            return false;
        std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
        g_voice_input.*field = toml_value;
        return true;
    };

    if (key.rfind("asr_token_", 0) == 0)
    {
        const std::string provider = key.substr(std::string("asr_token_").size());
        if (VoiceInput::AsrTokenSlotKey(provider) != key)
            return false;
        const std::string id = VoiceInput::NormalizeProviderId(provider);
        if (!WriteConfiguredValue("voice_input", key, EscapeTomlBasicString(value)))
            return false;
        std::string asr_provider;
        {
            std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
            g_voice_input.asr_tokens[id] = value;
            asr_provider = g_voice_input.asr_provider;
        }
        if (VoiceInput::NormalizeProviderId(asr_provider) == id)
            persist("asr_token", value, &VoiceInputConfig::asr_token);
        return true;
    }
    if (key.rfind("polish_token_", 0) == 0)
    {
        const std::string provider = key.substr(std::string("polish_token_").size());
        if (VoiceInput::PolishTokenSlotKey(provider) != key)
            return false;
        const std::string id = VoiceInput::NormalizeProviderId(provider);
        if (!WriteConfiguredValue("voice_input", key, EscapeTomlBasicString(value)))
            return false;
        std::string polish_provider;
        {
            std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
            g_voice_input.polish_tokens[id] = value;
            polish_provider = g_voice_input.polish_provider;
        }
        if (VoiceInput::NormalizeProviderId(polish_provider) == id)
            persist("polish_token", value, &VoiceInputConfig::polish_token);
        return true;
    }

    std::string VoiceInputConfig::*target = nullptr;
    if (key == "asr_provider")
        target = &VoiceInputConfig::asr_provider;
    else if (key == "asr_app_key")
        target = &VoiceInputConfig::asr_app_key;
    else if (key == "doubao_auth_mode")
        target = &VoiceInputConfig::doubao_auth_mode;
    else if (key == "asr_token")
        target = &VoiceInputConfig::asr_token;
    else if (key == "asr_endpoint")
        target = &VoiceInputConfig::asr_endpoint;
    else if (key == "asr_resource_id")
        target = &VoiceInputConfig::asr_resource_id;
    else if (key == "doubao_boosting_table_id")
        target = &VoiceInputConfig::doubao_boosting_table_id;
    else if (key == "asr_model")
        target = &VoiceInputConfig::asr_model;
    else if (key == "polish_provider")
        target = &VoiceInputConfig::polish_provider;
    else if (key == "polish_token")
        target = &VoiceInputConfig::polish_token;
    else if (key == "polish_endpoint")
        target = &VoiceInputConfig::polish_endpoint;
    else if (key == "polish_model")
        target = &VoiceInputConfig::polish_model;
    else if (key == "polish_thinking" && VoiceInput::IsValidThinkingLevel(value))
        target = &VoiceInputConfig::polish_thinking;
    else if (key == "polish_prompt_id")
        target = &VoiceInputConfig::polish_prompt_id;
    else if (key == "polish_prompt")
        target = &VoiceInputConfig::polish_prompt;
    else if (key == "polish_prompt_custom_1")
        target = &VoiceInputConfig::polish_prompt_custom_1;
    else if (key == "polish_prompt_custom_2")
        target = &VoiceInputConfig::polish_prompt_custom_2;
    else if (key == "polish_prompt_custom_3")
        target = &VoiceInputConfig::polish_prompt_custom_3;
    else if (key == "language")
        target = &VoiceInputConfig::language;
    else if (key == "commit_mode")
        target = &VoiceInputConfig::commit_mode;
    if (!target || !WriteConfiguredValue("voice_input", key, EscapeTomlBasicString(value)))
        return false;
    {
        std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
        g_voice_input.*target = value;
    }
    if (key == "polish_prompt_custom_1")
    {
        WriteConfiguredValue("voice_input", "polish_prompt", EscapeTomlBasicString(""));
        std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
        g_voice_input.polish_prompt.clear();
    }
    if (key == "asr_token")
    {
        const std::string asr_provider = SnapshotVoiceInput().asr_provider;
        const std::string slot = VoiceInput::AsrTokenSlotKey(asr_provider);
        if (!slot.empty())
        {
            const std::string id = VoiceInput::NormalizeProviderId(asr_provider);
            {
                std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
                g_voice_input.asr_tokens[id] = value;
            }
            WriteConfiguredValue("voice_input", slot, EscapeTomlBasicString(value));
        }
    }
    else if (key == "polish_token")
    {
        const std::string polish_provider = SnapshotVoiceInput().polish_provider;
        const std::string slot = VoiceInput::PolishTokenSlotKey(polish_provider);
        if (!slot.empty())
        {
            const std::string id = VoiceInput::NormalizeProviderId(polish_provider);
            {
                std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
                g_voice_input.polish_tokens[id] = value;
            }
            WriteConfiguredValue("voice_input", slot, EscapeTomlBasicString(value));
        }
    }
    else if (key == "asr_provider")
    {
        persist("asr_token", VoiceInput::ResolveAsrToken(SnapshotVoiceInput()), &VoiceInputConfig::asr_token);
    }
    else if (key == "polish_provider")
    {
        persist("polish_token", VoiceInput::ResolvePolishToken(SnapshotVoiceInput()), &VoiceInputConfig::polish_token);
    }
    return true;
}

bool SetConfiguredVoiceInputBool(const std::string &key, bool value)
{
    bool VoiceInputConfig::*target = nullptr;
    if (key == "voice_input")
        target = &VoiceInputConfig::enabled;
    else if (key == "hotkey_ralt")
        target = &VoiceInputConfig::hotkey_ralt;
    else if (key == "hotkey_ctrl_f9")
        target = &VoiceInputConfig::hotkey_ctrl_f9;
    else if (key == "hotkey_ctrl_win")
        target = &VoiceInputConfig::hotkey_ctrl_win;
    else if (key == "hotkey_rctrl_ralt")
        target = &VoiceInputConfig::hotkey_rctrl_ralt;
    else if (key == "hotkey_hold_space_lock")
        target = &VoiceInputConfig::hotkey_hold_space_lock;
    else if (key == "start_sound")
        target = &VoiceInputConfig::start_sound;
    else if (key == "end_sound")
        target = &VoiceInputConfig::end_sound;
    else if (key == "mute_system_audio")
        target = &VoiceInputConfig::mute_system_audio;
    else if (key == "doubao_enable_itn")
        target = &VoiceInputConfig::doubao_enable_itn;
    else if (key == "doubao_enable_punc")
        target = &VoiceInputConfig::doubao_enable_punc;
    else if (key == "doubao_enable_ddc")
        target = &VoiceInputConfig::doubao_enable_ddc;
    else if (key == "notification_sound")
    {
        // Accept settings pages from older installations during a rolling update.
        if (!WriteConfiguredValue("voice_input", key, value ? "true" : "false"))
            return false;
        std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
        g_voice_input.start_sound = value;
        g_voice_input.end_sound = value;
        return true;
    }
    else if (key == "polish_text")
        target = &VoiceInputConfig::polish_text;
    else if (key == "stream_inline_preedit")
        target = &VoiceInputConfig::stream_inline_preedit;
    if (!target || !WriteConfiguredValue("voice_input", key, value ? "true" : "false"))
        return false;
    std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
    g_voice_input.*target = value;
    return true;
}

const AiAssistantConfig &GetConfiguredAiAssistant()
{
    return g_ai_assistant;
}

static bool SaveAiProviderStore()
{
    if (!AiAssistant::ProviderStore::Save(g_ai_assistant.providers))
        return false;
    RememberProviderStoreWriteTime();
    return true;
}

static bool ApplyAiAssistantString(const std::string &key, const std::string &value);

bool SetAiAssistantProviders(const std::vector<AiAssistant::AiProvider> &entries)
{
    if (entries.empty())
        return false;
    for (const auto &entry : entries)
    {
        if (entry.id.empty())
            return false;
    }
    const std::vector<AiAssistant::AiProvider> previous = g_ai_assistant.providers;
    g_ai_assistant.providers = entries;
    // Tokens are edited through token_<id> and live in config.toml; whatever the page echoes
    // back here is not stored again.
    for (auto &entry : g_ai_assistant.providers)
        entry.token.clear();
    if (!SaveAiProviderStore())
    {
        g_ai_assistant.providers = previous;
        return false;
    }
    const auto active = std::find_if(entries.begin(), entries.end(),
                                     [](const auto &entry) { return entry.id == g_ai_assistant.provider; });
    if (active == entries.end())
    {
        // The active provider was deleted: switch to the first entry the same way the settings
        // page would, so its token, endpoint and model replace the deleted provider's.
        ApplyAiAssistantString("provider", entries.front().id);
    }
    else
    {
        AiAssistant::ProviderStore::ResolveActive(g_ai_assistant.active, g_ai_assistant.endpoint, g_ai_assistant.model,
                                                  g_ai_assistant.provider, g_ai_assistant.providers);
    }
    PublishTranslationConfig();
    return true;
}

const TencentTmtConfig &GetConfiguredTencentTmt()
{
    return g_tencent_tmt;
}

bool SetConfiguredTencentTmtString(const std::string &key, const std::string &value)
{
    std::string *target = nullptr;
    if (key == "secret_id")
        target = &g_tencent_tmt.secret_id;
    else if (key == "secret_key")
        target = &g_tencent_tmt.secret_key;
    else if (key == "region")
        target = &g_tencent_tmt.region;
    else if (key == "target_language" && IsValidTranslationTargetLanguage(value))
        target = &g_tencent_tmt.target_language;
    if (!target || !WriteConfiguredValue("tencent_tmt", key, EscapeTomlBasicString(value)))
        return false;
    *target = value;
    PublishTranslationConfig();
    return true;
}

const CustomTranslationConfig &GetConfiguredCustomTranslation()
{
    return g_custom_translation;
}

// 旧的 enabled 开关不再单独落盘，改成切换 [translation].provider：打开即选中该服务，关掉当前
// 生效的服务回落到腾讯云，和旧版两个布尔都为 false 时的效果一致。
static bool SetTranslationProviderFromLegacyBool(const std::string &provider, bool enabled)
{
    if (enabled)
        return SetConfiguredTranslationString("provider", provider);
    if (g_translation_provider != provider)
        return true;
    return SetConfiguredTranslationString("provider", "tencent");
}

bool SetConfiguredCustomTranslationBool(const std::string &key, bool value)
{
    // 校对开关是一个独立的布尔键，不走 enabled 的提供方推导：关掉它不该把整个翻译服务换掉。
    if (key == "review")
    {
        if (!WriteConfiguredValue("custom_translation", key, value ? "true" : "false"))
            return false;
        g_custom_translation.review = value;
        PublishTranslationConfig();
        return true;
    }
    return key == "enabled" && SetTranslationProviderFromLegacyBool("custom", value);
}

bool SetConfiguredCustomTranslationString(const std::string &key, const std::string &value)
{
    std::string *target = nullptr;
    if (key == "endpoint")
        target = &g_custom_translation.endpoint;
    else if (key == "api_key")
        target = &g_custom_translation.api_key;
    else if (key == "model")
        target = &g_custom_translation.model;
    else if (key == "prompt")
        // 旧版单槽：写进第一槽，保持向后兼容。
        target = &g_custom_translation.prompt_custom_1;
    else if (key == "prompt_custom_1")
        target = &g_custom_translation.prompt_custom_1;
    else if (key == "prompt_custom_2")
        target = &g_custom_translation.prompt_custom_2;
    else if (key == "prompt_custom_3")
        target = &g_custom_translation.prompt_custom_3;
    else if (key == "prompt_id" && (value == "custom_1" || value == "custom_2" || value == "custom_3"))
        target = &g_custom_translation.prompt_id;
    else if (key == "format" && (value == "openai" || value == "responses" || value == "anthropic" ||
                                 value == "gemini" || value == "deeplx"))
        target = &g_custom_translation.format;
    else if (key == "thinking" &&
             (value.empty() || value == "disabled" || value == "low" || value == "high" || value == "max"))
        target = &g_custom_translation.thinking;
    if (!target || !WriteConfiguredValue("custom_translation", key, EscapeTomlBasicString(value)))
        return false;
    *target = value;
    if (key == "prompt" || key.compare(0, 14, "prompt_custom_") == 0 || key == "prompt_id")
        g_custom_translation.prompt =
            g_custom_translation.prompt_id == "custom_2"   ? g_custom_translation.prompt_custom_2
            : g_custom_translation.prompt_id == "custom_3" ? g_custom_translation.prompt_custom_3
                                                           : g_custom_translation.prompt_custom_1;
    PublishTranslationConfig();
    return true;
}

const AliyunMtConfig &GetConfiguredAliyunMt()
{
    return g_aliyun_mt;
}

bool SetConfiguredAliyunMtBool(const std::string &key, bool value)
{
    return key == "enabled" && SetTranslationProviderFromLegacyBool("aliyun", value);
}

bool SetConfiguredAliyunMtString(const std::string &key, const std::string &value)
{
    std::string *target = nullptr;
    if (key == "secret_id")
        target = &g_aliyun_mt.secret_id;
    else if (key == "secret_key")
        target = &g_aliyun_mt.secret_key;
    if (!target || !WriteConfiguredValue("aliyun_mt", key, EscapeTomlBasicString(value)))
        return false;
    *target = value;
    PublishTranslationConfig();
    return true;
}

const NiuTransConfig &GetConfiguredNiuTrans()
{
    return g_niutrans;
}

bool SetConfiguredNiuTransBool(const std::string &key, bool value)
{
    return key == "enabled" && SetTranslationProviderFromLegacyBool("niutrans", value);
}

bool SetConfiguredNiuTransString(const std::string &key, const std::string &value)
{
    std::string *target = nullptr;
    if (key == "app_id")
        target = &g_niutrans.app_id;
    else if (key == "apikey")
        target = &g_niutrans.apikey;
    if (!target || !WriteConfiguredValue("niutrans", key, EscapeTomlBasicString(value)))
        return false;
    *target = value;
    PublishTranslationConfig();
    return true;
}

bool IsValidTranslationProvider(const std::string &provider)
{
    return provider == "tencent" || provider == "aliyun" || provider == "niutrans" || provider == "custom" ||
           provider == "ai";
}

namespace
{
std::mutex g_translation_snapshot_mutex;
std::shared_ptr<const TranslationConfig> g_translation_snapshot = std::make_shared<TranslationConfig>();
} // namespace

void ime_config_detail::PublishTranslationConfig()
{
    auto snapshot = std::make_shared<TranslationConfig>();
    snapshot->provider = g_translation_provider;
    snapshot->tencent = g_tencent_tmt;
    snapshot->aliyun = g_aliyun_mt;
    snapshot->custom = g_custom_translation;
    snapshot->niutrans = g_niutrans;
    // 候选词翻译只走 Chat Completions，选择器（jev）没有 chat 接口。AI 联想处在选择器模式时，
    // 复用「生成式上次用的那家」，这样选了排序也不会把翻译一起关掉。
    const std::string &translation_provider =
        g_ai_assistant.provider == "jev" ? g_ai_assistant.generate_provider : g_ai_assistant.provider;
    const auto slot_value = [&translation_provider](const std::map<std::string, std::string> &slots,
                                                    const std::string &fallback) {
        const auto found = slots.find(translation_provider);
        return found == slots.end() || found->second.empty() ? fallback : found->second;
    };
    const auto token = g_ai_assistant.tokens.find(translation_provider);
    snapshot->ai = {
        translation_provider,
        slot_value(g_ai_assistant.endpoints, AiAssistant::ProviderStore::BuiltinDefaultEndpoint(translation_provider)),
        token == g_ai_assistant.tokens.end() ? std::string() : token->second,
        slot_value(g_ai_assistant.models, AiAssistant::ProviderStore::BuiltinDefaultModel(translation_provider))};
    std::lock_guard<std::mutex> lock(g_translation_snapshot_mutex);
    g_translation_snapshot = std::move(snapshot);
}

std::shared_ptr<const TranslationConfig> GetConfiguredTranslation()
{
    std::lock_guard<std::mutex> lock(g_translation_snapshot_mutex);
    return g_translation_snapshot;
}

bool SetConfiguredTranslationString(const std::string &key, const std::string &value)
{
    if (key != "provider" || !IsValidTranslationProvider(value))
        return false;
    // 旧布尔与 provider 一次写入；旧版不认识的提供方对应布尔保持 false。
    const bool tencent = value == "tencent";
    const bool aliyun = value == "aliyun";
    const bool niutrans = value == "niutrans";
    const bool custom = value == "custom";
    if (!WriteConfiguredValues({{"translation", key, EscapeTomlBasicString(value)},
                                {"tencent_tmt", "enabled", tencent ? "true" : "false"},
                                {"aliyun_mt", "enabled", aliyun ? "true" : "false"},
                                {"niutrans", "enabled", niutrans ? "true" : "false"},
                                {"custom_translation", "enabled", custom ? "true" : "false"}}))
        return false;
    g_translation_provider = value;
    g_tencent_tmt.enabled = tencent;
    g_aliyun_mt.enabled = aliyun;
    g_niutrans.enabled = niutrans;
    g_custom_translation.enabled = custom;
    PublishTranslationConfig();
    return true;
}

std::string NormalizeNetworkProxyServer(const std::string &value)
{
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    const auto begin = std::find_if(value.begin(), value.end(), not_space);
    const auto end = std::find_if(value.rbegin(), value.rend(), not_space).base();
    std::string text = begin < end ? std::string(begin, end) : std::string();
    std::string lowered = text;
    for (char &ch : lowered)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lowered.rfind("http://", 0) == 0)
        text.erase(0, std::string("http://").size());
    else if (lowered.find("://") != std::string::npos)
        return {};
    while (!text.empty() && text.back() == '/')
        text.pop_back();

    const size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == text.size())
        return {};
    const std::string host = text.substr(0, colon);
    const std::string port = text.substr(colon + 1);
    if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](unsigned char ch) { return std::isdigit(ch); }))
        return {};
    const int port_number = std::stoi(port);
    if (port_number < 1 || port_number > 65535)
        return {};
    // 方括号 IPv6 字面量或主机名/IPv4；不收用户名密码（WinHTTP 的命名代理不认 user:pass@）。
    const bool bracketed = host.size() > 2 && host.front() == '[' && host.back() == ']' &&
                           std::all_of(host.begin() + 1, host.end() - 1,
                                       [](unsigned char ch) { return std::isxdigit(ch) || ch == ':' || ch == '.'; });
    const bool plain = std::all_of(host.begin(), host.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_';
    });
    if (!bracketed && !plain)
        return {};
    return host + ":" + std::to_string(port_number);
}

NetworkProxyConfig GetConfiguredNetworkProxy()
{
    std::lock_guard<std::mutex> lock(g_network_proxy_mutex);
    return g_network_proxy;
}

bool SetConfiguredNetworkString(const std::string &key, const std::string &value)
{
    std::string stored;
    if (key == "proxy_mode")
    {
        if (value != "system" && value != "none" && value != "custom")
            return false;
        stored = value;
    }
    else if (key == "proxy_server")
    {
        // 留空是合法的「未填写」；非空但解析不出 host:port 的一律拒绝，不把坏值写进配置。
        const bool blank = value.find_first_not_of(" \t") == std::string::npos;
        stored = blank ? std::string() : NormalizeNetworkProxyServer(value);
        if (!blank && stored.empty())
            return false;
    }
    else
    {
        return false;
    }
    if (!WriteConfiguredValue("network", key, EscapeTomlBasicString(stored)))
        return false;
    std::lock_guard<std::mutex> lock(g_network_proxy_mutex);
    (key == "proxy_mode" ? g_network_proxy.mode : g_network_proxy.server) = stored;
    return true;
}

// endpoint/model 落盘时只记用户改过的值：留空或等于提供商默认值都写空串，读回时再补默认值，
// 这样以后调整默认值，没改过的用户能跟着更新，而不是被切换提供商时顺手写进去的旧默认值钉住。
static std::string AiAssistantDefaultValue(const std::string &key, const std::string &provider)
{
    // 唯一来源在 ai_provider_store.cpp 的 BuiltinDefaults()；设置页的占位符也读同一份。
    return key == "endpoint" ? AiAssistant::ProviderStore::BuiltinDefaultEndpoint(provider)
                             : AiAssistant::ProviderStore::BuiltinDefaultModel(provider);
}

static std::string AiAssistantStoredValue(const std::string &key, const std::string &provider, const std::string &value)
{
    return value == AiAssistantDefaultValue(key, provider) ? std::string() : value;
}

static std::string AiAssistantEffectiveValue(const std::string &key, const std::string &provider,
                                             const std::string &value)
{
    return value.empty() ? AiAssistantDefaultValue(key, provider) : value;
}

static bool ApplyAiAssistantString(const std::string &key, const std::string &value)
{
    if (key == "prompt_id" && value != "custom_1" && value != "custom_2" && value != "custom_3")
        return false;
    // 生成式模式下最后选过的服务商。切到 Jev 之前把当前服务商记在这里，切回来时用它，
    // 否则 provider 被 jev 覆盖，用户每次切回生成式都得重新挑一遍。
    if (key == "generate_provider")
    {
        const std::string provider = VoiceInput::NormalizeProviderId(value);
        const auto registered =
            std::find_if(g_ai_assistant.providers.begin(), g_ai_assistant.providers.end(),
                         [&provider](const AiAssistant::AiProvider &entry) { return entry.id == provider; });
        if (provider == "jev" ||
            (AiAssistantTokenSlotKey(provider).empty() && registered == g_ai_assistant.providers.end()))
            return false;
        if (!WriteConfiguredValue("ai_assistant", key, EscapeTomlBasicString(provider)))
            return false;
        g_ai_assistant.generate_provider = provider;
        return true;
    }
    if (key == "thinking" && !VoiceInput::IsValidThinkingLevel(value))
        return false;
    if (key == "provider")
    {
        const std::string provider = VoiceInput::NormalizeProviderId(value);
        const auto registered =
            std::find_if(g_ai_assistant.providers.begin(), g_ai_assistant.providers.end(),
                         [&provider](const AiAssistant::AiProvider &entry) { return entry.id == provider; });
        // Accept the built-in slot ids plus anything in the provider registry.
        if (AiAssistantTokenSlotKey(provider).empty() && registered == g_ai_assistant.providers.end())
            return false;
        const std::string previous = g_ai_assistant.provider;
        const std::string token = g_ai_assistant.tokens[provider];
        std::string endpoint = AiAssistantEffectiveValue("endpoint", provider, g_ai_assistant.endpoints[provider]);
        std::string model = AiAssistantEffectiveValue("model", provider, g_ai_assistant.models[provider]);
        // Registry entries only speak for registry-only providers. Built-in ids stay
        // config.toml-authoritative, so a stale store copy cannot shadow fresh slot values.
        if (registered != g_ai_assistant.providers.end() && !AiAssistant::ProviderStore::IsBuiltinProvider(provider))
        {
            endpoint = registered->endpoint.empty() ? endpoint : registered->endpoint;
            model = registered->model.empty() ? model : registered->model;
        }
        const auto stored = [](const std::string &field, const std::string &id, const std::string &text) {
            return EscapeTomlBasicString(AiAssistantStoredValue(field, id, text));
        };
        if (!WriteConfiguredValues(
                {{"ai_assistant", "endpoint_" + previous, stored("endpoint", previous, g_ai_assistant.endpoint)},
                 {"ai_assistant", "model_" + previous, stored("model", previous, g_ai_assistant.model)},
                 {"ai_assistant", "provider", EscapeTomlBasicString(provider)},
                 // 离开生成式的那一刻把「上次用的服务商」记下来。记 previous 而不是 provider：
                 // 这样任何直接写 provider 的调用方都能得到正确结果，规则只在这里一处。
                 {"ai_assistant", "generate_provider",
                  EscapeTomlBasicString(previous == "jev" ? g_ai_assistant.generate_provider : previous)},
                 {"ai_assistant", "token", EscapeTomlBasicString(token)},
                 {"ai_assistant", "endpoint", stored("endpoint", provider, endpoint)},
                 {"ai_assistant", "model", stored("model", provider, model)}}))
            return false;
        g_ai_assistant.provider = provider;
        if (previous != "jev")
            g_ai_assistant.generate_provider = previous;
        g_ai_assistant.token = token;
        g_ai_assistant.endpoint = endpoint;
        g_ai_assistant.model = model;
        AiAssistant::ProviderStore::ResolveActive(g_ai_assistant.active, g_ai_assistant.endpoint, g_ai_assistant.model,
                                                  g_ai_assistant.provider, g_ai_assistant.providers);
        return true;
    }
    if (key == "endpoint" || key == "model")
    {
        const std::string effective = AiAssistantEffectiveValue(key, g_ai_assistant.provider, value);
        const std::string escaped =
            EscapeTomlBasicString(AiAssistantStoredValue(key, g_ai_assistant.provider, effective));
        if (!WriteConfiguredValues(
                {{"ai_assistant", key, escaped}, {"ai_assistant", key + "_" + g_ai_assistant.provider, escaped}}))
            return false;
        auto &slots = key == "endpoint" ? g_ai_assistant.endpoints : g_ai_assistant.models;
        slots[g_ai_assistant.provider] = effective;
        (key == "endpoint" ? g_ai_assistant.endpoint : g_ai_assistant.model) = effective;
        // A registry-only provider's endpoint/model live in the store (its config.toml slot does
        // not survive an upgrade), so keep that entry in step. Built-in ids are config-authoritative.
        const auto active =
            std::find_if(g_ai_assistant.providers.begin(), g_ai_assistant.providers.end(),
                         [](const AiAssistant::AiProvider &entry) { return entry.id == g_ai_assistant.provider; });
        if (active != g_ai_assistant.providers.end() &&
            !AiAssistant::ProviderStore::IsBuiltinProvider(g_ai_assistant.provider))
        {
            (key == "endpoint" ? active->endpoint : active->model) = effective;
            (key == "endpoint" ? g_ai_assistant.active.endpoint : g_ai_assistant.active.model) = effective;
            SaveAiProviderStore();
        }
        return true;
    }
    const auto persist = [](const std::string &toml_key, const std::string &toml_value, std::string &target) {
        if (!WriteConfiguredValue("ai_assistant", toml_key, EscapeTomlBasicString(toml_value)))
            return false;
        target = toml_value;
        return true;
    };

    if (key.rfind("token_", 0) == 0)
    {
        const std::string provider = key.substr(std::string("token_").size());
        const std::string id = VoiceInput::NormalizeProviderId(provider);
        // 不用 Token 的服务商（Ollama 的本机服务）没有槽位：拒绝写入，免得 config.toml 里多一个谁都不读的空值。
        if (!AiAssistant::ProviderUsesToken(id))
            return false;
        // Every provider's token is a config.toml credential (token_<id>, DPAPI-protected), the
        // registry ones included; the store never holds tokens.
        const auto registered = std::find_if(g_ai_assistant.providers.begin(), g_ai_assistant.providers.end(),
                                             [&id](const AiAssistant::AiProvider &entry) { return entry.id == id; });
        if (AiAssistantTokenSlotKey(provider) != key &&
            (registered == g_ai_assistant.providers.end() || provider != id))
            return false;
        if (!WriteConfiguredValue("ai_assistant", key, EscapeTomlBasicString(value)))
            return false;
        g_ai_assistant.tokens[id] = value;
        if (g_ai_assistant.provider == id)
            persist("token", value, g_ai_assistant.token);
        return true;
    }

    std::string *target = nullptr;
    if (key == "token")
        target = &g_ai_assistant.token;
    else if (key == "prompt_id")
        target = &g_ai_assistant.prompt_id;
    else if (key == "thinking")
        target = &g_ai_assistant.thinking;
    else if (key == "prompt_custom_1")
        target = &g_ai_assistant.prompt_custom_1;
    else if (key == "prompt_custom_2")
        target = &g_ai_assistant.prompt_custom_2;
    else if (key == "prompt_custom_3")
        target = &g_ai_assistant.prompt_custom_3;
    else if (key == "prompt")
        target = &g_ai_assistant.prompt;
    if (!target || !WriteConfiguredValue("ai_assistant", key, EscapeTomlBasicString(value)))
        return false;
    *target = value;
    if (key == "prompt_custom_1")
        WriteConfiguredValue("ai_assistant", "prompt", EscapeTomlBasicString(""));
    if (key == "prompt")
        g_ai_assistant.prompt_custom_1 = value;
    if (key == "prompt" || key == "prompt_id" || key.rfind("prompt_custom_", 0) == 0)
    {
        g_ai_assistant.prompt = g_ai_assistant.prompt_id == "custom_2"   ? g_ai_assistant.prompt_custom_2
                                : g_ai_assistant.prompt_id == "custom_3" ? g_ai_assistant.prompt_custom_3
                                                                         : g_ai_assistant.prompt_custom_1;
    }
    if (key == "token")
    {
        const std::string slot = AiAssistantTokenSlotKey(g_ai_assistant.provider);
        g_ai_assistant.tokens[g_ai_assistant.provider] = value;
        WriteConfiguredValue("ai_assistant", slot, EscapeTomlBasicString(value));
    }
    return true;
}

bool SetConfiguredAiAssistantString(const std::string &key, const std::string &value)
{
    if (!ApplyAiAssistantString(key, value))
        return false;
    // 候选词翻译选了 AI 辅助时复用这里的服务商、地址、Token 与模型。
    PublishTranslationConfig();
    return true;
}

bool SetConfiguredAiAssistantProviderValue(std::string_view provider, std::string_view field, const std::string &value)
{
    const std::string id = VoiceInput::NormalizeProviderId(provider);
    if (field != "endpoint" && field != "model")
        return false;
    const bool builtin = !AiAssistantTokenSlotKey(id).empty();
    const auto registered = std::find_if(g_ai_assistant.providers.begin(), g_ai_assistant.providers.end(),
                                         [&id](const AiAssistant::AiProvider &entry) { return entry.id == id; });
    if (!builtin && registered == g_ai_assistant.providers.end())
        return false;
    const std::string key(field);
    // 空值与默认值都存空串，读回时再补默认值；这样以后调整默认值，没改过的用户能跟着更新。
    const std::string escaped = EscapeTomlBasicString(AiAssistantStoredValue(key, id, value));
    std::vector<ConfigValueUpdate> writes{{"ai_assistant", key + "_" + id, escaped}};
    const bool active = g_ai_assistant.provider == id;
    if (active)
        writes.push_back({"ai_assistant", key, escaped});
    if (!WriteConfiguredValues(writes))
        return false;
    auto &slots = key == "endpoint" ? g_ai_assistant.endpoints : g_ai_assistant.models;
    slots[id] = value.empty() ? AiAssistantDefaultValue(key, id) : value;
    if (active)
        (key == "endpoint" ? g_ai_assistant.endpoint : g_ai_assistant.model) = slots[id];
    // 自定义服务商的槽位键活不过模板合并，值以注册表为准，这里同步改回去。
    if (!builtin)
    {
        if (key == "endpoint")
            registered->endpoint = slots[id];
        else
            registered->model = slots[id];
    }
    // 与 ApplyAiAssistantString 的 endpoint/model 分支保持一致：active 与单值键都跟随注册表解析。
    AiAssistant::ProviderStore::ResolveActive(g_ai_assistant.active, g_ai_assistant.endpoint, g_ai_assistant.model,
                                              g_ai_assistant.provider, g_ai_assistant.providers);
    // 注册表写失败时内存与 config.toml 已经改过，翻译快照仍要跟着更新，否则两边各说各话。
    const bool saved = builtin || SaveAiProviderStore();
    PublishTranslationConfig();
    return saved;
}

bool SetConfiguredAiAssistantBool(const std::string &key, bool value)
{
    if (key != "enabled" || !WriteConfiguredValue("ai_assistant", key, value ? "true" : "false"))
        return false;
    g_ai_assistant.enabled = value;
    return true;
}

bool SetConfiguredAiAssistantInt(const std::string &key, int value)
{
    if (key != "candidate_limit" || value < 1 || value > 10 ||
        !WriteConfiguredValue("ai_assistant", key, std::to_string(value)))
        return false;
    g_ai_assistant.candidate_limit = value;
    return true;
}
