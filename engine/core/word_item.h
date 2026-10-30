#pragma once

#include "scheme_type.h"
#include <string>
#include <cstdint>
#include <utility>

enum class CandidateSource
{
    Database,
    UserDatabase,
    CloudSuggestion,
    AiSuggestion,
    EnglishDictionary,
    QuickPhrase,
    Emoji,
    Kaomoji,
    Generated,
    Fallback,
    // 神经整句重排挑中的那一条：句子本身还是词格解出来的 n-best，chinese-ime-lm 的字符级
    // Transformer 只负责在里面挑最准的一句，挑中的那行记在这里，词格自己的第一名仍记
    // Generated。重排器弃权或后台还没算完时不会出现这两个来源。desktop 档更准更慢，
    // keyboard 档更快。两者都属于「猜出来的整句」，落库/学习判定与 Generated/Fallback 同类。
    NeuralDesktop,
    NeuralKeyboard,
    // 全拼/双拼里打 rq / sj / xq 等唤醒词时混进普通候选的日期、时间、星期。Shift+T 模式里的同一批
    // 文本仍记 Generated：那是独占的候选列表，不和别的候选排位置。
    DateTime,
    // 万象语法模型（octagram .gram）在词格 n-best 上重排挑中的那一句。句子仍是词格
    // 解出来的，搭配强度表只负责挑；落库/学习判定与 NeuralDesktop/NeuralKeyboard 同类。
    Collocation,
    // Shift+S 符号模式与符号混输查出的符号（others.db 的 symbol_code）。与 emoji 一样直接上屏，
    // 不进词库、不调频。
    Symbol,
};

struct WordItem
{
    // The input code matched by this candidate.  This remains scheme/raw-input
    // oriented because composition advancement must consume exactly what the
    // user typed (including abbreviated pinyin).
    std::string pinyin;
    // The complete quanpin key read from the database.  It is deliberately
    // separate from pinyin: abbreviated quanpin and shuangpin candidates use
    // their typed code for advancement, but phrase creation must persist a
    // complete, unambiguous pronunciation.
    std::string canonical_pinyin;
    std::string word;
    std::int64_t weight = 0;
    CandidateSource source = CandidateSource::Database;
    // 产出该候选的输入方案。五笔拼音混输时同一个组合里既有五笔码表候选又有拼音候选，
    // 调频、删除、固定位置和上屏后的组合推进都必须按候选自己的方案走，不能按会话方案
    // 一刀切。默认 Quanpin：拼音、英文、云、本地模式候选共用拼音侧的读写路径，只有
    // 五笔与日语候选需要显式标成自己的方案。
    SchemeType scheme = SchemeType::Quanpin;
    int fixed_position = 0;
    bool fuzzy = false; // Matched typed code may differ from canonical pronunciation.
    // 非空表示该候选来自纠错解释（scheme 别名层或纠错表改写了输入字母），值为纠错前的
    // 原始输入字母串（对标 librime tips / 搜狗纠错标记）。填充规则见
    // QuanpinDictionary::mark_autocorrect_candidates；前端仅据非空与否附加轻标记。
    std::string corrected_from;
    // 容错槽位只把单字母段展开成同首字母的完整音节、合法音节原样保留时置位。这类候选的
    // corrected_from 描述的是简拼展开，不是拼写纠错，候选窗据此不再附加 * 号。预编辑与
    // 排序仍按 corrected_from 走，不受这一位影响。
    bool jianpin_expansion = false;
    // 只有整句联想（Google 解码器的 Fallback、kenlm 词格的 Generated）产出的那一行才置位。
    // Generated/Fallback 同时被原样上屏、英文、日期、拆字、假名、译文等合成候选借用，
    // 候选窗的〔Trigram〕/〔Unigram〕来源标签只认这个标记，不能单凭 source 判断。
    bool sentence_association = false;
    // 繁体模式下这一项显示与上屏的写法，由 server 展开候选列表时填（见
    // server/src/ipc/traditional_variant_expansion_policy.h），组页与上屏读它。空表示这一项没有
    // 展开，输出文字由 server 按当前字形从 word 得到（简体模式、日语方案、U 模式拆字列表下就是 word）。
    // 引擎不填也不读；word 始终是简体底词，学习与删除都按 word 走。
    std::string output_text;
    // 同一个底词展开出的第几项：0 是这个底词在列表里留下的第一项，大于 0 是紧跟在它后面的其余写法。
    // 由同一处展开填，槽位计数、异步候选插入位置与调频前的过滤据此把一组写法当成一个候选。
    int variant_rank = 0;

    WordItem() = default;
    WordItem(std::string pinyin_value, std::string word_value, std::int64_t weight_value,
             CandidateSource source_value = CandidateSource::Database, std::string canonical_pinyin_value = {})
        : pinyin(std::move(pinyin_value)), canonical_pinyin(std::move(canonical_pinyin_value)),
          word(std::move(word_value)), weight(weight_value), source(source_value)
    {
    }
};
