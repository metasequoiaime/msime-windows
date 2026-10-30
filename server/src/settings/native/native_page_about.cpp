#include "settings/native/native_page_help_widgets.h"

#include "msime_version.h"
#include "settings/native/native_settings_update_download.h"
#include "settings/update_channel.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Window.h"

#include <shellapi.h>
#include <winhttp.h>

#include <chrono>
#include <cstdio>
#include <regex>

// partials/about-settings.html + modules/about-settings.ts + modules/update-manifest.ts;
// styles in styles/modules/document-settings.css (.about-*).
namespace native_settings
{
namespace
{
constexpr char kUpdateManifestUrl[] = "https://msime.app/update.json";
constexpr char kReleasesPageUrl[] = "https://github.com/metasequoiaime/MSIME-Windows/releases";
constexpr char kDownloadMirrorPrefix[] = "https://dl.msime.app/gh/";
constexpr char kWebsiteUrl[] = "https://msime.app/";
constexpr char kLicenseUrl[] = "https://github.com/metasequoiaime/MSIME-Windows/blob/main/LICENSE";
constexpr char kPrivacyUrl[] = "https://github.com/metasequoiaime/MSIME-Windows/blob/main/PRIVACY.md";
// The version the page shows (partials/about-settings.html), sourced from version.txt.
constexpr char kShownVersion[] = "v" MSIME_PRODUCT_VERSION;

struct Version
{
    std::string display;
    std::vector<long> parts;
};

bool ParseVersion(const std::string &value, Version &version)
{
    static const std::regex pattern(R"(^\s*[vV]?(\d+(?:\.\d+)*)(?:[-+].*)?\s*$)");
    std::smatch match;
    if (!std::regex_match(value, match, pattern))
        return false;
    version.display = match[1].str();
    version.parts.clear();
    size_t start = 0;
    while (start <= version.display.size())
    {
        const size_t dot = version.display.find('.', start);
        version.parts.push_back(std::stol(version.display.substr(start, dot - start)));
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    return true;
}

int CompareVersions(const Version &left, const Version &right)
{
    const size_t length = std::max(left.parts.size(), right.parts.size());
    for (size_t i = 0; i < length; ++i)
    {
        const long l = i < left.parts.size() ? left.parts[i] : 0;
        const long r = i < right.parts.size() ? right.parts[i] : 0;
        if (l != r)
            return l < r ? -1 : 1;
    }
    return 0;
}

// GET over WinHTTP; empty on any failure. Runs on a worker thread.
std::string HttpGet(const std::string &url)
{
    const std::wstring wide = Widen(url);
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256]{};
    wchar_t path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts))
        return {};
    std::string body;
    HINTERNET session =
        WinHttpOpen(L"MetasequoiaImeSettings", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session)
        return {};
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request =
        connection
            ? WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
            : nullptr;
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr) &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status,
                            &statusSize, nullptr) &&
        status == 200)
    {
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0 && body.size() < 1024 * 1024)
        {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
                break;
            body.append(chunk.data(), read);
        }
    }
    if (request)
        WinHttpCloseHandle(request);
    if (connection)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return body;
}

// `.about-link-row`: min-height 66px with a hairline under every row but the last.
class AboutRow : public msimeui::Panel
{
  public:
    AboutRow(VisualPtr left, VisualPtr right, bool last) : left_(std::move(left)), right_(std::move(right)), last_(last)
    {
        AddChild(left_);
        if (right_)
            AddChild(right_);
    }

    void SetOnClick(std::function<void()> handler, std::shared_ptr<Label> title)
    {
        onClick_ = std::move(handler);
        title_ = std::move(title);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        rightSize_ = right_ ? right_->MeasureInLayout(availableSize) : SizeF{};
        leftSize_ = left_->MeasureInLayout(
            {std::max(availableSize.width - (right_ ? rightSize_.width + 20.0f : 0.0f), 0.0f), availableSize.height});
        const float border = last_ ? 0.0f : CssBorder();
        return {availableSize.width, std::max(66.0f, std::max(leftSize_.height, rightSize_.height) + border)};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
        const float border = last_ ? 0.0f : CssBorder();
        const float height = finalRect.height - border;
        left_->ArrangeInLayout(
            {finalRect.x, finalRect.y + (height - leftSize_.height) * 0.5f, leftSize_.width, leftSize_.height});
        if (right_)
            right_->ArrangeInLayout({finalRect.x + finalRect.width - rightSize_.width,
                                     finalRect.y + (height - rightSize_.height) * 0.5f, rightSize_.width,
                                     rightSize_.height});
    }

