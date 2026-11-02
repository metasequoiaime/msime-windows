#include "settings/native/native_page_help_widgets.h"

#include <regex>

// partials/help-settings.html + modules/help-settings.ts, default-ime.ts and
// game-compat.ts; styles in styles/modules/document-settings.css.
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

TextStyle HelpParagraphStyle()
{
    // .help-simple-document p: 13px, line-height 1.8, --text-secondary
    return DocStyle(13.0f, DWRITE_FONT_WEIGHT_NORMAL, 1.8f, true);
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

struct HelpState
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
    // proxy
    RadioGroup proxyModes;
    std::shared_ptr<StyledInput> proxyServer;
    std::shared_ptr<Label> proxyStatus;
    std::string proxyMode = "system";
    bool proxyInvalid = false;
    std::unique_ptr<VisibilityWatch> watch;
};

std::wstring DescribeDefaultIme(const Json &response)
{
    if (response.value("isDefault", false))
        return L"水杉输入法已是默认输入法";
    if (response.contains("enabled") && response["enabled"].is_boolean() && !response["enabled"].get<bool>())
        return L"水杉输入法不在当前用户的键盘列表中，设为默认时会自动添加并排到第一位";
    return L"水杉输入法没有排在中文（简体）键盘列表的第一位";
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

void RefreshProxyState(HelpState &state)
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

VisualPtr BuildHelpPage(PageContext &context)
{
    PageBuilder page(L"帮助");
    page.SetTitleRole(TextRole::Secondary);
    SettingsModel *model = &context.model;
    auto state = std::make_shared<HelpState>();
    const auto openUrl = [model](const std::string &url) { model->Post("openExternalUrl", Json(url)); };
    const auto secondary = []() { return CurrentPalette().textSecondary; };

    // .help-document.help-simple-document: padding 24px 28px 26px.
    {
        auto doc = std::make_shared<Column>(0.0f);
        const auto paragraph = [&](std::vector<RichParagraph::Run> runs, float marginTop) {
            auto p = std::make_shared<RichParagraph>(std::move(runs), HelpParagraphStyle(), secondary, openUrl);
            p->SetMaxWidth(780.0f);
            p->SetHorizontalAlignment(msimeui::HorizontalAlignment::Leading);
            p->SetMargin({0.0f, marginTop, 0.0f, 0.0f});
            doc->Add(p);
        };
        paragraph({{L"水杉输入法是一款 Windows 平台的中文输入法。目前支持 Windows11/Windows10 平台。", {}}}, 0.0f);
        paragraph({{L"完整的使用说明见 ", {}},
                   {L"Wiki", "https://github.com/metasequoiaime/msime-windows/wiki"},
                   {L"，遇到问题可以先查看 ", {}},
                   {L"常见问题（FAQ）", "https://github.com/metasequoiaime/msime-windows/wiki/FAQ"},
                   {L"。", {}}},
                  0.0f);
        const auto section = [&](const wchar_t *title, std::vector<const wchar_t *> texts) {
            auto heading = MakeTitle(title);
            heading->SetMargin({0.0f, 28.0f, 0.0f, 0.0f});
            doc->Add(heading);
            for (const wchar_t *text : texts)
                paragraph({{text, {}}}, 8.0f);
        };
        section(L"快速上手", {L"安装输入法后，可以使用 Win + Space 快捷键切换到水杉输入法。默认是全拼输入法。"});
        section(L"基本功能",
                {L"支持全拼、双拼和五笔。可以在设置窗口下的输入功能分区进行切换。全拼和双拼均支持辅助码。辅助码方案目前"
                 L"支持自然码辅助码、蓝天小雨点、首右2."
                 L"0、首右plus、小鹤和拼音加加，也可以把自己的辅助码文件放进数据目录下的 helpcodes\\custom "
                 L"文件夹导入。可以在设置的辅助码分区进行辅助码的开关和切换。打字过程中，全拼使用辅助码需要用大写的字母"
                 L"唤起单码和双码辅助。双拼在开启辅助码的状态下默认使用单码辅助，第二个码是大写字母的情况下，则会使用完"
                 L"整双码进行筛选。",
                 L"语音识别和 AI 联想需要自行填入 API 和 token。内测期间，将默认使用开发者个人的 api 和 "
                 L"token，仅供测试使用。云联想目前支持谷歌的云接口，请用户注意网络问题，需要时可在下方设置网络代理。",
                 L"更多功能欢迎自由探索~"});
        auto card = std::make_shared<PaddedSection>(doc, Thickness{28.0f, 24.0f, 28.0f, 26.0f});
        // .default-ime-section { margin-top: 24px } collapses with the 18px below.
        page.AddRaw(card, 24.0f);
    }

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

        std::weak_ptr<HelpState> weak = state;
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
        post("status");
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

        std::weak_ptr<HelpState> weak = state;
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
        post("status", Json::object());
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

        std::weak_ptr<HelpState> weak = state;
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

    auto root = std::make_shared<OwnerBox>(page.Build(), state);
    // default-ime.ts / game-compat.ts: query again when the page shows or the window
    // regains focus.
    std::weak_ptr<msimeui::Visual> weakRoot = root;
    std::weak_ptr<HelpState> weakState = state;
    state->watch = std::make_unique<VisibilityWatch>(
        context.host.Hwnd(),
        [weakRoot]() {
            auto locked = weakRoot.lock();
            return locked && locked->GetParentVisual() != nullptr;
        },
        [model, weakState]() {
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
        });
    return root;
}
} // namespace native_settings
