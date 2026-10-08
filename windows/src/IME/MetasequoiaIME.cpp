#include "Private.h"
#include "Globals.h"
#include "MetasequoiaIME.h"
#include "CandidateListUIPresenter.h"
#include "CompositionProcessorEngine.h"
#include "Compartment.h"
#include "define.h"
#include <debugapi.h>
#include <namedpipeapi.h>
#include <winnt.h>
#include <winuser.h>
#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
#include "FanyLog.h"
#include "Ipc.h"
#include "CommonUtils.h"
#include "Global/FanyDefines.h"
#include "Utils/FanyUtils.h"
#include "../Utils/PerfTimer.h"
#include "MetasequoiaIMEInternal.h"

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace
{
constexpr UINT IPC_FAILURES_BEFORE_SERVER_LAUNCH = 6;
constexpr UINT SERVER_LAUNCH_RECONNECT_DELAY_MS = 500;
constexpr wchar_t SERVER_MUTEX_NAME[] = L"Local\\MetasequoiaImeServer_SingleInstance";
constexpr wchar_t SERVER_LAUNCH_MUTEX_NAME[] = L"Local\\MetasequoiaImeServer.Launch";
constexpr wchar_t INSTALL_REGISTRY_KEY[] = L"Software\\Metasequoia\\MetasequoiaIME";
constexpr wchar_t SERVER_PATH_REGISTRY_VALUE[] = L"ServerPath";
std::atomic<UINT> nextWindowMessageToken{0};
std::atomic<bool> serverLaunchInFlight{false};

bool IsServerAlreadyRunning()
{
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, SERVER_MUTEX_NAME);
    if (mutex)
    {
        CloseHandle(mutex);
        return true;
    }

    // uiAccess can put the Server at a different integrity level. Access
    // denied still proves that the named mutex exists.
    return GetLastError() == ERROR_ACCESS_DENIED;
}

std::wstring ReadServerPath()
{
    wchar_t path[32768]{};
    DWORD bytes = sizeof(path);
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, INSTALL_REGISTRY_KEY, SERVER_PATH_REGISTRY_VALUE,
                                        RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, path, &bytes);
    if (status != ERROR_SUCCESS || path[0] == L'\0')
    {
        return {};
    }
    return path;
}

bool LaunchServerIfNeeded()
{
    if (IsServerAlreadyRunning())
    {
        return true;
    }

    HANDLE launchMutex = CreateMutexW(nullptr, TRUE, SERVER_LAUNCH_MUTEX_NAME);
    if (!launchMutex)
    {
        // Another integrity level may own the launch gate.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(launchMutex);
        return true;
    }

    const std::wstring serverPath = ReadServerPath();
    if (serverPath.empty() || GetFileAttributesW(serverPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        if (Global::TsfDiagnosticLogEnabled.load(std::memory_order_relaxed))
        {
            QueueTsfDiagnosticLog(L"[msime]: Server path is unavailable; cannot revive Server.");
        }
        ReleaseMutex(launchMutex);
        CloseHandle(launchMutex);
        return false;
    }

    const size_t separator = serverPath.find_last_of(L"\\/");
    const std::wstring workingDirectory =
        separator == std::wstring::npos ? std::wstring{} : serverPath.substr(0, separator);
    const HINSTANCE result =
        ShellExecuteW(nullptr, L"open", serverPath.c_str(), nullptr,
                      workingDirectory.empty() ? nullptr : workingDirectory.c_str(), SW_SHOWNOACTIVATE);
    const bool launched = reinterpret_cast<INT_PTR>(result) > 32;
    ReleaseMutex(launchMutex);
    CloseHandle(launchMutex);
    return launched;
}

DWORD WINAPI ServerLaunchThreadProc(LPVOID parameter)
{
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    LaunchServerIfNeeded();
    if (SUCCEEDED(comInit))
    {
        CoUninitialize();
    }
    serverLaunchInFlight.store(false, std::memory_order_release);
    // The reference taken in RequestServerLaunch keeps the TIP mapped even if
    // the host deactivates it while the shell is still resolving the Server.
    // FreeLibraryAndExitThread is __declspec(noreturn); a trailing `return 0;`
    // here is dead code (C4702), not a missing-return guard.
    FreeLibraryAndExitThread(static_cast<HMODULE>(parameter), 0);
}

// Starting the uiAccess Server goes through the shell, which can block for
// seconds. Every caller here is on the host's TSF/UI thread, where a stall
// freezes the application's message pump and makes cross-apartment calls into
// this TIP (ITfLangBarItemButton::GetIcon among them) fail. Hand the launch to
// a throwaway thread and let the reconnect timer discover the Server.
//
// Returns true when the Server is running or a launch is under way.
bool RequestServerLaunch()
{
    if (IsServerAlreadyRunning())
    {
        return true;
    }
    if (serverLaunchInFlight.exchange(true, std::memory_order_acq_rel))
    {
        return true;
    }

    HMODULE selfModule = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&ServerLaunchThreadProc),
                            &selfModule))
    {
        serverLaunchInFlight.store(false, std::memory_order_release);
        return false;
    }

    const HANDLE launchThread = CreateThread(nullptr, 0, ServerLaunchThreadProc, selfModule, 0, nullptr);
    if (!launchThread)
    {
        FreeLibrary(selfModule);
        serverLaunchInFlight.store(false, std::memory_order_release);
        return false;
    }
    CloseHandle(launchThread);
    return true;
}

} // namespace

