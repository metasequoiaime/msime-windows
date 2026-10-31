// 通用功能开关：诊断日志、输入统计、中英与表情混输、候选翻译、云候选、整句联想、各工具模式与剪贴板历史。
#include "config/ime_config_internal.h"
#include <atomic>
#include <string>
#include "clipboard/clipboard_history.h"
#include "engine/contracts/v_mode_input.h"
#include "statistics/stats_store.h"

using namespace ime_config_detail;

bool GetConfiguredDiagnosticLogEnabled()
{
    return g_diagnostic_log_enabled.load(std::memory_order_relaxed);
}

bool SetConfiguredDiagnosticLogEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "diagnostic_log", enabled ? "true" : "false"))
        return false;
    g_diagnostic_log_enabled.store(enabled, std::memory_order_relaxed);
    return true;
}

bool GetConfiguredTsfDiagnosticLogEnabled()
{
    return g_tsf_diagnostic_log_enabled.load(std::memory_order_relaxed);
}

bool SetConfiguredTsfDiagnosticLogEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "tsf_diagnostic_log", enabled ? "true" : "false"))
        return false;
    g_tsf_diagnostic_log_enabled.store(enabled, std::memory_order_relaxed);
    return true;
}

bool GetConfiguredStatisticsEnabled()
{
    return g_statistics_enabled.load(std::memory_order_relaxed);
}

bool SetConfiguredStatisticsEnabled(bool enabled)
{
    if (!WriteConfiguredValue("statistics", "enabled", enabled ? "true" : "false"))
        return false;
    g_statistics_enabled.store(enabled, std::memory_order_relaxed);
    return true;
}

const std::string &GetConfiguredStatisticsRetention()
{
    return g_statistics_retention;
}

bool SetConfiguredStatisticsRetention(const std::string &retention)
{
    // 先校验后落盘：非法枚举不写文件也不动内存值，设置页的请求会被静默拒绝。
    MsimeStats::Retention parsed = MsimeStats::Retention::Forever;
    if (!MsimeStats::ParseRetention(retention, parsed))
        return false;
    if (!WriteConfiguredValue("statistics", "retention", EscapeTomlBasicString(retention)))
        return false;
    g_statistics_retention = retention;
    return true;
}

bool GetConfiguredEnglishCandidatesEnabled()
{
    return g_english_candidates_enabled;
}

bool SetConfiguredEnglishCandidatesEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "cn_en_mixed_input", enabled ? "true" : "false"))
    {
        return false;
    }
    g_english_candidates_enabled = enabled;
    return true;
}

bool GetConfiguredCandidateTranslationsEnabled()
{
    return g_candidate_translations_enabled;
}

bool SetConfiguredCandidateTranslationsEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "candidate_translations", enabled ? "true" : "false"))
        return false;
    g_candidate_translations_enabled = enabled;
    return true;
}

bool GetConfiguredCandidateExpansionPanelEnabled()
{
    return g_candidate_expansion_panel_enabled;
}

bool SetConfiguredCandidateExpansionPanelEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "candidate_expansion_panel_enabled", enabled ? "true" : "false"))
        return false;
    g_candidate_expansion_panel_enabled = enabled;
    return true;
}

bool GetConfiguredCandidateExpansionPanelTranslationEnabled()
{
    return g_candidate_expansion_panel_translation;
}

bool SetConfiguredCandidateExpansionPanelTranslationEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "candidate_expansion_panel_translation", enabled ? "true" : "false"))
        return false;
    g_candidate_expansion_panel_translation = enabled;
    return true;
}

bool GetConfiguredCandidateExpansionPanelEmojiEnabled()
{
    return g_candidate_expansion_panel_emoji;
}

bool SetConfiguredCandidateExpansionPanelEmojiEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "candidate_expansion_panel_emoji", enabled ? "true" : "false"))
        return false;
    g_candidate_expansion_panel_emoji = enabled;
    return true;
}

bool GetConfiguredCandidateExpansionPanelKaomojiEnabled()
{
    return g_candidate_expansion_panel_kaomoji;
}

bool SetConfiguredCandidateExpansionPanelKaomojiEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "candidate_expansion_panel_kaomoji", enabled ? "true" : "false"))
        return false;
    g_candidate_expansion_panel_kaomoji = enabled;
    return true;
}

bool GetConfiguredCandidateExpansionPanelSymbolsEnabled()
{
    return g_candidate_expansion_panel_symbols;
}

bool SetConfiguredCandidateExpansionPanelSymbolsEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "candidate_expansion_panel_symbols", enabled ? "true" : "false"))
        return false;
    g_candidate_expansion_panel_symbols = enabled;
    return true;
}

