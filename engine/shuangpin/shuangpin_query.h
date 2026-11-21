#pragma once

#include "shuangpin_profile.h"
#include "../contracts/mid_sentence_helpcode.h"
#include "../core/syllable_helpcode.h"
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace shuangpin
{

std::string segment_input(const std::string &raw_input, const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
// Raw offsets where one shuangpin syllable starts, always including 0 (when
// non-empty) and raw_input.size(). A unit is one syllable: 1-2 keys decided by
// the same forward-greedy acceptance pinyin_segmentation uses, so the Microsoft
// ';' final and the profile's syllable set are honored. Manual delimiters start
// a new chunk. Empty input yields an empty vector.
std::vector<std::size_t> segment_raw_boundaries(const std::string &raw_input,
                                                const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
std::string to_quanpin_segmentation(const std::string &segmented_input,
                                    const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
std::string normalize_input(const std::string &raw_input,
                            const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
std::string normalize_input_with_delimiters(const std::string &raw_input,
                                            const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
// 本地模式（U 拆字、S 符号、T 日期时间）的编码只由完整音节连成，按双拼从头每两键严格解成一个全拼
// 音节，不走 pinyin_segmentation 的「两键不成音节就退一键」——那条退路会把一对打错的键解成声母，
// 查出不相干的结果。返回这串码可能的全拼读法：偶数键时只有一种；末尾落单的一键是下一个音节的开头，
// 按声母键解（小鹤 v → zh），它若还是零声母音节的引导键（微软 o、国标 a），再补上那些音节可能的
// 首字母（排好序）。任何一对解不出音节、落单的是 ; 或码为空时返回空。
std::vector<std::string> decode_syllable_pairs(const std::string &code,
                                               const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
std::string remove_manual_delimiters(const std::string &text);
size_t effective_input_length(const std::string &raw_input);
// Maps a length measured in delimiter-stripped space back onto the raw input, so callers can slice a raw prefix that
// holds exactly that many effective characters.
size_t raw_length_for_effective_prefix(const std::string &raw_input, size_t effective_length);
// Drops the trailing `letter_count` effective characters while keeping every delimiter that precedes them.
std::string trim_trailing_letters_preserve_delimiters(const std::string &raw_input, size_t letter_count);
size_t detect_active_double_helpcode_length(const std::string &raw_input, const std::string &raw_input_with_cases,
                                            const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
bool is_complete_input(const std::string &raw_input, const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
std::string apply_segmentation_cases(const std::string &segmented_input, const std::string &raw_input_with_cases);
std::string get_first_han_char(const std::string &words);
std::string get_last_han_char(const std::string &words);
std::string::size_type count_utf8_chars(const std::string &text);
std::string::size_type count_han_chars(const std::string &text);

// 句中辅助码，规则见 core/syllable_helpcode.h，输入形状与 TSF 共用 contracts/mid_sentence_helpcode.h。
inline constexpr char kMidSentenceHelpcodeMarker = FanyImeMidSentenceHelpcode::kMarker;
// 末尾辅助码（间接）一段的开头，见 FanyImeMidSentenceHelpcode::kEndMarker。
inline constexpr char kEndHelpcodeMarker = FanyImeMidSentenceHelpcode::kEndMarker;
using HelpcodeUppercaseTriggers = FanyImeMidSentenceHelpcode::UppercaseTriggers;

struct MidSentenceHelpcodeInput
{
    // 每段换成一个 ' 之后的输入串，保留大小写。下游把它当作带手动分隔的普通双拼。位于末尾的末尾段
    // 直接去掉，后面没有音节要隔开。
    std::string input;
    // input[i] 在原串里的下标，末尾多一项等于原串长度。
    std::vector<std::size_t> source_index;
    SyllableHelpcodes helpcodes;
    // 每段原样的文本（含还没敲码的光杆反引号）及它挂在哪个音节后面，预编辑据此还原显示。
    std::vector<std::pair<std::size_t, std::string>> decorations;
    // 位于输入串末尾的末尾段敲下的辅码（小写，按敲的顺序，至多两个）。不在末尾的末尾段不筛。
    std::string end_helpcodes;
};

// triggers：哪种大写字母开段（句中辅助码或末尾辅助码的大写触发），规则见
// FanyImeMidSentenceHelpcode::StartsUppercaseBlock。返回串里有没有任何一段（句中段或末尾段）。
bool has_mid_sentence_helpcode(const std::string &raw_input, HelpcodeUppercaseTriggers triggers = {});
MidSentenceHelpcodeInput parse_mid_sentence_helpcodes(const std::string &raw_input_with_cases,
                                                      const ShuangpinProfile &profile = GetXiaoheShuangpinProfile(),
                                                      HelpcodeUppercaseTriggers triggers = {});
// 把每段反引号段按原样接回切分串里对应音节的后面：ul'pb'ih'fa → ul'pb`x'ih'fa。切分串必须是
// 由 parse_mid_sentence_helpcodes(...).input 切出来的（音节序号才对得上）。
std::string decorate_mid_sentence_segmentation(const std::string &segmentation, const std::string &raw_input_with_cases,
                                               const ShuangpinProfile &profile = GetXiaoheShuangpinProfile(),
                                               HelpcodeUppercaseTriggers triggers = {});
// 同上，段文本已经在手：直接辅助码由解析器给出（原串里没有反引号可解析）。
std::string decorate_segmentation(const std::string &segmentation,
                                  const std::vector<std::pair<std::size_t, std::string>> &decorations);
// 输入串末尾能否接一个反引号，即 FanyImeMidSentenceHelpcode::AcceptsMarker。
bool accepts_mid_sentence_helpcode_marker(const std::string &raw_input, HelpcodeUppercaseTriggers triggers = {});
// 光标停在 raw_input[caret] 时能否插入一个反引号，即 FanyImeMidSentenceHelpcode::AcceptsMarkerAt。
bool accepts_mid_sentence_helpcode_marker_at(const std::string &raw_input, std::size_t caret,
                                             HelpcodeUppercaseTriggers triggers = {});
// 输入串末尾能否接 ch 作为第二码：紧跟在只有第一码的段（「反引号 + 第一码」或大写触发的一码）之后的
// 大写字母，即 FanyImeMidSentenceHelpcode::AcceptsSecondCodeAt。
bool accepts_mid_sentence_second_code(const std::string &raw_input, char ch, HelpcodeUppercaseTriggers triggers = {});

} // namespace shuangpin
