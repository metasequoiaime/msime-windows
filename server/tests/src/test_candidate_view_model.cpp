#include "window/candidate_view_model.h"
#include "tests/includes/test_framework.h"

TEST_CASE(candidate_view_escapes_text_and_translation_without_turning_them_into_markup)
{
    CandidateViewItem item;
    item.text = "<&,\"";
    item.annotation = " AB";
    item.badge = " cloud";
    item.translation = "<gloss>,";
    item.fixed_position = true;
    REQUIRE_EQ(CandidateViewHtml(item), std::string("<span style=\"color:#379AD3\">&lt;&amp;,&quot; AB cloud</span>"
                                                    "<span class=\"cand-translation\">&lt;gloss&gt;,</span>"));
    REQUIRE_EQ(item.text, std::string("<&,\""));
}

TEST_CASE(candidate_view_keeps_commas_and_private_use_characters)
{
    CandidateViewItem item;
    item.text = "a,\xEF\x80\x80";
    item.translation = "b,\xEF\x80\x80";
    REQUIRE_EQ(CandidateViewHtml(item),
               std::string("a,\xEF\x80\x80<span class=\"cand-translation\">b,\xEF\x80\x80</span>"));
}

// 词性与分隔符各包一层 span，皮肤靠 .cand-pos / .cand-sep 给它们单独配色；分段里的文本同样要转义。
TEST_CASE(candidate_view_wraps_part_of_speech_and_separators_in_translation)
{
    CandidateViewItem item;
    item.text = "笑";
    item.translation = "adj. <spreading>; n. laugh";
    REQUIRE_EQ(CandidateViewHtml(item), std::string("笑<span class=\"cand-translation\">"
                                                    "<span class=\"cand-pos\">adj.</span> &lt;spreading&gt;"
                                                    "<span class=\"cand-sep\">;</span> "
                                                    "<span class=\"cand-pos\">n.</span> laugh</span>"));
}

TEST_CASE(candidate_view_pos_toggle_updates_display_and_refresh_signature)
{
    const std::string raw = "adj. <spreading>; n. laugh";
    CandidateViewItem item;
    item.text = "笑";
    item.translation = FanyImeIpc::FormatTranslationGlossForDisplay(raw, true);
    const auto shown_html = CandidateViewHtml(item);
    const auto shown_signature = CandidatePageSignature({item});
    const CandidateFramePageState page{2, 1, true, true, false, 0, 0, std::wstring{}};
    const auto shown_frame = CandidateFrameSignature(L"xiao", {item}, 10, 20, page);

    item.translation = FanyImeIpc::FormatTranslationGlossForDisplay(raw, false);
    REQUIRE_EQ(item.translation, std::string("<spreading>; laugh"));
    REQUIRE_EQ(CandidateViewHtml(item), std::string("笑<span class=\"cand-translation\">&lt;spreading&gt;"
                                                    "<span class=\"cand-sep\">;</span> laugh</span>"));
    REQUIRE(shown_signature != CandidatePageSignature({item}));
    REQUIRE(shown_frame != CandidateFrameSignature(L"xiao", {item}, 10, 20, page));

    item.translation = FanyImeIpc::FormatTranslationGlossForDisplay(raw, true);
    REQUIRE_EQ(CandidateViewHtml(item), shown_html);
    REQUIRE_EQ(CandidatePageSignature({item}), shown_signature);
    REQUIRE_EQ(CandidateFrameSignature(L"xiao", {item}, 10, 20, page), shown_frame);
}

TEST_CASE(candidate_page_signature_distinguishes_comma_split_between_items)
{
    CandidateViewItem ab, c, a, bc;
    ab.text = "a,b";
    c.text = "c";
    a.text = "a";
    bc.text = "b,c";
    REQUIRE(CandidatePageSignature({ab, c}) != CandidatePageSignature({a, bc}));
    REQUIRE_EQ(CandidatePageSignature({ab, c}), CandidatePageSignature({ab, c}));
}

TEST_CASE(candidate_frame_signature_includes_page_state)
{
    CandidateViewItem item;
    item.text = "candidate";
    const std::vector<CandidateViewItem> views{item};
    const CandidateFramePageState page{0, 1, false, true, false, 0, 0, std::wstring{}};
    const std::wstring original = CandidateFrameSignature(L"preedit", views, 10, 20, page);

    REQUIRE_EQ(original, CandidateFrameSignature(L"preedit", views, 10, 20, page));
    CandidateFramePageState changed = page;
    changed.page_index = 2;
    REQUIRE(original != CandidateFrameSignature(L"preedit", views, 10, 20, changed));
    changed = page;
    changed.has_previous_page = true;
    REQUIRE(original != CandidateFrameSignature(L"preedit", views, 10, 20, changed));
    changed = page;
    changed.has_next_page = false;
    REQUIRE(original != CandidateFrameSignature(L"preedit", views, 10, 20, changed));
}

