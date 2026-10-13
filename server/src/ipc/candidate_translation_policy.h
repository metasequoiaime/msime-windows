#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "input_key_policy.h"

namespace FanyImeIpc
{
inline constexpr uint32_t kVirtualKeyReturn = 0x0D;

// Ctrl+Enter commits the translation shown to the right of the highlighted
// candidate instead of the candidate itself. Shift or Alt must not be down:
// those combinations still belong to the application.
constexpr bool IsTranslationCommitKey(uint32_t keycode, uint32_t modifiers_down)
{
    return keycode == kVirtualKeyReturn && (modifiers_down & kKeyModifierMask) == kModifierControl;
}

// The machine-translated zh->en glosses (sources/cn-en/ in msime-dictionary) lead
// each sense with a part-of-speech label: "n. silver medal", "proper n. Wanshan
// District", "adj. spreading; n. sound of laughter". The label is shown in the
// candidate window but must not reach the committed text.
// Returns the length of the label at the start of `sense` without its trailing
// space ("n." -> 2), or 0 when the sense has no label.
inline size_t PartOfSpeechLabelLength(std::string_view sense)
{
    static constexpr std::string_view kLabels[] = {"proper n. ", "interj. ", "idiom. ", "pron. ", "prep. ",
                                                   "conj. ",     "adj. ",    "adv. ",   "num. ",  "det. ",
                                                   "aux. ",      "n. ",      "v. "};
    for (const auto label : kLabels)
    {
        if (sense.size() > label.size() && sense.substr(0, label.size()) == label)
            return label.size() - 1;
    }
    return 0;
}

inline std::string_view StripPartOfSpeechLabel(std::string_view sense)
{
    const size_t label = PartOfSpeechLabelLength(sense);
    if (label == 0)
        return sense;
    const size_t rest = sense.find_first_not_of(' ', label);
    return rest == std::string_view::npos ? sense : sense.substr(rest);
}

enum class TranslationSegmentKind
{
    Text,
    PartOfSpeech,
    Separator,
};

struct TranslationSegment
{
    TranslationSegmentKind kind = TranslationSegmentKind::Text;
    std::string text;
};

// Splits a displayed gloss into the runs the candidate window colors apart: the
// senses' text, their part-of-speech labels and the separators between senses.
// Concatenating the segments' text gives back `gloss` unchanged.
inline std::vector<TranslationSegment> SegmentTranslationGloss(std::string_view gloss)
{
    static constexpr std::string_view kFullwidthSemicolon = "\xEF\xBC\x9B";
    std::vector<TranslationSegment> segments;
    const auto push = [&segments](TranslationSegmentKind kind, std::string_view text) {
        if (text.empty())
            return;
        if (!segments.empty() && segments.back().kind == kind)
            segments.back().text.append(text);
        else
            segments.push_back({kind, std::string(text)});
    };
    const auto pushSense = [&push](std::string_view sense) {
        const size_t first = (std::min)(sense.find_first_not_of(" \t\r\n"), sense.size());
        push(TranslationSegmentKind::Text, sense.substr(0, first));
        sense.remove_prefix(first);
        const size_t label = PartOfSpeechLabelLength(sense);
        push(TranslationSegmentKind::PartOfSpeech, sense.substr(0, label));
        push(TranslationSegmentKind::Text, sense.substr(label));
    };
    size_t start = 0;
    while (start < gloss.size())
    {
        const size_t ascii = gloss.find(';', start);
        const size_t fullwidth = gloss.find(kFullwidthSemicolon, start);
        const size_t cut = (std::min)(ascii, fullwidth);
        if (cut == std::string_view::npos)
        {
            pushSense(gloss.substr(start));
            break;
        }
        pushSense(gloss.substr(start, cut - start));
        const size_t width = cut == fullwidth ? kFullwidthSemicolon.size() : 1;
        push(TranslationSegmentKind::Separator, gloss.substr(cut, width));
        start = cut + width;
    }
    return segments;
}

// Display-only formatting. Keep the original gloss for caching and Ctrl+Enter;
// unlike splitting commit candidates, hiding labels preserves every separator.
inline std::string FormatTranslationGlossForDisplay(std::string_view gloss, bool show_pos)
{
    if (show_pos)
        return std::string(gloss);

    std::string displayed;
    bool after_label = false;
    for (const auto &segment : SegmentTranslationGloss(gloss))
    {
        if (segment.kind == TranslationSegmentKind::PartOfSpeech)
        {
            after_label = true;
            continue;
        }
        std::string_view text = segment.text;
        if (after_label && segment.kind == TranslationSegmentKind::Text)
            text.remove_prefix((std::min)(text.find_first_not_of(" \t\r\n"), text.size()));
        displayed += text;
        after_label = false;
    }
    return displayed;
}

// english.db stores one gloss string per entry with the senses already joined:
// en->zh uses the fullwidth '；' ("苹果；家伙"), zh->en the ASCII "; " ("string; trail").
// Every sense is returned, with its part-of-speech label removed.
// A cloud gloss is a single sentence and has no separator, so it stays one entry.
// Header-only pure policy so tests can pin it without linking the server stack.
inline std::vector<std::string> SplitTranslationGloss(std::string_view gloss)
{
    static constexpr std::string_view kFullwidthSemicolon = "\xEF\xBC\x9B";
    std::vector<std::string> senses;
    size_t start = 0;
    const auto push = [&senses](std::string_view sense) {
        const size_t first = sense.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos)
            return;
        const size_t last = sense.find_last_not_of(" \t\r\n");
        senses.emplace_back(StripPartOfSpeechLabel(sense.substr(first, last - first + 1)));
    };
    while (start <= gloss.size())
    {
        const size_t ascii = gloss.find(';', start);
        const size_t fullwidth = gloss.find(kFullwidthSemicolon, start);
        const size_t cut = (std::min)(ascii, fullwidth);
        if (cut == std::string_view::npos)
        {
            push(gloss.substr(start));
            break;
        }
        push(gloss.substr(start, cut - start));
        start = cut + (cut == fullwidth ? kFullwidthSemicolon.size() : 1);
    }
    return senses;
}
} // namespace FanyImeIpc
