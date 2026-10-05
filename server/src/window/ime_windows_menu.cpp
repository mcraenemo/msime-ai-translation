// 托盘语言栏菜单宿主：按页面测得的 DIP 与当前 DPI 换算宿主尺寸，以及 WndProcMenuWindow。
#include "window/ime_windows_internal.h"
#include "global/globals.h"
#include "config/ime_config.h"
#include "ipc/ipc.h"
#include "ime_windows.h"
#include "window/tray_menu_presenter.h"
#include "window/tray_menu_placement.h"
#include "defines/defines.h"
#include "defines/globals.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include "webview2/windows_webview2.h"
#include "utils/webview_utils.h"
#include "utils/window_utils.h"
#include "window_hook.h"
#include "log/ftb_diag_log.h"

using namespace ime_windows_detail;

namespace
{
RECT g_trayMenuAnchor{};

POINT MenuAnchorCenter()
{
    return {g_trayMenuAnchor.left + (g_trayMenuAnchor.right - g_trayMenuAnchor.left) / 2,
            g_trayMenuAnchor.top + (g_trayMenuAnchor.bottom - g_trayMenuAnchor.top) / 2};
}

POINT MenuPosition()
{
    const RECT content{0, 0, ::MENU_WINDOW_WIDTH, ::MENU_WINDOW_HEIGHT};
    return FanyImeUi::TrayMenuPosition(g_trayMenuAnchor, content, GetMonitorCoordinatesFromPoint(MenuAnchorCenter()));
}

// A small CSS-DIP reserve absorbs fractional line-height/border rounding in
// Chromium. Because it is converted with the target monitor's DPI, this becomes
// 2/3/4 physical pixels at 100%/150%/200% instead of being scale-dependent.
constexpr double kMenuViewportSafetyDip = 2.0;

void UpdateMenuPhysicalSizeCache(HWND hwnd, FLOAT scale)
{
    if (!hwnd)
    {
        return;
    }
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }
    HalfScreenDipLimits limits = QueryHalfScreenDipLimitsForPoint(MenuAnchorCenter());
    // During WM_DPICHANGED, GetDpiForWindow can still expose the old DPI. Derive
    // the DIP budget from the message's scale so cap and pixel conversion agree.
    const double monitorWidthPx = static_cast<double>((std::max)(1, limits.monitor.right - limits.monitor.left));
    const double monitorHeightPx = static_cast<double>((std::max)(1, limits.monitor.bottom - limits.monitor.top));
    limits.scale = scale;
    limits.maxWidthDip = (monitorWidthPx * 0.5) / static_cast<double>(scale);
    limits.maxHeightDip = (monitorHeightPx * 0.5) / static_cast<double>(scale);
    ::MENU_CONTENT_WIDTH_DIP = ClampWidthDipToHalfScreen(::MENU_CONTENT_WIDTH_DIP, limits);
    ::MENU_CONTENT_HEIGHT_DIP = ClampHeightDipToHalfScreen(::MENU_CONTENT_HEIGHT_DIP, limits);
    const double hostWidthDip = ClampWidthDipToHalfScreen(::MENU_CONTENT_WIDTH_DIP + kMenuViewportSafetyDip, limits);
    const double hostHeightDip = ClampHeightDipToHalfScreen(::MENU_CONTENT_HEIGHT_DIP + kMenuViewportSafetyDip, limits);
    ::SCALE = scale;
    ::MENU_WINDOW_WIDTH = static_cast<int>(std::ceil(hostWidthDip * scale));
    ::MENU_WINDOW_HEIGHT = static_cast<int>(std::ceil(hostHeightDip * scale));
}

// Recompute menu host pixels from last measured CSS DIPs * scale.
void ApplyMenuPhysicalSizeFromDips(HWND hwnd, FLOAT scale, UINT flags)
{
    UpdateMenuPhysicalSizeCache(hwnd, scale);
    POINT position{};
    if (IsWindowVisible(hwnd))
    {
        position = MenuPosition();
        flags &= ~SWP_NOMOVE;
    }
    SetWindowPos(hwnd, nullptr, position.x, position.y, ::MENU_WINDOW_WIDTH, ::MENU_WINDOW_HEIGHT, flags);
    SyncHostWebViewBounds(::webviewControllerMenuWnd.Get(), hwnd);
}
} // namespace

RECT GetTrayMenuAnchorRect()
{
    return g_trayMenuAnchor;
}

