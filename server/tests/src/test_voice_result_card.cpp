#include "voice-input/voice_result_card_model.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
unsigned failures = 0;

void check(bool condition, const char *what)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}
} // namespace

int main()
{
    VoiceResultCardModel model;

    // A new generation hides and clears whatever card was showing.
    model.reset(1);
    check(model.show(1, L"第一次结果"), "initial show applies");
    check(model.snapshot().visible, "shown card is visible");
    model.reset(2);
    const VoiceResultCardModel::Snapshot after_reset = model.snapshot();
    check(!after_reset.visible && after_reset.text.empty() && after_reset.session == 2,
          "reset hides and clears the card for the new generation");

    // Stale-generation results never apply, even after a newer card is hidden.
    model.reset(3);
    check(!model.show(2, L"旧结果"), "stale session show is rejected");
    check(!model.snapshot().visible, "stale show leaves the card hidden");

    // Empty text and dismissed same-session shows are ignored.
    model.reset(4);
    check(!model.show(4, L""), "empty show is rejected");
    check(model.show(4, L"有效结果"), "current-session non-empty show applies");
    check(model.close(), "close dismisses the visible card");
    check(!model.show(4, L"迟到结果"), "same-session show after close is ignored");
    const VoiceResultCardModel::Snapshot after_close = model.snapshot();
    check(!after_close.visible && after_close.text.empty(), "close clears text and visibility");

    // A newer reset clears the dismissal and revives the card.
    model.reset(5);
    check(model.show(5, L"新结果"), "reset clears the prior dismissal");
    check(model.matches_display(5, L"新结果"), "matches_display true for the current visible card");
    check(!model.matches_display(5, L"别的文字"), "matches_display false for different text");

    // matches_display degrades once the generation advances.
    model.reset(6);
    check(!model.matches_display(5, L"新结果"), "matches_display false after the generation advances");
    check(!model.matches_display(6, L""), "matches_display false while hidden");

    // An older reset is rejected; reset of the newest generation still applies.
    model.reset(7);
    check(!model.reset(3), "older reset is rejected");
    check(model.reset(7), "reset of the newest generation applies (discard current)");

    VoiceResultCardModel raced;
    // close_if_current: a click on a stale card must never dismiss the newer generation.
    // reset(1)->show(1,"A") on the UI, then a background reset(2) switches generations; the stale
    // click close_if_current(1,"A") must fail, and the new result for generation 2 must still apply.
    raced.reset(1);
    check(raced.show(1, L"A"), "first card shows");
    check(raced.reset(2), "background reset switches to a newer generation");
    check(!raced.close_if_current(1, L"A"), "stale close_if_current cannot dismiss the new generation");
    check(raced.show(2, L"B"), "new-generation result still applies after a stale close");
    const VoiceResultCardModel::Snapshot after_stale_close = raced.snapshot();
    check(after_stale_close.visible && after_stale_close.session == 2 && after_stale_close.text == L"B",
          "new-generation card is visible after a stale close attempt");

    // close_if_current does dismiss when the click really targets the current visible card.
    raced.reset(3);
    check(raced.show(3, L"C"), "third card shows");
    check(raced.close_if_current(3, L"C"), "close_if_current dismisses the matching card");
    const VoiceResultCardModel::Snapshot after_own_close = raced.snapshot();
    check(!after_own_close.visible && after_own_close.text.empty(), "close_if_current clears text and visibility");
    check(!raced.show(3, L"D"), "same-session show after close_if_current is ignored");

    // close_if_current with the wrong text (UI copy diverged) leaves the current card alone.
    raced.reset(4);
    check(raced.show(4, L"X"), "fourth card shows");
    check(!raced.close_if_current(4, L"Y"), "close_if_current with mismatched text is rejected");
    check(raced.matches_display(4, L"X"), "card survives a mismatched close_if_current");

    // close_if_current on a hidden/empty card is a no-op.
    raced.reset(5);
    check(!raced.close_if_current(5, L""), "close_if_current on a hidden card is rejected");

    // read_current captures the full text under one lock for a matching card.
    raced.reset(6);
    check(raced.show(6, L"完整转录全文"), "sixth card shows");
    std::wstring out = L"sentinel";
    check(raced.read_current(6, L"完整转录全文", out) && out == L"完整转录全文",
          "read_current returns the full matching text");

    // read_current rejects mismatched text without touching the caller's buffer.
    out = L"sentinel";
    check(!raced.read_current(6, L"不一致的文字", out), "read_current rejects mismatched text");
    check(out == L"sentinel", "read_current leaves output untouched on text mismatch");

    // read_current rejects a stale session, even when text matches.
    out = L"sentinel";
    check(!raced.read_current(5, L"完整转录全文", out), "read_current rejects a stale session");
    check(out == L"sentinel", "read_current leaves output untouched on session mismatch");

    // read_current rejects once the card is dismissed/closed.
    raced.close();
    out = L"sentinel";
    check(!raced.read_current(6, L"完整转录全文", out), "read_current rejects a dismissed card");
    check(out == L"sentinel", "read_current leaves output untouched on a dismissed card");

    if (failures)
    {
        std::cerr << failures << " voice result card model check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Voice result card model tests passed\n";
    return EXIT_SUCCESS;
}