int GetConfiguredCandidateExpansionPanelTranslationMaxCount()
{
    return g_candidate_expansion_panel_translation_max_count;
}

bool SetConfiguredCandidateExpansionPanelTranslationMaxCount(int count)
{
    if (count < kCandidateExpansionPanelMaxCountMin || count > kCandidateExpansionPanelMaxCountMax)
        return false;
    if (!WriteConfiguredValue("general", "candidate_expansion_panel_translation_max_count", std::to_string(count)))
        return false;
    g_candidate_expansion_panel_translation_max_count = count;
    return true;
}

int GetConfiguredCandidateExpansionPanelEmojiMaxCount()
{
    return g_candidate_expansion_panel_emoji_max_count;
}

bool SetConfiguredCandidateExpansionPanelEmojiMaxCount(int count)
{
    if (count < kCandidateExpansionPanelMaxCountMin || count > kCandidateExpansionPanelMaxCountMax)
        return false;
    if (!WriteConfiguredValue("general", "candidate_expansion_panel_emoji_max_count", std::to_string(count)))
        return false;
    g_candidate_expansion_panel_emoji_max_count = count;
    return true;
}

int GetConfiguredCandidateExpansionPanelKaomojiMaxCount()
{
    return g_candidate_expansion_panel_kaomoji_max_count;
}

bool SetConfiguredCandidateExpansionPanelKaomojiMaxCount(int count)
{
    if (count < kCandidateExpansionPanelMaxCountMin || count > kCandidateExpansionPanelMaxCountMax)
        return false;
    if (!WriteConfiguredValue("general", "candidate_expansion_panel_kaomoji_max_count", std::to_string(count)))
        return false;
    g_candidate_expansion_panel_kaomoji_max_count = count;
    return true;
}

int GetConfiguredCandidateExpansionPanelSymbolsMaxCount()
{
    return g_candidate_expansion_panel_symbols_max_count;
}

bool SetConfiguredCandidateExpansionPanelSymbolsMaxCount(int count)
{
    if (count < kCandidateExpansionPanelMaxCountMin || count > kCandidateExpansionPanelMaxCountMax)
        return false;
    if (!WriteConfiguredValue("general", "candidate_expansion_panel_symbols_max_count", std::to_string(count)))
        return false;
    g_candidate_expansion_panel_symbols_max_count = count;
    return true;
}

const std::string &GetConfiguredCandidateExpansionPanelPlacement()
{
    return g_candidate_expansion_panel_placement;
}

bool SetConfiguredCandidateExpansionPanelPlacement(const std::string &placement)
{
    if (placement != kCandidateExpansionPanelPlacementDown && placement != kCandidateExpansionPanelPlacementRight)
        return false;
    if (!WriteConfiguredValue("general", "candidate_expansion_panel_placement", EscapeTomlBasicString(placement)))
        return false;
    g_candidate_expansion_panel_placement = placement;
    return true;
}

bool GetConfiguredCandidateTranslationShowPos()
{
    return g_candidate_translation_show_pos.load(std::memory_order_relaxed);
}

bool SetConfiguredCandidateTranslationShowPos(bool show)
{
    if (!WriteConfiguredValue("general", "candidate_translation_show_pos", show ? "true" : "false"))
        return false;
    g_candidate_translation_show_pos.store(show, std::memory_order_relaxed);
    return true;
}

bool GetConfiguredCandidateTranslationSingleSense()
{
    return g_candidate_translation_single_sense.load(std::memory_order_relaxed);
}

bool SetConfiguredCandidateTranslationSingleSense(bool single)
{
    if (!WriteConfiguredValue("general", "candidate_translation_single_sense", single ? "true" : "false"))
        return false;
    g_candidate_translation_single_sense.store(single, std::memory_order_relaxed);
    return true;
}

int GetConfiguredEnglishMixedInputMinChars()
{
    return g_english_mixed_input_min_chars;
}

bool SetConfiguredEnglishMixedInputMinChars(int min_chars)
{
    if (min_chars < kEnglishMixedInputMinCharsMin || min_chars > kEnglishMixedInputMinCharsMax)
        return false;
    if (!WriteConfiguredValue("general", "cn_en_mixed_input_min_chars", std::to_string(min_chars)))
        return false;
    g_english_mixed_input_min_chars = min_chars;
    return true;
}

int GetConfiguredEnglishMixedInputFirstPageCount()
{
    return g_english_mixed_input_first_page_count;
}

