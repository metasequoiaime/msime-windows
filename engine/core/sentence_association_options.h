#pragma once

#include "../ngram/octagram/octagram_gram.h"

#include <string>

// 整句联想的开关。全部默认关闭，由用户在设置里手动开启。
//
// 整句候选最多四行：词格首选、Google 解码器，以及两个神经模型的首选各一条。
//
// 前两个各是一条独立来源的整句。
//
// 后两个不一样。神经模型不再自己造句，它只在词格内部解出的最多 12 条 n-best 里挑一条（见
// engine/neural/neural_decoder.h）。它们不依赖 word_lattice 的显示开关：只开神经时词格仍会在内部
// 生成备选，但只显示神经选中的句子；同时开 word_lattice 时才额外保留词格首选作对照。两个神经
// 开关都开时各自独立打分，各出一条首选；两边选中相同文本时合并成一条。
struct SentenceAssociationOptions
{
    bool word_lattice = false;             // kenlm 词格整句，CandidateSource::Generated
    bool google = false;                   // Google 解码器整句，CandidateSource::Fallback
    bool neural_desktop = false;           // 用高精度档的神经模型重排词格整句
    bool neural_keyboard = false;          // 用快速档的神经模型重排词格整句
    bool context_candidate_rerank = false; // 实验：用轻量模型按上文重排完整读音的词库候选
    bool show_next_on_duplicate = false;   // 某来源首选被去重时，显示该来源下一条不同结果

    // 高精度档模型不随安装包分发，由宿主按需下载后把 .safetensors 路径（UTF-8）传入；
    // 空 = 模型缺席，neural_desktop 开着也静默不重排，与 collocation_model 的降级范式一致。
    std::string neural_desktop_model;

    // 词格打分的 kenlm 模型（sc.lm）。安装包不带它，宿主在用户下载后把路径（UTF-8）传入；
    // 空 = 用资源目录里的 sc.lm（开发与测试环境放在那里，产品安装里没有），再缺就退回启发式打分。
    std::string language_model;

    // 万象语法模型（octagram .gram）。宿主按模型包解析出 .gram 的路径传入，空 = 关闭；
    // 路径无效时词格静默退回无搭配打分，与其他整句来源的降级范式一致。
    // 加成只在解码期逐边叠加一次（weight 把 query() 换到 log10 域后缩放）；collocation_rerank
    // 只决定是否把解码最优路径作为〔万象〕整句来源行显示出来，不再做二次重排。
    std::string collocation_model;   // .gram 文件路径（UTF-8）
    double collocation_weight = 1.0; // 加成的标定系数（log10 域，1.0 ≈ librime 同相对量级）
    bool collocation_rerank = false; // 显示〔万象〕整句行的开关
    // octagram 六参数，由配置驱动（默认对齐万象现行，见 GrammarConfig）。
    gram::GrammarConfig collocation_grammar;

    // 任一整句来源开着。纠错的上下文消解（quanpin_dictionary.cpp）用它而不是只看
    // word_lattice：只开万象重排或神经模型时词格照样在内部解码，判断用户想打哪个读音
    // 不该跟着「显示不显示 Trigram 整句」这个开关走。
    bool any_sentence_source() const
    {
        return word_lattice || google || neural_desktop || neural_keyboard || collocation_rerank;
    }

    bool operator==(const SentenceAssociationOptions &other) const
    {
        return word_lattice == other.word_lattice && google == other.google && neural_desktop == other.neural_desktop &&
               neural_keyboard == other.neural_keyboard && context_candidate_rerank == other.context_candidate_rerank &&
               show_next_on_duplicate == other.show_next_on_duplicate &&
               neural_desktop_model == other.neural_desktop_model && language_model == other.language_model &&
               collocation_model == other.collocation_model && collocation_weight == other.collocation_weight &&
               collocation_rerank == other.collocation_rerank && collocation_grammar == other.collocation_grammar;
    }
    bool operator!=(const SentenceAssociationOptions &other) const
    {
        return !(*this == other);
    }

    // 只有效果档的模型路径不同。路径只决定词格要不要解 12-best 交给神经重排，影响范围止于
    // series_cache_；其余字段（尤其开关本身）会牵动词格 LM 与切分歧义仲裁，得清整张缓存。
    // 打字闸门的节奏只翻这个路径，见 Server 的 EngineInputSession::ApplyConfiguration。
    bool differs_only_in_effect_tier_model(const SentenceAssociationOptions &other) const
    {
        if (*this == other)
        {
            return false;
        }
        SentenceAssociationOptions mine = *this;
        SentenceAssociationOptions theirs = other;
        mine.neural_desktop_model.clear();
        theirs.neural_desktop_model.clear();
        return mine == theirs;
    }
};
