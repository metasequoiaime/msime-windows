#pragma once

// Matching for the settings search box.
//
// Split out of native_settings_controls.cpp on purpose: this is pure string work over a list of
// titles, it touches no Windows API, and keeping it separate lets it live in the portable test
// target where AddressSanitizer and UndefinedBehaviorSanitizer run on a Linux runner. The control
// header itself cannot go there, it pulls in D2D.

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace msimeui
{
class Visual;
}

namespace native_settings
{
// One indexed setting title.
//
// `target` is a weak handle on the visual that shows the title, and the page stays in the window's
// page cache for as long as the entry is useful, so locking it gives back the very visual that is
// on screen. A weak_ptr needs only a forward declaration, which is what keeps this header free of
// the control headers and their D2D dependency; the portable test target includes this one.
struct SettingsIndexEntry
{
    std::string pageId;
    std::wstring pageTitle;
    std::wstring title;
    std::wstring description;
    std::weak_ptr<msimeui::Visual> target;
};

// Matches `query` against entry titles and descriptions, best first.
//
// Not a full-text search. The settings tree has a few hundred rows with titles like
// "候选框最大高度" and what people type is a fragment of one ("候选", "高度"). Every query
// character must appear in the title in order, which is what a CJK user expects from a search box
// with no pinyin input. A title match outranks a description-only match, then the tighter match
// wins, so "候选" surfaces the candidate-box rows before long rows that merely mention the word.
//
// Comparison folds ASCII case and full-width forms, so "ai" finds "AI 与云联想" and "ＡＩ". Equal
// scores keep index order, so the same query always gives the same list.
std::vector<SettingsIndexEntry> SearchSettings(const std::vector<SettingsIndexEntry> &entries,
                                               const std::wstring &query, size_t limit = 12);
} // namespace native_settings
