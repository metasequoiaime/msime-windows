#include "english/windows_spell_checker.h"

#include <Windows.h>
#include <objbase.h>
#include <spellcheck.h>

#include <algorithm>
#include <cctype>

namespace
{
template <typename T> void SafeRelease(T *&pointer)
{
    if (pointer != nullptr)
    {
        pointer->Release();
        pointer = nullptr;
    }
}

bool IsPlainEnglishWord(const std::wstring &word)
{
    if (word.empty())
        return false;
    bool has_letter = false;
    for (const wchar_t ch : word)
    {
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z'))
            has_letter = true;
        else if (ch != L'\'' && ch != L'-')
            return false;
    }
    return has_letter;
}

bool EqualsIgnoringAsciiCase(const std::string &left, const std::string &right)
{
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
}
} // namespace

struct WindowsSpellChecker::Impl
{
    ISpellChecker *checker = nullptr;
};

WindowsSpellChecker::WindowsSpellChecker() : impl_(std::make_unique<Impl>())
{
    ISpellCheckerFactory *factory = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return;
    BOOL supported = FALSE;
    if (SUCCEEDED(factory->IsSupported(L"en-US", &supported)) && supported)
        (void)factory->CreateSpellChecker(L"en-US", &impl_->checker);
    SafeRelease(factory);
}

WindowsSpellChecker::~WindowsSpellChecker()
{
    SafeRelease(impl_->checker);
}

bool WindowsSpellChecker::available() const
{
    return impl_->checker != nullptr;
}

std::vector<std::string> WindowsSpellChecker::suggest(const std::string &word, std::size_t limit)
{
    std::vector<std::string> result;
    if (impl_->checker == nullptr || word.empty() || limit == 0)
        return result;

    // 调用方只传 ASCII 字母串，逐字节放宽成 UTF-16 即可。
    std::wstring wide;
    wide.reserve(word.size());
    for (const unsigned char ch : word)
        wide.push_back(static_cast<wchar_t>(ch));
    IEnumString *suggestions = nullptr;
    if (FAILED(impl_->checker->Suggest(wide.c_str(), &suggestions)) || suggestions == nullptr)
        return result;

    LPOLESTR text = nullptr;
    while (result.size() < limit && suggestions->Next(1, &text, nullptr) == S_OK)
    {
        const std::wstring suggestion = text != nullptr ? text : L"";
        CoTaskMemFree(text);
        text = nullptr;
        if (!IsPlainEnglishWord(suggestion))
            continue;
        // IsPlainEnglishWord 已保证每个字符都是 ASCII。
        std::string narrow;
        narrow.reserve(suggestion.size());
        for (const wchar_t ch : suggestion)
            narrow.push_back(static_cast<char>(ch));
        if (EqualsIgnoringAsciiCase(narrow, word) || std::find(result.begin(), result.end(), narrow) != result.end())
            continue;
        result.push_back(std::move(narrow));
    }
    SafeRelease(suggestions);
    return result;
}