bool SetConfiguredEnglishMixedInputFirstPageCount(int count)
{
    if (count < kEnglishMixedInputFirstPageCountMin || count > kEnglishMixedInputFirstPageCountMax)
        return false;
    if (!WriteConfiguredValue("general", "cn_en_mixed_input_first_page_count", std::to_string(count)))
        return false;
    g_english_mixed_input_first_page_count = count;
    return true;
}

bool GetConfiguredEmojiMixedInputEnabled()
{
    return g_emoji_mixed_input_enabled;
}

bool SetConfiguredEmojiMixedInputEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "emoji_mixed_input", enabled ? "true" : "false"))
    {
        return false;
    }
    g_emoji_mixed_input_enabled = enabled;
    return true;
}

bool GetConfiguredKaomojiMixedInputEnabled()
{
    return g_kaomoji_mixed_input_enabled;
}

bool SetConfiguredKaomojiMixedInputEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "kaomoji_mixed_input", enabled ? "true" : "false"))
    {
        return false;
    }
    g_kaomoji_mixed_input_enabled = enabled;
    return true;
}

bool GetConfiguredSymbolMixedInputEnabled()
{
    return g_symbol_mixed_input_enabled;
}

bool SetConfiguredSymbolMixedInputEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "symbol_mixed_input", enabled ? "true" : "false"))
    {
        return false;
    }
    g_symbol_mixed_input_enabled = enabled;
    return true;
}

bool GetConfiguredCloudCandidatesEnabled()
{
    return g_cloud_candidates_enabled;
}

bool SetConfiguredCloudCandidatesEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "cloud_candidates", enabled ? "true" : "false"))
    {
        return false;
    }
    g_cloud_candidates_enabled = enabled;
    return true;
}

// 整句候选来源与去重补位开关的 getter / setter。写入 [association] 段，值域为布尔。
bool GetConfiguredAssocSentenceWordLattice()
{
    return g_assoc_sentence_wordlattice;
}
bool SetConfiguredAssocSentenceWordLattice(bool enabled)
{
    if (!WriteConfiguredValue("association", "sentence_wordlattice", enabled ? "true" : "false"))
    {
        return false;
    }
    g_assoc_sentence_wordlattice = enabled;
    return true;
}

bool GetConfiguredAssocSentenceGoogle()
{
    return g_assoc_sentence_google;
}
bool SetConfiguredAssocSentenceGoogle(bool enabled)
{
    if (!WriteConfiguredValue("association", "sentence_google", enabled ? "true" : "false"))
    {
        return false;
    }
    g_assoc_sentence_google = enabled;
    return true;
}

bool GetConfiguredAssocSentenceNeuralDesktop()
{
    return g_assoc_sentence_neural_desktop;
}
bool SetConfiguredAssocSentenceNeuralDesktop(bool enabled)
{
    if (!WriteConfiguredValue("association", "sentence_neural_desktop", enabled ? "true" : "false"))
    {
        return false;
    }
    g_assoc_sentence_neural_desktop = enabled;
    return true;
}

bool GetConfiguredAssocContextCandidateRerank()
{
    return g_assoc_context_candidate_rerank;
}
bool SetConfiguredAssocContextCandidateRerank(bool enabled)
{
    if (!WriteConfiguredValue("association", "context_candidate_rerank", enabled ? "true" : "false"))
        return false;
    g_assoc_context_candidate_rerank = enabled;
    return true;
}

bool GetConfiguredAssocSentenceNeuralKeyboard()
{
    return g_assoc_sentence_neural_keyboard;
}
bool SetConfiguredAssocSentenceNeuralKeyboard(bool enabled)
{
    if (!WriteConfiguredValue("association", "sentence_neural_keyboard", enabled ? "true" : "false"))
    {
        return false;
    }
    g_assoc_sentence_neural_keyboard = enabled;
    return true;
}

// octagram 语法模型总开关。开启时解码期叠加字级搭配分，并在 n-best 上重排出整句来源行
// （标签由模型包自带：万象〔万象〕、八股文〔八股〕、白霜〔墨奇〕）。引擎侧加成没有独立
// 开关——它由 collocation_model 非空隐含开启，所以单个键就够。
bool GetConfiguredAssocSentenceCollocationEnabled()
{
    return g_assoc_sentence_collocation_enabled;
}
bool SetConfiguredAssocSentenceCollocationEnabled(bool enabled)
{
    if (!WriteConfiguredValue("association", "sentence_collocation_enabled", enabled ? "true" : "false"))
    {
        return false;
    }
    g_assoc_sentence_collocation_enabled = enabled;
    return true;
}

std::string GetConfiguredAssocSentenceCollocationModel()
{
    return g_assoc_sentence_collocation_model;
}

