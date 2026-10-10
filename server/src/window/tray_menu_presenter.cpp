#include "window/tray_menu_presenter.h"

#include "config/ime_config.h"
#include "defines/globals.h"
#include "ipc/ipc.h"
#include "log/ftb_diag_log.h"
#include "settings/settings_launcher.h"
#include "utils/window_utils.h"
#include "voice-input/voice_input_service.h"
#include "webview2/windows_webview2.h"
#include "window/ime_windows.h"
#include "window/tray_menu_placement.h"

#include "msimeui/Controls.h"
#include "msimeui/DeviceResources.h"
#include "msimeui/Layout.h"
#include "msimeui/Scene.h"
#include "msimeui/Window.h"

#include <algorithm>
#include <cmath>
#include <d2d1.h>
#include <dwmapi.h>
#include <string>
#include <vector>

namespace
{
D2D1_COLOR_F ColorFromRgb(UINT rgb, float alpha = 1.0f)
{
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}

constexpr float kShadowPadLeft = 16.0f;
constexpr float kShadowPadTop = 12.0f;
constexpr float kShadowPadRight = 16.0f;
constexpr float kShadowPadBottom = 20.0f;

// 输入方案二级菜单：宽度含 4 DIP 内边距；与主菜单卡片之间留 kSchemeSubmenuGap 的缝。
constexpr float kSchemeSubmenuWidth = 120.0f;
constexpr float kSchemeSubmenuGap = 4.0f;
// 主菜单项相对卡片四边内缩 4 DIP，见 RebuildScene 里 items 的 padding。
constexpr float kItemInset = 4.0f;

struct SchemeChoice
{
    const char *name;
    const wchar_t *label;
};

constexpr SchemeChoice kSchemeChoices[] = {
    {"quanpin", L"全拼"},
    {"shuangpin", L"双拼"},
    {"wubi", L"五笔"},
};

const char kSvgInputScheme[] =
    R"(<svg viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg">
            <path d="M4 8H17M14 4.5L17.5 8L14 11.5M20 16H7M10 12.5L6.5 16L10 19.5" stroke="currentColor"
              stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" />
          </svg>)";

const char kSvgCheck[] =
    R"(<svg viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg">
            <path d="M5 12.5L10 17.5L19 7.5" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"
              stroke-linejoin="round" />
          </svg>)";

// 未选中的方案占同样的图标位，让三项文字对齐。
const char kSvgBlank[] = R"(<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"></svg>)";

const char kSvgToolbar[] =
    R"(<svg viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg">
            <rect x="3" y="6" width="18" height="12" rx="3" stroke="currentColor" stroke-width="1.8" />
            <circle cx="7.5" cy="12" r="1.4" fill="currentColor" />
            <path d="M11 12H13M16 12H18" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" />
          </svg>)";

const char kSvgEmoji[] =
    R"(<svg viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg">
            <path fill-rule="evenodd" clip-rule="evenodd"
              d="M12 21C16.9706 21 21 16.9706 21 12C21 7.02944 16.9706 3 12 3C7.02944 3 3 7.02944 3 12C3 16.9706 7.02944 21 12 21Z"
              stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
            <path
              d="M15.6364 13.625C14.9091 14.8371 13.4546 15.625 12 15.625C10.5455 15.625 9.09092 14.8371 8.36365 13.625"
              stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
            <path d="M9.01 10H9" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" />
            <path d="M15.01 10H15" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" />
          </svg>)";

const char kSvgHandwriting[] =
    R"(<svg viewBox="0 -960 960 960" xmlns="http://www.w3.org/2000/svg">
            <path
              d="M140 0q-24.54 0-42.27-17.54Q80-35.08 80-60q0-24.54 17.73-42.27Q115.46-120 140-120h680q24.54 0 42.27 17.54Q880-84.92 880-60q0 24.54-17.73 42.27Q844.54 0 820 0H140Zm100-303.46h49.46l332-331.39-25.15-25.53-24.92-24.54-331.39 332v49.46Zm-60 23.84v-83.76q0-7.23 2.42-13.77 2.43-6.54 8.04-12.16l437.93-436.92q8.69-8.69 19.73-13.15 11.03-4.46 22.8-4.46 12.16 0 23.12 4.46t20.27 13.77l48.07 48.69q9.31 8.69 13.46 19.84 4.16 11.16 4.16 23.31 0 11.16-4.16 22.19-4.15 11.04-13.46 20.35L325.46-254.31q-5.62 5.62-12.15 8.23-6.54 2.62-13.77 2.62h-83.38q-15.47 0-25.81-10.35Q180-264.15 180-279.62Zm540.38-454.76-49.46-49.46 49.46 49.46Zm-98.92 99.53-25.15-25.53-24.92-24.54 50.07 50.07Z"
              fill="currentColor" />
          </svg>)";

