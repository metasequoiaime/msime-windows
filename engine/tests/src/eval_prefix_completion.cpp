// 长词前缀补全（#1343）的离线评测。不是测试，不进 ctest，手动跑，词库都只读打开。
//
//   eval_prefix_completion <msime.db> [--gram x.gram] [--lm sc.lm] [--neural sentence-model.safetensors]
//                          [--prefixes 前缀表.tsv]
// 对一批四字以上的前缀，先不用模型、再分别只用给出的每个模型，跑一遍和会话同一套的打分与门槛，列出前几名、
// 各项分数、能不能显示，以及查询与打分耗时。前缀表每行「完整读音<Tab>词」（zhong'hua'ren'min<Tab>中华人民），
// 不给就用内置的一批。
//
//   eval_prefix_completion --session [--gram x.gram] [--file 拼音表] [<拼音> ...]
// 在 METASEQUOIA_IME_DATA_DIR 指向的数据目录（要是一份副本）上开一个全拼会话，逐键敲入每串拼音。给了拼音时打印
// 前三个候选和开、关补全时的每键耗时；给了 --file 时逐行输出「拼音<Tab>首选<Tab>首选是否词库词<Tab>第二位
// <Tab>第二位是否补全」。--gram 是补全打分用的万象模型（会话里由设置页激活的模型给出）。只敲不选，不写词库。
#include "../../contracts/dictionary/format.h"
#include "../../core/input_session.h"
#include "../../neural/sentence_model.h"
#include "../../ngram/octagram/octagram_gram.h"
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
    {"zhong'hua'ren'min", "中华人民"},  {"ren'gong'zhi'neng", "人工智能"},    {"bei'jing'da'xue", "北京大学"},
    {"ji'suan'ji'ke", "计算机科"},      {"shui'shan'shu'ru", "水杉输入"},     {"xin'xi'ji'shu", "信息技术"},
    {"li'shi'wen'hua", "历史文化"},     {"ma'ke'si'zhu", "马克思主"},         {"she'hui'zhu'yi", "社会主义"},
    {"ke'xue'ji'shu", "科学技术"},      {"jing'ji'fa'zhan", "经济发展"},      {"wang'luo'an'quan", "网络安全"},
    {"ji'qi'xue'xi", "机器学习"},       {"qing'hua'da'xue", "清华大学"},      {"gai'ge'kai'fang", "改革开放"},
    {"zhi'shi'chan'quan", "知识产权"},  {"huan'jing'bao'hu", "环境保护"},     {"fa'lv'fa'gui", "法律法规"},
    {"zhong'hua'min'zu", "中华民族"},   {"chuang'qian'ming'yue", "床前明月"}, {"bai'ri'yi'shan", "白日依山"},
    {"chun'mian'bu'jue", "春眠不觉"},   {"hai'nei'cun'zhi", "海内存知"},      {"hui'dang'ling'jue", "会当凌绝"},
    {"bu'jin'chang'jiang", "不尽长江"}, {"wu'di'san'huang", "五帝三皇"},      {"jin'ri'fang'zhi", "今日方知"},
    {"pu'tao'tang'suan", "葡萄糖酸"},   {"jia'ji'bing'xi", "甲基丙烯"},
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
            std::unordered_map<std::string, sqlite3_stmt *> &statements,
            const quanpin::PrefixCompletionScorers &scorers)
{
    const bool prefix_is_word = is_dictionary_word(db, key, word);
    const double start = now_ms();
    // 与会话一致：前缀是词库词时按字补，否则只按读音补。
    auto rows = quanpin::query_prefix_completion_rows(db, statements, key, prefix_is_word ? word : std::string());
    const double queried = now_ms();
    const std::size_t syllables = static_cast<std::size_t>(std::count(key.begin(), key.end(), '\'')) + 1;
    quanpin::score_prefix_completions(rows, syllables, prefix_is_word, scorers);
    const double scored = now_ms();
    std::printf("[%-6s] %s%s rows=%zu query=%.3fms score=%.3fms ->", label, word.c_str(),
                prefix_is_word ? "" : "(非词)", rows.size(), queried - start, scored - queried);
    if (rows.empty() || !rows.front().confident)
        std::printf(" （不补）");
    for (std::size_t i = 0; i < rows.size() && i < 3; ++i)
    {
        const auto &row = rows[i];
        std::printf("  %s%s", row.confident ? "*" : "", row.value.c_str());
        if (row.scored)
            std::printf("(%.2f)", row.tail);
    }
    std::printf("\n");
}