    void Render(msimeui::DeviceResources &dr) override
    {
        if (title_)
            title_->SetColor(hovered_ ? CurrentPalette().accentStrong : CurrentPalette().text);
        left_->Render(dr);
        if (right_)
            right_->Render(dr);
        if (!last_)
        {
            const float border = CssBorder();
            FillRect(dr, {bounds_.x, bounds_.y + bounds_.height - border, bounds_.width, border},
                     CurrentPalette().border);
        }
    }

    msimeui::Visual *FindVisualAt(const PointF &point) override
    {
        if (onClick_)
            return HitTest(point) ? this : nullptr;
        return msimeui::Panel::FindVisualAt(point);
    }

    bool OnMouseDown(const POINT &point, WPARAM) override
    {
        pressed_ = onClick_ && window_ && HitTest(window_->ClientPixelsToDips(point));
        return pressed_;
    }

    bool OnMouseUp(const POINT &point, WPARAM) override
    {
        if (!pressed_)
            return false;
        pressed_ = false;
        if (window_ && HitTest(window_->ClientPixelsToDips(point)) && onClick_)
        {
            // A handler that closes the dialog destroys this row and onClick_ mid-call.
            const auto handler = onClick_;
            handler();
        }
        return true;
    }

    void OnMouseEnter() override
    {
        hovered_ = true;
        InvalidateVisual();
    }

    void OnMouseLeave() override
    {
        hovered_ = false;
        pressed_ = false;
        InvalidateVisual();
    }

    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, onClick_ ? IDC_HAND : IDC_ARROW);
    }

  private:
    VisualPtr left_;
    VisualPtr right_;
    bool last_ = false;
    SizeF leftSize_ = {};
    SizeF rightSize_ = {};
    std::function<void()> onClick_;
    std::shared_ptr<Label> title_;
    bool hovered_ = false;
    bool pressed_ = false;
};

// `.about-mark`: 52px tile with the 32px logo.
class AboutMark : public msimeui::Visual
{
  public:
    SizeF Measure(const SizeF &) override
    {
        return {52.0f, 52.0f};
    }
    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }
    void Render(msimeui::DeviceResources &dr) override
    {
        const Palette &p = CurrentPalette();
        FillRounded(dr, bounds_, 11.0f, p.surfaceSunken);
        StrokeRounded(dr, bounds_, 11.0f, p.border, 1.0f);
        logo_.Arrange({bounds_.x + 10.0f, bounds_.y + 10.0f, 32.0f, 32.0f});
        logo_.Render(dr);
    }

  private:
    BitmapImage logo_{AssetPath(L"msime.png"), 32.0f, 32.0f};
};

CssButton::Style DialogButtonStyle(bool primary)
{
    // .about-dialog-button: min-width 76px, padding 7px 13px, radius 6px, 12px.
    CssButton::Style style;
    style.look = primary ? CssButton::Look::Accent : CssButton::Look::Secondary;
    style.minWidth = 76.0f;
    style.paddingX = 13.0f;
    style.paddingY = 7.0f;
    style.fontSize = 12.0f;
    return style;
}

struct AboutState
{
    std::shared_ptr<CssButton> check;
    std::shared_ptr<Label> status;
    std::shared_ptr<Hideable> statusBox;
};

// 收尾：恢复按钮，把结论写给用户看。下载版复用它。
inline void FinishUpdateDownload(const std::shared_ptr<AboutState> &state, const bool ok, const std::wstring &message)
{
    state->check->SetDisabled(false);
    state->check->SetText(L"检查更新");
    state->status->SetText(message);
    state->status->SetRole(ok ? TextRole::Accent : TextRole::Danger);
    state->statusBox->SetVisible(true);
}

