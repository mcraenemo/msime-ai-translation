#include "utils/openai_chat_compatibility.h"
#include "tests/includes/test_framework.h"

TEST_CASE(chat_tokens_preserve_compatible_providers_and_old_models)
{
    for (const std::string_view provider : {"openai", "deepseek", "siliconflow", "groq", "custom"})
        for (const std::string_view model : {"gpt-6-luna", "gpt-6-luna-2026-09-01", "gpt-6", "gpt-5.2", "o3-mini",
                                             "gpt-4o", "gpt-60", "gpt-6custom", "deepseek-chat", "Qwen/Qwen3-8B"})
            for (const int limit : {1, 512, 4096})
            {
                const bool completion =
                    provider == "openai" && (model == "gpt-6-luna" || model == "gpt-6-luna-2026-09-01" ||
                                             model == "gpt-6" || model == "gpt-5.2" || model == "o3-mini");
                nlohmann::json body = {{"model", model}, {"temperature", 0.2}};
                OpenAiChatCompatibility::Apply(body, provider, model, limit);
                REQUIRE(body.contains("max_completion_tokens") == completion);
                REQUIRE(body.contains("max_tokens") != completion);
                REQUIRE(body.contains("temperature") != completion);
                const bool luna = provider == "openai" && (model == "gpt-6-luna" || model == "gpt-6-luna-2026-09-01");
                REQUIRE(body[completion ? "max_completion_tokens" : "max_tokens"] == (luna && limit < 2048 ? 2048 : limit));
                REQUIRE(body.contains("reasoning_effort") == luna);
                if (luna)
                    REQUIRE(body["reasoning_effort"] == "none");
            }
}

TEST_CASE(chat_response_rejects_truncated_output)
{
    REQUIRE(OpenAiChatCompatibility::OutputLimitReached(nlohmann::json{{"choices", {{{"finish_reason", "length"}}}}}));
    REQUIRE(!OpenAiChatCompatibility::OutputLimitReached(nlohmann::json{{"choices", {{{"finish_reason", "stop"}}}}}));
    REQUIRE(!OpenAiChatCompatibility::OutputLimitReached(nlohmann::json::object()));
    REQUIRE(!OpenAiChatCompatibility::OutputLimitReached(nlohmann::json::parse("invalid", nullptr, false)));
}
