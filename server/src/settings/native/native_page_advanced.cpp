#include "settings/native/native_page_help_widgets.h"
#include "settings/profile_icon.h"

#include <shobjidl.h>

#include <filesystem>
#include <regex>

using Microsoft::WRL::ComPtr;

// 高级：系统层面的开关（默认输入法、输入法图标、游戏兼容名称）、网络代理、DR Dict 词典联动、输入法进程与日志、
// 用户数据备份。
// 前四块原在「帮助」页（partials/help-settings.html + default-ime.ts / game-compat.ts），其余原在「关于」页
// （partials/about-settings.html）。
namespace native_settings
{
namespace
{
constexpr wchar_t kProxyHint[] =
    L"仅支持 HTTP 代理，格式为「主机:端口」，如 127.0.0.1:7890；Clash、v2rayN 等客户端的 HTTP/混合端口均可";

bool IsValidProxyServer(const std::wstring &value)
{
    static const std::wregex pattern(LR"(^(?:http://)?(\[[0-9a-f:.]+\]|[a-z0-9._-]+):(\d{1,5})/*$)", std::regex::icase);
    std::wstring trimmed = value;
    trimmed.erase(0, trimmed.find_first_not_of(L" \t"));
    trimmed.erase(trimmed.find_last_not_of(L" \t") + 1);
    std::wsmatch match;
    if (!std::regex_match(trimmed, match, pattern))
        return false;
    const int port = std::stoi(match[2].str());
    return port >= 1 && port <= 65535;
}

std::wstring Trim(std::wstring text)
{
    text.erase(0, text.find_first_not_of(L" \t\r\n"));
    const size_t end = text.find_last_not_of(L" \t\r\n");
    text.erase(end == std::wstring::npos ? 0 : end + 1);
    return text;
}

std::shared_ptr<Label> Description(std::wstring text)
{
    return MakeDescription(std::move(text));
}

CssButton::Style DefaultImeButtonStyle(bool primary)
{
    // .default-ime-button: min-width 92px, padding 7px 14px, 13px, radius 6px.
    CssButton::Style style;
    style.look = primary ? CssButton::Look::Accent : CssButton::Look::Secondary;
    style.minWidth = 92.0f;
    style.paddingX = 14.0f;
    style.paddingY = 7.0f;
    style.fontSize = 13.0f;
    style.transitionMs = 150.0;
    style.pressShift = true;
    return style;
}

// `.default-ime-status[data-kind]`
void SetStatus(Label &label, const std::wstring &message, const char *kind)
{
    label.SetText(message);
    if (std::string(kind) == "success")
        label.SetRole(TextRole::Accent);
    else if (std::string(kind) == "error")
        label.SetRole(TextRole::Danger);
    else
        label.SetRole(TextRole::Muted);
}

// `label.network-proxy-server`: 12px/500 caption over the input, gap 7px, max 360px.
std::shared_ptr<Column> FieldWithCaption(const std::wstring &caption, const std::shared_ptr<StyledInput> &input)
{
    auto column = std::make_shared<Column>(7.0f);
    column->Add(MakeLabel(caption, DocStyle(12.0f, DWRITE_FONT_WEIGHT_MEDIUM), TextRole::Secondary));
    column->Add(input);
    column->SetMaxWidth(360.0f);
    return column;
}

// Re-queries a system status whenever the help page comes back into view or the window
// regains focus (the user may just have returned from the system settings).
class VisibilityWatch
{
  public:
    VisibilityWatch(HWND hwnd, std::function<bool()> visible, std::function<void()> refresh)
        : visible_(std::move(visible)), refresh_(std::move(refresh)), timer_(hwnd, 400, [this, hwnd]() {
              const bool now = visible_() && IsWindowVisible(hwnd) && GetForegroundWindow() == hwnd;
              if (now && !was_)
                  refresh_();
              was_ = now;
          })
    {
    }