const char kSvgKeyboard[] =
    R"(<svg viewBox="0 0 12 12" fill="none" xmlns="http://www.w3.org/2000/svg">
            <path
              d="M3.25 10.25V9.5H5.625V8H2.15387C1.90129 8 1.6875 7.9125 1.5125 7.7375C1.3375 7.5625 1.25 7.34871 1.25 7.09613V2.65387C1.25 2.40129 1.3375 2.1875 1.5125 2.0125C1.6875 1.8375 1.90129 1.75 2.15387 1.75H9.84613C10.0987 1.75 10.3125 1.8375 10.4875 2.0125C10.6625 2.1875 10.75 2.40129 10.75 2.65387V7.09613C10.75 7.34871 10.6625 7.5625 10.4875 7.7375C10.3125 7.9125 10.0987 8 9.84613 8H6.375V9.5H8.75V10.25H3.25ZM2 7.09613C2 7.13463 2.01604 7.16992 2.04813 7.202C2.08013 7.234 2.11537 7.25 2.15387 7.25H9.84613C9.88463 7.25 9.91988 7.234 9.95188 7.202C9.98396 7.16992 10 7.13463 10 7.09613V2.65387C10 2.61537 9.98396 2.58012 9.95188 2.54812C9.91988 2.51604 9.88463 2.5 9.84613 2.5H2.15387C2.11537 2.5 2.08013 2.51604 2.04813 2.54812C2.01604 2.58012 2 2.61537 2 2.65387V7.09613Z"
              fill="currentColor" />
          </svg>)";

const char kSvgVoice[] =
    R"(<svg viewBox="0 0 12 12" fill="none" xmlns="http://www.w3.org/2000/svg">
            <path
              d="M3.687 11.813C3.60083 11.7267 3.55775 11.6224 3.55775 11.5C3.55775 11.3776 3.60083 11.2733 3.687 11.187C3.77325 11.1008 3.87758 11.0578 4 11.0578C4.12242 11.0578 4.22675 11.1008 4.313 11.187C4.39917 11.2733 4.44225 11.3776 4.44225 11.5C4.44225 11.6224 4.39917 11.7267 4.313 11.813C4.22675 11.8992 4.12242 11.9423 4 11.9423C3.87758 11.9423 3.77325 11.8992 3.687 11.813ZM6 11.9423C5.87758 11.9423 5.77325 11.8992 5.687 11.813C5.60083 11.7267 5.55775 11.6224 5.55775 11.5C5.55775 11.3776 5.60083 11.2733 5.687 11.187C5.77325 11.1008 5.87758 11.0578 6 11.0578C6.12242 11.0578 6.22675 11.1008 6.313 11.187C6.39917 11.2733 6.44225 11.3776 6.44225 11.5C6.44225 11.6224 6.39917 11.7267 6.313 11.813C6.22675 11.8992 6.12242 11.9423 6 11.9423ZM7.687 11.813C7.60083 11.7267 7.55775 11.6224 7.55775 11.5C7.55775 11.3776 7.60083 11.2733 7.687 11.187C7.77325 11.1008 7.87758 11.0578 8 11.0578C8.12242 11.0578 8.22675 11.1008 8.313 11.187C8.39917 11.2733 8.44225 11.3776 8.44225 11.5C8.44225 11.6224 8.39917 11.7267 8.313 11.813C8.22675 11.8992 8.12242 11.9423 8 11.9423C7.87758 11.9423 7.77325 11.8992 7.687 11.813ZM5.113 6.387C4.871 6.145 4.75 5.84933 4.75 5.5V2.5C4.75 2.15067 4.871 1.855 5.113 1.613C5.355 1.371 5.65067 1.25 6 1.25C6.34933 1.25 6.645 1.371 6.887 1.613C7.129 1.855 7.25 2.15067 7.25 2.5V5.5C7.25 5.84933 7.129 6.145 6.887 6.387C6.645 6.629 6.34933 6.75 6 6.75C5.65067 6.75 5.355 6.629 5.113 6.387ZM5.625 10.375V8.72888C4.8 8.63138 4.11458 8.27637 3.56875 7.66387C3.02292 7.05137 2.75 6.33008 2.75 5.5H3.5C3.5 6.19167 3.74375 6.78125 4.23125 7.26875C4.71875 7.75625 5.30833 8 6 8C6.69167 8 7.28125 7.75625 7.76875 7.26875C8.25625 6.78125 8.5 6.19167 8.5 5.5H9.25C9.25 6.33008 8.97708 7.05137 8.43125 7.66387C7.88542 8.27637 7.2 8.63138 6.375 8.72888V10.375H5.625ZM6.35625 5.85625C6.45208 5.76042 6.5 5.64167 6.5 5.5V2.5C6.5 2.35833 6.45208 2.23958 6.35625 2.14375C6.26042 2.04792 6.14167 2 6 2C5.85833 2 5.73958 2.04792 5.64375 2.14375C5.54792 2.23958 5.5 2.35833 5.5 2.5V5.5C5.5 5.64167 5.54792 5.76042 5.64375 5.85625C5.73958 5.95208 5.85833 6 6 6C6.14167 6 6.26042 5.95208 6.35625 5.85625Z"
              fill="currentColor" />
          </svg>)";

