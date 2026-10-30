#include "utils/process_priority.h"

#include <cstdio>
#include <windows.h>

namespace
{
constexpr int kSkipExitCode = 77;

// Returns false on failure. MEMORY_PRIORITY_LOWEST is a valid 0 on current SDKs, so 0 cannot be a
// failure sentinel.
bool ReadMemoryPriority(DWORD &priority)
{
    MEMORY_PRIORITY_INFORMATION info{};
    if (!GetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority, &info, sizeof(info)))
        return false;
    priority = info.MemoryPriority;
    return true;
}

bool WriteMemoryPriority(DWORD priority)
{
    MEMORY_PRIORITY_INFORMATION info{};
    info.MemoryPriority = priority;
    return SetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority, &info, sizeof(info)) != FALSE;
}
} // namespace

int main()
{
    const DWORD original_class = GetPriorityClass(GetCurrentProcess());
    DWORD original_memory = 0;
    if (original_class == 0 || !ReadMemoryPriority(original_memory))
    {
        std::printf("skip: cannot read current process priority (err=%lu)\n", GetLastError());
        return kSkipExitCode;
    }

    if (!SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS) ||
        !WriteMemoryPriority(MEMORY_PRIORITY_LOW))
    {
        std::printf("skip: cannot lower current process priority (err=%lu)\n", GetLastError());
        (void)SetPriorityClass(GetCurrentProcess(), original_class);
        (void)WriteMemoryPriority(original_memory);
        return kSkipExitCode;
    }

    // Read the lowered values back before exercising the restore. If a job or
    // policy accepted the calls without actually lowering the process, the
    // final assertion could pass without the helper having changed anything.
    const DWORD lowered_class = GetPriorityClass(GetCurrentProcess());
    DWORD lowered_memory = 0;
    if (lowered_class != BELOW_NORMAL_PRIORITY_CLASS || !ReadMemoryPriority(lowered_memory) ||
        lowered_memory != MEMORY_PRIORITY_LOW)
    {
        std::printf("skip: process priority did not stay lowered (class=0x%lx memory=%lu)\n", lowered_class,
                    lowered_memory);
        (void)SetPriorityClass(GetCurrentProcess(), original_class);
        (void)WriteMemoryPriority(original_memory);
        return kSkipExitCode;
    }

    const ProcessPriority::PriorityRestoreResult result = ProcessPriority::RestoreInteractiveProcessPriority();
    const DWORD restored_class = GetPriorityClass(GetCurrentProcess());
    DWORD restored_memory = 0;
    const bool restored_memory_read = ReadMemoryPriority(restored_memory);
    const bool ok = result.cpu_ok && result.memory_ok && restored_class == NORMAL_PRIORITY_CLASS &&
                    restored_memory_read && restored_memory == MEMORY_PRIORITY_NORMAL;

    std::printf("restore cpu_ok=%d cpu_error=%lu memory_ok=%d memory_error=%lu class=0x%lx memory=%lu\n",
                result.cpu_ok ? 1 : 0, result.cpu_error, result.memory_ok ? 1 : 0, result.memory_error, restored_class,
                restored_memory);

    (void)SetPriorityClass(GetCurrentProcess(), original_class);
    (void)WriteMemoryPriority(original_memory);
    return ok ? 0 : 1;
}
