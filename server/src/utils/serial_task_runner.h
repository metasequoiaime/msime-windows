#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

// Runs posted jobs one at a time, in posting order, on a single background
// thread. Used for work that must stay ordered but must not hold up the thread
// that produces it -- the IME worker hands dictionary learning writes here so a
// slow disk no longer delays the next keystroke.
class SerialTaskRunner
{
  public:
    // below_normal_priority lowers the worker thread's scheduling priority so
    // background writes never compete with input handling for the CPU.
    explicit SerialTaskRunner(bool below_normal_priority = false);
    ~SerialTaskRunner();

    SerialTaskRunner(const SerialTaskRunner &) = delete;
    SerialTaskRunner &operator=(const SerialTaskRunner &) = delete;

    // Queues a job. Ignored after Stop(). Jobs must not throw; an exception is
    // caught and dropped so one failing job cannot stop the ones behind it.
    void Post(std::function<void()> job);

    // Runs every job already queued, then joins the thread. Idempotent; also
    // called by the destructor.
    void Stop();

    // Called on the runner's own thread each time it goes idle, i.e. right after the queue empties. Used to move
    // background work that benefits from a quiet moment (a database checkpoint) off the caller's path.
    void SetIdleHandler(std::function<void()> handler);

  private:
    void Run();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    bool stopping_ = false;
    bool below_normal_priority_ = false;
    std::function<void()> idle_handler_;
    std::thread thread_;
};
