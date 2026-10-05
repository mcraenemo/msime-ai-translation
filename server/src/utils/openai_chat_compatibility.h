#pragma once

#include <nlohmann/json.hpp>
#include <string_view>

namespace OpenAiChatCompatibility
{
inline bool UsesCompletionTokens(std::string_view provider, std::string_view model)
{
    if (provider != "openai")
        return false;
    for (const std::string_view family : {"gpt-5", "gpt-6", "o1", "o3", "o4"})
    {
        if (model == family || (model.size() > family.size() && model.substr(0, family.size()) == family &&
                                (model[family.size()] == '-' || model[family.size()] == '.')))
            return true;
    }
    return false;
}

inline void Apply(nlohmann::json &body, std::string_view provider, std::string_view model, int limit)
{
    const bool completion = UsesCompletionTokens(provider, model);
    const bool luna = provider == "openai" && (model == "gpt-6-luna" || model.substr(0, 11) == "gpt-6-luna-");
    body[completion ? "max_completion_tokens" : "max_tokens"] = luna && limit < 2048 ? 2048 : limit;
    if (!completion)
        return;
    // 新 OpenAI 推理模型不接受原有温度；关闭 luna 推理，保留短请求的输出预算。
    body.erase("temperature");
    if (luna)
        body["reasoning_effort"] = "none";
}

inline bool OutputLimitReached(const nlohmann::json &response)
{
    const auto choices = response.find("choices");
    if (choices == response.end() || !choices->is_array() || choices->empty())
        return false;
    const auto &choice = choices->at(0);
    if (!choice.is_object())
        return false;
    const auto reason = choice.find("finish_reason");
    return reason != choice.end() && reason->is_string() && *reason == "length";
}
} // namespace OpenAiChatCompatibility
