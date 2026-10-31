// 长词前缀补全（#1343）的离线评测：对一批四字以上的前缀，在真实词库上列出补全的前几名、各项分数、
// 是否过门槛，以及每个前缀的查询和打分耗时。不是测试，不进 ctest，手动跑：
//   eval_prefix_completion <msime.db> [sc.lm] [前缀表.tsv]
// 前缀表每行「完整读音<Tab>词」（zhong'hua'ren'min<Tab>中华人民），不给就用内置的一批。
// 词库只读打开，不写。
//   eval_prefix_completion --session <拼音> [<拼音> ...]
// 在 METASEQUOIA_IME_DATA_DIR 指向的数据目录（要是一份副本）上开一个全拼会话，逐键敲入每串拼音，打印前三个
// 候选和开、关补全时的每键耗时。只敲不选，不写词库。
#include "../../contracts/dictionary/format.h"
#include "../../core/input_session.h"
#include "../../quanpin/prefix_completion.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
const std::vector<std::pair<std::string, std::string>> kDefaultPrefixes = {
    {"zhong'hua'ren'min", "中华人民"}, {"ren'gong'zhi'neng", "人工智能"}, {"bei'jing'da'xue", "北京大学"},
    {"ji'suan'ji'ke", "计算机科"},     {"shui'shan'shu'ru", "水杉输入"},  {"xin'xi'ji'shu", "信息技术"},
    {"li'shi'wen'hua", "历史文化"},    {"ma'ke'si'zhu", "马克思主"},      {"zhong'guo'ren'min", "中国人民"},
    {"guo'jia'fa'zhan", "国家发展"},   {"she'hui'zhu'yi", "社会主义"},    {"ke'xue'ji'shu", "科学技术"},
    {"jing'ji'fa'zhan", "经济发展"},   {"gao'deng'jiao'yu", "高等教育"},  {"wang'luo'an'quan", "网络安全"},
    {"ruan'jian'kai'fa", "软件开发"},  {"shu'ju'ku'guan", "数据库管"},    {"zi'ran'yu'yan", "自然语言"},
    {"shen'du'xue'xi", "深度学习"},    {"ji'qi'xue'xi", "机器学习"},      {"shang'hai'jiao'tong", "上海交通"},
    {"qing'hua'da'xue", "清华大学"},   {"yi'dai'yi'lu", "一带一路"},      {"gai'ge'kai'fang", "改革开放"},
    {"zhi'shi'chan'quan", "知识产权"}, {"huan'jing'bao'hu", "环境保护"},  {"fa'lv'fa'gui", "法律法规"},
    {"guo'wu'yuan'ban", "国务院办"},   {"quan'guo'ren'min", "全国人民"},  {"zhong'hua'min'zu", "中华民族"},
};

double now_ms()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

// 前缀本身是不是词库里的一行：会话里首选来自词库时就是，整句猜出来的半个词（马克思主）不是。
bool is_dictionary_word(sqlite3 *db, const std::string &key, const std::string &word)
{
    const std::size_t syllables = static_cast<std::size_t>(std::count(key.begin(), key.end(), '\'')) + 1;
    const std::string table = metasequoia::dictionary_format::quanpin_table(syllables, key.front());
    const std::string sql = "SELECT 1 FROM \"" + table + "\" WHERE key = ?1 AND value = ?2 LIMIT 1";
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(statement, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, word.c_str(), -1, SQLITE_TRANSIENT);
    const bool found = sqlite3_step(statement) == SQLITE_ROW;
    sqlite3_finalize(statement);
    return found;
}

