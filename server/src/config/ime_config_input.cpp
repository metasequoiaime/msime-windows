// 输入方案与输入行为配置：方案、中日文模式、简繁、中英切换键、双拼与五笔方案、上屏样式、辅助码、全拼纠错、模糊音和调频。
#include "config/ime_config_internal.h"
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>
#include "global/globals.h"
#include "defines/defines.h"
#include "engine/common/helpcode_utils.h"
#include "engine/contracts/direct_helpcode.h"
#include "engine/core/data_path.h"
#include "engine/shuangpin/shuangpin_profile.h"

using namespace ime_config_detail;

SchemeType GetConfiguredInputScheme()
{
    return g_input_scheme;
}

SchemeType GetConfiguredActiveInputScheme()
{
    return GetActiveInputMode() == "japanese" ? SchemeType::JapaneseRomaji : g_input_scheme;
}

std::string GetConfiguredInputSchemeName()
{
    switch (g_input_scheme)
    {
    case SchemeType::Quanpin:
        return "quanpin";
    case SchemeType::Shuangpin:
        return "shuangpin";
    case SchemeType::Wubi:
        return "wubi";
    default:
        return "shuangpin";
    }
}

bool SetConfiguredInputScheme(const std::string &scheme)
{
    if (scheme != "quanpin" && scheme != "shuangpin" && scheme != "wubi")
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "schema", EscapeTomlBasicString(scheme)))
    {
        return false;
    }
    g_input_scheme = ParseScheme(scheme);
    RefreshEffectiveTsfPreeditStyle();
    NotifyImeServerInputSchemeChanged();
    return true;
}

const std::string &GetConfiguredCharacterSet()
{
    return g_character_set;
}

bool SetConfiguredCharacterSet(const std::string &character_set)
{
    if (character_set != "simplified" && character_set != "traditional")
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "character_set", EscapeTomlBasicString(character_set)))
    {
        return false;
    }
    g_character_set = character_set;
    // Marshal WebView/native toolbar refreshes to their owner thread. Settings
    // in a separate process reaches the same refresh through ConfigChanged.
    NotifyImeServer(WM_REFRESH_CHARACTER_SET, L"ConfigChanged");
    return true;
}

bool GetConfiguredCharacterSetShortcutEnabled()
{
    return g_character_set_shortcut_enabled;
}

bool SetConfiguredCharacterSetShortcutEnabled(bool enabled)
{
    if (!WriteConfiguredValue("keybindings", "toggle_character_set_ctrl_shift_f", enabled ? "true" : "false"))
        return false;
    g_character_set_shortcut_enabled = enabled;
    return true;
}

const std::string &GetConfiguredDefaultImeMode()
{
    return g_default_ime_mode;
}

bool SetConfiguredDefaultImeMode(const std::string &mode)
{
    if (mode != "chinese" && mode != "english")
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "default_ime_mode", EscapeTomlBasicString(mode)))
    {
        return false;
    }
    g_default_ime_mode = mode;
    return true;
}

const std::string &GetConfiguredImeModeScope()
{
    return g_ime_mode_scope;
}

bool SetConfiguredImeModeScope(const std::string &scope)
{
    if (scope != "app" && scope != "global")
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "ime_mode_scope", EscapeTomlBasicString(scope)))
    {
        return false;
    }
    g_ime_mode_scope = scope;
    return true;
}

bool IsConfiguredImeModeScopeGlobal()
{
    return g_ime_mode_scope == "global";
}

bool GetConfiguredSwitchLanguageShiftEnabled()
{
    return g_switch_language_shift_enabled;
}

bool SetConfiguredSwitchLanguageShiftEnabled(bool enabled)
{
    if (!WriteConfiguredValue("keybindings", "switch_language_shift", enabled ? "true" : "false"))
    {
        return false;
    }
    g_switch_language_shift_enabled = enabled;
    return true;
}

bool GetConfiguredSwitchLanguageCtrlEnabled()
{
    return g_switch_language_ctrl_enabled;
}

