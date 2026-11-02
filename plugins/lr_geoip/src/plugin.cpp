// lr_geoip — Plugify C++ port of the Pisex LR module "[LR] GEOIP"
// (the original's lr_geoip.cpp; the Metamod module ships as addons/lr_modules/lr_geoip with
// lr_geoip.vdf).
//
// What it does (identical to the original): on every core player load, take the client's address,
// look the IP up in <configs>/geoip/GeoLite2-City.mmdb and write the country / region / city /
// country_code into the `<lr_table>_geoip` table (one row per SteamID).
//
// This plugin calls s2sdk directly - no pisexcompat, no guard library (docs/PLUGIN_STANDARD.md §7).
// The safety rules of handoff/weaponskins/PITFALLS.md therefore live right here:
//   * B1/B2 map gate: OnMapStart/OnMapEnd listeners are registered in OnPluginStart and the gate
//     g_mapReady is opened by OnMapStart (or by the first OnPluginUpdate when the plugin was loaded
//     into a running map) and dropped by OnMapEnd. The client address is read only while the gate
//     is open, so no s2sdk client call can reach a torn-down client list during a map change.
//   * C2 client checks: ClientIpAddress() requires the map gate, 0 <= slot < 64,
//     IsClientConnected, !IsClientSourceTV, !IsFakeClient and a non-null __s2sdk_GetClientIp
//     before s2sdk::GetClientIp is called.
//   * A7 unload: OnPluginEnd drops the gate, unhooks the levels_ranks callbacks, unregisters the
//     map listeners and closes the database, so `plg reload` leaves nothing behind that points
//     into the unloaded library.
//   * The database path is resolved by the plugin's own helpers (src/geoip_core.cpp): the
//     original pisex::JoinPath/FileExists/ResolvePath trio is gone, and no pisex:: global is read.
//
// Port notes (the rest is parity):
//   * The original linked libmaxminddb from its build machine. The plugify build image has no
//     libmaxminddb, so src/mmdb.{hpp,cpp} implement the small subset of the MaxMind DB v2 format
//     the module uses (mmap + tree walk + data decoder); tests/lr_geoip_tests.cpp verifies it
//     against the shipped GeoLite2-City.mmdb.
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
//   * Errors go through plg::Log (with the null check every pure-s2sdk plugin uses), never
//     pisex::ErrorLog.

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include <lr_api.hpp>

#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "geoip_core.hpp"
#include "mmdb.hpp"

