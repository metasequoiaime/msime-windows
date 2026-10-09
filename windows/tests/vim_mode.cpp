#include "Key/VimMode.h"
#include <iostream>

namespace
{
bool Check(bool condition, const char *scenario)
{
    if (!condition)
        std::cerr << scenario << '\n';
    return condition;
}
} // namespace

int main()
{
    const std::string config = R"yaml(
# Only explicitly enabled executables participate.
app_options:
  Code.exe:
    vim_mode: true
  gvim.exe: {vim_mode: true}
  notepad.exe:
    vim_mode: false
  missing.exe: {}
  invalid.exe: {vim_mode: perhaps}
  wrong-type.exe: true
)yaml";
    if (!Check(VimMode::EnabledForProcess(config, "Code.exe"), "configured editor") ||
        !Check(VimMode::EnabledForProcess(config, "CODE.EXE"), "executable name case") ||
        !Check(VimMode::EnabledForProcess(config, "gvim.exe"), "inline YAML mapping") ||
        !Check(!VimMode::EnabledForProcess(config, "notepad.exe"), "explicitly disabled app") ||
        !Check(!VimMode::EnabledForProcess(config, "other.exe"), "unlisted app") ||
        !Check(!VimMode::EnabledForProcess(config, ""), "unknown host") ||
        !Check(!VimMode::EnabledForProcess(config, "missing.exe"), "missing switch") ||
        !Check(!VimMode::EnabledForProcess(config, "invalid.exe"), "invalid switch") ||
        !Check(!VimMode::EnabledForProcess(config, "wrong-type.exe"), "invalid options") ||
        !Check(!VimMode::EnabledForProcess("", "Code.exe"), "empty file") ||
        !Check(!VimMode::EnabledForProcess("app_options: [", "Code.exe"), "malformed YAML") ||
        !Check(!VimMode::EnabledForProcess("app_options: [Code.exe]", "Code.exe"), "invalid app map") ||
        !Check(!VimMode::EnabledForProcess("[Code.exe]", "Code.exe"), "invalid root") ||
        !Check(!VimMode::EnabledForProcess("app_options: {Code.exe: {}}", "Code.exe"), "unset switch") ||
        !Check(!VimMode::EnabledForProcess(config, "C:\\editors\\Code.exe"), "only match executable basename"))
    {
        return 1;
    }

    constexpr unsigned escape = 0x1B;
    using VimMode::ShouldSwitchToEnglish;
    if (!Check(ShouldSwitchToEnglish(escape, false, false, 0, false, false, false),
               "idle Esc reaches editor and requests English") ||
        !Check(!ShouldSwitchToEnglish(escape, true, false, 0, false, true, false),
               "Esc cancelling preedit keeps existing IME handling") ||
        !Check(!ShouldSwitchToEnglish(escape, false, false, 0, false, true, false),
               "unclaimed Esc with a live composition cannot switch") ||
        !Check(!ShouldSwitchToEnglish(escape, false, false, 0, false, false, true),
               "queued/in-flight keys cannot be bypassed") ||
        !Check(!ShouldSwitchToEnglish(escape, false, true, 0, false, false, false),
               "holding cancellation Esc cannot change mode on repeat") ||
        !Check(!ShouldSwitchToEnglish('A', false, false, 0, false, false, false), "ordinary typing") ||
        !Check(!ShouldSwitchToEnglish(escape, false, false, 0, true, false, false), "Win shortcut"))
    {
        return 2;
    }
    for (unsigned modifiers = 1; modifiers < 8; ++modifiers)
    {
        if (!Check(!ShouldSwitchToEnglish(escape, false, false, modifiers, false, false, false),
                   "Shift/Ctrl/Alt shortcuts keep their existing behavior"))
            return 3;
    }
    // After cancelling the composition, a NEW Esc can switch; repeat above cannot.
    if (!Check(ShouldSwitchToEnglish(escape, false, false, 0, false, false, false), "second fresh Esc"))
        return 4;
    return 0;
}
