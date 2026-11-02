// Tests for plugins/lr_geoip (the ported "[LR] GEOIP" module):
//
//   * the local MaxMind DB reader (src/mmdb.cpp) against the *shipped* databases
//     (GeoLite2-City.mmdb and GeoLite2-Country.mmdb) — metadata, the IPv4-in-IPv6 subtree, the
//     data decoder and the four Location fields,
//   * MmdbCache: the open database is reused and a replaced file is reopened (a GeoLite2 update),
//   * the plugin's own path helpers (JoinPath / FileExists / DatabaseCandidates /
//     ResolveDatabasePath) — the replacement of pisex::JoinPath/FileExists/ResolvePath,
//   * the address extraction of the original's OnPlayerLoadedHook,
//   * the SQL layer (`_geoip` CREATE / REPLACE) against SQLite ":memory:" and, when MYSQL_HOST is
//     set (./test.sh provides it), against MySQL. Rows created in MySQL are deleted at the end.
//
// No engine, no Plugify host and no pisexcompat: the executable links only lr_geoip_core (and
// plgdb), like the other pure-s2sdk LR modules.
//
// The GeoLite2 files are third-party data that is not committed. The test looks for a complete
// City+Country pair at runtime (an explicit LR_GEOIP_TEST_DATA_DIR environment variable, the path
// CMake recorded, dist/ and the usual checkout/container layouts). With the pair the database
// sections run, and an unreadable or corrupt database is a real failure. Without it they print an
// explicit SKIP line and the rest of the suite (path helpers, reader failure paths, address
// extraction, SQL) still runs. When CMake saw the pair at build time (LR_GEOIP_TEST_HAS_DB=1) but
// the files are gone at runtime, the test fails instead of hiding a broken data mount.
//
// The reader's failure paths (a non-existent path and a non-MMDB file) never need the GeoLite2
// data: TestOpenFailures() runs unconditionally.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "geoip_core.hpp"
#include "mmdb.hpp"

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

// The two database paths, resolved at runtime by ResolveGeoLiteData(); the database sections are
// called only after a complete City+Country pair was found.
std::string g_city_db;
std::string g_country_db;

inline constexpr char kCityFile[] = "GeoLite2-City.mmdb";
inline constexpr char kCountryFile[] = "GeoLite2-Country.mmdb";

struct GeoLiteData {
	std::string dir;
	bool city = false;
	bool country = false;

	bool Complete() const { return city && country; }
};

GeoLiteData InspectGeoLiteDir(std::string dir) {
	GeoLiteData data;
	data.dir = std::move(dir);
	if (data.dir.empty()) {
		return data;
	}
	data.city = FileExists(JoinPath(data.dir, kCityFile));
	data.country = FileExists(JoinPath(data.dir, kCountryFile));
	return data;
}

// The places the ~68 MB third-party data can be found: an explicit LR_GEOIP_TEST_DATA_DIR (runtime
// environment first, then the path CMake recorded), a packaged dist/, and the usual checkout and
// container layouts. Relative paths are deliberate: the container test runners mount the checkout
// at /src and run the binary from /src/build/tests, while a host run may start at the repo root.
std::vector<std::string> GeoLiteCandidates() {
	std::vector<std::string> candidates;
	const auto add = [&candidates](std::string candidate) {
		if (candidate.empty()) {
			return;
		}
		for (const std::string& existing : candidates) {
			if (existing == candidate) {
				return;
			}
		}
		candidates.push_back(std::move(candidate));
	};
	if (const char* env = std::getenv("LR_GEOIP_TEST_DATA_DIR"); env != nullptr) {
		add(env);
	}
	add(LR_GEOIP_TEST_DATA_DIR);
	add("dist/configs/geoip");
	add("plugins/lr_geoip/dist/configs/geoip");
	add("plugins/lr_geoip/tests/data/geoip");
	add("ref/prod-27016/addons/configs/geoip");
	add("ref/prod-27016/game/csgo/addons/configs/geoip");
	add("../../plugins/lr_geoip/dist/configs/geoip");
	add("../../plugins/lr_geoip/tests/data/geoip");
	add("../../ref/prod-27016/addons/configs/geoip");
	add("../../ref/prod-27016/game/csgo/addons/configs/geoip");
	add("/ref/prod-27016/addons/configs/geoip");
	return candidates;
}

