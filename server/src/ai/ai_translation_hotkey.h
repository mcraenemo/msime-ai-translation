#pragma once
#include <Windows.h>
#include "config/ime_config.h"
#include <algorithm>
#include <cctype>
#include <string>

namespace AiTranslation
{
struct Hotkey
{
    bool valid = false;
    UINT modifiers = 0, key = 0;
};
inline Hotkey ParseHotkey(std::string text)
{
    if (text.empty())
        return {true, 0, 0};
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
               text.end());
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    Hotkey h;
    size_t start = 0;
    for (;;)
    {
        const size_t end = text.find('+', start);
        const auto part = text.substr(start, end == std::string::npos ? end : end - start);
        if (end != std::string::npos)
        {
            UINT mod = part == "CTRL"    ? MOD_CONTROL
                       : part == "ALT"   ? MOD_ALT
                       : part == "SHIFT" ? MOD_SHIFT
                       : part == "WIN"   ? MOD_WIN
                                         : 0;
            if (!mod || (h.modifiers & mod))
                return {};
            h.modifiers |= mod;
            start = end + 1;
            continue;
        }
        if (part.size() == 1 && ((part[0] >= 'A' && part[0] <= 'Z') || (part[0] >= '0' && part[0] <= '9')))
            h.key = part[0];
        else if (part.size() >= 2 && part[0] == 'F')
        {
            int n = 0;
            for (size_t i = 1; i < part.size(); ++i)
            {
                if (!std::isdigit(static_cast<unsigned char>(part[i])))
                    return {};
                n = n * 10 + part[i] - '0';
                if (n > 24)
                    return {};
            }
            if (n >= 1 && n <= 24)
                h.key = VK_F1 + n - 1;
        }
        h.valid = h.key != 0 && h.modifiers != 0;
        // 原有简繁快捷键不得被翻译热键抢占。
        if (h.key == 'F' && h.modifiers == (MOD_CONTROL | MOD_SHIFT))
            h.valid = false;
        return h;
    }
}
inline std::wstring ModeLabel(bool traditional, bool translation)
{
    return std::wstring(traditional ? L"繁" : L"简") + (translation ? L"译" : L"");
}
inline void CycleMode()
{
    const bool traditional = GetConfiguredCharacterSet() == "traditional";
    const bool translation = GetConfiguredAiAssistant().translation_enabled;
    if (SetConfiguredCharacterSet(traditional ? "simplified" : "traditional") && traditional)
        SetConfiguredAiAssistantBool("translation_enabled", !translation);
}
inline constexpr int kTranslationHotkeyId = 0x4D54;
inline void ConfigureHotkey(HWND hwnd)
{
    UnregisterHotKey(hwnd, kTranslationHotkeyId);
    auto hotkey = ParseHotkey(GetConfiguredAiAssistant().translation_hotkey);
    if (hotkey.valid && hotkey.key)
        RegisterHotKey(hwnd, kTranslationHotkeyId, hotkey.modifiers | MOD_NOREPEAT, hotkey.key);
}
} // namespace AiTranslation
