// Standalone policy tests for the phase-A voice input target snapshot. This
// target links only the pure policy in voice_target.h; the Windows/UIA adapter
// in voice_target.cpp is exercised on the real host, not here. The CHECK macro
// is plain code (never compiled out), unlike assert() in NDEBUG builds.
#include "voice-input/voice_target.h"
#include "voice-input/voice_input_delivery.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
#define CHECK(expr)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            throw std::runtime_error("CHECK failed: " #expr);                                                          \
        }                                                                                                              \
    } while (false)

using VoiceInput::CanCommitToTarget;
using VoiceInput::ClassifyCtrlVInjection;
using VoiceInput::PipeClientMatchesForeground;
using VoiceInput::SameCompositionOwner;
using VoiceInput::SameTarget;
using VoiceInput::SendUnicodeTextWith;
using VoiceInput::Target;

// A concrete editable text-field identity. Identical values are the only thing
// SameTarget/CanCommitToTarget may rely on, so tests mutate single fields.
Target MakeTarget(std::uint64_t generation = 10)
{
    Target target;
    target.editable = true;
    target.foreground = 0x10000;
    target.focus = 0x20000;
    target.process = 0x1234;
    target.focus_process = 0x2345;
    target.thread = 0x5678;
    target.generation = generation;
    target.runtime_id = {1, 2, 3};
    target.client = 7;
    target.epoch = 11;
    target.focus_token = 0xABC;
    return target;
}

void TestIdenticalEditableTargetCommits()
{
    const Target before = MakeTarget(42);
    const Target now = MakeTarget(42);
    CHECK(SameTarget(before, now));
    CHECK(CanCommitToTarget(before, now));
    // The identity must be reflexive for an editable target.
    CHECK(SameTarget(before, before));
}

void TestNeverConfirmedTargetNeverCommits()
{
    const Target unknown{}; // all-zero default: no focus, not editable
    const Target editable = MakeTarget(42);
    CHECK(!SameTarget(unknown, unknown));
    CHECK(!CanCommitToTarget(unknown, unknown));
    // "No target at stop, a target appears before finalization" still goes to
    // the card: stop.editable is the gate.
    CHECK(!CanCommitToTarget(unknown, editable));
    // "Editable at stop, no target at finalization" also goes to the card.
    CHECK(!CanCommitToTarget(editable, unknown));
    CHECK(!SameTarget(editable, unknown));
}

void TestReadOnlyOrDisabledStopTargetNeverCommits()
{
    // A read-only/disabled native control is captured with editable=false even
    // though its HWND identity is intact. Even if the same field later reports
    // editable, the stop-time verdict keeps the result in the card.
    Target readonly_stop = MakeTarget(42);
    readonly_stop.editable = false;
    Target same_field_now = MakeTarget(42);
    CHECK(!SameTarget(readonly_stop, same_field_now));
    CHECK(!CanCommitToTarget(readonly_stop, same_field_now));
}

void TestReadOnlyOrDisabledDecisionTargetBreaksCommit()
{
    // The final target turned read-only/disabled between stop and finalization:
    // the identity is unchanged but editability is lost, so the card wins.
    const Target before = MakeTarget(42);
    Target readonly_now = MakeTarget(42);
    readonly_now.editable = false;
    CHECK(!SameTarget(before, readonly_now));
    CHECK(!CanCommitToTarget(before, readonly_now));
}

void TestAnyIdentityFieldChangeInvalidates()
{
    const Target before = MakeTarget(42);

    Target different_foreground = MakeTarget(42);
    different_foreground.foreground = 0x99999;
    CHECK(!SameTarget(before, different_foreground));

    Target different_focus = MakeTarget(42);
    different_focus.focus = 0x88888;
    CHECK(!SameTarget(before, different_focus));

    Target different_process = MakeTarget(42);
    different_process.process = 0x9999;
    CHECK(!SameTarget(before, different_process));

    Target different_focus_process = MakeTarget(42);
    different_focus_process.focus_process = 0x9999;
    CHECK(!SameTarget(before, different_focus_process));

    Target different_thread = MakeTarget(42);
    different_thread.thread = 0x9999;
    CHECK(!SameTarget(before, different_thread));

    Target different_generation = MakeTarget(43);
    CHECK(!SameTarget(before, different_generation));

    Target different_runtime_id = MakeTarget(42);
    different_runtime_id.runtime_id = {1, 2, 99};
    CHECK(!SameTarget(before, different_runtime_id));

    Target different_client = MakeTarget(42);
    different_client.client = 99;
    CHECK(!SameTarget(before, different_client));

    Target different_epoch = MakeTarget(42);
    different_epoch.epoch = 99;
    CHECK(!SameTarget(before, different_epoch));

    Target different_focus_token = MakeTarget(42);
    different_focus_token.focus_token = 0x999;
    CHECK(!SameTarget(before, different_focus_token));
}

