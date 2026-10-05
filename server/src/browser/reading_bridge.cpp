#include "reading_bridge.h"
#include "reading_transport.h"
#include "ai/reading_translation.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <condition_variable>

namespace BrowserReading
{
namespace
{
using Json = nlohmann::json;
std::atomic<bool> running{false};
std::thread thread;
AiTranslation::Worker worker;
std::mutex mutex;
std::condition_variable cv;
AiTranslation::Result response;
bool ready = false;
uint64_t id = 0;
Json Error(const char *message)
{
    return {{"ok", false}, {"error", message}};
}
Json Translate(const Json &message)
{
    if (!message.is_object() || message.value("version", 0) != BrowserReadingContract::Version ||
        message.value("command", std::string{}) != "translate" || !message.contains("text") ||
        !message["text"].is_string())
        return Error("请求格式无效。");
    auto config = ReadingTranslation::Snapshot();
    const auto mode = ReadingTranslation::epoch.load();
    if (!ReadingTranslation::enabled || !config.translation_enabled)
        return Error("请先切换到简译或繁译模式。");
    const auto source = message["text"].get<std::string>();
    if (!AiTranslation::IsReadingSource(source))
        return Error("选中文字无效或过长。");
    AiTranslation::Request request;
    request.config = std::move(config);
    request.source = source;
    request.target = request.config.reading_target_language;
    request.character_set = AiTranslation::IsChineseSource(source) ? "simplified" : "auto";
    request.reading = true;
    request.activation_epoch = mode;
    request.client_id = ++id;
    {
        std::lock_guard lock(mutex);
        ready = false;
    }
    const auto generation = worker.Submit(std::move(request));
    std::unique_lock lock(mutex);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (!ready && running && ReadingTranslation::enabled && ReadingTranslation::epoch == mode &&
           std::chrono::steady_clock::now() < until)
        cv.wait_for(lock, std::chrono::milliseconds(100));
    if (!ready || !running || !ReadingTranslation::enabled || ReadingTranslation::epoch != mode)
    {
        lock.unlock();
        worker.Cancel();
        return Error("翻译已取消或超时。");
    }
    if (response.generation != generation || response.translation.empty())
        return Error("翻译失败，请检查 API 配置或稍后重试。");
    return {{"ok", true}, {"translation", response.translation}, {"cache_hit", response.cache_hit}};
}
void Run()
{
    const auto name = BrowserReadingTransport::PipeName();
    const auto sid = BrowserReadingTransport::UserSid();
    if (name.empty() || sid.empty())
        return;
    // Same Windows user only; low integrity label allows medium browser -> uiAccess Server.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    auto sddl = L"D:P(A;;GA;;;" + sid + L")S:(ML;;NW;;;LW)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        return;
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    HANDLE pipe = CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &security);
    LocalFree(descriptor);
    if (pipe == INVALID_HANDLE_VALUE)
        return;
    while (running)
    {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent)
            break;
        DWORD ignored = 0;
        bool connected = ConnectNamedPipe(pipe, &ov) != FALSE;
        auto error = GetLastError();
        if (!connected && error == ERROR_PIPE_CONNECTED)
            connected = true;
        if (!connected && error == ERROR_IO_PENDING)
            connected =
                BrowserReadingTransport::Finish(pipe, ov, ignored, 24 * 60 * 60 * 1000, [] { return !running; });
        CloseHandle(ov.hEvent);
        if (!running)
            break;
        if (connected)
        {
            std::string request;
            if (BrowserReadingTransport::ReadMessage(pipe, request, 5000, [] { return !running; }))
            {
                Json reply;
                try
                {
                    reply = Translate(Json::parse(request));
                }
                catch (...)
                {
                    reply = Error("请求格式无效。");
                }
                BrowserReadingTransport::WriteMessage(pipe, reply.dump(), [] { return !running; });
                // DisconnectNamedPipe discards unread data. Wait for the client's close
                // (or a bounded timeout) so the complete framed response is consumed.
                BYTE closed = 0;
                BrowserReadingTransport::Transfer(pipe, &closed, 1, false, 5000, [] { return !running; });
            }
        }
        DisconnectNamedPipe(pipe);
    }
    CloseHandle(pipe);
}
} // namespace
void Start(const std::string &database, AiTranslation::Fetcher fetcher)
{
    if (running.exchange(true))
        return;
    worker.Start(
        database,
        [](AiTranslation::Result result) {
            {
                std::lock_guard lock(mutex);
                response = std::move(result);
                ready = true;
            }
            cv.notify_all();
        },
        [fetcher](const AiTranslation::Request &request, const AiTranslation::Cancelled &cancel) {
            return fetcher(request, [&] {
                return !running || !ReadingTranslation::enabled ||
                       ReadingTranslation::epoch != request.activation_epoch || cancel();
            });
        },
        std::chrono::milliseconds(0));
    thread = std::thread(Run);
}
void Cancel()
{
    worker.Cancel();
    cv.notify_all();
}
void Stop()
{
    if (!running.exchange(false))
        return;
    Cancel();
    if (thread.joinable())
        thread.join();
    worker.Stop();
}
} // namespace BrowserReading
