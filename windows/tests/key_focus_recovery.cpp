#include "Key/KeyFocusRecovery.h"

int main()
{
    if (!ShouldRecoverNamedpipeOnKeyFocus(true, true, false, true))
        return 1;
    if (ShouldRecoverNamedpipeOnKeyFocus(false, true, false, true))
        return 2;
    if (ShouldRecoverNamedpipeOnKeyFocus(true, true, true, true))
        return 3;
    if (ShouldRecoverNamedpipeOnKeyFocus(true, true, false, false))
        return 4;
    // A TIP activated in a background process still becomes its thread's
    // keystroke handler; that alone must not claim the Server.
    if (ShouldRecoverNamedpipeOnKeyFocus(true, false, false, true))
        return 5;
    return 0;
}
