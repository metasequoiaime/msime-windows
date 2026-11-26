#pragma once

#include <cstdint>

namespace VoiceInput
{
bool Initialize();
void RefreshKeyboardHook();
void SetImeActive(bool active);
void Shutdown();
void ToggleRecording();
// External platform controllers enqueue lifecycle commands on the same
// serialized voice-control thread as tray and hotkey input.
void StartRecording();
void StopRecording();
void CancelRecording();
bool IsRecording();
void RecordTsfInsertReceipt(std::uint64_t client, std::uint64_t receipt, bool accepted);
} // namespace VoiceInput