  private:
    std::function<bool()> visible_;
    std::function<void()> refresh_;
    bool was_ = true;
    UiTimer timer_;
};

struct BackupState
{
    std::shared_ptr<Button> export_button;
    std::shared_ptr<Button> import_button;
    bool busy = false;
};

void SetBackupBusy(const std::shared_ptr<BackupState> &state, const bool busy)
{
    if (!state)
        return;
    state->busy = busy;
    if (state->export_button)
        state->export_button->SetDisabled(busy);
    if (state->import_button)
        state->import_button->SetDisabled(busy);
}

std::filesystem::path PickBackupDestination(HWND owner)
{
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return {};
    const COMDLG_FILTERSPEC filters[] = {{L"ZIP 备份 (*.zip)", L"*.zip"}, {L"所有文件", L"*.*"}};
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    dialog->SetDefaultExtension(L"zip");
    dialog->SetFileName(L"水杉IME-用户数据.zip");
    dialog->SetTitle(L"导出用户数据");
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);
    ComPtr<IShellItem> item;
    if (FAILED(dialog->Show(owner)) || FAILED(dialog->GetResult(&item)))
        return {};
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return {};
    const std::filesystem::path selected = path;
    CoTaskMemFree(path);
    return selected;
}

std::filesystem::path PickBackupSource(HWND owner)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return {};
    const COMDLG_FILTERSPEC filters[] = {{L"ZIP 备份 (*.zip)", L"*.zip"}, {L"所有文件", L"*.*"}};
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    dialog->SetTitle(L"导入用户数据");
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    ComPtr<IShellItem> item;
    if (FAILED(dialog->Show(owner)) || FAILED(dialog->GetResult(&item)))
        return {};
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return {};
    const std::filesystem::path selected = path;
    CoTaskMemFree(path);
    return selected;
}

struct AdvancedState
{
    // default IME
    std::shared_ptr<CssButton> setDefault;
    std::shared_ptr<Label> defaultStatus;
    std::map<std::string, std::string> defaultPending;
    // game compat
    std::shared_ptr<CheckOption> compatEnabled;
    std::shared_ptr<StyledInput> compatName;
    std::shared_ptr<CssButton> compatApply;
    std::shared_ptr<Label> compatStatus;
    std::map<std::string, std::string> compatPending;
    std::wstring compatDefaultName = L"拼音输入法";
    // profile icon
    RadioGroup iconChoices;
    std::shared_ptr<Hideable> iconCustomTile;
    std::shared_ptr<BitmapImage> iconCustomImage;
    std::shared_ptr<CssButton> iconApply;
    std::shared_ptr<CssButton> iconImport;
    std::shared_ptr<Label> iconStatus;
    std::map<std::string, std::string> iconPending;
    std::string iconCurrent;
    // proxy
    RadioGroup proxyModes;
    std::shared_ptr<StyledInput> proxyServer;
    std::shared_ptr<Label> proxyStatus;
    std::string proxyMode = "system";
    bool proxyInvalid = false;
    std::unique_ptr<VisibilityWatch> watch;
    // user data backup
    std::shared_ptr<BackupState> backup;
};

std::wstring DescribeDefaultIme(const Json &response)
{
    if (response.value("isDefault", false))
        return L"水杉输入法已是默认输入法";
    if (response.contains("enabled") && response["enabled"].is_boolean() && !response["enabled"].get<bool>())
        return L"水杉输入法不在当前用户的键盘列表中，设为默认时会自动添加并排到第一位";
    return L"水杉输入法没有排在中文（简体）键盘列表的第一位";
}

std::wstring DescribeProfileIcon(const Json &response)
{
    const std::string icon = response.value("icon", std::string());
    const std::wstring iconFile = Widen(response.value("iconFile", std::string()));
    if (iconFile.empty())
        return L"水杉输入法没有在系统中注册，请重新安装输入法";
    // A recorded choice whose file is not the registered one: a development registration or a repair install
    // registered the profile without reading the record (Register.cpp falls back when the file is missing).
    if (icon != profile_icon::kDefaultChoice && iconFile.find(L"MetasequoiaImeTip-") == std::wstring::npos)
        return L"记录的图标没有生效，请重新选择并点「应用」";
    if (icon == profile_icon::kCustomChoice)
        return L"当前使用自定义图标";
    if (const profile_icon::Builtin *builtin = profile_icon::FindBuiltin(icon))
        return std::wstring(L"当前使用「") + builtin->label + L"」";
    return L"当前图标：" + iconFile;
}

std::wstring DescribeGameCompat(const Json &response)
{
    const std::wstring name = Widen(response.value("name", std::string()));
    const std::wstring registeredName = Widen(response.value("registeredName", std::string()));
    const std::wstring registered = registeredName.empty() ? L"" : L"当前在系统中登记的名称：" + registeredName;
    if (!response.value("enabled", false))
        return registered.empty() ? L"未启用" : L"未启用。" + registered;
    if (!name.empty() && !registeredName.empty() && name != registeredName)
        return L"已启用「" + name + L"」，但系统中登记的仍是「" + registeredName + L"」，请点「应用」重新登记";
    return registered.empty() ? L"已启用" : L"已启用。" + registered;
}

void RefreshProxyState(AdvancedState &state)
{
    const bool custom = state.proxyMode == "custom";
    state.proxyServer->SetDisabled(!custom);
    if (custom && Trim(state.proxyServer->GetText()).empty())
    {
        state.proxyStatus->SetText(L"请填写代理地址；未填写时按「跟随系统代理设置」处理");
        state.proxyStatus->SetRole(TextRole::Danger);
    }
    else if (!state.proxyInvalid)
    {
        state.proxyStatus->SetText(kProxyHint);
        state.proxyStatus->SetRole(TextRole::Muted);
    }
}
} // namespace

