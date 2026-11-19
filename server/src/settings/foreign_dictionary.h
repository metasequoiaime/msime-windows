#pragma once

// 其他输入法词库格式的读取，只支持 Rime 与 LibIME（fcitx5）两种开源输入法的开放格式；
// 商业输入法的私有格式（搜狗、QQ、百度、Mac 等）有意不支持。
//
// 各格式的解析逻辑移植自「深蓝词库转换」imewlconverter：
//   https://github.com/studyzy/imewlconverter
//   Copyright studyzy and contributors, licensed under the GNU General Public License v3.0.
//
// 这里只负责「文件字节 -> 词条 + 全拼」，不碰词库：校验、去重、写库和用户日志都由
// dictionary_manager.cpp 按拼音批量导入的同一条路走。

#include <optional>
#include <string>
#include <vector>

namespace ForeignDictionary
{
struct Entry
{
    // UTF-8 词面。
    std::string word;
    // 用 ' 分隔的全拼音节（例如 ni'hao）。为空表示文件没给出读音，交给调用方自动注音。
    std::string pinyin;
    // 文件里给的词频，越大越常用；量纲随格式而定（次数、libime 的对数概率……），调用方只该
    // 拿来比高低。文件没写词频的词条为空，按「只有位置」处理。
    std::optional<double> frequency;
};

struct Format
{
    // 协议里用的格式标识，与 imewlconverter 的命令行名称一致（rime、libimetxt、libimebin）。
    const char *id;
    // 下拉框里的名字。
    const wchar_t *label;
    // 打开文件对话框的过滤器。
    const wchar_t *filterName;
    const wchar_t *filterPattern;
};

// 下拉框的选项顺序就是这里的顺序。
const std::vector<Format> &Formats();
const Format *FindFormat(const std::string &id);

// 解析整份文件。整份文件无法识别（格式不对、文件损坏）时返回 false 并在 error 里说明；
// 能识别但没有词条时返回 true 和空列表。
bool Parse(const std::string &format, const std::string &bytes, std::vector<Entry> &entries, std::string &error);
} // namespace ForeignDictionary
