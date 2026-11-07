// 自定义标点在 TSF 一侧的接线：两条分类路径怎么填吃键判定的输入，吃键时查到的 pick 怎么经消息窗口与标点上屏
// 一路带到出符号，成对栈怎么记下补出右半边的键。
//
// TSF 服务离不开宿主，没法在测试里实例化，所以这些路径从产品源码原样抽进 custom_punctuation_wiring_source.h
// （见 generate_custom_punctuation_wiring_fixture.py），它们调用的 TSF 成员在这里都是替身。期望值按施工规格手写。

#include "Composition/PunctuationKeyMap.h"
#include "Global/FanyDefines.h"
#include "Key/DeferredKeyFailurePolicy.h"
#include "Key/KeyCharacter.h"
#include "Key/PairedPunctuationPolicy.h"
#include "Key/PunctuationCommitText.h"
#include "Key/PunctuationKeyClaim.h"

#include "../../engine/contracts/custom_punctuation.h"
#include "../../engine/contracts/enumeration_comma.h"
#include "../../engine/contracts/temporary_japanese_input.h"
#include "../../engine/contracts/uiless_page.h"
#include "../../engine/contracts/windows_ipc.h"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

// ---- 抽出来的路径用到的 Win32 与项目外观 ----

using TfEditCookie = DWORD;

constexpr UINT WM_AsyncPunctuationCommit = WM_USER + 12;
constexpr UINT WM_AsyncServerCandidateKey = WM_USER + 14;

constexpr HRESULT FANY_S_COMMIT_REPLY_NOT_READY = MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_ITF, 0x0302);
constexpr HRESULT FANY_E_COMMIT_REPLY_AMBIGUOUS = __HRESULT_FROM_WIN32(ERROR_TIMEOUT);

enum class CommitReplyWaitStatus
{
    Ready,
    NotReady,
    Transport,
};

namespace Global
{
namespace DataFromServerMsgType = FanyImeReplyType;
// 候选窗开着时会先上屏高亮候选的标点键；测试只用到这几个。
const std::unordered_set<WCHAR> CommitWithHighlightedCandPunc = {L'-', L',', L'.', L'{', L'['};
std::atomic_bool PairedPunctuationEnabled{true};
thread_local std::wstring current_process_name = L"notepad.exe";
bool IsUiLessMode()
{
    return false;
}
} // namespace Global

class PerfTimer
{
};

struct ITfContext
{
    ULONG Release()
    {
        return 0;
    }
};

struct ITfDocumentMgr
{
    ITfContext *top = nullptr;
    HRESULT GetTop(ITfContext **context)
    {
        *context = top;
        return top != nullptr ? S_OK : E_FAIL;
    }
    ULONG Release()
    {
        return 0;
    }
};

struct ITfThreadMgr
{
    ITfDocumentMgr *focus = nullptr;
    HRESULT GetFocus(ITfDocumentMgr **documentMgr)
    {
        *documentMgr = focus;
        return focus != nullptr ? S_OK : E_FAIL;
    }
};

class CStringRange
{
  public:
    void Set(const WCHAR *text, size_t length)
    {
        text_.assign(text, length);
    }
    const std::wstring &Text() const
    {
        return text_;
    }

  private:
    std::wstring text_;
};

constexpr size_t PAIRED_PUNCTUATION_MAX_DEPTH = 16;

#include "custom_punctuation_wiring_types_source.h"