// 激活某个 octagram 模型包（单选）。空串 = 未选择任何模型（整句加成关闭），设置页单选
// 其他包时写入该包 id。写入经配置三重传导到 Server，下一次击键 ApplyConfiguration 即
// 生效，无需重启。
bool SetConfiguredAssocSentenceCollocationModel(const std::string &model_id)
{
    if (!WriteConfiguredValue("association", "sentence_collocation_model", EscapeTomlBasicString(model_id)))
    {
        return false;
    }
    g_assoc_sentence_collocation_model = model_id;
    return true;
}

double GetConfiguredAssocSentenceCollocationWeight()
{
    return g_assoc_sentence_collocation_weight;
}

int GetConfiguredAssocSentenceCollocationMaxLength()
{
    return g_assoc_sentence_collocation_max_length;
}

int GetConfiguredAssocSentenceCollocationMinLength()
{
    return g_assoc_sentence_collocation_min_length;
}

double GetConfiguredAssocSentenceCollocationPenalty()
{
    return g_assoc_sentence_collocation_penalty;
}

double GetConfiguredAssocSentenceCollocationNonPenalty()
{
    return g_assoc_sentence_collocation_non_penalty;
}

double GetConfiguredAssocSentenceCollocationWeakPenalty()
{
    return g_assoc_sentence_collocation_weak_penalty;
}

double GetConfiguredAssocSentenceCollocationRearPenalty()
{
    return g_assoc_sentence_collocation_rear_penalty;
}

bool GetConfiguredAssocSentenceShowNextOnDuplicate()
{
    return g_assoc_sentence_show_next_on_duplicate;
}
bool SetConfiguredAssocSentenceShowNextOnDuplicate(bool enabled)
{
    if (!WriteConfiguredValue("association", "sentence_show_next_on_duplicate", enabled ? "true" : "false"))
    {
        return false;
    }
    g_assoc_sentence_show_next_on_duplicate = enabled;
    return true;
}

// 只影响候选窗展示，组页时读取，不需要重建引擎会话。
bool GetConfiguredAssocSentenceSourceBadge()
{
    return g_assoc_sentence_source_badge;
}
bool SetConfiguredAssocSentenceSourceBadge(bool enabled)
{
    if (!WriteConfiguredValue("association", "sentence_source_badge", enabled ? "true" : "false"))
    {
        return false;
    }
    g_assoc_sentence_source_badge = enabled;
    return true;
}

bool GetConfiguredRadicalModeEnabled()
{
    return g_radical_mode_enabled;
}

bool SetConfiguredRadicalModeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "radical_mode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_radical_mode_enabled = enabled;
    return true;
}

bool GetConfiguredQuickPhraseEnabled()
{
    return g_quick_phrase_enabled;
}

bool SetConfiguredQuickPhraseEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "quick_phrase", enabled ? "true" : "false"))
    {
        return false;
    }
    g_quick_phrase_enabled = enabled;
    return true;
}

bool GetConfiguredQuickPhraseCandidatesEnabled()
{
    return g_quick_phrase_candidates_enabled;
}

bool SetConfiguredQuickPhraseCandidatesEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "quick_phrase_candidates", enabled ? "true" : "false"))
    {
        return false;
    }
    g_quick_phrase_candidates_enabled = enabled;
    return true;
}

bool GetConfiguredQuickPhraseFrequencyEnabled()
{
    return g_quick_phrase_frequency_enabled;
}

bool SetConfiguredQuickPhraseFrequencyEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "quick_phrase_frequency", enabled ? "true" : "false"))
    {
        return false;
    }
    g_quick_phrase_frequency_enabled = enabled;
    return true;
}

bool GetConfiguredMixedCandidatesEnabled()
{
    return g_mixed_candidates_enabled;
}

bool SetConfiguredMixedCandidatesEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "mixed_candidates", enabled ? "true" : "false"))
    {
        return false;
    }
    g_mixed_candidates_enabled = enabled;
    return true;
}

bool GetConfiguredDateTimeCandidatesEnabled()
{
    return g_date_time_candidates_enabled;
}

bool SetConfiguredDateTimeCandidatesEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "date_time_candidates", enabled ? "true" : "false"))
    {
        return false;
    }
    g_date_time_candidates_enabled = enabled;
    return true;
}

bool GetConfiguredDateTimeMenuEnabled()
{
    return g_date_time_menu_enabled;
}

bool SetConfiguredDateTimeMenuEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "date_time_menu", enabled ? "true" : "false"))
    {
        return false;
    }
    g_date_time_menu_enabled = enabled;
    return true;
}