// 下载结束后恢复按钮并给出结果。成功时先用清单摘要重新核对一遍磁盘上的文件，
// 再交给系统打开：它已经校验过，接下来是 UAC 和 Inno Setup 自己的界面，每一步仍由
// 用户确认。重核对是为了堵住「校验完成」到「用户点 UAC」之间被替换的窗口。
void FinishUpdateDownload(const std::shared_ptr<AboutState> &state, const std::string &installer_path,
                          const std::string &expected_digest, const std::string &message)
{
    if (installer_path.empty())
    {
        FinishUpdateDownload(state, false, Widen(message.empty() ? "下载失败" : message));
        return;
    }
    const std::filesystem::path path = std::filesystem::u8path(installer_path);
    if (!update_download::FileMatchesDigest(path, expected_digest))
    {
        FinishUpdateDownload(state, false, L"安装包在打开前核对失败，请重试");
        return;
    }
    const HINSTANCE opened = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) <= 32)
        FinishUpdateDownload(state, false, L"安装包已下载，但无法自动打开，请重试");
    else
        FinishUpdateDownload(state, true, L"安装包已下载并通过校验，已打开安装程序");
}

void ShowUpdateDialog(PageContext &context, const Version &latest, const std::string &releaseUrl,
                      const std::string &mirrorUrl, const nlohmann::json &manifest)
{
    SettingsModel *model = &context.model;
    HostServices *host = &context.host;
    auto column = std::make_shared<Column>(0.0f);
    column->Add(MakeLabel(L"发现新版本", DocStyle(16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD), TextRole::Primary));
    auto message = MakeLabel(L"水杉 IME v" + Widen(latest.display) + L" 已发布，是否前往下载？",
                             DocStyle(13.0f, DWRITE_FONT_WEIGHT_NORMAL, 1.65f, true), TextRole::Muted);
    message->SetMargin({0.0f, 10.0f, 0.0f, 0.0f});
    column->Add(message);

    // describeInstallerTrust
    static const std::regex installerName(R"(^MetasequoiaIME_Setup_v[\w.-]+\.exe$)", std::regex::icase);
    static const std::regex sha256(R"(^[0-9a-f]{64}$)");
    const std::string name = manifest.value("installerName", std::string());
    const std::string digest = manifest.value("installerSha256", std::string());
    const bool nameValid = std::regex_match(name, installerName);
    if (manifest.contains("signed") && manifest["signed"].is_boolean() && !manifest["signed"].get<bool>())
    {
        auto warning = MakeLabel(L"该版本未经代码签名，SmartScreen 会拦截，且 uiAccess "
                                 L"失效（候选窗无法浮在以管理员身份运行的程序之上）。请务必核对下面的校验值。",
                                 DocStyle(13.0f, DWRITE_FONT_WEIGHT_NORMAL, 1.65f, true), TextRole::Danger);
        warning->SetMargin({0.0f, 10.0f, 0.0f, 0.0f});
        column->Add(warning);
    }
    if (std::regex_match(digest, sha256))
    {
        auto label = MakeLabel(L"下载后请核对 SHA256：", DocStyle(13.0f, DWRITE_FONT_WEIGHT_NORMAL, 1.65f, true),
                               TextRole::Muted);
        label->SetMargin({0.0f, 10.0f, 0.0f, 6.0f});
        column->Add(label);
        const std::wstring file = nameValid ? Widen(name) : L"MetasequoiaIME_Setup_v<版本>.exe";
        TextStyle code = MonoStyle(11.0f);
        code.lineHeight = 1.5f;
        code.wrap = true;
        column->Add(MakeLabel(L"Get-FileHash .\\" + file + L" -Algorithm SHA256", code, TextRole::Primary));
        auto digestLabel = MakeLabel(Widen(digest), code, TextRole::Primary);
        digestLabel->SetMargin({0.0f, 6.0f, 0.0f, 0.0f});
        column->Add(digestLabel);
    }

    auto actions = std::make_shared<Row>(9.0f);
    actions->SetJustify(Row::Justify::End);
    actions->SetMargin({0.0f, 18.0f, 0.0f, 0.0f});
    auto later = std::make_shared<CssButton>(L"稍后", DialogButtonStyle(false));
    later->SetOnClick([host]() { host->CloseModal(); });
    actions->Add(later);
    if (!mirrorUrl.empty())
    {
        auto mirror = std::make_shared<CssButton>(L"国内镜像下载", DialogButtonStyle(false));
        mirror->SetOnClick([host, model, mirrorUrl]() {
            host->CloseModal();
            model->Post("openExternalUrl", Json(mirrorUrl));
        });
        actions->Add(mirror);
    }
    auto download = std::make_shared<CssButton>(L"前往下载", DialogButtonStyle(true));
    download->SetOnClick([host, model, releaseUrl]() {
        host->CloseModal();
        model->Post("openExternalUrl", Json(releaseUrl));
    });
    actions->Add(download);
    column->Add(actions);
    context.host.ShowModal(column);
}

