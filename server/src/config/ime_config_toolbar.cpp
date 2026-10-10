// 悬浮工具栏与光标状态角标配置：显隐、按钮项、缩放与字号，以及角标的开关、聚焦时显示和位置。
#include "window/caret_state_indicator_policy.h"
#include "config/ime_config_internal.h"
#include <cmath>
#include <string>

using namespace ime_config_detail;

bool GetConfiguredFloatingToolbarEnabled()
{
    return g_floating_toolbar_enabled;
}

bool SetConfiguredFloatingToolbarEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "floating_toolbar", enabled ? "true" : "false"))
    {
        return false;
    }
    g_floating_toolbar_enabled = enabled;
    return true;
}

bool GetConfiguredCaretStateIndicatorEnabled()
{
    return g_caret_state_indicator_enabled;
}

bool SetConfiguredCaretStateIndicatorEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "caret_state_indicator", enabled ? "true" : "false"))
    {
        return false;
    }
    g_caret_state_indicator_enabled = enabled;
    return true;
}

bool GetConfiguredCaretStateIndicatorOnFocus()
{
    return g_caret_state_indicator_on_focus;
}

bool SetConfiguredCaretStateIndicatorOnFocus(bool enabled)
{
    if (!WriteConfiguredValue("general", "caret_state_indicator_on_focus", enabled ? "true" : "false"))
    {
        return false;
    }
    g_caret_state_indicator_on_focus = enabled;
    return true;
}

const std::string &GetConfiguredCaretStateIndicatorPosition()
{
    return g_caret_state_indicator_position;
}

bool SetConfiguredCaretStateIndicatorPosition(const std::string &position)
{
    if (!FanyImeUi::IsValidCaretStatePosition(position))
        return false;
    if (!WriteConfiguredValue("general", "caret_state_indicator_position", EscapeTomlBasicString(position)))
        return false;
    g_caret_state_indicator_position = position;
    return true;
}

const FloatingToolbarItemsConfig &GetConfiguredFloatingToolbarItems()
{
    return g_floating_toolbar_items;
}

bool SetConfiguredFloatingToolbarItemEnabled(const std::string &item, bool enabled)
{
    bool *target = nullptr;
    if (item == "fullwidth")
        target = &g_floating_toolbar_items.fullwidth;
    else if (item == "punctuation")
        target = &g_floating_toolbar_items.punctuation;
    else if (item == "character_set")
        target = &g_floating_toolbar_items.character_set;
    else if (item == "emoji")
        target = &g_floating_toolbar_items.emoji;
    else if (item == "screen_keyboard")
        target = &g_floating_toolbar_items.screen_keyboard;
    else if (item == "settings")
        target = &g_floating_toolbar_items.settings;
    else
        return false;

    if (!WriteConfiguredValue("general", "floating_toolbar_" + item, enabled ? "true" : "false"))
        return false;
    *target = enabled;
    return true;
}

namespace
{
double SnapFloatingToolbarScale(double scale)
{
    static const double kAllowed[] = {0.75, 1.0, 1.25, 1.5};
    double best = 1.0;
    double bestDelta = std::abs(scale - best);
    for (double candidate : kAllowed)
    {
        const double delta = std::abs(scale - candidate);
        if (delta < bestDelta)
        {
            best = candidate;
            bestDelta = delta;
        }
    }
    return best;
}

std::string FormatFloatingToolbarScale(double scale)
{
    if (std::abs(scale - 0.75) < 0.001)
        return "0.75";
    if (std::abs(scale - 1.25) < 0.001)
        return "1.25";
    if (std::abs(scale - 1.5) < 0.001)
        return "1.5";
    return "1.0";
}
} // namespace

double GetConfiguredFloatingToolbarScale()
{
    return g_floating_toolbar_scale;
}

bool SetConfiguredFloatingToolbarScale(double scale)
{
    if (scale < kFloatingToolbarScaleMin || scale > kFloatingToolbarScaleMax)
        return false;
    scale = SnapFloatingToolbarScale(scale);
    if (!WriteConfiguredValue("general", "floating_toolbar_scale", FormatFloatingToolbarScale(scale)))
        return false;
    g_floating_toolbar_scale = scale;
    return true;
}

int GetConfiguredFloatingToolbarFontSize()
{
    return g_floating_toolbar_font_size;
}

bool SetConfiguredFloatingToolbarFontSize(int font_size)
{
    if (font_size < kFloatingToolbarFontSizeMin || font_size > kFloatingToolbarFontSizeMax)
        return false;
    if (!WriteConfiguredValue("general", "floating_toolbar_font_size", std::to_string(font_size)))
        return false;
    g_floating_toolbar_font_size = font_size;
    return true;
}

bool GetConfiguredFloatingToolbarAutoHide()
{
    return g_floating_toolbar_auto_hide;
}

bool SetConfiguredFloatingToolbarAutoHide(bool enabled)
{
    if (!WriteConfiguredValue("general", "floating_toolbar_auto_hide", enabled ? "true" : "false"))
        return false;
    g_floating_toolbar_auto_hide = enabled;
    return true;
}

int GetConfiguredFloatingToolbarAutoHideDelay()
{
    return g_floating_toolbar_auto_hide_delay;
}

bool SetConfiguredFloatingToolbarAutoHideDelay(int seconds)
{
    if (seconds < kFloatingToolbarAutoHideDelayMin || seconds > kFloatingToolbarAutoHideDelayMax)
        return false;
    if (!WriteConfiguredValue("general", "floating_toolbar_auto_hide_delay", std::to_string(seconds)))
        return false;
    g_floating_toolbar_auto_hide_delay = seconds;
    return true;
}

bool GetConfiguredFloatingToolbarShadow()
{
    return g_floating_toolbar_shadow;
}

bool SetConfiguredFloatingToolbarShadow(bool enabled)
{
    if (!WriteConfiguredValue("general", "floating_toolbar_shadow", enabled ? "true" : "false"))
        return false;
    g_floating_toolbar_shadow = enabled;
    return true;
}
