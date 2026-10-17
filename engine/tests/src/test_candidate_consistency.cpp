// 候选结果的对照测试：同一（输入序列 + 前文 + 配置）的候选集合、顺序与分页必须逐字一致。
//
// 这里对比的是引擎与它自己，而不是与一份写死的黄金基线：词库、kenlm 模型（sc.lm）与万象
// .gram 都不由 CI 的引擎任务提供（`scripts/ci/test-engine.ps1` 只铺 product-lock 锁住的词库），
// 同一份输入在装了模型与没装模型的机器上本来就会给出不同的整句排序。任何依赖词库/模型版本的
// golden 都会在别的机器上假红，而把今天的候选写死又验证不了 B 线真正会碰的那条路。
//
// 因此这份用例守的是 R6 的字面契约「不得引入任何按时间、负载或缓存命中与否分支的打分路径」：
// 同一输入在（新建会话 / 同一会话重打 / 清空缓存后重打 / 中间插过别的输入之后重打）下，
// 候选集合、顺序与分页必须完全一致；再加上一个钉子，锁住当前 fixture 词库上的头名结果，
// 让「同样的计算变便宜」之外的结果变化直接红。词库、路径与配置全部由本用例自建，
// 不读安装目录，也不依赖 product-lock 的那份词库。

#include "../../core/input_session.h"
#include "../../core/data_path.h"
#include "test_directory_cleanup.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
class Database
{
  public:
    explicit Database(const std::filesystem::path &path)
    {
        if (sqlite3_open(metasequoia::path_to_utf8(path).c_str(), &database_) != SQLITE_OK)
        {
            throw std::runtime_error("Failed to create the candidate-consistency test dictionary.");
        }
    }

    ~Database()
    {
        sqlite3_close(database_);
    }

