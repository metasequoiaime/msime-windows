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

TEST_CASE(scene_root_offset_moves_content_without_resizing_the_viewport)
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
    // Only the origin moves: the root keeps the slot size and its measured height.
    REQUIRE(root->GetBounds().width == 200.0f);
    REQUIRE(root->GetBounds().height == 30.0f);

    scene.SetRootOffset({0.0f, 0.0f});
    scene.EnsureLayout({200.0f, 100.0f});
    REQUIRE(root->GetBounds().x == 0.0f);
    REQUIRE(root->GetBounds().y == 0.0f);
}
