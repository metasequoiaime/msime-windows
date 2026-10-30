#include "tests/includes/settings_window_harness.h"
#include "tests/includes/test_framework.h"

#include "settings/native/native_page_model_combo.h"
#include "settings/native/native_settings_model.h"
#include "settings/native/native_settings_pages.h"
#include "settings/native/native_page_voice_fields.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
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
    static AiPageScene &Feedback(float dpi = 96.0f, bool snapshot = true)
    {
        auto *scene = new AiPageScene(dpi);
        if (snapshot)
            scene->Snapshot();
        return *scene;
    }

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

    template <class T> std::set<T *> Visible()
    {
        std::set<T *> found;
        for (float y = 0; y < 2400; y += 8)
            for (float x = 0; x < 800; x += 8)
                if (auto *visual = dynamic_cast<T *>(harness.VisualAt(x, y)))
                    found.insert(visual);
        return found;
    }

    native_settings::voice_fields::PlainButton *Button(const std::wstring &text)
    {
        for (auto *button : Visible<native_settings::voice_fields::PlainButton>())
            if (button->Text() == text)
                return button;
        throw std::runtime_error("Expected action button");
    }

    std::string Start(bool connection)
    {
        auto *button = Button(connection ? L"测试连接" : L"查询模型");
        REQUIRE(!button->IsDisabled());
        const auto rect = button->GetBounds();
        harness.Click(rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f);
        const Json message = Json::parse(sent.back());
        REQUIRE(message["type"] == (connection ? "apiCredentialTest" : "apiModelList"));
        return message["data"].value("requestId", std::string());
    }

    void Reply(bool connection, const std::string &id, bool ok, const std::string &text)
    {
        model.Receive(Json{{"type", connection ? "apiCredentialTestResult" : "apiModelListResult"},
                           {"requestId", id},
                           {"ok", ok},
                           {"message", text},
                           {"models", Json::array({"test-model"})}}
                          .dump());
        // A successful model query opens a menu that could otherwise obscure the feedback.
        harness.Window().GetScene()->ClearPopups(true);
        harness.Layout();
    }

    std::vector<std::wstring> Messages()
    {
        std::vector<std::pair<float, std::wstring>> lines;
        for (auto *label : Visible<native_settings::Label>())
            for (auto *parent = label->GetParentVisual(); parent; parent = parent->GetParentVisual())
                if (dynamic_cast<native_settings::voice_fields::StatusText *>(parent))
                {
                    lines.emplace_back(label->GetBounds().y, label->GetText());
                    break;
                }
        std::sort(lines.begin(), lines.end());
        std::vector<std::wstring> texts;
        for (const auto &line : lines)
            texts.push_back(line.second);
        return texts;
    }

    native_settings::RectF LabelBounds(const std::wstring &text)
    {
        for (auto *label : Visible<native_settings::Label>())
            if (label->GetText() == text)
                return label->GetBounds();
        throw std::runtime_error("Expected field label");
    }

    void Edit(const std::wstring &field)
    {
        const auto label = LabelBounds(field);
        msimeui::TextBox *input = nullptr;
        for (auto *candidate : Visible<msimeui::TextBox>())
        {
            const auto rect = candidate->GetBounds();
            if (rect.x >= label.x && rect.x < label.x + 40 && rect.y >= label.y + label.height && rect.y < label.y + 80)
                input = candidate;
        }
        REQUIRE(input != nullptr);
        harness.GiveWindowKeyboardFocus();
        const auto rect = input->GetBounds();
        harness.Click(rect.x + 8, rect.y + rect.height * 0.5f);
        harness.Type(L'x');
        // Do not commit: clearing must happen on the edit itself.
    }

    void Snapshot(const std::string &provider = "deepseek")
    {
        model.Receive(Json{{"type", "configSnapshot"},
                           {"data", {{"ai_assistant", {{"provider", provider}, {"generate_provider", provider}}}}}}
                          .dump());
        harness.Layout();
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

TEST_CASE(settings_ai_feedback_keeps_both_operations_in_both_response_orders)
{
    for (bool connectionFirst : {false, true})
        for (bool replyFirst : {false, true})
        {
            auto &scene = AiPageScene::Feedback();
            const auto first = scene.Start(connectionFirst);
            const auto second = scene.Start(!connectionFirst);
            REQUIRE_EQ(scene.Messages().size(), size_t{2});
            const auto respond = [&](bool firstReply) {
                const bool connection = firstReply ? connectionFirst : !connectionFirst;
                scene.Reply(connection, firstReply ? first : second, !connection, connection ? "invalid key" : "ok");
            };
            respond(replyFirst);
            REQUIRE_EQ(scene.Messages().size(), size_t{2});
            respond(!replyFirst);
            REQUIRE(scene.Messages() ==
                    std::vector<std::wstring>({L"模型查询：获取到 1 个模型", L"连接测试：invalid key"}));
        }
}

TEST_CASE(settings_ai_feedback_keeps_identical_errors_attributed)
{
    // Two independent operations keep their own row and prefix even when they report the same text.
    auto &outcomes = AiPageScene::Feedback();
    outcomes.Reply(true, outcomes.Start(true), false, "same error");
    outcomes.Reply(false, outcomes.Start(false), false, "same error");
    REQUIRE(outcomes.Messages() == std::vector<std::wstring>({L"模型查询：same error", L"连接测试：same error"}));
    outcomes.Snapshot();
    REQUIRE(outcomes.Messages() == std::vector<std::wstring>({L"模型查询：same error", L"连接测试：same error"}));

    // The common same-text case: both buttons report the same local precondition.
    auto &precondition = AiPageScene::Feedback();
    precondition.Reply(true, precondition.Start(true), false, "请先填写有效的 API Key。");
    precondition.Reply(false, precondition.Start(false), false, "请先填写有效的 API Key。");
    REQUIRE(precondition.Messages() ==
            std::vector<std::wstring>({L"模型查询：请先填写有效的 API Key。", L"连接测试：请先填写有效的 API Key。"}));

    // Same text but one succeeded and the other failed; they are still distinct records.
    auto &mixed = AiPageScene::Feedback();
    mixed.Reply(true, mixed.Start(true), true, "same error");
    mixed.Reply(false, mixed.Start(false), false, "same error");
    REQUIRE(mixed.Messages() == std::vector<std::wstring>({L"模型查询：same error", L"连接测试：same error"}));
}

TEST_CASE(settings_ai_feedback_reserves_one_row_and_expands_for_two_records)
{
    for (float dpi : {96.0f, 120.0f, 144.0f, 192.0f})
    {
        auto &scene = AiPageScene::Feedback(dpi);
        const float initial = scene.LabelBounds(L"候选数量").y;
        scene.Reply(true, scene.Start(true), false, "same error");
        REQUIRE(std::fabs(scene.LabelBounds(L"候选数量").y - initial) < 0.1f);
        const auto query = scene.Start(false);
        REQUIRE(scene.LabelBounds(L"候选数量").y > initial + 10);
        scene.Reply(false, query, false, "same error");
        REQUIRE(scene.LabelBounds(L"候选数量").y > initial + 10);
        scene.Snapshot("siliconflow");
        REQUIRE(scene.Messages().empty());
        REQUIRE(std::fabs(scene.LabelBounds(L"候选数量").y - initial) < 0.1f);
    }
}

TEST_CASE(settings_ai_feedback_clears_on_api_edits_and_ignores_superseded_replies)
{
    for (const std::wstring field : {L"API Token", L"接口地址", L"模型"})
    {
        auto &scene = AiPageScene::Feedback();
        scene.Reply(true, scene.Start(true), false, "invalid key");
        scene.Reply(false, scene.Start(false), true, "ok");
        scene.Edit(field);
        REQUIRE(scene.Messages().empty());
        scene.harness.Key(VK_RETURN);
        const auto oldTest = scene.Start(true);
        const auto oldModels = scene.Start(false);
        scene.Edit(field);
        REQUIRE(scene.Messages().empty());
        scene.Reply(true, oldTest, false, "old error");
        scene.Reply(false, oldModels, true, "ok");
        REQUIRE(scene.Messages().empty());
        REQUIRE(!scene.Button(L"测试连接")->IsDisabled());
        REQUIRE(!scene.Button(L"查询模型")->IsDisabled());
    }
}

TEST_CASE(settings_ai_feedback_preserves_success_when_the_other_operation_starts)
{
    for (bool connection : {false, true})
    {
        auto &scene = AiPageScene::Feedback();
        scene.Reply(connection, scene.Start(connection), true, "connected");
        const auto pending = scene.Start(!connection);
        REQUIRE(scene.Messages() ==
                (connection
                     ? std::vector<std::wstring>({L"模型查询：正在向服务商查询模型列表…", L"连接测试：connected"})
                     : std::vector<std::wstring>({L"模型查询：获取到 1 个模型", L"连接测试：正在连接服务，请稍候…"})));
        scene.Reply(!connection, pending, false, "other failed");
        REQUIRE(scene.Messages() ==
                (connection ? std::vector<std::wstring>({L"模型查询：other failed", L"连接测试：connected"})
                            : std::vector<std::wstring>({L"模型查询：获取到 1 个模型", L"连接测试：other failed"})));
    }
}

TEST_CASE(settings_ai_feedback_timeout_stays_separate_and_is_cancelled_by_edit)
{
    auto &active = AiPageScene::Feedback();
    active.context.credentialTestTimeoutMs = 100;
    const auto activeTest = active.Start(true);
    active.Reply(false, active.Start(false), true, "ok");
    auto &edited = AiPageScene::Feedback();
    edited.context.credentialTestTimeoutMs = 100;
    const auto oldTest = edited.Start(true);
    edited.Edit(L"API Token");
    REQUIRE(edited.Messages().empty());
    // Exercise the real timer path with a shortened timer. Keep the same 15 s scheduling
    // slack the old 30 s timer / 45 s deadline pair had, so a slow CI machine still passes.
    const ULONGLONG deadline = GetTickCount64() + 15000;
    auto *activeButton = active.Button(L"测试中…");
    while (activeButton->IsDisabled() && GetTickCount64() < deadline)
    {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_TIMER);
        MSG message{};
        while (PeekMessageW(&message, nullptr, WM_TIMER, WM_TIMER, PM_REMOVE))
            DispatchMessageW(&message);
    }
    active.harness.Layout();
    edited.harness.Layout();
    REQUIRE(active.Messages() ==
            std::vector<std::wstring>({L"模型查询：获取到 1 个模型", L"连接测试：测试超时，请重试"}));
    REQUIRE(edited.Messages().empty());
    active.Reply(true, activeTest, true, "late success");
    edited.Reply(true, oldTest, false, "late error");
    REQUIRE(active.Messages()[1] == L"连接测试：测试超时，请重试");
    REQUIRE(edited.Messages().empty());
}

