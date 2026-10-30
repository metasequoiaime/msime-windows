set(TEST_EXECUTABLE_NAME "MetasequoiaImeServerTests")

add_executable(test_native_settings_controls
    portable/main.cpp
    src/test_native_settings_controls.cpp
    ../src/settings/native/native_settings_controls.cpp
    ../src/settings/native/native_page_help_widgets.cpp
    ../src/settings/native/native_settings_theme.cpp
    ../src/utils/common_utils.cpp
    ../src/utils/ime_paths.cpp)
target_include_directories(test_native_settings_controls PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(test_native_settings_controls PRIVATE NOMINMAX=1)
target_link_libraries(test_native_settings_controls PRIVATE msimeui Boost::locale shell32 advapi32
    nlohmann_json::nlohmann_json)
add_test(NAME native_settings_controls COMMAND test_native_settings_controls)

add_executable(test_voice_control_dispatch src/test_voice_control_dispatch.cpp
    ../src/ipc/voice_control_dispatch.cpp)
target_include_directories(test_voice_control_dispatch PRIVATE ../src "${MSIME_ENGINE_DIR}")
target_compile_features(test_voice_control_dispatch PRIVATE cxx_std_17)
add_test(NAME voice_control_dispatch COMMAND test_voice_control_dispatch)

# Drives the real voice control queue and keyboard-hook entry with a gated fake
# microphone; this is not a portable state-machine copy.
add_executable(test_voice_lock_recovery
    src/test_voice_lock_recovery.cpp
    ${CMAKE_SOURCE_DIR}/src/voice-input/voice_input_service.cpp
    ${CMAKE_SOURCE_DIR}/src/voice-input/voice_providers.cpp
    ${CMAKE_SOURCE_DIR}/src/voice-input/voice_batch_protocol.cpp)
target_compile_definitions(test_voice_lock_recovery PRIVATE MSIME_VOICE_CONTROL_TEST)
target_include_directories(test_voice_lock_recovery
    PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${MSIME_REPO_ROOT}"
    "${MSIME_ENGINE_DIR}")
target_link_libraries(test_voice_lock_recovery
    PRIVATE
    MetasequoiaIme::Voice
    CURL::libcurl
    fmt::fmt
    nlohmann_json::nlohmann_json
    user32
    shcore)
add_test(NAME voice_lock_recovery COMMAND test_voice_lock_recovery)
set_tests_properties(voice_lock_recovery PROPERTIES TIMEOUT 60)

# Phase-A voice input target snapshot: pure policy in voice_target.h only. The
# Windows/UIA adapter (voice_target.cpp) is host-only and stays out of this
# standalone target; it links no engine, dictionary or third-party dependency.
add_executable(test_voice_target src/test_voice_target.cpp)
target_include_directories(test_voice_target PRIVATE ../src)
target_compile_features(test_voice_target PRIVATE cxx_std_17)
add_test(NAME voice_target COMMAND test_voice_target)

# Drives the real process-priority restore against a deliberately lowered
# process, so the Server startup path is covered without launching the Server.
add_executable(test_process_priority
    src/test_process_priority.cpp
    ../src/utils/process_priority.cpp)
target_include_directories(test_process_priority PRIVATE ../src)
target_compile_features(test_process_priority PRIVATE cxx_std_17)
add_test(NAME process_priority COMMAND test_process_priority)
set_tests_properties(process_priority PROPERTIES SKIP_RETURN_CODE 77)

# Exercises the scoped thread-priority boost against the real thread APIs: the
# guard must raise the thread, restore the entry priority, and skip cleanly on
# hosts whose policy rejects thread priority changes.
add_executable(test_thread_priority
    src/test_thread_priority.cpp
    ../src/utils/thread_priority.cpp)
target_include_directories(test_thread_priority PRIVATE ../src)
target_compile_features(test_thread_priority PRIVATE cxx_std_17)
add_test(NAME thread_priority COMMAND test_thread_priority)
set_tests_properties(thread_priority PROPERTIES SKIP_RETURN_CODE 77)

set(TEST_SOURCE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark_model_path.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_date_time_query.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_v_mode_query.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_emoji_query.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_kaomoji_query.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_jianpin_query.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_quanpin_jianpin_recall.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_fault_slot_jianpin.cpp"
    "${CMAKE_SOURCE_DIR}/src/clipboard/clipboard_history.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_kaomoji_sql.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_shuangpin_scheme.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_custom_shuangpin.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_quanpin_scheme.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_wubi.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_japanese_romaji.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_shuangpin_query.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_engine_shuangpin_session.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_caret_prefix_input_session.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_english_dictionary.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_mixed_english_placement.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_translation_gloss.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_default_config_credentials.cpp"
    "${CMAKE_SOURCE_DIR}/src/cloud/translation_gloss.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_custom_translation.cpp"
    "${CMAKE_SOURCE_DIR}/src/cloud/custom_translation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_ai_translation.cpp"
    "${CMAKE_SOURCE_DIR}/src/cloud/ai_translation.cpp"
    "${CMAKE_SOURCE_DIR}/src/ai/chat_completions.cpp"
    "${CMAKE_SOURCE_DIR}/src/ai/jev_client.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_ai_assistant_reply.cpp"
    "${CMAKE_SOURCE_DIR}/src/ai/ai_assistant_reply.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_jev_candidates.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_niutrans_translation.cpp"
    "${CMAKE_SOURCE_DIR}/src/cloud/niutrans_translation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_aliyun_mt.cpp"
    "${CMAKE_SOURCE_DIR}/src/cloud/aliyun_mt.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_active_client_state.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_outbound_session_state.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_pipe_write_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_input_key_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_ui_owner.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_async_request_origin.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_ai_assistant_cache_key.cpp"
    "${CMAKE_SOURCE_DIR}/src/ai/ai_assistant_cache_key.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_ai_assistant_cache.cpp"
    "${CMAKE_SOURCE_DIR}/src/ai/ai_assistant_cache.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_ai_assistant_request.cpp"
    "${CMAKE_SOURCE_DIR}/src/ai/ai_assistant_request.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_ui_action_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_translation_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_ai_context_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_ui_state.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_expansion_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_composition_selection_history.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_render_sync.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_selection_replay_guard.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_serial_task_runner.cpp"
    "${CMAKE_SOURCE_DIR}/src/utils/serial_task_runner.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_task_classes.cpp"
    "${CMAKE_SOURCE_DIR}/src/ipc/event_listener_task_class.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_input_cadence.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_sentence_association_options.cpp"
    "${CMAKE_SOURCE_DIR}/src/session/input_cadence.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_shuangpin_preedit_display.cpp"
    "${CMAKE_SOURCE_DIR}/src/utils/shuangpin_preedit_display.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_wheel_paging.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_view_model.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_text_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_traditional_region_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_traditional_variant_order_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_traditional_variant_expansion_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_settings_performance.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_size_estimator.cpp"
    "${CMAKE_SOURCE_DIR}/src/utils/candidate_size_estimator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_ipc_protocol_constants.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_diagnostic_log_naming.cpp"
    "${CMAKE_SOURCE_DIR}/src/log/diagnostic_log_naming.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_voice_composition_pipe.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_voice_providers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_ai_vendor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_ai_provider_store.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_reasoning_strip.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_voice_batch_protocol.cpp"
    "${CMAKE_SOURCE_DIR}/src/voice-input/voice_batch_protocol.cpp"
    "${CMAKE_SOURCE_DIR}/src/voice-input/voice_providers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_floating_toolbar_visibility_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_caret_state_indicator_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_tray_menu_placement.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_terminal_deactivation_policy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_user_dictionary_journal.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_config_template_merge.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_config_non_ascii_path.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_collocation_model.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_neural_model.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_lattice_model.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_network_proxy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_update_download.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_dictionary_replay_non_ascii_path.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_skin_catalog_non_ascii_path.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_skin_catalog_colors.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_skin_catalog_bases.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_skin_catalog_images.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_skin_catalog_page_arrows.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_skin_catalog_supports.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_skin_catalog_toolbar_image.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_skin_catalog_border_image_vertical.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_skin_palette.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_non_ascii_path_contract.cpp"
    "${CMAKE_SOURCE_DIR}/src/skin/candidate_skin_catalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/window/candidate_skin_palette.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_skin_tokens.cpp"
    "${CMAKE_SOURCE_DIR}/src/window/candidate_skin_tokens.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_candidate_skin_layers.cpp"
    "${CMAKE_SOURCE_DIR}/src/window/candidate_skin_layers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_floating_toolbar_skin.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_floating_toolbar_image_layout.cpp"
    "${CMAKE_SOURCE_DIR}/src/window/floating_toolbar_skin.cpp"
    "${CMAKE_SOURCE_DIR}/src/window/floating_toolbar_image_layout.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_system_font_family.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_dictionary_validation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_stats_frame_decode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_stats_aggregate.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_stats_overview.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_stats_store.cpp"
    "${CMAKE_SOURCE_DIR}/src/statistics/stats_frames.cpp"
    "${CMAKE_SOURCE_DIR}/src/statistics/stats_aggregate.cpp"
    "${CMAKE_SOURCE_DIR}/src/statistics/stats_overview.cpp"
    "${CMAKE_SOURCE_DIR}/src/statistics/stats_store.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/credential_protection.cpp"
    "${CMAKE_SOURCE_DIR}/src/ai/ai_provider_store.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ime_config.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ime_config_credentials.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ime_config_appearance.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ime_config_fonts.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ime_config_general.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ime_config_input.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ime_config_keys.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ime_config_services.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ime_config_shuangpin.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ime_config_template.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ime_config_theme.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ime_config_toml.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ime_config_toolbar.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ime_config_write.cpp"
    "${CMAKE_SOURCE_DIR}/src/session/engine_input_session.cpp"
    "${CMAKE_SOURCE_DIR}/src/utils/common_utils.cpp"
    "${CMAKE_SOURCE_DIR}/src/utils/hanzi_pinyin.cpp"
    "${CMAKE_SOURCE_DIR}/src/utils/ime_paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/utils/network_proxy.cpp"
    "${CMAKE_SOURCE_DIR}/src/settings/native/native_settings_update_download.cpp"
    # 通道判定与安装包落点都要能在没有网络的情况下测，所以链接真实的翻译单元。
    # update_check.cpp 不进测试：它带轮询线程与 IPC 广播，那两样在测试进程里没有意义。
    "${CMAKE_SOURCE_DIR}/src/settings/update_channel.cpp"
    "${CMAKE_SOURCE_DIR}/src/settings/dictionary_validation.cpp"
    # The catalog/download/delete behaviors under test live in collocation_model.cpp, so the
    # activation-fallback and id-guard cases link the real translation unit.
    "${CMAKE_SOURCE_DIR}/src/settings/collocation_model.cpp"
    "${CMAKE_SOURCE_DIR}/src/settings/neural_model.cpp"
    "${CMAKE_SOURCE_DIR}/src/settings/lattice_model.cpp"
    "${CMAKE_SOURCE_DIR}/src/settings/model_download.cpp"
    # Routing lives in dictionary_manager.cpp's HandleRequest, so the importHans dispatch test has
    # to link the real translation unit rather than restate the branch order.
    "${CMAKE_SOURCE_DIR}/src/settings/dictionary_manager.cpp"
)

add_executable(${TEST_EXECUTABLE_NAME} ${TEST_SOURCE_FILES})

target_include_directories(${TEST_EXECUTABLE_NAME}
    PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${MSIME_VENDOR_DIR}"
    "${MSIME_ENGINE_DIR}/utfcpp/source"
)

# test_default_config_credentials reads the config the installer actually ships, so the check is
# against the shipped file rather than a restatement of today's placeholder spellings.
# test_custom_shuangpin parses the example users are told to copy, so the documentation cannot drift
# from what the parser accepts.
# test_dictionary_replay_non_ascii_path launches the real replay tool, because the path handling it
# guards lives in that executable's main() and cannot be linked into the test binary.
target_compile_definitions(${TEST_EXECUTABLE_NAME}
    PRIVATE
    "MSIME_DEFAULT_CONFIG_PATH=\"${CMAKE_SOURCE_DIR}/../installer/default_config/config.default.toml\""
    "MSIME_CUSTOM_SHUANGPIN_EXAMPLE_PATH=\"${CMAKE_SOURCE_DIR}/../docs/custom-shuangpin.example.toml\""
    "MSIME_DICTIONARY_REPLAY_EXE=\"$<TARGET_FILE:${DICTIONARY_REPLAY_EXECUTABLE_NAME}>\""
)

add_dependencies(${TEST_EXECUTABLE_NAME} ${DICTIONARY_REPLAY_EXECUTABLE_NAME})

target_link_libraries(${TEST_EXECUTABLE_NAME}
    PRIVATE
    MetasequoiaIme::Engine
    MetasequoiaIme::Voice
    nlohmann_json::nlohmann_json
    fmt::fmt
    spdlog::spdlog
    Boost::locale
    Boost::json
    CURL::libcurl
    unofficial::sqlite3::sqlite3
    # dictionary_manager.cpp annotates pure-Chinese imports with cpp-pinyin and clears the
    # candidate cache through user32 window messages.
    cpp-pinyin::cpp-pinyin
    dwrite
    user32
    shell32
    advapi32
    ole32
    # native_settings_update_download.cpp 用 WinHTTP 取包、用 BCrypt 算摘要。
    winhttp
    bcrypt
)

add_test(NAME ${TEST_EXECUTABLE_NAME} COMMAND ${TEST_EXECUTABLE_NAME})

# Settings-window controls driven through a real msimeui::Window. The settings executable's own
# sources are compiled in, all but its entry point, so the controls under test are the shipped ones
# and the list cannot fall behind the executable's.
get_target_property(_settings_sources ${SETTINGS_EXECUTABLE_NAME} SOURCES)
set(SETTINGS_UI_TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/settings_ui_main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_settings_dropdown_scroll.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/test_settings_ai_model_list_scroll.cpp"
)
foreach(_source IN LISTS _settings_sources)
    if(_source MATCHES "settings_app\\.cpp$" OR NOT _source MATCHES "\\.cpp$")
        continue()
    endif()
    get_filename_component(_source "${_source}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    list(APPEND SETTINGS_UI_TEST_SOURCES "${_source}")
endforeach()
add_executable(MetasequoiaImeSettingsUiTests ${SETTINGS_UI_TEST_SOURCES})
target_include_directories(MetasequoiaImeSettingsUiTests PRIVATE "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/include")
target_link_libraries(MetasequoiaImeSettingsUiTests PRIVATE
    msimeui
    MetasequoiaIme::Engine
    fmt::fmt
    Boost::locale
    Boost::json
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
    spdlog::spdlog
    cpp-pinyin::cpp-pinyin
    CURL::libcurl
    ZLIB::ZLIB
    bcrypt
    winhttp
    dwmapi
    dcomp
    d2d1
    dwrite
    shell32
)
add_test(NAME MetasequoiaImeSettingsUiTests COMMAND MetasequoiaImeSettingsUiTests)

# 繁简转换走真实的 OpenCC。转换器从 exe 所在目录的 assets/opencc 读字典，这个目录由 server 的
# POST_BUILD 拷出，测试程序与 server 输出到同一个 bin 目录，所以先构建 server。
# 拷贝清单漏了字典时，只有干净构建会让它失败：增量构建里 copy_if_different 不删除已有的旧文件。
add_executable(test_chinese_converter
    portable/main.cpp
    src/test_chinese_converter.cpp
    ../src/conversion/chinese_converter.cpp)
target_include_directories(test_chinese_converter PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(test_chinese_converter PRIVATE NOMINMAX=1)
target_link_libraries(test_chinese_converter PRIVATE OpenCC::OpenCC)
add_dependencies(test_chinese_converter MetasequoiaImeServer)
add_test(NAME chinese_converter COMMAND test_chinese_converter)

# The identical fixture file also runs in Engine's JavaScript contract CI.
add_executable(test_webview_contract "${MSIME_ENGINE_DIR}/contracts/webview/test.cpp")
target_link_libraries(test_webview_contract PRIVATE Boost::json)
add_test(NAME webview_contract COMMAND test_webview_contract
    "${MSIME_ENGINE_DIR}/contracts/webview/fixtures.json")


# 繁体写法的转换结果来自真实的 OpenCC 字典：链接产品的 chinese_converter.cpp 与 traditional_spelling_index.cpp，
# 转换器从 exe 所在目录的 assets/opencc 读字典。这个目录由 server 的 POST_BUILD 拷出，测试程序与 server
# 输出到同一个 bin 目录，所以先构建 server。拷贝清单漏了字典时，只有干净构建会让它失败：增量构建里
# copy_if_different 不删除已有的旧文件。
add_executable(test_traditional_conversion
    portable/main.cpp
    src/test_traditional_conversion.cpp
    ../src/conversion/chinese_converter.cpp
    ../src/conversion/traditional_spelling_index.cpp)
target_include_directories(test_traditional_conversion PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(test_traditional_conversion PRIVATE NOMINMAX=1)
target_link_libraries(test_traditional_conversion PRIVATE OpenCC::OpenCC)
add_dependencies(test_traditional_conversion MetasequoiaImeServer)
add_test(NAME traditional_conversion COMMAND test_traditional_conversion)

add_executable(test_voice_result_card src/test_voice_result_card.cpp)
target_include_directories(test_voice_result_card PRIVATE ../src)
target_compile_features(test_voice_result_card PRIVATE cxx_std_17)
add_test(NAME voice_result_card COMMAND test_voice_result_card)
# Phase-C voice commit routing policy: pure decisions in voice_commit_policy.h
# and voice_target.h only; no Windows adapter, engine or dictionary dependency.
add_executable(test_voice_commit_policy src/test_voice_commit_policy.cpp)
target_include_directories(test_voice_commit_policy PRIVATE ../src)
target_compile_features(test_voice_commit_policy PRIVATE cxx_std_17)
add_test(NAME voice_commit_policy COMMAND test_voice_commit_policy)

# Exercise production lifecycle bodies without microphone, credentials or UI.
# Share the body extractor already used by the DLL's standalone fixtures.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(voice_service_source "${CMAKE_CURRENT_SOURCE_DIR}/../src/voice-input/voice_input_service.cpp")
set(voice_service_generator "${CMAKE_CURRENT_SOURCE_DIR}/generate_voice_service_fixture.py")
set(voice_fixture_helpers "${MSIME_REPO_ROOT}/windows/tests")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${voice_service_source}" "${voice_service_generator}" "${voice_fixture_helpers}/generate_reconnect_fixture.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${voice_service_generator}"
    "${voice_service_source}" "${voice_fixture_helpers}" "${CMAKE_CURRENT_BINARY_DIR}/voice_service_source.h"
    COMMAND_ERROR_IS_FATAL ANY)
add_executable(test_voice_service_lifecycle src/test_voice_service_lifecycle.cpp)
target_include_directories(test_voice_service_lifecycle PRIVATE ../src "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(test_voice_service_lifecycle PRIVATE cxx_std_17)
add_test(NAME voice_service_lifecycle COMMAND test_voice_service_lifecycle)
set_tests_properties(voice_service_lifecycle PROPERTIES TIMEOUT 15)

if(WIN32)
    # Real focus/UIA coverage is opt-in; ordinary CTest runs never take focus.
    add_executable(test_voice_target_windows src/test_voice_target_windows.cpp
        ../src/voice-input/voice_target.cpp)
    target_include_directories(test_voice_target_windows PRIVATE ../src)
    target_compile_features(test_voice_target_windows PRIVATE cxx_std_17)
    target_compile_definitions(test_voice_target_windows PRIVATE NOMINMAX UNICODE _UNICODE)
    target_link_libraries(test_voice_target_windows PRIVATE user32 ole32 oleaut32 uiautomationcore)
    add_test(NAME voice_target_windows COMMAND test_voice_target_windows)
    set_tests_properties(voice_target_windows PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(test_voice_overlay_layout src/test_voice_overlay_layout.cpp)
    target_include_directories(test_voice_overlay_layout PRIVATE ../src)
    target_compile_features(test_voice_overlay_layout PRIVATE cxx_std_17)
    target_compile_definitions(test_voice_overlay_layout PRIVATE NOMINMAX)
    add_test(NAME voice_overlay_layout COMMAND test_voice_overlay_layout)

    # Uses the real overlay on a message-only window: never shows UI or takes focus.
    find_package(tomlplusplus CONFIG REQUIRED)
    add_executable(test_voice_overlay_threading src/test_voice_overlay_threading.cpp
        ../src/voice-input/wave_overlay.cpp ../src/voice-input/voice_input_overlay_utils.cpp)
    target_include_directories(test_voice_overlay_threading PRIVATE ../src)
    target_compile_features(test_voice_overlay_threading PRIVATE cxx_std_17)
    target_compile_definitions(test_voice_overlay_threading PRIVATE NOMINMAX UNICODE _UNICODE)
    target_link_libraries(test_voice_overlay_threading PRIVATE tomlplusplus::tomlplusplus d2d1 dwrite dwmapi shcore)
    add_test(NAME voice_overlay_threading COMMAND test_voice_overlay_threading)
    set_tests_properties(voice_overlay_threading PROPERTIES TIMEOUT 10)
endif()
