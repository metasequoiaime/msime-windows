#pragma once

// ime_config*.cpp 之间共享的内部声明：拆分前同在 ime_config.cpp 匿名命名空间里、
// 现在跨文件使用的配置状态、常量与辅助函数。只给 server/src/config/ime_config*.cpp 包含，
// 其他地方不要引用。

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>
#include "config/ime_config.h"

namespace ime_config_detail
{
constexpr int kCandidateFontSizeMin = 12;
constexpr int kCandidateFontSizeMax = 32;
constexpr double kFloatingToolbarScaleMin = 0.75;
constexpr double kFloatingToolbarScaleMax = 1.5;
constexpr int kFloatingToolbarFontSizeMin = 16;
constexpr int kFloatingToolbarFontSizeMax = 28;
constexpr int kFloatingToolbarFontSizeDefault = 22;
constexpr int kFloatingToolbarAutoHideDelayMin = 1;
constexpr int kFloatingToolbarAutoHideDelayMax = 60;
constexpr int kFloatingToolbarAutoHideDelayDefault = 5;
constexpr int kEnglishMixedInputMinCharsMin = 1;
constexpr int kEnglishMixedInputMinCharsMax = 8;
constexpr int kEnglishMixedInputMinCharsDefault = 2;
constexpr int kEnglishMixedInputFirstPageCountMin = 1;
constexpr int kEnglishMixedInputFirstPageCountMax = 3;
constexpr int kEnglishMixedInputFirstPageCountDefault = 1;

// Config key ↔ engine rule bit. The key mirrors the enumerator name lowercased, so the
// only mapping to review is this table; the bit position lives in the enum itself.
struct FuzzyPinyinRuleKey
{
    const char *key;
    metasequoia::FuzzyPinyinRule rule;
};
constexpr FuzzyPinyinRuleKey kFuzzyPinyinRuleKeys[] = {
    {"fuzzy_z_zh", metasequoia::FuzzyPinyinRule::Z_ZH},
    {"fuzzy_c_ch", metasequoia::FuzzyPinyinRule::C_CH},
    {"fuzzy_s_sh", metasequoia::FuzzyPinyinRule::S_SH},
    {"fuzzy_n_l", metasequoia::FuzzyPinyinRule::N_L},
    {"fuzzy_f_h", metasequoia::FuzzyPinyinRule::F_H},
    {"fuzzy_r_l", metasequoia::FuzzyPinyinRule::R_L},
    {"fuzzy_an_ang", metasequoia::FuzzyPinyinRule::AN_ANG},
    {"fuzzy_en_eng", metasequoia::FuzzyPinyinRule::EN_ENG},
    {"fuzzy_in_ing", metasequoia::FuzzyPinyinRule::IN_ING},
    {"fuzzy_ian_iang", metasequoia::FuzzyPinyinRule::IAN_IANG},
    {"fuzzy_uan_uang", metasequoia::FuzzyPinyinRule::UAN_UANG},
};

class ConfigFileLock
{
  public:
    ConfigFileLock()
    {
        handle_ = CreateMutexW(nullptr, FALSE, L"Local\\MetasequoiaIme.ConfigFile");
        if (handle_)
        {
            const DWORD result = WaitForSingleObject(handle_, 5000);
            locked_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        }
    }

    ~ConfigFileLock()
    {
        if (locked_)
            ReleaseMutex(handle_);
        if (handle_)
            CloseHandle(handle_);
    }

    explicit operator bool() const
    {
        return locked_;
    }

