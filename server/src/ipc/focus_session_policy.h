#pragma once

#include <cstdint>

namespace FanyImeIpc
{
constexpr bool CanSendFocusSessionReady(uint64_t client_id, uint64_t activation_epoch, uint64_t focus_token)
{
    return client_id != 0 && activation_epoch != 0 && focus_token != 0;
}

// The UI-less flag is carried per packet, but the rendering owner is a property
// of the focus session. A key that arrives without the flag must not downgrade
// a session that was already known to be UI-less: the TSF sends the first key
// of a composition before BeginUIElement can report who draws the candidates.
// ClientActivated and deactivation reset the session state explicitly.
constexpr bool ResolveSessionUiLess(bool activate_ui_less, bool packet_ui_less, bool previous_session_ui_less)
{
    return activate_ui_less || packet_ui_less || previous_session_ui_less;
}
} // namespace FanyImeIpc
