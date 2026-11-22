#include "ipc/candidate_wubi_code_hint_policy.h"
#include "tests/includes/test_framework.h"

#include <initializer_list>

namespace
{
FanyImeIpc::WubiCodeHintContext WubiSessionWithHintOn()
{
    FanyImeIpc::WubiCodeHintContext context;
    context.session_scheme = SchemeType::Wubi;
    context.mixed_pinyin = true;
    context.hint_enabled = true;
    return context;
}
} // namespace

TEST_CASE(wubi_code_hint_needs_wubi_session_mixed_pinyin_and_switch)
{
    REQUIRE(FanyImeIpc::WubiCodeHintEnabled(WubiSessionWithHintOn()));

    // 拼音、双拼、日语会话没有「这个词的五笔打法」这个概念。
    for (const SchemeType scheme : {SchemeType::Quanpin, SchemeType::Shuangpin, SchemeType::JapaneseRomaji})
    {
        auto context = WubiSessionWithHintOn();
        context.session_scheme = scheme;
        REQUIRE(!FanyImeIpc::WubiCodeHintEnabled(context));
    }

    auto without_mixed = WubiSessionWithHintOn();
    without_mixed.mixed_pinyin = false;
    REQUIRE(!FanyImeIpc::WubiCodeHintEnabled(without_mixed));

    auto switched_off = WubiSessionWithHintOn();
    switched_off.hint_enabled = false;
    REQUIRE(!FanyImeIpc::WubiCodeHintEnabled(switched_off));
}

TEST_CASE(wubi_code_hint_is_suppressed_on_pages_without_pinyin_typed_words)
{
    auto ui_less = WubiSessionWithHintOn();
    ui_less.ui_less_mode = true;
    REQUIRE(!FanyImeIpc::WubiCodeHintEnabled(ui_less));

    auto sub_page = WubiSessionWithHintOn();
    sub_page.sub_page_active = true;
    REQUIRE(!FanyImeIpc::WubiCodeHintEnabled(sub_page));

    auto radical = WubiSessionWithHintOn();
    radical.radical_page = true;
    REQUIRE(!FanyImeIpc::WubiCodeHintEnabled(radical));

    auto translation = WubiSessionWithHintOn();
    translation.translation_page = true;
    REQUIRE(!FanyImeIpc::WubiCodeHintEnabled(translation));
}

TEST_CASE(wubi_code_hint_skips_rows_that_were_not_typed_as_pinyin)
{
    const auto context = WubiSessionWithHintOn();
    REQUIRE(FanyImeIpc::WubiCodeHintAnnotatesRow(context, CandidateSource::Database, SchemeType::Quanpin));
    // 码表自己出的那行，码就是用户刚敲进去的串，再标一遍是噪声。
    REQUIRE(!FanyImeIpc::WubiCodeHintAnnotatesRow(context, CandidateSource::Database, SchemeType::Wubi));

    for (const CandidateSource source :
         {CandidateSource::EnglishDictionary, CandidateSource::QuickPhrase, CandidateSource::Emoji,
          CandidateSource::Kaomoji, CandidateSource::Symbol, CandidateSource::DateTime, CandidateSource::Generated})
    {
        REQUIRE(FanyImeIpc::IsSynthesizedCandidateSource(source));
        // 合成候选的 word 不是词库里的词，反查只会撞上同形的无关词。
        REQUIRE(!FanyImeIpc::WubiCodeHintAnnotatesRow(context, source, SchemeType::Quanpin));
    }

    // 词库出来的行都要标，别把名单写成白名单。
    for (const CandidateSource source :
         {CandidateSource::Database, CandidateSource::UserDatabase, CandidateSource::CloudSuggestion,
          CandidateSource::AiSuggestion, CandidateSource::Fallback, CandidateSource::NeuralDesktop,
          CandidateSource::NeuralKeyboard, CandidateSource::Collocation})
        REQUIRE(!FanyImeIpc::IsSynthesizedCandidateSource(source));
}

TEST_CASE(wubi_code_hint_row_rule_inherits_the_page_gate)
{
    auto switched_off = WubiSessionWithHintOn();
    switched_off.hint_enabled = false;
    REQUIRE(!FanyImeIpc::WubiCodeHintAnnotatesRow(switched_off, CandidateSource::Database, SchemeType::Quanpin));

    auto radical = WubiSessionWithHintOn();
    radical.radical_page = true;
    REQUIRE(!FanyImeIpc::WubiCodeHintAnnotatesRow(radical, CandidateSource::Database, SchemeType::Quanpin));
}