bool SetConfiguredSwitchLanguageCtrlEnabled(bool enabled)
{
    if (!WriteConfiguredValue("keybindings", "switch_language_ctrl", enabled ? "true" : "false"))
    {
        return false;
    }
    g_switch_language_ctrl_enabled = enabled;
    return true;
}

bool GetConfiguredSwitchLanguageCtrlAltSpaceEnabled()
{
    return g_switch_language_ctrl_alt_space_enabled;
}

bool SetConfiguredSwitchLanguageCtrlAltSpaceEnabled(bool enabled)
{
    if (!WriteConfiguredValue("keybindings", "switch_language_ctrl_alt_space", enabled ? "true" : "false"))
    {
        return false;
    }
    g_switch_language_ctrl_alt_space_enabled = enabled;
    return true;
}

const std::string &GetConfiguredShuangpinSchema()
{
    return g_shuangpin_schema;
}

bool IsConfiguredShuangpinSemicolonFinal()
{
    return ShuangpinProfileUsesSemicolonFinal(GetShuangpinProfile(g_shuangpin_schema));
}

bool SetConfiguredShuangpinSchema(const std::string &schema)
{
    if (!LoadShuangpinSchema(schema))
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "shuangpin_schema", EscapeTomlBasicString(schema)))
    {
        return false;
    }
    g_shuangpin_schema = schema;
    return true;
}

const std::string &GetConfiguredWubiSchema()
{
    return g_wubi_schema;
}

bool SetConfiguredWubiSchema(const std::string &schema)
{
    if (schema != "wubi86")
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "wubi_schema", EscapeTomlBasicString(schema)))
    {
        return false;
    }
    g_wubi_schema = schema;
    return true;
}

const std::string &GetConfiguredWubiZMode()
{
    return g_wubi_z_mode;
}

bool SetConfiguredWubiZMode(const std::string &mode)
{
    if (mode != "off" && mode != "wildcard")
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "wubi_z_mode", EscapeTomlBasicString(mode)))
    {
        return false;
    }
    g_wubi_z_mode = mode;
    return true;
}

bool GetConfiguredWubiMixedPinyin()
{
    return g_wubi_mixed_pinyin;
}

bool SetConfiguredWubiMixedPinyin(bool enabled)
{
    if (!WriteConfiguredValue("input", "wubi_mixed_pinyin", enabled ? "true" : "false"))
    {
        return false;
    }
    g_wubi_mixed_pinyin = enabled;
    return true;
}

bool GetConfiguredWubiFourCodeAutoCommit()
{
    return g_wubi_four_code_auto_commit;
}

bool SetConfiguredWubiFourCodeAutoCommit(bool enabled)
{
    if (!WriteConfiguredValue("input", "wubi_four_code_auto_commit", enabled ? "true" : "false"))
    {
        return false;
    }
    g_wubi_four_code_auto_commit = enabled;
    return true;
}

bool GetConfiguredWubiFifthCodeTopCommit()
{
    return g_wubi_fifth_code_top_commit;
}

bool SetConfiguredWubiFifthCodeTopCommit(bool enabled)
{
    if (!WriteConfiguredValue("input", "wubi_fifth_code_top_commit", enabled ? "true" : "false"))
    {
        return false;
    }
    g_wubi_fifth_code_top_commit = enabled;
    return true;
}

bool GetConfiguredEscapeKeepsSelectedWord()
{
    return g_escape_keeps_selected_word;
}

bool SetConfiguredEscapeKeepsSelectedWord(bool enabled)
{
    if (!WriteConfiguredValue("input", "escape_keeps_selected_word", enabled ? "true" : "false"))
    {
        return false;
    }
    g_escape_keeps_selected_word = enabled;
    return true;
}

bool GetConfiguredEnterLearnsEnglishWord()
{
    return g_enter_learns_english_word;
}

bool SetConfiguredEnterLearnsEnglishWord(bool enabled)
{
    if (!WriteConfiguredValue("input", "enter_learns_english_word", enabled ? "true" : "false"))
    {
        return false;
    }
    g_enter_learns_english_word = enabled;
    return true;
}

