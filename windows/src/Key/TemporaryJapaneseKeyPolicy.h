#pragma once

#include "../IME/MetasequoiaIMEBaseStructure.h"
#include "../../../engine/contracts/temporary_japanese_input.h"

// Both normal and queued TSF classification consume this exact result. The caller
// supplies either the real raw buffer or its queued projection, never a cached R flag.
inline _KEYSTROKE_STATE ClassifyJapaneseKey(UINT key, WCHAR character, UINT modifiers, bool japanese,
                                            FanyImeTemporaryJapanese::Policy policy, const WCHAR *raw,
                                            std::size_t length, bool composition_active)
{
    using namespace FanyImeTemporaryJapanese;
    switch (ClassifyKey(key, character, modifiers, japanese, policy, raw, length, composition_active))
    {
    case KeyAction::LongVowel:
        return {CATEGORY_COMPOSING, FUNCTION_INPUT};
    case KeyAction::Punctuation:
        return {CATEGORY_COMPOSING, FUNCTION_PUNCTUATION};
    default:
        return {CATEGORY_NONE, FUNCTION_NONE};
    }
}