LRESULT CALLBACK WndProcMenuWindow(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (TrayMenuPresenter::Instance().HandleMessage(message, wParam, lParam))
    {
        return message == WM_ERASEBKGND ? 1 : 0;
    }

    switch (message)
    {
    case WM_MOUSEACTIVATE:
        // Same contract as the candidate / floating-toolbar hosts: the tray
        // language-bar menu must never take foreground. Stealing activation
        // drops the focused app's TSF document and forces a full IME
        // disconnect/reconnect on every right-click.
        return MA_NOACTIVATE;

    case WM_ACTIVATE: {
        if (LOWORD(wParam) != WA_INACTIVE)
        {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        break;
    }

    case WM_LANGBAR_RIGHTCLICK: {
        // A fresh right-click carries the icon rect; replayed deferred shows post
        // lParam 0 and keep the anchor captured by that right-click.
        if (std::unique_ptr<RECT> icon{reinterpret_cast<RECT *>(lParam)})
        {
            g_trayMenuAnchor = *icon;
        }
        if (TrayMenuPresenter::Instance().IsBound())
        {
            TrayMenuPresenter::Instance().ShowFromLangBar();
            SetHostWindowCloaked(::global_hwnd_menu, false);
            if (!g_mouseHook)
            {
                g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, nullptr, 0);
            }
            break;
        }
        // Host HWND alone is an invisible click-blocker. Do not show it until the
        // menu WebView controller exists (watchdog relaunch / early logon can
        // leave init pending; Prepare queues a retry and re-posts this message).
        if (!PrepareTrayMenuWebviewForShow())
        {
            FTB_DIAG_LOGF(L"menu show deferred: webview not ready yet");
            break;
        }
        FTB_DIAG_LOGF(L"menu show begin {}", DescribeTrayMenuHostState());
        // Preserve WebView text scaling while targeting the icon's monitor
        const FLOAT scale = QueryCandidateHalfScreenDipLimitsForPoint(hwnd, MenuAnchorCenter()).scale;
        ::SCALE = scale;
        if (::MENU_CONTENT_WIDTH_DIP <= 0.0)
        {
            ::MENU_CONTENT_WIDTH_DIP = 200.0;
        }
        if (::MENU_CONTENT_HEIGHT_DIP <= 0.0)
        {
            ::MENU_CONTENT_HEIGHT_DIP = 300.0;
        }
        UpdateMenuPhysicalSizeCache(hwnd, scale);
        const POINT position = MenuPosition();
        EnsureSmallWindowsTopmost(L"show-menu");
        // Host can appear before WebView paints; pending topmost/content refresh
        // runs when navigations complete. Pass cached physical size (kept current
        // by measure / WM_DPICHANGED) instead of SWP_NOSIZE so a stale HWND from
        // a prior display scale cannot clip the menu.
        //
        // Stay DWM-cloaked until Z-order is final: Ensure often pins FTB last
        // because the menu is still hidden at request time; raising afterward
        // while cloaked avoids the cover→uncover flicker.
        //
        // Never SetForegroundWindow here: WS_EX_NOACTIVATE + SWP_NOACTIVATE keep
        // the focused app's IME session alive; click-outside still comes from
        // the low-level mouse hook.
        UINT flag = SWP_SHOWWINDOW | SWP_NOACTIVATE;
        const HWND zorder = AreSmallWindowsTopmostApplied() ? HWND_TOPMOST : HWND_TOP;
        SetLastError(0);
        BOOL okShowMenu = SetWindowPos( //
            ::global_hwnd_menu,         //
            zorder,                     //
            position.x,                 //
            position.y,                 //
            ::MENU_WINDOW_WIDTH,        //
            ::MENU_WINDOW_HEIGHT,       //
            flag                        //
        );
        (void)0;
        if (!okShowMenu)
        {
            ShowWindow(::global_hwnd_menu, SW_SHOWNOACTIVATE);
        }
        RaiseTrayMenuAboveSmallWindows(L"show-menu");
        SetHostWindowCloaked(::global_hwnd_menu, false);
        if (::webviewControllerMenuWnd)
        {
            RECT bounds{};
            GetClientRect(hwnd, &bounds);
            ::webviewControllerMenuWnd->put_Bounds(bounds);
            ::webviewControllerMenuWnd->NotifyParentWindowPositionChanged();
            UpdateSmallWindowWebviewVisibility(hwnd, true);
        }
        // The state that decides whether the user sees anything. cloaked=no with
        // a sized visible child and wv_visible=true means the menu should be on
        // screen; if it is not, the controller is compositing against a stale
        // parent and nothing in this handler can be blamed.
        FTB_DIAG_LOGF(L"menu show end   pos=({},{}) size={}x{} zorder={} {}", position.x, position.y,
                      ::MENU_WINDOW_WIDTH, ::MENU_WINDOW_HEIGHT, zorder == HWND_TOPMOST ? L"topmost" : L"top",
                      DescribeTrayMenuHostState());
        // Refresh before paint so the toggle matches Settings / config.toml.
        SyncMenuFloatingToolbarToggle();
        /* 安装鼠标钩子 */
        if (!g_mouseHook)
        {
            g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, nullptr, 0);
        }
        break;
    }

    case WM_DPICHANGED: {
        if (TrayMenuPresenter::Instance().IsBound())
        {
            if (TrayMenuPresenter::Instance().IsOpenToUser())
            {
                TrayMenuPresenter::Instance().ShowFromLangBar();
            }
            return 0;
        }
        // Prefer dip * newScale over the suggested rect alone: the menu host is
        // content-sized, and a stale/oversized create-time rect would otherwise
        // keep the wrong physical size across display-scale changes.
        const FLOAT nativeScale = GetWindowScale(hwnd);
        const FLOAT rasterScale = GetWebViewRasterizationScale(hwnd);
        const FLOAT textScale = nativeScale > 0.0f ? rasterScale / nativeScale : 1.0f;
        const FLOAT scale = (HIWORD(wParam) / 96.0f) * textScale;
        const auto *suggested = reinterpret_cast<const RECT *>(lParam);
        UpdateMenuPhysicalSizeCache(hwnd, scale);
        if (suggested)
        {
            const POINT position = IsWindowVisible(hwnd) ? MenuPosition() : POINT{suggested->left, suggested->top};
            SetWindowPos(hwnd, nullptr, position.x, position.y, ::MENU_WINDOW_WIDTH, ::MENU_WINDOW_HEIGHT,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        else
        {
            ApplyMenuPhysicalSizeFromDips(hwnd, scale, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        SyncHostWebViewBounds(::webviewControllerMenuWnd.Get(), hwnd);
        (void)0;
        // Remeasure in case font/layout metrics shifted with the new DPI.
        SetTimer(hwnd, TIMER_ID_INIT_WEBVIEW_MENU, 1, nullptr);
        return 0;
    }

    case WM_SIZE: {
        // The menu is resized to match its HTML content after WebView startup.
        // Keep the controller surface aligned with the resized host HWND so it
        // can be hidden and shown repeatedly without losing its painted area.
        if (::webviewControllerMenuWnd && wParam != SIZE_MINIMIZED)
        {
            SyncHostWebViewBounds(::webviewControllerMenuWnd.Get(), hwnd);
        }
        break;
    }

    case WM_REFRESH_MENU_SIZE:
        SetTimer(hwnd, TIMER_ID_INIT_WEBVIEW_MENU, 1, nullptr);
        return 0;

    case WM_TIMER: {
        if (wParam == TIMER_ID_INIT_WEBVIEW_MENU)
        {
            KillTimer(hwnd, TIMER_ID_INIT_WEBVIEW_MENU);
            if (TrayMenuPresenter::Instance().IsBound() || UseD2dSmallWindowUi())
            {
                break;
            }
            if (::webviewMenuWnd) // 确保 webview 已初始化
            {
                GetContainerSizeMenu(webviewMenuWnd, [hwnd](std::pair<double, double> containerSize) {
                    if (hwnd == ::global_hwnd_menu)
                    {
                        if (containerSize.first > 1.0 && containerSize.second > 1.0)
                        {
                            ::MENU_CONTENT_WIDTH_DIP = containerSize.first;
                            ::MENU_CONTENT_HEIGHT_DIP = containerSize.second;
                        }
                        const bool wasVisible = IsWindowVisible(hwnd) != FALSE;
                        UINT flag = SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER;
                        flag |= wasVisible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
                        FLOAT scale = GetWebViewRasterizationScale(hwnd);
                        ApplyMenuPhysicalSizeFromDips(hwnd, scale, flag);
                        InjectSurfaceViewportLimits(::webviewMenuWnd.Get(), hwnd);
                        (void)0;
                    }
                });
            }
            else
            {
                // 如果 webview 还没准备好，再等一会
                SetTimer(hwnd, TIMER_ID_INIT_WEBVIEW_MENU, 100, nullptr);
            }
        }
        break;
    }
    default: {
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
    }
    return 0;
}
