#include "engine/contracts/webview/validator.h"
#include "config/ime_config.h"
#include "engine/core/data_path.h"
#include "global/globals.h"
#include "resource/resource.h"
#include "settings/settings_launcher.h"
#include "settings/api_credential_test.h"
#include "settings/collocation_model.h"
#include "settings/lattice_model.h"
#include "settings/neural_model.h"
#include "settings/default_input_method.h"
#include "settings/game_compat_name.h"
#include "settings/input_method_install.h"
#include "settings/dictionary_manager.h"
#include "settings/native/native_settings_window.h"

#include "msimeui/DeviceResources.h"
#include "settings/serial_task_queue.h"
#include "statistics/stats_overview.h"
#include "statistics/stats_store.h"
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include "skin/candidate_skin_catalog.h"
#include "window/candidate_skin_layers.h"
#include "utils/common_utils.h"
#include "utils/single_instance.h"
#include "voice-input/voice_providers.h"

#include <boost/json.hpp>
#include <dwmapi.h>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

namespace json = boost::json;

namespace
{
constexpr wchar_t kWindowClass[] = L"MetasequoiaImeSettingsWindow";
constexpr wchar_t kWindowTitle[] = L"Metasequoia IME Settings";
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\MetasequoiaImeSettings.SingleInstance";
constexpr UINT kActivateExistingWindow = WM_APP + 1;
constexpr UINT kOpenAboutSection = WM_APP + 2;
constexpr UINT kScanSkinCatalog = WM_APP + 3;
constexpr UINT kWorkerCompleted = WM_APP + 4;
// Sent by settings_launcher.cpp (the Server exiting) and by the installer: close for real.
constexpr UINT kQuitSettings = WM_APP + 5;
// 模型更新探测线程完成：UI 线程重新取一份快照，探测线程不碰 g_worker。
constexpr UINT kModelUpdateChecked = WM_APP + 6;
// Timer 1 is msimeui's caret blink in the Direct2D window, so ours start elsewhere.
constexpr UINT_PTR kConfigReloadTimer = 0x5301;
constexpr UINT_PTR kLingerTimer = 0x5302;
// Server 重启后主动刷新快照的短时定时器；不重启设置进程。
constexpr UINT_PTR kRestartRefreshTimer = 0x5303;
constexpr int kRestartRefreshTicks = 10; // 500ms × 10 = 5s，覆盖 Watchdog 拉起 Server 的时间

// The Direct2D settings window (msimeui). Null until wWinMain builds it.
std::unique_ptr<native_settings::SettingsWindow> g_native;

bool g_window_minimized = false;
bool g_open_about_on_ready = false;
HWND g_settings_hwnd = nullptr;
std::optional<CandidateSkinCatalog::ScanResult> g_candidate_skin_catalog;
uint64_t g_candidate_skin_catalog_revision = 0;
std::unique_ptr<SerialTaskQueue> g_worker;
bool g_closing = false;
bool g_lingering = false;
bool g_reload_pending = false;
bool g_theme_refresh_pending = false;
bool g_settings_light = false;
// 第一份快照真正下发给页面之后才启动更新探测；一个设置进程只探一轮。
bool g_model_update_check_started = false;
// restartServer 之后主动刷新的剩余次数；Server 落位 .gram.new 后提前结束。
int g_restart_refresh_remaining = 0;
// appearance.settings_window_linger 的 UI 线程副本，由 ConfigCompletion 更新。
std::string g_settings_window_linger = "off";
std::wstring g_last_config_message;
bool g_worker_com_initialized = false; // worker-only
void CloseSettings(HWND hwnd);
void HideSettings(HWND hwnd);

// The settings window is ready to receive host messages.
bool FrontendReady()
{
    return static_cast<bool>(g_native);
}

// Host -> window message: the settings window consumes the webview-contract JSON.
bool PostToPage(const std::wstring &message)
{
    if (!g_native)
        return false;
    g_native->OnHostMessage(message);
    return true;
}

// 给页面回一条只在宿主侧产生的消息。契约里的 server 段是穷举的，新类型必须同时写进
// engine/contracts/webview/messages.json，否则 Validate 会拒。
void PostToSettingsPage(const json::value &message)
{
    if (!g_native)
        return;
    PostToPage(string_to_wstring(json::serialize(message)));
}

// restartServer 的回执。页面那个「3 秒后恢复按钮」的定时器挂在设置窗口上，窗口一隐藏
// 就随之失效，按钮会永远停在「重启中…」，所以由宿主在重启流程里统一发这一条。
void PostRestartServerDone(bool ok)
{
    PostToSettingsPage(
        json::value{{"type", "restartServerDone"}, {"protocolVersion", metasequoia::webview::Version}, {"ok", ok}});
}

// 冷启动打点：设置 METASEQUOIA_IME_SETTINGS_TRACE=1 后，各阶段距进程启动的毫秒数追加到
// %TEMP%\metasequoia-settings-startup.log。UI 线程和 worker 都会调用。
void TraceStartup(const char *stage)
{
    static const bool enabled = [] {
        wchar_t value[8]{};
        return GetEnvironmentVariableW(L"METASEQUOIA_IME_SETTINGS_TRACE", value, 8) > 0 && value[0] == L'1';
    }();
    if (!enabled)
        return;
    static const LONGLONG start = [] {
        FILETIME creation{}, exit{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
        return static_cast<LONGLONG>((static_cast<ULONGLONG>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime);
    }();
    FILETIME now_ft{};
    GetSystemTimePreciseAsFileTime(&now_ft);
    const LONGLONG now =
        static_cast<LONGLONG>((static_cast<ULONGLONG>(now_ft.dwHighDateTime) << 32) | now_ft.dwLowDateTime);
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path path = std::filesystem::path(temp) / L"metasequoia-settings-startup.log";
    FILE *file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a") == 0 && file)
    {
        fprintf(file, "%lu %8.1f ms  %s\n", GetCurrentProcessId(), static_cast<double>(now - start) / 10000.0, stage);
        fclose(file);
    }
}

void ShowAboutSection()
{
    if (g_native)
    {
        g_native->ShowPage("about-settings");
    }
}

void ApplySettingsChromeTheme(HWND hwnd = nullptr)
{
    const HWND target = hwnd ? hwnd : g_settings_hwnd;
    const bool light = g_settings_light;
    if (target)
    {
        BOOL dark = light ? FALSE : TRUE;
        DwmSetWindowAttribute(target, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }
}

void ResetTitlebarHoverAfterVisibilityChange()
{
    if (g_native)
    {
        g_native->ResetTransientState();
    }
}

static nlohmann::json CustomHelpcodeSchemasJson()
{
    nlohmann::json schemas = nlohmann::json::array();
    for (const auto &schema : GetCustomHelpcodeSchemas())
        schemas.push_back({{"id", schema.schema}, {"name", schema.name}, {"name_en", schema.name_en}});
    return schemas;
}

static nlohmann::json CustomShuangpinSchemasJson()
{
    nlohmann::json schemas = nlohmann::json::array();
    for (const auto &schema : GetCustomShuangpinSchemas())
    {
        schemas.push_back(
            {{"id", schema.schema}, {"name", schema.name}, {"name_en", schema.name_en}, {"error", schema.error}});
    }
    return schemas;
}

// The provider registry as sent to the settings page: one entry per user-defined service. Built-in
// ids stay config.toml-authoritative, so their endpoint/model come from the parsed config rather
// than a possibly-stale store copy. Every token comes from config.toml; the store holds none.
nlohmann::json ProviderEntriesJson(const AiAssistantConfig &ai)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto &entry : ai.providers)
    {
        nlohmann::json item = {{"id", entry.id},
                               {"name", entry.name},
                               {"endpoint", entry.endpoint},
                               {"token", ""},
                               {"model", entry.model}};
        if (AiAssistant::ProviderStore::IsBuiltinProvider(entry.id))
        {
            const auto endpoint = ai.endpoints.find(entry.id);
            const auto model = ai.models.find(entry.id);
            if (endpoint != ai.endpoints.end())
                item["endpoint"] = endpoint->second;
            if (model != ai.models.end())
                item["model"] = model->second;
        }
        if (const auto token = ai.tokens.find(entry.id); token != ai.tokens.end())
            item["token"] = token->second;
        array.push_back(std::move(item));
    }
    return array;
}

// Index of the input-state shortcut written by a config path, or Definitions.size().
std::size_t InputStateShortcutIndex(const std::string &path)
{
    using namespace FanyImeInputStateShortcuts;
    for (std::size_t i = 0; i < Definitions.size(); ++i)
        if (path == "keybindings." + std::string(Definitions[i].key))
            return i;
    return Definitions.size();
}

bool IsInputStateShortcutPath(const std::string &path)
{
    return InputStateShortcutIndex(path) < FanyImeInputStateShortcuts::Definitions.size();
}

nlohmann::json BuildInputStateShortcutMetadata()
{
    using namespace FanyImeInputStateShortcuts;
    const auto &configured = GetConfiguredInputStateShortcuts();
    const Values values = InputStateShortcutViews(configured);
    nlohmann::json actions = nlohmann::json::array(), reserved = nlohmann::json::array(),
                   warnings = nlohmann::json::array(), keys = nlohmann::json::array();
    for (std::size_t i = 0; i < Definitions.size(); ++i)
    {
        const auto &d = Definitions[i];
        actions.push_back(
            nlohmann::json{{"key", std::string(d.key)},
                           {"label", std::string(d.label)},
                           {"editor", std::string(d.editor)},
                           {"defaultBinding", std::string(d.defaultBinding)},
                           // A non-string value has no binding to show; problem explains it.
                           {"binding", configured[i] == FanyImeKeyChord::InvalidValue ? std::string() : configured[i]},
                           {"problem", std::string(Validate(i, values[i], values))}});
    }
    for (const auto &rule : Reserved)
        reserved.push_back(nlohmann::json{{"binding", std::string(rule.binding)}, {"label", std::string(rule.label)}});
    for (const auto &rule : Warnings)
        warnings.push_back(nlohmann::json{{"binding", std::string(rule.binding)}, {"label", std::string(rule.label)}});
    for (const auto &key : FanyImeKeyChord::NamedKeys)
        keys.push_back(
            nlohmann::json{{"code", std::string(key.name)}, {"vk", key.key}, {"label", std::string(key.label)}});
    return {{"actions", std::move(actions)},
            {"reserved", std::move(reserved)},
            {"warnings", std::move(warnings)},
            {"keys", std::move(keys)}};
}

std::wstring BuildConfigMessage(bool refresh_skin_catalog)
{

    const VoiceInputConfig &voice = GetConfiguredVoiceInput();
    const AiAssistantConfig &ai = GetConfiguredAiAssistant();
    const TencentTmtConfig &tencent_tmt = GetConfiguredTencentTmt();
    const CustomTranslationConfig &custom_translation = GetConfiguredCustomTranslation();
    const AliyunMtConfig &aliyun_mt = GetConfiguredAliyunMt();
    const NiuTransConfig &niutrans = GetConfiguredNiuTrans();
    const auto translation = GetConfiguredTranslation();
    const NetworkProxyConfig network_proxy = GetConfiguredNetworkProxy();
    const FrequencyAdjustmentConfig &frequency = GetConfiguredFrequencyAdjustment();
    const FloatingToolbarItemsConfig &toolbar = GetConfiguredFloatingToolbarItems();
    const std::filesystem::path skins_root = std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"skins";
    if (refresh_skin_catalog)
    {
        g_candidate_skin_catalog = CandidateSkinCatalog::Scan(skins_root);
        ++g_candidate_skin_catalog_revision;
    }
    const std::string skin_layout = GetConfiguredCandidateWindowLayout();
    const std::string skin_theme = ResolveConfiguredTheme(GetConfiguredThemeCand());
    nlohmann::json external_skins = nlohmann::json::array();
    if (g_candidate_skin_catalog)
    {
        for (const auto &skin : g_candidate_skin_catalog->packages)
        {
            external_skins.push_back(
                // 设置页只用这些列出皮肤卡片；预览按皮肤清单自己画（CandidateSkinScene），不经这里。
                {{"id", skin.id},
                 {"name", skin.name},
                 {"version", skin.version},
                 {"author", skin.author},
                 {"description", skin.description},
                 {"base", skin.base},
                 {"layouts", skin.layouts},
                 {"themes", skin.themes},
                 {"decorationTopDip", skin.decorationTopDip},
                 {"compatible", CandidateSkinCatalog::Supports(skin, skin_layout, skin_theme)}});
        }
    }
    nlohmann::json skin_issues = nlohmann::json::array();
    if (g_candidate_skin_catalog)
    {
        for (const auto &issue : g_candidate_skin_catalog->issues)
            skin_issues.push_back({{"folder", issue.folder},
                                   {"reason", issue.reason},
                                   {"noticed", issue.kind == CandidateSkinCatalog::Issue::Kind::Noticed}});
    }
    nlohmann::json polish_presets = nlohmann::json::array();
    for (const auto &preset : VoiceInput::BuiltinPolishPromptPresets())
    {
        polish_presets.push_back({{"id", std::string(preset.id)},
                                  {"name", std::string(preset.name)},
                                  {"prompt", std::string(preset.prompt)}});
    }
    // 规则位取不门控视图：总开关关闭时复选框仍要展示用户已存的勾选；
    // 总开关本身单独下发。
    const metasequoia::FuzzyPinyinOptions fuzzy_rules = GetConfiguredFuzzyPinyinRuleStates();
    nlohmann::json payload = {
        {"type", "configSnapshot"},
        {"data",
         {{"input",
           {{"mode", GetConfiguredInputMode()},
            {"schema", GetConfiguredInputSchemeName()},
            {"japanese_schema", GetConfiguredJapaneseSchema()},
            {"character_set", GetConfiguredCharacterSet()},
            {"default_ime_mode", GetConfiguredDefaultImeMode()},
            {"ime_mode_scope", GetConfiguredImeModeScope()},
            {"shuangpin_schema", GetConfiguredShuangpinSchema()},
            {"wubi_schema", GetConfiguredWubiSchema()},
            {"wubi_mixed_pinyin", GetConfiguredWubiMixedPinyin()},
            {"escape_keeps_selected_word", GetConfiguredEscapeKeepsSelectedWord()},
            {"enter_learns_english_word", GetConfiguredEnterLearnsEnglishWord()},
            {"wubi_z_mode", GetConfiguredWubiZMode()},
            {"wubi_four_code_auto_commit", GetConfiguredWubiFourCodeAutoCommit()},
            {"wubi_fifth_code_top_commit", GetConfiguredWubiFifthCodeTopCommit()},
            {"word_to_character", GetConfiguredWordToCharacterEnabled()},
            {"word_to_character_keys", GetConfiguredWordToCharacterKeys()},
            {"smart_punctuation", GetConfiguredSmartPunctuationEnabled()},
            {"smart_punctuation_space_convert", GetConfiguredSmartPunctuationSpaceConvertEnabled()},
            {"smart_punctuation_direct_digit", GetConfiguredSmartPunctuationDirectDigitEnabled()},
            {"smart_punctuation_direct_letter", GetConfiguredSmartPunctuationDirectLetterEnabled()},
            {"smart_punctuation_repeat_to_chinese", GetConfiguredSmartPunctuationRepeatToChineseEnabled()},
            {"paired_punctuation", GetConfiguredPairedPunctuationEnabled()},
            {"enumeration_comma_backslash", GetConfiguredEnumerationCommaBackslashEnabled()},
            {"enumeration_comma_slash", GetConfiguredEnumerationCommaSlashEnabled()},
            {"punctuation_lock", GetConfiguredPunctuationLock()},
            {"fuzzy_pinyin", GetConfiguredFuzzyPinyinEnabled()},
            {"fuzzy_z_zh", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::Z_ZH)},
            {"fuzzy_c_ch", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::C_CH)},
            {"fuzzy_s_sh", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::S_SH)},
            {"fuzzy_n_l", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::N_L)},
            {"fuzzy_f_h", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::F_H)},
            {"fuzzy_r_l", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::R_L)},
            {"fuzzy_an_ang", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::AN_ANG)},
            {"fuzzy_en_eng", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::EN_ENG)},
            {"fuzzy_in_ing", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::IN_ING)},
            {"fuzzy_ian_iang", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::IAN_IANG)},
            {"fuzzy_uan_uang", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::UAN_UANG)}}},
          {"general",
           {{"diagnostic_log", GetConfiguredDiagnosticLogEnabled()},
            {"candidate_window_diagnostic_log", GetConfiguredDiagnosticLogEnabled()},
            {"tsf_diagnostic_log", GetConfiguredTsfDiagnosticLogEnabled()},
            {"floating_toolbar", GetConfiguredFloatingToolbarEnabled()},
            {"caret_state_indicator", GetConfiguredCaretStateIndicatorEnabled()},
            {"caret_state_indicator_on_focus", GetConfiguredCaretStateIndicatorOnFocus()},
            {"caret_state_indicator_position", GetConfiguredCaretStateIndicatorPosition()},
            {"floating_toolbar_fullwidth", toolbar.fullwidth},
            {"floating_toolbar_punctuation", toolbar.punctuation},
            {"floating_toolbar_character_set", toolbar.character_set},
            {"floating_toolbar_emoji", toolbar.emoji},
            {"floating_toolbar_screen_keyboard", toolbar.screen_keyboard},
            {"floating_toolbar_settings", toolbar.settings},
            {"floating_toolbar_scale", GetConfiguredFloatingToolbarScale()},
            {"floating_toolbar_font_size", GetConfiguredFloatingToolbarFontSize()},
            {"floating_toolbar_auto_hide", GetConfiguredFloatingToolbarAutoHide()},
            {"floating_toolbar_auto_hide_delay", GetConfiguredFloatingToolbarAutoHideDelay()},
            {"floating_toolbar_shadow", GetConfiguredFloatingToolbarShadow()},
            {"cn_en_mixed_input", GetConfiguredEnglishCandidatesEnabled()},
            {"candidate_translations", GetConfiguredCandidateTranslationsEnabled()},
            {"candidate_expansion_panel_enabled", GetConfiguredCandidateExpansionPanelEnabled()},
            {"candidate_expansion_panel_translation", GetConfiguredCandidateExpansionPanelTranslationEnabled()},
            {"candidate_expansion_panel_emoji", GetConfiguredCandidateExpansionPanelEmojiEnabled()},
            {"candidate_expansion_panel_kaomoji", GetConfiguredCandidateExpansionPanelKaomojiEnabled()},
            {"candidate_expansion_panel_symbols", GetConfiguredCandidateExpansionPanelSymbolsEnabled()},
            {"candidate_expansion_panel_translation_max_count",
             GetConfiguredCandidateExpansionPanelTranslationMaxCount()},
            {"candidate_expansion_panel_emoji_max_count", GetConfiguredCandidateExpansionPanelEmojiMaxCount()},
            {"candidate_expansion_panel_kaomoji_max_count", GetConfiguredCandidateExpansionPanelKaomojiMaxCount()},
            {"candidate_expansion_panel_symbols_max_count", GetConfiguredCandidateExpansionPanelSymbolsMaxCount()},
            {"candidate_expansion_panel_placement", GetConfiguredCandidateExpansionPanelPlacement()},
            {"candidate_translation_show_pos", GetConfiguredCandidateTranslationShowPos()},
            {"candidate_translation_single_sense", GetConfiguredCandidateTranslationSingleSense()},
            {"cn_en_mixed_input_min_chars", GetConfiguredEnglishMixedInputMinChars()},
            {"cn_en_mixed_input_first_page_count", GetConfiguredEnglishMixedInputFirstPageCount()},
            {"emoji_mixed_input", GetConfiguredEmojiMixedInputEnabled()},
            {"kaomoji_mixed_input", GetConfiguredKaomojiMixedInputEnabled()},
            {"symbol_mixed_input", GetConfiguredSymbolMixedInputEnabled()},
            {"cloud_candidates", GetConfiguredCloudCandidatesEnabled()},
            {"paging_minus_equal", GetConfiguredPagingMinusEqualEnabled()},
            {"paging_comma_period", GetConfiguredPagingCommaPeriodEnabled()},
            {"paging_brackets", GetConfiguredPagingBracketsEnabled()},
            {"paging_tab", GetConfiguredPagingTabEnabled()},
            {"paging_page_up_down", GetConfiguredPagingPageUpDownEnabled()},
            {"paging_mouse_wheel", GetConfiguredPagingMouseWheelEnabled()},
            {"candidate_arrow_navigation", GetConfiguredCandidateArrowNavigationEnabled()},
            {"candidate_horizontal_arrow_navigation", GetConfiguredCandidateHorizontalArrowNavigationEnabled()}}},
          {"association",
           {{"sentence_wordlattice", GetConfiguredAssocSentenceWordLattice()},
            {"sentence_wordlattice_model_status", [] {
                 const auto status = lattice_model::GetModelStatus();
                 return nlohmann::json{{"state", status.state},
                                       {"progress", status.progress},
                                       {"error", status.error},
                                       {"deletePending", status.delete_pending}};
             }()},
            {"sentence_wordlattice_model_url", lattice_model::SourceUrl()},
            {"sentence_google", GetConfiguredAssocSentenceGoogle()},
            {"sentence_neural_desktop", GetConfiguredAssocSentenceNeuralDesktop()},
            {"sentence_neural_desktop_model_status", [] {
                 const auto status = neural_model::GetModelStatus();
                 return nlohmann::json{
                     {"state", status.state}, {"progress", status.progress}, {"error", status.error}};
             }()},
            {"sentence_neural_desktop_model_url", neural_model::SourceUrl()},
            {"sentence_neural_keyboard", GetConfiguredAssocSentenceNeuralKeyboard()},
            {"context_candidate_rerank", GetConfiguredAssocContextCandidateRerank()},
            {"sentence_show_next_on_duplicate", GetConfiguredAssocSentenceShowNextOnDuplicate()},
            {"sentence_source_badge", GetConfiguredAssocSentenceSourceBadge()},
            {"sentence_collocation_enabled", GetConfiguredAssocSentenceCollocationEnabled()},
            {"sentence_collocation_model_status", [] {
                 nlohmann::json statuses = nlohmann::json::object();
                 for (const auto &[model_id, status] : collocation::GetModelStatuses())
                     statuses[model_id] = nlohmann::json{{"state", status.state},
                                                         {"progress", status.progress},
                                                         {"error", status.error},
                                                         {"updateAvailable", status.update_available},
                                                         {"updatePending", status.update_pending}};
                 return statuses;
             }()},
            {"sentence_collocation_model", GetConfiguredAssocSentenceCollocationModel()},
            {"sentence_collocation_catalog", [] {
                 // 页面不自持目录副本：id 与下载 URL 留在 Server 侧，只下发展示字段。
                 nlohmann::json catalog = nlohmann::json::array();
                 for (const auto &entry : collocation::Catalog())
                 {
                     catalog.push_back(nlohmann::json{{"id", entry.id},
                                                      {"displayName", entry.display_name},
                                                      {"sizeHint", entry.size_hint},
                                                      {"license", entry.license},
                                                      {"url", collocation::SourceUrl(entry)}});
                 }
                 return catalog;
             }()}}},
          {"keybindings",
           {{"switch_language_shift", GetConfiguredSwitchLanguageShiftEnabled()},
            {"switch_language_ctrl", GetConfiguredSwitchLanguageCtrlEnabled()},
            {"switch_language_ctrl_alt_space", GetConfiguredSwitchLanguageCtrlAltSpaceEnabled()},
            {"toggle_character_set_ctrl_shift_f", GetConfiguredCharacterSetShortcutEnabled()},
             {"commit_pinyin_shift_enter", GetConfiguredPinyinCommitShortcutEnabled()},
             {"candidate_expansion_key", GetConfiguredCandidateExpansionKey()},
              {"candidate_expansion_move_prev_key", GetConfiguredCandidateExpansionMovePrevKey()},
              {"candidate_expansion_move_next_key", GetConfiguredCandidateExpansionMoveNextKey()},
             {"input_state_shortcuts", BuildInputStateShortcutMetadata()},
            {"trilingual_cycle", GetConfiguredTrilingualCycleEnabled()}}},
          {"frequency_adjustment",
           {{"mode", frequency.mode},
            {"trigger_count", frequency.trigger_count},
            {"linear_step", frequency.linear_step}}},
          {"utility",
           {{"radical_mode", GetConfiguredRadicalModeEnabled()},
            {"quick_phrase", GetConfiguredQuickPhraseEnabled()},
            {"quick_phrase_candidates", GetConfiguredQuickPhraseCandidatesEnabled()},
            {"quick_phrase_frequency", GetConfiguredQuickPhraseFrequencyEnabled()},
            {"mixed_candidates", GetConfiguredMixedCandidatesEnabled()},
            {"date_time_candidates", GetConfiguredDateTimeCandidatesEnabled()},
            {"date_time_menu", GetConfiguredDateTimeMenuEnabled()},
            {"date_time_mode", GetConfiguredDateTimeModeEnabled()},
            {"emoji_mode", GetConfiguredEmojiModeEnabled()},
            {"kaomoji_mode", GetConfiguredKaomojiModeEnabled()},
            {"symbol_mode", GetConfiguredSymbolModeEnabled()},
            {"jianpin_mode", GetConfiguredJianpinModeEnabled()},
            {"y_mode", GetConfiguredYModeEnabled()},
            {"r_mode", GetConfiguredRModeEnabled()},
            {"v_mode", GetConfiguredVModeEnabled()},
            {"clipboard_history", GetConfiguredClipboardHistoryEnabled()}}},
          {"language_bar",
           {{"enabled", GetConfiguredLanguageBarEnabled()},
            {"show_ime_mode", GetConfiguredLanguageBarShowImeMode()},
            {"show_double_single_byte", GetConfiguredLanguageBarShowDoubleSingleByte()},
            {"show_punctuation", GetConfiguredLanguageBarShowPunctuation()}}},
          {"appearance",
           {{"settings_window_linger", GetConfiguredSettingsWindowLinger()},
            {"settings_launch_animation", GetConfiguredSettingsLaunchAnimation()},
            {"auto_download_update", GetConfiguredAutoDownloadUpdate()},
            {"candidate_window_layout", GetConfiguredCandidateWindowLayout()},
            {"candidate_window_follow_cursor", GetConfiguredCandidateWindowFollowCursor()},
            {"candidate_window_fit_content", GetConfiguredCandidateWindowFitContent()},
            {"candidate_window_single_line", GetConfiguredCandidateWindowSingleLine()},
            {"candidate_skin", GetConfiguredCandidateSkin()},
            {"candidate_window_preedit_style", GetConfiguredCandidateWindowPreeditStyle()},
            {"candidate_window_preedit_shuangpin_quanpin", GetConfiguredCandidateWindowPreeditShuangpinQuanpin()},
            {"candidate_fixed_badge", GetConfiguredCandidateFixedBadge()},
            {"candidate_fixed_badge_style", GetConfiguredCandidateFixedBadgeStyle()},
            {"candidate_ai_badge_style", GetConfiguredCandidateAiBadgeStyle()},
            {"tsf_preedit_style", GetConfiguredTsfPreeditStyle()},
            {"tsf_preedit_shuangpin_quanpin", GetConfiguredTsfPreeditShuangpinQuanpin()},
            {"theme_mode", GetConfiguredThemeMode()},
            {"theme_settings", GetConfiguredThemeSettings()},
            {"theme_cand", GetConfiguredThemeCand()},
            {"theme_ftb", GetConfiguredThemeFtb()},
            {"theme_menu", GetConfiguredThemeMenu()},
            {"theme_emoji", GetConfiguredThemeEmoji()},
            {"theme_screen_keyboard", GetConfiguredThemeScreenKeyboard()},
            {"theme_handwriting", GetConfiguredThemeHandwriting()},
            {"theme_voice", GetConfiguredThemeVoice()},
            {"page_size", GetConfiguredCandidatePageSize()},
            {"font", GetConfiguredCandidateFont()},
            {"font_css_family", ResolveSystemFontFamilyForCss(GetConfiguredCandidateFont())},
            {"fallback_fonts", GetConfiguredCandidateFallbackFonts()},
            {"fallback_font_css_families", GetConfiguredCandidateFallbackFontFamilies()},
            {"english_font", GetConfiguredCandidateEnglishFont()},
            {"english_font_css_family", ResolveSystemFontFamilyForCss(GetConfiguredCandidateEnglishFont())},
            {"default_font", GetConfiguredCandidateDefaultFont()},
            {"default_font_css_family", ResolveSystemFontFamilyForCss(GetConfiguredCandidateDefaultFont())},
            {"font_size", GetConfiguredCandidateFontSize()},
            {"candidate_window_preedit_font_size", GetConfiguredCandidateWindowPreeditFontSize()},
            {"cand_text_color", GetConfiguredCandidateTextColor()},
            {"system_fonts", GetSystemFontFamilies()},
            {"external_candidate_skins", std::move(external_skins)},
            {"candidate_skin_scan_issues", std::move(skin_issues)},
            {"candidate_skin_catalog_scanned", g_candidate_skin_catalog.has_value()},
            {"candidate_skin_catalog_revision", g_candidate_skin_catalog_revision},
            {"candidate_skin_directory", skins_root.u8string()}}},
          {"voice_input",
           {{"enabled", voice.enabled},
            {"hotkey_ralt", voice.hotkey_ralt},
            {"hotkey_ctrl_f9", voice.hotkey_ctrl_f9},
            {"hotkey_ctrl_win", voice.hotkey_ctrl_win},
            {"hotkey_rctrl_ralt", voice.hotkey_rctrl_ralt},
            {"hotkey_hold_space_lock", voice.hotkey_hold_space_lock},
            {"asr_provider", voice.asr_provider},
            {"doubao_auth_mode", voice.doubao_auth_mode},
            {"asr_app_key", voice.asr_app_key},
            {"asr_token", voice.asr_token},
            {"asr_tokens", voice.asr_tokens},
            {"asr_endpoint", voice.asr_endpoint},
            {"asr_resource_id", voice.asr_resource_id},
            {"doubao_enable_itn", voice.doubao_enable_itn},
            {"doubao_enable_punc", voice.doubao_enable_punc},
            {"doubao_enable_ddc", voice.doubao_enable_ddc},
            {"doubao_boosting_table_id", voice.doubao_boosting_table_id},
            {"asr_model", voice.asr_model},
            {"polish_provider", voice.polish_provider},
            {"polish_token", voice.polish_token},
            {"polish_tokens", voice.polish_tokens},
            {"polish_endpoint", voice.polish_endpoint},
            {"polish_model", voice.polish_model},
            {"polish_thinking", voice.polish_thinking},
            {"polish_prompt_id", voice.polish_prompt_id},
            {"polish_prompt", voice.polish_prompt},
            {"polish_prompt_custom_1", voice.polish_prompt_custom_1},
            {"polish_prompt_custom_2", voice.polish_prompt_custom_2},
            {"polish_prompt_custom_3", voice.polish_prompt_custom_3},
            {"language", voice.language},
            {"start_sound", voice.start_sound},
            {"end_sound", voice.end_sound},
            {"mute_system_audio", voice.mute_system_audio},
            {"notification_sound", voice.start_sound && voice.end_sound},
            {"polish_text", voice.polish_text},
            {"stream_inline_preedit", voice.stream_inline_preedit},
            {"commit_mode", voice.commit_mode}}},
          {"ai_assistant",
           {{"enabled", ai.enabled},
            {"provider", ai.provider},
            {"generate_provider", ai.generate_provider},
            {"token", ai.token},
            {"tokens", ai.tokens},
            {"endpoint", ai.endpoint},
            {"model", ai.model},
            {"endpoints", ai.endpoints},
            {"models", ai.models},
            {"providers", ProviderEntriesJson(ai)},
            {"candidate_limit", ai.candidate_limit},
            {"thinking", ai.thinking},
            {"json_mode", ai.json_mode},
            {"prompt", ai.prompt},
            {"prompt_id", ai.prompt_id},
            {"prompt_custom_1", ai.prompt_custom_1},
            {"prompt_custom_2", ai.prompt_custom_2},
            {"prompt_custom_3", ai.prompt_custom_3}}},
          {"tencent_tmt",
           {{"secret_id", tencent_tmt.secret_id},
            {"secret_key", tencent_tmt.secret_key},
            {"region", tencent_tmt.region},
            {"target_language", tencent_tmt.target_language}}},
          {"aliyun_mt", {{"enabled", aliyun_mt.enabled}, {"secret_id", aliyun_mt.secret_id},
                         {"secret_key", aliyun_mt.secret_key}}},
          {"custom_translation",
           {{"enabled", custom_translation.enabled},
            {"endpoint", custom_translation.endpoint},
            {"api_key", custom_translation.api_key}}},
          {"niutrans", {{"enabled", niutrans.enabled}, {"app_id", niutrans.app_id}, {"apikey", niutrans.apikey}}},
          {"translation", {{"provider", translation->provider}}},
          {"network", {{"proxy_mode", network_proxy.mode}, {"proxy_server", network_proxy.server}}},
          {"helpcode",
           {{"shuangpin_helpcode", GetConfiguredShuangpinHelpcodeEnabled()},
            {"shuangpin_helpcode_direct", GetConfiguredShuangpinHelpcodeDirect()},
            {"shuangpin_helpcode_trigger_backtick", GetConfiguredShuangpinHelpcodeTriggerBacktick()},
            {"shuangpin_helpcode_trigger_semicolon", GetConfiguredShuangpinHelpcodeTriggerSemicolon()},
            {"shuangpin_helpcode_trigger_uppercase", GetConfiguredShuangpinHelpcodeTriggerUppercase()},
            {"shuangpin_mid_sentence_helpcode", GetConfiguredShuangpinMidSentenceHelpcodeEnabled()},
            {"shuangpin_mid_sentence_helpcode_backtick", GetConfiguredShuangpinMidSentenceHelpcodeBacktick()},
            {"shuangpin_mid_sentence_helpcode_semicolon", GetConfiguredShuangpinMidSentenceHelpcodeSemicolon()},
            {"shuangpin_mid_sentence_helpcode_uppercase", GetConfiguredShuangpinMidSentenceHelpcodeUppercase()},
            {"shuangpin_direct_helpcode", GetConfiguredShuangpinDirectHelpcodeEnabled()},
            {"shuangpin_direct_helpcode_slash", GetConfiguredShuangpinDirectHelpcodeSlash()},
            {"shuangpin_direct_helpcode_uppercase", GetConfiguredShuangpinDirectHelpcodeUppercase()},
            {"shuangpin_direct_helpcode_end_trigger", GetConfiguredShuangpinDirectHelpcodeEndTrigger()},
            {"shuangpin_helpcode_schema", GetConfiguredShuangpinHelpcodeSchema()},
            {"quanpin_helpcode", GetConfiguredQuanpinHelpcodeEnabled()},
            {"quanpin_helpcode_schema", GetConfiguredQuanpinHelpcodeSchema()},
            {"show_sp_helpcode_in_candidate_window", GetConfiguredShowShuangpinHelpcodeInCandidateWindow()},
            {"show_qp_helpcode_in_candidate_window", GetConfiguredShowQuanpinHelpcodeInCandidateWindow()}}},
          {"quanpin",
           {{"autocorrect_transposition", GetConfiguredQuanpinAutocorrectTransposition()},
            {"autocorrect_neighbor", GetConfiguredQuanpinAutocorrectNeighbor()},
            {"autocorrect_deletion", GetConfiguredQuanpinAutocorrectDeletion()},
            {"autocorrect_insertion", GetConfiguredQuanpinAutocorrectInsertion()},
            {"autocorrect_marker", GetConfiguredQuanpinAutocorrectMarker()}}},
          {"statistics",
           {{"enabled", GetConfiguredStatisticsEnabled()}, {"retention", GetConfiguredStatisticsRetention()}}}}}};
    payload["data"]["voice_input"]["polish_presets"] = std::move(polish_presets);
    payload["data"]["helpcode"]["custom_schemas"] = CustomHelpcodeSchemasJson();
    payload["data"]["helpcode"]["custom_directory"] = GetCustomHelpcodeDirectory();
    payload["data"]["input"]["custom_shuangpin_schemas"] = CustomShuangpinSchemasJson();
    payload["data"]["input"]["custom_shuangpin_directory"] = GetCustomShuangpinDirectory();
    payload["protocolVersion"] = metasequoia::webview::Version;
    const std::string serialized = payload.dump();
    if (!metasequoia::webview::Validate(json::parse(serialized), "server"))
        throw std::runtime_error("Invalid config snapshot");
    return string_to_wstring(serialized);
}