namespace {

// GetLogTag() of the original.
constexpr char kLogTag[] = "[LR-GEOIP]";

// s2sdk player slots are 0..63 (CS2's MAXPLAYERS); the original fed the hook's slot to the engine,
// the port bounds-checks it (PITFALLS C2).
constexpr int kMaxSlots = 64;

// The original's g_pLRCore.
levels_ranks::LrApi* g_pLRCore = &levels_ranks::LrApi::Get();

// "No GeoIP entry found" is expected for private/LAN addresses; each distinct address is reported
// once instead of on every connect (the repetition is console noise, the first line is not). The
// cap keeps a long-lived server from growing the set without bound; an address seen for the first
// time after the cap is still reported (and again on its next connect, since it is not recorded).
constexpr std::size_t kMaxMissingAddresses = 512;

using namespace lr_geoip;

// --- logging (pisex::ErrorLog replacement) -----------------------------------------------------

void LogMessage(std::string_view message, plg::Severity severity) {
	// plg::Log is filled by the plugify module right before OnPluginStart; the null check keeps the
	// plugin silent (not crashing) in an unusual host that never provides it.
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

// The printf-style convenience of the original's pisex::ErrorLog, on top of plg::Log; the format
// attribute keeps -Werror format checking working for every call site.
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void LogError(const char* format, ...) {
	if (plg::Log == nullptr) {
		return;
	}
	va_list args;
	va_start(args, format);
	va_list sizeArgs;
	va_copy(sizeArgs, args);
	const int size = std::vsnprintf(nullptr, 0, format, sizeArgs);
	va_end(sizeArgs);
	std::string message;
	if (size > 0) {
		message.resize(static_cast<std::size_t>(size) + 1);
		std::vsnprintf(message.data(), message.size(), format, args);
		message.resize(static_cast<std::size_t>(size));
	} else {
		message = format; // vsnprintf failed: still report something useful
	}
	va_end(args);
	LogMessage(message, plg::Severity::Error);
}

std::string ToString(const plg::string& text) {
	return std::string(std::string_view(text));
}

// --- map gate (PITFALLS B1/B2) ---------------------------------------------------------------

bool g_mapReady = false;              // a map is up: s2sdk client calls are safe
bool g_mapStartSeen = false;          // an OnMapStart arrived since OnPluginStart / the last OnMapEnd
bool g_mapEndSeen = false;            // an OnMapEnd arrived since OnPluginStart
bool g_mapListenersRegistered = false; // OnMapStart/OnMapEnd are registered with s2sdk

// s2sdk listener callbacks are plain function pointers called on the game thread; nothing here may
// throw, and OnMapEnd must not touch a client (PITFALLS B2).

void OnMapStartListener() {
	// The engine calls this once the new map and its client list exist: readiness is granted here
	// and by the next OnPluginUpdate (the PITFALLS B1 work is only legal from this point on).
	g_mapReady = true;
	g_mapStartSeen = true;
}

void OnMapEndListener() {
	// From here until the next OnMapStart s2sdk's client list is torn down and any client query
	// crashes - so this handler only drops state.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
}

void RegisterMapListeners() {
	if (g_mapListenersRegistered) {
		return;
	}
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr) {
		// Without the listeners nothing could ever close the gate again, so it is never opened
		// (fail safe): the GeoIP writes stay off instead of risking a query during a map change.
		LogError("%s the s2sdk map listeners are unavailable - the GeoIP lookups stay off", kLogTag);
		return;
	}
	s2sdk::OnMapStart_Register(&OnMapStartListener);
	s2sdk::OnMapEnd_Register(&OnMapEndListener);
	g_mapListenersRegistered = true;
}

void UnregisterMapListeners() {
	if (!g_mapListenersRegistered) {
		return;
	}
	g_mapListenersRegistered = false;
	if (__s2sdk_OnMapStart_Unregister != nullptr) {
		s2sdk::OnMapStart_Unregister(&OnMapStartListener);
	}
	if (__s2sdk_OnMapEnd_Unregister != nullptr) {
		s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
	}
}

// --- client checks (PITFALLS C2) ---------------------------------------------------------------

// The client's address, or "" when it cannot be read right now: no map, no client, a bot, GOTV or
// a host that never filled the s2sdk binding. Only a result of this function may be handed to
// ExtractIp / the MaxMind lookup.
std::string ClientIpAddress(int iSlot) {
	if (!g_mapReady || iSlot < 0 || iSlot >= kMaxSlots) {
		return {};
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientSourceTV == nullptr ||
		__s2sdk_IsFakeClient == nullptr || __s2sdk_GetClientIp == nullptr) {
		return {};
	}
	try {
		if (!s2sdk::IsClientConnected(iSlot) || s2sdk::IsClientSourceTV(iSlot) ||
			s2sdk::IsFakeClient(iSlot)) {
			return {};
		}
		return ToString(s2sdk::GetClientIp(iSlot));
	} catch (...) {
		return {};
	}
}

// --- the plugin ----------------------------------------------------------------------------------

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
	// The original's addons/configs/geoip/GeoLite2-City.mmdb is mapped onto <configs>/geoip (and
	// the plugin's own <configs>/lr_geoip, PLUGIN_STANDARD §3); the Metamod spellings relative to
	// <GetBaseDir()> and to the working directory are the fallbacks. The core owns the order.
	const std::string configsDir =
		plg::GetConfigsDir != nullptr ? ToString(plg::GetConfigsDir()) : std::string();
	const std::string baseDir = plg::GetBaseDir != nullptr ? ToString(plg::GetBaseDir()) : std::string();
	const std::vector<std::string> candidates = DatabaseCandidates(configsDir, baseDir);
	for (const std::string& candidate : candidates) {
		if (FileExists(candidate)) {
			m_mmdb_path = candidate;
			return true;
		}
	}
	std::string list;
	for (const std::string& candidate : candidates) {
		if (!list.empty()) {
			list += ", ";
		}
		list += candidate;
	}
	LogError("%s Failed to locate the GeoIP database (tried: %s)", kLogTag, list.c_str());
	return false;
}

// --- the core callbacks --------------------------------------------------------------------------

