#pragma once
#include <cstdint>
namespace BrowserReadingContract
{
inline constexpr wchar_t PipePrefix[] = L"\\\\.\\pipe\\MSIME.Reading.v1.";
inline constexpr uint32_t Version = 1, MaxMessageBytes = 65536;
inline constexpr char HostName[] = "org.metasequoiaime.reading";
// Desktop transport has the same framing as Chromium Native Messaging: length + UTF-8 JSON.
} // namespace BrowserReadingContract
