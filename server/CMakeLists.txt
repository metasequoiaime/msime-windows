cmake_minimum_required(VERSION 3.25.0)
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()

set(PROJECT_NAME "MetasequoiaImeServer")

cmake_policy(SET CMP0167 NEW)
project(${PROJECT_NAME} VERSION 1.0)

# Release binaries need usable symbols for crash diagnosis. Keep the PDB beside each executable so
# the release job can validate and publish it without changing the binary's install layout.
#
# The debug info is embedded in each object (/Z7) rather than written to a compile-time PDB (/Zi).
# The two produce the same final .pdb once the linker's /DEBUG runs over them, but sccache -- the CI
# compiler cache -- can only cache an MSVC compile whose debug info lives in the object. With /Zi it
# treats every translation unit as uncacheable and the cache sits at a 0% hit rate. /Z7 keeps the
# beside-the-executable PDB the release job expects while letting that cache actually work.
if(MSVC)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")
    add_compile_options("$<$<CONFIG:Release>:/Z7>")
    add_link_options("$<$<CONFIG:Release>:/DEBUG>")
endif()

include(CTest)

# The engine is a first-party component of this repository, vendored in-tree at engine/ next to
# server/, ui/ and windows/. The remaining third-party sources are still submodules under vendor/.
set(MSIME_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(MSIME_ENGINE_DIR "${MSIME_REPO_ROOT}/engine")
set(MSIME_VENDOR_DIR "${MSIME_REPO_ROOT}/vendor")

# 内置皮肤的出厂清单（skins/default/<id>/skin.toml）编进程序：数据目录里那一份缺失或写坏时，
# 外观仍按出厂清单画。清单是唯一的来源，这里只把它们原样嵌成字符串；改了清单会触发重新配置。
set(MSIME_FACTORY_SKIN_IDS fluent wechat wechat_scroll graphite willow_green autumn_osmanthus microsoft canvas)
set(MSIME_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(MSIME_VERSION_FILE "${MSIME_REPO_ROOT}/version.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MSIME_VERSION_FILE}")
set(MSIME_PRODUCT_VERSION_OVERRIDE "" CACHE STRING "Product version to embed in the settings page")
if(MSIME_PRODUCT_VERSION_OVERRIDE)
    set(MSIME_PRODUCT_VERSION "${MSIME_PRODUCT_VERSION_OVERRIDE}")
else()
    file(STRINGS "${MSIME_VERSION_FILE}" MSIME_PRODUCT_VERSION LIMIT_COUNT 1)
endif()
string(STRIP "${MSIME_PRODUCT_VERSION}" MSIME_PRODUCT_VERSION)
# 版本主体必须是数字段；允许一个可选的连字符后缀（semver 预发布段），dev-build 用它携带
# 日期与 commit（0.7.1-dev.20261019.<hash>），设置窗口关于页原样显示。关于页的 ParseVersion
# 会剥掉后缀做更新检查的数字比较，所以后缀不影响版本判定。
if(NOT MSIME_PRODUCT_VERSION MATCHES "^[0-9]+(\\.[0-9]+)*(-[0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "Invalid product version in ${MSIME_VERSION_FILE}: '${MSIME_PRODUCT_VERSION}'")
endif()
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/msime_version.h.in"
    "${MSIME_GENERATED_DIR}/msime_version.h"
    @ONLY
)
set(_msime_factory_skins "")
foreach(_skin_id IN LISTS MSIME_FACTORY_SKIN_IDS)
    set(_skin_manifest "${MSIME_REPO_ROOT}/skins/default/${_skin_id}/skin.toml")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_skin_manifest}")
    file(READ "${_skin_manifest}" _skin_text)
    string(APPEND _msime_factory_skins "{\"${_skin_id}\", R\"msimeskin(${_skin_text})msimeskin\"},\n")
endforeach()
# 只在内容变了时才写，免得每次配置都让皮肤目录的编译单元重编。
set(_msime_factory_skins_file "${MSIME_GENERATED_DIR}/builtin_skin_manifests.inc")
set(_msime_factory_skins_old "")
if(EXISTS "${_msime_factory_skins_file}")
    file(READ "${_msime_factory_skins_file}" _msime_factory_skins_old)
endif()
if(NOT _msime_factory_skins_old STREQUAL _msime_factory_skins)
    file(WRITE "${_msime_factory_skins_file}" "${_msime_factory_skins}")
endif()
include_directories("${MSIME_GENERATED_DIR}")

# Boost
# An explicit -DBoost_ROOT wins, then BOOST_ROOT from the environment, and only then the scoop
# layout under the current user's profile. The fallback used to name one developer's home directory,
# which meant anyone else without BOOST_ROOT set got a configure failure pointing at a path that
# does not exist on their machine.
if(NOT DEFINED Boost_ROOT)
    if(DEFINED ENV{BOOST_ROOT})
        set(Boost_ROOT "$ENV{BOOST_ROOT}")
    elseif(DEFINED ENV{USERPROFILE} AND EXISTS "$ENV{USERPROFILE}/scoop/apps/boost/current")
        set(Boost_ROOT "$ENV{USERPROFILE}/scoop/apps/boost/current")
    endif()
endif()
set (Boost_USE_STATIC_LIBS ON)
find_package(Boost REQUIRED COMPONENTS locale json)
include_directories(${Boost_INCLUDE_DIRS})

# fmt, spdlog and sqlite3
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

# The engine ships its own CMake target. Consume that instead of listing its
# translation units here, so that adding a file to the engine no longer requires
# a matching edit in this file. EXCLUDE_FROM_ALL keeps the engine's own test
# executables out of the default build.
#
# Which is also why BUILD_TESTING has to be off across that add_subdirectory: the engine registers
# its own add_test() entries behind that flag, and EXCLUDE_FROM_ALL then never builds the
# executables they point at, so ctest reports an "Unable to find executable" failure per entry. The
# engine's own suite runs in the separate Engine job of ci.yml (scripts/ci/test-engine.ps1), which
# builds the engine standalone. Restored immediately afterwards so this repository's own tests/
# still register.
set(_metasequoia_saved_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
set(METASEQUOIA_IME_BUILD_VOICE ON CACHE BOOL "" FORCE)
set(MSIME_VOICE_CAPTURE ON CACHE BOOL "" FORCE)
add_subdirectory("${MSIME_ENGINE_DIR}" "${CMAKE_BINARY_DIR}/engine" EXCLUDE_FROM_ALL)
set(BUILD_TESTING "${_metasequoia_saved_build_testing}")
unset(_metasequoia_saved_build_testing)

# Build the OpenCC submodule as a static dependency of the server.
add_subdirectory("${MSIME_VENDOR_DIR}/opencc" "${CMAKE_BINARY_DIR}/opencc" EXCLUDE_FROM_ALL)

# OpenCC's Windows dictionary helper copies the library into the tools output
# directory before MSBuild has necessarily created that directory.
add_custom_target(opencc_tools_output_dir
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_BINARY_DIR}/opencc/src/tools/$<CONFIG>"
)
add_dependencies(copy_libopencc_to_dir_of_opencc_dict opencc_tools_output_dir)

# cpp-pinyin: settings uses it to annotate pure-Chinese phrase imports.
set(_cpp_pinyin_saved_shared_libs "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
set(CPP_PINYIN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CPP_PINYIN_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${MSIME_VENDOR_DIR}/cpp-pinyin" "${CMAKE_BINARY_DIR}/cpp-pinyin" EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS "${_cpp_pinyin_saved_shared_libs}")
unset(_cpp_pinyin_saved_shared_libs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_definitions(
    /D_UNICODE=1
    /DUNICODE=1
    /DFANY_DEBUG=1
)

# HandwritingPanel still uses the legacy <experimental/coroutine>; on newer
# MSVC STL the deprecation is a hard error, so silence it explicitly.
add_definitions(/D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS)

# add_definitions(
#     /DFANY_DEBUG=1
#     /DFANY_IPC_DEBUG=1
# )

# For utf8cpp, make it use C++11 and later
add_compile_options(/Zc:__cplusplus /FS /utf-8)

include_directories("./")
include_directories("./src/")
# The repository root is on the include path so engine headers are reached as "engine/...", which
# names the component the same way the source tree does.
include_directories("${MSIME_REPO_ROOT}")
include_directories("${MSIME_VENDOR_DIR}")
include_directories("${MSIME_ENGINE_DIR}/utfcpp/source/")

set(MY_EXECUTABLE_NAME "MetasequoiaImeServer")
set(WATCHDOG_EXECUTABLE_NAME "MetasequoiaImeWatchdog")
set(DICTIONARY_REPLAY_EXECUTABLE_NAME "MetasequoiaImeDictionaryReplay")
set(DICTIONARY_BUILD_EXECUTABLE_NAME "MetasequoiaImeDictionaryBuild")

add_executable(${WATCHDOG_EXECUTABLE_NAME} WIN32
    "./src/watchdog/main.cpp"
    "./src/watchdog/watchdog_protocol.h"
    "./src/utils/single_instance.h"
    "./src/resource/resource.h"
    "./src/resource/resource.rc"
)
target_compile_definitions(${WATCHDOG_EXECUTABLE_NAME} PRIVATE
    NOMINMAX=1
    WIN32_LEAN_AND_MEAN=1
)
target_link_libraries(${WATCHDOG_EXECUTABLE_NAME} PRIVATE shell32 ole32)
if(MSVC)
    target_compile_options(${WATCHDOG_EXECUTABLE_NAME} PRIVATE /W4 /permissive-)
endif()

set(SRC_FILES
    "./src/ai/ai_assistant.cpp"
    "./src/ai/ai_assistant.h"
    "./src/ai/ai_assistant_cache.cpp"
    "./src/ai/ai_assistant_cache.h"
    "./src/ai/ai_assistant_cache_key.cpp"
    "./src/ai/ai_assistant_cache_key.h"
    "./src/ai/ai_assistant_request.cpp"
    "./src/ai/ai_assistant_request.h"
    "./src/ai/ai_assistant_reply.cpp"
    "./src/ai/ai_assistant_reply.h"
    "./src/ai/ai_provider_store.cpp"
    "./src/ai/ai_provider_store.h"
    "./src/config/ai_provider.h"
    "./src/ai/chat_completions.cpp"
    "./src/ai/jev_client.cpp"
    "./src/ai/chat_completions.h"
    "./src/ai/chat_request_params.h"
    "./src/clipboard/clipboard_history.cpp"
    "./src/clipboard/clipboard_history.h"
    "./src/cloud/ai_translation.cpp"
    "./src/cloud/ai_translation.h"
    "./src/cloud/cloud_ime.cpp"
    "./src/cloud/cloud_ime.h"
    "./src/cloud/cloud_translation.cpp"
    "./src/cloud/cloud_translation.h"
    "./src/cloud/custom_translation.cpp"
    "./src/cloud/custom_translation.h"
    "./src/cloud/aliyun_mt.cpp"
    "./src/cloud/aliyun_mt.h"
    "./src/cloud/niutrans_translation.cpp"
    "./src/cloud/niutrans_translation.h"
    "./src/cloud/tencent_tmt.cpp"
    "./src/cloud/tencent_tmt.h"
    "./src/cloud/translation_gloss.cpp"
    "./src/cloud/translation_gloss.h"
    "./src/config/credential_protection.cpp"
    "./src/config/credential_protection.h"
    "./src/config/ime_config.cpp"
    "./src/config/ime_config_credentials.cpp"
    "./src/config/ime_config.h"
    "./src/config/ime_config_appearance.cpp"
    "./src/config/ime_config_fonts.cpp"
    "./src/config/ime_config_general.cpp"
    "./src/config/ime_config_input.cpp"
    "./src/config/ime_config_internal.h"
    "./src/config/ime_config_keys.cpp"
    "./src/config/ime_config_services.cpp"
    "./src/config/ime_config_shuangpin.cpp"
    "./src/config/ime_config_template.cpp"
    "./src/config/ime_config_theme.cpp"
    "./src/config/ime_config_toml.cpp"
    "./src/config/ime_config_toolbar.cpp"
    "./src/config/ime_config_write.cpp"
    "./src/conversion/chinese_converter.cpp"
    "./src/conversion/chinese_converter.h"
    "./src/conversion/opencc_character_tables.h"
    "./src/conversion/traditional_forms.h"
    "./src/conversion/traditional_spelling_index.cpp"
    "./src/conversion/traditional_spelling_index.h"
    "./src/defines/defines.h"
    "./src/english/english_ime.cpp"
    "./src/english/english_ime.h"
    "./src/global/globals.h"
    "./src/ipc/candidate_translation_policy.h"
    "./src/ipc/event_listener.cpp"
    "./src/ipc/event_listener.h"
    "./src/ipc/event_listener_async_candidates.cpp"
    "./src/ipc/event_listener_candidates.cpp"
    "./src/ipc/event_listener_internal.h"
    "./src/ipc/event_listener_keys.cpp"
    "./src/ipc/event_listener_pipes.cpp"
    "./src/ipc/event_listener_selection.cpp"
    "./src/ipc/event_listener_task_class.cpp"
    "./src/ipc/event_listener_worker.cpp"
    "./src/ipc/ipc.cpp"
    "./src/ipc/ipc.h"
    "./src/ipc/voice_composition_pipe.h"
    "./src/ipc/voice_control_dispatch.cpp"
    "./src/ipc/voice_control_dispatch.h"
    "./src/mixed/date_time_candidates.h"
    "./src/mixed/mixed_candidates.cpp"
    "./src/mixed/mixed_candidates.h"
    "./src/log/fanylog.cpp"
    "./src/log/fanylog.h"
    "./src/log/candidate_diag_log.cpp"
    "./src/log/candidate_diag_log.h"
    "./src/log/diagnostic_log_naming.cpp"
    "./src/log/diagnostic_log_naming.h"
    "./src/log/ftb_diag_log.h"
    "./src/resource/resource.h"
    "./src/resource/resource.rc"
    "./src/settings/settings_launcher.cpp"
    "./src/settings/settings_launcher.h"
    "./src/settings/api_credential_test.cpp"
    "./src/settings/api_credential_test.h"
    "./src/settings/api_credential_test_request.h"
    "./src/settings/collocation_model.cpp"
    "./src/settings/collocation_model.h"
    # 后台检查更新与自动下载：轮询跑在常驻的 Server 上，关掉设置窗口也不中断。
    "./src/settings/update_channel.cpp"
    "./src/settings/update_channel.h"
    "./src/settings/update_check.cpp"
    "./src/settings/update_check.h"
    "./src/settings/native/native_settings_update_download.cpp"
    "./src/settings/native/native_settings_update_download.h"
    "./src/skin/candidate_skin_catalog.cpp"
    "./src/skin/candidate_skin_catalog.h"
    "./src/session/engine_input_session.cpp"
    "./src/session/engine_input_session.h"
    "./src/session/input_cadence.cpp"
    "./src/session/input_cadence.h"
    "./src/session/input_session.h"
    "./src/session/session_factory.cpp"
    "./src/session/session_factory.h"
    "./src/statistics/stats_aggregate.cpp"
    "./src/statistics/stats_aggregate.h"
    "./src/statistics/stats_candidate_capture.cpp"
    "./src/statistics/stats_candidate_capture.h"
    "./src/statistics/stats_frames.cpp"
    "./src/statistics/stats_frames.h"
    "./src/statistics/stats_pipe.cpp"
    "./src/statistics/stats_pipe.h"
    "./src/statistics/stats_store.cpp"
    "./src/statistics/stats_store.h"
    "./src/statistics/stats_types.h"
    "./src/utils/candidate_size_estimator.cpp"
    "./src/utils/candidate_size_estimator.h"
    "./src/utils/common_utils.cpp"
    "./src/utils/common_utils.h"
    "./src/utils/hanzi_pinyin.cpp"
    "./src/utils/hanzi_pinyin.h"
    "./src/utils/ime_paths.cpp"
    "./src/utils/ime_utils.cpp"
    "./src/utils/ime_utils.h"
    "./src/utils/network_proxy.cpp"
    "./src/utils/network_proxy.h"
    "./src/utils/process_priority.cpp"
    "./src/utils/process_priority.h"
    "./src/utils/serial_task_runner.cpp"
    "./src/utils/serial_task_runner.h"
    "./src/utils/shuangpin_preedit_display.cpp"
    "./src/utils/shuangpin_preedit_display.h"
    "./src/utils/thread_priority.cpp"
    "./src/utils/thread_priority.h"
    "./src/utils/window_utils.cpp"
    "./src/utils/window_utils.h"
    "./src/window/candidate_presenter.cpp"
    "./src/window/candidate_presenter.h"
    "./src/window/caret_fallback.cpp"
    "./src/window/caret_fallback.h"
    "./src/window/candidate_skin_layers.cpp"
    "./src/window/candidate_skin_layers.h"
    "./src/window/candidate_skin_overlay.cpp"
    "./src/window/candidate_skin_overlay.h"
    "./src/window/candidate_skin_palette.cpp"
    "./src/window/candidate_skin_palette.h"
    "./src/window/candidate_skin_scene.cpp"
    "./src/window/candidate_skin_scene.h"
    "./src/window/candidate_skin_tokens.cpp"
    "./src/window/candidate_skin_tokens.h"
    "./src/window/tray_menu_presenter.cpp"
    "./src/window/tray_menu_presenter.h"
    "./src/window/floating_toolbar_presenter.cpp"
    "./src/window/floating_toolbar_presenter.h"
    "./src/window/floating_toolbar_image_layout.cpp"
    "./src/window/floating_toolbar_image_layout.h"
    "./src/window/floating_toolbar_skin.cpp"
    "./src/window/floating_toolbar_skin.h"
    "./src/window/caret_state_indicator.cpp"
    "./src/window/caret_state_indicator.h"
    "./src/window/ime_windows.cpp"
    "./src/window/ime_windows.h"
    "./src/window/ime_windows_candidate.cpp"
    "./src/window/ime_windows_internal.h"
    "./src/window/ime_windows_menu.cpp"
    "./src/window/ime_windows_toolbar.cpp"
    "./src/window/small_windows.cpp"
    "./src/window/small_windows.h"
    "./src/window/window_hook.cpp"
    "./src/window/window_hook.h"
    "./src/voice-input/voice_input_service.cpp"
    "./src/voice-input/voice_input_service.h"
    "./src/voice-input/system_audio_muter.cpp"
    "./src/voice-input/system_audio_muter.h"
    "./src/voice-input/doubao_asr_client.cpp"
    "./src/voice-input/doubao_asr_client.h"
    "./src/voice-input/voice_batch_protocol.cpp"
    "./src/voice-input/voice_providers.cpp"
    "./src/voice-input/voice_providers.h"
    "./src/voice-input/wave_overlay.cpp"
    "./src/voice-input/wave_overlay.h"
    "./src/voice-input/cue_player.cpp"
    "./src/voice-input/cue_player.h"
    "./src/voice-input/mvi_utils.h"
    "./src/voice-input/voice_input_overlay_utils.cpp"
    "./src/voice-input/voice_target.cpp"
    "./src/voice-input/voice_target.h"
    "./src/voice-input/voice_result_card.cpp"
    "./src/voice-input/voice_result_card.h"
    "./src/voice-input/voice_result_card_model.h"
    "./src/voice-input/voice_overlay_layout.h"
    "./src/voice-input/voice_commit_policy.h"
    "./src/main.cpp"
)

add_executable(${MY_EXECUTABLE_NAME} WIN32
    ${SRC_FILES}
)

# Tomlplusplus and other header-only libraries
target_include_directories(${PROJECT_NAME}
    PRIVATE
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/include"
    "${CMAKE_SOURCE_DIR}/src/voice-input"
    "${MSIME_ENGINE_DIR}/voice/third_party/miniaudio"
)

target_link_libraries(${PROJECT_NAME} PRIVATE MetasequoiaIme::Engine MetasequoiaIme::Voice MetasequoiaIme::VoiceCapture)
target_link_libraries(${PROJECT_NAME} PRIVATE unofficial::sqlite3::sqlite3 fmt::fmt spdlog::spdlog Boost::locale Boost::json nlohmann_json::nlohmann_json)
# U 模式候选的注音（utils/hanzi_pinyin）与设置页的导入注音共用 cpp-pinyin。
target_link_libraries(${PROJECT_NAME} PRIVATE cpp-pinyin::cpp-pinyin)
target_link_libraries(${PROJECT_NAME} PRIVATE OpenCC::OpenCC)
add_dependencies(${PROJECT_NAME} Dictionaries)
target_link_libraries(${PROJECT_NAME} PRIVATE winhttp)
target_link_libraries(${PROJECT_NAME} PRIVATE CURL::libcurl)
target_link_libraries(${PROJECT_NAME} PRIVATE ZLIB::ZLIB)
target_link_libraries(${PROJECT_NAME} PRIVATE d2d1 dwrite dwmapi shell32 shcore ole32 oleaut32 oleacc bcrypt)
target_link_libraries(${PROJECT_NAME} PRIVATE oleaut32)

# The helper executables below already build at /W4. The server itself -- the process that owns
# every network call and the skin loader -- was still at the default /W3.
if(MSVC)
    target_compile_options(${MY_EXECUTABLE_NAME} PRIVATE /W4)
endif()

add_executable(${DICTIONARY_REPLAY_EXECUTABLE_NAME}
    "./src/user-dictionary-replay/main.cpp"
    "./src/resource/resource.h"
    "./src/resource/resource.rc"
)
target_link_libraries(${DICTIONARY_REPLAY_EXECUTABLE_NAME} PRIVATE
    MetasequoiaIme::Engine
    unofficial::sqlite3::sqlite3
)
target_compile_definitions(${DICTIONARY_REPLAY_EXECUTABLE_NAME} PRIVATE NOMINMAX=1)
add_dependencies(${PROJECT_NAME} ${DICTIONARY_REPLAY_EXECUTABLE_NAME})

# 安装时把 scripts/pack-dictionary-source.py 导出的文本源还原成词库，安装包因此不必带 SQLite 文件。
add_executable(${DICTIONARY_BUILD_EXECUTABLE_NAME}
    "./src/dictionary-build/main.cpp"
    "./src/resource/resource.h"
    "./src/resource/resource.rc"
)
target_link_libraries(${DICTIONARY_BUILD_EXECUTABLE_NAME} PRIVATE unofficial::sqlite3::sqlite3 bcrypt)
target_compile_definitions(${DICTIONARY_BUILD_EXECUTABLE_NAME} PRIVATE NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)
if(MSVC)
    target_compile_options(${DICTIONARY_BUILD_EXECUTABLE_NAME} PRIVATE /W4 /permissive-)
endif()
add_dependencies(${PROJECT_NAME} ${DICTIONARY_BUILD_EXECUTABLE_NAME})

# msimeui stays a separately built GUI framework component at ../ui. The product-owned emoji panel
# lives under src/emoji-panel so it can evolve separately from the framework's example applications.
# msimeui's standalone layout tests belong to its own CI job. Its EXCLUDE_FROM_ALL
# targets are not built here, so registering those tests would break Server CTest.
set(_metasequoia_saved_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../ui" "${CMAKE_BINARY_DIR}/msimeui" EXCLUDE_FROM_ALL)
set(BUILD_TESTING "${_metasequoia_saved_build_testing}")
unset(_metasequoia_saved_build_testing)
set(EMOJI_EXECUTABLE_NAME "MetasequoiaImeEmojiPanel")
add_executable(${EMOJI_EXECUTABLE_NAME} WIN32
    "./src/emoji-panel/EmojiPanel.cpp"
    "./src/emoji-panel/EmojiPanel.h"
    "./src/emoji-panel/emoji_panel_icons.cpp"
    "./src/emoji-panel/emoji_panel_icons.h"
    "./src/emoji-panel/emoji_panel_splash.cpp"
    "./src/emoji-panel/emoji_panel_splash.h"
    "./src/emoji-panel/svg_path_geometry.cpp"
    "./src/emoji-panel/svg_path_geometry.h"
    "./src/emoji-panel/main.cpp"
    "./src/emoji-panel/emoji_panel.rc"
    "./src/emoji-panel/resource.h"
    "./src/clipboard/clipboard_history.cpp"
    "./src/clipboard/clipboard_history.h"
    "./src/utils/ime_paths.cpp"
    "./src/utils/surface_theme_config.cpp"
    "./src/utils/surface_theme_config.h"
)
add_dependencies(${MY_EXECUTABLE_NAME} ${WATCHDOG_EXECUTABLE_NAME})
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX=1)
target_link_libraries(${PROJECT_NAME} PRIVATE msimeui d3d11 dxgi dcomp)
target_link_libraries(${EMOJI_EXECUTABLE_NAME} PRIVATE msimeui unofficial::sqlite3::sqlite3 shell32 advapi32 ole32)
target_compile_definitions(${EMOJI_EXECUTABLE_NAME} PRIVATE
    NOMINMAX=1
    WIN32_LEAN_AND_MEAN=1
)
if(MSVC)
    target_compile_options(${EMOJI_EXECUTABLE_NAME} PRIVATE /W4 /permissive-)
endif()
add_dependencies(${PROJECT_NAME} ${EMOJI_EXECUTABLE_NAME})
add_custom_command(TARGET ${EMOJI_EXECUTABLE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/$<$<CONFIG:Debug>:debug/>bin/sqlite3.dll"
        $<TARGET_FILE_DIR:${EMOJI_EXECUTABLE_NAME}>
)

set(KEYBOARD_EXECUTABLE_NAME "MetasequoiaImeKeyboardPanel")
add_executable(${KEYBOARD_EXECUTABLE_NAME} WIN32
    "./src/keyboard-panel/KeyboardPanel.cpp"
    "./src/keyboard-panel/KeyboardPanel.h"
    "./src/keyboard-panel/main.cpp"
    "./src/keyboard-panel/keyboard_panel.rc"
    "./src/keyboard-panel/resource.h"
    "./src/utils/ime_paths.cpp"
    "./src/utils/surface_theme_config.cpp"
    "./src/utils/surface_theme_config.h"
)
target_link_libraries(${KEYBOARD_EXECUTABLE_NAME} PRIVATE msimeui shell32 advapi32 ole32)
target_compile_definitions(${KEYBOARD_EXECUTABLE_NAME} PRIVATE
    NOMINMAX=1
    WIN32_LEAN_AND_MEAN=1
)
if(MSVC)
    target_compile_options(${KEYBOARD_EXECUTABLE_NAME} PRIVATE /W4 /permissive-)
endif()
add_dependencies(${PROJECT_NAME} ${KEYBOARD_EXECUTABLE_NAME})

set(HANDWRITING_EXECUTABLE_NAME "MetasequoiaImeHandwritingPanel")
add_executable(${HANDWRITING_EXECUTABLE_NAME} WIN32
    "./src/handwriting-panel/HandwritingPanel.cpp"
    "./src/handwriting-panel/HandwritingPanel.h"
    "./src/handwriting-panel/main.cpp"
    "./src/handwriting-panel/handwriting_panel.rc"
    "./src/handwriting-panel/resource.h"
    "./src/utils/ime_paths.cpp"
    "./src/utils/surface_theme_config.cpp"
    "./src/utils/surface_theme_config.h"
)
target_link_libraries(${HANDWRITING_EXECUTABLE_NAME} PRIVATE msimeui windowsapp.lib shell32 advapi32 ole32)
target_compile_definitions(${HANDWRITING_EXECUTABLE_NAME} PRIVATE
    NOMINMAX=1
    WIN32_LEAN_AND_MEAN=1
)
if(MSVC)
    target_compile_options(${HANDWRITING_EXECUTABLE_NAME} PRIVATE /W4 /permissive-)
endif()
add_dependencies(${PROJECT_NAME} ${HANDWRITING_EXECUTABLE_NAME})

set(SETTINGS_EXECUTABLE_NAME "MetasequoiaImeSettings")
add_executable(${SETTINGS_EXECUTABLE_NAME} WIN32
    "./src/settings/settings_app.cpp"
    "./src/ai/chat_request_params.h"
    "./src/settings/api_credential_test.cpp"
    "./src/settings/api_credential_test.h"
    "./src/ai/ai_provider_store.cpp"
    "./src/ai/ai_provider_store.h"
    "./src/config/ai_provider.h"
    "./src/settings/api_credential_test_request.h"
    "./src/settings/collocation_model.cpp"
    "./src/settings/collocation_model.h"
    "./src/settings/neural_model.cpp"
    "./src/settings/neural_model.h"
    "./src/settings/lattice_model.cpp"
    "./src/settings/lattice_model.h"
    "./src/settings/model_download.cpp"
    "./src/settings/model_download.h"
    "./src/settings/default_input_method.cpp"
    "./src/settings/default_input_method.h"
    "./src/settings/game_compat_name.cpp"
    "./src/settings/game_compat_name.h"
    "./src/settings/input_method_install.cpp"
    "./src/settings/input_method_install.h"
    # 设置页要判断通道、读后台下载的结果，只用到通道判定与状态文件，不引轮询线程。
    "./src/settings/update_channel.cpp"
    "./src/settings/update_channel.h"
    "./src/settings/settings_launcher.cpp"
    "./src/settings/settings_launcher.h"
    "./src/settings/native/native_settings_bindings.cpp"
    "./src/settings/native/native_settings_controls.cpp"
    "./src/settings/native/native_settings_controls.h"
    "./src/settings/native/native_settings_model.cpp"
    "./src/settings/native/native_settings_model.h"
    "./src/settings/native/native_settings_pages.h"
    "./src/settings/native/native_settings_search.cpp"
    "./src/settings/native/native_settings_search.h"
    "./src/settings/native/native_settings_pill.cpp"
    "./src/settings/native/native_settings_pill.h"
    "./src/settings/native/native_settings_theme.cpp"
    "./src/settings/native/native_settings_theme.h"
    "./src/settings/native/native_settings_window.cpp"
    "./src/settings/native/native_settings_window.h"
    "./src/settings/native/native_page_appearance.cpp"
    "./src/settings/native/native_page_input.cpp"
    "./src/settings/native/native_page_helpcode.cpp"
    "./src/settings/native/native_page_model_combo.cpp"
    "./src/settings/native/native_page_model_combo.h"
    "./src/settings/native/native_page_shortcut.cpp"
    "./src/settings/native/native_page_dictionary.cpp"
    "./src/settings/native/native_page_dictionary_panel.cpp"
    "./src/settings/native/native_page_dictionary_panel.h"
    "./src/settings/native/native_page_skin.cpp"
    "./src/settings/native/native_page_skin_common.cpp"
    "./src/settings/native/native_page_skin_common.h"
    "./src/settings/native/native_page_skin_widgets.cpp"
    "./src/settings/native/native_page_voice.cpp"
    "./src/settings/native/native_page_voice_fields.cpp"
    "./src/settings/native/native_page_voice_fields.h"
    "./src/settings/native/native_page_screenkb.cpp"
    "./src/settings/native/native_page_handwriting.cpp"
    "./src/settings/native/native_page_tools.cpp"
    "./src/settings/native/native_page_ai.cpp"
    "./src/settings/native/native_page_floating_toolbar.cpp"
    "./src/settings/native/native_page_stats.cpp"
    "./src/settings/native/native_page_help.cpp"
    "./src/settings/native/native_page_help_widgets.cpp"
    "./src/settings/native/native_page_help_widgets.h"
    "./src/settings/native/native_page_about.cpp"
    "./src/settings/native/native_settings_update_download.cpp"
    "./src/settings/native/native_settings_update_download.h"
    "./src/settings/native/native_page_feedback.cpp"
    "./src/skin/candidate_skin_catalog.cpp"
    "./src/skin/candidate_skin_catalog.h"
    "./src/window/candidate_skin_layers.cpp"
    "./src/window/candidate_skin_layers.h"
    # 皮肤预览与候选窗共用同一套场景与皮肤解析，画出来的才一模一样。
    "./src/window/candidate_skin_overlay.cpp"
    "./src/window/candidate_skin_overlay.h"
    "./src/window/candidate_skin_palette.cpp"
    "./src/window/candidate_skin_palette.h"
    "./src/window/candidate_skin_scene.cpp"
    "./src/window/candidate_skin_scene.h"
    "./src/window/candidate_skin_tokens.cpp"
    "./src/window/candidate_skin_tokens.h"
    "./src/window/floating_toolbar_image_layout.cpp"
    "./src/window/floating_toolbar_image_layout.h"
    "./src/window/floating_toolbar_skin.cpp"
    "./src/window/floating_toolbar_skin.h"
    "./src/settings/dictionary_manager.cpp"
    "./src/settings/dictionary_manager.h"
    "./src/settings/dictionary_validation.cpp"
    "./src/settings/dictionary_validation.h"
    "./src/statistics/stats_overview.cpp"
    "./src/statistics/stats_overview.h"
    "./src/statistics/stats_store.cpp"
    "./src/statistics/stats_store.h"
    "./src/config/credential_protection.cpp"
    "./src/config/credential_protection.h"
    "./src/config/ime_config.cpp"
    "./src/config/ime_config_credentials.cpp"
    "./src/config/ime_config.h"
    "./src/config/ime_config_appearance.cpp"
    "./src/config/ime_config_fonts.cpp"
    "./src/config/ime_config_general.cpp"
    "./src/config/ime_config_input.cpp"
    "./src/config/ime_config_internal.h"
    "./src/config/ime_config_keys.cpp"
    "./src/config/ime_config_services.cpp"
    "./src/config/ime_config_shuangpin.cpp"
    "./src/config/ime_config_template.cpp"
    "./src/config/ime_config_theme.cpp"
    "./src/config/ime_config_toml.cpp"
    "./src/config/ime_config_toolbar.cpp"
    "./src/config/ime_config_write.cpp"
    "./src/ai/chat_completions.cpp"
    "./src/ai/jev_client.cpp"
    "./src/ai/chat_completions.h"
    "./src/ai/ai_assistant_reply.cpp"
    "./src/ai/ai_assistant_reply.h"
    "./src/ai/ai_assistant_request.cpp"
    "./src/ai/ai_assistant_request.h"
    "./src/cloud/ai_translation.cpp"
    "./src/cloud/ai_translation.h"
    "./src/cloud/custom_translation.cpp"
    "./src/cloud/custom_translation.h"
    "./src/cloud/aliyun_mt.cpp"
    "./src/cloud/aliyun_mt.h"
    "./src/cloud/niutrans_translation.cpp"
    "./src/cloud/niutrans_translation.h"
    "./src/cloud/tencent_tmt.cpp"
    "./src/cloud/tencent_tmt.h"
    "./src/cloud/translation_gloss.cpp"
    "./src/cloud/translation_gloss.h"
    "./src/voice-input/doubao_asr_client.cpp"
    "./src/voice-input/doubao_asr_client.h"
    "./src/voice-input/voice_providers.cpp"
    "./src/voice-input/voice_providers.h"
    "./src/clipboard/clipboard_history.cpp"
    "./src/clipboard/clipboard_history.h"
    "./src/utils/common_utils.cpp"
    "./src/utils/common_utils.h"
    "./src/utils/hanzi_pinyin.cpp"
    "./src/utils/hanzi_pinyin.h"
    "./src/utils/ime_paths.cpp"
    "./src/utils/network_proxy.cpp"
    "./src/utils/network_proxy.h"
    "./src/resource/resource.rc"
)
target_include_directories(${SETTINGS_EXECUTABLE_NAME}
    PRIVATE
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/include"
)
target_link_libraries(${SETTINGS_EXECUTABLE_NAME} PRIVATE
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
if(MSVC)
    target_compile_options(${SETTINGS_EXECUTABLE_NAME} PRIVATE /W4)
endif()
# The server launches the settings executable from the same output directory.
# Building the server target alone must therefore also produce Settings.
add_dependencies(${PROJECT_NAME} ${SETTINGS_EXECUTABLE_NAME})
add_custom_command(TARGET ${SETTINGS_EXECUTABLE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${MSIME_VENDOR_DIR}/cpp-pinyin/res/dict"
        "$<TARGET_FILE_DIR:${SETTINGS_EXECUTABLE_NAME}>/dict"
)

# OpenCC 配置与它们引用的字典：s2t 做标准繁体转换，t2tw、t2hk 在 s2t 结果上取台湾、香港写法，
# t2s 把繁体转回简体，STCharacters 同时给单字的全部写法。tests/ 的真数据测试读这里拷出的目录。
set(MSIME_OPENCC_RUNTIME_FILES
    ${MSIME_VENDOR_DIR}/opencc/data/config/s2t.json
    ${MSIME_VENDOR_DIR}/opencc/data/config/t2tw.json
    ${MSIME_VENDOR_DIR}/opencc/data/config/t2hk.json
    ${CMAKE_BINARY_DIR}/opencc/data/CJK_Compatibility_Ideographs.ocd2
    ${CMAKE_BINARY_DIR}/opencc/data/STCharacters.ocd2
    ${CMAKE_BINARY_DIR}/opencc/data/STPhrases.ocd2
    ${CMAKE_BINARY_DIR}/opencc/data/STPhrases_GeneratedFromRegionalPhrases.ocd2
    ${MSIME_VENDOR_DIR}/opencc/data/config/t2s.json
    ${CMAKE_BINARY_DIR}/opencc/data/TSCharacters.ocd2
    ${CMAKE_BINARY_DIR}/opencc/data/TSCharactersExt.ocd2
    ${CMAKE_BINARY_DIR}/opencc/data/TSPhrases.ocd2
    ${CMAKE_BINARY_DIR}/opencc/data/TWVariants.ocd2
    ${CMAKE_BINARY_DIR}/opencc/data/TWVariantsPhrases.ocd2
    ${CMAKE_BINARY_DIR}/opencc/data/HKVariants.ocd2
    ${CMAKE_BINARY_DIR}/opencc/data/HKVariantsPhrases.ocd2
)

# Runtime assets next to the executable
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/audios
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/assets/audios/start.mp3
        ${CMAKE_SOURCE_DIR}/assets/audios/end.mp3
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/audios
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/opencc
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MSIME_OPENCC_RUNTIME_FILES}
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/opencc
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