const std::string &GetConfiguredShuangpinPreeditMode()
{
    return g_shuangpin_preedit_mode;
}

const std::string &GetConfiguredTsfPreeditStyle()
{
    return g_tsf_preedit_style;
}

bool SetConfiguredTsfPreeditStyle(const std::string &style)
{
    if (!GlobalSettings::isKnownTsfPreeditStyle(style))
    {
        return false;
    }
    if (!WriteConfiguredValue("appearance", "tsf_preedit_style", EscapeTomlBasicString(style)))
    {
        return false;
    }
    g_tsf_preedit_style = style;
    RefreshEffectiveTsfPreeditStyle();
    return true;
}

bool GetConfiguredTsfPreeditShuangpinQuanpin()
{
    return g_tsf_preedit_shuangpin_quanpin;
}

bool SetConfiguredTsfPreeditShuangpinQuanpin(bool enabled)
{
    if (!WriteConfiguredValue("appearance", "tsf_preedit_shuangpin_quanpin", enabled ? "true" : "false"))
    {
        return false;
    }
    g_tsf_preedit_shuangpin_quanpin = enabled;
    RefreshEffectiveTsfPreeditStyle();
    return true;
}

namespace ime_config_detail
{
void RefreshEffectiveTsfPreeditStyle()
{
    // 原始按键样式下 TSF 自己显示按键缓冲，从不等 Server 的预编辑；双拼要显示转换后的全拼就得
    // 让它按分词样式等回包，回包里再按用户选的样式去掉分词符号（见 BuildTsfPreedit）。
    const bool shuangpin_quanpin = g_tsf_preedit_shuangpin_quanpin && g_tsf_preedit_style == "raw" &&
                                   GetConfiguredActiveInputScheme() == SchemeType::Shuangpin;
    GlobalSettings::setTsfPreeditStyle(shuangpin_quanpin ? std::string(GlobalSettings::TsfPreeditStyle::Pinyin)
                                                         : g_tsf_preedit_style);
}
} // namespace ime_config_detail

bool GetConfiguredShuangpinHelpcodeEnabled()
{
    return g_shuangpin_helpcode_enabled;
}

bool SetConfiguredShuangpinHelpcodeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("helpcode", "shuangpin_helpcode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_shuangpin_helpcode_enabled = enabled;
    return true;
}

bool GetConfiguredShuangpinMidSentenceHelpcodeEnabled()
{
    return g_shuangpin_mid_sentence_helpcode_enabled;
}

