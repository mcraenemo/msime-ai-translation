#include "translation_sync.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#pragma comment(lib, "bcrypt.lib")

namespace TranslationSync
{
namespace
{
using J = nlohmann::json;
void Check(int result)
{
    if (result != SQLITE_OK && result != SQLITE_DONE)
        throw std::runtime_error("本地翻译数据库忙或不可写，请稍后重试");
}
void Sql(sqlite3 *db, const char *sql)
{
    Check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}
struct Db
{
    sqlite3 *db = nullptr;
    explicit Db(const std::string &path, bool readonly = false)
    {
        if (sqlite3_open_v2(path.c_str(), &db,
                            readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                            nullptr) != SQLITE_OK)
        {
            sqlite3_close(db);
            db = nullptr;
            throw std::runtime_error("无法打开本地翻译数据库");
        }
        sqlite3_busy_timeout(db, readonly ? 25 : 500);
        if (!readonly && !EnsureSchema(db))
        {
            sqlite3_close(db);
            db = nullptr;
            throw std::runtime_error("无法初始化同步数据库");
        }
    }
    ~Db()
    {
        sqlite3_close(db);
    }
};
struct Stmt
{
    sqlite3_stmt *s = nullptr;
    Stmt(sqlite3 *db, const char *sql)
    {
        Check(sqlite3_prepare_v2(db, sql, -1, &s, nullptr));
    }
    ~Stmt()
    {
        sqlite3_finalize(s);
    }
    void Text(int i, const std::string &v)
    {
        Check(sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT));
    }
    void Int(int i, int64_t v)
    {
        Check(sqlite3_bind_int64(s, i, v));
    }
    std::string Text(int i)
    {
        const auto p = sqlite3_column_text(s, i);
        return p ? reinterpret_cast<const char *>(p) : "";
    }
    void Done()
    {
        Check(sqlite3_step(s));
    }
};
std::map<std::string, Record> Load(sqlite3 *db)
{
    std::map<std::string, Record> out;
    Stmt s(db, "SELECT "
               "source_text,target_language,translation,origin,source_character_set,context_key,provider,model,updated_"
               "at FROM translations");
    int rc;
    while ((rc = sqlite3_step(s.s)) == SQLITE_ROW)
    {
        Record r{s.Text(0), s.Text(1), s.Text(2),
                 s.Text(3), s.Text(4), s.Text(5),
                 s.Text(6), s.Text(7), sqlite3_column_int64(s.s, 8),
                 0};
        Stmt h(db, "SELECT hit_count FROM translation_usage WHERE identity=?");
        h.Text(1, J::array({r.source, r.language, r.charset, r.context}).dump());
        if (sqlite3_step(h.s) == SQLITE_ROW)
            r.hits = sqlite3_column_int64(h.s, 0);
        out[r.Id()] = r;
    }
    if (rc != SQLITE_DONE)
        Check(rc);
    return out;
}
std::map<std::string, Record> Shadows(sqlite3 *db)
{
    std::map<std::string, Record> out;
    Stmt s(db, "SELECT record_id,snapshot FROM translation_sync_shadow");
    int rc;
    while ((rc = sqlite3_step(s.s)) == SQLITE_ROW)
        out[s.Text(0)] = Record::Parse(J::parse(s.Text(1)));
    if (rc != SQLITE_DONE)
        Check(rc);
    return out;
}
void Save(sqlite3 *db, const Record &r)
{
    Stmt s(db, "INSERT INTO "
               "translations(source_text,target_language,source_character_set,context_key,translation,updated_at,"
               "origin,provider,model) VALUES(?,?,?,?,?,?,?,?,?) ON "
               "CONFLICT(source_text,target_language,source_character_set,context_key) DO UPDATE SET "
               "translation=excluded.translation,updated_at=excluded.updated_at,origin=excluded.origin,provider="
               "excluded.provider,model=excluded.model");
    s.Text(1, r.source);
    s.Text(2, r.language);
    s.Text(3, r.charset);
    s.Text(4, r.context);
    s.Text(5, r.text);
    s.Int(6, r.updated);
    s.Text(7, r.origin);
    s.Text(8, r.provider);
    s.Text(9, r.model);
    s.Done();
    Stmt h(
        db,
        "INSERT INTO translation_usage VALUES(?,?) ON CONFLICT(identity) DO UPDATE SET hit_count=excluded.hit_count");
    h.Text(1, J::array({r.source, r.language, r.charset, r.context}).dump());
    h.Int(2, r.hits);
    h.Done();
}
bool ContentChanged(const Record &a, const Record &b)
{
    return a.text != b.text || a.origin != b.origin || a.updated != b.updated;
}
void Meta(sqlite3 *db, const std::string &key, const std::string &v)
{
    Stmt s(db, "INSERT INTO translation_sync_meta VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    s.Text(1, key);
    s.Text(2, v);
    s.Done();
}
std::string Meta(sqlite3 *db, const std::string &key)
{
    Stmt s(db, "SELECT value FROM translation_sync_meta WHERE key=?");
    s.Text(1, key);
    return sqlite3_step(s.s) == SQLITE_ROW ? s.Text(0) : "";
}
std::string LocalDay(int64_t t)
{
    const time_t ts = t;
    tm result{};
    localtime_s(&result, &ts);
    char out[16]{};
    strftime(out, sizeof(out), "%Y-%m-%d", &result);
    return out;
}
std::atomic<bool> stopping{true}, enabled{true};
std::atomic<bool> authorizationBusy{false};
std::thread worker;
std::mutex waitMutex;
std::condition_variable wake;
} // namespace
std::string Record::Id() const
{
    const auto data = J::array({source, language, charset, context}).dump();
    unsigned char digest[32]{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(data.data())),
                   static_cast<ULONG>(data.size()), digest, 32) < 0)
        throw std::runtime_error("无法计算翻译记录 ID");
    const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (auto c : digest)
    {
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}
J Record::Json() const
{
    return J::array({source, language, text, origin, updated, hits, charset, context, Id(), provider, model});
}
Record Record::Parse(const J &j)
{
    return {j.at(0).get<std::string>(), j.at(1).get<std::string>(),  j.at(2).get<std::string>(),
            j.at(3).get<std::string>(), j.at(6).get<std::string>(),  j.at(7).get<std::string>(),
            j.at(9).get<std::string>(), j.at(10).get<std::string>(), j.at(4).get<int64_t>(),
            j.at(5).get<int64_t>()};
}
bool EnsureSchema(sqlite3 *db)
{
    // 保持原 translations 表列数，旧版回滚仍可读写；同步和命中数使用独立小表。
    return sqlite3_exec(
               db,
               "PRAGMA journal_mode=WAL;"
               "CREATE TABLE IF NOT EXISTS translation_meta(id INTEGER PRIMARY KEY CHECK(id=1),epoch INTEGER NOT "
               "NULL);INSERT OR IGNORE INTO translation_meta VALUES(1,0);"
               "CREATE TABLE IF NOT EXISTS translations(source_text TEXT NOT NULL,target_language TEXT NOT "
               "NULL,source_character_set TEXT NOT NULL,context_key TEXT NOT NULL,translation TEXT NOT NULL,updated_at "
               "INTEGER NOT NULL,origin TEXT NOT NULL DEFAULT 'ai',provider TEXT NOT NULL,model TEXT NOT NULL,PRIMARY "
               "KEY(source_text,target_language,source_character_set,context_key));"
               "CREATE TABLE IF NOT EXISTS translation_usage(identity TEXT PRIMARY KEY,hit_count INTEGER NOT NULL "
               "DEFAULT 0);"
               "CREATE TABLE IF NOT EXISTS translation_sync_shadow(record_id TEXT PRIMARY KEY,snapshot TEXT NOT NULL);"
               "CREATE TABLE IF NOT EXISTS translation_sync_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
               "CREATE TABLE IF NOT EXISTS translation_sync_conflicts(id INTEGER PRIMARY KEY,record_id TEXT NOT "
               "NULL,local_snapshot TEXT NOT NULL,remote_snapshot TEXT NOT NULL,created_at INTEGER NOT NULL);",
               nullptr, nullptr, nullptr) == SQLITE_OK;
}
void Hit(sqlite3 *db, const std::string &identity)
{
    // 仅在翻译后台线程记录命中，网络同步从不进入输入路径。
    sqlite3_stmt *s = nullptr;
    if (sqlite3_prepare_v2(
            db, "INSERT INTO translation_usage VALUES(?,1) ON CONFLICT(identity) DO UPDATE SET hit_count=hit_count+1",
            -1, &s, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text(s, 1, identity.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(s);
    }
    sqlite3_finalize(s);
}
Record Merge(const Record &l, const Record &r, const Record *shadow)
{
    auto remote = r;
    // 人工编辑 AI 行时通常不会更新时间/来源，快照差异仍按人工修改保护。
    if (shadow && r.text != shadow->text)
    {
        remote.origin = "user";
        remote.updated = std::max(r.updated, static_cast<int64_t>(time(nullptr)));
    }
    const bool lc = !shadow || ContentChanged(l, *shadow), rc = !shadow || ContentChanged(remote, *shadow);
    Record result = l;
    if (rc && (!lc || remote.origin == "user" || (l.origin != "user" && remote.updated >= l.updated)))
        result = remote;
    const auto base = shadow ? shadow->hits : 0;
    result.hits = std::max(l.hits, r.hits);
    if (shadow)
        result.hits = base + std::max<int64_t>(0, l.hits - base) + std::max<int64_t>(0, r.hits - base);
    return result;
}
void Synchronize(const std::string &path, const Transport &transport)
{
    Db db(path);
    Stmt epochQuery(db.db, "SELECT epoch FROM translation_meta WHERE id=1");
    if (sqlite3_step(epochQuery.s) != SQLITE_ROW)
        throw std::runtime_error("无法读取缓存纪元");
    const auto epoch = sqlite3_column_int64(epochQuery.s, 0);
    sqlite3_reset(epochQuery.s);
    auto local = Load(db.db), shadows = Shadows(db.db);
    const auto remote = transport.read();
    std::map<std::string, Record> merged;
    std::vector<Edit> edits;
    std::map<std::string, Record> remoteById;
    for (const auto &row : remote)
    {
        const auto id = row.record.Id();
        if (remoteById.count(id))
            throw std::runtime_error("表格存在重复记录，请合并重复行后重试；未写入表格");
        remoteById[id] = row.record;
        auto l = local.find(id), s = shadows.find(id);
        auto result =
            l == local.end() ? row.record : Merge(l->second, row.record, s == shadows.end() ? nullptr : &s->second);
        if (s != shadows.end() && row.record.text != s->second.text)
        {
            result.origin = "user";
            result.updated = std::max(result.updated, static_cast<int64_t>(time(nullptr)));
        }
        merged[id] = result;
        if (result.Json() != row.record.Json() || row.raw.at(8) == "" || row.raw.at(3) == "" || row.raw.at(6) == "" ||
            row.raw.at(4) == "")
            edits.push_back({row.row, result, row.raw});
    }
    for (const auto &[id, r] : local)
        if (!remoteById.count(id))
        {
            merged[id] = r;
            // 不把云端手动删除的旧行无条件复活；只有本地新记录/改动才追加。
            auto s = shadows.find(id);
            if (s == shadows.end() || r.Json() != s->second.Json())
                edits.push_back({0, r, J()});
        }
    // 所有网络操作发生在 SQLite 写事务之外。
    if (!edits.empty())
        transport.write(edits);
    Sql(db.db, "BEGIN IMMEDIATE");
    try
    {
        Stmt checkEpoch(db.db, "SELECT epoch FROM translation_meta WHERE id=1");
        if (sqlite3_step(checkEpoch.s) != SQLITE_ROW || sqlite3_column_int64(checkEpoch.s, 0) != epoch)
            throw std::runtime_error("同步期间本地缓存已清空，请再次同步");
        sqlite3_reset(checkEpoch.s);
        auto current = Load(db.db);
        for (auto &[id, r] : merged)
        {
            auto now = current.find(id), old = local.find(id);
            // 同步期间新增的 AI 结果或命中不能被旧快照吞掉，下次再上传。
            if (now != current.end() && old != local.end())
            {
                r.hits += std::max<int64_t>(0, now->second.hits - old->second.hits);
                if (ContentChanged(now->second, old->second))
                {
                    if (r.origin != "user")
                    {
                        r = now->second;
                    }
                    else
                    {
                        Stmt c(db.db, "INSERT INTO "
                                      "translation_sync_conflicts(record_id,local_snapshot,remote_snapshot,created_at) "
                                      "VALUES(?,?,?,strftime('%s','now'))");
                        c.Text(1, id);
                        c.Text(2, now->second.Json().dump());
                        c.Text(3, r.Json().dump());
                        c.Done();
                    }
                }
            }
            if (old != local.end() && old->second.text != r.text)
            {
                Stmt c(db.db,
                       "INSERT INTO translation_sync_conflicts(record_id,local_snapshot,remote_snapshot,created_at) "
                       "VALUES(?,?,?,strftime('%s','now'))");
                c.Text(1, id);
                c.Text(2, old->second.Json().dump());
                c.Text(3, r.Json().dump());
                c.Done();
            }
            const auto cloud =
                std::find_if(edits.begin(), edits.end(), [&](const Edit &e) { return e.record.Id() == id; });
            const auto snapshot = cloud != edits.end() ? cloud->record : (remoteById.count(id) ? remoteById.at(id) : r);
            if (now == current.end() || now->second.Json() != r.Json())
                Save(db.db, r);
            auto previous = shadows.find(id);
            if (previous == shadows.end() || previous->second.Json() != snapshot.Json())
            {
                Stmt s(db.db, "INSERT INTO translation_sync_shadow VALUES(?,?) ON CONFLICT(record_id) DO UPDATE SET "
                              "snapshot=excluded.snapshot");
                s.Text(1, id);
                s.Text(2, snapshot.Json().dump());
                s.Done();
            }
        }
        Meta(db.db, "last_success", std::to_string(time(nullptr)));
        Meta(db.db, "status", "同步成功（按记录合并）");
        Sql(db.db, "COMMIT");
    }
    catch (...)
    {
        sqlite3_exec(db.db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}
J Status(const std::string &path)
{
    J result = {{"translation_sync_last_time", "尚未同步"},
                {"translation_sync_status", "请导入 Google 桌面 OAuth 客户端并授权"},
                {"translation_sync_client", HasClient()},
                {"translation_sync_authorized", HasAuthorization()}};
    try
    {
        Db db(path, true);
        auto last = Meta(db.db, "last_success");
        if (!last.empty())
        {
            time_t ts = std::stoll(last);
            tm t{};
            localtime_s(&t, &ts);
            char out[32]{};
            strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &t);
            result["translation_sync_last_time"] = out;
        }
        auto state = Meta(db.db, "status");
        if (!state.empty())
            result["translation_sync_status"] = state;
    }
    catch (...)
    {
    }
    return result;
}
bool Action(const std::string &path, const std::string &action)
{
    if (action == "translation_sync_authorize" || action == "translation_sync_import_client")
    {
        // 两个设置宿主都立即返回。浏览器授权/选文件不占用输入或设置消息线程。
        if (authorizationBusy.exchange(true))
            return true;
        std::thread([path, action] {
            try
            {
                Db db(path);
                if (action == "translation_sync_import_client")
                {
                    if (ImportClient() && HasClient())
                        Meta(db.db, "status", "客户端已导入，请点击「授权 Google 账号」");
                }
                else
                {
                    Meta(db.db, "status", "等待浏览器 Google 授权…");
                    Authorize();
                    Meta(db.db, "status", "Google 授权成功，可立即同步");
                }
            }
            catch (const std::exception &e)
            {
                try
                {
                    Db db(path);
                    Meta(db.db, "status", e.what());
                }
                catch (...)
                {
                }
            }
            authorizationBusy = false;
            wake.notify_all();
        }).detach();
        return true;
    }
    try
    {
        Db db(path);
        if (action == "translation_sync_now")
        {
            Meta(db.db, "request", std::to_string(std::stoll("0" + Meta(db.db, "request")) + 1));
            Meta(db.db, "status", "已排队，等待后台同步…");
        }
        else
            return false;
        wake.notify_all();
        return true;
    }
    catch (const std::exception &e)
    {
        try
        {
            Db db(path);
            Meta(db.db, "status", e.what());
        }
        catch (...)
        {
        }
        return true;
    }
}
void Enable(bool value)
{
    enabled = value;
    wake.notify_all();
}
bool DailyDue(bool value, int64_t last_success, int64_t now)
{
    return value && (last_success == 0 || LocalDay(last_success) != LocalDay(now));
}
void Start(std::string path, bool value)
{
    if (!stopping.exchange(false))
        return;
    enabled = value;
    worker = std::thread([path = std::move(path)] {
        int64_t retry = 0;
        while (!stopping)
        {
            try
            {
                Db db(path);
                const auto requested = Meta(db.db, "request"), completed = Meta(db.db, "completed_request"),
                           last = Meta(db.db, "last_success");
                const bool manual = !requested.empty() && requested != completed;
                const auto now = static_cast<int64_t>(time(nullptr));
                const bool daily = now >= retry && DailyDue(enabled, last.empty() ? 0 : std::stoll(last), now);
                if (manual || (daily && HasAuthorization()))
                {
                    if (manual)
                        Meta(db.db, "completed_request", requested);
                    Meta(db.db, "status", "正在同步…");
                    try
                    {
                        if (!HasAuthorization())
                            throw std::runtime_error("尚未授权 Google：请先导入客户端 JSON，再点击授权");
                        Synchronize(path, GoogleTransport(stopping));
                    }
                    catch (const std::exception &e)
                    {
                        Meta(db.db, "status", e.what());
                        retry = now + 3600;
                    }
                }
            }
            catch (...)
            {
            }
            std::unique_lock lock(waitMutex);
            wake.wait_for(lock, std::chrono::seconds(2), [] { return stopping.load(); });
        }
    });
}
void Stop()
{
    stopping = true;
    wake.notify_all();
    if (worker.joinable())
        worker.join();
}
} // namespace TranslationSync
