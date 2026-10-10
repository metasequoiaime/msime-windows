#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace CodexCli
{
struct Request
{
    std::string executable = "codex";
    std::string model;
    std::string prompt;
    std::chrono::milliseconds timeout{15000};
    // nullopt inherits the environment; empty forces direct; a URL overrides child proxy variables.
    std::optional<std::string> proxy;
};

struct Result
{
    bool ok = false;
    std::string output;
    // A fixed diagnostic, never CLI stderr, input text, or authentication material.
    std::string error;
};

Result Run(const Request &request, const std::function<bool()> &cancelled = {});

namespace detail
{
std::wstring QuoteArgument(const std::wstring &argument);
std::filesystem::path ResolveExecutable(const std::string &configured);
std::vector<std::wstring> Arguments(const std::filesystem::path &schema, const std::string &model);
// Fail closed on tool items, malformed events, or failed turns. Final messages remain JSON text.
bool ConsumeEvent(const std::string &line, std::string &last_message, bool &completed);
} // namespace detail
} // namespace CodexCli
