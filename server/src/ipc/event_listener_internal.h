#pragma once

// event_listener*.cpp 之间共享的内部声明：拆分前同在 event_listener.cpp 里、现在跨文件使用的状态、类型与函数。
// 生产代码只给 server/src/ipc/event_listener*.cpp 包含；另允许覆盖任务分级纯策略的
// server/tests/src/test_task_classes.cpp 包含，其余地方不要引用。

#include <Windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "ipc.h"
#include "ipc/async_request_origin.h"
#include "ipc/candidate_expansion_panel_policy.h"
#include "ipc/candidate_selection_policy.h"
#include "ipc/candidate_text_policy.h"
#include "ipc/candidate_ui_owner.h"
#include "ipc/traditional_variant_expansion_policy.h"
#include "ipc/event_listener.h"
#include "engine/contracts/v_mode_input.h"
#include "window/caret_state_indicator_policy.h"

// IPC logging is compiled out. The macros still have to *mention* their arguments, otherwise every
// parameter of the log helpers below is unreferenced (C4100). sizeof keeps the arguments in an
// unevaluated context, so nothing is computed and no side effect runs — only the name is used.
template <typename... Args> int FanyIpcDiscardLogArgs(const Args &...);
#define FANY_IPC_LOG_RAW(message) ((void)sizeof(FanyIpcDiscardLogArgs(message)))
#define FANY_IPC_LOGW(message) ((void)sizeof(FanyIpcDiscardLogArgs(message)))
#define FANY_IPC_LOGF(...) ((void)sizeof(FanyIpcDiscardLogArgs(__VA_ARGS__)))

