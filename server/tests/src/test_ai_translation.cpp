#include "tests/includes/test_framework.h"

#include "cloud/ai_translation.h"

TEST_CASE(AiTranslationValidatesConfig)
{
    AiTranslation::Config valid;
    valid.provider = "deepseek";
    valid.endpoint = "https://api.deepseek.com/chat/completions";
    valid.token = "sk-test";
    valid.model = "deepseek-chat";
    REQUIRE(AiTranslation::IsUsableConfig(valid));

    AiTranslation::Config missing_token = valid;
    missing_token.token = "";
    REQUIRE(!AiTranslation::IsUsableConfig(missing_token));

    AiTranslation::Config missing_endpoint = valid;
    missing_endpoint.endpoint = "";
    REQUIRE(!AiTranslation::IsUsableConfig(missing_endpoint));

    AiTranslation::Config missing_model = valid;
    missing_model.model = "";
    REQUIRE(!AiTranslation::IsUsableConfig(missing_model));
}

TEST_CASE(AiTranslationParsesJsonResponses)
{
    // 标准 JSON 对象包含 translations 数组
    const std::string standard = R"({"translations":["hello","world"]})";
    const auto r1 = AiTranslation::ParseTranslations(standard, 2);
    REQUIRE_EQ(r1.size(), 2u);
    REQUIRE_EQ(r1[0], std::string("hello"));
    REQUIRE_EQ(r1[1], std::string("world"));

    // 纯 JSON 数组
    const std::string raw_array = R"(["bonjour","monde"])";
    const auto r2 = AiTranslation::ParseTranslations(raw_array, 2);
    REQUIRE_EQ(r2.size(), 2u);
    REQUIRE_EQ(r2[0], std::string("bonjour"));
    REQUIRE_EQ(r2[1], std::string("monde"));

    // Markdown 代码块包裹
    const std::string fenced = "```json\n{\"translations\":[\"hola\"]}\n```";
    const auto r3 = AiTranslation::ParseTranslations(fenced, 1);
    REQUIRE_EQ(r3.size(), 1u);
    REQUIRE_EQ(r3[0], std::string("hola"));

    // 格式损坏或空响应安全返回空数组
    const auto r_empty = AiTranslation::ParseTranslations("not a json", 2);
    REQUIRE_EQ(r_empty.size(), 2u);
    REQUIRE(r_empty[0].empty());
    REQUIRE(r_empty[1].empty());
}
