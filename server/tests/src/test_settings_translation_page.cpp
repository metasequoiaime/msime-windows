#include "tests/includes/settings_window_harness.h"
#include "tests/includes/test_framework.h"

#include "settings/native/native_settings_controls.h"
#include "settings/native/native_settings_model.h"
#include "settings/native/native_settings_pages.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using native_settings::Json;

// 「翻译与英文候选」页：中文候选与英文候选各一张卡片，开关、目标语种、译文显示和在线翻译服务各写各的配置键。
namespace
{
constexpr float kWidth = 800.0f;
constexpr float kHeight = 3200.0f;

class NullHost : public native_settings::HostServices
{
  public:
    explicit NullHost(msimeui::Window &window) : window_(window)
    {
    }
    HWND Hwnd() const override
    {
        return nullptr;
    }
    msimeui::Window *Window() const override
    {
        return &window_;
    }
    void ShowToast(const std::wstring &, bool) override
    {
    }
    void Confirm(const std::wstring &, std::function<void(bool)>, const std::wstring &, const std::wstring &,
                 const std::wstring &) override
    {
    }
    void ShowModal(native_settings::VisualPtr, std::function<void()>) override
    {
    }
    void CloseModal() override
    {
    }
    void NavigateTo(const std::string &) override
    {
    }
    void ScrollIntoView(const msimeui::Visual &) override
    {
    }

  private:
    msimeui::Window &window_;
};

// 页面持有 PageContext，整个进程里都不销毁（同 test_settings_ai_model_list_scroll.cpp）。
class TranslationPageScene
{
  public:
    static TranslationPageScene &Open(bool chinese = false, bool english = true, const char *provider = "bilibili",
                                      const char *englishProvider = "bilibili", const char *target = "en")
    {
        auto *scene = new TranslationPageScene();
        scene->model.Receive(
            Json{{"type", "configSnapshot"},
                 {"data",
                  {{"general", {{"candidate_translations", chinese}, {"english_candidate_translations", english}}},
                   {"translation",
                    {{"provider", provider}, {"english_provider", englishProvider}, {"english_target_language", "ja"}}},
                   {"tencent_tmt", {{"target_language", target}}}}}}
                .dump());
        scene->harness.Layout();
        return *scene;
    }

    bool Shows(const std::wstring &text)
    {
        for (auto *label : Visible<native_settings::Label>())
            if (label->GetText() == text)
                return true;
        return false;
    }

    template <class T> std::vector<T *> Visible()
    {
        std::vector<T *> found;
        std::set<T *> seen;
        for (float y = 0; y < kHeight; y += 6)
            for (float x = 0; x < kWidth; x += 6)
                if (auto *visual = dynamic_cast<T *>(harness.VisualAt(x, y)); visual && seen.insert(visual).second)
                    found.push_back(visual);
        return found;
    }

    std::vector<native_settings::RectF> Labels(const std::wstring &text)
    {
        std::vector<native_settings::RectF> matches;
        for (auto *label : Visible<native_settings::Label>())
            if (label->GetText() == text)
                matches.push_back(label->GetBounds());
        std::sort(matches.begin(), matches.end(),
                  [](const native_settings::RectF &a, const native_settings::RectF &b) { return a.y < b.y; });
        return matches;
    }

    native_settings::RectF Label(const std::wstring &text, size_t occurrence = 0)
    {
        const auto matches = Labels(text);
        if (occurrence >= matches.size())
            throw std::runtime_error("Expected label");
        return matches[occurrence];
    }

    // 标题所在那一行右边的开关。
    native_settings::ToggleSwitch *ToggleBeside(const std::wstring &title, size_t occurrence = 0)
    {
        const auto label = Label(title, occurrence);
        native_settings::ToggleSwitch *best = nullptr;
        float distance = 1e9f;
        for (auto *toggle : Visible<native_settings::ToggleSwitch>())
        {
            const auto rect = toggle->GetBounds();
            const float d = std::fabs((rect.y + rect.height * 0.5f) - (label.y + label.height * 0.5f));
            if (rect.x > label.x && d < distance)
            {
                best = toggle;
                distance = d;
            }
        }
        if (!best || distance > 40.0f)
            throw std::runtime_error("Expected toggle beside title");
        return best;
    }

    // 夹在两个分区标题之间的那个 Dimmable（开关下面的整块设置）。不可用的 Dimmable 命中测试只返回它自己，
    // 里面的标签找不到，所以按位置找。
    native_settings::Dimmable *DimmableBetween(const std::wstring &title, const std::wstring &next)
    {
        const float top = Label(title).y;
        const float bottom = Label(next).y;
        std::set<native_settings::Dimmable *> seen;
        for (float y = top; y < bottom; y += 4)
            for (float x = 0; x < kWidth; x += 6)
                for (auto *visual = harness.VisualAt(x, y); visual; visual = visual->GetParentVisual())
                    if (auto *dimmable = dynamic_cast<native_settings::Dimmable *>(visual))
                        seen.insert(dimmable);
        if (seen.size() != 1)
            throw std::runtime_error("Expected exactly one dimmable block between the titles");
        return *seen.begin();
    }

    native_settings::Dimmable *ChineseOptions()
    {
        return DimmableBetween(L"中文候选翻译", L"英文候选翻译");
    }

    native_settings::Dimmable *EnglishOptions()
    {
        return DimmableBetween(L"英文候选翻译", L"英文候选显示音标");
    }

    // 标签落在哪张卡片里：中文候选卡片在「英文候选翻译」之上，英文候选卡片在它与「英文候选显示音标」之间。
    bool InChineseCard(const native_settings::RectF &rect)
    {
        return rect.y > Label(L"中文候选翻译").y && rect.y < Label(L"英文候选翻译").y;
    }