namespace event_listener_detail
{
void ClearJevSelection();
void RefreshJevSelection();
const std::string &JevSelectionWord();
using AsyncRequestOrigin = FanyImeIpc::AsyncRequestOrigin;

extern bool g_quick_phrase_triggered;
extern bool g_radical_mode_triggered;
extern bool g_date_time_mode_triggered;
extern bool g_emoji_mode_triggered;
extern bool g_kaomoji_mode_triggered;
extern bool g_symbol_mode_triggered;
extern bool g_jianpin_mode_triggered;
extern bool g_y_mode_triggered;
extern bool g_r_mode_triggered;
extern std::shared_ptr<IInputSession> g_r_mode_original_session;
// The other native language's session, parked by a language cycle so switching
// back neither rebuilds the engine nor reopens msime.db on the key path. Worker
// thread only, like g_inputSession; a task that recreates the session from
// config drops it, since a scheme or profile change makes it stale.
extern std::shared_ptr<IInputSession> g_language_cycle_parked_session;
extern bool g_english_input_mode;
// Glosses are a cache keyed by TranslationIdentity, not the current page's
// results: the next keystroke's page mostly repeats the same words, and
// rebuilding it without their glosses first shrank every row, then grew it back
// once the lookup answered. Bounded like the cloud cache, and dropped whenever
// the provider set changes (the target language is already in the identity).
constexpr size_t kMaxCandidateTranslationGlosses = 2048;
struct CandidateTranslationGloss
{
    std::string text;
    // english.db glosses use ", " between synonyms of one sense; a cloud gloss is
    // a sentence whose commas are punctuation, so only dictionary glosses may be
    // trimmed at them.
    bool from_dictionary = false;
};
extern std::unordered_map<std::string, CandidateTranslationGloss> g_candidate_translation_glosses;
extern std::string g_candidate_translation_signature;
extern bool g_translation_candidates_active;
// 日期页：选中混输里的「📅日期」入口后，候选框换成整组日期时间格式。和译文页一样不动输入串，
// 两者互斥，共用下面这份保存的候选与页码，退出时原样放回。
extern bool g_date_time_page_active;
extern std::string g_date_time_page_keyword;
extern std::vector<WordItem> g_translation_saved_items;
extern int g_translation_saved_page_index;
extern int g_translation_saved_selected_index;
extern bool g_translation_saved_selection_started;
extern bool g_dedicated_english_answer_pending;
extern int g_authoritative_cn_mode;

std::string TranslationIdentity(const EnglishIme::TranslationQuery &query);
// 译文页或日期页正占着候选框：items 不是这次输入的候选，异步结果不能往里合并，也不能按会话扩展。
bool IsCandidateSubPageActive();
// 把界面线程记下的鼠标操作落到 candidate_ui.selection_started。工作线程每取一个任务先调一次，
// 这样排在鼠标操作之前入队的异步结果也看得到它。
void LatchCandidateUiInteraction();
// 异步候选（云、AI、英文、混输、重排）能否改动当前列表：子页占着或用户已开始选词时都不行。
bool CanMergeAsyncCandidates();
bool IsUiLessMode();
void ApplyUiLessFromPacket(const FanyImeNamedpipeData &pipe_data);
void RequestShowCandidateWindow();
void HideCandidateWindowAndDropItems();
bool IsQuickPhraseCompositionActive(const std::string &raw);
// U 模式（部件拆字反查）：Shift+U 的唤醒标记仍在，且输入串以大写 U 开头。U 之后的字符不看。
bool IsRadicalCompositionActive(const std::string &raw);
bool IsDateTimeCompositionActive(const std::string &raw);
bool IsEmojiCompositionActive(const std::string &raw);
bool IsKaomojiCompositionActive(const std::string &raw);
// S 模式（符号快捷输入）：唤醒键 S 之后是一串字母编码。
bool IsSymbolCompositionActive(const std::string &raw);
bool IsJianpinCompositionActive(const std::string &raw);
bool IsYModeCompositionActive(const std::string &raw);
bool IsYModeInput(const std::string &raw);
// 当前方案下哪个前缀开启 V 模式，和发给 TSF 的 VModeChanged 是同一个值。
FanyImeVModeInput::Trigger CurrentVModeTrigger();
bool IsVModeCompositionActive(const std::string &raw);
void ClearSpecialModeTriggers();
bool IsSpecialModeCompositionActive(const std::string &raw);
// 五笔会话里属于 T/V 模式的串：交给引擎时标成字面输入，原样保留、不查码表、不算完整四码。
bool IsWubiLiteralComposition(const std::string &raw);

void PublishCandidateUiOwner(uint64_t client_id, uint64_t activation_epoch);
FanyImeIpc::CandidateUiOwner SnapshotCandidateUiOwner();
bool CandidateUiOwnerIsCurrent(const FanyImeIpc::CandidateUiOwner &owner);
void PublishStatusSnapshotValue(int packed_state);
void SetEnglishInputMode(bool enabled);
void RememberClientStatusSnapshot(uint64_t client_id, int packed_state);
void ForgetClientStatusSnapshot(uint64_t client_id);
int RecallClientStatusSnapshot(uint64_t client_id);
void PostCaretStateBadge(FanyImeUi::CaretStateBadge badge, int x, int y);

void UpdateCloudInput(const std::string &input, uint64_t client_id = 0, uint64_t activation_epoch = 0);
void UpdateEnglishInput(const std::string &input, uint64_t client_id = 0, uint64_t activation_epoch = 0,
                        bool dedicated_mode = false);
// 混输（英文、emoji、颜文字、日期时间）的查询请求；request.input 为空表示取消。
void UpdateMixedInput(MixedCandidates::Request request, uint64_t client_id = 0, uint64_t activation_epoch = 0);
void UpdateAiInput(const std::string &identity, uint64_t client_id = 0, uint64_t activation_epoch = 0);
AsyncRequestOrigin FindCloudRequestOrigin(const std::string &input, uint64_t generation);
AsyncRequestOrigin FindEnglishRequestOrigin(const std::string &input, uint64_t generation);
AsyncRequestOrigin FindMixedRequestOrigin(const std::string &input, uint64_t generation);
AsyncRequestOrigin FindAiRequestOrigin(const std::string &input, uint64_t generation);
// 不属于当前候选列表的文字的输出形式：繁体字符集下做简转繁，日语方案原样输出。这类文字有只有一条译义时
// 直接上屏的那条译义、AI 上下文、按键原串。当前候选列表里的某条候选（含译文页上列出的译义）不要用它，
// 用 CandidateOutputText；造词前缀用 creating_word.prefix_text。
std::string CandidateTextForOutput(const std::string &text);
// 当前候选列表是 U 模式（部件拆字）的结果：唤醒标记仍在、输入串以大写 U 开头，且不在译文页上。
// 组合一旦被 reset_state 或 ClearSpecialModeTriggers 清掉就回 false，要在清之前判。
bool IsRadicalCandidateListActive();
// 当前显示的候选用的字形（"traditional" 或 "simplified"）：主列表是组页时记下的（CandidateListCharacterSet），
// 译文页、日期页是当前配置。显示、上屏以及与显示文字比较的第一写法都按它取。只在任务线程上调用。
// 选词时要在推进组合、为下一段重新组页之前取，之后读到的是下一张列表的字形。
const std::string &ShownCandidateCharacterSet();
// 当前候选列表里一条候选的显示与上屏文字，字形取 ShownCandidateCharacterSet。
// 不做简转繁时（简体、日语方案、U 模式拆字结果）是 word；繁体时是展开填的写法，没展开的候选转成繁体。
// 同一条候选的显示文字与上屏文字都要取自这里，两者才一致。要在 reset_state 之前取，理由同
// IsRadicalCandidateListActive。只在任务线程上调用。
std::string CandidateOutputText(const WordItem &item);
// 按当前候选列表组页时的字形把候选展开成每种写法一项（规则见 FanyImeIpc::ExpandTraditionalVariants）；
// 不做简转繁时原样返回。
std::vector<WordItem> WithTraditionalVariants(std::vector<WordItem> items);
// 同 WithTraditionalVariants，字形取 character_set（"traditional" 或 "simplified"），不读配置。
std::vector<WordItem> WithTraditionalVariantsFor(std::vector<WordItem> items, const std::string &character_set);
// 在每组写法只占一项的列表上做 reorder（apply_fixed_positions、place_quick_phrases 这类按 word 删插的
// 重排），再按组页时的字形重新展开（规则见 FanyImeIpc::ReorderAsOneItemPerWord）。items 可以是已展开的
// ui.items；不做简转繁时等于直接 reorder(items)。
void ReorderCandidatesAsOneItemPerWord(std::vector<WordItem> &items,
                                       const std::function<void(std::vector<WordItem> &)> &reorder);
// 当前候选列表组页时的字形、方案、U 模式拆字列表状态与繁体写法地区，交给 FanyImeIpc 里填写法与判定
// 异步插入的纯函数。
FanyImeIpc::CandidateSpellingSettings CandidateListSpellingSettings();
// 当前候选列表是按繁体组的页时，给一条要插进列表的云或 AI 候选填上排序后的第一写法；否则原样返回
// （规则见 FanyImeIpc::WithFirstTraditionalSpellingOf）。
WordItem WithFirstTraditionalSpelling(WordItem item);
// 一段简体文字整串的第一写法：ShownCandidateCharacterSet 是繁体（非日语方案）时取排序后的第一种写法，
// 否则原样返回。不看 U 模式：造词前缀不来自拆字结果。
std::string FirstSpellingOfText(const std::string &text);
// 当前造词已选部分（GlobalIme::composition.creating_word）的简体底词、显示文字与第一写法标记。
// ShownCandidateCharacterSet 与第一段选中时不同，第一写法标记为 false，上屏按显示的文字拼接。
FanyImeIpc::ChosenText CreatingWordChosenText();
// 每个按键包到达时调用：记下它是否来自密码 / PIN 输入框，并在宿主或焦点会话变化时清空 AI 上下文。
void NoteAiContextKeySource(uint64_t client_id, uint64_t activation_epoch, bool sensitive_field);
void AppendAiContext(const std::string &committed_word);
// current_output_text 是造词前缀的输出文字（creating_word.prefix_text），原样放进管道。
std::wstring BuildCreateWordPipePayload(const std::string &remaining_raw_input_with_cases,
                                        const std::string &current_output_text);
// segmented_pinyin 定下来之后调用：双拼组合时把原串切分和全拼切分记进 GlobalIme::composition，
// 供「双拼显示全拼」使用，其余组合清空。
void SyncShuangpinPreeditForms();

// event_listener_candidates.cpp
void EnsureCandidatePageReady();
std::wstring BuildUiLessCandidatePageW();
void RefreshCandidatePageUi(bool show_window);

// event_listener_pipes.cpp
void WakeNamedPipeListenersForShutdown();
} // namespace event_listener_detail

