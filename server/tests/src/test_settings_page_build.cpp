#include "settings/native/native_settings_pages.h"
#include "tests/includes/settings_window_harness.h"
#include "tests/includes/test_framework.h"

#include <memory>
#include <string>
#include <vector>

namespace
{
using native_settings::HostServices;
using native_settings::PageContext;
using native_settings::SettingsModel;
using native_settings::VisualPtr;

class NullHost : public HostServices
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
    void ShowModal(VisualPtr, std::function<void()>) override
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
} // namespace

// 设置窗口打开后，索引器会把还没被访问过的页面逐个建出来，只为收集搜索索引
// （native_settings_window.cpp 的 BuildPageAndIndex / IndexNextPage）。builder 因此只搭控件：
// 查词库、解码图片这类动作会为一张用户没打开的页面付出代价，失败时还会在无关页面上弹提示。
// 词典面板的首次查询就挂在页面 Attach 上，而不是构造函数。
TEST_CASE(BuildingEverySettingsPageSendsNoMessage)
{
    SettingsWindowHarness window(96.0f, {800.0f, 1200.0f});
    NullHost host(window.Window());
    std::vector<std::string> sent;
    SettingsModel model([&sent](const std::string &message) { sent.push_back(message); });
    PageContext context(model, host);

    std::vector<std::string> offenders;
    for (const native_settings::PageInfo &page : native_settings::Pages())
    {
        REQUIRE(page.build != nullptr);
        sent.clear();
        const VisualPtr visual = page.build(context);
        REQUIRE(visual != nullptr);
        // 建页面期间发给宿主的任何消息都意味着索引器替用户触发了它。一次列出全部违规页面，
        // 否则修掉一个才能看见下一个。
        if (!sent.empty())
            offenders.push_back(std::string(page.id) + " posted " + sent.front());
    }
    if (!offenders.empty())
    {
        std::string joined;
        for (const std::string &offender : offenders)
            joined += (joined.empty() ? "" : "; ") + offender;
        throw std::runtime_error(joined);
    }
}