namespace
{
// ---- 管道与候选窗的替身 ----

FanyImeNamedpipeDataToTsf g_reply{};
CommitReplyWaitStatus g_replyStatus = CommitReplyWaitStatus::Ready;

CommitReplyWaitStatus TryReadCommitReplyFromServerPipe(uint64_t, DWORD, FanyImeNamedpipeDataToTsf **reply)
{
    *reply = g_replyStatus == CommitReplyWaitStatus::NotReady ? nullptr : &g_reply;
    return g_replyStatus;
}

FanyImeNamedpipeDataToTsf *TryReadDataFromServerPipeWithTimeout(uint64_t, bool)
{
    return &g_reply;
}

bool IsDeliveredServerRequestId(uint64_t requestId)
{
    return requestId != UINT64_MAX && requestId != 0;
}

void SetReply(std::uint32_t type, const wchar_t *text)
{
    g_reply = {};
    g_reply.msg_type = type;
    std::wcsncpy(g_reply.candidate_string, text, std::size(g_reply.candidate_string) - 1);
}

class CCandidateListUIPresenter
{
  public:
    bool _IsAsyncCleanupPending() const
    {
        return false;
    }
    template <typename Page> bool _ApplyUiLessPayload(const Page &)
    {
        return false;
    }
    void _NotifyUiLessHost()
    {
    }
};

class CCompositionProcessorEngine
{
  public:
    size_t GetVirtualKeyLength() const
    {
        return virtualKeyLength;
    }
    void BalanceNestPairAfterAutoClose(WCHAR openingCode, const CustomPunctuationPick &custom)
    {
        map.BalanceNestPairAfterAutoClose(openingCode, custom);
    }

    size_t virtualKeyLength = 0;
    PunctuationKeyMap map;
};

// ---- TSF 服务的替身：抽出来的路径调用的成员，记下它们收到了什么 ----

class CMetasequoiaIME
{
  public:
    using AsyncKeyRequest = ::AsyncKeyRequest;
    using PairedPunctuationEntry = ::PairedPunctuationEntry;

    // 抽出来的产品定义。
    PunctuationKeyInput _SyncPunctuationKeyInput(const KeyCharacter &key, UINT virtualKey, BOOL punctuationOpen,
                                                 bool freshCompositionState);
    PunctuationKeyInput _DeferredPunctuationKeyInput(const KeyCharacter &key, UINT virtualKey,
                                                     const DeferredShadowState &shadow);
    HRESULT _HandleCompositionPunctuation(TfEditCookie ec, ITfContext *pContext, UINT code, WCHAR wch,
                                          uint64_t requestId, const std::wstring &prefetchedText,
                                          const CustomPunctuationPick &custom);
    void _PushPairedPunctuation(WCHAR opening, WCHAR closing, WCHAR openedByKey);
    void _ClearPairedPunctuationStack();
    bool _TryStepOverPairedPunctuation(TfEditCookie ec, ITfContext *pContext, WCHAR closing);

    // 状态。
    CANDIDATE_MODE _candidateMode = CANDIDATE_NONE;
    CCandidateListUIPresenter *_pCandidateListUIPresenter = nullptr;
    CCompositionProcessorEngine *_pCompositionProcessorEngine = nullptr;
    bool _deferredKeyProjectionValid = true;
    BOOL composing = FALSE;
    std::vector<PairedPunctuationEntry> _pairedPunctuationStack;
    // 文档里光标右边那个字符（_GetFollowingDocumentChar）。
    WCHAR followingChar = 0;
    ITfThreadMgr threadMgr;

