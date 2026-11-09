#include "settings/native/native_page_help_widgets.h"
#include "settings/native/native_page_voice_fields.h"
#include "settings/native/native_settings_pages.h"

#include <atomic>
#include <ctime>

namespace native_settings
{
using namespace voice_fields;

// 用户数据同步页。WebView2 时期没有这个功能，这一页是全新的。
//
// 布局沿用「帮助」页的网络代理块：一张卡片放同步方式与字段，另一张放测试连接、上传、
// 恢复与设备列表。所有字段都写进 config.toml 的 [sync] 段；密码与密钥由配置层统一做
// DPAPI 加密，页面本身不碰加密。
namespace
{
// 一行说明，与页面其余部分的 .section-description 一致。
std::shared_ptr<Label> SyncDescription(std::wstring text)
{
    auto label = MakeDescription(std::move(text));
    label->SetMargin({0.0f, 0.0f, 0.0f, 0.0f});
    return label;
}

std::shared_ptr<Column> FieldWithCaption(const std::wstring &caption, const std::shared_ptr<InputBox> &input)
{
    auto column = std::make_shared<Column>(7.0f);
    column->Add(MakeLabel(caption, DocStyle(12.0f, DWRITE_FONT_WEIGHT_MEDIUM), TextRole::Secondary));
    column->Add(input);
    column->SetMaxWidth(360.0f);
    return column;
}

std::shared_ptr<InputBox> MakeField(const wchar_t *placeholder, bool secret = false)
{
    InputBox::Options options;
    options.height = 34.0f;
    options.placeholder = placeholder;
    options.password = secret;
    options.reveal = secret;
    return std::make_shared<InputBox>(options);
}

std::wstring TrimText(std::wstring text)
{
    text.erase(0, text.find_first_not_of(L" \t\r\n"));
    const size_t end = text.find_last_not_of(L" \t\r\n");
    text.erase(end == std::wstring::npos ? 0 : end + 1);
    return text;
}

std::wstring FormatBytes(std::uint64_t bytes)
{
    if (bytes >= 1024ull * 1024 * 1024)
        return std::to_wstring(bytes / (1024ull * 1024 * 1024)) + L" GB";
    if (bytes >= 1024ull * 1024)
        return std::to_wstring(bytes / (1024ull * 1024)) + L" MB";
    if (bytes >= 1024ull)
        return std::to_wstring(bytes / 1024ull) + L" KB";
    return std::to_wstring(bytes) + L" B";
}

// 秒级时间戳转本地日期时间，只显示到分钟。
std::wstring FormatTime(std::int64_t unix_seconds)
{
    if (unix_seconds <= 0)
        return L"未知时间";
    const std::time_t stamp = static_cast<std::time_t>(unix_seconds);
    std::tm local{};
    localtime_s(&local, &stamp);
    wchar_t buffer[32]{};
    std::wcsftime(buffer, std::size(buffer), L"%Y-%m-%d %H:%M", &local);
    return buffer;
}

std::shared_ptr<CssButton> MakeSyncButton(const wchar_t *text, CssButton::Look look)
{
    CssButton::Style style;
    style.look = look;
    style.minWidth = 92.0f;
    style.paddingX = 14.0f;
    style.paddingY = 7.0f;
    style.fontSize = 12.0f;
    style.transitionMs = 150.0;
    style.pressShift = true;
    return std::make_shared<CssButton>(text, style);
}

// 设备列表。不销毁重建：msimeui::Window 的 focusedVisual_ / hoveredVisual_ /
// capturedVisual_ 是裸指针，销毁行里的按钮会留下悬垂指针。改成复用一组固定的行，
// 只更新文本与按钮回调，多余的行隐藏。
class DeviceList : public Column
{
  public:
    struct Row
    {
        std::shared_ptr<Hideable> holder;
        std::shared_ptr<Label> title;
        std::shared_ptr<Label> detail;
        std::shared_ptr<CssButton> restore;
    };

    DeviceList() : Column(0.0f)
    {
    }

    // 空列表时的说明行。它只创建一次，之后靠可见性切换，同样不销毁。
    void EnsureEmptyHint(bool visible)
    {
        if (!empty_hint_)
        {
            empty_hint_ = std::make_shared<Hideable>(
                SyncDescription(L"云端还没有其他设备的备份。上传一次后，换台电脑就能在这里看到。"), false);
            Add(empty_hint_);
        }
        empty_hint_->SetVisible(visible);
    }