TEST_CASE(candidate_frame_signature_includes_expansion_panel_state)
{
    CandidateViewItem item;
    item.text = "candidate";
    const std::vector<CandidateViewItem> views{item};
    const CandidateFramePageState page{0, 1, false, true, false, 0, 0, std::wstring{}};
    const std::wstring original = CandidateFrameSignature(L"preedit", views, 10, 20, page);

    // 面板内 ↑↓ 只改 expansion_panel_selected：必须改变签名，否则快速移动会被 250ms 去抖吞掉，
    // 面板高亮停在上一项而服务端真实选中已到下一项（空格上屏下一项）。
    CandidateFramePageState changed = page;
    changed.expansion_panel_active = true;
    changed.expansion_panel_signature = L"1:a2:hi";
    const std::wstring with_panel = CandidateFrameSignature(L"preedit", views, 10, 20, changed);
    REQUIRE(original != with_panel);

    changed.expansion_panel_selected = 1;
    REQUIRE(with_panel != CandidateFrameSignature(L"preedit", views, 10, 20, changed));
    changed.expansion_panel_selected = 2;
    REQUIRE(with_panel != CandidateFrameSignature(L"preedit", views, 10, 20, changed));

    changed = page;
    changed.expansion_panel_active = true;
    changed.expansion_panel_anchor = 1;
    changed.expansion_panel_signature = L"1:a2:hi";
    REQUIRE(with_panel != CandidateFrameSignature(L"preedit", views, 10, 20, changed));

    // 条数不变、文本变（译文第二波回填、词性显示切换）：必须改变签名，否则面板停在旧文本。
    changed = page;
    changed.expansion_panel_active = true;
    changed.expansion_panel_signature = L"1:a2:yo";
    REQUIRE(with_panel != CandidateFrameSignature(L"preedit", views, 10, 20, changed));

    // 条目数变（改「每类最多显示」）：文本签名自然不同。
    changed = page;
    changed.expansion_panel_active = true;
    changed.expansion_panel_signature = L"1:a1:b2:hi";
    REQUIRE(with_panel != CandidateFrameSignature(L"preedit", views, 10, 20, changed));
}

TEST_CASE(candidate_expansion_panel_signature_length_prefixes_fields)
{
    struct PanelItem
    {
        std::wstring tag;
        std::wstring display;
    };
    const std::vector<PanelItem> ab_c{{L"", L"a,b"}, {L"", L"c"}};
    const std::vector<PanelItem> a_bc{{L"", L"a"}, {L"", L"b,c"}};
    REQUIRE(CandidateExpansionPanelSignature(ab_c) != CandidateExpansionPanelSignature(a_bc));
    REQUIRE_EQ(CandidateExpansionPanelSignature(ab_c), CandidateExpansionPanelSignature(ab_c));

    // tag 与 display 的边界也不能串位：tag="a2:b"+display="" 与 tag="a"+display="2:b" 必须不同。
    const std::vector<PanelItem> boundary_a{{L"a2:b", L""}};
    const std::vector<PanelItem> boundary_b{{L"a", L"2:b"}};
    REQUIRE(CandidateExpansionPanelSignature(boundary_a) != CandidateExpansionPanelSignature(boundary_b));
}

TEST_CASE(candidate_frame_signature_keeps_preedit_selection_and_caret)
{
    CandidateViewItem first, second;
    first.text = "first";
    second.text = "second";
    const std::vector<CandidateViewItem> views{first, second};
    const CandidateFramePageState page{0, 1, false, true, false, 0, 0, std::wstring{}};
    const std::wstring original = CandidateFrameSignature(L"preedit", views, 10, 20, page);

    CandidateFramePageState changed = page;
    changed.selected_index = 1;
    REQUIRE(original != CandidateFrameSignature(L"preedit", views, 10, 20, changed));
    REQUIRE(original != CandidateFrameSignature(L"edited", views, 10, 20, page));
    REQUIRE(original != CandidateFrameSignature(L"preedit", views, 11, 20, page));
    REQUIRE(original != CandidateFrameSignature(L"preedit", views, 10, 21, page));
}

TEST_CASE(candidate_frame_signature_keeps_length_prefixed_candidate_identity)
{
    CandidateViewItem ab, c, a, bc;
    ab.text = "a,b";
    c.text = "c";
    a.text = "a";
    bc.text = "b,c";

    const CandidateFramePageState page{0, 0, false, true, false, 0, 0, std::wstring{}};
    REQUIRE(CandidateFrameSignature(L"", {ab, c}, 10, 20, page) != CandidateFrameSignature(L"", {a, bc}, 10, 20, page));
}

