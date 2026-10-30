#include "ime_utils.h"
#include <algorithm>
#include "config/ime_config.h"
#include "engine/contracts/preedit_caret_map.h"
#include "utils/common_utils.h"
#include "utils/shuangpin_preedit_display.h"
#include "global/globals.h"

std::wstring GetPreedit()
{
    std::wstring preedit_pinyin = string_to_wstring(GlobalIme::composition.segmented_pinyin);
    if (!GlobalIme::composition.creating_word.prefix_text.empty())
    {
        preedit_pinyin = string_to_wstring(GlobalIme::composition.creating_word.prefix_text) + preedit_pinyin;
    }
    return preedit_pinyin;
}

std::wstring GetPreeditWithCaretMarker()
{
    const auto &composition = GlobalIme::composition;
    if (GetConfiguredCandidateWindowPreeditShuangpinQuanpin() && composition.has_shuangpin_forms())
    {
        const std::string &raw = composition.raw_input_with_cases;
        const ShuangpinQuanpinPreedit quanpin =
            BuildShuangpinQuanpinPreedit(raw, composition.shuangpin_raw_segmentation,
                                         composition.shuangpin_quanpin_segmentation, /*keep_separators=*/true);
        const std::wstring word = string_to_wstring(composition.creating_word.prefix_text);
        std::wstring preedit = word + string_to_wstring(quanpin.text);
        const size_t caret = (std::min)(composition.caret_position, raw.size());
        preedit.insert(word.size() + quanpin.caret_map[caret], 1, L'\uE000');
        return preedit;
    }

    std::wstring preedit = GetPreedit();
    const std::string &raw = GlobalIme::composition.raw_input_with_cases;
    const size_t caret = (std::min)(GlobalIme::composition.caret_position, raw.size());
    size_t letters_before_caret = 0;
    for (size_t i = 0; i < caret; ++i)
    {
        if (raw[i] != '\'')
        {
            ++letters_before_caret;
        }
    }

    const size_t word_prefix = string_to_wstring(GlobalIme::composition.creating_word.prefix_text).size();
    size_t display_pos = (std::min)(word_prefix, preedit.size());
    size_t seen_letters = 0;
    while (display_pos < preedit.size() && seen_letters < letters_before_caret)
    {
        if (preedit[display_pos] != L'\'')
        {
            ++seen_letters;
        }
        ++display_pos;
    }
    if (caret > 0 && raw[caret - 1] == '\'')
    {
        while (display_pos < preedit.size() && preedit[display_pos] == L'\'')
        {
            ++display_pos;
        }
    }
    preedit.insert(display_pos, 1, L'\uE000');
    return preedit;
}

std::wstring BuildTsfPreedit(const std::wstring &word, const std::string &raw)
{
    const auto &composition = GlobalIme::composition;
    if (!GetConfiguredTsfPreeditShuangpinQuanpin())
    {
        return word + string_to_wstring(composition.segmented_pinyin);
    }
    const bool raw_style = GetConfiguredTsfPreeditStyle() == GlobalSettings::TsfPreeditStyle::Raw;
    if (!composition.has_shuangpin_forms())
    {
        // 原始按键样式只是为了双拼才改由回包驱动，其余组合（特殊模式、英文模式等）照旧显示按键。
        return word + string_to_wstring(raw_style ? raw : composition.segmented_pinyin);
    }
    const ShuangpinQuanpinPreedit quanpin =
        BuildShuangpinQuanpinPreedit(raw, composition.shuangpin_raw_segmentation,
                                     composition.shuangpin_quanpin_segmentation, /*keep_separators=*/!raw_style);
    std::vector<std::size_t> caret_map;
    caret_map.reserve(quanpin.caret_map.size());
    for (const std::size_t position : quanpin.caret_map)
    {
        caret_map.push_back(word.size() + position);
    }
    return FanyImePreeditCaretMap::Encode(word + string_to_wstring(quanpin.text), caret_map);
}

std::wstring GetTsfPreedit()
{
    return BuildTsfPreedit(string_to_wstring(GlobalIme::composition.creating_word.prefix_text),
                           GlobalIme::composition.raw_input_with_cases);
}
