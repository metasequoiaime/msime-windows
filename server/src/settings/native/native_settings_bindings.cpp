#include "settings/native/native_settings_pages.h"

#include "utils/common_utils.h"

#include <algorithm>
#include <cmath>

namespace native_settings
{
namespace
{
// The snapshot value as the string a dropdown option carries.
std::string ValueAsString(const Json *value)
{
    if (!value)
        return {};
    if (value->is_string())
        return value->get<std::string>();
    if (value->is_boolean())
        return value->get<bool>() ? "true" : "false";
    if (value->is_number_integer())
        return std::to_string(value->get<int64_t>());
    if (value->is_number())
    {
        const double number = value->get<double>();
        if (std::floor(number) == number)
            return std::to_string(static_cast<int64_t>(number));
        std::string text = std::to_string(number);
        text.erase(text.find_last_not_of('0') + 1);
        if (!text.empty() && text.back() == '.')
            text.pop_back();
        return text;
    }
    return {};
}

Json ParseValue(const std::string &value, ValueKind kind)
{
    switch (kind)
    {
    case ValueKind::Integer:
        try
        {
            return Json(std::stoll(value));
        }
        catch (...)
        {
            return Json(0);
        }
    case ValueKind::Number:
        try
        {
            return Json(std::stod(value));
        }
        catch (...)
        {
            return Json(0.0);
        }
    case ValueKind::String:
    default:
        return Json(value);
    }
}
} // namespace

PageContext::PageContext(SettingsModel &model, HostServices &host) : model(model), host(host)
{
}

void PageContext::OnConfig(std::function<void()> sync)
{
    if (model.HasConfig())
        sync();
    model.AddConfigListener(std::move(sync));
}

void PageContext::OnMessage(const std::string &type, std::function<void(const Json &)> listener)
{
    model.AddMessageListener(type, std::move(listener));
}

std::shared_ptr<ToggleSwitch> BindToggle(PageContext &context, std::string path,
                                         std::function<void(bool on)> afterChange)
{
    auto toggle = std::make_shared<ToggleSwitch>(context.model.GetBool(path));
    SettingsModel *model = &context.model;
    toggle->SetOnChanged([model, path, afterChange](bool on) {
        model->Update(path, on);
        if (afterChange)
            afterChange(on);
    });
    std::weak_ptr<ToggleSwitch> weak = toggle;
    context.OnConfig([weak, model, path]() {
        if (auto control = weak.lock())
            control->SetChecked(model->GetBool(path));
    });
    return toggle;
}

std::shared_ptr<Dropdown> BindDropdown(PageContext &context, std::string path, std::vector<DropdownOption> options,
                                       Dropdown::Options settings, ValueKind kind,
                                       std::function<void(const std::string &value)> afterChange)
{
    auto dropdown = std::make_shared<Dropdown>(std::move(options), settings);
    SettingsModel *model = &context.model;
    dropdown->SetOnChanged([model, path, kind, afterChange](size_t, const DropdownOption &option) {
        model->Update(path, ParseValue(option.value, kind));
        if (afterChange)
            afterChange(option.value);
    });
    std::weak_ptr<Dropdown> weak = dropdown;
    context.OnConfig([weak, model, path]() {
        if (auto control = weak.lock())
            control->SetValue(ValueAsString(model->Find(path)));
    });
    return dropdown;
}

std::shared_ptr<CheckOption> BindCheck(PageContext &context, std::wstring label, std::string path,
                                       std::function<void(bool checked)> afterChange)
{
    auto check = std::make_shared<CheckOption>(std::move(label), context.model.GetBool(path));
    SettingsModel *model = &context.model;
    check->SetOnChanged([model, path, afterChange](bool checked) {
        model->Update(path, checked);
        if (afterChange)
            afterChange(checked);
    });
    std::weak_ptr<CheckOption> weak = check;
    context.OnConfig([weak, model, path]() {
        if (auto control = weak.lock())
            control->SetChecked(model->GetBool(path));
    });
    return check;
}

std::vector<std::shared_ptr<CheckOption>> AtLeastOneChecks(
    PageContext &context, const std::vector<std::pair<std::wstring, std::string>> &items)
{
    auto checks = std::make_shared<std::vector<std::shared_ptr<CheckOption>>>();
    SettingsModel *model = &context.model;
    for (size_t i = 0; i < items.size(); ++i)
    {
        auto check = std::make_shared<CheckOption>(items[i].first, model->GetBool(items[i].second));
        const std::string path = items[i].second;
        // The group outlives none of its checks: the page and its controls live as long as
        // the window, so the checks simply share it.
        std::vector<std::shared_ptr<CheckOption>> *all = checks.get();
        CheckOption *self = check.get();
        check->SetOnChanged([checks, all, self, model, path](bool checked) {
            const bool anyChecked =
                std::any_of(all->begin(), all->end(), [](const auto &item) { return item->IsChecked(); });
            if (!anyChecked)
            {
                self->SetChecked(true);
                return;
            }
            model->Update(path, checked);
        });
        std::weak_ptr<CheckOption> weak = check;
        context.OnConfig([weak, model, path]() {
            if (auto control = weak.lock())
                control->SetChecked(model->GetBool(path));
        });
        checks->push_back(check);
    }
    return *checks;
}

std::shared_ptr<TextField> BindText(PageContext &context, std::string path, TextField::Options options)
{
    auto field = std::make_shared<TextField>(std::move(options));
    SettingsModel *model = &context.model;
    field->SetOnCommit([model, path](const std::wstring &text) { model->Update(path, Narrow(text)); });
    std::weak_ptr<TextField> weak = field;
    context.OnConfig([weak, model, path]() {
        if (auto control = weak.lock())
            control->SetText(Widen(model->GetString(path)));
    });
    return field;
}

std::shared_ptr<NumberStepper> BindStepper(PageContext &context, std::string path, int minimum, int maximum, int step)
{
    auto stepper = std::make_shared<NumberStepper>(minimum, maximum, step);
    SettingsModel *model = &context.model;
    stepper->SetOnChanged([model, path](int value) { model->Update(path, value); });
    std::weak_ptr<NumberStepper> weak = stepper;
    context.OnConfig([weak, model, path, minimum]() {
        if (auto control = weak.lock())
            control->SetValue(static_cast<int>(model->GetInt(path, minimum)));
    });
    return stepper;
}

std::vector<DropdownOption> Options(std::initializer_list<std::pair<const char *, const wchar_t *>> items)
{
    std::vector<DropdownOption> options;
    options.reserve(items.size());
    for (const auto &[value, label] : items)
        options.push_back({label, value, false, {}});
    return options;
}

std::vector<DropdownOption> NumberOptions(int first, int last, const wchar_t *suffix)
{
    std::vector<DropdownOption> options;
    for (int value = first; value <= last; ++value)
        options.push_back({std::to_wstring(value) + suffix, std::to_string(value), false, {}});
    return options;
}

std::wstring Widen(const std::string &text)
{
    return string_to_wstring(text);
}

std::string Narrow(const std::wstring &text)
{
    return wstring_to_string(text);
}

const std::vector<PageInfo> &Pages()
{
    static const std::vector<PageInfo> pages = {
        // 输入在外观之前、也是默认打开的页面：冷启动不用排版候选窗预览、解码皮肤图片。
        {"input", L"输入", L"sidebar/input.svg", &BuildInputPage},
        {"appearance", L"外观", L"sidebar/appearance.svg", &BuildAppearancePage},
        {"helpcode", L"辅助码", L"sidebar/helpcode.svg", &BuildHelpcodePage},
        {"shortcut", L"快捷键", L"sidebar/shortcut.svg", &BuildShortcutPage},
        {"dict", L"词库", L"sidebar/dictionary.svg", &BuildDictionaryPage},
        {"skin", L"皮肤", L"sidebar/skin.svg", &BuildSkinPage},
        {"voice", L"语音输入", L"sidebar/voice_input.svg", &BuildVoicePage},
        {"screenkb-settings", L"屏幕键盘", L"sidebar/screen_keyboard.svg", &BuildScreenKeyboardPage},
        {"handwriting-settings", L"手写识别板", L"sidebar/handwriting.svg", &BuildHandwritingPage},
        {"tools-settings", L"实用功能", L"sidebar/utilities.svg", &BuildToolsPage},
        {"ai-settings", L"AI 与云联想", L"sidebar/ai.svg", &BuildAiPage},
        {"floating-toolbar", L"悬浮工具栏", L"sidebar/floating_toolbar.svg", &BuildFloatingToolbarPage},
        {"stats", L"统计", L"sidebar/stats.svg", &BuildStatsPage},
        {"help-settings", L"帮助", L"sidebar/help.svg", &BuildHelpPage},
        {"account-settings", L"账号", L"sidebar/account.svg", &BuildAccountPage},
        {"about-settings", L"关于", L"sidebar/about.svg", &BuildAboutPage},
        {"feedback-settings", L"反馈", L"sidebar/feedback.svg", &BuildFeedbackPage},
    };
    return pages;
}
} // namespace native_settings
