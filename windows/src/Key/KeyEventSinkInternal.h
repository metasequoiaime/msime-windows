#pragma once

// Declarations shared by KeyEventSink*.cpp: helpers that lived in KeyEventSink.cpp's anonymous
// namespace before the split and are now used by more than one of those files.
// Include only from windows/src/Key/KeyEventSink*.cpp.

#include "Private.h"
#include "MetasequoiaIMEBaseStructure.h"

// 0xF003, 0xF004 are the keys that the touch keyboard sends for next/previous
#define THIRDPARTY_NEXTPAGE static_cast<WORD>(0xF003)
#define THIRDPARTY_PREVPAGE static_cast<WORD>(0xF004)

namespace key_event_sink_detail
{
KEYSTROKE_FUNCTION SegmentEditFunction(UINT code, UINT modifiers);
UINT CaptureIpcModifiers();
bool IsEnglishInputModeToggle(UINT code, UINT modifiers);
bool IsTranslationCommitShortcut(UINT code, UINT modifiers);
bool IsPinyinCommitShortcut(UINT code, UINT modifiers);
bool IsCharacterSetInputModeToggle(UINT code, UINT modifiers);
bool IsTrilingualCycleEnabled();
void PostOwnerMessageWithSyncFallback(HWND window, UINT message, WPARAM wParam = 0, LPARAM lParam = 0);
bool IsShiftVk(UINT code);
} // namespace key_event_sink_detail

// Because the code mostly works with VKeys, here map a WCHAR back to a VKKey for certain
// vkeys that the IME handles specially
__inline UINT VKeyFromVKPacketAndWchar(UINT vk, WCHAR wch)
{
    UINT vkRet = vk;
    if (LOWORD(vk) == VK_PACKET)
    {
        if (wch == L' ')
        {
            vkRet = VK_SPACE;
        }
        else if ((wch >= L'0') && (wch <= L'9'))
        {
            vkRet = static_cast<UINT>(wch);
        }
        else if ((wch >= L'a') && (wch <= L'z'))
        {
            vkRet = (UINT)(L'A') + ((UINT)(L'z') - static_cast<UINT>(wch));
        }
        else if ((wch >= L'A') && (wch <= L'Z'))
        {
            vkRet = static_cast<UINT>(wch);
        }
        else if (wch == THIRDPARTY_NEXTPAGE)
        {
            vkRet = VK_NEXT;
        }
        else if (wch == THIRDPARTY_PREVPAGE)
        {
            vkRet = VK_PRIOR;
        }
    }
    return vkRet;
}
