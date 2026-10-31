// 候选页的构建与发布：按输入模式查候选（PrepareCandidateList）、组页并发布给候选窗，
// 以及调频用的排序键和翻页时的候选扩展。
#include "ipc/event_listener_internal.h"
#include <Windows.h>
#include <string>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <utility>
#include "ipc.h"
#include "ipc/candidate_expansion_policy.h"
#include "ipc/candidate_ui_owner.h"
#include "ipc/input_key_policy.h"
#include "utils/common_utils.h"
#include "utils/hanzi_pinyin.h"
#include <utf8.h>
#include "global/globals.h"
#include "engine/common/helpcode_utils.h"
#include "engine/quanpin/quanpin_query.h"
#include "engine/user_dictionary/user_dictionary_journal.h"
#include "cloud/cloud_translation.h"
#include "cloud/translation_gloss.h"
#include "english/english_ime.h"
#include "config/ime_config.h"
#include "ai/jev_candidate_policy.h"
#include "settings/collocation_model.h"
#include "engine/local_modes/quick_phrase_query.h"
#include "engine/local_modes/date_time_query.h"
#include "engine/local_modes/emoji_query.h"
#include "engine/local_modes/kaomoji_query.h"
#include "engine/local_modes/symbol_query.h"
#include "engine/local_modes/jianpin_query.h"
#include "engine/local_modes/v_mode_query.h"
#include "engine/shuangpin/shuangpin_profile.h"
#include "mixed/date_time_candidates.h"
#include "log/candidate_diag_log.h"

using namespace event_listener_detail;

namespace
{
// The engine's local mode queries take a resolved ShuangpinProfile and default it to Xiaohe. The
// modules this file used to call resolved the *configured* scheme instead, so every call site here
// has to pass this explicitly: letting the default through would silently decode J mode, emoji and
// kaomoji as Xiaohe for anyone on Ziranma, Shoudao or Microsoft shuangpin.
const ShuangpinProfile &ConfiguredShuangpinProfile()
{
    return GetShuangpinProfile(GetConfiguredShuangpinSchema());
}

std::string BuildCurrentCandidatePage();
void PrepareCandidateTranslationRequest();

std::string g_candidate_translation_scope;

bool BuildTranslationQuery(const WordItem &item, EnglishIme::TranslationQuery &query)
{
    // Dictionary keys remain simplified even when the visible candidate is
    // converted to traditional Chinese at render/commit time.
    const std::string visible = item.word;
    if (visible.empty())
        return false;
    if (item.source == CandidateSource::Emoji || item.source == CandidateSource::Kaomoji ||
        item.source == CandidateSource::Symbol || item.source == CandidateSource::DateTime)
        return false;
    if (item.source == CandidateSource::EnglishDictionary && !item.pinyin.empty())
    {
        query = {item.pinyin, EnglishIme::TranslationDirection::EnglishToChinese};
        return true;
    }

    bool has_ascii_letter = false;
    bool english = true;
    std::string normalized;
    normalized.reserve(visible.size());
    for (const unsigned char ch : visible)
    {
        if (ch >= 'A' && ch <= 'Z')
        {
            normalized.push_back(static_cast<char>(ch + ('a' - 'A')));
            has_ascii_letter = true;
        }
        else if (ch >= 'a' && ch <= 'z')
        {
            normalized.push_back(static_cast<char>(ch));
            has_ascii_letter = true;
        }
        else if (ch == ' ' || ch == '-' || ch == '\'')
        {
            normalized.push_back(static_cast<char>(ch));
        }
        else
        {
            english = false;
            break;
        }
    }
    if (english && has_ascii_letter)
    {
        query = {std::move(normalized), EnglishIme::TranslationDirection::EnglishToChinese};
        return true;
    }
    if (HelpcodeUtils::count_han_chars(visible) > 0)
    {
        query = {visible, EnglishIme::TranslationDirection::ChineseToEnglish};
        return true;
    }
    return false;
}

bool IsQuickPhraseInput(const std::string &raw)
{
    return g_quick_phrase_triggered && raw.size() > 1 && raw.front() == 'K' &&
           std::all_of(raw.begin() + 1, raw.end(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; });
}

// 快捷短语混进全拼/双拼的普通候选（Shift+K 模式另走
// IsQuickPhraseInput）：按整串编码精确匹配，只在输入是纯小写字母、不在造词、光标前缀
// 没截断时出现。整组插在第 slot 个普通候选之前，槽位由调频学出来，最大是每页候选数减一：
// 组前面最多有这么多个普通候选。放置在繁体写法展开之前，按候选个数数；前面的候选展开出多种写法时，
// 组在显示上随之后移，可能落到第二页。
// 不看辅助码：双拼开着辅助码时，任何「完整音节 + 一个字母」的奇数长编码（dia = di + a）
// 都会被认成带辅助码，按辅助码拦掉就等于这类编码永远出不来。整串精确等于用户自定义的
// 编码时，按短语处理。
void PlaceQuickPhrases(std::vector<WordItem> &items, const std::string &current_input)
{
    const SchemeType scheme = g_inputSession->current_scheme_type();
    if (!GetConfiguredMixedCandidatesEnabled() || !GetConfiguredQuickPhraseCandidatesEnabled() ||
        (scheme != SchemeType::Quanpin && scheme != SchemeType::Shuangpin) ||
        GlobalIme::composition.creating_word.active || g_inputSession->prefix_end() != current_input.size() ||
        current_input.empty() || !std::all_of(current_input.begin(), current_input.end(), [](unsigned char ch) {
            return ch >= 'a' && ch <= 'z';
        }))
        return;
    auto phrases = metasequoia::local_modes::query_quick_phrases_by_code(current_input).candidates;
    if (phrases.empty())
        return;
    std::size_t slot = 0;
    if (GetConfiguredQuickPhraseFrequencyEnabled() && GetConfiguredFrequencyAdjustment().mode != "disabled")
    {
        const int stored = user_dictionary::quick_phrase_slot(user_dictionary::default_user_db_path(), current_input);
        const int max_slot = (std::max)(GetConfiguredCandidatePageSize() - 1, 0);
        slot = static_cast<std::size_t>(std::clamp(stored, 0, max_slot));
    }
    metasequoia::local_modes::place_quick_phrases(items, std::move(phrases), slot);
}

// 日期时间混入普通候选的唤醒词，与 Shift+T 模式同一套（rq / riqi / date、sj / shijian / time、
// xq / xingqi / week）。整串精确匹配才算：全拼直接认原始输入；双拼的整拼要按方案换算，shijian 在
// 小鹤里打成 uijm，所以再拿换算出的全拼认一次。
std::string DateTimeMixedKeyword(const std::string &current_input, SchemeType scheme)
{
    if (current_input.empty() || !std::all_of(current_input.begin(), current_input.end(),
                                              [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; }))
        return {};
    if (metasequoia::local_modes::is_date_time_keyword(current_input))
        return current_input;
    if (scheme != SchemeType::Shuangpin)
        return {};
    std::string quanpin = g_inputSession->get_quanpin();
    quanpin.erase(std::remove_if(quanpin.begin(), quanpin.end(), [](char ch) { return ch == '\'' || ch == ' '; }),
                  quanpin.end());
    return metasequoia::local_modes::is_date_time_keyword(quanpin) ? quanpin : std::string{};
}

// 符号混输按整串编码查。双拼的按键串是音节码，用户要的是它读出来的全拼（小鹤 sj 是 san → ③），
// 所以先查换算出的全拼；全拼查不到再查原始按键，alpha 这类不是音节的编码在双拼下照样能混。
std::vector<std::string> SymbolMixedCodes(const std::string &current_input, SchemeType scheme)
{
    if (current_input.empty() || !std::all_of(current_input.begin(), current_input.end(),
                                              [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; }))
        return {};
    std::vector<std::string> codes;
    if (scheme == SchemeType::Shuangpin)
    {
        std::string quanpin = g_inputSession->get_quanpin();
        quanpin.erase(std::remove_if(quanpin.begin(), quanpin.end(), [](char ch) { return ch == '\'' || ch == ' '; }),
                      quanpin.end());
        if (!quanpin.empty() && quanpin != current_input)
            codes.push_back(std::move(quanpin));
    }
    codes.push_back(current_input);
    return codes;
}

// U 模式（部件拆字反查）：唤醒键 U 之后已经输入了至少一个部件字母才算一次可查询的输入。
bool IsRadicalInput(const std::string &raw)
{
    return IsRadicalCompositionActive(raw) && raw.size() > 1;
}

bool IsDateTimeInput(const std::string &raw)
{
    if (!IsDateTimeCompositionActive(raw) || raw.size() <= 1)
        return false;
    return metasequoia::local_modes::is_date_time_query(raw.substr(1));
}

bool IsEmojiInput(const std::string &raw)
{
    return IsEmojiCompositionActive(raw) && raw.size() > 1;
}

bool IsKaomojiInput(const std::string &raw)
{
    return IsKaomojiCompositionActive(raw) && raw.size() > 1;
}

bool IsSymbolInput(const std::string &raw)
{
    return IsSymbolCompositionActive(raw) && raw.size() > 1;
}

bool IsJianpinInput(const std::string &raw)
{
    return IsJianpinCompositionActive(raw) && raw.size() > 1 &&
           std::all_of(raw.begin() + 1, raw.end(),
                       [](unsigned char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); });
}
} // namespace

