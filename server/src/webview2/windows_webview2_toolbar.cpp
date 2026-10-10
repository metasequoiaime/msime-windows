// 悬浮工具栏 WebView：工具栏状态缓存与渲染脚本、controller 的创建与页面消息处理，以及各个 UpdateFtb*。
#include "webview2/windows_webview2_internal.h"
#include "engine/contracts/webview/validator.h"
#include "config/ime_config.h"
#include "defines/globals.h"
#include "global/globals.h"
#include "ipc/ipc.h"
#include "ipc/event_listener.h"
#include "settings/settings_launcher.h"
#include "utils/common_utils.h"
#include "utils/window_utils.h"
#include "window/floating_toolbar_presenter.h"
#include <dwmapi.h>
#include <cmath>
#include <filesystem>
#include <string>

using namespace windows_webview2_detail;

namespace
{
struct FloatingToolbarState
{
    // Keep these defaults aligned with the TSF compartment defaults:
    // Chinese input, half-width characters, and Chinese punctuation.
    int cn_en = 1;
    int double_single_byte = 0;
    int punctuation = 1;
    int english_input_mode = 0;
    int caps_lock = 0;
    int japanese_input_mode = 0;
};

FloatingToolbarState floatingToolbarState;

bool UpdateBinaryState(int value, int &state)
{
    // The existing UI contract only defines 0 and 1. Ignore malformed values
    // instead of replacing a known-good cached state.
    if ((value == 0 || value == 1) && value != state)
    {
        state = value;
        return true;
    }
    return false;
}

void RenderFloatingToolbarState(ICoreWebView2 *webview)
{
    if (FloatingToolbarPresenter::Instance().IsBound())
    {
        FloatingToolbarPresenter::Instance().SyncUi(
            floatingToolbarState.cn_en, floatingToolbarState.double_single_byte, floatingToolbarState.punctuation,
            floatingToolbarState.english_input_mode, floatingToolbarState.caps_lock,
            floatingToolbarState.japanese_input_mode);
    }

    if (!floatingToolbarNavigationReady || webview == nullptr)
    {
        return;
    }

    std::wstring script;
    script.reserve(1600);
    const bool japanese_mode = floatingToolbarState.japanese_input_mode == 1;
    constexpr wchar_t kHideJa[] = L"{const ja=document.getElementById('ja');if(ja)ja.style.display='none';}";
    constexpr wchar_t kShowJa[] = L"{const ja=document.getElementById('ja');if(ja)ja.style.display='flex';}";
    constexpr wchar_t kHideCap[] = L"{const cap=document.getElementById('cap');if(cap)cap.style.display='none';}";
    constexpr wchar_t kShowCap[] = L"{const cap=document.getElementById('cap');if(cap)cap.style.display='flex';}";
    const wchar_t *lang_token = L"cn";
    if (floatingToolbarState.cn_en != 1)
    {
        lang_token = L"en";
    }
    else if (floatingToolbarState.english_input_mode == 1)
    {
        lang_token = L"en-candidate";
    }
    else if (japanese_mode)
    {
        lang_token = L"ja";
    }
    script.append(L"{const host=document.getElementById('cn-en');if(host)host.dataset.lang='");
    script.append(lang_token);
    script.append(L"';}");

    if (floatingToolbarState.caps_lock == 1)
    {
        script.append(L"document.getElementById('cn').style.display = 'none';"
                      L"document.getElementById('en-candidate').style.display = 'none';"
                      L"document.getElementById('en').style.display = 'none';");
        script.append(kHideJa);
        script.append(kShowCap);
    }
    else if (floatingToolbarState.cn_en == 1)
    {
        script.append(kHideCap);
        if (floatingToolbarState.english_input_mode == 1)
        {
            script.append(L"document.getElementById('cn').style.display = 'none';"
                          L"document.getElementById('en-candidate').style.display = 'flex';"
                          L"document.getElementById('en').style.display = 'none';");
            script.append(kHideJa);
        }
        else if (japanese_mode)
        {
            script.append(L"document.getElementById('cn').style.display = 'none';"
                          L"document.getElementById('en-candidate').style.display = 'none';"
                          L"document.getElementById('en').style.display = 'none';");
            script.append(kShowJa);
        }
        else
        {
            script.append(L"document.getElementById('cn').style.display = 'flex';"
                          L"document.getElementById('en-candidate').style.display = 'none';"
                          L"document.getElementById('en').style.display = 'none';");
            script.append(kHideJa);
        }
    }
    else
    {
        script.append(kHideCap);
        script.append(L"document.getElementById('cn').style.display = 'none';");
        script.append(L"document.getElementById('en-candidate').style.display = 'none';");
        script.append(L"document.getElementById('en').style.display = 'flex';");
        script.append(kHideJa);
    }

    if (floatingToolbarState.double_single_byte == 1)
    {
        script.append(L"document.getElementById('fullwidth').style.display = 'flex';");
        script.append(L"document.getElementById('halfwidth').style.display = 'none';");
    }
    else
    {
        script.append(L"document.getElementById('fullwidth').style.display = 'none';");
        script.append(L"document.getElementById('halfwidth').style.display = 'flex';");
    }

    if (floatingToolbarState.punctuation == 1)
    {
        script.append(L"document.getElementById('puncCn').style.display = 'flex';");
        script.append(L"document.getElementById('puncEn').style.display = 'none';");
    }
    else
    {
        script.append(L"document.getElementById('puncCn').style.display = 'none';");
        script.append(L"document.getElementById('puncEn').style.display = 'flex';");
    }

    if (GetConfiguredCharacterSet() == "traditional")
    {
        script.append(L"document.getElementById('character-set-simplified').style.display = 'none';");
        script.append(L"document.getElementById('character-set-traditional').style.display = 'flex';");
    }
    else
    {
        script.append(L"document.getElementById('character-set-simplified').style.display = 'flex';");
        script.append(L"document.getElementById('character-set-traditional').style.display = 'none';");
    }

    const FloatingToolbarItemsConfig &items = GetConfiguredFloatingToolbarItems();
    script.append(L"window.setToolbarItem=(id,shown)=>{const item=document.getElementById(id);"
                  L"if(item)item.style.display=shown?'flex':'none';};");
    script.append(items.fullwidth ? L"window.setToolbarItem('char-width-mode',true);"
                                  : L"window.setToolbarItem('char-width-mode',false);");
    script.append(items.punctuation ? L"window.setToolbarItem('punctuation-mode',true);"
                                    : L"window.setToolbarItem('punctuation-mode',false);");
    script.append(items.character_set ? L"window.setToolbarItem('character-set',true);"
                                      : L"window.setToolbarItem('character-set',false);");
    script.append(items.emoji ? L"window.setToolbarItem('emoji-panel',true);"
                              : L"window.setToolbarItem('emoji-panel',false);");
    script.append(items.screen_keyboard ? L"window.setToolbarItem('screen-keyboard',true);"
                                        : L"window.setToolbarItem('screen-keyboard',false);");
    script.append(items.settings ? L"window.setToolbarItem('settings',true);"
                                 : L"window.setToolbarItem('settings',false);");
    script.append(L"if(window.CheckContentTruncation)window.CheckContentTruncation();");

    webview->ExecuteScript(script.c_str(), nullptr);
}
} // namespace

