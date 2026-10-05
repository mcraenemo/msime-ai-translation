#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include "statistics/stats_overview.h"
#include "statistics/stats_store.h"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using MsimeStats::Batch;
using MsimeStats::DailyRow;
using MsimeStats::DayDelta;
using MsimeStats::HourDelta;
using MsimeStats::HourlyRow;
using MsimeStats::Retention;
using MsimeStats::Store;

// The directory name is deliberately non-ASCII: every store path crosses the
// wide/narrow boundary into SQLite, and the profile directory can contain
// Chinese on a real machine.
class ScopedTempDir
{
  public:
    ScopedTempDir()
    {
        root_ = fs::temp_directory_path() / L"msime-统计测试-存储";
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);
        REQUIRE(!ec);
    }

    ~ScopedTempDir()
    {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    ScopedTempDir(const ScopedTempDir &) = delete;
    ScopedTempDir &operator=(const ScopedTempDir &) = delete;

    const fs::path &Root() const
    {
        return root_;
    }

  private:
    fs::path root_;
};

std::unique_ptr<Store> OpenStore(const ScopedTempDir &temp, fs::path &db_path)
{
    db_path = temp.Root() / L"stats.db";
    std::string error;
    std::unique_ptr<Store> store = Store::Open(db_path, error);
    REQUIRE(store != nullptr);
    return store;
}

int DayKey(int year, int month, int day)
{
    return year * 10000 + month * 100 + day;
}

bool HasDay(const std::vector<DailyRow> &rows, int day_key)
{
    return std::any_of(rows.begin(), rows.end(), [day_key](const DailyRow &row) { return row.day_key == day_key; });
}

bool HasHour(const std::vector<HourlyRow> &rows, int day_key, int hour)
{
    return std::any_of(rows.begin(), rows.end(),
                       [day_key, hour](const HourlyRow &row) { return row.day_key == day_key && row.hour == hour; });
}

class RawDb
{
  public:
    explicit RawDb(const fs::path &path)
    {
        if (sqlite3_open_v2(test::Utf8(path).c_str(), &db_, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
        {
            if (db_ != nullptr)
            {
                sqlite3_close(db_);
                db_ = nullptr;
            }
            throw std::runtime_error("Requirement failed: raw sqlite connection");
        }
    }

    ~RawDb()
    {
        if (db_ != nullptr)
        {
            sqlite3_close(db_);
        }
    }

    RawDb(const RawDb &) = delete;
    RawDb &operator=(const RawDb &) = delete;

    sqlite3 *get() const
    {
        return db_;
    }

  private:
    sqlite3 *db_ = nullptr;
};
} // namespace

TEST_CASE(stats_store_upsert_accumulates_and_maintains_first_day)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    std::string error;
    int first = 0;
    bool present = false;
    REQUIRE(store->FirstDay(first, present, error));
    REQUIRE(!present);

    Batch first_batch;
    first_batch.days.push_back(DayDelta{20260920, 10, 2, 0, 0, 0, 5000});
    first_batch.days.push_back(DayDelta{20260918, 1, 0, 1, 0, 0, 1000});
    first_batch.hours.push_back(HourDelta{20260920, 9, 12, 5000});
    REQUIRE(store->Upsert(first_batch, error));

    Batch second_batch;
    second_batch.days.push_back(DayDelta{20260920, 5, 0, 0, 1, 0, 2000});
    second_batch.days.push_back(DayDelta{20260901, 0, 0, 0, 0, 3, 300});
    second_batch.hours.push_back(HourDelta{20260920, 9, 6, 2000});
    second_batch.hours.push_back(HourDelta{20260920, 10, 6, 2000});
    REQUIRE(store->Upsert(second_batch, error));

    std::vector<DailyRow> daily;
    REQUIRE(store->DailyRows(daily, error));
    REQUIRE_EQ(daily.size(), 3u);
    // Rows come back ascending.
    REQUIRE_EQ(daily[0].day_key, 20260901);
    REQUIRE_EQ(daily[2].day_key, 20260920);
    REQUIRE_EQ(daily[2].cjk, 15);
    REQUIRE_EQ(daily[2].latin, 2);
    REQUIRE_EQ(daily[2].punct, 1);
    REQUIRE_EQ(daily[2].active_ms, 7000);