    void execute(const char *sql)
    {
        char *error = nullptr;
        if (sqlite3_exec(database_, sql, nullptr, nullptr, &error) != SQLITE_OK)
        {
            const std::string message = error == nullptr ? "SQLite operation failed." : error;
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

  private:
    sqlite3 *database_ = nullptr;
};

void require(bool condition, const std::string &message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void set_data_directory(const std::filesystem::path &directory)
{
#ifdef _WIN32
    if (_wputenv_s(L"METASEQUOIA_IME_DATA_DIR", directory.c_str()) != 0)
#else
    if (setenv("METASEQUOIA_IME_DATA_DIR", metasequoia::path_to_utf8(directory).c_str(), 1) != 0)
#endif
    {
        throw std::runtime_error("Failed to set the data directory override.");
    }
}

void prepare_fixture(const std::filesystem::path &directory)
{
    std::filesystem::create_directories(directory);
    Database database(directory / "msime.db");
    // 表名按音节数分表（contracts/dictionary/format.json）：tbl_{N}_{首音节首字母}。
    database.execute("CREATE TABLE tbl_1_n(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                     "INSERT INTO tbl_1_n VALUES('ni', 'n', '你', 200);"
                     "INSERT INTO tbl_1_n VALUES('ni', 'n', '泥', 120);"
                     "INSERT INTO tbl_1_n VALUES('ni', 'n', '拟', 90);"
                     "CREATE TABLE tbl_1_h(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                     "INSERT INTO tbl_1_h VALUES('hao', 'h', '好', 200);"
                     "INSERT INTO tbl_1_h VALUES('hao', 'h', '号', 110);"
                     "CREATE TABLE tbl_1_m(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                     "INSERT INTO tbl_1_m VALUES('ma', 'm', '吗', 200);"
                     "INSERT INTO tbl_1_m VALUES('ma', 'm', '妈', 180);"
                     "CREATE TABLE tbl_1_y(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                     "INSERT INTO tbl_1_y VALUES('yi', 'y', '一', 200);"
                     "INSERT INTO tbl_1_y VALUES('yi', 'y', '以', 120);"
                     "CREATE TABLE tbl_1_z(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                     "INSERT INTO tbl_1_z VALUES('zhi', 'z', '之', 200);"
                     "INSERT INTO tbl_1_z VALUES('zhi', 'z', '只', 100);"
                     "CREATE TABLE tbl_2_n(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                     "INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', '你好', 200);"
                     "INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', '拟好', 60);"
                     "CREATE TABLE tbl_2_y(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                     "INSERT INTO tbl_2_y VALUES('yi''zhi', 'yz', '一致', 200);"
                     "INSERT INTO tbl_2_y VALUES('yi''zhi', 'yz', '一直', 180);"
                     "CREATE TABLE tbl_2_m(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                     "INSERT INTO tbl_2_m VALUES('ma''ma', 'mm', '妈妈', 200);"
                     "CREATE TABLE tbl_3_n(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                     "INSERT INTO tbl_3_n VALUES('ni''hao''ma', 'nhm', '你好吗', 200);");
}

metasequoia::RuntimePaths fixture_paths(const std::filesystem::path &directory)
{
    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;
    return paths;
}

// 一次解码的完整结果：候选集合、顺序（含出处与权重），以及跟着一起变的预编辑与切分。
// 候选列表就是引擎对外的唯一权威顺序，前端的「分页」只是它的切片，所以这里逐字比对
// 这份列表就等于比对分页。
struct Snapshot
{
    std::string preedit;
    std::string segmentation;
    std::vector<std::string> rows;

    bool operator==(const Snapshot &other) const
    {
        return preedit == other.preedit && segmentation == other.segmentation && rows == other.rows;
    }
};

std::string describe(const Snapshot &snapshot)
{
    std::string text = "preedit=[" + snapshot.preedit + "] segmentation=[" + snapshot.segmentation + "]";
    for (const std::string &row : snapshot.rows)
    {
        text += " [" + row + "]";
    }
    return text;
}

Snapshot snapshot_of(metasequoia::InputSession &session)
{
    Snapshot snapshot;
    snapshot.preedit = session.preedit();
    snapshot.segmentation = session.get_pinyin_segmentation_with_cases();
    for (const WordItem &item : session.candidates())
    {
        snapshot.rows.push_back(item.word + "|" + item.canonical_pinyin + "|" +
                                std::to_string(static_cast<int>(item.source)) + "|" + std::to_string(item.weight));
    }
    return snapshot;
}

void type(metasequoia::InputSession &session, const std::string &text)
{
    for (const char character : text)
    {
        require(session.handle_character(character).handled, "A pinyin character was not handled.");
    }
}

// 逐键快照：整串一致还不够，每一个中间前缀也必须一致，否则「同一输入序列」在按键级别就已经
// 分叉了（缓存按前缀命中，分叉最先出现在这里）。
std::vector<Snapshot> type_and_snapshot_each_key(metasequoia::InputSession &session, const std::string &text)
{
    std::vector<Snapshot> snapshots;
    for (const char character : text)
    {
        require(session.handle_character(character).handled, "A pinyin character was not handled.");
        snapshots.push_back(snapshot_of(session));
    }
    return snapshots;
}

void require_same_snapshots(const std::vector<Snapshot> &left, const std::vector<Snapshot> &right,
                            const std::string &what)
{
    require(left.size() == right.size(), what + ": a different number of keystrokes produced candidates");
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        require(left[i] == right[i], what + ": keystroke " + std::to_string(i + 1) +
                                         " differs\n  left: " + describe(left[i]) + "\n  right: " + describe(right[i]));
    }
}

void require_same_snapshot(const Snapshot &left, const Snapshot &right, const std::string &what)
{
    require(left == right, what + " differs\n  left: " + describe(left) + "\n  right: " + describe(right));
}

// 输入序列固定在两组：一组让逐键前缀都命中词库行（你好），一组到第二键还没有整词、
// 要靠切分与整句来源（一致 / 你好吗），后者才是缓存最容易改结果的地方。
const char *const kInputs[] = {"nihao", "yizhi", "nihaoma", "nih"};

void run_candidate_consistency_tests(const std::filesystem::path &data_directory)
{
    const std::filesystem::path directory = data_directory / "candidate-consistency";
    std::filesystem::remove_all(directory);
    prepare_fixture(directory);
    const metasequoia::RuntimePaths paths = fixture_paths(directory);

    for (const char *input : kInputs)
    {
        const std::string typed(input);

        // ① 两个各自新建的会话给出同一份结果：进程级共享状态（共享模型、语句缓存）不得
        //    让第二次查询看到第一次的结果。
        metasequoia::InputSession first(SchemeType::Quanpin, 0u, false, true, false, paths);
        const std::vector<Snapshot> first_keys = type_and_snapshot_each_key(first, typed);
        metasequoia::InputSession second(SchemeType::Quanpin, 0u, false, true, false, paths);
        require_same_snapshots(first_keys, type_and_snapshot_each_key(second, typed),
                               typed + " across two fresh sessions");

        // ② 同一会话重打、以及清空缓存后重打：命中与未命中之间不得有可观测差异
        //    （implement.md 步骤 2b 的 R6 护栏）。
        const Snapshot cold = snapshot_of(first);
        first.reset_state();
        first.reset_cache();
        type(first, typed);
        require_same_snapshot(cold, snapshot_of(first), typed + " after reset_cache");

        first.reset_state();
        type(first, typed);
        require_same_snapshot(cold, snapshot_of(first), typed + " with warm caches");

        // ③ 中间插过别的输入再回来：跨按键复用的缓存不得把上一个输入的行带进来。
        //    这里不清缓存，否则上一个输入留下的缓存行在比较前就被抹掉，这一项就什么都测不到。
        first.reset_state();
        type(first, "mama");
        first.reset_state();
        type(first, typed);
        require_same_snapshot(cold, snapshot_of(first), typed + " after an unrelated input");

        // 钉子：当前 fixture 词库上的头名。词库与配置都由本用例自建，「同样的计算变便宜」
        // 之外的任何排序变化都会在这里红。
        require(!cold.rows.empty(), typed + " produced no candidates at all");
        const std::string head = cold.rows.front().substr(0, cold.rows.front().find('|'));
        const std::string expected_head = typed == "nihao"     ? "你好"
                                          : typed == "yizhi"   ? "一致"
                                          : typed == "nihaoma" ? "你好吗"
                                                               : "你好";
        require(head == expected_head,
                typed + ": the fixture dictionary must still rank " + expected_head + " first, got " + head);
    }

    std::filesystem::remove_all(directory);
}
} // namespace

int run_test()
{
    const auto unique_suffix = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const std::filesystem::path data_directory =
        std::filesystem::temp_directory_path() /
        std::filesystem::u8path("metasequoia-consistency-词库-" + unique_suffix);
    metasequoia::test::ScopedDataDirectoryCleanup cleanup(data_directory);
    std::filesystem::create_directories(data_directory);
    set_data_directory(data_directory);

    run_candidate_consistency_tests(data_directory);
    return 0;
}

int main()
{
    try
    {
        return run_test();
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    catch (...)
    {
        std::fprintf(stderr, "An exception escaped the test body.\n");
        return 1;
    }
}
