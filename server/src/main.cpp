#include "ipc/event_listener.h"
#include "log/fanylog.h"
#include "window/ime_windows.h"
#include <commctrl.h>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include "ipc/ipc.h"
#include "config/ime_config.h"
#include "conversion/chinese_converter.h"
#include "clipboard/clipboard_history.h"
#include <windows.h>
#include <fmt/xchar.h>
#include <spdlog/spdlog.h>
#include "cloud/cloud_ime.h"
#include "cloud/cloud_translation.h"
#include "ai/ai_assistant.h"
#include "english/english_ime.h"
#include "mixed/mixed_candidates.h"
#include "engine/neural/rescore_worker.h"
#include "engine/contracts/assets/assets.h"
#include "engine/core/runtime_paths.h"
#include "engine/local_modes/radical_query.h"
#include "global/globals.h"
#include "utils/common_utils.h"
#include "utils/hanzi_pinyin.h"
#include "utils/process_priority.h"
#include "utils/single_instance.h"
#include "session/session_factory.h"
#include "settings/collocation_model.h"
#include "settings/update_check.h"
#include "statistics/stats_pipe.h"
#include "voice-input/voice_input_service.h"

namespace
{
void StartWatchdogIfNeeded(const char *command_line)
{
    // The Watchdog passes this marker when it owns the Server lifecycle.
    // A Server revived directly by TSF has no marker and restores the
    // Watchdog, preserving crash recovery after the six-keystroke fallback.
    if (command_line && std::strstr(command_line, "--watchdog-managed"))
        return;

    wchar_t server_path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, server_path, MAX_PATH))
        return;
    std::wstring watchdog_path(server_path);
    const size_t separator = watchdog_path.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
        return;
    watchdog_path.replace(separator + 1, std::wstring::npos, L"MetasequoiaImeWatchdog.exe");
    if (GetFileAttributesW(watchdog_path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;

    std::wstring command = L"\"" + watchdog_path + L"\"";
    STARTUPINFOW startup_info{sizeof(startup_info)};
    PROCESS_INFORMATION process_info{};
    if (CreateProcessW(watchdog_path.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                       &startup_info, &process_info))
    {
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
    }
}

bool IsPipeProbe(const char *command_line)
{
    return command_line && std::strstr(command_line, "--pipe-probe");
}

const char *SchemeTypeToString(SchemeType scheme_type)
{
    switch (scheme_type)
    {
    case SchemeType::Quanpin:
        return "quanpin";
    case SchemeType::Shuangpin:
        return "shuangpin";
    case SchemeType::Wubi:
        return "wubi";
    case SchemeType::JapaneseRomaji:
        return "japanese-romaji";
    default:
        return "unknown";
    }
}

} // namespace

int CALLBACK WinMain(_In_ HINSTANCE hInstance, _In_ HINSTANCE /*hPrevInstance*/, _In_ LPSTR lpCmdLine,
                     _In_ int /*nCmdShow*/)
{
    const bool pipe_probe = IsPipeProbe(lpCmdLine);
    CommonUtils::SingleInstanceGuard single_instance(L"Local\\MetasequoiaImeServer_SingleInstance");
    if (!single_instance.is_valid())
    {
        (void)0;
        return 0;
    }
    if (single_instance.already_running())
    {
        (void)0;
        return 0;
    }

    if (!pipe_probe)
        StartWatchdogIfNeeded(lpCmdLine);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult))
    {
        (void)0;
        return 1;
    }

    // #ifdef FANY_DEBUG
    // Initialize for logging
    InitializeSpdLog();
    // #endif

    // 登录任务的默认 Priority 是 7（Below normal），watchdog 和它拉起的 Server 会继承这个
    // 优先级；TSF、手工启动和以后的拉起路径也可能带进来。启动时统一复位，失败只记 warning，
    // 不阻止 Server 启动。SetPriorityClass 不会恢复内存优先级，两项必须分开设置。
    const ProcessPriority::PriorityRestoreResult priority_result = ProcessPriority::RestoreInteractiveProcessPriority();
    if (!priority_result.cpu_ok)
    {
        spdlog::warn("Failed to restore NORMAL_PRIORITY_CLASS (Win32 error {})", priority_result.cpu_error);
    }
    if (!priority_result.memory_ok)
    {
        spdlog::warn("Failed to restore MEMORY_PRIORITY_NORMAL (Win32 error {})", priority_result.memory_error);
    }

    // Initialize config
    InitImeConfig();
    // 上一次退出时未能落位的模型更新在这里生效：必须早于任何会话创建（此时还没有 .gram 映射）。
    collocation::ApplyPendingUpdates();
    VoiceInput::Initialize();
    Global::candidate_ui.page_size = GetConfiguredCandidatePageSize();

    ::InitIpc();
    ::InitNamedPipe();
    g_inputSession = CreateInputSessionFromConfig();
    const std::string configured_backend = DescribeConfiguredInputSessionBackendFromConfig();
    const std::string effective_backend = DescribeEffectiveInputSessionBackendFromConfig();
    const std::string session_summary =
        fmt::format("Input session backend configured={}, effective={}, scheme={}",
                    configured_backend.empty() ? "legacy" : configured_backend, effective_backend,
                    SchemeTypeToString(g_inputSession->current_scheme_type()));
    (void)0;
#ifdef FANY_DEBUG
    (void)0;