// 手动检查：只回答「有没有新版本」，新版本由用户自己决定去哪下载。
void CheckForUpdate(PageContext &context, const std::shared_ptr<AboutState> &state)
{
    const auto setStatus = [state](const std::wstring &message, TextRole role) {
        state->status->SetText(message);
        state->status->SetRole(role);
        state->statusBox->SetVisible(!message.empty());
    };
    Version current;
    if (!ParseVersion(kShownVersion, current))
    {
        setStatus(L"无法识别当前版本", TextRole::Danger);
        return;
    }
    state->check->SetDisabled(true);
    state->check->SetText(L"正在检查…");
    setStatus(L"", TextRole::Muted);
    PageContext *ctx = &context;
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    const std::string url = std::string(kUpdateManifestUrl) + "?t=" + std::to_string(stamp);
    RunInBackground(
        context.host.Hwnd(), [url]() { return HttpGet(url); },
        [ctx, state, current, setStatus](const std::string &body) {
            state->check->SetDisabled(false);
            state->check->SetText(L"检查更新");
            const nlohmann::json manifest = nlohmann::json::parse(body, nullptr, false);
            const std::string releases = kReleasesPageUrl;
            Version latest;
            std::string releaseUrl;
            bool valid = manifest.is_object() && manifest.contains("version") && manifest["version"].is_string() &&
                         manifest.contains("releaseUrl") && manifest["releaseUrl"].is_string();
            if (valid)
            {
                releaseUrl = manifest["releaseUrl"].get<std::string>();
                valid = (releaseUrl == releases || releaseUrl.rfind(releases + "/", 0) == 0) &&
                        ParseVersion(manifest["version"].get<std::string>(), latest);
            }
            if (!valid)
            {
                setStatus(L"检查失败，请稍后重试", TextRole::Danger);
                return;
            }
            if (CompareVersions(latest, current) <= 0)
            {
                setStatus(L"已是最新版本", TextRole::Accent);
                return;
            }
            // mirrorDownloadUrl
            std::string mirror;
            static const std::regex tag(R"(^https://github\.com/metasequoiaime/([\w.-]+)/releases/tag/([\w.+-]+)$)");
            static const std::regex installer(R"(^MetasequoiaIME_Setup_v[\w.-]+\.exe$)", std::regex::icase);
            std::smatch match;
            const std::string name = manifest.value("installerName", std::string());
            if (std::regex_match(releaseUrl, match, tag) && std::regex_match(name, installer))
                mirror = std::string(kDownloadMirrorPrefix) + "https://github.com/metasequoiaime/" + match[1].str() +
                         "/releases/download/" + match[2].str() + "/" + name;
            setStatus(L"发现新版本 v" + Widen(latest.display), TextRole::Accent);
            ShowUpdateDialog(*ctx, latest, releaseUrl, mirror, manifest);
        });
}