namespace event_listener_detail
{
void EnsureCandidatePageReady()
{
    if (!Global::candidate_ui.page_words.empty())
    {
        return;
    }
    if (Global::candidate_ui.items.empty())
    {
        return;
    }
    BuildCurrentCandidatePage();
}

std::wstring BuildUiLessCandidatePageW()
{
    EnsureCandidatePageReady();
    auto &ui = Global::candidate_ui;
    if (ui.page_words.empty() && !ui.items.empty())
    {
        BuildCurrentCandidatePage();
    }
    std::wstring page;
    for (size_t i = 0; i < ui.page_words.size(); ++i)
    {
        if (i != 0)
        {
            page += L',';
        }
        page += ui.page_words[i];
    }
    return page;
}
} // namespace event_listener_detail

namespace
{
// 卷轴展开面板一次最多带出的后续候选数。滚到底部时界面会请求再展开一批，
// 已加载的候选超过这个数时只显示前面这些。
constexpr size_t kExpandedCandidateLimit = 300;

std::string BuildCurrentCandidatePage()
{
    auto &ui = Global::candidate_ui;
    ui.clear_page();
    const SchemeType current_scheme = g_inputSession->current_scheme_type();
    const bool uppercase_all_helpcodes = current_scheme == SchemeType::Quanpin;
    // 副候选框里装的是译文，不是这次输入的候选：助记码、云/AI 角标和「右侧译文」都不适用，
    // 而且 g_candidate_translation_glosses 还留着原候选的译文，照常查会把译文再标注一遍。
    const bool translation_page = g_translation_candidates_active;
    const bool show_helpcodes =
        !translation_page && ((current_scheme == SchemeType::Shuangpin && IsConfiguredShuangpinHelpcodeHintShown()) ||
                              (current_scheme == SchemeType::Quanpin && GetConfiguredQuanpinHelpcodeEnabled() &&
                               GetConfiguredShowQuanpinHelpcodeInCandidateWindow()));
    // U 模式（部件拆字反查）：候选上反显该字的注音。annotation 在这个模式下专属注音，辅助码
    // 提示不在该模式出现（候选来源是 Generated，本来也不走辅助码的整句规则）。
    const bool radical_mode = IsRadicalCandidateListActive();

    // 组页时读一次徽标配置，循环内不再逐条查
    const bool show_fixed_badge = GetConfiguredCandidateFixedBadge();
    const std::string fixed_badge_style = GetConfiguredCandidateFixedBadgeStyle();
    // AI 联想候选的来源徽标随配置切换（🤖 / 🐳），同样每次组页解析一次。
    const std::string ai_badge = AiSuggestionBadge(GetConfiguredCandidateAiBadgeStyle());
    const bool show_sentence_source_badge = GetConfiguredAssocSentenceSourceBadge();
    const bool show_autocorrect_marker = GetConfiguredQuanpinAutocorrectMarker();
    const bool show_translation_pos = GetConfiguredCandidateTranslationShowPos();
    const bool single_translation_sense = GetConfiguredCandidateTranslationSingleSense();
    // 行内英文注释只在 candidate_translations 开时画到候选右侧。展开面板开着但行内注释关时，
    // 译文仍要查出来填进 page_glosses（面板用），只是不画到候选行上。
    const bool show_inline_translation = GetConfiguredCandidateTranslationsEnabled();
    // 徽标开启时才解析模型包的来源标签，每次组页解析一次，不逐候选查。标签由目录条目
    // 自己声明（万象/八股/墨奇），各家互不冒名；激活值留空时不产生整句候选，徽章分支
    // 不会被走到。
    const std::string collocation_badge =
        show_sentence_source_badge
            ? " 〔" + std::string(collocation::BadgeForModel(GetConfiguredAssocSentenceCollocationModel())) + "〕"
            : std::string();

    const int start = ui.current_page_start();
    const int loop = ui.current_page_count();

    int maxCount = 0;
    std::string candidate_string;
    for (int i = 0; i < loop; i++)
    {
        const auto &item = ui.items[start + i];
        const std::string word = CandidateOutputText(item);

        CandidateViewItem view;
        view.text = word;
        if (show_autocorrect_marker && !item.corrected_from.empty() && !item.jianpin_expansion)
        {
            // Correction-sourced candidates carry a light visible marker (PRD R5/AC7),
            // unless the user turned it off via quanpin.autocorrect_marker.
            // jianpin_expansion marks a pure abbreviation expansion: it keeps
            // corrected_from for preedit and ranking, but it is not a typo repair,
            // so it stays unmarked.
            // Only the display text is touched: commits, word frequency updates and
            // pinned-position lookups read item.word / page_words and must never see
            // the marker suffix.
            view.text += "*";
        }
        if (radical_mode)
        {
            // U 模式：候选上反显该字的注音；扩展区字没有注音时留空，属可接受降级。
            const std::string pinyin = HanziPinyin::Annotate(item.word);
            if (!pinyin.empty())
            {
                view.annotation = " " + pinyin;
            }
        }
        else if (item.source == CandidateSource::Generated && show_helpcodes)
        {
            // Generated whole-sentence candidates carry the raw spelling in
            // item.pinyin.  When helpcodes are enabled, do not expose that
            // full preedit; sentence annotations must use the normal
            // first/last-character helpcode rule instead.  If the helpcode
            // cannot be computed, leave the annotation empty rather than
            // falling back to the raw preedit.
            view.annotation = g_inputSession->get_helpcode_annotation(item.word, uppercase_all_helpcodes);
        }
        if (show_helpcodes && item.source != CandidateSource::EnglishDictionary &&
            item.source != CandidateSource::QuickPhrase && item.source != CandidateSource::Emoji &&
            item.source != CandidateSource::Kaomoji && item.source != CandidateSource::Symbol &&
            item.source != CandidateSource::DateTime && item.source != CandidateSource::Generated)
            view.annotation = g_inputSession->get_helpcode_annotation(item.word, uppercase_all_helpcodes);
        if (!IsCandidateSubPageActive() && item.word == JevSelectionWord() && Jev::IsPoolSource(item) &&
            item.fixed_position == 0)
            view.badge = " 〔Jev〕";
        else if (item.source == CandidateSource::CloudSuggestion)
            view.badge = " ☁️";
        else if (item.source == CandidateSource::AiSuggestion)
            view.badge = ai_badge;
        // 整句来源标签与设置页名称保持一致，方便同时比较四个来源。两家选中同一句时只剩一行，
        // 标签归先保留下来的来源；开启去重补位后，其余来源会改为显示自己的下一条不同结果。
        // Generated/Fallback 也被原样上屏、英文、日期等合成候选借用，只有引擎标了整句联想的才挂标签。
        // 设置页可整体关闭这组标签，候选本身和排序不受影响。
        else if (show_sentence_source_badge)
        {
            if (item.source == CandidateSource::Generated && item.sentence_association)
                view.badge = " 〔Trigram〕";
            else if (item.source == CandidateSource::Fallback && item.sentence_association)
                view.badge = " 〔Unigram〕";
            else if (item.source == CandidateSource::NeuralDesktop)
                view.badge = " 〔神经D〕";
            else if (item.source == CandidateSource::NeuralKeyboard)
                view.badge = " 〔神经K〕";
            else if (item.source == CandidateSource::Collocation)
                view.badge = collocation_badge;
        }
        view.fixed_position = item.fixed_position > 0;
        ApplyFixedPositionBadge(view, show_fixed_badge, fixed_badge_style);
        std::string raw_translation;
        bool dictionary_translation = false;
        EnglishIme::TranslationQuery translation_query;
        if (!translation_page && BuildTranslationQuery(item, translation_query))
        {
            const auto gloss = g_candidate_translation_glosses.find(TranslationIdentity(translation_query));
            if (gloss != g_candidate_translation_glosses.end())
            {
                raw_translation = gloss->second.text;
                dictionary_translation = gloss->second.from_dictionary;
            }
        }
        if (show_inline_translation)
        {
            view.translation = FanyImeIpc::FormatTranslationGlossForDisplay(
                raw_translation, show_translation_pos, single_translation_sense,
                single_translation_sense && dictionary_translation);
        }
        const std::string visible = view.text + view.annotation + view.badge;
        const int display_length = static_cast<int>(utf8::distance(visible.begin(), visible.end()));
        candidate_string += CandidateViewHtml(view);
        // Ctrl+Enter consumes the original senses, independently of display preferences.
        ui.page_glosses.push_back(string_to_wstring(raw_translation));
        ui.page_views.push_back(std::move(view));
        maxCount = (std::max)(maxCount, display_length);
        ui.page_words.push_back(string_to_wstring(word));
        if (i < loop - 1)
        {
            candidate_string += ",";
        }
    }

    if (maxCount > 2)
    {
        ui.cur_page_max_word_len = maxCount;
    }
    ui.cur_page_item_cnt = loop;
    if (!ui.page_words.empty())
    {
        ui.selected_index_in_page = std::clamp(ui.selected_index_in_page, 0, loop - 1);
        ui.selected_text = ui.page_words[ui.selected_index_in_page];
    }
    return candidate_string;
}

void PrepareCandidateTranslationRequest()
{
    // 行内译文注释（general.candidate_translations）关掉时主列表不显示英文，但展开面板真的开着就要查译文：
    // 竖排面板的 senses 来自 page_glosses，查询与行内显示解耦。面板没开时这条查询门控一并关掉，
    // 开面板那颗键的补查见 ProbeTranslationForHighlightedCandidate。
    const bool enabled = FanyNamedPipe::CandidateTranslationLookupEnabled();
    auto &ui = Global::candidate_ui;
    if (IsCandidateSubPageActive())
    {
        // 译文页、日期页不再查译文。已经取出的 glosses 也不能清，退出子模式后原候选还要用。
        return;
    }
    if (!enabled)
    {
        g_candidate_translation_glosses.clear();
    }
    if (!enabled || ui.items.empty())
    {
        // An empty page (English mode between keystrokes, or the end of a
        // composition) only cancels the pending lookups; the cache stays for
        // the next page.
        if (!g_candidate_translation_signature.empty())
        {
            g_candidate_translation_signature.clear();
            EnglishIme::ClearTranslations();
            CloudTranslation::Clear();
        }
        return;
    }
    const std::string scope = CloudTranslation::ActiveProviderScope();
    if (scope != g_candidate_translation_scope)
    {
        g_candidate_translation_scope = scope;
        g_candidate_translation_glosses.clear();
    }

    std::vector<EnglishIme::TranslationQuery> queries;
    std::string signature;
    const int start = ui.current_page_start();
    const int count = ui.current_page_count();
    for (int i = 0; i < count; ++i)
    {
        EnglishIme::TranslationQuery query;
        if (!BuildTranslationQuery(ui.items[start + i], query))
            continue;
        const std::string identity = TranslationIdentity(query);
        signature += std::to_string(identity.size()) + ":" + identity;
        if (std::none_of(queries.begin(), queries.end(), [&](const auto &existing) {
                return existing.key == query.key && existing.direction == query.direction;
            }))
            queries.push_back(std::move(query));
    }

    if (signature == g_candidate_translation_signature)
        return;
    g_candidate_translation_signature = std::move(signature);
    CloudTranslation::Clear();
    EnglishIme::RequestTranslations(std::move(queries), GetConfiguredTencentTmt().target_language == "en");
}

// Copy the page the worker just finished into an immutable snapshot for the UI thread. This is the single publish
// point: set_items() and clear_page() are only intermediate steps of a rebuild, so publishing there would hand the UI a
// half-built page.
void PublishBuiltCandidatePage(const std::wstring &candidate_string)
{
    const auto &ui = Global::candidate_ui;
    auto snapshot = std::make_shared<Global::CandidatePageSnapshot>();
    snapshot->page_views = ui.page_views;
    snapshot->page_words = ui.page_words;
    snapshot->candidate_string = candidate_string;
    snapshot->page_index = ui.page_index;
    snapshot->selected_index_in_page = ui.selected_index_in_page;
    snapshot->page_count = ui.current_page_count();
    snapshot->page_item_count = ui.cur_page_item_cnt;
    snapshot->has_previous_page = ui.has_prev_page();
    snapshot->has_next_page =
        ui.has_next_page() || (ui.is_current_page_full() && FanyNamedPipe::CanExpandCandidatesKeepingPagePosition());
    if (Global::candidate_expand_panel_open.load(std::memory_order_acquire) && !IsUiLessMode())
    {
        // 展开面板只显示文字，徽标、辅助码与译文仍只在首行（当前页）上。
        const size_t start = static_cast<size_t>(ui.current_page_start() + ui.current_page_count());
        const size_t end = (std::min)(ui.items.size(), start + kExpandedCandidateLimit);
        snapshot->expanded_start = static_cast<int>(start);
        for (size_t i = start; i < end; ++i)
            snapshot->expanded_words.push_back(string_to_wstring(CandidateOutputText(ui.items[i])));
    }
    // 候选展开面板：发布时按当前高亮候选跑提供方管线派生条目（当前只有翻译），面板跟随高亮
    // （输入变化、移高亮、翻页都走这一处）。gloss 变了说明高亮换到了别的词，面板内选中位归零；
    // 高亮到没有任何启用类别条目的词时面板这一帧不出现，状态保持打开。
    if (ui.expansion_panel_active)
    {
        auto &mutable_ui = Global::candidate_ui;
        const int highlighted = (std::max)(0, mutable_ui.selected_index_in_page);
        // signature 记锚点词（不是 gloss）：高亮换到别的词时把面板内选中位归零。emoji-only 的候选没有
        // gloss，只有用词才能正确触发归零。
        const std::wstring anchor = string_to_wstring(FanyNamedPipe::CurrentExpansionAnchorWord());
        if (anchor != mutable_ui.expansion_panel_signature)
        {
            mutable_ui.expansion_panel_signature = anchor;
            mutable_ui.expansion_panel_selected = 0;
        }
        // 与按键路径共用同一份管线，保证面板里看到的与上屏的是同一批条目。展示按
        // candidate_translation_show_pos 决定词性，上屏始终剥词性（在 ExpansionItem 里已分开）。
        const auto items = FanyNamedPipe::CurrentExpansionPanelItems();
        if (!items.empty())
        {
            snapshot->expansion_panel_active = true;
            snapshot->expansion_panel_anchor = highlighted;
            snapshot->expansion_panel_selected =
                std::clamp(mutable_ui.expansion_panel_selected, 0, static_cast<int>(items.size()) - 1);
            for (const auto &item : items)
            {
                snapshot->expansion_panel_items.push_back({string_to_wstring(item.display), string_to_wstring(item.tag),
                                                           string_to_wstring(item.commit),
                                                           string_to_wstring(item.category_id)});
            }
        }
    }
    // Stamp before publishing so the UI thread can echo back exactly which page it painted.
    snapshot->generation = ++Global::candidate_page_generation;
    Global::PublishCandidatePageSnapshot(std::move(snapshot));
}
} // namespace

