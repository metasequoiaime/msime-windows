#include "tests/includes/test_framework.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Layout.h"
#include "msimeui/ScrollAxis.h"
#include "msimeui/Window.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <windows.h>

using namespace msimeui;

// Every expected offset below is worked out by hand from the scrollbar geometry: the
// thumb is as long as the visible share of the track (never shorter than its minimum),
// and the thumb's travel along the rest of the track maps linearly onto 0..max offset.
namespace
{
class FixedContent : public Visual
{
  public:
    explicit FixedContent(SizeF size) : size_(size)
    {
    }

    SizeF Measure(const SizeF &) override
    {
        return size_;
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(DeviceResources &) override
    {
    }

  private:
    SizeF size_;
};

constexpr float kViewportWidth = 200.0f;
constexpr float kViewportHeight = 300.0f;
// Default Appearance: a 10 DIP track along the right edge, so x = 194 is on it.
constexpr float kOnTrackX = 194.0f;

class ScrollViewerFixture
{
  public:
    explicit ScrollViewerFixture(float contentHeight, float dpi = 96.0f,
                                 const ScrollViewer::Appearance &appearance = {},
                                 float viewportHeight = kViewportHeight)
        : window(L"scroll-viewer-test", L"", 400, 400), dpi_(dpi)
    {
        window.SetDpiOverride(dpi);
        content = std::make_shared<FixedContent>(SizeF{kViewportWidth, contentHeight});
        viewer = std::make_shared<ScrollViewer>(content);
        viewer->SetAppearance(appearance);
        viewer->Attach(&window);
        viewer->MeasureInLayout({kViewportWidth, viewportHeight});
        viewer->ArrangeInLayout({0.0f, 0.0f, kViewportWidth, viewportHeight});
    }

    // Client pixels for a point given in DIPs, as the window would deliver it.
    POINT Pixels(float x, float y) const
    {
        const float scale = dpi_ / 96.0f;
        return {static_cast<LONG>(std::lround(x * scale)), static_cast<LONG>(std::lround(y * scale))};
    }

    bool Down(float x, float y)
    {
        return viewer->OnMouseDown(Pixels(x, y), MK_LBUTTON);
    }

    bool DragTo(float x, float y)
    {
        return viewer->OnMouseMove(Pixels(x, y), MK_LBUTTON);
    }

    bool Up(float x, float y)
    {
        return viewer->OnMouseUp(Pixels(x, y), 0);
    }

    Window window;
    std::shared_ptr<FixedContent> content;
    std::shared_ptr<ScrollViewer> viewer;

  private:
    float dpi_ = 96.0f;
};

// Makes SystemWheelScrollLines answer `lines` while it lives, so wheel tests run at a
// chosen "lines to scroll" setting without touching the machine's.
class WheelLinesOverride
{
  public:
    explicit WheelLinesOverride(UINT lines)
        : previousLines_(Lines()), previousSource_(SetWheelScrollLinesSource(&Answer))
    {
        Lines() = lines;
    }

    // Gives back the setting it replaced, an enclosing override's included.
    ~WheelLinesOverride()
    {
        Lines() = previousLines_;
        SetWheelScrollLinesSource(previousSource_);
    }

    WheelLinesOverride(const WheelLinesOverride &) = delete;
    WheelLinesOverride &operator=(const WheelLinesOverride &) = delete;

  private:
    static UINT &Lines()
    {
        static UINT lines = 3;
        return lines;
    }

    static UINT Answer()
    {
        return Lines();
    }

    UINT previousLines_;
    WheelScrollLinesSource previousSource_;
};

// Offset after one notch down from the top at `lines` per notch.
float OffsetAfterOneNotch(UINT lines)
{
    WheelLinesOverride setting(lines);
    ScrollViewerFixture f(1000.0f);
    REQUIRE(f.viewer->OnMouseWheel(f.Pixels(100.0f, 100.0f), -WHEEL_DELTA, 0));
    return f.viewer->GetScrollOffset();
}

// A real render target, so ScrollViewer::Render runs the glide step it only takes while
// drawing.
class RenderSurface
{
  public:
    RenderSurface()
    {
        REQUIRE(com_.Initialized());
        REQUIRE(window_.Created());
        resources_ = std::make_unique<DeviceResources>();
        REQUIRE(resources_->EnsureForWindow(window_.Handle()));
    }

    void Draw(Visual &visual)
    {
        REQUIRE(resources_->BeginDraw());
        visual.Render(*resources_);
        resources_->EndDraw();
    }

