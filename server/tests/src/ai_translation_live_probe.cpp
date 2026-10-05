// 手动联网验证入口，不注册为 CTest；凭证只读入内存，输出仅包含成功状态和请求计数。
#include "ai/ai_translation.h"
#include "utils/network_proxy.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <curl/curl.h>

namespace
{
NetworkProxyConfig proxy;
}
NetworkProxyConfig GetConfiguredNetworkProxy()
{
    return proxy;
}
// 配置内已经是 Server 持久化后的规范地址，不再改变连接配置。
std::string NormalizeNetworkProxyServer(const std::string &value)
{
    return value;
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3)
    {
        std::fprintf(stderr, "Usage: ai_translation_live_probe <existing-config-path>\n");
        return 2;
    }
    try
    {
        std::ifstream file(std::filesystem::u8path(argv[1]));
        if (!file)
            return 2;
        const std::string contents((std::istreambuf_iterator<char>(file)), {});
        const auto config = toml::parse(contents);
        AiTranslation::Request r;
        const auto ai = config["ai_assistant"];
        r.config.provider = ai["provider"].value_or(std::string("openai"));
        r.config.token = ai["token_" + r.config.provider].value_or(std::string());
        if (r.config.token.empty())
            r.config.token = ai["token"].value_or(std::string());
        const AiAssistantConfig defaults;
        const auto effective = [&](const std::string &key, const auto &slots) {
            auto value = ai[key + "_" + r.config.provider].value_or(std::string());
            if (value.empty())
                value = ai[key].value_or(std::string());
            if (value.empty())
                value = slots.at(r.config.provider);
            return value;
        };
        r.config.endpoint = effective("endpoint", defaults.endpoints);
        r.config.model = effective("model", defaults.models);
        r.config.translation_enabled = true;
        r.config.enabled = false;
        proxy.mode = config["network"]["proxy_mode"].value_or(std::string("system"));
        proxy.server = config["network"]["proxy_server"].value_or(std::string());
        r.source = u8"你今天上班了吗？";
        r.target = "en";
        r.character_set = "simplified";
        const bool reading = argc == 3 && std::string(argv[2]) == "--reading";
        if (reading)
        {
            r.reading = true;
            r.source = "I have to work tomorrow.";
            r.target = "zh-Hans";
            r.character_set = "auto";
        }
        const auto path =
            (std::filesystem::temp_directory_path() /
             std::filesystem::path("msime-live-translation-" + std::to_string(GetCurrentProcessId()) + ".db"))
                .u8string();
        curl_global_init(CURL_GLOBAL_DEFAULT);
        AiTranslation::Worker worker;
        std::atomic<int> calls{0};
        std::mutex mutex;
        std::condition_variable cv;
        int completed = 0;
        bool last_hit = false, valid = false;
        worker.Start(
            path,
            [&](const auto &result) {
                std::lock_guard lock(mutex);
                ++completed;
                last_hit = result.cache_hit;
                valid = !result.translation.empty() && (!reading || AiTranslation::IsChineseSource(result.translation));
                cv.notify_all();
            },
            [&](const auto &request, const auto &cancelled) {
                ++calls;
                return AiTranslation::Fetch(request, cancelled);
            });
        auto wait = [&](int count) {
            std::unique_lock lock(mutex);
            return cv.wait_for(lock, std::chrono::seconds(20), [&] { return completed >= count; }) && valid;
        };
        worker.Submit(r);
        bool ok = wait(1) && calls == 1 && !last_hit;
        if (ok)
        {
            worker.Submit(r);
            ok = wait(2) && calls == 1 && last_hit;
        }
        if (ok)
        {
            if (reading)
                r.source = "Kailangan kong magtrabaho bukas.";
            else
                r.target = "tl";
            worker.Submit(r);
            ok = wait(3) && calls == 2 && !last_hit;
        }
        worker.Stop();
        curl_global_cleanup();
        for (const auto suffix : {"", "-wal", "-shm"})
        {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::u8path(path + suffix), ec);
        }
        std::printf("Live English/Tagalog translation (reading flag selects source language): %s; cache reuse: %s; "
                    "HTTP requests: %d\n",
                    ok ? "PASS" : "FAIL", ok ? "PASS" : "not verified", calls.load());
        return ok ? 0 : 1;
    }
    catch (...)
    {
        std::fprintf(stderr, "Live probe failed (details omitted to protect credentials).\n");
        return 1;
    }
}