int run_table(int argc, char **argv)
{
    sqlite3 *db = nullptr;
    const std::string uri = std::string("file:") + argv[1] + "?mode=ro";
    if (sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK)
    {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::shared_ptr<const gram::GramDb> gram_db;
    std::unique_ptr<ngram::LanguageModel> language_model;
    std::unique_ptr<neural::SentenceModel> neural_model;
    auto prefixes = kDefaultPrefixes;
    for (int i = 2; i + 1 < argc; i += 2)
    {
        const std::string option = argv[i];
        if (option == "--gram")
            gram_db = gram::shared_gram_db(argv[i + 1]);
        else if (option == "--lm")
            language_model = std::make_unique<ngram::LanguageModel>(argv[i + 1]);
        else if (option == "--neural")
            neural_model = neural::SentenceModel::load_file(argv[i + 1]);
        else if (option == "--prefixes")
        {
            prefixes.clear();
            std::ifstream input(argv[i + 1]);
            std::string line;
            while (std::getline(input, line))
            {
                const auto tab = line.find('\t');
                if (tab != std::string::npos)
                    prefixes.emplace_back(line.substr(0, tab), line.substr(tab + 1));
            }
        }
    }
    std::vector<std::pair<const char *, quanpin::PrefixCompletionScorers>> runs{{"无模型", {}}};
    if (gram_db && gram_db->valid())
    {
        quanpin::PrefixCompletionScorers scorers;
        scorers.collocation = gram_db.get();
        runs.emplace_back("万象", scorers);
    }
    if (language_model && language_model->valid())
    {
        quanpin::PrefixCompletionScorers scorers;
        scorers.language_model = language_model.get();
        runs.emplace_back("sc.lm", scorers);
    }
    if (neural_model)
    {
        quanpin::PrefixCompletionScorers scorers;
        scorers.neural = neural_model.get();
        runs.emplace_back("神经", scorers);
    }
    std::unordered_map<std::string, sqlite3_stmt *> statements;
    for (const auto &[key, word] : prefixes)
    {
        for (const auto &[label, scorers] : runs)
            report(label, key, word, db, statements, scorers);
    }
    for (auto &entry : statements)
        sqlite3_finalize(entry.second);
    sqlite3_close(db);
    return 0;
}

metasequoia::PrefixCompletionOptions enabled_options(const std::string &gram_path)
{
    metasequoia::PrefixCompletionOptions options;
    options.enabled = true;
    options.collocation_model = gram_path;
    return options;
}

int run_session(int argc, char **argv)
{
    std::string gram_path;
    std::string file;
    std::vector<std::string> inputs;
    for (int i = 2; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--gram" && i + 1 < argc)
            gram_path = argv[++i];
        else if (argument == "--file" && i + 1 < argc)
            file = argv[++i];
        else
            inputs.push_back(argument);
    }
    metasequoia::InputSession session(SchemeType::Quanpin);
    // 产品默认的整句来源：Unigram（Google）开、Trigram 关。sc.lm 在数据目录里时补全照样用它打分。
    SentenceAssociationOptions association;
    association.google = true;
    session.set_sentence_association(association);
    if (!file.empty())
    {
        session.set_prefix_completion_options(enabled_options(gram_path));
        std::ifstream input(file);
        std::string line;
        while (std::getline(input, line))
        {
            if (line.empty())
                continue;
            session.reset_state();
            for (const char character : line)
                session.handle_character(character);
            const auto &candidates = session.candidates();
            const bool head_is_word = !candidates.empty() && (candidates[0].source == CandidateSource::Database ||
                                                              candidates[0].source == CandidateSource::UserDatabase);
            std::printf("%s\t%s\t%d\t%s\t%d\n", line.c_str(), candidates.empty() ? "" : candidates[0].word.c_str(),
                        head_is_word ? 1 : 0, candidates.size() > 1 ? candidates[1].word.c_str() : "",
                        candidates.size() > 1 && candidates[1].prefix_completion ? 1 : 0);
        }
        session.reset_state();
        return 0;
    }
    for (const auto &input : inputs)
    {
        for (const bool enabled : {false, true})
        {
            auto options = enabled_options(gram_path);
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
        std::fprintf(stderr, "usage: eval_prefix_completion <msime.db> [--gram x.gram] [--lm sc.lm] [--neural model] "
                             "[--prefixes tsv]\n       eval_prefix_completion --session [--gram x.gram] "
                             "[--file inputs] [pinyin ...]\n");
        return 2;
    }
    return run_table(argc, argv);
}
