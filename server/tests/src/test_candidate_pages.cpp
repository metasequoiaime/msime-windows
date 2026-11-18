#include "ipc/candidate_pages.h"
#include "tests/includes/test_framework.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
using FanyImeIpc::CandidatePage;
using FanyImeIpc::CandidatePages;
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
    snapshot->page_start = page_index * 5;
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

std::unique_ptr<CandidatePages> MakePages(FakeSurface &surface, milliseconds wait = milliseconds{80})
{
    return std::make_unique<CandidatePages>(surface, wait);
}
} // namespace

TEST_CASE(candidate_pages_show_publishes_first_frame)
{
    FakeSurface surface;
    auto pages = MakePages(surface);

    pages->Show(MakePage("photon", {"跑通", "炮筒"}));

    REQUIRE(surface.publish_count == 1);
    REQUIRE(surface.show_count == 1);
    REQUIRE(surface.last.generation == 1);
}

TEST_CASE(candidate_pages_async_update_paints_immediately)
{
    FakeSurface surface;
    auto pages = MakePages(surface);

    pages->Show(MakePage("photon", {"跑通"}));
    REQUIRE(pages->Update(MakePage("photon", {"炮同"})));

    // 没有合并窗口：回包当场发布并请求显示。
    REQUIRE(surface.show_count == 2);
    REQUIRE(surface.last.generation == 2);
    REQUIRE(surface.last.items->front().word == "炮同");
}

TEST_CASE(candidate_pages_every_async_update_for_an_input_paints)
{
    FakeSurface surface;
    auto pages = MakePages(surface);

    // 混输、重排、译文先后回来，每一份真实变化都要上屏；不再有「每个输入一帧」的额度把后到的挡掉，
    // 否则列表里有的候选屏上没有，要翻页才看得到。
    pages->Show(MakePage("photon", {"跑通"}));
    REQUIRE(pages->Update(MakePage("photon", {"炮同"})));
    pages->Publish(MakePage("photon", {"跑痛"}));
    REQUIRE(pages->UpdateCurrent());
    REQUIRE(pages->Update(MakePage("photon", {"泡桐"})));

    REQUIRE(surface.show_count == 4);
    REQUIRE(surface.last.items->front().word == "泡桐");
}

TEST_CASE(candidate_pages_rejects_async_update_for_another_input)
{
    FakeSurface surface;
    auto pages = MakePages(surface);

    pages->Show(MakePage("photon", {"跑通"}));
    REQUIRE(!pages->Update(MakePage("photo", {"迟到"})));
    REQUIRE(surface.show_count == 1);
    REQUIRE(surface.publish_count == 1);
}

TEST_CASE(candidate_pages_block_holds_until_allowed_or_new_input)
{
    FakeSurface surface;
    auto pages = MakePages(surface);

    pages->Show(MakePage("photon", {"跑通"}));
    pages->BlockAsyncUpdates();
    REQUIRE(!pages->Update(MakePage("photon", {"炮同"})));
    REQUIRE(!pages->UpdateCurrent());
    REQUIRE(surface.show_count == 1);

    pages->AllowAsyncUpdates();
    REQUIRE(pages->Update(MakePage("photon", {"炮同"})));

    pages->BlockAsyncUpdates();
    pages->Show(MakePage("photonx", {"跑通x"}));
    REQUIRE(pages->Update(MakePage("photonx", {"炮同x"})));
}
TEST_CASE(candidate_pages_publish_defers_show_until_requested)
{
    FakeSurface surface;
    auto pages = MakePages(surface);

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
    FakeSurface surface;
    auto pages = MakePages(surface, milliseconds{500});

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
    FakeSurface surface;
    surface.is_uiless = true;
    auto pages = MakePages(surface);

    pages->Show(MakePage("photon", {"跑通"}));
    const auto choice = pages->CommitSlot(0);

    REQUIRE(choice);
    REQUIRE(choice->item.word == "跑通");
    REQUIRE(!choice->waited);
    REQUIRE(!choice->timed_out);
}

TEST_CASE(candidate_pages_uiless_commit_highlighted_uses_the_published_page)
{
    FakeSurface surface;
    surface.is_uiless = true;
    auto pages = MakePages(surface);

    pages->Show(MakePage("photon", {"跑通", "炮同"}, 0, 1));
    const auto choice = pages->CommitHighlighted();

    REQUIRE(choice);
    REQUIRE(choice->item.word == "炮同");
    REQUIRE(choice->index_in_page == 1);
    REQUIRE(!choice->waited);
}

TEST_CASE(candidate_pages_ignores_an_echo_older_than_the_rendered_page)
{
    FakeSurface surface;
    auto pages = MakePages(surface, milliseconds{0});

    pages->Show(MakePage("photon", {"旧页"}));
    REQUIRE(pages->Painted(1, true));
    pages->Show(MakePage("photon", {"新页"}));
    REQUIRE(pages->Painted(2, true));
    REQUIRE(!pages->Painted(1, true));

    const auto choice = pages->CommitSlot(0);
    REQUIRE(choice);
    REQUIRE(choice->item.word == "新页");
}

TEST_CASE(candidate_pages_update_current_shows_the_published_page)
{
    FakeSurface surface;
    auto pages = MakePages(surface);

    pages->Publish(MakePage("photon", {"跑通"}));
    REQUIRE(surface.show_count == 0);
    REQUIRE(pages->UpdateCurrent());

    REQUIRE(surface.show_count == 1);
    REQUIRE(surface.last.generation == 2);
    REQUIRE(surface.last.items->front().word == "跑通");
}

