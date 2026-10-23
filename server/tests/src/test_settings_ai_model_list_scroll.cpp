#include "tests/includes/settings_window_harness.h"
#include "tests/includes/test_framework.h"

#include "settings/native/native_settings_model.h"
#include "settings/native/native_settings_pages.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <typeinfo>
#include <vector>

using native_settings::Json;

// The model list of the AI page, reached the way a user gets there: the arrow of the
// empty model box asks the host for models, the host answers, and the list opens.
//
// Expectations are worked out from the `.compact-scroll-menu` geometry: 28 DIP rows 2
// apart inside a 2 DIP inset, 180 DIPs showing, a 4 DIP thumb 2 from the right edge and
// 4 from each end. With 20 models the content is 602 DIPs and scrolls up to 422; every
// DIP the thumb moves scrolls 602 / 172 = 3.5 DIPs.
namespace
{
constexpr float kRow = 28.0f;
constexpr float kRowPitch = 30.0f;
constexpr float kInset = 2.0f;
constexpr float kVisible = 180.0f;
constexpr float kThumbEndGap = 4.0f;
constexpr float kContentPerThumbDip = 3.5f;
constexpr const char *kModelPathPrefix = "ai_assistant.model@";

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

std::string ModelName(size_t index)
{
    char name[16];
    std::snprintf(name, sizeof(name), "model-%02zu", index);
    return name;
}

// BuildAiPage keeps every page alive for the rest of the process, and the page holds on
// to its PageContext, so a scene is never destroyed.
class AiPageScene
{
  public:
    static AiPageScene &Open(float dpi)
    {
        auto *scene = new AiPageScene(dpi);
        scene->OpenModelList(20);
        return *scene;
    }

    float Y(float localY) const
    {
        return bounds.y + localY;
    }

    float ThumbX() const
    {
        return bounds.x + bounds.width - 4.0f;
    }

    float RowX() const
    {
        return bounds.x + bounds.width * 0.5f;
    }

    float RowCentre(size_t row, float scroll) const
    {
        return bounds.y + kInset + static_cast<float>(row) * kRowPitch + kRow * 0.5f - scroll;
    }

    bool ListIsOpen()
    {
        return harness.VisualAt(RowX(), Y(10.0f)) == list;
    }

    // Models written to the config so far, in order.
    std::vector<std::string> Picked() const
    {
        std::vector<std::string> picked;
        for (const std::string &raw : sent)
        {
            const Json message = Json::parse(raw, nullptr, false);
            if (!message.is_object() || message.value("type", std::string()) != "configUpdate")
                continue;
            const Json &data = message["data"];
            if (data.value("path", std::string()).rfind(kModelPathPrefix, 0) == 0)
                picked.push_back(data.value("value", std::string()));
        }
        return picked;
    }

    SettingsWindowHarness harness;
    std::vector<std::string> sent;
    native_settings::SettingsModel model;
    NullHost host;
    native_settings::PageContext context;
    msimeui::Visual *list = nullptr;
    msimeui::RectF bounds = {};

  private:
    explicit AiPageScene(float dpi)
        : harness(dpi, {800.0f, 2400.0f}), model([this](const std::string &message) { sent.push_back(message); }),
          host(harness.Window()), context(model, host)
    {
        harness.SetRoot(native_settings::BuildAiPage(context));
    }

    // The model box: hit testing returns the box itself only on its arrow.
    msimeui::Visual *FindModelBoxArrow()
    {
        for (float y = 0.0f; y < 2400.0f; y += 4.0f)
        {
            for (float x = 0.0f; x < 800.0f; x += 6.0f)
            {
                msimeui::Visual *visual = harness.VisualAt(x, y);
                if (visual && std::string(typeid(*visual).name()).find("ModelCombo") != std::string::npos)
                    return visual;
            }
        }
        return nullptr;
    }

    void OpenModelList(size_t count)
    {
        msimeui::Visual *box = FindModelBoxArrow();
        REQUIRE(box != nullptr);
        const msimeui::RectF boxBounds = box->GetBounds();
        // The arrow: 18 DIPs square, 6 DIPs plus the border in from the right edge.
        harness.Click(boxBounds.x + boxBounds.width - 1.0f - 6.0f - 9.0f, boxBounds.y + boxBounds.height * 0.5f);

        std::string requestId;
        for (const std::string &raw : sent)
        {
            const Json message = Json::parse(raw, nullptr, false);
            if (message.is_object() && message.value("type", std::string()) == "apiModelList")
                requestId = message["data"].value("requestId", std::string());
        }
        REQUIRE(!requestId.empty());

        Json models = Json::array();
        for (size_t i = 0; i < count; ++i)
            models.push_back(ModelName(i));
        model.Receive(
            Json{{"type", "apiModelListResult"}, {"requestId", requestId}, {"ok", true}, {"models", models}}.dump());
        harness.Layout();

        list = harness.VisualAt(boxBounds.x + boxBounds.width * 0.5f, boxBounds.y + boxBounds.height + 40.0f);
        REQUIRE(list != nullptr);
        REQUIRE(list != box);
        bounds = list->GetBounds();
        REQUIRE(std::fabs(bounds.height - kVisible) < 0.01f);
    }
};

void CheckThumbDrag(float dpi)
{
    AiPageScene &scene = AiPageScene::Open(dpi);
    scene.harness.Press(scene.ThumbX(), scene.Y(kThumbEndGap + 16.0f));
    scene.harness.MoveHeld(scene.ThumbX(), scene.Y(kThumbEndGap + 76.0f));
    scene.harness.Release(scene.ThumbX(), scene.Y(kThumbEndGap + 76.0f));
    REQUIRE(scene.Picked().empty());
    REQUIRE(scene.ListIsOpen());

    const float scroll = 60.0f * kContentPerThumbDip;
    scene.harness.Click(scene.RowX(), scene.RowCentre(10, scroll));
    REQUIRE(scene.Picked() == std::vector<std::string>{ModelName(10)});
}
} // namespace

TEST_CASE(settings_ai_model_list_dragging_the_thumb_scrolls_the_list)
{
    CheckThumbDrag(96.0f);
}

TEST_CASE(settings_ai_model_list_press_on_the_scrollbar_picks_no_model)
{
    AiPageScene &scene = AiPageScene::Open(96.0f);
    scene.harness.Click(scene.ThumbX(), scene.RowCentre(0, 0.0f));
    REQUIRE(scene.Picked().empty());
    REQUIRE(scene.ListIsOpen());
    scene.harness.Click(scene.ThumbX(), scene.RowCentre(5, 0.0f));
    REQUIRE(scene.Picked().empty());
    REQUIRE(scene.ListIsOpen());
}