TEST_CASE(fixed_position_candidates_get_pin_badge_visible_in_both_backends)
{
    CandidateViewItem item;
    item.text = "你好";
    item.badge = " cloud";
    item.fixed_position = true;
    ApplyFixedPositionBadge(item, true, "paperclip");
    // 追加而非覆盖：徽标并存时顺序稳定；候选串里固定词条整体落在蓝色 span 内。
    REQUIRE_EQ(item.badge, std::string(" cloud \xF0\x9F\x93\x8E"));
    REQUIRE_EQ(CandidateViewHtml(item),
               std::string("<span style=\"color:#379AD3\">你好 cloud \xF0\x9F\x93\x8E</span>"));

    // 样式枚举逐个映射，存储的是名字而不是字面 emoji
    CandidateViewItem pinned;
    pinned.text = "钉";
    pinned.fixed_position = true;
    ApplyFixedPositionBadge(pinned, true, "pushpin");
    REQUIRE_EQ(pinned.badge, std::string(" \xF0\x9F\x93\x8C"));
    CandidateViewItem dotted;
    dotted.text = "点";
    dotted.fixed_position = true;
    ApplyFixedPositionBadge(dotted, true, "dot");
    REQUIRE_EQ(dotted.badge, std::string(" \xC2\xB7"));

    // 开关关闭：不加徽标，但整条仍走蓝色 span（固定状态本身还有颜色提示）
    CandidateViewItem hidden;
    hidden.text = "你好";
    hidden.fixed_position = true;
    ApplyFixedPositionBadge(hidden, false, "paperclip");
    REQUIRE(hidden.badge.empty());
    REQUIRE_EQ(CandidateViewHtml(hidden), std::string("<span style=\"color:#379AD3\">你好</span>"));

    // 未列入白名单的样式不加徽标，手改 TOML 写错也不会渲染出畸形标记
    CandidateViewItem bogus;
    bogus.text = "你好";
    bogus.fixed_position = true;
    ApplyFixedPositionBadge(bogus, true, "smiley");
    REQUIRE(bogus.badge.empty());

    // 未固定的词条不加徽标。
    CandidateViewItem plain;
    plain.text = "世界";
    ApplyFixedPositionBadge(plain, true, "paperclip");
    REQUIRE(plain.badge.empty());
    REQUIRE_EQ(CandidateViewHtml(plain), std::string("世界"));
}

TEST_CASE(ai_suggestion_badge_switches_between_robot_and_whale)
{
    // 出厂默认是 🤖：与改动前的硬编码值逐字节一致，升级不改变默认观感。
    REQUIRE_EQ(AiSuggestionBadge("robot"), std::string(" \xF0\x9F\xA4\x96"));
    REQUIRE_EQ(AiSuggestionBadge("whale"), std::string(" \xF0\x9F\x90\xB3"));

    // 映射表同时是配置层白名单：已知样式有各自的字形，未知样式没有。
    REQUIRE(AiSuggestionBadgeGlyph("robot") != nullptr);
    REQUIRE(AiSuggestionBadgeGlyph("whale") != nullptr);
    REQUIRE(AiSuggestionBadgeGlyph("") == nullptr);
    REQUIRE(AiSuggestionBadgeGlyph("smiley") == nullptr);

    // 未列入白名单的样式按出厂默认处理，手改 TOML 写错也不会让 AI 候选失去来源标识。
    REQUIRE_EQ(AiSuggestionBadge(""), AiSuggestionBadge("robot"));
    REQUIRE_EQ(AiSuggestionBadge("smiley"), AiSuggestionBadge("robot"));

    // 徽标经既有 badge 通道渲染进词条文本，两种样式在两个后端都可见。
    CandidateViewItem whale;
    whale.text = "联想";
    whale.badge = AiSuggestionBadge("whale");
    REQUIRE_EQ(whale.badge, std::string(" \xF0\x9F\x90\xB3"));
    REQUIRE_EQ(CandidateViewHtml(whale), std::string("联想 \xF0\x9F\x90\xB3"));

    CandidateViewItem robot;
    robot.text = "联想";
    robot.badge = AiSuggestionBadge("robot");
    REQUIRE_EQ(robot.badge, std::string(" \xF0\x9F\xA4\x96"));
    REQUIRE_EQ(CandidateViewHtml(robot), std::string("联想 \xF0\x9F\xA4\x96"));

    // AI 徽标是覆盖而非追加：同一词条既固定又是 AI 候选时，两个徽标并存且各占一段。
    CandidateViewItem both;
    both.text = "联想";
    both.fixed_position = true;
    both.badge = AiSuggestionBadge("whale");
    ApplyFixedPositionBadge(both, true, "paperclip");
    REQUIRE_EQ(both.badge, std::string(" \xF0\x9F\x90\xB3 \xF0\x9F\x93\x8E"));
}