void report(const char *label, const std::string &key, const std::string &word, sqlite3 *db,
            std::unordered_map<std::string, sqlite3_stmt *> &statements, const ngram::LanguageModel *model)
{
    const bool prefix_is_word = is_dictionary_word(db, key, word);
    const double start = now_ms();
    // 与会话一致：前缀是词库词时按字补，否则只按读音补。
    auto rows = quanpin::query_prefix_completion_rows(db, statements, key, prefix_is_word ? word : std::string());
    const double queried = now_ms();
    const std::size_t syllables = static_cast<std::size_t>(std::count(key.begin(), key.end(), '\'')) + 1;
    quanpin::score_prefix_completions(rows, syllables, prefix_is_word, model);
    const double scored = now_ms();
    std::size_t shown = 0;
    for (const auto &row : rows)
        shown += row.confident ? 1 : 0;
    std::printf("[%s] %s%s  rows=%zu confident=%zu  query=%.3fms score=%.3fms\n", label, word.c_str(),
                prefix_is_word ? "" : " (not a word)", rows.size(), shown, queried - start, scored - queried);
    for (std::size_t i = 0; i < rows.size() && i < 4; ++i)
    {
        const auto &row = rows[i];
        std::printf("    %c %-24s w=%-6lld prior=%5.2f tail=%7.2f score=%7.2f\n", row.confident ? '*' : ' ',
                    row.value.c_str(), static_cast<long long>(row.weight), row.prior, row.tail, row.score);
    }
}
int run_session(int argc, char **argv)
{
    metasequoia::InputSession session(SchemeType::Quanpin);
    // 产品默认的整句来源：Unigram（Google）开、Trigram 关。sc.lm 在数据目录里时补全照样用它打分。
    SentenceAssociationOptions association;
    association.google = true;
    session.set_sentence_association(association);
    for (int i = 2; i < argc; ++i)
    {
        const std::string input = argv[i];
        for (const bool enabled : {false, true})
        {
            metasequoia::PrefixCompletionOptions options;
            options.enabled = enabled;
            session.set_prefix_completion_options(options);
            session.reset_state();
            session.reset_cache();
            double total = 0.0;
            double worst = 0.0;
            for (const char character : input)
            {
                const double start = now_ms();
                session.handle_character(character);
                const double elapsed = now_ms() - start;
                total += elapsed;
                worst = (std::max)(worst, elapsed);
            }
            std::printf("%-22s completion=%-3s total=%.2fms worst-key=%.2fms :", input.c_str(), enabled ? "on" : "off",
                        total, worst);
            const auto &candidates = session.candidates();
            for (std::size_t c = 0; c < candidates.size() && c < 3; ++c)
                std::printf("  %zu.%s%s", c + 1, candidates[c].word.c_str(),
                            candidates[c].prefix_completion ? "[补]" : "");
            std::printf("\n");
        }
    }
    session.reset_state();
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc >= 2 && std::string(argv[1]) == "--session")
        return run_session(argc, argv);
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: eval_prefix_completion <msime.db> [sc.lm] [prefixes.tsv]\n");
        return 2;
    }
    sqlite3 *db = nullptr;
    const std::string uri = std::string("file:") + argv[1] + "?mode=ro";
    if (sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK)
    {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::unique_ptr<ngram::LanguageModel> model;
    if (argc >= 3 && std::string(argv[2]) != "-")
    {
        model = std::make_unique<ngram::LanguageModel>(argv[2]);
        if (!model->valid())
            std::fprintf(stderr, "language model not loaded: %s\n", model->error().c_str());
    }
    std::vector<std::pair<std::string, std::string>> prefixes = kDefaultPrefixes;
    if (argc >= 4)
    {
        prefixes.clear();
        std::ifstream input(argv[3]);
        std::string line;
        while (std::getline(input, line))
        {
            const auto tab = line.find('\t');
            if (tab != std::string::npos)
                prefixes.emplace_back(line.substr(0, tab), line.substr(tab + 1));
        }
    }
    std::unordered_map<std::string, sqlite3_stmt *> statements;
    for (const auto &[key, word] : prefixes)
    {
        report("no-model", key, word, db, statements, nullptr);
        if (model && model->valid())
            report("sc.lm", key, word, db, statements, model.get());
    }
    for (auto &entry : statements)
        sqlite3_finalize(entry.second);
    sqlite3_close(db);
    return 0;
}
