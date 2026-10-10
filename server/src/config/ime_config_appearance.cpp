// 候选窗外观配置：每页候选数、字体与字号、文字颜色、布局与跟随光标、小窗 UI 后端、
// 皮肤、预编辑样式、固定徽标和设置窗口保留时长。
#include "window/ui_backend_policy.h"
#include "config/ime_config_internal.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

using namespace ime_config_detail;

int GetConfiguredCandidatePageSize()
{
    return g_candidate_page_size;
}

bool SetConfiguredCandidatePageSize(int page_size)
{
    if (page_size < 3 || page_size > 9)
        return false;
    if (!WriteConfiguredValue("appearance", "page_size", std::to_string(page_size)))
        return false;
    g_candidate_page_size = page_size;
    return true;
}

const std::string &GetConfiguredCandidateFont()
{
    return g_candidate_font;
}

bool SetConfiguredCandidateFont(const std::string &font)
{
    if (font.empty() || font.size() > 64)
        return false;
    if (!WriteConfiguredValue("appearance", "font", EscapeTomlBasicString(font)))
        return false;
    g_candidate_font = font;
    return true;
}

const std::string &GetConfiguredCandidateEnglishFont()
{
    return g_candidate_english_font;
}

bool SetConfiguredCandidateEnglishFont(const std::string &font)
{
    if (font.empty() || font.size() > 64)
        return false;
    if (!WriteConfiguredValue("appearance", "english_font", EscapeTomlBasicString(font)))
        return false;
    g_candidate_english_font = font;
    return true;
}

const std::string &GetConfiguredCandidateDefaultFont()
{
    return g_candidate_default_font;
}

bool SetConfiguredCandidateDefaultFont(const std::string &font)
{
    if (font.empty() || font.size() > 64)
        return false;
    if (!WriteConfiguredValue("appearance", "default_font", EscapeTomlBasicString(font)))
        return false;
    g_candidate_default_font = font;
    return true;
}

int GetConfiguredCandidateFontSize()
{
    return g_candidate_font_size;
}

const std::vector<std::string> &GetConfiguredCandidateFallbackFonts()
{
    return g_candidate_fallback_fonts;
}

std::vector<std::string> GetConfiguredCandidateFallbackFontFamilies()
{
    std::vector<std::string> families;
    for (const auto &font : g_candidate_fallback_fonts)
        families.push_back(ResolveSystemFontFamilyForCss(font));
    return families;
}

bool SetConfiguredCandidateFallbackFonts(const std::vector<std::string> &fonts)
{
    if (fonts.size() > 32)
        return false;
    std::vector<std::string> normalized;
    std::string value = "[";
    for (const auto &font : fonts)
    {
        if (font.empty() || font.size() > 256 ||
            std::any_of(font.begin(), font.end(), [](unsigned char ch) { return ch < 32 || ch == 127; }))
            return false;
        if (std::find(normalized.begin(), normalized.end(), font) != normalized.end())
            continue;
        if (!normalized.empty())
            value += ", ";
        value += EscapeTomlBasicString(font);
        normalized.push_back(font);
    }
    value += "]";
    if (!WriteConfiguredValue("appearance", "fallback_fonts", value))
        return false;
    g_candidate_fallback_fonts = std::move(normalized);
    return true;
}

bool SetConfiguredCandidateFontSize(int font_size)
{
    if (font_size < kCandidateFontSizeMin || font_size > kCandidateFontSizeMax)
        return false;
    if (!WriteConfiguredValue("appearance", "font_size", std::to_string(font_size)))
        return false;
    g_candidate_font_size = font_size;
    return true;
}

int GetConfiguredCandidateWindowPreeditFontSize()
{
    return g_candidate_window_preedit_font_size;
}

bool SetConfiguredCandidateWindowPreeditFontSize(int font_size)
{
    if (font_size < kCandidateFontSizeMin || font_size > kCandidateFontSizeMax)
        return false;
    if (!WriteConfiguredValue("appearance", "candidate_window_preedit_font_size", std::to_string(font_size)))
        return false;
    g_candidate_window_preedit_font_size = font_size;
    return true;
}

namespace
{
bool IsValidCandidateTextColor(const std::string &color)
{
    if (color.empty() || color == "auto")
        return true;
    if (color.size() != 7 || color[0] != '#')
        return false;
    for (size_t i = 1; i < color.size(); ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(color[i]);
        if (!std::isxdigit(ch))
            return false;
    }
    return true;
}
} // namespace

const std::string &GetConfiguredCandidateTextColor()
{
    return g_candidate_text_color;
}

