#include "mmdb.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace lr_geoip {
namespace {

// "\xAB\xCD\xEF" + "MaxMind.com" — the marker in front of the metadata section.
constexpr char kMarker[] = "\xAB\xCD\xEFMaxMind.com";
constexpr std::size_t kMarkerSize = sizeof(kMarker) - 1; // without the NUL
// The data section is preceded by 16 zero bytes (MMDB_DATA_SECTION_SEPARATOR); both the tree's
// record values and the data pointers are relative to the start of that separator + 16.
constexpr std::size_t kSeparator = 16;
// Depth cap of the value decoder (real records nest about six levels).
constexpr int kMaxDepth = 48;
// Cap of a map/array entry count (a corrupt file must not try to allocate gigabytes).
constexpr uint64_t kMaxContainer = 1u << 20;

uint32_t ReadBig(const uint8_t* bytes, std::size_t count) {
	uint32_t value = 0;
	for (std::size_t i = 0; i < count; ++i) {
		value = (value << 8) | bytes[i];
	}
	return value;
}

std::string Truncate(std::string text, std::size_t length) {
	if (text.size() > length) {
		text.resize(length);
	}
	return text;
}

} // namespace

Mmdb::~Mmdb() {
	if (data_ != nullptr) {
		::munmap(const_cast<uint8_t*>(data_), size_);
	}
}

std::unique_ptr<Mmdb> Mmdb::Open(const std::string& path, std::string* error) {
	const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		if (error != nullptr) {
			*error = "cannot open " + path + ": " + std::strerror(errno);
		}
		return nullptr;
	}
	struct stat info {};
	if (::fstat(fd, &info) != 0 || info.st_size <= 0) {
		::close(fd);
		if (error != nullptr) {
			*error = "cannot stat " + path;
		}
		return nullptr;
	}
	void* mapping = ::mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
	::close(fd);
	if (mapping == MAP_FAILED) {
		if (error != nullptr) {
			*error = "cannot mmap " + path + ": " + std::strerror(errno);
		}
		return nullptr;
	}

	std::unique_ptr<Mmdb> db(new Mmdb());
	db->data_ = static_cast<const uint8_t*>(mapping);
	db->size_ = static_cast<std::size_t>(info.st_size);
	if (!db->ParseMetadata()) {
		if (error != nullptr) {
			*error = path + ": " + (db->error_.empty() ? std::string("not a MaxMind DB v2 file") : db->error_);
		}
		return nullptr; // ~Mmdb unmaps
	}
	return db;
}

const Mmdb* MmdbCache::Get(const std::string& path, std::string* error) {
	if (path != path_) {
		path_ = path;
		db_.reset();
		size_ = -1;
		mtime_ = -1;
	}
	struct stat info {};
	if (path.empty() || ::stat(path.c_str(), &info) != 0) {
		// A file that vanished (or is unreadable) must not keep an open database alive.
		db_.reset();
		size_ = -1;
		mtime_ = -1;
		if (error != nullptr) {
			*error = "cannot access " + path + ": " + std::strerror(errno);
		}
		return nullptr;
	}
	if (db_ != nullptr && static_cast<std::int64_t>(info.st_size) == size_ &&
		static_cast<std::int64_t>(info.st_mtime) == mtime_) {
		return db_.get(); // the same file: the open database is reused
	}
	// The first use, or the file changed on disk (a GeoLite2 update): (re)open it. A failed open
	// drops the cached database, so the next Get retries.
	std::unique_ptr<Mmdb> db = Mmdb::Open(path_, error);
	if (db == nullptr) {
		db_.reset();
		size_ = -1;
		mtime_ = -1;
		return nullptr;
	}
	db_ = std::move(db);
	size_ = static_cast<std::int64_t>(info.st_size);
	mtime_ = static_cast<std::int64_t>(info.st_mtime);
	return db_.get();
}

