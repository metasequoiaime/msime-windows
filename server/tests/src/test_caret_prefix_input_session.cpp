// 光标驱动的前缀解码（PRD R2–R7/R9，引擎 Stage 1）的 CI 回归。
// 同一组断言已先落在 engine/tests/src/test_input_session.cpp 的
// run_caret_prefix_session_tests，但 engine/tests 不进 CI（见 engine spec 的 helpcode
// 接入契约），PRD AC9 要求单测 CI 可跑，因此按 server 测试框架移植一份等价实现；
// 两侧语义以引擎侧为准，这里只消费 set_caret / recompute_candidates / prefix_end /
// pending_suffix 这些公开访问器。
#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"
#include "engine/core/input_session.h"
#include "engine/shuangpin/shuangpin_profile.h"

#include <sqlite3.h>
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
// 隔离词库夹具：caret 前缀断言需要确定的候选集与权重次序，绝不依赖开发机或 CI
// 机器上的全局 msime.db。表内容与 engine 侧同名夹具逐字一致，权重决定候选顺序。
std::filesystem::path CreateCaretPrefixFixture()
{
    // PID 后缀：目录开场会被 remove_all，固定名在并行 ctest 或多工作树同时跑时会删掉
    // 另一个进程正在用的夹具。
    const auto directory =
        test::TempRoot() / ("msime-caret-prefix-input-session-test-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    std::filesystem::create_directories(directory);

    sqlite3 *database = nullptr;
    if (sqlite3_open(test::Utf8(directory / "msime.db").c_str(), &database) != SQLITE_OK)
    {
        throw std::runtime_error("Failed to create the caret-prefix fixture dictionary.");
    }
    const auto execute = [database](const char *sql) {
        char *error = nullptr;
        if (sqlite3_exec(database, sql, nullptr, nullptr, &error) != SQLITE_OK)
        {
            const std::string message = error == nullptr ? "SQLite operation failed." : error;
            sqlite3_free(error);
            sqlite3_close(database);
            throw std::runtime_error(message);
        }
    };
    execute("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
            "INSERT INTO tbl_1_n VALUES('ni','n','你',100);"
            "INSERT INTO tbl_1_n VALUES('ni','n','拟',90);");
    execute("CREATE TABLE tbl_2_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
            "INSERT INTO tbl_2_n VALUES('ni''hao','nh','你好',200);"
            "INSERT INTO tbl_2_n VALUES('ni''hao','nh','拟好',100);");
    execute("CREATE TABLE tbl_1_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
            "INSERT INTO tbl_1_s VALUES('shi','sh','是',100);");
    execute("CREATE TABLE tbl_1_j(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
            "INSERT INTO tbl_1_j VALUES('jie','j','接',100);");
    execute("CREATE TABLE wubi86(key TEXT,value TEXT,weight INTEGER);"
            "INSERT INTO wubi86 VALUES('aaaa','工',100);"
            "INSERT INTO wubi86 VALUES('aaaa','或',50);");
    sqlite3_close(database);
    return directory;
}

metasequoia::RuntimePaths FixturePaths(const std::filesystem::path &directory)
{
    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;
    return paths;
}

// 把夹具里「拟」的权重抬到「你」之上，模拟组合进行中的词库重排：用户词典调频、
// 配置热更新都会触发 reset_cache，前缀候选必须随之刷新。
void RaiseNiCandidateWeight(const std::filesystem::path &directory)
{
    sqlite3 *database = nullptr;
    if (sqlite3_open(test::Utf8(directory / "msime.db").c_str(), &database) != SQLITE_OK)
    {
        throw std::runtime_error("Failed to open the caret-prefix fixture dictionary.");
    }
    // 会话的连接同时开着，写入可能短暂拿不到锁；等它释放而不是直接失败。
    sqlite3_busy_timeout(database, 5000);
    char *error = nullptr;
    if (sqlite3_exec(database, "UPDATE tbl_1_n SET weight=150 WHERE key='ni' AND value='拟';", nullptr, nullptr,
                     &error) != SQLITE_OK)
    {
        const std::string message = error == nullptr ? "SQLite weight update failed." : error;
        sqlite3_free(error);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    sqlite3_close(database);
}

void TypeText(metasequoia::InputSession &session, const std::string &text)
{
    for (const char character : text)
    {
        REQUIRE(session.handle_character(character).handled);
    }
}

std::vector<std::string> CandidateWords(const metasequoia::InputSession &session)
{
    std::vector<std::string> words;
    words.reserve(session.candidates().size());
    for (const WordItem &item : session.candidates())
    {
        words.push_back(item.word);
    }
    return words;
}

bool SameCandidateWords(const metasequoia::InputSession &session, const std::vector<std::string> &expected)
{
    if (session.candidates().size() != expected.size())
    {
        return false;
    }
    return std::equal(session.candidates().begin(), session.candidates().end(), expected.begin(),
                      [](const WordItem &left, const std::string &right) { return left.word == right; });
}

std::size_t CandidateWordIndex(const metasequoia::InputSession &session, const std::string &word)
{
    const auto &candidates = session.candidates();
    const auto found =
        std::find_if(candidates.begin(), candidates.end(), [&word](const WordItem &item) { return item.word == word; });
    return static_cast<std::size_t>(found - candidates.begin());
}

// 前缀等价断言的基准：同一拼写独立键入得到的候选列表。
std::vector<std::string> QuanpinCandidateWordsFor(const std::string &typed, const metasequoia::RuntimePaths &paths)
{
    metasequoia::InputSession session(SchemeType::Quanpin, 0, true, true, false, paths);
    TypeText(session, typed);
    return CandidateWords(session);
}

std::vector<std::string> ShuangpinCandidateWordsFor(const std::string &typed, const metasequoia::RuntimePaths &paths)
{
    metasequoia::InputSession session(SchemeType::Shuangpin, GetMicrosoftShuangpinProfile(), paths);
    TypeText(session, typed);
    return CandidateWords(session);
}
} // namespace

// R2/R3/R7：音节内 caret 向下取整到最后一个完整单元边界，候选等于独立键入该前缀的
// 候选；caret 未设置或落在末尾时与整串解码零差异；R4：前缀为空时无候选且组合不动。
TEST_CASE(CaretPrefixDecodesQuanpinByCompleteSyllablePrefix)
{
    const auto directory = CreateCaretPrefixFixture();
    const auto paths = FixturePaths(directory);

    // 会话必须先于 remove_all 析构：它持有词库句柄，Windows 上删不掉打开的文件。
    {
        const std::string sentence = "ni'hao'shi'jie";
        metasequoia::InputSession session(SchemeType::Quanpin, 0, true, true, false, paths);
        TypeText(session, sentence);
        REQUIRE(session.has_composition());
        REQUIRE_EQ(session.prefix_end(), sentence.size());
        REQUIRE(session.pending_suffix().empty());
        const auto full_words = CandidateWords(session);
        REQUIRE(SameCandidateWords(session, full_words));

        for (const std::size_t caret : {std::size_t{5}, std::size_t{6}})
        {
            session.set_caret(caret);
            session.recompute_candidates();
            REQUIRE_EQ(session.prefix_end(), std::size_t{3});
            REQUIRE_EQ(session.pending_suffix(), std::string("hao'shi'jie"));
            REQUIRE(CandidateWordIndex(session, "你") < session.candidates().size());
            REQUIRE(SameCandidateWords(session, QuanpinCandidateWordsFor("ni", paths)));
        }

        session.set_caret(7);
        session.recompute_candidates();
        REQUIRE_EQ(session.prefix_end(), std::size_t{7});
        REQUIRE_EQ(session.pending_suffix(), std::string("shi'jie"));
        REQUIRE(CandidateWordIndex(session, "你好") < session.candidates().size());
        REQUIRE(SameCandidateWords(session, QuanpinCandidateWordsFor("ni'hao", paths)));
        const auto at_hao = CandidateWords(session);

        session.set_caret(9);
        session.recompute_candidates();
        REQUIRE_EQ(session.prefix_end(), std::size_t{7});
        REQUIRE(SameCandidateWords(session, at_hao));

        session.set_caret(13);
        session.recompute_candidates();
        REQUIRE_EQ(session.prefix_end(), std::size_t{11});
        REQUIRE_EQ(session.pending_suffix(), std::string("jie"));
        REQUIRE(!session.candidates().empty());
        REQUIRE(SameCandidateWords(session, QuanpinCandidateWordsFor("ni'hao'shi", paths)));

        session.set_caret(0);
        session.recompute_candidates();
        REQUIRE(session.candidates().empty());
        REQUIRE_EQ(session.prefix_end(), std::size_t{0});
        REQUIRE_EQ(session.pending_suffix(), sentence);
        REQUIRE_EQ(session.editing_text(), sentence);
        REQUIRE_EQ(session.caret_position(), std::size_t{0});
        REQUIRE_EQ(session.preedit(), sentence);

        // 越界 caret 被夹到串尾，退化为整串解码；nullopt 恢复默认。
        session.set_caret(sentence.size() + 10);
        session.recompute_candidates();
        REQUIRE_EQ(session.caret_position(), sentence.size());
        REQUIRE_EQ(session.prefix_end(), sentence.size());
        REQUIRE(SameCandidateWords(session, full_words));
        session.set_caret(std::nullopt);
        session.recompute_candidates();
        REQUIRE(SameCandidateWords(session, full_words));
    }

    std::filesystem::remove_all(directory);
}

// 回归：前缀候选按前缀文本缓存，修复前只有文本变化才重查——caret 停在内点时
// 外部重排词库（用户词典调频、配置热更新都走 reset_cache），重算后候选仍停在
// 旧权重上。reset_cache 必须连带失效前缀缓存，caret 与后缀不受影响。
TEST_CASE(PrefixCandidatesRefreshAfterResetCache)
{
    const auto directory = CreateCaretPrefixFixture();
    const auto paths = FixturePaths(directory);

    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0, true, true, false, paths);
        TypeText(session, "ni'hao");
        session.set_caret(4);
        session.recompute_candidates();
        REQUIRE_EQ(session.prefix_end(), std::size_t{3});
        REQUIRE_EQ(session.pending_suffix(), std::string("hao"));
        REQUIRE(CandidateWordIndex(session, "你") < CandidateWordIndex(session, "拟"));

        RaiseNiCandidateWeight(directory);

        session.reset_cache();
        session.recompute_candidates();
        // caret 不动、后缀不变，只有前缀候选按新权重重查。
        REQUIRE_EQ(session.prefix_end(), std::size_t{3});
        REQUIRE_EQ(session.pending_suffix(), std::string("hao"));
        REQUIRE(CandidateWordIndex(session, "拟") < CandidateWordIndex(session, "你"));
    }

    std::filesystem::remove_all(directory);
}

