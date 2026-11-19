#include "tests/includes/test_framework.h"
#include "settings/dictionary_page.h"
#include "settings/serial_task_queue.h"
#include <chrono>
#include <future>
#include <set>

TEST_CASE(settings_worker_preserves_order_and_drains_on_close)
{
    const auto owner = std::this_thread::get_id();
    std::promise<void> started, release;
    auto wait = release.get_future().share();
    std::vector<int> work, delivered;
    bool off_ui = false;
    {
        SerialTaskQueue queue([] {}, [] {});
        REQUIRE(queue.Submit([&] {
            off_ui = std::this_thread::get_id() != owner;
            started.set_value();
            wait.wait();
            work.push_back(1);
            return [&] { delivered.push_back(1); };
        }));
        const auto status = started.get_future().wait_for(std::chrono::seconds(5));
        // Release even if scheduling failed so a failed test cannot deadlock destruction.
        if (status != std::future_status::ready)
            release.set_value();
        REQUIRE(status == std::future_status::ready);
        REQUIRE(queue.Submit([&] {
            work.push_back(2);
            return [&] { delivered.push_back(2); };
        }));
        queue.Stop();
        REQUIRE(!queue.Submit([] { return SerialTaskQueue::Completion{}; }));
        queue.Drain(false);
        REQUIRE(delivered.empty());
        release.set_value();
    }
    REQUIRE(off_ui);
    REQUIRE_EQ(work, (std::vector<int>{1, 2}));
    REQUIRE(delivered.empty()); // Destruction never invokes a callback into a closed UI.
}

TEST_CASE(settings_worker_delivers_results_only_on_owner_and_survives_errors)
{
    const auto owner = std::this_thread::get_id();
    std::promise<void> done;
    int errors = 0, delivered = 0;
    SerialTaskQueue queue([] {},
                          [&] {
                              REQUIRE(std::this_thread::get_id() == owner);
                              ++errors;
                          },
                          [&] { done.set_value(); });
    queue.Submit([]() -> SerialTaskQueue::Completion { throw std::runtime_error("test"); });
    queue.Submit([&] {
        return [&] {
            REQUIRE(std::this_thread::get_id() == owner);
            ++delivered;
        };
    });
    queue.Stop();
    REQUIRE(done.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE_EQ(errors, 0);
    REQUIRE_EQ(delivered, 0);
    queue.Drain();
    REQUIRE_EQ(errors, 1);
    REQUIRE_EQ(delivered, 1);
}

TEST_CASE(dictionary_pages_cover_ties_without_duplicates_or_unbounded_results)
{
    namespace json = boost::json;
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(":memory:", &db) == SQLITE_OK);
    struct Close
    {
        sqlite3 *db;
        ~Close()
        {
            sqlite3_close(db);
        }
    } close{db};
    REQUIRE(sqlite3_exec(db, "CREATE TABLE words(code TEXT,word TEXT PRIMARY KEY,weight INTEGER)", nullptr, nullptr,
                         nullptr) == SQLITE_OK);
    for (int i = 0; i < 205; ++i)
    {
        const auto sql = "INSERT INTO words VALUES('a','word" + std::to_string(i) + "',100)";
        REQUIRE(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    }
    std::set<std::string> seen;
    for (int offset : {0, 100, 200})
    {
        json::object request{{"offset", offset}, {"limit", 100}};
        const auto sql = "SELECT code,word,weight,COUNT(*) OVER() FROM words ORDER BY weight DESC,code,word" +
                         SettingsDictionary::Paging::Sql(request);
        sqlite3_stmt *stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
        auto result = SettingsDictionary::Paging::Read(stmt, request);
        sqlite3_finalize(stmt);
        REQUIRE(result.at("ok").as_bool());
        REQUIRE_EQ(result.at("offset").as_int64(), offset);
        REQUIRE_EQ(result.at("hasMore").as_bool(), offset < 200);
        REQUIRE_EQ(result.at("rows").as_array().size(), offset < 200 ? size_t{100} : size_t{5});
        REQUIRE_EQ(result.at("total").as_int64(), 205);
        for (const auto &row : result.at("rows").as_array())
            REQUIRE(seen.insert(json::value_to<std::string>(row.at("word"))).second);
    }
    REQUIRE_EQ(seen.size(), size_t{205});
    REQUIRE_EQ(SettingsDictionary::Paging::Limit({{"limit", 999999}}), 200);
}
