#include <winsock2.h>

#include "ai/ai_assistant.h"
#include "ai/chat_completions.h"
#include "tests/includes/test_framework.h"

#include <curl/curl.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>

namespace
{
using namespace std::chrono_literals;

class LocalChatServer
{
  public:
    explicit LocalChatServer(bool hold_first = false) : hold_first_(hold_first)
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            throw std::runtime_error("WSAStartup failed");
        const auto fail = [this](const char *message) {
            if (listener_ != INVALID_SOCKET)
                closesocket(listener_);
            WSACleanup();
            throw std::runtime_error(message);
        };
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET)
            fail("socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(listener_, 2) != 0)
            fail("bind or listen failed");
        int length = sizeof(address);
        if (getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &length) != 0)
            fail("getsockname failed");
        endpoint_ = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/v1/chat/completions";
        worker_ = std::thread([this] { Run(); });
    }

    ~LocalChatServer()
    {
        stop_ = true;
        ReleaseFirst();
        if (worker_.joinable())
            worker_.join();
        if (first_worker_.joinable())
            first_worker_.join();
        closesocket(listener_);
        WSACleanup();
    }

    const std::string &Endpoint() const
    {
        return endpoint_;
    }

    bool WaitForRequests(int count, std::chrono::milliseconds timeout = 5s)
    {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return accepted_ >= count; });
    }

    std::chrono::steady_clock::time_point SecondRequestAt()
    {
        std::lock_guard lock(mutex_);
        return second_accepted_at_;
    }

    void ReleaseFirst()
    {
        {
            std::lock_guard lock(mutex_);
            first_released_ = true;
        }
        cv_.notify_all();
    }

  private:
    void Reply(SOCKET client, int number)
    {
        if (hold_first_ && number == 1)
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return first_released_ || stop_; });
        }
        const std::string content = number == 1 && hold_first_ ? "old" : "new";
        const std::string body = "{\"choices\":[{\"message\":{\"content\":\"{\\\"candidates\\\":[{\\\"text\\\":\\\"" +
                                 content + "\\\"}]}\"}}]}";
        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body;
        send(client, response.data(), static_cast<int>(response.size()), 0);
        closesocket(client);
    }

    void Run()
    {
        while (!stop_)
        {
            fd_set readers;
            FD_ZERO(&readers);
            FD_SET(listener_, &readers);
            timeval timeout{0, 100000};
            if (select(0, &readers, nullptr, nullptr, &timeout) <= 0)
                continue;
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET)
                continue;
            char request[4096];
            recv(client, request, sizeof(request), 0);
            int number;
            {
                std::lock_guard lock(mutex_);
                number = ++accepted_;
                if (number == 2)
                    second_accepted_at_ = std::chrono::steady_clock::now();
            }
            cv_.notify_all();
            if (hold_first_ && number == 1)
                first_worker_ = std::thread([this, client, number] { Reply(client, number); });
            else
                Reply(client, number);
        }
    }

    SOCKET listener_ = INVALID_SOCKET;
    std::thread worker_;
    std::thread first_worker_;
    std::string endpoint_;
    bool hold_first_ = false;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    int accepted_ = 0;
    std::chrono::steady_clock::time_point second_accepted_at_{};
    bool first_released_ = false;
};

struct CurlScope
{
    CurlScope()
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    ~CurlScope()
    {
        curl_global_cleanup();
    }
};

struct AssistantScope
{
    ~AssistantScope()
    {
        AiAssistant::Stop();
    }
};

struct AfterFetchHookScope
{
    ~AfterFetchHookScope()
    {
        AiAssistant::testing::SetAfterFetchHook({});
    }
};
} // namespace

TEST_CASE(chat_request_without_cancellation_receives_content)
{
    CurlScope curl;
    LocalChatServer server;
    const auto result = ChatCompletions::FetchContent(server.Endpoint(), "", "{}", 5000);
    REQUIRE(result.failure == ChatCompletions::Failure::None);
    REQUIRE(result.content.has_value());
    REQUIRE(result.content->find("new") != std::string::npos);
}

TEST_CASE(chat_request_cancelled_before_transfer_skips_network)
{
    CurlScope curl;
    LocalChatServer server;
    const auto result = ChatCompletions::FetchContent(server.Endpoint(), "", "{}", 5000, [] { return true; });
    REQUIRE(result.failure == ChatCompletions::Failure::Cancelled);
    REQUIRE(!result.content.has_value());
    REQUIRE(!server.WaitForRequests(1, 200ms));
}

TEST_CASE(chat_request_cancelled_during_transfer_stops_before_timeout)
{
    CurlScope curl;
    LocalChatServer server(true);
    std::atomic<bool> cancelled{false};
    auto pending = std::async(std::launch::async, [&] {
        return ChatCompletions::FetchContent(server.Endpoint(), "", "{}", 8000, [&] { return cancelled.load(); });
    });
    const bool started = server.WaitForRequests(1);
    cancelled = true;
    const bool stopped = pending.wait_for(3s) == std::future_status::ready;
    server.ReleaseFirst();
    const auto result = pending.get();
    REQUIRE(started);
    REQUIRE(stopped);
    REQUIRE(result.failure == ChatCompletions::Failure::Cancelled);
    REQUIRE(!result.content.has_value());
    REQUIRE(ChatCompletions::DescribeFailure(result.failure, result.detail).empty());
}