bool SetConfiguredCandidateTextColor(const std::string &color)
{
    const std::string normalized = color.empty() ? "auto" : color;
    if (!IsValidCandidateTextColor(normalized))
        return false;
    if (!WriteConfiguredValue("appearance", "cand_text_color", EscapeTomlBasicString(normalized)))
        return false;
    g_candidate_text_color = normalized;
    return true;
}

const std::string &GetConfiguredCandidateWindowLayout()
{
    return g_candidate_window_layout;
}

bool SetConfiguredCandidateWindowLayout(const std::string &layout)
{
    if (layout != "vertical" && layout != "horizontal")
    {
        return false;
    }

    if (!WriteConfiguredValue("appearance", "candidate_window_layout", EscapeTomlBasicString(layout)))
    {
        return false;
    }
    g_candidate_window_layout = layout;
    return true;
}

bool GetConfiguredCandidateWindowFollowCursor()
{
    return g_candidate_window_follow_cursor;
}

bool SetConfiguredCandidateWindowFollowCursor(bool enabled)
{
    if (!WriteConfiguredValue("appearance", "candidate_window_follow_cursor", enabled ? "true" : "false"))
    {
        return false;
    }
    g_candidate_window_follow_cursor = enabled;
    return true;
}

const std::string &GetConfiguredUiBackend()
{
    return g_ui_backend;
}

bool SetConfiguredUiBackend(const std::string &backend)
{
    if (!UiBackendPolicy::IsSupported(backend))
        return false;
    const std::string normalized = NormalizeSmallWindowUiBackend(backend);
    if (!WriteConfiguredValue("appearance", "ui_backend", EscapeTomlBasicString(normalized)))
    {
        return false;
    }
    g_ui_backend = normalized;
    return true;
}

bool UseD2dSmallWindowUi()
{
    return UiBackendPolicy::Resolve(UiBackendPolicy::Surface::Candidate, g_ui_backend_active) ==
           UiBackendPolicy::Backend::Native;
}

void FallBackToD2dSmallWindowUi()
{
    g_ui_backend_active = "d2d";
}

const std::string &GetConfiguredCandidateSkin()
{
    return g_candidate_skin;
}

bool SetConfiguredCandidateSkin(const std::string &skin)
{
    if (!IsValidCandidateSkinId(skin))
    {
        return false;
    }
    if (!WriteConfiguredValue("appearance", "candidate_skin", EscapeTomlBasicString(skin)))
    {
        return false;
    }
    g_candidate_skin = skin;
    return true;
}

const std::string &GetConfiguredCandidateWindowPreeditStyle()
{
    return g_candidate_window_preedit_style;
}

bool SetConfiguredCandidateWindowPreeditStyle(const std::string &style)
{
    if (style != "pinyin" && style != "empty")
    {
        return false;
    }
    if (!WriteConfiguredValue("appearance", "candidate_window_preedit_style", EscapeTomlBasicString(style)))
    {
        return false;
    }
    g_candidate_window_preedit_style = style;
    return true;
}

bool GetConfiguredCandidateWindowPreeditShuangpinQuanpin()
{
    return g_candidate_window_preedit_shuangpin_quanpin;
}

bool SetConfiguredCandidateWindowPreeditShuangpinQuanpin(bool enabled)
{
    if (!WriteConfiguredValue("appearance", "candidate_window_preedit_shuangpin_quanpin", enabled ? "true" : "false"))
    {
        return false;
    }
    g_candidate_window_preedit_shuangpin_quanpin = enabled;
    return true;
}

bool GetConfiguredCandidateFixedBadge()
{
    return g_candidate_fixed_badge;
}

bool SetConfiguredCandidateFixedBadge(bool enabled)
{
    if (!WriteConfiguredValue("appearance", "candidate_fixed_badge", enabled ? "true" : "false"))
    {
        return false;
    }
    g_candidate_fixed_badge = enabled;
    return true;
}

const std::string &GetConfiguredCandidateFixedBadgeStyle()
{
    return g_candidate_fixed_badge_style;
}

bool SetConfiguredCandidateFixedBadgeStyle(const std::string &style)
{
    if (!IsValidCandidateFixedBadgeStyle(style))
    {
        return false;
    }
    if (!WriteConfiguredValue("appearance", "candidate_fixed_badge_style", EscapeTomlBasicString(style)))
    {
        return false;
    }
    g_candidate_fixed_badge_style = style;
    return true;
}

const std::string &GetConfiguredSettingsWindowLinger()
{
    return g_settings_window_linger;
}

bool SetConfiguredSettingsWindowLinger(const std::string &linger)
{
    if (!IsValidSettingsWindowLinger(linger))
    {
        return false;
    }
    if (!WriteConfiguredValue("appearance", "settings_window_linger", EscapeTomlBasicString(linger)))
    {
        return false;
    }
    g_settings_window_linger = linger;
    return true;
}
