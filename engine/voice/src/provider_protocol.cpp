#include "provider_protocol.h"
#include "stt_service.h"
#include <nlohmann/json.hpp>
#include <atomic>

namespace metasequoia::voice
{
namespace
{
constexpr std::size_t maximum_response_bytes = 1024 * 1024;
nlohmann::json parse_response(std::string_view response)
{
    if (response.size() > maximum_response_bytes)
        throw VoiceError("Voice response is too large");
    try
    {
        return nlohmann::json::parse(response.begin(), response.end());
    }
    catch (const nlohmann::json::exception &)
    {
        throw VoiceError("Invalid voice response");
    }
}
std::string text_member(const nlohmann::json &object, const char *key)
{
    if (!object.is_object())
        return {};
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}
std::string base64_encode(std::string_view data)
{
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3)
    {
        unsigned value = static_cast<unsigned char>(data[i]) << 16;
        if (i + 1 < data.size())
            value |= static_cast<unsigned char>(data[i + 1]) << 8;
        if (i + 2 < data.size())
            value |= static_cast<unsigned char>(data[i + 2]);
        out.push_back(kAlphabet[(value >> 18) & 63]);
        out.push_back(kAlphabet[(value >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? kAlphabet[(value >> 6) & 63] : '=');
        out.push_back(i + 2 < data.size() ? kAlphabet[value & 63] : '=');
    }
    return out;
}
} // namespace
MultipartRequest make_transcription_request(std::string_view wav, std::string_view model, std::string_view language)
{
    if (wav.empty() || wav.size() > maximum_encoded_audio_bytes || model.empty())
        throw VoiceError("Audio and transcription model are required within the upload limit");
    // A boundary must not occur in any field, including the arbitrary binary payload.
    static std::atomic<unsigned long long> sequence{0};
    std::string boundary;
    do
    {
        boundary = "----MetasequoiaVoice" + std::to_string(sequence.fetch_add(1));
    } while (wav.find(boundary) != std::string_view::npos || model.find(boundary) != std::string_view::npos || language.find(boundary) != std::string_view::npos);
    MultipartRequest request{"multipart/form-data; boundary=" + boundary, {}};
    auto &body = request.body;
    body.reserve(wav.size() + model.size() + language.size() + 512);
    const auto field = [&](const char *name, std::string_view value) {
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n";
        body.append(value.data(), value.size());
        body += "\r\n";
    };
    field("model", model);
    if (!language.empty())
        field("language", language);
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    body.append(wav.data(), wav.size());
    body += "\r\n--" + boundary + "--\r\n";
    return request;
}
std::string make_chat_transcription_request(std::string_view wav, std::string_view model)
{
    if (wav.empty() || wav.size() > maximum_encoded_audio_bytes || model.empty())
        throw VoiceError("Audio and transcription model are required within the upload limit");
    nlohmann::json input_audio;
    input_audio["data"] = base64_encode(wav);
    input_audio["format"] = "wav";
    nlohmann::json part;
    part["type"] = "input_audio";
    part["input_audio"] = input_audio;
    nlohmann::json message;
    message["role"] = "user";
    message["content"] = nlohmann::json::array({part});
    nlohmann::json body;
    body["model"] = std::string(model);
    body["stream"] = false;
    body["messages"] = nlohmann::json::array({message});
    return body.dump();
}
std::string parse_transcription(std::string_view response)
{
    const auto json = parse_response(response);
    for (const char *key : {"text", "transcription"})
    {
        auto text = text_member(json, key);
        if (!text.empty())
            return text;
    }
    if (json.is_object() && json.contains("result"))
    {
        auto text = text_member(json["result"], "text");
        if (!text.empty())
            return text;
    }
    // Chat-completions gateways answer with the transcript as the assistant message. Taken
    // verbatim: this is recogniser output, not a model answer, so no reasoning stripping here.
    if (json.is_object() && json.contains("choices") && json["choices"].is_array() && !json["choices"].empty())
    {
        const auto &first = json["choices"][0];
        if (first.is_object() && first.contains("message"))
        {
            auto text = text_member(first["message"], "content");
            if (!text.empty())
                return text;
        }
    }
    throw VoiceError("Missing transcription text");
}
std::string make_polish_request(std::string_view model, std::string_view system_prompt, std::string_view user_message)
{
    if (model.empty() || system_prompt.empty() || user_message.empty())
        throw VoiceError("Polish model, prompt and text are required");
    try
    {
        return nlohmann::json{{"model", std::string(model)}, {"stream", false}, {"messages", {{{"role", "system"}, {"content", std::string(system_prompt)}}, {{"role", "user"}, {"content", std::string(user_message)}}}}}.dump();
    }
    catch (const nlohmann::json::exception &)
    {
        throw VoiceError("Invalid polish request text");
    }
}
std::string parse_polished_text(std::string_view response)
{
    const auto json = parse_response(response);
    try
    {
        const auto text = json.at("choices").at(0).at("message").at("content").get<std::string>();
        if (!text.empty())
        {
            const auto clean = strip_reasoning(text);
            // 剥完之后什么都不剩（整段都是思考，或思考被截断）也按缺文本处理：
            // 抛错让调用方回退到原始 ASR 文本，而不是把空串当润色结果提交。
            if (!clean.empty())
                return clean;
        }
    }
    catch (const nlohmann::json::exception &)
    {
        throw VoiceError("Missing polished text");
    }
    throw VoiceError("Missing polished text");
}

std::string strip_reasoning(std::string_view text)
{
    std::string result(text);
    constexpr std::string_view open_tag = "<think>";
    constexpr std::string_view close_tag = "</think>";
    for (;;)
    {
        const size_t begin = result.find(open_tag);
        if (begin == std::string::npos)
            break;
        const size_t end = result.find(close_tag, begin);
        if (end == std::string::npos)
        {
            // 没有闭合标签就没有可靠边界：max_tokens 截断的半截思考比错误答案更危险，
            // 从 <think> 起全部丢弃，让调用方回退到原始 ASR 文本。
            result.erase(begin, result.size() - begin);
            break;
        }
        result.erase(begin, end - begin + close_tag.size());
    }
    // 剥完通常剩下开头的换行和缩进，一并清掉。
    const size_t first = result.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    result.erase(0, first);
    return result;
}
} // namespace metasequoia::voice
