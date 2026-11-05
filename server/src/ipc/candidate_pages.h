#pragma once

#include "engine/core/word_item.h"
#include "global/globals.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
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

enum class CandidateFreshness
{
    Fast,
    Late,
    Immediate,
};

// Worker-affine publish/show/settle/latch/selection state machine. The only UI-thread entries are Painted() and
// Gesture(); everything else runs on the task thread and is serialized by the task queue.
class CandidatePages
{
  public:
    using Clock = std::chrono::steady_clock;

    class ClockPort
    {
      public:
        virtual ~ClockPort() = default;
        virtual Clock::time_point now() const = 0;
        // Runs fn on the worker after the deadline. A callback that fires after the page was replaced must be a
        // no-op; the module checks whether a frame is still pending.
        virtual void schedule_at(Clock::time_point deadline, std::function<void()> fn) = 0;
    };

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

    CandidatePages(ClockPort &clock, SurfacePort &surface,
                   std::chrono::milliseconds settle = std::chrono::milliseconds{120},
                   std::chrono::milliseconds wait = std::chrono::milliseconds{80})
        : clock_(clock), surface_(surface), settle_(settle), wait_(wait)
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
    // page this way and the caller decides whether to show it; a new page also supersedes any pending settle frame.
    void Publish(CandidatePage page)
    {
        std::lock_guard lock(mutex_);
        const bool input_changed = page.input != input_;
        if (input_changed)
        {
            pending_.reset();
            fast_done_ = false;
            late_done_ = false;
            latched_ = false;
        }
        else if (pending_)
        {
            // A synchronous rebuild on the same input is newer than the queued enrichment page; keep the frame
            // that is already scheduled, but let it paint this page instead of the stale one.
            pending_->page = page;
        }
        StorePage(std::move(page));
    }

    // First frame, first choice, page move or layout change: publish and request a paint immediately.
    void Show(CandidatePage page)
    {
        std::lock_guard lock(mutex_);
        const bool input_changed = page.input != input_;
        pending_.reset();
        if (input_changed)
        {
            fast_done_ = false;
            late_done_ = false;
            latched_ = false;
        }
        StorePage(std::move(page));
        surface_.request_show();
    }

    // Show the page that was already published, without stamping a new generation.
    void Show()
    {
        std::lock_guard lock(mutex_);
        surface_.request_show();
    }

    // Fast: at most one coalesced frame per input. Late: at most one late frame. Immediate: cloud/AI, every arrival
    // paints. Returns false when the update is already covered, stale, or blocked by a latched interaction.
    bool Update(CandidatePage page, CandidateFreshness freshness)
    {
        std::lock_guard lock(mutex_);
        if (input_.empty() || page.input != input_ || latched_)
            return false;

        if (freshness == CandidateFreshness::Immediate)
        {
            // Cloud/AI answers are newer than any pending settle frame; never let the older frame land after them.
            pending_.reset();
            StorePage(std::move(page));
            surface_.request_show();
            return true;
        }

        if (freshness == CandidateFreshness::Fast)
        {
            if (fast_done_)
                return false;
            if (!pending_)
            {
                pending_ = Pending{std::move(page), true, false};
                Schedule();
            }
            else
            {
                pending_->page = std::move(page);
                pending_->fast = true;
            }
            return true;
        }

        if (late_done_)
            return false;
        if (!pending_)
        {
            pending_ = Pending{std::move(page), false, true};
            Schedule();
        }
        else
        {
            pending_->page = std::move(page);
            pending_->late = true;
        }
        return true;
    }

    // Schedule one frame for the page that is already published. Callers use this after a silent Publish when an
    // asynchronous source only needs to decide timing, not build another page.
    bool UpdateCurrent(CandidateFreshness freshness)
    {
        std::shared_ptr<const CandidatePage> page;
        {
            std::lock_guard lock(mutex_);
            page = published_;
        }
        if (!page)
            return false;
        return Update(*page, freshness);
    }

    // A new keystroke starts a new input: drop any pending frame and allow this input its own fast and late frames.
    void ResetFrameBudget()
    {
        std::lock_guard lock(mutex_);
        pending_.reset();
        fast_done_ = false;
        late_done_ = false;
    }

