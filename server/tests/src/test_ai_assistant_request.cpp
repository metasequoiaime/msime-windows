#include "ai/ai_assistant_request.h"
#include "settings/api_credential_test_request.h"
#include "tests/includes/test_framework.h"
#include "voice-input/voice_providers.h"

#include <msime/voice/provider_protocol.h>

namespace
{
// 厂商字段现在按接口地址判断，所以构造请求时必须显式给地址；默认给一个 DeepSeek 地址，
// 让"标签=deepseek"的历史用例保持原意。
AiAssistant::Request MakeRequest(const std::string &provider, const std::string &thinking,
                                 const std::string &endpoint = "https://api.deepseek.com/chat/completions",
                                 const std::string &json_mode = "force")
{
    AiAssistant::Request request;
    request.pinyin_segments = {"shang", "hai"};
    request.context = "我准备去";
    request.config.provider = provider;
    request.config.model = "example-model";
    request.config.endpoint = endpoint;
    request.config.prompt = "Return the best candidate.";
    request.config.thinking = thinking;
    request.config.json_mode = json_mode;
    return request;
}
} // namespace

TEST_CASE(ai_request_disables_deepseek_thinking_by_default)
{
    const auto body = AiAssistant::detail::BuildRequestBody(MakeRequest("deepseek", "disabled"));
    REQUIRE_EQ(body["thinking"]["type"].get<std::string>(), "disabled");
    REQUIRE(!body.contains("reasoning_effort"));
    REQUIRE_EQ(body["max_tokens"].get<int>(), 512);
    REQUIRE(!body.contains("max_completion_tokens"));
    REQUIRE_EQ(body["temperature"].get<double>(), 0.2);
    REQUIRE_EQ(body["response_format"]["type"].get<std::string>(), "json_object");
    REQUIRE_EQ(AiAssistant::detail::RequestTimeoutMs(MakeRequest("deepseek", "disabled").config), 8000L);
}

TEST_CASE(ai_request_sends_deepseek_thinking_level)
{
    for (const char *level : {"low", "high", "max"})
    {
        const auto request = MakeRequest("deepseek", level);
        const auto body = AiAssistant::detail::BuildRequestBody(request);
        REQUIRE_EQ(body["thinking"]["type"].get<std::string>(), "enabled");
        REQUIRE_EQ(body["reasoning_effort"].get<std::string>(), std::string(level));
        REQUIRE(body["max_tokens"].get<int>() > 512);
        REQUIRE(!body.contains("max_completion_tokens"));
        REQUIRE_EQ(body["temperature"].get<double>(), 0.2);
        REQUIRE(AiAssistant::detail::RequestTimeoutMs(request.config) > 8000L);
    }
}

TEST_CASE(ai_request_ignores_thinking_level_for_other_providers)
{
    // A plain OpenAI-compatible endpoint stays a plain request, thinking level or not.
    const auto request = MakeRequest("custom", "high", "https://relay.example.com/v1/chat/completions");
    const auto body = AiAssistant::detail::BuildRequestBody(request);
    REQUIRE(!body.contains("thinking"));
    REQUIRE(!body.contains("reasoning_effort"));
    REQUIRE_EQ(body["max_tokens"].get<int>(), 512);
    REQUIRE_EQ(AiAssistant::detail::RequestTimeoutMs(request.config), 8000L);
}

TEST_CASE(ai_request_follows_the_endpoint_not_the_provider_label)
{
    // 标签写着 deepseek、地址却是别的服务：带着 thinking.type 会被对方直接拒绝
    // （MiniMax 返回 2013 requires adaptive thinking），所以必须按地址判断。
    const auto foreign = MakeRequest("deepseek", "high", "https://api.minimax.cn/v1/chat/completions");
    const auto foreign_body = AiAssistant::detail::BuildRequestBody(foreign);
    REQUIRE(!foreign_body.contains("thinking"));
    REQUIRE(!foreign_body.contains("reasoning_effort"));
    REQUIRE_EQ(AiAssistant::detail::RequestTimeoutMs(foreign.config), 8000L);

    // 反过来：custom 槽指向真正的 DeepSeek 时，thinking 依然可用。
    const auto real = MakeRequest("custom", "high", "https://api.deepseek.com/chat/completions");
    const auto real_body = AiAssistant::detail::BuildRequestBody(real);
    REQUIRE_EQ(real_body["thinking"]["type"].get<std::string>(), "enabled");
    REQUIRE_EQ(real_body["reasoning_effort"].get<std::string>(), "high");
    REQUIRE(AiAssistant::detail::RequestTimeoutMs(real.config) > 8000L);
}

