#include "ipc/candidate_pages.h"
#include "tests/includes/test_framework.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
using FanyImeIpc::CandidateFreshness;
using FanyImeIpc::CandidatePage;
using FanyImeIpc::CandidatePages;
using Clock = CandidatePages::Clock;
using milliseconds = std::chrono::milliseconds;

std::wstring Wide(const std::string &text)
{
    return std::wstring(text.begin(), text.end());
}

CandidatePage MakePage(const std::string &input, const std::vector<std::string> &words, int page_index = 0,
                       int selected = 0)
{
    auto items = std::make_shared<std::vector<WordItem>>();
    auto snapshot = std::make_shared<Global::CandidatePageSnapshot>();
    for (const std::string &word : words)
    {
        WordItem item;
        item.word = word;
        item.pinyin = input;
        items->push_back(item);
        snapshot->page_words.push_back(Wide(word));
    }
    snapshot->page_index = page_index;
    snapshot->selected_index_in_page = selected;
    snapshot->page_item_count = static_cast<int>(words.size());

    CandidatePage page;
    page.input = input;
    page.layout_epoch = 1;
    page.page_size = 5;
    page.snapshot = snapshot;
    page.items = items;
    return page;
}

class FakeClock : public CandidatePages::ClockPort
{
  public:
    Clock::time_point now() const override
    {
        return now_;
    }

    void schedule_at(Clock::time_point deadline, std::function<void()> fn) override
    {
        timers_.push_back({deadline, std::move(fn)});
    }

    void Advance(milliseconds delta)
    {
        now_ += delta;
        for (;;)
        {
            const auto due = std::find_if(timers_.begin(), timers_.end(),
                                          [this](const auto &timer) { return timer.deadline <= now_; });
            if (due == timers_.end())
                return;
            auto fn = std::move(due->fn);
            timers_.erase(due);
            fn();
        }
    }

  private:
    struct Timer
    {
        Clock::time_point deadline;
        std::function<void()> fn;
    };
    Clock::time_point now_{};
    std::vector<Timer> timers_;
};

class FakeSurface : public CandidatePages::SurfacePort
{
  public:
    void publish(const CandidatePage &page) override
    {
        ++publish_count;
        last = page;
    }

    void request_show() override
    {
        ++show_count;
        is_visible = true;
    }

    void hide() override
    {
        ++hide_count;
        is_visible = false;
    }

    bool visible() const override
    {
        return is_visible;
    }

    bool uiless() const override
    {
        return is_uiless;
    }

    int publish_count = 0;
    int show_count = 0;
    int hide_count = 0;
    CandidatePage last;
    bool is_visible = true;
    bool is_uiless = false;
};

std::unique_ptr<CandidatePages> MakePages(FakeClock &clock, FakeSurface &surface, milliseconds wait = milliseconds{80})
{
    return std::make_unique<CandidatePages>(clock, surface, milliseconds{120}, wait);
}
} // namespace

TEST_CASE(candidate_pages_show_publishes_first_frame)
{
    FakeClock clock;
    FakeSurface surface;
    auto pages = MakePages(clock, surface);

    pages->Show(MakePage("photon", {"跑通", "炮筒"}));

    REQUIRE(surface.publish_count == 1);
    REQUIRE(surface.show_count == 1);
    REQUIRE(surface.last.generation == 1);
}

TEST_CASE(candidate_pages_publish_defers_show_until_requested)
{
    FakeClock clock;
    FakeSurface surface;
    auto pages = MakePages(clock, surface);

    pages->Publish(MakePage("photon", {"跑通"}));
    REQUIRE(surface.publish_count == 1);
    REQUIRE(surface.show_count == 0);
    REQUIRE(surface.last.generation == 1);

    pages->Show();
    REQUIRE(surface.publish_count == 1);
    REQUIRE(surface.show_count == 1);
}

TEST_CASE(candidate_pages_keyboard_commit_waits_for_the_first_paint)
{
    FakeClock clock;
    FakeSurface surface;
    auto pages = MakePages(clock, surface);

    pages->Show(MakePage("photon", {"跑通"}));

    std::thread painter([&pages] {
        std::this_thread::sleep_for(milliseconds{10});
        pages->Painted(1, true);
    });
    const auto choice = pages->CommitSlot(0);
    painter.join();

    REQUIRE(choice);
    REQUIRE(choice->item.word == "跑通");
    REQUIRE(choice->waited);
    REQUIRE(!choice->timed_out);
}

