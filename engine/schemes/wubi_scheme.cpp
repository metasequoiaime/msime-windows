#include "wubi_scheme.h"
#include <algorithm>
#include <cctype>

namespace
{
bool is_wubi_letter(char lower, bool mixed_pinyin_allowed, bool z_wildcard)
{
    // Both flags exist to let z in: mixed input because z is a pinyin letter, wildcard because
    // z stands for a code letter nobody knows. They are independent settings; accepting either
    // is the same one line either way.
    return (lower >= 'a' && lower <= 'y') || (lower == 'z' && (mixed_pinyin_allowed || z_wildcard));
}

// The code letters of input with their case kept, clipped to max_length: anything that is not a
// code letter (a z with mixed input and wildcard off, a digit, an operator) is dropped.
std::string wubi_code_letters(const std::string &input, size_t max_length, bool mixed_pinyin_allowed, bool z_wildcard)
{
    std::string letters;
    letters.reserve((std::min)(input.size(), max_length));
    for (const unsigned char ch : input)
    {
        if (letters.size() == max_length)
        {
            break;
        }
        if (std::isalpha(ch) && is_wubi_letter(static_cast<char>(std::tolower(ch)), mixed_pinyin_allowed, z_wildcard))
        {
            letters.push_back(static_cast<char>(ch));
        }
    }
    return letters;
}

std::string to_lower_ascii(std::string text)
{
    for (char &ch : text)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}
} // namespace

void WubiScheme::reset()
{
    raw_input_with_cases_.clear();
    raw_input_.clear();
    key_strokes_.clear();
    extended_length_allowed_ = false;
    literal_input_ = false;
}

void WubiScheme::handle_key(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch)
{
    if (vk == ImeKey::Backspace)
    {
        if (!raw_input_with_cases_.empty())
        {
            raw_input_with_cases_.pop_back();
        }
        // Rederived rather than popped: in literal input the last character may be a digit or an
        // operator that was never a code letter, and popping the code too would drift the two apart.
        raw_input_ = to_lower_ascii(
            wubi_code_letters(raw_input_with_cases_, max_code_length(), mixed_pinyin_allowed_, z_wildcard_));
        if (!key_strokes_.empty())
        {
            key_strokes_.pop_back();
        }
        return;
    }

    if (vk == ImeKey::Escape || vk == ImeKey::Return)
    {
        reset();
        return;
    }

    if (vk < 'A' || vk > 'Z')
    {
        return;
    }
    // Same rule as the Server's raw editing: the letter the key produced when it produced one,
    // otherwise the key's own letter. Both views take the same letter, so they cannot disagree on
    // a layout where the key and the character differ.
    const bool wch_is_letter = (wch >= 'A' && wch <= 'Z') || (wch >= 'a' && wch <= 'z');
    const char typed = wch_is_letter ? static_cast<char>(wch) : static_cast<char>(vk + ('a' - 'A'));
    const char code_letter = static_cast<char>(std::tolower(static_cast<unsigned char>(typed)));
    if (!is_wubi_letter(code_letter, mixed_pinyin_allowed_, z_wildcard_) || raw_input_.size() >= max_code_length())
    {
        return;
    }

    key_strokes_.push_back(KeyStroke{vk, modifiers_down, wch});
    raw_input_.push_back(code_letter);
    raw_input_with_cases_.push_back(typed);
}

void WubiScheme::set_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    const std::string &typed = raw_input_with_cases.empty() ? raw_input : raw_input_with_cases;
    raw_input_with_cases_ =
        literal_input_ ? typed : wubi_code_letters(typed, max_code_length(), mixed_pinyin_allowed_, z_wildcard_);
    raw_input_ =
        to_lower_ascii(wubi_code_letters(raw_input_with_cases_, max_code_length(), mixed_pinyin_allowed_, z_wildcard_));
    key_strokes_.clear();
}

void WubiScheme::set_literal_input(bool literal)
{
    literal_input_ = literal;
}

void WubiScheme::set_extended_length_allowed(bool allowed)
{
    extended_length_allowed_ = allowed;
}

void WubiScheme::set_mixed_pinyin_allowed(bool allowed)
{
    mixed_pinyin_allowed_ = allowed;
}

void WubiScheme::set_z_wildcard(bool enabled)
{
    z_wildcard_ = enabled;
}

size_t WubiScheme::max_code_length() const
{
    return extended_length_allowed_ ? kMaxMixedCodeLength : kMaxCodeLength;
}

QueryRequest WubiScheme::build_request() const
{
    QueryRequest request;
    request.scheme = type();
    request.raw_input = raw_input_;
    request.raw_input_with_cases = raw_input_with_cases_;
    request.normalized_input = raw_input_;
    request.raw_segmentation = raw_input_;
    request.normalized_segmentation = raw_input_;
    request.segmentation = raw_input_;
    request.key_strokes = key_strokes_;
    // Wildcard mode only: the provider switches from a range scan to a pattern match when the
    // code actually holds a z. Set here rather than in the provider so mixed input alone never
    // reaches the pattern path; with both on, the session ranks pinyin ahead of the wildcard rows.
    request.wubi_z_wildcard = z_wildcard_ && raw_input_.find('z') != std::string::npos;
    request.valid = !raw_input_.empty();
    return request;
}

std::string WubiScheme::get_preedit() const
{
    return raw_input_;
}

SchemeType WubiScheme::type() const
{
    return SchemeType::Wubi;
}
