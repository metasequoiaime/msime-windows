#pragma once

// TSF 拿不到组字位置时的兜底（#618）：Firefox 地址栏文字溢出后，GetTextExt 对仍然可见的组字区域
// 返回全零矩形，DLL 只能送来 (0, INVALID_Y)。这里改从宿主的系统光标、MSAA 光标对象或 UIA
// 文本模式读光标位置。这些调用都跨进程、可能被宿主卡住，所以跑在独立线程上，结果投递回调用方窗口。
#include <windows.h>
#include <cstdint>
#include <string>

namespace CaretFallback
{
enum class Source
{
    None,
    SystemCaret,
    MsaaCaret,
    UiaCaretRange,
    UiaSelection,
};

struct Result
{
    uint32_t generation = 0;
    Source source = Source::None;
    // 物理屏幕坐标，与 DLL 送来的锚点一致：光标左下角
    POINT anchor{};
    ULONGLONG elapsedMs = 0;
    // 每一级尝试的结果，只记状态和坐标，不含任何文本
    std::wstring trace;
};

// 只在前台窗口属于 clientProcessId 时才查询。完成后向 notifyWindow 投递 notifyMessage，
// wParam 是 generation；用 TakeResult 取结果。后发的请求覆盖还没开始执行的旧请求。
void Request(HWND notifyWindow, UINT notifyMessage, uint32_t generation, DWORD clientProcessId);

// 取出 generation 对应的结果；不是这一代的结果返回 false
bool TakeResult(uint32_t generation, Result *result);

const wchar_t *SourceName(Source source);
} // namespace CaretFallback
