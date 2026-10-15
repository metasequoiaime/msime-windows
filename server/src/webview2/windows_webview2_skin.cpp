// 皮肤与外观：读取页面 HTML、外部候选皮肤的 CSS 注入与内嵌资源、PrepareHtmlForWnds，
// 以及候选窗、悬浮工具栏的布局 / 主题 / 字体等外观应用。
#include "webview2/windows_webview2_internal.h"
#include "webview2/inline_protocol.h"
#include "webview2/skin_css_policy.h"
#include "config/ime_config.h"
#include "defines/globals.h"
#include "skin/candidate_skin_catalog.h"
#include "utils/common_utils.h"
#include "window/candidate_presenter.h"
#include "window/candidate_skin_palette.h"
#include "window/floating_toolbar_presenter.h"
#include "fmt/xchar.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
std::optional<CandidateSkinCatalog::Package> activeExternalCandidateSkin;
} // namespace

double GetActiveCandidateSkinDecorationTopDip()
{
    return activeExternalCandidateSkin ? activeExternalCandidateSkin->decorationTopDip : 0.0;
}

double GetActiveCandidateSkinDecorationWidthDip()
{
    return activeExternalCandidateSkin ? activeExternalCandidateSkin->decorationWidthDip : 0.0;
}

namespace
{
double ActiveOuterInset(bool top, bool right, bool bottom)
{
    if (!activeExternalCandidateSkin || !activeExternalCandidateSkin->outerInsetsDip)
    {
        return 0.0;
    }
    const CandidateSkinCatalog::InsetsDip &insets = *activeExternalCandidateSkin->outerInsetsDip;
    if (top)
        return insets.top;
    if (right)
        return insets.right;
    if (bottom)
        return insets.bottom;
    return insets.left;
}
} // namespace

double GetActiveCandidateSkinOuterTopDip()
{
    return ActiveOuterInset(true, false, false);
}

double GetActiveCandidateSkinOuterRightDip()
{
    return ActiveOuterInset(false, true, false);
}

double GetActiveCandidateSkinOuterBottomDip()
{
    return ActiveOuterInset(false, false, true);
}

double GetActiveCandidateSkinOuterLeftDip()
{
    return ActiveOuterInset(false, false, false);
}

std::wstring ReadHtmlFile(const std::wstring &filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file)
        return L"";
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return string_to_wstring(buffer.str());
}

// An unreadable themed asset must never turn into an empty NavigateToString:
// that renders a blank, title-less page which looks like a dead WebView.
std::wstring ReadHtmlFileWithFallback(const std::wstring &primaryPath, const std::wstring &fallbackPath)
{
    std::wstring content = ReadHtmlFile(primaryPath);
    if (content.empty() && primaryPath != fallbackPath)
    {
        content = ReadHtmlFile(fallbackPath);
    }
    return content;
}

namespace
{
std::wstring NormalizeSkinCssUrl(std::wstring url)
{
    while (!url.empty() && iswspace(url.front()))
    {
        url.erase(url.begin());
    }
    while (!url.empty() && iswspace(url.back()))
    {
        url.pop_back();
    }
    while (url.size() >= 2 && url[0] == L'.' && url[1] == L'/')
    {
        url.erase(0, 2);
    }
    std::replace(url.begin(), url.end(), L'\\', L'/');
    return url;
}

std::wstring EncodeBase64(const std::vector<unsigned char> &bytes)
{
    static constexpr wchar_t kTable[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < bytes.size())
    {
        const unsigned int triple = (static_cast<unsigned int>(bytes[i]) << 16) |
                                    (static_cast<unsigned int>(bytes[i + 1]) << 8) |
                                    static_cast<unsigned int>(bytes[i + 2]);
        out.push_back(kTable[(triple >> 18) & 63]);
        out.push_back(kTable[(triple >> 12) & 63]);
        out.push_back(kTable[(triple >> 6) & 63]);
        out.push_back(kTable[triple & 63]);
        i += 3;
    }
    if (i < bytes.size())
    {
        unsigned int triple = static_cast<unsigned int>(bytes[i]) << 16;
        if (i + 1 < bytes.size())
        {
            triple |= static_cast<unsigned int>(bytes[i + 1]) << 8;
        }
        out.push_back(kTable[(triple >> 18) & 63]);
        out.push_back(kTable[(triple >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? kTable[(triple >> 6) & 63] : L'=');
        out.push_back(L'=');
    }
    return out;
}

std::wstring MimeForSkinAsset(const std::wstring &relativePath)
{
    std::wstring lower = relativePath;
    for (wchar_t &ch : lower)
    {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, L".png") == 0)
        return L"image/png";
    if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, L".jpg") == 0)
        return L"image/jpeg";
    if (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, L".jpeg") == 0)
        return L"image/jpeg";
    if (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, L".webp") == 0)
        return L"image/webp";
    if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, L".gif") == 0)
        return L"image/gif";
    if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, L".svg") == 0)
        return L"image/svg+xml";
    return L"application/octet-stream";
}

std::wstring EmbedSkinCssUrl(const std::wstring &skinsRoot, const std::string &skinId, const std::wstring &rawUrl)
{
    const std::wstring relative = NormalizeSkinCssUrl(rawUrl);
    const std::filesystem::path filePath = std::filesystem::path(skinsRoot) / std::filesystem::u8path(skinId) /
                                           std::filesystem::u8path(wstring_to_string(relative));
    std::error_code ec;
    constexpr std::uintmax_t kMaxEmbedBytes = 1500 * 1024;
    const auto fileSize = std::filesystem::file_size(filePath, ec);
    if (!ec && fileSize > 0 && fileSize <= kMaxEmbedBytes)
    {
        std::ifstream stream(filePath, std::ios::binary);
        std::vector<unsigned char> bytes(static_cast<size_t>(fileSize));
        if (stream && stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(fileSize)))
        {
            return L"url(\"data:" + MimeForSkinAsset(relative) + L";base64," + EncodeBase64(bytes) + L"\")";
        }
    }
    return L"url(\"https://candidate-skins/" + string_to_wstring(skinId) + L"/" + relative + L"\")";
}

void NeutralizeEmbeddedStyleClosers(std::wstring &css)
{
    for (size_t i = 0; i + 7 < css.size(); ++i)
    {
        if (css[i] != L'<' || css[i + 1] != L'/')
        {
            continue;
        }
        std::wstring tag = css.substr(i + 2, 5);
        for (wchar_t &ch : tag)
        {
            ch = static_cast<wchar_t>(towlower(ch));
        }
        if (tag == L"style")
        {
            css.insert(i + 1, 1, L' ');
            i += 2;
        }
    }
}

