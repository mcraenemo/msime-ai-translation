#pragma once
#include <string>
#include <utility>

namespace AiTranslation
{
// Only Begin() authorizes a translation. Editing and selecting never submit work.
class SentenceBuffer
{
  public:
    std::string text;
    bool translating = false;
    std::string status;
    void Append(const std::string &part)
    {
        text += part;
        status.clear();
    }
    void Backspace()
    {
        if (text.empty())
            return;
        size_t i = text.size() - 1;
        while (i && (static_cast<unsigned char>(text[i]) & 0xc0) == 0x80)
            --i;
        text.erase(i);
        status.clear();
    }
    bool Begin(bool explicit_ctrl_enter)
    {
        if (!explicit_ctrl_enter || translating || text.empty())
            return false;
        translating = true;
        status = "正在翻译…";
        return true;
    }
    void Fail()
    {
        translating = false;
        status = "翻译失败，请重试或按 Enter 提交中文";
    }
    void CancelRequest()
    {
        translating = false;
        status.clear();
    }
    void Clear()
    {
        text.clear();
        CancelRequest();
    }
};
} // namespace AiTranslation
