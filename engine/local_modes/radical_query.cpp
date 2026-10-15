#include "radical_query.h"

#include "../core/data_path.h"

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
// 一行拆字数据。code 是部件拼音连写、word 是汉字、weight 是字频。
struct Entry
{
    std::string code;
    std::string word;
    std::int64_t weight = 0;
};

struct Table
{
    // 文件顺序即字频降序（生成脚本已排序），所以某前缀的首个命中就是它最高频的那条。
    std::vector<Entry> entries;
    // entries 的下标按 code 排序。前缀命中的行在排序视图里连续，查询只扫这一小段。
    std::vector<std::size_t> by_code;
};

// 生成脚本不会写 BOM，但保留与 helpcode 相同的防御：表由外部工具产出，首行多一个 BOM
// 不该让整张表失效。
constexpr char kUtf8Bom[] = "\xEF\xBB\xBF";

bool is_lower_ascii_code(const std::string &code)
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
        std::string code = line.substr(0, first_tab);
        std::string word = line.substr(first_tab + 1, second_tab - first_tab - 1);
        // 拆字输入只可能是 a-z，码字域之外的码（繁体、注音、含撇号未归一）没有查询语义。
        if (code.empty() || word.empty() || !is_lower_ascii_code(code))
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
        table->entries.push_back({std::move(code), std::move(word), weight});
    }

    table->by_code.resize(table->entries.size());
    for (std::size_t index = 0; index < table->entries.size(); ++index)
    {
        table->by_code[index] = index;
    }
    std::sort(table->by_code.begin(), table->by_code.end(), [&](std::size_t left, std::size_t right) {
        return table->entries[left].code < table->entries[right].code;
    });
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
} // namespace

std::vector<WordItem> query_radical(const std::filesystem::path &decomposition_path, const std::string &code, int limit)
{
    if (limit <= 0 || code.empty() || !is_lower_ascii_code(code))
    {
        return {};
    }
    const std::shared_ptr<const Table> table = cached_table(decomposition_path);
    if (!table || table->entries.empty())
    {
        return {};
    }

    const auto lower = std::lower_bound(
        table->by_code.begin(), table->by_code.end(), code,
        [&](std::size_t index, const std::string &value) { return table->entries[index].code < value; });

    // 全码命中在前、仅前缀命中在后，各自按字频：打全了某个生僻字的拆法，它不能被一串更高频的
    // 长码挤出 limit。
    std::vector<std::size_t> exact_matches;
    std::vector<std::size_t> prefix_matches;
    for (auto it = lower; it != table->by_code.end(); ++it)
    {
        const std::string &entry_code = table->entries[*it].code;
        if (entry_code.size() < code.size() || entry_code.compare(0, code.size(), code) != 0)
        {
            // 同一前缀的行在排序视图里连续，越过前缀范围即可停。
            break;
        }
        (entry_code.size() == code.size() ? exact_matches : prefix_matches).push_back(*it);
    }
    // 下标就是文件顺序（字频降序），排序后即高频优先。
    std::sort(exact_matches.begin(), exact_matches.end());
    std::sort(prefix_matches.begin(), prefix_matches.end());

    const auto count = static_cast<std::size_t>(limit);
    std::vector<WordItem> result;
    // 一个字常有几种拆法（駉 = majiong / majiongkou / matongkou），共享同一前缀时只出一次，
    // 位置取它排得最前的那条。
    std::unordered_set<std::string_view> seen;
    for (const auto *matches : {&exact_matches, &prefix_matches})
    {
        for (const std::size_t index : *matches)
        {
            if (result.size() >= count)
            {
                return result;
            }
            const Entry &entry = table->entries[index];
            if (!seen.insert(entry.word).second)
            {
                continue;
            }
            // 注音不在这里填：引擎没有汉字→拼音能力，由 Server 组装候选时补。
            result.emplace_back(std::string{}, entry.word, entry.weight, CandidateSource::Generated);
        }
    }
    return result;
}

void preload_radical_table(const std::filesystem::path &decomposition_path)
{
    cached_table(decomposition_path);
}
} // namespace metasequoia::local_modes