    REQUIRE(store->FirstDay(first, present, error));
    REQUIRE(present);
    REQUIRE_EQ(first, 20260901);

    std::vector<HourlyRow> hourly;
    REQUIRE(store->HourlyRows(20260920, hourly, error));
    REQUIRE_EQ(hourly.size(), 2u);
    REQUIRE_EQ(hourly[0].hour, 9);
    REQUIRE_EQ(hourly[0].chars, 18);
    REQUIRE_EQ(hourly[1].hour, 10);
    REQUIRE_EQ(hourly[1].chars, 6);
}

// SQLite must not invent the parent directory: a broken DataDir has to surface as
// a failed open with a message, so the caller can tell "no statistics yet" (absent
// file) apart from "the data directory is unusable" (absent parent).
TEST_CASE(stats_store_open_without_parent_directory_fails)
{
    ScopedTempDir temp;
    const fs::path missing = temp.Root() / L"absent" / L"stats.db";

    std::string error;
    std::unique_ptr<Store> store = Store::Open(missing, error);
    REQUIRE(store == nullptr);
    REQUIRE(!error.empty());
    REQUIRE(!fs::exists(missing));
}

TEST_CASE(stats_store_upsert_empty_batch_is_noop)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    std::string error;
    REQUIRE(store->Upsert(Batch{}, error));
    int first = 0;
    bool present = false;
    REQUIRE(store->FirstDay(first, present, error));
    REQUIRE(!present);
}

TEST_CASE(stats_store_persistence_across_reopen)
{
    ScopedTempDir temp;
    const fs::path db_path = temp.Root() / L"stats.db";

    {
        std::string error;
        std::unique_ptr<Store> store = Store::Open(db_path, error);
        REQUIRE(store != nullptr);
        Batch batch;
        batch.days.push_back(DayDelta{20260920, 42, 0, 0, 0, 0, 1234});
        REQUIRE(store->Upsert(batch, error));
    }

    std::string error;
    std::unique_ptr<Store> reopened = Store::Open(db_path, error);
    REQUIRE(reopened != nullptr);
    std::vector<DailyRow> daily;
    REQUIRE(reopened->DailyRows(daily, error));
    REQUIRE_EQ(daily.size(), 1u);
    REQUIRE_EQ(daily[0].cjk, 42);
    REQUIRE_EQ(daily[0].active_ms, 1234);

    std::string value;
    bool present = false;
    REQUIRE(reopened->Meta(MsimeStats::kStoreMetaSchemaVersion, value, present, error));
    REQUIRE(present);
    REQUIRE_EQ(value, std::string(MsimeStats::kStoreSchemaVersion));
}

TEST_CASE(stats_store_clear_through_boundaries)
{
    const int today = DayKey(2026, 9, 20);
    struct Case
    {
        Retention retention;
        int keep_days;
    };
    const std::vector<Case> cases = {
        {Retention::Days30, 30},
        {Retention::Days90, 90},
        {Retention::Days180, 180},
        {Retention::Days365, 365},
    };

    for (const Case &test_case : cases)
    {
        ScopedTempDir temp;
        fs::path db_path;
        std::unique_ptr<Store> store = OpenStore(temp, db_path);

        // One row per day across a window wider than any retention, seeded in a
        // single batch to keep the test quick.
        Batch seed;
        seed.days.reserve(400);
        seed.hours.reserve(400);
        for (int index = 0; index < 400; ++index)
        {
            const int key = MsimeStats::AddDays(today, -index);
            seed.days.push_back(DayDelta{key, 1, 0, 0, 0, 0, 1000});
            seed.hours.push_back(HourDelta{key, 12, 1, 1000});
        }
        std::string error;
        REQUIRE(store->Upsert(seed, error));

        int cutoff = 0;
        REQUIRE(MsimeStats::RetentionCutoff(test_case.retention, today, cutoff));
        int64_t removed = 0;
        REQUIRE(store->ClearThrough(cutoff, removed, error));
        REQUIRE_EQ(removed, static_cast<int64_t>(400 - test_case.keep_days));

        const int kept = MsimeStats::AddDays(today, -(test_case.keep_days - 1));
        int first = 0;
        bool present = false;
        REQUIRE(store->FirstDay(first, present, error));
        REQUIRE(present);
        REQUIRE_EQ(first, kept);

        // The boundary day survives, the day before it does not.
        std::vector<DailyRow> daily;
        REQUIRE(store->DailyRows(daily, error));
        REQUIRE(HasDay(daily, kept));
        REQUIRE(!HasDay(daily, MsimeStats::AddDays(kept, -1)));

        // Hour buckets follow the same boundary.
        std::vector<HourlyRow> hourly;
        REQUIRE(store->HourlyRows(kept, hourly, error));
        REQUIRE(HasHour(hourly, kept, 12));
        hourly.clear();
        REQUIRE(store->HourlyRows(MsimeStats::AddDays(kept, -1), hourly, error));
        REQUIRE(hourly.empty());
    }
}