SerialTaskQueue::Completion ConfigCompletion(bool refresh_skin_catalog = false, bool force_snapshot = false)
{
    TraceStartup(refresh_skin_catalog ? "worker: BuildConfigMessage(skins) begin" : "worker: BuildConfigMessage begin");
    auto message = BuildConfigMessage(refresh_skin_catalog);
    TraceStartup("worker: BuildConfigMessage end");
    const bool light = ResolveConfiguredTheme(GetConfiguredThemeSettings()) == "light";
    return [message = std::move(message), light, linger = GetConfiguredSettingsWindowLinger(), force_snapshot] {
        g_settings_light = light;
        g_settings_window_linger = linger;
        ApplySettingsChromeTheme();
        if (g_native)
            g_native->SetLightTheme(light);
        if (FrontendReady() && (force_snapshot || message != g_last_config_message))
        {
            if (PostToPage(message))
            {
                g_last_config_message = message;
                TraceStartup("ui: config posted to page");
            }
        }
        else if (!FrontendReady())
        {
            TraceStartup("ui: config dropped (window not ready)");
        }
    };
}

void PostConfig(bool refresh_skin_catalog = false)
{
    if (!g_worker || g_closing)
        return;
    g_worker->Submit([refresh_skin_catalog] { return ConfigCompletion(refresh_skin_catalog); });
}

