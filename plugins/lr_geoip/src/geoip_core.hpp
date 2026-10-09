#pragma once
// Engine-free core of the lr_geoip port (the Pisex LR module "[LR] GEOIP",
// ref/src/cs2-lvl_ranks_modules/LR_ExStatsGeoIP/lr_geoip.cpp, 204 lines):
// the address extraction of OnPlayerLoadedHook, the `<lr_table>_geoip` table of OnCoreIsReadyHook
// and the lookup -> row mapping. The engine side (s2sdk, the LR core hooks, plgdb) lives in
// src/plugin.cpp; the MaxMind DB reader in src/mmdb.{hpp,cpp}.
//
// The original bound libmaxminddb from its build machine; the plugify build image has no
// libmaxminddb, so the reader is local (see mmdb.hpp) and this core is the part of the module that
// is testable without an engine.

#include <string>
#include <string_view>

#include <plgdb/db.hpp>

namespace lr_geoip {

// The original's OnPlayerLoadedHook address handling:
//     auto sIp2 = std::string(playerNetInfo->GetAddress());         // "1.2.3.4:27005"
//     auto sIp = sIp2.substr(0, sIp2.find(":"));                    // cut at the first colon
// s2sdk's GetClientIp hands out the same "ip:port" form (its "<unknown>" placeholder when the
// client has no channel yet). Returns "" for an address that cannot be looked up.
std::string ExtractIp(std::string_view address);

// The `<base>_geoip` table name (the original's g_sTableName + "_geoip").
std::string TableName(std::string_view base);

// CREATE TABLE IF NOT EXISTS of OnCoreIsReadyHook: steam PK + clientip/country/region/city/
// country_code. The MySQL spelling is the original's, byte for byte (the production database is
// shared with the Metamod server); SQLite has no CHARSET/COLLATE clauses.
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
