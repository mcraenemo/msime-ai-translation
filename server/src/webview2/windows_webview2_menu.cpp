// 托盘语言栏菜单 WebView：controller 的创建、页面消息处理，以及与配置同步悬浮工具栏开关。
#include "webview2/windows_webview2_internal.h"
#include "engine/contracts/webview/validator.h"
#include "config/ime_config.h"
#include "defines/defines.h"
#include "defines/globals.h"
#include "settings/settings_launcher.h"
#include "utils/common_utils.h"
#include "utils/window_utils.h"
#include "voice-input/voice_input_service.h"
#include "window/tray_menu_presenter.h"
#include <algorithm>
#include <filesystem>
#include <string>

using namespace windows_webview2_detail;

//
//
// 菜单窗口 webview
//
//

/**
 * @brief Handle menu window webview2 controller creation
 *
 * @param hwnd
 * @param result
 * @param controller
 * @return HRESULT
 */
HRESULT OnControllerCreatedMenuWnd(     //
    HWND hwnd,                          //
    HRESULT result,                     //
    ICoreWebView2Controller *controller //
)
{
    if (!controller || FAILED(result))
    {
        FTB_DIAG_LOGF(L"menu controller creation failed hr={:#x}", static_cast<unsigned>(result));
        OnSmallWindowControllerSettled(FAILED(result) ? result : E_FAIL);
        return E_FAIL;
    }

    /* 给 controller 和 webview 赋值 */
    webviewControllerMenuWnd = controller;
    const HRESULT getMenuWebviewHr = webviewControllerMenuWnd->get_CoreWebView2(webviewMenuWnd.GetAddressOf());
    // A controller built against a host that is already topmost, or that is not
    // on a monitor, is the state that never recovers.
    FTB_DIAG_LOGF(L"menu controller created {}", DescribeTrayMenuHostState());

    if (!webviewMenuWnd)
    {
        webviewControllerMenuWnd.Reset();
        OnSmallWindowControllerSettled(FAILED(getMenuWebviewHr) ? getMenuWebviewHr : E_FAIL);
        return E_FAIL;
    }

    UpdateSmallWindowWebviewVisibility(hwnd, IsWindowVisible(hwnd) != FALSE);

    // Configure webviewMenuWindow settings
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webviewMenuWnd->get_Settings(&settings)))
    {
        settings->put_IsScriptEnabled(TRUE);
        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        settings->put_IsWebMessageEnabled(TRUE);
        settings->put_AreHostObjectsAllowed(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
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

    webviewControllerMenuWnd->put_ZoomFactor(1.0);

    // Configure virtual host path
    if (SUCCEEDED(webviewMenuWnd->QueryInterface(IID_PPV_ARGS(&webview3MenuWnd))))
    {
        const auto contractsPath =
            std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"html" / L"webview2" / L"shared";
        webview3MenuWnd->SetVirtualHostNameToFolderMapping(L"msime-contracts", contractsPath.wstring().c_str(),
                                                           COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);

        // Assets mapping
        webview3MenuWnd->SetVirtualHostNameToFolderMapping(  //
            L"appassets",                                    //
            GetLocalAssetsPath().c_str(),                    //
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS //
        );                                                   //
    }

    // Set transparent background
    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&webviewController2MenuWnd))))
    {
        COREWEBVIEW2_COLOR backgroundColor = {0, 0, 0, 0};
        webviewController2MenuWnd->put_DefaultBackgroundColor(backgroundColor);
    }

    // Adjust to window size
    RECT bounds;
    GetClientRect(hwnd, &bounds);
    webviewControllerMenuWnd->put_Bounds(bounds);

    EventRegistrationToken menuNavigationCompletedToken{};
    webviewMenuWnd->add_NavigationCompleted(
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [hwnd](ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                BOOL success = FALSE;
                if (!args || FAILED(args->get_IsSuccess(&success)) || !success)
                {
                    // PrepareTrayMenuWebviewForShow re-navigates over an in-flight
                    // navigation, which aborts the first one exactly like this. Only
                    // a run of these means the menu is never becoming ready.
                    FTB_DIAG_LOGF(L"menu navigation completed unsuccessfully");
                    return S_OK;
                }
                FTB_DIAG_LOGF(L"menu navigation completed {}", DescribeTrayMenuHostState());
                NotifySmallWindowNavigationReady(menuNavigationReady, L"menu");
                // Older menu assets already render the handwriting entry but do not
                // give it an id or native click bridge. Wire it at runtime so existing
                // installations gain the launcher without replacing their skin.
                sender->ExecuteScript(LR"((() => {
                        const item = [...document.querySelectorAll('.menu-item')]
                            .find(element => element.textContent.includes('\u624b\u5199\u8bc6\u522b\u677f'));
                        if (item && !item.dataset.nativeHandwritingLauncher) {
                            item.dataset.nativeHandwritingLauncher = 'true';
                            item.addEventListener('click', () => {
                                window.chrome.webview.postMessage(JSON.stringify({ type: 'handwritingPanel' }));
                            });
                        }
                    })())",
                                      nullptr);
                SyncMenuFloatingToolbarToggle();
                InjectSurfaceViewportLimits(sender, hwnd);
                PostMessage(hwnd, WM_REFRESH_MENU_SIZE, 0, 0);
                return S_OK;
            })
            .Get(),
        &menuNavigationCompletedToken);

    // Navigate to HTML
    webviewMenuWnd->NavigateToString(::HTMLStringMenuWnd.c_str());

    /* Debug console */
    // webviewMenuWindow->OpenDevToolsWindow();

    webviewMenuWnd->add_WebMessageReceived(
        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [hwnd](ICoreWebView2 * /*sender*/, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                wil::unique_cotaskmem_string message;
                HRESULT hr = args->TryGetWebMessageAsString(&message);

                if (SUCCEEDED(hr) && message.get())
                {
                    std::wstring msg(message.get());
                    // 解析 msg，执行相应操作
                    json::value val = json::parse(wstring_to_string(msg));
                    if (!metasequoia::webview::Validate(val, "client", "menu"))
                        return S_OK;
                    std::string type = json::value_to<std::string>(val.at("type"));
                    if (type == "floatingToggle")
                    {
                        bool needShown = json::value_to<bool>(val.at("data"));
                        if (SetConfiguredFloatingToolbarEnabled(needShown))
                        {
                            ApplyConfiguredFloatingToolbarVisibility(L"tray-menu-toggle");
                            PostSettingsConfig();
                        }
                    }
                    else if (type == "settings")
                    {
                        OpenSettingsApplication();
                        ShowWindow(::global_hwnd_menu, SW_HIDE);
                    }
                    else if (type == "about")
                    {
                        OpenSettingsAboutApplication();
                        ShowWindow(::global_hwnd_menu, SW_HIDE);
                    }
                    else if (type == "emojiSymbols")
                    {
                        OpenEmojiPanelApplication();
                        ShowWindow(::global_hwnd_menu, SW_HIDE);
                    }
                    else if (type == "keyboardPanel")
                    {
                        OpenKeyboardPanelApplication();
                        ShowWindow(::global_hwnd_menu, SW_HIDE);
                    }
                    else if (type == "handwritingPanel")
                    {
                        OpenHandwritingPanelApplication();
                        ShowWindow(::global_hwnd_menu, SW_HIDE);
                    }
                    else if (type == "voiceInput")
                    {
                        VoiceInput::ToggleRecording();
                        ShowWindow(::global_hwnd_menu, SW_HIDE);
                    }
                    else if (type == "contentTruncated")
                    {
                        if (HandleContentTruncatedMessage(hwnd, webviewMenuWnd.Get(), webviewControllerMenuWnd.Get(),
                                                          val, g_last_content_truncation_menu_ms, 0))
                        {
                            ::MENU_CONTENT_WIDTH_DIP =
                                (std::max)(::MENU_CONTENT_WIDTH_DIP, JsonNumberAsDouble(val.at("data").at("width")));
                            ::MENU_CONTENT_HEIGHT_DIP =
                                (std::max)(::MENU_CONTENT_HEIGHT_DIP, JsonNumberAsDouble(val.at("data").at("height")));
                            const HalfScreenDipLimits limits = QueryWebViewHalfScreenDipLimitsForHwnd(hwnd);
                            ::MENU_CONTENT_WIDTH_DIP = ClampWidthDipToHalfScreen(::MENU_CONTENT_WIDTH_DIP, limits);
                            ::MENU_CONTENT_HEIGHT_DIP = ClampHeightDipToHalfScreen(::MENU_CONTENT_HEIGHT_DIP, limits);
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
 * @brief Handle menu window webview2 environment creation
 *
 * @param hwnd
 * @param result
 * @param env
 * @return HRESULT
 */
HRESULT OnMenuWindowEnvironmentCreated(HWND hwnd, HRESULT result, ICoreWebView2Environment *env)
{
    if (FAILED(result) || !env)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return result;
    }

    // Create WebView2 controller
    return env->CreateCoreWebView2Controller(                                                //
        hwnd,                                                                                //
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>( //
            [hwnd](HRESULT result, ICoreWebView2Controller *controller) -> HRESULT {         //
                return OnControllerCreatedMenuWnd(hwnd, result, controller);                 //
            })                                                                               //
            .Get()                                                                           //
    );                                                                                       //
}

/**
 * @brief Keep tray-menu floating-toolbar toggle aligned with config.toml.
 */
void SyncMenuFloatingToolbarToggle()
{
    if (TrayMenuPresenter::Instance().IsBound())
    {
        if (TrayMenuPresenter::Instance().IsOpenToUser())
        {
            TrayMenuPresenter::Instance().ShowFromLangBar();
        }
        else
        {
            TrayMenuPresenter::Instance().ApplyTheme();
        }
        return;
    }
    if (!::webviewMenuWnd)
    {
        return;
    }

    // Keep the tray-menu toggle aligned with config.toml so Settings and tray
    // never drift apart when either side writes general.floating_toolbar.
    const wchar_t *script = GetConfiguredFloatingToolbarEnabled() ? LR"((() => {
                                      const toggle = document.getElementById('floatingToggle');
                                      if (toggle) toggle.classList.add('active');
                                  })())"
                                                                  : LR"((() => {
                                      const toggle = document.getElementById('floatingToggle');
                                      if (toggle) toggle.classList.remove('active');
                                  })())";
    ::webviewMenuWnd->ExecuteScript(script, nullptr);
}
