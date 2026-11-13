#include "radical_query.h"

#include "../core/data_path.h"
#include "../shuangpin/shuangpin_query.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace metasequoia::local_modes
{
namespace
{
// 一行拆字数据。code 是部件拼音连写、word 是汉字、weight 是字频。两段文字首尾相接存在
// Table::text 里：十几万行各带两个 std::string 要占十多 MB 常驻内存，压成一块文本池后只剩几 MB。
struct Entry
{
    std::uint32_t offset = 0;
    std::uint16_t code_size = 0;
    std::uint16_t word_size = 0;
    std::int64_t weight = 0;
};

struct Table
{
    std::string text;
    // 文件顺序即字频降序（生成脚本已排序），所以某前缀的首个命中就是它最高频的那条。
    std::vector<Entry> entries;
    // entries 的下标按 code 排序。前缀命中的行在排序视图里连续，查询只扫这一小段。
    std::vector<std::uint32_t> by_code;

    std::string_view code(std::size_t index) const
    {
        const Entry &entry = entries[index];
        return std::string_view(text).substr(entry.offset, entry.code_size);
    }

    std::string_view word(std::size_t index) const
    {
        const Entry &entry = entries[index];
        return std::string_view(text).substr(entry.offset + entry.code_size, entry.word_size);
    }
};

// 生成脚本不会写 BOM，但保留与 helpcode 相同的防御：表由外部工具产出，首行多一个 BOM
// 不该让整张表失效。
constexpr char kUtf8Bom[] = "\xEF\xBB\xBF";

bool is_lower_ascii_code(std::string_view code)
{
    return std::all_of(code.begin(), code.end(),
                       [](unsigned char character) { return character >= 'a' && character <= 'z'; });
}

std::shared_ptr<const Table> load_table(const std::filesystem::path &path)
{
    auto table = std::make_shared<Table>();
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }

    std::string line;
    bool first_line = true;
    while (std::getline(input, line))
    {
        if (first_line && line.rfind(kUtf8Bom, 0) == 0)
        {
            line.erase(0, 3);
        }
        first_line = false;
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#')
        {
            continue;
        }
        const auto first_tab = line.find('\t');
        if (first_tab == std::string::npos)
        {
            continue;
        }
        const auto second_tab = line.find('\t', first_tab + 1);
        if (second_tab == std::string::npos)
        {
            continue;
        }
        const std::string_view code = std::string_view(line).substr(0, first_tab);
        const std::string_view word = std::string_view(line).substr(first_tab + 1, second_tab - first_tab - 1);
        // 拆字输入只可能是 a-z，码字域之外的码（繁体、注音、含撇号未归一）没有查询语义。
        if (code.empty() || word.empty() || !is_lower_ascii_code(code))
        {
            continue;
        }
        // 条目里的长度和偏移是定宽整数；真实数据的一行只有几十字节，超出的行不可能是合法拆字。
        if (code.size() > UINT16_MAX || word.size() > UINT16_MAX ||
            table->text.size() + code.size() + word.size() > UINT32_MAX)
        {
            continue;
        }
        std::int64_t weight = 0;
        try
        {
            weight = std::stoll(line.substr(second_tab + 1));
        }
        catch (...)
        {
            continue;
        }
        Entry entry;
        entry.offset = static_cast<std::uint32_t>(table->text.size());
        entry.code_size = static_cast<std::uint16_t>(code.size());
        entry.word_size = static_cast<std::uint16_t>(word.size());
        entry.weight = weight;
        table->text.append(code);
        table->text.append(word);
        table->entries.push_back(entry);
    }
    table->text.shrink_to_fit();
    table->entries.shrink_to_fit();

    table->by_code.resize(table->entries.size());
    for (std::size_t index = 0; index < table->entries.size(); ++index)
    {
        table->by_code[index] = static_cast<std::uint32_t>(index);
    }
    std::sort(table->by_code.begin(), table->by_code.end(),
              [&](std::uint32_t left, std::uint32_t right) { return table->code(left) < table->code(right); });
    return table;
}

// 表加载后不可变，进程内按路径只读一次。按路径缓存而不是单个静态 local，是为了让测试
// 能指向自己的临时表而不与部署布局打架。
std::shared_ptr<const Table> cached_table(const std::filesystem::path &path)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, std::shared_ptr<const Table>> cache;
    const std::string key = path_to_utf8(path);
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = cache.find(key);
    if (found != cache.end())
    {
        return found->second;
    }
    std::shared_ptr<const Table> table = load_table(path);
    cache.emplace(key, table);
    return table;
}