void TestSameHwndDifferentUiaFieldBreaksCommit()
{
    // Chromium-style: two input fields share one HWND/process/thread and only
    // the UIA runtime ID differs. They must not compare as the same target.
    const Target before = MakeTarget(42);
    Target other_field = MakeTarget(42);
    other_field.runtime_id = {4, 5, 6};
    CHECK(before.foreground == other_field.foreground);
    CHECK(before.focus == other_field.focus);
    CHECK(before.generation == other_field.generation);
    CHECK(!SameTarget(before, other_field));
    CHECK(!CanCommitToTarget(before, other_field));
}

void TestSwitchAwayAndBackAdvancesGeneration()
{
    // Focus left the field and returned to the exact same element: every
    // WinEvent advanced the epoch, so the snapshots differ even though every
    // HWND and the runtime ID are identical.
    const Target before = MakeTarget(42);
    const Target returned = MakeTarget(44); // +2: away, then back
    CHECK(before.foreground == returned.foreground);
    CHECK(before.focus == returned.focus);
    CHECK(before.runtime_id == returned.runtime_id);
    CHECK(!SameTarget(before, returned));
    CHECK(!CanCommitToTarget(before, returned));
}

void TestCompositionOwnerIgnoresOnlyGeneration()
{
    const Target before = MakeTarget(42);
    const Target returned = MakeTarget(44);
    CHECK(SameCompositionOwner(before, returned));
    CHECK(!SameTarget(before, returned));

    // Cancellation may ignore only the generation. Every other identity and
    // route field still has to match, otherwise a new field's preedit could be
    // torn down by an old owner snapshot.
    Target changed = before;
    changed.foreground = 0x99999;
    CHECK(!SameCompositionOwner(before, changed));
    changed = before;
    changed.focus = 0x88888;
    CHECK(!SameCompositionOwner(before, changed));
    changed = before;
    changed.process = 0x9999;
    CHECK(!SameCompositionOwner(before, changed));
    changed = before;
    changed.focus_process = 0x9999;
    CHECK(!SameCompositionOwner(before, changed));
    changed = before;
    changed.thread = 0x9999;
    CHECK(!SameCompositionOwner(before, changed));
    changed = before;
    changed.runtime_id = {1, 2, 99};
    CHECK(!SameCompositionOwner(before, changed));
    changed = before;
    changed.client = 99;
    CHECK(!SameCompositionOwner(before, changed));
    changed = before;
    changed.epoch = 99;
    CHECK(!SameCompositionOwner(before, changed));
    changed = before;
    changed.focus_token = 0x999;
    CHECK(!SameCompositionOwner(before, changed));
    changed = before;
    changed.editable = false;
    CHECK(!SameCompositionOwner(before, changed));
}

void TestPipeClientIdentityMatchesHostOrFocusProcess()
{
    const Target target = MakeTarget(42);
    constexpr std::uint32_t foreground_thread = 0x3456;
    // The foreground host's input thread and the focused child's own thread are
    // both valid. A host PID with a child TID (or the reverse) is not.
    CHECK(PipeClientMatchesForeground(target, target.process, foreground_thread, foreground_thread));
    CHECK(PipeClientMatchesForeground(target, target.focus_process, target.thread, foreground_thread));
    CHECK(!PipeClientMatchesForeground(target, target.process, target.thread, foreground_thread));
    CHECK(!PipeClientMatchesForeground(target, target.focus_process, foreground_thread, foreground_thread));
    CHECK(!PipeClientMatchesForeground(target, target.process + 1, foreground_thread, foreground_thread));
    CHECK(!PipeClientMatchesForeground(target, target.focus_process + 1, target.thread, foreground_thread));
    CHECK(!PipeClientMatchesForeground(target, 0, foreground_thread, foreground_thread));
    CHECK(!PipeClientMatchesForeground(target, target.process, 0, foreground_thread));

    Target no_focus_process = target;
    no_focus_process.focus_process = 0;
    CHECK(!PipeClientMatchesForeground(no_focus_process, target.process, foreground_thread, foreground_thread));
}