// pending_suffix 保留原始大小写：光标处插入的大写字母原样留在后缀里。
TEST_CASE(CaretPrefixSuffixKeepsTheTypedCasing)
{
    const auto directory = CreateCaretPrefixFixture();
    const auto paths = FixturePaths(directory);

    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0, true, true, false, paths);
        TypeText(session, "ni'hao");
        session.handle_command(metasequoia::Command::MoveHome);
        REQUIRE(session.handle_character('H').handled);
        REQUIRE_EQ(session.editing_text(), std::string("Hni'hao"));
        REQUIRE_EQ(session.caret_position(), std::size_t{1});

        session.set_caret(0);
        session.recompute_candidates();
        REQUIRE_EQ(session.prefix_end(), std::size_t{0});
        REQUIRE_EQ(session.pending_suffix(), std::string("Hni'hao"));

        session.set_caret(std::nullopt);
        session.recompute_candidates();
        REQUIRE_EQ(session.editing_text(), std::string("Hni'hao"));
    }

    std::filesystem::remove_all(directory);
}

// R9：五笔等无单元模型（segment_raw_boundaries 为空）的方案，caret 移动不量化、
// 候选零变化，维持整串解码的现状。
TEST_CASE(CaretPrefixLeavesSchemesWithoutUnitModelUntouched)
{
    const auto directory = CreateCaretPrefixFixture();
    const auto paths = FixturePaths(directory);

    {
        metasequoia::InputSession session(SchemeType::Wubi, 0, true, true, false, paths);
        TypeText(session, "aaaa");
        REQUIRE(session.has_composition());
        REQUIRE(CandidateWordIndex(session, "工") < session.candidates().size());
        const auto native = CandidateWords(session);

        for (const std::size_t caret : {std::size_t{0}, std::size_t{2}})
        {
            session.set_caret(caret);
            session.recompute_candidates();
            REQUIRE(SameCandidateWords(session, native));
            REQUIRE_EQ(session.prefix_end(), std::size_t{4});
            REQUIRE(session.pending_suffix().empty());
        }
    }

    std::filesystem::remove_all(directory);
}

