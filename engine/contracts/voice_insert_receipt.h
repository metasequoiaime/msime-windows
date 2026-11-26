#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace FanyImeVoiceInsert
{
// The worker packet uses a terminated UTF-16 payload. Keep both IDs in its
// text prefix so the packet layout stays compatible with older clients.
constexpr std::size_t MaxIdChars = std::numeric_limits<std::uint64_t>::digits10 + 1;
constexpr std::size_t MaxPrefixChars = 2 * (MaxIdChars + 1);

inline std::wstring Encode(std::uint64_t receipt, std::uint64_t focus, std::wstring_view text)
{
    auto payload = std::to_wstring(receipt);
    payload += L'\t';
    payload += std::to_wstring(focus);
    payload += L'\t';
    payload.append(text);
    return payload;
}

struct Payload
{
    std::uint64_t receipt = 0;
    std::uint64_t focus = 0;
    std::wstring_view text;
};

inline std::uint64_t TakeId(std::wstring_view &payload)
{
    const auto split = payload.find(L'\t');
    if (split == 0 || split > MaxIdChars)
        return 0;

    std::uint64_t id = 0;
    for (const auto ch : payload.substr(0, split))
    {
        if (ch < L'0' || ch > L'9')
            return 0;
        const auto digit = static_cast<unsigned>(ch - L'0');
        if (id > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10)
            return 0;
        id = id * 10 + digit;
    }
    payload.remove_prefix(split + 1);
    return id;
}

inline Payload Decode(std::wstring_view payload)
{
    const auto receipt = TakeId(payload);
    const auto focus = TakeId(payload);
    if (receipt == 0 || focus == 0 || payload.empty())
        return {};
    return {receipt, focus, payload};
}
} // namespace FanyImeVoiceInsert
