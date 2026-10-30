#pragma once

#include <string>

#include "fmt/xchar.h"

// Unified Server diagnostic trace. It covers input latency, candidate windows,
// the floating toolbar, tray menus and the IPC lifecycle that drives them. The
// trace deliberately records only state, counts, identifiers, coordinates and
// error codes; user input and candidate text must never be written here.
//
// Server and TSF records go to separate files under <data directory>\logs, one
// pair per process run, so a report carries its own generation time and does
// not mix with older sessions.
namespace DiagnosticLog
{
// When disabled, call sites pay for one relaxed atomic load and do not format
// strings or touch the filesystem.
bool IsEnabled();

// Appends one timestamped line to the Server log. Thread-safe, bounded by
// rotation and retention, never throws.
void Write(const std::wstring &line);

// Appends one timestamped line to the TSF log. TSF records arrive batched from
// the diagnostic pipe and belong in their own file.
void WriteTsf(const std::wstring &line);
} // namespace DiagnosticLog

#define DIAG_LOGF(...)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        if (::DiagnosticLog::IsEnabled())                                                                              \
        {                                                                                                              \
            ::DiagnosticLog::Write(fmt::format(__VA_ARGS__));                                                          \
        }                                                                                                              \
    } while (0)

// Kept while call sites are migrated; both names use the one global switch
// and the same Server log file.
#define CAND_DIAG_LOGF(...) DIAG_LOGF(__VA_ARGS__)
