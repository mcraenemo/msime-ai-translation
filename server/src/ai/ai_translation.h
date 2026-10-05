#pragma once
#include "config/ime_config.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>

namespace AiTranslation
{
struct Request
{
    AiAssistantConfig config;
    std::string source, target, character_set, context;
    bool reading = false;
    uint64_t client_id = 0, activation_epoch = 0;
};
struct Result
{
    Request request;
    std::string translation;
    uint64_t generation = 0;
    bool cache_hit = false;
};
using Cancelled = std::function<bool()>;
using Fetcher = std::function<std::string(const Request &, const Cancelled &)>;
using Callback = std::function<void(Result)>;
// 源文本必须是引擎确定的中文候选；短句保留上下文键，长句按自身内容复用。
bool IsChineseSource(const std::string &source);
bool IsReadingSource(const std::string &source);
bool IsValidSource(const Request &request);
std::string ContextKey(const Request &request);
std::string Identity(const Request &request);
bool ClearCache(const std::string &path);
std::string Fetch(const Request &request, const Cancelled &cancelled);

class Worker
{
  public:
    ~Worker();
    void Start(std::string path, Callback callback, Fetcher fetcher,
               std::chrono::milliseconds delay = std::chrono::milliseconds(650));
    uint64_t Submit(Request request);
    void Cancel();
    void Stop();
    bool IsCurrent(uint64_t generation) const;

  private:
    void Run();
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> generation_{0};
    Request latest_;
    std::string path_;
    Callback callback_;
    Fetcher fetcher_;
    std::chrono::milliseconds delay_{650};
    std::chrono::steady_clock::time_point changed_;
};
Worker &Instance();
} // namespace AiTranslation
