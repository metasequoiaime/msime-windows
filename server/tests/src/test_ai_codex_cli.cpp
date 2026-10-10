#include "ai/codex_cli.h"
#include "tests/includes/test_framework.h"

#include <windows.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <thread>

namespace
{
std::string Utf8(const std::wstring &text)
{
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

CodexCli::Request Fixture(const std::string &model = "fixture.success")
{
    return {MSIME_CODEX_CLI_TEST_EXE, model, "synthetic 中文 input & | < > \" \\ --model injection"};
}

class EnvironmentOverride
{
  public:
    EnvironmentOverride(const wchar_t *name, const std::wstring &value) : name_(name)
    {
        const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        present_ = size != 0;
        if (size)
        {
            previous_.resize(size);
            previous_.resize(GetEnvironmentVariableW(name, previous_.data(), size));
        }
        REQUIRE(SetEnvironmentVariableW(name, value.c_str()));
    }
    ~EnvironmentOverride()
    {
        SetEnvironmentVariableW(name_.c_str(), present_ ? previous_.c_str() : nullptr);
    }

  private:
    std::wstring name_, previous_;
    bool present_ = false;
};
} // namespace

TEST_CASE(ai_codex_cli_native_child_receives_exact_stdin_and_model_without_shell)
{
    auto request = Fixture("a model \"with quotes\" & --dangerously-bypass-approvals-and-sandbox \\");
    const auto result = CodexCli::Run(request);
    REQUIRE(result.ok);
    const auto response = nlohmann::json::parse(result.output);
    REQUIRE_EQ(response.at("fixture_input").get<std::string>(), request.prompt);
    REQUIRE_EQ(response.at("fixture_model").get<std::string>(), request.model);
}

TEST_CASE(ai_codex_cli_rejects_tool_events_failed_turns_and_bad_json)
{
    std::string message;
    bool completed = false;
    REQUIRE(!CodexCli::detail::ConsumeEvent("not json", message, completed));
    REQUIRE(!CodexCli::detail::ConsumeEvent(R"({"type":"turn.failed"})", message, completed));
    REQUIRE(!CodexCli::detail::ConsumeEvent(R"({"type":"unrecognized.tool.event"})", message, completed));
    REQUIRE(!CodexCli::detail::ConsumeEvent(R"({"type":"error","message":"fatal"})", message, completed));
    REQUIRE(!CodexCli::detail::ConsumeEvent(
        R"({"type":"item.completed","item":{"type":"error","message":"unexpected failure"}})", message, completed));
    REQUIRE(CodexCli::detail::ConsumeEvent(
        R"({"type":"item.completed","item":{"type":"error","message":"Ignoring malformed agent role definition: synthetic role"}})",
        message, completed));
    REQUIRE(!CodexCli::detail::ConsumeEvent(R"({"type":"item.started","item":{"type":"file_change"}})", message,
                                            completed));
    REQUIRE(!CodexCli::Run(Fixture("fixture.tool")).ok);
    REQUIRE(!CodexCli::Run(Fixture("fixture.failure")).ok);
    REQUIRE(!CodexCli::Run(Fixture("fixture.flood")).ok);
}

TEST_CASE(ai_codex_cli_proxy_environment_isolated_inherited_and_direct)
{
    EnvironmentOverride http(L"HTTP_PROXY", L"http://inherited.invalid:1111");
    EnvironmentOverride https(L"HTTPS_PROXY", L"http://inherited.invalid:2222");
    EnvironmentOverride all(L"ALL_PROXY", L"socks5://inherited.invalid:3333");
    EnvironmentOverride no_proxy(L"NO_PROXY", L"existing.invalid");
    EnvironmentOverride sentinel(L"MSIME_CODEX_AUTH_SENTINEL", L"synthetic preserved value");
    auto request = Fixture();
    auto result = CodexCli::Run(request);
    REQUIRE(result.ok);
    auto output = nlohmann::json::parse(result.output);
    REQUIRE_EQ(output.at("fixture_https_proxy"), "http://inherited.invalid:2222");
    REQUIRE_EQ(output.at("fixture_all_proxy"), "socks5://inherited.invalid:3333");
    request.proxy = "http://configured.invalid:4444";
    result = CodexCli::Run(request);
    REQUIRE(result.ok);
    output = nlohmann::json::parse(result.output);
    REQUIRE_EQ(output.at("fixture_http_proxy"), *request.proxy);
    REQUIRE_EQ(output.at("fixture_https_proxy"), *request.proxy);
    REQUIRE_EQ(output.at("fixture_all_proxy"), "");
    REQUIRE_EQ(output.at("fixture_no_proxy"), "localhost,127.0.0.1,::1");
    REQUIRE_EQ(output.at("fixture_sentinel"), "synthetic preserved value");
    request.proxy = "";
    result = CodexCli::Run(request);
    REQUIRE(result.ok);
    output = nlohmann::json::parse(result.output);
    REQUIRE_EQ(output.at("fixture_http_proxy"), "");
    REQUIRE_EQ(output.at("fixture_https_proxy"), "");
    REQUIRE_EQ(output.at("fixture_all_proxy"), "");
    REQUIRE_EQ(output.at("fixture_no_proxy"), "*");
    wchar_t value[128]{};
    REQUIRE(GetEnvironmentVariableW(L"HTTPS_PROXY", value, 128));
    REQUIRE_EQ(std::wstring(value), L"http://inherited.invalid:2222");
    REQUIRE(GetEnvironmentVariableW(L"ALL_PROXY", value, 128));
    REQUIRE_EQ(std::wstring(value), L"socks5://inherited.invalid:3333");
}

TEST_CASE(ai_codex_cli_timeout_and_cancellation_interrupt_a_blocked_stdin_write)
{
    auto request = Fixture("fixture.hang");
    request.prompt.assign(32 * 1024, 'x');
    request.timeout = std::chrono::milliseconds(150);
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(!CodexCli::Run(request).ok);
    REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    request.timeout = std::chrono::seconds(8);
    const auto cancel_started = std::chrono::steady_clock::now();
    REQUIRE(!CodexCli::Run(request, [&] {
                 return std::chrono::steady_clock::now() - cancel_started >= std::chrono::milliseconds(150);
             }).ok);
    REQUIRE(std::chrono::steady_clock::now() - cancel_started < std::chrono::seconds(2));
}

TEST_CASE(ai_codex_cli_job_terminates_descendants)
{
    const auto marker = std::filesystem::temp_directory_path() /
                        (L"msime-codex-child-" + std::to_wstring(GetCurrentProcessId()) + L".pid");
    std::error_code ignored;
    std::filesystem::remove(marker, ignored);
    REQUIRE(SetEnvironmentVariableW(L"MSIME_CODEX_CHILD_PID_FILE", marker.c_str()));
    auto request = Fixture("fixture.spawn");
    request.timeout = std::chrono::milliseconds(500);
    const auto result = CodexCli::Run(request);
    SetEnvironmentVariableW(L"MSIME_CODEX_CHILD_PID_FILE", nullptr);
    REQUIRE(!result.ok);
    DWORD child_pid = 0;
    std::ifstream(marker) >> child_pid;
    std::filesystem::remove(marker, ignored);
    REQUIRE(child_pid != 0);
    const HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, child_pid);
    if (child)
    {
        const DWORD wait = WaitForSingleObject(child, 1000);
        CloseHandle(child);
        REQUIRE_EQ(wait, WAIT_OBJECT_0);
    }
}