TEST_CASE(ai_request_uses_openai_completion_limit_for_older_models)
{
    auto request = MakeRequest("openai", "disabled", "https://api.openai.com/v1/chat/completions");
    request.config.model = "gpt-4o-mini";
    const auto body = AiAssistant::detail::BuildRequestBody(request);
    REQUIRE_EQ(body["max_completion_tokens"].get<int>(), 512);
    REQUIRE(!body.contains("max_tokens"));
    REQUIRE_EQ(body["temperature"].get<double>(), 0.2);
    REQUIRE_EQ(body["response_format"]["type"].get<std::string>(), "json_object");
    REQUIRE(!body.contains("reasoning_effort"));
    REQUIRE(!body.contains("thinking"));
    REQUIRE_EQ(AiAssistant::detail::RequestTimeoutMs(request.config), 8000L);
}

TEST_CASE(ai_request_omits_unsupported_sampling_for_openai_reasoning_models)
{
    for (const char *model : {"gpt-5.6-luna", "GPT-5.6-Luna", "gpt-5", "o3-mini", "O3-mini"})
    {
        auto request = MakeRequest("OpenAI", "disabled", "https://api.openai.com/v1/chat/completions");
        request.config.model = model;
        const auto body = AiAssistant::detail::BuildRequestBody(request);
        REQUIRE_EQ(body["model"].get<std::string>(), std::string(model));
        REQUIRE_EQ(body["max_completion_tokens"].get<int>(), 8192);
        REQUIRE(!body.contains("max_tokens"));
        REQUIRE(!body.contains("temperature"));
        REQUIRE_EQ(body["reasoning_effort"].get<std::string>(), "low");
        REQUIRE_EQ(body["response_format"]["type"].get<std::string>(), "json_object");
        REQUIRE(!body.contains("thinking"));
        REQUIRE(AiAssistant::detail::RequestTimeoutMs(request.config) > 8000L);
    }
}

TEST_CASE(ai_request_uses_json_mode_strategy)
{
    // 自动档：本机地址不带 response_format，原生实现带上。
    const auto local = MakeRequest("deepseek", "disabled", "http://127.0.0.1:11434/v1/chat/completions", "auto");
    const auto local_body = AiAssistant::detail::BuildRequestBody(local);
    REQUIRE(!local_body.contains("response_format"));
    // 标签写着 deepseek，地址却是本机：thinking 同样不能跟着标签走。
    REQUIRE(!local_body.contains("thinking"));

    const auto native = MakeRequest("deepseek", "disabled", "https://api.deepseek.com/chat/completions", "auto");
    REQUIRE(AiAssistant::detail::BuildRequestBody(native).contains("response_format"));

    // 兼容档一律不带，强制档一律带上。
    const auto off = MakeRequest("deepseek", "disabled", "https://api.deepseek.com/chat/completions", "off");
    REQUIRE(!AiAssistant::detail::BuildRequestBody(off).contains("response_format"));
    const auto force = MakeRequest("deepseek", "disabled", "http://127.0.0.1:1234/v1/chat/completions", "force");
    REQUIRE(AiAssistant::detail::BuildRequestBody(force).contains("response_format"));
}