    // Selection, hide or another user action: a pending enrichment frame must not land on top of it, and later
    // async results for this input must not retry the frame that was spent.
    void CancelPending()
    {
        std::lock_guard lock(mutex_);
        pending_.reset();
        fast_done_ = true;
        late_done_ = true;
    }

    void Hide()
    {
        std::lock_guard lock(mutex_);
        input_.clear();
        pending_.reset();
        published_.reset();
        rendered_.reset();
        published_pages_.clear();
        fast_done_ = false;
        late_done_ = false;
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
        const auto page = rendered_ ? rendered_ : published_;
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

    // UI thread: the presenter echoes the generation it actually painted (including the dedup path).
    void Painted(std::uint64_t generation, bool visible)
    {
        std::lock_guard lock(mutex_);
        if (generation == 0 || generation > published_generation_ || generation < rendered_generation_)
            return;
        const auto found = std::find_if(published_pages_.begin(), published_pages_.end(),
                                        [generation](const auto &page) { return page->generation == generation; });
        if (found == published_pages_.end() || (*found)->layout_epoch != layout_epoch_)
            return;
        rendered_ = *found;
        rendered_generation_ = generation;
        rendered_visible_ = visible;
        cv_.notify_all();
    }

    // UI thread: the user interacted with the page it saw. Freeze only that page; a pending fast/late frame is
    // discarded, and later async updates are rejected until the next Show().
    void Gesture(std::uint64_t generation)
    {
        std::lock_guard lock(mutex_);
        if (generation == 0 || generation != rendered_generation_ || !rendered_ || !rendered_visible_)
            return;
        latched_ = true;
        pending_.reset();
    }

    void Visibility(bool visible)
    {
        std::lock_guard lock(mutex_);
        rendered_visible_ = visible;
        if (!visible)
        {
            rendered_.reset();
            rendered_generation_ = 0;
        }
    }

  private:
    struct Pending
    {
        CandidatePage page;
        bool fast = false;
        bool late = false;
    };

    void Schedule()
    {
        clock_.schedule_at(clock_.now() + settle_, [this] { FlushPending(); });
    }

    void FlushPending()
    {
        std::lock_guard lock(mutex_);
        if (!pending_)
            return;
        Pending pending = std::move(*pending_);
        pending_.reset();
        if (pending.fast)
            fast_done_ = true;
        if (pending.late)
            late_done_ = true;
        StorePage(std::move(pending.page));
        surface_.request_show();
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
        if (allow_wait && wait_.count() > 0 && surface_.visible() && !surface_.uiless() &&
            rendered_generation_ < published_generation_)
        {
            // A silently published page has no frame on the way. Ask for one before waiting, or the timeout
            // would always expire; the presenter dedups when the page is already on screen.
            surface_.request_show();
            waited = true;
            timed_out = !cv_.wait_for(lock, wait_,
                                      [this] { return rendered_generation_ >= published_generation_ || !published_; });
        }

        const auto page = rendered_ ? rendered_ : published_;
        if (!page || !page->snapshot || !page->items)
            return std::nullopt;

        const int count = static_cast<int>(page->snapshot->page_words.size());
        if (index_in_page < 0 || index_in_page >= count || page->page_size <= 0)
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

    static constexpr std::size_t kPageHistory = 8;

    ClockPort &clock_;
    SurfacePort &surface_;
    std::chrono::milliseconds settle_;
    std::chrono::milliseconds wait_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string input_;
    std::uint64_t layout_epoch_ = 0;
    std::uint64_t published_generation_ = 0;
    std::uint64_t rendered_generation_ = 0;
    bool rendered_visible_ = false;
    bool fast_done_ = false;
    bool late_done_ = false;
    bool latched_ = false;
    std::optional<Pending> pending_;
    std::shared_ptr<const CandidatePage> published_;
    std::shared_ptr<const CandidatePage> rendered_;
    std::deque<std::shared_ptr<const CandidatePage>> published_pages_;
};
} // namespace FanyImeIpc
