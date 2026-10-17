#include "tests/includes/test_framework.h"

#include "msimeui/Layout.h"

#include <memory>

using namespace msimeui;

namespace
{
// 固定尺寸的叶子控件，用于观察 OverlayPanel 的测量与绝对摆放。
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
} // namespace

TEST_CASE(overlay_panel_positions_children_absolutely)
{
    auto panel = std::make_shared<OverlayPanel>();
    auto background = std::make_shared<FixedBox>(SizeF{100.0f, 50.0f});
    auto icon = std::make_shared<FixedBox>(SizeF{20.0f, 20.0f});
    panel->AddChildAt(background, 0.0f, 0.0f);
    panel->AddChildAt(icon, 90.0f, 40.0f);

    const SizeF measured = panel->MeasureInLayout({1000.0f, 1000.0f});
    REQUIRE_NEAR(measured.width, 110.0f); // max(0+100, 90+20)
    REQUIRE_NEAR(measured.height, 60.0f); // max(0+50, 40+20)

    panel->ArrangeInLayout({0.0f, 0.0f, measured.width, measured.height});

    const RectF backgroundBounds = background->GetBounds();
    REQUIRE_NEAR(backgroundBounds.x, 0.0f);
    REQUIRE_NEAR(backgroundBounds.y, 0.0f);
    REQUIRE_NEAR(backgroundBounds.width, 100.0f);
    REQUIRE_NEAR(backgroundBounds.height, 50.0f);

    const RectF iconBounds = icon->GetBounds();
    REQUIRE_NEAR(iconBounds.x, 90.0f);
    REQUIRE_NEAR(iconBounds.y, 40.0f);
    REQUIRE_NEAR(iconBounds.width, 20.0f);
    REQUIRE_NEAR(iconBounds.height, 20.0f);
}

TEST_CASE(overlay_panel_set_position_relocates_child)
{
    auto panel = std::make_shared<OverlayPanel>();
    auto child = std::make_shared<FixedBox>(SizeF{10.0f, 10.0f});
    panel->AddChildAt(child, 0.0f, 0.0f);

    panel->SetPosition(0, 5.0f, 6.0f);
    const SizeF measured = panel->MeasureInLayout({100.0f, 100.0f});
    panel->ArrangeInLayout({0.0f, 0.0f, measured.width, measured.height});

    const RectF bounds = child->GetBounds();
    REQUIRE_NEAR(bounds.x, 5.0f);
    REQUIRE_NEAR(bounds.y, 6.0f);
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

    // 后加的 icon 叠在背景之上，命中时优先返回 icon。
    REQUIRE(panel->FindVisualAt({55.0f, 55.0f}) == icon.get());
}