bool GetConfiguredDateTimeModeEnabled()
{
    return g_date_time_mode_enabled;
}

bool SetConfiguredDateTimeModeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "date_time_mode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_date_time_mode_enabled = enabled;
    return true;
}

bool GetConfiguredEmojiModeEnabled()
{
    return g_emoji_mode_enabled;
}

bool SetConfiguredEmojiModeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "emoji_mode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_emoji_mode_enabled = enabled;
    return true;
}

bool GetConfiguredKaomojiModeEnabled()
{
    return g_kaomoji_mode_enabled;
}

bool SetConfiguredKaomojiModeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "kaomoji_mode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_kaomoji_mode_enabled = enabled;
    return true;
}

bool GetConfiguredSymbolModeEnabled()
{
    return g_symbol_mode_enabled;
}

bool SetConfiguredSymbolModeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "symbol_mode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_symbol_mode_enabled = enabled;
    return true;
}

bool GetConfiguredJianpinModeEnabled()
{
    return g_jianpin_mode_enabled;
}

bool SetConfiguredJianpinModeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "jianpin_mode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_jianpin_mode_enabled = enabled;
    return true;
}

bool GetConfiguredYModeEnabled()
{
    return g_y_mode_enabled;
}

bool SetConfiguredYModeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "y_mode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_y_mode_enabled = enabled;
    return true;
}

bool GetConfiguredRModeEnabled()
{
    return g_r_mode_enabled;
}

bool SetConfiguredRModeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "r_mode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_r_mode_enabled = enabled;
    return true;
}

bool GetConfiguredVModeEnabled()
{
    return g_v_mode_enabled;
}

bool SetConfiguredVModeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "v_mode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_v_mode_enabled = enabled;
    return true;
}

std::wstring FormatVModeWorkerPayload()
{
    using FanyImeVModeInput::Trigger;
    // 日语模式下当前方案是 JapaneseRomaji，自然落到 Off。
    // 五笔与双拼同款：只认大写 V。小写 v 是合法码字，v+数字现状是「码 v 选第 N 个候选」，
    // 大小写都认会把它变成 V 模式，破坏选词。
    const SchemeType scheme = GetConfiguredActiveInputScheme();
    const Trigger trigger = !g_v_mode_enabled                 ? Trigger::Off
                            : scheme == SchemeType::Quanpin   ? Trigger::AnyCase
                            : scheme == SchemeType::Shuangpin ? Trigger::UppercaseOnly
                            : scheme == SchemeType::Wubi      ? Trigger::UppercaseOnly
                                                              : Trigger::Off;
    return std::wstring(1, FanyImeVModeInput::PayloadFromTrigger(trigger));
}

bool GetConfiguredClipboardHistoryEnabled()
{
    return g_clipboard_history_enabled;
}

bool SetConfiguredClipboardHistoryEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "clipboard_history", enabled ? "true" : "false"))
    {
        return false;
    }
    const bool was_enabled = g_clipboard_history_enabled;
    g_clipboard_history_enabled = enabled;
    if (was_enabled && !enabled)
        ClipboardHistory::Clear();
    ClipboardMonitor::Sync(enabled);
    return true;
}

bool GetConfiguredLanguageBarEnabled()
{
    return g_language_bar_enabled;
}

bool SetConfiguredLanguageBarEnabled(bool enabled)
{
    if (!WriteConfiguredValue("language_bar", "enabled", enabled ? "true" : "false"))
    {
        return false;
    }
    g_language_bar_enabled = enabled;
    return true;
}

bool GetConfiguredLanguageBarShowImeMode()
{
    return g_language_bar_show_ime_mode;
}

bool SetConfiguredLanguageBarShowImeMode(bool enabled)
{
    if (!WriteConfiguredValue("language_bar", "show_ime_mode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_language_bar_show_ime_mode = enabled;
    return true;
}

bool GetConfiguredLanguageBarShowDoubleSingleByte()
{
    return g_language_bar_show_double_single_byte;
}

bool SetConfiguredLanguageBarShowDoubleSingleByte(bool enabled)
{
    if (!WriteConfiguredValue("language_bar", "show_double_single_byte", enabled ? "true" : "false"))
    {
        return false;
    }
    g_language_bar_show_double_single_byte = enabled;
    return true;
}

bool GetConfiguredLanguageBarShowPunctuation()
{
    return g_language_bar_show_punctuation;
}

bool SetConfiguredLanguageBarShowPunctuation(bool enabled)
{
    if (!WriteConfiguredValue("language_bar", "show_punctuation", enabled ? "true" : "false"))
    {
        return false;
    }
    g_language_bar_show_punctuation = enabled;
    return true;
}