struct Matches
{
    std::vector<std::size_t> exact;
    std::vector<std::size_t> prefix;
};

// 拆字码以 code 开头的行：全码命中与仅前缀命中分开，各自按下标（文件顺序即字频降序）排好。
Matches collect_matches(const Table &table, std::string_view code)
{
    Matches matches;
    const auto lower =
        std::lower_bound(table.by_code.begin(), table.by_code.end(), code,
                         [&](std::uint32_t index, std::string_view value) { return table.code(index) < value; });
    for (auto it = lower; it != table.by_code.end(); ++it)
    {
        const std::string_view entry_code = table.code(*it);
        if (entry_code.size() < code.size() || entry_code.compare(0, code.size(), code) != 0)
        {
            // 同一前缀的行在排序视图里连续，越过前缀范围即可停。
            break;
        }
        (entry_code.size() == code.size() ? matches.exact : matches.prefix).push_back(*it);
    }
    std::sort(matches.exact.begin(), matches.exact.end());
    std::sort(matches.prefix.begin(), matches.prefix.end());
    return matches;
}

std::vector<WordItem> query_codes(const std::filesystem::path &decomposition_path,
                                  const std::vector<std::string> &codes, int limit)
{
    if (limit <= 0 || codes.empty())
    {
        return {};
    }
    const std::shared_ptr<const Table> table = cached_table(decomposition_path);
    if (!table || table->entries.empty())
    {
        return {};
    }

    // 全码命中在前、仅前缀命中在后，各自按字频：打全了某个生僻字的拆法，它不能被一串更高频的
    // 长码挤出 limit。有几种读法时（双拼的码与按全拼读的原串）各层里按读法的先后排。
    std::vector<Matches> per_code;
    per_code.reserve(codes.size());
    for (const std::string &code : codes)
    {
        per_code.push_back(collect_matches(*table, code));
    }

    const auto count = static_cast<std::size_t>(limit);
    std::vector<WordItem> result;
    // 一个字常有几种拆法（駉 = majiong / majiongkou / matongkou），共享同一前缀时只出一次，
    // 位置取它排得最前的那条。
    std::unordered_set<std::string_view> seen;
    std::vector<const std::vector<std::size_t> *> tiers;
    for (const Matches &matches : per_code)
    {
        tiers.push_back(&matches.exact);
    }
    for (const Matches &matches : per_code)
    {
        tiers.push_back(&matches.prefix);
    }
    for (const auto *matches : tiers)
    {
        for (const std::size_t index : *matches)
        {
            if (result.size() >= count)
            {
                return result;
            }
            const std::string_view word = table->word(index);
            if (!seen.insert(word).second)
            {
                continue;
            }
            // 注音不在这里填：引擎没有汉字→拼音能力，由 Server 组装候选时补。
            result.emplace_back(std::string{}, std::string(word), table->entries[index].weight,
                                CandidateSource::Generated);
        }
    }
    return result;
}
} // namespace

std::vector<WordItem> query_radical(const std::filesystem::path &decomposition_path, const std::string &code, int limit)
{
    if (code.empty() || !is_lower_ascii_code(code))
    {
        return {};
    }
    return query_codes(decomposition_path, {code}, limit);
}

std::vector<WordItem> query_radical(const std::filesystem::path &decomposition_path, const std::string &code,
                                    SchemeType scheme, const ShuangpinProfile &profile, int limit)
{
    if (scheme != SchemeType::Shuangpin)
    {
        return query_radical(decomposition_path, code, limit);
    }
    // 用 ; 当 ing 韵母的方案（微软、搜狗、紫光），; 只会出现在一对的第二键上，解不出音节时整串作罢。
    const bool semicolon_final = ShuangpinProfileUsesSemicolonFinal(profile);
    if (code.empty() || !std::all_of(code.begin(), code.end(), [&](unsigned char character) {
            return (character >= 'a' && character <= 'z') || (semicolon_final && character == ';');
        }))
    {
        return {};
    }
    std::vector<std::string> codes = shuangpin::decode_syllable_pairs(code, profile);
    if (is_lower_ascii_code(code) && std::find(codes.begin(), codes.end(), code) == codes.end())
    {
        codes.push_back(code);
    }
    return query_codes(decomposition_path, codes, limit);
}

void preload_radical_table(const std::filesystem::path &decomposition_path)
{
    cached_table(decomposition_path);
}
} // namespace metasequoia::local_modes
