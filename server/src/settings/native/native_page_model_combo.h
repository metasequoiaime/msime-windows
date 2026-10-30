#pragma once

#include "settings/native/native_page_voice_fields.h"

#include "msimeui/Controls.h"
#include "msimeui/DeviceResources.h"
#include "msimeui/Layout.h"
#include "msimeui/Scene.h"
#include "msimeui/Window.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// 模型下拉：`.dropdown-toggle.font-combobox` 加一个可滚动的候选列表，「查询模型」把它填满。
// AI 联想页与语音润色页共用这一个控件：模型清单端点只依赖 Token 与接口地址
// （见 ApiCredentialTest::FetchModels），两页的差别只在回调里写哪个配置键。
namespace native_settings::voice_fields
{
class ModelCombo;

// `#aiModelMenu.font-dropdown-menu.compact-scroll-menu` 的行列表。
class ModelList : public msimeui::Visual
{
  public:
    static constexpr float kItem = 28.0f;
    static constexpr float kGap = 2.0f;
    static constexpr float kInset = 2.0f;

    explicit ModelList(ModelCombo *owner);

    void SetItems(std::vector<std::wstring> items);
    bool Empty() const;
    float ContentHeight() const;

    SizeF Measure(const SizeF &) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    msimeui::Visual *FindFocusableAt(const PointF &point) override;
    bool OnMouseMove(const POINT &point, WPARAM keyState) override;
    void OnMouseLeave() override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    bool OnMouseWheel(const POINT &point, short delta, WPARAM keyState) override;

  private:
    static constexpr size_t kNone = static_cast<size_t>(-1);

    RectF ItemRect(size_t i) const;
    size_t HitRow(const PointF &point) const;
    void LayoutScrollbar();

    ModelCombo *owner_ = nullptr;
    std::vector<std::wstring> items_;
    size_t hovered_ = kNone;
    size_t pressed_ = kNone;
    CompactMenuScrollbar scrollbar_;
};

// 承载 ModelList 的弹出层，滚轮事件转给它。
class ModelPopup : public msimeui::Popup
{
  public:
    explicit ModelPopup(const std::shared_ptr<ModelList> &list);

    void Render(msimeui::DeviceResources &dr) override;
    bool OnMouseWheel(const POINT &point, short delta, WPARAM keyState) override;

  private:
    ModelList *list_ = nullptr;
};

class ModelCombo : public msimeui::Visual
{
  public:
    using Handler = std::function<void(const std::wstring &)>;

    ModelCombo();

    std::shared_ptr<InputBox> Input() const
    {
        return input_;
    }

    void SetOnPicked(Handler handler)
    {
        onPicked_ = std::move(handler);
    }

    // 还没有候选列表时点箭头：等同于「查询模型」。
    void SetOnFetch(std::function<void()> handler)
    {
        onFetch_ = std::move(handler);
    }

    void SetModels(std::vector<std::wstring> models);
    bool HasModels() const;
    // 写进输入框并触发一次 OnPicked，供列表选中时调用。
    void Pick(const std::wstring &model);
    void Open();
    void Close();

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(msimeui::DeviceResources &dr) override;
    void Attach(msimeui::Window *window) override;
    msimeui::Visual *FindVisualAt(const PointF &point) override;
    msimeui::Visual *FindFocusableAt(const PointF &point) override;
    bool KeepsPopupsOpenOnClick() const override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    void OnMouseEnter() override;
    void OnMouseLeave() override;

  private:
    RectF ArrowRect() const;

    std::shared_ptr<InputBox> input_;
    std::shared_ptr<ModelList> list_;
    std::shared_ptr<ModelPopup> popup_;
    Handler onPicked_;
    std::function<void()> onFetch_;
    bool hovered_ = false;
    bool pressed_ = false;
    bool open_ = false;
};
} // namespace native_settings::voice_fields
