#include "quanpin_scheme.h"
#include "../common/helpcode_utils.h"
#include "../quanpin/quanpin_query.h"
#include "../quanpin/quanpin_utils.h"
#include "../shuangpin/shuangpin_query.h"
#include <cctype>

namespace
{
bool is_alpha_vk(ImeKeyCode vk)
{
    return vk >= 'A' && vk <= 'Z';
}
} // namespace

void QuanpinScheme::reset()
{
    raw_input_.clear();
    key_strokes_.clear();
}

void QuanpinScheme::set_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    raw_input_ = raw_input_with_cases.empty() ? raw_input : raw_input_with_cases;
    key_strokes_.clear();
}

void QuanpinScheme::set_autocorrect_types(unsigned autocorrect_types)
{
    include_correction_aliases_ = quanpin::correction_aliases_enabled(autocorrect_types);
}

void QuanpinScheme::handle_key(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch)
{
    if (vk == ImeKey::Backspace)
    {
        if (!raw_input_.empty())
        {
            raw_input_.pop_back();
        }
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

    if (vk == ImeKey::Apostrophe)
    {
        if (raw_input_.empty() || raw_input_.back() != '\'')
        {
            key_strokes_.push_back(KeyStroke{vk, modifiers_down, wch});
            raw_input_.push_back('\'');
        }
        return;
    }

    if (!is_alpha_vk(vk))
    {
        return;
    }

    key_strokes_.push_back(KeyStroke{vk, modifiers_down, wch});
    if (wch >= L'A' && wch <= L'Z')
    {
        raw_input_.push_back(static_cast<char>(wch));
    }
    else if (wch >= L'a' && wch <= L'z')
    {
        raw_input_.push_back(static_cast<char>(wch));
    }
    else
    {
        raw_input_.push_back(static_cast<char>(vk + ('a' - 'A')));
    }
}

QueryRequest QuanpinScheme::build_request() const
{
    QueryRequest request;
    request.scheme = type();
    request.raw_input_with_cases = raw_input_;
    request.raw_input.reserve(raw_input_.size());
    for (const char ch : raw_input_)
    {
        request.raw_input.push_back(ch == '\'' ? ch : static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    request.key_strokes = key_strokes_;

    const size_t helpcode_length =
        quanpin::detect_active_helpcode_length(request.raw_input, request.raw_input_with_cases);
    std::string normalized_source = quanpin::strip_active_helpcodes(request.raw_input, request.raw_input_with_cases);

    request.normalized_input.reserve(normalized_source.size());
    for (const char ch : normalized_source)
    {
        if (ch != '\'')
        {
            request.normalized_input.push_back(ch);
        }
    }
    if (!request.normalized_input.empty())
    {
        try
        {
            const auto segments =
                quanpin::cut_pinyin_by_mode(normalized_source, "correction", include_correction_aliases_);
            if (!segments.empty())
            {
                request.normalized_segmentation = quanpin::join_segments(segments.front());
            }
        }
        catch (...)
        {
        }
    }

    if (request.normalized_segmentation.empty())
    {
        request.normalized_segmentation = normalized_source;
    }

    std::string cased_source = request.raw_input_with_cases;
    if (helpcode_length > 0 && cased_source.size() >= helpcode_length)
    {
        cased_source = cased_source.substr(0, cased_source.size() - helpcode_length);
    }

    request.raw_segmentation = shuangpin::apply_segmentation_cases(request.normalized_segmentation, cased_source);
    if (!cased_source.empty() && cased_source.back() == '\'' &&
        (request.raw_segmentation.empty() || request.raw_segmentation.back() != '\''))
    {
        request.raw_segmentation.push_back('\'');
    }
    if (helpcode_length > 0)
    {
        request.raw_segmentation += "'";
        request.raw_segmentation +=
            request.raw_input_with_cases.substr(request.raw_input_with_cases.size() - helpcode_length, helpcode_length);
    }
    request.segmentation = request.normalized_segmentation;

    request.valid = !request.normalized_input.empty();
    return request;
}

std::string QuanpinScheme::get_preedit() const
{
    return raw_input_;
}

SchemeType QuanpinScheme::type() const
{
    return SchemeType::Quanpin;
}
