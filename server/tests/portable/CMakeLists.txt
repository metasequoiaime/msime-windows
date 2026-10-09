cmake_minimum_required(VERSION 3.25.0)
project(MetasequoiaImePortableTests LANGUAGES CXX)

# A second, deliberately tiny test target holding the server sources that parse untrusted input and
# do not touch a Windows API. The point is that it builds anywhere, with no vcpkg tree and no Boost,
# so CI can run it under AddressSanitizer and UndefinedBehaviorSanitizer on a Linux runner in
# seconds. The full server test executable cannot: it links Boost, curl, sqlite, WebView2 and D2D,
# and a sanitized build of that chain is a long job on a Windows runner.
#
# What belongs here: host-independent logic that reads data the user did not write -- skin packages,
# dictionary files, IPC payloads, config. Add sources as they become separable; anything needing a
# Windows header stays in the main test executable next door.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SERVER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(portable_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${SERVER_ROOT}/tests/src/test_skin_css_policy.cpp"
    "${SERVER_ROOT}/tests/src/test_input_key_policy.cpp"
    "${SERVER_ROOT}/tests/src/test_candidate_text_policy.cpp"
    "${SERVER_ROOT}/src/webview2/skin_css_policy.cpp"
)

target_include_directories(portable_tests PRIVATE "${SERVER_ROOT}" "${SERVER_ROOT}/src" "${SERVER_ROOT}/..")

if(MSIME_PORTABLE_TESTS_SANITIZE AND NOT MSVC)
    target_compile_options(portable_tests PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
    target_link_options(portable_tests PRIVATE -fsanitize=address,undefined)
endif()

if(MSVC)
    # Standalone project, so the server's directory-wide /utf-8 does not reach it.
    target_compile_options(portable_tests PRIVATE /utf-8)
else()
    target_compile_options(portable_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

include(CTest)
add_test(NAME portable_tests COMMAND portable_tests)
