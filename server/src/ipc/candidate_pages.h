#pragma once

#include "engine/core/word_item.h"
#include "global/globals.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace FanyImeIpc
{
// A page is the immutable value produced by the candidate build. The module owns its generation and never exposes
// the worker's live item list to selection.
struct CandidatePage
{
    std::string input;
    std::uint64_t layout_epoch = 0;
    int page_size = 0;
    Global::CandidatePageSnapshotPtr snapshot;
    std::shared_ptr<const std::vector<WordItem>> items;
    std::uint64_t generation = 0;
};

struct CandidateChoice
{
    std::shared_ptr<const CandidatePage> page;
    WordItem item;
    int index_in_page = 0;
    int absolute_index = 0;
    bool waited = false;
    bool timed_out = false;
};

// Worker-affine publish/show/latch/selection state machine. The only UI-thread entries are Painted() and
// Gesture(); everything else runs on the task thread and is serialized by the task queue.
class CandidatePages
{
  public:
    class SurfacePort
    {
      public:
        virtual ~SurfacePort() = default;
        virtual void publish(const CandidatePage &page) = 0;
        virtual void request_show() = 0;
        virtual void hide() = 0;
        virtual bool visible() const = 0;
        virtual bool uiless() const = 0;
    };

    explicit CandidatePages(SurfacePort &surface, std::chrono::milliseconds wait = std::chrono::milliseconds{80})
        : surface_(surface), wait_(wait)
    {
    }

    // True while a page is published for the current input. Callers use this to decide whether a resolution can
    // trust the module instead of falling back to the worker's live list.
    bool HasPage() const
    {
        std::lock_guard lock(mutex_);
        return static_cast<bool>(published_);
    }

    // Silent publish: the page is what the next show will paint, but no frame is requested. The key path builds a
    // page this way and the caller decides whether to show it.
    void Publish(CandidatePage page)
    {
        std::lock_guard lock(mutex_);
        BeginInputIfChanged(page.input);
        StorePage(std::move(page));
    }

    // First frame, first choice, page move or layout change: publish and request a paint immediately.
    void Show(CandidatePage page)
    {
        std::lock_guard lock(mutex_);
        BeginInputIfChanged(page.input);
        StorePage(std::move(page));
        surface_.request_show();
    }

    // Show the page that was already published, without stamping a new generation.
    void Show()
    {
        std::lock_guard lock(mutex_);
        surface_.request_show();
    }

    // An asynchronous result (mixed input, rerank, cloud/AI, gloss) for the current input: publish and paint now.
    // There is no coalescing window and no per-input frame budget — callers only reach here when the visible list
    // really changed, and holding a real change back leaves the screen behind the list. Returns false when the page
    // belongs to another input, or a latched interaction or a selection blocks async updates.
    bool Update(CandidatePage page)
    {
        std::lock_guard lock(mutex_);
        if (input_.empty() || page.input != input_ || latched_ || async_blocked_)
            return false;
        StorePage(std::move(page));
        surface_.request_show();
        return true;
    }

    // Paint the page that is already published. Callers use this after a silent Publish when an asynchronous source
    // decided the rebuilt page should be shown.
    bool UpdateCurrent()
    {
        std::shared_ptr<const CandidatePage> page;
        {
            std::lock_guard lock(mutex_);
            page = published_;
        }
        if (!page)
            return false;
        return Update(*page);
    }

    // A new keystroke starts a new input: async results for it may paint again.
    void AllowAsyncUpdates()
    {
        std::lock_guard lock(mutex_);
        async_blocked_ = false;
    }

    // Selection or another user action: later async results for this input must not repaint on top of it.
    void BlockAsyncUpdates()
    {
        std::lock_guard lock(mutex_);
        async_blocked_ = true;
    }

    void Hide()
    {
        std::lock_guard lock(mutex_);
        input_.clear();
        published_.reset();
        rendered_.reset();
        published_pages_.clear();
        async_blocked_ = false;
        latched_ = false;
        rendered_generation_ = 0;
        rendered_visible_ = false;
        surface_.hide();
    }

    // Keyboard selection: waits at most wait_ for the rendered generation to catch up, then commits the rendered
    // page. It never commits a newer rebuild the user has not seen.
    std::optional<CandidateChoice> CommitSlot(int index_in_page)
    {
        return Commit(index_in_page, true);
    }

    std::optional<CandidateChoice> CommitHighlighted()
    {
        std::unique_lock lock(mutex_);
        const auto page = (rendered_ && rendered_->input == input_) ? rendered_ : published_;
        if (!page || !page->snapshot)
            return std::nullopt;
        const int index = page->snapshot->selected_index_in_page;
        lock.unlock();
        return Commit(index, true);
    }

    // Pointer selection: the pointer already targeted the rendered page, so do not block the worker.
    std::optional<CandidateChoice> CommitAt(int index_in_page)
    {
        return Commit(index_in_page, false);
    }

    // UI thread: the presenter echoes the generation it actually painted (including the dedup path). Returns true
    // when this echo belongs to a page the module still knows; a stale or foreign echo must not advance the mirror.
    bool Painted(std::uint64_t generation, bool visible)
    {
        std::lock_guard lock(mutex_);
        if (generation == 0 || generation > published_generation_ || generation < rendered_generation_)
            return false;
        const auto found = std::find_if(published_pages_.begin(), published_pages_.end(),
                                        [generation](const auto &page) { return page->generation == generation; });
        if (found == published_pages_.end() || (*found)->layout_epoch != layout_epoch_)
            return false;
        rendered_ = *found;
        rendered_generation_ = generation;
        rendered_visible_ = visible;
        cv_.notify_all();
        return true;
    }

    // UI thread: the user interacted with the page it saw. Freeze only that page; later async updates are rejected
    // until a page for a new input is published or the page is hidden.
    void Gesture(std::uint64_t generation)
    {
        std::lock_guard lock(mutex_);
        if (generation == 0 || generation != rendered_generation_ || !rendered_ || !rendered_visible_)
            return;
        latched_ = true;
    }

    void Visibility(bool visible)
    {
        std::lock_guard lock(mutex_);
        rendered_visible_ = visible;
        if (!visible)
            rendered_.reset();
    }

  private:
    void BeginInputIfChanged(const std::string &input)
    {
        if (input == input_)
            return;
        async_blocked_ = false;
        latched_ = false;
    }

    void StorePage(CandidatePage page)
    {
        input_ = page.input;
        layout_epoch_ = page.layout_epoch;
        const auto published = Store(std::move(page));
        published_ = published;
        surface_.publish(*published);
    }

    std::shared_ptr<const CandidatePage> Store(CandidatePage page)
    {
        page.generation = ++published_generation_;
        if (page.snapshot)
        {
            auto snapshot = std::make_shared<Global::CandidatePageSnapshot>(*page.snapshot);
            snapshot->generation = page.generation;
            page.snapshot = std::move(snapshot);
        }
        auto stored = std::make_shared<const CandidatePage>(std::move(page));
        published_pages_.push_back(stored);
        while (published_pages_.size() > kPageHistory)
            published_pages_.pop_front();
        return stored;
    }

    std::optional<CandidateChoice> Commit(int index_in_page, bool allow_wait)
    {
        std::unique_lock lock(mutex_);
        if (!published_ && !rendered_)
            return std::nullopt;

        bool waited = false;
        bool timed_out = false;
        const bool rendered_is_current = rendered_ && rendered_->input == input_;
        if (allow_wait && wait_.count() > 0 && surface_.visible() && !surface_.uiless() &&
            (!rendered_is_current || rendered_generation_ < published_generation_))
        {
            // A silently published page has no frame on the way. Ask for one before waiting, or the timeout
            // would always expire; the presenter dedups when the page is already on screen.
            surface_.request_show();
            waited = true;
            timed_out = !cv_.wait_for(lock, wait_,
                                      [this] { return rendered_generation_ >= published_generation_ || !published_; });
        }

        const auto page = (rendered_ && rendered_->input == input_) ? rendered_ : published_;
        if (!page || !page->snapshot || !page->items)
            return std::nullopt;
        // With no painted page for the current input there is nothing older to fall back to: use the published
        // page rather than dropping the key. This is the first-frame/timeout path; every other path prefers the
        // page the user actually saw.

        const int count = static_cast<int>(page->snapshot->page_words.size());
        if (index_in_page < 0 || index_in_page >= count || index_in_page >= page->page_size || page->page_size <= 0)
            return std::nullopt;
        const int start = page->snapshot->page_index * page->page_size;
        const int absolute = start + index_in_page;
        if (absolute < 0 || absolute >= static_cast<int>(page->items->size()))
            return std::nullopt;

        CandidateChoice choice;
        choice.page = page;
        choice.item = (*page->items)[static_cast<std::size_t>(absolute)];
        choice.index_in_page = index_in_page;
        choice.absolute_index = absolute;
        choice.waited = waited;
        choice.timed_out = timed_out;
        return choice;
    }

    static constexpr std::size_t kPageHistory = 32;

    SurfacePort &surface_;
    std::chrono::milliseconds wait_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string input_;
    std::uint64_t layout_epoch_ = 0;
    std::uint64_t published_generation_ = 0;
    std::uint64_t rendered_generation_ = 0;
    bool rendered_visible_ = false;
    bool async_blocked_ = false;
    bool latched_ = false;
    std::shared_ptr<const CandidatePage> published_;
    std::shared_ptr<const CandidatePage> rendered_;
    std::deque<std::shared_ptr<const CandidatePage>> published_pages_;
};
} // namespace FanyImeIpc