void AppendExternalCandidateColorCss(std::wstring &css, const CandidateSkinCatalog::CandidateColors &colors)
{
    auto add = [&](const std::string &value, const wchar_t *selector, const wchar_t *property) {
        if (value.empty())
        {
            return;
        }
        css.append(selector);
        css.append(L" { ");
        css.append(property);
        css.append(L": ");
        css.append(string_to_wstring(value));
        css.append(L"; }\n");
    };
    add(colors.accent, L".cursor, .first::before", L"background");
    // 悬停在选中项上保持选中色；base 皮肤的 .cand.first:hover（如微信绿的 #07c160）会盖过低特异度的 .cand.first。
    add(colors.selected, L".first, .cand.first, .hover-active .cand.first:hover", L"background-color");
    add(colors.hover, L".hover-active .cand:not(.first):hover, .hover-active .page-arrow:not(.disabled):hover",
        L"background-color");
    add(colors.surface, L".container", L"background");
    add(colors.border, L".container", L"border-color");
    // .text 画的是 var(--cand-text)，只改 .container 的 color 够不着它。设置页的候选文字色写在 :root 的内联样式里，
    // 仍然压过这一条，与 D2D 的优先级一致。
    add(colors.text, L":root", L"--cand-text");
    add(colors.text, L".container", L"color");
    // 翻页箭头与 D2D 一样用序号色。
    add(colors.number, L".num, .cand-no, .page-arrow", L"color");
    // 翻译默认继承 .text 的颜色再叠 opacity .62；单独配色时取原值，不再叠透明度。
    add(colors.translation, L".cand-translation", L"color");
    if (!colors.translation.empty())
    {
        css.append(L".cand-translation { opacity: 1; }\n");
        // 换了翻译色就不再用基础皮肤给词性/分隔符配的颜色，没单独写的跟随新的翻译色。
        css.append(L".cand-translation .cand-pos, .cand-translation .cand-sep { color: inherit; }\n");
    }
    add(colors.translationPos, L".cand-translation .cand-pos", L"color");
    add(colors.translationSeparator, L".cand-translation .cand-sep", L"color");
    if (colors.showSelectedBar.has_value() && !*colors.showSelectedBar)
    {
        css.append(L".first::before { display: none; }\n");
    }

    // 细分配色。候选与预编辑文字先读设置页的 --msime-user-text，与 D2D 里用户文字色压过皮肤一致。
    // :where(.cand) 把特异度压到与基础 .text 相同，基础皮肤 `.first .text` 的选中行配色仍然生效。
    if (!colors.candidateText.empty())
        css.append(L":where(.cand) .text { color: var(--msime-user-text, " + string_to_wstring(colors.candidateText) +
                   L"); }\n");
    if (!colors.preeditText.empty())
        css.append(L".pinyin .text { color: var(--msime-user-text, " + string_to_wstring(colors.preeditText) +
                   L"); }\n");
    add(colors.preeditCaret, L".cursor", L"background");
    add(colors.selectedBar, L".first::before", L"background");
    add(colors.selectedText, L".cand.first .text", L"color");
    add(colors.selectedNumber, L".cand.first .num, .cand.first .cand-no", L"color");
    add(colors.selectedTranslation, L".cand.first .cand-translation", L"color");
    if (!colors.selectedTranslation.empty())
    {
        css.append(L".cand.first .cand-translation { opacity: 1; }\n");
        css.append(
            L".cand.first .cand-translation .cand-pos, .cand.first .cand-translation .cand-sep { color: inherit; "
            L"}\n");
    }
    add(colors.selectedTranslationPos, L".cand.first .cand-translation .cand-pos", L"color");
    add(colors.selectedTranslationSeparator, L".cand.first .cand-translation .cand-sep", L"color");
    add(colors.menuBackground, L".context-menu, .context-submenu", L"background");
    add(colors.menuBorder, L".context-menu, .context-submenu", L"border-color");
    add(colors.menuText, L".context-menu, .context-submenu, .context-menu-item", L"color");
    add(colors.menuHover, L".context-menu-item:hover", L"background");
}

std::wstring CssRgba(D2D1_COLOR_F color, double alpha)
{
    return fmt::format(L"rgba({}, {}, {}, {:.3f})", static_cast<int>(std::lround(color.r * 255.0f)),
                       static_cast<int>(std::lround(color.g * 255.0f)), static_cast<int>(std::lround(color.b * 255.0f)),
                       alpha);
}

// 预编辑底色带、分隔线、外框线宽、高亮圆角与卡片阴影。D2D 端在 CandidatePresenter::ApplySkin 里用同一组值。
void AppendExternalCandidateGeometryCss(std::wstring &css, const CandidateSkinCatalog::Package &skin, bool light)
{
    const CandidateSkinCatalog::CandidateColors &colors = light ? skin.light : skin.dark;
    const double itemRadius =
        skin.itemCornerRadiusDip ? *skin.itemCornerRadiusDip : CandidateSkinBaseItemRadiusDip(skin.base);
    if (!colors.preeditBackground.empty())
    {
        css.append(fmt::format(L".row.pinyin {{ background: {}; border-radius: {}px; }}\n",
                               string_to_wstring(colors.preeditBackground), itemRadius));
    }
    if (!colors.preeditDivider.empty())
    {
        css.append(L".row.pinyin { border-bottom: 1px solid " + string_to_wstring(colors.preeditDivider) + L"; }\n");
    }
    if (skin.borderWidthDip)
    {
        // 杨柳青、秋桂的基础 CSS 是 border: none，只改线宽会没有线型，所以连同线型和解析后的边框色一起写。
        const D2D1_COLOR_F border = ResolveCandidateSkinPalette(skin.base, light, {}, &colors, skin.base).border;
        css.append(fmt::format(L".container:not(:empty) {{ border: {}px solid {}; }}\n", *skin.borderWidthDip,
                               CssRgba(border, border.a)));
    }
    if (skin.itemCornerRadiusDip)
    {
        // 贴着外框四角的那几个角由更高特异度的贴角规则接管，仍跟外框圆角。
        const std::wstring radius = fmt::format(L"{}px", *skin.itemCornerRadiusDip);
        css.append(L".container { --ao-item-radius: " + radius + L"; }\n");
        css.append(L".container .cand, .container .cand.first { border-radius: " + radius + L"; }\n");
    }
    if (!skin.shadow.empty())
    {
        // 杨柳青的 .container 带 clip-path，阴影挂在 .containerParent 上；其余基础皮肤挂在 .container 上。
        const wchar_t *selector =
            skin.base == "willow_green" ? L".containerParent:not(:empty)" : L".container:not(:empty)";
        std::wstring value = L"none";
        if (skin.shadow != "none")
        {
            const double scale = skin.shadow == "soft" ? 0.5 : 1.6;
            const D2D1_COLOR_F black = D2D1::ColorF(0, 1.0f);
            value = L"8px 10px 24px " + CssRgba(black, (std::min)((light ? 0.18 : 0.34) * scale, 1.0)) +
                    L", 2px 3px 8px " + CssRgba(black, (std::min)((light ? 0.10 : 0.22) * scale, 1.0));
        }
        css.append(std::wstring(selector) + L" { box-shadow: " + value + L"; }\n");
    }
}

