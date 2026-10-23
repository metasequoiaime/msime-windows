#include "utils/serial_task_runner.h"

#include <windows.h>

#include <utility>

SerialTaskRunner::SerialTaskRunner(bool below_normal_priority)
    : below_normal_priority_(below_normal_priority), thread_([this] { Run(); })
{
}

SerialTaskRunner::~SerialTaskRunner()
{
    Stop();
}

void SerialTaskRunner::Post(std::function<void()> job)
{
    if (!job)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
        {
            return;
        }
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void SerialTaskRunner::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    if (thread_.joinable())
    {
        thread_.join();
    }
}

void SerialTaskRunner::SetIdleHandler(std::function<void()> handler)
{
    std::lock_guard<std::mutex> lock(mutex_);
    idle_handler_ = std::move(handler);
}

void SerialTaskRunner::Run()
{
    if (below_normal_priority_)
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    }
    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            // Drain before exiting: a queued learning write is the user's
            // selection and must not be lost at shutdown.
            if (jobs_.empty())
            {
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        try
        {
            job();
        }
        catch (...)
        {
        }
        // Housekeeping runs only when this instant finds the queue empty, which in practice means the caller has
        // nothing more queued and the user has stopped typing. It is copied out under the lock and run outside it so a
        // job posted meanwhile is still picked up immediately.
        std::function<void()> idle;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stopping_ && jobs_.empty())
            {
                idle = idle_handler_;
            }
        }
        if (idle)
        {
            try
            {
                idle();
            }
            catch (...)
            {
            }
        }
    }
}
