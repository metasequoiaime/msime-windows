#pragma once

#include "utils/window_utils.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

class CandidatePresenter
{
  public:
    static CandidatePresenter &Instance();

    bool Bind(HWND hwnd);
    bool IsBound() const;
    void ShowFromGlobalState();
    void ShowFromGlobalState(POINT caret);
    void Hide();
    void Present();
    bool HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    // Puts the opaque card's top-left exactly on this physical point instead of
    // deriving it from the caret, for when no caret position is known at all.
    // nullptr returns to caret placement; Hide() clears it too.
    void SetPinnedCardOrigin(const POINT *origin);

  private:
    CandidatePresenter();
    ~CandidatePresenter();
    CandidatePresenter(const CandidatePresenter &) = delete;
    CandidatePresenter &operator=(const CandidatePresenter &) = delete;

    void RebuildScene();
    void ApplySkin();
    // Returns the generation of the snapshot that was actually rendered, so the caller can echo it
    // back to Global::rendered_candidate_page_generation once the paint is on screen.
    std::uint64_t FillItemsFromUi();
    // `scale` carries the scale resolved once per show in ShowFromGlobalState
    // so measure, clamping, sizing and rendering all share one source; an
    // unset (scale == 0) value makes PlaceAndShow resolve it itself.
    void PlaceAndShow(POINT caret, float widthDip, float heightDip, float cardLeftDip, float cardTopDip,
                      const ResolvedCandidateScale &scale = {});
    // Hide leaves the host cloaked; clearing it keeps the next uncloak from
    // briefly showing the previous card.
    void PresentEmptyFrame();
    void ArmHoverIfPointerMoved();
    void NoteHoverOverItem(POINT clientPoint);
    void CommitItem(size_t pageIndex);
    // 卷轴展开面板：点选网格里的候选、切换展开、滚到底部时再要一批、滚轮。
    void CommitExpandedItem(size_t gridIndex);
    void SetExpanded(bool expanded);
    void ToggleExpanded();
    void RequestMoreExpandedIfNearEnd();
    void ScrollExpanded(short delta);
    void ShowItemContextMenu(size_t pageIndex, POINT clientPoint);
    void CloseContextMenu(bool restoreHost);
    void OpenFixSubmenu();
    void CloseFixSubmenu();
    void ExpandHostForMenu(POINT clientPoint);
    void RestoreHostAfterMenu();
    // 候选翻译竖排面板：把面板 Popup 挂到高亮候选下方，并在 PlaceAndShow 之前把宿主窗口
    // 撑到装得下它（同一帧画出来，不先画一帧没有面板的）。need* 返回所需布局尺寸。
    void CloseTranslationPanel();
    bool PrepareExpansionPanel(float &needWidthDip, float &needHeightDip, POINT caret, float scale);
    // 动图的帧时钟：Present 之后按场景给出的下一次换帧时刻排一个一次性定时器，到点再 Present。
    // 只在候选窗显示时排；隐藏、换皮肤、窗口销毁时停。
    void ScheduleAnimationFrame();
    void StopAnimationFrames();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    HWND hwnd_ = nullptr;
    bool bound_ = false;
    bool hoverArmed_ = false;
    bool ignoreSelectionCallback_ = false;
    // Set while PlaceAndShow resizes the host; it presents right afterwards.
    bool placingHost_ = false;
    bool cardOriginPinned_ = false;
    POINT pinnedCardOrigin_{};
    POINT hoverBaseline_{};
    // 已为哪一页报过「指针停在候选上」，同一页只报一次。
    std::uint64_t hoverNotedGeneration_ = 0;
    // 卡片最小宽度：默认 160，外部皮肤的 min_width_dip 与装饰图宽度可以把它撑大，
    // 装饰图因此始终落在卡片宽度之内。
    float cardMinWidthDip_ = 160.0f;
    // 皮肤开了 fit_content：没有默认的 160，卡片宽度也不在同一输入里只增不减。
    bool fitContent_ = false;
    // 只有 v2（canvas）皮肤播放动图；v1 皮肤的图片保持原先的静态首帧。
    bool skinAnimates_ = false;
    // 皮肤要亚克力且系统开着透明效果时，卡片下面垫一层系统毛玻璃（DeviceResources 的 backdrop composition）。
    bool backdropActive_ = false;
    // 这台机器建不起 Windows.UI.Composition（低于 Win10 1903 等）时置位，本次运行不再尝试，退回交换链。
    bool backdropUnavailable_ = false;
    float backdropRadiusDip_ = 0.0f;
    // 动画从候选窗这一次显示起算；隐藏或换皮肤后，下一次显示重新从第一帧开始。
    bool animationClockRunning_ = false;
    std::string lastSkinFingerprint_;
    int lastHostWidthPx_ = 0;
    int lastHostHeightPx_ = 0;
    float lastLayoutWidthDip_ = 0.0f;
    float lastLayoutHeightDip_ = 0.0f;
    // Largest card size kept for the current input (preedit + caret + page,
    // stickyCardKey_); a new key or Hide() starts from the natural size.
    std::wstring stickyCardKey_;
    float stickyCardWidthDip_ = 0.0f;
    float stickyCardHeightDip_ = 0.0f;
    int wheelDeltaAccumulator_ = 0;
    // 卷轴展开面板的界面状态。展开与收起只认展开按钮，继续输入时保持；候选窗关闭时收起。
    bool expanded_ = false;
    // 网格里显示的后续候选，取自最近一次画出来的候选页；expandedStart_ 是第一个的绝对下标。
    int expandedStart_ = 0;
    std::vector<std::wstring> expandedWords_;
    // 上一次请求再展开时网格里有多少个候选，同一批不重复请求。
    size_t expandRequestedCount_ = 0;
    int expandRows_ = 4;
    float expandRowStepDip_ = 30.0f;
};