// Image paths come from the package's own skin.toml, so they get the same treatment as a URL
// written inside the stylesheet rather than being trusted because they arrived through a
// manifest field.
std::wstring ManifestImageCssUrl(const std::wstring &skinsRoot, const std::string &skinId, const std::string &path)
{
    const std::wstring raw = string_to_wstring(path);
    if (path.empty() || msime::skin_css::ClassifyUrl(raw) != msime::skin_css::UrlAction::Embed)
    {
        return L"none";
    }
    return EmbedSkinCssUrl(skinsRoot, skinId, raw);
}

void AppendExternalCandidateCornerCss(std::wstring &css, const CandidateSkinCatalog::Package &skin)
{
    if (!skin.cornerRadiusDip)
    {
        return;
    }
    const std::wstring radius = fmt::format(L"{}px", *skin.cornerRadiusDip);
    // 杨柳青、秋桂的外框与贴角高亮都读这两个变量，改变量即可；杨柳青的外框阴影挂在 .containerParent 上。
    css.append(L"body, .container { border-radius: " + radius + L"; }\n");
    css.append(L".container { --wg-radius: " + radius + L"; --ao-radius: " + radius + L"; }\n");
    if (skin.base == "willow_green")
    {
        css.append(L".containerParent { border-radius: " + radius + L"; }\n");
    }
    // Fluent 与微信横排把贴着外框四角的高亮角写死成外框圆角，这里用同一组选择器改成 R。
    if ((skin.base == "fluent" || skin.base == "wechat") && GetConfiguredCandidateWindowLayout() == "horizontal")
    {
        css.append(L".container > .pinyin + .row-wrapper > .cand { border-bottom-left-radius: " + radius +
                   L"; }\n"
                   L".container.preedit-hidden > .pinyin + .row-wrapper > .cand { border-top-left-radius: " +
                   radius +
                   L"; }\n"
                   L".container > .row-wrapper:is(:last-child, .last-visible) > .cand { "
                   L"border-bottom-right-radius: " +
                   radius +
                   L"; }\n"
                   L".container.preedit-hidden > .row-wrapper:is(:last-child, .last-visible) > .cand { "
                   L"border-top-right-radius: " +
                   radius + L"; }\n");
    }
}

