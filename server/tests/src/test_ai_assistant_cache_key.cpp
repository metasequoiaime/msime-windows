#include "ai/ai_assistant_cache_key.h"
#include "tests/includes/test_framework.h"

namespace
{
AiAssistant::Request MakeRequest()
{
    AiAssistant::Request request;
    request.pinyin_segments = {"shang", "hai"};
    request.context = "我准备去";
    request.config.provider = "deepseek";
    request.config.endpoint = "https://api.example.com/chat/completions";
    request.config.model = "example-model";
    request.config.prompt = "Return the best candidate.";
    request.config.candidate_limit = 3;
    return request;
}
} // namespace

TEST_CASE(ai_suggestion_cache_hits_for_the_same_semantic_request)
{
    const auto request = MakeRequest();
    const auto same_request = request;

    REQUIRE(AiAssistant::detail::BuildSuggestionCacheKey(request) ==
            AiAssistant::detail::BuildSuggestionCacheKey(same_request));
}

TEST_CASE(ai_suggestion_cache_misses_when_context_changes)
{
    const auto request = MakeRequest();
    auto changed_request = request;
    changed_request.context = "他受到了严重";

    REQUIRE(AiAssistant::detail::BuildSuggestionCacheKey(request) !=
            AiAssistant::detail::BuildSuggestionCacheKey(changed_request));
}

TEST_CASE(ai_suggestion_cache_misses_when_prompt_changes)
{
    const auto request = MakeRequest();
    auto changed_request = request;
    changed_request.config.prompt = "Prefer place names.";

    REQUIRE(AiAssistant::detail::BuildSuggestionCacheKey(request) !=
            AiAssistant::detail::BuildSuggestionCacheKey(changed_request));
}

TEST_CASE(ai_suggestion_cache_key_tolerates_invalid_utf8)
{
    auto request = MakeRequest();
    request.context = std::string("\xE6\x88", 2);
    request.config.prompt = std::string("\xFF", 1);

    REQUIRE(!AiAssistant::detail::BuildSuggestionCacheKey(request).empty());
}

TEST_CASE(ai_suggestion_cache_misses_when_candidate_limit_changes)
{
    const auto request = MakeRequest();
    auto changed_request = request;
    changed_request.config.candidate_limit = 1;

    REQUIRE(AiAssistant::detail::BuildSuggestionCacheKey(request) !=
            AiAssistant::detail::BuildSuggestionCacheKey(changed_request));
}

TEST_CASE(ai_suggestion_cache_misses_when_codex_executable_changes)
{
    auto request = MakeRequest();
    request.config.provider = "codex";
    auto changed = request;
    changed.config.codex_executable = "different-codex.exe";
    REQUIRE(AiAssistant::detail::BuildSuggestionCacheKey(request) !=
            AiAssistant::detail::BuildSuggestionCacheKey(changed));
}