TEST_CASE(ai_request_keeps_custom_provider_request_shape)
{
    auto request = MakeRequest("custom", "high", "https://relay.example.com/v1/chat/completions");
    request.config.model = "gpt-5.6-luna";
    const auto body = AiAssistant::detail::BuildRequestBody(request);
    REQUIRE_EQ(body["max_tokens"].get<int>(), 512);
    REQUIRE(!body.contains("max_completion_tokens"));
    REQUIRE_EQ(body["temperature"].get<double>(), 0.2);
    REQUIRE_EQ(body["response_format"]["type"].get<std::string>(), "json_object");
    REQUIRE(!body.contains("reasoning_effort"));
    REQUIRE(!body.contains("thinking"));
}

namespace
{
AiAssistantConfig ProbeConfig(const std::string &provider, const std::string &model, const std::string &endpoint,
                              const std::string &json_mode = "auto", const std::string &thinking = "disabled")
{
    AiAssistantConfig config;
    config.provider = provider;
    config.model = model;
    config.endpoint = endpoint;
    config.prompt = "Return the best candidate as JSON.";
    config.json_mode = json_mode;
    config.thinking = thinking;
    return config;
}
} // namespace

// 探测请求必须就是真实联想请求。以前两边各写一份，探测没有 system 提示词、没有
// response_format、上限 1，验收却按联想格式卡，必然误报（issue #1265）。
TEST_CASE(credential_test_probe_is_the_real_assistant_request)
{
    for (const char *model : {"gpt-4o-mini", "gpt-5.6-luna", "o3-mini", "deepseek-v4-flash", "Qwen/Qwen3-8B"})
    {
        const auto config = ProbeConfig("openai", model, "https://api.openai.com/v1/chat/completions");
        const auto probe = ApiCredentialTest::detail::BuildChatProbeRequestBody(config);
        const auto real =
            AiAssistant::detail::BuildRequestBody(ApiCredentialTest::detail::BuildChatProbeRequest(config));
        REQUIRE_EQ(probe, real);
        // 真实的 system 提示词要在场，模型才知道要回 candidates JSON。
        REQUIRE_EQ(probe["messages"][0]["role"].get<std::string>(), "system");
        REQUIRE_EQ(probe["messages"][0]["content"].get<std::string>(), config.prompt);
        // 1 token 装不下一个候选，上限必须跟真实请求一致。
        REQUIRE(probe.contains("max_tokens") ? probe["max_tokens"].get<int>() >= 512
                                             : probe["max_completion_tokens"].get<int>() >= 512);
    }
}

TEST_CASE(credential_test_probe_follows_json_mode_and_timeout)
{
    const std::string endpoint = "https://api.deepseek.com/chat/completions";
    // 强制档一定带 response_format，兼容档一定不带——「输出格式」下拉现在真的影响探测。
    const auto force = ProbeConfig("deepseek", "deepseek-v4-flash", endpoint, "force");
    REQUIRE(ApiCredentialTest::detail::BuildChatProbeRequestBody(force).contains("response_format"));
    const auto off = ProbeConfig("deepseek", "deepseek-v4-flash", endpoint, "off");
    REQUIRE(!ApiCredentialTest::detail::BuildChatProbeRequestBody(off).contains("response_format"));

    // 推理模型在正式使用时等 30 秒，探测不能先用 15 秒判它超时。
    const auto reasoning = ProbeConfig("openai", "o3-mini", "https://api.openai.com/v1/chat/completions");
    REQUIRE_EQ(ApiCredentialTest::detail::ChatProbeTimeoutMs(reasoning),
               AiAssistant::detail::RequestTimeoutMs(reasoning));
    REQUIRE(ApiCredentialTest::detail::ChatProbeTimeoutMs(reasoning) > 15000L);
}

TEST_CASE(credential_test_probe_input_is_fixed)
{
    // 探测发的是固定样例，不是用户的真实输入，也不该受调用顺序影响。
    AiAssistantConfig config = ProbeConfig("deepseek", "deepseek-chat", "https://api.deepseek.com/chat/completions");
    const auto request = ApiCredentialTest::detail::BuildChatProbeRequest(config);
    REQUIRE_EQ(request.pinyin_segments, std::vector<std::string>({"shi", "shi"}));
    REQUIRE(!request.context.empty());
    REQUIRE_EQ(request.config.model, config.model);
}

