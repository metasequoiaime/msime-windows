#include "tests/includes/test_framework.h"

#include "settings/native/native_settings_search.h"

#include <memory>
#include <string>
#include <vector>

namespace
{
using native_settings::SearchSettings;
using native_settings::SettingsIndexEntry;

// A live stand-in for a setting title. The header only forward-declares msimeui::Visual, so the test
// cannot construct one; a non-null pointer with a no-op deleter is enough to make the weak handle
// lock and a null one to make it expired, which is all SearchSettings looks at.
msimeui::Visual *FakeRowPointer()
{
    static char storage = 0;
    return reinterpret_cast<msimeui::Visual *>(&storage);
}

std::shared_ptr<msimeui::Visual> MakeFakeRow()
{
    return std::shared_ptr<msimeui::Visual>(FakeRowPointer(), [](msimeui::Visual *) {});
}

struct RowBuilder
{
    std::vector<SettingsIndexEntry> entries;
    std::vector<std::shared_ptr<msimeui::Visual>> kept;

    // `alive = false` gives an entry with no row, the way a malformed entry or one built for a
    // page that was torn down would look.
    void Add(const char *page, const wchar_t *pageTitle, const wchar_t *title, const wchar_t *description,
             bool alive = true)
    {
        SettingsIndexEntry entry;
        entry.pageId = page;
        entry.pageTitle = pageTitle;
        entry.title = title;
        entry.description = description;
        if (alive)
        {
            auto row = MakeFakeRow();
            kept.push_back(row);
            entry.target = row;
        }
        entries.push_back(std::move(entry));
    }
};

std::vector<std::wstring> Titles(const std::vector<SettingsIndexEntry> &hits)
{
    std::vector<std::wstring> titles;
    for (const SettingsIndexEntry &hit : hits)
        titles.push_back(hit.title);
    return titles;
}

RowBuilder SampleTree()
{
    RowBuilder builder;
    builder.Add("input", L"输入", L"候选框最大高度", L"调整候选窗显示的行数上限");
    builder.Add("input", L"输入", L"候选框最小宽度", L"候选窗在窄屏上的最小宽度");
    builder.Add("input", L"输入", L"云候选同步", L"把云端的候选合入本地结果");
    builder.Add("appearance", L"外观", L"界面缩放比例", L"整个设置窗口的缩放");
    builder.Add("appearance", L"外观", L"候选窗圆角", L"候选窗四角的圆角半径");
    builder.Add("ai-settings", L"AI 与云联想", L"服务端点", L"用于云联想的接口地址");
    return builder;
}
} // namespace

TEST_CASE(SettingsSearchFindsRowsByTitleFragment)
{
    RowBuilder tree = SampleTree();
    const auto hits = SearchSettings(tree.entries, L"候选框");
    const auto titles = Titles(hits);
    REQUIRE_EQ(titles.size(), size_t{2});
    REQUIRE_EQ(titles[0], L"候选框最大高度");
    REQUIRE_EQ(titles[1], L"候选框最小宽度");
}

TEST_CASE(SettingsSearchMatchesOutOfOrderCharacters)
{
    RowBuilder tree = SampleTree();
    // "候选" + "高度" typed as one run: the characters are not adjacent in the title.
    const auto hits = SearchSettings(tree.entries, L"候高");
    REQUIRE(!hits.empty());
    REQUIRE_EQ(hits.front().title, L"候选框最大高度");
}

TEST_CASE(SettingsSearchFallsBackToDescription)
{
    RowBuilder tree = SampleTree();
    const auto hits = SearchSettings(tree.entries, L"窄屏");
    REQUIRE_EQ(hits.size(), size_t{1});
    REQUIRE_EQ(hits.front().title, L"候选框最小宽度");
}

TEST_CASE(SettingsSearchPrefersTitleOverDescription)
{
    RowBuilder tree;
    tree.Add("a", L"甲", L"标题里有云联想", L"");
    tree.Add("b", L"乙", L"无关标题", L"描述里也提到云联想");
    const auto hits = SearchSettings(tree.entries, L"云联想");
    REQUIRE_EQ(hits.size(), size_t{2});
    REQUIRE_EQ(hits[0].title, L"标题里有云联想");
    REQUIRE_EQ(hits[1].title, L"无关标题");
}

TEST_CASE(SettingsSearchIgnoresAsciiCaseAndFullWidthForms)
{
    RowBuilder tree;
    tree.Add("ai", L"AI 与云联想", L"AI 服务端点", L"");
    tree.Add("ai", L"AI 与云联想", L"模型选择", L"");
    // Lower-case ASCII finds an upper-case title.
    const auto ascii = SearchSettings(tree.entries, L"ai");
    REQUIRE_EQ(ascii.size(), size_t{1});
    REQUIRE_EQ(ascii.front().title, L"AI 服务端点");
    // Full-width input finds the same row.
    const auto wide = SearchSettings(tree.entries, L"ＡＩ");
    REQUIRE_EQ(wide.size(), size_t{1});
    REQUIRE_EQ(wide.front().title, L"AI 服务端点");
}

TEST_CASE(SettingsSearchSkipsEntriesWithoutARow)
{
    RowBuilder tree;
    tree.Add("a", L"甲", L"候选框高度", L"", false);
    tree.Add("a", L"甲", L"候选框宽度", L"", true);
    const auto hits = SearchSettings(tree.entries, L"候选框");
    REQUIRE_EQ(hits.size(), size_t{1});
    REQUIRE_EQ(hits.front().title, L"候选框宽度");
}

TEST_CASE(SettingsSearchReturnsNothingForEmptyOrUnmatchedQueries)
{
    RowBuilder tree = SampleTree();
    REQUIRE(SearchSettings(tree.entries, L"").empty());
    REQUIRE(SearchSettings(tree.entries, L"绝对不存在的词").empty());
    // Every character has to appear: "候框" skips 选 and must not match "候选框".
    REQUIRE(SearchSettings(tree.entries, L"候框元").empty());
}

TEST_CASE(SettingsSearchHonoursTheLimit)
{
    RowBuilder tree;
    for (int i = 0; i < 20; ++i)
    {
        const std::wstring title = L"候选框 " + std::to_wstring(i);
        tree.Add("input", L"输入", title.c_str(), L"");
    }
    REQUIRE_EQ(SearchSettings(tree.entries, L"候选框", 5).size(), size_t{5});
    REQUIRE_EQ(SearchSettings(tree.entries, L"候选框").size(), size_t{12});
}
