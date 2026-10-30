#include "tests/includes/test_framework.h"
#include "window/floating_toolbar_visibility_policy.h"

TEST_CASE(floating_toolbar_visibility_requires_active_ime_preference_and_nonfullscreen)
{
    REQUIRE(FanyImeUi::ShouldShowFloatingToolbar(true, false, true, false, true));
    REQUIRE(!FanyImeUi::ShouldShowFloatingToolbar(false, false, true, false, true));
    REQUIRE(!FanyImeUi::ShouldShowFloatingToolbar(true, true, true, false, true));
    REQUIRE(!FanyImeUi::ShouldShowFloatingToolbar(true, false, false, false, true));
    REQUIRE(!FanyImeUi::ShouldShowFloatingToolbar(false, true, false, false, true));
}

TEST_CASE(floating_toolbar_stays_hidden_once_auto_hidden)
{
    REQUIRE(!FanyImeUi::ShouldShowFloatingToolbar(true, false, true, true, true));
}

TEST_CASE(floating_toolbar_hides_when_the_foreground_window_uses_another_ime)
{
    // The TIP is still activated (in the window the user left), so ime_active
    // stays true; only the foreground window says this toolbar does not belong
    // here. No other term may override that.
    REQUIRE(!FanyImeUi::ShouldShowFloatingToolbar(true, false, true, false, false));
    REQUIRE(!FanyImeUi::ShouldShowFloatingToolbar(true, false, false, false, false));
    REQUIRE(!FanyImeUi::ShouldShowFloatingToolbar(false, false, true, false, false));
}
