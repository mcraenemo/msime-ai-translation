#include "../contracts/assets/assets.h"
#include "local_database.h"

#include "../core/data_path.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace metasequoia::local_modes
{
namespace
{
std::shared_ptr<sqlite3> open_read_only(const std::filesystem::path &path)
{
    sqlite3 *raw = nullptr;
    // FULLMUTEX: a shared connection is used from the IME worker and from the
    // emoji/kaomoji lookup threads.
    if (sqlite3_open_v2(path_to_utf8(path).c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) !=
        SQLITE_OK)
    {
        if (raw != nullptr)
        {
            sqlite3_close(raw);
        }
        return {};
    }
    sqlite3_busy_timeout(raw, 1000);
    return std::shared_ptr<sqlite3>(raw, [](sqlite3 *database) { sqlite3_close(database); });
}

// Only the shipped dictionaries are shared: msime.db is held open by the engine
// for the whole session anyway, and others.db is replaced only by the installer
// while the Server is stopped. Keeping anything else open could block a tool or
// a test that deletes its database afterwards, which Windows refuses while a
// handle is open.
bool is_shared_dictionary(const std::filesystem::path &path)
{
    return path == data_file_path(metasequoia::assets::main_dictionary) ||
           path == data_file_path(metasequoia::assets::other_dictionary);
}

struct CachedConnection
{
    std::filesystem::path path;
    std::shared_ptr<sqlite3> connection;
};

std::mutex &cache_mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<CachedConnection> &cache()
{
    static std::vector<CachedConnection> connections;
    return connections;
}
} // namespace

std::shared_ptr<sqlite3> open_local_database(const std::filesystem::path &path)
{
    if (path.empty())
    {
        return {};
    }
    if (!is_shared_dictionary(path))
    {
        return open_read_only(path);
    }

    const std::lock_guard<std::mutex> guard(cache_mutex());
    auto &connections = cache();
    for (const CachedConnection &cached : connections)
    {
        if (cached.path == path)
        {
            return cached.connection;
        }
    }
    auto connection = open_read_only(path);
    if (connection)
    {
        // At most one entry per shipped dictionary: a connection cached for a
        // data directory that is no longer active is released here.
        connections.erase(
            std::remove_if(connections.begin(), connections.end(),
                           [&](const CachedConnection &cached) { return cached.path.filename() == path.filename(); }),
            connections.end());
        connections.push_back({path, connection});
    }
    return connection;
}

void close_cached_local_databases()
{
    const std::lock_guard<std::mutex> guard(cache_mutex());
    cache().clear();
}
} // namespace metasequoia::local_modes
