#include "ai_translation.h"
#include "translation_sync.h"
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <memory>
#include <windows.h>

namespace AiTranslation
{
namespace
{
struct Database
{
    sqlite3 *db = nullptr;
    explicit Database(const std::string &path)
    {
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
        {
            sqlite3_close(db);
            db = nullptr;
            return;
        }
        sqlite3_busy_timeout(db, 500);
        if (!TranslationSync::EnsureSchema(db))
        {
            sqlite3_close(db);
            db = nullptr;
        }
    }
    ~Database()
    {
        if (db)
            sqlite3_close(db);
    }
    int64_t Epoch()
    {
        sqlite3_stmt *stmt = nullptr;
        int64_t epoch = -1;
        if (db &&
            sqlite3_prepare_v2(db, "SELECT epoch FROM translation_meta WHERE id=1", -1, &stmt, nullptr) == SQLITE_OK &&
            sqlite3_step(stmt) == SQLITE_ROW)
            epoch = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return epoch;
    }
    void BindKey(sqlite3_stmt *stmt, const Request &r)
    {
        const std::string context = ContextKey(r);
        sqlite3_bind_text(stmt, 1, r.source.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, r.target.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, r.character_set.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, context.c_str(), -1, SQLITE_TRANSIENT);
    }
    std::string Find(const Request &r)
    {
        sqlite3_stmt *stmt = nullptr;
        std::string text;
        Request matched = r;
        if (db &&
            sqlite3_prepare_v2(
                db,
                "SELECT translation,source_character_set FROM translations WHERE source_text=? AND target_language=? "
                "AND (source_character_set=? OR context_key='') AND context_key=?",
                -1, &stmt, nullptr) == SQLITE_OK)
        {
            BindKey(stmt, r);
            if (sqlite3_step(stmt) == SQLITE_ROW)
            {
                text = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
                matched.character_set = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            }
        }
        sqlite3_finalize(stmt);
        if (!text.empty())
            TranslationSync::Hit(db, Identity(matched));
        return text;
    }
    void Store(const Request &r, const std::string &text, int64_t epoch)
    {
        sqlite3_stmt *stmt = nullptr;
        // 清库与插入的纪元检查在同一条 SQL 内，跨进程清库不会被旧请求重新填回。
        if (db && epoch >= 0 &&
            sqlite3_prepare_v2(db,
                               "INSERT INTO translations SELECT ?,?,?,?,?,strftime('%s','now'),'ai',?,? "
                               "WHERE (SELECT epoch FROM translation_meta WHERE id=1)=? ON "
                               "CONFLICT(source_text,target_language,source_character_set,context_key) DO UPDATE SET "
                               "translation=excluded.translation,updated_at=excluded.updated_at,origin=excluded.origin,"
                               "provider=excluded.provider,model=excluded.model WHERE translations.origin<>'user'",
                               -1, &stmt, nullptr) == SQLITE_OK)
        {
            BindKey(stmt, r);
            sqlite3_bind_text(stmt, 5, text.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, r.config.provider.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 7, r.config.model.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 8, epoch);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }
};
size_t ChineseCount(const std::string &s)
{
    size_t count = 0;
    for (size_t i = 0; i < s.size();)
    {
        unsigned char c = s[i++];
        uint32_t cp = c;
        int more = 0;
        if (c >= 0xF0)
        {
            cp = c & 7;
            more = 3;
        }
        else if (c >= 0xE0)
        {
            cp = c & 15;
            more = 2;
        }
        else if (c >= 0xC0)
        {
            cp = c & 31;
            more = 1;
        }
        if (i + more > s.size())
            return 0;
        while (more--)
        {
            unsigned char next = s[i++];
            if ((next & 0xC0) != 0x80)
                return 0;
            cp = (cp << 6) | (next & 63);
        }
        if ((cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0x20000 && cp <= 0x323AF))
            ++count;
    }
    return count;
}
} // namespace
bool IsChineseSource(const std::string &source)
{
    return !source.empty() && source.size() <= 4096 && ChineseCount(source) > 0;
}
bool IsReadingSource(const std::string &source)
{
    if (source.empty() || source.size() > 16384)
        return false;
    int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(), static_cast<int>(source.size()), nullptr, 0);
    if (!size)
        return false;
    std::wstring text(size, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(), static_cast<int>(source.size()), text.data(),
                        size);
    int letters = 0;
    for (wchar_t c : text)
    {
        WORD kind = 0;
        GetStringTypeW(CT_CTYPE1, &c, 1, &kind);
        if (kind & C1_ALPHA)
            ++letters;
        if (c < 32 && c != L'\n' && c != L'\r' && c != L'\t')
            return false;
    }
    return letters >= 2;
}
bool IsValidSource(const Request &r)
{
    return r.reading ? IsReadingSource(r.source) : IsChineseSource(r.source);
}
std::string ContextKey(const Request &r)
{
    const auto count = ChineseCount(r.source);
    // 完整问题和明确名词不因上屏历史变化而重新收费；省略句、指代和常见多义短语才带上下文。
    if (count <= 1)
        return r.context;
    if (count <= 12)
        for (const auto word :
             {u8"这个", u8"那个", u8"这里", u8"那里", u8"这样", u8"那样", u8"它", u8"他", u8"她", u8"这边", u8"那边"})
            if (r.source.find(word) != std::string::npos)
                return r.context;
    if (count <= 4)
        for (const auto word : {u8"行", u8"好", u8"可以", u8"方便", u8"东西", u8"没关系", u8"没事", u8"再说", u8"还是",
                                u8"苹果", u8"会", u8"打", u8"开", u8"关", u8"来", u8"去", u8"上", u8"下"})
            if (r.source.find(word) != std::string::npos)
                return r.context;
    return {};
}
std::string Identity(const Request &r)
{
    return nlohmann::json::array({r.source, r.target, r.character_set, ContextKey(r)}).dump();
}
bool ClearCache(const std::string &path)
{
    Database db(path);
    if (!db.db)
        return false;
    if (sqlite3_exec(db.db,
                     "BEGIN IMMEDIATE;DELETE FROM translations;DELETE FROM translation_usage;UPDATE translation_meta "
                     "SET epoch=epoch+1 WHERE id=1;COMMIT;",
                     nullptr, nullptr, nullptr) == SQLITE_OK)
        return true;
    sqlite3_exec(db.db, "ROLLBACK", nullptr, nullptr, nullptr);
    return false;
}
Worker::~Worker()
{
    Stop();
}
void Worker::Start(std::string path, Callback callback, Fetcher fetcher, std::chrono::milliseconds delay)
{
    if (running_)
        return;
    path_ = std::move(path);
    callback_ = std::move(callback);
    fetcher_ = std::move(fetcher);
    delay_ = delay;
    running_ = true;
    thread_ = std::thread(&Worker::Run, this);
}
uint64_t Worker::Submit(Request r)
{
    std::lock_guard lock(mutex_);
    latest_ = std::move(r);
    changed_ = std::chrono::steady_clock::now();
    const auto generation = ++generation_;
    cv_.notify_one();
    return generation;
}
void Worker::Cancel()
{
    Submit({});
}
void Worker::Stop()
{
    if (!running_.exchange(false))
        return;
    ++generation_;
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}
bool Worker::IsCurrent(uint64_t generation) const
{
    return running_ && generation_.load() == generation;
}
void Worker::Run()
{
    uint64_t observed = 0;
    while (running_)
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return !running_ || observed != generation_.load(); });
        if (!running_)
            break;
        observed = generation_;
        Request r = latest_;
        if (!r.config.translation_enabled || !IsValidSource(r))
            continue;
        lock.unlock();
        // 数据库打开、查缓存和网络都只在后台线程；输入线程只提交快照。
        Database db(path_);
        const int64_t epoch = db.Epoch();
        std::string text = db.Find(r);
        bool hit = !text.empty();
        if (!hit)
        {
            lock.lock();
            if (cv_.wait_until(lock, changed_ + delay_, [&] { return !IsCurrent(observed); }))
                continue;
            lock.unlock();
            if (!IsCurrent(observed))
                continue;
            const uint64_t generation = observed;
            try
            {
                text = fetcher_(r, [this, generation] { return !IsCurrent(generation); });
            }
            catch (...)
            {
                text.clear();
            }
            if (!text.empty() && IsCurrent(observed))
                db.Store(r, text, epoch);
        }
        if (IsCurrent(observed) && db.Epoch() == epoch && callback_)
            callback_({std::move(r), std::move(text), observed, hit});
    }
}
Worker &Instance()
{
    static Worker worker;
    return worker;
}
} // namespace AiTranslation