const Mmdb::Value* Mmdb::Value::Find(std::string_view key) const {
	if (kind != Kind::Map) {
		return nullptr;
	}
	for (const auto& [name, value] : map) {
		if (name == key) {
			return &value;
		}
	}
	return nullptr;
}

const Mmdb::Value* Mmdb::Value::At(std::size_t index) const {
	if (kind != Kind::Array || index >= array.size()) {
		return nullptr;
	}
	return &array[index];
}

bool Mmdb::ParseMetadata() {
	if (size_ < kMarkerSize) {
		error_ = "file too small";
		return false;
	}
	// The metadata section is the last thing in the file; take the last marker occurrence (the
	// byte sequence could also appear inside the data section).
	std::size_t marker = std::string::npos;
	for (std::size_t i = size_ - kMarkerSize + 1; i-- > 0;) {
		if (std::memcmp(data_ + i, kMarker, kMarkerSize) == 0) {
			marker = i;
			break;
		}
	}
	if (marker == std::string::npos) {
		error_ = "metadata marker not found";
		return false;
	}
	metadata_start_ = marker + kMarkerSize;

	Value metadata;
	if (!DecodeValue(metadata_start_, metadata_start_, &metadata) || metadata.kind != Value::Kind::Map) {
		error_ = "metadata is not a map";
		return false;
	}
	const auto read_uint = [&metadata](std::string_view key, uint32_t* out) {
		const Value* value = metadata.Find(key);
		if (value == nullptr || value->kind != Value::Kind::Uint) {
			return false;
		}
		*out = static_cast<uint32_t>(value->uint);
		return true;
	};
	if (!read_uint("node_count", &node_count_)) {
		error_ = "metadata has no node_count";
		return false;
	}
	if (!read_uint("record_size", &record_size_)) {
		error_ = "metadata has no record_size";
		return false;
	}
	if (!read_uint("ip_version", &ip_version_)) {
		ip_version_ = 6;
	}
	if (node_count_ == 0 || (record_size_ != 24 && record_size_ != 28 && record_size_ != 32) ||
		(ip_version_ != 4 && ip_version_ != 6)) {
		error_ = "metadata has invalid values";
		return false;
	}
	if (const Value* type = metadata.Find("database_type"); type != nullptr && type->kind == Value::Kind::String) {
		database_type_ = type->text;
	}

	node_bytes_ = static_cast<std::size_t>(node_count_) * record_size_ * 2 / 8;
	if (node_bytes_ + kSeparator > size_) {
		error_ = "tree does not fit into the file";
		return false;
	}
	// The 16-byte separator is what the data offsets are counted from; a file without it would
	// make every lookup silently wrong, so it is validated here.
	for (std::size_t i = 0; i < kSeparator; ++i) {
		if (data_[node_bytes_ + i] != 0) {
			error_ = "data section separator is not zero";
			return false;
		}
	}
	data_start_ = node_bytes_ + kSeparator;
	return true;
}

bool Mmdb::DecodeValue(std::size_t section_start, std::size_t offset, Value* out, std::size_t* next) const {
	return Decode(section_start, offset, out, next, 0);
}

