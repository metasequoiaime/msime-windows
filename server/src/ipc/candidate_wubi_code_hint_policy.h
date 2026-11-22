#pragma once

#include "engine/core/scheme_type.h"
#include "engine/core/word_item.h"

namespace FanyImeIpc
{
// 五笔码注解：五笔会话里，把拼音打出来的那些候选标上该词的五笔全码，方便学五笔。
// 组页时读一次上下文、循环里逐行判定；判定放在这里是为了不依赖 IPC 全局状态就能自动
// 验证（见 tests/src/test_candidate_wubi_code_hint_policy.cpp），生产调用点只有
// event_listener_candidates.cpp 的 BuildCurrentCandidatePage()。
struct WubiCodeHintContext
{
    SchemeType session_scheme = SchemeType::Quanpin;
    bool mixed_pinyin = false;
    bool hint_enabled = false;
    // 下面四条是「这个页的候选谈不上五笔打法」的语境：UILess 宿主自绘候选窗、不读注解；
    // 拆字页与译文页的候选不是拼音打出来的词；候选子页同理。
    bool ui_less_mode = false;
    bool sub_page_active = false;
    bool radical_page = false;
    bool translation_page = false;
};

// 只限五笔会话：拼音、双拼会话没有「这个词的五笔打法」这个概念。
inline bool WubiCodeHintEnabled(const WubiCodeHintContext &context)
{
    return !context.radical_page && !context.translation_page && !context.ui_less_mode && !context.sub_page_active &&
           context.session_scheme == SchemeType::Wubi && context.mixed_pinyin && context.hint_enabled;
}

// 合成候选的 word 不是词库里的词：反查要么查不到，要么撞上一个同形的无关词。辅助码注解也靠
// 这条名单排除，两处共用一份，别各写一排。
inline bool IsSynthesizedCandidateSource(CandidateSource source)
{
    return source == CandidateSource::EnglishDictionary || source == CandidateSource::QuickPhrase ||
           source == CandidateSource::Emoji || source == CandidateSource::Kaomoji ||
           source == CandidateSource::Symbol || source == CandidateSource::DateTime ||
           source == CandidateSource::Generated;
}

// 五笔码表命中的那行，码就是用户刚敲进去的串，再标一遍是噪声；只标拼音侧的行。只有五笔码表
// 会把行标成 SchemeType::Wubi，反过来就用它区分两侧。
inline bool WubiCodeHintAnnotatesRow(const WubiCodeHintContext &context, CandidateSource source, SchemeType row_scheme)
{
    return WubiCodeHintEnabled(context) && !IsSynthesizedCandidateSource(source) && row_scheme != SchemeType::Wubi;
}
} // namespace FanyImeIpc