namespace FanyNamedPipe
{
enum class TaskType
{
    ShowCandidate,
    HideCandidate,
    HideCaretState,
    MoveCandidate,
    ImeKeyEvent,
    LangbarRightClick,
    IMESwitch,
    PuncSwitch,
    DoubleSingleByteSwitch,
    ApplyCloudCandidate,
    ApplyAiCandidate,
    ApplyEnglishCandidates,
    ApplyCandidateTranslations,
    ApplyMixedCandidates,
    StoreUserPhrase,
    ClientActivated,
    ClientDeactivated,
    ClientSuspended,
    StatusSnapshot,
    UiCommitCandidate,
    UiPinCandidate,
    UiDeleteCandidate,
    UiFixCandidatePosition,
    UiClearCandidatePosition,
    UiPageUp,
    UiPageDown,
    UiCommitCandidateAt,
    UiExpandCandidates,
    // 竖排译文面板被点了外面：presenter 请工作线程把面板状态关掉。
    UiCloseTranslationPanel,
    // 候选词展开面板里点选了某条译义：与数字键 1..N 上屏同一条路径。
    UiCommitExpansionPanelItem,
    ReloadInputSession,
    EnsureInputSessionMatchesConfig,
    ApplyCandidatePageSize,
    RefreshCandidatePage,
    ResetInputSessionCache,
    ExitEnglishInputMode,
    ToolbarLanguageToggle,
    // 神经整句重排在后台线程里算完了，候选顺序需要就地更新一次。
    ApplyRescoredOrder,
    // 停手到点后把效果档放回来，并按当前组合重查一次候选。
    RefreshWithEnrichment,
};

// 任务分级：决定 WorkerThread 先取哪条队列。按键和它触发的界面动作是用户正在等的结果，
// 后台富化（云/AI/英文/翻译/混输/重排）只负责补齐内容，排在后面。Maintenance 是字典
// checkpoint、配置重载、缓存整理这类没人盯着结果、但不能无限期不做的事。
enum class TaskClass
{
    Foreground,
    UserAction,
    Enrichment,
    Maintenance,
};

constexpr std::size_t kTaskClassCount = 4;

constexpr std::size_t TaskClassIndex(TaskClass type)
{
    return static_cast<std::size_t>(type);
}

// 每条类别队列的队首状态；has_task == false 时 wait_ms 不参与判定。
struct TaskClassHead
{
    bool has_task = false;
    double wait_ms = 0.0;
};

// Maintenance 队首等过这么久就允许越过 Enrichment，防止用户一直有输入时它永远轮不到。
// 它只越过 Enrichment，Foreground / UserAction 永远在它前面。
constexpr double kMaintenanceMaxWaitMs = 500.0;

// 分类表穷举 TaskType 且不写 default：新增类型忘了归类时编译失败（见 .cpp 的 C4062 设置）。
TaskClass ClassifyTask(TaskType type);
// 排队诊断日志用的类别名；只用于观测，不参与调度。
const wchar_t *TaskClassName(TaskClass type);
// 输入线程的 scoped Above normal 只给用户正在等结果的 Foreground 任务；WorkerThread 与测试共用这一判定。
bool NeedsInputPriorityBoost(TaskClass type);
// 依据每类队首的等待时长选出下一个该出队的类。纯函数，测试直接调它，不依赖全局队列。
// 调用方保证至少一类有任务。
TaskClass NextTaskClass(const std::array<TaskClassHead, kTaskClassCount> &heads);

enum class PageMoveResult
{
    Unchanged,
    // An expansion filled out the current page; the page index did not move.
    CurrentPageRefilled,
    Moved,
};

// event_listener_candidates.cpp
// 当前候选列表按哪种字形显示（"traditional" 或 "simplified"），PrepareCandidateList 每次组页、
// RespellCandidatesForCharacterSetChange 切换字形时记下；还没组过页时是空串。只在任务线程上调用。
const std::string &CandidateListCharacterSet();
std::string CurrentRankingContextKey();
std::string EnglishRankingContextKey();
// 混输英文槽位：按小写输入记（Y 模式去掉前缀），是英文在候选列表里的下标，上限是首页末位。
std::string EnglishInputKey();
int EnglishSlotMaximum();
FanyImeIpc::EnglishPlacement CurrentEnglishPlacement(const std::vector<WordItem> &items);
// 只判断 ExpandCandidatesKeepingPagePosition 还有没有可能展开出新候选，不改任何状态。
bool CanExpandCandidatesKeepingPagePosition();
bool ExpandCandidatesKeepingPagePosition();
PageMoveResult MoveCandidatePage(int offset);
// 把当前页挪到卷轴展开面板里点中的那个候选所在的页，返回它在页内的下标；核对不上返回 -1。
int MoveToExpandedCandidate(int absolute_index, const std::wstring &expected_text);
std::pair<std::string, std::string> RankingKeysForCandidate(const WordItem &item);

// event_listener_worker.cpp
void NoteTopCommitPushed(uint64_t client_id, uint64_t activation_epoch);
void EnqueueTask(TaskType type, const FanyImeNamedpipeData &pipeData, uint64_t activation_epoch);
// wubi 指明目标候选来自五笔码表；混输组合里拼音候选写拼音词典、五笔候选写码表，不能按会话方案判断。
// ranking_candidates 为空时按当前候选页排位；句中辅助码组合传去掉约束后的候选。
void EnqueueAdjustCandidateRankingTask(bool english, bool wubi, const std::string &context_key,
                                       const std::string &entry_key, const std::string &word, uint64_t client_id,
                                       uint64_t activation_epoch,
                                       const std::vector<WordItem> *ranking_candidates = nullptr);
void EnqueueLearnEnteredEnglishWordTask(const std::string &word);
// 快捷短语组的调频。word 非空：选中了组里的这一条，first_in_group 表示它本来就排在组首；
// word 为空：越过整组选了普通候选，ordinary_rank 是它在普通候选里的名次。
void EnqueueLearnQuickPhraseOrderTask(const std::string &code, const std::string &word, bool first_in_group,
                                      int ordinary_rank, uint64_t client_id, uint64_t activation_epoch);
// 混输英文槽位的调频。selected_index 为空：选中了占槽位的英文（english_index），按名次前移；
// 否则越过了英文选了 selected_index 处的候选，英文后退一位。
void EnqueueLearnEnglishSlotTask(const std::string &code, int english_index, std::optional<int> selected_index,
                                 uint64_t client_id, uint64_t activation_epoch);
// 日期时间格式的调频：选中了 format_id（WordItem::pinyin）那种格式。
void EnqueueLearnDateTimeOrderTask(const std::string &format_id, uint64_t client_id, uint64_t activation_epoch);

// event_listener_keys.cpp
// 把译文页或日期页换回进入之前的那一屏，进入之后切过字形时按当前字形换写法；都没打开时什么都不做。
void ExitCandidateSubPage();
// 关闭候选展开面板（展开键）。面板状态只有 candidate_ui 里那一份，presenter 每帧按
// 发布快照重建；这里只清状态，调用方负责让下一帧不带面板。
void CloseCandidateExpansionPanel();
// 选中「📅日期」入口：候选框换成 keyword 这组的全部格式。查不到格式时返回 false，候选框不动。
bool EnterDateTimeCandidatePage(const std::string &keyword);
// 日期页上置顶、固定位置之后按新顺序重查，页码和高亮位留在原处。
void RebuildDateTimeCandidatePage();
// 工具栏语言按钮：开了中日英循环时和循环热键走同一步，否则仍是中英二态切换。
void HandleToolbarLanguageToggle(bool from_ime_enabled);

// event_listener.cpp
bool SendUiLessCompositionToClient(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id);
// UI-less only: tell the candidate owner that its page changed outside a key reply, so the host redraws the list
// that digits and Space now commit from. A no-op for clients that draw nothing themselves.
void PushUiLessCandidatesToOwner();

// 拆分前 event_listener.cpp 里的前置声明，定义分布在各个 event_listener*.cpp。
void PrepareCandidateList(uint64_t client_id, uint64_t activation_epoch);
// 当前候选列表记下的字形与配置不同（组字途中切换了简繁）时，记下当前字形。这张列表组页时按字形展开过写法的，
// 在它上面按当前字形换写法：候选的先后顺序不变；写法全归列表里别的底词的候选（「余」「馀」同在列表时的「馀」）
// 切到繁体时不出现，切回简体也找不回来，要到下一次组页；高亮移到原来那个底词的写法组组首，新列表里没有它时
// 移到第一页第一项。没展开过写法的列表（V 模式、日期时间等）连同页码与高亮原样不动。然后刷新本页、
// 显示并推给 UI-less 宿主，返回 true；列表为空、在译文页或日期页上、列表不随字形变（见
// FanyImeIpc::CandidateListFollowsCharacterSet）、属主已失效时什么都不做，返回 false。
// 只在任务线程上调用。
bool RespellCandidatesForCharacterSetChange();
void HandleImeKey(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id);
void ClearState();
// count_first_word = false 时这次上屏不进首词命中率（五笔四码唯一自动上屏：用户没有做选择）。
void ProcessSelectionKey(UINT keycode, uint64_t client_id, uint64_t activation_epoch, int forced_index_in_page = -1,
                         bool count_first_word = true);
void WaitForCandidateRenderSync(UINT keycode);
void ApplyCloudCandidate(const std::string &candidate, const std::string &pinyin, uint64_t generation,
                         const std::optional<metasequoia::OnlineQuery> &query);
void ApplyRescoredOrder();
// 用当前会话状态重查一次候选，保留只活在 series cache 里的云/AI 候选；候选没变就不动 UI。
// 调用方负责前置守卫（选词中/造词/子页、owner 有效性）。返回是否重发了候选页。
bool RequeryCandidatesPreservingOnline(const FanyImeIpc::CandidateUiOwner &owner);
void ApplyAiCandidate(const std::string &candidate, const std::string &identity, uint64_t generation,
                      const std::optional<metasequoia::OnlineQuery> &query);
void ApplyEnglishCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation);
void ApplyCandidateTranslations(std::vector<EnglishIme::TranslationResult> results, uint64_t generation, bool merge);
// 译文查询门控：行内注释（candidate_translations）或「展开面板真的开着（expansion_panel_active）且
// 配好展开键」任一成立就要查译文；行内是否显示另由 candidate_translations 单独控制。查询与显示解耦，
// PrepareCandidateTranslationRequest 与 ApplyCandidateTranslations 共用这一份判断，避免一边放宽另一边
// 仍卡住导致 page_glosses 为空。面板分支看的是打开状态而不是配置：面板没开、行内又关着时这条查询
// 不发生（出厂默认因此不再让所有用户每键都查译文），开面板那颗键靠下面的探测函数补高亮词的译文。
bool CandidateTranslationLookupEnabled();
// 候选展开键的开面板路径专用：面板条目为空且译义类启用时调用。同步探测高亮词（english.db 一次
// 索引查询，命中即种进 gloss 缓存），或「配了云端 provider 且云端可译」时返回 true 让面板乐观打开、
// 译义类异步回填；都不成立返回 false，调用方据此不开面板。
bool ProbeTranslationForHighlightedCandidate();
// 候选展开面板：当前高亮候选按启用类别平铺出的条目（含上屏文本与类别前缀标签）。发布快照与
// 按键路径（移动/上屏/数字选词）共用同一份，保证「面板里看到的」与「上屏的」来自同一次管线。
// CurrentExpansionCategoryOrder 按各启用类别的开关拼出启用顺序（当前：翻译、表情、颜文字）。
// CurrentExpansionAnchorWord 是高亮候选的干净词（UTF-8），emoji/颜文字取数与「换词即归零」的面板 signature
// 都以它为锚点。
std::vector<std::string> CurrentExpansionCategoryOrder();
std::vector<FanyImeIpc::ExpansionItem> CurrentExpansionPanelItems();
std::string CurrentExpansionAnchorWord();
// 面板条目上屏的公共内核：先等渲染同步再取当前面板条目，forced_index 指定面板下标（-1 用高亮位），
// 无论成败都关掉面板。上屏成功返回 true 并已置好 CommitExactText 与 selected_text；调用方负责投递
// （按键路径走 SendCurrentDataToClient，候选窗点击走 worker packet）。见 event_listener_keys.cpp。
bool CommitCandidateExpansionPanelSenseText(int forced_index, UINT render_sync_keycode);
void ApplyMixedCandidates(MixedCandidates::Result result, const std::string &input, uint64_t generation);
void EnqueueStoreUserPhraseTask(const std::string &pinyin, const std::string &word, bool pinyin_is_canonical = false);
bool ResolveCandidateItem(int one_based_index, WordItem &item);
bool SendCurrentDataToClient(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id);
} // namespace FanyNamedPipe
