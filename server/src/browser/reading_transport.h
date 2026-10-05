#pragma once
#include "engine/contracts/browser/reading.h"
#include <windows.h>
#include <sddl.h>
#include <string>
#include <vector>
#include <functional>
namespace BrowserReadingTransport
{
inline std::wstring UserSid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    std::wstring sid;
    if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size))
    {
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &text))
        {
            sid = text;
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return sid;
}
inline std::wstring PipeName()
{
    auto sid = UserSid();
#ifdef MSIME_READING_TESTING
    if (!sid.empty())
        sid += L".Probe." + std::to_wstring(GetCurrentProcessId());
#endif
    return sid.empty() ? std::wstring{} : std::wstring(BrowserReadingContract::PipePrefix) + sid;
}
// Overlapped transport keeps idle work asleep; bounded wait, no UI or IME thread involved.
inline bool Finish(HANDLE pipe, OVERLAPPED &overlapped, DWORD &bytes, DWORD timeout,
                   const std::function<bool()> &cancel)
{
    const auto end = GetTickCount64() + timeout;
    while (!cancel() && GetTickCount64() < end)
    {
        if (WaitForSingleObject(overlapped.hEvent, 100) == WAIT_OBJECT_0)
            return GetOverlappedResult(pipe, &overlapped, &bytes, FALSE) != FALSE;
    }
    CancelIoEx(pipe, &overlapped);
    // The stack OVERLAPPED must stay alive until cancellation completes.
    GetOverlappedResult(pipe, &overlapped, &bytes, TRUE);
    return false;
}
inline bool Transfer(HANDLE pipe, void *data, DWORD size, bool write, DWORD timeout,
                     const std::function<bool()> &cancel)
{
    auto *cursor = static_cast<BYTE *>(data);
    while (size)
    {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent)
            return false;
        DWORD bytes = 0;
        bool ok =
            (write ? WriteFile(pipe, cursor, size, &bytes, &ov) : ReadFile(pipe, cursor, size, &bytes, &ov)) != FALSE;
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = Finish(pipe, ov, bytes, timeout, cancel);
        CloseHandle(ov.hEvent);
        if (!ok || !bytes)
            return false;
        cursor += bytes;
        size -= bytes;
    }
    return true;
}
inline bool ReadMessage(HANDLE pipe, std::string &text, DWORD timeout, const std::function<bool()> &cancel)
{
    uint32_t size = 0;
    if (!Transfer(pipe, &size, sizeof(size), false, timeout, cancel) || !size ||
        size > BrowserReadingContract::MaxMessageBytes)
        return false;
    text.resize(size);
    return Transfer(pipe, text.data(), size, false, timeout, cancel);
}
inline bool WriteMessage(HANDLE pipe, std::string text, const std::function<bool()> &cancel)
{
    uint32_t size = static_cast<uint32_t>(text.size());
    return size <= BrowserReadingContract::MaxMessageBytes && Transfer(pipe, &size, sizeof(size), true, 5000, cancel) &&
           Transfer(pipe, text.data(), size, true, 5000, cancel);
}
} // namespace BrowserReadingTransport