// The first directory that holds BOTH files. A directory with only one of them is returned as a
// partial result (the caller reports the missing file); tests never run on a partial pair.
GeoLiteData ResolveGeoLiteData(std::string* tried = nullptr) {
	GeoLiteData firstPartial;
	std::string list;
	for (const std::string& candidate : GeoLiteCandidates()) {
		if (!list.empty()) {
			list += ", ";
		}
		list += candidate;
		GeoLiteData data = InspectGeoLiteDir(candidate);
		if (data.Complete()) {
			if (tried != nullptr) {
				*tried = list;
			}
			return data;
		}
		if (firstPartial.dir.empty() && (data.city || data.country)) {
			firstPartial = std::move(data);
		}
	}
	if (tried != nullptr) {
		*tried = list;
	}
	return firstPartial;
}

std::string PartialDescription(const GeoLiteData& data) {
	if (data.dir.empty()) {
		return {};
	}
	if (data.city) {
		return "only " + std::string(kCityFile);
	}
	if (data.country) {
		return "only " + std::string(kCountryFile);
	}
	return "neither " + std::string(kCityFile) + " nor " + std::string(kCountryFile);
}

// --- the MaxMind DB reader ----------------------------------------------------------------------

void TestCityDatabase() {
	Section("GeoLite2-City.mmdb");
	std::string error;
	std::unique_ptr<Mmdb> db = Mmdb::Open(g_city_db, &error);
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
	std::unique_ptr<Mmdb> db = Mmdb::Open(g_city_db, &error);
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
	std::unique_ptr<Mmdb> db = Mmdb::Open(g_country_db, &error);
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

// The reader's failure paths do not need the shipped data, so this section always runs - a missing
// GeoLite2 pair must not hide a broken Mmdb::Open.
void TestOpenFailures() {
	Section("reader failure paths (always run)");
	const std::string missing = "/nonexistent/GeoLite2-City.mmdb";
	// A non-existent path is rejected by the path helper as well as by the reader.
	CHECK(!FileExists(missing));
	std::string error;
	CHECK(Mmdb::Open(missing, &error) == nullptr);
	CHECK(!error.empty());
	std::printf("  error: %s\n", error.c_str());

	// A short non-MMDB file must be rejected, not misread.
	const std::string junk = "/tmp/lr_geoip_junk_test.mmdb";
	if (FILE* file = std::fopen(junk.c_str(), "wb"); file != nullptr) {
		std::fputs("this is not a maxmind db", file);
		std::fclose(file);
	}
	CHECK(Mmdb::Open(junk, &error) == nullptr);
	CHECK(!error.empty());
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
	const Mmdb* city = cache.Get(g_city_db, &error);
	CHECK(city != nullptr);
	if (city == nullptr) {
		std::printf("  open error: %s\n", error.c_str());
		return;
	}
	CHECK_EQ_STR(city->DatabaseType(), "GeoLite2-City");
	CHECK(cache.Get(g_city_db, &error) == city);

	// A replaced file is reopened: prime the cache with the Country database, overwrite the file
	// with the City one, and the next Get must read the new file.
	const std::string temp = "/tmp/lr_geoip_cache_test.mmdb";
	CHECK(CopyFile(g_country_db, temp));
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

		CHECK(CopyFile(g_city_db, temp));
		const Mmdb* replaced = cache.Get(temp, &error);
		CHECK(replaced != nullptr);
		if (replaced != nullptr) {
			CHECK_EQ_STR(replaced->DatabaseType(), "GeoLite2-City"); // reopened, not the stale file
		}

		// A file that disappeared: no database (and a reason), and it opens again once back.
		std::remove(temp.c_str());
		CHECK(cache.Get(temp, &error) == nullptr);
		CHECK(!error.empty());
		CHECK(CopyFile(g_country_db, temp));
		const Mmdb* restored = cache.Get(temp, &error);
		CHECK(restored != nullptr);
		if (restored != nullptr) {
			CHECK_EQ_STR(restored->DatabaseType(), "GeoLite2-Country");
		}
	}
	std::remove(temp.c_str());
}

// --- the database path helpers (pisex::JoinPath/FileExists/ResolvePath replacement) ---------------

void TestJoinPathAndFileExists() {
	Section("JoinPath / FileExists");
	CHECK_EQ_STR(JoinPath("", "GeoLite2-City.mmdb"), "GeoLite2-City.mmdb");
	CHECK_EQ_STR(JoinPath("/configs", ""), "/configs");
	CHECK_EQ_STR(JoinPath("", ""), "");
	CHECK_EQ_STR(JoinPath("/configs", "geoip"), "/configs/geoip");
	CHECK_EQ_STR(JoinPath("/configs/", "geoip"), "/configs/geoip");
	CHECK_EQ_STR(JoinPath("/configs", "/geoip"), "/configs/geoip");

	const std::string temp = "/tmp/lr_geoip_file_exists_test.bin";
	std::remove(temp.c_str());
	CHECK(!FileExists(temp));
	CHECK(!FileExists(""));
	if (FILE* file = std::fopen(temp.c_str(), "wb"); file != nullptr) {
		std::fputc('x', file);
		std::fclose(file);
	}
	CHECK(FileExists(temp));
	std::remove(temp.c_str());
	CHECK(!FileExists(temp));
}