TEST_CASE(stats_store_clear_forever_deletes_nothing)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    Batch seed;
    seed.days.reserve(500);
    for (int index = 0; index < 500; ++index)
    {
        seed.days.push_back(DayDelta{MsimeStats::AddDays(today, -index), 1, 0, 0, 0, 0, 0});
    }
    std::string error;
    REQUIRE(store->Upsert(seed, error));

    int cutoff = 0;
    REQUIRE(!MsimeStats::RetentionCutoff(Retention::Forever, today, cutoff));
    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE_EQ(rows.size(), 500u);
}

TEST_CASE(stats_store_clear_to_empty_resets_first_day)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    Batch seed;
    seed.days.push_back(DayDelta{today, 5, 0, 0, 0, 0, 0});
    seed.hours.push_back(HourDelta{today, 8, 5, 0});
    std::string error;
    REQUIRE(store->Upsert(seed, error));

    int cutoff = 0;
    REQUIRE(MsimeStats::RetentionCutoff(Retention::Days30, today, cutoff));
    int64_t removed = 0;
    REQUIRE(store->ClearThrough(cutoff, removed, error));
    REQUIRE_EQ(removed, 0);

    REQUIRE(store->ClearThrough(MsimeStats::AddDays(today, 1), removed, error));
    REQUIRE_EQ(removed, 1);

    int first = 0;
    bool present = false;
    REQUIRE(store->FirstDay(first, present, error));
    REQUIRE(!present);

    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE(rows.empty());
    std::vector<HourlyRow> hourly;
    REQUIRE(store->HourlyRows(today, hourly, error));
    REQUIRE(hourly.empty());
}

// The privacy contract: the on-disk schema must not be able to store text.
TEST_CASE(stats_store_schema_has_no_text_columns)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);
    RawDb raw(db_path);

    sqlite3_stmt *statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw.get(),
                               "SELECT name FROM pragma_table_info('stats_daily') "
                               "UNION ALL SELECT name FROM pragma_table_info('stats_hourly')",
                               -1, &statement, nullptr) == SQLITE_OK);
    const std::vector<std::string> allowed = {"day_key", "hour",  "cjk",   "latin",    "digit",
                                              "punct",   "other", "chars", "active_ms"};
    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW)
    {
        const char *name = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
        REQUIRE(name != nullptr);
        REQUIRE(std::find(allowed.begin(), allowed.end(), std::string(name)) != allowed.end());
    }
    REQUIRE_EQ(step, SQLITE_DONE);
    sqlite3_finalize(statement);

    // WAL is a database property, so a second connection can assert it too.
    statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw.get(), "PRAGMA journal_mode", -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE_EQ(sqlite3_step(statement), SQLITE_ROW);
    const char *mode = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
    REQUIRE_EQ(std::string(mode != nullptr ? mode : ""), std::string("wal"));
    sqlite3_finalize(statement);
}