bool Mmdb::Decode(std::size_t section_start, std::size_t offset, Value* out, std::size_t* next, int depth) const {
	if (out == nullptr || data_ == nullptr || depth > kMaxDepth || offset >= size_) {
		return false;
	}
	*out = Value{};
	std::size_t pos = offset;
	uint8_t control = data_[pos++];
	unsigned type = control >> 5;

	if (type == 1) {
		// Pointer: the 2 size bits (3-4) select the width (1..4 bytes); the low 3 bits of the
		// control byte are the *high* bits of the value (verified: 0x21 0x10 -> offset 272, the
		// "time_zone" string); sizes 1 and 2 add 2048 and 526336.
		const uint32_t selector = (control >> 3) & 0x03;
		const int width = static_cast<int>(selector) + 1;
		if (pos + static_cast<std::size_t>(width) > size_) {
			return false;
		}
		uint64_t pointer = (static_cast<uint64_t>(control & 0x07) << (8 * width)) |
						   ReadBig(data_ + pos, static_cast<std::size_t>(width));
		pos += static_cast<std::size_t>(width);
		if (selector == 1) {
			pointer += 2048;
		} else if (selector == 2) {
			pointer += 526336;
		}
		Value pointed;
		if (!Decode(section_start, section_start + static_cast<std::size_t>(pointer), &pointed, nullptr,
					depth + 1)) {
			return false;
		}
		*out = std::move(pointed);
		if (next != nullptr) {
			*next = pos;
		}
		return true;
	}

	if (type == 0) { // extended type: the next byte + 7
		if (pos + 1 > size_) {
			return false;
		}
		type = static_cast<unsigned>(data_[pos++]) + 7;
	}

	uint64_t size = control & 0x1f;
	switch (size) {
	case 29:
		if (pos + 1 > size_) {
			return false;
		}
		size = 29 + data_[pos];
		pos += 1;
		break;
	case 30:
		if (pos + 2 > size_) {
			return false;
		}
		size = 285 + ReadBig(data_ + pos, 2);
		pos += 2;
		break;
	case 31:
		if (pos + 3 > size_) {
			return false;
		}
		size = 65821 + ReadBig(data_ + pos, 3);
		pos += 3;
		break;
	default:
		break;
	}

	switch (type) {
	case 2: // UTF-8 string
		if (size > size_ || pos + size > size_) {
			return false;
		}
		out->kind = Value::Kind::String;
		out->text.assign(reinterpret_cast<const char*>(data_ + pos), static_cast<std::size_t>(size));
		pos += size;
		break;
	case 3: { // double (a zero double is written without payload bytes)
		if (size == 0) {
			out->kind = Value::Kind::Double;
			out->dbl = 0.0;
			break;
		}
		if (size != 8 || pos + 8 > size_) {
			return false;
		}
		const uint64_t bits = (static_cast<uint64_t>(ReadBig(data_ + pos, 4)) << 32) | ReadBig(data_ + pos + 4, 4);
		double value = 0.0;
		std::memcpy(&value, &bits, sizeof(value));
		out->kind = Value::Kind::Double;
		out->dbl = value;
		pos += 8;
		break;
	}
	case 4: // bytes
		if (size > size_ || pos + size > size_) {
			return false;
		}
		out->kind = Value::Kind::Bytes;
		out->text.assign(reinterpret_cast<const char*>(data_ + pos), static_cast<std::size_t>(size));
		pos += size;
		break;
	case 5: // uint16 (the size field is the byte count, 0..2)
		if (size > 2 || pos + size > size_) {
			return false;
		}
		out->kind = Value::Kind::Uint;
		out->uint = ReadBig(data_ + pos, static_cast<std::size_t>(size));
		pos += size;
		break;
	case 6: // uint32 (0..4 bytes)
		if (size > 4 || pos + size > size_) {
			return false;
		}
		out->kind = Value::Kind::Uint;
		out->uint = ReadBig(data_ + pos, static_cast<std::size_t>(size));
		pos += size;
		break;
	case 7: // map
		if (size > kMaxContainer) {
			return false;
		}
		out->kind = Value::Kind::Map;
		out->map.reserve(static_cast<std::size_t>(size));
		for (uint64_t i = 0; i < size; ++i) {
			Value key;
			Value value;
			if (!Decode(section_start, pos, &key, &pos, depth + 1) ||
				!Decode(section_start, pos, &value, &pos, depth + 1)) {
				return false;
			}
			out->map.emplace_back(key.kind == Value::Kind::String ? key.text : std::string(), std::move(value));
		}
		break;
	case 8: { // int32, sign-extended from `size` bytes (0..4)
		if (size > 4 || pos + size > size_) {
			return false;
		}
		const uint32_t raw = ReadBig(data_ + pos, static_cast<std::size_t>(size));
		const int shift = 32 - static_cast<int>(size) * 8;
		out->kind = Value::Kind::Int;
		out->sint = static_cast<int32_t>(raw << shift) >> shift;
		pos += size;
		break;
	}
	case 9: // uint64 (0..8 bytes)
		if (size > 8 || pos + size > size_) {
			return false;
		}
		out->kind = Value::Kind::Uint;
		out->uint = ReadBig(data_ + pos, static_cast<std::size_t>(size));
		pos += size;
		break;
	case 10: // uint128: not read by this module; kept as its raw bytes
		if (size > size_ || pos + size > size_) {
			return false;
		}
		out->kind = Value::Kind::Bytes;
		out->text.assign(reinterpret_cast<const char*>(data_ + pos), static_cast<std::size_t>(size));
		pos += size;
		break;
	case 11: // array
		if (size > kMaxContainer) {
			return false;
		}
		out->kind = Value::Kind::Array;
		out->array.reserve(static_cast<std::size_t>(size));
		for (uint64_t i = 0; i < size; ++i) {
			Value value;
			if (!Decode(section_start, pos, &value, &pos, depth + 1)) {
				return false;
			}
			out->array.push_back(std::move(value));
		}
		break;
	case 12: // container (deprecated in the spec, never written by GeoLite2)
		return false;
	case 13: // end marker
		out->kind = Value::Kind::None;
		break;
	case 14: // boolean
		out->kind = Value::Kind::Bool;
		out->boolean = size != 0;
		break;
	case 15: { // float (a zero float is written without payload bytes)
		if (size == 0) {
			out->kind = Value::Kind::Double;
			out->dbl = 0.0;
			break;
		}
		if (size != 4 || pos + 4 > size_) {
			return false;
		}
		const uint32_t bits = ReadBig(data_ + pos, 4);
		float value = 0.0f;
		std::memcpy(&value, &bits, sizeof(value));
		out->kind = Value::Kind::Double;
		out->dbl = value;
		pos += 4;
		break;
	}
	default:
		return false;
	}

	if (next != nullptr) {
		*next = pos;
	}
	return true;
}

