#include "geoip_core.hpp"

#include <utility>

namespace lr_geoip {

std::string ExtractIp(std::string_view address) {
	// The original cut the string at the first ':' (the port its address came from), which also
	// leaves a bare IPv4 address untouched. s2sdk's "<unknown>" placeholder (no net channel yet) is
	// rejected like an empty address was.
	const std::size_t colon = address.find(':');
	std::string ip(address.substr(0, colon == std::string_view::npos ? address.size() : colon));
	if (ip.empty() || ip.front() == '<') {
		return {};
	}
	return ip;
}

std::string TableName(std::string_view base) {
	return std::string(base) + "_geoip";
}

std::string CreateTableSql(std::string_view table, plgdb::Driver driver) {
	const std::string name(table);
	if (driver == plgdb::Driver::MySQL) {
		// The CREATE TABLE of the original, byte for byte.
		return "CREATE TABLE IF NOT EXISTS `" + name + "` ("
			   "`steam` varchar(32) NOT NULL default '' PRIMARY KEY, "
			   "`clientip` varchar(16) NOT NULL default '', "
			   "`country` varchar(48) NOT NULL default '', "
			   "`region` varchar(48) NOT NULL default '', "
			   "`city` varchar(48) NOT NULL default '', "
			   "`country_code` varchar(4) NOT NULL default ''"
			   ") CHARSET=utf8 COLLATE utf8_general_ci";
	}
	// SQLite has no CHARSET/COLLATE clauses; the columns and the primary key are the same.
	return "CREATE TABLE IF NOT EXISTS `" + name + "` ("
		   "`steam` TEXT NOT NULL DEFAULT '' PRIMARY KEY, "
		   "`clientip` TEXT NOT NULL DEFAULT '', "
		   "`country` TEXT NOT NULL DEFAULT '', "
		   "`region` TEXT NOT NULL DEFAULT '', "
		   "`city` TEXT NOT NULL DEFAULT '', "
		   "`country_code` TEXT NOT NULL DEFAULT ''"
		   ");";
}

Statement UpsertStatement(std::string_view table, const Row& row) {
	Statement statement;
	// The original's `INSERT IGNORE ... ON DUPLICATE KEY UPDATE` (MySQL only) becomes a REPLACE
	// INTO, which SQLite and MySQL both accept and which the `steam` primary key makes equivalent.
	statement.sql = "REPLACE INTO `" + std::string(table) + "` "
					"(`steam`, `clientip`, `country`, `region`, `city`, `country_code`) "
					"VALUES (?, ?, ?, ?, ?, ?);";
	statement.params.emplace_back(row.steam);
	statement.params.emplace_back(row.clientip);
	statement.params.emplace_back(row.country);
	statement.params.emplace_back(row.region);
	statement.params.emplace_back(row.city);
	statement.params.emplace_back(row.country_code);
	return statement;
}

} // namespace lr_geoip