namespace event_listener_detail
{
void RefreshCandidatePageUi(bool show_window)
{
    RefreshJevSelection();
    PrepareCandidateTranslationRequest();
    const std::string candidate_string = BuildCurrentCandidatePage();
    // Host-drawn UI wants plain words (Microsoft IME style), not helpcodes.
    const std::wstring published = IsUiLessMode() ? BuildUiLessCandidatePageW() : string_to_wstring(candidate_string);
    ::WriteDataToSharedMemory(published, true);
    PublishBuiltCandidatePage(published);
    CAND_DIAG_LOGF(L"candidate UI refreshed show={} uiless={} items={} page_words={} selected={} page={} "
                   L"serialized_units={}",
                   show_window, IsUiLessMode(), Global::candidate_ui.items.size(),
                   Global::candidate_ui.page_words.size(), Global::candidate_ui.selected_index_in_page,
                   Global::candidate_ui.page_index, candidate_string.size());
    if (show_window)
    {
        RequestShowCandidateWindow();
    }
}
} // namespace event_listener_detail

namespace FanyNamedPipe
{
// 译文查询门控（见 internal.h 声明）。行内显示与查询解耦：这里只决定「要不要查、要不要填
// page_glosses」，行内是否把英文画到候选右侧由 BuildCurrentCandidatePage 按 candidate_translations
// 单独控制。展开面板的 senses 来自 page_glosses，所以面板真的开着才查（惰性门控：面板没开、
// 行内又关着时，这条查询对任何用户都不发生）；开面板那颗键由 ProbeTranslationForHighlightedCandidate
// 给高亮词补上译文，面板开着期间的后续按键再走这条正常路径。
bool CandidateTranslationLookupEnabled()
{
    const bool japanese = g_inputSession && g_inputSession->current_scheme_type() == SchemeType::JapaneseRomaji;
    const bool panel_translation_active =
        Global::candidate_ui.expansion_panel_active && GetConfiguredCandidateExpansionPanelEnabled() &&
        GetConfiguredCandidateExpansionPanelTranslationEnabled() && !GetConfiguredCandidateExpansionKey().empty();
    return (GetConfiguredCandidateTranslationsEnabled() || panel_translation_active) && !IsUiLessMode() && !japanese;
}

// 开面板路径的补查（见 internal.h 声明）。只查高亮这一条词：本地词典命中就直接种进 gloss 缓存
// （调用方随后的 RefreshCandidatePageUi 会重建 page_glosses，面板首开即有数据，不走异步回填）；
// 本地没命中但配了云端 provider 且这个词云端可译，也返回 true——面板乐观打开，译义类等异步结果
// 到达后回填。两者都不成立才返回 false，调用方据此拒绝开面板。
bool ProbeTranslationForHighlightedCandidate()
{
    const auto &ui = Global::candidate_ui;
    const int highlighted = (std::max)(0, ui.selected_index_in_page);
    const int index = ui.current_page_start() + highlighted;
    if (index < 0 || index >= static_cast<int>(ui.items.size()))
    {
        return false;
    }
    EnglishIme::TranslationQuery query;
    if (!BuildTranslationQuery(ui.items[static_cast<size_t>(index)], query))
    {
        return false;
    }
    const std::string gloss = EnglishIme::LookupTranslationSync(query);
    if (!gloss.empty())
    {
        g_candidate_translation_glosses[TranslationIdentity(query)] = {std::move(gloss), true};
        return true;
    }
    if (!CloudTranslation::ActiveProviderScope().empty())
    {
        return query.direction == EnglishIme::TranslationDirection::EnglishToChinese
                   ? CloudTranslation::IsCloudTranslatableEnglish(query.key)
                   : CloudTranslation::IsCloudTranslatableChinese(query.key);
    }
    return false;
}

std::string CurrentRankingContextKey()
{
    if (g_inputSession)
    {
        const std::string raw = g_inputSession->get_pinyin_sequence_with_cases();
        if (IsJianpinCompositionActive(raw) && raw.size() > 1)
            return metasequoia::local_modes::jianpin_ranking_context(
                raw.substr(1), g_inputSession->current_scheme_type(), ConfiguredShuangpinProfile());
    }
    std::string converted = g_inputSession->get_quanpin();
    if (converted.empty())
        converted = g_inputSession->get_pinyin_segmentation();
    if (g_inputSession->get_pinyin_sequence().size() == 1)
        return converted;
    std::string plain = converted;
    plain.erase(std::remove(plain.begin(), plain.end(), '\''), plain.end());
    // 与引擎侧 InputSession::position_context 同一规则（quanpin::correction_aliases_enabled 只看
    // 换位、邻键两个开关）：全拼跟纠错开关走，双拼保持原样。
    const bool correction_aliases = g_inputSession->current_scheme_type() == SchemeType::Shuangpin ||
                                    GetConfiguredQuanpinAutocorrectTransposition() ||
                                    GetConfiguredQuanpinAutocorrectNeighbor();
    const auto cuts = quanpin::cut_pinyin_by_mode(plain, "correction", correction_aliases);
    return cuts.empty() ? converted : quanpin::join_segments(cuts.front());
}

std::string EnglishInputKey()
{
    std::string key = g_inputSession->get_pinyin_sequence_with_cases();
    if (IsYModeInput(key))
        key = key.substr(1);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return key;
}

std::string EnglishRankingContextKey()
{
    return "english:" + EnglishInputKey();
}

int EnglishSlotMaximum()
{
    return (std::max)(GetConfiguredCandidatePageSize() - 1, 1);
}

FanyImeIpc::EnglishPlacement CurrentEnglishPlacement(const std::vector<WordItem> &items)
{
    FanyImeIpc::EnglishPlacement placement;
    // 每页候选数没有英文时也要给：混输 emoji 的首个靠它放到首页末位。
    placement.page_size = static_cast<size_t>(EnglishSlotMaximum()) + 1;
    placement.first_page_count = static_cast<size_t>(GetConfiguredEnglishMixedInputFirstPageCount());
    // 没有英文候选就用不上，省掉一次用户库查询。
    if (!g_inputSession || std::none_of(items.begin(), items.end(), [](const WordItem &item) {
            return item.source == CandidateSource::EnglishDictionary;
        }))
        return placement;
    placement.input = EnglishInputKey();
    placement.require_exact = g_inputSession->reads_as_pinyin();
    // 调频关闭时不用学到的槽位，英文回到默认位置。
    if (GetConfiguredFrequencyAdjustment().mode != "disabled")
    {
        if (const auto slot = user_dictionary::english_slot(user_dictionary::default_user_db_path(), placement.input))
            placement.slot = static_cast<size_t>(std::clamp(*slot, 0, EnglishSlotMaximum()));
    }
    return placement;
}

namespace
{
// 当前候选列表按哪种字形显示：PrepareCandidateList 组页、RespellCandidatesForCharacterSetChange 切换字形时记下。
// 只在任务线程上读写。
std::string g_candidate_list_character_set;
// PrepareCandidateList 组页时这张列表有没有按字形展开写法（简拼与普通拼音的候选有，V 模式、日期时间、
// Y 模式原文、补的整串候选这些没有）。没有展开的列表切换字形时不换写法。只在任务线程上读写。
bool g_candidate_list_spelled_by_character_set = false;

bool CandidateListAcceptsExpansion()
{
    // 译文页、日期页是一份固定的列表，session 里再多的候选也不属于它。
    return !IsCandidateSubPageActive() &&
           !IsSpecialModeCompositionActive(GlobalIme::composition.raw_input_with_cases) && g_inputSession;
}
} // namespace

const std::string &CandidateListCharacterSet()
{
    return g_candidate_list_character_set;
}

bool CanExpandCandidatesKeepingPagePosition()
{
    return CandidateListAcceptsExpansion() && g_inputSession->can_expand_initial_candidates();
}

// Pulls the next batch of candidates out of the session without disturbing
// where the user currently is: set_items resets the page and the selection, so
// both are restored afterwards. Returns false when nothing more was loaded, or
// when everything loaded was already on the list.
bool ExpandCandidatesKeepingPagePosition()
{
    auto &ui = Global::candidate_ui;
    if (!CandidateListAcceptsExpansion() || !g_inputSession->expand_initial_candidates())
    {
        return false;
    }
    auto expanded = g_inputSession->get_candidates();
    user_dictionary::apply_fixed_positions(
        user_dictionary::default_user_db_path(), CurrentRankingContextKey(), expanded, true,
        [](const std::string &key, const std::string &value) { return g_inputSession->find_candidate(key, value); },
        g_inputSession->has_active_helpcode());
    PlaceQuickPhrases(expanded, g_inputSession->get_pinyin_sequence_with_cases());
    expanded = WithTraditionalVariants(std::move(expanded));
    return FanyImeIpc::SetExpandedCandidatesKeepingPagePosition(ui, std::move(expanded),
                                                                CurrentEnglishPlacement(ui.items));
}

// Moves one page in `offset`'s direction, expanding the candidate list first
// when the move would run off the end. Anything other than Unchanged needs a
// UI refresh.
PageMoveResult MoveCandidatePage(int offset)
{
    auto &ui = Global::candidate_ui;
    ui.begin_selection();
    if (offset > 0 && ui.is_next_page_partial_last_page())
    {
        // Populate the last partial page before entering it, so the first
        // display of that page is already full.
        ExpandCandidatesKeepingPagePosition();
    }
    else if (offset > 0 && !ui.has_next_page())
    {
        const bool current_page_was_full = ui.is_current_page_full();
        if (ExpandCandidatesKeepingPagePosition() && !current_page_was_full)
        {
            // Newly loaded items first fill the unused slots on the current last
            // page. Refresh that page instead of skipping those items by
            // advancing immediately.
            return PageMoveResult::CurrentPageRefilled;
        }
    }
    if (offset < 0 ? ui.has_prev_page() : ui.has_next_page())
    {
        ui.page_index += offset;
        return PageMoveResult::Moved;
    }
    return PageMoveResult::Unchanged;
}

int MoveToExpandedCandidate(int absolute_index, const std::wstring &expected_text)
{
    auto &ui = Global::candidate_ui;
    if (ui.page_size <= 0 || expected_text.empty())
        return -1;
    const auto matches = [&](size_t index) {
        return string_to_wstring(CandidateOutputText(ui.items[index])) == expected_text;
    };
    // 点下去之后候选可能被异步结果插队；下标对不上时按显示文字重新定位，同字取最靠前的那个。
    size_t index = static_cast<size_t>((std::max)(absolute_index, 0));
    if (index >= ui.items.size() || !matches(index))
    {
        index = 0;
        while (index < ui.items.size() && !matches(index))
            ++index;
        if (index >= ui.items.size())
            return -1;
    }
    ui.begin_selection();
    ui.page_index = static_cast<int>(index) / ui.page_size;
    ui.selected_index_in_page = static_cast<int>(index) % ui.page_size;
    ui.clear_page();
    return ui.selected_index_in_page;
}

std::string CandidateDatabaseKey(const WordItem &item, const std::string &context_key)
{
    if (!item.canonical_pinyin.empty())
        return item.canonical_pinyin;
    if (g_inputSession->get_pinyin_sequence().size() == 1)
        return item.pinyin;
    auto segments = quanpin::split_segments(context_key);
    const size_t han_count = HelpcodeUtils::count_han_chars(item.word);
    if (segments.empty() || han_count == 0)
        return item.pinyin;
    if (segments.size() > han_count)
        segments.resize(han_count);
    return quanpin::join_segments(segments);
}

std::pair<std::string, std::string> RankingKeysForCandidate(const WordItem &item)
{
    // 混输组合里五笔候选与拼音候选共存，按候选自己的方案选键：五笔候选写码表（context 与
    // entry 都是码），拼音候选写拼音词典。此时 CurrentRankingContextKey() 从 get_quanpin()
    // 取到的正是那串字母的拼音上下文，可以直接用。
    if (item.scheme == SchemeType::Wubi)
    {
        const std::string key =
            item.pinyin.empty() && g_inputSession ? g_inputSession->get_pinyin_sequence() : item.pinyin;
        return {key, item.pinyin};
    }
    const std::string context_key = CurrentRankingContextKey();
    return {context_key, CandidateDatabaseKey(item, context_key)};
}

// Stopwatch for the per-keystroke candidate build. Everything this function does
// runs on the shared task thread, so a stall anywhere in it delays the hide/show
// messages queued behind it — which is what reaches the screen as flicker. The
// splits exist to tell those segments apart: the engine candidate lookup and the
// user-dictionary fixed-position pass both touch a database, and only a
// measurement says which one is paying for it.
class CandidateBuildTimer
{
  public:
    // Milliseconds since the previous split, then restarts the segment.
    double Split()
    {
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - last_).count();
        last_ = now;
        return ms;
    }
    double TotalMs() const
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
    }

  private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_ = start_;
};

