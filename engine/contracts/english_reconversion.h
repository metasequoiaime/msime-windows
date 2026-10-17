#pragma once
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace FanyImeEnglishReconversion
{
// Offsets into the original bounded text. Never cross a line or consume spaces
// when replacing the word. Refuse truncated long tokens and non-ASCII selections.
template <typename Char>
std::optional<std::pair<size_t, size_t>> WordRange(std::basic_string_view<Char> text, bool selected)
{
    const auto letter = [](Char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); };
    size_t end = text.size();
    size_t start = end;
    while (start > 0 && letter(text[start - 1]))
        --start;
    const size_t length = end - start;
    if (length < 3 || length > 24 || (selected && (start != 0 || end != text.size())) ||
        (!selected && start == 0 && text.size() == 64))
        return std::nullopt;
    return std::pair<size_t, size_t>{start, end};
}
} // namespace FanyImeEnglishReconversion
