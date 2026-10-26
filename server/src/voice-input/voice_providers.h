#pragma once

#include "config/ime_config.h"

#include <string>
#include <string_view>
#include <vector>

namespace VoiceInput
{
struct PolishPromptPreset
{
    std::string_view id;
    std::string_view name;
    std::string_view prompt;
};

// Doubao console generations. The new console issues a single API Key sent as X-Api-Key;
// the legacy console issues App ID + Access Token sent as X-Api-App-Key + X-Api-Access-Key.
// Both are accepted by the same endpoints, so this only selects which headers to send.
inline constexpr std::string_view kDoubaoAuthApiKey = "api_key";
inline constexpr std::string_view kDoubaoAuthLegacy = "legacy";

// Streaming endpoints. bigmodel_nostream streams audio up but returns whole-sentence
// results; Volcengine documents it as more accurate and recommends it for IME input.
inline constexpr std::string_view kDoubaoEndpointNostream =
    "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_nostream";
inline constexpr std::string_view kDoubaoEndpointAsync = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async";

bool IsDoubaoAsrProvider(std::string_view provider);
std::string NormalizeProviderId(std::string_view provider);
// MiMo-style transcription: an ASR endpoint that is a Chat Completions URL takes the clip as
// base64 input_audio in a chat body instead of an OpenAI multipart upload. Keyed off the endpoint
// rather than the provider label so a "custom" slot pointed at such a gateway behaves the same.
bool UsesChatTranscription(std::string_view endpoint);
// DeepSeek thinking levels: disabled | low | high | max (the latter three are sent as reasoning_effort).
bool IsValidThinkingLevel(std::string_view level);
// True only for DeepSeek with a level other than disabled; every other case keeps thinking off.
bool UsesDeepSeekThinking(std::string_view provider, std::string_view level);

// Which service actually answers at an endpoint. Vendor-specific request fields (DeepSeek's
// thinking.type, SiliconFlow's enable_thinking) have to key off this rather than off the provider
// label chosen in the settings UI: a slot can hold any OpenAI-compatible endpoint, and sending
// thinking.type to a service that does not accept it makes it reject the whole request - MiniMax
// answers "model requires adaptive thinking" and the suggestion never comes back.
enum class ChatVendor
{
    Other,
    DeepSeek,
    SiliconFlow,
    Groq,
    OpenAI
};

ChatVendor VendorFromChatEndpoint(std::string_view endpoint);

// The vendor that owns the endpoint, or ChatVendor::Other when the label says something the
// endpoint does not. Used to keep DeepSeek/SiliconFlow fields off foreign endpoints.
ChatVendor VendorForRequest(std::string_view provider, std::string_view endpoint);

// Same question as UsesDeepSeekThinking, but resolved from the endpoint that will really be
// called, so a mislabelled slot cannot turn thinking on (or off) for the wrong service.
bool UsesDeepSeekThinking(std::string_view provider, std::string_view endpoint, std::string_view level);
// Resolves an explicit doubao_auth_mode, falling back to inferring the legacy console from a
// usable App ID when the key is absent (configs written before the setting existed).
std::string NormalizeDoubaoAuthMode(std::string_view mode, std::string_view app_key);
bool UsesDoubaoLegacyAuth(const VoiceInputConfig &config);
bool IsPlaceholderToken(std::string_view token);
std::string UsableToken(std::string_view token);
std::string AsrTokenSlotKey(std::string_view provider);
std::string PolishTokenSlotKey(std::string_view provider);
std::string ResolveAsrToken(const VoiceInputConfig &config);
std::string ResolvePolishToken(const VoiceInputConfig &config);
const std::vector<std::string_view> &AsrProviders();
const std::vector<std::string_view> &PolishProviders();
std::string DefaultAsrEndpoint(std::string_view provider);
std::string DefaultAsrModel(std::string_view provider);
std::string DefaultPolishEndpoint(std::string_view provider);
std::string DefaultPolishModel(std::string_view provider);
std::string ResolveAsrModel(const VoiceInputConfig &config);
std::string ResolvePolishModel(const VoiceInputConfig &config);
const std::vector<PolishPromptPreset> &BuiltinPolishPromptPresets();
std::string ResolvePolishSystemPrompt(const VoiceInputConfig &config);
std::string WrapAsrUserMessage(const std::string &asr_text);
} // namespace VoiceInput
