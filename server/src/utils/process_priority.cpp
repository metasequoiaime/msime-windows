#include "utils/process_priority.h"

namespace ProcessPriority
{
PriorityRestoreResult RestoreInteractiveProcessPriority()
{
    PriorityRestoreResult result;

    // SetPriorityClass only changes the CPU scheduling class. Task Scheduler's
    // default Priority 7 lowers both the class and the memory priority, and
    // CreateProcess only inherits the class, so the memory priority has to be
    // reset through its own API.
    if (SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS))
        result.cpu_ok = true;
    else
        result.cpu_error = GetLastError();

    MEMORY_PRIORITY_INFORMATION memory_priority{};
    memory_priority.MemoryPriority = MEMORY_PRIORITY_NORMAL;
    if (SetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority, &memory_priority, sizeof(memory_priority)))
        result.memory_ok = true;
    else
        result.memory_error = GetLastError();

    return result;
}
} // namespace ProcessPriority