const char kSvgSettings[] =
    R"(<svg viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg">
            <path fill-rule="evenodd" clip-rule="evenodd"
              d="M5.07699 14.3816L3.59497 13.9054C3.24043 13.7915 3 13.4617 3 13.0894V10.9106C3 10.5383 3.24043 10.2085 3.59497 10.0946L5.07699 9.61845C5.52769 9.47366 5.77568 8.99091 5.63089 8.54021C5.60187 8.44991 5.55808 8.36505 5.50127 8.28908L4.86839 7.44272C4.61323 7.10148 4.64746 6.62461 4.94875 6.32332L6.32332 4.94875C6.62461 4.64746 7.10148 4.61323 7.44272 4.86839L8.28908 5.50127C8.6682 5.78476 9.20535 5.70724 9.48884 5.32812C9.54564 5.25216 9.58944 5.1673 9.61845 5.07699L10.0946 3.59497C10.2085 3.24043 10.5383 3 10.9106 3H13.0894C13.4617 3 13.7915 3.24043 13.9054 3.59497L14.3816 5.07699C14.5263 5.52769 15.0091 5.77568 15.4598 5.63089C15.5501 5.60187 15.635 5.55808 15.7109 5.50127L16.5573 4.86839C16.8985 4.61323 17.3754 4.64746 17.6767 4.94875L19.0512 6.32332C19.3525 6.62461 19.3868 7.10148 19.1316 7.44272L18.4987 8.28908C18.2152 8.6682 18.2928 9.20535 18.6719 9.48884C18.7478 9.54564 18.8327 9.58944 18.923 9.61845L20.405 10.0946C20.7596 10.2085 21 10.5383 21 10.9106V13.0894C21 13.4617 20.7596 13.7915 20.405 13.9054L18.923 14.3816C18.4723 14.5263 18.2243 15.0091 18.3691 15.4598C18.3981 15.5501 18.4419 15.635 18.4987 15.7109L19.1316 16.5573C19.3868 16.8985 19.3525 17.3754 19.0512 17.6767L17.6767 19.0512C17.3754 19.3525 16.8985 19.3868 16.5573 19.1316L15.7109 18.4987C15.3318 18.2152 14.7947 18.2928 14.5112 18.6719C14.4544 18.7478 14.4106 18.8327 14.3816 18.923L13.9054 20.405C13.7915 20.7596 13.4617 21 13.0894 21H10.9106C10.5383 21 10.2085 20.7596 10.0946 20.405L9.61845 18.923C9.47366 18.4723 8.99091 18.2243 8.54021 18.3691C8.44991 18.3981 8.36505 18.4419 8.28908 18.4987L7.44272 19.1316C7.10148 19.3868 6.62461 19.3525 6.32332 19.0512L4.94875 17.6767C4.64746 17.3754 4.61323 16.8985 4.86839 16.5573L5.50127 15.7109C5.78476 15.3318 5.70724 14.7947 5.32812 14.5112C5.25216 14.4544 5.1673 14.4106 5.07699 14.3816Z"
              stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
            <path fill-rule="evenodd" clip-rule="evenodd"
              d="M14.1213 9.87868C12.9497 8.70711 11.0503 8.70711 9.87868 9.87868C8.70711 11.0503 8.70711 12.9497 9.87868 14.1213C11.0503 15.2929 12.9497 15.2929 14.1213 14.1213C15.2929 12.9497 15.2929 11.0503 14.1213 9.87868Z"
              stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" />
          </svg>)";