  private:
    // Each step that must be undone lives in a member, so a REQUIRE that throws from the
    // constructor still unwinds it. Members are destroyed last-declared first, which puts
    // the Direct2D and WIC objects before the window, and the window before COM.
    class Com
    {
      public:
        Com() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))
        {
        }
        ~Com()
        {
            if (Initialized())
                CoUninitialize();
        }
        Com(const Com &) = delete;
        Com &operator=(const Com &) = delete;

        bool Initialized() const
        {
            return SUCCEEDED(result_);
        }

      private:
        HRESULT result_ = E_FAIL;
    };

    class HiddenWindow
    {
      public:
        HiddenWindow() = default;
        ~HiddenWindow()
        {
            if (hwnd_)
                DestroyWindow(hwnd_);
        }
        HiddenWindow(const HiddenWindow &) = delete;
        HiddenWindow &operator=(const HiddenWindow &) = delete;

        bool Created()
        {
            if (hwnd_)
                return true;
            hwnd_ = CreateWindowExW(0, L"STATIC", L"scroll viewer render", WS_POPUP, 0, 0, 256, 256, nullptr, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
            return hwnd_ != nullptr;
        }
        HWND Handle() const
        {
            return hwnd_;
        }

      private:
        HWND hwnd_ = nullptr;
    };

    Com com_;
    HiddenWindow window_;
    std::unique_ptr<DeviceResources> resources_;
};
} // namespace

// Content 1000 in a 300 viewport: max offset 700, thumb 90 long, 210 of travel.
TEST_CASE(scroll_viewer_dragging_the_thumb_scrolls_in_proportion_to_its_travel)
{
    ScrollViewerFixture f(1000.0f);
    REQUIRE_NEAR(f.viewer->GetMaxScrollOffset(), 700.0f);

    REQUIRE(f.Down(kOnTrackX, 44.0f));
    REQUIRE(f.DragTo(kOnTrackX, 149.0f));
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 105.0f / 210.0f * 700.0f);
    REQUIRE_NEAR(f.content->GetBounds().y, -350.0f);

    f.DragTo(kOnTrackX, 1000.0f);
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 700.0f);
    f.DragTo(kOnTrackX, -500.0f);
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 0.0f);

    REQUIRE(f.Up(kOnTrackX, -500.0f));
    f.DragTo(kOnTrackX, 200.0f);
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 0.0f);
}

TEST_CASE(scroll_viewer_drag_needs_the_left_button_held)
{
    ScrollViewerFixture f(1000.0f);
    REQUIRE(f.Down(kOnTrackX, 44.0f));
    f.viewer->OnMouseMove(f.Pixels(kOnTrackX, 149.0f), 0);
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 0.0f);
}

TEST_CASE(scroll_viewer_track_click_centres_the_thumb_on_the_pointer_and_keeps_dragging)
{
    ScrollViewerFixture f(1000.0f);
    // Thumb top = 250 - 90 / 2 = 205.
    REQUIRE(f.Down(kOnTrackX, 250.0f));
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 205.0f / 210.0f * 700.0f);
    // The drag continues from the thumb's centre: pointer 150 puts the top at 105.
    f.DragTo(kOnTrackX, 150.0f);
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 350.0f);
}

TEST_CASE(scroll_viewer_track_click_near_the_end_stops_the_thumb_at_the_end_and_drags_from_the_grab_point)
{
    ScrollViewerFixture f(1000.0f);
    // 296 - 45 = 251 is past the last thumb top (210), so the thumb stops there and the
    // pointer holds it 86 below its top.
    REQUIRE(f.Down(kOnTrackX, 296.0f));
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 700.0f);
    f.DragTo(kOnTrackX, 186.0f);
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 100.0f / 210.0f * 700.0f);
}

TEST_CASE(scroll_viewer_track_click_above_the_thumb_jumps_up)
{
    ScrollViewerFixture f(1000.0f);
    f.viewer->SetScrollOffset(700.0f);
    REQUIRE(f.Down(kOnTrackX, 20.0f));
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 0.0f);
    // The thumb could not move above 0, so the pointer holds it 20 below its top.
    f.DragTo(kOnTrackX, 125.0f);
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 350.0f);
}

TEST_CASE(scroll_viewer_track_click_respects_the_thumb_inset)
{
    ScrollViewer::Appearance appearance;
    appearance.thumbInset = 4.0f;
    appearance.thumbMinLength = 24.0f;
    ScrollViewerFixture f(1000.0f, 96.0f, appearance);
    // Track 300 minus 4 at each end = 292; thumb 292 * 0.3 = 87.6; travel 204.4 from 4.
    REQUIRE(f.Down(kOnTrackX, 200.0f));
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), (200.0f - 43.8f - 4.0f) / 204.4f * 700.0f);
}

