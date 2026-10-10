#pragma once

#include <fmt/xchar.h>

#include <string>
#include <vector>

// Candidate template arguments use slot 0 for the preedit text and slots 1-9
// for the candidates on the current page.
inline std::wstring EscapeCandidateTemplateText(const std::wstring &text)
{
    std::wstring escaped;
    escaped.reserve(text.size());
    for (wchar_t ch : text)
    {
        switch (ch)
        {
        case L'&':
            escaped += L"&amp;";
            break;
        case L'<':
            escaped += L"&lt;";
            break;
        case L'>':
            escaped += L"&gt;";
            break;
        case L'"':
            escaped += L"&quot;";
            break;
        default:
            escaped += ch;
            break;
        }
    }
    return escaped;
}

inline std::wstring InflateCandidateTemplate(const std::wstring &templ, std::vector<std::wstring> words)
{
    const int size = static_cast<int>(words.size());
    while (words.size() < 10)
    {
        words.push_back(L"");
    }

    std::wstring result = fmt::format(templ, words[0], words[1], words[2], words[3], words[4], words[5], words[6],
                                      words[7], words[8], words[9]);
    if (size < 10)
    {
        const size_t pos = result.find(fmt::format(L"<!--{}Anchor-->", size));
        result = result.substr(0, pos);
    }
    return result;
}