bool ApplyConfigUpdate(const json::object &data)
{
    const std::string path = json::value_to<std::string>(data.at("path"));
    if (path == "input.mode")
        return SetConfiguredInputMode(json::value_to<std::string>(data.at("value")));
    if (path == "input.schema")
        return SetConfiguredInputScheme(json::value_to<std::string>(data.at("value")));
    if (path == "input.japanese_schema")
        return SetConfiguredJapaneseSchema(json::value_to<std::string>(data.at("value")));
    if (path == "input.character_set")
        return SetConfiguredCharacterSet(json::value_to<std::string>(data.at("value")));
    if (path == "input.default_ime_mode")
        return SetConfiguredDefaultImeMode(json::value_to<std::string>(data.at("value")));
    if (path == "input.ime_mode_scope")
        return SetConfiguredImeModeScope(json::value_to<std::string>(data.at("value")));
    if (path == "input.shuangpin_schema")
        return SetConfiguredShuangpinSchema(json::value_to<std::string>(data.at("value")));
    if (path == "input.wubi_schema")
        return SetConfiguredWubiSchema(json::value_to<std::string>(data.at("value")));
    if (path == "input.wubi_mixed_pinyin")
        return SetConfiguredWubiMixedPinyin(json::value_to<bool>(data.at("value")));
    if (path == "input.escape_keeps_selected_word")
        return SetConfiguredEscapeKeepsSelectedWord(json::value_to<bool>(data.at("value")));
    if (path == "input.enter_learns_english_word")
        return SetConfiguredEnterLearnsEnglishWord(json::value_to<bool>(data.at("value")));
    if (path == "input.wubi_z_mode")
        return SetConfiguredWubiZMode(json::value_to<std::string>(data.at("value")));
    if (path == "input.wubi_four_code_auto_commit")
        return SetConfiguredWubiFourCodeAutoCommit(json::value_to<bool>(data.at("value")));
    if (path == "input.wubi_fifth_code_top_commit")
        return SetConfiguredWubiFifthCodeTopCommit(json::value_to<bool>(data.at("value")));
    if (path == "input.word_to_character")
        return SetConfiguredWordToCharacterEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.word_to_character_keys")
        return SetConfiguredWordToCharacterKeys(json::value_to<std::string>(data.at("value")));
    if (path == "input.smart_punctuation")
        return SetConfiguredSmartPunctuationEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.smart_punctuation_space_convert")
        return SetConfiguredSmartPunctuationSpaceConvertEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.smart_punctuation_direct_digit")
        return SetConfiguredSmartPunctuationDirectDigitEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.smart_punctuation_direct_letter")
        return SetConfiguredSmartPunctuationDirectLetterEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.smart_punctuation_repeat_to_chinese")
        return SetConfiguredSmartPunctuationRepeatToChineseEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.paired_punctuation")
        return SetConfiguredPairedPunctuationEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.enumeration_comma_backslash")
        return SetConfiguredEnumerationCommaBackslashEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.enumeration_comma_slash")
        return SetConfiguredEnumerationCommaSlashEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.punctuation_lock")
        return SetConfiguredPunctuationLock(json::value_to<std::string>(data.at("value")));
    constexpr std::string_view input_section_prefix = "input.";
    constexpr std::string_view fuzzy_prefix = "input.fuzzy_";
    // 精确分支必须在前：总开关键同样命中 fuzzy_ 前缀，不能靠 prefix setter 拒绝它。
    if (path == "input.fuzzy_pinyin")
        return SetConfiguredFuzzyPinyinEnabled(json::value_to<bool>(data.at("value")));
    if (path.rfind(fuzzy_prefix, 0) == 0)
        return SetConfiguredFuzzyPinyinRule(path.substr(input_section_prefix.size()),
                                            json::value_to<bool>(data.at("value")));
    if (path == "appearance.tsf_preedit_style")
        return SetConfiguredTsfPreeditStyle(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.tsf_preedit_shuangpin_quanpin")
        return SetConfiguredTsfPreeditShuangpinQuanpin(json::value_to<bool>(data.at("value")));
    if (path == "appearance.settings_window_linger")
        return SetConfiguredSettingsWindowLinger(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.settings_launch_animation")
        return SetConfiguredSettingsLaunchAnimation(json::value_to<bool>(data.at("value")));
    if (path == "appearance.auto_download_update")
        return SetConfiguredAutoDownloadUpdate(json::value_to<bool>(data.at("value")));
    if (path == "appearance.candidate_window_layout")
        return SetConfiguredCandidateWindowLayout(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.candidate_window_follow_cursor")
        return SetConfiguredCandidateWindowFollowCursor(json::value_to<bool>(data.at("value")));
    if (path == "appearance.candidate_window_fit_content")
        return SetConfiguredCandidateWindowFitContent(json::value_to<bool>(data.at("value")));
    if (path == "appearance.candidate_window_single_line")
        return SetConfiguredCandidateWindowSingleLine(json::value_to<bool>(data.at("value")));
    if (path == "appearance.candidate_skin")
        return SetConfiguredCandidateSkin(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.candidate_window_preedit_style")
        return SetConfiguredCandidateWindowPreeditStyle(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.candidate_window_preedit_shuangpin_quanpin")
        return SetConfiguredCandidateWindowPreeditShuangpinQuanpin(json::value_to<bool>(data.at("value")));
    if (path == "appearance.candidate_fixed_badge")
        return SetConfiguredCandidateFixedBadge(json::value_to<bool>(data.at("value")));
    if (path == "appearance.candidate_fixed_badge_style")
        return SetConfiguredCandidateFixedBadgeStyle(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.candidate_ai_badge_style")
        return SetConfiguredCandidateAiBadgeStyle(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.page_size")
        return SetConfiguredCandidatePageSize(static_cast<int>(data.at("value").as_int64()));
    if (path == "appearance.font")
        return SetConfiguredCandidateFont(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.fallback_fonts")
        return SetConfiguredCandidateFallbackFonts(
            json::value_to<std::vector<std::string>>(json::parse(json::value_to<std::string>(data.at("value")))));
    if (path == "appearance.english_font")
        return SetConfiguredCandidateEnglishFont(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.font_size")
        return SetConfiguredCandidateFontSize(static_cast<int>(data.at("value").as_int64()));
    if (path == "appearance.candidate_window_preedit_font_size")
        return SetConfiguredCandidateWindowPreeditFontSize(static_cast<int>(data.at("value").as_int64()));
    if (path == "appearance.cand_text_color")
        return SetConfiguredCandidateTextColor(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_mode")
        return SetConfiguredThemeMode(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_settings")
        return SetConfiguredThemeSettings(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_cand")
        return SetConfiguredThemeCand(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_ftb")
        return SetConfiguredThemeFtb(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_menu")
        return SetConfiguredThemeMenu(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_emoji")
        return SetConfiguredThemeEmoji(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_screen_keyboard")
        return SetConfiguredThemeScreenKeyboard(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_handwriting")
        return SetConfiguredThemeHandwriting(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_voice")
        return SetConfiguredThemeVoice(json::value_to<std::string>(data.at("value")));
    if (path == "general.floating_toolbar")
        return SetConfiguredFloatingToolbarEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.caret_state_indicator")
        return SetConfiguredCaretStateIndicatorEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.caret_state_indicator_on_focus")
        return SetConfiguredCaretStateIndicatorOnFocus(json::value_to<bool>(data.at("value")));
    if (path == "general.caret_state_indicator_position")
        return SetConfiguredCaretStateIndicatorPosition(json::value_to<std::string>(data.at("value")));
    if (path == "general.diagnostic_log" || path == "general.candidate_window_diagnostic_log")
        return SetConfiguredDiagnosticLogEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.tsf_diagnostic_log")
        return SetConfiguredTsfDiagnosticLogEnabled(json::value_to<bool>(data.at("value")));
    if (path == "statistics.enabled")
        return SetConfiguredStatisticsEnabled(json::value_to<bool>(data.at("value")));
    if (path == "statistics.retention")
        return SetConfiguredStatisticsRetention(json::value_to<std::string>(data.at("value")));
    if (path == "general.floating_toolbar_scale")
    {
        const json::value &value = data.at("value");
        const double scale = value.is_double() ? value.as_double() : static_cast<double>(value.as_int64());
        return SetConfiguredFloatingToolbarScale(scale);
    }
    if (path == "general.floating_toolbar_font_size")
        return SetConfiguredFloatingToolbarFontSize(static_cast<int>(data.at("value").as_int64()));
    // Must precede the item prefix below, which would reject these as unknown items.
    if (path == "general.floating_toolbar_auto_hide")
        return SetConfiguredFloatingToolbarAutoHide(json::value_to<bool>(data.at("value")));
    if (path == "general.floating_toolbar_auto_hide_delay")
        return SetConfiguredFloatingToolbarAutoHideDelay(static_cast<int>(data.at("value").as_int64()));
    if (path == "general.floating_toolbar_shadow")
        return SetConfiguredFloatingToolbarShadow(json::value_to<bool>(data.at("value")));
    constexpr std::string_view toolbar_prefix = "general.floating_toolbar_";
    if (path.rfind(toolbar_prefix, 0) == 0)
        return SetConfiguredFloatingToolbarItemEnabled(path.substr(toolbar_prefix.size()),
                                                       json::value_to<bool>(data.at("value")));
    if (path == "general.cn_en_mixed_input")
        return SetConfiguredEnglishCandidatesEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.candidate_translations")
        return SetConfiguredCandidateTranslationsEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.candidate_expansion_panel_enabled")
        return SetConfiguredCandidateExpansionPanelEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.candidate_expansion_panel_translation")
        return SetConfiguredCandidateExpansionPanelTranslationEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.candidate_expansion_panel_emoji")
        return SetConfiguredCandidateExpansionPanelEmojiEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.candidate_expansion_panel_kaomoji")
        return SetConfiguredCandidateExpansionPanelKaomojiEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.candidate_expansion_panel_symbols")
        return SetConfiguredCandidateExpansionPanelSymbolsEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.candidate_expansion_panel_translation_max_count")
        return SetConfiguredCandidateExpansionPanelTranslationMaxCount(static_cast<int>(data.at("value").as_int64()));
    if (path == "general.candidate_expansion_panel_emoji_max_count")
        return SetConfiguredCandidateExpansionPanelEmojiMaxCount(static_cast<int>(data.at("value").as_int64()));
    if (path == "general.candidate_expansion_panel_kaomoji_max_count")
        return SetConfiguredCandidateExpansionPanelKaomojiMaxCount(static_cast<int>(data.at("value").as_int64()));
    if (path == "general.candidate_expansion_panel_symbols_max_count")
        return SetConfiguredCandidateExpansionPanelSymbolsMaxCount(static_cast<int>(data.at("value").as_int64()));
    if (path == "general.candidate_expansion_panel_placement")
        return data.at("value").is_string() &&
               SetConfiguredCandidateExpansionPanelPlacement(json::value_to<std::string>(data.at("value")));
    if (path == "general.candidate_translation_show_pos")
        return SetConfiguredCandidateTranslationShowPos(json::value_to<bool>(data.at("value")));
    if (path == "general.candidate_translation_single_sense")
        return SetConfiguredCandidateTranslationSingleSense(json::value_to<bool>(data.at("value")));
    if (path == "general.cn_en_mixed_input_min_chars")
        return SetConfiguredEnglishMixedInputMinChars(static_cast<int>(data.at("value").as_int64()));
    if (path == "general.cn_en_mixed_input_first_page_count")
        return SetConfiguredEnglishMixedInputFirstPageCount(static_cast<int>(data.at("value").as_int64()));
    if (path == "general.emoji_mixed_input")
        return SetConfiguredEmojiMixedInputEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.kaomoji_mixed_input")
        return SetConfiguredKaomojiMixedInputEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.symbol_mixed_input")
        return SetConfiguredSymbolMixedInputEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.cloud_candidates")
        return SetConfiguredCloudCandidatesEnabled(json::value_to<bool>(data.at("value")));
    if (path == "association.sentence_wordlattice")
        return SetConfiguredAssocSentenceWordLattice(json::value_to<bool>(data.at("value")));
    if (path == "association.sentence_google")
        return SetConfiguredAssocSentenceGoogle(json::value_to<bool>(data.at("value")));
    if (path == "association.sentence_neural_desktop")
        return SetConfiguredAssocSentenceNeuralDesktop(json::value_to<bool>(data.at("value")));
    if (path == "association.context_candidate_rerank")
        return SetConfiguredAssocContextCandidateRerank(json::value_to<bool>(data.at("value")));
    if (path == "association.sentence_neural_keyboard")
        return SetConfiguredAssocSentenceNeuralKeyboard(json::value_to<bool>(data.at("value")));
    if (path == "association.sentence_show_next_on_duplicate")
        return SetConfiguredAssocSentenceShowNextOnDuplicate(json::value_to<bool>(data.at("value")));
    if (path == "association.sentence_source_badge")
        return SetConfiguredAssocSentenceSourceBadge(json::value_to<bool>(data.at("value")));
    if (path == "association.sentence_collocation_enabled")
        return SetConfiguredAssocSentenceCollocationEnabled(json::value_to<bool>(data.at("value")));
    if (path == "association.sentence_collocation_model")
        return SetConfiguredAssocSentenceCollocationModel(json::value_to<std::string>(data.at("value")));
    if (path == "utility.radical_mode")
        return SetConfiguredRadicalModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.quick_phrase")
        return SetConfiguredQuickPhraseEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.quick_phrase_candidates")
        return SetConfiguredQuickPhraseCandidatesEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.quick_phrase_frequency")
        return SetConfiguredQuickPhraseFrequencyEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.mixed_candidates")
        return SetConfiguredMixedCandidatesEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.date_time_candidates")
        return SetConfiguredDateTimeCandidatesEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.date_time_menu")
        return SetConfiguredDateTimeMenuEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.date_time_mode")
        return SetConfiguredDateTimeModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.emoji_mode")
        return SetConfiguredEmojiModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.kaomoji_mode")
        return SetConfiguredKaomojiModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.symbol_mode")
        return SetConfiguredSymbolModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.jianpin_mode")
        return SetConfiguredJianpinModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.y_mode")
        return SetConfiguredYModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.r_mode")
        return SetConfiguredRModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.v_mode")
        return SetConfiguredVModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.clipboard_history")
        return SetConfiguredClipboardHistoryEnabled(json::value_to<bool>(data.at("value")));
    if (path == "language_bar.enabled")
        return SetConfiguredLanguageBarEnabled(json::value_to<bool>(data.at("value")));
    if (path == "language_bar.show_ime_mode")
        return SetConfiguredLanguageBarShowImeMode(json::value_to<bool>(data.at("value")));
    if (path == "language_bar.show_double_single_byte")
        return SetConfiguredLanguageBarShowDoubleSingleByte(json::value_to<bool>(data.at("value")));
    if (path == "language_bar.show_punctuation")
        return SetConfiguredLanguageBarShowPunctuation(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_minus_equal")
        return SetConfiguredPagingMinusEqualEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_tab")
        return SetConfiguredPagingTabEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_comma_period")
        return SetConfiguredPagingCommaPeriodEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_brackets")
        return SetConfiguredPagingBracketsEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_page_up_down")
        return SetConfiguredPagingPageUpDownEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_mouse_wheel")
        return SetConfiguredPagingMouseWheelEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.candidate_arrow_navigation")
        return SetConfiguredCandidateArrowNavigationEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.candidate_horizontal_arrow_navigation")
        return SetConfiguredCandidateHorizontalArrowNavigationEnabled(json::value_to<bool>(data.at("value")));
    if (path == "keybindings.switch_language_shift")
        return SetConfiguredSwitchLanguageShiftEnabled(json::value_to<bool>(data.at("value")));
    if (path == "keybindings.switch_language_ctrl")
        return SetConfiguredSwitchLanguageCtrlEnabled(json::value_to<bool>(data.at("value")));
    if (path == "keybindings.switch_language_ctrl_alt_space")
        return SetConfiguredSwitchLanguageCtrlAltSpaceEnabled(json::value_to<bool>(data.at("value")));
    if (path == "keybindings.toggle_character_set_ctrl_shift_f")
        return SetConfiguredCharacterSetShortcutEnabled(json::value_to<bool>(data.at("value")));
    if (path == "keybindings.commit_pinyin_shift_enter")
        return SetConfiguredPinyinCommitShortcutEnabled(json::value_to<bool>(data.at("value")));
    if (path == "keybindings.candidate_expansion_key")
        return data.at("value").is_string() &&
               SetConfiguredCandidateExpansionKey(json::value_to<std::string>(data.at("value")));
    if (path == "keybindings.candidate_expansion_move_prev_key")
        return data.at("value").is_string() &&
               SetConfiguredCandidateExpansionMovePrevKey(json::value_to<std::string>(data.at("value")));
    if (path == "keybindings.candidate_expansion_move_next_key")
        return data.at("value").is_string() &&
               SetConfiguredCandidateExpansionMoveNextKey(json::value_to<std::string>(data.at("value")));
    if (path == "keybindings.trilingual_cycle")
        return SetConfiguredTrilingualCycleEnabled(json::value_to<bool>(data.at("value")));
    if (const std::size_t index = InputStateShortcutIndex(path); index < FanyImeInputStateShortcuts::Definitions.size())
        return data.at("value").is_string() &&
               SetConfiguredInputStateShortcut(index, json::value_to<std::string>(data.at("value")));
    if (path.rfind("frequency_adjustment.", 0) == 0)
    {
        const std::string key = path.substr(21);
        const json::value &value = data.at("value");
        if (value.is_string())
            return SetConfiguredFrequencyAdjustmentString(key, json::value_to<std::string>(value));
        if (value.is_int64())
            return SetConfiguredFrequencyAdjustmentInt(key, static_cast<int>(value.as_int64()));
        return false;
    }
    if (path.rfind("voice_input.", 0) == 0)
    {
        const std::string key = path.substr(12);
        const json::value &value = data.at("value");
        if (value.is_bool())
        {
            return SetConfiguredVoiceInputBool(key, json::value_to<bool>(value));
        }
        if (value.is_string())
            return SetConfiguredVoiceInputString(key, json::value_to<std::string>(value));
        return false;
    }
    if (path.rfind("ai_assistant.", 0) == 0)
    {
        std::string key = path.substr(13);
        const json::value &value = data.at("value");
        // `endpoint@<id>` / `model@<id>`：设置页按服务商写槽位。@ 不会出现在任何配置键名里，
        // 用它在一条 configUpdate 里指明目标服务商，宿主据此判断单值键要不要跟着改，
        // 不必拿后端此刻的活动服务商去猜页面在说哪一家。
        const size_t at = key.find('@');
        if (at != std::string::npos)
        {
            const std::string field = key.substr(0, at);
            const std::string provider = key.substr(at + 1);
            if ((field != "endpoint" && field != "model") || provider.empty() || !value.is_string())
                return false;
            return SetConfiguredAiAssistantProviderValue(provider, field, json::value_to<std::string>(value));
        }
        if (value.is_bool())
            return SetConfiguredAiAssistantBool(key, json::value_to<bool>(value));
        if (value.is_string())
            return SetConfiguredAiAssistantString(key, json::value_to<std::string>(value));
        if (value.is_int64())
            return SetConfiguredAiAssistantInt(key, static_cast<int>(value.as_int64()));
        return false;
    }
    if (path.rfind("tencent_tmt.", 0) == 0)
    {
        const json::value &value = data.at("value");
        return value.is_string() && SetConfiguredTencentTmtString(path.substr(std::string("tencent_tmt.").size()),
                                                                  json::value_to<std::string>(value));
    }
    if (path == "aliyun_mt.enabled")
        return SetConfiguredAliyunMtBool("enabled", json::value_to<bool>(data.at("value")));
    if (path.rfind("aliyun_mt.", 0) == 0)
    {
        const json::value &value = data.at("value");
        return value.is_string() && SetConfiguredAliyunMtString(path.substr(std::string("aliyun_mt.").size()),
                                                                json::value_to<std::string>(value));
    }
    if (path == "custom_translation.enabled")
        return SetConfiguredCustomTranslationBool("enabled", json::value_to<bool>(data.at("value")));
    if (path.rfind("custom_translation.", 0) == 0)
    {
        const json::value &value = data.at("value");
        return value.is_string() &&
               SetConfiguredCustomTranslationString(path.substr(std::string("custom_translation.").size()),
                                                    json::value_to<std::string>(value));
    }
    if (path == "niutrans.enabled")
        return SetConfiguredNiuTransBool("enabled", json::value_to<bool>(data.at("value")));
    if (path.rfind("niutrans.", 0) == 0)
    {
        const json::value &value = data.at("value");
        return value.is_string() && SetConfiguredNiuTransString(path.substr(std::string("niutrans.").size()),
                                                                json::value_to<std::string>(value));
    }
    if (path == "translation.provider")
    {
        const json::value &value = data.at("value");
        return value.is_string() && SetConfiguredTranslationString("provider", json::value_to<std::string>(value));
    }
    if (path.rfind("network.", 0) == 0)
    {
        const json::value &value = data.at("value");
        return value.is_string() && SetConfiguredNetworkString(path.substr(std::string("network.").size()),
                                                               json::value_to<std::string>(value));
    }
    if (path == "helpcode.show_sp_helpcode_in_candidate_window")
        return SetConfiguredShowShuangpinHelpcodeInCandidateWindow(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_helpcode")
        return SetConfiguredShuangpinHelpcodeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_helpcode_direct")
        return SetConfiguredShuangpinHelpcodeDirect(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_helpcode_trigger_backtick")
        return SetConfiguredShuangpinHelpcodeTriggerBacktick(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_helpcode_trigger_semicolon")
        return SetConfiguredShuangpinHelpcodeTriggerSemicolon(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_helpcode_trigger_uppercase")
        return SetConfiguredShuangpinHelpcodeTriggerUppercase(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_mid_sentence_helpcode")
        return SetConfiguredShuangpinMidSentenceHelpcodeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_mid_sentence_helpcode_backtick")
        return SetConfiguredShuangpinMidSentenceHelpcodeBacktick(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_mid_sentence_helpcode_semicolon")
        return SetConfiguredShuangpinMidSentenceHelpcodeSemicolon(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_mid_sentence_helpcode_uppercase")
        return SetConfiguredShuangpinMidSentenceHelpcodeUppercase(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_direct_helpcode")
        return SetConfiguredShuangpinDirectHelpcodeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_direct_helpcode_slash")
        return SetConfiguredShuangpinDirectHelpcodeSlash(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_direct_helpcode_uppercase")
        return SetConfiguredShuangpinDirectHelpcodeUppercase(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_direct_helpcode_end_trigger")
        return SetConfiguredShuangpinDirectHelpcodeEndTrigger(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_helpcode_schema")
        return SetConfiguredShuangpinHelpcodeSchema(json::value_to<std::string>(data.at("value")));
    if (path == "helpcode.quanpin_helpcode")
        return SetConfiguredQuanpinHelpcodeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.quanpin_helpcode_schema")
        return SetConfiguredQuanpinHelpcodeSchema(json::value_to<std::string>(data.at("value")));
    if (path == "helpcode.show_qp_helpcode_in_candidate_window")
        return SetConfiguredShowQuanpinHelpcodeInCandidateWindow(json::value_to<bool>(data.at("value")));
    if (path == "quanpin.autocorrect_transposition")
        return SetConfiguredQuanpinAutocorrectTransposition(json::value_to<bool>(data.at("value")));
    if (path == "quanpin.autocorrect_neighbor")
        return SetConfiguredQuanpinAutocorrectNeighbor(json::value_to<bool>(data.at("value")));
    if (path == "quanpin.autocorrect_deletion")
        return SetConfiguredQuanpinAutocorrectDeletion(json::value_to<bool>(data.at("value")));
    if (path == "quanpin.autocorrect_insertion")
        return SetConfiguredQuanpinAutocorrectInsertion(json::value_to<bool>(data.at("value")));
    if (path == "quanpin.autocorrect_marker")
        return SetConfiguredQuanpinAutocorrectMarker(json::value_to<bool>(data.at("value")));
    return false;
}

bool CopyTextToClipboard(HWND hwnd, const std::wstring &text)
{
    if (!OpenClipboard(hwnd))
        return false;
    EmptyClipboard();

    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory)
    {
        CloseClipboard();
        return false;
    }

    void *destination = GlobalLock(memory);
    if (!destination)
    {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }

    memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory))
    {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}

std::filesystem::path StatisticsDatabasePath()
{
    return std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"stats.db";
}

// The local calendar day "today" is bucketed into. GetLocalTime resolves the
// current timezone and DST, matching the Server-side aggregator's resolver.
int LocalTodayDayKey()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return MsimeStats::DayKeyOf(now.wYear, now.wMonth, now.wDay);
}

json::object OverviewToJson(const MsimeStats::Overview &overview)
{
    json::object categories;
    categories["cjk"] = overview.categories.cjk;
    categories["latin"] = overview.categories.latin;
    categories["digit"] = overview.categories.digit;
    categories["punct"] = overview.categories.punct;
    categories["other"] = overview.categories.other;

    json::array hourly;
    hourly.reserve(overview.today_hourly.size());
    for (const int64_t chars : overview.today_hourly)
        hourly.push_back(json::value(chars));

    json::array daily;
    daily.reserve(overview.daily.size());
    for (const MsimeStats::DailyRow &row : overview.daily)
    {
        json::object entry;
        entry["dayKey"] = row.day_key;
        entry["cjk"] = row.cjk;
        entry["latin"] = row.latin;
        entry["digit"] = row.digit;
        entry["punct"] = row.punct;
        entry["other"] = row.other;
        entry["activeMs"] = row.active_ms;
        entry["firstWordHits"] = row.first_word_hits;
        entry["firstWordTotal"] = row.first_word_total;
        daily.push_back(std::move(entry));
    }

    json::object result;
    result["hasData"] = overview.has_data;
    result["firstDayKey"] = overview.first_day_key;
    result["lastDayKey"] = overview.last_day_key;
    result["days"] = overview.days;
    result["totalChars"] = overview.total_chars;
    result["totalActiveMs"] = overview.total_active_ms;
    result["todayDayKey"] = overview.today_day_key;
    result["todayChars"] = overview.today_chars;
    result["todayActiveMs"] = overview.today_active_ms;
    result["averagePerDay"] = overview.average_per_day;
    result["currentStreak"] = overview.current_streak;
    result["longestStreak"] = overview.longest_streak;
    result["bestDayKey"] = overview.best_day_key;
    result["bestDayChars"] = overview.best_day_chars;
    result["todaySpeed"] = overview.today_speed;
    result["averageSpeed"] = overview.average_speed;
    result["fastestSpeed"] = overview.fastest_speed;
    result["fastestDayKey"] = overview.fastest_day_key;
    // optional 留空时写 JSON null：设置页按「无测量」渲染，绝不读成 0%。boost::json
    // 不认识 std::optional，须显式落成数值或 null。
    if (overview.today_first_word_ratio)
    {
        result["todayFirstWordRatio"] = *overview.today_first_word_ratio;
    }
    else
    {
        result["todayFirstWordRatio"] = json::value(nullptr);
    }
    if (overview.total_first_word_ratio)
    {
        result["totalFirstWordRatio"] = *overview.total_first_word_ratio;
    }
    else
    {
        result["totalFirstWordRatio"] = json::value(nullptr);
    }
    result["todayFirstWordTotal"] = overview.today_first_word_total;
    result["categories"] = std::move(categories);
    result["todayHourly"] = std::move(hourly);
    result["daily"] = std::move(daily);
    return result;
}

// Handles one statistics request on the worker thread. Each request opens the
// store, uses it and closes it: the settings process can exit at any time, the
// store carries no cross-request state, and all requests are serialised by the
// single serial task queue, so no in-process lock is needed. WAL plus the
// store's busy timeout arbitrate with the Server process that writes the same
// file.
json::object HandleStatsRequest(const json::object &data, HWND hwnd)
{
    const std::string request_id = json::value_to<std::string>(data.at("requestId"));
    const std::string action = json::value_to<std::string>(data.at("action"));
    json::object response;
    response["requestId"] = request_id;
    response["action"] = action;
    response["ok"] = true;
    response["message"] = "成功";

    if (action == "openDirectory")
    {
        const std::filesystem::path directory = StatisticsDatabasePath().parent_path();
        std::error_code error_code;
        std::filesystem::create_directories(directory, error_code);
        ShellExecuteW(hwnd, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        response["message"] = "已打开数据目录";
        return response;
    }
    if (action != "overview" && action != "clearAll")
    {
        response["ok"] = false;
        response["message"] = "未知的统计操作。";
        return response;
    }

    // The missing-file check is the observable half of "recording is off by
    // default": Store::Open always opens with SQLITE_OPEN_CREATE, so merely
    // browsing statistics must not create stats.db.
    std::error_code exists_error;
    const std::filesystem::path database_path = StatisticsDatabasePath();
    const bool database_exists = std::filesystem::exists(database_path, exists_error) && !exists_error;
    std::unique_ptr<MsimeStats::Store> store;
    if (database_exists)
    {
        std::string error;
        store = MsimeStats::Store::Open(database_path, error);
        if (!store)
        {
            response["ok"] = false;
            response["message"] = "无法打开统计数据库：" + error;
            return response;
        }
    }

    const int today = LocalTodayDayKey();
    if (action == "clearAll")
    {
        int64_t removed_days = 0;
        if (store)
        {
            std::string error;
            if (!store->ClearAll(removed_days, error))
            {
                response["ok"] = false;
                response["message"] = "清空统计数据失败：" + error;
                return response;
            }
        }
        response["removedDays"] = removed_days;
        response["message"] =
            removed_days > 0 ? "已清空 " + std::to_string(removed_days) + " 天数据" : "没有需要清空的数据";
    }

    MsimeStats::Overview overview;
    if (store)
    {
        std::vector<MsimeStats::DailyRow> daily;
        std::vector<MsimeStats::HourlyRow> hourly;
        std::string error;
        if (!store->DailyRows(daily, error) || !store->HourlyRows(today, hourly, error))
        {
            response["ok"] = false;
            response["message"] = "读取统计数据失败：" + error;
            return response;
        }
        overview = MsimeStats::ComputeOverview(daily, hourly, today);
    }
    else
    {
        // Derive the empty overview through the same path so the JSON shape is
        // identical (24 zeroed hour buckets, zeroed categories).
        overview = MsimeStats::ComputeOverview({}, {}, today);
    }
    response["overview"] = OverviewToJson(overview);
    if (action == "overview")
        response["message"] = "已加载统计数据";
    return response;
}

// The settings window posts its requests as webview-contract client messages, validated here
// against the same contract as every other surface.
void HandleClientMessage(HWND hwnd, const std::string &raw)
{
    if (g_closing || !g_worker)
        return;

    try
    {
        json::value value = json::parse(raw);
        if (!metasequoia::webview::Validate(value, "client", "settings"))
            return;
        const std::string type = json::value_to<std::string>(value.at("type"));
        if (type == "configRequest")
        {
            TraceStartup("ui: configRequest");
            // A snapshot sent before the new document installed its listener may be lost.
            g_last_config_message.clear();
            PostConfig(false);
            PostMessageW(hwnd, kScanSkinCatalog, 0, 0);
        }
        else if (type == "skinCatalogRequest")
        {
            g_worker->Submit([] {
                auto completion = ConfigCompletion(true);
                NotifyImeServerCandidateSkinRefresh();
                return completion;
            });
        }
        else if (type == "openSkinDirectory")
        {
            const std::filesystem::path skins = std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"skins";
            std::error_code ec;
            std::filesystem::create_directories(skins, ec);
            if (!ec)
                ShellExecuteW(hwnd, L"open", skins.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (type == "openLogDirectory")
        {
            // Keep in sync with the writer in src/log/candidate_diag_log.cpp: the
            // diagnostics live under the data directory, not on the Desktop.
            const std::filesystem::path logs = std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"logs";
            std::error_code ec;
            std::filesystem::create_directories(logs, ec);
            if (!ec)
                ShellExecuteW(hwnd, L"open", logs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (type == "openHelpcodeDirectory")
        {
            const std::filesystem::path directory = metasequoia::path_from_utf8(GetCustomHelpcodeDirectory().c_str());
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            if (!ec)
                ShellExecuteW(hwnd, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (type == "openShuangpinDirectory")
        {
            const std::filesystem::path directory = metasequoia::path_from_utf8(GetCustomShuangpinDirectory().c_str());
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            if (!ec)
                ShellExecuteW(hwnd, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (type == "collocationModelDownload")
        {
            // 后台线程下载，进度随下一次配置快照回给页面；页面在下载态轮询 configRequest。
            // 忙碌（其他模型在下载）时 StartDownload 拒绝，页面在下载态禁用其余下载按钮。
            collocation::StartDownload(json::value_to<std::string>(value.at("data").at("modelId")));
            PostConfig(false);
        }
        else if (type == "collocationModelDelete")
        {
            // 下载中与当前生效解析的 id 在 DeleteModel 里拒绝；无论成败都回快照刷新列表。
            collocation::DeleteModel(json::value_to<std::string>(value.at("data").at("modelId")));
            PostConfig(false);
        }
        else if (type == "collocationModelImport")
        {
            // 应用内下载太慢时，用户可以用浏览器下好再导入；选择对话框在 UI 线程模态弹出，
            // 复制与格式校验在后台线程，进度同样随快照回放（页面在 importing 态轮询）。
            const std::string model_id = json::value_to<std::string>(value.at("data").at("modelId"));
            const std::filesystem::path source = collocation::PromptForModelFile(hwnd);
            if (!source.empty())
                collocation::ImportModel(model_id, source);
            PostConfig(false);
        }
        else if (type == "collocationModelStatusRequest")
        {
            PostConfig(false);
        }
        else if (type == "neuralModelDownload")
        {
            // 与八股文模型同一套：后台下载，进度随快照回放，页面在下载态轮询。
            neural_model::StartDownload();
            PostConfig(false);
        }
        else if (type == "neuralModelImport")
        {
            const std::filesystem::path source = neural_model::PromptForModelFile(hwnd);
            if (!source.empty())
                neural_model::ImportModel(source);
            PostConfig(false);
        }
        else if (type == "neuralModelDelete")
        {
            neural_model::DeleteModel();
            PostConfig(false);
        }
        else if (type == "neuralModelStatusRequest")
        {
            PostConfig(false);
        }
        else if (type == "latticeModelDownload")
        {
            // 词格语言模型 sc.lm，与神经模型同一套：后台下载，进度随快照回放，页面在下载态轮询。
            lattice_model::StartDownload();
            PostConfig(false);
        }
        else if (type == "latticeModelImport")
        {
            const std::filesystem::path source = lattice_model::PromptForModelFile(hwnd);
            if (!source.empty())
                lattice_model::ImportModel(source);
            PostConfig(false);
        }
        else if (type == "latticeModelDelete")
        {
            // Server 映射着文件时删不掉，DeleteModel 改留删除标记，快照里 deletePending 提示重启。
            lattice_model::DeleteModel();
            PostConfig(false);
        }
        else if (type == "latticeModelStatusRequest")
        {
            PostConfig(false);
        }
        else if (type == "configUpdate")
        {
            const auto data = value.at("data").as_object();
            g_worker->Submit([data] {
                const bool saved = ApplyConfigUpdate(data);
                const std::string path = json::value_to<std::string>(data.at("path"));
                // The shortcut editor waits for persisted state even for a failed/no-op write.
                const bool force_snapshot = IsInputStateShortcutPath(path);
                auto completion = ConfigCompletion(false, force_snapshot);
                return [saved, completion = std::move(completion)] {
                    completion();
                    if (!saved)
                        MessageBoxW(g_settings_hwnd, L"设置保存失败，请重试。", L"水杉输入法", MB_OK | MB_ICONWARNING);
                };
            });
        }
        else if (type == "apiCredentialTest")
        {
            const auto &data = value.at("data").as_object();
            const std::string request_id = json::value_to<std::string>(data.at("requestId"));
            ApiCredentialTest::Request request;
            request.service = json::value_to<std::string>(data.at("service"));
            for (const auto &[key, item] : data.at("config").as_object())
            {
                if (item.is_string())
                    request.config.emplace(std::string(key), json::value_to<std::string>(item));
            }
            g_worker->Submit([request_id, request = std::move(request)]() -> SerialTaskQueue::Completion {
                ApiCredentialTest::Result result;
                try
                {
                    result = ApiCredentialTest::Run(request);
                }
                catch (...)
                {
                    result = {false, "测试失败：内部错误。"};
                }
                json::value response = {{"type", "apiCredentialTestResult"},
                                        {"requestId", request_id},
                                        {"ok", result.ok},
                                        {"message", result.message},
                                        {"protocolVersion", metasequoia::webview::Version}};
                if (!metasequoia::webview::Validate(response, "server"))
                    throw std::runtime_error("Invalid API credential test response");
                auto message = string_to_wstring(json::serialize(response));
                return [message = std::move(message)] { PostToPage(message); };
            });
        }
        else if (type == "apiModelList")
        {
            const auto &data = value.at("data").as_object();
            const std::string request_id = json::value_to<std::string>(data.at("requestId"));
            ApiCredentialTest::Request request;
            request.service = json::value_to<std::string>(data.at("service"));
            for (const auto &[key, item] : data.at("config").as_object())
            {
                if (item.is_string())
                    request.config.emplace(std::string(key), json::value_to<std::string>(item));
            }
            g_worker->Submit([request_id, request = std::move(request)]() -> SerialTaskQueue::Completion {
                ApiCredentialTest::ModelListResult result;
                try
                {
                    result = ApiCredentialTest::FetchModels(request);
                }
                catch (...)
                {
                    result = {false, "获取模型列表失败：内部错误。", {}};
                }
                json::array models_array;
                for (const auto &m : result.models)
                    models_array.push_back(json::value(m));
                json::value response = {{"type", "apiModelListResult"},
                                        {"requestId", request_id},
                                        {"ok", result.ok},
                                        {"message", result.message},
                                        {"models", std::move(models_array)},
                                        {"protocolVersion", metasequoia::webview::Version}};
                if (!metasequoia::webview::Validate(response, "server"))
                    throw std::runtime_error("Invalid API model list response");
                auto message = string_to_wstring(json::serialize(response));
                return [message = std::move(message)] { PostToPage(message); };
            });
        }
        else if (type == "aiProviderSync")
        {
            const auto &data = value.at("data").as_object();
            const std::string request_id = json::value_to<std::string>(data.at("requestId"));
            std::vector<AiAssistant::AiProvider> entries;
            for (const auto &item : data.at("entries").as_array())
            {
                const auto &object = item.as_object();
                AiAssistant::AiProvider entry;
                entry.id = json::value_to<std::string>(object.at("id"));
                entry.name = json::value_to<std::string>(object.at("name"));
                entry.endpoint = json::value_to<std::string>(object.at("endpoint"));
                entry.token = json::value_to<std::string>(object.at("token"));
                entry.model = json::value_to<std::string>(object.at("model"));
                entries.push_back(std::move(entry));
            }
            const bool ok = SetAiAssistantProviders(entries);
            const std::string result_message = ok ? "已保存 " + std::to_string(entries.size()) + " 个服务商"
                                                  : "保存失败：服务商列表为空或包含无效条目。";
            json::value response = {{"type", "aiProviderSyncResult"},
                                    {"requestId", request_id},
                                    {"ok", ok},
                                    {"message", result_message},
                                    {"protocolVersion", metasequoia::webview::Version}};
            if (!metasequoia::webview::Validate(response, "server"))
                throw std::runtime_error("Invalid AI provider sync response");
            auto message = string_to_wstring(json::serialize(response));
            g_worker->Submit([message = std::move(message)]() -> SerialTaskQueue::Completion {
                return [message] { PostToPage(message); };
            });
        }
        else if (type == "openKeyboardPanel")
        {
            OpenKeyboardPanelApplication();
        }
        else if (type == "openHandwritingPanel")
        {
            OpenHandwritingPanelApplication();
        }
        else if (type == "restartServer")
        {
            // 入口已保证 g_worker 非空且未在关闭：从那里到这里只经过 JSON 解析和这次同步的
            // 管道写，都不泵消息，所以 g_closing / g_worker 中途不会变。若有哪个分支被改成
            // 会泵消息的等待，就要改回「发送后重新检查状态、不可用也发回执」的写法。
            const bool ok = NotifyImeServerRestart();
            if (!ok)
            {
                MessageBoxW(g_settings_hwnd, L"未能重启输入法进程，请稍后重试。", L"水杉输入法",
                            MB_OK | MB_ICONWARNING);
            }
            else
            {
                // Watchdog 拉起新 Server 后才会落位 .gram.new；设置进程不会因为这次重启
                // 收到配置变化，所以在接下来几秒主动拉快照，待更新清掉或次数用完就停。
                // 不重启设置进程：避免单实例/重开时序问题和窗口闪现。
                g_restart_refresh_remaining = kRestartRefreshTicks;
                SetTimer(hwnd, kRestartRefreshTimer, 500, nullptr);
            }
            // 回执只有一个出口：无论重启是否成功，页面按 restartServerDone 恢复按钮。
            PostRestartServerDone(ok);
        }
        else if (type == "openExternalUrl")
        {
            const std::string url = json::value_to<std::string>(value.at("data"));
            if (url.rfind("https://", 0) == 0)
                ShellExecuteW(hwnd, L"open", string_to_wstring(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (type == "copyText")
        {
            CopyTextToClipboard(hwnd, string_to_wstring(json::value_to<std::string>(value.at("data"))));
        }
        else if (type == "dictionaryRequest")
        {
            const auto data = value.at("data").as_object();
            g_worker->Submit([data]() -> SerialTaskQueue::Completion {
                json::value response;
                try
                {
                    response = SettingsDictionary::HandleRequest(data);
                }
                catch (...)
                {
                    response =
                        json::object{{"ok", false}, {"message", "词库操作失败，请重试"}, {"rows", json::array{}}};
                }
                auto &object = response.as_object();
                object["type"] = "dictionaryResponse";
                object["requestId"] = data.at("requestId");
                object["action"] = data.at("action");
                object["dictionary"] = data.at("dictionary");
                object["protocolVersion"] = metasequoia::webview::Version;
                if (!metasequoia::webview::Validate(response, "server"))
                    throw std::runtime_error("Invalid dictionary response");
                auto message = string_to_wstring(json::serialize(response));
                return [message = std::move(message)] { PostToPage(message); };
            });
        }
        else if (type == "defaultImeRequest")
        {
            const auto &data = value.at("data").as_object();
            const std::string request_id = json::value_to<std::string>(data.at("requestId"));
            const std::string action = json::value_to<std::string>(data.at("action"));
            if (action == "openSystemSettings")
                default_input_method::OpenSystemSettings(hwnd);
            // 设为默认会调用 input.dll 并复核注册表，放到工作线程，避免卡住设置窗口。
            g_worker->Submit([request_id, action]() -> SerialTaskQueue::Completion {
                bool ok = true;
                std::string message;
                if (action == "setDefault")
                {
                    ok = default_input_method::SetAsDefault(message);
                    if (ok)
                        message = "已设为默认输入法，水杉输入法在中文（简体）中排第一位。";
                }
                const default_input_method::Status status = default_input_method::QueryStatus();
                json::value response = {{"type", "defaultImeResponse"},
                                        {"requestId", request_id},
                                        {"action", action},
                                        {"ok", ok},
                                        {"message", message},
                                        {"enabled", status.enabled},
                                        {"isDefault", status.is_default},
                                        {"protocolVersion", metasequoia::webview::Version}};
                if (!metasequoia::webview::Validate(response, "server"))
                    throw std::runtime_error("Invalid default IME response");
                auto serialized = string_to_wstring(json::serialize(response));
                return [serialized = std::move(serialized)] { PostToPage(serialized); };
            });
        }
        else if (type == "gameCompatRequest")
        {
            const auto &data = value.at("data").as_object();
            const std::string request_id = json::value_to<std::string>(data.at("requestId"));
            const std::string action = json::value_to<std::string>(data.at("action"));
            const bool enabled = data.contains("enabled") && data.at("enabled").as_bool();
            const std::wstring name =
                data.contains("name") ? string_to_wstring(json::value_to<std::string>(data.at("name"))) : L"";
            // Apply waits for the UAC prompt and the elevated instance; keep it off the window thread.
            g_worker->Submit([request_id, action, enabled, name, hwnd]() -> SerialTaskQueue::Completion {
                bool ok = true;
                std::string message;
                if (action == "apply")
                {
                    ok = game_compat_name::Apply(hwnd, enabled, name, message);
                    if (ok)
                        message = enabled ? "已应用。请完全退出并重新启动游戏后生效。"
                                          : "已恢复原名称。请重新启动游戏后生效。";
                }
                const game_compat_name::Status status = game_compat_name::QueryStatus();
                json::value response = {
                    {"type", "gameCompatResponse"},
                    {"requestId", request_id},
                    {"action", action},
                    {"ok", ok},
                    {"message", message},
                    {"enabled", status.enabled},
                    {"name", wstring_to_string(status.name)},
                    {"registeredName", wstring_to_string(status.registeredName)},
                    {"defaultName", wstring_to_string(std::wstring(game_compat_name::kDefaultName))},
                    {"protocolVersion", metasequoia::webview::Version}};
                if (!metasequoia::webview::Validate(response, "server"))
                    throw std::runtime_error("Invalid game compat response");
                auto serialized = string_to_wstring(json::serialize(response));
                return [serialized = std::move(serialized)] { PostToPage(serialized); };
            });
        }
        else if (type == "statsRequest")
        {
            const auto data = value.at("data").as_object();
            g_worker->Submit([data, hwnd]() -> SerialTaskQueue::Completion {
                json::object response;
                try
                {
                    response = HandleStatsRequest(data, hwnd);
                }
                catch (...)
                {
                    response["ok"] = false;
                    response["message"] = "统计操作失败，请重试";
                }
                response["requestId"] = data.at("requestId");
                response["action"] = data.at("action");
                response["type"] = "statsResponse";
                response["protocolVersion"] = metasequoia::webview::Version;
                if (!metasequoia::webview::Validate(response, "server"))
                    throw std::runtime_error("Invalid statistics response");
                auto message = string_to_wstring(json::serialize(response));
                return [message = std::move(message)] { PostToPage(message); };
            });
        }
    }
    catch (const std::exception &)
    {
        // Ignore malformed messages from the page; the next snapshot restores its state.
    }
}

// 隐藏或关闭窗口时按着的键不会再有抬起送到这里，不放掉抓取的话下一次抬起会落到隐藏的窗口上。
void ReleaseMouseCapture(HWND hwnd)
{
    if (GetCapture() == hwnd)
        ReleaseCapture();
}

int TopNonClientInset(HWND hwnd)
{
    const UINT dpi = GetDpiForWindow(hwnd);
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    RECT caption{}, no_caption{};
    AdjustWindowRectExForDpi(&caption, style, FALSE, ex_style, dpi);
    AdjustWindowRectExForDpi(&no_caption, style & ~WS_CAPTION, FALSE, ex_style, dpi);
    return no_caption.top - caption.top + GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
           GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

// 重新露面：取消延迟退出，恢复渲染与配置轮询。
void CancelLinger(HWND hwnd)
{
    if (!g_lingering)
        return;
    g_lingering = false;
    KillTimer(hwnd, kLingerTimer);
    SetTimer(hwnd, kConfigReloadTimer, 300, nullptr);
}

void ActivateWindow(HWND hwnd)
{
    if (g_closing)
        return;
    CancelLinger(hwnd);
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
    else
        ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
}

// 驻留时长（毫秒）：0 表示关闭即退出，std::nullopt 表示一直驻留。
std::optional<UINT> SettingsLingerTimeoutMs(const std::string &linger)
{
    if (linger == "forever")
        return std::nullopt;
    if (linger == "off")
        return 0u;
    unsigned minutes = 10;
    if (linger == "1m")
        minutes = 1;
    else if (linger == "5m")
        minutes = 5;
    else if (linger == "30m")
        minutes = 30;
    else if (linger == "60m")
        minutes = 60;
    return minutes * 60u * 1000u;
}

// 用户关闭窗口时走这里：只隐藏，驻留到 appearance.settings_window_linger 指定的时长才真正退出，
// 驻留期间重新打开不用再冷启动。隐藏期间停掉配置轮询，避免白拿 CPU。
void HideSettings(HWND hwnd)
{
    if (g_closing)
        return;
    const std::optional<UINT> linger_ms = SettingsLingerTimeoutMs(g_settings_window_linger);
    if (linger_ms == 0u)
    {
        CloseSettings(hwnd);
        return;
    }
    if (g_lingering)
    {
        ReleaseMouseCapture(hwnd);
        ShowWindow(hwnd, SW_HIDE);
        return;
    }
    g_lingering = true;
    // 窗口隐藏时保留的那一帧会在下次打开时先显示出来，带着高亮的标题栏按钮就会闪一下。
    // 窗口自己的关闭按钮已经在点击时清过，这里覆盖 Alt+F4 等不经过它的路径。
    ResetTitlebarHoverAfterVisibilityChange();
    KillTimer(hwnd, kConfigReloadTimer);
    ReleaseMouseCapture(hwnd);
    ShowWindow(hwnd, SW_HIDE);
    if (linger_ms)
        SetTimer(hwnd, kLingerTimer, *linger_ms, nullptr);
}

void CloseSettings(HWND hwnd)
{
    if (g_closing)
        return;
    g_closing = true;
    g_lingering = false;
    KillTimer(hwnd, kLingerTimer);
    KillTimer(hwnd, kConfigReloadTimer);
    KillTimer(hwnd, kRestartRefreshTimer);
    ReleaseMouseCapture(hwnd);
    ShowWindow(hwnd, SW_HIDE);
    if (g_worker)
        g_worker->Stop();
    else
        DestroyWindow(hwnd);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
{
    // The Direct2D window paints and takes input itself; activation, lingering, the
    // worker and the timers are handled below.
    if (g_native)
    {
        LRESULT result = 0;
        if (g_native->HandleMessage(message, w_param, l_param, result))
            return result;
    }
    switch (message)
    {
    case kActivateExistingWindow:
        ActivateWindow(hwnd);
        return 0;
    case kOpenAboutSection:
        ActivateWindow(hwnd);
        ShowAboutSection();
        return 0;
    case kScanSkinCatalog:
        PostConfig(true);
        return 0;
    case WM_SETTINGCHANGE:
        if (l_param && lstrcmpiW(reinterpret_cast<LPCWSTR>(l_param), L"ImmersiveColorSet") == 0 && g_worker &&
            !g_closing && !g_theme_refresh_pending)
        {
            // 系统主题变化不会修改 config.toml，必须主动重算主题并在 UI 线程应用。
            // 快照里只有 theme_* 原始值，跟随系统时切换前后内容相同，必须强制推给页面才会重算预览。
            // 一次切换会连发几条广播，在途期间只排一次。
            g_theme_refresh_pending = true;
            g_worker->Submit([] {
                SerialTaskQueue::Completion completion;
                try
                {
                    completion = ConfigCompletion(false, true);
                }
                catch (...)
                { /* 不是用户操作，失败不弹框，下次广播或配置变化再同步。 */
                }
                return [completion = std::move(completion)] {
                    g_theme_refresh_pending = false;
                    if (completion)
                        completion();
                };
            });
        }
        break;
    case WM_NCCALCSIZE:
        if (w_param)
        {
            auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(l_param);
            const LRESULT result = DefWindowProcW(hwnd, message, w_param, l_param);
            params->rgrc[0].top -= IsZoomed(hwnd) ? GetSystemMetrics(SM_CYCAPTION) : TopNonClientInset(hwnd);
            return result;
        }
        break;
    case WM_ERASEBKGND: {
        RECT client{};
        GetClientRect(hwnd, &client);
        const bool light = g_settings_light;
        HBRUSH brush = CreateSolidBrush(light ? RGB(243, 243, 243) : RGB(32, 32, 32));
        FillRect(reinterpret_cast<HDC>(w_param), &client, brush);
        DeleteObject(brush);
        return 1;
    }
    case WM_GETMINMAXINFO: {
        auto *info = reinterpret_cast<MINMAXINFO *>(l_param);
        const UINT dpi = GetDpiForWindow(hwnd);
        info->ptMinTrackSize = {MulDiv(600, dpi, 96), MulDiv(400, dpi, 96)};
        return 0;
    }
    case WM_SIZE: {
        const bool minimized = w_param == SIZE_MINIMIZED;
        if (minimized || g_window_minimized)
            ResetTitlebarHoverAfterVisibilityChange();
        g_window_minimized = minimized;
        break;
    }
    case WM_TIMER:
        if (w_param == kLingerTimer)
        {
            CloseSettings(hwnd);
            return 0;
        }
        if (w_param == kRestartRefreshTimer)
        {
            if (g_restart_refresh_remaining > 0)
                --g_restart_refresh_remaining;
            bool pending = false;
            for (const auto &entry : collocation::GetModelStatuses())
                pending = pending || entry.second.update_pending;
            // 待更新已经被新 Server 落位，或兜底次数用完，就停；仍刷新一次让页面恢复。
            if (!pending || g_restart_refresh_remaining <= 0)
                KillTimer(hwnd, kRestartRefreshTimer);
            if (g_worker && !g_closing)
                PostConfig(false);
            return 0;
        }
        if (w_param == kConfigReloadTimer && g_worker && !g_closing && !g_reload_pending)
        {
            g_reload_pending = true;
            g_worker->Submit([] {
                SerialTaskQueue::Completion completion;
                try
                {
                    if (ReloadImeConfigIfChanged())
                        completion = ConfigCompletion();
                }
                catch (...)
                { /* Retry on the next timer tick; do not poison the queue. */
                }
                return [completion = std::move(completion)] {
                    g_reload_pending = false;
                    if (completion)
                        completion();
                };
            });
        }
        return 0;
    case kWorkerCompleted:
        if (g_worker)
        {
            g_worker->Drain(!g_closing);
            // 第一份快照真正下发之后才探测：此时页面已经有模型列表，探测结果回来只需刷新
            // 快照。线程不碰 g_worker，避免进程退出时的生命周期竞争；窗口先关掉时消息自然丢弃。
            if (!g_model_update_check_started && !g_closing && !g_last_config_message.empty())
            {
                g_model_update_check_started = true;
                const HWND target = hwnd;
                std::thread([target] {
                    collocation::CheckForUpdates();
                    PostMessageW(target, kModelUpdateChecked, 0, 0);
                }).detach();
            }
            if (g_closing && g_worker->Stopped())
            {
                // The settings window leaves without destroying its window: see the
                // end of wWinMain for why the process must skip the TSF teardown.
                PostQuitMessage(0);
            }
        }
        return 0;
    case kModelUpdateChecked:
        // 探测线程只回一条消息，刷新快照仍在 worker 队列上做，UI 线程不被网络阻塞。
        PostConfig(false);
        return 0;
    case WM_CLOSE:
        HideSettings(hwnd);
        return 0;
    case kQuitSettings:
        CloseSettings(hwnd);
        return 0;
    case WM_DESTROY:
        g_closing = true;
        g_lingering = false;
        if (g_worker)
            g_worker->Stop();
        KillTimer(hwnd, kLingerTimer);
        KillTimer(hwnd, kConfigReloadTimer);
        KillTimer(hwnd, kRestartRefreshTimer);
        g_native.reset();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, w_param, l_param);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show_command)
{
    TraceStartup("wWinMain");
    // Per-user keyboard list entry, started by the installer as the original user; no window, no single-instance
    // check. Must come before the elevated half below: that one writes HKLM and must not touch HKCU.
    if (const int profile_result = input_method_install::RunRegisterProfileIfRequested(command_line);
        profile_result >= 0)
        return profile_result;
    // The elevated half of the game-compat switch: write HKLM and exit, no window, no single-instance check.
    if (const int elevated_result = game_compat_name::RunElevatedApplyIfRequested(command_line); elevated_result >= 0)
        return elevated_result;
    const std::wstring arguments = command_line ? command_line : L"";
    g_open_about_on_ready = arguments.find(L"--about") != std::wstring::npos;
    // Older shortcuts may still pass --d2d / --webview2, which used to pick the renderer
    // for one run. There is only the Direct2D window now; both are ignored.
    // The Direct2D window's first frame spends ~200ms bringing up the GPU driver. Start
    // that now, alongside the config load and the page build; the window takes the
    // device when it first paints.
    msimeui::DeviceResources::PrewarmComposition();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result))
        return 1;

    // METASEQUOIA_IME_SETTINGS_INSTANCE gives a development build its own instance slot,
    // so it can run next to an installed copy that is open at the same time.
    std::wstring mutex_name = kSingleInstanceMutex;
    if (wchar_t suffix[64]{}; GetEnvironmentVariableW(L"METASEQUOIA_IME_SETTINGS_INSTANCE", suffix, 64) > 0)
        mutex_name += std::wstring(L".") + suffix;
    CommonUtils::SingleInstanceGuard single_instance(mutex_name.c_str());
    if (!single_instance.is_valid())
        return 1;
    if (single_instance.already_running())
    {
        HWND existing = FindWindowW(kWindowClass, nullptr);
        if (existing)
        {
            PostMessageW(existing, g_open_about_on_ready ? kOpenAboutSection : kActivateExistingWindow, 0, 0);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_IME_ICON));
    window_class.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SETTINGS_ICON));
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class))
        return 1;

    const UINT dpi = GetDpiForSystem();
    const int width = MulDiv(900, dpi, 96);
    const int height = MulDiv(710, dpi, 96);
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    HWND hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr,
                                nullptr, instance, nullptr);
    if (!hwnd)
        return 1;
    g_settings_hwnd = hwnd;
    g_worker = std::make_unique<SerialTaskQueue>(
        [hwnd] { PostMessageW(hwnd, kWorkerCompleted, 0, 0); },
        [] { MessageBoxW(g_settings_hwnd, L"设置操作失败，请重试。", L"水杉输入法", MB_OK | MB_ICONWARNING); },
        [] {
            if (g_worker_com_initialized)
                CoUninitialize();
        });
    auto initial_config = std::make_shared<std::promise<bool>>();
    std::future<bool> initial_light = initial_config->get_future();
    g_worker->Submit([initial_config] {
        g_worker_com_initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        TraceStartup("worker: InitImeConfig begin");
        InitImeConfig();
        TraceStartup("worker: InitImeConfig end");
        // 上一次退出时没能落位的新版本在这里生效：此时还没有任何会话/映射。
        collocation::ApplyPendingUpdates();
        initial_config->set_value(ResolveConfiguredTheme(GetConfiguredThemeSettings()) == "light");
        return ConfigCompletion();
    });
    // 配置加载通常只要几毫秒，先等它拿到主题再画窗口，免得浅色主题先闪一下深色。
    // 等不到就按深色先亮出来，之后由 ConfigCompletion 切换。
    bool launch_animation = true;
    if (initial_light.wait_for(std::chrono::milliseconds(1000)) == std::future_status::ready)
    {
        try
        {
            g_settings_light = initial_light.get();
            // 拿到主题时 worker 上的 InitImeConfig 已经跑完，这里读配置全局量是安全的。
            launch_animation = GetConfiguredSettingsLaunchAnimation();
        }
        catch (...)
        {
        }
    }

    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_NONE;
    ApplySettingsChromeTheme(hwnd);
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    MARGINS margins{};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // CreateWindow sends the initial WM_NCCALCSIZE before the DWM/custom-frame
    // attributes above are installed. Force a second frame calculation now;
    // otherwise the native caption remains until the first minimize/restore.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    SetTimer(hwnd, kConfigReloadTimer, 300, nullptr);
    // The Direct2D window draws its first frame at once, so there is no splash.
    native_settings::SettingsWindow::Callbacks callbacks;
    callbacks.postToHost = [hwnd](const std::string &message) { HandleClientMessage(hwnd, message); };
    g_native = std::make_unique<native_settings::SettingsWindow>(hwnd, std::move(callbacks),
                                                                 g_open_about_on_ready ? "about-settings" : "");
    g_native->SetLightTheme(g_settings_light);
    g_native->SetLaunchAnimation(launch_animation);
    TraceStartup("ui: native window built");
    g_open_about_on_ready = false;
    // Cloaked until the first frame is drawn, so the compositor never shows the
    // surface before it is painted (white, whatever the theme).
    BOOL cloak = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
    ShowWindow(hwnd, show_command == SW_HIDE ? SW_SHOWNORMAL : show_command);
    UpdateWindow(hwnd);
    cloak = FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
    TraceStartup("ui: native window shown");

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_worker.reset(); // Drain accepted writes even if the window was destroyed externally.
    // The Direct2D window is a focused Win32 window, so this thread is an ordinary
    // input client of the IME. Destroying the window or uninitializing COM lets TSF
    // deactivate the text service here, which reports a terminal deactivation, and
    // the Server hides the floating toolbar as if the user had switched input
    // methods. The window the user returns to may not re-activate, so the toolbar
    // stayed hidden until they clicked elsewhere. Every write has been drained
    // above; end the process without that teardown (no DestroyWindow, no
    // CoUninitialize), so the Server only sees the pipes close (a route suspension,
    // which leaves the toolbar alone).
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(message.wParam));
    return static_cast<int>(message.wParam);
}