// 双拼贪心配对（engine spec #187）：nihkb; → {0,2,4,6}、nihcb; → {0,2,3,5,6}，
// caret 落在段中间时同样 floor 到完整段边界；首单元内侧向上取整到首单元末尾。
TEST_CASE(CaretPrefixFloorsInsideShuangpinGreedyUnits)
{
    const auto directory = CreateCaretPrefixFixture();
    const auto paths = FixturePaths(directory);

    {
        metasequoia::InputSession session(SchemeType::Shuangpin, GetMicrosoftShuangpinProfile(), paths);
        TypeText(session, "nihkb;");
        REQUIRE(CandidateWordIndex(session, "你好") < session.candidates().size());
        REQUIRE_EQ(session.prefix_end(), std::size_t{6});
        REQUIRE(session.pending_suffix().empty());

        session.set_caret(1);
        session.recompute_candidates();
        REQUIRE_EQ(session.prefix_end(), std::size_t{2});
        REQUIRE_EQ(session.pending_suffix(), std::string("hkb;"));
        REQUIRE(SameCandidateWords(session, ShuangpinCandidateWordsFor("ni", paths)));

        session.set_caret(3);
        session.recompute_candidates();
        REQUIRE_EQ(session.prefix_end(), std::size_t{2});
        REQUIRE_EQ(session.pending_suffix(), std::string("hkb;"));
        REQUIRE(SameCandidateWords(session, ShuangpinCandidateWordsFor("ni", paths)));

        session.set_caret(5);
        session.recompute_candidates();
        REQUIRE_EQ(session.prefix_end(), std::size_t{4});
        REQUIRE_EQ(session.pending_suffix(), std::string("b;"));
        REQUIRE(CandidateWordIndex(session, "你好") < session.candidates().size());
        REQUIRE(SameCandidateWords(session, ShuangpinCandidateWordsFor("nihk", paths)));

        session.set_caret(6);
        session.recompute_candidates();
        REQUIRE_EQ(session.prefix_end(), std::size_t{6});
        REQUIRE(!session.candidates().empty());

        session.handle_command(metasequoia::Command::Cancel);
        TypeText(session, "nihcb;");
        session.set_caret(4);
        session.recompute_candidates();
        REQUIRE_EQ(session.prefix_end(), std::size_t{3});
        REQUIRE_EQ(session.pending_suffix(), std::string("cb;"));
        REQUIRE(SameCandidateWords(session, ShuangpinCandidateWordsFor("nih", paths)));
    }

    std::filesystem::remove_all(directory);
}