    // 保证至少有 count 行；不够就补，多了就隐藏。
    void EnsureRows(std::size_t count)
    {
        while (rows_.size() < count && rows_.size() < kMaxRows)
        {
            Row row;
            row.title = MakeLabel(L"", DocStyle(13.0f, DWRITE_FONT_WEIGHT_MEDIUM), TextRole::Primary);
            row.detail = MakeLabel(L"", DocStyle(12.0f), TextRole::Muted);
            row.detail->SetMargin({0.0f, 4.0f, 0.0f, 0.0f});
            auto left = std::make_shared<Column>(0.0f);
            left->Add(row.title);
            left->Add(row.detail);
            row.restore = MakeSyncButton(L"恢复…", CssButton::Look::Secondary);
            // Hideable 是仓库里唯一控制可见性的容器；SettingRow 本身没有可见性开关。
            row.holder = std::make_shared<Hideable>(std::make_shared<SettingRow>(left, row.restore), false);
            Add(row.holder);
            rows_.push_back(std::move(row));
        }
        for (std::size_t index = 0; index < rows_.size(); ++index)
            rows_[index].holder->SetVisible(index < count);
        InvalidateMeasure();
    }

    Row &At(std::size_t index)
    {
        return rows_[index];
    }

  private:
    // 设备多到这个数已经不正常，超出的不显示也比让列表无限增长安全。
    static constexpr std::size_t kMaxRows = 32;
    std::vector<Row> rows_;
    std::shared_ptr<Hideable> empty_hint_;
};

struct SyncState
{
    RadioGroup backend;
    std::string backendValue = "off";
    std::shared_ptr<InputBox> webdavUrl;
    std::shared_ptr<InputBox> webdavUser;
    std::shared_ptr<InputBox> webdavPassword;
    std::shared_ptr<InputBox> s3Endpoint;
    std::shared_ptr<InputBox> s3Region;
    std::shared_ptr<InputBox> s3Bucket;
    std::shared_ptr<InputBox> s3AccessKey;
    std::shared_ptr<InputBox> s3SecretKey;
    std::shared_ptr<InputBox> s3Prefix;
    std::shared_ptr<Hideable> webdavFields;
    std::shared_ptr<Hideable> s3Fields;
    std::shared_ptr<Label> status;
    std::shared_ptr<CssButton> probe;
    std::shared_ptr<CssButton> upload;
    std::shared_ptr<CssButton> refresh;
    std::shared_ptr<DeviceList> devices;
    bool busy = false;
    std::string localDeviceId;
};

// 按当前后端显示对应的字段组，另一组整块隐藏。
void RefreshBackendVisibility(SyncState &state)
{
    const bool webdav = state.backendValue == "webdav";
    const bool s3 = state.backendValue == "s3";
    if (state.webdavFields)
        state.webdavFields->SetVisible(webdav);
    if (state.s3Fields)
        state.s3Fields->SetVisible(s3);
}

void SetBusy(SyncState &state, bool busy)
{
    state.busy = busy;
    if (state.probe)
        state.probe->SetDisabled(busy);
    if (state.upload)
        state.upload->SetDisabled(busy);
    if (state.refresh)
        state.refresh->SetDisabled(busy);
    if (busy && state.status)
    {
        state.status->SetText(L"正在处理，请稍候…");
        state.status->SetRole(TextRole::Muted);
    }
}

// 生成一个只在本次进程内有意义的请求标识，用来把回执和发出去的请求对上。
std::string NextRequestId(const char *prefix)
{
    static std::atomic<unsigned> counter{0};
    return std::string(prefix) + "-" + std::to_string(++counter);
}

void PostSync(SettingsModel *model, const std::string &requestId, const std::string &action,
              const std::string &deviceId)
{
    Json data = {{"requestId", requestId}, {"action", action}};
    if (!deviceId.empty())
        data["deviceId"] = deviceId;
    model->Post("userDataSyncRequest", data);
}

void RebuildDeviceList(const std::shared_ptr<SyncState> &state, SettingsModel *model, const Json &devices)
{
    if (!state || !state->devices)
        return;
    std::vector<const Json *> entries;
    if (devices.is_array())
    {
        for (const Json &entry : devices)
        {
            if (entry.is_object())
                entries.push_back(&entry);
        }
    }
    state->devices->EnsureEmptyHint(entries.empty());
    state->devices->EnsureRows(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        const Json &entry = *entries[index];
        DeviceList::Row &row = state->devices->At(index);
        const std::string deviceId = entry.value("deviceId", std::string());
        const std::wstring hostname = Widen(entry.value("hostname", std::string()));
        const std::int64_t uploaded = entry.value("uploadedUnix", static_cast<std::int64_t>(0));
        const std::uint64_t size = static_cast<std::uint64_t>(entry.value("size", static_cast<std::int64_t>(0)));
        std::wstring detail = FormatTime(uploaded) + L" · " + FormatBytes(size);
        const bool local = !state->localDeviceId.empty() && deviceId == state->localDeviceId;
        if (local)
            detail = L"本机 · " + detail;
        row.title->SetText(hostname.empty() ? L"未命名设备" : hostname);
        row.detail->SetText(detail);
        // 本机自己那份没有恢复的意义，直接禁用，避免用户误点后覆盖正在使用的数据。
        row.restore->SetDisabled(local);
        std::weak_ptr<SyncState> weak = state;
        row.restore->SetOnClick([model, weak, deviceId, local]() {
            auto locked = weak.lock();
            if (!locked || locked->busy || local || deviceId.empty())
                return;
            const int answer =
                MessageBoxW(GetActiveWindow(),
                            L"恢复会先停止输入法后台进程，并把将被覆盖的配置、统计、皮肤和模型备份到数据目录的 "
                            L".backup-before-import。词库按日志合并，不会替换现有数据库。\n\n继续恢复吗？",
                            L"从云端恢复", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer != IDOK)
                return;
            SetBusy(*locked, true);
            PostSync(model, NextRequestId("download"), "download", deviceId);
        });
    }
}
} // namespace

VisualPtr BuildSyncPage(PageContext &context)
{
    PageBuilder page(L"数据同步");
    page.SetTitleRole(TextRole::Secondary);
    SettingsModel *model = &context.model;
    auto state = std::make_shared<SyncState>();

    // 同步方式与目标
    {
        auto column = std::make_shared<Column>(0.0f);
        column->Add(MakeTitle(L"同步目标"));
        column->Add(
            SyncDescription(L"把配置、用户词条和输入统计备份到自己的网盘或对象存储。凭据不参与同步，也不会写进备份。"));
        auto options = std::make_shared<Column>(0.0f);
        options->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
        const std::pair<const char *, const wchar_t *> modes[] = {{"off", L"不启用"},
                                                                  {"webdav", L"WebDAV（坚果云、Nextcloud 等）"},
                                                                  {"s3", L"S3 兼容（MinIO、R2、B2、OSS 等）"}};
        for (size_t i = 0; i < std::size(modes); ++i)
        {
            if (i > 0)
                options->Add(std::make_shared<Divider>(Divider::Kind::Option));
            auto radio = state->backend.Add(modes[i].second, modes[i].first);
            radio->SetInnerPadding({4.0f, 8.0f, 4.0f, 8.0f});
            radio->SetHorizontalAlignment(msimeui::HorizontalAlignment::Leading);
            options->Add(radio);
        }
        state->backend.SetValue("off");
        column->Add(options);
        column->Add(std::make_shared<Divider>());

        // WebDAV 字段组
        auto webdav = std::make_shared<Column>(10.0f);
        state->webdavUrl = MakeField(L"https://dav.example.com/msime");
        state->webdavUser = MakeField(L"账号");
        state->webdavPassword = MakeField(L"密码", true);
        webdav->Add(FieldWithCaption(L"备份目录地址", state->webdavUrl));
        webdav->Add(FieldWithCaption(L"用户名", state->webdavUser));
        webdav->Add(FieldWithCaption(L"密码", state->webdavPassword));
        webdav->Add(
            SyncDescription(L"目录要事先在网盘里建好。坚果云填 https://dav.jianguoyun.com/dav/水杉备份，Nextcloud 填"
                            L" https://你的域名/remote.php/dav/files/用户名/msime。"));
        state->webdavFields = std::make_shared<Hideable>(webdav, false);
        column->Add(state->webdavFields);

        // S3 字段组
        auto s3 = std::make_shared<Column>(10.0f);
        state->s3Endpoint = MakeField(L"https://s3.us-east-1.amazonaws.com");
        state->s3Region = MakeField(L"us-east-1");
        state->s3Bucket = MakeField(L"存储桶名称");
        state->s3AccessKey = MakeField(L"Access Key");
        state->s3SecretKey = MakeField(L"Secret Key", true);
        state->s3Prefix = MakeField(L"msime");
        s3->Add(FieldWithCaption(L"服务地址", state->s3Endpoint));
        s3->Add(FieldWithCaption(L"区域", state->s3Region));
        s3->Add(FieldWithCaption(L"存储桶", state->s3Bucket));
        s3->Add(FieldWithCaption(L"Access Key", state->s3AccessKey));
        s3->Add(FieldWithCaption(L"Secret Key", state->s3SecretKey));
        s3->Add(FieldWithCaption(L"对象前缀", state->s3Prefix));
        s3->Add(MakeRow(L"路径风格", L"MinIO 与多数自建网关需要打开；AWS S3 通常关闭，改用虚拟主机风格",
                        BindToggle(context, "sync.s3_path_style")));
        s3->Add(SyncDescription(
            L"Cloudflare R2 的区域填 auto，Backblaze B2 填它给出的 region，阿里云 OSS 的地址形如"
            L" https://bucket.oss-cn-hangzhou.aliyuncs.com。自建 MinIO 用路径风格，把存储桶填在单独的字段里。"));
        state->s3Fields = std::make_shared<Hideable>(s3, false);
        column->Add(state->s3Fields);
        page.AddSection(column);

        std::weak_ptr<SyncState> weak = state;
        state->backend.SetOnChanged([model, weak](const std::string &value) {
            auto locked = weak.lock();
            if (!locked)
                return;
            locked->backendValue = value;
            model->Update("sync.backend", value);
            RefreshBackendVisibility(*locked);
        });

        // 字段写回：提交时更新配置，不提交就不写。
        const auto bindField = [model](const std::shared_ptr<InputBox> &input, const char *path) {
            input->SetOnCommit(
                [model, path](const std::wstring &text) { model->Update(path, Narrow(TrimText(text))); });
        };
        bindField(state->webdavUrl, "sync.webdav_url");
        bindField(state->webdavUser, "sync.webdav_username");
        bindField(state->webdavPassword, "sync.webdav_password");
        bindField(state->s3Endpoint, "sync.s3_endpoint");
        bindField(state->s3Region, "sync.s3_region");
        bindField(state->s3Bucket, "sync.s3_bucket");
        bindField(state->s3AccessKey, "sync.s3_access_key");
        bindField(state->s3SecretKey, "sync.s3_secret_key");
        bindField(state->s3Prefix, "sync.s3_prefix");
    }

    // 开关与自动上传
    {
        auto rows = std::make_shared<Column>(0.0f);
        rows->Add(MakeRow(L"启用同步", L"关闭后不再上传，也不影响已经上传的快照", BindToggle(context, "sync.enabled")));
        rows->Add(std::make_shared<Divider>());
        Dropdown::Options frequency;
        frequency.minWidth = 130.0f;
        rows->Add(MakeRow(L"自动上传间隔", L"按固定间隔把当前数据打包上传一次；仅手动表示只在下面点「立即上传」",
                          BindDropdown(context, "sync.auto_upload_minutes",
                                       Options({{"0", L"仅手动"},
                                                {"30", L"每 30 分钟"},
                                                {"120", L"每 2 小时"},
                                                {"360", L"每 6 小时"},
                                                {"1440", L"每天一次"}}),
                                       frequency, ValueKind::Integer)));
        rows->Add(std::make_shared<Divider>());
        Dropdown::Options keep;
        keep.minWidth = 110.0f;
        rows->Add(
            MakeRow(L"保留快照份数", L"每台设备在云端保留最近几份，旧的自动删除",
                    BindDropdown(context, "sync.keep_snapshots", NumberOptions(1, 20), keep, ValueKind::Integer)));
        rows->Add(std::make_shared<Divider>());
        rows->Add(MakeRow(L"包含已下载模型",
                          L"勾选时把数据目录 models/ 下的模型一起打包。模型动辄几百 MB，通常不必同步。",
                          BindToggle(context, "sync.include_models")));
        page.AddSection(rows);
    }

    // 操作与设备列表
    {
        auto column = std::make_shared<Column>(0.0f);
        auto actions = std::make_shared<Row>(10.0f);
        state->probe = MakeSyncButton(L"测试连接", CssButton::Look::Secondary);
        state->upload = MakeSyncButton(L"立即上传", CssButton::Look::Accent);
        state->refresh = MakeSyncButton(L"刷新设备", CssButton::Look::Secondary);
        actions->Add(state->probe);
        actions->Add(state->upload);
        actions->Add(state->refresh);
        column->Add(actions);

        state->status = MakeLabel(L"", DocStyle(12.0f), TextRole::Muted);
        state->status->SetMargin({0.0f, 10.0f, 0.0f, 0.0f});
        column->Add(state->status);

        auto heading = MakeTitle(L"云端设备");
        heading->SetMargin({0.0f, 20.0f, 0.0f, 0.0f});
        column->Add(heading);
        state->devices = std::make_shared<DeviceList>();
        state->devices->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
        column->Add(state->devices);
        page.AddSection(column);

        std::weak_ptr<SyncState> weak = state;
        state->probe->SetOnClick([model, weak]() {
            auto locked = weak.lock();
            if (!locked || locked->busy)
                return;
            SetBusy(*locked, true);
            PostSync(model, NextRequestId("probe"), "probe", {});
        });
        state->upload->SetOnClick([model, weak]() {
            auto locked = weak.lock();
            if (!locked || locked->busy)
                return;
            SetBusy(*locked, true);
            PostSync(model, NextRequestId("upload"), "upload", {});
        });
        state->refresh->SetOnClick([model, weak]() {
            auto locked = weak.lock();
            if (!locked || locked->busy)
                return;
            SetBusy(*locked, true);
            PostSync(model, NextRequestId("list"), "listDevices", {});
        });

        // 回执：成功给绿色提示，失败给红色；列表类回执顺带重建设备列表。
        context.OnMessage("userDataSyncResponse", [weak, model](const Json &message) {
            auto locked = weak.lock();
            if (!locked)
                return;
            SetBusy(*locked, false);
            const bool ok = message.contains("ok") && message["ok"].is_boolean() && message["ok"].get<bool>();
            const std::string text = message.contains("message") && message["message"].is_string()
                                         ? message["message"].get<std::string>()
                                         : std::string(ok ? "操作完成" : "操作失败");
            if (locked->status)
            {
                locked->status->SetText(Widen(text));
                locked->status->SetRole(ok ? TextRole::Accent : TextRole::Danger);
            }
            if (message.contains("localDeviceId") && message["localDeviceId"].is_string())
                locked->localDeviceId = message["localDeviceId"].get<std::string>();
            if (message.contains("devices") && message["devices"].is_array())
                RebuildDeviceList(locked, model, message["devices"]);
        });

        // 配置快照回来时同步字段：页面可能在别处被改过，或者刚打开。
        // 正在输入的字段不覆盖：快照可能在用户敲密码时到达，SetText 会把他打的字换掉。
        // 同目录的帮助页对代理地址用的是同一种做法。
        context.OnConfig([weak, model]() {
            auto locked = weak.lock();
            if (!locked)
                return;
            locked->backendValue = model->GetString("sync.backend");
            if (locked->backendValue.empty())
                locked->backendValue = "off";
            locked->backend.SetValue(locked->backendValue);
            RefreshBackendVisibility(*locked);
            const auto sync_field = [model](const std::shared_ptr<InputBox> &input, const char *path) {
                if (input->IsFocused())
                    return;
                input->SetText(Widen(model->GetString(path)));
            };
            sync_field(locked->webdavUrl, "sync.webdav_url");
            sync_field(locked->webdavUser, "sync.webdav_username");
            sync_field(locked->webdavPassword, "sync.webdav_password");
            sync_field(locked->s3Endpoint, "sync.s3_endpoint");
            sync_field(locked->s3Region, "sync.s3_region");
            sync_field(locked->s3Bucket, "sync.s3_bucket");
            sync_field(locked->s3AccessKey, "sync.s3_access_key");
            sync_field(locked->s3SecretKey, "sync.s3_secret_key");
            sync_field(locked->s3Prefix, "sync.s3_prefix");
        });
    }

    return page.Build();
}
} // namespace native_settings
