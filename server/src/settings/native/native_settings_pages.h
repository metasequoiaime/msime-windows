#pragma once

#include "settings/native/native_settings_controls.h"
#include "settings/native/native_settings_model.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Page builders of the Direct2D settings window, one per sidebar entry. They were ported
// one to one from the former WebView2 settings page, which was removed together with
// WebView2; the "partials/<id>.html + modules/<id>.ts" notes at the top of each
// native_page_*.cpp name its sources, still in git history under
// ui-html/webview2/settings/ime-settings/src/. A builder returns the page
// content once; afterwards the page keeps itself in sync through PageContext::OnConfig
// and the model's message listeners.
namespace native_settings
{
// Services the window offers to pages.
class HostServices
{
  public:
    virtual ~HostServices() = default;
    virtual HWND Hwnd() const = 0;
    virtual msimeui::Window *Window() const = 0;
    // `.dict-toast`: bottom-center of the content area, hides after a few seconds.
    virtual void ShowToast(const std::wstring &message, bool success = true) = 0;
    // `confirmDialog()`: modal with a cancel and a danger button.
    virtual void Confirm(const std::wstring &message, std::function<void(bool confirmed)> done,
                         const std::wstring &title = L"确认删除", const std::wstring &confirmText = L"删除",
                         const std::wstring &cancelText = L"取消") = 0;
    // `.dict-modal` + `.dict-dialog`: shows `dialog` centered over the content area on
    // the overlay; `onDismiss` runs when the overlay itself is clicked or Esc pressed.
    virtual void ShowModal(VisualPtr dialog, std::function<void()> onDismiss = {}) = 0;
    virtual void CloseModal() = 0;
    // Switches to another sidebar page, e.g. a link that points at the dictionary page.
    virtual void NavigateTo(const std::string &pageId) = 0;
    // Brings `visual` (already in the current page) into view.
    virtual void ScrollIntoView(const msimeui::Visual &visual) = 0;
};

class PageContext
{
  public:
    PageContext(SettingsModel &model, HostServices &host);

    SettingsModel &model;
    HostServices &host;

    // Runs `sync` now (when a snapshot is already there) and after every snapshot.
    void OnConfig(std::function<void()> sync);
    // Message listener that lives as long as the window.
    void OnMessage(const std::string &type, std::function<void(const Json &)> listener);
};

// --------------------------------------------------------------------------------
// Bindings between controls and config paths, mirroring setupToggleButton,
// setupDropdownMenu and friends: user changes post configUpdate, snapshots update
// the control without calling back into it.
// --------------------------------------------------------------------------------
enum class ValueKind
{
    String,
    Integer,
    Number,
};

std::shared_ptr<ToggleSwitch> BindToggle(PageContext &context, std::string path,
                                         std::function<void(bool on)> afterChange = {});
std::shared_ptr<Dropdown> BindDropdown(PageContext &context, std::string path, std::vector<DropdownOption> options,
                                       Dropdown::Options settings = {}, ValueKind kind = ValueKind::String,
                                       std::function<void(const std::string &value)> afterChange = {});
std::shared_ptr<CheckOption> BindCheck(PageContext &context, std::wstring label, std::string path,
                                       std::function<void(bool checked)> afterChange = {});
// A group of (label, path) config checkboxes where the last checked one cannot be unchecked.
std::vector<std::shared_ptr<CheckOption>> AtLeastOneChecks(
    PageContext &context, const std::vector<std::pair<std::wstring, std::string>> &items);
// Writes on commit (blur / Enter), like the page's `change` listeners.
std::shared_ptr<TextField> BindText(PageContext &context, std::string path, TextField::Options options);
std::shared_ptr<NumberStepper> BindStepper(PageContext &context, std::string path, int minimum, int maximum,
                                           int step = 1);
// Options for a dropdown from (value, label) pairs.
std::vector<DropdownOption> Options(std::initializer_list<std::pair<const char *, const wchar_t *>> items);
// Numeric options from `first` to `last` labelled with the number itself.
std::vector<DropdownOption> NumberOptions(int first, int last, const wchar_t *suffix = L"");

// UTF-8 <-> UTF-16 for JSON strings.
std::wstring Widen(const std::string &text);
std::string Narrow(const std::wstring &text);

using PageFactory = VisualPtr (*)(PageContext &context);

struct PageInfo
{
    const char *id;       // data-target of the sidebar item
    const wchar_t *title; // sidebar label
    const wchar_t *icon;  // assets/sidebar/<icon>
    PageFactory build;
};

// Sidebar order, as in partials/sidebar.html except that 输入 now comes before 外观. The
// first entry is the page the window opens on.
const std::vector<PageInfo> &Pages();

VisualPtr BuildAppearancePage(PageContext &context);
VisualPtr BuildInputPage(PageContext &context);
VisualPtr BuildHelpcodePage(PageContext &context);
VisualPtr BuildShortcutPage(PageContext &context);
VisualPtr BuildDictionaryPage(PageContext &context);
VisualPtr BuildSkinPage(PageContext &context);
VisualPtr BuildVoicePage(PageContext &context);
VisualPtr BuildScreenKeyboardPage(PageContext &context);
VisualPtr BuildHandwritingPage(PageContext &context);
VisualPtr BuildToolsPage(PageContext &context);
VisualPtr BuildAiPage(PageContext &context);
VisualPtr BuildFloatingToolbarPage(PageContext &context);
VisualPtr BuildStatsPage(PageContext &context);
VisualPtr BuildHelpPage(PageContext &context);
VisualPtr BuildAboutPage(PageContext &context);
VisualPtr BuildFeedbackPage(PageContext &context);
VisualPtr BuildAccountPage(PageContext &context);
} // namespace native_settings