void TestSendUnicodeStopsOnZeroOrPartialInjection()
{
    const std::wstring text = L"abc";
    std::size_t calls = 0;
    const auto inject_zero = [&calls](wchar_t) -> std::uint32_t {
        ++calls;
        return 0;
    };
    CHECK(SendUnicodeTextWith(text, inject_zero) == 0);
    CHECK(calls == 1); // The first short result stops all later input.

    calls = 0;
    const std::uint32_t partial_results[] = {2, 1, 2};
    const auto inject_partial = [&calls, &partial_results](wchar_t) -> std::uint32_t {
        const std::uint32_t result = partial_results[calls];
        ++calls;
        return result;
    };
    CHECK(SendUnicodeTextWith(text, inject_partial) == 1);
    CHECK(calls == 2);

    const auto inject_all = [](wchar_t) -> std::uint32_t { return 2; };
    CHECK(SendUnicodeTextWith(text, inject_all) == text.size());
}

void TestCtrlVInjectionCleanupClassification()
{
    const auto check = [](std::uint32_t injected, bool delivered, bool ctrl_down, bool v_down) {
        const VoiceInput::CtrlVInjectionOutcome outcome = ClassifyCtrlVInjection(injected);
        CHECK(outcome.delivered == delivered);
        CHECK(outcome.ctrl_key_down == ctrl_down);
        CHECK(outcome.v_key_down == v_down);
    };
    check(0, false, false, false);
    check(1, false, true, false); // Ctrl down only: release Ctrl.
    check(2, false, true, true);  // Ctrl+V down: release V, then Ctrl.
    check(3, false, true, false); // V up included: release Ctrl only.
    check(4, true, false, false); // Complete paste: no cleanup.
}

void TestUnboundRouteFieldsCompareZero()
{
    // Before the voice service binds a route, platform snapshots carry
    // client/epoch/focus_token == 0; two editable targets that match in every
    // platform identity and in the (zero) route binding still match.
    Target before = MakeTarget(42);
    before.client = 0;
    before.epoch = 0;
    before.focus_token = 0;
    Target now = before;
    CHECK(SameTarget(before, now));
    CHECK(CanCommitToTarget(before, now));
}
} // namespace

int main()
{
    const struct
    {
        const char *name;
        void (*fn)();
    } cases[] = {
        {"identical editable target commits", TestIdenticalEditableTargetCommits},
        {"never-confirmed target never commits", TestNeverConfirmedTargetNeverCommits},
        {"read-only/disabled stop target never commits", TestReadOnlyOrDisabledStopTargetNeverCommits},
        {"read-only/disabled decision target breaks commit", TestReadOnlyOrDisabledDecisionTargetBreaksCommit},
        {"any single identity field change invalidates", TestAnyIdentityFieldChangeInvalidates},
        {"same HWND, different UIA field breaks commit", TestSameHwndDifferentUiaFieldBreaksCommit},
        {"switch away and back advances generation", TestSwitchAwayAndBackAdvancesGeneration},
        {"composition owner ignores only generation", TestCompositionOwnerIgnoresOnlyGeneration},
        {"pipe client matches host or focus process", TestPipeClientIdentityMatchesHostOrFocusProcess},
        {"sendinput stops on zero or partial injection", TestSendUnicodeStopsOnZeroOrPartialInjection},
        {"ctrl+v partial injection cleanup classification", TestCtrlVInjectionCleanupClassification},
        {"unbound zero route fields compare equal", TestUnboundRouteFieldsCompareZero},
    };
    for (const auto &entry : cases)
    {
        try
        {
            entry.fn();
            std::cout << "[PASS] " << entry.name << "\n";
        }
        catch (const std::exception &ex)
        {
            std::cerr << "[FAIL] " << entry.name << ": " << ex.what() << "\n";
            return 1;
        }
    }
    std::cout << "voice target policy: all " << sizeof(cases) / sizeof(cases[0]) << " case(s) passed\n";
    return 0;
}