TEST_CASE(ai_codex_cli_resolves_npm_shim_to_native_executable)
{
    const auto root =
        std::filesystem::temp_directory_path() / (L"msime-codex-resolve-" + std::to_wstring(GetCurrentProcessId()));
    const auto native = root / L"node_modules" / L"@openai" / L"codex" / L"node_modules" / L"@openai" /
#if defined(_M_ARM64)
                        L"codex-win32-arm64" / L"vendor" / L"aarch64-pc-windows-msvc" / L"bin" / L"codex.exe";
#else
                        L"codex-win32-x64" / L"vendor" / L"x86_64-pc-windows-msvc" / L"bin" / L"codex.exe";
#endif
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(native.parent_path());
    std::ofstream(root / L"codex.cmd") << "This fixture must never be executed by a shell";
    std::ofstream(native) << "resolution fixture";
    REQUIRE_EQ(CodexCli::detail::ResolveExecutable(Utf8((root / L"codex.cmd").wstring())), native);
    REQUIRE(CodexCli::detail::ResolveExecutable(Utf8((root / L"arbitrary.cmd").wstring())).empty());
    {
        EnvironmentOverride path(L"PATH", L".;\"" + root.wstring() + L"\"");
        REQUIRE_EQ(CodexCli::detail::ResolveExecutable("codex"), native);
        REQUIRE_EQ(CodexCli::detail::ResolveExecutable("codex.cmd"), native);
    }
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE(ai_codex_cli_rejects_oversize_inputs_and_pre_cancelled_requests)
{
    auto request = Fixture();
    request.prompt.assign(32 * 1024 + 1, 'x');
    REQUIRE(!CodexCli::Run(request).ok);
    REQUIRE(!CodexCli::Run(Fixture(), [] { return true; }).ok);
    REQUIRE(CodexCli::detail::ResolveExecutable("codex\" & something").empty());
}