    BOOL _IsComposing()
    {
        return composing;
    }
    ITfThreadMgr *_GetThreadMgr()
    {
        return &threadMgr;
    }
    uint64_t _CaptureFocusSessionToken() const
    {
        return 7;
    }
    bool _IsFocusSessionCurrent(uint64_t, ITfContext * = nullptr) const
    {
        return true;
    }
    bool _IsCompositionEpochCurrent(uint64_t) const
    {
        return true;
    }
    DWORD _SessionCommitReplyWaitMs() const
    {
        return 0;
    }
    WCHAR _GetFollowingDocumentChar(TfEditCookie, ITfContext *)
    {
        return followingChar;
    }
    WCHAR _GetPrecedingDocumentChar(TfEditCookie, ITfContext *)
    {
        return 0;
    }
    WCHAR _GetPrecedingCharForSmartPunctuation(TfEditCookie, ITfContext *)
    {
        return 0;
    }
    // 出符号的那一步：用真的默认映射，自定义标点只看传进来的 pick。
    std::wstring _ResolveSmartPunctuation(WCHAR wch, WCHAR, const CustomPunctuationPick &custom)
    {
        resolvedPicks.push_back(custom);
        return _pCompositionProcessorEngine->map.Resolve(wch, FanyImeEnumerationComma::kBackslash, custom);
    }
    void _ClearSmartPunctuationAction()
    {
    }
    void _InvalidateSmartPunctuationShadow()
    {
    }
    void _QueuePairedPunctuationCaretMove(int delta)
    {
        caretMoves.push_back(delta);
    }
    void _NoteCommittedChinesePunctuation(const std::wstring &, bool, bool, WCHAR)
    {
    }
    HRESULT _InsertTextToComposition(TfEditCookie, ITfContext *, CStringRange *text)
    {
        committed.push_back(text->Text());
        return S_OK;
    }
    HRESULT _AddComposingAndChar(TfEditCookie, ITfContext *, CStringRange *text)
    {
        committed.push_back(text->Text());
        return S_OK;
    }
    HRESULT _AddCharAndFinalize(TfEditCookie, ITfContext *, CStringRange *text)
    {
        committed.push_back(text->Text());
        return S_OK;
    }
    HRESULT _HandleComplete(TfEditCookie, ITfContext *)
    {
        return S_OK;
    }
    HRESULT _HandleCompleteCommitFirst(TfEditCookie, ITfContext *)
    {
        return S_OK;
    }

    // 消息窗口那两个分支用到的。
    bool _TakeAsyncKeyRequest(UINT message, UINT, AsyncKeyRequest &request)
    {
        if (!staged || staged->message != message)
            return false;
        request = *staged;
        staged = nullptr;
        return true;
    }
    void _FailSupersededAsyncKeyRequest(const AsyncKeyRequest &)
    {
        failures.push_back(L"superseded");
    }
    bool _FetchCommitReplyForEditSession(UINT, UINT, AsyncKeyRequest &)
    {
        return true;
    }
    bool _PollCommitReply(UINT, UINT, AsyncKeyRequest &, FanyImeNamedpipeDataToTsf **reply)
    {
        *reply = &g_reply;
        return true;
    }
    void _ResetSessionAfterFailure(DeferredKeyFailureKind)
    {
        failures.push_back(L"reset");
    }
    void _CompleteDeferredKeyReplay(uint64_t)
    {
        ++completedReplays;
    }
    void _FailDeferredKey(uint64_t, DeferredKeyFailureReason)
    {
        failures.push_back(L"failed");
    }
    HRESULT _RequestDirectPunctuationEditSession(ITfContext *, UINT code, WCHAR wch, uint64_t,
                                                 std::wstring prefetchedText,
                                                 const CustomPunctuationPick &customPunctuation, uint64_t, uint64_t,
                                                 uint64_t, UINT, ULONGLONG)
    {
        sessions.push_back({L"direct", code, wch, std::move(prefetchedText), FUNCTION_NONE, customPunctuation});
        return S_OK;
    }
    HRESULT _InvokeKeyHandler(ITfContext *, UINT code, WCHAR wch, DWORD, _KEYSTROKE_STATE keyState, uint64_t,
                              std::wstring prefetchedText, UINT, uint64_t, uint64_t, uint64_t, UINT = 0, ULONGLONG = 0)
    {
        sessions.push_back(
            {L"key handler", code, wch, std::move(prefetchedText), keyState.Function, keyState.CustomPunctuation});
        return S_OK;
    }
    bool _PostAsyncKeyRequest(UINT message, UINT code, WCHAR wch, uint64_t, std::wstring prefetchedText, uint64_t,
                              uint64_t, uint64_t, KEYSTROKE_FUNCTION keystrokeFunction,
                              const CustomPunctuationPick &customPunctuation)
    {
        posts.push_back({message, code, wch, std::move(prefetchedText), keystrokeFunction, customPunctuation});
        return true;
    }

