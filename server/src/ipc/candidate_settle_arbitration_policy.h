#pragma once

#include "ipc/event_listener_task_class.h"

namespace FanyImeIpc
{
enum class SettleFlushDecision
{
    FlushSettle,
    ProcessTask,
};

// A due settle frame must not jump ahead of a queued foreground/user-action task: a hide, a selection or a new key
// has to take effect first, and that task's latch may cancel the settle entirely. Enrichment tasks must also run
// first: they are the fast results the settle is supposed to include. Maintenance can wait behind the frame.
inline SettleFlushDecision DecideSettleFlush(bool settle_due, bool has_queued_task,
                                             FanyNamedPipe::TaskClass next_task_class)
{
    if (!settle_due)
        return SettleFlushDecision::ProcessTask;
    if (!has_queued_task)
        return SettleFlushDecision::FlushSettle;
    switch (next_task_class)
    {
    case FanyNamedPipe::TaskClass::Foreground:
    case FanyNamedPipe::TaskClass::UserAction:
    case FanyNamedPipe::TaskClass::Enrichment:
        return SettleFlushDecision::ProcessTask;
    case FanyNamedPipe::TaskClass::Maintenance:
        return SettleFlushDecision::FlushSettle;
    }
    return SettleFlushDecision::FlushSettle;
}
} // namespace FanyImeIpc