//
//
// floating toolbar(ftb) 窗口 webview
//
//

/**
 * @brief Handle floating toolbar window webview2 controller creation
 *
 * @param hwnd
 * @param result
 * @param controller
 * @return HRESULT
 */
HRESULT OnControllerCreatedFtbWnd(      //
    HWND hwnd,                          //
    HRESULT result,                     //
    ICoreWebView2Controller *controller //
)
{
    if (!controller || FAILED(result))
    {
        OnSmallWindowControllerSettled(FAILED(result) ? result : E_FAIL);
        return E_FAIL;
    }

    /* 给 controller 和 webview 赋值 */
    webviewControllerFtbWnd = controller;
    const HRESULT getFtbWebviewHr = webviewControllerFtbWnd->get_CoreWebView2(webviewFtbWnd.GetAddressOf());

    if (!webviewFtbWnd)
    {
        webviewControllerFtbWnd.Reset();
        OnSmallWindowControllerSettled(FAILED(getFtbWebviewHr) ? getFtbWebviewHr : E_FAIL);
        return E_FAIL;
    }

    // WebView2 only completes raster setup against an on-monitor visible host.
    // If this ever reports host_visible=false, the toolbar for this session will
    // stay blank no matter how often it is later shown. Cloaked is the healthy
    // warmup state: visible to WebView2, invisible to the user.
    DWORD hostCloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &hostCloaked, sizeof(hostCloaked));
    FTB_DIAG_LOGF(L"ftb controller created host_visible={} host_cloaked={}", IsWindowVisible(hwnd) != FALSE,
                  hostCloaked != 0);
    UpdateSmallWindowWebviewVisibility(hwnd, IsWindowVisible(hwnd) != FALSE);

    // Configure webviewFtbWindow settings
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webviewFtbWnd->get_Settings(&settings)))
    {
        settings->put_IsScriptEnabled(TRUE);
        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        settings->put_IsWebMessageEnabled(TRUE);
        settings->put_AreHostObjectsAllowed(FALSE);
        // 禁用右键菜单和开发者工具
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        // 禁止界面缩放
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);

        ComPtr<ICoreWebView2Settings3> settings3;
        if (SUCCEEDED(settings.As(&settings3)))
        {
            settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        }

        ComPtr<ICoreWebView2Settings5> settings5;
        if (SUCCEEDED(settings.As(&settings5)))
        {
            settings5->put_IsGeneralAutofillEnabled(FALSE);
            settings5->put_IsPasswordAutosaveEnabled(FALSE);
        }
    }

    // 初始时缩放设置成 1.0
    webviewControllerFtbWnd->put_ZoomFactor(1.0);

    // Configure virtual host path
    if (SUCCEEDED(webviewFtbWnd->QueryInterface(IID_PPV_ARGS(&webview3FtbWnd))))
    {
        const auto contractsPath =
            std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"html" / L"webview2" / L"shared";
        webview3FtbWnd->SetVirtualHostNameToFolderMapping(L"msime-contracts", contractsPath.wstring().c_str(),
                                                          COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);

        // Assets mapping
        webview3FtbWnd->SetVirtualHostNameToFolderMapping(   //
            L"appassets",                                    //
            GetLocalAssetsPath().c_str(),                    //
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS //
        );                                                   //
    }

    // Set transparent background
    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&webviewController2FtbWnd))))
    {
        COREWEBVIEW2_COLOR backgroundColor = {0, 0, 0, 0};
        webviewController2FtbWnd->put_DefaultBackgroundColor(backgroundColor);
    }

    // Adjust to window size
    RECT bounds;
    GetClientRect(hwnd, &bounds);
    webviewControllerFtbWnd->put_Bounds(bounds);

    // State notifications can arrive before the controller exists or while
    // NavigateToString is still loading. Only render after a successful
    // navigation, then replay the complete cached state in one operation.
    ClearFloatingToolbarNavigationState();
    EventRegistrationToken navigationCompletedToken{};
    const HRESULT navigationCompletedResult = webviewFtbWnd->add_NavigationCompleted(
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                BOOL success = FALSE;
                if (args)
                {
                    args->get_IsSuccess(&success);
                }
                if (success)
                {
                    NotifySmallWindowNavigationReady(floatingToolbarNavigationReady, L"floating-toolbar");
                    ApplyConfiguredFloatingToolbarAppearance();
                    floatingToolbarState.japanese_input_mode = GetActiveInputMode() == "japanese" ? 1 : 0;
                    RenderFloatingToolbarState(sender);
                    ApplyConfiguredFloatingToolbarSize();
                    InjectSurfaceViewportLimits(sender, ::global_hwnd_ftb);
                    // Show first so a cold user-data folder can finish painting;
                    // a short timer then hides or keeps it based on real state.
                    ApplyConfiguredFloatingToolbarVisibility(L"ftb-navigation-completed");
                }
                else
                {
                    // Cold user-data bring-up can fail the first NavigateToString.
                    // Retry once against the same controller instead of leaving
                    // the host permanently cloaked with webview_ready=false.
                    if (!floatingToolbarNavigationRetryUsed && !::HTMLStringFtbWnd.empty())
                    {
                        floatingToolbarNavigationRetryUsed = true;
                        floatingToolbarNavigationReady = false;
                        floatingToolbarPaintGraceActive = false;
                        FTB_DIAG_LOGF(L"ftb navigation failed; retrying NavigateToString once");
                        sender->NavigateToString(::HTMLStringFtbWnd.c_str());
                    }
                    else
                    {
                        FTB_DIAG_LOGF(L"ftb navigation failed permanently after retry");
                    }
                }
                return S_OK;
            })
            .Get(),
        &navigationCompletedToken);
    if (FAILED(navigationCompletedResult))
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return navigationCompletedResult;
    }

    // Navigate to HTML
    HRESULT hr = webviewFtbWnd->NavigateToString(::HTMLStringFtbWnd.c_str());
    if (SUCCEEDED(hr))
    {
        loadedFloatingToolbarSkin = preparedCandidateSkin;
    }
    if (FAILED(hr))
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return hr;
    }

    /* Debug console */
    // webviewFtbWindow->OpenDevToolsWindow();

    /* 处理 js 发过来的消息 */
    webviewFtbWnd->add_WebMessageReceived(
        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [hwnd](ICoreWebView2 * /*sender*/, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                wil::unique_cotaskmem_string message;
                HRESULT hr = args->TryGetWebMessageAsString(&message);
                if (SUCCEEDED(hr) && message.get())
                {
                    std::wstring msg(message.get());
                    // 解析 msg，执行相应操作
                    json::value val = json::parse(wstring_to_string(msg));
                    if (!metasequoia::webview::Validate(val, "client", "toolbar"))
                        return S_OK;
                    std::string type = json::value_to<std::string>(val.at("type"));
                    /* 使 floating toolbar 窗口可拖动 */
                    if (type == "dragStart")
                    {
                        ReleaseCapture();
                        PostMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                    }
                    else if (type == "ready")
                    {
                        NotifyFloatingToolbarPageReady();
                    }
                    else if (type == "changeIMEMode")
                    {
                        std::string mode = json::value_to<std::string>(val.at("data"));

                        if (mode == "cn") // Change to CN
                        {
#ifdef FANY_DEBUG
                            (void)0;
#endif
                            SendToTsfWorkerThreadViaNamedpipe(
                                Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToCn, L"");
                        }
                        else if (mode == "en") // Change to EN
                        {
#ifdef FANY_DEBUG
                            (void)0;
#endif
                            SendToTsfWorkerThreadViaNamedpipe(
                                Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToEn, L"");
                        }
                    }
                    else if (type == "exitEnglishInputMode")
                    {
                        FanyNamedPipe::EnqueueExitEnglishInputModeTask();
                    }
                    else if (type == "changeCharMode")
                    {
                        std::string mode = json::value_to<std::string>(val.at("data"));
                        if (mode == "fullwidth")
                        {
#ifdef FANY_DEBUG
                            (void)0;
#endif
                            SendToTsfWorkerThreadViaNamedpipe(
                                Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToFullwidth, L"");
                        }
                        else if (mode == "halfwidth")
                        {
#ifdef FANY_DEBUG
                            (void)0;
#endif
                            SendToTsfWorkerThreadViaNamedpipe(
                                Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToHalfwidth, L"");
                        }
                    }
                    else if (type == "changePuncMode")
                    {
                        std::string mode = json::value_to<std::string>(val.at("data"));
                        if (mode == "puncEn")
                        {
#ifdef FANY_DEBUG
                            (void)0;
#endif
                            SendToTsfWorkerThreadViaNamedpipe(
                                Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToPuncEn, L"");
                        }
                        else if (mode == "puncCn")
                        {
#ifdef FANY_DEBUG
                            (void)0;
#endif
                            SendToTsfWorkerThreadViaNamedpipe(
                                Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToPuncCn, L"");
                        }
                    }
                    else if (type == "changeCharacterSet")
                    {
                        const std::string next =
                            GetConfiguredCharacterSet() == "traditional" ? "simplified" : "traditional";
                        SetConfiguredCharacterSet(next);
                    }
                    else if (type == "openSettings")
                    {
#ifdef FANY_DEBUG
                        (void)0;
#endif
                        OpenSettingsApplication();
                    }
                    else if (type == "openEmojiPanel")
                    {
#ifdef FANY_DEBUG
                        (void)0;
#endif
                        OpenEmojiPanelApplication();
                    }
                    else if (type == "openKeyboardPanel")
                    {
                        OpenKeyboardPanelApplication();
                    }
                    else if (type == "contentTruncated")
                    {
                        if (HandleContentTruncatedMessage(hwnd, webviewFtbWnd.Get(), webviewControllerFtbWnd.Get(), val,
                                                          g_last_content_truncation_ftb_ms, ::FTB_WND_SHADOW_WIDTH))
                        {
                            const double widthDip = JsonNumberAsDouble(val.at("data").at("width"));
                            const double heightDip = JsonNumberAsDouble(val.at("data").at("height"));
                            const HalfScreenDipLimits limits = QueryWebViewHalfScreenDipLimitsForHwnd(hwnd);
                            ::FTB_CONTENT_WIDTH_DIP = ClampWidthDipToHalfScreen(widthDip, limits);
                            ::FTB_CONTENT_HEIGHT_DIP = ClampHeightDipToHalfScreen(heightDip, limits);
                            ::FTB_WND_WIDTH =
                                static_cast<int>(std::ceil(::FTB_CONTENT_WIDTH_DIP * kTruncationSizeFactor));
                            ::FTB_WND_HEIGHT =
                                static_cast<int>(std::ceil(::FTB_CONTENT_HEIGHT_DIP * kTruncationSizeFactor));
                        }
                    }
                }

                return S_OK;
            })
            .Get(),
        nullptr);

    OnSmallWindowControllerSettled(S_OK);
    return S_OK;
}