  private:
    HANDLE handle_ = nullptr;
    bool locked_ = false;
};

SchemeType ParseScheme(const std::string &value);

// 安装包装入的本版出厂模板，以及上次合并时用的那份模板（升级基线）。
const char *const kConfigTemplateFileName = "config.default.toml";
const char *const kConfigBaselineFileName = "config.base.toml";

using TomlAssignmentVisitor =
    std::function<void(const std::string &section, const std::string &key, size_t value_begin, size_t value_end)>;

struct ConfigValueUpdate
{
    std::string section;
    std::string key;
    std::string value;
};

// 配置状态：定义与初始化顺序都在 ime_config.cpp。
extern SchemeType g_input_scheme;
extern std::string g_input_mode;
enum class ActiveInputMode
{
    Configured,
    Chinese,
    Japanese
};
// The key thread reads the language without the config lock. These mirror
// g_input_mode and [keybindings] trilingual_cycle for those lock-free reads.
extern std::atomic<bool> g_configured_input_mode_japanese;
extern std::atomic<ActiveInputMode> g_active_input_mode;
extern std::atomic<bool> g_trilingual_cycle_enabled;
extern std::string g_japanese_schema;
extern std::string g_character_set;
extern std::string g_default_ime_mode;
extern std::string g_ime_mode_scope;
extern bool g_switch_language_shift_enabled;
extern bool g_switch_language_ctrl_enabled;
extern bool g_switch_language_ctrl_alt_space_enabled;
extern bool g_character_set_shortcut_enabled;
extern bool g_commit_pinyin_shift_enter_enabled;
extern InputStateShortcutValues g_input_state_shortcuts;
extern int g_candidate_page_size;
extern std::string g_candidate_font;
extern std::string g_candidate_english_font;
extern std::string g_candidate_default_font;
extern std::vector<std::string> g_candidate_fallback_fonts;
extern int g_candidate_font_size;
extern int g_candidate_window_preedit_font_size;
extern std::atomic_bool g_diagnostic_log_enabled;
extern std::atomic_bool g_tsf_diagnostic_log_enabled;
extern std::atomic_bool g_statistics_enabled;
extern std::string g_statistics_retention;
extern std::string g_candidate_text_color;
extern std::string g_shuangpin_schema;
extern std::string g_wubi_schema;
extern bool g_wubi_mixed_pinyin;
extern bool g_escape_keeps_selected_word;
extern bool g_enter_learns_english_word;
extern std::string g_wubi_z_mode;
extern bool g_wubi_four_code_auto_commit;
extern bool g_wubi_fifth_code_top_commit;
extern std::string g_shuangpin_preedit_mode;
extern std::string g_tsf_preedit_style;
extern bool g_tsf_preedit_shuangpin_quanpin;
extern bool g_candidate_window_preedit_shuangpin_quanpin;
extern bool g_shuangpin_helpcode_enabled;
extern bool g_shuangpin_helpcode_direct;
extern bool g_shuangpin_helpcode_trigger_backtick;
extern bool g_shuangpin_helpcode_trigger_semicolon;
extern bool g_shuangpin_helpcode_trigger_uppercase;
extern bool g_shuangpin_mid_sentence_helpcode_enabled;
extern bool g_shuangpin_mid_sentence_helpcode_backtick;
extern bool g_shuangpin_mid_sentence_helpcode_semicolon;
extern bool g_shuangpin_mid_sentence_helpcode_uppercase;
extern bool g_shuangpin_direct_helpcode_enabled;
extern bool g_shuangpin_direct_helpcode_slash;
extern bool g_shuangpin_direct_helpcode_uppercase;
extern bool g_shuangpin_direct_helpcode_end_trigger;
extern bool g_quanpin_helpcode_enabled;
extern std::string g_shuangpin_helpcode_schema;
extern std::string g_quanpin_helpcode_schema;
extern bool g_show_shuangpin_helpcode_in_candidate_window;
extern bool g_show_quanpin_helpcode_in_candidate_window;
extern bool g_quanpin_autocorrect_transposition;
extern bool g_quanpin_autocorrect_neighbor;
extern bool g_quanpin_autocorrect_deletion;
extern bool g_quanpin_autocorrect_insertion;
extern bool g_quanpin_autocorrect_marker;
extern std::uint32_t g_fuzzy_pinyin_rules;
extern bool g_fuzzy_pinyin_enabled;
extern bool g_fuzzy_seeded;
extern bool g_floating_toolbar_enabled;
extern bool g_caret_state_indicator_enabled;
extern bool g_caret_state_indicator_on_focus;
extern std::string g_caret_state_indicator_position;
extern FloatingToolbarItemsConfig g_floating_toolbar_items;
extern double g_floating_toolbar_scale;
extern int g_floating_toolbar_font_size;
extern bool g_floating_toolbar_auto_hide;
extern int g_floating_toolbar_auto_hide_delay;
extern bool g_floating_toolbar_shadow;
extern bool g_english_candidates_enabled;
extern bool g_candidate_translations_enabled;
extern std::atomic_bool g_candidate_translation_show_pos;
extern std::atomic_bool g_candidate_translation_single_sense;
extern int g_english_mixed_input_min_chars;
extern int g_english_mixed_input_first_page_count;
extern bool g_cloud_candidates_enabled;
extern bool g_assoc_sentence_wordlattice;
extern bool g_assoc_sentence_google;
extern bool g_assoc_sentence_neural_desktop;
extern bool g_assoc_sentence_neural_keyboard;
extern bool g_assoc_context_candidate_rerank;
extern bool g_assoc_sentence_show_next_on_duplicate;
extern bool g_assoc_sentence_source_badge;
extern bool g_assoc_sentence_collocation_enabled;
extern std::string g_assoc_sentence_collocation_model;
extern double g_assoc_sentence_collocation_weight;
// octagram 六参数（万象现行默认）。不进设置页，供配置文件高级调参。
extern int g_assoc_sentence_collocation_max_length;
extern int g_assoc_sentence_collocation_min_length;
extern double g_assoc_sentence_collocation_penalty;
extern double g_assoc_sentence_collocation_non_penalty;
extern double g_assoc_sentence_collocation_weak_penalty;
extern double g_assoc_sentence_collocation_rear_penalty;
extern bool g_emoji_mixed_input_enabled;
extern bool g_kaomoji_mixed_input_enabled;
extern bool g_symbol_mixed_input_enabled;
extern bool g_radical_mode_enabled;
extern bool g_quick_phrase_enabled;
extern bool g_quick_phrase_candidates_enabled;
extern bool g_quick_phrase_frequency_enabled;
extern bool g_mixed_candidates_enabled;
extern bool g_date_time_candidates_enabled;
extern bool g_date_time_menu_enabled;
extern bool g_date_time_mode_enabled;
extern bool g_emoji_mode_enabled;
extern bool g_kaomoji_mode_enabled;
extern bool g_symbol_mode_enabled;
extern bool g_jianpin_mode_enabled;
extern bool g_y_mode_enabled;
extern bool g_r_mode_enabled;
extern bool g_v_mode_enabled;
extern bool g_clipboard_history_enabled;
extern bool g_language_bar_enabled;
extern bool g_language_bar_show_ime_mode;
extern bool g_language_bar_show_double_single_byte;
extern bool g_language_bar_show_punctuation;
extern bool g_paging_minus_equal_enabled;
extern bool g_paging_comma_period_enabled;
extern bool g_paging_brackets_enabled;
extern bool g_paging_tab_enabled;
extern bool g_paging_page_up_down_enabled;
extern bool g_paging_mouse_wheel_enabled;
extern bool g_candidate_arrow_navigation_enabled;
extern bool g_candidate_horizontal_arrow_navigation_enabled;
extern bool g_word_to_character_enabled;
extern std::string g_word_to_character_keys;
extern bool g_smart_punctuation_enabled;
extern bool g_smart_punctuation_space_convert_enabled;
extern bool g_smart_punctuation_direct_digit_enabled;
extern bool g_smart_punctuation_direct_letter_enabled;
extern bool g_smart_punctuation_repeat_to_chinese_enabled;
extern bool g_paired_punctuation_enabled;
extern bool g_enumeration_comma_backslash_enabled;
extern bool g_enumeration_comma_slash_enabled;
extern std::string g_punctuation_lock;
extern std::string g_candidate_window_layout;
extern bool g_candidate_window_follow_cursor;
extern bool g_candidate_window_fit_content;
extern bool g_candidate_window_single_line;
extern std::string g_candidate_skin;
extern std::string g_candidate_window_preedit_style;
extern bool g_candidate_fixed_badge;
extern std::string g_candidate_fixed_badge_style;
extern std::string g_candidate_ai_badge_style;
extern std::string g_settings_window_linger;
extern bool g_settings_launch_animation;
extern bool g_auto_download_update;
extern std::string g_theme_mode;
extern std::string g_theme_settings;
extern std::string g_theme_cand;
extern std::string g_theme_ftb;
extern std::string g_theme_menu;
extern std::string g_theme_emoji;
extern std::string g_theme_screen_keyboard;
extern std::string g_theme_handwriting;
extern std::string g_theme_voice;
extern std::shared_mutex g_voice_input_mutex;
extern VoiceInputConfig g_voice_input;
extern AiAssistantConfig g_ai_assistant;
extern TencentTmtConfig g_tencent_tmt;
extern AliyunMtConfig g_aliyun_mt;
extern CustomTranslationConfig g_custom_translation;
extern NiuTransConfig g_niutrans;
extern std::string g_translation_provider;
extern std::mutex g_network_proxy_mutex;
extern NetworkProxyConfig g_network_proxy; // guarded by g_network_proxy_mutex
extern FrequencyAdjustmentConfig g_frequency_adjustment;
extern std::filesystem::path g_config_path;

bool IsHelpcodeSchemaAvailable(const std::string &schema);
// ime_config_input.cpp：按用户选的行内预编辑样式、「双拼显示全拼」开关和当前方案算出 TSF 实际
// 使用的样式，写进 GlobalSettings。
void RefreshEffectiveTsfPreeditStyle();
// ime_config_shuangpin.cpp：内置方案直接返回 true；自定义方案重新读文件、解析并登记给引擎，
// 文件缺失或写错时返回 false，调用方据此回落到小鹤。
bool LoadShuangpinSchema(const std::string &schema);
VoiceInputConfig SnapshotVoiceInput();
bool IsValidCandidateSkinId(const std::string &skin);
bool IsValidCandidateFixedBadgeStyle(const std::string &style);
bool IsValidCandidateAiBadgeStyle(const std::string &style);
bool IsValidSettingsWindowLinger(const std::string &linger);
bool IsValidTranslationTargetLanguage(const std::string &language);
std::string AiAssistantTokenSlotKey(std::string_view provider);
// 实现在 ime_config_services.cpp；声明与注释见 ime_config.h 的同名条目。
bool SetConfiguredAiAssistantProviderValue(std::string_view provider, std::string_view field, const std::string &value);
// ime_config_services.cpp：按当前各段全局配置重建候选词翻译快照。改动 provider、腾讯云、
// 阿里云、自定义翻译、小牛翻译或 AI 辅助服务商配置的地方，改完都要调一次。
void PublishTranslationConfig();
void RememberConfigWriteTime();
// 记下 ai_providers.json 当前的修改时间。本进程自己保存注册表后调用，免得把自己的写入当成外部变更重载。
void RememberProviderStoreWriteTime();

// ime_config_toml.cpp
std::string EscapeTomlBasicString(const std::string &value);
bool ReplaceTomlValuePreservingFormatting(std::string &text, const std::string &section, const std::string &key,
                                          const std::string &replacement);
bool InsertTomlValuePreservingFormatting(std::string &text, const std::string &section, const std::string &key,
                                         const std::string &value);
std::string ReadFileText(const std::filesystem::path &path);
bool WriteFileBytes(const std::filesystem::path &path, const std::string &text);
bool WriteFileTextAtomically(const std::filesystem::path &path, const std::string &text);
bool TomlTextIsParseable(const std::string &text);
void ForEachTomlAssignment(const std::string &text, const TomlAssignmentVisitor &visit);
std::string MakeTomlAssignmentId(const std::string &section, const std::string &key);
std::map<std::string, std::string> ParseTomlAssignments(const std::string &text);

// ime_config_credentials.cpp
bool IsCredentialKey(const std::string &key);
// 凭证键的 TOML 字面量换成密文字面量；非凭证键、占位符和已加密的值原样返回。DPAPI 失败返回 nullopt。
std::optional<std::string> ProtectCredentialLiteral(const std::string &key, const std::string &literal);
void RevealStoredCredentials(toml::table &tbl);
void ProtectPlaintextCredentials();

// ime_config_template.cpp
void RecoverLegacyAcpMangledConfig();
void SyncConfigWithInstalledTemplate();

// 配置文件的 FNV-1a 64 位摘要，用来在修改时间不可靠时判断内容是否真的变了。
uint64_t HashFileContents(const std::filesystem::path &path);

// ime_config_write.cpp
bool WriteConfiguredValues(const std::vector<ConfigValueUpdate> &updates);
bool WriteConfiguredValue(const std::string &section, const std::string &key, const std::string &replacement);
void NotifyImeServer(UINT windowMessage, const wchar_t *auxMessage, WPARAM wParam = 0);
} // namespace ime_config_detail
