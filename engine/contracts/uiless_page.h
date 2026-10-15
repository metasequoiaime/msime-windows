#pragma once

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cstdlib>
#include <string>

// Candidate page handed to a UI-less (host-drawn) TSF client, carried by FanyImeReplyType::UiLessComposition and
// FanyImeWorkerReplyType::UiLessCandidatesChanged:
//
//   <preedit> \t <page> \t <selection> \t <generation>
//
// page is the current page's words joined by ','; selection is page-relative. generation is the Server's candidate
// page generation: the worker push and the key reply travel on different pipes, so the client applies a page only
// when it is not older than the one it already shows. Generation 0 means "unknown" (a Server predating the field)
// and is always applied. DLLs that predate the field read selection with _wtoi, which stops at the third tab.
namespace FanyImeUiLessPage
{
struct Payload
{
    std::wstring preedit;
    std::wstring page;
    int selection = 0;
    std::uint64_t generation = 0;
    // The payload carried a page and selection, i.e. it was built for a UI-less client.
    bool hasPage = false;
};

inline std::wstring Format(const std::wstring &preedit, const std::wstring &page, int selection,
                           std::uint64_t generation)
{
    return preedit + L'\t' + page + L'\t' + std::to_wstring(selection) + L'\t' + std::to_wstring(generation);
}

inline Payload Parse(const std::wstring &payload)
{
    Payload result;
    const std::size_t firstTab = payload.find(L'\t');
    if (firstTab == std::wstring::npos)
    {
        result.preedit = payload;
        return result;
    }
    result.preedit = payload.substr(0, firstTab);
    const std::size_t secondTab = payload.find(L'\t', firstTab + 1);
    if (secondTab == std::wstring::npos)
    {
        result.page = payload.substr(firstTab + 1);
        return result;
    }
    result.page = payload.substr(firstTab + 1, secondTab - firstTab - 1);
    result.hasPage = true;
    result.selection = static_cast<int>(std::wcstol(payload.c_str() + secondTab + 1, nullptr, 10));
    const std::size_t thirdTab = payload.find(L'\t', secondTab + 1);
    if (thirdTab != std::wstring::npos)
    {
        result.generation = std::wcstoull(payload.c_str() + thirdTab + 1, nullptr, 10);
    }
    return result;
}

// True when a page tagged `incoming` may replace the one tagged `shown`.
inline bool IsCurrent(std::uint64_t incoming, std::uint64_t shown)
{
    return incoming == 0 || incoming >= shown;
}
} // namespace FanyImeUiLessPage