/**
 * @brief Handle floating toolbar window webview2 environment creation
 *
 * @param hwnd
 * @param result
 * @param env
 * @return HRESULT
 */
HRESULT OnFtbWindowEnvironmentCreated(HWND hwnd, HRESULT result, ICoreWebView2Environment *env)
{
    if (FAILED(result) || !env)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return result;
    }

    // Create WebView2 controller
    return env->CreateCoreWebView2Controller(                                        //
        hwnd,                                                                        //
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(         //
            [hwnd](HRESULT result, ICoreWebView2Controller *controller) -> HRESULT { //
                return OnControllerCreatedFtbWnd(hwnd, result, controller);          //
            })                                                                       //
            .Get()                                                                   //
    );                                                                               //
}

/**
 * @brief 更新 floating toolbar 窗口的中英文切换状态
 *
 * @param webview
 * @param cnEnState 1: 中文, 0: 英文
 */
void UpdateFtbCnEnState(ComPtr<ICoreWebView2> webview, int cnEnState)
{
    if (UpdateBinaryState(cnEnState, floatingToolbarState.cn_en))
    {
        RenderFloatingToolbarState(webview.Get());
    }
}

/**
 * @brief 更新 floating toolbar 窗口的中英文切换状态和标点切换状态
 *
 * @param webview
 * @param cnEnState 1: 中文, 0: 英文
 * @param puncState 1: 中文标点, 0: 英文标点
 */
