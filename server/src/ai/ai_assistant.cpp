#include "ai_assistant.h"
#include "ai_assistant_cache.h"
#include "ai_assistant_cache_key.h"
#include "ai_assistant_request.h"
#include "chat_completions.h"
#include "cloud/ai_wire.h"
#include "jev_client.h"

#include <msime/voice/provider_protocol.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <fmt/format.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
constexpr auto kIdleDelay = std::chrono::milliseconds(650);
auto IdleDelay(const AiAssistant::Request &request)
{
    return request.config.provider == "jev" ? std::chrono::milliseconds(200) : kIdleDelay;
}
std::mutex g_mutex;
std::condition_variable g_cv;
std::thread g_worker;
std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_generation{0};
AiAssistant::Request g_latest;
std::chrono::steady_clock::time_point g_last_input;
AiAssistant::ApplyCallback g_callback;
AiAssistant::detail::SuggestionCache g_candidate_cache;

std::string Fetch(const AiAssistant::Request &request, uint64_t generation, Jev::Client &jev_client)
{
    const auto &config = request.config;
    const std::string token = AiAssistant::RequestToken(config.provider, config.token);
    // 自建或转发的选择器可以不要 Key（如 OpenCode Zen 的免费模型），Jev 的 Token 交给 Jev::Client 判断；
    // Ollama 这类本机服务本来就不鉴权，ProviderUsesToken 直接放行。
    const bool token_missing =
        token.empty() && config.provider != "jev" && AiAssistant::ProviderUsesToken(config.provider);
    if (!config.enabled || token_missing || config.endpoint.empty() || config.model.empty() ||
        request.pinyin_segments.empty() || g_generation.load() != generation)
    {
        (void)0;
        return {};
    }

    if (config.provider == "jev")
    {
        // A single candidate leaves nothing to choose; skip sending the context off-device.
        if (request.candidates.size() < 2)
            return {};
        try
        {
            const auto body =
                Jev::BuildRequest(config.model, request.context, request.pinyin_segments, request.candidates);
            const auto response = jev_client.Fetch(config.endpoint, config.token, body.dump(), [generation] {
                return !g_running || g_generation.load() != generation;
            });
            return response ? Jev::ParseChoice(*response, request.candidates) : std::string{};
        }
        catch (const std::exception &)
        {
            return {};
        }
    }
    // 设置页里的 provider 现在可能是协议 id（responses / anthropic / gemini）：
    // Chat 沿用原来的厂商特判，其余三种用统一构造的四协议请求体。
    const auto wire = AiWire::FromId(config.provider);
    nlohmann::json body;
    if (wire == AiWire::Wire::Chat)
    {
        body = AiAssistant::detail::BuildRequestBody(request);
    }
    else
    {
        nlohmann::json input = {{"segmented_pinyin", request.pinyin_segments},
                                {"context", request.context},
                                {"candidate_limit", config.candidate_limit}};
        body = AiWire::BuildBody(wire, config.model, config.prompt, input.dump(), 1024, 0.2);
    }
    const std::string url = AiWire::NormalizeEndpoint(config.endpoint, wire, config.model);
    const auto outcome = AiWire::Send(url, wire, token, body.dump(), AiAssistant::detail::RequestTimeoutMs(config));
    if (!outcome.ok || outcome.status < 200 || outcome.status >= 300)
        return {};
    const std::string content = AiWire::ExtractText(wire, outcome.body);
    if (content.empty() || g_generation.load() != generation)
        return {};

    try
    {
        // MiniMax M3.1 这类强制思考的模型会把思考过程混进 content（<think>…</think>），
        // 不剥掉的话下面的 JSON 解析直接失败，联想候选就静默消失。
        const auto result_json = nlohmann::json::parse(metasequoia::voice::strip_reasoning(content));
        const auto &candidates = result_json.at("candidates");
        if (!candidates.is_array() || candidates.empty())
        {
            (void)0;
            return {};
        }
        const std::string candidate = candidates.at(0).value("text", std::string{});
        (void)0;
        return candidate;
    }
    catch (const std::exception &)
    {
        return {};
    }
}

void WorkerLoop()
{
    Jev::Client jev_client;
    uint64_t observed = 0;
    while (g_running)
    {
        std::unique_lock lock(g_mutex);
        g_cv.wait(lock, [&] { return !g_running || g_generation.load() != observed; });
        if (!g_running)
            break;
        observed = g_generation.load();
        auto request = g_latest;
        const auto cached = g_candidate_cache.Find(AiAssistant::detail::BuildSuggestionCacheKey(request));
        if (!request.identity.empty() && cached.has_value())
        {
            const uint64_t cached_generation = observed;
            lock.unlock();
            (void)0;
            if (g_running && g_generation.load() == cached_generation && g_callback)
                g_callback(*cached, request.identity, cached_generation);
            continue;
        }
        auto target = g_last_input + IdleDelay(request);
        while (g_running &&
               g_cv.wait_until(lock, target, [&] { return !g_running || g_generation.load() != observed; }))
        {
            if (!g_running)
                return;
            observed = g_generation.load();
            request = g_latest;
            target = g_last_input + IdleDelay(request);
        }
        lock.unlock();
        if (request.identity.empty())
            continue;
        const std::string candidate = Fetch(request, observed, jev_client);
        if (!candidate.empty() && g_generation.load() == observed && g_callback)
        {
            {
                std::lock_guard cache_lock(g_mutex);
                g_candidate_cache.Store(AiAssistant::detail::BuildSuggestionCacheKey(request), candidate);
            }
            (void)0;
            g_callback(candidate, request.identity, observed);
        }
    }
}
} // namespace

namespace AiAssistant
{
void Start(ApplyCallback callback)
{
    if (g_running)
        return;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_callback = std::move(callback);
    g_running = true;
    g_worker = std::thread(WorkerLoop);
}
void Stop()
{
    if (!g_running)
        return;
    g_running = false;
    g_cv.notify_all();
    if (g_worker.joinable())
        g_worker.join();
    {
        std::lock_guard lock(g_mutex);
        g_candidate_cache.Clear();
    }
    curl_global_cleanup();
}
void OnInputChanged(Request request)
{
    {
        std::lock_guard lock(g_mutex);
        g_latest = std::move(request);
        g_last_input = std::chrono::steady_clock::now();
        ++g_generation;
        (void)0;
    }
    g_cv.notify_one();
}
void Clear()
{
    OnInputChanged({});
}
} // namespace AiAssistant