void TestDatabaseCandidates() {
	Section("DatabaseCandidates");
	const std::vector<std::string> both = DatabaseCandidates("/configs", "/base");
	CHECK_EQ_INT(both.size(), 5);
	if (both.size() == 5) {
		CHECK_EQ_STR(both[0], "/configs/geoip/GeoLite2-City.mmdb");
		CHECK_EQ_STR(both[1], "/configs/lr_geoip/GeoLite2-City.mmdb");
		CHECK_EQ_STR(both[2], "/base/addons/configs/geoip/GeoLite2-City.mmdb");
		CHECK_EQ_STR(both[3], "/base/configs/geoip/GeoLite2-City.mmdb");
		CHECK_EQ_STR(both[4], kLegacyDatabasePath);
	}
	// Empty directories are skipped; the working-directory fallback is always last.
	const std::vector<std::string> configsOnly = DatabaseCandidates("/configs", "");
	CHECK_EQ_INT(configsOnly.size(), 3);
	const std::vector<std::string> baseOnly = DatabaseCandidates("", "/base");
	CHECK_EQ_INT(baseOnly.size(), 3);
	const std::vector<std::string> none = DatabaseCandidates("", "");
	CHECK_EQ_INT(none.size(), 1);
	if (!none.empty()) {
		CHECK_EQ_STR(none[0], kLegacyDatabasePath);
	}
	// <configs>/geoip and <base>/configs/geoip are the same file when configs == <base>/configs.
	const std::vector<std::string> dedup = DatabaseCandidates("/base/configs", "/base");
	CHECK_EQ_INT(dedup.size(), 4);
	if (dedup.size() == 4) {
		CHECK_EQ_STR(dedup[0], "/base/configs/geoip/GeoLite2-City.mmdb");
		CHECK_EQ_STR(dedup[1], "/base/configs/lr_geoip/GeoLite2-City.mmdb");
		CHECK_EQ_STR(dedup[2], "/base/addons/configs/geoip/GeoLite2-City.mmdb");
		CHECK_EQ_STR(dedup[3], kLegacyDatabasePath);
	}
}

std::filesystem::path CreateTempRoot(const std::string& tag) {
	const std::filesystem::path root =
		std::filesystem::temp_directory_path() / ("lr_geoip_paths_" + tag);
	std::error_code error;
	std::filesystem::remove_all(root, error);
	std::filesystem::create_directories(root, error);
	return root;
}

bool CreateEmptyFile(const std::filesystem::path& path) {
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
	std::ofstream file(path, std::ios::binary);
	return file.good();
}

void RemovePath(const std::filesystem::path& path) {
	std::error_code error;
	std::filesystem::remove(path, error);
}

// The priority order of the plugin (src/plugin.cpp ResolveDatabasePath): <configs>/geoip, the
// plugin's own <configs>/lr_geoip, the Metamod layout under <GetBaseDir()>, then the original path
// relative to the server working directory.
void TestResolveDatabasePath(const std::string& tag) {
	Section("ResolveDatabasePath (configs > plugin config > base > working dir)");
	const std::filesystem::path root = CreateTempRoot(tag);
	const std::filesystem::path configs = root / "configs";
	const std::filesystem::path base = root / "base";
	const std::filesystem::path working = root / "cwd";
	const std::string primary = (configs / "geoip" / std::string(kDatabaseFile)).string();
	const std::string own = (configs / "lr_geoip" / std::string(kDatabaseFile)).string();
	const std::string metamod = (base / "addons/configs/geoip" / std::string(kDatabaseFile)).string();
	const std::filesystem::path legacy = working / std::string(kLegacyDatabasePath);

	std::error_code error;
	std::filesystem::create_directories(working, error);
	const std::filesystem::path previous = std::filesystem::current_path(error);
	std::filesystem::current_path(working, error);

	// Nothing on disk: no path (the relative candidate is checked against the empty working dir).
	CHECK_EQ_STR(ResolveDatabasePath(configs.string(), base.string()), "");

	CHECK(CreateEmptyFile(legacy)); // the original Metamod path, relative to the working directory
	CHECK_EQ_STR(ResolveDatabasePath(configs.string(), base.string()), kLegacyDatabasePath);

	CHECK(CreateEmptyFile(metamod)); // <base>/addons/configs/geoip
	CHECK_EQ_STR(ResolveDatabasePath(configs.string(), base.string()), metamod);

	CHECK(CreateEmptyFile(own)); // <configs>/lr_geoip (PLUGIN_STANDARD §3)
	CHECK_EQ_STR(ResolveDatabasePath(configs.string(), base.string()), own);

	CHECK(CreateEmptyFile(primary)); // <configs>/geoip wins
	CHECK_EQ_STR(ResolveDatabasePath(configs.string(), base.string()), primary);

	// The first file disappears: the next candidate takes over again (a GeoLite2 update is not a
	// rename, but a deleted file must not leave the plugin pointing at nothing).
	RemovePath(primary);
	CHECK_EQ_STR(ResolveDatabasePath(configs.string(), base.string()), own);
	RemovePath(own);
	CHECK_EQ_STR(ResolveDatabasePath(configs.string(), base.string()), metamod);
	RemovePath(metamod);
	CHECK_EQ_STR(ResolveDatabasePath(configs.string(), base.string()), kLegacyDatabasePath);
	RemovePath(legacy);
	CHECK_EQ_STR(ResolveDatabasePath(configs.string(), base.string()), "");

	std::filesystem::current_path(previous, error);
	std::filesystem::remove_all(root, error);
}