TEST_CASE(settings_shared_credential_test_keeps_default_feedback_without_ai_observer)
{
    using namespace native_settings::voice_fields;
    auto &scene = AiPageScene::Feedback();
    auto button = std::make_shared<PlainButton>(L"测试连接", CredentialButtonStyle());
    auto status = std::make_shared<StatusText>();
    auto config = std::make_shared<Json>(Json{{"token", ""}});
    // Retain the helper with the scene because PageContext owns its message listener.
    new CredentialTest(scene.context, button, status, []() { return "voice.asr"; }, [config]() { return *config; });
    auto root = std::make_shared<native_settings::Row>(10.0f);
    root->Add(button);
    root->Add(status);
    scene.harness.SetRoot(root);
    scene.Reply(true, scene.Start(true), true, "voice ok");
    REQUIRE(scene.Messages() == std::vector<std::wstring>({L"voice ok"}));
    const auto pending = scene.Start(true);
    (*config)["token"] = "FAKESECRET_changed";
    scene.Reply(true, pending, true, "voice ok");
    REQUIRE(scene.Messages() == std::vector<std::wstring>({L"配置已变更，请重新测试"}));
    REQUIRE(!button->IsDisabled());
}

TEST_CASE(settings_ai_feedback_invalidates_requests_started_before_the_first_snapshot)
{
    auto &scene = AiPageScene::Feedback(96.0f, false);
    const auto connection = scene.Start(true);
    const auto models = scene.Start(false);
    scene.Snapshot("siliconflow");
    REQUIRE(scene.Messages().empty());
    scene.Reply(true, connection, false, "old error");
    scene.Reply(false, models, true, "ok");
    REQUIRE(scene.Messages().empty());
    REQUIRE(!scene.Button(L"测试连接")->IsDisabled());
    REQUIRE(!scene.Button(L"查询模型")->IsDisabled());
}