// 润色探测的形状必须与引擎的 make_polish_request 一致。设置进程不链 Voice 库，
// 所以形状是复刻的；这条用例对着引擎实现比对，改一边忘了另一边会红。
TEST_CASE(credential_test_polish_probe_matches_engine_contract)
{
    const std::string prompt = "把口语整理成书面语。";
    const std::string user_text = ApiCredentialTest::detail::kPolishProbeText;
    const auto engine_body = nlohmann::json::parse(
        metasequoia::voice::make_polish_request("some-model", prompt, VoiceInput::WrapAsrUserMessage(user_text)));
    const VoiceInputConfig config;
    const auto probe_body = ApiCredentialTest::detail::BuildPolishProbeRequestBody(
        "some-model", prompt, "custom", "https://relay.example.com/v1/chat/completions", config);
    // 复刻的部分必须逐字段相等；探测在引擎形状之外只额外带 vendor 字段，且这里用的 custom
    // 地址不属于任何已知厂商，所以两者应当完全一样。
    REQUIRE_EQ(probe_body, engine_body);

    // 润色回复按纯文本解析，与引擎 parse_polished_text 同口径。
    const std::string reply = R"({"choices":[{"message":{"content":"整理后的句子。"}}]})";
    REQUIRE_EQ(ApiCredentialTest::detail::ParsePolishedText("整理后的句子。"),
               metasequoia::voice::parse_polished_text(reply));
}

TEST_CASE(credential_test_polish_probe_is_plain_text)
{
    const std::string prompt = "把口语整理成书面语，不要改变原意。";
    const VoiceInputConfig config;
    const auto body = ApiCredentialTest::detail::BuildPolishProbeRequestBody(
        "some-model", prompt, "custom", "https://relay.example.com/v1/chat/completions", config);
    REQUIRE_EQ(body["messages"][0]["role"].get<std::string>(), "system");
    REQUIRE_EQ(body["messages"][0]["content"].get<std::string>(), prompt);
    REQUIRE_EQ(body["messages"].size(), 2u);
    // 真实润色把转录文本包在 <asr_text> 里，探测也要一样，否则内置提示词对不上输入形态。
    REQUIRE_EQ(body["messages"][1]["content"].get<std::string>(),
               VoiceInput::WrapAsrUserMessage(ApiCredentialTest::detail::kPolishProbeText));
    // 润色的真实请求不带 response_format，探测也不能带。
    REQUIRE(!body.contains("response_format"));
    REQUIRE(!body.contains("max_tokens"));
    REQUIRE(!body.contains("max_completion_tokens"));

    // 入参与 FetchContent 的返回值一致，是 choices[0].message.content 的文本本身。
    REQUIRE_EQ(ApiCredentialTest::detail::ParsePolishedText("整理后的句子。"), std::string("整理后的句子。"));
    REQUIRE_EQ(ApiCredentialTest::detail::ParsePolishedText("  \n整理后的句子。"), std::string("整理后的句子。"));
    REQUIRE_EQ(ApiCredentialTest::detail::ParsePolishedText("<think>想一下</think>整理后的句子。"),
               std::string("整理后的句子。"));
    REQUIRE(ApiCredentialTest::detail::ParsePolishedText("<think>没闭合的思考").empty());
    REQUIRE(ApiCredentialTest::detail::ParsePolishedText("   ").empty());
    // 润色的验收只看「剥掉思考段后是不是有文本」，与引擎 parse_polished_text 同口径。
    // 引擎也不会因为模型把 JSON 当纯文本回出来就判失败，探测不在这里多造一条规则，
    // 否则又变成探测与正式使用标准不一致。这条断言钉的正是「两边同口径」。
    REQUIRE_EQ(ApiCredentialTest::detail::ParsePolishedText(R"({"candidates":[{"text":"实施"}]})"),
               std::string(R"({"candidates":[{"text":"实施"}]})"));
}