std::wstring BuildExternalCandidateSkinCss(const CandidateSkinCatalog::Package &skin, const std::wstring &skinsRoot)
{
    // 卡片至少与装饰图同宽，装饰图因此总落在卡片宽度之内；D2D 端用同一规则撑开卡片。
    std::wstring css = L".container:not(:empty) { min-width: max(7em, var(--msime-skin-min-width, 0px), "
                       L"var(--msime-skin-decoration-width, 0px)); }\n";
    if (skin.decorationTopDip > 0.0)
    {
        // 装饰盒底边贴卡片顶边、不与卡片重叠，按 align 贴卡片左/中/右，图片在盒内 contain 居中。
        const wchar_t *horizontal = L"right: 0;";
        if (skin.decorationAlign == "left")
            horizontal = L"left: 0;";
        else if (skin.decorationAlign == "center")
            horizontal = L"left: 0; right: 0; margin-inline: auto;";
        css.append(L".containerParent { padding-top: var(--msime-skin-decoration-top, 0px); "
                   L"position: relative; box-sizing: border-box; }\n"
                   L".containerParent:not(:empty)::before { content: \"\"; position: absolute; "
                   L"z-index: 0; top: 0; ");
        css.append(horizontal);
        css.append(L" width: var(--msime-skin-decoration-width, 0px); "
                   L"height: var(--msime-skin-decoration-top, 0px); background: ");
        css.append(ManifestImageCssUrl(skinsRoot, skin.id, skin.decorationImage));
        css.append(L" center / contain no-repeat; pointer-events: none; }\n"
                   L".container { position: relative; z-index: 1; }\n");
    }
    if (skin.outerInsetsDip && (skin.outerInsetsDip->left > 0.0 || skin.outerInsetsDip->top > 0.0))
    {
        // MarginLeft / MarginTop 指向外留白信封的左上角，这里把卡片连同挂件相对定位回卡片原位。
        // 用相对定位而不是 padding：.containerParent 是挂件的定位容器，加 padding 会把挂件锚点挪到信封上。
        css.append(fmt::format(L".containerParent {{ position: relative; left: {}px; top: {}px; }}\n",
                               skin.outerInsetsDip->left, skin.outerInsetsDip->top));
    }
    AppendExternalCandidateCornerCss(css, skin);
    const bool light = ResolveConfiguredTheme(GetConfiguredThemeCand()) == "light";
    const CandidateSkinCatalog::CandidateColors &colors = light ? skin.light : skin.dark;
    AppendExternalCandidateColorCss(css, colors);
    AppendExternalCandidateGeometryCss(css, skin, light);
    if (!skin.backgroundImage.empty())
    {
        // 与 D2D 一致：背景图铺满整个卡片（含边框下方），边框画在它上面。不能用 ::before 垫图——
        // 卡片是 overflow 滚动容器，子元素只能画到内边距盒，半透明边框下会露出一圈底色。
        // 图层不能单独设 opacity，于是在图上再盖一层 (1 - opacity) 的底色，效果等同于把图按 opacity 叠在底色上。
        const wchar_t *size = L"cover";
        if (skin.backgroundFit == "contain")
            size = L"contain";
        else if (skin.backgroundFit == "stretch")
            size = L"100% 100%";
        const D2D1_COLOR_F surface = ResolveCandidateSkinPalette(skin.base, light, {}, &colors, skin.base).surface;
        const std::wstring veil =
            fmt::format(L"rgba({}, {}, {}, {:.3f})", static_cast<int>(std::lround(surface.r * 255.0f)),
                        static_cast<int>(std::lround(surface.g * 255.0f)),
                        static_cast<int>(std::lround(surface.b * 255.0f)), surface.a * (1.0 - skin.backgroundOpacity));
        css.append(L".container:not(:empty) { background-image: linear-gradient(" + veil + L", " + veil + L"), ");
        css.append(ManifestImageCssUrl(skinsRoot, skin.id, skin.backgroundImage));
        css.append(fmt::format(L"; background-size: auto, {}; background-position: center; "
                               L"background-repeat: no-repeat; background-origin: border-box; "
                               L"background-clip: border-box; }}\n",
                               size));
    }
    if (skin.borderImage)
    {
        // v2 专有九宫格贴图边框。stretch_dip 给出时它才是可拉伸区，语义比 slice_dip 直白：
        // 面板图里画死的装饰放在它之外就保持 1:1，不会被横向拉长。
        const auto &bi = *skin.borderImage;
        const std::vector<double> &sliceSrc = bi.stretchDip.empty() ? bi.sliceDip : bi.stretchDip;
        std::wstring sliceStr;
        for (size_t i = 0; i < sliceSrc.size(); ++i)
        {
            if (i > 0)
                sliceStr += L" ";
            sliceStr += fmt::format(L"{}", sliceSrc[i]);
        }
        if (bi.fill)
            sliceStr += L" fill";

        const std::vector<double> &widthSrc = bi.widthDip.empty() ? sliceSrc : bi.widthDip;
        std::wstring widthStr;
        for (size_t i = 0; i < widthSrc.size(); ++i)
        {
            if (i > 0)
                widthStr += L" ";
            widthStr += fmt::format(L"{}px", widthSrc[i]);
        }

        css.append(L".container:not(:empty) { border-style: solid; border-image-source: ");
        css.append(ManifestImageCssUrl(skinsRoot, skin.id, bi.source));
        // border-width 决定内容盒的内缩量，必须与 border-image-width 一致；只写 border-image-width
        // 时内容盒仍按基础皮肤的 border 宽度内缩，文字会压在贴图上。
        css.append(fmt::format(L"; border-width: {}; border-image-slice: {}; border-image-width: {}; "
                               L"border-image-repeat: stretch; "
                               L"background-color: transparent; border-color: transparent; border-radius: 0; }}\n",
                               widthStr, sliceStr, widthStr));
    }
    if (skin.contentInsetsDip)
    {
        // 四值展开成「上右下左」，1 值全边、2 值纵横、4 值四边，与 CSS 一致。
        const auto expand4 = [](const std::vector<double> &v, double out[4]) {
            if (v.size() == 1)
                out[0] = out[1] = out[2] = out[3] = v[0];
            else if (v.size() == 2)
            {
                out[0] = out[2] = v[0];
                out[1] = out[3] = v[1];
            }
            else
            {
                for (size_t i = 0; i < 4; ++i)
                    out[i] = i < v.size() ? v[i] : 0.0;
            }
        };
        double borderPx[4] = {0.0, 0.0, 0.0, 0.0};
        if (skin.borderImage)
        {
            const auto &bi = *skin.borderImage;
            const std::vector<double> &widthSrc =
                bi.widthDip.empty() ? (bi.stretchDip.empty() ? bi.sliceDip : bi.stretchDip) : bi.widthDip;
            expand4(widthSrc, borderPx);
        }
        // content_insets_dip 从面板外缘量起，而 CSS 的 padding 从边框内缘起算，这里把边框宽度扣掉。
        const auto &insets = *skin.contentInsetsDip;
        const double raw[4] = {insets.top, insets.right, insets.bottom, insets.left};
        double padding[4] = {0.0, 0.0, 0.0, 0.0};
        for (size_t i = 0; i < 4; ++i)
            padding[i] = (std::max)(0.0, raw[i] - borderPx[i]);
        css.append(fmt::format(L".container:not(:empty) {{ padding: {}px {}px {}px {}px !important; }}\n", padding[0],
                               padding[1], padding[2], padding[3]));
    }
    if (skin.lineGapDip)
    {
        // 预编辑行与候选行之间的行距。基础皮肤把它并进 .row 的上下边距，这里显式覆盖。
        css.append(fmt::format(L".row.pinyin {{ margin-bottom: {}px; }}\n", *skin.lineGapDip));
    }
    // 自由挂件：数量不限。候选框页面里没有能承载任意数量挂件的元素，所以按皮肤声明逐个生成
    // 一份 CSS 规则，由 InjectExternalCandidateSkin 注入对应数量的空 div，挂件层数不再受
    // ::before / ::after 两层限制。
    if (!skin.freeDecorations.empty())
    {
        css.append(L".containerParent { position: relative; }\n");
        css.append(L".msime-skin-decor { position: absolute; pointer-events: none; "
                   L"background-position: center; background-repeat: no-repeat; background-size: contain; }\n");
    }
    for (size_t idx = 0; idx < skin.freeDecorations.size(); ++idx)
    {
        const auto &dec = skin.freeDecorations[idx];
        // offset 语义统一：正数向右、向下，与 D2D 侧 CandidateSkinOverlay 一致。
        const double ox = dec.offsetXDip;
        const double oy = dec.offsetYDip;
        std::wstring posRule;
        if (dec.anchor == "top-left")
            posRule = fmt::format(L"left: {}px; top: {}px;", ox, oy);
        else if (dec.anchor == "top-right")
            posRule = fmt::format(L"right: {}px; top: {}px;", -ox, oy);
        else if (dec.anchor == "bottom-left")
            posRule = fmt::format(L"left: {}px; bottom: {}px;", ox, -oy);
        else if (dec.anchor == "bottom-right")
            posRule = fmt::format(L"right: {}px; bottom: {}px;", -ox, -oy);
        else if (dec.anchor == "left")
            posRule = fmt::format(L"left: {}px; top: 50%; transform: translateY(calc(-50% + {}px));", ox, oy);
        else if (dec.anchor == "right")
            posRule = fmt::format(L"right: {}px; top: 50%; transform: translateY(calc(-50% + {}px));", -ox, oy);
        else if (dec.anchor == "top")
            posRule = fmt::format(L"top: {}px; left: 50%; transform: translateX(calc(-50% + {}px));", oy, ox);
        else if (dec.anchor == "bottom")
            posRule = fmt::format(L"bottom: {}px; left: 50%; transform: translateX(calc(-50% + {}px));", -oy, ox);
        else
            posRule = fmt::format(L"left: 50%; top: 50%; "
                                  L"transform: translate(calc(-50% + {}px), calc(-50% + {}px));",
                                  ox, oy);

        css.append(fmt::format(L".msime-skin-decor[data-i=\"{}\"] {{ z-index: {}; {} width: {}px; height: {}px; "
                               L"background-image: ",
                               idx, dec.zIndex, posRule, dec.widthDip, dec.heightDip));
        css.append(ManifestImageCssUrl(skinsRoot, skin.id, dec.image));
        css.append(L"; }\n");
    }
    return css;
}

