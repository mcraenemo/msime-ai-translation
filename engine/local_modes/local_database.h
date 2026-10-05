#pragma once

#include <sqlite3.h>

#include <filesystem>
#include <memory>

namespace metasequoia::local_modes
{
// Opens a read-only connection for a local-mode query (jianpin, quick phrases,
// emoji, kaomoji). Those queries run on every keystroke of their mode, and a
// fresh connection re-parses the dictionary schema -- a couple of hundred tables
// in msime.db -- on its first prepare. For the shipped dictionaries of the
// active data directory the connection is therefore opened once and shared.
// Any other path (tests, tools) gets its own connection that closes with the
// last reference, so the file is never held open behind a caller's back.
// Returns null when the database cannot be opened.
std::shared_ptr<sqlite3> open_local_database(const std::filesystem::path &path);

// Drops the shared connections so the dictionary files can be deleted or
// replaced; each closes once the last query using it returns. Called by
// user_dictionary::close_default_user_database(), which is what callers use to
// let go of the data directory.
void close_cached_local_databases();
} // namespace metasequoia::local_modes
