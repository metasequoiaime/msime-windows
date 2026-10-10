#include "tests/includes/test_framework.h"
#include "webview2/candidate_window_template.h"

TEST_CASE(candidate_window_template_renders_ninth_candidate)
{
    const std::wstring templ = L"<!--0Anchor-->{0}<!--1Anchor-->{1}<!--8Anchor-->{8}<!--9Anchor-->{9}";
    const std::vector<std::wstring> words = {L"preedit", L"one", L"two",   L"three", L"four",
                                             L"five",    L"six", L"seven", L"eight", L"nine"};

    const std::wstring result = InflateCandidateTemplate(templ, words);

    REQUIRE(result.find(L"nine") != std::wstring::npos);
    REQUIRE(result.find(L"<!--9Anchor-->") != std::wstring::npos);
}

TEST_CASE(candidate_window_template_trims_after_eighth_candidate)
{
    const std::wstring templ = L"<!--0Anchor-->{0}<!--1Anchor-->{1}<!--8Anchor-->{8}<!--9Anchor-->{9}";
    const std::vector<std::wstring> words = {L"preedit", L"one", L"two",   L"three", L"four",
                                             L"five",    L"six", L"seven", L"eight"};

    const std::wstring result = InflateCandidateTemplate(templ, words);

    REQUIRE(result.find(L"eight") != std::wstring::npos);
    REQUIRE(result.find(L"{9}") == std::wstring::npos);
}

TEST_CASE(candidate_window_template_keeps_commas_and_private_use_characters)
{
    const std::wstring templ = L"<!--0Anchor-->{0}<!--1Anchor-->{1}<!--2Anchor-->{2}";
    const std::vector<std::wstring> words = {L"pre,edit", L"a,\uF000", L"b,\uF000"};

    REQUIRE_EQ(InflateCandidateTemplate(templ, words),
               std::wstring(L"<!--0Anchor-->pre,edit<!--1Anchor-->a,\uF000<!--2Anchor-->b,\uF000"));
}

TEST_CASE(candidate_window_template_escapes_preedit_markup_without_changing_commas)
{
    REQUIRE_EQ(EscapeCandidateTemplateText(L"<a,&\"\uF000>"), std::wstring(L"&lt;a,&amp;&quot;\uF000&gt;"));
}
