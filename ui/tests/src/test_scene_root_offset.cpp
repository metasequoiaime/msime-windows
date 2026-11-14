#include "tests/includes/test_framework.h"

#include "msimeui/Layout.h"
#include "msimeui/Scene.h"

#include <memory>

using namespace msimeui;

namespace
{
class OffsetProbe : public Visual
{
  public:
    SizeF Measure(const SizeF &) override
    {
        return {80.0f, 30.0f};
    }

    void Arrange(const RectF &rect) override
    {
        bounds_ = rect;
    }

    void Render(DeviceResources &) override
    {
    }
};
} // namespace

TEST_CASE(scene_root_offset_moves_content_and_keeps_it_inside_the_viewport)
{
    auto root = std::make_shared<OffsetProbe>();
    Scene scene;
    scene.SetRoot(root);
    scene.Layout({200.0f, 100.0f});
    REQUIRE(root->GetBounds().x == 0.0f);
    REQUIRE(root->GetBounds().y == 0.0f);

    // A host that grows left/up shifts the root by the same amount so the card
    // it draws keeps its position on screen.
    scene.SetRootOffset({12.0f, 34.0f});
    scene.EnsureLayout({200.0f, 100.0f});
    REQUIRE(root->GetBounds().x == 12.0f);
    REQUIRE(root->GetBounds().y == 34.0f);
    // The root gets the space left past the offset: its right edge stays on the
    // viewport's right edge instead of running 12 DIP past it.
    REQUIRE(root->GetBounds().width == 188.0f);
    REQUIRE(root->GetBounds().x + root->GetBounds().width == 200.0f);
    REQUIRE(root->GetBounds().y + root->GetBounds().height <= 100.0f);

    // An offset past the viewport leaves an empty slot rather than a negative one.
    scene.SetRootOffset({250.0f, 150.0f});
    scene.EnsureLayout({200.0f, 100.0f});
    REQUIRE(root->GetBounds().width == 0.0f);

    scene.SetRootOffset({0.0f, 0.0f});
    scene.EnsureLayout({200.0f, 100.0f});
    REQUIRE(root->GetBounds().x == 0.0f);
    REQUIRE(root->GetBounds().y == 0.0f);
    REQUIRE(root->GetBounds().width == 200.0f);
}