void PrepareCandidateList(uint64_t client_id, uint64_t activation_epoch)
{
    CandidateBuildTimer segment;
    // Zero means the branch taken this keystroke has no such segment, not that
    // the segment was instant.
    double queryMs = 0;
    double fixedPosMs = 0;

    auto &ui = Global::candidate_ui;
    // 界面线程随时可能改字形：展开、记下与组页显示（CandidateOutputText 读记下的值）都用这一次读到的字形。
    const std::string character_set = GetConfiguredCharacterSet();
    bool spelled_by_character_set = false;
    // 日期页的列表是选中入口时另查的，按会话重新组页就说明已经回到了普通候选。
    if (g_date_time_page_active)
    {
        g_date_time_page_active = false;
        g_date_time_page_keyword.clear();
        g_translation_saved_items.clear();
    }
    std::string pinyin = wstring_to_string(Global::PinyinString);
    const std::string current_input = g_inputSession->get_pinyin_sequence_with_cases();
    std::vector<WordItem> items;
    if (g_english_input_mode)
    {
        // Do not expose transient Chinese/raw fallback candidates while the
        // dedicated English query is in flight.
    }
    else if (IsRadicalInput(current_input))
    {
        // 串里混进了 a-z 以外的字符（撇号、大写字母等）时查不到任何字，候选为空。
        items = g_inputSession->query_radical(current_input.substr(1));
    }
    else if (IsQuickPhraseInput(current_input))
    {
        items = metasequoia::local_modes::query_quick_phrases(current_input.substr(1)).candidates;
    }
    else if (IsDateTimeInput(current_input))
    {
        items = OrderedDateTimeCandidates(current_input.substr(1));
    }
    else if (IsEmojiInput(current_input))
    {
        items = metasequoia::local_modes::query_emoji(current_input.substr(1), g_inputSession->current_scheme_type(),
                                                      10, ConfiguredShuangpinProfile())
                    .candidates;
    }
    else if (IsKaomojiInput(current_input))
    {
        items = metasequoia::local_modes::query_kaomoji(current_input.substr(1), g_inputSession->current_scheme_type(),
                                                        10, ConfiguredShuangpinProfile())
                    .candidates;
    }
    else if (IsSymbolInput(current_input))
    {
        // 一个分组可以有上百个符号（jt 箭头 148 个），整组都放进列表，靠翻页看全。
        items = metasequoia::local_modes::query_symbols(current_input.substr(1)).candidates;
    }
    else if (IsJianpinInput(current_input))
    {
        const int limit = current_input.size() == 2 ? 24 : 100;
        items = metasequoia::local_modes::query_jianpin(current_input.substr(1), g_inputSession->current_scheme_type(),
                                                        limit, ConfiguredShuangpinProfile())
                    .candidates;
        const std::string typed = g_inputSession->get_pinyin_sequence();
        for (auto &item : items)
            item.pinyin = typed;
        queryMs = segment.Split();
        user_dictionary::apply_fixed_positions(user_dictionary::default_user_db_path(), CurrentRankingContextKey(),
                                               items, false);
        fixedPosMs = segment.Split();
        items = WithTraditionalVariantsFor(std::move(items), character_set);
        spelled_by_character_set = true;
    }
    else if (IsYModeInput(current_input))
    {
        // Show the typed English immediately; dictionary completions arrive asynchronously.
        items.emplace_back("", current_input.substr(1), 0, CandidateSource::Generated);
    }
    else if (!g_english_input_mode && IsVModeCompositionActive(current_input))
    {
        // V 模式：数字转中文或算式结果。候选来源是 Generated，选中时直接上屏，不进词库、不调频。
        items = metasequoia::local_modes::query_v_mode(current_input.substr(1));
    }
    else if (IsSpecialModeCompositionActive(current_input))
    {
        // A K/U/T/E/M/S/J/Y special-mode prefix that is not yet a complete input (e.g.
        // "K", "U", "U+", "Tw", "Txin", "E", "M", "S", "J", "Y"): do not translate it into
        // normal pinyin candidates. Leave items empty so only the raw typed text
        // shows as the fallback.
    }
    else
    {
        items = g_inputSession->get_candidates();
        queryMs = segment.Split();
        user_dictionary::apply_fixed_positions(
            user_dictionary::default_user_db_path(), CurrentRankingContextKey(), items,
            g_inputSession->get_pinyin_sequence().size() == 1,
            [](const std::string &key, const std::string &value) { return g_inputSession->find_candidate(key, value); },
            g_inputSession->has_active_helpcode());
        PlaceQuickPhrases(items, current_input);
        fixedPosMs = segment.Split();
        if (g_inputSession->get_pinyin_sequence().size() == 1 && items.size() > 24)
            items.resize(24);
        // 单字母输入截到前 24 个候选之后再展开写法，截的仍是候选个数，不切开写法组。
        items = WithTraditionalVariantsFor(std::move(items), character_set);
        spelled_by_character_set = true;
    }

    // R4：光标前缀为空时不造「整串假候选」——前缀为空就该没有候选（候选窗由调用方
    // 收起）。caret 未设置时 prefix_end 等于串长，此分支永不触发，现状零差异。
    if (items.empty() && !g_english_input_mode &&
        !FanyImeIpc::IsCaretPrefixEmpty(g_inputSession->prefix_end(),
                                        g_inputSession->get_pinyin_sequence_with_cases().size()))
    {
        // A spelling carrying a literal '-', '_' or '+' cannot be split by the
        // engine, so every rebuild has to reproduce the same typed-string
        // fallback the key published; otherwise the next ShowCandidate drops the
        // only entry and Space has nothing to commit.
        const bool literal_spelling = current_input.find_first_of("-_+") != std::string::npos;
        const std::string fallback = literal_spelling ? current_input : pinyin;
        items.emplace_back(fallback, fallback, 1, CandidateSource::Fallback);
        spelled_by_character_set = false;
    }

    // Whatever the branch above did that the two splits did not already claim.
    const double branchMs = segment.Split();
    const size_t itemCount = items.size();

    g_candidate_list_character_set = character_set;
    g_candidate_list_spelled_by_character_set = spelled_by_character_set;
    ui.set_items(std::move(items));
    RefreshCandidatePageUi(false);
    PublishCandidateUiOwner(client_id, activation_epoch);
    const double uiMs = segment.Split();

    const SchemeType scheme = g_inputSession->current_scheme_type();
    g_dedicated_english_answer_pending = false;
    if (g_english_input_mode)
    {
        UpdateEnglishInput(current_input, client_id, activation_epoch, true);
        g_dedicated_english_answer_pending = !current_input.empty() && EnglishIme::IsRunning();
    }
    else if (IsYModeInput(current_input))
    {
        UpdateEnglishInput(current_input.substr(1), client_id, activation_epoch, true);
        g_dedicated_english_answer_pending = EnglishIme::IsRunning();
    }
    else
    {
        UpdateEnglishInput("");
    }
    const double englishMs = segment.Split();

    // 混输的英文、emoji、颜文字、日期时间交给同一条后台线程（MixedCandidates），查完一次合并。
    // 快捷短语不在这里：它要跟着翻页扩展重新放置，已经在上面同步混进去了。
    MixedCandidates::Request mixed;
    if (!g_english_input_mode && !IsSpecialModeCompositionActive(current_input) &&
        GetConfiguredMixedCandidatesEnabled() && (scheme == SchemeType::Quanpin || scheme == SchemeType::Shuangpin) &&
        !GlobalIme::composition.creating_word.active)
    {
        mixed.english = GetConfiguredEnglishCandidatesEnabled();
        mixed.english_min_prefix = static_cast<size_t>(GetConfiguredEnglishMixedInputMinChars());
        mixed.english_first_page_count = static_cast<size_t>(GetConfiguredEnglishMixedInputFirstPageCount());
        // emoji 和符号按整串编码精确匹配，光标前缀截断时整串不是用户要的编码。
        mixed.emoji = GetConfiguredEmojiMixedInputEnabled() && g_inputSession->prefix_end() == current_input.size();
        mixed.kaomoji = GetConfiguredKaomojiMixedInputEnabled();
        if (GetConfiguredSymbolMixedInputEnabled() && g_inputSession->prefix_end() == current_input.size())
            mixed.symbol_codes = SymbolMixedCodes(current_input, scheme);
        if (GetConfiguredDateTimeCandidatesEnabled() && g_inputSession->prefix_end() == current_input.size())
            mixed.date_time_keyword = DateTimeMixedKeyword(current_input, scheme);
        if (mixed.english || mixed.emoji || mixed.kaomoji || !mixed.symbol_codes.empty() ||
            !mixed.date_time_keyword.empty())
        {
            mixed.input = current_input;
            mixed.scheme = scheme;
        }
    }
    UpdateMixedInput(std::move(mixed), client_id, activation_epoch);
    const double mixedMs = segment.Split();

    CAND_DIAG_LOGF(L"candidate build total_ms={:.1f} query_ms={:.1f} fixed_pos_ms={:.1f} branch_ms={:.1f} "
                   L"ui_ms={:.1f} english_ms={:.1f} mixed_ms={:.1f} items={}",
                   segment.TotalMs(), queryMs, fixedPosMs, branchMs, uiMs, englishMs, mixedMs, itemCount);
}