    bool InEnglishCard(const native_settings::RectF &rect)
    {
        return rect.y > Label(L"英文候选翻译").y && rect.y < Label(L"英文候选显示音标").y;
    }

    void Click(native_settings::ToggleSwitch *toggle)
    {
        const auto rect = toggle->GetBounds();
        harness.Click(rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f);
    }

    Json LastUpdate()
    {
        for (auto it = sent.rbegin(); it != sent.rend(); ++it)
        {
            const Json message = Json::parse(*it, nullptr, false);
            if (message.is_object() && message.value("type", std::string()) == "configUpdate")
                return message["data"];
        }
        throw std::runtime_error("Expected configUpdate");
    }

    SettingsWindowHarness harness;
    std::vector<std::string> sent;
    native_settings::SettingsModel model;
    NullHost host;
    native_settings::PageContext context;

  private:
    TranslationPageScene()
        : harness(96.0f, {kWidth, kHeight}), model([this](const std::string &message) { sent.push_back(message); }),
          host(harness.Window()), context(model, host)
    {
        harness.SetRoot(native_settings::BuildTranslationPage(context));
    }
};
} // namespace

TEST_CASE(translation_page_keeps_chinese_and_english_translation_switches_apart)
{
    auto &scene = TranslationPageScene::Open();
    auto *chinese = scene.ToggleBeside(L"中文候选翻译");
    auto *english = scene.ToggleBeside(L"英文候选翻译");
    REQUIRE(chinese != english);
    REQUIRE(!chinese->IsChecked());
    REQUIRE(english->IsChecked());
    // 中文那边关着：它下面的整块设置不可用；英文那边开着：可用。
    auto *chineseOptions = scene.ChineseOptions();
    auto *englishOptions = scene.EnglishOptions();
    REQUIRE(chineseOptions != englishOptions);
    REQUIRE(chineseOptions->IsDisabled());
    REQUIRE(!englishOptions->IsDisabled());

    scene.Click(english);
    Json update = scene.LastUpdate();
    REQUIRE(update["path"] == "general.english_candidate_translations");
    REQUIRE(update["value"] == false);
    REQUIRE(englishOptions->IsDisabled());

    scene.Click(chinese);
    update = scene.LastUpdate();
    REQUIRE(update["path"] == "general.candidate_translations");
    REQUIRE(update["value"] == true);
    REQUIRE(!chineseOptions->IsDisabled());
}

TEST_CASE(translation_page_gives_each_side_its_own_display_switches)
{
    auto &scene = TranslationPageScene::Open(true, true);
    // 两张卡片各有一行「显示词性」「只显示单条释义」，写各自的键。
    REQUIRE(scene.Labels(L"显示词性").size() == 2);
    REQUIRE(scene.Labels(L"只显示单条释义").size() == 2);
    scene.Click(scene.ToggleBeside(L"显示词性", 0));
    REQUIRE(scene.LastUpdate()["path"] == "general.candidate_translation_show_pos");
    scene.Click(scene.ToggleBeside(L"显示词性", 1));
    REQUIRE(scene.LastUpdate()["path"] == "general.english_candidate_translation_show_pos");
    scene.Click(scene.ToggleBeside(L"只显示单条释义", 0));
    REQUIRE(scene.LastUpdate()["path"] == "general.candidate_translation_single_sense");
    scene.Click(scene.ToggleBeside(L"只显示单条释义", 1));
    REQUIRE(scene.LastUpdate()["path"] == "general.english_candidate_translation_single_sense");
    // 英文候选的音标开关单独一行，不在英文候选翻译的卡片里，也不跟着它变灰。
    scene.Click(scene.ToggleBeside(L"英文候选显示音标"));
    REQUIRE(scene.LastUpdate()["path"] == "general.english_candidate_phonetics");
    REQUIRE(!scene.Shows(L"英文候选显示音标与释义"));
}

TEST_CASE(translation_page_offers_phonetics_for_chinese_candidates_only_when_translating_to_english)
{
    auto &english = TranslationPageScene::Open(true, true, "bilibili", "bilibili", "en");
    REQUIRE(english.InChineseCard(english.Label(L"显示音标")));
    english.Click(english.ToggleBeside(L"显示音标"));
    REQUIRE(english.LastUpdate()["path"] == "general.candidate_translation_phonetics");

    auto &japanese = TranslationPageScene::Open(true, true, "bilibili", "bilibili", "ja");
    REQUIRE(!japanese.Shows(L"显示音标"));
}

TEST_CASE(translation_page_lists_each_sides_credentials_in_its_own_card)
{
    // 中文候选用腾讯云、英文候选用阿里云：各自的卡片里只有自己那家的凭据。
    auto &scene = TranslationPageScene::Open(true, true, "tencent", "aliyun");
    REQUIRE(scene.Labels(L"腾讯云 API 凭据").size() == 1);
    REQUIRE(scene.InChineseCard(scene.Label(L"腾讯云 API 凭据")));
    REQUIRE(scene.Labels(L"阿里云 API 凭据").size() == 1);
    REQUIRE(scene.InEnglishCard(scene.Label(L"阿里云 API 凭据")));
    REQUIRE(!scene.Shows(L"小牛翻译 API 凭据"));
    REQUIRE(!scene.Shows(L"译文显示与服务凭据"));

    // 两边选同一家：两张卡片都列出它的凭据（写的是同一组键）。
    auto &same = TranslationPageScene::Open(true, true, "niutrans", "niutrans");
    const auto both = same.Labels(L"小牛翻译 API 凭据");
    REQUIRE(both.size() == 2);
    REQUIRE(same.InChineseCard(both[0]));
    REQUIRE(same.InEnglishCard(both[1]));
}