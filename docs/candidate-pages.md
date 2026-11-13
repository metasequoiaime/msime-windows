# 候选页发布与交互：CandidatePages

- 状态：已实现（2026-11-06）
- 范围：`server/` 候选窗；不改 IPC、引擎、配置和 Direct2D presenter

## 背景

候选页曾经有两套真相：

- worker 线程的 `Global::candidate_ui.items`：每次按键、异步混输、神经重排都会重建，并推进发布代次；
- UI 线程实际画出的 `CandidatePageSnapshot`：用户看到、悬浮/点击/数字键选词所依据的那一页。

发布、show/deferred、settle、latch、选词分别读不同的一套，导致过：

- 首屏没变也会发布新代次，选词要等一次从未发生的绘制；
- 用户在旧页上点击，latch 冻结的却是新 `items` 的顺序；
- 连打时每键 reset settle，快速结果在整个连打期间不上屏；
- 页大小、横排容量、末页不满时，“可见首页”的判定和实际渲染页不一致。

## 模块

`server/src/ipc/candidate_pages.h` 的 `FanyImeIpc::CandidatePages` 把 **build 之后的 publish → show/defer → rendered echo → interaction/selection** 收进同一个 worker-affine 模块。

```cpp
enum class CandidateFreshness { Fast, Late, Immediate };

struct CandidatePage {
    std::string input;
    std::uint64_t layout_epoch;
    int page_size;
    Global::CandidatePageSnapshotPtr snapshot;
    std::shared_ptr<const std::vector<WordItem>> items;
    std::uint64_t generation;
};

struct CandidateChoice {
    std::shared_ptr<const CandidatePage> page;
    WordItem item;
    int index_in_page;
    int absolute_index;
    bool waited;
    bool timed_out;
};

class CandidatePages {
public:
    CandidatePages(ClockPort &, SurfacePort &, milliseconds settle = 120ms, milliseconds wait = 80ms);

    void Publish(CandidatePage);                 // 静默发布：下一次 show 画它，不请求帧
    void Show(CandidatePage);                    // 首帧/首选/翻页/换布局：发布并立即请求显示
    void Show();                                 // 显示已经发布的那一页
    bool Update(CandidatePage, CandidateFreshness); // Fast 一帧，Late 一帧，Immediate 每次显示
    bool UpdateCurrent(CandidateFreshness);      // 给已发布页排一次 Fast/Late 帧
    bool HasPage() const;

    void Hide();
    void ResetFrameBudget();                     // 新按键：清掉上一段的帧预算
    void CancelPending();                        // 选词/隐藏：挂起帧不落地，也不在本段重试

    std::optional<CandidateChoice> CommitSlot(int index_in_page);       // 数字键
    std::optional<CandidateChoice> CommitHighlighted();                 // 空格
    std::optional<CandidateChoice> CommitAt(int index_in_page);         // 鼠标：不等待

    // UI 线程
    bool Painted(std::uint64_t generation, bool visible);
    void Gesture(std::uint64_t rendered_generation);
    void Visibility(bool visible);
};
```

## 不变量

1. **已渲染页是交互的唯一真相。** `Commit*` 只解析当前输入、`Painted` 回显过的页；`CandidateChoice` 自带页快照，上屏、调频、造词不得再读 worker 的最新 `items`。没有当前页可用时（首帧尚未画出或 UI-less 宿主）才用已发布页，避免丢键。
2. **发布不可变、代次单调。** `Publish/Show/Update` 只发布不可变 `CandidatePage`；同一 input 的 Fast 至多一帧、Late 至多一帧；`Immediate`（云/AI）不受此预算限制。同一 input 的同步重建只替换挂起帧的页，不额外给一帧。
3. **首帧立即。** `Show` 先发布再请求显示，不等时钟。
4. **latch 只冻结已渲染页。** `Gesture` 仅在 `generation == rendered_generation` 且页面可见时生效；生效后异步 `Update` 丢弃，直到新 input 的页发布或隐藏。
5. **键盘选词有界等待，鼠标不等待。** `CommitSlot/Highlighted` 只在当前页尚未画出、窗口可见且非 UI-less 时最多等 80ms；等待前先请求一帧。超时仍提交已渲染页；没有可用的已渲染页时才提交已发布页。`CommitAt` 不等待。
6. **隐藏清空可见真相。** `Hide` 清掉 published/rendered/pending/latch；迟到结果不能复活候选窗。
7. **UI-less 宿主画的是共享页。** 没有绘制回显时，Fast/Late/Immediate 都立即发布；选词直接解析已发布页。

## 依赖与适配器

- `ClockPort`：生产用 steady_clock + worker 定时队列；测试用假时钟。延迟帧的排期在模块内，不暴露 `NextWakeup`。
- `SurfacePort`：`publish(page)` 写共享内存和不可变快照，`request_show()/hide()` 发窗口消息，`visible()/uiless()` 读宿主状态。D2D presenter 继续从快照绘制；去重命中时也回显 `Painted`。
- 组合（候选词、辅助码、徽标、译文、候选混输策略）留在调用方；模块只认“页是值”，不认候选语义。

## 影响

- 调用方从约 15 个发布/show/settle/latch/wait 点收敛为 `Publish/Show/Update/Commit*`；
- 旧的 `CandidatePresentSettle` 及其测试已删除；fast/late 预算、定时、latch 和渲染等待都在模块内部；
- 测试在模块接口上用假时钟、假 Surface、手工 `Painted` 覆盖发布未画、连打丢帧、旧页点击、页大小错位和 UI-less 选词。

## 备选

- **最小三入口（Submit/Painted/Pick）**：深度最高，但不覆盖页大小/布局 epoch 和多场景；
- **多 presenter Stage**：支持多窗、reflow、快照环，但当前产品只有一个候选窗 + UI-less 宿主，属于过度设计；
- **维持现状分散状态**：已证明会产生两套真相的时序 bug。
