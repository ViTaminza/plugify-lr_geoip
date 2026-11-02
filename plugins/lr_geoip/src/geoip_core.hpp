#pragma once
// Engine-free core of the lr_geoip port (the Pisex LR module "[LR] GEOIP", the original's
// lr_geoip.cpp): the address extraction of OnPlayerLoadedHook, the `<lr_table>_geoip` table of
// OnCoreIsReadyHook and the lookup -> row mapping. The engine side (s2sdk, the LR core hooks,
// plgdb) lives in src/plugin.cpp; the MaxMind DB reader in src/mmdb.{hpp,cpp}.
//
// The original bound libmaxminddb from its build machine; the plugify build image has no
// libmaxminddb, so the reader is local (see mmdb.hpp) and this core is the part of the module that
// is testable without an engine.

#include <string>
#include <string_view>
#include <vector>

#include <plgdb/db.hpp>

namespace lr_geoip {

// --- where the database is read from -------------------------------------------------------------
// The original opened "addons/configs/geoip/GeoLite2-City.mmdb" (the Metamod layout). Under
// Plugify the same database lives in the configs tree, either in the original's <configs>/geoip/
// or in the plugin's own <configs>/lr_geoip/ (PLUGIN_STANDARD §3); a server that only has the
// Metamod tree is covered by the <GetBaseDir()>-relative and the working-directory-relative
// spellings. These are the plugin's own helpers - no pisexcompat (PLUGIN_STANDARD §7).

inline constexpr std::string_view kDatabaseFile = "GeoLite2-City.mmdb";
// The original's relative path, kept as the last resort (it is relative to the server working
// directory, which is what pisex::ResolvePath tried first).
inline constexpr std::string_view kLegacyDatabasePath = "addons/configs/geoip/GeoLite2-City.mmdb";

// |base| + '/' + |leaf| (an empty part gives the other one; a leading '/' of |leaf| is ignored).
std::string JoinPath(std::string_view base, std::string_view leaf);

// True when |path| names a readable file (the pisex::FileExists the original relied on).
bool FileExists(const std::string& path);

// The database candidates in resolution order, skipping empty directories and removing
// duplicates:
//   1. <configs>/geoip/GeoLite2-City.mmdb        the original's location under the Plugify tree
//   2. <configs>/lr_geoip/GeoLite2-City.mmdb     the plugin's own config directory (§3)
//   3. <base>/addons/configs/geoip/...           the Metamod layout under the server root
//   4. <base>/configs/geoip/...                  a host whose base dir is the configs root
//   5. addons/configs/geoip/...                  the original path, relative to the working dir
std::vector<std::string> DatabaseCandidates(std::string_view configsDir, std::string_view baseDir);

// The first DatabaseCandidates() entry that exists on disk, or "" when none does.
std::string ResolveDatabasePath(std::string_view configsDir, std::string_view baseDir);

// The original's OnPlayerLoadedHook address handling: take the client's network info address
// ("1.2.3.4:27005") and cut it at the first colon.
// s2sdk's GetClientIp hands out the same "ip:port" form (its "<unknown>" placeholder when the
// client has no channel yet). Returns "" for an address that cannot be looked up.
std::string ExtractIp(std::string_view address);

// The `<base>_geoip` table name (the original's g_sTableName + "_geoip").
std::string TableName(std::string_view base);

// CREATE TABLE IF NOT EXISTS of OnCoreIsReadyHook: steam PK + clientip/country/region/city/
// country_code. The MySQL spelling is the original's, byte for byte (the table lands in the same
// database as the LR core's); SQLite has no CHARSET/COLLATE clauses.
std::string CreateTableSql(std::string_view table, plgdb::Driver driver);

// The original's INSERT ... ON DUPLICATE KEY UPDATE, as one statement that both drivers accept
// (REPLACE INTO has the same effect for this row: the `steam` key identifies it).
struct Statement {
	std::string sql;
	plgdb::Params params;
};
// |location| fields must already be truncated to the column widths (ApplyLookupFields does that).
struct Row {
	std::string steam;
	std::string clientip;
	std::string country;
	std::string region;
	std::string city;
	std::string country_code;
};
Statement UpsertStatement(std::string_view table, const Row& row);

} // namespace lr_geoip