TEST_CASE(candidate_pages_does_not_commit_a_page_from_a_previous_input)
{
    FakeSurface surface;
    auto pages = MakePages(surface, milliseconds{0});

    pages->Show(MakePage("old", {"旧页"}));
    REQUIRE(pages->Painted(1, true));
    pages->Show(MakePage("new", {"新页"}));

    const auto choice = pages->CommitSlot(0);
    REQUIRE(choice);
    REQUIRE(choice->item.word == "新页");
}

// 横排固定宽度一行只画出了前两个：第三个不在屏幕上，数字键选不到它。
TEST_CASE(candidate_pages_refuses_slots_the_presenter_did_not_draw)
{
    FakeSurface surface;
    auto pages = MakePages(surface, milliseconds{0});

    pages->Show(MakePage("shi", {"是", "事", "时"}));
    REQUIRE(pages->Painted(1, true, 2));
    REQUIRE(pages->CommitSlot(1));
    REQUIRE(!pages->CommitSlot(2));

    // 重画时整页都放下了，回显不带个数，限制随之解除。
    REQUIRE(pages->Painted(1, true));
    const auto choice = pages->CommitSlot(2);
    REQUIRE(choice);
    REQUIRE(choice->item.word == "时");
}

TEST_CASE(candidate_pages_commit_starts_at_the_snapshot_page_start)
{
    FakeSurface surface;
    auto pages = MakePages(surface, milliseconds{0});

    // 固定宽度下第二页从下标 2 开始，不是 page_index * page_size。
    auto page = MakePage("shi", {"是", "事", "时", "市"}, 1);
    auto snapshot = std::make_shared<Global::CandidatePageSnapshot>(*page.snapshot);
    snapshot->page_start = 2;
    snapshot->page_words = {L"时", L"市"};
    page.snapshot = snapshot;
    pages->Show(page);
    REQUIRE(pages->Painted(1, true));
    const auto choice = pages->CommitSlot(1);
    REQUIRE(choice);
    REQUIRE(choice->item.word == "市");
    REQUIRE(choice->absolute_index == 3);
}

TEST_CASE(candidate_pages_commit_uses_the_rendered_page)
{
    FakeSurface surface;
    auto pages = MakePages(surface, milliseconds{0});

    pages->Show(MakePage("photon", {"跑通"}));
    pages->Painted(1, true);
    pages->Update(MakePage("photon", {"坏页"}));
    REQUIRE(surface.last.generation == 2);

    const auto unseen = pages->CommitSlot(0);
    REQUIRE(unseen);
    REQUIRE(unseen->item.word == "跑通");

    pages->Painted(2, true);
    const auto seen = pages->CommitSlot(0);
    REQUIRE(seen);
    REQUIRE(seen->item.word == "坏页");
    REQUIRE(seen->index_in_page == 0);
    REQUIRE(seen->absolute_index == 0);
}

TEST_CASE(candidate_pages_gesture_freezes_the_rendered_page)
{
    FakeSurface surface;
    auto pages = MakePages(surface);

    pages->Show(MakePage("photon", {"跑通"}));
    pages->Painted(1, true);
    pages->Gesture(1);
    REQUIRE(!pages->Update(MakePage("photon", {"新页"})));
    REQUIRE(surface.publish_count == 1);

    const auto choice = pages->CommitSlot(0);
    REQUIRE(choice);
    REQUIRE(choice->item.word == "跑通");
}

TEST_CASE(candidate_pages_hide_drops_late_results)
{
    FakeSurface surface;
    auto pages = MakePages(surface);

    pages->Show(MakePage("photon", {"跑通"}));
    pages->Painted(1, true);
    pages->Hide();
    REQUIRE(!pages->Update(MakePage("photon", {"迟到"})));

    REQUIRE(surface.publish_count == 1);
    REQUIRE(!pages->CommitSlot(0));
}

TEST_CASE(candidate_pages_keyboard_commit_waits_for_the_pending_paint)
{
    FakeSurface surface;
    auto pages = MakePages(surface, milliseconds{500});

    pages->Show(MakePage("photon", {"跑通"}));
    pages->Painted(1, true);
    pages->Update(MakePage("photon", {"炮同"}));
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
    REQUIRE(surface.show_count == 3);
}

TEST_CASE(candidate_pages_keyboard_commit_times_out_on_the_rendered_page)
{
    FakeSurface surface;
    auto pages = MakePages(surface, milliseconds{5});

    pages->Show(MakePage("photon", {"跑通"}));
    pages->Painted(1, true);
    pages->Update(MakePage("photon", {"炮同"}));

    const auto choice = pages->CommitSlot(0);
    REQUIRE(choice);
    REQUIRE(choice->item.word == "跑通");
    REQUIRE(choice->waited);
    REQUIRE(choice->timed_out);
}

TEST_CASE(candidate_pages_pointer_commit_never_waits)
{
    FakeSurface surface;
    auto pages = MakePages(surface);

    pages->Show(MakePage("photon", {"跑通"}));
    pages->Painted(1, true);
    pages->Update(MakePage("photon", {"炮同"}));

    const auto choice = pages->CommitAt(0);
    REQUIRE(choice);
    REQUIRE(choice->item.word == "跑通");
    REQUIRE(!choice->waited);
}
