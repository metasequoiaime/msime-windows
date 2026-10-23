#include "voice-input/voice_overlay_layout.h"
#include <iostream>

int main()
{
    int failed = 0;
    auto check = [&](bool ok, const char *label) {
        std::cout << (ok ? "PASS " : "FAIL ") << label << '\n';
        failed += !ok;
    };
    for (UINT dpi : {96U, 120U, 144U, 192U})
    {
        VoiceOverlayAnchor anchor;
        anchor.work = {-1920, -1080, 0, -40};
        anchor.full = {-1920, -1080, 0, 0};
        anchor.dpi = dpi;
        anchor.valid = true;
        RECT compact =
            VoiceOverlayBottomAnchoredRect(anchor, VoiceOverlayDipToPx(112, dpi), VoiceOverlayDipToPx(32, dpi));
        RECT card =
            VoiceOverlayBottomAnchoredRect(anchor, VoiceOverlayDipToPx(420, dpi), VoiceOverlayDipToPx(213, dpi));
        check(compact.bottom == card.bottom && card.bottom == anchor.work.bottom - anchor.bottom_margin_px(),
              "card size changes retain the 24 DIP bottom edge");
        // Odd physical widths at fractional DPI round the center by at most half a pixel.
        check(std::abs((compact.left + compact.right) - (anchor.work.left + anchor.work.right)) <= 1 &&
                  std::abs((card.left + card.right) - (anchor.work.left + anchor.work.right)) <= 1,
              "negative-origin work area center stays within half a physical pixel");
    }
    VoiceOverlayAnchor tiny;
    tiny.work = {-80, -20, 20, 10};
    tiny.dpi = 192;
    tiny.valid = true;
    const RECT clamped = VoiceOverlayBottomAnchoredRect(tiny, 840, 426);
    check(EqualRect(&clamped, &tiny.work), "oversized card clamps to a small work area");
    const RECT empty = VoiceOverlayBottomAnchoredRect(VoiceOverlayAnchor{}, 420, 200);
    check(IsRectEmpty(&empty), "invalid anchor produces no rectangle");
    return failed ? 1 : 0;
}
