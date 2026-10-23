#pragma once

#include <cstdint>
#include <mutex>
#include <string>

// Desired-state model for the voice result card. No Win32 or Direct2D types: this header is a
// standalone unit-testable policy, and the card translation unit drives a real window from it.
//
// The model deliberately stores only what the UI needs to reconcile against: the newest generation
// (session), the full result text, whether a card should be visible, and whether the current
// generation was dismissed. Background workers update it under the mutex, and the UI thread reads
// complete snapshots from it; window messages never carry text or pointers.
class VoiceResultCardModel
{
  public:
    struct Snapshot
    {
        std::uint64_t session = 0;
        std::wstring text;
        bool visible = false;
    };

    // New generation: hide and clear whatever card is showing, and accept subsequent results for
    // this session. A reset that is not the newest generation is rejected so out-of-order
    // background callbacks can never resurrect or shadow the current card.
    bool reset(std::uint64_t session);
    // Accept a final result for the current generation. Ignored when the session is stale, the
    // card was already dismissed, or the text is empty.
    bool show(std::uint64_t session, const std::wstring &text);
    // Dismiss the current generation unconditionally. Later same-generation show() calls are
    // ignored, so a late async result can never re-show a card that was closed.
    bool close();
    // Dismiss the current generation only when the visible card still matches (session, text),
    // decided inside one lock. The UI uses this for close clicks: a click on a stale card must
    // never dismiss the newer generation that reset() already switched to.
    bool close_if_current(std::uint64_t session, const std::wstring &text);
    // Inside one lock, confirm the visible card still matches (session, expected_text) and capture
    // its full text. The UI uses this for copy clicks so it never copies the text of a card that a
    // newer generation has already replaced.
    bool read_current(std::uint64_t session, const std::wstring &expected_text, std::wstring &text_out);
    // Is (session, text) still the desired visible card?
    bool matches_display(std::uint64_t session, const std::wstring &text) const;
    // Latest complete model, read atomically by the UI thread.
    Snapshot snapshot() const;

  private:
    mutable std::mutex mutex_;
    std::uint64_t latest_session_ = 0;
    std::wstring text_;
    bool visible_ = false;
    bool dismissed_ = false;
};

inline bool VoiceResultCardModel::reset(std::uint64_t session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (session < latest_session_)
        return false;
    latest_session_ = session;
    text_.clear();
    visible_ = false;
    dismissed_ = false;
    return true;
}

inline bool VoiceResultCardModel::show(std::uint64_t session, const std::wstring &text)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (session != latest_session_ || dismissed_ || text.empty())
        return false;
    if (visible_ && text_ == text)
        return false;
    text_ = text;
    visible_ = true;
    return true;
}

inline bool VoiceResultCardModel::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const bool was_visible = visible_;
    text_.clear();
    visible_ = false;
    dismissed_ = true;
    return was_visible;
}

inline bool VoiceResultCardModel::close_if_current(std::uint64_t session, const std::wstring &text)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (session != latest_session_ || !visible_ || text != text_)
        return false;
    text_.clear();
    visible_ = false;
    dismissed_ = true;
    return true;
}

inline bool VoiceResultCardModel::read_current(std::uint64_t session, const std::wstring &expected_text,
                                               std::wstring &text_out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (session != latest_session_ || !visible_ || expected_text != text_)
        return false;
    text_out = text_;
    return true;
}

inline bool VoiceResultCardModel::matches_display(std::uint64_t session, const std::wstring &text) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return visible_ && session == latest_session_ && text == text_;
}

inline VoiceResultCardModel::Snapshot VoiceResultCardModel::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot snap;
    snap.session = latest_session_;
    snap.text = text_;
    snap.visible = visible_;
    return snap;
}
