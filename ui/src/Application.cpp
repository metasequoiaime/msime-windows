#include "msimeui/Application.h"

#include "msimeui/Compat.h"

#include <objbase.h>
#include <windows.h>

namespace msimeui
{
bool Application::Initialize()
{
    compat::SetProcessDpiAwarenessPerMonitorV2();
    return SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
}

void Application::Shutdown()
{
    CoUninitialize();
}
} // namespace msimeui
