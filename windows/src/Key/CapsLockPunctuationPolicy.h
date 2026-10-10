#pragma once

// Punctuation follows the effective input language, and Caps Lock makes letters English, so the
// mode-following paths select English punctuation while it is on. A manual punctuation toggle
// does not go through this rule: the user can still pick Chinese punctuation under Caps Lock.
inline bool FollowedPunctuationOpen(bool imeOpen, bool capsLockEnabled)
{
    return imeOpen && !capsLockEnabled;
}

// Caps Lock re-syncs punctuation only on a real edge. The key sink and the Server broadcast both
// report the same press; repeating it must not undo a manual toggle made after the first report.
inline bool ShouldResyncPunctuationForCapsLock(bool known, bool lastApplied, bool capsLockEnabled)
{
    return !known || lastApplied != capsLockEnabled;
}
