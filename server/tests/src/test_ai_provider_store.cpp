// The provider registry is the HanaAgent-style replacement for the five hard-coded provider
// slots: any number of OpenAI-compatible services, persisted in the data directory. These tests
// stay off the real data directory by passing explicit paths.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ai/ai_provider_store.h"
#include "config/ime_config.h"
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include <filesystem>
#include <fstream>

namespace
{
std::filesystem::path TempStorePath()
{
    const auto base = test::TempRoot();
    static int counter = 0;
    return base / ("ai-provider-store-test-" + std::to_string(++counter) + ".json");
}
} // namespace

TEST_CASE(ai_provider_store_round_trips_entries)
{
    const auto path = TempStorePath();
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // Absent file reads as nullopt so the caller knows to seed from the legacy slots.
    const auto before = AiAssistant::ProviderStore::Load(path);
    REQUIRE(!before.has_value());

    AiAssistant::AiProvider minimax;
    minimax.id = "minimax";
    minimax.name = "MiniMax";
    minimax.endpoint = "https://api.minimax.cn/v1/chat/completions";
    minimax.token = "sk-test";
    minimax.model = "MiniMax-M3.1-Flash-Preview";
    AiAssistant::AiProvider deepseek;
    deepseek.id = "deepseek";
    deepseek.name = "DeepSeek";
    deepseek.endpoint = "https://api.deepseek.com/chat/completions";
    REQUIRE(AiAssistant::ProviderStore::Save({deepseek, minimax}, path));

    const auto loaded = AiAssistant::ProviderStore::Load(path);
    REQUIRE(loaded.has_value());
    REQUIRE_EQ(loaded->size(), static_cast<size_t>(2));
    REQUIRE_EQ((*loaded)[0].id, std::string("deepseek"));
    REQUIRE_EQ((*loaded)[1].endpoint, std::string("https://api.minimax.cn/v1/chat/completions"));
    REQUIRE_EQ((*loaded)[1].model, std::string("MiniMax-M3.1-Flash-Preview"));
    // Token 留在 config.toml 里由 DPAPI 保护，绝不明文落进这个文件。
    REQUIRE((*loaded)[1].token.empty());
    std::ifstream input(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    REQUIRE(text.find("sk-test") == std::string::npos);
    input.close();
    std::filesystem::remove(path, ec);
}

TEST_CASE(ai_provider_store_scrubs_tokens_written_by_older_builds)
{
    const auto path = TempStorePath();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    {
        std::ofstream output(path, std::ios::binary);
        output
            << R"({"version":1,"providers":[{"id":"relay","name":"Relay","endpoint":"https://relay.example.test/v1",)"
               R"("token":"sk-plain","model":"m"}]})";
    }
    const auto loaded = AiAssistant::ProviderStore::Load(path);
    REQUIRE(loaded.has_value());
    REQUIRE_EQ(loaded->size(), static_cast<size_t>(1));
    REQUIRE((*loaded)[0].token.empty());
    std::ifstream input(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    REQUIRE(text.find("sk-plain") == std::string::npos);
    REQUIRE(text.find("relay.example.test") != std::string::npos);
    input.close();
    std::filesystem::remove(path, ec);
}

TEST_CASE(ai_provider_store_tolerates_a_corrupt_file)
{
    const auto path = TempStorePath();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    {
        std::ofstream output(path, std::ios::binary);
        output << "{ this is not json";
    }
    // Corrupt reads as absent: the next Save() rewrites the file wholesale.
    const auto loaded = AiAssistant::ProviderStore::Load(path);
    REQUIRE(!loaded.has_value());
    std::filesystem::remove(path, ec);
}

TEST_CASE(ai_provider_store_seeds_every_builtin_provider)
{
    const auto seeded = AiAssistant::ProviderStore::SeedBuiltins();
    const auto &ids = AiAssistant::ProviderStore::BuiltinProviderIds();
    REQUIRE_EQ(seeded.size(), ids.size());
    REQUIRE_EQ(seeded.front().id, std::string("deepseek"));
    REQUIRE(AiAssistant::ProviderStore::IsBuiltinProvider("openrouter"));
    REQUIRE(!AiAssistant::ProviderStore::IsBuiltinProvider("my-relay"));
}

TEST_CASE(ai_provider_store_resolves_the_active_entry)
{
    AiAssistant::ProviderStore::Entries entries;
    AiAssistant::AiProvider relay;
    relay.id = "my-relay";
    relay.endpoint = "https://relay.example.test/v1/chat/completions";
    relay.model = "relay-model";
    entries.push_back(relay);

    AiAssistant::AiProvider active;
    std::string endpoint = "https://api.deepseek.com/chat/completions";
    std::string model = "deepseek-v4-flash";
    AiAssistant::ProviderStore::ResolveActive(active, endpoint, model, "my-relay", entries);
    REQUIRE_EQ(endpoint, std::string("https://relay.example.test/v1/chat/completions"));
    REQUIRE_EQ(model, std::string("relay-model"));
    REQUIRE_EQ(active.id, std::string("my-relay"));

    // 未知 id：一切保持调用方的值（回退到 config.toml 的旧字段）。
    AiAssistant::ProviderStore::ResolveActive(active, endpoint, model, "ghost", entries);
    REQUIRE_EQ(endpoint, std::string("https://relay.example.test/v1/chat/completions"));
    REQUIRE(active.id.empty());

    // 内置服务商的值以 config.toml 为准：store 里的旧副本不得把新写入的值盖回去。
    for (const char *builtin : {"deepseek", "minimax"})
    {
        entries.front().id = builtin;
        entries.front().endpoint = "https://stale.example.test/v1/chat/completions";
        std::string fresh_endpoint = "https://fresh.example.test/chat/completions";
        AiAssistant::ProviderStore::ResolveActive(active, fresh_endpoint, model, builtin, entries);
        REQUIRE_EQ(fresh_endpoint, std::string("https://fresh.example.test/chat/completions"));
        REQUIRE_EQ(active.id, std::string(builtin));
    }
}
TEST_CASE(ai_provider_ollama_sends_no_token)
{
    REQUIRE(AiAssistant::ProviderStore::IsBuiltinProvider("ollama"));
    REQUIRE(!AiAssistant::ProviderUsesToken("ollama"));
    for (const auto id : AiAssistant::ProviderStore::BuiltinProviderIds())
        REQUIRE_EQ(AiAssistant::ProviderUsesToken(id), id != "ollama");

    REQUIRE(AiAssistant::RequestToken("ollama", "sk-left-over").empty());
    REQUIRE_EQ(AiAssistant::RequestToken("deepseek", "sk-test"), std::string("sk-test"));

    // 没有 Token 就不带 Authorization 头，而不是发一个空的 Bearer。
    REQUIRE(AiAssistant::AuthorizationHeader("").empty());
    REQUIRE_EQ(AiAssistant::AuthorizationHeader("sk-test"), std::string("Authorization: Bearer sk-test"));
}

TEST_CASE(ai_provider_store_defaults_match_the_config_struct)
{
    // 页面占位符、空槽位回退和配置结构体的初始值必须来自同一份默认值。三处任意一处
    // 改了忘改另一处，用户就会看到「占位符写着 A、实际请求发的是 B」。
    const AiAssistantConfig config;
    for (const char *id : {"deepseek", "openai", "siliconflow", "groq", "minimax", "moonshot", "zhipu", "dashscope",
                           "volcengine", "openrouter", "ollama", "jev"})
    {
        REQUIRE_EQ(config.endpoints.at(id), AiAssistant::ProviderStore::BuiltinDefaultEndpoint(id));
        REQUIRE_EQ(config.models.at(id), AiAssistant::ProviderStore::BuiltinDefaultModel(id));
    }
    // custom 不预设地址与模型，全部由用户填写。
    REQUIRE_EQ(AiAssistant::ProviderStore::BuiltinDefaultEndpoint("custom"), std::string());
    REQUIRE_EQ(AiAssistant::ProviderStore::BuiltinDefaultModel("custom"), std::string());
    REQUIRE_EQ(AiAssistant::ProviderStore::BuiltinDefaultModel("ghost"), std::string());
}
