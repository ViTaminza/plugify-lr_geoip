// Tests for plugins/lr_geoip (the ported "[LR] GEOIP" module):
//
//   * the local MaxMind DB reader (src/mmdb.cpp) against the *shipped* databases
//     (GeoLite2-City.mmdb and GeoLite2-Country.mmdb) — metadata, the IPv4-in-IPv6 subtree, the
//     data decoder and the four Location fields,
//   * MmdbCache: the open database is reused and a replaced file is reopened (a GeoLite2 update),
//   * the address extraction of the original's OnPlayerLoadedHook,
//   * the SQL layer (`_geoip` CREATE / REPLACE) against SQLite ":memory:" and, when MYSQL_HOST is
//     set (./test.sh provides it), against MySQL. Rows created in MySQL are deleted at the end.

#include <plg/plugin.hpp>

#include <pisex/paths.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

#include "geoip_core.hpp"
#include "mmdb.hpp"

// A real plugin gets these from PLUGIFY_PLUGIN; the test leaves them null.
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

namespace {

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name) {
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                            \
	do {                                                                                       \
		++g_checks;                                                                            \
		if (!(expr)) {                                                                         \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),         \
						g_section.data(), __FILE__, __LINE__, #expr);                          \
		}                                                                                      \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                         \
	do {                                                                                       \
		++g_checks;                                                                            \
		const long long a_ = static_cast<long long>(actual);                                   \
		const long long e_ = static_cast<long long>(expected);                                 \
		if (a_ != e_) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: got %lld, expected %lld\n",                        \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,       \
						__LINE__, a_, e_);                                                     \
		}                                                                                      \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                         \
	do {                                                                                       \
		++g_checks;                                                                            \
		const std::string a_{std::string_view(actual)};                                        \
		const std::string e_{std::string_view(expected)};                                      \
		if (a_ != e_) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                    \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,       \
						__LINE__, a_.c_str(), e_.c_str());                                     \
		}                                                                                      \
	} while (false)

using namespace lr_geoip;

constexpr char kCityDb[] = LR_GEOIP_TEST_DATA_DIR "/GeoLite2-City.mmdb";
constexpr char kCountryDb[] = LR_GEOIP_TEST_DATA_DIR "/GeoLite2-Country.mmdb";

// --- the MaxMind DB reader ----------------------------------------------------------------------

void TestCityDatabase() {
	Section("GeoLite2-City.mmdb");
	std::string error;
	std::unique_ptr<Mmdb> db = Mmdb::Open(kCityDb, &error);
	CHECK(db != nullptr);
	if (db == nullptr) {
		std::printf("  open error: %s\n", error.c_str());
		return;
	}
	std::printf("  type=%s node_count=%u record_size=%u ip_version=%u\n",
				std::string(db->DatabaseType()).c_str(), db->NodeCount(), db->RecordSize(),
				db->IpVersion());
	CHECK_EQ_STR(db->DatabaseType(), "GeoLite2-City");
	CHECK(db->RecordSize() == 24 || db->RecordSize() == 28 || db->RecordSize() == 32);
	CHECK_EQ_INT(db->IpVersion(), 6);
	CHECK(db->NodeCount() > 0);

	const auto report = [&db](std::string_view ip) {
		Mmdb::Location location;
		const bool parsed = db->Lookup(ip, &location);
		std::printf("  %-30.*s parsed=%d found=%d country=\"%s\" code=\"%s\" region=\"%s\" city=\"%s\"\n",
					static_cast<int>(ip.size()), ip.data(), parsed ? 1 : 0, location.found ? 1 : 0,
					location.country.c_str(), location.country_code.c_str(), location.region.c_str(),
					location.city.c_str());
		return location;
	};

	// 8.8.8.8 (Google DNS): the shipped database carries the 8.8.8.0/23 record, which has a
	// country, a location and a registered_country but no city/subdivisions.
	{
		const Mmdb::Location location = report("8.8.8.8");
		CHECK(location.found);
		CHECK_EQ_STR(location.country, "United States");
		CHECK_EQ_STR(location.country_code, "US");
		CHECK_EQ_STR(location.city, "");
		CHECK_EQ_STR(location.region, "");
	}
	// 77.88.55.66 (Yandex, Russia).
	{
		const Mmdb::Location location = report("77.88.55.66");
		CHECK(location.found);
		CHECK_EQ_STR(location.country_code, "RU");
		CHECK_EQ_STR(location.country, "Russia");
	}
	// A plain IPv6 address (Google public DNS).
	{
		const Mmdb::Location location = report("2001:4860:4860::8888");
		CHECK(location.found);
		CHECK_EQ_STR(location.country_code, "US");
	}
	// Private ranges are not in the database.
	CHECK(!report("10.0.0.1").found);
	CHECK(!report("192.168.1.1").found);
	// Unparseable input (the original's gai_error path) must not found and must not crash.
	{
		Mmdb::Location location;
		CHECK(!db->Lookup("not-an-ip", &location));
		CHECK(!db->Lookup("", &location));
		CHECK(!db->Lookup("<unknown>", &location));
		CHECK(!db->Lookup("1.2.3.4.5", &location));
	}
	// The Country database in an IPv4-only context is covered by the second test below.
}

// The decoder over many records: every found record must carry a 2-letter country code when it
// has one, and the city/subdivision paths (city map names.en, subdivisions[0].names.en) must
// produce values for a meaningful number of random public addresses.
void TestCityCoverage() {
	Section("coverage over random public IPv4s");
	std::string error;
	std::unique_ptr<Mmdb> db = Mmdb::Open(kCityDb, &error);
	CHECK(db != nullptr);
	if (db == nullptr) {
		return;
	}
	std::mt19937_64 rng(20261008ULL);
	int found = 0;
	int codes = 0;
	int cities = 0;
	int regions = 0;
	for (int i = 0; i < 3000; ++i) {
		// A public IPv4 address (skip the private ranges and the multicast/reserved space).
		uint32_t value = 0;
		do {
			value = static_cast<uint32_t>(rng() & 0xFFFFFFFFu);
		} while ((value >> 24) == 10 || (value >> 24) == 127 || (value >> 24) >= 224 ||
				 (value >> 20) == 0xAC1 || (value >> 16) == 0xC0A8);
		char text[16];
		std::snprintf(text, sizeof(text), "%u.%u.%u.%u", (value >> 24) & 0xFF, (value >> 16) & 0xFF,
					  (value >> 8) & 0xFF, value & 0xFF);
		Mmdb::Location location;
		if (!db->Lookup(text, &location)) {
			std::printf("FAIL lookup of %s did not parse\n", text);
			++g_failures;
			continue;
		}
		if (!location.found) {
			continue;
		}
		++found;
		if (!location.country_code.empty()) {
			++codes;
			if (location.country_code.size() != 2) {
				std::printf("FAIL %s has country code \"%s\"\n", text, location.country_code.c_str());
				++g_failures;
			}
		}
		if (!location.city.empty()) {
			++cities;
		}
		if (!location.region.empty()) {
			++regions;
		}
	}
	std::printf("  %d found, %d country codes, %d cities, %d regions\n", found, codes, cities, regions);
	CHECK(found > 500);
	CHECK(codes > 500);
	CHECK(cities > 50);  // the city map path (city.names.en) decodes
	CHECK(regions > 50); // the subdivisions array path decodes
}

void TestCountryDatabase() {
	Section("GeoLite2-Country.mmdb");
	std::string error;
	std::unique_ptr<Mmdb> db = Mmdb::Open(kCountryDb, &error);
	CHECK(db != nullptr);
	if (db == nullptr) {
		std::printf("  open error: %s\n", error.c_str());
		return;
	}
	std::printf("  type=%s node_count=%u record_size=%u ip_version=%u\n",
				std::string(db->DatabaseType()).c_str(), db->NodeCount(), db->RecordSize(),
				db->IpVersion());
	CHECK_EQ_STR(db->DatabaseType(), "GeoLite2-Country");

	Mmdb::Location location;
	CHECK(db->Lookup("8.8.8.8", &location));
	CHECK(location.found);
	CHECK_EQ_STR(location.country, "United States");
	CHECK_EQ_STR(location.country_code, "US");
	// The country database has no city/subdivision names for this address.
	CHECK_EQ_STR(location.city, "");
	CHECK(db->Lookup("77.88.55.66", &location));
	CHECK(location.found);
	CHECK_EQ_STR(location.country_code, "RU");
}

void TestOpenFailures() {
	Section("reader failure paths");
	std::string error;
	CHECK(Mmdb::Open("/nonexistent/GeoLite2-City.mmdb", &error) == nullptr);
	CHECK(!error.empty());
	std::printf("  error: %s\n", error.c_str());

	// A short non-MMDB file must be rejected, not misread.
	const std::string junk = "/tmp/lr_geoip_junk_test.mmdb";
	if (FILE* file = std::fopen(junk.c_str(), "wb"); file != nullptr) {
		std::fputs("this is not a maxmind db", file);
		std::fclose(file);
	}
	CHECK(Mmdb::Open(junk, &error) == nullptr);
	std::printf("  junk error: %s\n", error.c_str());
	std::remove(junk.c_str());
}

// Copies a file (the test's way of simulating a GeoLite2 update replacing the database).
bool CopyFile(const std::string& from, const std::string& to) {
	std::FILE* in = std::fopen(from.c_str(), "rb");
	if (in == nullptr) {
		return false;
	}
	std::FILE* out = std::fopen(to.c_str(), "wb");
	if (out == nullptr) {
		std::fclose(in);
		return false;
	}
	char buffer[64 * 1024];
	std::size_t read = 0;
	bool ok = true;
	while ((read = std::fread(buffer, 1, sizeof(buffer), in)) > 0) {
		if (std::fwrite(buffer, 1, read, out) != read) {
			ok = false;
			break;
		}
	}
	if (std::ferror(in) != 0) {
		ok = false;
	}
	std::fclose(in);
	std::fclose(out);
	return ok;
}

// The database behind the path is opened once and reused; a changed file (the size/mtime stamp)
// is reopened, a broken or missing one leaves no stale database behind.
void TestMmdbCache() {
	Section("MmdbCache (open once, reopen on change)");
	MmdbCache cache;
	std::string error;

	// No usable file: no database, with the reason.
	CHECK(cache.Get("/nonexistent/GeoLite2-City.mmdb", &error) == nullptr);
	CHECK(!error.empty());

	// The first use opens the file, the next one hands out the same open database.
	const Mmdb* city = cache.Get(kCityDb, &error);
	CHECK(city != nullptr);
	if (city == nullptr) {
		std::printf("  open error: %s\n", error.c_str());
		return;
	}
	CHECK_EQ_STR(city->DatabaseType(), "GeoLite2-City");
	CHECK(cache.Get(kCityDb, &error) == city);

	// A replaced file is reopened: prime the cache with the Country database, overwrite the file
	// with the City one, and the next Get must read the new file.
	const std::string temp = "/tmp/lr_geoip_cache_test.mmdb";
	CHECK(CopyFile(kCountryDb, temp));
	const Mmdb* country = cache.Get(temp, &error);
	CHECK(country != nullptr);
	if (country == nullptr) {
		std::printf("  open error: %s\n", error.c_str());
	} else {
		CHECK_EQ_STR(country->DatabaseType(), "GeoLite2-Country");
		Mmdb::Location location;
		CHECK(country->Lookup("8.8.8.8", &location));
		CHECK(location.found);
		CHECK_EQ_STR(location.city, ""); // the Country database carries no city

		CHECK(CopyFile(kCityDb, temp));
		const Mmdb* replaced = cache.Get(temp, &error);
		CHECK(replaced != nullptr);
		if (replaced != nullptr) {
			CHECK_EQ_STR(replaced->DatabaseType(), "GeoLite2-City"); // reopened, not the stale file
		}

		// A file that disappeared: no database (and a reason), and it opens again once back.
		std::remove(temp.c_str());
		CHECK(cache.Get(temp, &error) == nullptr);
		CHECK(!error.empty());
		CHECK(CopyFile(kCountryDb, temp));
		const Mmdb* restored = cache.Get(temp, &error);
		CHECK(restored != nullptr);
		if (restored != nullptr) {
			CHECK_EQ_STR(restored->DatabaseType(), "GeoLite2-Country");
		}
	}
	std::remove(temp.c_str());
}

// --- the original's address handling --------------------------------------------------------------

void TestExtractIp() {
	Section("ExtractIp (OnPlayerLoadedHook)");
	CHECK_EQ_STR(ExtractIp("1.2.3.4:27005"), "1.2.3.4");
	CHECK_EQ_STR(ExtractIp("1.2.3.4"), "1.2.3.4");
	CHECK_EQ_STR(ExtractIp("255.255.255.255:1"), "255.255.255.255");
	// The original's substr(0, find(":")) behaviour (IPv6 falls through it like it did there).
	CHECK_EQ_STR(ExtractIp("[::1]:27015"), "[");
	CHECK_EQ_STR(ExtractIp(""), "");
	CHECK_EQ_STR(ExtractIp("<unknown>"), "");
	CHECK_EQ_STR(ExtractIp(":27015"), "");
}

// --- SQL -----------------------------------------------------------------------------------------

plgdb::Config SqliteConfig() {
	plgdb::Config config;
	config.driver = plgdb::Driver::SQLite;
	config.sqlitePath = ":memory:";
	return config;
}

bool MySqlConfig(plgdb::Config& config, std::string& error) {
	const char* host = std::getenv("MYSQL_HOST");
	if (host == nullptr || *host == '\0') {
		return false;
	}
	config.driver = plgdb::Driver::MySQL;
	config.host = host;
	config.user = std::getenv("MYSQL_USER") != nullptr ? std::getenv("MYSQL_USER") : "";
	config.password = std::getenv("MYSQL_PASSWORD") != nullptr ? std::getenv("MYSQL_PASSWORD") : "";
	config.database = std::getenv("MYSQL_DATABASE") != nullptr ? std::getenv("MYSQL_DATABASE") : "";
	const char* port = std::getenv("MYSQL_PORT");
	if (port != nullptr && *port != '\0') {
		try {
			const int value = std::stoi(port);
			if (value < 1 || value > 65535) {
				error = "MYSQL_PORT out of range";
				return false;
			}
			config.port = static_cast<uint16_t>(value);
		} catch (const std::exception&) {
			error = "MYSQL_PORT is not a number";
			return false;
		}
	}
	return true;
}

void TestDatabase(const plgdb::Config& config, std::string_view label, const std::string& table,
				  bool cleanup) {
	Section(std::string("database ") + std::string(label));
	std::unique_ptr<plgdb::Database> db = plgdb::Database::Create(config);
	CHECK(db != nullptr);
	if (db == nullptr) {
		return;
	}
	CHECK_EQ_STR(TableName("lvl_base"), "lvl_base_geoip");
	CHECK(db->QuerySync(CreateTableSql(table, db->GetDriver()), {}).Ok());

	// The row of a lookup (the OnPlayerLoadedHook path).
	Row row;
	row.steam = "STEAM_1:1:90000001";
	row.clientip = "8.8.8.8";
	row.country = "United States";
	row.region = "California";
	row.city = "Mountain View";
	row.country_code = "US";
	{
		Statement upsert = UpsertStatement(table, row);
		CHECK(db->QuerySync(upsert.sql, upsert.params).Ok());
	}
	// The same key again (what ON DUPLICATE KEY UPDATE did): the row is replaced, not duplicated.
	row.city = "Moscow";
	row.country = "Russia";
	row.country_code = "RU";
	{
		Statement upsert = UpsertStatement(table, row);
		CHECK(db->QuerySync(upsert.sql, upsert.params).Ok());
	}
	{
		const plgdb::Result result = db->QuerySync(
			"SELECT `clientip`, `country`, `region`, `city`, `country_code` FROM `" + table +
				"` WHERE `steam` = ?",
			{std::string("STEAM_1:1:90000001")});
		CHECK(result.Ok());
		CHECK_EQ_INT(result.RowCount(), 1);
		if (result.RowCount() == 1) {
			CHECK_EQ_STR(result.GetString(0, 0), "8.8.8.8");
			CHECK_EQ_STR(result.GetString(0, 1), "Russia");
			CHECK_EQ_STR(result.GetString(0, 2), "California");
			CHECK_EQ_STR(result.GetString(0, 3), "Moscow");
			CHECK_EQ_STR(result.GetString(0, 4), "RU");
		}
	}

	if (cleanup) {
		db->QuerySync("DROP TABLE `" + table + "`;", {});
	}
}

} // namespace

int main() {
	std::printf("lr_geoip_tests\n");

	std::random_device device;
	std::mt19937_64 rng(device());
	const std::string tag = std::to_string(rng() % 1000000ULL);

	TestCityDatabase();
	TestCityCoverage();
	TestCountryDatabase();
	TestOpenFailures();
	TestMmdbCache();
	TestExtractIp();

	TestDatabase(SqliteConfig(), "sqlite (:memory:)", "lvl_base_geoip_sqlite", /*cleanup=*/false);

	plgdb::Config mysql;
	std::string error;
	if (MySqlConfig(mysql, error)) {
		std::printf("== mysql: %s:%u/%s\n", mysql.host.c_str(), mysql.port, mysql.database.c_str());
		TestDatabase(mysql, "mysql", "lvl_geoip_test_" + tag, /*cleanup=*/true);
	} else if (!error.empty()) {
		std::printf("FATAL: %s\n", error.c_str());
		++g_failures;
	}

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
