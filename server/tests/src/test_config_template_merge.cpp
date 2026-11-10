#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "config/ime_config.h"
#include "tests/includes/test_framework.h"
#include <type_traits>
#include <fstream>
#include <iterator>

TEST_CASE(font_fallback_upgrade_preserves_legacy_fonts_and_order)
{
    const std::string next = "[appearance]\nfallback_fonts = [\"Noto Sans SC\", \"Microsoft YaHei\"]\n";
    const std::string old = "[appearance]\nfont = \"SimSun\"\ndefault_font = \"DengXian\"\n";
    const auto migrated = MergeConfigIntoTemplate(next, old, old);
    REQUIRE(migrated.find("[\"SimSun\", \"DengXian\"]") != std::string::npos);
    const std::string custom = "[appearance]\nfallback_fonts = [\n \"Font#1\", # comment\n \"Font]2\"\n]\n";
    REQUIRE(MergeConfigIntoTemplate(next, custom, next).find("Font]2") != std::string::npos);
    REQUIRE(MergeConfigIntoTemplate(next, "[appearance]\nfallback_fonts = []\n", next).find("fallback_fonts = []") !=
            std::string::npos);
}

TEST_CASE(radical_mode_upgrade_inherits_renamed_unicode_mode_switch)
{
    const std::string next = "[utility]\nradical_mode = true\n";
    const std::string old_template = "[utility]\nunicode_mode = true\n";
    // 关掉过旧开关的用户，升级后新开关也是关的；改名不能把 Shift+U 悄悄重新打开。
    REQUIRE(
        MergeConfigIntoTemplate(next, "[utility]\nunicode_mode = false\n", old_template).find("radical_mode = false") !=
        std::string::npos);
    REQUIRE(MergeConfigIntoTemplate(next, old_template, old_template).find("radical_mode = true") != std::string::npos);
    // 已经有新键时以新键为准，残留的旧键不再起作用。
    REQUIRE(MergeConfigIntoTemplate(next, "[utility]\nunicode_mode = false\nradical_mode = true\n", next)
                .find("radical_mode = true") != std::string::npos);
}

