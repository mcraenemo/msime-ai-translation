#pragma once
#include "config/ime_config.h"
#include "ai_translation.h"
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>

namespace ReadingTranslation
{
inline std::mutex config_mutex;
inline AiAssistantConfig config_snapshot;
inline std::atomic<bool> enabled{false};
inline std::atomic<uint64_t> epoch{0};
inline std::atomic<HWND> host{nullptr};
inline std::atomic<DWORD> hook_thread_id{0};
inline constexpr UINT ConfigMessage = WM_APP + 301;
inline AiAssistantConfig Snapshot()
{
    std::lock_guard lock(config_mutex);
    return config_snapshot;
}
inline void Configure(const AiAssistantConfig &config)
{
    bool changed;
    {
        std::lock_guard lock(config_mutex);
        changed = config.translation_enabled != config_snapshot.translation_enabled ||
                  config.reading_target_language != config_snapshot.reading_target_language ||
                  config.provider != config_snapshot.provider || config.model != config_snapshot.model ||
                  config.endpoint != config_snapshot.endpoint || config.token != config_snapshot.token;
        config_snapshot = config;
        enabled = config.translation_enabled;
        if (changed)
            ++epoch;
    }
    if (changed && hook_thread_id.load())
        PostThreadMessageW(hook_thread_id.load(), ConfigMessage, 0, 0);
    if (changed && host.load())
        PostMessageW(host.load(), ConfigMessage, 0, 0);
}
void Start(const std::string &database, AiTranslation::Fetcher fetcher = AiTranslation::Fetch);
#ifdef MSIME_READING_TESTING
void TestSelection(POINT point);
void TestTranslateEntry();
void TestMouse(WPARAM message, POINT point);
void TestBusyPopup(int milliseconds);
int TestHookCount();
#endif
void Stop();
} // namespace ReadingTranslation