void LrGeoipPlugin::OnCoreIsReady() {
	// OnCoreIsReadyHook() of the original: table name, connection, CREATE TABLE, the hook.
	m_table = TableName(g_pLRCore->GetTableName());
	m_db = plgdb::Database::Create(
		levels_ranks::MakeDatabaseConfig(g_pLRCore->GetDatabaseConfig(), "lr.sqlite3"));
	if (m_db == nullptr) {
		LogError("%s the database configuration is invalid - the GeoIP data is not stored", kLogTag);
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
	if (!g_mapReady) {
		// PITFALLS B1/B2: no client query before the first map or in the map-change window. There
		// is nothing to store then either.
		return;
	}
	const std::string ip = ExtractIp(ClientIpAddress(slot));
	if (ip.empty()) {
		LogError("%s Failed to retrieve IP address for slot %d", kLogTag, slot);
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
		LogError("%s Error opening the GeoIP database: %s", kLogTag, error.c_str());
		return;
	}
	Mmdb::Location location;
	if (!database->Lookup(ip, &location)) {
		// The original's gai_error path: the address could not be parsed.
		LogError("%s GeoIP lookup failed for IP: %s", kLogTag, ip.c_str());
		return;
	}
	if (!location.found) {
		// Logged once per distinct address: a LAN/private IP misses on every connect of every player
		// behind it, and the repetition (the config's expected case) is not operational news.
		if (m_missing_addresses.count(ip) != 0) {
			return; // already reported once (the set may be at its cap, so this check comes first)
		}
		if (m_missing_addresses.size() < kMaxMissingAddresses) {
			m_missing_addresses.insert(ip);
		}
		LogError("%s No GeoIP entry found for IP: %s", kLogTag, ip.c_str());
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
		LogError("%s Database connection is null. Query not executed.", kLogTag);
		return;
	}
	Statement statement = UpsertStatement(m_table, row);
	m_db->Query(std::move(statement.sql), std::move(statement.params),
				[](const plgdb::Result& result) {
					if (!result.Ok()) {
						LogError("%s Failed to execute GeoIP query: %s", kLogTag,
								 result.Error().c_str());
					}
				});
}

// --- lifecycle ------------------------------------------------------------------------------------

plg::PluginResult LrGeoipPlugin::OnPluginStart() {
	try {
		// Only the map listeners - no client call happens here (PITFALLS B1: none exists before the
		// first map). The database path probe is a filesystem check and is safe at any time.
		RegisterMapListeners();
		ResolveDatabasePath(); // logged; the lookup retries per player load

		// AllPluginsLoaded() of the original.
		if (g_pLRCore->CoreIsLoaded()) {
			OnCoreIsReady();
		} else {
			g_pLRCore->HookOnCoreIsReady([this] { OnCoreIsReady(); });
		}
		LogInfo(std::string(kLogTag) + " ready");
		return plg::PluginResult{};
	} catch (const std::exception& ex) {
		LogError("%s OnPluginStart failed: %s", kLogTag, ex.what());
	} catch (...) {
		LogError("%s OnPluginStart failed with an unknown exception", kLogTag);
	}
	return plg::PluginResult{}; // never Failed: that stops every plugify plugin
}

plg::PluginResult LrGeoipPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		// PITFALLS B1/B2: the host pumps this from the server frame, so the first update proves a
		// live map for a plugin loaded into a running map (it never saw an OnMapStart). An OnMapEnd
		// seen since OnPluginStart means the server is between maps, so the latch waits for the next
		// OnMapStart. Without the map listeners nothing could ever close the gate again, so it is
		// not opened at all then (fail safe).
		if (!g_mapReady && g_mapListenersRegistered && (g_mapStartSeen || !g_mapEndSeen)) {
			g_mapReady = true;
		}
		if (m_db != nullptr) {
			m_db->Poll();
		}
	} catch (const std::exception& ex) {
		LogError("%s OnPluginUpdate failed: %s", kLogTag, ex.what());
	} catch (...) {
		LogError("%s OnPluginUpdate failed with an unknown exception", kLogTag);
	}
	return plg::PluginResult{};
}

plg::PluginResult LrGeoipPlugin::OnPluginEnd() {
	try {
		// PITFALLS A7: nothing may point into this library after the unload.
		g_mapReady = false;
		// The Unhook* are no-ops when levels_ranks is not loaded (lr_api.hpp Unregister checks the
		// null binding slot); the Available() guard only keeps that intent visible here.
		if (g_pLRCore->Available()) {
			if (m_db_hooked) {
				g_pLRCore->UnhookOnPlayerLoaded();
				m_db_hooked = false;
			}
			g_pLRCore->UnhookOnCoreIsReady();
		}
		UnregisterMapListeners();
		g_mapStartSeen = false;
		g_mapEndSeen = false;
		m_db.reset();
	} catch (...) {
		// nothing left to do at unload
	}
	return plg::PluginResult{};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