    // 记下的结果。
    struct Session
    {
        std::wstring kind;
        UINT code;
        WCHAR wch;
        std::wstring prefetchedText;
        KEYSTROKE_FUNCTION function;
        CustomPunctuationPick custom;
    };
    struct Post
    {
        UINT message;
        UINT code;
        WCHAR wch;
        std::wstring prefetchedText;
        KEYSTROKE_FUNCTION function;
        CustomPunctuationPick custom;
    };
    const AsyncKeyRequest *staged = nullptr;
    std::vector<std::wstring> committed;
    std::vector<int> caretMoves;
    std::vector<CustomPunctuationPick> resolvedPicks;
    std::vector<Session> sessions;
    std::vector<Post> posts;
    std::vector<std::wstring> failures;
    int completedReplays = 0;
};

#include "custom_punctuation_wiring_source.h"

int g_failures = 0;

void Check(bool condition, const char *what, int line)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(condition) Check((condition), #condition, __LINE__)

namespace cp = FanyImeCustomPunctuation;

// 美式键盘主键盘上的虚拟键码（VK_OEM_*）。
constexpr UINT kVkOemMinus = 0xBD;
constexpr UINT kVkOemComma = 0xBC;
constexpr UINT kVkOemOpenBracket = 0xDB;
constexpr UINT kVkOemCloseBracket = 0xDD;

cp::Table MakeTable(std::initializer_list<std::pair<wchar_t, const wchar_t *>> rows)
{
    cp::Table table;
    for (const auto &row : rows)
        CHECK(table.Add(row.first, row.second));
    return table;
}

// 中文标点状态、没有输入时，主键盘上这次按键吃键判定给出的 pick。
CustomPunctuationPick PickFor(const cp::Table &table, wchar_t ch, UINT vk, bool shift)
{
    PunctuationKeyInput input;
    input.character = ch;
    input.virtualKey = vk;
    input.shiftDown = shift;
    input.punctuationOpen = true;
    input.enumerationCommaKeys = FanyImeEnumerationComma::kBackslash;
    return DecidePunctuationKey(input, table).custom;
}

struct Service
{
    CCompositionProcessorEngine engine;
    CCandidateListUIPresenter presenter;
    ITfContext context;
    ITfDocumentMgr documentMgr;
    CMetasequoiaIME ime;

    Service()
    {
        ime._pCompositionProcessorEngine = &engine;
        documentMgr.top = &context;
        ime.threadMgr.focus = &documentMgr;
    }
};

// ---- 两条分类路径填的输入 ----

void SyncInputTakesShiftFromTheKeyItWasGiven()
{
    Service service;
    GlobalIme::word_for_creating_word.clear();
    // Shift 取自与字符同一份键盘状态得出的那一位，不现读物理键（测试进程里 Shift 没按着）。
    PunctuationKeyInput input =
        service.ime._SyncPunctuationKeyInput(KeyCharacter{L'{', true}, kVkOemOpenBracket, TRUE, false);
    CHECK(input.character == L'{');
    CHECK(input.virtualKey == kVkOemOpenBracket);
    CHECK(input.shiftDown);
    CHECK(input.punctuationOpen);
    CHECK(!input.compositionActive);
    CHECK(!input.candidateListShown);
    CHECK(!input.createWordPending);
    CHECK(!input.candidateListRejectsPunctuation);
    CHECK(!input.defaultKeysIgnoreCandidateList);
    input = service.ime._SyncPunctuationKeyInput(KeyCharacter{L'[', false}, kVkOemOpenBracket, FALSE, false);
    CHECK(!input.shiftDown);
    CHECK(!input.punctuationOpen);
}

void SyncInputSeesCompositionCandidatesAndTheRetractionWindow()
{
    Service service;
    const KeyCharacter equal{L'=', false};
    // 造词的撤回窗口：组合已经空了、正在造的词还在。
    GlobalIme::word_for_creating_word = L"水杉";
    CHECK(service.ime._SyncPunctuationKeyInput(equal, 0xBB, TRUE, false).createWordPending);
    // 按全新组合的状态分类时（组合已清空）不算在窗口里，也不算有输入、有候选窗。
    service.ime.composing = TRUE;
    service.ime._candidateMode = CANDIDATE_ORIGINAL;
    PunctuationKeyInput fresh = service.ime._SyncPunctuationKeyInput(equal, 0xBB, TRUE, true);
    CHECK(!fresh.createWordPending);
    CHECK(!fresh.compositionActive);
    CHECK(!fresh.candidateListShown);
    CHECK(!fresh.candidateListRejectsPunctuation);
    GlobalIme::word_for_creating_word.clear();
    CHECK(!service.ime._SyncPunctuationKeyInput(equal, 0xBB, TRUE, false).createWordPending);

    // 有组合，或键入缓冲里有字。
    service.ime._candidateMode = CANDIDATE_NONE;
    CHECK(service.ime._SyncPunctuationKeyInput(equal, 0xBB, TRUE, false).compositionActive);
    service.ime.composing = FALSE;
    service.engine.virtualKeyLength = 2;
    CHECK(service.ime._SyncPunctuationKeyInput(equal, 0xBB, TRUE, false).compositionActive);
    service.engine.virtualKeyLength = 0;
    CHECK(!service.ime._SyncPunctuationKeyInput(equal, 0xBB, TRUE, false).compositionActive);

    // 候选窗：开着都算 shown；只有不是边打边出的那种挡住标点。
    service.ime._candidateMode = CANDIDATE_ORIGINAL;
    PunctuationKeyInput original = service.ime._SyncPunctuationKeyInput(equal, 0xBB, TRUE, false);
    CHECK(original.candidateListShown);
    CHECK(original.candidateListRejectsPunctuation);
    service.ime._candidateMode = CANDIDATE_INCREMENTAL;
    PunctuationKeyInput incremental = service.ime._SyncPunctuationKeyInput(equal, 0xBB, TRUE, false);
    CHECK(incremental.candidateListShown);
    CHECK(!incremental.candidateListRejectsPunctuation);
}

void DeferredInputFollowsTheProjection()
{
    Service service;
    GlobalIme::word_for_creating_word.clear();
    DeferredShadowState shadow;
    shadow.punctuationOpen = true;
    // 入队时得出的字符与 Shift 原样进判定。
    PunctuationKeyInput input =
        service.ime._DeferredPunctuationKeyInput(KeyCharacter{L'{', true}, kVkOemOpenBracket, shadow);
    CHECK(input.character == L'{');
    CHECK(input.virtualKey == kVkOemOpenBracket);
    CHECK(input.shiftDown);
    CHECK(input.punctuationOpen);
    CHECK(!input.compositionActive);
    CHECK(!input.candidateListShown);
    CHECK(!input.createWordPending);
    // 延迟分类吃默认标点键时不看候选窗。
    CHECK(input.defaultKeysIgnoreCandidateList);
    CHECK(!service.ime._DeferredPunctuationKeyInput(KeyCharacter{L'[', false}, kVkOemOpenBracket, shadow).shiftDown);

    // 投影里有输入、有候选窗。
    shadow.inputLength = 2;
    shadow.candidateActive = true;
    input = service.ime._DeferredPunctuationKeyInput(KeyCharacter{L',', false}, kVkOemComma, shadow);
    CHECK(input.compositionActive);
    CHECK(input.candidateListShown);
    CHECK(input.candidateListRejectsPunctuation);
    // 候选窗开着时默认键照吃，只有自定义标点认的键不吃。
    CHECK(DecidePunctuationKey(input, cp::Table()).claimed);
    const cp::Table equalTable = MakeTable({{L'=', L"＝"}});
    input = service.ime._DeferredPunctuationKeyInput(KeyCharacter{L'=', false}, 0xBB, shadow);
    CHECK(!DecidePunctuationKey(input, equalTable).claimed);

    // 投影作废时看真实状态。
    shadow = DeferredShadowState{};
    shadow.punctuationOpen = true;
    service.ime._deferredKeyProjectionValid = false;
    service.ime.composing = TRUE;
    service.ime._candidateMode = CANDIDATE_ORIGINAL;
    input = service.ime._DeferredPunctuationKeyInput(KeyCharacter{L'=', false}, 0xBB, shadow);
    CHECK(input.compositionActive);
    CHECK(input.candidateListShown);
    // 投影有效时不看真实状态。
    service.ime._deferredKeyProjectionValid = true;
    input = service.ime._DeferredPunctuationKeyInput(KeyCharacter{L'=', false}, 0xBB, shadow);
    CHECK(!input.compositionActive);
    CHECK(!input.candidateListShown);

    // 造词的撤回窗口。
    GlobalIme::word_for_creating_word = L"水杉";
    CHECK(service.ime._DeferredPunctuationKeyInput(KeyCharacter{L'=', false}, 0xBB, shadow).createWordPending);
    GlobalIme::word_for_creating_word.clear();
}

// ---- 标点上屏与成对栈 ----

void TheSameCustomKeyStepsOverTheClosingItAutoCompleted()
{
    Service service;
    Global::PairedPunctuationEnabled = true;
    const cp::Table table = MakeTable({{L'{', L"「"}, {L'[', L"「"}});
    const CustomPunctuationPick shiftBracket = PickFor(table, L'{', kVkOemOpenBracket, true);
    const CustomPunctuationPick bracket = PickFor(table, L'[', kVkOemOpenBracket, false);
    CHECK(shiftBracket.Symbol() == L"「");

    // Shift + [ 出 「 并补出 」，光标放回中间；成对栈记下是 { 这个键补出来的。
    CHECK(service.ime._HandleCompositionPunctuation(0, &service.context, kVkOemOpenBracket, L'{', 1, L"",
                                                    shiftBracket) == S_OK);
    CHECK(service.ime.committed.size() == 1 && service.ime.committed.back() == L"「」");
    CHECK(service.ime.caretMoves.size() == 1 && service.ime.caretMoves.back() == -1);
    CHECK(service.ime._pairedPunctuationStack.size() == 1);
    CHECK(service.ime._pairedPunctuationStack.back().closing == L'」');
    CHECK(service.ime._pairedPunctuationStack.back().openedByKey == L'{');

    // 别的键也定成 「：不跨，再补一对。
    service.ime.followingChar = L'」';
    service.ime._HandleCompositionPunctuation(0, &service.context, kVkOemOpenBracket, L'[', 2, L"", bracket);
    CHECK(service.ime.committed.size() == 2 && service.ime.committed.back() == L"「」");
    CHECK(service.ime._pairedPunctuationStack.size() == 2);
    CHECK(service.ime._pairedPunctuationStack.back().openedByKey == L'[');
    service.ime._ClearPairedPunctuationStack();
    service.ime.committed.clear();
    service.ime.caretMoves.clear();

    // 同一个键再按一次：跨过它补出的 」，不再上屏。
    service.ime._HandleCompositionPunctuation(0, &service.context, kVkOemOpenBracket, L'{', 3, L"", shiftBracket);
    CHECK(service.ime.committed.size() == 1);
    service.ime._HandleCompositionPunctuation(0, &service.context, kVkOemOpenBracket, L'{', 4, L"", shiftBracket);
    CHECK(service.ime.committed.size() == 1);
    CHECK(service.ime.caretMoves.size() == 2 && service.ime.caretMoves.back() == 1);
    CHECK(service.ime._pairedPunctuationStack.empty());
}

void CustomKeysPairWithThePairingSwitchOff()
{
    Service service;
    Global::PairedPunctuationEnabled = false;
    const cp::Table table = MakeTable({{L'{', L"「"}});
    const CustomPunctuationPick shiftBracket = PickFor(table, L'{', kVkOemOpenBracket, true);

    // 成对补全关着：自定义键出的 「 照样补出 」、光标放回中间，同一个键再按跨过它。
    service.ime._HandleCompositionPunctuation(0, &service.context, kVkOemOpenBracket, L'{', 1, L"", shiftBracket);
    CHECK(service.ime.committed.size() == 1 && service.ime.committed.back() == L"「」");
    CHECK(service.ime.caretMoves.size() == 1 && service.ime.caretMoves.back() == -1);
    CHECK(service.ime._pairedPunctuationStack.size() == 1);
    service.ime.followingChar = L'」';
    service.ime._HandleCompositionPunctuation(0, &service.context, kVkOemOpenBracket, L'{', 2, L"", shiftBracket);
    CHECK(service.ime.committed.size() == 1);
    CHECK(service.ime.caretMoves.size() == 2 && service.ime.caretMoves.back() == 1);
    CHECK(service.ime._pairedPunctuationStack.empty());

    // 没被接管的 [ 跟开关走：只出 【。
    service.ime._HandleCompositionPunctuation(0, &service.context, kVkOemOpenBracket, L'[', 3, L"",
                                              CustomPunctuationPick{});
    CHECK(service.ime.committed.size() == 2 && service.ime.committed.back() == L"【");
    CHECK(service.ime._pairedPunctuationStack.empty());

    // 被排除的宿主里自定义键也不补。
    Global::current_process_name = L"EXCEL.EXE";
    service.ime._HandleCompositionPunctuation(0, &service.context, kVkOemOpenBracket, L'{', 4, L"", shiftBracket);
    CHECK(service.ime.committed.size() == 3 && service.ime.committed.back() == L"「");
    CHECK(service.ime._pairedPunctuationStack.empty());
    Global::current_process_name = L"notepad.exe";
    Global::PairedPunctuationEnabled = true;
}

void PunctuationCommitResolvesWithThePickItWasGiven()
{
    Service service;
    Global::PairedPunctuationEnabled = false;
    const cp::Table table = MakeTable({{L'-', L"——"}, {L'[', L"※"}});

    // 没有候选时：出 pick 里的符号。
    service.ime._HandleCompositionPunctuation(0, &service.context, kVkOemOpenBracket, L'[', 1, L"",
                                              PickFor(table, L'[', kVkOemOpenBracket, false));
    CHECK(service.ime.committed.back() == L"※");
    // 同一个键不带 pick：默认映射。
    service.ime._HandleCompositionPunctuation(0, &service.context, kVkOemOpenBracket, L'[', 2, L"",
                                              CustomPunctuationPick{});
    CHECK(service.ime.committed.back() == L"【");

    // 候选窗开着时按被接管的主键盘 -：先上屏候选，再接映射后的符号，不按 ASCII 接 -。
    service.ime._candidateMode = CANDIDATE_ORIGINAL;
    service.ime._pCandidateListUIPresenter = &service.presenter;
    service.ime.composing = TRUE;
    SetReply(FanyImeReplyType::Normal, L"你");
    service.ime._HandleCompositionPunctuation(0, &service.context, kVkOemMinus, L'-', 3, L"",
                                              PickFor(table, L'-', kVkOemMinus, false));
    CHECK(service.ime.committed.back() == L"你——");
    // 没被接管的 - 照旧按 ASCII 接在候选后面。
    service.ime._HandleCompositionPunctuation(0, &service.context, kVkOemMinus, L'-', 4, L"", CustomPunctuationPick{});
    CHECK(service.ime.committed.back() == L"你-");
    Global::PairedPunctuationEnabled = true;
}

// ---- 消息窗口：标点上屏与 Server 候选键的回复 ----

AsyncKeyRequest Request(UINT message, UINT code, WCHAR wch, const CustomPunctuationPick &pick,
                        std::wstring prefetchedText = {})
{
    AsyncKeyRequest request;
    request.message = message;
    request.code = code;
    request.wch = wch;
    request.requestId = 41;
    request.deferredReplayToken = 5;
    request.prefetchedText = std::move(prefetchedText);
    request.customPunctuation = pick;
    return request;
}

void PunctuationCommitMessageHandsThePickToTheSession()
{
    const cp::Table table = MakeTable({{L'{', L"「"}});
    const CustomPunctuationPick pick = PickFor(table, L'{', kVkOemOpenBracket, true);

    // 没有组合、没有候选窗：直接上屏标点的编辑会话。
    Service direct;
    const AsyncKeyRequest directRequest = Request(WM_AsyncPunctuationCommit, kVkOemOpenBracket, L'{', pick, L"「");
    direct.ime.staged = &directRequest;
    DispatchAsyncPunctuationMessage(&direct.ime, WM_AsyncPunctuationCommit, 1);
    CHECK(direct.ime.sessions.size() == 1);
    CHECK(direct.ime.sessions.back().kind == L"direct");
    CHECK(direct.ime.sessions.back().custom.Symbol() == L"「");
    CHECK(direct.ime.failures.empty());

    // 有组合：按键处理的编辑会话，pick 放在按键状态里。
    Service composing;
    composing.ime.composing = TRUE;
    const AsyncKeyRequest composingRequest = Request(WM_AsyncPunctuationCommit, kVkOemOpenBracket, L'{', pick, L"你「");
    composing.ime.staged = &composingRequest;
    DispatchAsyncPunctuationMessage(&composing.ime, WM_AsyncPunctuationCommit, 1);
    CHECK(composing.ime.sessions.size() == 1);
    CHECK(composing.ime.sessions.back().kind == L"key handler");
    CHECK(composing.ime.sessions.back().function == FUNCTION_PUNCTUATION);
    CHECK(composing.ime.sessions.back().custom.Symbol() == L"「");
}

void ServerCandidateReplyRepostsThePunctuationWithItsPick()
{
    const cp::Table table = MakeTable({{L'-', L"——"}, {L']', L"」"}});
    const CustomPunctuationPick minus = PickFor(table, L'-', kVkOemMinus, false);

    // 回复是候选：候选接上映射后的符号，带着 pick 转去标点上屏。
    Service normal;
    normal.ime._candidateMode = CANDIDATE_ORIGINAL;
    SetReply(FanyImeReplyType::Normal, L"你");
    const AsyncKeyRequest normalRequest = Request(WM_AsyncServerCandidateKey, kVkOemMinus, L'-', minus);
    normal.ime.staged = &normalRequest;
    DispatchAsyncPunctuationMessage(&normal.ime, WM_AsyncServerCandidateKey, 1);
    CHECK(normal.ime.resolvedPicks.size() == 1 && normal.ime.resolvedPicks.back().Symbol() == L"——");
    CHECK(normal.ime.posts.size() == 1);
    CHECK(normal.ime.posts.back().message == WM_AsyncPunctuationCommit);
    CHECK(normal.ime.posts.back().prefetchedText == L"你——");
    CHECK(normal.ime.posts.back().custom.Symbol() == L"——");

    // 回复是精确上屏文本（以词定字）：原样转去标点上屏，pick 照样带着。
    Service exact;
    exact.ime._candidateMode = CANDIDATE_ORIGINAL;
    SetReply(FanyImeReplyType::CommitExactText, L"水");
    const CustomPunctuationPick bracket = PickFor(table, L']', kVkOemCloseBracket, false);
    const AsyncKeyRequest exactRequest = Request(WM_AsyncServerCandidateKey, kVkOemCloseBracket, L']', bracket);
    exact.ime.staged = &exactRequest;
    DispatchAsyncPunctuationMessage(&exact.ime, WM_AsyncServerCandidateKey, 1);
    CHECK(exact.ime.posts.size() == 1);
    CHECK(exact.ime.posts.back().prefetchedText == L"水");
    CHECK(exact.ime.posts.back().custom.Symbol() == L"」");
}
} // namespace

int main()
{
    SyncInputTakesShiftFromTheKeyItWasGiven();
    SyncInputSeesCompositionCandidatesAndTheRetractionWindow();
    DeferredInputFollowsTheProjection();
    TheSameCustomKeyStepsOverTheClosingItAutoCompleted();
    CustomKeysPairWithThePairingSwitchOff();
    PunctuationCommitResolvesWithThePickItWasGiven();
    PunctuationCommitMessageHandsThePickToTheSession();
    ServerCandidateReplyRepostsThePunctuationWithItsPick();
    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("PASS: custom punctuation wiring");
    return 0;
}
