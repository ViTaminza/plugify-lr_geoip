#pragma once
// Minimal, dependency-free reader for MaxMind DB (mmdb) version 2 files — the format of the
// GeoLite2-City.mmdb / GeoLite2-Country.mmdb that ship in
// addons/configs/geoip/ — plus the small cache (MmdbCache) that keeps the open database for
// reuse.
//
// The original module linked libmaxminddb.a from its build machine; the plugify build image has no
// libmaxminddb, so this file re-implements the small subset the module uses:
//   * open the file with mmap (read-only, lazily paged — the City database is ~60 MB),
//   * decode the metadata section (the map behind the "\xAB\xCD\xEFMaxMind.com" marker),
//   * walk the binary search tree for an IPv4/IPv6 address (record sizes 24/28/32),
//   * decode the data-section value behind the record (the type table of the spec) and pick the
//     four fields the original read: country.names.en, country.iso_code,
//     subdivisions[0].names.en, city.names.en.
//
// Format details, verified against the two shipped databases
// (tests/geoip_tests.cpp decodes them and checks the results):
//   * the metadata starts right after the last marker occurrence; its pointers resolve from there;
//   * the tree is followed by a 16-byte zero separator, then the single data section (GeoLite2
//     files have exactly one); data pointers and record values both resolve from the first byte
//     after the separator;
//   * a record < node_count is the next node, == node_count is "no data", > node_count is a data
//     pointer with offset (record - node_count - 16);
//   * 28-bit nodes are 7 bytes: each record is a 4-bit high nibble followed by a 24-bit big-endian
//     value (the left record's nibble is the high nibble of byte 3);
//   * pointers: the 2 size bits (3-4) of the control byte select the width (1..4 bytes), the low
//     3 bits of the control byte are the *high* bits of the value, and sizes 1/2 add 2048/526336;
//   * integers are written with the minimum number of bytes (the size field is 0..width);
//   * in an IPv6 database the IPv4 subtree is reached through a 96-bit prefix; both the
//     IPv4-mapped (::ffff:0:0/96) and the zero (::/96) spellings are tried, because the shipped
//     files answer either.
//
// Everything is engine-free and testable (tests/geoip_tests.cpp reads the shipped databases).

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lr_geoip {

class Mmdb {
public:
	// The four strings the original selected from the record (empty when the database has no value
	// for the field). |region| is the first subdivision entry of the City database.
	struct Location {
		bool found = false;         // result.found_entry of the original
		std::string country;        // country.names.en
		std::string country_code;   // country.iso_code
		std::string region;         // subdivisions.0.names.en
		std::string city;           // city.names.en
	};

	// One decoded value of the data format (the metadata map and the records).
	struct Value {
		enum class Kind {
			None,    // end marker / not decoded
			String,
			Bytes,
			Uint,
			Int,
			Double,
			Bool,
			Map,
			Array,
		};
		Kind kind = Kind::None;
		std::string text;                               // String/Bytes
		uint64_t uint = 0;                              // Uint
		int64_t sint = 0;                               // Int
		double dbl = 0.0;                               // Double
		bool boolean = false;                           // Bool
		std::vector<std::pair<std::string, Value>> map; // Map
		std::vector<Value> array;                       // Array

		// Map lookup (nullptr when the key is missing or the value is not a map).
		const Value* Find(std::string_view key) const;
		const Value* At(std::size_t index) const; // array element (nullptr when out of range)
	};

	~Mmdb();
	Mmdb(const Mmdb&) = delete;
	Mmdb& operator=(const Mmdb&) = delete;

	// Maps |path| read-only. Returns nullptr and fills *error on failure (the original's
	// MMDB_open error path).
	static std::unique_ptr<Mmdb> Open(const std::string& path, std::string* error = nullptr);

	// Looks up a dotted-quad IPv4 or an IPv6 address string. Returns false only when the address
	// itself cannot be parsed or the internal state is broken; an address that is simply not in the
	// database sets out->found = false (the original's found-entry flag was false).
	bool Lookup(std::string_view address, Location* out) const;

	// Decodes the value at |offset| (absolute file offset). |section_start| is where the pointers
	// of that section resolve from (the metadata start, or the data contents start).
	bool DecodeValue(std::size_t section_start, std::size_t offset, Value* out,
					 std::size_t* next = nullptr) const;

	bool IsValid() const { return data_ != nullptr; }
	std::string_view DatabaseType() const { return database_type_; }
	std::string_view Error() const { return error_; }
	// Metadata values (exposed for diagnostics and the tests).
	uint32_t NodeCount() const { return node_count_; }
	uint32_t RecordSize() const { return record_size_; }
	uint32_t IpVersion() const { return ip_version_; }

private:
	Mmdb() = default;

	// The metadata section parser (Open): node_count / record_size / ip_version / separator check.
	bool ParseMetadata();
	// The recursive value decoder behind DecodeValue.
	bool Decode(std::size_t section_start, std::size_t offset, Value* out, std::size_t* next,
				int depth) const;
	// The record |right| (0/1) of node |node|; false when out of the tree's bounds.
	bool ReadRecord(uint32_t node, int right, uint32_t* out) const;
	// Walks |bits| bits of |prefix|; returns the raw data record reached (see Format notes).
	bool Walk(const uint8_t* prefix, std::size_t bits, uint32_t* record) const;
	// Decodes the data value a tree record points at and picks the four Location fields.
	bool DecodeRecord(uint32_t record, Location* out) const;
	// One address: 4 bytes (IPv4) or 16 (IPv6); |ipv4_mapped| selects the 96-bit prefix used for
	// an IPv4 address in an IPv6 database.
	bool LookupBytes(const uint8_t* address, std::size_t size, bool ipv4_mapped, Location* out) const;

	// mmap'd file bytes; nullptr when the object is not valid.
	const uint8_t* data_ = nullptr;
	std::size_t size_ = 0;

	uint32_t node_count_ = 0;
	uint32_t record_size_ = 24;
	uint32_t ip_version_ = 4;
	std::size_t node_bytes_ = 0;     // the tree's byte size
	std::size_t data_start_ = 0;     // first byte of the data section (after the 16-byte separator)
	std::size_t metadata_start_ = 0; // first byte of the metadata section
	std::string database_type_;
	std::string error_;
};

// One open database kept for reuse: opening the ~60 MB GeoLite2-City.mmdb (open + mmap + the
// metadata parse) on every player load is wasted work. The cache hands out the open database and
// reopens the file only when it changed on disk (size + modification time), so an updated
// GeoLite2 file is picked up without a plugin reload.
class MmdbCache {
public:
	// The database of |path|: opened now when there is no open one or the file changed; nullptr
	// (with *error filled) when it cannot be opened. A path of another file drops the open
	// database.
	const Mmdb* Get(const std::string& path, std::string* error = nullptr);

private:
	std::string path_;
	std::unique_ptr<Mmdb> db_;
	std::int64_t size_ = -1;  // of the file the open database was mapped from
	std::int64_t mtime_ = -1;
};

} // namespace lr_geoip
