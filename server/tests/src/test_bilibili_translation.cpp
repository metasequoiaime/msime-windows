#include "tests/includes/test_framework.h"

#include "cloud/bilibili_translation.h"

TEST_CASE(BilibiliTranslationDefaultsToTwoBillionModel)
{
    REQUIRE_EQ(std::string(BilibiliTranslation::kDefaultModel), std::string("Index-Translate-2B"));
    REQUIRE_EQ(std::string(BilibiliTranslation::Models().front().id), std::string("Index-Translate-2B"));
    REQUIRE_EQ(BilibiliTranslation::Config{}.model, std::string("Index-Translate-2B"));
}

TEST_CASE(BilibiliTranslationAcceptsOnlyListedModels)
{
    REQUIRE(BilibiliTranslation::IsSupportedModel("Index-Translate-2B"));
    REQUIRE(BilibiliTranslation::IsSupportedModel("Index-Translate-9B"));
    REQUIRE(BilibiliTranslation::IsSupportedModel("Index-Translate-35B-A3B"));
    REQUIRE(!BilibiliTranslation::IsSupportedModel(""));
    REQUIRE(!BilibiliTranslation::IsSupportedModel("index-translate-2b"));
    REQUIRE_EQ(BilibiliTranslation::NormalizeModel("gpt-4o"), std::string("Index-Translate-2B"));
    REQUIRE_EQ(BilibiliTranslation::NormalizeModel("Index-Translate-9B"), std::string("Index-Translate-9B"));
}

TEST_CASE(BilibiliTranslationBuildsOfficialPrompt)
{
    REQUIRE_EQ(BilibiliTranslation::detail::BuildPrompt("你好", "zh", "en"),
               std::string("请将以下中文文本翻译为英语，直接输出翻译结果，不要进行任何解释。\n\n你好"));
    REQUIRE_EQ(BilibiliTranslation::detail::BuildPrompt("hello", "en", "zh"),
               std::string("请将以下英语文本翻译为中文，直接输出翻译结果，不要进行任何解释。\n\nhello"));
}

TEST_CASE(BilibiliTranslationRequestDisablesThinking)
{
    const auto body = BilibiliTranslation::detail::BuildRequestBody({"Index-Translate-9B"}, "你好", "zh", "ja");
    REQUIRE_EQ(body.at("model").get<std::string>(), std::string("Index-Translate-9B"));
    REQUIRE(!body.at("stream").get<bool>());
    REQUIRE_EQ(body.at("temperature").get<double>(), 0.0);
    REQUIRE(!body.at("chat_template_kwargs").at("enable_thinking").get<bool>());
    REQUIRE_EQ(body.at("messages").size(), size_t{1});
    REQUIRE_EQ(body.at("messages")[0].at("role").get<std::string>(), std::string("user"));
    REQUIRE(body.at("messages")[0].at("content").get<std::string>().find("翻译为日语") != std::string::npos);

    // 配置里写坏的模型名不会原样发出去。
    const auto fallback = BilibiliTranslation::detail::BuildRequestBody({"bogus"}, "x", "en", "zh");
    REQUIRE_EQ(fallback.at("model").get<std::string>(), std::string("Index-Translate-2B"));
}
