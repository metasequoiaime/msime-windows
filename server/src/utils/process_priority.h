#pragma once

#include <windows.h>

namespace ProcessPriority
{
struct PriorityRestoreResult
{
    bool cpu_ok = false;
    DWORD cpu_error = 0;
    bool memory_ok = false;
    DWORD memory_error = 0;
};

// Restores the process-wide CPU and memory priorities to the interactive
// defaults. Kept independent per component so a partial failure can be logged
// without stopping the caller.
PriorityRestoreResult RestoreInteractiveProcessPriority();
} // namespace ProcessPriority