#endif

    if (!pipe_probe)
    {
        RegisterCandidateWindowMessage();

        WNDCLASSEX wcex;
        RegisterIMEWindowsClass(wcex, hInstance);
    }

    //
    // Pipe
    //
    /* Named Pipe for IPC between tsf and server */
    std::thread pipe_worker(FanyNamedPipe::WorkerThread);
    std::thread pipe_listener(FanyNamedPipe::EventListenerLoopThread);
    /* Named Pipe for IPC, used to pass data to TSF */
    std::thread to_tsf_pipe_listener(FanyNamedPipe::ToTsfPipeEventListenerLoopThread);
    ::mainPipeThread = pipe_listener.native_handle();
    ::toTsfPipeThread = to_tsf_pipe_listener.native_handle();
    /* To Tsf Worker Thread Named Pipe for IPC, used to pass data to TSF */
    std::thread to_tsf_worker_thread_pipe_listener(FanyNamedPipe::ToTsfWorkerThreadPipeEventListenerLoopThread);
    ::toTsfWorkerThreadPipeThread = to_tsf_worker_thread_pipe_listener.native_handle();
    /* Aux Named Pipe: session-less UI notices (e.g. langbar right-click menu) */
    std::thread aux_pipe_listener(FanyNamedPipe::AuxPipeEventListenerLoopThread);
    /* Short-lived TSF diagnostic batches; isolated from all control pipes. */
    std::thread tsf_diagnostic_pipe_listener(FanyNamedPipe::TsfDiagnosticPipeEventListenerLoopThread);
    /* Input statistics frames: one-shot per flush, validated before aggregation. */
    std::thread stats_pipe_listener(MsimeStats::StatsPipeEventListenerLoopThread);

    if (pipe_probe)
    {
        // The release runner is a Windows service without an interactive
        // window station. The pipe probe still exercises the real Server and
        // all IPC worker threads, but must not create candidate windows or
        // start UI-bound background services.
        Sleep(INFINITE);
    }

    CloudIme::Start([](const std::string &candidate, const std::string &pinyin, uint64_t generation) {
        FanyNamedPipe::EnqueueCloudCandidate(candidate, pinyin, generation);
    });
    AiAssistant::Start([](const std::string &candidate, const std::string &identity, uint64_t generation) {
        FanyNamedPipe::EnqueueAiCandidate(candidate, identity, generation);
    });
    // 神经整句重排：打分在引擎自己的后台线程上跑，算完通过这个回调回到任务队列，由队列去重查候选。
    // 不 Start 什么线程——没开这个开关的用户不会平白多一条。
    neural::RescoreWorker::instance().set_ready_callback([] { FanyNamedPipe::EnqueueRescoredCandidates(); });
    EnglishIme::Start(
        CommonUtils::get_ime_data_path() + "\\english.db",
        [](std::vector<WordItem> candidates, const std::string &input, uint64_t generation) {
            FanyNamedPipe::EnqueueEnglishCandidates(std::move(candidates), input, generation);
        },
        [](std::vector<EnglishIme::TranslationResult> results, uint64_t generation) {
            FanyNamedPipe::EnqueueCandidateTranslations(std::move(results), generation);
        });
    CloudTranslation::Start(CommonUtils::get_ime_data_path() + "\\english.db",
                            [](std::vector<EnglishIme::TranslationResult> results, uint64_t generation) {
                                FanyNamedPipe::EnqueueCandidateTranslations(std::move(results), generation, true);
                            });
    MixedCandidates::Start(CommonUtils::get_ime_data_path() + "\\english.db",
                           CommonUtils::get_ime_data_path() + "\\others.db",
                           [](MixedCandidates::Result result, const std::string &input, uint64_t generation) {
                               FanyNamedPipe::EnqueueMixedCandidates(std::move(result), input, generation);
                           });
    // U 模式第一次查询要解析十几万行的拆字表、加载 cpp-pinyin 词典，放在按键回复里会卡住 Shift+U
    // 之后的第一个字母。没开这个模式的用户不多读这两份数据。
    std::thread radical_warmup;
    if (GetConfiguredRadicalModeEnabled())
    {
        radical_warmup = std::thread([] {
            metasequoia::local_modes::preload_radical_table(
                metasequoia::RuntimePaths::legacy().resource(metasequoia::assets::radical_decomposition));
            (void)HanziPinyin::Engine();
        });
    }

    // 繁体模式的候选要按 s2t、t2tw、t2hk 与单字字表展开写法，几份字典第一次载入同样不该落在按键上。
    // 运行中切到繁体时也走同一条后台线程，进程内只起一次。
    if (GetConfiguredCharacterSet() == "traditional")
        ChineseConverter::PreloadInBackground();

    int ret = CreateCandidateWindow(hInstance);
    if (radical_warmup.joinable())
        radical_warmup.join();
    ChineseConverter::JoinBackgroundPreload();

    // 自动下载安装包的后台轮询。只在开着输入法进程时跑；用户关掉设置窗口不影响它，
    // 这正是把下载放在 Server 而不是设置进程的原因。
    update_check::StartPolling();

    FanyNamedPipe::RegisterStatusSnapshotWindow(nullptr);

    EnglishIme::Stop();
    CloudTranslation::Stop();
    MixedCandidates::Stop();
    AiAssistant::Stop();
    CloudIme::Stop();
    ClipboardMonitor::Stop();
    VoiceInput::Shutdown();

    pipe_running = false;
    pipe_queueCv.notify_one();
    pipe_worker.join();
    FanyNamedPipe::ShutdownDictionaryWriter();
    pipe_listener.join();
    // To Tsf Pipe
    to_tsf_pipe_listener.join();
    to_tsf_worker_thread_pipe_listener.join();
    aux_pipe_listener.join();
    tsf_diagnostic_pipe_listener.join();
    stats_pipe_listener.join();
    MsimeStats::ShutdownStatsPipe();

    ::CloseIpc();
    CoUninitialize();
    return ret;
}
