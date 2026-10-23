#include "utils/serial_task_runner.h"
#include "tests/includes/test_framework.h"

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

TEST_CASE(serial_task_runner_runs_jobs_in_posting_order)
{
    std::vector<int> order;
    {
        SerialTaskRunner runner;
        for (int i = 0; i < 100; ++i)
        {
            // Only the runner thread touches `order`, so no lock is needed.
            runner.Post([&order, i] { order.push_back(i); });
        }
    }
    REQUIRE(order.size() == 100);
    for (int i = 0; i < 100; ++i)
    {
        REQUIRE(order[i] == i);
    }
}

TEST_CASE(serial_task_runner_stop_drains_queued_jobs)
{
    std::atomic<int> done{0};
    SerialTaskRunner runner(/*below_normal_priority=*/true);
    // Hold the thread busy so the rest are still queued when Stop() arrives.
    runner.Post([&done] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ++done;
    });
    for (int i = 0; i < 10; ++i)
    {
        runner.Post([&done] { ++done; });
    }
    runner.Stop();
    REQUIRE(done.load() == 11);
    // Posting after Stop() is ignored rather than run or crashing.
    runner.Post([&done] { ++done; });
    runner.Stop();
    REQUIRE(done.load() == 11);
}

TEST_CASE(serial_task_runner_survives_a_throwing_job)
{
    std::atomic<int> done{0};
    {
        SerialTaskRunner runner;
        runner.Post([] { throw std::runtime_error("write failed"); });
        runner.Post([&done] { ++done; });
    }
    REQUIRE(done.load() == 1);
}

TEST_CASE(serial_task_runner_runs_the_idle_handler_only_when_the_queue_empties)
{
    std::atomic<int> idle_runs{0};
    std::atomic<int> jobs_done{0};
    // Signalled from the runner thread the first time it goes idle, so the test knows the handler has run instead of
    // waiting on a clock and racing Stop() against the runner's own idle check.
    std::promise<void> idle_ran;
    std::future<void> idle_seen = idle_ran.get_future();
    SerialTaskRunner runner;
    runner.SetIdleHandler([&idle_runs, &idle_ran] {
        if (++idle_runs == 1)
        {
            try
            {
                idle_ran.set_value();
            }
            catch (...)
            {
                // Already satisfied; the handler is allowed to run more than once.
            }
        }
    });
    // Hold the thread busy first, so the handler cannot run between the posts below.
    runner.Post([&jobs_done] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ++jobs_done;
    });
    for (int i = 0; i < 10; ++i)
    {
        runner.Post([&jobs_done] { ++jobs_done; });
    }
    // Stop() sets stopping_, and the runner returns instead of running the idle handler once it is stopping, so the
    // handler has to be observed before the queue is torn down.
    REQUIRE(idle_seen.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    runner.Stop();
    REQUIRE(jobs_done.load() == 11);
    REQUIRE(idle_runs.load() >= 1);
    // At most once per drain, and there are only the 11 jobs here: the handler must never outnumber them.
    REQUIRE(idle_runs.load() <= 11);
}

TEST_CASE(serial_task_runner_survives_a_throwing_idle_handler)
{
    std::atomic<int> done{0};
    {
        SerialTaskRunner runner;
        runner.SetIdleHandler([] { throw std::runtime_error("checkpoint failed"); });
        runner.Post([&done] { ++done; });
        runner.Post([&done] { ++done; });
    }
    REQUIRE(done.load() == 2);
}

TEST_CASE(serial_task_runner_does_not_run_jobs_on_the_posting_thread)
{
    const std::thread::id poster = std::this_thread::get_id();
    std::thread::id runner_thread;
    {
        SerialTaskRunner runner;
        runner.Post([&runner_thread] { runner_thread = std::this_thread::get_id(); });
    }
    REQUIRE(runner_thread != std::thread::id());
    REQUIRE(runner_thread != poster);
}