TEST_CASE(enumeration_comma_slash_defaults_on_only_for_new_installs)
{
    std::ifstream input(MSIME_DEFAULT_CONFIG_PATH, std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    const std::string installed((std::istreambuf_iterator<char>(input)), {});
    // 新装：没有旧配置，出厂模板里 \ 和 / 都开。
    const auto fresh = toml::parse(MergeConfigIntoTemplate(installed, "", ""));
    REQUIRE(fresh["input"]["enumeration_comma_backslash"].value_or(false));
    REQUIRE(fresh["input"]["enumeration_comma_slash"].value_or(false));
    // 升级：旧配置没有这两个键，保持原来只有 \ 出顿号。
    const std::string legacy = "[input]\npaired_punctuation = true\n";
    const auto upgraded = toml::parse(MergeConfigIntoTemplate(installed, legacy, legacy));
    REQUIRE(upgraded["input"]["enumeration_comma_backslash"].value_or(false));
    REQUIRE(!upgraded["input"]["enumeration_comma_slash"].value_or(true));
    // 已经有这个键（新装后的下一次升级，或用户在设置页改过）时以用户的值为准。
    const std::string chosen = legacy + "enumeration_comma_slash = true\n";
    REQUIRE(
        toml::parse(MergeConfigIntoTemplate(installed, chosen, installed))["input"]["enumeration_comma_slash"].value_or(
            false));
    const auto again = MergeConfigIntoTemplate(installed, installed, installed);
    REQUIRE(toml::parse(again)["input"]["enumeration_comma_slash"].value_or(false));
}

TEST_CASE(shortcut_config_upgrade_preserves_word_selection_and_adds_defaults)
{
    const std::string old_config =
        "[input]\nword_to_character = true\n\n[keybindings]\nswitch_language_shift = false\n";
    const std::string new_template =
        "[input]\nword_to_character = false\nword_to_character_keys = \"brackets\"\n\n"
        "[keybindings]\nswitch_language_shift = true\ntoggle_character_set_ctrl_shift_f = true\n";
    const auto merged = MergeConfigIntoTemplate(new_template, old_config, "");
    REQUIRE(merged.find("word_to_character = true") != std::string::npos);
    REQUIRE(merged.find("word_to_character_keys = \"brackets\"") != std::string::npos);
    REQUIRE(merged.find("switch_language_shift = false") != std::string::npos);
    REQUIRE(merged.find("toggle_character_set_ctrl_shift_f = true") != std::string::npos);
}

TEST_CASE(commit_pinyin_shortcut_upgrade_adds_default_and_preserves_choice)
{
    std::ifstream input(MSIME_DEFAULT_CONFIG_PATH, std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    const std::string installed((std::istreambuf_iterator<char>(input)), {});
    const std::string legacy = "[keybindings]\nswitch_language_shift = true\n";
    // 升级：旧配置没有这个键，出厂模板补上默认值，否则模板缺键会让它每次升级都被丢掉。
    const auto added = toml::parse(MergeConfigIntoTemplate(installed, legacy, legacy));
    REQUIRE(added["keybindings"]["commit_pinyin_shift_enter"].value_or(false));
    // 用户在设置页关掉过，升级后必须还是关的。
    const std::string disabled = legacy + "commit_pinyin_shift_enter = false\n";
    const auto merged = toml::parse(MergeConfigIntoTemplate(installed, disabled, legacy));
    REQUIRE(!merged["keybindings"]["commit_pinyin_shift_enter"].value_or(true));
    REQUIRE(merged["keybindings"]["switch_language_shift"].value_or(false));
}

TEST_CASE(candidate_expansion_key_upgrade_adds_default_and_preserves_choice)
{
    std::ifstream input(MSIME_DEFAULT_CONFIG_PATH, std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    const std::string installed((std::istreambuf_iterator<char>(input)), {});
    const auto working_path =
        std::filesystem::path(MSIME_DEFAULT_CONFIG_PATH).parent_path().parent_path().parent_path() /
        "server/assets/config/config.toml";
    std::ifstream development(working_path, std::ios::binary);
    REQUIRE(static_cast<bool>(development));
    const std::string working((std::istreambuf_iterator<char>(development)), {});
    REQUIRE_EQ(toml::parse(installed)["keybindings"]["candidate_expansion_key"].value_or(std::string()),
               std::string("Backquote"));
    REQUIRE_EQ(toml::parse(working)["keybindings"]["candidate_expansion_key"].value_or(std::string()),
               std::string("Backquote"));
    const std::string legacy = "[keybindings]\nswitch_language_shift = true\n";
    const auto added = toml::parse(MergeConfigIntoTemplate(installed, legacy, legacy));
    REQUIRE_EQ(added["keybindings"]["candidate_expansion_key"].value_or(std::string()), std::string("Backquote"));
    const std::string disabled = legacy + "candidate_expansion_key = \"\"\n";
    const auto merged = toml::parse(MergeConfigIntoTemplate(installed, disabled, legacy));
    REQUIRE_EQ(merged["keybindings"]["candidate_expansion_key"].value_or(std::string("missing")), std::string(""));
    REQUIRE(merged["keybindings"]["switch_language_shift"].value_or(false));
}

TEST_CASE(input_state_shortcut_upgrade_preserves_custom_and_disabled_bindings)
{
    std::ifstream input(MSIME_DEFAULT_CONFIG_PATH, std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    const std::string installed((std::istreambuf_iterator<char>(input)), {});
    const std::string legacy = "[keybindings]\nswitch_language_shift = true\n";
    const auto added = toml::parse(MergeConfigIntoTemplate(installed, legacy, legacy));
    for (const auto &definition : FanyImeInputStateShortcuts::Definitions)
    {
        REQUIRE_EQ(added["keybindings"][definition.key].value_or(std::string()),
                   std::string(definition.defaultBinding));
        for (const std::string binding : {"Ctrl+Shift+KeyG", ""})
        {
            const std::string configured = legacy + std::string(definition.key) + " = \"" + binding + "\"\n";
            const auto merged = toml::parse(MergeConfigIntoTemplate(installed, configured, legacy));
            REQUIRE_EQ(merged["keybindings"][definition.key].value_or(std::string("missing")), binding);
            REQUIRE(merged["keybindings"]["switch_language_shift"].value_or(false));
        }
    }
}

TEST_CASE(config_merge_adds_default_caret_state_indicator)
{
    const std::string template_text = "[general]\nfloating_toolbar = true\ncaret_state_indicator = "
                                      "false\ncaret_state_indicator_position = \"top-left\"\n";
    const std::string old_config = "[general]\nfloating_toolbar = false\n";
    const std::string baseline = "[general]\nfloating_toolbar = true\n";
    const auto merged = MergeConfigIntoTemplate(template_text, old_config, baseline);
    REQUIRE(merged.find("floating_toolbar = false") != std::string::npos);
    REQUIRE(merged.find("caret_state_indicator = false") != std::string::npos);
    REQUIRE(merged.find("caret_state_indicator_position = \"top-left\"") != std::string::npos);
}

TEST_CASE(shipped_caret_indicator_defaults_and_upgrade_behavior)
{
    std::ifstream input(MSIME_DEFAULT_CONFIG_PATH, std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    const std::string installed((std::istreambuf_iterator<char>(input)), {});
    const auto working_path =
        std::filesystem::path(MSIME_DEFAULT_CONFIG_PATH).parent_path().parent_path().parent_path() /
        "server/assets/config/config.toml";
    std::ifstream development(working_path, std::ios::binary);
    REQUIRE(static_cast<bool>(development));
    const std::string working((std::istreambuf_iterator<char>(development)), {});
    REQUIRE(!toml::parse(installed)["general"]["caret_state_indicator"].value_or(true));
    REQUIRE(!toml::parse(working)["general"]["caret_state_indicator"].value_or(true));
    // value_or(true) also fails when a template omits the key: an upgrade
    // would then drop a user's choice (MergeConfigIntoTemplate keeps only
    // keys the shipped template has).
    REQUIRE(!toml::parse(installed)["general"]["caret_state_indicator_on_focus"].value_or(true));
    REQUIRE(!toml::parse(working)["general"]["caret_state_indicator_on_focus"].value_or(true));
    const std::string focus_opt_in = "[general]\nfloating_toolbar = true\ncaret_state_indicator_on_focus = true\n";
    const std::string focus_baseline = "[general]\nfloating_toolbar = true\ncaret_state_indicator_on_focus = false\n";
    REQUIRE(toml::parse(MergeConfigIntoTemplate(installed, focus_opt_in,
                                                focus_baseline))["general"]["caret_state_indicator_on_focus"]
                .value_or(false));

    const std::string old_template = "[general]\nfloating_toolbar = true\n";
    const auto upgraded = toml::parse(MergeConfigIntoTemplate(installed, old_template, old_template));
    REQUIRE(!upgraded["general"]["caret_state_indicator"].value_or(true));

    const std::string enabled = "[general]\nfloating_toolbar = true\ncaret_state_indicator = true\n";
    const auto preserved = toml::parse(MergeConfigIntoTemplate(installed, enabled, old_template));
    REQUIRE(preserved["general"]["caret_state_indicator"].value_or(false));

    // A value identical to the previous baseline is indistinguishable from an untouched default.
    const auto changed = toml::parse(MergeConfigIntoTemplate(installed, enabled, enabled));
    REQUIRE(!changed["general"]["caret_state_indicator"].value_or(true));
    const std::string previous_off = "[general]\nfloating_toolbar = true\ncaret_state_indicator = false\n";
    const auto opted_in = toml::parse(MergeConfigIntoTemplate(installed, enabled, previous_off));
    REQUIRE(opted_in["general"]["caret_state_indicator"].value_or(false));
}

TEST_CASE(shipped_translation_pos_defaults_and_upgrade_preserve_user_choice)
{
    std::ifstream input(MSIME_DEFAULT_CONFIG_PATH, std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    const std::string installed((std::istreambuf_iterator<char>(input)), {});
    const auto working_path =
        std::filesystem::path(MSIME_DEFAULT_CONFIG_PATH).parent_path().parent_path().parent_path() /
        "server/assets/config/config.toml";
    std::ifstream development(working_path, std::ios::binary);
    REQUIRE(static_cast<bool>(development));
    const std::string working((std::istreambuf_iterator<char>(development)), {});
    REQUIRE(toml::parse(installed)["general"]["candidate_translation_show_pos"].value_or(false));
    REQUIRE(toml::parse(working)["general"]["candidate_translation_show_pos"].value_or(false));

    const std::string old_template = "[general]\ncandidate_translations = false\n";
    const auto upgraded = toml::parse(MergeConfigIntoTemplate(installed, old_template, old_template));
    REQUIRE(upgraded["general"]["candidate_translation_show_pos"].value_or(false));
    for (const bool show : {false, true})
    {
        const std::string configured =
            old_template + "candidate_translation_show_pos = " + (show ? "true\n" : "false\n");
        for (const auto &baseline : {std::string(), old_template, installed})
        {
            const auto preserved = toml::parse(MergeConfigIntoTemplate(installed, configured, baseline));
            REQUIRE_EQ(preserved["general"]["candidate_translation_show_pos"].value_or(!show), show);
            REQUIRE(!preserved["general"]["candidate_translations"].value_or(true));
        }
    }
}

TEST_CASE(shipped_translation_single_sense_defaults_and_upgrade_preserve_user_choice)
{
    std::ifstream input(MSIME_DEFAULT_CONFIG_PATH, std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    const std::string installed((std::istreambuf_iterator<char>(input)), {});
    const auto working_path =
        std::filesystem::path(MSIME_DEFAULT_CONFIG_PATH).parent_path().parent_path().parent_path() /
        "server/assets/config/config.toml";
    std::ifstream development(working_path, std::ios::binary);
    REQUIRE(static_cast<bool>(development));
    const std::string working((std::istreambuf_iterator<char>(development)), {});
    REQUIRE(!toml::parse(installed)["general"]["candidate_translation_single_sense"].value_or(true));
    REQUIRE(!toml::parse(working)["general"]["candidate_translation_single_sense"].value_or(true));

    const std::string old_template = "[general]\ncandidate_translations = false\n";
    const auto upgraded = toml::parse(MergeConfigIntoTemplate(installed, old_template, old_template));
    REQUIRE(!upgraded["general"]["candidate_translation_single_sense"].value_or(true));
    for (const bool single : {false, true})
    {
        const std::string configured =
            old_template + "candidate_translation_single_sense = " + (single ? "true\n" : "false\n");
        for (const auto &baseline : {std::string(), old_template, installed})
        {
            const auto preserved = toml::parse(MergeConfigIntoTemplate(installed, configured, baseline));
            REQUIRE_EQ(preserved["general"]["candidate_translation_single_sense"].value_or(!single), single);
        }
    }
}

TEST_CASE(shipped_english_first_page_count_defaults_and_upgrade_preserve_user_choice)
{
    std::ifstream input(MSIME_DEFAULT_CONFIG_PATH, std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    const std::string installed((std::istreambuf_iterator<char>(input)), {});
    const auto working_path =
        std::filesystem::path(MSIME_DEFAULT_CONFIG_PATH).parent_path().parent_path().parent_path() /
        "server/assets/config/config.toml";
    std::ifstream development(working_path, std::ios::binary);
    REQUIRE(static_cast<bool>(development));
    const std::string working((std::istreambuf_iterator<char>(development)), {});
    REQUIRE_EQ(toml::parse(installed)["general"]["cn_en_mixed_input_first_page_count"].value_or(0), 1);
    REQUIRE_EQ(toml::parse(working)["general"]["cn_en_mixed_input_first_page_count"].value_or(0), 1);

    const std::string old_template = "[general]\ncn_en_mixed_input = true\n";
    const auto upgraded = toml::parse(MergeConfigIntoTemplate(installed, old_template, old_template));
    REQUIRE_EQ(upgraded["general"]["cn_en_mixed_input_first_page_count"].value_or(0), 1);
    for (const int count : {1, 2, 3})
    {
        const auto configured = old_template + "cn_en_mixed_input_first_page_count = " + std::to_string(count) + "\n";
        for (const auto &baseline : {std::string(), old_template, installed})
        {
            const auto preserved = toml::parse(MergeConfigIntoTemplate(installed, configured, baseline));
            REQUIRE_EQ(preserved["general"]["cn_en_mixed_input_first_page_count"].value_or(0), count);
        }
    }
}

TEST_CASE(shipped_tsf_diagnostic_log_survives_template_upgrade)
{
    std::ifstream input(MSIME_DEFAULT_CONFIG_PATH, std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    const std::string installed((std::istreambuf_iterator<char>(input)), {});
    const std::string configured = "[general]\ntsf_diagnostic_log = true\n";
    const std::string legacy_template = "[general]\ndiagnostic_log = false\n";

    for (const auto &baseline : {std::string(), legacy_template, installed})
    {
        const auto merged = toml::parse(MergeConfigIntoTemplate(installed, configured, baseline));
        REQUIRE(merged["general"]["tsf_diagnostic_log"].value_or(false));
    }
    REQUIRE(!toml::parse(installed)["general"]["tsf_diagnostic_log"].value_or(true));
}

TEST_CASE(shipped_shuangpin_helpcode_triggers_default_and_survive_template_upgrade)
{
    std::ifstream input(MSIME_DEFAULT_CONFIG_PATH, std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    const std::string installed((std::istreambuf_iterator<char>(input)), {});
    const auto fresh = toml::parse(installed);
    REQUIRE(fresh["helpcode"]["shuangpin_helpcode_direct"].value_or(false));
    REQUIRE(!fresh["helpcode"]["shuangpin_helpcode_trigger_backtick"].value_or(true));
    REQUIRE(fresh["helpcode"]["shuangpin_helpcode_trigger_semicolon"].value_or(false));
    REQUIRE(!fresh["helpcode"]["shuangpin_helpcode_trigger_uppercase"].value_or(true));

    const std::string configured = "[helpcode]\nshuangpin_helpcode_direct = false\n"
                                   "shuangpin_helpcode_trigger_backtick = true\n"
                                   "shuangpin_helpcode_trigger_semicolon = false\n"
                                   "shuangpin_helpcode_trigger_uppercase = true\n";
    for (const auto &baseline : {std::string(), installed})
    {
        const auto merged = toml::parse(MergeConfigIntoTemplate(installed, configured, baseline));
        REQUIRE(!merged["helpcode"]["shuangpin_helpcode_direct"].value_or(true));
        REQUIRE(merged["helpcode"]["shuangpin_helpcode_trigger_backtick"].value_or(false));
        REQUIRE(!merged["helpcode"]["shuangpin_helpcode_trigger_semicolon"].value_or(true));
        REQUIRE(merged["helpcode"]["shuangpin_helpcode_trigger_uppercase"].value_or(false));
    }
}

TEST_CASE(shipped_vim_mode_apps_survive_template_upgrade)
{
    // Only the TSF DLL reads this list; the template is what keeps it across upgrades.
    std::ifstream input(MSIME_DEFAULT_CONFIG_PATH, std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    const std::string installed((std::istreambuf_iterator<char>(input)), {});
    const auto shipped_config = toml::parse(installed);
    const auto *shipped = shipped_config["input"]["vim_mode_apps"].as_array();
    REQUIRE(shipped != nullptr && shipped->empty());

    const std::string configured = "[input]\nvim_mode_apps = [\n  \"Code.exe\", # VS Code\n  \"gvim.exe\",\n]\n";
    for (const auto &baseline : {std::string(), installed})
    {
        const auto merged = toml::parse(MergeConfigIntoTemplate(installed, configured, baseline));
        const auto *apps = merged["input"]["vim_mode_apps"].as_array();
        REQUIRE(apps != nullptr && apps->size() == 2);
        REQUIRE((*apps)[0].value_or(std::string()) == "Code.exe");
        REQUIRE((*apps)[1].value_or(std::string()) == "gvim.exe");
    }
}

TEST_CASE(candidate_key_config_rejects_invalid_groups_without_changing_state)
{
    const auto keys = GetConfiguredWordToCharacterKeys();
    const bool enabled = GetConfiguredWordToCharacterEnabled();
    const bool brackets = GetConfiguredPagingBracketsEnabled();
    const bool minus = GetConfiguredPagingMinusEqualEnabled();
    REQUIRE(!SetConfiguredWordToCharacterKeys("unsupported"));
    REQUIRE_EQ(GetConfiguredWordToCharacterKeys(), keys);
    REQUIRE_EQ(GetConfiguredWordToCharacterEnabled(), enabled);
    REQUIRE_EQ(GetConfiguredPagingBracketsEnabled(), brackets);
    REQUIRE_EQ(GetConfiguredPagingMinusEqualEnabled(), minus);
}

TEST_CASE(config_merge_keeps_customized_values_and_adds_new_keys)
{
    const std::string template_text = "[general]\n"
                                      "# 悬浮工具栏\n"
                                      "floating_toolbar = true\n"
                                      "cloud_candidates = true\n"
                                      "\n"
                                      "[input]\n"
                                      "schema = \"shuangpin\" # 可选：quanpin/shuangpin\n";
    const std::string user_text = "[general]\n"
                                  "floating_toolbar = false\n"
                                  "\n"
                                  "[input]\n"
                                  "schema = \"quanpin\"\n";
    const std::string baseline_text = "[general]\n"
                                      "floating_toolbar = true\n"
                                      "\n"
                                      "[input]\n"
                                      "schema = \"shuangpin\"\n";

    const std::string expected = "[general]\n"
                                 "# 悬浮工具栏\n"
                                 "floating_toolbar = false\n"
                                 "cloud_candidates = true\n"
                                 "\n"
                                 "[input]\n"
                                 "schema = \"quanpin\" # 可选：quanpin/shuangpin\n";
    REQUIRE_EQ(MergeConfigIntoTemplate(template_text, user_text, baseline_text), expected);
}

TEST_CASE(config_merge_lets_new_defaults_win_for_untouched_keys)
{
    const std::string template_text = "[appearance]\npage_size = 9\nfont_size = 18\n";
    const std::string user_text = "[appearance]\npage_size = 8\nfont_size = 20\n";
    const std::string baseline_text = "[appearance]\npage_size = 8\nfont_size = 16\n";

    // page_size 仍是旧默认值 -> 跟随新默认值；font_size 被用户改过 -> 保留。
    REQUIRE_EQ(MergeConfigIntoTemplate(template_text, user_text, baseline_text),
               "[appearance]\npage_size = 9\nfont_size = 20\n");
}

// 0.10.0 把这些键的出厂值改成了新装用户的默认。从 0.9.5 升级上来、一直用着旧默认值的用户，升级后
// 必须还是原来的行为：辅助码不能被关掉，横竖排和皮肤不能被换掉。
TEST_CASE(shipped_fresh_install_defaults_do_not_flip_upgraded_users)
{
    std::ifstream input(MSIME_DEFAULT_CONFIG_PATH, std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    const std::string installed((std::istreambuf_iterator<char>(input)), {});
    const std::string previous_defaults = "[general]\nemoji_mixed_input = true\ncandidate_translations = true\n"
                                          "floating_toolbar_font_size = 24\n"
                                          "[association]\nsentence_wordlattice = true\n"
                                          "[input]\ndefault_ime_mode = \"english\"\n"
                                          "[appearance]\ncandidate_window_layout = \"vertical\"\n"
                                          "candidate_skin = \"willow_green\"\n"
                                          "[helpcode]\nshuangpin_helpcode = true\nquanpin_helpcode = true\n"
                                          "[utility]\nemoji_mode = true # Shift+E\nkaomoji_mode = true\n"
                                          "[voice_input]\nvoice_input = true\ndoubao_enable_ddc = true\n"
                                          "[ai_assistant]\nenabled = true\n"
                                          "[tencent_tmt]\nenabled = true\n"
                                          "[niutrans]\nenabled = false\n";

    const auto upgraded = toml::parse(MergeConfigIntoTemplate(installed, previous_defaults, previous_defaults));
    REQUIRE(upgraded["general"]["emoji_mixed_input"].value_or(false));
    REQUIRE(upgraded["general"]["candidate_translations"].value_or(false));
    REQUIRE_EQ(upgraded["general"]["floating_toolbar_font_size"].value_or(0), 24);
    // 0.12.0 起 sc.lm 改为按需下载，新装默认关；升级用户的模型由安装器挪进 models\language-model\，开关保持原样。
    REQUIRE(upgraded["association"]["sentence_wordlattice"].value_or(false));
    REQUIRE_EQ(upgraded["input"]["default_ime_mode"].value_or(std::string()), std::string("english"));
    REQUIRE_EQ(upgraded["appearance"]["candidate_window_layout"].value_or(std::string()), std::string("vertical"));
    REQUIRE_EQ(upgraded["appearance"]["candidate_skin"].value_or(std::string()), std::string("willow_green"));
    REQUIRE(upgraded["helpcode"]["shuangpin_helpcode"].value_or(false));
    REQUIRE(upgraded["helpcode"]["quanpin_helpcode"].value_or(false));
    REQUIRE(upgraded["utility"]["emoji_mode"].value_or(false));
    REQUIRE(upgraded["utility"]["kaomoji_mode"].value_or(false));
    REQUIRE(upgraded["voice_input"]["voice_input"].value_or(false));
    REQUIRE(upgraded["voice_input"]["doubao_enable_ddc"].value_or(false));
    REQUIRE(upgraded["ai_assistant"]["enabled"].value_or(false));
    REQUIRE(upgraded["tencent_tmt"]["enabled"].value_or(false));
    REQUIRE(!upgraded["niutrans"]["enabled"].value_or(true));

    // 新装用户拿到的是模板本身。
    const auto fresh = toml::parse(installed);
    REQUIRE_EQ(fresh["appearance"]["candidate_window_layout"].value_or(std::string()), std::string("horizontal"));
    REQUIRE(!fresh["helpcode"]["shuangpin_helpcode"].value_or(true));
    REQUIRE(!fresh["association"]["sentence_wordlattice"].value_or(true));
}

TEST_CASE(config_merge_drops_keys_and_sections_absent_from_template)
{
    const std::string template_text = "[general]\nkept = true\n";
    const std::string user_text = "[general]\nkept = false\nretired = 1\n\n[gone]\nvalue = \"x\"\n";

    REQUIRE_EQ(MergeConfigIntoTemplate(template_text, user_text, std::string()), "[general]\nkept = false\n");
}

TEST_CASE(config_merge_keeps_all_user_values_without_a_baseline)
{
    // 从没有 config.base.toml 的旧版本升级时，无法区分「用户改的」和「旧默认值」。
    const std::string template_text = "[voice_input]\nasr_token = \"<YOUR_OWN_ASR_TOKEN>\"\nlanguage = \"zh-cn\"\n";
    const std::string user_text = "[voice_input]\nasr_token = \"sk-real-token\"\nlanguage = \"en\"\n";

    REQUIRE_EQ(MergeConfigIntoTemplate(template_text, user_text, std::string()),
               "[voice_input]\nasr_token = \"sk-real-token\"\nlanguage = \"en\"\n");
}

TEST_CASE(config_merge_handles_multiline_string_values)
{
    const std::string template_text = "[ai_assistant]\n"
                                      "prompt = \"\"\"new\ndefault prompt\"\"\"\n"
                                      "model = \"v2\"\n";
    const std::string user_text = "[ai_assistant]\n"
                                  "prompt = \"\"\"my\nown prompt\"\"\"\n"
                                  "model = \"v1\"\n";
    const std::string baseline_text = "[ai_assistant]\n"
                                      "prompt = \"\"\"old\ndefault prompt\"\"\"\n"
                                      "model = \"v1\"\n";

    const std::string expected = "[ai_assistant]\n"
                                 "prompt = \"\"\"my\nown prompt\"\"\"\n"
                                 "model = \"v2\"\n";
    REQUIRE_EQ(MergeConfigIntoTemplate(template_text, user_text, baseline_text), expected);
}

TEST_CASE(config_merge_ignores_assignments_inside_multiline_strings)
{
    const std::string template_text = "[ai_assistant]\n"
                                      "prompt = \"\"\"say hi\"\"\"\n"
                                      "model = \"v2\"\n";
    // prompt 正文里的 model = ... 只是提示词内容，不能被当成一个键。
    const std::string user_text = "[ai_assistant]\n"
                                  "prompt = \"\"\"say hi\nmodel = \"hijacked\"\n\"\"\"\n";

    const std::string expected = "[ai_assistant]\n"
                                 "prompt = \"\"\"say hi\nmodel = \"hijacked\"\n\"\"\"\n"
                                 "model = \"v2\"\n";
    REQUIRE_EQ(MergeConfigIntoTemplate(template_text, user_text, std::string()), expected);
}

TEST_CASE(config_merge_ignores_commented_out_keys)
{
    const std::string template_text = "[input]\n# schema = \"wubi\"\nschema = \"shuangpin\"\n";
    const std::string user_text = "[input]\n# schema = \"quanpin\"\nschema = \"wubi\"\n";

    REQUIRE_EQ(MergeConfigIntoTemplate(template_text, user_text, std::string()),
               "[input]\n# schema = \"wubi\"\nschema = \"wubi\"\n");
}

TEST_CASE(configured_voice_input_is_handed_out_as_a_snapshot)
{
    // LoadImeConfig rewrites the whole voice config on the IPC worker thread (asr_tokens / polish_tokens are cleared
    // and refilled) while the voice control thread and the low-level keyboard hook read it on every keystroke. Handing
    // out a reference lets those readers walk strings and maps that are being rewritten, so this accessor must return a
    // snapshot by value.
    REQUIRE(!std::is_reference_v<decltype(GetConfiguredVoiceInput())>);
    REQUIRE((std::is_same_v<decltype(GetConfiguredVoiceInput()), VoiceInputConfig>));

    const VoiceInputConfig snapshot = GetConfiguredVoiceInput();
    REQUIRE_EQ(snapshot.commit_mode, VoiceInputConfig().commit_mode);
}

// 读取安装器真正分发的模板，防止开发配置齐全、安装模板漏项导致升级后丢失设置。
TEST_CASE(shipped_translation_settings_survive_template_upgrade)
{
    std::ifstream input(MSIME_DEFAULT_CONFIG_PATH, std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    const std::string installed((std::istreambuf_iterator<char>(input)), {});
    const std::string configured =
        "[custom_translation]\nenabled = true\nendpoint = \"https://translation.example/translate\"\n"
        "api_key = \"test-translation-key\"\n[tencent_tmt]\ntarget_language = \"ja\"\n";
    const std::string legacy_template = "[tencent_tmt]\nenabled = true\n";

    // 同时覆盖旧版本缺少这些字段，以及新版本已经包含这些字段的升级。
    for (const auto &baseline : {legacy_template, installed})
    {
        const auto merged = toml::parse(MergeConfigIntoTemplate(installed, configured, baseline));
        REQUIRE(merged["custom_translation"]["enabled"].value_or(false));
        REQUIRE_EQ(merged["custom_translation"]["endpoint"].value_or(std::string()),
                   std::string("https://translation.example/translate"));
        REQUIRE_EQ(merged["custom_translation"]["api_key"].value_or(std::string()),
                   std::string("test-translation-key"));
        REQUIRE_EQ(merged["tencent_tmt"]["target_language"].value_or(std::string()), std::string("ja"));
    }
}

// 模板漂移：新模板意外漏掉了某个 token 键。用户填过的真 token 不能因此被丢弃，
// 必须被重新写回它所属的分节。
TEST_CASE(config_merge_reappends_real_credentials_absent_from_template)
{
    const std::string template_text = "[ai_assistant]\nprovider = \"deepseek\"\nmodel = \"v2\"\n";
    const std::string user_text =
        "[ai_assistant]\nprovider = \"deepseek\"\ntoken = \"sk-real-secret\"\ntoken_openai = \"sk-openai-real\"\n";
    const auto merged = toml::parse(MergeConfigIntoTemplate(template_text, user_text, std::string()));
    REQUIRE_EQ(merged["ai_assistant"]["token"].value_or(std::string()), std::string("sk-real-secret"));
    REQUIRE_EQ(merged["ai_assistant"]["token_openai"].value_or(std::string()), std::string("sk-openai-real"));
}

// 用户从没填过这个 token（还是占位符）：升级不该把旧占位符粘住，让新模板的默认占位符生效即可。
TEST_CASE(config_merge_keeps_new_placeholder_for_untouched_credential)
{
    const std::string template_text = "[ai_assistant]\ntoken = \"<YOUR_AI_TOKEN_DEEPSEEK>\"\n";
    const std::string user_text = "[ai_assistant]\ntoken = \"<YOUR_OLD_TOKEN>\"\n";
    const std::string baseline_text = "[ai_assistant]\ntoken = \"<YOUR_OLD_TOKEN>\"\n";
    REQUIRE_EQ(MergeConfigIntoTemplate(template_text, user_text, baseline_text),
               "[ai_assistant]\ntoken = \"<YOUR_AI_TOKEN_DEEPSEEK>\"\n");
}

// 抢救路径的核心：一份解析不过的 config.toml（这里结尾留了半行）仍能逐行捞回前面的真 token，
// 重放到新模板上。SyncConfigWithInstalledTemplate 的 unparseable 分支走的正是这条 Merge。
TEST_CASE(config_salvage_recovers_credentials_from_unparseable_config)
{
    const std::string corrupt = "[ai_assistant]\ntoken = \"sk-real-secret\"\nmodel = \"v1\"\nbroken = \n";
    bool corrupt_parses = true;
    try
    {
        (void)toml::parse(corrupt);
    }
    catch (const toml::parse_error &)
    {
        corrupt_parses = false;
    }
    REQUIRE(!corrupt_parses);

    const std::string template_text = "[ai_assistant]\ntoken = \"<YOUR_AI_TOKEN_DEEPSEEK>\"\nmodel = \"v2\"\n";
    const auto salvaged = toml::parse(MergeConfigIntoTemplate(template_text, corrupt, std::string()));
    REQUIRE_EQ(salvaged["ai_assistant"]["token"].value_or(std::string()), std::string("sk-real-secret"));
}

// 读取安装器真正分发的模板，逐个分节确认所有凭证类键都能挺过升级——无论有没有基线。
TEST_CASE(shipped_credentials_survive_template_upgrade)
{
    std::ifstream input(MSIME_DEFAULT_CONFIG_PATH, std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    const std::string installed((std::istreambuf_iterator<char>(input)), {});

    const std::string configured = "[tencent_tmt]\nsecret_id = \"real-secret-id\"\nsecret_key = \"real-secret-key\"\n"
                                   "[aliyun_mt]\nsecret_id = \"real-aliyun-id\"\nsecret_key = \"real-aliyun-key\"\n"
                                   "[custom_translation]\napi_key = \"real-api-key\"\n"
                                   "[niutrans]\napp_id = \"real-app-id\"\napikey = \"real-apikey\"\n"
                                   "[voice_input]\nasr_token = \"real-asr\"\nasr_token_doubao = \"real-doubao\"\n"
                                   "polish_token_deepseek = \"real-polish\"\n"
                                   "[ai_assistant]\ntoken = \"real-ai\"\ntoken_openai = \"real-ai-openai\"\n";

    for (const auto &baseline : {std::string(), installed})
    {
        const auto merged = toml::parse(MergeConfigIntoTemplate(installed, configured, baseline));
        REQUIRE_EQ(merged["tencent_tmt"]["secret_id"].value_or(std::string()), std::string("real-secret-id"));
        REQUIRE_EQ(merged["tencent_tmt"]["secret_key"].value_or(std::string()), std::string("real-secret-key"));
        REQUIRE_EQ(merged["aliyun_mt"]["secret_id"].value_or(std::string()), std::string("real-aliyun-id"));
        REQUIRE_EQ(merged["aliyun_mt"]["secret_key"].value_or(std::string()), std::string("real-aliyun-key"));
        REQUIRE_EQ(merged["custom_translation"]["api_key"].value_or(std::string()), std::string("real-api-key"));
        REQUIRE_EQ(merged["niutrans"]["app_id"].value_or(std::string()), std::string("real-app-id"));
        REQUIRE_EQ(merged["niutrans"]["apikey"].value_or(std::string()), std::string("real-apikey"));
        REQUIRE_EQ(merged["voice_input"]["asr_token"].value_or(std::string()), std::string("real-asr"));
        REQUIRE_EQ(merged["voice_input"]["asr_token_doubao"].value_or(std::string()), std::string("real-doubao"));
        REQUIRE_EQ(merged["voice_input"]["polish_token_deepseek"].value_or(std::string()), std::string("real-polish"));
        REQUIRE_EQ(merged["ai_assistant"]["token"].value_or(std::string()), std::string("real-ai"));
        REQUIRE_EQ(merged["ai_assistant"]["token_openai"].value_or(std::string()), std::string("real-ai-openai"));
    }
}