void UpdateFtbCnEnAndPuncState(ComPtr<ICoreWebView2> webview, int cnEnState, int puncState)
{
    bool changed = UpdateBinaryState(cnEnState, floatingToolbarState.cn_en);
    changed |= UpdateBinaryState(puncState, floatingToolbarState.punctuation);
    if (changed)
    {
        RenderFloatingToolbarState(webview.Get());
        RevealAutoHiddenFloatingToolbar(L"input-state-changed");
    }
}

/**
 * @brief 更新 floating toolbar 窗口的中英文切换状态和标点切换状态
 *
 * @param webview
 * @param cnEnState 1: 中文, 0: 英文
 * @param doubleSingleByteState 1: 全角, 0: 半角
 * @param puncState 1: 中文标点, 0: 英文标点
 */
void UpdateFtbCnEnAndDoubleSingleAndPuncState( //
    ComPtr<ICoreWebView2> webview,             //
    int cnEnState,                             //
    int doubleSingleByteState,                 //
    int puncState,                             //
    int capsLockState                          //
)
{
    bool changed = UpdateBinaryState(cnEnState, floatingToolbarState.cn_en);
    changed |= UpdateBinaryState(doubleSingleByteState, floatingToolbarState.double_single_byte);
    changed |= UpdateBinaryState(puncState, floatingToolbarState.punctuation);
    changed |= UpdateBinaryState(capsLockState, floatingToolbarState.caps_lock);
    if (changed)
    {
        RenderFloatingToolbarState(webview.Get());
        RevealAutoHiddenFloatingToolbar(L"input-state-changed");
    }
}