TEST_CASE(scroll_viewer_thumb_keeps_its_minimum_length_on_long_content)
{
    ScrollViewerFixture f(100000.0f);
    // 300 / 100000 of the track is under 36, so the thumb is 36 long and travels 264.
    REQUIRE(f.Down(kOnTrackX, 150.0f));
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 132.0f / 264.0f * 99700.0f);
}

TEST_CASE(scroll_viewer_thumb_that_fills_the_track_maps_one_dip_onto_the_whole_range)
{
    // A 30 DIP viewport is shorter than the minimum thumb, so the thumb fills the track
    // and has no travel; the drag still treats the travel as 1 DIP.
    ScrollViewerFixture f(1000.0f, 96.0f, {}, 30.0f);
    REQUIRE(f.Down(kOnTrackX, 10.0f));
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 0.0f);
    f.DragTo(kOnTrackX, 11.0f);
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 970.0f);
}

TEST_CASE(scroll_viewer_press_off_the_track_is_left_to_the_content)
{
    ScrollViewerFixture f(1000.0f);
    REQUIRE(!f.Down(100.0f, 100.0f));
    f.DragTo(100.0f, 200.0f);
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 0.0f);
}

TEST_CASE(scroll_viewer_track_is_hit_by_the_viewer_and_not_focusable)
{
    ScrollViewerFixture f(1000.0f);
    REQUIRE(f.viewer->FindVisualAt({kOnTrackX, 100.0f}) == f.viewer.get());
    REQUIRE(f.viewer->FindFocusableAt({kOnTrackX, 100.0f}) == nullptr);
    REQUIRE(f.viewer->FindVisualAt({100.0f, 100.0f}) == f.content.get());
    // The overflowing content gives up the reserved 20 DIPs on the right.
    REQUIRE_NEAR(f.content->GetBounds().width, kViewportWidth - 20.0f);
}

TEST_CASE(scroll_viewer_content_that_fits_has_no_scrollbar)
{
    ScrollViewerFixture f(200.0f);
    REQUIRE_NEAR(f.viewer->GetMaxScrollOffset(), 0.0f);
    REQUIRE(!f.Down(kOnTrackX, 100.0f));
    REQUIRE(f.viewer->FindVisualAt({kOnTrackX, 100.0f}) == f.content.get());
    REQUIRE_NEAR(f.content->GetBounds().width, kViewportWidth);
    REQUIRE(!f.viewer->OnMouseWheel(f.Pixels(100.0f, 100.0f), -WHEEL_DELTA, 0));
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 0.0f);
}

TEST_CASE(scroll_viewer_set_scroll_offset_clamps_to_the_content)
{
    ScrollViewerFixture f(1000.0f);
    f.viewer->SetScrollOffset(-50.0f);
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 0.0f);
    f.viewer->SetScrollOffset(5000.0f);
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 700.0f);
    REQUIRE_NEAR(f.content->GetBounds().y, -700.0f);
    f.viewer->SetScrollOffset(123.0f);
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 123.0f);
}

TEST_CASE(scroll_viewer_scroll_into_view_moves_only_as_far_as_needed)
{
    ScrollViewerFixture f(1000.0f);
    // Below the view: its bottom (450) lands on the viewport's bottom.
    f.viewer->ScrollIntoView({0.0f, 400.0f, 10.0f, 50.0f});
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 150.0f);
    // Already visible (window y 50 is content 200): nothing moves.
    f.viewer->ScrollIntoView({0.0f, 50.0f, 10.0f, 50.0f});
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 150.0f);
    // Above the view: content 100 sits at window y -50; its top lands on the top.
    f.viewer->ScrollIntoView({0.0f, -50.0f, 10.0f, 20.0f});
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 100.0f);
    // Taller than the view: its top wins.
    f.viewer->ScrollIntoView({0.0f, 300.0f, 10.0f, 400.0f});
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 400.0f);
    // Past the end: clamped.
    f.viewer->ScrollIntoView({0.0f, 900.0f, 10.0f, 50.0f});
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 700.0f);
}

TEST_CASE(scroll_viewer_wheel_notch_follows_the_lines_to_scroll_setting)
{
    // wheelStep 72 is the notch at 3 lines; other settings scale it, 0 lines scrolls
    // nothing, and one screen per notch moves seven eighths of the 300 DIP viewport.
    REQUIRE_NEAR(OffsetAfterOneNotch(3), 72.0f);
    REQUIRE_NEAR(OffsetAfterOneNotch(1), 24.0f);
    REQUIRE_NEAR(OffsetAfterOneNotch(5), 120.0f);
    REQUIRE_NEAR(OffsetAfterOneNotch(0), 0.0f);
    REQUIRE_NEAR(OffsetAfterOneNotch(WHEEL_PAGESCROLL), 262.5f);
}

