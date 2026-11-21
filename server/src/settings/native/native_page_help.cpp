#include "settings/native/native_page_help_widgets.h"

// partials/help-settings.html + modules/help-settings.ts. The default-IME, input method icon,
// game-compat and proxy blocks that used to follow the document now live on the advanced page
// (native_page_advanced.cpp).
namespace native_settings
{
namespace
{
TextStyle HelpParagraphStyle()
{
    // .help-simple-document p: 13px, line-height 1.8, --text-secondary
    return DocStyle(13.0f, DWRITE_FONT_WEIGHT_NORMAL, 1.8f, true);
}

} // namespace

VisualPtr BuildHelpPage(PageContext &context)
{
    PageBuilder page(L"帮助");
    page.SetTitleRole(TextRole::Secondary);
    SettingsModel *model = &context.model;
    const auto openUrl = [model](const std::string &url) { model->Post("openExternalUrl", Json(url)); };
    const auto secondary = []() { return CurrentPalette().textSecondary; };

    // .help-document.help-simple-document: padding 24px 28px 26px.
    {
        auto doc = std::make_shared<Column>(0.0f);
        const auto paragraph = [&](std::vector<RichParagraph::Run> runs, float marginTop) {
            auto p = std::make_shared<RichParagraph>(std::move(runs), HelpParagraphStyle(), secondary, openUrl);
            p->SetMaxWidth(780.0f);
            p->SetHorizontalAlignment(msimeui::HorizontalAlignment::Leading);
            p->SetMargin({0.0f, marginTop, 0.0f, 0.0f});
            doc->Add(p);
        };
        paragraph({{L"水杉输入法是一款 Windows 平台的中文输入法。目前支持 Windows11/Windows10 平台。", {}}}, 0.0f);
        paragraph({{L"完整的使用说明见 ", {}},
                   {L"Wiki", "https://github.com/metasequoiaime/msime-windows/wiki"},
                   {L"，遇到问题可以先查看 ", {}},
                   {L"常见问题（FAQ）", "https://github.com/metasequoiaime/msime-windows/wiki/FAQ"},
                   {L"。", {}}},
                  0.0f);
        const auto section = [&](const wchar_t *title, std::vector<const wchar_t *> texts) {
            auto heading = MakeTitle(title);
            heading->SetMargin({0.0f, 28.0f, 0.0f, 0.0f});
            doc->Add(heading);
            for (const wchar_t *text : texts)
                paragraph({{text, {}}}, 8.0f);
        };
        section(L"快速上手", {L"安装输入法后，可以使用 Win + Space 快捷键切换到水杉输入法。默认是全拼输入法。"});
        section(
            L"基本功能",
            {L"支持全拼、双拼和五笔。可以在设置窗口下的输入功能分区进行切换。全拼和双拼均支持辅助码。辅助码方案目前"
             L"支持自然码辅助码、蓝天小雨点、首右2."
             L"0、首右plus、小鹤和拼音加加，也可以把自己的辅助码文件放进数据目录下的 helpcodes\\custom "
             L"文件夹导入。可以在设置的辅助码分区进行辅助码的开关和切换。打字过程中，全拼使用辅助码需要用大写的字母"
             L"唤起单码和双码辅助。双拼在开启辅助码的状态下默认使用单码辅助，第二个码是大写字母的情况下，则会使用完"
             L"整双码进行筛选。",
             L"语音识别和 AI 联想需要自行填入 API 和 token。内测期间，将默认使用开发者个人的 api 和 "
             L"token，仅供测试使用。云联想目前支持谷歌的云接口，请用户注意网络问题，需要时可在「高级」页设置网络代理。",
             L"更多功能欢迎自由探索~"});
        auto card = std::make_shared<PaddedSection>(doc, Thickness{28.0f, 24.0f, 28.0f, 26.0f});
        // .default-ime-section { margin-top: 24px } collapses with the 18px below.
        page.AddRaw(card, 24.0f);
    }

    return page.Build();
}
} // namespace native_settings
