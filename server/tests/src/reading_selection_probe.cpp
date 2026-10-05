// Isolated Win32 controls + real UI Automation capture + fake network; never reads user configuration.
#include "ai/reading_translation.h"
#include "browser/reading_transport.h"
#include <nlohmann/json.hpp>
#include "utils/network_proxy.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
namespace
{
AiAssistantConfig config;
}
const AiAssistantConfig &GetConfiguredAiAssistant()
{
    return config;
}
NetworkProxyConfig GetConfiguredNetworkProxy()
{
    return {};
}
std::string NormalizeNetworkProxyServer(const std::string &value)
{
    return value;
}
void Pump(int milliseconds)
{
    const auto end = GetTickCount64() + milliseconds;
    while (GetTickCount64() < end)
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(5);
    }
}
void Require(bool condition, const char *name)
{
    if (!condition)
        throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}
nlohmann::json BrowserRequest(const std::string &source)
{
    const auto name = BrowserReadingTransport::PipeName();
    WaitNamedPipeW(name.c_str(), 2000);
    HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED, nullptr);
    Require(pipe != INVALID_HANDLE_VALUE, "browser bridge connected");
    std::string reply;
    auto input = nlohmann::json({{"version", 1}, {"command", "translate"}, {"text", source}}).dump();
    const bool ok = BrowserReadingTransport::WriteMessage(pipe, input, [] { return false; }) &&
                    BrowserReadingTransport::ReadMessage(pipe, reply, 10000, [] { return false; });
    CloseHandle(pipe);
    Require(ok, "browser bridge framed response");
    return nlohmann::json::parse(reply);
}
int main(int argc, char **argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto path = (std::filesystem::temp_directory_path() /
                 ("msime-reading-probe-" + std::to_string(GetCurrentProcessId()) + ".db"))
                    .u8string();
    std::atomic<int> calls{0};
    std::atomic<bool> slow{false}, started{false}, cancelled{false}, fail{false};
    HWND fixture =
        CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"MSIME isolated reading test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60,
                        60, 700, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND edit = CreateWindowExW(0, L"EDIT", L"I have to work tomorrow.", WS_CHILD | WS_VISIBLE | ES_MULTILINE, 10, 10,
                                650, 80, fixture, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND password = CreateWindowExW(0, L"EDIT", L"Never capture this password", WS_CHILD | WS_VISIBLE | ES_PASSWORD, 10,
                                    120, 650, 40, fixture, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetForegroundWindow(fixture);
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 0, -1);
    ReadingTranslation::Start(path, [&](const auto &, const auto &cancel) {
        ++calls;
        started = true;
        while (slow && !cancel())
            Sleep(5);
        if (cancel())
        {
            cancelled = true;
            return std::string{};
        }
        return fail ? std::string{} : std::string(u8"我明天得上班。");
    });
    if (argc > 1 && std::string(argv[1]) == "--interactive")
    {
        config.translation_enabled = true;
        ReadingTranslation::Configure(config);
        std::cout << "Interactive selection probe active: isolated cache, mock translations, no user credentials.\n"
                  << std::flush;
        Pump(240000);
        ReadingTranslation::Stop();
        DestroyWindow(fixture);
        return 0;
    }
    try
    {
        Pump(200);
        POINT point{25, 25};
        ClientToScreen(fixture, &point);
        ReadingTranslation::TestSelection(point);
        Pump(800);
        Require(calls == 0 && ReadingTranslation::TestHookCount() == 0, "ordinary mode: no hooks or requests");
        Require(!BrowserRequest("I have to work tomorrow.")["ok"].get<bool>() && calls == 0,
                "browser translation rejected in ordinary mode with zero API");
        config.translation_enabled = true;
        ReadingTranslation::Configure(config);
        Pump(150);
        Require(ReadingTranslation::TestHookCount() == 3, "translation mode installs hooks");
        HWND unsupported = CreateWindowExW(0, L"BUTTON", L"No text selection", WS_CHILD | WS_VISIBLE, 10, 165, 200, 25,
                                           fixture, nullptr, GetModuleHandleW(nullptr), nullptr);
        POINT unsupported_point{20, 175};
        ClientToScreen(fixture, &unsupported_point);
        ReadingTranslation::TestSelection(unsupported_point);
        Pump(2800);
        Require(calls == 0, "unsupported text pattern never crashes or calls API");
        DestroyWindow(unsupported);
        SetFocus(edit);
        SendMessageW(edit, EM_SETSEL, 0, -1);
        ReadingTranslation::TestBusyPopup(300);
        Sleep(30);
        const auto callback_start = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i)
            ReadingTranslation::TestMouse(WM_LBUTTONDOWN, point);
        const auto elapsed = std::chrono::steady_clock::now() - callback_start;
        Require(elapsed < 50ms, "mouse callbacks stay fast while popup thread is blocked");
        Pump(400);
        ReadingTranslation::TestMouse(WM_LBUTTONDOWN, point);
        Pump(150);
        for (int i = 0; i < 30; ++i)
            ReadingTranslation::TestMouse(WM_MOUSEMOVE, point);
        Pump(450);
        Require(calls == 0, "dragging and mouse moves produce zero requests");
        ReadingTranslation::TestMouse(WM_LBUTTONUP, point);
        Pump(1200);
        Require(calls == 0, "selection release never auto-translates");
        ReadingTranslation::TestMouse(WM_RBUTTONUP, point);
        Pump(300);
        Require(calls == 0, "right click only opens entry: zero API calls");
        SetFocus(edit);
        SendMessageW(edit, EM_SETSEL, 0, -1);
        ReadingTranslation::TestTranslateEntry(); // actual desktop translation entry
        Pump(3500);
        Require(calls == 1, "real UIA English selection invokes one translation");
        wchar_t unchanged[128]{};
        GetWindowTextW(edit, unchanged, 128);
        Require(std::wstring(unchanged) == L"I have to work tomorrow.", "original application text unchanged");
        ReadingTranslation::TestSelection(point);
        Pump(600);
        Require(calls == 1, "duplicate selection deduplicated");
        Pump(2100);
        ReadingTranslation::TestSelection(point);
        Pump(600);
        Require(calls == 1, "reselection uses SQLite cache with zero network calls");
        const auto browser_cached = BrowserRequest("I have to work tomorrow.");
        Require(browser_cached["ok"].get<bool>() && browser_cached["cache_hit"].get<bool>() && calls == 1,
                "browser shares desktop SQLite cache with zero API");
        Require(!BrowserRequest("123 ! ?")["ok"].get<bool>() && calls == 1, "browser noise filtering never calls API");
        SetWindowTextW(edit, L"Kailangan kong magtrabaho bukas.");
        SendMessageW(edit, EM_SETSEL, 0, -1);
        ReadingTranslation::TestSelection(point);
        Pump(3500);
        Require(calls == 2, "Tagalog selection accepted");
        std::wstring longText;
        for (int i = 0; i < 100; ++i)
            longText += L"This is a long reading sentence. ";
        SetWindowTextW(edit, longText.c_str());
        SendMessageW(edit, EM_SETSEL, 0, -1);
        ReadingTranslation::TestSelection(point);
        Pump(3500);
        Require(calls == 3, "long reading text accepted");
        SetWindowTextW(edit, L"12345 ! ?");
        SendMessageW(edit, EM_SETSEL, 0, -1);
        ReadingTranslation::TestSelection(point);
        Pump(750);
        Require(calls == 3, "numeric punctuation selection filtered");
        POINT protectedPoint{25, 135};
        ClientToScreen(fixture, &protectedPoint);
        SetFocus(password);
        SendMessageW(password, EM_SETSEL, 0, -1);
        ReadingTranslation::TestSelection(protectedPoint);
        Pump(750);
        Require(calls == 3, "password field rejected");
        SetFocus(edit);
        SetWindowTextW(edit, L"Cancel this unfinished request.");
        SendMessageW(edit, EM_SETSEL, 0, -1);
        slow = true;
        started = false;
        ReadingTranslation::TestSelection(point);
        Pump(900);
        Require(started, "async request started without blocking fixture");
        ReadingTranslation::TestSelection(point);
        Pump(650);
        Require(calls == 4, "same selection while request pending never starts a second request");
        config.translation_enabled = false;
        ReadingTranslation::Configure(config);
        Pump(150);
        Require(cancelled && ReadingTranslation::TestHookCount() == 0,
                "mode exit cancels request and uninstalls all hooks");
        ReadingTranslation::TestSelection(point);
        Pump(650);
        Require(calls == 4, "ordinary mode remains inactive");
        config.translation_enabled = true;
        ReadingTranslation::Configure(config);
        slow = false;
        fail = true;
        SetWindowTextW(edit, L"Network failure preserves the text.");
        SendMessageW(edit, EM_SETSEL, 0, -1);
        Pump(100);
        ReadingTranslation::TestSelection(point);
        Pump(900);
        GetWindowTextW(edit, unchanged, 128);
        Require(std::wstring(unchanged) == L"Network failure preserves the text.",
                "network failure leaves original text unchanged");
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        ReadingTranslation::Stop();
        DestroyWindow(fixture);
        return 1;
    }
    ReadingTranslation::Stop();
    DestroyWindow(fixture);
    for (auto suffix : {"", "-wal", "-shm"})
    {
        std::error_code error;
        std::filesystem::remove(std::filesystem::u8path(path + suffix), error);
    }
    return 0;
}