bool Mmdb::ReadRecord(uint32_t node, int right, uint32_t* out) const {
	if (data_ == nullptr || node >= node_count_) {
		return false;
	}
	switch (record_size_) {
	case 24: {
		const std::size_t base = static_cast<std::size_t>(node) * 6 + (right != 0 ? 3 : 0);
		if (base + 3 > node_bytes_) {
			return false;
		}
		*out = ReadBig(data_ + base, 3);
		return true;
	}
	case 28: {
		// 7-byte nodes: the left record is the first 3 bytes with the high nibble of byte 3 as its
		// high 4 bits, the right one is the low nibble of byte 3 with bytes 4..6.
		const std::size_t base = static_cast<std::size_t>(node) * 7;
		if (base + 7 > node_bytes_) {
			return false;
		}
		const uint8_t* bytes = data_ + base;
		const uint32_t left = (static_cast<uint32_t>(bytes[3] >> 4) << 24) |
							  (static_cast<uint32_t>(bytes[0]) << 16) |
							  (static_cast<uint32_t>(bytes[1]) << 8) | bytes[2];
		const uint32_t right_value = (static_cast<uint32_t>(bytes[3] & 0x0f) << 24) |
									 (static_cast<uint32_t>(bytes[4]) << 16) |
									 (static_cast<uint32_t>(bytes[5]) << 8) | bytes[6];
		*out = right != 0 ? right_value : left;
		return true;
	}
	case 32: {
		const std::size_t base = static_cast<std::size_t>(node) * 8 + (right != 0 ? 4 : 0);
		if (base + 4 > node_bytes_) {
			return false;
		}
		*out = ReadBig(data_ + base, 4);
		return true;
	}
	default:
		return false;
	}
}