// R5 前缀选词结算序列（Server ProcessSelectionKey 对会话的确切调用序）：advance 消
// 耗的正是前缀本身、剩余 raw 就是后缀；结算把光标复位为 nullopt 后，后缀按全新整串
// 解码（AC2）。同时钉住「选中候选短于前缀」的部分消费：剩余 raw 从被消费的那段之后
// 开始，而不是从旧边界之后开始。
TEST_CASE(PrefixSelectionSettlementLeavesFreshFullStringSuffix)
{
    const auto directory = CreateCaretPrefixFixture();
    const auto paths = FixturePaths(directory);

    {
        const std::string sentence = "ni'hao'shi'jie";
        metasequoia::InputSession session(SchemeType::Quanpin, 0, true, true, false, paths);
        TypeText(session, sentence);

        // 光标在 ni'hao 之后：候选等于前缀 ni'hao 的候选，选中「你好」。
        session.set_caret(7);
        session.recompute_candidates();
        REQUIRE_EQ(session.prefix_end(), std::size_t{7});
        const std::size_t you_hao = CandidateWordIndex(session, "你好");
        REQUIRE(you_hao < session.candidates().size());
        const WordItem selected = session.candidates()[you_hao];

        const auto transition =
            session.advance_composition_after_selection(selected.pinyin, selected.word, selected.canonical_pinyin);
        REQUIRE(transition.continues_composition);
        REQUIRE_EQ(transition.consumed_raw_input_with_cases, std::string("ni'hao"));
        REQUIRE_EQ(session.get_pinyin_sequence_with_cases(), std::string("shi'jie"));

        // 结算收尾（Server 侧的确切序列）：复位会话光标后，后缀按整串解码，
        // 与独立键入 shi'jie 的候选一致。
        session.set_caret(std::nullopt);
        session.recompute_candidates();
        REQUIRE(session.has_composition());
        REQUIRE(session.pending_suffix().empty());
        REQUIRE(SameCandidateWords(session, QuanpinCandidateWordsFor("shi'jie", paths)));
    }

    {
        // 部分消费：光标在 ni 之后（floor 到边界 2），选中的「你」只消耗 ni，
        // 剩余 raw 从 hao 重新开始；hao 此时不完整，属于新的待转后缀。
        metasequoia::InputSession session(SchemeType::Quanpin, 0, true, true, false, paths);
        TypeText(session, "ni'hao'shi'jie");
        session.set_caret(4);
        session.recompute_candidates();
        REQUIRE_EQ(session.prefix_end(), std::size_t{3});
        const std::size_t ni_index = CandidateWordIndex(session, "你");
        REQUIRE(ni_index < session.candidates().size());
        const WordItem selected = session.candidates()[ni_index];

        const auto transition =
            session.advance_composition_after_selection(selected.pinyin, selected.word, selected.canonical_pinyin);
        REQUIRE(transition.continues_composition);
        REQUIRE_EQ(transition.consumed_raw_input_with_cases, std::string("ni"));
        REQUIRE_EQ(session.get_pinyin_sequence_with_cases(), std::string("hao'shi'jie"));

        session.set_caret(std::nullopt);
        session.recompute_candidates();
        REQUIRE(SameCandidateWords(session, QuanpinCandidateWordsFor("hao'shi'jie", paths)));
    }

    std::filesystem::remove_all(directory);
}

