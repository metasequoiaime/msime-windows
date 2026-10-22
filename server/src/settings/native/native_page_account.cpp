#include "settings/native/native_settings_pages.h"

#include "settings/native/native_page_help_widgets.h"

#include "settings/account/msime_account_client.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Window.h"

#include <nlohmann/json.hpp>

#include <unordered_map>

// The account page talks to the shared Rust implementation through the C ABI
// (settings/account/msime_account_client.*); this file is presentation only.
// Every blocking call runs through RunInBackground, whose completion callback
// runs back on the UI thread (see native_page_help_widgets.cpp).
namespace native_settings
{
namespace
{
using Json = nlohmann::json;

// The status operations are the only ones the page issues without user input;
// they are local file reads, but the envelope is identical, so one helper covers
// everything.
void PostAccountCall(PageContext &context, const std::string &action_json,
                     std::function<void(const msime_account::CallResult &)> done)
{
    RunInBackground(
        context.host.Hwnd(),
        [action_json]() {
            const msime_account::CallResult result = msime_account::CallAccount(action_json);
            // The background channel carries strings; serialize the result.
            nlohmann::json wire = {
                {"ok", result.ok},
                {"error", result.error},
                {"retry", result.retry_after_seconds},
                {"value", result.value_json.empty() ? nullptr : Json::parse(result.value_json, nullptr, false)}};
            return wire.dump();
        },
        [done](const std::string &wire_text) {
            const Json wire = Json::parse(wire_text, nullptr, false);
            msime_account::CallResult result;
            if (!wire.is_object())
            {
                result.error = "account_unavailable";
            }
            else
            {
                result.ok = wire.value("ok", false);
                result.error = wire.value("error", std::string());
                result.retry_after_seconds = wire.value("retry", 0L);
                if (wire.contains("value") && !wire["value"].is_null() && wire["value"].is_string())
                {
                    result.value_json = wire["value"].get<std::string>();
                }
            }
            done(result);
        });
}

// Human-readable text for the shared error codes; unknown codes pass through.
std::wstring ErrorText(const msime_account::CallResult &result)
{
    if (result.retry_after_seconds > 0)
    {
        const long minutes = (result.retry_after_seconds + 59) / 60;
        return L"请求过于频繁，请约 " + std::to_wstring(minutes) + L" 分钟后再试";
    }
    static const std::unordered_map<std::string, std::wstring> texts = {
        {"account_rate_limited", L"请求过于频繁，请稍后再试"},
        {"account_unauthorized", L"登录状态已失效，请重新登录"},
        {"account_network", L"网络不可用，请检查连接"},
        {"account_unavailable", L"账号服务暂不可用"},
        {"account_invalid", L"输入无效"},
        {"account_blocked_content", L"昵称包含被屏蔽的内容"},
        {"account_banned", L"账号已被封禁"},
        {"account_too_large", L"文件太大"},
        {"account_conflict", L"操作冲突，请重试"},
    };
    const auto it = texts.find(result.error);
    return it != texts.end() ? it->second : Widen(result.error);
}

struct UserView
{
    bool logged_in = false;
    std::wstring id;
    std::wstring display_name;
    std::wstring email;
};

UserView ParseUser(const Json &user)
{
    UserView view;
    view.logged_in = user.is_object() && !user.value("id", std::string()).empty();
    if (view.logged_in)
    {
        view.id = Widen(user.value("id", std::string()));
        view.display_name = Widen(user.value("displayName", std::string()));
        view.email = Widen(user.value("email", std::string()));
    }
    return view;
}

struct PageState
{
    std::shared_ptr<Label> headline;
    std::shared_ptr<Label> detail;
    std::shared_ptr<CssButton> login;
    std::shared_ptr<CssButton> logout;
    std::shared_ptr<Hideable> loginArea;
    std::shared_ptr<Hideable> loggedInArea;
    // The verify-code flow widgets.
    std::shared_ptr<TextField> contact;
    std::shared_ptr<TextField> code;
    std::shared_ptr<CssButton> sendCode;
    std::shared_ptr<CssButton> confirmCode;
    std::shared_ptr<Hideable> codeArea;
    // Set while a challenge is outstanding; the confirm button keys off it.
    std::string challenge_id;
};

std::wstring HeadlineFor(const UserView &user)
{
    if (!user.logged_in)
        return L"未登录";
    if (!user.display_name.empty())
        return user.display_name;
    return Widen("账号 ") + user.id.substr(0, 8);
}

std::wstring DetailFor(const UserView &user)
{
    if (!user.logged_in)
        return L"登录水杉账号后，设置、常用语与个人词库会在你的设备之间同步。";
    if (!user.email.empty())
        return user.email;
    return L"ID: " + user.id;
}

void RefreshStatus(PageContext &context, const std::shared_ptr<PageState> &state)
{
    PostAccountCall(context, R"({"operation":"status"})", [state](const msime_account::CallResult &result) {
        UserView user;
        if (result.ok && !result.value_json.empty())
        {
            const Json value = Json::parse(result.value_json, nullptr, false);
            if (value.is_object() && value.contains("user"))
                user = ParseUser(value["user"]);
        }
        state->headline->SetText(HeadlineFor(user));
        state->detail->SetText(DetailFor(user));
        state->loginArea->SetVisible(!user.logged_in);
        state->loggedInArea->SetVisible(user.logged_in);
    });
}

} // namespace

VisualPtr BuildAccountPage(PageContext &context)
{
    PageBuilder page(L"账号");
    auto state = std::make_shared<PageState>();

    state->headline = std::make_shared<Label>(L"未登录", TextStyle{}, TextRole::Primary);
    state->detail = MakeDescription(L"登录水杉账号后，设置、常用语与个人词库会在你的设备之间同步。");
    state->detail->SetMaxWidth(52.0f * kFontCaption);

    auto statusColumn = std::make_shared<Column>(0.0f);
    statusColumn->Add(state->headline);
    statusColumn->Add(state->detail);
    page.AddSection(statusColumn);

    // Login widgets.
    TextField::Options contactOptions;
    contactOptions.placeholder = L"name@example.com";
    state->contact = std::make_shared<TextField>(contactOptions);
    TextField::Options codeOptions;
    codeOptions.placeholder = L"000000";
    state->code = std::make_shared<TextField>(codeOptions);

    state->sendCode = std::make_shared<CssButton>(L"发送验证码", CssButton::Style{});
    state->confirmCode = std::make_shared<CssButton>(L"验证并登录", CssButton::Style{});
    state->login = std::make_shared<CssButton>(L"发送验证码登录", CssButton::Style{});
    state->logout = std::make_shared<CssButton>(L"退出登录", CssButton::Style{});

    auto codeColumn = std::make_shared<Column>(0.0f);
    codeColumn->Add(MakeRow(L"验证码", L"输入收到的六位验证码", state->code, true));
    codeColumn->Add(MakeRow(L"确认", L"验证码五分钟内有效", state->confirmCode, true));
    codeColumn->SetMargin({0.0f, 8.0f, 0.0f, 0.0f});
    state->codeArea = std::make_shared<Hideable>(codeColumn, false);

    auto loginColumn = std::make_shared<Column>(0.0f);
    loginColumn->Add(MakeRow(L"邮箱或手机号", L"接收登录验证码", state->contact, true));
    loginColumn->Add(MakeRow(L"获取验证码", L"点击后输入收到的验证码", state->sendCode, true));
    loginColumn->Add(state->codeArea);
    loginColumn->Add(MakeRow(L"登录", L"验证码登录；Google 与 Apple 登录请使用水杉共享应用", state->login, true));
    state->loginArea = std::make_shared<Hideable>(loginColumn, true);

    auto loggedInColumn = std::make_shared<Column>(0.0f);
    loggedInColumn->Add(MakeRow(L"退出登录", L"退出当前设备；其他设备保持登录", state->logout, true));
    state->loggedInArea = std::make_shared<Hideable>(loggedInColumn, false);

    auto cardColumn = std::make_shared<Column>(0.0f);
    cardColumn->Add(state->loginArea);
    cardColumn->Add(state->loggedInArea);
    page.AddSection(cardColumn);

    // Send a verification code to the address in the contact box.
    state->sendCode->SetOnClick([ctx = &context, state]() {
        const std::string target = Narrow(state->contact->GetText());
        if (target.empty())
        {
            ctx->host.ShowToast(L"请先填写邮箱或手机号", false);
            return;
        }
        // Email addresses vs. phone numbers: the backend decides by the value
        // shape; the provider field follows the leading + sign (phone).
        const char *provider = target.rfind("+", 0) == 0 ? "phone" : "email";
        Json action = {{"operation", "request_code"}, {"provider", provider}, {"target", target}};
        state->sendCode->SetDisabled(true);
        state->sendCode->SetText(L"发送中…");
        PostAccountCall(*ctx, action.dump(), [ctx, state](const msime_account::CallResult &result) {
            state->sendCode->SetDisabled(false);
            state->sendCode->SetText(L"发送验证码");
            if (!result.ok)
            {
                ctx->host.ShowToast(ErrorText(result), false);
                return;
            }
            const Json value = Json::parse(result.value_json, nullptr, false);
            state->challenge_id = value.value("challengeId", std::string());
            state->codeArea->SetVisible(true);
            ctx->host.ShowToast(L"验证码已发送，五分钟内有效");
        });
    });

    state->confirmCode->SetOnClick([ctx = &context, state]() {
        if (state->challenge_id.empty())
        {
            ctx->host.ShowToast(L"请先获取验证码", false);
            return;
        }
        const std::string code = Narrow(state->code->GetText());
        if (code.size() != 6)
        {
            ctx->host.ShowToast(L"验证码是六位数字", false);
            return;
        }
        Json action = {{"operation", "login"}, {"challenge_id", state->challenge_id}, {"code", code}};
        state->confirmCode->SetDisabled(true);
        PostAccountCall(*ctx, action.dump(), [ctx, state](const msime_account::CallResult &result) {
            state->confirmCode->SetDisabled(false);
            if (!result.ok)
            {
                ctx->host.ShowToast(ErrorText(result), false);
                return;
            }
            state->challenge_id.clear();
            state->code->SetText(L"");
            state->codeArea->SetVisible(false);
            ctx->host.ShowToast(L"登录成功");
            RefreshStatus(*ctx, state);
        });
    });

    state->login->SetOnClick([state]() {
        // One entry point: fill the code area and reuse the code flow.
        state->codeArea->SetVisible(true);
    });

    state->logout->SetOnClick([ctx = &context, state]() {
        ctx->host.Confirm(L"退出当前设备的登录？", [ctx, state](bool confirmed) {
            if (!confirmed)
                return;
            PostAccountCall(*ctx, R"({"operation":"logout","all":false})",
                            [ctx, state](const msime_account::CallResult &result) {
                                if (!result.ok)
                                {
                                    ctx->host.ShowToast(ErrorText(result), false);
                                    return;
                                }
                                ctx->host.ShowToast(L"已退出登录");
                                RefreshStatus(*ctx, state);
                            });
        });
    });

    RefreshStatus(context, state);
    return page.Build();
}
} // namespace native_settings
