#include "tests/includes/settings_window_harness.h"
#include "tests/includes/test_framework.h"

#include "settings/native/native_page_voice_fields.h"
#include "settings/native/native_settings_model.h"
#include "settings/native/native_settings_pages.h"

#include <set>
#include <string>
#include <vector>

using native_settings::Json;

// 语音输入页的「本地模型（离线）」：选中后隐藏 Token、接口地址与测试按钮，显示模型选择与下载行；
// 下载按钮带着所选模型 id 发 localAsrModelDownload，并先把它记进配置。
namespace
{
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
    void Confirm(const std::wstring &, std::function<void(bool)> done, const std::wstring &, const std::wstring &,
                 const std::wstring &) override
    {
        confirmed = true;
        done(true);
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
    bool confirmed = false;

  private:
    msimeui::Window &window_;
};

Json Catalog()
{
    return Json::array({Json{{"id", "x-asr-zh-en-streaming"},
                             {"title", "中英流式（X-ASR）"},
                             {"description", "自带标点"},
                             {"sizeHint", "约 134 MB"},
                             {"memoryHint", "约 0.5 GB"},
                             {"license", "Apache-2.0"},
                             {"url", "https://github.com/example/stream.tar.bz2"},
                             {"needsVad", false}},
                        Json{{"id", "sense-voice-small"},
                             {"title", "快速整句（SenseVoice）"},
                             {"description", "速度最快"},
                             {"sizeHint", "约 163 MB"},
                             {"memoryHint", "约 0.9 GB"},
                             {"license", "FunASR Model License 1.1"},
                             {"url", "https://github.com/example/sense.tar.bz2"},
                             {"needsVad", true}}});
}

Json Status(const std::string &state, bool installed = false, int progress = 0)
{
    return Json{{"state", state}, {"progress", progress}, {"error", ""}, {"detail", ""}, {"modelInstalled", installed}};
}

// BuildVoicePage keeps the page alive for the rest of the process together with its context.
class VoicePageScene
{
  public:
    static VoicePageScene &Create()
    {
        return *new VoicePageScene();
    }

    void Snapshot(const std::string &provider, const std::string &model, const Json &statuses)
    {
        this->model.Receive(Json{{"type", "configSnapshot"},
                                 {"data",
                                  {{"voice_input",
                                    {{"asr_provider", provider},
                                     {"local_asr_model", model},
                                     {"local_asr_catalog", Catalog()},
                                     {"local_asr_status", statuses},
                                     {"local_asr_runtime_url", "https://github.com/example/runtime.tar.bz2"},
                                     {"local_asr_vad_url", "https://github.com/example/silero_vad.onnx"}}}}}}
                                .dump());
        harness.Layout();
    }

    template <class T> std::set<T *> Visible()
    {
        std::set<T *> found;
        for (float y = 0; y < 2400; y += 6)
            for (float x = 0; x < 800; x += 6)
                if (auto *visual = dynamic_cast<T *>(harness.VisualAt(x, y)))
                    found.insert(visual);
        return found;
    }

    bool LabelVisible(const std::wstring &text)
    {
        for (auto *label : Visible<native_settings::Label>())
            if (label->GetText() == text)
                return true;
        return false;
    }

    int CountLabels(const std::wstring &text)
    {
        int count = 0;
        for (auto *label : Visible<native_settings::Label>())
            count += label->GetText() == text ? 1 : 0;
        return count;
    }

    bool LabelStartsWith(const std::wstring &prefix)
    {
        for (auto *label : Visible<native_settings::Label>())
            if (label->GetText().rfind(prefix, 0) == 0)
                return true;
        return false;
    }

    native_settings::Button *FindButton(const std::wstring &text)
    {
        for (auto *button : Visible<native_settings::Button>())
            if (button->Text() == text)
                return button;
        return nullptr;
    }

    bool PlainButtonVisible(const std::wstring &text)
    {
        for (auto *button : Visible<native_settings::voice_fields::PlainButton>())
            if (button->Text() == text)
                return true;
        return false;
    }

    void Click(native_settings::Button *button)
    {
        const auto rect = button->GetBounds();
        harness.Click(rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f);
    }

    std::vector<Json> Sent(const std::string &type)
    {
        std::vector<Json> found;
        for (const std::string &raw : sent)
        {
            const Json message = Json::parse(raw, nullptr, false);
            if (message.is_object() && message.value("type", std::string()) == type)
                found.push_back(message);
        }
        return found;
    }

    bool ConfigWritten(const std::string &path, const std::string &value)
    {
        for (const Json &message : Sent("configUpdate"))
            if (message["data"].value("path", std::string()) == path &&
                message["data"].value("value", std::string()) == value)
                return true;
        return false;
    }

    SettingsWindowHarness harness;
    std::vector<std::string> sent;
    native_settings::SettingsModel model;
    NullHost host;
    native_settings::PageContext context;

