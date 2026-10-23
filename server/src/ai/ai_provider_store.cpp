#include "ai_provider_store.h"

#include "engine/core/data_path.h"

#include <nlohmann/json.hpp>
#include <process.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <utility>

namespace AiAssistant::ProviderStore
{
namespace
{
constexpr int kSchemaVersion = 1;
} // namespace

const std::vector<std::string_view> &BuiltinProviderIds()
{
    static const std::vector<std::string_view> ids{"deepseek",   "openai",   "siliconflow", "groq",      "custom",
                                                   "minimax",    "moonshot", "zhipu",       "dashscope", "volcengine",
                                                   "openrouter", "jev",      "ollama"};
    return ids;
}

bool IsBuiltinProvider(std::string_view id)
{
    const auto &ids = BuiltinProviderIds();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

namespace
{
const std::map<std::string_view, std::pair<std::string_view, std::string_view>> &BuiltinDefaults()
{
    // endpoint, model。custom 不预设，全部由用户填写。
    static const std::map<std::string_view, std::pair<std::string_view, std::string_view>> defaults{
        {"deepseek", {"https://api.deepseek.com/chat/completions", "deepseek-v4-flash"}},
        {"openai", {"https://api.openai.com/v1/chat/completions", "gpt-4o-mini"}},
        {"siliconflow", {"https://api.siliconflow.cn/v1/chat/completions", "Qwen/Qwen3-8B"}},
        {"groq", {"https://api.groq.com/openai/v1/chat/completions", "qwen/qwen3.8-27b"}},
        {"minimax", {"https://api.minimax.cn/v1/chat/completions", "MiniMax-M3.1-Flash-Preview"}},
        {"moonshot", {"https://api.moonshot.cn/v1/chat/completions", "kimi-k2-0905-preview"}},
        {"zhipu", {"https://open.bigmodel.cn/api/paas/v4/chat/completions", "glm-4-flash"}},
        {"dashscope", {"https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions", "qwen-plus"}},
        {"volcengine", {"https://ark.cn-beijing.volces.com/api/v3/chat/completions", "doubao-seed-1.6-flash"}},
        {"openrouter", {"https://openrouter.ai/api/v1/chat/completions", "openrouter/auto"}},
        // 本机的 Ollama 服务，不做鉴权。装了哪些模型因人而异，不预设模型，用「查询模型」从 /v1/models 列出。
        {"ollama", {"http://localhost:11434/v1/chat/completions", ""}},
        {"jev", {"https://api.typesafe.ai/v1/systemone", "jev-1.13.0"}},
    };
    return defaults;
}
} // namespace

std::string BuiltinDefaultEndpoint(std::string_view id)
{
    const auto found = BuiltinDefaults().find(id);
    return found == BuiltinDefaults().end() ? std::string() : std::string(found->second.first);
}

std::string BuiltinDefaultModel(std::string_view id)
{
    const auto found = BuiltinDefaults().find(id);
    return found == BuiltinDefaults().end() ? std::string() : std::string(found->second.second);
}

std::filesystem::path StorePath()
{
    return metasequoia::data_directory() / "ai_providers.json";
}

std::optional<Entries> Load(const std::filesystem::path &explicit_path)
{
    const std::filesystem::path path = explicit_path.empty() ? StorePath() : explicit_path;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    const auto root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return std::nullopt;
    Entries entries;
    bool holds_token = false;
    if (root.contains("providers") && root.at("providers").is_array())
    {
        for (const auto &item : root.at("providers"))
        {
            if (!item.is_object())
                continue;
            AiProvider entry;
            const auto read_string = [&item](const char *key, std::string &out) {
                if (item.contains(key) && item.at(key).is_string())
                    out = item.at(key).get<std::string>();
            };
            read_string("id", entry.id);
            read_string("name", entry.name);
            read_string("endpoint", entry.endpoint);
            read_string("model", entry.model);
            holds_token = holds_token || item.contains("token");
            if (!entry.id.empty())
                entries.push_back(std::move(entry));
        }
    }
    // Early builds of the registry wrote tokens here in plaintext. The same values were also
    // written to config.toml, so rewriting the file without them loses nothing.
    if (holds_token)
        Save(entries, path);
    return entries;
}

bool Save(const Entries &entries, const std::filesystem::path &explicit_path)
{
    const std::filesystem::path path = explicit_path.empty() ? StorePath() : explicit_path;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    nlohmann::json root = {{"version", kSchemaVersion}, {"providers", nlohmann::json::array()}};
    for (const auto &entry : entries)
    {
        root.at("providers")
            .push_back({{"id", entry.id}, {"name", entry.name}, {"endpoint", entry.endpoint}, {"model", entry.model}});
    }
    // Server and the settings process can both save (or seed) at once; a per-process tmp name
    // keeps them from truncating each other's half-written file.
    std::filesystem::path tmp = path;
    tmp += "." + std::to_string(_getpid()) + ".tmp";
    {
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;
        const std::string text = root.dump();
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output)
        {
            output.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::error_code rename_ec;
    std::filesystem::rename(tmp, path, rename_ec);
    if (rename_ec)
    {
        // rename onto an existing file can fail on some filesystems; replace() handles it.
        std::error_code replace_ec;
        std::filesystem::remove(path, replace_ec);
        std::filesystem::rename(tmp, path, replace_ec);
        return !replace_ec;
    }
    return true;
}

Entries SeedBuiltins()
{
    Entries entries;
    for (const auto id : BuiltinProviderIds())
    {
        AiProvider entry;
        entry.id = std::string(id);
        // The settings page shows its own display name for built-in ids.
        entry.name = entry.id;
        entries.push_back(std::move(entry));
    }
    return entries;
}

void ResolveActive(AiProvider &active_out, std::string &endpoint, std::string &model, const std::string &provider,
                   const Entries &entries)
{
    active_out = AiProvider{};
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [&provider](const AiProvider &entry) { return entry.id == provider; });
    if (found == entries.end())
        return;
    active_out = *found;
    // Only registry-only providers resolve through the store. Built-in ids keep config.toml
    // authoritative - the settings page still writes endpoint/model there, and treating a
    // possibly-stale store copy as the winner would pin old values over new ones.
    if (IsBuiltinProvider(provider))
        return;
    if (!found->endpoint.empty())
        endpoint = found->endpoint;
    if (!found->model.empty())
        model = found->model;
}
} // namespace AiAssistant::ProviderStore