TEST_CASE(settings_ai_model_query_reply_keeps_select_mode_button_disabled)
{
    auto &scene = AiPageScene::Feedback();
    scene.Snapshot("jev");
    REQUIRE(scene.Button(L"查询模型")->IsDisabled());
    const auto boxes = scene.Visible<native_settings::voice_fields::ModelCombo>();
    REQUIRE(boxes.size() == 1);
    const auto bounds = (*boxes.begin())->GetBounds();
    scene.harness.Click(bounds.x + bounds.width - 16.0f, bounds.y + bounds.height * 0.5f);
    const Json request = Json::parse(scene.sent.back());
    REQUIRE(request["type"] == "apiModelList");
    const auto id = request["data"].value("requestId", std::string());
    scene.Reply(false, id, false, "model list unavailable");
    REQUIRE(scene.Button(L"查询模型")->IsDisabled());
}

TEST_CASE(settings_status_text_only_notifies_when_text_or_kind_changes)
{
    native_settings::voice_fields::StatusText status;
    int changes = 0;
    status.SetOnChanged([&]() { ++changes; });
    status.Clear();
    REQUIRE(changes == 0);
    status.Set(L"invalid key", "error");
    status.Set(L"invalid key", "error");
    REQUIRE(changes == 1);
    status.Set(L"invalid key", "success");
    REQUIRE(changes == 2);
    status.Set(L"connected", "success");
    REQUIRE(changes == 3);
    status.Clear();
    status.Clear();
    REQUIRE(changes == 4);
}