// 挂件层数的 CSS 已经按 data-i 生成，这里把对应数量的空 div 补进 .containerParent。
// 页面自己的脚本只换 .container 的内容，不动 .containerParent，所以这些 div 不会被打掉。
std::wstring BuildFreeDecorationLayerScript(const CandidateSkinCatalog::Package &skin)
{
    if (skin.freeDecorations.empty())
    {
        return L"";
    }
    std::wstring indices;
    for (size_t i = 0; i < skin.freeDecorations.size(); ++i)
    {
        if (i > 0)
            indices += L",";
        indices += std::to_wstring(i);
    }
    return L"<script id=\"external-candidate-skin-decor\">(function(){"
           L"function add(){var parent=document.getElementById('realContainerParent');if(!parent){return;}"
           L"[" +
           indices +
           L"].forEach(function(i){if(parent.querySelector('.msime-skin-decor[data-i=\"'+i+'\"]')){return;}"
           L"var node=document.createElement('div');node.className='msime-skin-decor';"
           L"node.setAttribute('data-i',String(i));parent.appendChild(node);});}"
           L"if(document.readyState==='loading'){document.addEventListener('DOMContentLoaded',add);}else{add();}"
           L"})();</script>";
}

bool InjectExternalCandidateSkin(std::wstring &html, const CandidateSkinCatalog::Package &skin,
                                 const std::wstring &skinsRoot)
{
    if (html.empty() || skinsRoot.empty())
    {
        return false;
    }
    const std::wstring vars =
        fmt::format(L"<style id=\"external-candidate-skin-vars\">:root{{--msime-skin-min-width:{}px;"
                    L"--msime-skin-decoration-top:{}px;--msime-skin-decoration-width:{}px;}}</style>",
                    skin.minWidthDip, skin.decorationTopDip, skin.decorationWidthDip);
    std::wstring css = BuildExternalCandidateSkinCss(skin, skinsRoot);
    NeutralizeEmbeddedStyleClosers(css);
    std::wstring generated;
    if (!css.empty())
    {
        generated = L"<style id=\"external-candidate-skin\">" + css + L"</style>";
    }
    const size_t headEnd = html.find(L"</head>");
    if (headEnd == std::wstring::npos)
    {
        return false;
    }
    html.insert(headEnd, vars + generated + BuildFreeDecorationLayerScript(skin));
    return true;
}

// 工具栏页面是按深浅主题各一张的，这里只生成当前主题的覆盖；D2D 端由 ApplyFloatingToolbarSkinOverrides 读同一份值。
std::wstring BuildExternalToolbarSkinCss(const CandidateSkinCatalog::Package &skin, bool light)
{
    std::wstring css;
    auto add = [&](const std::string &value, const wchar_t *selector, const wchar_t *property) {
        if (value.empty())
        {
            return;
        }
        css.append(selector);
        css.append(L" { ");
        css.append(property);
        css.append(L": ");
        css.append(string_to_wstring(value));
        css.append(L"; }\n");
    };
    const CandidateSkinCatalog::ToolbarColors &colors = light ? skin.toolbarLight : skin.toolbarDark;
    add(colors.background, L".status-bar", L"background-color");
    add(colors.border, L".status-bar", L"border-color");
    add(colors.handle, L".drag-handle", L"background");
    add(colors.divider, L".divider", L"background-color");
    add(colors.icon, L".icon", L"color");
    add(colors.hover, L".icon:hover", L"background-color");
    if (skin.toolbarCornerRadiusDip)
    {
        // 部分 base 页面把圆角直接写成 calc(Npx * var(--ftb-scale))，所以覆盖 border-radius 本身而不是 --ftb-radius。
        css.append(fmt::format(L".status-bar {{ border-radius: calc({}px * var(--ftb-scale)); }}\n",
                               *skin.toolbarCornerRadiusDip));
    }
    return css;
}

bool InjectExternalToolbarSkin(std::wstring &html, const CandidateSkinCatalog::Package &skin, bool light)
{
    std::wstring css = BuildExternalToolbarSkinCss(skin, light);
    const size_t headEnd = html.find(L"</head>");
    if (css.empty() || headEnd == std::wstring::npos)
    {
        return false;
    }
    NeutralizeEmbeddedStyleClosers(css);
    html.insert(headEnd, L"<style id=\"external-toolbar-skin\">" + css + L"</style>");
    return true;
}

void InjectCandidateDocumentSkin(std::wstring &html, const std::wstring &builtInCss, const std::string &skin,
                                 const std::string &base, const std::string &layout, const std::string &theme,
                                 bool pageArrows, bool fitContent)
{
    if (html.empty())
        return;
    const size_t htmlTagEnd = html.find(L'>', html.find(L"<html"));
    if (htmlTagEnd != std::wstring::npos)
    {
        // data-page-arrows 由候选页的共享样式读取，决定翻页箭头显示与否；
        // data-fit-content 由下面的规则和卡片的只增不减脚本读取。
        html.insert(htmlTagEnd,
                    fmt::format(L" data-candidate-skin=\"{}\" data-candidate-base=\"{}\" "
                                L"data-candidate-layout=\"{}\" data-candidate-theme=\"{}\" "
                                L"data-page-arrows=\"{}\" data-fit-content=\"{}\"",
                                string_to_wstring(skin), string_to_wstring(base), string_to_wstring(layout),
                                string_to_wstring(theme), pageArrows ? L"on" : L"off", fitContent ? L"on" : L"off"));
    }
    const size_t headEnd = html.find(L"</head>");
    if (headEnd != std::wstring::npos && !builtInCss.empty())
    {
        // fit_content 去掉各皮肤 .container 的 7em 下限，只留外部皮肤的 min_width_dip 与装饰图宽度
        // （内置皮肤这两个变量未定义，取 0）；选择器比外部皮肤的 .container:not(:empty) 更具体。
        html.insert(headEnd, L"<style id=\"built-in-candidate-skin\">" + builtInCss +
                                 L"\nhtml[data-fit-content=\"on\"] .container:not(:empty) { min-width: "
                                 L"max(var(--msime-skin-min-width, 0px), var(--msime-skin-decoration-width, 0px)); "
                                 L"}\n</style>");
    }
}
} // namespace

