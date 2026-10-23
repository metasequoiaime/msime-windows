#include "tests/includes/test_framework.h"

#include "msimeui/ImageMask.h"
#include "msimeui/Layout.h"

#include <memory>
#include <vector>

using namespace msimeui;

namespace
{
// Fixed-size leaf for observing how OverlayPanel measures and places children.
class FixedBox : public Visual
{
  public:
    explicit FixedBox(SizeF size) : size_(size)
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

// A BGRA bitmap whose alpha is 255 where `mask` has '#', 0 elsewhere.
std::vector<BYTE> MaskPixels(const std::vector<const char *> &mask, UINT width)
{
    std::vector<BYTE> pixels(static_cast<size_t>(width) * 4 * mask.size(), 0);
    for (size_t y = 0; y < mask.size(); ++y)
    {
        for (UINT x = 0; x < width; ++x)
        {
            pixels[(y * width + x) * 4 + 3] = mask[y][x] == '#' ? 255 : 0;
        }
    }
    return pixels;
}

bool SameRect(const RECT &rect, LONG left, LONG top, LONG right, LONG bottom)
{
    return rect.left == left && rect.top == top && rect.right == right && rect.bottom == bottom;
}
} // namespace

TEST_CASE(overlay_panel_places_children_at_their_positions)
{
    auto panel = std::make_shared<OverlayPanel>();
    auto background = std::make_shared<FixedBox>(SizeF{100.0f, 50.0f});
    auto icon = std::make_shared<FixedBox>(SizeF{20.0f, 20.0f});
    panel->AddChildAt(background, 0.0f, 0.0f);
    panel->AddChildAt(icon, 90.0f, 40.0f);

    const SizeF measured = panel->MeasureInLayout({1000.0f, 1000.0f});
    REQUIRE_NEAR(measured.width, 110.0f);
    REQUIRE_NEAR(measured.height, 60.0f);

    panel->ArrangeInLayout({10.0f, 5.0f, measured.width, measured.height});
    const RectF backgroundBounds = background->GetBounds();
    REQUIRE_NEAR(backgroundBounds.x, 10.0f);
    REQUIRE_NEAR(backgroundBounds.y, 5.0f);
    REQUIRE_NEAR(backgroundBounds.width, 100.0f);
    REQUIRE_NEAR(backgroundBounds.height, 50.0f);
    const RectF iconBounds = icon->GetBounds();
    REQUIRE_NEAR(iconBounds.x, 100.0f);
    REQUIRE_NEAR(iconBounds.y, 45.0f);
    REQUIRE_NEAR(iconBounds.width, 20.0f);
    REQUIRE_NEAR(iconBounds.height, 20.0f);
}

TEST_CASE(overlay_panel_moves_a_child_it_holds)
{
    auto panel = std::make_shared<OverlayPanel>();
    auto child = std::make_shared<FixedBox>(SizeF{10.0f, 10.0f});
    auto stranger = std::make_shared<FixedBox>(SizeF{10.0f, 10.0f});
    panel->AddChildAt(child, 0.0f, 0.0f);

    panel->SetChildPosition(child.get(), 5.0f, 6.0f);
    // A visual outside the panel is ignored rather than moving some other child.
    panel->SetChildPosition(stranger.get(), 50.0f, 60.0f);
    const SizeF measured = panel->MeasureInLayout({100.0f, 100.0f});
    panel->ArrangeInLayout({0.0f, 0.0f, measured.width, measured.height});

    REQUIRE_NEAR(child->GetBounds().x, 5.0f);
    REQUIRE_NEAR(child->GetBounds().y, 6.0f);
    REQUIRE_NEAR(panel->GetChildPosition(child.get()).x, 5.0f);
    REQUIRE_NEAR(measured.width, 15.0f);
}

TEST_CASE(overlay_panel_hit_test_prefers_later_child)
{
    auto panel = std::make_shared<OverlayPanel>();
    auto background = std::make_shared<FixedBox>(SizeF{100.0f, 100.0f});
    auto icon = std::make_shared<FixedBox>(SizeF{10.0f, 10.0f});
    panel->AddChildAt(background, 0.0f, 0.0f);
    panel->AddChildAt(icon, 50.0f, 50.0f);

    const SizeF measured = panel->MeasureInLayout({1000.0f, 1000.0f});
    panel->ArrangeInLayout({0.0f, 0.0f, measured.width, measured.height});

    REQUIRE(panel->FindVisualAt({55.0f, 55.0f}) == icon.get());
    REQUIRE(panel->FindVisualAt({5.0f, 5.0f}) == background.get());
}

TEST_CASE(alpha_mask_rects_merge_identical_rows)
{
    const UINT width = 6;
    const std::vector<BYTE> pixels = MaskPixels({"##..##", //
                                                 "##..##", //
                                                 "######", //
                                                 "......"},
                                                width);
    const std::vector<RECT> rects = AlphaMaskRects(pixels.data(), width, 4, width * 4, 0);
    REQUIRE(rects.size() == 3);
    REQUIRE(SameRect(rects[0], 0, 0, 2, 2));
    REQUIRE(SameRect(rects[1], 4, 0, 6, 2));
    REQUIRE(SameRect(rects[2], 0, 2, 6, 3));
}

TEST_CASE(alpha_mask_rects_respect_threshold_and_empty_images)
{
    const UINT width = 3;
    std::vector<BYTE> pixels = MaskPixels({"###"}, width);
    // Semi-transparent edge pixel: inside at threshold 0, outside at 128.
    pixels[3] = 100;
    REQUIRE(AlphaMaskRects(pixels.data(), width, 1, width * 4, 0).size() == 1);
    const std::vector<RECT> strict = AlphaMaskRects(pixels.data(), width, 1, width * 4, 128);
    REQUIRE(strict.size() == 1);
    REQUIRE(SameRect(strict[0], 1, 0, 3, 1));

    const std::vector<BYTE> clear = MaskPixels({"...", "..."}, width);
    REQUIRE(AlphaMaskRects(clear.data(), width, 2, width * 4, 0).empty());
    REQUIRE(AlphaMaskRects(nullptr, width, 2, width * 4, 0).empty());
}
