#include "reading_translation.h"
#include "ai_translation.h"
#include "browser/reading_bridge.h"
#include <UIAutomation.h>
#include <wrl/client.h>
#include <algorithm>
#include <condition_variable>
#include <thread>
#include <utility>
#include <vector>

namespace ReadingTranslation
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr UINT CaptureMessage = WM_APP + 302, ResultMessage = WM_APP + 303;
std::thread ui_thread, capture_thread, hook_thread;
std::atomic<int> hook_count{0};
constexpr UINT MouseMessage = WM_APP + 306;
bool capture_reset = false;
std::atomic<bool> running{false};
std::atomic<uint64_t> selection_epoch{0};
AiTranslation::Worker worker;
HHOOK mouse_hook = nullptr, keyboard_hook = nullptr;
HWINEVENTHOOK foreground_hook = nullptr;
HWND popup = nullptr;
std::mutex capture_mutex, result_mutex;
std::condition_variable capture_cv;
struct Selection
{
    POINT point{};
    HWND foreground = nullptr;
    uint64_t generation = 0, mode = 0;
    bool show_errors = false;
};
Selection pending;
bool capture_pending = false, show_capture_errors = false, action_menu = false;
HWND action_foreground = nullptr;
std::vector<AiTranslation::Result> results;
std::wstring source_display, translated_display;
std::string copy_text, last_identity, last_translation;
bool last_finished = false;
uint64_t last_time = 0, request_generation = 0, request_selection = 0, request_mode = 0;
POINT anchor{};
int scroll = 0;
std::wstring Wide(const std::string &text)
{
    const int n =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(n, 0);
    if (n)
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(),
                            n);
    return result;
}
std::string Utf8(const std::wstring &text)
{
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string result(n, 0);
    if (n)
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), n,
                            nullptr, nullptr);
    return result;
}
bool Current(const Selection &s)
{
    bool foreground = GetForegroundWindow() == s.foreground;
#ifdef MSIME_READING_TESTING
    foreground = true; // Isolated owned controls are tested without stealing the user's keyboard focus.
#endif
    return running && enabled && s.generation == selection_epoch && s.mode == epoch && foreground &&
           !(GetAsyncKeyState(VK_LBUTTON) & 0x8000);
}
void CALLBACK Foreground(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD)
{
    if (enabled && host.load())
        PostMessageW(host.load(), CaptureMessage, 2, 0);
}
void Hide(bool cancel = true)
{
    if (cancel)
    {
        worker.Cancel();
        last_identity.clear();
        last_translation.clear();
        last_finished = false;
    }
    action_menu = false;
    ++selection_epoch;
    KillTimer(popup, 1);
    KillTimer(popup, 2);
    ShowWindow(popup, SW_HIDE);
    source_display.clear();
    translated_display.clear();
    copy_text.clear();
}
bool OwnPoint(POINT point)
{
    HWND window = WindowFromPoint(point);
    DWORD process = 0;
    if (window)
        GetWindowThreadProcessId(window, &process);
#ifdef MSIME_READING_TESTING
    return false; // The isolated test fixture shares this process, unlike real applications.
#else
    return process == GetCurrentProcessId();
#endif
}
HWND SelectionForeground(POINT point)
{
#ifdef MSIME_READING_TESTING
    return GetAncestor(WindowFromPoint(point), GA_ROOT);
#else
    return GetForegroundWindow();
#endif
}
bool BrowserWindow(HWND window)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    wchar_t path[32768]{};
    DWORD length = 32768;
    bool browser = false;
    if (QueryFullProcessImageNameW(process, 0, path, &length))
    {
        auto *name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        browser = !_wcsicmp(name, L"chrome.exe") || !_wcsicmp(name, L"msedge.exe") || !_wcsicmp(name, L"brave.exe") ||
                  !_wcsicmp(name, L"opera.exe") || !_wcsicmp(name, L"vivaldi.exe");
    }
    CloseHandle(process);
    return browser;
}
LRESULT CALLBACK Mouse(int code, WPARAM message, LPARAM data)
{
    if (code >= 0 && enabled && (message == WM_RBUTTONUP))
    {
        const auto &info = *reinterpret_cast<MSLLHOOKSTRUCT *>(data);
        const auto packed = static_cast<uint64_t>(static_cast<uint32_t>(info.pt.x)) |
                            (static_cast<uint64_t>(static_cast<uint32_t>(info.pt.y)) << 32);
        if (host.load())
            PostMessageW(host.load(), MouseMessage, message, static_cast<LPARAM>(packed));
    }
    return CallNextHookEx(nullptr, code, message, data);
}
LRESULT CALLBACK Keyboard(int code, WPARAM message, LPARAM data)
{
    if (code >= 0 && enabled && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
        reinterpret_cast<KBDLLHOOKSTRUCT *>(data)->vkCode == VK_ESCAPE)
        if (host.load())
            PostMessageW(host.load(), CaptureMessage, 2, 0);
    return CallNextHookEx(nullptr, code, message, data);
}
// Fail closed if a provider cannot report the password flag. Check ancestors too.
bool SafeElement(IUIAutomation *automation, IUIAutomationElement *element, const Selection &selected, uint64_t deadline)
{
    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(automation->get_RawViewWalker(&walker)))
        return false;
    ComPtr<IUIAutomationElement> current = element;
    for (int depth = 0; current && depth < 20; ++depth)
    {
        if (!Current(selected) || GetTickCount64() > deadline)
            return false;
        BOOL password = TRUE;
        if (FAILED(current->get_CurrentIsPassword(&password)) || password)
            return false;
        UIA_HWND native = nullptr;
        if (SUCCEEDED(current->get_CurrentNativeWindowHandle(&native)) && native &&
            (GetWindowLongPtrW(static_cast<HWND>(native), GWL_STYLE) & ES_PASSWORD))
        {
            wchar_t name[32]{};
            GetClassNameW(static_cast<HWND>(native), name, 32);
            if (_wcsicmp(name, L"Edit") == 0 || wcsstr(name, L"RichEdit") || wcsstr(name, L"RICHEDIT"))
                return false;
        }
        if (native && static_cast<HWND>(native) == selected.foreground)
            return true;
        ComPtr<IUIAutomationElement> parent;
        if (FAILED(walker->GetParentElement(current.Get(), &parent)))
            return false;
        current = std::move(parent);
    }
    return !current;
}
std::wstring PatternSelection(IUIAutomationElement *element, POINT point)
{
    ComPtr<IUIAutomationTextPattern> pattern;
    if (!element || FAILED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&pattern))) || !pattern)
        return {};
    ComPtr<IUIAutomationTextRangeArray> ranges;
    int count = 0;
    if (FAILED(pattern->GetSelection(&ranges)) || !ranges || FAILED(ranges->get_Length(&count)) || count != 1)
        return {};
    ComPtr<IUIAutomationTextRange> range;
    if (FAILED(ranges->GetElement(0, &range)) || !range)
        return {};
    SAFEARRAY *rects = nullptr;
    bool selection_near = false;
    if (SUCCEEDED(range->GetBoundingRectangles(&rects)) && rects)
    {
        LONG lower = 0, upper = -1;
        SafeArrayGetLBound(rects, 1, &lower);
        SafeArrayGetUBound(rects, 1, &upper);
        double *values = nullptr;
        if (SUCCEEDED(SafeArrayAccessData(rects, reinterpret_cast<void **>(&values))))
        {
            for (LONG i = 0; i + 3 <= upper - lower; i += 4)
                if (point.x >= values[i] - 64 && point.x <= values[i] + values[i + 2] + 64 &&
                    point.y >= values[i + 1] - 64 && point.y <= values[i + 1] + values[i + 3] + 64)
                    selection_near = true;
            SafeArrayUnaccessData(rects);
        }
        SafeArrayDestroy(rects);
    }
    if (!selection_near)
        return {}; // Never translate a stale selection elsewhere in the document.
    BSTR text = nullptr;
    if (FAILED(range->GetText(8193, &text)) || !text)
        return {};
    std::wstring result(text, SysStringLen(text));
    SysFreeString(text);
    if (result.size() > 8192)
        return {};
    return result;
}
std::wstring NativeEditSelection(POINT point)
{
    HWND window = WindowFromPoint(point);
    wchar_t name[32]{};
    GetClassNameW(window, name, 32);
    // Classic Unicode Edit fallback; no simulated Copy and no clipboard access.
    if (_wcsicmp(name, L"Edit") || !IsWindowUnicode(window) || (GetWindowLongPtrW(window, GWL_STYLE) & ES_PASSWORD))
        return {};
    DWORD first = 0, last = 0;
    DWORD_PTR out = 0;
    if (!SendMessageTimeoutW(window, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last),
                             SMTO_ABORTIFHUNG, 100, &out) ||
        last <= first || last - first > 8192)
        return {};
    if (!SendMessageTimeoutW(window, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, 100, &out) || out > 65536)
        return {};
    std::wstring text(static_cast<size_t>(out) + 1, 0);
    if (!SendMessageTimeoutW(window, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()), SMTO_ABORTIFHUNG,
                             100, &out) ||
        last > out)
        return {};
    return text.substr(first, last - first);
}
void CaptureLoop()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomation2> timed;
    while (running)
    {
        Selection selected;
        {
            std::unique_lock lock(capture_mutex);
            capture_cv.wait(lock, [] { return !running || capture_pending || capture_reset; });
            if (!running)
                break;
            if (capture_reset)
            {
                capture_reset = false;
                timed.Reset();
                automation.Reset();
                continue;
            }
            selected = pending;
            capture_pending = false;
        }
        if (!Current(selected) || OwnPoint(selected.point))
            continue;
        if (!automation)
        {
            if (FAILED(
                    CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))))
                CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
            if (automation && SUCCEEDED(automation.As(&timed)))
            {
                timed->put_ConnectionTimeout(1000);
                timed->put_TransactionTimeout(500);
            }
        }
        if (!automation || !Current(selected))
            continue;
        const uint64_t deadline = GetTickCount64() + 2500;
        ComPtr<IUIAutomationElement> element;
        std::wstring text;
        if (SUCCEEDED(automation->ElementFromPoint(selected.point, &element)) && element &&
            SafeElement(automation.Get(), element.Get(), selected, deadline))
        {
            ComPtr<IUIAutomationTreeWalker> walker;
            automation->get_RawViewWalker(&walker);
            ComPtr<IUIAutomationElement> current = element;
            for (int i = 0; current && i < 10 && Current(selected) && GetTickCount64() <= deadline; ++i)
            {
                text = PatternSelection(current.Get(), selected.point);
                if (!text.empty())
                    break;
                ComPtr<IUIAutomationElement> parent;
                if (!walker || FAILED(walker->GetParentElement(current.Get(), &parent)))
                    break;
                current = std::move(parent);
            }
            if (text.empty() && Current(selected))
                text = NativeEditSelection(selected.point);
        }
        if (!Current(selected))
            continue;
        auto bytes = Utf8(text);
        if (!AiTranslation::IsReadingSource(bytes))
        {
            if (selected.show_errors && Current(selected))
                PostMessageW(popup, WM_APP + 308, bytes.empty() ? 1 : 0, static_cast<LPARAM>(selected.generation));
            continue;
        }
        auto config = Snapshot();
        if (!config.translation_enabled || selected.mode != epoch)
            continue;
        AiTranslation::Request request;
        request.config = std::move(config);
        request.source = std::move(bytes);
        request.target = request.config.reading_target_language;
        request.reading = true;
        // Use the existing source representation for Chinese, without a direction-specific cache namespace.
        request.character_set = AiTranslation::IsChineseSource(request.source) ? "simplified" : "auto";
        request.activation_epoch = selected.mode;
        request.client_id = selected.generation;
        {
            std::lock_guard lock(result_mutex);
            AiTranslation::Result captured;
            captured.request = std::move(request);
            results.push_back(std::move(captured));
        }
        PostMessageW(popup, ResultMessage, 0, 0);
    }
    timed.Reset();
    automation.Reset();
    CoUninitialize();
}
int Scale(int value, UINT dpi)
{
    return MulDiv(value, dpi, 96);
}
void Show(POINT point)
{
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    UINT dpi = GetDpiForWindow(popup);
    int width = std::min<int>(Scale(520, dpi), info.rcWork.right - info.rcWork.left);
    int height = std::min<int>(Scale(action_menu ? 48 : 360, dpi), info.rcWork.bottom - info.rcWork.top);
    if (action_menu)
        width = std::min<int>(Scale(230, dpi), info.rcWork.right - info.rcWork.left);
    int x = std::clamp<int>(static_cast<int>(point.x), info.rcWork.left, info.rcWork.right - width);
    int y = std::clamp<int>(static_cast<int>(point.y) + Scale(20, dpi), info.rcWork.top, info.rcWork.bottom - height);
    SetWindowPos(popup, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    scroll = 0;
    InvalidateRect(popup, nullptr, TRUE);
}
void Copy()
{
    if (copy_text.empty() || !OpenClipboard(popup))
        return;
    auto text = Wide(copy_text);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (data)
    {
        void *memory = GlobalLock(data);
        if (memory)
        {
            memcpy(memory, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(data);
            if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, data))
                GlobalFree(data);
        }
        else
            GlobalFree(data);
    }
    CloseClipboard();
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
#ifdef MSIME_READING_TESTING
    if (message == WM_APP + 307)
    {
        Sleep(static_cast<DWORD>(wparam));
        return 0;
    }
    if (message == WM_APP + 304)
    {
        anchor = *reinterpret_cast<POINT *>(lparam);
        return WindowProc(window, CaptureMessage, 1, 0);
    }
    if (message == WM_APP + 305)
        return hook_count.load();
#endif
    if (message == WM_APP + 308)
    {
        if (!enabled || static_cast<uint64_t>(lparam) != selection_epoch)
            return 0;
        if (wparam)
        {
            source_display = L"无法读取选区";
            translated_display = L"当前控件没有提供安全可读取的选中文字，或读取超时。密码及安全输入框不会读取。";
            copy_text.clear();
            Show(anchor);
        }
        else
            ShowWindow(window, SW_HIDE);
        return 0;
    }
    if (message == MouseMessage)
    {
        const auto packed = static_cast<uint64_t>(lparam);
        POINT point{static_cast<LONG>(static_cast<uint32_t>(packed)),
                    static_cast<LONG>(static_cast<uint32_t>(packed >> 32))};
        if (!enabled)
            return 0;
        if (wparam == WM_RBUTTONUP && !OwnPoint(point))
        {
            Hide();
            anchor = point;
            action_foreground = SelectionForeground(point);
            // Only browsers use the extension; Electron desktop applications retain this entry.
            if (BrowserWindow(action_foreground))
                return 0;
            action_menu = true;
            Show(anchor);
            SetTimer(window, 2, 8000, nullptr);
        }
        return 0;
    }
    if (message == ConfigMessage)
    {
        Hide();
        BrowserReading::Cancel();
        if (hook_thread_id.load())
            PostThreadMessageW(hook_thread_id.load(), ConfigMessage, 0, 0);
        {
            std::lock_guard lock(capture_mutex);
            capture_pending = false;
            capture_reset = true;
        }
        capture_cv.notify_one();
        return 0;
    }
    if (message == CaptureMessage)
    {
        Hide(wparam == 2);
        if (wparam == 1 && enabled)
            SetTimer(window, 1, 350, nullptr);
        return 0;
    }
    if (message == WM_TIMER && wparam == 2)
    {
        KillTimer(window, 2);
        if (action_menu)
            Hide();
        return 0;
    }
    if (message == WM_TIMER && wparam == 1)
    {
        KillTimer(window, 1);
        if (!enabled || (GetAsyncKeyState(VK_LBUTTON) & 0x8000))
            return 0;
        std::lock_guard lock(capture_mutex);
        pending = {anchor, SelectionForeground(anchor), selection_epoch.load(), epoch.load(), show_capture_errors};
        capture_pending = true;
        if (show_capture_errors)
        {
            source_display = L"阅读翻译";
            translated_display = L"正在读取选区…";
            copy_text.clear();
            Show(anchor);
        }
        capture_cv.notify_one();
        return 0;
    }
    if (message == ResultMessage)
    {
        std::vector<AiTranslation::Result> delivered;
        {
            std::lock_guard lock(result_mutex);
            delivered.swap(results);
        }
        for (auto &result : delivered)
        {
            if (!enabled || result.request.activation_epoch != epoch)
                continue;
            if (!result.generation)
            {
                if (result.request.client_id != selection_epoch)
                    continue;
                auto identity = AiTranslation::Identity(result.request);
                const auto now = GetTickCount64();
                if (identity == last_identity && (!last_finished || now - last_time < 2000))
                {
                    request_selection = result.request.client_id;
                    source_display = Wide(result.request.source);
                    copy_text = last_translation;
                    translated_display =
                        last_finished ? (copy_text.empty() ? L"翻译失败，请稍后重新选择文字重试。" : Wide(copy_text))
                                      : L"正在翻译…";
                    Show(anchor);
                    continue;
                }
                last_identity = std::move(identity);
                last_time = now;
                last_finished = false;
                last_translation.clear();
                source_display = Wide(result.request.source);
                translated_display = L"正在翻译…";
                copy_text.clear();
                request_selection = result.request.client_id;
                request_mode = result.request.activation_epoch;
                request_generation = worker.Submit(std::move(result.request));
                Show(anchor);
            }
            else if (result.generation == request_generation && worker.IsCurrent(result.generation))
            {
                last_translation = std::move(result.translation);
                last_finished = true;
                if (request_selection != selection_epoch)
                    continue;
                copy_text = last_translation;
                translated_display =
                    copy_text.empty() ? L"翻译失败。请重新选择文字重试；原应用内容未改变。" : Wide(copy_text);
                InvalidateRect(window, nullptr, TRUE);
            }
        }
        return 0;
    }
    if (message == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (message == WM_DPICHANGED)
    {
        auto rect = reinterpret_cast<RECT *>(lparam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }
    if (message == WM_MOUSEWHEEL)
    {
        scroll = std::max(0, scroll - GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * 60);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    if (message == WM_LBUTTONUP)
    {
        RECT rect{};
        GetClientRect(window, &rect);
        const int x = LOWORD(lparam), y = HIWORD(lparam);
        if (action_menu)
        {
            KillTimer(window, 2);
            action_menu = false;
            show_capture_errors = true;
            source_display = L"阅读翻译";
            translated_display = L"正在读取选区…";
            Show(anchor);
            {
                std::lock_guard lock(capture_mutex);
                pending = {anchor, action_foreground, selection_epoch.load(), epoch.load(), true};
                capture_pending = true;
            }
            capture_cv.notify_one();
            return 0;
        }
        if (y > rect.bottom - Scale(40, GetDpiForWindow(window)))
        {
            if (x > rect.right / 2)
                Hide();
            else
                Copy();
        }
        return 0;
    }
    if (message == WM_PAINT)
    {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT bounds{};
        GetClientRect(window, &bounds);
        FillRect(dc, &bounds, GetSysColorBrush(COLOR_WINDOW));
        SetBkMode(dc, TRANSPARENT);
        const UINT dpi = GetDpiForWindow(window);
        if (action_menu)
        {
            HFONT menu_font = CreateFontW(-Scale(15, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                          L"Microsoft YaHei UI");
            auto old_font = SelectObject(dc, menu_font);
            DrawTextW(dc, L"翻译选中文字", -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old_font);
            DeleteObject(menu_font);
            EndPaint(window, &paint);
            return 0;
        }
        int pad = Scale(16, dpi), footer = Scale(40, dpi);
        HFONT font =
            CreateFontW(-Scale(15, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        auto old = SelectObject(dc, font);
        RECT clip{pad, pad, bounds.right - pad, bounds.bottom - footer};
        IntersectClipRect(dc, clip.left, clip.top, clip.right, clip.bottom);
        std::wstring text = source_display.substr(0, 512) + L"\n\n" + translated_display;
        RECT content{pad, pad - scroll, bounds.right - pad, bounds.bottom};
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &content, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        scroll = std::min(scroll, std::max<int>(0, content.bottom - content.top - (clip.bottom - clip.top)));
        const int text_height = content.bottom - content.top;
        content.top = pad - scroll;
        content.bottom = content.top + text_height;
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &content, DT_WORDBREAK | DT_NOPREFIX);
        SelectClipRgn(dc, nullptr);
        RECT copy{pad, bounds.bottom - footer, bounds.right / 2, bounds.bottom};
        DrawTextW(dc, L"复制译文", -1, &copy, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        RECT close{bounds.right / 2, bounds.bottom - footer, bounds.right - pad, bounds.bottom};
        DrawTextW(dc, L"关闭", -1, &close, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        SelectObject(dc, old);
        DeleteObject(font);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_CLOSE)
    {
        Hide();
        return 0;
    }
    if (message == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
void HookLoop()
{
    MSG message;
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    hook_thread_id = GetCurrentThreadId();
    PostThreadMessageW(hook_thread_id.load(), ConfigMessage, 0, 0);
    while (running && GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (message.message != ConfigMessage)
        {
            DispatchMessageW(&message);
            continue;
        }
        if (mouse_hook)
            UnhookWindowsHookEx(mouse_hook);
        if (keyboard_hook)
            UnhookWindowsHookEx(keyboard_hook);
        if (foreground_hook)
            UnhookWinEvent(foreground_hook);
        mouse_hook = nullptr;
        keyboard_hook = nullptr;
        foreground_hook = nullptr;
        hook_count = 0;
        if (enabled && running)
        {
            mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, Mouse, GetModuleHandleW(nullptr), 0);
            keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, Keyboard, GetModuleHandleW(nullptr), 0);
            foreground_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, Foreground, 0,
                                              0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            hook_count = (mouse_hook ? 1 : 0) + (keyboard_hook ? 1 : 0) + (foreground_hook ? 1 : 0);
        }
    }
    if (mouse_hook)
        UnhookWindowsHookEx(mouse_hook);
    if (keyboard_hook)
        UnhookWindowsHookEx(keyboard_hook);
    if (foreground_hook)
        UnhookWinEvent(foreground_hook);
    mouse_hook = nullptr;
    keyboard_hook = nullptr;
    foreground_hook = nullptr;
    hook_count = 0;
    hook_thread_id = 0;
}
void UiLoop()
{
    WNDCLASSW type{};
    type.lpfnWndProc = WindowProc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"MSIME.ReadingTranslation";
    type.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&type);
    popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, type.lpszClassName, L"水杉阅读翻译",
                            WS_POPUP | WS_BORDER, 0, 0, 520, 360, nullptr, nullptr, type.hInstance, nullptr);
    host = popup;
    if (!popup)
    {
        running = false;
        capture_cv.notify_all();
        return;
    }
    PostMessageW(popup, ConfigMessage, 0, 0);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    host = nullptr;
    DestroyWindow(popup);
    popup = nullptr;
}
} // namespace
void Start(const std::string &database, AiTranslation::Fetcher fetcher)
{
    if (running.exchange(true))
        return;
    Configure(GetConfiguredAiAssistant());
    worker.Start(
        database,
        [](AiTranslation::Result result) {
            if (!enabled || result.request.activation_epoch != epoch)
                return;
            {
                std::lock_guard lock(result_mutex);
                results.push_back(std::move(result));
            }
            if (host.load())
                PostMessageW(host.load(), ResultMessage, 0, 0);
        },
        [fetcher](const AiTranslation::Request &request, const AiTranslation::Cancelled &cancelled) {
            if (!enabled || request.activation_epoch != epoch || cancelled())
                return std::string{};
            return fetcher(request, [&] { return !enabled || request.activation_epoch != epoch || cancelled(); });
        },
        std::chrono::milliseconds(100));
    BrowserReading::Start(database, fetcher);
    capture_thread = std::thread(CaptureLoop);
    ui_thread = std::thread(UiLoop);
    hook_thread = std::thread(HookLoop);
}
#ifdef MSIME_READING_TESTING
void TestBusyPopup(int milliseconds)
{
    if (host.load())
        PostMessageW(host.load(), WM_APP + 307, milliseconds, 0);
}
void TestMouse(WPARAM message, POINT point)
{
    MSLLHOOKSTRUCT input{};
    input.pt = point;
    Mouse(0, message, reinterpret_cast<LPARAM>(&input));
}
void TestSelection(POINT point)
{
    if (host.load())
        SendMessageW(host.load(), WM_APP + 304, 0, reinterpret_cast<LPARAM>(&point));
}
void TestTranslateEntry()
{
    if (host.load())
        SendMessageW(host.load(), WM_LBUTTONUP, 0, MAKELPARAM(10, 10));
}
int TestHookCount()
{
    return host.load() ? static_cast<int>(SendMessageW(host.load(), WM_APP + 305, 0, 0)) : 0;
}
#endif
void Stop()
{
    if (!running.exchange(false))
        return;
    enabled = false;
    ++epoch;
    ++selection_epoch;
    capture_cv.notify_all();
    BrowserReading::Stop();
    worker.Stop();
    if (hook_thread_id.load())
        PostThreadMessageW(hook_thread_id.load(), WM_QUIT, 0, 0);
    if (hook_thread.joinable())
        hook_thread.join();
    if (host.load())
        PostMessageW(host.load(), WM_DESTROY, 0, 0);
    if (ui_thread.joinable())
        ui_thread.join();
    if (capture_thread.joinable())
        capture_thread.join();
}
} // namespace ReadingTranslation