int PrepareHtmlForWnds()
{
    // 用户数据目录，默认 %LOCALAPPDATA%\metasequoiaime，安装时可以改到别的盘。
    std::wstring assetPath = CommonUtils::get_ime_data_path_w();

    //
    // 候选窗口
    //
    const bool isHorizontal = GetConfiguredCandidateWindowLayout() == "horizontal";
    const bool candLight = ResolveConfiguredTheme(GetConfiguredThemeCand()) == "light";
    const std::string candidateSkin = GetConfiguredCandidateSkin();
    const std::string candidateLayout = isHorizontal ? "horizontal" : "vertical";
    const std::string candidateTheme = candLight ? "light" : "dark";
    activeExternalCandidateSkin.reset();
    std::string baseCandidateSkin = candidateSkin;
    if (!CandidateSkinCatalog::IsBuiltIn(candidateSkin))
    {
        const std::wstring skinsRoot = assetPath + L"\\skins";
        // LoadSupported 只在皮肤声明支持当前布局与主题时才返回，两个后端走同一条规则。
        activeExternalCandidateSkin =
            CandidateSkinCatalog::LoadSupported(skinsRoot, candidateSkin, candidateLayout, candidateTheme);
        baseCandidateSkin = activeExternalCandidateSkin ? activeExternalCandidateSkin->base : "fluent";
    }
    std::wstring htmlCandWnd;
    std::wstring bodyHtmlCandWnd;
    std::wstring measureHtmlCandWnd;
    if (isHorizontal)
    {
        htmlCandWnd = L"/html/webview2/candwnd/horizontal_candidate_window.html";
        // Body/measure fragments are theme-agnostic markup; keep the existing dark assets.
        bodyHtmlCandWnd = L"/html/webview2/candwnd/body/horizontal_candidate_window_dark.html";
        measureHtmlCandWnd = L"/html/webview2/candwnd/body/horizontal_candidate_window_dark_measure.html";
    }
    else
    {
        htmlCandWnd = L"/html/webview2/candwnd/vertical_candidate_window.html";
        bodyHtmlCandWnd = L"/html/webview2/candwnd/body/vertical_candidate_window_dark.html";
        measureHtmlCandWnd = L"/html/webview2/candwnd/body/vertical_candidate_window_dark_measure.html";
    }

    std::wstring entireHtmlPathCandWnd = assetPath + htmlCandWnd;
    ::HTMLStringCandWnd = ReadHtmlFile(entireHtmlPathCandWnd);
    const std::wstring candidateStyleName =
        fmt::format(L"{}_{}.css", string_to_wstring(candidateLayout), string_to_wstring(candidateTheme));
    const std::wstring candidateStylePath =
        fmt::format(L"/html/webview2/candwnd/skins/{}/{}", string_to_wstring(baseCandidateSkin), candidateStyleName);
    std::wstring builtInCandidateCss = ReadHtmlFile(assetPath + candidateStylePath);
    if (builtInCandidateCss.empty())
    {
        builtInCandidateCss = ReadHtmlFile(assetPath + L"/html/webview2/candwnd/skins/fluent/" + candidateStyleName);
    }
    if (builtInCandidateCss.empty() && candLight)
    {
        builtInCandidateCss = ReadHtmlFile(assetPath + L"/html/webview2/candwnd/skins/fluent/" +
                                           string_to_wstring(candidateLayout) + L"_dark.css");
    }
    const bool pageArrows =
        CandidateSkinCatalog::ResolvePageArrows(std::filesystem::path(assetPath + L"\\skins"), candidateSkin,
                                                activeExternalCandidateSkin ? &*activeExternalCandidateSkin : nullptr);
    const bool fitContent =
        GetConfiguredCandidateWindowFitContent() ||
        CandidateSkinCatalog::ResolveFitContent(std::filesystem::path(assetPath + L"\\skins"), candidateSkin,
                                                activeExternalCandidateSkin ? &*activeExternalCandidateSkin : nullptr);
    InjectCandidateDocumentSkin(::HTMLStringCandWnd, builtInCandidateCss, candidateSkin, baseCandidateSkin,
                                candidateLayout, candidateTheme, pageArrows, fitContent);
    if (activeExternalCandidateSkin)
    {
        const std::wstring skinsRoot = assetPath + L"\\skins";
        if (!InjectExternalCandidateSkin(::HTMLStringCandWnd, *activeExternalCandidateSkin, skinsRoot))
        {
            // Without the stylesheet, native decoration insets would clip the card.
            activeExternalCandidateSkin.reset();
        }
    }
    std::wstring bodyHtmlPathCandWnd = assetPath + bodyHtmlCandWnd;
    ::BodyStringCandWnd = ReadHtmlFile(bodyHtmlPathCandWnd);
    std::wstring measureHtmlPathCandWnd = assetPath + measureHtmlCandWnd;
    ::MeasureStringCandWnd = ReadHtmlFile(measureHtmlPathCandWnd);
    (void)0;

    //
    // 托盘语言区菜单窗口
    //
    const bool menuLight = ResolveConfiguredTheme(GetConfiguredThemeMenu()) == "light";
    std::wstring htmlMenuWnd =
        menuLight ? L"/html/webview2/menu/default_light.html" : L"/html/webview2/menu/default.html";
    std::wstring entireHtmlPathMenuWnd = assetPath + htmlMenuWnd;
    ::HTMLStringMenuWnd =
        ReadHtmlFileWithFallback(entireHtmlPathMenuWnd, assetPath + L"/html/webview2/menu/default.html");

    //
    // floating toolbar 窗口
    //
    const bool ftbLight = ResolveConfiguredTheme(GetConfiguredThemeFtb()) == "light";
    std::wstring htmlFtbWnd;
    if (baseCandidateSkin == "wechat")
    {
        htmlFtbWnd = ftbLight ? L"/html/webview2/ftb/wechat_light.html" : L"/html/webview2/ftb/wechat.html";
    }
    else if (baseCandidateSkin == "graphite")
    {
        htmlFtbWnd = ftbLight ? L"/html/webview2/ftb/graphite_light.html" : L"/html/webview2/ftb/graphite.html";
    }
    else if (baseCandidateSkin == "willow_green")
    {
        htmlFtbWnd = ftbLight ? L"/html/webview2/ftb/willow_green_light.html" : L"/html/webview2/ftb/willow_green.html";
    }
    else if (baseCandidateSkin == "autumn_osmanthus")
    {
        htmlFtbWnd =
            ftbLight ? L"/html/webview2/ftb/autumn_osmanthus_light.html" : L"/html/webview2/ftb/autumn_osmanthus.html";
    }
    else
    {
        htmlFtbWnd = ftbLight ? L"/html/webview2/ftb/default_light.html" : L"/html/webview2/ftb/default.html";
    }
    std::wstring entireHtmlPathFtbWnd = assetPath + htmlFtbWnd;
    ::HTMLStringFtbWnd = ReadHtmlFileWithFallback(entireHtmlPathFtbWnd, assetPath + L"/html/webview2/ftb/default.html");
    if (baseCandidateSkin == "microsoft")
    {
        // 微软皮肤沿用默认工具栏页面，只把拖拽条换成候选框选中条的紫色；与 ResolveFloatingToolbarSkin 同值。
        // 插在外部皮肤的覆盖之前，外部皮肤写了 handle 时仍以它为准。
        const size_t headEnd = ::HTMLStringFtbWnd.find(L"</head>");
        if (headEnd != std::wstring::npos)
            ::HTMLStringFtbWnd.insert(
                headEnd, L"<style id=\"built-in-toolbar-skin\">.drag-handle { background: #E183D9; }</style>");
    }
    if (activeExternalCandidateSkin && !::HTMLStringFtbWnd.empty())
    {
        const size_t htmlTag = ::HTMLStringFtbWnd.find(L"<html");
        const size_t htmlTagEnd =
            htmlTag == std::wstring::npos ? std::wstring::npos : ::HTMLStringFtbWnd.find(L'>', htmlTag);
        if (htmlTagEnd != std::wstring::npos)
        {
            ::HTMLStringFtbWnd.insert(htmlTagEnd,
                                      fmt::format(L" data-candidate-skin=\"{}\" data-candidate-base=\"{}\" "
                                                  L"data-candidate-theme=\"{}\"",
                                                  string_to_wstring(candidateSkin),
                                                  string_to_wstring(baseCandidateSkin), ftbLight ? L"light" : L"dark"));
        }
        InjectExternalToolbarSkin(::HTMLStringFtbWnd, *activeExternalCandidateSkin, ftbLight);
    }
    // The small windows navigate from strings. Inline the pinned local runtime
    // before navigation instead of blocking first paint on two virtual-host loads.
    // Immutable for this process, like the executable's protocol contract.
    static const std::wstring protocolSchema = ReadHtmlFile(assetPath + L"/html/webview2/shared/schema.js");
    static const std::wstring protocolRuntime = ReadHtmlFile(assetPath + L"/html/webview2/shared/runtime.js");
    InlineWebViewProtocolScripts(::HTMLStringCandWnd, protocolSchema, protocolRuntime);
    InlineWebViewProtocolScripts(::HTMLStringMenuWnd, protocolSchema, protocolRuntime);
    InlineWebViewProtocolScripts(::HTMLStringFtbWnd, protocolSchema, protocolRuntime);
    preparedCandidateSkin = candidateSkin;

    return 0;
}

