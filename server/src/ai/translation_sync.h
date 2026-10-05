#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <sqlite3.h>

namespace TranslationSync
{
std::string SpreadsheetId();
struct Record
{
    std::string source, language, text, origin = "ai", charset = "simplified", context, provider, model;
    int64_t updated = 0, hits = 0;
    std::string Id() const;
    nlohmann::json Json() const;
    static Record Parse(const nlohmann::json &j);
};
bool EnsureSchema(sqlite3 *db);
void Hit(sqlite3 *db, const std::string &identity);
Record Merge(const Record &local, const Record &remote, const Record *shadow);
struct RemoteRow
{
    Record record;
    int row = 0;
    nlohmann::json raw;
};
struct Edit
{
    int row;
    Record record;
    nlohmann::json before;
};
struct Transport
{
    std::function<std::vector<RemoteRow>()> read;
    // 先重新读取并比较所有目标行，再增量写入；发生人工并发修改则重试，不覆写。
    std::function<void(const std::vector<Edit> &)> write;
};
void Synchronize(const std::string &path, const Transport &transport);
nlohmann::json Status(const std::string &path);
bool Action(const std::string &path, const std::string &action);
void Start(std::string path, bool enabled);
void Stop();
void Enable(bool enabled);
bool DailyDue(bool enabled, int64_t last_success, int64_t now);
// 凭据只通过 Windows Credential Manager 读取；状态接口不返回任何凭据。
bool HasClient();
bool HasAuthorization();
bool ImportClient();
void Authorize();
Transport GoogleTransport(const std::atomic<bool> &stop);
} // namespace TranslationSync