// --- the original's address handling --------------------------------------------------------------

void TestExtractIp() {
	Section("ExtractIp (OnPlayerLoadedHook)");
	CHECK_EQ_STR(ExtractIp("1.2.3.4:27005"), "1.2.3.4");
	CHECK_EQ_STR(ExtractIp("1.2.3.4"), "1.2.3.4");
	CHECK_EQ_STR(ExtractIp("255.255.255.255:1"), "255.255.255.255");
	// IPv6: a bracketed address is unwrapped (with or without a port), a bare one is kept whole.
	CHECK_EQ_STR(ExtractIp("[::1]:27015"), "::1");
	CHECK_EQ_STR(ExtractIp("[::1]"), "::1");
	CHECK_EQ_STR(ExtractIp("2001:db8::1"), "2001:db8::1");
	CHECK_EQ_STR(ExtractIp("::1"), "::1");
	CHECK_EQ_STR(ExtractIp("["), "");
	CHECK_EQ_STR(ExtractIp("[]"), "");
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
	// The IPv6 address of a dual-stack client must fit the column.
	CHECK(CreateTableSql(table, plgdb::Driver::MySQL).find("`clientip` varchar(45)") != std::string::npos);
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

	// The reader's failure paths do not need the GeoLite2 files: run them first and unconditionally,
	// so a missing database pair cannot mask a regression in Mmdb::Open.
	TestOpenFailures();

	// The GeoLite2 databases are third-party data that is not committed (see CMakeLists.txt). The
	// directory is re-resolved at runtime, so the same binary works in a checkout, in the container
	// with the data mounted at /ref, and in a packaged tree. The sections run only with BOTH files;
	// a partial installation is reported explicitly and skipped.
	std::string tried;
	const GeoLiteData data = ResolveGeoLiteData(&tried);
	constexpr bool kDataExpected = LR_GEOIP_TEST_HAS_DB != 0;
	if (data.Complete()) {
		g_city_db = JoinPath(data.dir, kCityFile);
		g_country_db = JoinPath(data.dir, kCountryFile);
		std::printf("== GeoLite2 data: %s\n", data.dir.c_str());
		TestCityDatabase();
		TestCityCoverage();
		TestCountryDatabase();
		TestMmdbCache();
	} else if (kDataExpected) {
		// CMake found both files at build time: their disappearance is an environment/deployment
		// error. Fail loudly instead of hiding a broken test-data mount behind a SKIP.
		std::string partial;
		if (!data.dir.empty()) {
			partial = "; " + PartialDescription(data) + " in " + data.dir;
		}
		std::printf("FAIL [GeoLite2] the databases were present at build time, but no complete "
					"City+Country pair is visible now%s; tried: %s\n",
					partial.c_str(), tried.c_str());
		++g_failures;
	} else if (!data.dir.empty()) {
		std::printf("-- SKIP GeoLite2 sections: both %s and %s are required; %s in %s (tried: %s)\n",
					kCityFile, kCountryFile, PartialDescription(data).c_str(), data.dir.c_str(),
					tried.c_str());
	} else {
		std::printf("-- SKIP GeoLite2 sections: neither %s nor %s found; tried: %s\n", kCityFile,
					kCountryFile, tried.c_str());
	}
	TestJoinPathAndFileExists();
	TestDatabaseCandidates();
	TestResolveDatabasePath(tag);
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
