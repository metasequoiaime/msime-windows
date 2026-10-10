// Native executable fixture. It never authenticates or sends network requests.
#include <windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
std::string Environment(const wchar_t *name)
{
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size)
        return {};
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), bytes, nullptr,
                        nullptr);
    return result;
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    std::wstring model;
    std::vector<std::wstring> arguments;
    for (int index = 1; index < argc; ++index)
    {
        arguments.emplace_back(argv[index]);
        if (arguments.back() == L"--model" && index + 1 < argc)
            model = argv[index + 1];
    }
    if (model == L"fixture.descendant")
    {
        Sleep(30000);
        return 0;
    }
    if (model == L"fixture.hang" || model == L"fixture.spawn")
    {
        if (model == L"fixture.spawn")
        {
            wchar_t filename[MAX_PATH + 1]{}, marker[MAX_PATH + 1]{};
            GetModuleFileNameW(nullptr, filename, MAX_PATH);
            GetEnvironmentVariableW(L"MSIME_CODEX_CHILD_PID_FILE", marker, MAX_PATH);
            std::wstring command = L"\"" + std::wstring(filename) + L"\" --model fixture.descendant";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION child{};
            if (!CreateProcessW(filename, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                &startup, &child))
                return 4;
            std::ofstream(std::filesystem::path(marker)) << child.dwProcessId;
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
        }
        // Deliberately never read stdin: prompts larger than the pipe buffer must be cancellable.
        Sleep(30000);
        return 0;
    }
    const std::string input((std::istreambuf_iterator<char>(std::cin)), {});
    if (model == L"fixture.flood")
    {
        std::cout << std::string(600 * 1024, 'x') << std::endl;
        return 0;
    }
    if (model == L"fixture.tool")
    {
        std::cout << R"JSON({"type":"item.started","item":{"type":"command_execution"}})JSON" << std::endl;
        return 0;
    }
    if (model == L"fixture.failure")
        return 7;
    wchar_t working[MAX_PATH + 1]{};
    GetCurrentDirectoryW(MAX_PATH, working);
    const bool schema_present = std::filesystem::exists(std::filesystem::path(working) / L"candidates.schema.json");
    const auto has = [&](const wchar_t *value) {
        return std::find(arguments.begin(), arguments.end(), value) != arguments.end();
    };
    if (!schema_present || !has(L"--ignore-user-config") || !has(L"--ignore-rules") || !has(L"--ephemeral") ||
        !has(L"read-only") || !has(L"features.shell_tool=false") || !has(L"features.apps=false") ||
        !has(L"mcp_servers={}") || !has(L"approval_policy=\"never\"") || arguments.back() != L"-")
        return 8;
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, model.data(), static_cast<int>(model.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8_model(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, model.data(), static_cast<int>(model.size()), utf8_model.data(), bytes, nullptr,
                        nullptr);
    const auto output =
        nlohmann::json{{"candidates", {{{"text", "合成候选"}, {"type", "chinese"}, {"confidence", 0.9}}}},
                       {"fixture_input", input},
                       {"fixture_model", utf8_model},
                       {"fixture_http_proxy", Environment(L"HTTP_PROXY")},
                       {"fixture_https_proxy", Environment(L"https_proxy")},
                       {"fixture_all_proxy", Environment(L"ALL_PROXY")},
                       {"fixture_no_proxy", Environment(L"NO_PROXY")},
                       {"fixture_sentinel", Environment(L"MSIME_CODEX_AUTH_SENTINEL")}}
            .dump();
    std::cout
        << nlohmann::json{{"type", "item.completed"}, {"item", {{"type", "agent_message"}, {"text", output}}}}.dump()
        << std::endl;
    std::cout << R"JSON({"type":"turn.completed","usage":{}})JSON" << std::endl;
    return 0;
}