bool ApplyConfiguredCandidateWindowLayout()
{
    // 外部皮肤可能只声明了部分布局，换布局后皮肤是否生效会变，工具栏跟着候选窗皮肤走，要一起刷新。
    if (FloatingToolbarPresenter::Instance().IsBound())
    {
        FloatingToolbarPresenter::Instance().ApplyTheme();
    }
    const std::wstring previousFtbHtml = HTMLStringFtbWnd;
    PrepareHtmlForWnds();
    if (webviewFtbWnd && !HTMLStringFtbWnd.empty() && HTMLStringFtbWnd != previousFtbHtml)
    {
        ClearFloatingToolbarNavigationState();
        if (SUCCEEDED(webviewFtbWnd->NavigateToString(HTMLStringFtbWnd.c_str())))
        {
            loadedFloatingToolbarSkin = preparedCandidateSkin;
        }
    }
    if (CandidatePresenter::Instance().IsBound() && !webviewCandWnd)
    {
        return true;
    }
    if (!webviewCandWnd || HTMLStringCandWnd.empty())
    {
        return false;
    }
    const bool ok = SUCCEEDED(webviewCandWnd->NavigateToString(HTMLStringCandWnd.c_str()));
    if (ok)
    {
        loadedCandidateSkin = preparedCandidateSkin;
    }
    return ok;
}

bool ApplyConfiguredUiThemes()
{
    if (FloatingToolbarPresenter::Instance().IsBound())
    {
        FloatingToolbarPresenter::Instance().ApplyTheme();
    }
    const std::string candidateSkin = GetConfiguredCandidateSkin();
    PrepareHtmlForWnds();
    bool ok = true;
    if (webviewCandWnd && !HTMLStringCandWnd.empty())
    {
        const bool candidateOk = SUCCEEDED(webviewCandWnd->NavigateToString(HTMLStringCandWnd.c_str()));
        if (candidateOk)
        {
            loadedCandidateSkin = candidateSkin;
        }
        ok = candidateOk && ok;
    }
    if (webviewFtbWnd && !HTMLStringFtbWnd.empty())
    {
        ClearFloatingToolbarNavigationState();
        const bool floatingToolbarOk = SUCCEEDED(webviewFtbWnd->NavigateToString(HTMLStringFtbWnd.c_str()));
        if (floatingToolbarOk)
        {
            loadedFloatingToolbarSkin = candidateSkin;
        }
        ok = floatingToolbarOk && ok;
    }
    if (webviewMenuWnd && !HTMLStringMenuWnd.empty())
    {
        ok = SUCCEEDED(webviewMenuWnd->NavigateToString(HTMLStringMenuWnd.c_str())) && ok;
    }
    return ok;
}

