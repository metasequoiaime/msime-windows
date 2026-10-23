#pragma once

#include "settings/native/native_settings_controls.h"

#include <string>

namespace native_settings
{
// 胶囊状状态徽标（`.status-pill`）：小圆点加一句短文字，用于页首说明当前是否已经配置好。
// 颜色沿用 StatusText 的 success / error / pending 三档。
class StatusPill : public msimeui::Visual
{
  public:
    explicit StatusPill(std::wstring text = {}, std::string kind = "pending");

    void Set(std::wstring text, std::string kind);
    void Clear();

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;

  private:
    TextLayout layout_;
    std::wstring text_;
    std::string kind_;
    float measuredWidth_ = 0.0f;
};
} // namespace native_settings