TEST_CASE(candidate_pages_uiless_commit_never_waits_for_a_paint_echo)
{
    FakeClock clock;
    FakeSurface surface;
    surface.is_uiless = true;
    auto pages = MakePages(clock, surface);

    pages->Show(MakePage("photon", {"跑通"}));
    const auto choice = pages->CommitSlot(0);

    REQUIRE(choice);
    REQUIRE(choice->item.word == "跑通");
    REQUIRE(!choice->waited);
    REQUIRE(!choice->timed_out);
}

TEST_CASE(candidate_pages_ignores_an_echo_older_than_the_rendered_page)
{
    FakeClock clock;
    FakeSurface surface;
    auto pages = MakePages(clock, surface, milliseconds{0});

    pages->Show(MakePage("photon", {"旧页"}));
    pages->Painted(1, true);
    pages->Show(MakePage("photon", {"新页"}));
    pages->Painted(2, true);
    pages->Painted(1, true);

    const auto choice = pages->CommitSlot(0);
    REQUIRE(choice);
    REQUIRE(choice->item.word == "新页");
}

TEST_CASE(candidate_pages_update_current_schedules_the_published_page)
{
    FakeClock clock;
    FakeSurface surface;
    auto pages = MakePages(clock, surface);

    pages->Publish(MakePage("photon", {"跑通"}));
    REQUIRE(pages->UpdateCurrent(CandidateFreshness::Fast));
    REQUIRE(surface.show_count == 0);

    clock.Advance(milliseconds{120});
    REQUIRE(surface.show_count == 1);
    REQUIRE(surface.last.generation == 2);
    REQUIRE(surface.last.items->front().word == "跑通");
}

TEST_CASE(candidate_pages_fast_updates_fold_into_one_frame)
{
    FakeClock clock;
    FakeSurface surface;
    auto pages = MakePages(clock, surface);

    pages->Show(MakePage("photon", {"跑通"}));
    REQUIRE(pages->Update(MakePage("photon", {"炮同"}), CandidateFreshness::Fast));
    REQUIRE(pages->Update(MakePage("photon", {"跑痛"}), CandidateFreshness::Fast));
    clock.Advance(milliseconds{119});
    REQUIRE(surface.show_count == 1);
    clock.Advance(milliseconds{1});
    REQUIRE(surface.show_count == 2);
    REQUIRE(surface.last.items->front().word == "跑痛");

    REQUIRE(!pages->Update(MakePage("photon", {"泡桐"}), CandidateFreshness::Fast));
    clock.Advance(milliseconds{120});
    REQUIRE(surface.show_count == 2);
}

TEST_CASE(candidate_pages_late_adds_one_frame)
{
    FakeClock clock;
    FakeSurface surface;
    auto pages = MakePages(clock, surface);

    pages->Show(MakePage("photon", {"跑通"}));
    pages->Update(MakePage("photon", {"炮同"}), CandidateFreshness::Fast);
    clock.Advance(milliseconds{120});
    REQUIRE(surface.show_count == 2);

    pages->Update(MakePage("photon", {"跑痛"}), CandidateFreshness::Late);
    clock.Advance(milliseconds{120});
    REQUIRE(surface.show_count == 3);
    REQUIRE(surface.last.items->front().word == "跑痛");

    REQUIRE(!pages->Update(MakePage("photon", {"泡桐"}), CandidateFreshness::Late));
    clock.Advance(milliseconds{120});
    REQUIRE(surface.show_count == 3);
}

TEST_CASE(candidate_pages_immediate_paints_every_arrival)
{
    FakeClock clock;
    FakeSurface surface;
    auto pages = MakePages(clock, surface);

    pages->Show(MakePage("photon", {"跑通"}));
    pages->Update(MakePage("photon", {"云候选"}), CandidateFreshness::Immediate);
    pages->Update(MakePage("photon", {"AI候选"}), CandidateFreshness::Immediate);

    REQUIRE(surface.show_count == 3);
    REQUIRE(surface.last.items->front().word == "AI候选");
}

