#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "voice-input/voice_providers.h"
#include "tests/includes/test_framework.h"

TEST_CASE(voice_providers_resolve_openai_compatible_defaults)
{
    REQUIRE(VoiceInput::IsDoubaoAsrProvider("doubao"));
    REQUIRE(VoiceInput::IsDoubaoAsrProvider("Doubao"));
    REQUIRE(!VoiceInput::IsDoubaoAsrProvider("openai"));

    REQUIRE_EQ(VoiceInput::DefaultAsrEndpoint("openai"), "https://api.openai.com/v1/audio/transcriptions");
    REQUIRE_EQ(VoiceInput::DefaultAsrModel("openai"), "whisper-1");
    REQUIRE_EQ(VoiceInput::DefaultAsrModel("siliconflow"), "FunAudioLLM/SenseVoiceSmall");
    // MiniMax 语音转文字：multipart file+model，响应 {text}，与现有解析兼容（实测 HTTP 200）。
    REQUIRE_EQ(VoiceInput::DefaultAsrEndpoint("minimax"), "https://api.minimax.cn/v1/speech_to_text");
    REQUIRE_EQ(VoiceInput::DefaultAsrModel("minimax"), "asr-1.0");
    REQUIRE_EQ(VoiceInput::AsrTokenSlotKey("minimax"), "asr_token_minimax");
    REQUIRE_EQ(VoiceInput::AsrTokenSlotKey("siliconflow"), "asr_token_siliconflow");
    REQUIRE(VoiceInput::IsPlaceholderToken("<YOUR_OWN_DOUBAO_ACCESS_TOKEN>"));
    {
        VoiceInputConfig config;
        config.asr_provider = "doubao";
        config.asr_token = "sk-should-not-be-used";
        config.asr_tokens["doubao"] = "doubao-access";
        config.asr_tokens["siliconflow"] = "sk-siliconflow";
        REQUIRE_EQ(VoiceInput::ResolveAsrToken(config), "doubao-access");
        config.asr_provider = "siliconflow";
        REQUIRE_EQ(VoiceInput::ResolveAsrToken(config), "sk-siliconflow");
        config.asr_provider = "groq";
        REQUIRE_EQ(VoiceInput::ResolveAsrToken(config), "");
        config.asr_provider = "siliconflow";
        REQUIRE_EQ(VoiceInput::ResolveAsrModel(config), "FunAudioLLM/SenseVoiceSmall");
        config.asr_model = "custom/asr-model";
        REQUIRE_EQ(VoiceInput::ResolveAsrModel(config), "custom/asr-model");
    }
    {
        VoiceInputConfig config;
        config.polish_provider = "deepseek";
        config.polish_token = "sk-should-not-be-used";
        config.polish_tokens["siliconflow"] = "sk-sf-polish";
        config.polish_tokens["deepseek"] = "sk-deepseek";
        REQUIRE_EQ(VoiceInput::ResolvePolishToken(config), "sk-deepseek");
        config.polish_provider = "siliconflow";
        REQUIRE_EQ(VoiceInput::ResolvePolishToken(config), "sk-sf-polish");
    }
    REQUIRE_EQ(VoiceInput::DefaultPolishEndpoint("deepseek"), "https://api.deepseek.com/chat/completions");
    REQUIRE_EQ(VoiceInput::DefaultPolishModel("openai"), "gpt-4o-mini");
}