bool SetConfiguredShuangpinMidSentenceHelpcodeEnabled(bool enabled)
{
    // 句中辅助码与直接辅助码互斥：开这个就先关掉那个，设置页随回推的配置快照一起更新。先关后开，
    // 写到一半失败时最多两个都关着，不会出现两个都开着。
    if (enabled && g_shuangpin_direct_helpcode_enabled)
    {
        if (!WriteConfiguredValue("helpcode", "shuangpin_direct_helpcode", "false"))
        {
            return false;
        }
        g_shuangpin_direct_helpcode_enabled = false;
    }
    if (!WriteConfiguredValue("helpcode", "shuangpin_mid_sentence_helpcode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_shuangpin_mid_sentence_helpcode_enabled = enabled;
    return true;
}

bool GetConfiguredShuangpinMidSentenceHelpcodeBacktick()
{
    return g_shuangpin_mid_sentence_helpcode_backtick;
}

bool SetConfiguredShuangpinMidSentenceHelpcodeBacktick(bool enabled)
{
    if (!WriteConfiguredValue("helpcode", "shuangpin_mid_sentence_helpcode_backtick", enabled ? "true" : "false"))
    {
        return false;
    }
    g_shuangpin_mid_sentence_helpcode_backtick = enabled;
    return true;
}

bool GetConfiguredShuangpinMidSentenceHelpcodeSemicolon()
{
    return g_shuangpin_mid_sentence_helpcode_semicolon;
}

bool SetConfiguredShuangpinMidSentenceHelpcodeSemicolon(bool enabled)
{
    if (!WriteConfiguredValue("helpcode", "shuangpin_mid_sentence_helpcode_semicolon", enabled ? "true" : "false"))
    {
        return false;
    }
    g_shuangpin_mid_sentence_helpcode_semicolon = enabled;
    return true;
}

bool GetConfiguredShuangpinMidSentenceHelpcodeUppercase()
{
    return g_shuangpin_mid_sentence_helpcode_uppercase;
}

bool SetConfiguredShuangpinMidSentenceHelpcodeUppercase(bool enabled)
{
    if (!WriteConfiguredValue("helpcode", "shuangpin_mid_sentence_helpcode_uppercase", enabled ? "true" : "false"))
    {
        return false;
    }
    g_shuangpin_mid_sentence_helpcode_uppercase = enabled;
    return true;
}

bool IsConfiguredMidSentenceHelpcodeUppercaseTrigger()
{
    // 与反引号、分号同一套门控：直接辅助码接管辅码时句中辅助码的触发键都不生效。
    return g_shuangpin_mid_sentence_helpcode_enabled && g_shuangpin_mid_sentence_helpcode_uppercase &&
           !g_shuangpin_direct_helpcode_enabled;
}

std::wstring FormatMidSentenceHelpcodeUppercaseWorkerPayload()
{
    return IsConfiguredMidSentenceHelpcodeUppercaseTrigger() &&
                   GetConfiguredActiveInputScheme() == SchemeType::Shuangpin
               ? L"1"
               : L"0";
}

bool GetConfiguredShuangpinDirectHelpcodeEnabled()
{
    return g_shuangpin_direct_helpcode_enabled;
}

bool SetConfiguredShuangpinDirectHelpcodeEnabled(bool enabled)
{
    // 与句中辅助码互斥，规则同 SetConfiguredShuangpinMidSentenceHelpcodeEnabled。
    if (enabled && g_shuangpin_mid_sentence_helpcode_enabled)
    {
        if (!WriteConfiguredValue("helpcode", "shuangpin_mid_sentence_helpcode", "false"))
        {
            return false;
        }
        g_shuangpin_mid_sentence_helpcode_enabled = false;
    }
    if (!WriteConfiguredValue("helpcode", "shuangpin_direct_helpcode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_shuangpin_direct_helpcode_enabled = enabled;
    return true;
}

bool GetConfiguredShuangpinDirectHelpcodeSlash()
{
    return g_shuangpin_direct_helpcode_slash;
}

bool SetConfiguredShuangpinDirectHelpcodeSlash(bool enabled)
{
    // 至少留一个：设置页取消不掉最后一个，这里同样不收。
    if (!enabled && !g_shuangpin_direct_helpcode_uppercase)
    {
        return false;
    }
    if (!WriteConfiguredValue("helpcode", "shuangpin_direct_helpcode_slash", enabled ? "true" : "false"))
    {
        return false;
    }
    g_shuangpin_direct_helpcode_slash = enabled;
    return true;
}

bool GetConfiguredShuangpinDirectHelpcodeUppercase()
{
    return g_shuangpin_direct_helpcode_uppercase;
}

bool SetConfiguredShuangpinDirectHelpcodeUppercase(bool enabled)
{
    if (!enabled && !g_shuangpin_direct_helpcode_slash)
    {
        return false;
    }
    if (!WriteConfiguredValue("helpcode", "shuangpin_direct_helpcode_uppercase", enabled ? "true" : "false"))
    {
        return false;
    }
    g_shuangpin_direct_helpcode_uppercase = enabled;
    return true;
}

std::wstring FormatDirectHelpcodeWorkerPayload()
{
    const bool enabled =
        g_shuangpin_direct_helpcode_enabled && GetConfiguredActiveInputScheme() == SchemeType::Shuangpin;
    return std::wstring(1, FanyImeDirectHelpcode::PayloadFor(enabled, g_shuangpin_direct_helpcode_slash));
}

bool IsConfiguredMidSentenceHelpcodeTrigger(wchar_t ch)
{
    // 直接辅助码开着时引擎不收反引号段（两套辅码会把同一个音节约束两遍），触发键跟着失效。
    if (!g_shuangpin_mid_sentence_helpcode_enabled || g_shuangpin_direct_helpcode_enabled)
    {
        return false;
    }
    return (ch == L'`' && g_shuangpin_mid_sentence_helpcode_backtick) ||
           (ch == L';' && g_shuangpin_mid_sentence_helpcode_semicolon);
}

std::wstring FormatMidSentenceHelpcodeWorkerPayload()
{
    // TSF 只需要知道反引号此刻有没有可能是编码键：开关开着、勾了反引号且正在用双拼。
    return IsConfiguredMidSentenceHelpcodeTrigger(L'`') && GetConfiguredActiveInputScheme() == SchemeType::Shuangpin
               ? L"1"
               : L"0";
}

std::wstring FormatMidSentenceHelpcodeSemicolonWorkerPayload()
{
    return IsConfiguredMidSentenceHelpcodeTrigger(L';') && GetConfiguredActiveInputScheme() == SchemeType::Shuangpin
               ? L"1"
               : L"0";
}

const std::string &GetConfiguredShuangpinHelpcodeSchema()
{
    return g_shuangpin_helpcode_schema;
}

bool SetConfiguredShuangpinHelpcodeSchema(const std::string &schema)
{
    if (!IsHelpcodeSchemaAvailable(schema))
        return false;
    if (!WriteConfiguredValue("helpcode", "shuangpin_helpcode_schema", EscapeTomlBasicString(schema)))
        return false;
    g_shuangpin_helpcode_schema = schema;
    return true;
}

bool GetConfiguredQuanpinHelpcodeEnabled()
{
    return g_quanpin_helpcode_enabled;
}

bool SetConfiguredQuanpinHelpcodeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("helpcode", "quanpin_helpcode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_quanpin_helpcode_enabled = enabled;
    return true;
}

bool GetConfiguredQuanpinAutocorrectTransposition()
{
    return g_quanpin_autocorrect_transposition;
}

bool SetConfiguredQuanpinAutocorrectTransposition(bool enabled)
{
    if (!WriteConfiguredValue("quanpin", "autocorrect_transposition", enabled ? "true" : "false"))
    {
        return false;
    }
    g_quanpin_autocorrect_transposition = enabled;
    return true;
}

bool GetConfiguredQuanpinAutocorrectNeighbor()
{
    return g_quanpin_autocorrect_neighbor;
}

bool SetConfiguredQuanpinAutocorrectNeighbor(bool enabled)
{
    if (!WriteConfiguredValue("quanpin", "autocorrect_neighbor", enabled ? "true" : "false"))
    {
        return false;
    }
    g_quanpin_autocorrect_neighbor = enabled;
    return true;
}

bool GetConfiguredQuanpinAutocorrectMarker()
{
    return g_quanpin_autocorrect_marker;
}

bool SetConfiguredQuanpinAutocorrectMarker(bool enabled)
{
    if (!WriteConfiguredValue("quanpin", "autocorrect_marker", enabled ? "true" : "false"))
    {
        return false;
    }
    g_quanpin_autocorrect_marker = enabled;
    return true;
}

bool GetConfiguredFuzzyPinyinEnabled()
{
    return g_fuzzy_pinyin_enabled;
}

bool SetConfiguredFuzzyPinyinEnabled(bool enabled)
{
    // 首次启用播种：出厂态第一次开总开关，11 条规则全部置 true 并持久化；之后总开关的
    // 任何翻动只写总开关一个键，用户修剪过的选择在临时停用/恢复间原样保留。不能拿
    // 「规则位全零」当首次信号——用户故意全部取消勾选后位图同样是零，无标记会把每次
    // 开启都误判成首次启用，反复改写用户的空选择。
    if (enabled && !g_fuzzy_seeded)
    {
        std::vector<ConfigValueUpdate> updates;
        updates.reserve(std::size(kFuzzyPinyinRuleKeys) + 2);
        updates.push_back({"input", "fuzzy_pinyin", "true"});
        updates.push_back({"input", "fuzzy_seeded", "true"});
        for (const auto &entry : kFuzzyPinyinRuleKeys)
            updates.push_back({"input", entry.key, "true"});
        // 一次批量写：WriteConfiguredValues 走单文件临时改名，无部分失败态。
        if (!WriteConfiguredValues(updates))
            return false;
        g_fuzzy_pinyin_enabled = true;
        g_fuzzy_seeded = true;
        for (const auto &entry : kFuzzyPinyinRuleKeys)
            g_fuzzy_pinyin_rules |= static_cast<std::uint32_t>(entry.rule);
        return true;
    }
    if (!WriteConfiguredValue("input", "fuzzy_pinyin", enabled ? "true" : "false"))
        return false;
    g_fuzzy_pinyin_enabled = enabled;
    return true;
}

// The master switch is gated here and nowhere else: sessions see all-zero rules while it is
// off, and the cached rule bits survive the toggle so re-enabling restores the prior choice.
metasequoia::FuzzyPinyinOptions GetConfiguredFuzzyPinyinOptions()
{
    metasequoia::FuzzyPinyinOptions options;
    if (g_fuzzy_pinyin_enabled)
        options.rules = g_fuzzy_pinyin_rules;
    return options;
}

metasequoia::FuzzyPinyinOptions GetConfiguredFuzzyPinyinRuleStates()
{
    metasequoia::FuzzyPinyinOptions options;
    options.rules = g_fuzzy_pinyin_rules;
    return options;
}

bool SetConfiguredFuzzyPinyinRule(const std::string &key, bool enabled)
{
    const FuzzyPinyinRuleKey *entry = nullptr;
    for (const auto &candidate : kFuzzyPinyinRuleKeys)
    {
        if (key == candidate.key)
        {
            entry = &candidate;
            break;
        }
    }
    if (!entry)
        return false;
    const auto bit = static_cast<std::uint32_t>(entry->rule);
    if (!WriteConfiguredValue("input", entry->key, enabled ? "true" : "false"))
        return false;
    if (enabled)
        g_fuzzy_pinyin_rules |= bit;
    else
        g_fuzzy_pinyin_rules &= ~bit;
    return true;
}

const std::string &GetConfiguredQuanpinHelpcodeSchema()
{
    return g_quanpin_helpcode_schema;
}

bool SetConfiguredQuanpinHelpcodeSchema(const std::string &schema)
{
    if (!IsHelpcodeSchemaAvailable(schema))
        return false;
    if (!WriteConfiguredValue("helpcode", "quanpin_helpcode_schema", EscapeTomlBasicString(schema)))
        return false;
    g_quanpin_helpcode_schema = schema;
    return true;
}

std::vector<CustomHelpcodeSchemaInfo> GetCustomHelpcodeSchemas()
{
    std::vector<CustomHelpcodeSchemaInfo> result;
    for (const auto &schema : HelpcodeUtils::list_custom_helpcode_schemas(metasequoia::data_directory()))
    {
        const std::string &name = schema.name.empty() ? schema.name_en : schema.name;
        const std::string &name_en = schema.name_en.empty() ? schema.name : schema.name_en;
        result.push_back(
            {schema.schema, name.empty() ? schema.file_stem : name, name_en.empty() ? schema.file_stem : name_en});
    }
    return result;
}

std::string GetCustomHelpcodeDirectory()
{
    return metasequoia::path_to_utf8(HelpcodeUtils::custom_helpcode_directory(metasequoia::data_directory()));
}

bool GetConfiguredShowShuangpinHelpcodeInCandidateWindow()
{
    return g_show_shuangpin_helpcode_in_candidate_window;
}

bool SetConfiguredShowShuangpinHelpcodeInCandidateWindow(bool enabled)
{
    if (!WriteConfiguredValue("helpcode", "show_sp_helpcode_in_candidate_window", enabled ? "true" : "false"))
    {
        return false;
    }
    g_show_shuangpin_helpcode_in_candidate_window = enabled;
    return true;
}

bool GetConfiguredShowQuanpinHelpcodeInCandidateWindow()
{
    return g_show_quanpin_helpcode_in_candidate_window;
}

bool SetConfiguredShowQuanpinHelpcodeInCandidateWindow(bool enabled)
{
    if (!WriteConfiguredValue("helpcode", "show_qp_helpcode_in_candidate_window", enabled ? "true" : "false"))
    {
        return false;
    }
    g_show_quanpin_helpcode_in_candidate_window = enabled;
    return true;
}

const std::string &GetConfiguredInputMode()
{
    return g_input_mode;
}

bool SetConfiguredInputMode(const std::string &mode)
{
    if (mode != "chinese" && mode != "japanese")
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "mode", EscapeTomlBasicString(mode)))
    {
        return false;
    }
    g_input_mode = mode;
    g_configured_input_mode_japanese.store(mode == "japanese", std::memory_order_relaxed);
    // Choosing a language in settings is where the next cycle starts from;
    // the cycle switch itself is independent of it.
    g_active_input_mode.store(ActiveInputMode::Configured, std::memory_order_relaxed);
    RefreshEffectiveTsfPreeditStyle();
    NotifyImeServerInputSchemeChanged();
    return true;
}

bool GetConfiguredTrilingualCycleEnabled()
{
    return g_trilingual_cycle_enabled.load(std::memory_order_relaxed);
}

bool SetConfiguredTrilingualCycleEnabled(bool enabled)
{
    if (!WriteConfiguredValue("keybindings", "trilingual_cycle", enabled ? "true" : "false"))
    {
        return false;
    }
    if (g_trilingual_cycle_enabled.exchange(enabled, std::memory_order_relaxed) != enabled)
        g_active_input_mode.store(ActiveInputMode::Configured, std::memory_order_relaxed);
    RefreshEffectiveTsfPreeditStyle();
    return true;
}

const std::string &GetActiveInputMode()
{
    static const std::string chinese = "chinese";
    static const std::string japanese = "japanese";
    switch (g_active_input_mode.load(std::memory_order_relaxed))
    {
    case ActiveInputMode::Chinese:
        return chinese;
    case ActiveInputMode::Japanese:
        return japanese;
    default:
        // Never hand out g_input_mode itself: a config reload on another
        // thread may reassign it while the key thread compares the result.
        return g_configured_input_mode_japanese.load(std::memory_order_relaxed) ? japanese : chinese;
    }
}

bool SetActiveInputMode(const std::string &mode)
{
    if (!g_trilingual_cycle_enabled.load(std::memory_order_relaxed) || (mode != "chinese" && mode != "japanese"))
    {
        return false;
    }
    g_active_input_mode.store(mode == "japanese" ? ActiveInputMode::Japanese : ActiveInputMode::Chinese,
                              std::memory_order_relaxed);
    RefreshEffectiveTsfPreeditStyle();
    return true;
}

const std::string &GetConfiguredJapaneseSchema()
{
    return g_japanese_schema;
}

bool SetConfiguredJapaneseSchema(const std::string &schema)
{
    if (schema != "romaji")
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "japanese_schema", EscapeTomlBasicString(schema)))
    {
        return false;
    }
    g_japanese_schema = schema;
    return true;
}

const FrequencyAdjustmentConfig &GetConfiguredFrequencyAdjustment()
{
    return g_frequency_adjustment;
}

bool SetConfiguredFrequencyAdjustmentString(const std::string &key, const std::string &value)
{
    if (key != "mode" ||
        (value != "disabled" && value != "pin" && value != "halve" && value != "linear" && value != "promote") ||
        !WriteConfiguredValue("frequency_adjustment", key, EscapeTomlBasicString(value)))
        return false;
    g_frequency_adjustment.mode = value;
    return true;
}

bool SetConfiguredFrequencyAdjustmentInt(const std::string &key, int value)
{
    if ((key != "trigger_count" && key != "linear_step") || value < 1 || value > 10 ||
        !WriteConfiguredValue("frequency_adjustment", key, std::to_string(value)))
        return false;
    if (key == "trigger_count")
        g_frequency_adjustment.trigger_count = value;
    else
        g_frequency_adjustment.linear_step = value;
    return true;
}