/**
 * @brief 更新 floating toolbar 窗口的标点切换状态
 *
 * @param webview
 * @param puncState 1: 中文标点, 0: 英文标点
 */
void UpdateFtbPuncState(ComPtr<ICoreWebView2> webview, int puncState)
{
    if (UpdateBinaryState(puncState, floatingToolbarState.punctuation))
    {
        RenderFloatingToolbarState(webview.Get());
        RevealAutoHiddenFloatingToolbar(L"input-state-changed");
    }
}

/**
 * @brief 更新 floating toolbar 窗口的全角和半角状态
 *
 * @param webview
 * @param doubleSingleByteState 0: 半角, 1: 全角
 */
void UpdateFtbDoubleSingleByteState(ComPtr<ICoreWebView2> webview, int doubleSingleByteState)
{
    if (UpdateBinaryState(doubleSingleByteState, floatingToolbarState.double_single_byte))
    {
        RenderFloatingToolbarState(webview.Get());
        RevealAutoHiddenFloatingToolbar(L"input-state-changed");
    }
}

void UpdateFtbEnglishInputModeState(ComPtr<ICoreWebView2> webview, int enabled)
{
    if (UpdateBinaryState(enabled, floatingToolbarState.english_input_mode))
    {
        RenderFloatingToolbarState(webview.Get());
        RevealAutoHiddenFloatingToolbar(L"input-state-changed");
    }
}

void UpdateFtbCapsLockState(ComPtr<ICoreWebView2> webview, int enabled)
{
    if (UpdateBinaryState(enabled, floatingToolbarState.caps_lock))
    {
        RenderFloatingToolbarState(webview.Get());
        RevealAutoHiddenFloatingToolbar(L"input-state-changed");
    }
}

void UpdateFtbCharacterSetState(ComPtr<ICoreWebView2> webview)
{
    RenderFloatingToolbarState(webview.Get());
    RevealAutoHiddenFloatingToolbar(L"input-state-changed");
}

void UpdateFtbInputModeState(ComPtr<ICoreWebView2> webview, int japaneseMode)
{
    if (japaneseMode != 0 && japaneseMode != 1)
    {
        return;
    }
    const bool changed = floatingToolbarState.japanese_input_mode != japaneseMode;
    floatingToolbarState.japanese_input_mode = japaneseMode;
    RenderFloatingToolbarState(webview.Get());
    if (changed)
    {
        RevealAutoHiddenFloatingToolbar(L"input-state-changed");
    }
}

void ApplyConfiguredFloatingToolbarItems()
{
    RenderFloatingToolbarState(::webviewFtbWnd.Get());
    ApplyConfiguredFloatingToolbarSize();
}
