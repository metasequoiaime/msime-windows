#include "window/floating_toolbar_image_layout.h"

std::vector<ToolbarImageButtonSlot> LayoutToolbarImageButtons(const CandidateSkinCatalog::Package &package,
                                                              const std::vector<ToolbarImageButtonRequest> &shown)
{
    using CandidateSkinCatalog::ToolbarButton;
    const ToolbarButton &defaults = package.toolbarButtonDefaults;
    const std::optional<CandidateSkinCatalog::ToolbarButtonFlow> &flow = package.toolbarButtonFlow;
    // 排到哪了：按钮框左上角（DIP）。
    double flowX = flow ? flow->xDip : 0.0;
    double flowY = flow ? flow->yDip : 0.0;

    std::vector<ToolbarImageButtonSlot> slots;
    const ToolbarButton none;
    for (const ToolbarImageButtonRequest &request : shown)
    {
        const auto found = package.toolbarButtons.find(request.key);
        const ToolbarButton &button = found != package.toolbarButtons.end() ? found->second : none;
        if (!button.visible.value_or(true))
        {
            continue;
        }
        ToolbarImageButtonSlot slot;
        slot.key = request.key;
        slot.widthDip =
            button.widthDip.value_or(defaults.widthDip.value_or(CandidateSkinCatalog::kDefaultToolbarButtonSizeDip));
        slot.heightDip =
            button.heightDip.value_or(defaults.heightDip.value_or(CandidateSkinCatalog::kDefaultToolbarButtonSizeDip));
        slot.iconSizeDip = button.iconSizeDip.value_or(defaults.iconSizeDip.value_or(0.0));
        slot.background = !button.background.empty() ? button.background : defaults.background;
        const auto stateImage = button.images.find(request.state);
        slot.image = stateImage != button.images.end() ? stateImage->second : button.image;
        if (button.xDip && button.yDip)
        {
            slot.xDip = *button.xDip;
            slot.yDip = *button.yDip;
        }
        else if (flow)
        {
            slot.xDip = flowX;
            slot.yDip = flowY;
            if (flow->vertical)
            {
                flowY += slot.heightDip + flow->gapDip;
            }
            else
            {
                flowX += slot.widthDip + flow->gapDip;
            }
        }
        else
        {
            continue;
        }
        slots.push_back(std::move(slot));
    }
    return slots;
}
