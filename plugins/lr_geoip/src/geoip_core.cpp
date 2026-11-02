#include "geoip_core.hpp"

#include <fstream>
#include <utility>

namespace lr_geoip {
namespace {

// Appends |candidate| unless it is empty or already listed (the configs and the base directory
// can spell the same file).
void AddCandidate(std::vector<std::string>& candidates, std::string candidate) {
	if (candidate.empty()) {
		return;
	}
	for (const std::string& existing : candidates) {
		if (existing == candidate) {
			return;
		}
	}
	candidates.push_back(std::move(candidate));
}

} // namespace

std::string JoinPath(std::string_view base, std::string_view leaf) {
	if (base.empty()) {
		return std::string(leaf);
	}
	if (leaf.empty()) {
		return std::string(base);
	}
	std::string out(base);
	if (out.back() != '/') {
		out.push_back('/');
	}
	if (leaf.front() == '/') {
		leaf.remove_prefix(1);
	}
	out.append(leaf);
	return out;
}

bool FileExists(const std::string& path) {
	if (path.empty()) {
		return false;
	}
	std::ifstream file(path, std::ios::binary);
	return file.good();
}

std::vector<std::string> DatabaseCandidates(std::string_view configsDir, std::string_view baseDir) {
	std::vector<std::string> candidates;
	if (!configsDir.empty()) {
		AddCandidate(candidates, JoinPath(JoinPath(configsDir, "geoip"), kDatabaseFile));
		AddCandidate(candidates, JoinPath(JoinPath(configsDir, "lr_geoip"), kDatabaseFile));
	}
	if (!baseDir.empty()) {
		AddCandidate(candidates, JoinPath(JoinPath(baseDir, "addons/configs/geoip"), kDatabaseFile));
		AddCandidate(candidates, JoinPath(JoinPath(baseDir, "configs/geoip"), kDatabaseFile));
	}
	AddCandidate(candidates, std::string(kLegacyDatabasePath));
	return candidates;
}

std::string ResolveDatabasePath(std::string_view configsDir, std::string_view baseDir) {
	for (const std::string& candidate : DatabaseCandidates(configsDir, baseDir)) {
		if (FileExists(candidate)) {
			return candidate;
		}
	}
	return {};
}

std::string ExtractIp(std::string_view address) {
	// The original cut the string at the first ':' (the port of its address source), which left a
	// bare IPv4 address untouched but broke every IPv6 client ("[::1]:27015" became "["). s2sdk's
	// GetClientIp answers the address without the port, but keep the port handling for the
	// original's "ip:port" form: a bracketed address is unwrapped up to ']', a single ':' is still
	// the IPv4 ":port" separator, and a bare IPv6 address (two or more ':') is kept whole. s2sdk's
	// "<unknown>" placeholder (no net channel yet) is rejected like an empty address was.
	if (address.empty() || address.front() == '<') {
		return {};
	}
	if (address.front() == '[') {
		const std::size_t close = address.find(']');
		if (close == std::string_view::npos || close <= 1) {
			return {};
		}
		return std::string(address.substr(1, close - 1));
	}
	const std::size_t colon = address.find(':');
	if (colon == std::string_view::npos) {
		return std::string(address);
	}
	if (address.find(':', colon + 1) != std::string_view::npos) {
		return std::string(address); // a bare IPv6 address: every ':' belongs to it
	}
	std::string ip(address.substr(0, colon));
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
		// The CREATE TABLE of the original; `clientip` is varchar(45) instead of the original's
		// varchar(16) so a dual-stack client's IPv6 address fits as well.
		return "CREATE TABLE IF NOT EXISTS `" + name + "` ("
			   "`steam` varchar(32) NOT NULL default '' PRIMARY KEY, "
			   "`clientip` varchar(45) NOT NULL default '', "
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