const char kSvgAbout[] =
    R"(<svg viewBox="0 0 12 12" fill="none" xmlns="http://www.w3.org/2000/svg">
            <path
              d="M5.625 8.375H6.375V5.5H5.625V8.375ZM6.2865 4.52813C6.36475 4.45071 6.40388 4.35479 6.40388 4.24037C6.40388 4.12596 6.36517 4.03004 6.28775 3.95263C6.21033 3.87529 6.11442 3.83662 6 3.83662C5.88558 3.83662 5.78967 3.87529 5.71225 3.95263C5.63483 4.03004 5.59613 4.12596 5.59613 4.24037C5.59613 4.35479 5.63525 4.45071 5.7135 4.52813C5.79167 4.60554 5.88717 4.64425 6 4.64425C6.11283 4.64425 6.20833 4.60554 6.2865 4.52813ZM6.00088 10.75C5.34388 10.75 4.72633 10.6253 4.14825 10.376C3.57017 10.1267 3.06733 9.78829 2.63975 9.36088C2.21217 8.93346 1.87362 8.43083 1.62412 7.853C1.37471 7.27517 1.25 6.65779 1.25 6.00088C1.25 5.34388 1.37467 4.72633 1.624 4.14825C1.87333 3.57017 2.21171 3.06733 2.63913 2.63975C3.06654 2.21217 3.56917 1.87362 4.147 1.62412C4.72483 1.37471 5.34221 1.25 5.99912 1.25C6.65613 1.25 7.27367 1.37467 7.85175 1.624C8.42983 1.87333 8.93267 2.21171 9.36025 2.63913C9.78783 3.06654 10.1264 3.56917 10.3759 4.147C10.6253 4.72483 10.75 5.34221 10.75 5.99912C10.75 6.65613 10.6253 7.27367 10.376 7.85175C10.1267 8.42983 9.78829 8.93267 9.36088 9.36025C8.93346 9.78783 8.43083 10.1264 7.853 10.3759C7.27517 10.6253 6.65779 10.75 6.00088 10.75ZM6 10C7.11667 10 8.0625 9.6125 8.8375 8.8375C9.6125 8.0625 10 7.11667 10 6C10 4.88333 9.6125 3.9375 8.8375 3.1625C8.0625 2.3875 7.11667 2 6 2C4.88333 2 3.9375 2.3875 3.1625 3.1625C2.3875 3.9375 2 4.88333 2 6C2 7.11667 2.3875 8.0625 3.1625 8.8375C3.9375 9.6125 4.88333 10 6 10Z"
              fill="currentColor" />
          </svg>)";
} // namespace

struct TrayMenuPresenter::Impl
{
    msimeui::DeviceResources resources;
    std::unique_ptr<msimeui::Window> window;
    std::shared_ptr<msimeui::StackPanel> root;
    std::shared_ptr<msimeui::Card> card;
    std::shared_ptr<msimeui::Container> frame;
    std::shared_ptr<msimeui::StackPanel> items;
    std::shared_ptr<msimeui::MenuFlyoutItem> floatingToggle;
    std::shared_ptr<msimeui::MenuFlyoutItem> schemeItem;
    std::shared_ptr<msimeui::Popup> schemeSubmenu;
    std::vector<std::shared_ptr<msimeui::MenuFlyoutItem>> rows;
    bool schemeSubmenuOpen = false;
    // 子菜单开着时宿主窗口向一侧加宽；记下加宽前的窗口矩形以便还原。
    bool hostExpanded = false;
    bool hostExpandedLeft = false;
    RECT hostRectBeforeSubmenu{};
    D2D1_COLOR_F fill = ColorFromRgb(0x2B2B2B);
    D2D1_COLOR_F border = ColorFromRgb(0x3A3A3A);
    D2D1_COLOR_F text = ColorFromRgb(0xE0E0E0);
    D2D1_COLOR_F hover = ColorFromRgb(0x3B3B3B);
};