namespace
{
// 只换当前列表的写法，不重新组页：翻页扩展出的候选、已插入的云和 AI 候选、混输结果按原来的先后顺序留着。
// 写法全归列表里别的底词的候选（「余」「馀」同在列表时的「馀」）切到繁体时不出现，切回简体也找不回来，
// 要到下一次组页。高亮回到切换前那个底词的写法组组首，新列表里没有它时落在第一页第一项。
void RespellCandidateListKeepingHighlight(const std::string &character_set)
{
    auto &ui = Global::candidate_ui;
    const size_t highlighted_index = static_cast<size_t>(ui.current_page_start() + ui.selected_index_in_page);
    const bool selection_started = ui.selection_started;
    auto respelled = FanyImeIpc::RespellCandidateList(
        std::move(ui.items), highlighted_index, ui.page_size,
        [&](std::vector<WordItem> items) { return WithTraditionalVariantsFor(std::move(items), character_set); });
    ui.set_items(std::move(respelled.items));
    ui.page_index = respelled.highlight.page_index;
    ui.selected_index_in_page = respelled.highlight.index_in_page;
    ui.selection_started = selection_started;
}
} // namespace

bool RespellCandidatesForCharacterSetChange()
{
    if (!g_inputSession || Global::candidate_ui.items.empty() || IsCandidateSubPageActive() ||
        g_candidate_list_character_set == GetConfiguredCharacterSet())
        return false;
    if (!FanyImeIpc::CandidateListFollowsCharacterSet(
            /*japanese_scheme=*/g_inputSession->current_scheme_type() == SchemeType::JapaneseRomaji,
            /*radical_candidate_list=*/IsRadicalCandidateListActive(), /*english_input_mode=*/g_english_input_mode))
        return false;
    const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
    if (!owner || !IsPipeActivationCurrent(owner.client_id, owner.activation_epoch))
        return false;
    const std::string character_set = GetConfiguredCharacterSet();
    g_candidate_list_character_set = character_set;
    // 没有按字形展开过写法的列表原样留着，显示时按新字形逐项转换（CandidateOutputText）。
    if (g_candidate_list_spelled_by_character_set)
        RespellCandidateListKeepingHighlight(character_set);
    RefreshCandidatePageUi(false);
    RequestShowCandidateWindow();
    PushUiLessCandidatesToOwner();
    return true;
}
} // namespace FanyNamedPipe
