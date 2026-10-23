#pragma once

#include "key_event.h"
#include "fuzzy_pinyin_options.h"
#include "scheme_type.h"
#include "sentence_association_options.h"
#include "syllable_helpcode.h"
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

struct KeyStroke
{
    ImeKeyCode vk = 0;
    ImeModifierMask modifiers_down = 0;
    ImeCharacter wch = 0;
};

struct QueryRequest
{
    SchemeType scheme = SchemeType::Quanpin;
    std::string raw_input;
    std::string raw_input_with_cases;
    std::string normalized_input;
    std::string raw_segmentation;
    std::string normalized_segmentation;
    std::string segmentation;
    bool enable_shuangpin_helpcode = false;
    bool enable_quanpin_helpcode = false;
    // 句中辅助码，见 syllable_helpcode.h。输入串里带反引号段时，raw_input / raw_input_with_cases /
    // 各切分都是把每段换成一个 ' 之后的结果，下游按普通手动分隔来切分、推进；原样的输入串放在
    // raw_input_with_syllable_helpcodes，宿主回读输入串、显示预编辑时用它。没有反引号段时两者
    // 都为空。syllable_helpcodes 不受开关影响（开关只管能不能敲进去），由开关决定是否参与筛选。
    bool enable_mid_sentence_helpcode = false;
    std::string raw_input_with_syllable_helpcodes;
    SyllableHelpcodes syllable_helpcodes;
    // 去段串（raw_input_with_cases）到原串（raw_input_with_syllable_helpcodes）的下标映射，末尾多一项
    // 等于原串长度；以及每段原样的辅码文本挂在哪个音节后面。下游推进选词、显示预编辑时直接用它，
    // 不再从原串反推：反推要知道当时的触发规则（大写触发开没开），而直接辅助码的原串里根本没有段
    // 标记，切分是解析器整句解码选出来的。由方案层（反引号段、大写段）或直接辅助码的解析器填写。
    bool has_syllable_helpcode_layout = false;
    std::vector<std::size_t> syllable_helpcode_source_index;
    std::vector<std::pair<std::size_t, std::string>> syllable_helpcode_decorations;
    // 双拼末尾辅助码（间接）：输入串末尾那一段引导键（或大写触发）后面敲的辅码，小写、按敲的顺序，一个
    // 或两个。段本身连同辅码已从 raw_input 等去掉，原样留在 raw_input_with_syllable_helpcodes，显示挂在
    // syllable_helpcode_decorations 里；查询时一码重排、两码筛选，与直接触发的末尾辅助码同一套规则。
    // enable_shuangpin_helpcode 只管直接触发（按输入形状认辅码），与这一项无关。
    std::string end_helpcodes;
    // 会话开着直接辅助码（不论这次请求有没有被改写）。词典层据此让词格与解析器共用一份跨度查询缓存。
    bool enable_direct_helpcode = false;
    // 直接辅助码在句中用什么结束四码：补 /（uiab/），或第二位辅码大写（uiaB），见 direct_helpcode/spelling_graph.h。
    bool direct_helpcode_slash_marker = true;
    bool direct_helpcode_uppercase_marker = false;
    // Autocorrection is type-gated (bit0 transposition, bit1 neighbor, bit2 deletion,
    // bit3 insertion in the session-level mask). One bool per switch, no linkage
    // between them; all default off, so a fresh install never rewrites the user's
    // spelling.
    bool enable_quanpin_autocorrect_transposition = false;
    bool enable_quanpin_autocorrect_neighbor = false;
    bool enable_quanpin_autocorrect_deletion = false;
    bool enable_quanpin_autocorrect_insertion = false;
    std::vector<KeyStroke> key_strokes;
    metasequoia::FuzzyPinyinOptions fuzzy_pinyin;
    // 整句候选来源与去重补位选项，默认全关。
    SentenceAssociationOptions sentence_association;
    // 给神经重排看的前文：本会话最近上屏的文本。空着也能重排，只是模型看不到语境，「shanghai」
    // 到底是上海还是伤害就只能靠词格自己的静态分。词典层只取末尾若干字（RerankOptions::
    // context_chars），所以这里给多了也无妨。
    std::string rescoring_context;
    // Wubi wildcard mode and the code in hand holds a z: the provider matches the code as a
    // pattern (z = one arbitrary letter) instead of a prefix range. Never set for other schemes.
    // With mixed input also on, the same z is offered to pinyin as a plain letter and those
    // candidates are placed ahead of the wildcard rows (see ImeSession::refresh_candidates).
    bool wubi_z_wildcard = false;
    bool valid = false;
};