TEST_CASE(scroll_viewer_wheel_without_smoothing_jumps_by_the_system_notch)
{
    WheelLinesOverride setting(3);
    ScrollViewerFixture f(1000.0f);
    REQUIRE(f.viewer->OnMouseWheel(f.Pixels(100.0f, 100.0f), -WHEEL_DELTA, 0));
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 72.0f);
    REQUIRE(f.viewer->OnMouseWheel(f.Pixels(100.0f, 100.0f), WHEEL_DELTA, 0));
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 0.0f);
    // Already at the top: the notch is taken but nothing moves.
    REQUIRE(f.viewer->OnMouseWheel(f.Pixels(100.0f, 100.0f), WHEEL_DELTA, 0));
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 0.0f);
    // Off the viewer: not taken.
    REQUIRE(!f.viewer->OnMouseWheel(f.Pixels(100.0f, 350.0f), -WHEEL_DELTA, 0));
}

TEST_CASE(scroll_viewer_smooth_wheel_does_not_jump_and_stacks_notches_to_the_end)
{
    ScrollViewer::Appearance appearance;
    appearance.smoothScrollMs = 260.0;
    WheelLinesOverride setting(3);
    ScrollViewerFixture f(5000.0f, 96.0f, appearance);
    const float notch = 72.0f;
    RenderSurface surface;

    // The waits below only need to outlast the 260 ms glide, so a slow machine can make
    // them longer but not shorter; nothing depends on how fast a frame is drawn.
    REQUIRE(f.viewer->OnMouseWheel(f.Pixels(100.0f, 100.0f), -WHEEL_DELTA, 0));
    // Nothing moves until a frame is drawn: the notch did not jump.
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 0.0f);

    // A second notch while gliding extends the glide from where it was heading.
    REQUIRE(f.viewer->OnMouseWheel(f.Pixels(100.0f, 100.0f), -WHEEL_DELTA, 0));
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 0.0f);
    Sleep(400);
    surface.Draw(*f.viewer);
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), notch * 2.0f);
}

TEST_CASE(scroll_viewer_set_scroll_offset_cancels_a_glide)
{
    ScrollViewer::Appearance appearance;
    appearance.smoothScrollMs = 260.0;
    WheelLinesOverride setting(3);
    ScrollViewerFixture f(5000.0f, 96.0f, appearance);
    RenderSurface surface;

    REQUIRE(f.viewer->OnMouseWheel(f.Pixels(100.0f, 100.0f), -WHEEL_DELTA, 0));
    f.viewer->SetScrollOffset(10.0f);
    Sleep(300);
    surface.Draw(*f.viewer);
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 10.0f);
}

TEST_CASE(scroll_viewer_mouse_works_in_pixels_at_150_percent)
{
    ScrollViewerFixture f(1000.0f, 144.0f);
    // The same DIP gestures as at 96 DPI arrive as 1.5x pixels and give the same offsets.
    REQUIRE(f.Down(kOnTrackX, 44.0f));
    f.DragTo(kOnTrackX, 150.0f);
    const float offset = 106.0f / 210.0f * 700.0f;
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), offset);
    f.Up(kOnTrackX, 150.0f);

    REQUIRE(f.Down(kOnTrackX, 250.0f));
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), 205.0f / 210.0f * 700.0f);
    // A press 6 DIPs left of the track is not on it.
    f.Up(kOnTrackX, 250.0f);
    REQUIRE(!f.Down(184.0f, 100.0f));
}

TEST_CASE(scroll_viewer_places_content_on_whole_device_pixels_at_150_percent)
{
    // Content 1100 in 300: max offset 800, thumb 81.8, travel 218.2, so one DIP of thumb
    // is 11 / 3 DIPs of content and most drags end between device pixels.
    ScrollViewerFixture f(1100.0f, 144.0f);
    REQUIRE(f.viewer->OnMouseDown({291, 66}, MK_LBUTTON));
    f.viewer->OnMouseMove({291, 224}, MK_LBUTTON);
    // The pointer moved 158 pixels, 105.33 DIPs: offset 386.22, which is 579.33 pixels.
    const float offset = 158.0f / 1.5f * 800.0f / (300.0f - 300.0f * 300.0f / 1100.0f);
    REQUIRE_NEAR(f.viewer->GetScrollOffset(), offset);
    REQUIRE(std::fabs(offset * 1.5f - std::round(offset * 1.5f)) > 0.1f);
    // The content moves by the offset rounded to 579 pixels.
    REQUIRE_NEAR(f.content->GetBounds().y, -579.0f / 1.5f);
}