  private:
    VoicePageScene()
        : harness(96.0f, {800.0f, 2400.0f}), model([this](const std::string &message) { sent.push_back(message); }),
          host(harness.Window()), context(model, host)
    {
        harness.SetRoot(native_settings::BuildVoicePage(context));
    }
};
} // namespace

TEST_CASE(voice_page_local_provider_replaces_cloud_fields)
{
    auto &scene = VoicePageScene::Create();
    scene.Snapshot("doubao", "", Json::object());
    // ASR 与文本润色各有一个「接口地址」。
    REQUIRE(scene.CountLabels(L"接口地址") == 2);
    REQUIRE(scene.PlainButtonVisible(L"测试配置"));
    REQUIRE(!scene.LabelVisible(L"本地模型"));

    scene.Snapshot("local", "", Json{{"x-asr-zh-en-streaming", Status("absent")}});
    REQUIRE(scene.LabelVisible(L"本地模型"));
    REQUIRE(scene.CountLabels(L"接口地址") == 1);
    REQUIRE(!scene.LabelVisible(L"Access Token / API Key"));
    REQUIRE(!scene.LabelVisible(L"API Key"));
    REQUIRE(scene.LabelVisible(L"未下载"));
    REQUIRE(scene.LabelVisible(L"运行库压缩包"));
    // 流式模型不需要 VAD，不给它的下载链接。
    REQUIRE(!scene.LabelVisible(L"silero_vad.onnx"));
    REQUIRE(scene.FindButton(L"下载") != nullptr);
    REQUIRE(scene.FindButton(L"删除") == nullptr);

    // 配置里还没选模型：点下载时把界面上的首项记进配置，再请求下载它。
    scene.Click(scene.FindButton(L"下载"));
    REQUIRE(scene.ConfigWritten("voice_input.local_asr_model", "x-asr-zh-en-streaming"));
    const auto downloads = scene.Sent("localAsrModelDownload");
    REQUIRE(downloads.size() == 1);
    REQUIRE(downloads[0]["data"]["modelId"] == "x-asr-zh-en-streaming");
}

TEST_CASE(voice_page_local_model_status_drives_actions)
{
    auto &scene = VoicePageScene::Create();
    scene.Snapshot(
        "local", "sense-voice-small",
        Json{{"sense-voice-small", Status("downloading", false, 42)}, {"x-asr-zh-en-streaming", Status("absent")}});
    REQUIRE(scene.LabelVisible(L"下载中 42%"));
    REQUIRE(scene.LabelVisible(L"silero_vad.onnx"));
    auto *download = scene.FindButton(L"下载");
    REQUIRE(download != nullptr && download->IsDisabled());

    scene.Snapshot(
        "local", "sense-voice-small",
        Json{{"sense-voice-small", Status("ready", true, 100)}, {"x-asr-zh-en-streaming", Status("absent")}});
    REQUIRE(scene.LabelStartsWith(L"模型已就绪"));
    REQUIRE(scene.FindButton(L"下载")->IsDisabled());
    REQUIRE(scene.FindButton(L"导入") == nullptr);
    auto *remove = scene.FindButton(L"删除");
    REQUIRE(remove != nullptr);
    scene.Click(remove);
    REQUIRE(scene.host.confirmed);
    const auto deletes = scene.Sent("localAsrModelDelete");
    REQUIRE(deletes.size() == 1);
    REQUIRE(deletes[0]["data"]["modelId"] == "sense-voice-small");
}

TEST_CASE(voice_page_switching_to_local_keeps_token_slots_untouched)
{
    auto &scene = VoicePageScene::Create();
    scene.Snapshot("doubao", "", Json::object());
    scene.sent.clear();
    // 下拉框选「本地模型（离线）」：走与用户点选相同的 SwitchAsrProvider。
    native_settings::Dropdown *provider = nullptr;
    for (auto *dropdown : scene.Visible<native_settings::Dropdown>())
        if (dropdown->GetValue() == "doubao")
            provider = dropdown;
    REQUIRE(provider != nullptr);
    // 「本地模型（离线）」是最后一项：打开菜单、↑ 跳到末项、回车选中。
    REQUIRE(provider->GetOptions().back().value == "local");
    scene.harness.GiveWindowKeyboardFocus();
    const auto rect = provider->GetBounds();
    scene.harness.Click(rect.x + 20.0f, rect.y + rect.height * 0.5f);
    scene.harness.Key(VK_UP);
    scene.harness.Key(VK_RETURN);
    REQUIRE(provider->GetValue() == "local");
    REQUIRE(scene.ConfigWritten("voice_input.asr_provider", "local"));
    REQUIRE(scene.LabelVisible(L"本地模型"));
    // asr_token_local 这个槽位不存在，写它会让宿主报「设置保存失败」。
    for (const Json &message : scene.Sent("configUpdate"))
        REQUIRE(message["data"].value("path", std::string()) != "voice_input.asr_token_local");
}