bool ApplyConfiguredCandidateSkinIfChanged()
{
    const std::string &candidateSkin = GetConfiguredCandidateSkin();
    const bool candidateCurrent = !webviewCandWnd || loadedCandidateSkin == candidateSkin;
    const bool floatingToolbarCurrent = !webviewFtbWnd || loadedFloatingToolbarSkin == candidateSkin;
    if (candidateCurrent && floatingToolbarCurrent)
    {
        return true;
    }
    return ApplyConfiguredUiThemes();
}

uint64_t GetCandidateSkinReloadRevision()
{
    return candidateSkinReloadRevision;
}

bool ForceReloadConfiguredCandidateSkin()
{
    ++candidateSkinReloadRevision;
    loadedCandidateSkin.clear();
    loadedFloatingToolbarSkin.clear();
    const bool cloakCandidate = ::is_global_wnd_cand_shown && ::global_hwnd && IsWindow(::global_hwnd) &&
                                !CandidatePresenter::Instance().IsBound();
    if (cloakCandidate)
        SetCandidateHostCloaked(true);
    const bool ok = ApplyConfiguredUiThemes();
    if (!ok && cloakCandidate)
        SetCandidateHostCloaked(false);
    return ok;
}

bool ApplyConfiguredCandidateAppearance()
{
    if (CandidatePresenter::Instance().IsBound() && !webviewCandWnd)
    {
        return true;
    }
    if (!webviewCandWnd)
    {
        return false;
    }

    nlohmann::json cfg = {{"font", ResolveSystemFontFamilyForCss(GetConfiguredCandidateFont())},
                          {"english_font", ResolveSystemFontFamilyForCss(GetConfiguredCandidateEnglishFont())},
                          {"fallback_fonts", GetConfiguredCandidateFallbackFontFamilies()},
                          {"font_size", GetConfiguredCandidateFontSize()},
                          {"preedit_font_size", GetConfiguredCandidateWindowPreeditFontSize()},
                          {"cand_text_color", GetConfiguredCandidateTextColor()}};
    std::string family;
    auto appendFont = [&](const std::string &font) {
        // JSON quoting also escapes CSS quotes/backslashes; font names exclude control characters.
        if (!family.empty())
            family += ", ";
        family += nlohmann::json(font).dump(-1, ' ', false);
    };
    // 皮肤的 font_family 排在最前，用户配置的字体整体退为回退，与 D2D 端同序。
    if (activeExternalCandidateSkin && !activeExternalCandidateSkin->fontFamily.empty())
        appendFont(ResolveSystemFontFamilyForCss(activeExternalCandidateSkin->fontFamily));
    appendFont(ResolveSystemFontFamilyForCss(GetConfiguredCandidateEnglishFont()));
    for (const auto &font : GetConfiguredCandidateFallbackFontFamilies())
        appendFont(font);
    cfg["font_family"] = family + ", sans-serif";
    const std::wstring script =
        L"(function(c){"
        L"const root=document.documentElement;"
        L"const family=c.font_family;"
        L"root.style.setProperty('--cand-font-family', family);"
        L"root.style.setProperty('--cand-font-size', String(c.font_size||16)+'px');"
        L"root.style.setProperty('--preedit-font-size', String(c.preedit_font_size||c.font_size||16)+'px');"
        L"const color=(c.cand_text_color||'auto');"
        L"if(color&&color!=='auto'){"
        L"root.style.setProperty('--cand-text', color);"
        L"root.style.setProperty('--cand-num', color.length===7?color+'9d':color);"
        // 外部皮肤的 candidate_text / preedit_text 写成 var(--msime-user-text, 皮肤色)，设置页的文字色照样压过它们。
        L"root.style.setProperty('--msime-user-text', color);"
        L"}else{"
        L"root.style.removeProperty('--cand-text');"
        L"root.style.removeProperty('--cand-num');"
        L"root.style.removeProperty('--msime-user-text');"
        L"}"
        // Drop any stale nowrap fast-layout sheet so the skin's wrap-at-max-width
        // rules apply; forcing a single line makes the card overflow the cap and
        // get clipped by .container{overflow-x:hidden}.
        L"document.getElementById('msime-fast-layout')?.remove();"
        L"})(" +
        string_to_wstring(cfg.dump()) + L");";
    return SUCCEEDED(webviewCandWnd->ExecuteScript(script.c_str(), nullptr));
}

bool ApplyConfiguredFloatingToolbarAppearance()
{
    return ApplyConfiguredFloatingToolbarAppearance(nullptr);
}

bool ApplyConfiguredFloatingToolbarAppearance(std::function<void()> onComplete)
{
    if (FloatingToolbarPresenter::Instance().IsBound())
    {
        FloatingToolbarPresenter::Instance().ApplyAppearance();
        if (onComplete)
        {
            onComplete();
        }
        return true;
    }
    if (!webviewFtbWnd)
    {
        if (onComplete)
        {
            onComplete();
        }
        return false;
    }

    nlohmann::json cfg = {{"scale", GetConfiguredFloatingToolbarScale()},
                          {"font_size", GetConfiguredFloatingToolbarFontSize()},
                          {"shadow", GetConfiguredFloatingToolbarShadow()}};
    // An inline box-shadow beats both the page's own and the injected skin rules.
    const std::wstring script = L"(function(c){"
                                L"const root=document.documentElement;"
                                L"const scale=(typeof c.scale==='number'&&c.scale>0)?c.scale:1;"
                                L"const icon=(typeof c.font_size==='number'&&c.font_size>0)?c.font_size:24;"
                                L"root.style.setProperty('--ftb-scale', String(scale));"
                                L"root.style.setProperty('--ftb-icon-size', String(icon)+'px');"
                                L"const bar=document.querySelector('.status-bar');"
                                L"if(bar)bar.style.boxShadow=c.shadow===false?'none':'';"
                                L"return true;"
                                L"})(" +
                                string_to_wstring(cfg.dump()) + L");";
    if (!onComplete)
    {
        return SUCCEEDED(webviewFtbWnd->ExecuteScript(script.c_str(), nullptr));
    }
    return SUCCEEDED(webviewFtbWnd->ExecuteScript(
        script.c_str(), Callback<ICoreWebView2ExecuteScriptCompletedHandler>([onComplete](HRESULT, LPCWSTR) -> HRESULT {
                            onComplete();
                            return S_OK;
                        }).Get()));
}
