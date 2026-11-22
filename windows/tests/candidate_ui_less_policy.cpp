#include "UI/CandidateUiLessPolicy.h"

// Do not use assert: Release builds must execute these checks too.
int main()
{
    using namespace CandidateUiLessPolicy;

    // A normal host asks for the UI element, so the IME draws it.
    if (!ImeDrawsElement(true, false, false))
    {
        return 1;
    }
    // A host that answers pbShow=FALSE draws the candidates itself. This is the
    // Overwatch case: activation flags did not carry UIELEMENTENABLEDONLY.
    if (ImeDrawsElement(false, false, false))
    {
        return 2;
    }
    // IgnoreHostUiLess keeps the IME window for Valve games.
    if (!ImeDrawsElement(false, true, true))
    {
        return 3;
    }
    // Activation flags still force UI-less for hosts that never answer.
    if (ImeDrawsElement(true, false, true))
    {
        return 4;
    }
    // The ignored-host override wins over the activation flag.
    if (!ImeDrawsElement(false, true, false))
    {
        return 5;
    }

    // Every composition starts before BeginUIElement can report the host's
    // choice. The session-level flag is cleared at EndUIElement, so the page
    // request runs again for the next composition instead of relying on a
    // latch that could leak into a password field or another document.
    if (!ShouldRequestPageAfterBegin(false, false))
    {
        return 6;
    }
    if (ShouldRequestPageAfterBegin(true, false))
    {
        return 7;
    }
    if (ShouldRequestPageAfterBegin(false, true))
    {
        return 8;
    }
    if (ShouldRequestPageAfterBegin(true, true))
    {
        return 9;
    }

    return 0;
}
