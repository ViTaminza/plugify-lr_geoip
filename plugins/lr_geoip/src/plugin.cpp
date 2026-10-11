// lr_geoip — Plugify C++ port of the Pisex LR module "[LR] GEOIP"
// (the original's lr_geoip.cpp; the Metamod module ships as addons/lr_modules/lr_geoip with
// lr_geoip.vdf).
//
// What it does (identical to the original): on every core player load, take the client's address,
// look the IP up in <configs>/geoip/GeoLite2-City.mmdb and write the country / region / city /
// country_code into the `<lr_table>_geoip` table (one row per SteamID).
//
// Port notes (the rest is parity):
//   * The original linked libmaxminddb from its build machine. The plugify build image has no
//     libmaxminddb, so src/mmdb.{hpp,cpp} implement the small subset of the MaxMind DB v2 format
//     the module uses (mmap + tree walk + data decoder); tests/geoip_tests.cpp verifies it against
//     the shipped GeoLite2-City.mmdb.
//   * The database is its own plgdb connection with the core's settings
//     (GetDatabaseConfig(), "lr.sqlite3" for the SQLite fallback); a Plugify plugin cannot share
//     another plugin's connection object. The `INSERT ... ON DUPLICATE KEY UPDATE` of the original
//     (MySQL only) is one REPLACE INTO, which both drivers accept with the same effect.
//   * The field truncation of the original's char buffers is kept: country/region/city to 44
//     characters, country_code to 2 (the widths of the original's buffers).
//   * The original opened the .mmdb on every player load (open + mmap + metadata parse of the
//     ~60 MB file on the game thread); the port keeps one open database and reopens it only when
//     the file changed (its size/modification time), so an updated GeoLite2 file is still picked
//     up without a reload.
//   * Client work happens in the OnPlayerLoaded hook, i.e. never before a map exists.

#include <pisex/pisex.hpp>

#include <plg/plugin.hpp>

#include <s2sdk/s2sdk.hpp>

#include <lr_api.hpp>

#include <cstdint>
#include <exception>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "geoip_core.hpp"
#include "mmdb.hpp"

namespace {

// GetLogTag() of the original.
constexpr char kLogTag[] = "[LR-GEOIP]";

// The original's g_pLRCore.
levels_ranks::LrApi* g_pLRCore = &levels_ranks::LrApi::Get();

// "No GeoIP entry found" is expected for private/LAN addresses; each distinct address is reported
// once instead of on every connect (the repetition is console noise, the first line is not). The
// cap keeps a long-lived server from growing the set without bound.
constexpr std::size_t kMaxMissingAddresses = 512;

using namespace lr_geoip;

class LrGeoipPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

private:
	void OnCoreIsReady();
	void OnPlayerLoaded(int slot, const char* steamid);
	// AllPluginsLoaded() of the original: the module aborts when the LR core is missing; the port
	// logs instead (dropping the plugin would stop every Plugify plugin).
	bool ResolveDatabasePath();

	std::unique_ptr<plgdb::Database> m_db;
	std::string m_table;   // <lr_table>_geoip
	std::string m_mmdb_path;
	// The MaxMind database, opened on the first lookup and reused (reopened when the file changes).
	MmdbCache m_mmdb;
	bool m_db_hooked = false;
	// Addresses whose missing GeoIP entry has been logged already (see kMaxMissingAddresses).
	std::set<std::string> m_missing_addresses;
};

LrGeoipPlugin g_plugin;

// --- the MaxMind database ------------------------------------------------------------------------

bool LrGeoipPlugin::ResolveDatabasePath() {
	// The Metamod layout is addons/configs/geoip/GeoLite2-City.mmdb; under Plugify that is
	// <configs>/geoip/GeoLite2-City.mmdb. pisex::ResolvePath also tries the path as given (relative
	// to the server working directory, i.e. the Metamod layout) and the plugin's own config/data
	// directories.
	const std::string relative = "addons/configs/geoip/GeoLite2-City.mmdb";
	std::vector<std::string> tried;
	if (plg::GetConfigsDir != nullptr) {
		const std::string absolute = pisex::JoinPath(plg::GetConfigsDir(), "geoip/GeoLite2-City.mmdb");
		tried.push_back(absolute);
		if (pisex::FileExists(absolute)) {
			m_mmdb_path = absolute;
			return true;
		}
	}
	const std::string resolved = pisex::ResolvePath(relative);
	if (!resolved.empty()) {
		m_mmdb_path = resolved;
		return true;
	}
	tried.push_back(relative);
	std::string list;
	for (const std::string& candidate : tried) {
		if (!list.empty()) {
			list += ", ";
		}
		list += candidate;
	}
	pisex::ErrorLog("%s Failed to locate the GeoIP database (tried: %s)", kLogTag, list.c_str());
	return false;
}

// --- the core callbacks --------------------------------------------------------------------------

