#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace metasequoia::voice
{
// Platform transports may use these codecs without duplicating provider wire formats.
// Audio is an already encoded WAV; recording duration/format policy belongs to the host.
inline constexpr std::size_t maximum_encoded_audio_bytes = 20 * 1024 * 1024;
struct MultipartRequest
{
    std::string content_type;
    std::string body;
};
MultipartRequest make_transcription_request(std::string_view wav, std::string_view model, std::string_view language = {});
// Chat-completions transcription (MiMo-style gateways): the clip travels as base64 input_audio in
// a chat body. These gateways reject multipart uploads and extra text parts - their gateway injects
// its own transcription prompt - so language is deliberately not part of the request.
std::string make_chat_transcription_request(std::string_view wav, std::string_view model);
// Accepts the OpenAI transcription shapes (text / transcription / result.text) and, as a fallback,
// a chat completion whose assistant message carries the transcript verbatim (no reasoning strip:
// transcribed speech must stay faithful).
std::string parse_transcription(std::string_view response);
// user_message is sent verbatim. The caller owns prompt policy and any ASR delimiters.
std::string make_polish_request(std::string_view model, std::string_view system_prompt, std::string_view user_message);
std::string parse_polished_text(std::string_view response);
// 推理模型（DeepSeek R 系、MiniMax M3 系这类不许关思考的）会把思考过程混在 content 里带回来。
// 剥掉成对的 <think>…</think>；未闭合的 <think> 之后没有可靠边界，全部视为思考残留。
// 输入法只上屏答案。
std::string strip_reasoning(std::string_view text);
} // namespace metasequoia::voice
