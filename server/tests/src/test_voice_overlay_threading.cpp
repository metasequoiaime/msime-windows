#include "config/ime_config.h"
#include "voice-input/wave_overlay.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

namespace
{
DWORD ui_thread = 0;
std::atomic<int> theme_reads{0};
std::atomic<bool> theme_read_off_ui{false};

void require(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << std::endl;
        // Do not join a worker blocked in a cross-thread window call on regression.
        std::_Exit(1);
    }
}

template <typename Work> void while_ui_blocked(Work work)
{
    std::promise<void> finished;
    auto result = finished.get_future();
    std::thread worker([&] {
        work();
        finished.set_value();
    });
    // Deliberately do not pump messages, as when UI cancel waits for g_send_mutex.
    require(result.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
            "overlay call waited for the UI thread");
    worker.join();
}

void dispatch_pending(HWND hwnd)
{
    MSG message{};
    while (PeekMessageW(&message, hwnd, 0, 0, PM_REMOVE))
        DispatchMessageW(&message);
}
} // namespace

// Fixed theme isolates window dispatch from configuration files and the installed IME.
bool ReloadImeConfigIfChanged()
{
    ++theme_reads;
    if (GetCurrentThreadId() != ui_thread)
        theme_read_off_ui.store(true);
    return false;
}

const std::string &GetConfiguredThemeVoice()
{
    static const std::string theme = "light";
    return theme;
}

std::string ResolveConfiguredTheme(const std::string &theme)
{
    return theme;
}

int main()
{
    ui_thread = GetCurrentThreadId();
    WaveOverlay overlay;
    require(overlay.init(GetModuleHandleW(nullptr), {}), "overlay initialization failed");
    HWND hwnd = nullptr;
    EnumThreadWindows(
        ui_thread,
        [](HWND candidate, LPARAM result) -> BOOL {
            wchar_t name[64]{};
            GetClassNameW(candidate, name, 64);
            if (std::wstring(name) != L"MviWaveOverlayWindow")
                return TRUE;
            *reinterpret_cast<HWND *>(result) = candidate;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&hwnd));
    require(hwnd != nullptr, "overlay window not found");
    // Convert the initially hidden window before any show request. Even show-path tests
    // cannot put a window on the desktop, change focus or interfere with user input.
    SetParent(hwnd, HWND_MESSAGE);
    require(FindWindowExW(HWND_MESSAGE, nullptr, L"MviWaveOverlayWindow", nullptr) == hwnd,
            "overlay was not converted to a message-only window");

    while_ui_blocked([&] { overlay.show(); });
    require(theme_reads.load() == 0, "show touched UI-owned state before message dispatch");
    dispatch_pending(hwnd);
    require(theme_reads.load() > 0, "UI did not apply show request");
    require(!theme_read_off_ui.load(), "theme/render preparation ran off the UI thread");

    while_ui_blocked([&] { overlay.hide(); });
    dispatch_pending(hwnd);
    require(!IsWindowVisible(hwnd), "UI did not apply hide request");

    const int reads_before = theme_reads.load();
    while_ui_blocked([&] {
        overlay.show();
        overlay.hide();
    });
    dispatch_pending(hwnd);
    require(theme_reads.load() == reads_before && !IsWindowVisible(hwnd),
            "queued show revived an overlay hidden by a newer request");

    while_ui_blocked([&] {
        overlay.hide();
        overlay.show();
    });
    dispatch_pending(hwnd);
    require(theme_reads.load() > reads_before, "queued hide suppressed a newer show request");
    overlay.hide();
    dispatch_pending(hwnd);
    overlay.shutdown();
    std::cout << "overlay dispatch is nonblocking and applies the latest visibility on the UI thread\n";
    return 0;
}
