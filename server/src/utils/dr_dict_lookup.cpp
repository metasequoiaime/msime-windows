#include "utils/dr_dict_lookup.h"

#include "utils/dr_dict_lookup_policy.h"

#include <objbase.h>
#include <shellapi.h>

#include <thread>
#include <utility>

namespace DrDictLookup
{
namespace
{
// 做法与 DR Reader 的「查词典」一致（dr-reader app/src/application.cpp 的 lookup_in_dr_dict）：
// 已运行的 DR Dict 经 WM_COPYDATA 交给它的弹窗宿主；否则用 --lookup 启动它。
void Lookup(const std::wstring &word, const Placement &placement)
{
    if (HWND popup = FindWindowW(kPopupWindowClass, nullptr))
    {
        DWORD process = 0;
        GetWindowThreadProcessId(popup, &process);
        // 弹窗要拿到焦点：前台进程（候选右键时就是本进程，快捷键时是 TSF 所在的宿主，TSF 那边也放行了一次）
        // 才有资格放行。
        if (process != 0)
            AllowSetForegroundWindow(process);
        const std::string payload = BuildCopyDataPayload(word, placement);
        COPYDATASTRUCT data{static_cast<ULONG_PTR>(kLookupRequest), static_cast<DWORD>(payload.size()),
                            const_cast<char *>(payload.data())};
        DWORD_PTR handled = 0;
        if (SendMessageTimeoutW(popup, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 3000,
                                &handled) &&
            handled)
        {
            return;
        }
    }

    // ShellExecuteEx 要求调用线程初始化 COM（App Paths 解析、关联程序都可能走 shell 扩展）。
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const std::wstring arguments = BuildLaunchArguments(word, placement);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    info.lpFile = kExecutable;
    info.lpParameters = arguments.c_str();
    info.nShow = SW_SHOWNORMAL;
    // 没装或从没运行过 DR Dict 时 App Paths 里没有登记，这里静默失败：设置页的说明已经写明要先运行一次。
    (void)ShellExecuteExW(&info);
    if (SUCCEEDED(com))
        CoUninitialize();
}

void LookupAsync(std::wstring word, Placement placement)
{
    word = NormalizeWord(word);
    if (word.empty())
        return;
    try
    {
        std::thread([word = std::move(word), placement = std::move(placement)]() { Lookup(word, placement); }).detach();
    }
    catch (...)
    {
        // 起不了线程就放弃这一次查询，不能让异常落到窗口过程或按键工作线程里。
    }
}
} // namespace

void LookupAtPointer(std::wstring word, POINT at)
{
    Placement placement;
    placement.x = at.x;
    placement.y = at.y;
    LookupAsync(std::move(word), std::move(placement));
}

void LookupFromKeyboard(std::wstring word, std::optional<RECT> caret)
{
    Placement placement;
    placement.keyboard = true;
    if (caret)
    {
        placement.caret = CaretRect{caret->left, caret->top, caret->right, caret->bottom};
        // 旧版 DR Dict 不认 caret，只按 x/y 开：给它光标行的左下角，也比鼠标位置接近。
        placement.x = caret->left;
        placement.y = caret->bottom;
    }
    else
    {
        POINT at{};
        GetCursorPos(&at);
        placement.x = at.x;
        placement.y = at.y;
    }
    LookupAsync(std::move(word), std::move(placement));
}
} // namespace DrDictLookup
