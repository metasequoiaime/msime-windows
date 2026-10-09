#pragma once

#include <string>

namespace VimMode
{
// YAML app_options, keyed by executable basename. Missing/invalid entries are disabled.
bool EnabledForProcess(const std::string &yaml, const std::string &processName);

// VK_ESCAPE is 0x1B; the policy stays independent of Windows/TSF for local tests.
constexpr bool ShouldSwitchToEnglish(unsigned key, bool eaten, bool repeat, unsigned modifiers, bool winDown,
                                     bool compositionActive, bool deferredKeysPending)
{
    return key == 0x1B && !eaten && !repeat && modifiers == 0 && !winDown && !compositionActive && !deferredKeysPending;
}
} // namespace VimMode