bool Mmdb::Walk(const uint8_t* prefix, std::size_t bits, uint32_t* record) const {
	uint32_t node = 0;
	for (std::size_t i = 0; i < bits; ++i) {
		const int bit = (prefix[i / 8] >> (7 - (i % 8))) & 1;
		uint32_t next = 0;
		if (!ReadRecord(node, bit, &next)) {
			return false;
		}
		if (next == node_count_) {
			return false; // the search ended without data (result.found_entry == false)
		}
		if (next > node_count_) {
			*record = next;
			return true;
		}
		node = next;
	}
	return false; // the bit string was exhausted inside the tree: no data
}

bool Mmdb::DecodeRecord(uint32_t record, Location* out) const {
	// Data-section addresses (both the tree's record values and the pointers inside the data
	// values) are relative to the first byte after the 16-byte separator.
	const std::size_t offset = static_cast<std::size_t>(record) - node_count_ - kSeparator;
	Value value;
	if (!DecodeValue(data_start_, data_start_ + offset, &value)) {
		return false;
	}
	out->found = true;

	const auto names_en = [](const Value* node) -> std::string {
		if (node == nullptr || node->kind != Value::Kind::Map) {
			return {};
		}
		const Value* names = node->Find("names");
		if (names == nullptr || names->kind != Value::Kind::Map) {
			return {};
		}
		const Value* english = names->Find("en");
		return english != nullptr && english->kind == Value::Kind::String ? english->text : std::string();
	};

	const Value* country = value.Find("country");
	out->country = Truncate(names_en(country), 44); // the original's char[45] Format target
	if (country != nullptr) {
		const Value* iso = country->Find("iso_code");
		if (iso != nullptr && iso->kind == Value::Kind::String) {
			out->country_code = Truncate(iso->text, 2); // the original's char[3] ("%.*s" cut)
		}
	}
	out->city = Truncate(names_en(value.Find("city")), 44);
	if (const Value* subdivisions = value.Find("subdivisions");
		subdivisions != nullptr && subdivisions->kind == Value::Kind::Array) {
		out->region = Truncate(names_en(subdivisions->At(0)), 44);
	}
	return true;
}

bool Mmdb::LookupBytes(const uint8_t* address, std::size_t size, bool ipv4_mapped, Location* out) const {
	if (size == 4 && ip_version_ == 6) {
		// IPv4 in an IPv6 database: the IPv4 subtree hangs under a 96-bit prefix; the shipped
		// databases answer both the IPv4-mapped (::ffff:0:0/96) and the zero (::/96) spelling.
		uint8_t prefix[16] = {};
		if (ipv4_mapped) {
			prefix[10] = 0xff;
			prefix[11] = 0xff;
		}
		std::memcpy(prefix + 12, address, 4);
		uint32_t record = 0;
		if (!Walk(prefix, 128, &record)) {
			return true; // not found (or an unreadable tree): the caller sees found == false
		}
		return DecodeRecord(record, out);
	}
	if (size == 16 && ip_version_ == 4) {
		return true; // an IPv6 address cannot be in an IPv4 database
	}
	uint32_t record = 0;
	if (!Walk(address, size * 8, &record)) {
		return true;
	}
	return DecodeRecord(record, out);
}

bool Mmdb::Lookup(std::string_view address, Location* out) const {
	if (out == nullptr || !IsValid()) {
		return false;
	}
	*out = Location{};
	const std::string text(address);
	uint8_t bytes[16] = {};
	if (::inet_pton(AF_INET, text.c_str(), bytes) == 1) {
		if (LookupBytes(bytes, 4, /*ipv4_mapped=*/true, out) && out->found) {
			return true;
		}
		if (ip_version_ == 6) {
			// The other 96-bit spelling (a database built without the IPv4-mapped alias).
			*out = Location{};
			if (!LookupBytes(bytes, 4, /*ipv4_mapped=*/false, out)) {
				return false;
			}
		}
		return true;
	}
	if (::inet_pton(AF_INET6, text.c_str(), bytes) == 1) {
		return LookupBytes(bytes, 16, /*ipv4_mapped=*/true, out);
	}
	return false; // unparseable address (the original's gai_error path)
}

} // namespace lr_geoip