// busy_timeout must make a write wait for a foreign writer instead of failing
// immediately; with WAL both processes can otherwise work concurrently.
TEST_CASE(stats_store_waits_for_a_cross_process_writer)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    sqlite3 *blocker = nullptr;
    REQUIRE(sqlite3_open_v2(test::Utf8(db_path).c_str(), &blocker, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(blocker, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);

    std::atomic<bool> started{false};
    bool upsert_ok = false;
    std::string upsert_error;
    int64_t waited_ms = 0;
    std::thread writer([&]() {
        started.store(true);
        const auto begin = std::chrono::steady_clock::now();
        Batch batch;
        batch.days.push_back(DayDelta{DayKey(2026, 9, 20), 1, 0, 0, 0, 0, 0});
        upsert_ok = store->Upsert(batch, upsert_error);
        waited_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
    });
    while (!started.load())
    {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(sqlite3_exec(blocker, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK);
    writer.join();

    REQUIRE(upsert_ok);
    REQUIRE(waited_ms >= 200);
    sqlite3_close(blocker);

    std::vector<DailyRow> rows;
    std::string error;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE_EQ(rows.size(), 1u);
}

TEST_CASE(stats_store_overview_reads_through_queries)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    Batch batch;
    batch.days.push_back(DayDelta{20260919, 10, 0, 0, 0, 0, 60'000});
    batch.days.push_back(DayDelta{today, 20, 5, 0, 0, 0, 120'000});
    batch.hours.push_back(HourDelta{today, 14, 25, 120'000});
    std::string error;
    REQUIRE(store->Upsert(batch, error));

    std::vector<DailyRow> daily;
    std::vector<HourlyRow> hourly;
    REQUIRE(store->DailyRows(daily, error));
    REQUIRE(store->HourlyRows(today, hourly, error));
    const MsimeStats::Overview overview = MsimeStats::ComputeOverview(daily, hourly, today);
    REQUIRE(overview.has_data);
    REQUIRE_EQ(overview.days, 2);
    REQUIRE_EQ(overview.total_chars, 35);
    REQUIRE_EQ(overview.today_chars, 25);
    REQUIRE_EQ(overview.current_streak, 2);
    REQUIRE_EQ(overview.today_hourly[14], 25);
}

TEST_CASE(stats_store_parse_retention)
{
    Retention retention = Retention::Forever;
    REQUIRE(MsimeStats::ParseRetention("30d", retention));
    REQUIRE(retention == Retention::Days30);
    REQUIRE(MsimeStats::ParseRetention("90d", retention));
    REQUIRE(retention == Retention::Days90);
    REQUIRE(MsimeStats::ParseRetention("180d", retention));
    REQUIRE(retention == Retention::Days180);
    REQUIRE(MsimeStats::ParseRetention("365d", retention));
    REQUIRE(retention == Retention::Days365);
    REQUIRE(MsimeStats::ParseRetention("forever", retention));
    REQUIRE(retention == Retention::Forever);
    REQUIRE(!MsimeStats::ParseRetention("2d", retention));
    REQUIRE(!MsimeStats::ParseRetention("", retention));
}

// The retention marker is the whole idempotence mechanism: the first call of a
// day runs the policy, every later call that day only reads the marker, and a
// new day runs it again. No timer is involved.
TEST_CASE(stats_store_retention_runs_once_per_local_day)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    Batch seed;
    seed.days.reserve(400);
    for (int index = 0; index < 400; ++index)
    {
        seed.days.push_back(DayDelta{MsimeStats::AddDays(today, -index), 1, 0, 0, 0, 0, 1000});
    }
    std::string error;
    REQUIRE(store->Upsert(seed, error));
    // Yesterday's marker: today has not run yet.
    REQUIRE(
        store->SetMeta(MsimeStats::kStoreMetaLastRetentionDay, std::to_string(MsimeStats::AddDays(today, -1)), error));

    MsimeStats::RetentionOutcome outcome;
    REQUIRE(MsimeStats::RunDailyRetention(*store, Retention::Days30, today, outcome, error));
    REQUIRE(!outcome.already_ran_today);
    REQUIRE_EQ(outcome.removed_days, static_cast<int64_t>(400 - 30));

    std::string marker;
    bool present = false;
    REQUIRE(store->Meta(MsimeStats::kStoreMetaLastRetentionDay, marker, present, error));
    REQUIRE(present);
    REQUIRE_EQ(marker, std::to_string(today));

    // A second call on the same day must not delete anything - not even a row
    // that just arrived with an expired day key.
    Batch late;
    late.days.push_back(DayDelta{MsimeStats::AddDays(today, -200), 1, 0, 0, 0, 0, 1000});
    REQUIRE(store->Upsert(late, error));
    REQUIRE(MsimeStats::RunDailyRetention(*store, Retention::Days30, today, outcome, error));
    REQUIRE(outcome.already_ran_today);
    REQUIRE_EQ(outcome.removed_days, 0);
    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE(HasDay(rows, MsimeStats::AddDays(today, -200)));

    // The next local day runs the policy again and clears the late row.
    const int tomorrow = MsimeStats::AddDays(today, 1);
    REQUIRE(MsimeStats::RunDailyRetention(*store, Retention::Days30, tomorrow, outcome, error));
    REQUIRE(!outcome.already_ran_today);
    rows.clear();
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE(!HasDay(rows, MsimeStats::AddDays(today, -200)));
}

// Forever is not "skip the day": it still records the marker, so a later
// switch to a finite window starts cleaning from the next write.
TEST_CASE(stats_store_retention_forever_records_day_without_deleting)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    Batch seed;
    seed.days.reserve(500);
    for (int index = 0; index < 500; ++index)
    {
        seed.days.push_back(DayDelta{MsimeStats::AddDays(today, -index), 1, 0, 0, 0, 0, 0});
    }
    std::string error;
    REQUIRE(store->Upsert(seed, error));

    MsimeStats::RetentionOutcome outcome;
    REQUIRE(MsimeStats::RunDailyRetention(*store, Retention::Forever, today, outcome, error));
    REQUIRE(!outcome.already_ran_today);
    REQUIRE_EQ(outcome.removed_days, 0);

    std::string marker;
    bool present = false;
    REQUIRE(store->Meta(MsimeStats::kStoreMetaLastRetentionDay, marker, present, error));
    REQUIRE(present);
    REQUIRE_EQ(marker, std::to_string(today));

    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE_EQ(rows.size(), 500u);

    REQUIRE(MsimeStats::RunDailyRetention(*store, Retention::Forever, today, outcome, error));
    REQUIRE(outcome.already_ran_today);
    REQUIRE_EQ(outcome.removed_days, 0);
}

// The window includes today and the days back to today-(days-1); the first day
// outside it is removed, together with its hour buckets and the first_day key.
TEST_CASE(stats_store_retention_boundary_keeps_today_and_window_start)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    const int window_start = MsimeStats::AddDays(today, -29);
    const int expired = MsimeStats::AddDays(today, -30);
    Batch seed;
    seed.days.push_back(DayDelta{expired, 1, 0, 0, 0, 0, 0});
    seed.days.push_back(DayDelta{window_start, 1, 0, 0, 0, 0, 0});
    seed.days.push_back(DayDelta{today, 1, 0, 0, 0, 0, 0});
    seed.hours.push_back(HourDelta{expired, 9, 1, 0});
    seed.hours.push_back(HourDelta{window_start, 9, 1, 0});
    std::string error;
    REQUIRE(store->Upsert(seed, error));

    MsimeStats::RetentionOutcome outcome;
    REQUIRE(MsimeStats::RunDailyRetention(*store, Retention::Days30, today, outcome, error));
    REQUIRE_EQ(outcome.removed_days, 1);

    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE(HasDay(rows, window_start));
    REQUIRE(HasDay(rows, today));
    REQUIRE(!HasDay(rows, expired));

    std::vector<HourlyRow> hourly;
    REQUIRE(store->HourlyRows(window_start, hourly, error));
    REQUIRE(HasHour(hourly, window_start, 9));
    hourly.clear();
    REQUIRE(store->HourlyRows(expired, hourly, error));
    REQUIRE(hourly.empty());

    int first = 0;
    bool present = false;
    REQUIRE(store->FirstDay(first, present, error));
    REQUIRE(present);
    REQUIRE_EQ(first, window_start);
}

// A failed cleanup must not advance the marker, otherwise the stale rows would
// survive until the next calendar day before the retry.
TEST_CASE(stats_store_retention_failure_keeps_marker_stale)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    const int yesterday = MsimeStats::AddDays(today, -1);
    Batch seed;
    seed.days.push_back(DayDelta{MsimeStats::AddDays(today, -100), 1, 0, 0, 0, 0, 0});
    seed.days.push_back(DayDelta{today, 1, 0, 0, 0, 0, 0});
    std::string error;
    REQUIRE(store->Upsert(seed, error));
    REQUIRE(store->SetMeta(MsimeStats::kStoreMetaLastRetentionDay, std::to_string(yesterday), error));

    // Break the table the cleanup deletes from: ClearThrough fails and the
    // marker must keep yesterday's value, so the next write retries.
    {
        RawDb raw(db_path);
        REQUIRE(sqlite3_exec(raw.get(), "DROP TABLE stats_daily", nullptr, nullptr, nullptr) == SQLITE_OK);
    }

    MsimeStats::RetentionOutcome outcome;
    REQUIRE(!MsimeStats::RunDailyRetention(*store, Retention::Days30, today, outcome, error));
    REQUIRE(!error.empty());

    std::string marker;
    bool present = false;
    REQUIRE(store->Meta(MsimeStats::kStoreMetaLastRetentionDay, marker, present, error));
    REQUIRE(present);
    REQUIRE_EQ(marker, std::to_string(yesterday));
}

// The explicit "clear all statistics" action wipes both tables and both meta
// keys, so the overview falls back to the empty state.
TEST_CASE(stats_store_clear_all_resets_rows_and_meta)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    Batch seed;
    seed.days.push_back(DayDelta{MsimeStats::AddDays(today, -2), 1, 0, 0, 0, 0, 0});
    seed.days.push_back(DayDelta{MsimeStats::AddDays(today, -1), 1, 0, 0, 0, 0, 0});
    seed.days.push_back(DayDelta{today, 1, 0, 0, 0, 0, 0});
    seed.hours.push_back(HourDelta{today, 8, 1, 0});
    std::string error;
    REQUIRE(store->Upsert(seed, error));
    REQUIRE(store->SetMeta(MsimeStats::kStoreMetaLastRetentionDay, std::to_string(today), error));

    int64_t removed = 0;
    REQUIRE(store->ClearAll(removed, error));
    REQUIRE_EQ(removed, 3);

    std::vector<DailyRow> daily;
    REQUIRE(store->DailyRows(daily, error));
    REQUIRE(daily.empty());
    std::vector<HourlyRow> hourly;
    REQUIRE(store->HourlyRows(today, hourly, error));
    REQUIRE(hourly.empty());

    int first = 0;
    bool present = false;
    REQUIRE(store->FirstDay(first, present, error));
    REQUIRE(!present);
    std::string marker;
    REQUIRE(store->Meta(MsimeStats::kStoreMetaLastRetentionDay, marker, present, error));
    REQUIRE(!present);
    // schema_version is written on open and is not user data; it must survive.
    REQUIRE(store->Meta(MsimeStats::kStoreMetaSchemaVersion, marker, present, error));
    REQUIRE(present);
    REQUIRE_EQ(marker, std::string(MsimeStats::kStoreSchemaVersion));

    // Clearing an already empty database is a success that removed nothing.
    REQUIRE(store->ClearAll(removed, error));
    REQUIRE_EQ(removed, 0);
}

// A marker corrupted by a hand edit must not stall the policy forever: a value
// that is not a positive decimal day key is treated as "never ran", so the
// cleanup runs once and rewrites a valid key.
TEST_CASE(stats_store_retention_malformed_marker_self_repairs)
{
    ScopedTempDir temp;
    fs::path db_path;
    std::unique_ptr<Store> store = OpenStore(temp, db_path);

    const int today = DayKey(2026, 9, 20);
    Batch seed;
    seed.days.push_back(DayDelta{MsimeStats::AddDays(today, -100), 1, 0, 0, 0, 0, 0});
    seed.days.push_back(DayDelta{today, 1, 0, 0, 0, 0, 0});
    std::string error;
    REQUIRE(store->Upsert(seed, error));

    MsimeStats::RetentionOutcome outcome;
    std::string marker;
    bool present = false;
    for (const char *bad : {"not-a-day", "-5", "20260920x", ""})
    {
        REQUIRE(store->SetMeta(MsimeStats::kStoreMetaLastRetentionDay, bad, error));
        REQUIRE(MsimeStats::RunDailyRetention(*store, Retention::Days30, today, outcome, error));
        REQUIRE(!outcome.already_ran_today);
        REQUIRE(store->Meta(MsimeStats::kStoreMetaLastRetentionDay, marker, present, error));
        REQUIRE(present);
        REQUIRE_EQ(marker, std::to_string(today));
    }

    // The first repair run applied the policy; the day outside the window is gone.
    std::vector<DailyRow> rows;
    REQUIRE(store->DailyRows(rows, error));
    REQUIRE(!HasDay(rows, MsimeStats::AddDays(today, -100)));
    REQUIRE(HasDay(rows, today));
}
