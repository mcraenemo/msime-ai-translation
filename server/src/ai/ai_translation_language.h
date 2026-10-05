#pragma once
#include <string>
#include <string_view>

namespace AiTranslation
{
// 允许语言代码或用户填写的语言名称，不把模型支持范围写死为一份白名单。
inline bool IsValidTargetLanguage(std::string_view language)
{
    if (language.empty() || language.size() > 80 || language.front() == ' ' || language.back() == ' ')
        return false;
    bool hasName = false;
    for (unsigned char c : language)
    {
        if (c < 32 || c == 127)
            return false;
        if (c != ' ')
            hasName = true;
    }
    return hasName;
}
} // namespace AiTranslation