namespace metasequoia_ime_detail
{
UINT NextWindowMessageToken()
{
    UINT token = 0;
    do
    {
        token = nextWindowMessageToken.fetch_add(1, std::memory_order_relaxed) + 1;
    } while (token == 0);
    return token;
}
} // namespace metasequoia_ime_detail

using namespace metasequoia_ime_detail;

//+---------------------------------------------------------------------------
//
// CreateInstance
//
//----------------------------------------------------------------------------

/* static */
HRESULT CMetasequoiaIME::CreateInstance(_In_ IUnknown *pUnkOuter, REFIID riid, _Outptr_ void **ppvObj)
{
    CMetasequoiaIME *pMetasequoiaIME = nullptr;
    HRESULT hr = S_OK;

    if (ppvObj == nullptr)
    {
        return E_INVALIDARG;
    }

    *ppvObj = nullptr;

    if (nullptr != pUnkOuter)
    {
        return CLASS_E_NOAGGREGATION;
    }

    pMetasequoiaIME = new (std::nothrow) CMetasequoiaIME();
    if (pMetasequoiaIME == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    hr = pMetasequoiaIME->QueryInterface(riid, ppvObj);

    pMetasequoiaIME->Release();

    return hr;
}

//+---------------------------------------------------------------------------
//
// ctor
//
//----------------------------------------------------------------------------

CMetasequoiaIME::CMetasequoiaIME()
{
    DllAddRef();

    _pThreadMgr = nullptr;

    _threadMgrEventSinkCookie = TF_INVALID_COOKIE;

    _pTextEditSinkContext = nullptr;
    _textEditSinkCookie = TF_INVALID_COOKIE;

    _activeLanguageProfileNotifySinkCookie = TF_INVALID_COOKIE;

    _dwThreadFocusSinkCookie = TF_INVALID_COOKIE;

    _pComposition = nullptr;

    _pCompositionProcessorEngine = nullptr;

    _candidateMode = CANDIDATE_NONE;
    _pCandidateListUIPresenter = nullptr;
    _isCandidateWithWildcard = FALSE;

    _pDocMgrLastFocused = nullptr;

    _pSIPIMEOnOffCompartment = nullptr;
    _dwSIPIMEOnOffCompartmentSinkCookie = 0;
    _msgWndHandle = nullptr;
    _themeRegKey = nullptr;
    _themeRegEvent = nullptr;
    _pThemeWatcherThread = nullptr;
    _stopThemeWatcher = false;

    _pContext = nullptr;

    _refCount = 1;
    _pITfFnSearchCandidateProvider = nullptr;

    _msgWndHandle = nullptr;
    _pIpcThread = nullptr;
    _hToTsfWorkerThreadPipe.store(nullptr);
    _workerPipeGeneration.store(0);
    _ipcStopEvent = nullptr;
    _workerAckEvent = nullptr;
    _workerPipePublishedEvent = nullptr;
    _shouldStopIpcThread = false;
    _ipcReconnectDelayMs = CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS;
    _ipcConsecutiveFailures = 0;
    _workerCommitReady.store(false);
    _expectedWorkerFocusToken.store(0);
    _acknowledgedWorkerFocusToken.store(0);
    _compositionEpoch.store(1);
    _voiceCompositionAssemble.clear();
    _voiceCompositionAssembleMsg = 0;
    _voiceCompositionAssembleGeneration = 0;
    _voiceCompositionAssembleActive = false;
    _voiceCompositionActive = false;
    _localSessionResetPending.store(false);
    _localSessionResetToken.store(0);
    _localResetEditSessionQueued = false;
    _queuedLocalResetToken = 0;
    _localResyncResetToken = 0;
    _focusResetPending = false;
    _activationRequired = false;
    _focusLostToWindowsTextInputHost = false;
    _focusLossDeferPending = false;
    _focusAnnouncementWindow = nullptr;
    _focusAnnouncementEditable = false;
    _threadFocusLostForAnnouncement = false;
    _hasPendingServerCandidate = false;
    _pendingServerCandidateMsgType = Global::DataFromServerMsgType::OutofRange;
    _hasDeferredKeyInFlight = false;
    _deferredKeyReplayToken = 0;
    _nextDeferredKeyReplayToken = 0;
    _deferredKeyProjectionValid = false;
    _deferredProjectedImeOpen = false;
    _deferredProjectedJapaneseMode = false;
    _deferredProjectedPunctuationOpen = false;
    _deferredProjectedDoubleSingleByteOpen = false;
    _deferredProjectedInputLength = 0;
    _deferredProjectedRawInput.clear();
    _deferredProjectedCaret = 0;
    _deferredProjectedCandidateActive = false;
    _deferredProjectedUnicodeMode = false;
    _deferredKeyFocusGeneration = 1;
    _deferredKeyDrainPosted = false;
    _serverUnavailableFallbackActive = false;
    _backspaceHoldArmed = false;
    _passthroughStatsVirtualKey = 0;
    _passthroughStatsMessageTime = 0;
    _capsLockTestKeyDownMessageTime = 0;
    _capsLockTestKeyDownPending = false;
    _shiftHotkeyArmed = false;
    _ctrlHotkeyArmed = false;
    _modifierHotkeyExpire = {};
    _bareShiftHook = nullptr;
    _bareShiftDownMask = 0;
    _bareShiftArmed = false;
    _bareShiftSequence = 0;
    _bareShiftHandledSequence = 0;
    _bareShiftFocusGeneration = 0;
    _bareShiftExpireTick = 0;
}

thread_local CMetasequoiaIME *CMetasequoiaIME::_bareShiftHookOwner = nullptr;

//+---------------------------------------------------------------------------
//
// dtor
//
//----------------------------------------------------------------------------

CMetasequoiaIME::~CMetasequoiaIME()
{
    _UninitBareShiftKeyboardHook();
    _ClearDeferredKeyDowns();
    // Deactivate normally owns the unbind.  Keep this owner-aware fallback for
    // partial activation failures without allowing a delayed old service
    // destructor to clear a newer service's TLS bindings.
    UnbindNamedpipeFocusState(this);
    if (_pCandidateListUIPresenter)
    {
        delete _pCandidateListUIPresenter;
        _pCandidateListUIPresenter = nullptr;
    }
    _DrainPendingCandidatePresenterCleanup();
    DllRelease();

    /* 处理线程的清理 */
    if (_pIpcThread)
    {
        _shouldStopIpcThread.store(true);
        if (_ipcStopEvent)
        {
            SetEvent(_ipcStopEvent);
        }
        HANDLE workerPipe = _hToTsfWorkerThreadPipe.load();
        if (workerPipe && workerPipe != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(workerPipe, nullptr);
        }
        if (_pIpcThread->joinable())
        {
            _pIpcThread->join();
        }
        delete _pIpcThread;
        _pIpcThread = nullptr;
    }
    if (_ipcStopEvent)
    {
        CloseHandle(_ipcStopEvent);
        _ipcStopEvent = nullptr;
    }
    // The owner-aware unbind above already cleared the TLS copies.
    _CloseIpcWakeEvents();
}

uint64_t CMetasequoiaIME::_CaptureFocusSessionToken() const
{
    return _expectedWorkerFocusToken.load(std::memory_order_acquire);
}

bool CMetasequoiaIME::_IsFocusSessionCurrent(uint64_t focusToken, _In_opt_ ITfContext *expectedContext) const
{
    if (focusToken == 0 || !Global::g_connected ||
        _expectedWorkerFocusToken.load(std::memory_order_acquire) != focusToken ||
        _acknowledgedWorkerFocusToken.load(std::memory_order_acquire) != focusToken ||
        !_workerCommitReady.load(std::memory_order_acquire) ||
        _localSessionResetPending.load(std::memory_order_acquire))
    {
        return false;
    }
    if (expectedContext == nullptr)
    {
        return true;
    }
    if (_pThreadMgr == nullptr)
    {
        return false;
    }

    ITfDocumentMgr *documentMgr = nullptr;
    ITfContext *topContext = nullptr;
    bool matches = false;
    if (SUCCEEDED(_pThreadMgr->GetFocus(&documentMgr)) && documentMgr)
    {
        if (SUCCEEDED(documentMgr->GetTop(&topContext)) && topContext)
        {
            matches = topContext == expectedContext;
            topContext->Release();
        }
        documentMgr->Release();
    }
    return matches;
}

uint64_t CMetasequoiaIME::_CaptureCompositionEpoch() const
{
    return _compositionEpoch.load(std::memory_order_acquire);
}

bool CMetasequoiaIME::_IsCompositionEpochCurrent(uint64_t compositionEpoch) const
{
    return compositionEpoch != 0 && _compositionEpoch.load(std::memory_order_acquire) == compositionEpoch;
}

bool CMetasequoiaIME::_IsCompositionCurrent(_In_opt_ ITfComposition *expectedComposition) const
{
    return expectedComposition != nullptr && _pComposition == expectedComposition;
}

bool CMetasequoiaIME::_IsLocalSessionResetCurrent(UINT resetToken) const
{
    return resetToken != 0 && _localResetEditSessionQueued && _queuedLocalResetToken == resetToken &&
           _localSessionResetPending.load(std::memory_order_acquire) &&
           _localSessionResetToken.load(std::memory_order_acquire) == resetToken;
}

void CMetasequoiaIME::_CompleteLocalSessionReset(UINT resetToken)
{
    if (!_localResetEditSessionQueued || _queuedLocalResetToken != resetToken)
    {
        return;
    }
    _localResetEditSessionQueued = false;
    _queuedLocalResetToken = 0;

    const UINT currentToken = _localSessionResetToken.load(std::memory_order_acquire);
    if (_localSessionResetPending.load(std::memory_order_acquire) && currentToken != resetToken)
    {
        // A focus loss superseded an already-granted dirty reset.  The old
        // cancel has finished; process the exact newer token before reopening
        // the key path.
        _RequestLocalSessionReset(nullptr, currentToken);
        return;
    }
    if (currentToken == resetToken && !_IsComposing() && _pCandidateListUIPresenter == nullptr)
    {
        _localSessionResetPending.store(false, std::memory_order_release);
        _OnLocalSessionResetReleased(resetToken);
    }
    else if (currentToken == resetToken && _localSessionResetPending.load(std::memory_order_acquire))
    {
        // A granted edit session can still fail inside the key-state handler.
        // Do not reopen the transport while the old local composition or
        // presenter survives; retire this queue slot and retry the exact token.
        if (Global::g_connected && _msgWndHandle && IsWindow(_msgWndHandle))
        {
            _ipcReconnectDelayMs = CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS;
            SetTimer(_msgWndHandle, TIMER_CONNECT_ALL_NAMEDPIPE, _ipcReconnectDelayMs, nullptr);
        }
        return;
    }
    if (Global::g_connected && _msgWndHandle && IsWindow(_msgWndHandle))
    {
        PostMessage(_msgWndHandle, WM_IpcReconnect, 0, 0);
        _ScheduleDeferredKeyDownDrain();
    }
}

void CMetasequoiaIME::_RequestLocalSessionReset(_In_opt_ ITfContext *preferredContext, UINT resetToken)
{
    if (resetToken == 0 || _localSessionResetToken.load(std::memory_order_acquire) != resetToken ||
        !_localSessionResetPending.load(std::memory_order_acquire))
    {
        return;
    }
    if (resetToken != _localResyncResetToken)
    {
        // A resync keeps the focus token, so its worker acknowledgement stays
        // valid; the pending gate alone already fences worker deliveries.
        _workerCommitReady.store(false, std::memory_order_release);
    }
    _ClearPendingIpcRequests();

    if (_localResetEditSessionQueued)
    {
        return;
    }

    const bool needsCancel = _IsComposing() || _pCandidateListUIPresenter != nullptr;
    if (!needsCancel)
    {
        if (_localSessionResetToken.load(std::memory_order_acquire) == resetToken)
        {
            _localSessionResetPending.store(false, std::memory_order_release);
            _OnLocalSessionResetReleased(resetToken);
        }
        if (Global::g_connected && _msgWndHandle && IsWindow(_msgWndHandle))
        {
            PostMessage(_msgWndHandle, WM_IpcReconnect, 0, 0);
            _ScheduleDeferredKeyDownDrain();
        }
        return;
    }

    ITfContext *resetContext = preferredContext;
    if (resetContext)
    {
        resetContext->AddRef();
    }
    else if (_pContext)
    {
        resetContext = _pContext;
        resetContext->AddRef();
    }
    else if (_pThreadMgr)
    {
        ITfDocumentMgr *documentMgr = nullptr;
        if (SUCCEEDED(_pThreadMgr->GetFocus(&documentMgr)) && documentMgr)
        {
            documentMgr->GetTop(&resetContext);
            documentMgr->Release();
        }
    }

    if (resetContext)
    {
        _KEYSTROKE_STATE keyState = {};
        keyState.Category = CATEGORY_COMPOSING;
        keyState.Function = FUNCTION_CANCEL;
        _localResetEditSessionQueued = true;
        _queuedLocalResetToken = resetToken;
        const HRESULT resetRequestHr =
            _InvokeKeyHandler(resetContext, 0, L'\0', 0, keyState, FANY_IME_NO_REQUEST_ID, {}, resetToken);
        if (FAILED(resetRequestHr) && _localResetEditSessionQueued && _queuedLocalResetToken == resetToken)
        {
            // The edit session was not accepted and therefore cannot retire
            // its queue slot. Keep the exact reset gate closed and retry from
            // the reconnect timer; reopening IPC here could bind the old
            // local composition to a new server focus token.
            _localResetEditSessionQueued = false;
            _queuedLocalResetToken = 0;
            if (Global::g_connected && _msgWndHandle && IsWindow(_msgWndHandle))
            {
                _ipcReconnectDelayMs = CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS;
                SetTimer(_msgWndHandle, TIMER_CONNECT_ALL_NAMEDPIPE, _ipcReconnectDelayMs, nullptr);
            }
        }
        resetContext->Release();
        return;
    }

    // No context remains capable of granting an edit session. At least tear
    // down the local presenter so a stale candidate cannot commit later.
    _DeleteCandidateList(TRUE, nullptr);
    if (_localSessionResetToken.load(std::memory_order_acquire) == resetToken)
    {
        _localSessionResetPending.store(false, std::memory_order_release);
        _OnLocalSessionResetReleased(resetToken);
    }
    if (Global::g_connected && _msgWndHandle && IsWindow(_msgWndHandle))
    {
        PostMessage(_msgWndHandle, WM_IpcReconnect, 0, 0);
        _ScheduleDeferredKeyDownDrain();
    }
}

bool CMetasequoiaIME::_RequestLocalResync()
{
    if (!IsNamedpipeFocusStateOwner(this) || !_msgWndHandle || !IsWindow(_msgWndHandle))
    {
        return false;
    }
    if (_localSessionResetPending.load(std::memory_order_acquire))
    {
        // The pending reset already cancels this composition before the next
        // key is drained, and either rotates the focus token or resyncs.
        return true;
    }
    const UINT resetToken = BeginNamedpipeLocalSessionReset();
    if (resetToken == 0)
    {
        return false;
    }
    _localResyncResetToken = resetToken;
    if (!PostMessage(_msgWndHandle, WM_IpcSessionDirty, static_cast<WPARAM>(resetToken), 0))
    {
        // Always called on the owner thread: run the reset now instead of
        // leaving the gate closed with nothing to reopen it.
        _RequestLocalSessionReset(nullptr, resetToken);
    }
    return true;
}

void CMetasequoiaIME::_OnLocalSessionResetReleased(UINT resetToken)
{
    if (resetToken != 0 && resetToken == _localResyncResetToken)
    {
        _localResyncResetToken = 0;
        // The local cancel could not tell the Server: the reset gate holds back
        // every ordinary packet. The focus token was kept, so clear the Server
        // composition on that same token, ahead of any key drained after this.
        // A transport reset that superseded the resync already rotated the
        // token; its activation clears the Server instead.
        if (Global::g_connected && _IsFocusSessionCurrent(_CaptureFocusSessionToken()))
        {
            SendHideCandidateWndEventToUIProcess();
        }
    }
    _ReleaseIdleDeferredProjection();
}

void CMetasequoiaIME::_ScheduleCandidatePresenterCleanup(_In_ CCandidateListUIPresenter *pPresenter)
{
    if (pPresenter == nullptr)
    {
        return;
    }

    pPresenter->_PrepareForAsyncCleanup();
    _pendingCandidatePresenterCleanup.push_back(pPresenter);

    if (_msgWndHandle)
    {
        PostMessage(_msgWndHandle, WM_CleanupCandidatePresenter, 0, 0);
    }
}

void CMetasequoiaIME::_DrainPendingCandidatePresenterCleanup()
{
    PerfTimer timer;
    size_t cleanedCount = 0;
    while (!_pendingCandidatePresenterCleanup.empty())
    {
        CCandidateListUIPresenter *pPresenter = _pendingCandidatePresenterCleanup.front();
        _pendingCandidatePresenterCleanup.pop_front();
        if (pPresenter)
        {
            PerfTimer deleteTimer;
            delete pPresenter;
            ++cleanedCount;
        }
    }
}

//+---------------------------------------------------------------------------
//
// QueryInterface
//
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::QueryInterface(REFIID riid, _Outptr_ void **ppvObj)
{
    if (ppvObj == nullptr)
    {
        return E_INVALIDARG;
    }

    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfTextInputProcessor))
    {
        *ppvObj = (ITfTextInputProcessor *)this;
    }
    else if (IsEqualIID(riid, IID_ITfTextInputProcessorEx))
    {
        *ppvObj = (ITfTextInputProcessorEx *)this;
    }
    else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink))
    {
        *ppvObj = (ITfThreadMgrEventSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfTextEditSink))
    {
        *ppvObj = (ITfTextEditSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfKeyEventSink))
    {
        *ppvObj = (ITfKeyEventSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfActiveLanguageProfileNotifySink))
    {
        *ppvObj = (ITfActiveLanguageProfileNotifySink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfCompositionSink))
    {
        *ppvObj = (ITfCompositionSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfDisplayAttributeProvider))
    {
        *ppvObj = (ITfDisplayAttributeProvider *)this;
    }
    else if (IsEqualIID(riid, IID_ITfThreadFocusSink))
    {
        *ppvObj = (ITfThreadFocusSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfFunctionProvider))
    {
        *ppvObj = (ITfFunctionProvider *)this;
    }
    else if (IsEqualIID(riid, IID_ITfFunction))
    {
        *ppvObj = (ITfFunction *)this;
    }
    else if (IsEqualIID(riid, IID_ITfFnGetPreferredTouchKeyboardLayout))
    {
        *ppvObj = (ITfFnGetPreferredTouchKeyboardLayout *)this;
    }

    if (*ppvObj)
    {
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

//+---------------------------------------------------------------------------
//
// AddRef
//
//----------------------------------------------------------------------------

STDAPI_(ULONG) CMetasequoiaIME::AddRef()
{
    return ++_refCount;
}

//+---------------------------------------------------------------------------
//
// Release
//
//----------------------------------------------------------------------------

STDAPI_(ULONG) CMetasequoiaIME::Release()
{
    LONG cr = --_refCount;

    assert(_refCount >= 0);

    if (_refCount == 0)
    {
        delete this;
    }

    return cr;
}

//+---------------------------------------------------------------------------
//
// ITfTextInputProcessorEx::ActivateEx
//
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::ActivateEx(ITfThreadMgr *pThreadMgr, TfClientId tfClientId, DWORD dwFlags)
{
    _pThreadMgr = pThreadMgr;
    _pThreadMgr->AddRef();

    _tfClientId = tfClientId;
    _dwActivateFlags = dwFlags;
    Global::HostUiLessMode = _IsUiLessMode() ? true : false;
    Global::CandidateUiLessMode = false;
    // Match Weasel's activation-time recovery: switching to this TIP is an
    // explicit user request, so revive a missing Server immediately. Merely
    // focusing another text box still uses reconnect-only behavior.
    _WakeServerIfNeeded();
    _focusResetPending = false;
    _activationRequired = false;
    _focusLossDeferPending = false;
    _workerCommitReady.store(false, std::memory_order_release);
    _acknowledgedWorkerFocusToken.store(0, std::memory_order_release);
    _localSessionResetPending.store(false, std::memory_order_release);
    _localResetEditSessionQueued = false;
    _queuedLocalResetToken = 0;
    BindNamedpipeFocusState(this, &_focusResetPending, &_activationRequired, &_expectedWorkerFocusToken,
                            &_localSessionResetPending, &_localSessionResetToken, &_workerCommitReady,
                            &_acknowledgedWorkerFocusToken, &_hToTsfWorkerThreadPipe, &_workerPipeGeneration);

    /*
    std::wstring processName = FanyUtils::GetCurrentProcessName();
    if (Global::VSCodeSeries.find(processName) != Global::VSCodeSeries.end())
    {
        Global::IsVSCodeLike = true;
    }
    */
    // Set up IPC(named pipe)
    // InitIpc();
    // TODO: 去掉共享内存，只保留命名管道
    // InitNamedpipe();

    Global::current_process_name = GetCurrentProcessName();

    if (!_InitThreadMgrEventSink())
    {
        goto ExitError;
    }

    // Register generic window class for message-only window
    {
        WNDCLASSEX wcex = {};
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.lpfnWndProc = CMetasequoiaIME_WindowProc;
        wcex.hInstance = Global::dllInstanceHandle;
        wcex.lpszClassName = L"MetasequoiaIMEWorkerWnd";
        wcex.cbWndExtra = sizeof(LONG_PTR);
        RegisterClassEx(&wcex);
    }

    _msgWndHandle = CreateWindowEx( //
        0,                          //
        L"MetasequoiaIMEWorkerWnd", //
        L"MetasequoiaIMEWorkerWnd", //
        0, 0, 0, 0, 0,              //
        HWND_MESSAGE,               //
        nullptr,                    //
        Global::dllInstanceHandle,  //
        this);
    if (!_msgWndHandle)
    {
        goto ExitError;
    }
    SetWindowLongPtr(_msgWndHandle, GWLP_USERDATA, (LONG_PTR)this);
    Global::msgWndHandle = _msgWndHandle;
    _StartThemeRegistryWatcher();

    BOOL hasThreadFocus = FALSE;
    const HRESULT threadFocusResult = _pThreadMgr->IsThreadFocus(&hasThreadFocus);
    // Preserve the historical eager-connect behavior if a host cannot report
    // focus. Otherwise an already-focused activation could remain offline
    // forever because no later focus callback is guaranteed.
    Global::g_connected = SUCCEEDED(threadFocusResult) ? hasThreadFocus != FALSE : true;
    if (Global::g_connected)
    {
        RequireNamedpipeFocusActivation();
    }

    /* 创建 IPC 线程 */
    _ipcStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!_ipcStopEvent)
    {
        goto ExitError;
    }
    // Optional: if either fails the waits fall back to the original polling.
    _workerAckEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    _workerPipePublishedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    BindNamedpipeWakeEvents(this, _workerAckEvent, _workerPipePublishedEvent);
    _shouldStopIpcThread.store(false);
    _pIpcThread = new std::thread(IpcWorkerThread, this);

    if (Global::g_connected)
    {
        PostMessage(_msgWndHandle, WM_ConnectNamedpipe, 0, 0);
    }

    ITfDocumentMgr *pDocMgrFocus = nullptr;
    if (SUCCEEDED(_pThreadMgr->GetFocus(&pDocMgrFocus)) && (pDocMgrFocus != nullptr))
    {
        _InitTextEditSink(pDocMgrFocus);
        pDocMgrFocus->Release();
    }

    if (!_InitKeyEventSink())
    {
        goto ExitError;
    }

    if (!_InitActiveLanguageProfileNotifySink())
    {
        goto ExitError;
    }

    if (!_InitThreadFocusSink())
    {
        goto ExitError;
    }

    if (!_InitDisplayAttributeGuidAtom())
    {
        goto ExitError;
    }

    if (!_InitFunctionProviderSink())
    {
        goto ExitError;
    }

    if (!_AddTextProcessorEngine())
    {
        goto ExitError;
    }

    Global::CapsLockEnabled.store((GetKeyState(VK_CAPITAL) & 0x0001) != 0, std::memory_order_relaxed);

    // Apply configured default CN/EN whenever switching back to this IME
    _pCompositionProcessorEngine->InitializeMetasequoiaIMECompartment(pThreadMgr, tfClientId);
    _InitBareShiftKeyboardHook();

    // The first connect timer can run before the engine exists. Re-arm after
    // compartment initialization so an already-focused activation always
    // replays both ownership and a complete status snapshot.
    if (Global::g_connected)
    {
        PostMessage(_msgWndHandle, WM_ThreadFocus, 0, 0);
    }

    // TSF lifecycle is published on the epoch-checked Main pipe. The Server
    // uses terminal activation/deactivation to reconcile floating-toolbar
    // visibility and status snapshots to update its displayed mode.

    return S_OK;

ExitError:
    Deactivate();
    return E_FAIL;
}

