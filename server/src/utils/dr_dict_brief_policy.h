#pragma once

// DR Dict 简短释义（英文候选模式里高亮候选下方的释义）的纯函数部分：请求负载与回复解析。
//
// 协议来自 DR Dict（app/src/popup.h 的 brief_request / brief_reply、docs/architecture.md「供其他程序调用」）：
//   - 向窗口类 DrDictPopup 发 WM_COPYDATA，dwData = 0x46425244（"DRBF"），数据是 UTF-8 JSON
//     {"word": "...", "reply": 回复窗口句柄（整数）, "id": 请求号, "max": 字数上限}；请求立即返回，不开弹窗；
//   - DR Dict 查完向 reply 窗口发 WM_COPYDATA，dwData = 0x52425244（"DRBR"），数据是
//     {"id", "word", "found", "headword", "dictionary", "text"}。
//   - 请求另带两个可选字段：「"brief": false」不要摘要，「"phonetics": true」要音标。要了音标时回复里多一个
//     "phonetics": {"uk", "us", "dictionary"}（英式、美式，不带斜杠；查不到是 {}）。旧版 DR Dict 不认这两个
//     字段，照旧只回摘要、回复里没有 phonetics，据此退回本地音标。
// 与 dr_dict_lookup_policy.h 一样，只用简单的窗口消息，不另开管道。

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace DrDictBrief
{
inline constexpr std::uintptr_t kBriefRequest = 0x46425244; // "DRBF"
inline constexpr std::uintptr_t kBriefReply = 0x52425244;   // "DRBR"
// 候选窗一行放得下的长度；DR Dict 那边超过就截断并以「…」结尾。
inline constexpr std::size_t kMaxChars = 80;
// 回复负载的上限。DR Dict 只回一段摘要，远小于此；更长的不是它发的，直接丢掉。
inline constexpr std::size_t kMaxReplyBytes = 16 * 1024;

inline std::string BuildRequestPayload(const std::string &word, std::int64_t reply_window, std::uint64_t id,
                                       std::size_t max_chars = kMaxChars, bool brief = true, bool phonetics = false)
{
    nlohmann::json payload = {{"word", word}, {"reply", reply_window}, {"id", id}, {"max", max_chars}};
    // 只在和默认不同时带上，请求和旧版一字不差。
    if (!brief)
        payload["brief"] = false;
    if (phonetics)
        payload["phonetics"] = true;
    // 词来自英文候选，只有 ASCII；replace 防止异常字节让 dump 抛异常。
    return payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

struct Reply
{
    std::uint64_t id = 0;
    std::string word;
    bool found = false;
    std::string text;
    // 回复里有 phonetics 对象（DR Dict 认这个字段）；uk / us 可以都为空（查不到）。
    bool has_phonetics = false;
    std::string uk;
    std::string us;
};

// 解析失败（不是 JSON、缺 id 或 word、类型不对）返回空；没查到时 found 为假、text 为空。
inline std::optional<Reply> ParseReply(std::string_view payload)
{
    if (payload.empty() || payload.size() > kMaxReplyBytes)
        return std::nullopt;
    const auto json = nlohmann::json::parse(payload, nullptr, false);
    if (!json.is_object() || !json.contains("id") || !json["id"].is_number_integer() || !json.contains("word") ||
        !json["word"].is_string())
        return std::nullopt;
    Reply reply;
    reply.id = json["id"].get<std::uint64_t>();
    reply.word = json["word"].get<std::string>();
    reply.found = json.contains("found") && json["found"].is_boolean() && json["found"].get<bool>();
    if (reply.found && json.contains("text") && json["text"].is_string())
        reply.text = json["text"].get<std::string>();
    if (reply.text.empty())
        reply.found = false;
    if (json.contains("phonetics") && json["phonetics"].is_object())
    {
        const auto &phonetics = json["phonetics"];
        reply.has_phonetics = true;
        const auto read = [&phonetics](const char *key) {
            return phonetics.contains(key) && phonetics[key].is_string() ? phonetics[key].get<std::string>()
                                                                         : std::string();
        };
        reply.uk = read("uk");
        reply.us = read("us");
    }
    return reply;
}
} // namespace DrDictBrief
