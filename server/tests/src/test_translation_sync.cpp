#include "ai/translation_sync.h"
#include "tests/includes/test_framework.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <map>
#include <thread>
using namespace TranslationSync;
namespace
{
Record Sample()
{
    return {u8"你今天上班了吗？", "en", "Did you work today?", "ai", "simplified", "", "openai", "test", 100, 2};
}
struct Fixture
{
    std::string path;
    sqlite3 *db = nullptr;
    std::vector<RemoteRow> cloud;
    int writes = 0, reads = 0;
    Fixture()
    {
        static int id = 0;
        path = (std::filesystem::temp_directory_path() /
                ("msime-sync-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++id) + ".db"))
                   .u8string();
        REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
        REQUIRE(EnsureSchema(db));
    }
    ~Fixture()
    {
        sqlite3_close(db);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path + "-wal", ec);
        std::filesystem::remove(path + "-shm", ec);
    }
    void Save(Record r)
    {
        sqlite3_stmt *s = nullptr;
        REQUIRE(sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO translations VALUES(?,?,?,?,?,?,?,?,?)", -1, &s,
                                   nullptr) == SQLITE_OK);
        const std::string values[] = {r.source, r.language, r.charset, r.context, r.text};
        for (int i = 0; i < 5; ++i)
            sqlite3_bind_text(s, i + 1, values[i].c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 6, r.updated);
        sqlite3_bind_text(s, 7, r.origin.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 8, r.provider.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 9, r.model.c_str(), -1, SQLITE_TRANSIENT);
        REQUIRE(sqlite3_step(s) == SQLITE_DONE);
        sqlite3_finalize(s);
    }
    std::string Text()
    {
        sqlite3_stmt *s = nullptr;
        sqlite3_prepare_v2(db, "SELECT translation FROM translations LIMIT 1", -1, &s, nullptr);
        std::string out;
        if (sqlite3_step(s) == SQLITE_ROW)
            out = reinterpret_cast<const char *>(sqlite3_column_text(s, 0));
        sqlite3_finalize(s);
        return out;
    }
    Transport Api()
    {
        return {[this] {
                    ++reads;
                    return cloud;
                },
                [this](const std::vector<Edit> &edits) {
                    ++writes;
                    for (const auto &e : edits)
                    {
                        auto row =
                            std::find_if(cloud.begin(), cloud.end(), [&](const auto &r) { return r.row == e.row; });
                        if (row == cloud.end())
                            cloud.push_back({e.record, static_cast<int>(cloud.size() + 2), e.record.Json()});
                        else
                        {
                            row->record = e.record;
                            row->raw = e.record.Json();
                        }
                    }
                }};
    }
};
} // namespace
TEST_CASE(translation_sync_upload_once_no_whole_sheet_overwrite)
{
    Fixture f;
    f.Save(Sample());
    Synchronize(f.path, f.Api());
    REQUIRE_EQ(f.writes, 1);
    REQUIRE_EQ(f.cloud.size(), size_t(1));
    Synchronize(f.path, f.Api());
    REQUIRE_EQ(f.writes, 1);
    REQUIRE_EQ(f.Text(), Sample().text);
}
TEST_CASE(translation_sync_manual_cloud_edit_without_timestamp_wins)
{
    Fixture f;
    f.Save(Sample());
    Synchronize(f.path, f.Api());
    f.cloud[0].record.text = "Are you working today?";
    f.cloud[0].raw = f.cloud[0].record.Json();
    auto local = Sample();
    local.text = "Did you go to work today?";
    local.updated = 200;
    f.Save(local);
    Synchronize(f.path, f.Api());
    REQUIRE_EQ(f.Text(), "Are you working today?");
    REQUIRE_EQ(f.cloud[0].record.origin, "user");
    const int calls = f.writes;
    Synchronize(f.path, f.Api());
    REQUIRE_EQ(f.writes, calls);
    sqlite3_stmt *s = nullptr;
    sqlite3_prepare_v2(f.db, "SELECT COUNT(*) FROM translation_sync_conflicts", -1, &s, nullptr);
    REQUIRE(sqlite3_step(s) == SQLITE_ROW);
    REQUIRE(sqlite3_column_int(s, 0) > 0);
    sqlite3_finalize(s);
}
TEST_CASE(translation_sync_remote_new_manual_record_import)
{
    Fixture f;
    auto r = Sample();
    r.origin = "user";
    f.cloud.push_back({r, 2, r.Json()});
    Synchronize(f.path, f.Api());
    REQUIRE_EQ(f.Text(), r.text);
    REQUIRE_EQ(f.writes, 0);
}
TEST_CASE(translation_sync_network_failure_keeps_local_and_no_success_marker)
{
    Fixture f;
    f.Save(Sample());
    auto api = f.Api();
    api.write = [](const auto &) { throw std::runtime_error("network error"); };
    bool failed = false;
    try
    {
        Synchronize(f.path, api);
    }
    catch (...)
    {
        failed = true;
    }
    REQUIRE(failed);
    REQUIRE_EQ(f.Text(), Sample().text);
    const auto status = Status(f.path);
    REQUIRE_EQ(status["translation_sync_last_time"], "尚未同步");
}
TEST_CASE(translation_sync_duplicate_rows_abort_without_write)
{
    Fixture f;
    auto r = Sample();
    f.cloud = {{r, 2, r.Json()}, {r, 3, r.Json()}};
    bool failed = false;
    try
    {
        Synchronize(f.path, f.Api());
    }
    catch (...)
    {
        failed = true;
    }
    REQUIRE(failed);
    REQUIRE_EQ(f.writes, 0);
    REQUIRE_EQ(f.Text(), "");
}
TEST_CASE(translation_sync_context_charset_language_identity_distinct)
{
    auto a = Sample(), b = a;
    b.context = "context";
    REQUIRE(a.Id() != b.Id());
    b = a;
    b.charset = "traditional";
    REQUIRE(a.Id() != b.Id());
    b = a;
    b.language = "ja";
    REQUIRE(a.Id() != b.Id());
    REQUIRE_EQ(Record::Parse(a.Json()).Id(), a.Id());
}
TEST_CASE(translation_sync_hit_counts_merge_deltas_without_recount)
{
    auto base = Sample(), l = base, r = base;
    l.hits = 5;
    r.hits = 4;
    auto result = Merge(l, r, &base);
    REQUIRE_EQ(result.hits, 7);
    REQUIRE_EQ(Merge(result, result, &result).hits, 7);
}
TEST_CASE(translation_sync_local_manual_protected_from_cloud_ai)
{
    auto l = Sample(), r = l;
    l.origin = "user";
    l.text = "Human translation";
    r.updated = 1000;
    r.text = "AI rewrite";
    REQUIRE_EQ(Merge(l, r, nullptr).text, l.text);
}
TEST_CASE(translation_sync_ai_result_created_during_network_is_preserved)
{
    Fixture f;
    auto original = Sample();
    f.Save(original);
    auto api = f.Api();
    auto write = api.write;
    api.write = [&](const auto &edits) {
        auto newer = original;
        newer.text = "Newer local result";
        newer.updated = 200;
        f.Save(newer);
        write(edits);
    };
    Synchronize(f.path, api);
    REQUIRE_EQ(f.Text(), "Newer local result");
    Synchronize(f.path, f.Api());
    REQUIRE_EQ(f.cloud[0].record.text, "Newer local result");
}
TEST_CASE(translation_sync_clear_during_network_does_not_repopulate_cache)
{
    Fixture f;
    f.Save(Sample());
    auto api = f.Api();
    auto write = api.write;
    api.write = [&](const auto &edits) {
        REQUIRE(sqlite3_exec(f.db, "DELETE FROM translations;UPDATE translation_meta SET epoch=epoch+1 WHERE id=1",
                             nullptr, nullptr, nullptr) == SQLITE_OK);
        write(edits);
    };
    bool failed = false;
    try
    {
        Synchronize(f.path, api);
    }
    catch (...)
    {
        failed = true;
    }
    REQUIRE(failed);
    REQUIRE_EQ(f.Text(), "");
}
TEST_CASE(translation_sync_iso_cloud_rows_do_not_trigger_repeated_upload)
{
    Fixture f;
    f.Save(Sample());
    Synchronize(f.path, f.Api());
    f.cloud[0].raw[4] = "1970-01-01T00:01:40Z";
    const auto calls = f.writes;
    Synchronize(f.path, f.Api());
    REQUIRE_EQ(f.writes, calls);
}
TEST_CASE(translation_sync_daily_once_disabled_and_restart)
{
    const int64_t now = 1791086400;
    REQUIRE(DailyDue(true, 0, now));
    REQUIRE(!DailyDue(false, 0, now));
    REQUIRE(!DailyDue(true, now, now + 60));
    REQUIRE(DailyDue(true, now, now + 86400));
}
