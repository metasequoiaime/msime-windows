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
    const std::string config = "\xEF\xBB\xBF[general]\n"
                               "vim_mode_apps = [\"general.exe\"]\n"
                               "[input]\n"
                               "punctuation_lock = \"follow\" # vim_mode_apps = [\"comment.exe\"]\n"
                               "# vim_mode_apps = [\"commented.exe\"]\n"
                               "vim_mode_apps = [\n"
                               "  \"Code.exe\", # VS Code\n"
                               "  'gvim.exe',\n"
                               "  \"quoted\\\"name.exe\",\n"
                               "]\n"
                               "[keybindings]\n"
                               "vim_mode_apps = [\"keybindings.exe\"]\n";
    if (!Check(VimMode::EnabledForProcess(config, "Code.exe"), "configured editor") ||
        !Check(VimMode::EnabledForProcess(config, "CODE.EXE"), "executable name case") ||
        !Check(VimMode::EnabledForProcess(config, "gvim.exe"), "literal string") ||
        !Check(VimMode::EnabledForProcess(config, "quoted\"name.exe"), "escaped quote") ||
        !Check(!VimMode::EnabledForProcess(config, "other.exe"), "unlisted app") ||
        !Check(!VimMode::EnabledForProcess(config, ""), "unknown host") ||
        !Check(!VimMode::EnabledForProcess(config, "comment.exe"), "trailing comment") ||
        !Check(!VimMode::EnabledForProcess(config, "commented.exe"), "commented-out key") ||
        !Check(!VimMode::EnabledForProcess(config, "general.exe"), "key in another section") ||
        !Check(!VimMode::EnabledForProcess(config, "keybindings.exe"), "key after the input section") ||
        !Check(!VimMode::EnabledForProcess(config, "C:\\editors\\Code.exe"), "only match executable basename") ||
        !Check(!VimMode::EnabledForProcess("", "Code.exe"), "empty file") ||
        !Check(!VimMode::EnabledForProcess("[input]\nvim_mode_apps = []\n", "Code.exe"), "empty list") ||
        !Check(!VimMode::EnabledForProcess("[input]\nvim_mode_apps = [\"Code.exe\"\n", "Code.exe"), "unclosed list") ||
        !Check(!VimMode::EnabledForProcess("[input]\nvim_mode_apps = \"Code.exe\"\n", "Code.exe"), "not a list") ||
        !Check(!VimMode::EnabledForProcess("[input]\nvim_mode_apps = [\"Code.exe\", 1]\n", "Code.exe"),
               "non-string item") ||
        !Check(!VimMode::EnabledForProcess("[input]\nvim_mode_apps = [\"Code.exe\" \"gvim.exe\"]\n", "Code.exe"),
               "missing comma") ||
        !Check(!VimMode::EnabledForProcess("[input]\nvim_mode_apps = [\"Code.exe\\t\"]\n", "Code.exe"),
               "unsupported escape"))
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
