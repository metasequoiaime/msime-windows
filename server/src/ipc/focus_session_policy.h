#pragma once

#include <cstdint>

namespace FanyImeIpc
{
constexpr bool CanSendFocusSessionReady(uint64_t client_id, uint64_t activation_epoch, uint64_t focus_token)
{
    return client_id != 0 && activation_epoch != 0 && focus_token != 0;
}
} // namespace FanyImeIpc