// R7 回归（Stage 2 质检）：前缀激活后光标移回串尾，会话必须完全回到整串态，
// 否则空格结算会错误走前缀选词路径。ApplyCompositionEditKey 的 resegment_by_caret
// 在串尾执行的就是 set_caret(nullopt) + recompute_candidates()；接线本身依赖
// GlobalIme/管道全局态，event_listener.cpp 不参与测试链接，CI 只能在这层钉住
// 同一调用序的契约：从激活的内点 caret_ 直接喂 nullopt 后，prefix_end 与候选
// 和全新整串会话零差异，空格结算据 prefix_end == raw.size() 走整串路径。
TEST_CASE(CaretReturnToTailAfterPrefixActivationRestoresFreshFullStringDecode)
{
    const auto directory = CreateCaretPrefixFixture();
    const auto paths = FixturePaths(directory);

    {
        const std::string sentence = "ni'hao'shi'jie";
        metasequoia::InputSession session(SchemeType::Quanpin, 0, true, true, false, paths);
        TypeText(session, sentence);
        const auto full_words = CandidateWords(session);

        // 光标移到中间：前缀激活，引擎 caret_ 停在内点（修复前串尾不喂时残留的状态）。
        session.set_caret(7);
        session.recompute_candidates();
        REQUIRE_EQ(session.prefix_end(), std::size_t{7});
        REQUIRE_EQ(session.pending_suffix(), std::string("shi'jie"));
        REQUIRE(SameCandidateWords(session, QuanpinCandidateWordsFor("ni'hao", paths)));

        // 光标移回串尾：与 Server 侧修复后相同的确切调用序，结果必须与全新整串零差异。
        session.set_caret(std::nullopt);
        session.recompute_candidates();
        REQUIRE_EQ(session.caret_position(), sentence.size());
        REQUIRE_EQ(session.prefix_end(), sentence.size());
        REQUIRE(session.pending_suffix().empty());
        REQUIRE(SameCandidateWords(session, full_words));
        REQUIRE(SameCandidateWords(session, QuanpinCandidateWordsFor(sentence, paths)));
    }

    std::filesystem::remove_all(directory);
}
