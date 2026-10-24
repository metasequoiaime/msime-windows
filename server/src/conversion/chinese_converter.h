#pragma once

#include <string>

namespace ChineseConverter
{
// Converts simplified text to traditional. Returns the original text when the conversion
// resources cannot be loaded or the conversion fails; only an allocation failure
// (std::bad_alloc) can propagate.
std::string ToTraditional(const std::string &text);

// Converts traditional text to simplified. It is not an identity on simplified text: a few
// simplified words are rewritten as well (乾清宫 becomes 干清宫). Returns the original text when
// the conversion resources cannot be loaded or the conversion fails; only an allocation failure
// (std::bad_alloc) can propagate.
std::string ToSimplified(const std::string &text);
} // namespace ChineseConverter