// 下载并校验安装包。只在正式版上真正下载，非正式版退回到 CheckForUpdate。
void DownloadUpdate(PageContext &context, const std::shared_ptr<AboutState> &state)
{
    Version current;
    if (!ParseVersion(kShownVersion, current))
    {
        state->status->SetText(L"无法识别当前版本");
        state->status->SetRole(TextRole::Danger);
        state->statusBox->SetVisible(true);
        return;
    }
    // 安装包下载只在正式版上开放。nightly 与 dev 构建点这个按钮仍然要能知道有没有
    // 新版本，但不去下载：它们的清单不由官网提供，下载下来也没有可以比较的版本。
    // 所以非正式版走 CheckForUpdate，结果落在同一块状态文字里。
    if (!update_download::IsReleaseVersion(MSIME_PRODUCT_VERSION))
    {
        CheckForUpdate(context, state);
        return;
    }
    state->check->SetDisabled(true);
    state->check->SetText(L"正在检查…");
    state->status->SetText(L"正在检查更新");
    state->status->SetRole(TextRole::Muted);
    state->statusBox->SetVisible(true);

    PageContext *ctx = &context;
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    const std::string url = std::string(kUpdateManifestUrl) + "?t=" + std::to_string(stamp);
    std::weak_ptr<AboutState> weak = state;
    RunInBackground(
        context.host.Hwnd(), [url]() { return HttpGet(url); },
        [ctx, state, weak, current](const std::string &body) {
            const nlohmann::json manifest = nlohmann::json::parse(body, nullptr, false);
            Version latest;
            std::string releaseUrl;
            bool valid = manifest.is_object() && manifest.contains("version") && manifest["version"].is_string() &&
                         manifest.contains("releaseUrl") && manifest["releaseUrl"].is_string();
            if (valid)
            {
                releaseUrl = manifest["releaseUrl"].get<std::string>();
                valid =
                    (releaseUrl == kReleasesPageUrl || releaseUrl.rfind(std::string(kReleasesPageUrl) + "/", 0) == 0) &&
                    ParseVersion(manifest["version"].get<std::string>(), latest);
            }
            if (!valid)
            {
                FinishUpdateDownload(state, false, L"检查更新失败，请稍后重试");
                return;
            }
            if (CompareVersions(latest, current) <= 0)
            {
                FinishUpdateDownload(state, true, L"已是最新版本");
                return;
            }

            const std::string name = manifest.value("installerName", std::string());
            const std::string digest = manifest.value("installerSha256", std::string());
            if (!update_download::IsInstallerName(name) || !update_download::IsSha256Hex(digest))
            {
                // 清单没给可用的包装与摘要，退回到原来那条路：让用户自己去下载页。
                state->check->SetDisabled(false);
                state->check->SetText(L"检查更新");
                state->status->SetText(L"发现新版本 v" + Widen(latest.display) + L"，请在弹窗里选择下载方式");
                state->status->SetRole(TextRole::Accent);
                state->statusBox->SetVisible(true);
                ShowUpdateDialog(*ctx, latest, releaseUrl, std::string(), manifest);
                return;
            }

            state->check->SetText(L"正在下载…");
            state->status->SetText(L"正在下载 v" + Widen(latest.display) + L"，稍等几分钟");
            state->status->SetRole(TextRole::Muted);

            update_download::Request request;
            request.current_version = MSIME_PRODUCT_VERSION;
            request.release_url = releaseUrl;
            request.installer_name = name;
            request.installer_sha256 = digest;
            request.directory = update_download::DefaultDirectory();
            // 下载本身在后台线程上跑，进度只能靠结束后一次性回报，所以把整个下载
            // 放进 RunInBackground 的 work 里；返回一个编码过的结果串交给窗口线程。
            RunInBackground(
                ctx->host.Hwnd(),
                [request]() {
                    const update_download::Result result = update_download::Download(request);
                    if (result.outcome == update_download::Outcome::Ok)
                    {
                        update_download::PruneOldInstallers(request.directory, request.installer_name);
                        // 四段结果：状态、路径、摘要、说明，用换行分隔。
                        return std::string("ok\n") + result.path.u8string() + "\n" + request.installer_sha256 + "\n";
                    }
                    return std::string("err\n\n\n") + result.message;
                },
                [weak](const std::string &encoded) {
                    const std::shared_ptr<AboutState> locked = weak.lock();
                    if (!locked)
                        return;
                    // 四段：状态、路径、摘要、说明。状态不是 ok 时按失败收尾。
                    const auto next = [&encoded](size_t &start) {
                        const size_t end = encoded.find('\n', start);
                        const std::string field =
                            encoded.substr(start, end == std::string::npos ? std::string::npos : end - start);
                        start = end == std::string::npos ? encoded.size() : end + 1;
                        return field;
                    };
                    size_t cursor = 0;
                    const std::string status = next(cursor);
                    const std::string path = next(cursor);
                    const std::string digest = next(cursor);
                    const std::string note = cursor < encoded.size() ? encoded.substr(cursor) : std::string();
                    if (status == "ok")
                        FinishUpdateDownload(locked, path, digest, note);
                    else
                        FinishUpdateDownload(locked, std::string(), std::string(),
                                             note.empty() ? std::string("下载失败") : note);
                });
        });
}
} // namespace

