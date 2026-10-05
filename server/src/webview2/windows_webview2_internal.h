#pragma once

// windows_webview2*.cpp 之间共享的内部声明：拆分前同在 windows_webview2.cpp 里、现在跨文件使用的状态、宏与函数。
// 只给 server/src/webview2/windows_webview2*.cpp 包含，其他地方不要引用。

#include <windows.h>
#include <boost/json.hpp>
#include <cstdint>
#include <string>
#include "windows_webview2.h"
#include "log/candidate_diag_log.h"
#include "log/ftb_diag_log.h"

// WebView diagnostics were useful while fixing the rendering issues, but they
// overwhelm the input-latency trace. Keep these call sites compiled out.
// The disabled macro still has to *mention* its arguments, otherwise every HRESULT and parameter
// that exists only to be logged looks unreferenced (C4189/C4100). sizeof keeps them in an
// unevaluated context, so nothing is computed and no side effect runs — only the name is used.
template <typename... Args> int DiscardDiagLogArgs(const Args &...);
#undef DIAG_LOGF
#define DIAG_LOGF(...) ((void)sizeof(DiscardDiagLogArgs(__VA_ARGS__)))
#define CAND_WEBVIEW_TRACE_LOGF(...)                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (::DiagnosticLog::IsEnabled())                                                                              \
        {                                                                                                              \
            ::DiagnosticLog::Write(fmt::format(__VA_ARGS__));                                                          \
        }                                                                                                              \
    } while (0)

namespace json = boost::json;

int FineTuneWindow(HWND hwnd);
void ApplyConfiguredFloatingToolbarVisibility(const wchar_t *reason);
void ApplyConfiguredFloatingToolbarSize();
void ReconcileFloatingToolbarVisibilityAfterReady(const wchar_t *reason);
void RestartFloatingToolbarAutoHide(const wchar_t *reason);
void RevealAutoHiddenFloatingToolbar(const wchar_t *reason);
void ApplyConfiguredInputScheme();
void ApplyConfiguredShuangpinSchema();
bool EnsureSmallWindowsTopmost(const wchar_t *reason);
void UpdateSmallWindowWebviewVisibility(HWND hwnd, bool visible);
void SetCandidateHostCloaked(bool cloaked);
void ClearFloatingToolbarNavigationState();
std::wstring DescribeTrayMenuHostState();
std::wstring DescribeCandidateHostState();
std::wstring GetAppdataPath();
HRESULT OnEnvironmentCreated(HWND hwnd, HRESULT result, ICoreWebView2Environment *env);
HRESULT OnMenuWindowEnvironmentCreated(HWND hwnd, HRESULT result, ICoreWebView2Environment *env);
HRESULT OnFtbWindowEnvironmentCreated(HWND hwnd, HRESULT result, ICoreWebView2Environment *env);

extern std::string loadedCandidateSkin;
extern std::string loadedFloatingToolbarSkin;
extern std::string preparedCandidateSkin;
extern uint64_t candidateSkinReloadRevision;

namespace windows_webview2_detail
{
constexpr double kTruncationSizeFactor = 1.2;

extern bool floatingToolbarNavigationReady;
extern bool floatingToolbarPaintGraceActive;
extern bool floatingToolbarNavigationRetryUsed;
extern ULONGLONG g_last_content_truncation_ftb_ms;
extern ULONGLONG g_last_content_truncation_menu_ms;
extern ULONGLONG g_last_content_truncation_cand_ms;
extern bool candidateNavigationReady;
extern bool menuNavigationReady;

double JsonNumberAsDouble(const json::value &value);
bool HandleContentTruncatedMessage(HWND hwnd, ICoreWebView2 *webview, ICoreWebView2Controller *controller,
                                   const json::value &val, ULONGLONG &cooldownSlot, int extraShadowDip);
void OnSmallWindowControllerSettled(HRESULT hr);
void NotifySmallWindowNavigationReady(bool &readyFlag, const wchar_t *which);
} // namespace windows_webview2_detail