VisualPtr BuildAdvancedPage(PageContext &context)
{
    PageBuilder page(L"高级");
    SettingsModel *model = &context.model;
    auto state = std::make_shared<AdvancedState>();

    // 默认输入法
    {
        auto left = std::make_shared<Column>(0.0f);
        left->Add(MakeTitle(L"默认输入法"));
        left->Add(
            Description(L"设为默认后，水杉输入法会排在系统设置「语言和区域 → "
                        L"中文（简体）」键盘列表的第一位，切到中文时直接使用水杉输入法。不会改动语言顺序和界面语言"));
        state->defaultStatus = Description(L"正在检查…");
        state->defaultStatus->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
        left->Add(state->defaultStatus);
        state->setDefault = std::make_shared<CssButton>(L"设为默认", DefaultImeButtonStyle(true));
        state->setDefault->SetDisabled(true);
        auto system = std::make_shared<CssButton>(L"系统设置", DefaultImeButtonStyle(false));
        auto actions = std::make_shared<Row>(8.0f);
        actions->Add(state->setDefault);
        actions->Add(system);
        auto row = std::make_shared<Row>(20.0f);
        row->Add(left, 1.0f);
        row->Add(actions);
        page.AddSection(row)->SetMargin({0.0f, 0.0f, 0.0f, 24.0f});

        std::weak_ptr<AdvancedState> weak = state;
        const auto post = [model, weak](const std::string &action) {
            auto locked = weak.lock();
            if (!locked)
                return;
            const std::string id = model->NewRequestId("default-ime");
            locked->defaultPending[id] = action;
            model->Post("defaultImeRequest", Json{{"requestId", id}, {"action", action}});
        };
        state->setDefault->SetOnClick([weak, post]() {
            if (auto locked = weak.lock())
            {
                locked->setDefault->SetDisabled(true);
                locked->setDefault->SetText(L"正在设置…");
                SetStatus(*locked->defaultStatus, L"正在设置…", "idle");
            }
            post("setDefault");
        });
        system->SetOnClick([post]() { post("openSystemSettings"); });
        context.OnMessage("defaultImeResponse", [weak](const Json &response) {
            auto locked = weak.lock();
            if (!locked)
                return;
            const auto it = locked->defaultPending.find(response.value("requestId", std::string()));
            if (it == locked->defaultPending.end())
                return;
            const std::string action = it->second;
            locked->defaultPending.erase(it);
            const bool isDefault = response.value("isDefault", false);
            if (action == "setDefault")
                locked->setDefault->SetText(L"设为默认");
            locked->setDefault->SetDisabled(isDefault);
            const std::wstring message = Widen(response.value("message", std::string()));
            if (action == "setDefault" && !response.value("ok", false))
                SetStatus(*locked->defaultStatus, message.empty() ? L"设置失败，请点击「系统设置」手动选择" : message,
                          "error");
            else if (action == "setDefault")
                SetStatus(*locked->defaultStatus, message.empty() ? DescribeDefaultIme(response) : message, "success");
            else
                SetStatus(*locked->defaultStatus, DescribeDefaultIme(response), isDefault ? "success" : "idle");
        });
    }

    // 输入法图标（Win+Space 切换列表里的那个）
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(MakeTitle(L"输入法图标", L"Win+Space 切换输入法时显示的图标"));
        column->Add(Description(L"按 Win+Space 切换输入法、以及系统设置「语言和区域」键盘列表里显示的水杉图标。"
                                L"更换需要管理员权限"));
        // Windows caches the profile icon in explorer; nothing short of restarting it picks up the new file.
        auto restartHint =
            Description(L"应用后需要重启 Windows 资源管理器才会显示新图标：按 Ctrl+Shift+Esc "
                        L"打开任务管理器，在「进程」页找到「Windows 资源管理器」（explorer.exe），"
                        L"右键选择「重新启动」。任务栏会短暂消失再出现，属正常现象；也可以注销后重新登录");
        restartHint->SetMargin({0.0f, 4.0f, 0.0f, 0.0f});
        column->Add(restartHint);
        auto tiles = std::make_shared<Row>(24.0f);
        tiles->SetWrap(true, 12.0f);
        tiles->SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
        const auto makeTile = [](std::shared_ptr<BitmapImage> image, std::shared_ptr<RadioOption> radio) {
            auto tile = std::make_shared<Column>(6.0f);
            image->SetHorizontalAlignment(msimeui::HorizontalAlignment::Leading);
            tile->Add(image);
            radio->SetInnerPadding({0.0f, 4.0f, 4.0f, 4.0f});
            tile->Add(radio);
            return tile;
        };
        for (const profile_icon::Builtin &builtin : profile_icon::Builtins())
        {
            // BitmapImage decodes on first paint, so building the page for the search index stays cheap.
            auto image = std::make_shared<BitmapImage>(AssetPath(builtin.asset), 40.0f, 40.0f);
            tiles->Add(makeTile(image, state->iconChoices.Add(builtin.label, builtin.id)));
        }
        // The current custom icon, read from where the elevated helper put it; only shown while it is in use.
        state->iconCustomImage = std::make_shared<BitmapImage>(L"", 40.0f, 40.0f);
        state->iconCustomTile = std::make_shared<Hideable>(
            makeTile(state->iconCustomImage, state->iconChoices.Add(L"自定义", profile_icon::kCustomChoice)), false);
        tiles->Add(state->iconCustomTile);
        state->iconChoices.SetValue(profile_icon::kDefaultChoice);
        column->Add(tiles);

        state->iconApply = std::make_shared<CssButton>(L"应用", DefaultImeButtonStyle(true));
        state->iconApply->SetDisabled(true);
        state->iconImport = std::make_shared<CssButton>(L"选择图片…", DefaultImeButtonStyle(false));
        auto actions = std::make_shared<Row>(8.0f);
        actions->SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
        actions->Add(state->iconApply);
        actions->Add(state->iconImport);
        column->Add(actions);
        state->iconStatus = Description(L"正在检查…");
        state->iconStatus->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
        column->Add(state->iconStatus);
        page.AddSection(column)->SetMargin({0.0f, 0.0f, 0.0f, 24.0f});

        std::weak_ptr<AdvancedState> weak = state;
        const auto refreshApply = [weak]() {
            auto locked = weak.lock();
            if (!locked)
                return;
            bool busy = false;
            for (const auto &entry : locked->iconPending)
                busy = busy || entry.second != "status";
            const std::string &selected = locked->iconChoices.GetValue();
            // "custom" has nothing to apply by itself: picking a new image is what the import button is for.
            locked->iconApply->SetDisabled(busy || selected == locked->iconCurrent ||
                                           selected == profile_icon::kCustomChoice);
            locked->iconImport->SetDisabled(busy);
        };
        state->iconChoices.SetOnChanged([refreshApply](const std::string &) { refreshApply(); });
        const auto post = [model, weak, refreshApply](const std::string &action, Json extra) {
            auto locked = weak.lock();
            if (!locked)
                return;
            const std::string id = model->NewRequestId("profile-icon");
            locked->iconPending[id] = action;
            Json data = {{"requestId", id}, {"action", action}};
            for (auto &[key, value] : extra.items())
                data[key] = value;
            refreshApply();
            model->Post("profileIconRequest", data);
        };
        state->iconApply->SetOnClick([weak, post]() {
            auto locked = weak.lock();
            if (!locked)
                return;
            locked->iconApply->SetText(L"正在应用…");
            SetStatus(*locked->iconStatus, L"正在应用，请在弹出的 UAC 窗口中确认…", "idle");
            post("apply", Json{{"icon", locked->iconChoices.GetValue()}});
        });
        state->iconImport->SetOnClick([weak, post]() {
            if (auto locked = weak.lock())
                SetStatus(*locked->iconStatus, L"请选择图片；确认后会弹出 UAC 窗口…", "idle");
            post("import", Json::object());
        });
        context.OnMessage("profileIconResponse", [weak, refreshApply](const Json &response) {
            auto locked = weak.lock();
            if (!locked)
                return;
            const auto it = locked->iconPending.find(response.value("requestId", std::string()));
            if (it == locked->iconPending.end())
                return;
            const std::string action = it->second;
            locked->iconPending.erase(it);
            locked->iconApply->SetText(L"应用");
            const std::string icon = response.value("icon", std::string());
            const std::wstring iconFile = Widen(response.value("iconFile", std::string()));
            if (!icon.empty())
            {
                locked->iconCurrent = icon;
                locked->iconChoices.SetValue(icon);
            }
            const bool custom = icon == profile_icon::kCustomChoice;
            locked->iconCustomTile->SetVisible(custom);
            locked->iconCustomImage->SetPath(custom ? iconFile : std::wstring());
            refreshApply();
            const std::wstring message = Widen(response.value("message", std::string()));
            const bool ok = response.value("ok", false);
            if (response.value("cancelled", false))
                SetStatus(*locked->iconStatus, DescribeProfileIcon(response), "idle");
            else if (!ok)
                SetStatus(*locked->iconStatus, message.empty() ? L"应用失败，请重试" : message, "error");
            else if (action != "status" && !message.empty())
                SetStatus(*locked->iconStatus,
                          message + L"请在任务管理器中重新启动「Windows 资源管理器」（explorer.exe）后生效", "success");
            else
                SetStatus(*locked->iconStatus, DescribeProfileIcon(response), "idle");
        });
    }

    // 游戏兼容
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(MakeTitle(L"游戏兼容（伪装名称 实验性）"));
        column->Add(Description(L"部分游戏（CS2、Dota 2）使用白名单，只允许固定几个输入法使用；不在名单里无法使用"));
        column->Add(Description(L"功能不稳定：已知问题包括，上屏后的第一下退格会被游戏吞掉（游戏那边的行为）。"
                                L"建议保持默认「拼音输入法」，或者改成任意大厂输入法的名称。"));
        state->compatEnabled = std::make_shared<CheckOption>(L"启用游戏兼容名称");
        state->compatEnabled->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
        state->compatEnabled->SetHorizontalAlignment(msimeui::HorizontalAlignment::Leading);
        column->Add(state->compatEnabled);
        state->compatName = std::make_shared<StyledInput>(L"拼音输入法", 12.0f);
        state->compatName->SetDisabled(true);
        auto field = FieldWithCaption(L"伪装名称", state->compatName);
        state->compatApply = std::make_shared<CssButton>(L"应用", DefaultImeButtonStyle(true));
        state->compatApply->SetDisabled(true);
        auto row = std::make_shared<Row>(20.0f);
        row->SetAlign(Row::Align::End);
        row->SetJustify(Row::Justify::SpaceBetween);
        row->SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
        row->Add(field);
        row->Add(state->compatApply);
        column->Add(row);
        state->compatStatus = Description(L"正在检查…");
        state->compatStatus->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
        column->Add(state->compatStatus);
        page.AddSection(column)->SetMargin({0.0f, 0.0f, 0.0f, 24.0f});

        std::weak_ptr<AdvancedState> weak = state;
        state->compatEnabled->SetOnChanged([weak](bool checked) {
            if (auto locked = weak.lock())
                locked->compatName->SetDisabled(!checked);
        });
        const auto post = [model, weak](const std::string &action, Json extra) {
            auto locked = weak.lock();
            if (!locked)
                return;
            const std::string id = model->NewRequestId("game-compat");
            locked->compatPending[id] = action;
            Json data = {{"requestId", id}, {"action", action}};
            for (auto &[key, value] : extra.items())
                data[key] = value;
            model->Post("gameCompatRequest", data);
        };
        state->compatApply->SetOnClick([weak, post]() {
            auto locked = weak.lock();
            if (!locked)
                return;
            const bool enabled = locked->compatEnabled->IsChecked();
            locked->compatApply->SetDisabled(true);
            locked->compatApply->SetText(L"正在应用…");
            SetStatus(*locked->compatStatus, L"正在应用，请在弹出的 UAC 窗口中确认…", "idle");
            Json extra = {{"enabled", enabled}};
            if (enabled)
            {
                std::wstring name = Trim(locked->compatName->GetText());
                if (name.empty())
                    name = locked->compatDefaultName;
                extra["name"] = Narrow(name.substr(0, 64));
            }
            post("apply", extra);
        });
        context.OnMessage("gameCompatResponse", [weak](const Json &response) {
            auto locked = weak.lock();
            if (!locked)
                return;
            const auto it = locked->compatPending.find(response.value("requestId", std::string()));
            if (it == locked->compatPending.end())
                return;
            const std::string action = it->second;
            locked->compatPending.erase(it);
            const std::string defaultName = response.value("defaultName", std::string());
            if (!defaultName.empty())
                locked->compatDefaultName = Widen(defaultName);
            locked->compatName->SetPlaceholder(locked->compatDefaultName);
            if (response.contains("enabled") && response["enabled"].is_boolean())
                locked->compatEnabled->SetChecked(response["enabled"].get<bool>());
            if (response.contains("name") && response["name"].is_string())
                locked->compatName->SetText(Widen(response["name"].get<std::string>()));
            locked->compatName->SetDisabled(!locked->compatEnabled->IsChecked());
            locked->compatApply->SetDisabled(false);
            locked->compatApply->SetText(L"应用");
            const std::wstring message = Widen(response.value("message", std::string()));
            const bool ok = response.value("ok", false);
            if (action == "apply")
            {
                if (!ok)
                    SetStatus(*locked->compatStatus, message.empty() ? L"应用失败，请重试" : message, "error");
                else if (!message.empty())
                    SetStatus(*locked->compatStatus,
                              message + L" 任务栏和 Win+Space 仍然显示旧名称时，重启资源管理器或重新登录即可",
                              "success");
                else
                    SetStatus(*locked->compatStatus, DescribeGameCompat(response), "success");
                return;
            }
            if (!ok)
                SetStatus(*locked->compatStatus, message.empty() ? L"无法读取游戏兼容状态" : message, "error");
            else
                SetStatus(*locked->compatStatus, DescribeGameCompat(response),
                          response.value("enabled", false) ? "success" : "idle");
        });
    }

    // 网络代理
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(MakeTitle(L"网络代理"));
        column->Add(
            Description(L"云输入、语法模型下载、AI 联想、翻译和语音识别等联网功能共用这里的设置，修改后立即生效"));
        auto options = std::make_shared<Column>(0.0f);
        options->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
        const std::pair<const char *, const wchar_t *> modes[] = {
            {"system", L"跟随系统代理设置"}, {"none", L"不使用代理（直连）"}, {"custom", L"使用自定义 HTTP 代理"}};
        for (size_t i = 0; i < std::size(modes); ++i)
        {
            if (i > 0)
                options->Add(std::make_shared<Divider>(Divider::Kind::Option));
            auto radio = state->proxyModes.Add(modes[i].second, modes[i].first);
            radio->SetInnerPadding({4.0f, 8.0f, 4.0f, 8.0f});
            radio->SetHorizontalAlignment(msimeui::HorizontalAlignment::Leading);
            options->Add(radio);
        }
        state->proxyModes.SetValue("system");
        column->Add(options);
        state->proxyServer = std::make_shared<StyledInput>(L"127.0.0.1:7890", 12.0f);
        auto field = FieldWithCaption(L"代理地址", state->proxyServer);
        field->SetMargin({0.0f, 12.0f, 0.0f, 0.0f});
        column->Add(field);
        state->proxyStatus = Description(kProxyHint);
        state->proxyStatus->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
        column->Add(state->proxyStatus);
        page.AddSection(column);

        std::weak_ptr<AdvancedState> weak = state;
        state->proxyModes.SetOnChanged([model, weak](const std::string &value) {
            auto locked = weak.lock();
            if (!locked)
                return;
            locked->proxyMode = value;
            model->Update("network.proxy_mode", value);
            RefreshProxyState(*locked);
        });
        state->proxyServer->SetOnCommit([model, weak](const std::wstring &text) {
            auto locked = weak.lock();
            if (!locked)
                return;
            const std::wstring value = Trim(text);
            if (!value.empty() && !IsValidProxyServer(value))
            {
                locked->proxyInvalid = true;
                locked->proxyServer->SetInvalid(true);
                locked->proxyStatus->SetText(
                    L"代理地址格式不正确：应为「主机:端口」，如 127.0.0.1:7890（不支持 socks5://）");
                locked->proxyStatus->SetRole(TextRole::Danger);
                return;
            }
            locked->proxyInvalid = false;
            locked->proxyServer->SetInvalid(false);
            model->Update("network.proxy_server", Narrow(value));
            RefreshProxyState(*locked);
        });
        context.OnConfig([model, weak]() {
            auto locked = weak.lock();
            if (!locked)
                return;
            const std::string mode = model->GetString("network.proxy_mode");
            if (mode == "system" || mode == "none" || mode == "custom")
            {
                locked->proxyMode = mode;
                locked->proxyModes.SetValue(mode);
            }
            if (const Json *server = model->Find("network.proxy_server");
                server && server->is_string() && !locked->proxyServer->IsFocused())
            {
                locked->proxyServer->SetText(Widen(server->get<std::string>()));
                locked->proxyInvalid = false;
                locked->proxyServer->SetInvalid(false);
            }
            RefreshProxyState(*locked);
        });
        RefreshProxyState(*state);
    }

    // DR Dict 词典联动：总开关管候选右键菜单的「查词典」，两个快捷键各自另有开关，总开关关着时置灰。
    {
        auto candidateHotkey = BindToggle(context, "dr_dict.lookup_candidate_ctrl_shift_enter");
        auto preeditHotkey = BindToggle(context, "dr_dict.lookup_preedit_ctrl_alt_enter");
        page.AddSection(MakeRows({
            MakeRow(L"DR Dict 词典联动",
                    L"在候选的右键菜单中增加「查词典」，在 DR Dict 的弹窗中查询该候选。需要安装 DR Dict "
                    L"并至少运行过一次；DR Dict 没在运行时会自动启动它。",
                    BindToggle(context, "dr_dict.enabled")),
            MakeRow(L"Ctrl+Shift+Enter 查询高亮候选",
                    L"有候选时按下，在 DR Dict 中查询当前高亮的候选，并像按 Esc "
                    L"一样清掉当前输入，不上屏。开启上面的词典联动后生效。",
                    candidateHotkey),
            MakeRow(L"Ctrl+Alt+Enter 查询预编辑",
                    L"候选窗显示时按下，在 DR Dict 中查询当前输入的原始字母（例如英文单词），并像按 Esc "
                    L"一样清掉当前输入，不上屏。由输入法的全局键盘钩子识别，Edge、Chrome 里也能用；"
                    L"候选窗不显示时不拦截。开启上面的词典联动后生效。",
                    preeditHotkey),
        }));
        SettingsModel *dictModel = &context.model;
        std::weak_ptr<ToggleSwitch> weakCandidate = candidateHotkey;
        std::weak_ptr<ToggleSwitch> weakPreedit = preeditHotkey;
        context.OnConfig([dictModel, weakCandidate, weakPreedit]() {
            const bool disabled = !dictModel->GetBool("dr_dict.enabled");
            if (auto toggle = weakCandidate.lock())
                toggle->SetDisabled(disabled);
            if (auto toggle = weakPreedit.lock())
                toggle->SetDisabled(disabled);
        });
    }

    // 输入法进程与设置窗口
    {
        Dropdown::Options linger;
        linger.minWidth = 150.0f;
        linger.matchToggleWidth = true;
        auto restart = std::make_shared<Button>(L"重启", ButtonKind::Secondary);
        std::weak_ptr<Button> weakRestart = restart;
        restart->SetOnClick([model, weakRestart]() {
            // 重启由 Watchdog 执行。按钮在收到宿主的 restartServerDone 之前保持禁用，
            // 防止连点排队；这里不自己起定时器，窗口隐藏时它会随窗口一起失效。
            auto button = weakRestart.lock();
            if (!button || button->IsDisabled())
                return;
            button->SetDisabled(true);
            button->SetText(L"重启中…");
            model->Post("restartServer");
        });
        context.OnMessage("restartServerDone", [weakRestart](const Json &) {
            if (auto button = weakRestart.lock())
            {
                button->SetDisabled(false);
                button->SetText(L"重启");
            }
        });
        page.AddSection(MakeRows({
            MakeRow(L"重启输入法进程", L"立即重启输入法后台进程，使需要重启的设置生效。", restart),
            MakeRow(L"设置窗口后台驻留",
                    L"关闭设置窗口后在后台保留一段时间，期间重新打开无需等待加载；驻留越久占用内存越久。",
                    BindDropdown(context, "appearance.settings_window_linger",
                                 Options({{"off", L"关闭后立即退出"},
                                          {"1m", L"1 分钟"},
                                          {"5m", L"5 分钟"},
                                          {"10m", L"10 分钟"},
                                          {"30m", L"30 分钟"},
                                          {"60m", L"1 小时"},
                                          {"forever", L"一直驻留"}}),
                                 linger)),
        }));
    }

    page.AddSection(MakeRow(L"启动动画",
                            L"打开设置窗口时先在窗口中央显示齿轮图标，再淡入主界面。关闭后只短暂显示纯色背景。",
                            BindToggle(context, "appearance.settings_launch_animation")));
    page.AddSection(
        MakeRow(L"Server 端日志",
                L"排查 Server "
                L"通信和输入延迟时开启。记录慢请求阶段、候选窗、悬浮工具栏、菜单、焦点会话和通信状态，不记录按键、输入"
                L"内容或候选文本。日志写到数据目录的 logs 文件夹，文件名带生成时间，例如 "
                L"水杉IME诊断日志-server-20261029-153012.log。",
                BindToggle(context, "general.diagnostic_log")));
    page.AddSection(MakeRow(L"TSF 端日志",
                            L"排查应用内预编辑和输入延迟时开启。TSF "
                            L"日志会在内存中限量缓冲，并通过独立管道批量汇总到同一个 logs 文件夹，文件名以 tsf "
                            L"标识，与 Server 日志分开保存；不记录按键、输入内容或候选文本。",
                            BindToggle(context, "general.tsf_diagnostic_log")));
    {
        auto openLogs = std::make_shared<Button>(L"打开日志目录", ButtonKind::Secondary);
        openLogs->SetOnClick([model]() { model->Post("openLogDirectory"); });
        page.AddSection(MakeRow(
            L"日志文件", L"复现一次后打开日志文件夹，发送最近生成的那份文件。最多保留最近 10 个文件，旧的自动删除。",
            openLogs));
    }
    // 用户数据备份：导出不停 Server，导入先完整校验再停机应用。
    {
        auto backup = std::make_shared<BackupState>();
        state->backup = backup;
        auto export_button = std::make_shared<Button>(L"导出…", ButtonKind::Secondary);
        auto import_button = std::make_shared<Button>(L"导入…", ButtonKind::Secondary);
        backup->export_button = export_button;
        backup->import_button = import_button;
        auto include_models = std::make_shared<ToggleSwitch>(true);
        const HWND owner = context.host.Hwnd();
        std::weak_ptr<BackupState> weak_backup = backup;
        export_button->SetOnClick([model, owner, weak_backup, include_models]() {
            const std::shared_ptr<BackupState> current = weak_backup.lock();
            if (!current || current->busy)
                return;
            const std::filesystem::path destination = PickBackupDestination(owner);
            if (destination.empty())
                return;
            SetBackupBusy(current, true);
            model->Post("userDataBackupRequest", Json{{"action", "export"},
                                                      {"path", destination.u8string()},
                                                      {"includeModels", include_models->IsChecked()}});
        });
        import_button->SetOnClick([model, owner, weak_backup, host = &context.host]() {
            const std::shared_ptr<BackupState> current = weak_backup.lock();
            if (!current || current->busy)
                return;
            const std::filesystem::path source = PickBackupSource(owner);
            if (source.empty())
                return;
            host->Confirm(
                L"导入会先停止输入法后台进程，并把将被覆盖的配置、统计、皮肤和模型备份到数据目录的 "
                L".backup-before-import。词库只合并条目，不会替换现有数据库。",
                [model, weak_backup, source](bool confirmed) {
                    const std::shared_ptr<BackupState> current = weak_backup.lock();
                    if (!confirmed || !current || current->busy)
                        return;
                    SetBackupBusy(current, true);
                    model->Post("userDataBackupRequest", Json{{"action", "import"}, {"path", source.u8string()}});
                },
                L"导入用户数据", L"继续导入", L"取消");
        });
        context.OnMessage("userDataBackupDone", [weak_backup, host = &context.host](const Json &message) {
            const std::shared_ptr<BackupState> current = weak_backup.lock();
            if (!current)
                return;
            SetBackupBusy(current, false);
            const bool ok = message.contains("ok") && message["ok"].is_boolean() && message["ok"].get<bool>();
            const std::string text = message.contains("message") && message["message"].is_string()
                                         ? message["message"].get<std::string>()
                                         : std::string(ok ? "操作完成" : "操作失败");
            host->ShowToast(Widen(text), ok);
        });
        page.AddSection(MakeRows({
            MakeRow(L"导出用户数据", L"把配置、统计、用户皮肤、用户词条和模型打包成 ZIP。导出时输入法继续运行。",
                    export_button),
            MakeRow(L"包含已下载模型",
                    L"勾选时把数据目录 models/ 下的模型一起打包；导入按 ZIP 内容恢复，没有模型条目的 ZIP "
                    L"不会删除本地已有模型。",
                    include_models),
            MakeRow(L"导入用户数据", L"先完整校验 ZIP，通过后才停止输入法并覆盖文件；词库只合并条目。", import_button),
        }));
    }

    // default-ime.ts / game-compat.ts: query when the page shows or the window regains focus.
    // 索引器会在页面被打开之前就建好它，所以构建期一次都不发；首次显示由 root 的 Attach 补上。
    std::weak_ptr<AdvancedState> weakState = state;
    const auto refresh_statuses = [model, weakState]() {
        auto locked = weakState.lock();
        if (!locked)
            return;
        bool busy = false;
        for (const auto &entry : locked->defaultPending)
            busy = busy || entry.second == "setDefault";
        if (!busy)
        {
            const std::string id = model->NewRequestId("default-ime");
            locked->defaultPending[id] = "status";
            model->Post("defaultImeRequest", Json{{"requestId", id}, {"action", "status"}});
        }
        busy = false;
        for (const auto &entry : locked->compatPending)
            busy = busy || entry.second == "apply";
        if (!busy)
        {
            const std::string id = model->NewRequestId("game-compat");
            locked->compatPending[id] = "status";
            model->Post("gameCompatRequest", Json{{"requestId", id}, {"action", "status"}});
        }
        busy = false;
        for (const auto &entry : locked->iconPending)
            busy = busy || entry.second != "status";
        if (!busy)
        {
            const std::string id = model->NewRequestId("profile-icon");
            locked->iconPending[id] = "status";
            model->Post("profileIconRequest", Json{{"requestId", id}, {"action", "status"}});
        }
    };
    auto root = std::make_shared<OwnerBox>(page.Build(), state, refresh_statuses);
    std::weak_ptr<msimeui::Visual> weakRoot = root;
    state->watch = std::make_unique<VisibilityWatch>(
        context.host.Hwnd(),
        [weakRoot]() {
            auto locked = weakRoot.lock();
            return locked && locked->GetParentVisual() != nullptr;
        },
        refresh_statuses);
    return root;
}
} // namespace native_settings