VisualPtr BuildAboutPage(PageContext &context)
{
    PageBuilder page(L"关于");
    page.SetTitleRole(TextRole::Secondary);
    SettingsModel *model = &context.model;
    const auto openUrl = [model](const std::string &url) { model->Post("openExternalUrl", Json(url)); };

    // .about-hero: mark, then eyebrow / title / text, gap 20px. The eyebrow is uppercased.
    auto heroRow = std::make_shared<Row>(20.0f);
    heroRow->Add(std::make_shared<AboutMark>());
    heroRow->Add(MakeDocumentHero(L"METASEQUOIA IME", L"水杉 IME", L"为现代 Windows 桌面体验打造的开放中文输入法。"),
                 1.0f);
    page.AddRaw(std::make_shared<PaddedSection>(heroRow, Thickness{28.0f, 26.0f, 28.0f, 26.0f}));

    // .about-links: no vertical padding, rows of 66px.
    auto state = std::make_shared<AboutState>();
    auto links = std::make_shared<Column>(0.0f);
    {
        auto left = std::make_shared<Column>(0.0f);
        left->Add(MakeLabel(L"当前版本", DocStyle(13.0f, DWRITE_FONT_WEIGHT_MEDIUM), TextRole::Primary));
        auto version = MakeLabel(Widen(kShownVersion), DocStyle(12.0f), TextRole::Muted);
        version->SetMargin({0.0f, 5.0f, 0.0f, 0.0f});
        left->Add(version);
        state->status = MakeLabel(L"", DocStyle(11.0f), TextRole::Muted);
        state->status->SetMinHeight(16.0f);
        state->statusBox = std::make_shared<Hideable>(state->status, false);
        state->statusBox->SetMargin({0.0f, 3.0f, 0.0f, 0.0f});
        left->Add(state->statusBox);
        // .about-update-button: padding 7px 13px, fully rounded, 12px; border-strong on
        // hover and pressed down 1px, like .default-ime-button.
        CssButton::Style style;
        style.paddingX = 13.0f;
        style.paddingY = 7.0f;
        style.radius = 99.0f;
        style.fontSize = 12.0f;
        style.transitionMs = 150.0;
        style.pressShift = true;
        style.disabledOpacity = 0.65f;
        state->check = std::make_shared<CssButton>(L"检查更新", style);
        PageContext *ctx = &context;
        std::weak_ptr<AboutState> weak = state;
        state->check->SetOnClick([ctx, weak]() {
            if (auto locked = weak.lock())
                DownloadUpdate(*ctx, locked);
        });
        links->Add(std::make_shared<AboutRow>(left, state->check, false));
    }
    // 自动下载是一个独立设置行，不塞进上面的按钮行：它改的是后台行为，和「现在检查一次」
    // 是两件事。只有正式版参与自动更新，dev 与 nightly 渲染出来只会是个勾了也没用的开关。
    if (update_download::IsReleaseVersion(MSIME_PRODUCT_VERSION))
    {
        auto left = std::make_shared<Column>(0.0f);
        left->Add(MakeLabel(L"自动下载更新", DocStyle(13.0f, DWRITE_FONT_WEIGHT_MEDIUM), TextRole::Primary));
        auto detail =
            MakeLabel(L"发现新版本时由输入法后台下载并校验，下载完成后询问是否安装", DocStyle(12.0f), TextRole::Muted);
        detail->SetMargin({0.0f, 5.0f, 0.0f, 0.0f});
        left->Add(detail);
        // 后台进度：Server 每收到一块数据就更新一次 progress.txt，这里定时读。
        // 页面不显示时定时器也会继续跑，但只读一个小文件，不值得为它做启停。
        auto progressLabel = MakeLabel(L"", DocStyle(11.0f), TextRole::Muted);
        auto progressBox = std::make_shared<Hideable>(progressLabel, false);
        progressBox->SetMargin({0.0f, 3.0f, 0.0f, 0.0f});
        left->Add(progressBox);
        // 只持有弱引用：timer 每跑一次就拿一次标签，页面没了就不再碰它，也不会拖住页面。
        std::weak_ptr<Label> weakLabel = progressLabel;
        std::weak_ptr<Hideable> weakBox = progressBox;
        const auto refresh = [weakLabel, weakBox]() {
            const std::shared_ptr<Label> label = weakLabel.lock();
            const std::shared_ptr<Hideable> box = weakBox.lock();
            if (!label || !box)
                return false;
            const update_channel::DownloadProgress progress = update_channel::ReadProgress();
            std::wstring text;
            TextRole role = TextRole::Muted;
            switch (progress.state)
            {
            case update_channel::DownloadProgress::State::Downloading: {
                text = L"正在下载 v" + Widen(progress.version);
                if (progress.total > 0)
                {
                    const auto percent = static_cast<unsigned>(progress.received * 100 / progress.total);
                    text += L"：" + std::to_wstring(percent) + L"%";
                }
                else if (progress.received > 0)
                {
                    text += L"：" + std::to_wstring(progress.received / (1024 * 1024)) + L" MB";
                }
                break;
            }
            case update_channel::DownloadProgress::State::Ready:
                text = L"v" + Widen(progress.version) + L" 已下载并通过校验，等待安装";
                role = TextRole::Accent;
                break;
            case update_channel::DownloadProgress::State::Failed:
                text = L"上次自动下载失败：" + Widen(progress.reason);
                role = TextRole::Danger;
                break;
            case update_channel::DownloadProgress::State::Idle:
            default:
                break;
            }
            label->SetText(text);
            label->SetRole(role);
            box->SetVisible(!text.empty());
            return true;
        };
        refresh();
        // 每秒读一次进度。tick 里只拿弱引用，页面销毁后下一次触发就自我重置，顺带 KillTimer。
        auto timer = std::make_shared<std::unique_ptr<UiTimer>>();
        std::weak_ptr<std::unique_ptr<UiTimer>> weakTimer = timer;
        *timer = std::make_unique<UiTimer>(context.host.Hwnd(), 1000, [refresh, weakTimer]() {
            if (refresh())
                return;
            if (const auto held = weakTimer.lock())
                held->reset();
        });
        links->Add(std::make_shared<AboutRow>(left, BindToggle(context, "appearance.auto_download_update"), false));
    }
    struct Link
    {
        const wchar_t *title;
        const wchar_t *detail;
        const char *url;
    };
    const Link documents[] = {
        // The space before the detail <span> belongs to the title text.
        {L"官网 ", L"msime.app", kWebsiteUrl},
        {L"开源许可协议", nullptr, kLicenseUrl},
        {L"隐私政策", nullptr, kPrivacyUrl},
    };
    for (size_t i = 0; i < std::size(documents); ++i)
    {
        auto title = MakeLabel(documents[i].title, DocStyle(13.0f, DWRITE_FONT_WEIGHT_MEDIUM), TextRole::Primary);
        auto titleRow = std::make_shared<Row>(8.0f);
        titleRow->SetAlign(Row::Align::End);
        titleRow->Add(title);
        if (documents[i].detail)
            titleRow->Add(MakeLabel(documents[i].detail, DocStyle(12.0f), TextRole::Muted));
        auto arrow = MakeLabel(L"↗", DocStyle(16.0f), TextRole::Muted);
        auto row = std::make_shared<AboutRow>(titleRow, arrow, i + 1 == std::size(documents));
        const std::string url = documents[i].url;
        row->SetOnClick([openUrl, url]() { openUrl(url); }, title);
        links->Add(row);
    }
    auto linksSection = std::make_shared<PaddedSection>(links, Thickness{24.0f, 0.0f, 24.0f, 0.0f});
    page.AddRaw(linksSection);

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
    // The button's handler only holds the state weakly (the state holds the button), so
    // the page has to own it or the first click finds it gone.
    return std::make_shared<OwnerBox>(page.Build(), state);
}
} // namespace native_settings