TrayMenuPresenter::TrayMenuPresenter() = default;

TrayMenuPresenter &TrayMenuPresenter::Instance()
{
    static TrayMenuPresenter instance;
    return instance;
}

bool TrayMenuPresenter::IsBound() const
{
    return bound_;
}

bool TrayMenuPresenter::IsOpenToUser() const
{
    if (!bound_ || !hwnd_ || !openToUser_ || !IsWindowVisible(hwnd_))
    {
        return false;
    }
    DWORD cloaked = 0;
    DwmGetWindowAttribute(hwnd_, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return cloaked == 0;
}

bool TrayMenuPresenter::Bind(HWND hwnd)
{
    hwnd_ = hwnd;
    if (!hwnd)
    {
        bound_ = false;
        impl_.reset();
        return false;
    }
    impl_ = std::make_unique<Impl>();
    impl_->window = std::make_unique<msimeui::Window>(L"metasequoiaime_windows", L"menu", 1, 1);
    impl_->window->AdoptExistingHwnd(hwnd);
    impl_->window->SetStealFocusOnClick(false);
    bound_ = impl_->resources.EnsureForComposition(hwnd);
    ApplyTheme();
    return bound_;
}

void TrayMenuPresenter::RebuildScene()
{
    if (!impl_ || !impl_->window)
    {
        return;
    }
    impl_->items = std::make_shared<msimeui::StackPanel>(2.0f);
    // Items sit 4 DIP inside the card on every side; MenuFlyoutItem's tray hover
    // radius (6) is the card radius (10) minus that inset, keeping corners concentric.
    impl_->items->SetPadding({4.0f, 4.0f, 4.0f, 4.0f});
    impl_->rows.clear();
    // 旧场景连同其中的子菜单一起被换掉，窗口尺寸由随后的 PlaceAndShow 重新给出。
    impl_->schemeSubmenuOpen = false;
    impl_->hostExpanded = false;

    auto addItem = [this](const std::wstring &label, const char *svg, bool toggle) {
        auto item = std::make_shared<msimeui::MenuFlyoutItem>(label, false);
        item->SetLeadingSvg(svg);
        item->SetTrailingToggle(toggle);
        // 悬停到其他行时收起输入方案子菜单，与候选窗右键菜单的「固定排位」一致。
        item->SetOnHover([this](bool hovered) {
            if (hovered)
            {
                CloseSchemeSubmenu();
            }
        });
        impl_->items->AddChild(item);
        impl_->rows.push_back(item);
        return item;
    };

    impl_->floatingToggle = addItem(L"悬浮工具栏", kSvgToolbar, true);
    impl_->floatingToggle->SetOnClick([this]() {
        if (!impl_->floatingToggle)
        {
            return;
        }
        const bool on = impl_->floatingToggle->IsToggleOn();
        if (SetConfiguredFloatingToolbarEnabled(on))
        {
            ApplyConfiguredFloatingToolbarVisibility(L"tray-menu-toggle");
        }
    });

    impl_->schemeItem = std::make_shared<msimeui::MenuFlyoutItem>(L"输入方案", true);
    impl_->schemeItem->SetLeadingSvg(kSvgInputScheme);
    impl_->schemeItem->SetOnHover([this](bool hovered) {
        if (hovered)
        {
            OpenSchemeSubmenu();
        }
    });
    impl_->items->AddChild(impl_->schemeItem);
    impl_->rows.push_back(impl_->schemeItem);

    auto schemeStack = std::make_shared<msimeui::StackPanel>(2.0f);
    const std::string currentScheme = GetConfiguredInputSchemeName();
    for (const SchemeChoice &choice : kSchemeChoices)
    {
        auto item = std::make_shared<msimeui::MenuFlyoutItem>(choice.label, false);
        item->SetLeadingSvg(currentScheme == choice.name ? kSvgCheck : kSvgBlank);
        const std::string scheme = choice.name;
        item->SetOnClick([this, scheme]() {
            Hide();
            if (scheme != GetConfiguredInputSchemeName() && SetConfiguredInputScheme(scheme))
            {
                ApplyConfiguredInputScheme();
            }
        });
        schemeStack->AddChild(item);
        impl_->rows.push_back(item);
    }
    impl_->schemeSubmenu = std::make_shared<msimeui::Popup>(schemeStack);
    impl_->schemeSubmenu->SetMatchAnchorWidth(false);
    impl_->schemeSubmenu->SetWidth(kSchemeSubmenuWidth);
    impl_->schemeSubmenu->SetPadding({kItemInset, kItemInset, kItemInset, kItemInset});
    impl_->schemeSubmenu->SetBackgroundFill(impl_->fill);
    impl_->schemeSubmenu->SetBorderColor(impl_->border);
    impl_->schemeSubmenu->SetCornerRadius(10.0f);
    impl_->schemeSubmenu->SetShadowEnabled(true);
    impl_->schemeSubmenu->SetConstrainToViewport(true);

    auto launch = [this](auto fn) {
        return [this, fn]() {
            Hide();
            fn();
        };
    };
    addItem(L"表情/符号面板", kSvgEmoji, false)->SetOnClick(launch(OpenEmojiPanelApplication));
    addItem(L"手写识别板", kSvgHandwriting, false)->SetOnClick(launch(OpenHandwritingPanelApplication));
    addItem(L"屏幕键盘", kSvgKeyboard, false)->SetOnClick(launch(OpenKeyboardPanelApplication));
    addItem(L"语音输入", kSvgVoice, false)->SetOnClick([this]() {
        Hide();
        VoiceInput::ToggleRecording();
    });
    addItem(L"设置", kSvgSettings, false)->SetOnClick(launch(OpenSettingsApplication));
    addItem(L"关于", kSvgAbout, false)->SetOnClick(launch(OpenSettingsAboutApplication));

    msimeui::Brush brush;
    brush.fill = impl_->fill;
    brush.stroke = impl_->border;
    brush.strokeWidth = 1.0f;
    brush.radiusX = 10.0f;
    brush.radiusY = 10.0f;
    impl_->card = std::make_shared<msimeui::Card>(brush, 0.0f);
    impl_->card->AddChild(impl_->items);
    impl_->frame = std::make_shared<msimeui::Container>();
    impl_->frame->SetPadding({kShadowPadLeft, kShadowPadTop, kShadowPadRight, kShadowPadBottom});
    impl_->frame->SetChild(impl_->card);
    impl_->root = std::make_shared<msimeui::StackPanel>(0.0f);
    impl_->root->AddChild(impl_->frame);
    auto scene = std::make_unique<msimeui::Scene>();
    scene->SetRoot(impl_->root);
    impl_->window->SetScene(std::move(scene));
}

void TrayMenuPresenter::ApplyTheme()
{
    if (!impl_)
    {
        return;
    }
    const bool light = ResolveConfiguredTheme(GetConfiguredThemeMenu()) == "light";
    if (light)
    {
        impl_->fill = ColorFromRgb(0xFFFFFF);
        impl_->border = D2D1::ColorF(0, 0.10f);
        impl_->text = ColorFromRgb(0x1A1A1A);
        impl_->hover = ColorFromRgb(0xF0F0F0);
    }
    else
    {
        impl_->fill = ColorFromRgb(0x2B2B2B);
        impl_->border = ColorFromRgb(0x3A3A3A);
        impl_->text = ColorFromRgb(0xE0E0E0);
        impl_->hover = ColorFromRgb(0x3B3B3B);
    }
    RebuildScene();
    for (const auto &row : impl_->rows)
    {
        row->SetColors(impl_->text, impl_->hover);
    }
    if (impl_->floatingToggle)
    {
        impl_->floatingToggle->SetToggleOn(GetConfiguredFloatingToolbarEnabled());
    }
}

void TrayMenuPresenter::SyncFloatingToolbarToggle()
{
    if (impl_ && impl_->floatingToggle)
    {
        impl_->floatingToggle->SetToggleOn(GetConfiguredFloatingToolbarEnabled());
        if (openToUser_)
        {
            Present();
        }
    }
}

void TrayMenuPresenter::PlaceAndShow(float widthDip, float heightDip)
{
    const RECT icon = GetTrayMenuAnchorRect();
    const POINT anchor{icon.left + (icon.right - icon.left) / 2, icon.top + (icon.bottom - icon.top) / 2};
    const HalfScreenDipLimits limits = QueryHalfScreenDipLimitsForPoint(anchor);
    const FLOAT scale = limits.scale;
    widthDip = static_cast<float>(ClampWidthDipToHalfScreen(widthDip, limits));
    heightDip = static_cast<float>(ClampHeightDipToHalfScreen(heightDip, limits));
    const int widthPx = (std::max)(1, static_cast<int>(std::ceil(widthDip * scale)));
    const int heightPx = (std::max)(1, static_cast<int>(std::ceil(heightDip * scale)));
    float cardLeftDip = kShadowPadLeft;
    if (impl_->card)
    {
        const msimeui::RectF card = impl_->card->GetBounds();
        if (card.width > 1.0f)
        {
            cardLeftDip = card.x;
        }
    }
    const int cardLeftPx = static_cast<int>(std::lround(cardLeftDip * scale));
    const RECT content{cardLeftPx, static_cast<LONG>(std::lround(kShadowPadTop * scale)),
                       widthPx - static_cast<LONG>(std::lround(kShadowPadRight * scale)),
                       heightPx - static_cast<LONG>(std::lround(kShadowPadBottom * scale))};
    const POINT position = FanyImeUi::TrayMenuPosition(icon, content, limits.monitor);
    ::MENU_CONTENT_WIDTH_DIP = widthDip;
    ::MENU_CONTENT_HEIGHT_DIP = heightDip;
    ::MENU_WINDOW_WIDTH = widthPx;
    ::MENU_WINDOW_HEIGHT = heightPx;
    EnsureSmallWindowsTopmost(L"show-menu");
    const HWND zorder = AreSmallWindowsTopmostApplied() ? HWND_TOPMOST : HWND_TOP;
    SetWindowPos(hwnd_, zorder, position.x, position.y, widthPx, heightPx, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    RaiseTrayMenuAboveSmallWindows(L"show-menu");
    impl_->resources.EnsureForComposition(hwnd_);
    Present();
    openToUser_ = true;
    FTB_DIAG_LOGF(L"menu d2d show pos=({},{}) size={}x{}", position.x, position.y, widthPx, heightPx);
}

void TrayMenuPresenter::ShowFromLangBar()
{
    if (!bound_ || !hwnd_ || !impl_ || !impl_->root)
    {
        return;
    }
    ApplyTheme();
    impl_->root->InvalidateMeasure();
    const msimeui::SizeF measured = impl_->root->MeasureInLayout({320.0f, 640.0f});
    impl_->root->InvalidateArrange();
    impl_->root->ArrangeInLayout({0.0f, 0.0f, measured.width, measured.height});
    PlaceAndShow((std::max)(measured.width, 1.0f), (std::max)(measured.height, 80.0f));
}

void TrayMenuPresenter::OpenSchemeSubmenu()
{
    if (!impl_ || !impl_->window || !impl_->schemeSubmenu || !impl_->schemeItem || impl_->schemeSubmenuOpen)
    {
        return;
    }
    const bool openLeft = ExpandHostForSubmenu();
    // 加宽会改 frame 的内边距，先按新尺寸排一遍版，锚点才是加宽后的位置。
    Present();
    const msimeui::RectF anchor = impl_->schemeItem->GetBounds();
    if (anchor.width < 1.0f || anchor.height < 1.0f)
    {
        RestoreHostAfterSubmenu();
        return;
    }
    // Popup 放在锚点下方再加 offset：纵向上移一行加内边距，让第一项与「输入方案」对齐；
    // 横向越过卡片边缘再留一道缝。
    const float offsetX = openLeft ? -(kItemInset + kSchemeSubmenuGap + kSchemeSubmenuWidth)
                                   : anchor.width + kItemInset + kSchemeSubmenuGap;
    impl_->schemeSubmenu->SetAnchorRect(anchor);
    impl_->schemeSubmenu->SetOffset(offsetX, -anchor.height - kItemInset);
    if (msimeui::Scene *scene = impl_->window->GetScene())
    {
        scene->AddPopup(impl_->schemeSubmenu, [this]() {
            // 点击空白处由 Scene 收起子菜单。这里只记状态，不在鼠标分发中途改窗口尺寸；
            // 加宽的部分在下次悬停其他行或菜单重新弹出时还原。
            if (impl_)
            {
                impl_->schemeSubmenuOpen = false;
            }
        });
        impl_->schemeSubmenuOpen = true;
    }
    Present();
}

void TrayMenuPresenter::CloseSchemeSubmenu()
{
    if (!impl_ || !impl_->window)
    {
        return;
    }
    if (impl_->schemeSubmenuOpen && impl_->schemeSubmenu)
    {
        // 子菜单离开场景，Window 缓存的悬停目标要一并清掉；焦点不动，主菜单仍开着。
        impl_->window->DispatchImportedMessage(WM_MOUSELEAVE, 0, 0);
        if (msimeui::Scene *scene = impl_->window->GetScene())
        {
            scene->RemovePopup(impl_->schemeSubmenu.get(), false);
        }
        impl_->schemeSubmenuOpen = false;
    }
    RestoreHostAfterSubmenu();
}

bool TrayMenuPresenter::ExpandHostForSubmenu()
{
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
    const float scale = impl_->window->GetDpi() / 96.0f;
    const int extraPx = static_cast<int>(std::ceil((kSchemeSubmenuWidth + kSchemeSubmenuGap) * scale));
    // 托盘通常在屏幕右下角，右侧放不下就向左展开；两边都放不下时取较宽的一侧。
    const LONG roomRight = mi.rcWork.right - rc.right;
    const LONG roomLeft = rc.left - mi.rcWork.left;
    if (impl_->hostExpanded)
    {
        return impl_->hostExpandedLeft;
    }
    const bool openLeft = roomRight < extraPx && roomLeft > roomRight;
    impl_->hostRectBeforeSubmenu = rc;
    impl_->hostExpanded = true;
    impl_->hostExpandedLeft = openLeft;
    // 用换算回来的 DIP 当内边距，卡片在屏幕上的位置不随像素取整漂移。
    const float extraDip = static_cast<float>(extraPx) / scale;
    if (impl_->frame)
    {
        impl_->frame->SetPadding({kShadowPadLeft + (openLeft ? extraDip : 0.0f), kShadowPadTop,
                                  kShadowPadRight + (openLeft ? 0.0f : extraDip), kShadowPadBottom});
    }
    SetWindowPos(hwnd_, nullptr, openLeft ? rc.left - extraPx : rc.left, rc.top, rc.right - rc.left + extraPx,
                 rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
    return openLeft;
}

void TrayMenuPresenter::RestoreHostAfterSubmenu()
{
    if (!impl_ || !impl_->hostExpanded)
    {
        return;
    }
    impl_->hostExpanded = false;
    if (impl_->frame)
    {
        impl_->frame->SetPadding({kShadowPadLeft, kShadowPadTop, kShadowPadRight, kShadowPadBottom});
    }
    const RECT &rc = impl_->hostRectBeforeSubmenu;
    SetWindowPos(hwnd_, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    Present();
}

void TrayMenuPresenter::Hide()
{
    openToUser_ = false;
    if (!hwnd_)
    {
        return;
    }
    ShowWindow(hwnd_, SW_HIDE);
}

void TrayMenuPresenter::Present()
{
    if (!bound_ || !hwnd_ || !impl_ || !impl_->window)
    {
        return;
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const UINT width = static_cast<UINT>((std::max)(1L, rc.right));
    const UINT height = static_cast<UINT>((std::max)(1L, rc.bottom));
    if (!impl_->resources.EnsureForComposition(hwnd_))
    {
        return;
    }
    impl_->resources.Resize(width, height);
    ID2D1RenderTarget *target = impl_->resources.GetRenderTarget();
    if (!target)
    {
        return;
    }
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0.0f));
    if (msimeui::Scene *scene = impl_->window->GetScene())
    {
        const float dpi = impl_->window->GetDpi();
        scene->EnsureLayout({msimeui::PixelsToDips(static_cast<float>(width), dpi),
                             msimeui::PixelsToDips(static_cast<float>(height), dpi)});
        scene->Render(impl_->resources);
    }
    const HRESULT hr = target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        impl_->resources.DiscardTarget();
        return;
    }
    impl_->resources.Present();
}

bool TrayMenuPresenter::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (!bound_ || !impl_ || !impl_->window)
    {
        return false;
    }
    switch (message)
    {
    case WM_ERASEBKGND:
        return true;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Present();
        EndPaint(hwnd_, &ps);
        return true;
    }
    case WM_SIZE:
        if (openToUser_)
        {
            impl_->resources.Resize(static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)));
            Present();
        }
        return true;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MOUSELEAVE:
        impl_->window->DispatchImportedMessage(message, wParam, lParam);
        Present();
        return true;
    default:
        return false;
    }
}

TrayMenuPresenter::~TrayMenuPresenter() = default;
