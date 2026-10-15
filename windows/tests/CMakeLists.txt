cmake_minimum_required(VERSION 3.25)

# Also usable without the TSF DLL, the Windows SDK or the engine.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(MsimeTsfLocalPolicyTests LANGUAGES CXX)
    enable_testing()
    # windows/CMakeLists.txt sets this for the in-tree build; the sources and headers are UTF-8.
    if(MSVC)
        add_compile_options(/utf-8)
    endif()
endif()

add_executable(test_vim_mode vim_mode.cpp ../src/Key/VimMode.cpp)
target_include_directories(test_vim_mode PRIVATE ../src)
target_compile_features(test_vim_mode PRIVATE cxx_std_17)
add_test(NAME vim_mode COMMAND test_vim_mode)

add_executable(test_client_fallback client_fallback.cpp)
target_include_directories(test_client_fallback PRIVATE ../src)
target_compile_features(test_client_fallback PRIVATE cxx_std_17)
add_test(NAME client_fallback COMMAND test_client_fallback)

add_executable(test_key_repeat_guard key_repeat_guard.cpp)
target_include_directories(test_key_repeat_guard PRIVATE ../src)
target_compile_features(test_key_repeat_guard PRIVATE cxx_std_17)
add_test(NAME key_repeat_guard COMMAND test_key_repeat_guard)

add_executable(test_caps_lock_punctuation_policy caps_lock_punctuation_policy.cpp)
target_include_directories(test_caps_lock_punctuation_policy PRIVATE ../src)
target_compile_features(test_caps_lock_punctuation_policy PRIVATE cxx_std_17)
add_test(NAME caps_lock_punctuation_policy COMMAND test_caps_lock_punctuation_policy)

add_executable(test_commit_candidate_and_continue_payload commit_candidate_and_continue_payload.cpp)
target_include_directories(test_commit_candidate_and_continue_payload PRIVATE ../src)
target_compile_features(test_commit_candidate_and_continue_payload PRIVATE cxx_std_17)
add_test(NAME commit_candidate_and_continue_payload COMMAND test_commit_candidate_and_continue_payload)

add_executable(test_punctuation_commit_text punctuation_commit_text.cpp)
target_include_directories(test_punctuation_commit_text PRIVATE ../src)
target_compile_features(test_punctuation_commit_text PRIVATE cxx_std_17)
add_test(NAME punctuation_commit_text COMMAND test_punctuation_commit_text)

# The classifier and the event queue are compiled into this target directly:
# they must stay free of TSF and DLL lifecycle dependencies.
add_executable(test_statistics statistics.cpp ../src/Statistics/char_classify.cpp)
target_include_directories(test_statistics PRIVATE ../src)
target_compile_features(test_statistics PRIVATE cxx_std_17)
add_test(NAME statistics COMMAND test_statistics)

add_executable(test_key_focus_recovery key_focus_recovery.cpp)
target_include_directories(test_key_focus_recovery PRIVATE ../src)
target_compile_features(test_key_focus_recovery PRIVATE cxx_std_17)
add_test(NAME key_focus_recovery COMMAND test_key_focus_recovery)

add_executable(test_deferred_key_failure_policy deferred_key_failure_policy.cpp)
target_include_directories(test_deferred_key_failure_policy PRIVATE ../src)
target_compile_features(test_deferred_key_failure_policy PRIVATE cxx_std_17)
add_test(NAME deferred_key_failure_policy COMMAND test_deferred_key_failure_policy)

add_executable(test_caret_anchor_policy caret_anchor_policy.cpp)
target_include_directories(test_caret_anchor_policy PRIVATE ../src)
target_compile_features(test_caret_anchor_policy PRIVATE cxx_std_17)
add_test(NAME caret_anchor_policy COMMAND test_caret_anchor_policy)

add_executable(test_focus_announcement_policy focus_announcement_policy.cpp)
target_include_directories(test_focus_announcement_policy PRIVATE ../src)
target_compile_features(test_focus_announcement_policy PRIVATE cxx_std_17)
add_test(NAME focus_announcement_policy COMMAND test_focus_announcement_policy)

# Exercise the production reconnect sequencing without loading a TIP into a host.
# Regenerate the fixture when either handler changes, so a missing recovery step
# cannot be hidden by a copied implementation in the test.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(reconnect_window_source "${CMAKE_CURRENT_SOURCE_DIR}/../src/IME/MetasequoiaIME_WindowProc.cpp")
set(reconnect_key_source "${CMAKE_CURRENT_SOURCE_DIR}/../src/Key/KeyEventSink_DeferredKeys.cpp")
set(reconnect_fixture_generator "${CMAKE_CURRENT_SOURCE_DIR}/generate_reconnect_fixture.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${reconnect_window_source}" "${reconnect_key_source}" "${reconnect_fixture_generator}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${reconnect_fixture_generator}"
        "${reconnect_window_source}" "${reconnect_key_source}"
        "${CMAKE_CURRENT_BINARY_DIR}/reconnect_recovery_source.h"
    COMMAND_ERROR_IS_FATAL ANY)
add_executable(test_reconnect_recovery reconnect_recovery.cpp)
target_include_directories(test_reconnect_recovery PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(test_reconnect_recovery PRIVATE cxx_std_17)
add_test(NAME reconnect_recovery COMMAND test_reconnect_recovery)