// The new console issues one API Key sent as X-Api-Key; the legacy console needs App ID +
// Access Token. Picking the wrong header set fails authentication, so the fallback for configs
// written before doubao_auth_mode existed has to key off a *usable* App ID, not a placeholder.
TEST_CASE(voice_providers_select_doubao_auth_mode)
{
    REQUIRE_EQ(VoiceInput::NormalizeDoubaoAuthMode("api_key", "real-app-id"), "api_key");
    REQUIRE_EQ(VoiceInput::NormalizeDoubaoAuthMode("legacy", ""), "legacy");
    REQUIRE_EQ(VoiceInput::NormalizeDoubaoAuthMode("LEGACY", ""), "legacy");

    // Absent or unrecognised setting: infer from the App ID.
    REQUIRE_EQ(VoiceInput::NormalizeDoubaoAuthMode("", "real-app-id"), "legacy");
    REQUIRE_EQ(VoiceInput::NormalizeDoubaoAuthMode("", ""), "api_key");
    REQUIRE_EQ(VoiceInput::NormalizeDoubaoAuthMode("nonsense", "real-app-id"), "legacy");
    // The shipped placeholders must not be mistaken for a configured legacy console.
    REQUIRE_EQ(VoiceInput::NormalizeDoubaoAuthMode("", "<YOUR_OWN_DOUBAO_APP_ID>"), "api_key");
    REQUIRE_EQ(VoiceInput::NormalizeDoubaoAuthMode("", "<YOUR_ASR_APP_KEY>"), "api_key");

    VoiceInputConfig config;
    config.doubao_auth_mode = "api_key";
    config.asr_app_key = "real-app-id";
    REQUIRE(!VoiceInput::UsesDoubaoLegacyAuth(config));
    config.doubao_auth_mode = "legacy";
    REQUIRE(VoiceInput::UsesDoubaoLegacyAuth(config));
    config.doubao_auth_mode.clear();
    REQUIRE(VoiceInput::UsesDoubaoLegacyAuth(config));
    config.asr_app_key.clear();
    REQUIRE(!VoiceInput::UsesDoubaoLegacyAuth(config));
}

TEST_CASE(voice_providers_use_builtin_prompt_until_overridden)
{
    VoiceInputConfig config;
    config.polish_prompt_id = "faithful";
    const std::string faithful = VoiceInput::ResolvePolishSystemPrompt(config);
    REQUIRE(faithful.find("校对") != std::string::npos);
    REQUIRE(faithful.find("<asr_text>") != std::string::npos);

    config.polish_prompt = "只用这一句。";
    REQUIRE_EQ(VoiceInput::ResolvePolishSystemPrompt(config), "只用这一句。");

    REQUIRE_EQ(VoiceInput::WrapAsrUserMessage("明天开会"), "<asr_text>\n明天开会\n</asr_text>");
}

TEST_CASE(voice_providers_empty_custom_falls_back_to_cleanup)
{
    VoiceInputConfig config;
    config.polish_prompt_id = "custom_2";
    config.polish_prompt.clear();
    config.polish_prompt_custom_2 = "第二个自定义提示。";
    REQUIRE_EQ(VoiceInput::ResolvePolishSystemPrompt(config), "第二个自定义提示。");
    config.polish_prompt_custom_2.clear();
    const std::string prompt = VoiceInput::ResolvePolishSystemPrompt(config);
    REQUIRE(prompt.find("整理助手") != std::string::npos);
}

// 小米 MiMo：ASR 与润色共用一个聊天补全网关。ASR 侧的转写走 chat 协议（base64 input_audio），
// 由接口地址而不是供应商标签判定，这样「自定义」槽位指向同一网关时行为一致。
TEST_CASE(voice_providers_mimo_defaults_and_chat_transcription_detection)
{
    REQUIRE_EQ(VoiceInput::DefaultAsrEndpoint("mimo"), "https://token-plan-cn.xiaomimimo.com/v1/chat/completions");
    REQUIRE_EQ(VoiceInput::DefaultAsrModel("mimo"), "mimo-v2.5-asr");
    REQUIRE_EQ(VoiceInput::AsrTokenSlotKey("mimo"), "asr_token_mimo");
    REQUIRE_EQ(VoiceInput::DefaultPolishEndpoint("mimo"), "https://token-plan-cn.xiaomimimo.com/v1/chat/completions");
    REQUIRE_EQ(VoiceInput::DefaultPolishModel("mimo"), "mimo-v2.6-flash");
    REQUIRE_EQ(VoiceInput::PolishTokenSlotKey("mimo"), "polish_token_mimo");

    REQUIRE(VoiceInput::UsesChatTranscription("https://token-plan-cn.xiaomimimo.com/v1/chat/completions"));
    REQUIRE(VoiceInput::UsesChatTranscription("https://example.com/v1/chat/completions?api-version=1"));
    REQUIRE(VoiceInput::UsesChatTranscription("https://example.com/v1/chat/completions/"));
    REQUIRE(!VoiceInput::UsesChatTranscription("https://api.openai.com/v1/audio/transcriptions"));
    REQUIRE(!VoiceInput::UsesChatTranscription("https://api.minimax.cn/v1/speech_to_text"));
    REQUIRE(!VoiceInput::UsesChatTranscription(""));
}