//+---------------------------------------------------------------------------
//
// ITfTextInputProcessorEx::Deactivate
//
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::Deactivate()
{
    _UninitBareShiftKeyboardHook();
    Global::HostUiLessMode = false;
    Global::CandidateUiLessMode = false;
    // Send this synchronously before destroying the message window. OnKill can
    // queue the same lifecycle event, but that queued message may never run
    // during a rapid TIP deactivation; the server treats duplicates as idempotent.
    const uint64_t deactivatedFocusToken = _expectedWorkerFocusToken.load(std::memory_order_acquire);
    Global::g_connected = false;
    _workerCommitReady.store(false, std::memory_order_release);
    BeginNamedpipeLocalSessionReset(); // invalidate queued dirty-reset messages
    _localSessionResetPending.store(false, std::memory_order_release);
    _localResetEditSessionQueued = false;
    _queuedLocalResetToken = 0;
    _localResyncResetToken = 0;
    _ClearDeferredKeyDowns();
    MarkNamedpipeFocusLost();
    FlushNamedpipeImeDeactivation(deactivatedFocusToken);
    _ClearPendingIpcRequests();

    // 注销此输入法时，向 server 端发送一个注销的消息
    // 注销不要给消息窗口发送消息，而是直接在这里处理
    // SendIMEDeactivationEventToUIProcessViaNamedPipe();

    // ClientDeactivated closes the Server-side input session and hides the
    // toolbar because the TIP itself is going away. A temporary thread-focus
    // loss uses ClientSuspended instead and deliberately keeps it visible.

    /* 清理 IPC 线程 */
    _shouldStopIpcThread.store(true);
    if (_ipcStopEvent)
    {
        SetEvent(_ipcStopEvent);
    }
    HANDLE workerPipe = _hToTsfWorkerThreadPipe.load();
    if (workerPipe && workerPipe != INVALID_HANDLE_VALUE)
    {
        CancelIoEx(workerPipe, nullptr);
    }
    if (_pIpcThread && _pIpcThread->joinable())
    {
        _pIpcThread->join();
        delete _pIpcThread;
        _pIpcThread = nullptr;
    }
    _hToTsfWorkerThreadPipe.store(nullptr);
    InvalidateNamedpipeWorkerGeneration();
    // The worker no longer has an outstanding OVERLAPPED operation, so the
    // UI thread can now close all three handles safely.
    CloseIpc();
    if (_ipcStopEvent)
    {
        CloseHandle(_ipcStopEvent);
        _ipcStopEvent = nullptr;
    }
    // _EndComposition below can still reach the focus-activation wait on this
    // thread; unbind first so it falls back to polling instead of waiting on a
    // closed (or recycled) handle.
    BindNamedpipeWakeEvents(this, nullptr, nullptr);
    _CloseIpcWakeEvents();
    // Stop callbacks from the previously focused top context before tearing
    // down the engine and thread manager. Context stack changes can otherwise
    // re-enter a half-deactivated service.
    _InitTextEditSink(nullptr);
    // TODO: 去掉共享内存，只保留命名管道
    // CloseNamedpipe();

    ITfContext *pContext = _pContext;
    if (_pContext)
    {
        pContext->AddRef();
        _EndComposition(_pContext, _pComposition, true);
    }

    if (_pCandidateListUIPresenter)
    {
        delete _pCandidateListUIPresenter;
        _pCandidateListUIPresenter = nullptr;

        _candidateMode = CANDIDATE_NONE;
        _isCandidateWithWildcard = FALSE;
    }

    if (pContext)
    {
        pContext->Release();
    }

    _DrainPendingCandidatePresenterCleanup();

    if (_pCompositionProcessorEngine)
    {
        delete _pCompositionProcessorEngine;
        _pCompositionProcessorEngine = nullptr;
    }

    _UninitFunctionProviderSink();

    _UninitThreadFocusSink();

    _UninitActiveLanguageProfileNotifySink();

    _UninitKeyEventSink();

    _UninitThreadMgrEventSink();

    CCompartment CompartmentKeyboardOpen(_pThreadMgr, _tfClientId, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
    CompartmentKeyboardOpen._ClearCompartment();

    CCompartment CompartmentDoubleSingleByte(_pThreadMgr, _tfClientId,
                                             Global::MetasequoiaIMEGuidCompartmentDoubleSingleByte);
    CompartmentDoubleSingleByte._ClearCompartment();

    CCompartment CompartmentPunctuation(_pThreadMgr, _tfClientId, Global::MetasequoiaIMEGuidCompartmentPunctuation);
    CompartmentPunctuation._ClearCompartment();

    if (_pThreadMgr != nullptr)
    {
        _pThreadMgr->Release();
        _pThreadMgr = nullptr;
    }

    _tfClientId = TF_CLIENTID_NULL;

    if (_pDocMgrLastFocused)
    {
        _pDocMgrLastFocused->Release();
        _pDocMgrLastFocused = nullptr;
    }

    /* 清理消息窗口 */
    _StopThemeRegistryWatcher();
    if (_msgWndHandle)
    {
        KillTimer(_msgWndHandle, TIMER_CONNECT_ALL_NAMEDPIPE);
        KillTimer(_msgWndHandle, TIMER_CONNECT_TO_TSF_NAMEDPIPE);
        KillTimer(_msgWndHandle, TIMER_REFRESH_LANG_BAR_THEME);
        KillTimer(_msgWndHandle, TIMER_DEFERRED_FOCUS_LOSS);
        KillTimer(_msgWndHandle, TIMER_FOCUS_STATUS_RESEND);
        KillTimer(_msgWndHandle, TIMER_FOCUS_CARET_STATE);
        // 不同于上面几个：成对标点的重试定时器还带着 _pairedCaretRetryTimerActive
        // 这一份状态，只 KillTimer 会让标志停在 true，消息窗口重建后就再也装不上
        // 定时器了，所以走完整的取消路径。
        _CancelPairedPunctuationCaretMove();
        DestroyWindow(_msgWndHandle);
        if (Global::msgWndHandle == _msgWndHandle)
        {
            Global::msgWndHandle = nullptr;
        }
        _msgWndHandle = nullptr;
    }
    UnregisterClass(L"MetasequoiaIMEWorkerWnd", Global::dllInstanceHandle);

    UnbindNamedpipeFocusState(this);
    _focusResetPending = false;
    _activationRequired = false;
    _focusLossDeferPending = false;

    return S_OK;
}

void CMetasequoiaIME::_CloseIpcWakeEvents()
{
    if (_workerAckEvent)
    {
        CloseHandle(_workerAckEvent);
        _workerAckEvent = nullptr;
    }
    if (_workerPipePublishedEvent)
    {
        CloseHandle(_workerPipePublishedEvent);
        _workerPipePublishedEvent = nullptr;
    }
}

//+---------------------------------------------------------------------------
//
// _WakeServerIfNeeded
//
//----------------------------------------------------------------------------
void CMetasequoiaIME::_WakeServerIfNeeded()
{
    if (_IsSecureMode() || _IsComLess() || IsServerAlreadyRunning())
        return;
    if (!RequestServerLaunch())
        return;

    _ipcConsecutiveFailures = 0;
    if (Global::g_connected && _msgWndHandle && IsWindow(_msgWndHandle))
    {
        _ipcReconnectDelayMs = SERVER_LAUNCH_RECONNECT_DELAY_MS;
        SetTimer(_msgWndHandle, TIMER_CONNECT_ALL_NAMEDPIPE, _ipcReconnectDelayMs, nullptr);
    }
}

//+---------------------------------------------------------------------------
//
// _NoteKeyEventIpcFailure
//
//----------------------------------------------------------------------------
void CMetasequoiaIME::_NoteKeyEventIpcFailure()
{
    // Only a real keystroke counts as evidence that the user wants the Server
    // back. The background reconnect timer must never revive it on its own,
    // otherwise merely focusing a text box resurrects a deliberately killed
    // Server.
    if (++_ipcConsecutiveFailures < IPC_FAILURES_BEFORE_SERVER_LAUNCH)
    {
        return;
    }

    _ipcConsecutiveFailures = 0;
    if (IsServerAlreadyRunning())
    {
        // A live Server that has merely lost its focus session is recovered by
        // the ordinary reconnect timer. Do not disturb that schedule.
        return;
    }
    if (_IsSecureMode() || _IsComLess() || !RequestServerLaunch())
    {
        return;
    }
    if (Global::g_connected && _msgWndHandle && IsWindow(_msgWndHandle))
    {
        _ipcReconnectDelayMs = SERVER_LAUNCH_RECONNECT_DELAY_MS;
        SetTimer(_msgWndHandle, TIMER_CONNECT_ALL_NAMEDPIPE, _ipcReconnectDelayMs, nullptr);
    }
}

//+---------------------------------------------------------------------------
//
// ITfFunctionProvider::GetType
//
//----------------------------------------------------------------------------
HRESULT CMetasequoiaIME::GetType(__RPC__out GUID *pguid)
{
    HRESULT hr = E_INVALIDARG;
    if (pguid)
    {
        *pguid = Global::MetasequoiaIMECLSID;
        hr = S_OK;
    }
    return hr;
}

//+---------------------------------------------------------------------------
//
// ITfFunctionProvider::::GetDescription
//
//----------------------------------------------------------------------------
HRESULT CMetasequoiaIME::GetDescription(__RPC__deref_out_opt BSTR *pbstrDesc)
{
    HRESULT hr = E_INVALIDARG;
    if (pbstrDesc != nullptr)
    {
        *pbstrDesc = nullptr;
        hr = E_NOTIMPL;
    }
    return hr;
}

//+---------------------------------------------------------------------------
//
// ITfFunctionProvider::::GetFunction
//
//----------------------------------------------------------------------------
HRESULT CMetasequoiaIME::GetFunction(__RPC__in REFGUID rguid, __RPC__in REFIID riid,
                                     __RPC__deref_out_opt IUnknown **ppunk)
{
    HRESULT hr = E_NOINTERFACE;

    if ((IsEqualGUID(rguid, GUID_NULL)) && (IsEqualGUID(riid, __uuidof(ITfFnSearchCandidateProvider))))
    {
        if (_pITfFnSearchCandidateProvider != nullptr)
        {
            hr = _pITfFnSearchCandidateProvider->QueryInterface(riid, (void **)ppunk);
        }
    }
    else if (IsEqualGUID(rguid, GUID_NULL))
    {
        hr = QueryInterface(riid, (void **)ppunk);
    }

    return hr;
}

//+---------------------------------------------------------------------------
//
// ITfFunction::GetDisplayName
//
//----------------------------------------------------------------------------
HRESULT CMetasequoiaIME::GetDisplayName(_Out_ BSTR *pbstrDisplayName)
{
    HRESULT hr = E_INVALIDARG;
    if (pbstrDisplayName != nullptr)
    {
        *pbstrDisplayName = nullptr;
        hr = E_NOTIMPL;
    }
    return hr;
}

//+---------------------------------------------------------------------------
//
// ITfFnGetPreferredTouchKeyboardLayout::GetLayout
// The tkblayout will be Optimized layout.
//----------------------------------------------------------------------------
HRESULT CMetasequoiaIME::GetLayout(_Out_ TKBLayoutType *ptkblayoutType, _Out_ WORD *pwPreferredLayoutId)
{
    HRESULT hr = E_INVALIDARG;
    if ((ptkblayoutType != nullptr) && (pwPreferredLayoutId != nullptr))
    {
        *ptkblayoutType = TKBLT_OPTIMIZED;
        *pwPreferredLayoutId = TKBL_OPT_SIMPLIFIED_CHINESE_PINYIN;
        hr = S_OK;
    }
    return hr;
}
