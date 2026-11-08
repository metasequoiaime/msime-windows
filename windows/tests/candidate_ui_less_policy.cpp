#include "UI/CandidateUiLessPolicy.h"

// Do not use assert: Release builds must execute these checks too.
int main()
{
    using namespace CandidateUiLessPolicy;

    // A normal host lets the IME draw its UI element.
    if (!HostDrawsElement(true, false, false))
    {
        return 1;
    }
    // A host that answers pbShow=FALSE draws the candidates itself. This is the
    // Overwatch case: activation flags did not carry UIELEMENTENABLEDONLY.
    if (HostDrawsElement(false, false, false))
    {
        return 2;
    }
    // IgnoreHostUiLess keeps the host window for Valve games even when the
    // activation flags report UI-less.
    if (!HostDrawsElement(false, true, true))
    {
        return 3;
    }
    // Activation flags still force UI-less for hosts that never answer.
    if (HostDrawsElement(true, false, true))
    {
        return 4;
    }

    // The first composition starts before BeginUIElement can report the host's
    // choice. Only that first one needs a page request; once the flag is
    // latched, later first keys already carry it.
    if (!ShouldRequestPageAfterBegin(false, false))
    {
        return 5;
    }
    if (ShouldRequestPageAfterBegin(true, false))
    {
        return 6;
    }
    if (ShouldRequestPageAfterBegin(false, true))
    {
        return 7;
    }
    if (ShouldRequestPageAfterBegin(true, true))
    {
        return 8;
    }

    return 0;
}
