#include "engine/core/sentence_association_options.h"
#include "tests/includes/test_framework.h"

// 打字闸门每段输入只翻效果档的模型路径：那种变化只影响 series_cache_，词典层只清整句那层。
// 其余任何字段都会牵动词格 LM、切分歧义仲裁或搭配打分，必须走全量 reset_cache；判据就是这里。
TEST_CASE(sentence_association_only_an_effect_tier_model_path_is_series_only)
{
    SentenceAssociationOptions configured;
    configured.word_lattice = true;
    configured.google = true;
    configured.neural_desktop = true;
    configured.neural_desktop_model = "C:/models/desktop.safetensors";
    configured.language_model = "C:/models/sc.lm";
    configured.collocation_model = "C:/models/collocation.gram";

    SentenceAssociationOptions suppressed = configured;
    suppressed.neural_desktop_model.clear();
    REQUIRE(suppressed.differs_only_in_effect_tier_model(configured));
    REQUIRE(configured.differs_only_in_effect_tier_model(suppressed));

    // 完全相同的选项不属于「只变路径」：调用方应先走自己的相等早退，而不是清缓存。
    const SentenceAssociationOptions same = configured;
    REQUIRE(!same.differs_only_in_effect_tier_model(configured));

    // 开关本身翻转会改 any_sentence_source()，绝不能当成 series-only。
    SentenceAssociationOptions switch_flipped = suppressed;
    switch_flipped.neural_desktop = false;
    REQUIRE(!switch_flipped.differs_only_in_effect_tier_model(configured));

    // 其余整句来源、词格 LM 路径与语法参数同理。
    SentenceAssociationOptions google_flipped = suppressed;
    google_flipped.google = false;
    REQUIRE(!google_flipped.differs_only_in_effect_tier_model(configured));

    SentenceAssociationOptions language_model_flipped = suppressed;
    language_model_flipped.language_model.clear();
    REQUIRE(!language_model_flipped.differs_only_in_effect_tier_model(configured));

    SentenceAssociationOptions grammar_flipped = suppressed;
    grammar_flipped.collocation_grammar.collocation_penalty += 1.0;
    REQUIRE(!grammar_flipped.differs_only_in_effect_tier_model(configured));
}