TEST_CASE(chat_request_timeout_remains_distinct_from_cancellation)
{
    CurlScope curl;
    LocalChatServer server(true);
    const auto result = ChatCompletions::FetchContent(server.Endpoint(), "", "{}", 250, [] { return false; });
    server.ReleaseFirst();
    REQUIRE(result.failure == ChatCompletions::Failure::Timeout);
    REQUIRE(!result.content.has_value());
}

TEST_CASE(ai_assistant_discards_stale_transfer_and_retries_same_request)
{
    LocalChatServer server(true);
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> callbacks;
    AiAssistant::Start([&](const std::string &candidate, const std::string &, uint64_t) {
        {
            std::lock_guard lock(mutex);
            callbacks.push_back(candidate);
        }
        cv.notify_all();
    });
    AssistantScope assistant;
    AiAssistant::Request request;
    request.identity = "test";
    request.pinyin_segments = {"ni"};
    request.config.enabled = true;
    request.config.provider = "custom";
    request.config.token = "test-token";
    request.config.endpoint = server.Endpoint();
    request.config.model = "test-model";
    request.config.prompt = "test";
    AiAssistant::OnInputChanged(request);
    const bool first_started = server.WaitForRequests(1);
    AiAssistant::Clear();
    const auto next_input_at = std::chrono::steady_clock::now();
    AiAssistant::OnInputChanged(request);
    const bool second_started = server.WaitForRequests(2);
    server.ReleaseFirst();
    std::unique_lock lock(mutex);
    const bool received = cv.wait_for(lock, 5s, [&] { return !callbacks.empty(); });
    REQUIRE(first_started);
    REQUIRE(second_started);
    REQUIRE(server.SecondRequestAt() - next_input_at >= 500ms);
    REQUIRE(received);
    REQUIRE(callbacks.size() == 1);
    REQUIRE(callbacks.front() == "new");
}

TEST_CASE(ai_assistant_discards_completed_reply_after_generation_changes)
{
    LocalChatServer server(true);
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool old_fetched = false;
    bool resume_worker = false;
    AiAssistant::testing::SetAfterFetchHook([&](const std::string &candidate) {
        if (candidate != "old")
            return;
        std::unique_lock lock(gate_mutex);
        old_fetched = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return resume_worker; });
    });
    AfterFetchHookScope hook;

    std::mutex callback_mutex;
    std::condition_variable callback_cv;
    std::vector<std::string> callbacks;
    AiAssistant::Start([&](const std::string &candidate, const std::string &, uint64_t) {
        {
            std::lock_guard lock(callback_mutex);
            callbacks.push_back(candidate);
        }
        callback_cv.notify_all();
    });
    AssistantScope assistant;
    AiAssistant::Request request;
    request.identity = "same-cache-key";
    request.pinyin_segments = {"ni"};
    request.config.enabled = true;
    request.config.provider = "custom";
    request.config.token = "test-token";
    request.config.endpoint = server.Endpoint();
    request.config.model = "test-model";
    request.config.prompt = "test";
    AiAssistant::OnInputChanged(request);
    const bool first_started = server.WaitForRequests(1);
    server.ReleaseFirst();
    bool old_completed;
    {
        std::unique_lock lock(gate_mutex);
        old_completed = gate_cv.wait_for(lock, 5s, [&] { return old_fetched; });
    }
    // The old HTTP result is complete. Change the generation before cache insertion.
    AiAssistant::OnInputChanged(request);
    {
        std::lock_guard lock(gate_mutex);
        resume_worker = true;
    }
    gate_cv.notify_all();

    const bool second_started = server.WaitForRequests(2);
    std::unique_lock lock(callback_mutex);
    const bool received = callback_cv.wait_for(lock, 5s, [&] { return !callbacks.empty(); });
    REQUIRE(first_started);
    REQUIRE(old_completed);
    REQUIRE(second_started);
    REQUIRE(received);
    REQUIRE(callbacks.size() == 1);
    REQUIRE(callbacks.front() == "new");
}

TEST_CASE(ai_assistant_stop_cancels_active_transfer)
{
    LocalChatServer server(true);
    std::atomic<int> callbacks{0};
    AiAssistant::Start([&](const std::string &, const std::string &, uint64_t) { ++callbacks; });
    AssistantScope assistant;
    AiAssistant::Request request;
    request.identity = "test";
    request.pinyin_segments = {"ni"};
    request.config.enabled = true;
    request.config.provider = "custom";
    request.config.token = "test-token";
    request.config.endpoint = server.Endpoint();
    request.config.model = "test-model";
    AiAssistant::OnInputChanged(request);
    const bool started = server.WaitForRequests(1);
    auto stopping = std::async(std::launch::async, [] { AiAssistant::Stop(); });
    const bool stopped = stopping.wait_for(3s) == std::future_status::ready;
    server.ReleaseFirst();
    stopping.get();
    REQUIRE(started);
    REQUIRE(stopped);
    REQUIRE(callbacks == 0);
}