void LrGeoipPlugin::OnCoreIsReady() {
	// OnCoreIsReadyHook() of the original: table name, connection, CREATE TABLE, the hook.
	m_table = TableName(g_pLRCore->GetTableName());
	m_db = plgdb::Database::Create(
		levels_ranks::MakeDatabaseConfig(g_pLRCore->GetDatabaseConfig(), "lr.sqlite3"));
	if (m_db == nullptr) {
		pisex::ErrorLog("%s the database configuration is invalid - the GeoIP data is not stored", kLogTag);
	} else {
		// Queued (FIFO before every later statement), like the original's async Query.
		m_db->Query(CreateTableSql(m_table, m_db->GetDriver()), {});
	}
	if (!m_db_hooked) {
		g_pLRCore->HookOnPlayerLoaded([this](int slot, const char* steamid) { OnPlayerLoaded(slot, steamid); });
		m_db_hooked = true;
	}
}

void LrGeoipPlugin::OnPlayerLoaded(int slot, const char* steamid) {
	// OnPlayerLoadedHook() of the original, in order: the address, then the lookup, then the query.
	const char* address = pisex::g_pPlayers->GetIpAddress(slot);
	const std::string ip = ExtractIp(address != nullptr ? address : "");
	if (ip.empty()) {
		pisex::ErrorLog("%s Failed to retrieve IP address for slot %d", kLogTag, slot);
		return;
	}
	if (m_mmdb_path.empty() && !ResolveDatabasePath()) {
		return; // logged by ResolveDatabasePath
	}
	std::string error;
	const Mmdb* database = m_mmdb.Get(m_mmdb_path, &error);
	if (database == nullptr) {
		// The original's database-open error; the open database is kept between lookups and a
		// changed file is reopened, so a replaced database is still picked up without a reload.
		pisex::ErrorLog("%s Error opening the GeoIP database: %s", kLogTag, error.c_str());
		return;
	}
	Mmdb::Location location;
	if (!database->Lookup(ip, &location)) {
		// The original's gai_error path: the address could not be parsed.
		pisex::ErrorLog("%s GeoIP lookup failed for IP: %s", kLogTag, ip.c_str());
		return;
	}
	if (!location.found) {
		// Logged once per distinct address: a LAN/private IP misses on every connect of every player
		// behind it, and the repetition (the config's expected case) is not operational news.
		if (m_missing_addresses.size() < kMaxMissingAddresses &&
			!m_missing_addresses.insert(ip).second) {
			return;
		}
		pisex::ErrorLog("%s No GeoIP entry found for IP: %s", kLogTag, ip.c_str());
		return;
	}
	Row row;
	row.steam = steamid != nullptr ? steamid : "";
	row.clientip = ip;
	row.country = location.country;
	row.region = location.region;
	row.city = location.city;
	row.country_code = location.country_code;
	if (m_db == nullptr) {
		pisex::ErrorLog("%s Database connection is null. Query not executed.", kLogTag);
		return;
	}
	Statement statement = UpsertStatement(m_table, row);
	m_db->Query(std::move(statement.sql), std::move(statement.params),
				[](const plgdb::Result& result) {
					if (!result.Ok()) {
						pisex::ErrorLog("%s Failed to execute GeoIP query: %s", kLogTag,
										result.Error().c_str());
					}
				});
}

// --- lifecycle ------------------------------------------------------------------------------------

plg::PluginResult LrGeoipPlugin::OnPluginStart() {
	try {
		pisex::Init("lr_geoip");
		ResolveDatabasePath(); // logged; the lookup retries per player load

		// AllPluginsLoaded() of the original.
		if (g_pLRCore->CoreIsLoaded()) {
			OnCoreIsReady();
		} else {
			g_pLRCore->HookOnCoreIsReady([this] { OnCoreIsReady(); });
		}
		return plg::PluginResult{};
	} catch (const std::exception& ex) {
		pisex::ErrorLog("%s OnPluginStart failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::ErrorLog("%s OnPluginStart failed with an unknown exception", kLogTag);
	}
	return plg::PluginResult{}; // never Failed: that stops every plugify plugin
}

plg::PluginResult LrGeoipPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		pisex::Poll();
		if (m_db != nullptr) {
			m_db->Poll();
		}
	} catch (const std::exception& ex) {
		pisex::ErrorLog("%s OnPluginUpdate failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::ErrorLog("%s OnPluginUpdate failed with an unknown exception", kLogTag);
	}
	return plg::PluginResult{};
}

plg::PluginResult LrGeoipPlugin::OnPluginEnd() {
	try {
		if (m_db_hooked) {
			g_pLRCore->UnhookOnPlayerLoaded();
			m_db_hooked = false;
		}
		g_pLRCore->UnhookOnCoreIsReady();
		pisex::Shutdown();
		m_db.reset();
	} catch (...) {
		// nothing left to do at unload
	}
	return plg::PluginResult{};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