TEST_CASE(candidate_pages_commit_uses_the_rendered_page)
{
    FakeClock clock;
    FakeSurface surface;
    auto pages = MakePages(clock, surface, milliseconds{0});

    pages->Show(MakePage("photon", {"跑通"}));
    pages->Painted(1, true);
    pages->Update(MakePage("photon", {"坏页"}), CandidateFreshness::Fast);
    clock.Advance(milliseconds{120});
    REQUIRE(surface.last.generation == 2);

    const auto unseen = pages->CommitSlot(0);
    REQUIRE(unseen);
    REQUIRE(unseen->item.word == "跑通");

    pages->Painted(2, true);
    const auto seen = pages->CommitSlot(0);
    REQUIRE(seen);
    REQUIRE(seen->item.word == "坏页");
}

TEST_CASE(candidate_pages_gesture_freezes_the_rendered_page)
{
    FakeClock clock;
    FakeSurface surface;
    auto pages = MakePages(clock, surface);

    pages->Show(MakePage("photon", {"跑通"}));
    pages->Painted(1, true);
    pages->Gesture(1);
    REQUIRE(!pages->Update(MakePage("photon", {"新页"}), CandidateFreshness::Fast));
    clock.Advance(milliseconds{120});
    REQUIRE(surface.publish_count == 1);

    const auto choice = pages->CommitSlot(0);
    REQUIRE(choice);
    REQUIRE(choice->item.word == "跑通");
}

TEST_CASE(candidate_pages_hide_drops_late_results)
{
    FakeClock clock;
    FakeSurface surface;
    auto pages = MakePages(clock, surface);

    pages->Show(MakePage("photon", {"跑通"}));
    pages->Painted(1, true);
    pages->Hide();
    REQUIRE(!pages->Update(MakePage("photon", {"迟到"}), CandidateFreshness::Fast));
    clock.Advance(milliseconds{120});

    REQUIRE(surface.publish_count == 1);
    REQUIRE(!pages->CommitSlot(0));
}

TEST_CASE(candidate_pages_keyboard_commit_waits_for_the_pending_paint)
{
    FakeClock clock;
    FakeSurface surface;
    auto pages = MakePages(clock, surface);

    pages->Show(MakePage("photon", {"跑通"}));
    pages->Painted(1, true);
    pages->Update(MakePage("photon", {"炮同"}), CandidateFreshness::Fast);
    clock.Advance(milliseconds{120});
    REQUIRE(surface.last.generation == 2);

    std::thread painter([&pages] {
        std::this_thread::sleep_for(milliseconds{10});
        pages->Painted(2, true);
    });
    const auto choice = pages->CommitSlot(0);
    painter.join();

    REQUIRE(choice);
    REQUIRE(choice->item.word == "炮同");
    REQUIRE(choice->waited);
    REQUIRE(!choice->timed_out);
}

TEST_CASE(candidate_pages_keyboard_commit_times_out_on_the_rendered_page)
{
    FakeClock clock;
    FakeSurface surface;
    auto pages = MakePages(clock, surface, milliseconds{5});

    pages->Show(MakePage("photon", {"跑通"}));
    pages->Painted(1, true);
    pages->Update(MakePage("photon", {"炮同"}), CandidateFreshness::Fast);
    clock.Advance(milliseconds{120});

    const auto choice = pages->CommitSlot(0);
    REQUIRE(choice);
    REQUIRE(choice->item.word == "跑通");
    REQUIRE(choice->waited);
    REQUIRE(choice->timed_out);
}

TEST_CASE(candidate_pages_pointer_commit_never_waits)
{
    FakeClock clock;
    FakeSurface surface;
    auto pages = MakePages(clock, surface);

    pages->Show(MakePage("photon", {"跑通"}));
    pages->Painted(1, true);
    pages->Update(MakePage("photon", {"炮同"}), CandidateFreshness::Fast);
    clock.Advance(milliseconds{120});

    const auto choice = pages->CommitAt(0);
    REQUIRE(choice);
    REQUIRE(choice->item.word == "跑通");
    REQUIRE(!choice->waited);
}
