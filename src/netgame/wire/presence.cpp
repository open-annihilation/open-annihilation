// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/presence.hpp"

#include "oa/netgame/records.hpp"

#include <cassert>
#include <cstring>
#include <string_view>

namespace oa::netgame {
namespace {

/// Bit 0 of the developer flags: Developer Mode is on. Every other bit is 0.
constexpr uint8_t developer_mode_flag = 0x01;
/// Bit 0 of the content flags: the machine can fetch content. Bits 1 to 7
/// are sent as 0 and ignored when read.
constexpr uint8_t content_fetches_flag = 0x01;
/// The content field's value: one byte of flags.
constexpr std::size_t content_value_bytes = 1;
/// Bit 0 of the bundles flags: a chosen bundle differs from a known hash.
/// Bits 1 to 7 are sent as 0 and ignored when read.
constexpr uint8_t bundles_differ_flag = 0x01;
/// The longest language name the language field carries.
constexpr std::size_t language_text_max = 32;

constexpr std::size_t digest_bytes = 32;
constexpr std::size_t version_text_max = 32;
constexpr std::size_t platform_text_max = 16;
constexpr std::size_t architecture_text_max = 16;
constexpr std::size_t mod_id_max = 64;
constexpr std::size_t mod_name_max = 64;
constexpr std::size_t mod_version_max = 32;
constexpr std::size_t registry_id_max = 32;
constexpr std::size_t descriptor_url_max = 255;
constexpr std::size_t hack_id_max = 64;
constexpr std::size_t pack_key_max = 64;
constexpr std::size_t pack_name_max = 64;
constexpr std::size_t pack_version_max = 32;
constexpr std::size_t mutator_id_max = 64;
constexpr std::size_t mutator_version_max = 32;
constexpr std::size_t mutator_option_key_max = 32;
constexpr std::size_t mutator_option_value_max = 32;
/// Bit 0 of the mutators flags: the layer was built. Bit 1: a package is
/// lacking. Bit 2: a mutator failed. Bits 3 to 7 are sent as 0 and ignored.
constexpr uint8_t mutators_built_flag = 0x01;
constexpr uint8_t mutators_lacking_flag = 0x02;
constexpr uint8_t mutators_failed_flag = 0x04;
/// Flags, loadout digest, layer digest, index, total, held and the state word.
constexpr std::size_t mutators_fixed_value_bytes = 1 + digest_bytes + digest_bytes + 1 + 1 + 1 + 8;
/// The mutator-modes field is one little-endian mask.
constexpr std::size_t mutator_modes_value_bytes = 4;
static_assert(mutators_fixed_value_bytes == 76);

enum class ByteKind { ascii, kebab, id, text };

/// Tells whether a code unit is an ascii byte of the record's strings.
bool ascii_byte(unsigned char byte) noexcept {
    return byte >= 0x21 && byte <= 0x7e;
}

/// Tells whether a string is kebab, or an id when dots are allowed.
bool kebab_text(std::string_view text, bool dots) noexcept {
    if (text.empty() || text.front() == '-' || text.back() == '-')
        return false;
    for (const char byte : text) {
        const auto unit = static_cast<unsigned char>(byte);
        if (unit >= 'a' && unit <= 'z')
            continue;
        if (unit >= '0' && unit <= '9')
            continue;
        if (byte == '-' || (dots && byte == '.'))
            continue;
        return false;
    }
    return true;
}

/// Tells whether text is well-formed UTF-8 with no control character.
bool text_bytes(const uint8_t* bytes, std::size_t size) noexcept {
    std::size_t at = 0;
    while (at < size) {
        const auto lead = bytes[at];
        std::size_t length = 0;
        uint32_t value = 0;
        uint32_t least = 0;
        if (lead < 0x80) {
            if (lead < 0x20 || lead == 0x7f)
                return false;
            ++at;
            continue;
        }
        if ((lead & 0xe0) == 0xc0) {
            length = 2;
            value = lead & 0x1fu;
            least = 0x80;
        } else if ((lead & 0xf0) == 0xe0) {
            length = 3;
            value = lead & 0x0fu;
            least = 0x800;
        } else if ((lead & 0xf8) == 0xf0) {
            length = 4;
            value = lead & 0x07u;
            least = 0x10000;
        } else {
            return false;
        }
        if (size - at < length)
            return false;
        for (std::size_t next = 1; next < length; ++next) {
            const auto byte = bytes[at + next];
            if ((byte & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (byte & 0x3fu);
        }
        constexpr uint32_t first_surrogate = 0xd800;
        constexpr uint32_t last_surrogate = 0xdfff;
        constexpr uint32_t last_code_point = 0x10ffff;
        if (value < least || value > last_code_point ||
            (value >= first_surrogate && value <= last_surrogate))
            return false;
        if (value < 0x20 || value == 0x7f || (value >= 0x80 && value <= 0x9f))
            return false;
        at += length;
    }
    return true;
}

/// Tells whether a string is one of the record's four kinds, of a length in range.
bool string_in_rule(
    std::string_view text, std::size_t least, std::size_t most, ByteKind kind
) noexcept {
    if (text.size() < least || text.size() > most)
        return false;
    switch (kind) {
    case ByteKind::ascii:
        for (const char byte : text)
            if (!ascii_byte(static_cast<unsigned char>(byte)))
                return false;
        return true;
    case ByteKind::kebab:
        return text.empty() ? least == 0 : kebab_text(text, false);
    case ByteKind::id:
        return kebab_text(text, true);
    case ByteKind::text:
        return text_bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }
    return false;
}

/// Reads a little-endian u64. The caller has eight bytes.
uint64_t load_le64(const uint8_t* bytes) noexcept {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8)
        value |= static_cast<uint64_t>(bytes[shift / 8]) << shift;
    return value;
}

/// A window over bytes that never reads past its end.
struct Window {
    const uint8_t* data{};
    std::size_t size{};

    /// Takes n bytes, or refuses when they are not there.
    bool take(std::size_t n, const uint8_t** out) noexcept {
        if (n > size)
            return false;
        *out = data;
        data += n;
        size -= n;
        return true;
    }

    bool u8(uint8_t* out) noexcept {
        const uint8_t* bytes = nullptr;
        if (!take(1, &bytes))
            return false;
        *out = bytes[0];
        return true;
    }

    bool u16(uint16_t* out) noexcept {
        const uint8_t* bytes = nullptr;
        if (!take(2, &bytes))
            return false;
        *out = load_u16(bytes);
        return true;
    }

    bool u32(uint32_t* out) noexcept {
        const uint8_t* bytes = nullptr;
        if (!take(4, &bytes))
            return false;
        *out = load_u32(bytes);
        return true;
    }

    bool u64(uint64_t* out) noexcept {
        const uint8_t* bytes = nullptr;
        if (!take(8, &bytes))
            return false;
        *out = load_le64(bytes);
        return true;
    }
};

/// Reads one length-prefixed string of the given rule.
bool read_string(
    Window* window, std::size_t least, std::size_t most, ByteKind kind, std::string* out
) {
    uint8_t length = 0;
    if (!window->u8(&length) || length < least || length > most)
        return false;
    const uint8_t* bytes = nullptr;
    if (!window->take(length, &bytes))
        return false;
    const std::string_view text(reinterpret_cast<const char*>(bytes), length);
    if (!string_in_rule(text, least, most, kind))
        return false;
    *out = std::string(text);
    return true;
}

/// Appends raw bytes. The caller has already kept the record within its limit.
void put_bytes(uint8_t* out, std::size_t* used, const void* bytes, std::size_t size) noexcept {
    if (size != 0)
        std::memcpy(out + *used, bytes, size);
    *used += size;
}

void put_u8(uint8_t* out, std::size_t* used, uint8_t value) noexcept {
    out[(*used)++] = value;
}

void put_u16(uint8_t* out, std::size_t* used, uint16_t value) noexcept {
    store_u16(out + *used, value);
    *used += 2;
}

void put_u32(uint8_t* out, std::size_t* used, uint32_t value) noexcept {
    store_u32(out + *used, value);
    *used += 4;
}

void put_u64(uint8_t* out, std::size_t* used, uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8)
        put_u8(out, used, static_cast<uint8_t>(value >> shift));
}

void put_string(uint8_t* out, std::size_t* used, std::string_view text) noexcept {
    put_u8(out, used, static_cast<uint8_t>(text.size()));
    put_bytes(out, used, text.data(), text.size());
}

/// Bytes of one length-prefixed string.
std::size_t string_bytes(std::string_view text) noexcept {
    return 1 + text.size();
}

/// Tells whether the record's frame is whole: length, version and fields that fill it.
WireError framed_record(const uint8_t* record, std::size_t size) noexcept {
    uint16_t length = 0;
    const auto error = presence_record_length(record, size, &length);
    if (error != WireError::ok)
        return error;
    if (static_cast<std::size_t>(length) != size)
        return WireError::length_mismatch;
    if (size < presence_record_header_bytes)
        return WireError::truncated;
    assert(size <= presence_record_max_bytes);
    if (record[3] == 0)
        return WireError::bad_argument;
    std::size_t at = presence_record_header_bytes;
    while (at < size) {
        assert(at < size);
        if (size - at < presence_field_header_bytes)
            return WireError::truncated;
        const auto value_size = static_cast<std::size_t>(load_u16(record + at + 1));
        if (value_size > size - at - presence_field_header_bytes)
            return WireError::truncated;
        at += presence_field_header_bytes + value_size;
    }
    assert(at == size);
    return WireError::ok;
}

bool engine_ok(const PresenceEngine& engine) noexcept {
    return string_in_rule(engine.version, 1, version_text_max, ByteKind::ascii) &&
           string_in_rule(engine.platform, 0, platform_text_max, ByteKind::ascii) &&
           string_in_rule(engine.architecture, 0, architecture_text_max, ByteKind::ascii);
}

std::size_t engine_value_bytes(const PresenceEngine& engine) noexcept {
    return string_bytes(engine.version) + string_bytes(engine.platform) +
           string_bytes(engine.architecture);
}

bool mod_ok(const PresenceMod& mod) noexcept {
    return string_in_rule(mod.id, 1, mod_id_max, ByteKind::kebab) &&
           string_in_rule(mod.name, 1, mod_name_max, ByteKind::text) &&
           string_in_rule(mod.version, 0, mod_version_max, ByteKind::text);
}

std::size_t mod_value_bytes(const PresenceMod& mod) noexcept {
    return string_bytes(mod.id) + string_bytes(mod.name) + string_bytes(mod.version) + 2;
}

bool origin_ok(const PresenceOrigin& origin) noexcept {
    return origin.release >= 1 &&
           string_in_rule(origin.registry, 1, registry_id_max, ByteKind::kebab) &&
           string_in_rule(origin.descriptor_url, 0, descriptor_url_max, ByteKind::ascii);
}

std::size_t origin_value_bytes(const PresenceOrigin& origin) noexcept {
    return string_bytes(origin.registry) + string_bytes(origin.descriptor_url) + 4 + digest_bytes;
}

bool hack_id_ok(std::string_view id) noexcept {
    return string_in_rule(id, 1, hack_id_max, ByteKind::id);
}

bool hacks_ok(const PresenceHacks& hacks) noexcept {
    if (hacks.ids.size() > hacks.total)
        return false;
    for (const auto& id : hacks.ids)
        if (!hack_id_ok(id))
            return false;
    return true;
}

bool developer_ok(const PresenceDeveloper& developer) noexcept {
    if (developer.overrides.size() > developer.total)
        return false;
    for (const auto& over : developer.overrides)
        if (!hack_id_ok(over.hack))
            return false;
    return true;
}

bool map_pack_ok(const PresenceMapPack& pack) noexcept {
    return string_in_rule(pack.key, 1, pack_key_max, ByteKind::kebab) &&
           string_in_rule(pack.registry, 1, registry_id_max, ByteKind::kebab) &&
           string_in_rule(pack.descriptor_url, 0, descriptor_url_max, ByteKind::ascii) &&
           string_in_rule(pack.name, 0, pack_name_max, ByteKind::text) &&
           string_in_rule(pack.version, 0, pack_version_max, ByteKind::text);
}

std::size_t map_pack_value_bytes(const PresenceMapPack& pack) noexcept {
    return string_bytes(pack.key) + 4 + digest_bytes + string_bytes(pack.registry) +
           string_bytes(pack.descriptor_url) + string_bytes(pack.name) + string_bytes(pack.version);
}

void write_engine(uint8_t* out, std::size_t* used, const PresenceEngine& engine) noexcept {
    put_u8(out, used, static_cast<uint8_t>(PresenceTag::engine));
    put_u16(out, used, static_cast<uint16_t>(engine_value_bytes(engine)));
    put_string(out, used, engine.version);
    put_string(out, used, engine.platform);
    put_string(out, used, engine.architecture);
}

/// Writes the content field: its header and its one byte of flags.
void write_content(uint8_t* out, std::size_t* used, const PresenceContent& content) noexcept {
    put_u8(out, used, static_cast<uint8_t>(PresenceTag::content));
    put_u16(out, used, static_cast<uint16_t>(content_value_bytes));
    put_u8(out, used, content.fetches ? content_fetches_flag : 0);
}

bool bundle_id_ok(std::string_view id) noexcept {
    return string_in_rule(id, 1, presence_bundle_id_max, ByteKind::kebab);
}

bool bundles_ok(const PresenceBundles& bundles) noexcept {
    if (!bundle_id_ok(bundles.base_game) || bundles.expansions.size() > presence_bundle_count_max)
        return false;
    for (const auto& id : bundles.expansions)
        if (!bundle_id_ok(id))
            return false;
    return true;
}

std::size_t bundles_value_bytes(const PresenceBundles& bundles) noexcept {
    std::size_t size = 1 + string_bytes(bundles.base_game) + 1;
    for (const auto& id : bundles.expansions)
        size += string_bytes(id);
    return size;
}

bool language_ok(std::string_view language) noexcept {
    return string_in_rule(language, 1, language_text_max, ByteKind::text);
}

void write_bundles(uint8_t* out, std::size_t* used, const PresenceBundles& bundles) noexcept {
    put_u8(out, used, static_cast<uint8_t>(PresenceTag::bundles));
    put_u16(out, used, static_cast<uint16_t>(bundles_value_bytes(bundles)));
    put_u8(out, used, bundles.differs_from_known ? bundles_differ_flag : 0);
    put_string(out, used, bundles.base_game);
    put_u8(out, used, static_cast<uint8_t>(bundles.expansions.size()));
    for (const auto& id : bundles.expansions)
        put_string(out, used, id);
}

void write_language(uint8_t* out, std::size_t* used, std::string_view language) noexcept {
    put_u8(out, used, static_cast<uint8_t>(PresenceTag::language));
    put_u16(out, used, static_cast<uint16_t>(string_bytes(language)));
    put_string(out, used, language);
}

bool read_bundles(Window window, PresenceBundles* out) {
    PresenceBundles bundles;
    uint8_t flags = 0;
    uint8_t count = 0;
    if (!window.u8(&flags) ||
        !read_string(&window, 1, presence_bundle_id_max, ByteKind::kebab, &bundles.base_game) ||
        !window.u8(&count) || count > presence_bundle_count_max)
        return false;
    bundles.differs_from_known = (flags & bundles_differ_flag) != 0;
    bundles.expansions.reserve(count);
    for (uint8_t index = 0; index < count; ++index) {
        std::string id;
        if (!read_string(&window, 1, presence_bundle_id_max, ByteKind::kebab, &id))
            return false;
        bundles.expansions.push_back(std::move(id));
    }
    if (window.size != 0)
        return false;
    *out = std::move(bundles);
    return true;
}

bool read_language(Window window, std::string* out) {
    std::string language;
    if (!read_string(&window, 1, language_text_max, ByteKind::text, &language) || window.size != 0)
        return false;
    *out = std::move(language);
    return true;
}

/// One bundles field, parsed without allocating. Music and duplicates are left out.
struct BundleReading {
    std::string_view base{};
    std::string_view ids[presence_bundle_count_max]{};
    uint8_t count{};
    bool differs{};
    bool present{};
};

bool take_bundle_id(const uint8_t*& cursor, const uint8_t* end, std::string_view* id) noexcept {
    if (cursor >= end)
        return false;
    const auto length = static_cast<std::size_t>(*cursor);
    ++cursor;
    if (length < 1 || length > presence_bundle_id_max ||
        static_cast<std::size_t>(end - cursor) < length)
        return false;
    const std::string_view text(reinterpret_cast<const char*>(cursor), length);
    cursor += length;
    if (!kebab_text(text, false))
        return false;
    *id = text;
    return true;
}

bool read_bundle_value(std::span<const uint8_t> value, BundleReading* reading) noexcept {
    if (value.empty())
        return false;
    const uint8_t* cursor = value.data();
    const uint8_t* end = cursor + value.size();
    const uint8_t flags = *cursor;
    ++cursor;
    std::string_view base;
    if (!take_bundle_id(cursor, end, &base) || cursor >= end)
        return false;
    const auto count = *cursor;
    ++cursor;
    if (count > presence_bundle_count_max)
        return false;
    BundleReading parsed;
    parsed.present = true;
    parsed.differs = (flags & bundles_differ_flag) != 0;
    parsed.base = base;
    for (uint8_t index = 0; index < count; ++index) {
        std::string_view id;
        if (!take_bundle_id(cursor, end, &id))
            return false;
        if (id == presence_uncompared_expansion)
            continue;
        bool seen = false;
        for (uint8_t have = 0; have < parsed.count; ++have)
            if (parsed.ids[have] == id)
                seen = true;
        if (seen)
            continue;
        parsed.ids[parsed.count] = id;
        ++parsed.count;
    }
    if (cursor != end)
        return false;
    *reading = parsed;
    return true;
}

bool bundles_of(std::span<const uint8_t> record, BundleReading* reading) noexcept {
    *reading = {};
    if (record.empty())
        return false;
    std::span<const uint8_t> value;
    if (!find_presence_field(record.data(), record.size(), PresenceTag::bundles, &value))
        return false;
    return read_bundle_value(value, reading);
}

/// The rule-changing expansions of a bundles statement, music and duplicates left out.
bool rule_changing_ids(
    const PresenceBundles& bundles, std::string_view* ids, uint8_t* count
) noexcept {
    *count = 0;
    if (!bundle_id_ok(bundles.base_game))
        return false;
    for (const auto& id : bundles.expansions) {
        if (id == presence_uncompared_expansion)
            continue;
        if (!bundle_id_ok(id))
            return false;
        bool seen = false;
        for (uint8_t have = 0; have < *count; ++have)
            if (ids[have] == id)
                seen = true;
        if (seen)
            continue;
        if (*count == presence_bundle_count_max)
            return false;
        ids[*count] = id;
        ++*count;
    }
    for (uint8_t index = 1; index < *count; ++index) {
        const std::string_view key = ids[index];
        uint8_t place = index;
        while (place > 0 && ids[place - 1] > key) {
            ids[place] = ids[place - 1];
            --place;
        }
        ids[place] = key;
    }
    return true;
}

void write_mod(uint8_t* out, std::size_t* used, const PresenceMod& mod) noexcept {
    put_u8(out, used, static_cast<uint8_t>(PresenceTag::mod));
    put_u16(out, used, static_cast<uint16_t>(mod_value_bytes(mod)));
    put_string(out, used, mod.id);
    put_string(out, used, mod.name);
    put_string(out, used, mod.version);
    put_u16(out, used, mod.packaging_revision);
}

void write_origin(uint8_t* out, std::size_t* used, const PresenceOrigin& origin) noexcept {
    put_u8(out, used, static_cast<uint8_t>(PresenceTag::mod_origin));
    put_u16(out, used, static_cast<uint16_t>(origin_value_bytes(origin)));
    put_string(out, used, origin.registry);
    put_string(out, used, origin.descriptor_url);
    put_u32(out, used, origin.release);
    put_bytes(out, used, origin.sha256.data(), digest_bytes);
}

void write_sim(uint8_t* out, std::size_t* used, const PresenceDigest& hash) noexcept {
    put_u8(out, used, static_cast<uint8_t>(PresenceTag::sim_hash));
    put_u16(out, used, static_cast<uint16_t>(digest_bytes));
    put_bytes(out, used, hash.data(), digest_bytes);
}

void write_map_pack_value(uint8_t* out, std::size_t* used, const PresenceMapPack& pack) noexcept {
    put_string(out, used, pack.key);
    put_u32(out, used, pack.release);
    put_bytes(out, used, pack.sha256.data(), digest_bytes);
    put_string(out, used, pack.registry);
    put_string(out, used, pack.descriptor_url);
    put_string(out, used, pack.name);
    put_string(out, used, pack.version);
}

void write_map_pack(uint8_t* out, std::size_t* used, const PresenceMapPack& pack) noexcept {
    put_u8(out, used, static_cast<uint8_t>(PresenceTag::map_pack));
    put_u16(out, used, static_cast<uint16_t>(map_pack_value_bytes(pack)));
    write_map_pack_value(out, used, pack);
}

bool read_engine(Window window, PresenceEngine* out) {
    PresenceEngine engine;
    if (!read_string(&window, 1, version_text_max, ByteKind::ascii, &engine.version) ||
        !read_string(&window, 0, platform_text_max, ByteKind::ascii, &engine.platform) ||
        !read_string(&window, 0, architecture_text_max, ByteKind::ascii, &engine.architecture) ||
        window.size != 0)
        return false;
    *out = std::move(engine);
    return true;
}

bool read_mod(Window window, PresenceMod* out) {
    PresenceMod mod;
    if (!read_string(&window, 1, mod_id_max, ByteKind::kebab, &mod.id) ||
        !read_string(&window, 1, mod_name_max, ByteKind::text, &mod.name) ||
        !read_string(&window, 0, mod_version_max, ByteKind::text, &mod.version) ||
        !window.u16(&mod.packaging_revision) || window.size != 0)
        return false;
    *out = std::move(mod);
    return true;
}

bool read_origin(Window window, PresenceOrigin* out) {
    PresenceOrigin origin;
    const uint8_t* digest = nullptr;
    if (!read_string(&window, 1, registry_id_max, ByteKind::kebab, &origin.registry) ||
        !read_string(&window, 0, descriptor_url_max, ByteKind::ascii, &origin.descriptor_url) ||
        !window.u32(&origin.release) || origin.release < 1 || !window.take(digest_bytes, &digest) ||
        window.size != 0)
        return false;
    std::memcpy(origin.sha256.data(), digest, digest_bytes);
    *out = std::move(origin);
    return true;
}

bool read_hacks(Window window, PresenceHacks* out) {
    PresenceHacks hacks;
    if (!window.u16(&hacks.total))
        return false;
    while (window.size != 0) {
        std::string id;
        if (!read_string(&window, 1, hack_id_max, ByteKind::id, &id))
            return false;
        hacks.ids.push_back(std::move(id));
    }
    if (hacks.ids.size() > hacks.total)
        return false;
    *out = std::move(hacks);
    return true;
}

bool read_developer(Window window, PresenceDeveloper* out) {
    PresenceDeveloper developer;
    uint8_t flags = 0;
    if (!window.u8(&flags) || (flags != 0 && flags != developer_mode_flag) ||
        !window.u16(&developer.total))
        return false;
    developer.on = flags == developer_mode_flag;
    while (window.size != 0) {
        PresenceOverride over;
        uint8_t on = 0;
        if (!window.u8(&on) || (on != 0 && on != 1) ||
            !read_string(&window, 1, hack_id_max, ByteKind::id, &over.hack))
            return false;
        over.on = on == 1;
        developer.overrides.push_back(std::move(over));
    }
    if (developer.overrides.size() > developer.total)
        return false;
    *out = std::move(developer);
    return true;
}

bool read_map_pack(Window window, PresenceMapPack* out) {
    PresenceMapPack pack;
    const uint8_t* digest = nullptr;
    if (!read_string(&window, 1, pack_key_max, ByteKind::kebab, &pack.key) ||
        !window.u32(&pack.release) || !window.take(digest_bytes, &digest) ||
        !read_string(&window, 1, registry_id_max, ByteKind::kebab, &pack.registry) ||
        !read_string(&window, 0, descriptor_url_max, ByteKind::ascii, &pack.descriptor_url) ||
        !read_string(&window, 0, pack_name_max, ByteKind::text, &pack.name) ||
        !read_string(&window, 0, pack_version_max, ByteKind::text, &pack.version) ||
        window.size != 0)
        return false;
    std::memcpy(pack.sha256.data(), digest, digest_bytes);
    *out = std::move(pack);
    return true;
}

/// Tells whether every state bit at or past total is clear.
bool states_in_rule(uint64_t states, uint8_t total) noexcept {
    if (total >= presence_mutators_max)
        return true;
    const auto shift = static_cast<unsigned>(2u * total);
    const uint64_t used = (uint64_t{1} << shift) - 1;
    return (states & ~used) == 0;
}

/// Tells whether the fixed part of a mutators field is inside its rule.
bool mutators_prefix_ok(
    bool built,
    const PresenceDigest& layer,
    uint8_t index,
    uint8_t total,
    uint8_t held,
    uint64_t states
) noexcept {
    if (total < 1 || total > presence_mutators_max || held > total)
        return false;
    if (index != presence_mutator_index_none && index >= total)
        return false;
    if (!built) {
        for (const uint8_t byte : layer)
            if (byte != 0)
                return false;
    }
    return states_in_rule(states, total);
}

/// Tells whether every bit of a clash mask names a mutator in the list.
bool clash_mask_fits(uint32_t mask, uint8_t total) noexcept {
    if (total < 1 || total > presence_mutators_max)
        return false;
    if (total == presence_mutators_max)
        return true;
    const uint32_t used = (uint32_t{1} << total) - 1u;
    return (mask & ~used) == 0;
}

bool option_ok(const PresenceMutatorOption& option) noexcept {
    return string_in_rule(option.key, 1, mutator_option_key_max, ByteKind::ascii) &&
           string_in_rule(option.value, 0, mutator_option_value_max, ByteKind::text);
}

bool mutator_ok(const PresenceMutator& item) noexcept {
    if (!string_in_rule(item.id, 1, mutator_id_max, ByteKind::kebab) ||
        !string_in_rule(item.version, 0, mutator_version_max, ByteKind::text) ||
        item.options.size() > presence_mutator_options_max)
        return false;
    for (const auto& option : item.options)
        if (!option_ok(option))
            return false;
    return true;
}

bool mutators_ok(const PresenceMutators& field) noexcept {
    if (!mutators_prefix_ok(
            field.built, field.layer_digest, field.index, field.total, field.held, field.states
        ) ||
        field.mutators.size() > field.total)
        return false;
    for (const auto& item : field.mutators)
        if (!mutator_ok(item))
            return false;
    return true;
}

bool part_ok(const PresenceMutatorPart& part) noexcept {
    if (part.first >= presence_mutators_max || part.mutators.empty() ||
        part.mutators.size() > presence_mutators_max - part.first)
        return false;
    for (const auto& item : part.mutators)
        if (!mutator_ok(item))
            return false;
    return true;
}

std::size_t option_value_bytes(const PresenceMutatorOption& option) noexcept {
    return string_bytes(option.key) + string_bytes(option.value);
}

std::size_t mutator_value_bytes(const PresenceMutator& item) noexcept {
    std::size_t size =
        string_bytes(item.id) + string_bytes(item.version) + presence_mutator_prefix_bytes + 1;
    for (const auto& option : item.options)
        size += option_value_bytes(option);
    return size;
}

std::size_t mutator_part_value_bytes(const PresenceMutatorPart& part) noexcept {
    std::size_t size = digest_bytes + 1;
    for (const auto& item : part.mutators)
        size += mutator_value_bytes(item);
    return size;
}

void write_mutator(uint8_t* out, std::size_t* used, const PresenceMutator& item) noexcept {
    put_string(out, used, item.id);
    put_string(out, used, item.version);
    put_bytes(out, used, item.sha256_prefix.data(), presence_mutator_prefix_bytes);
    put_u8(out, used, static_cast<uint8_t>(item.options.size()));
    for (const auto& option : item.options) {
        put_string(out, used, option.key);
        put_string(out, used, option.value);
    }
}

void write_mutators_prefix(
    uint8_t* out, std::size_t* used, const PresenceMutators& field
) noexcept {
    unsigned flags = 0;
    if (field.built)
        flags |= mutators_built_flag;
    if (field.lacking)
        flags |= mutators_lacking_flag;
    if (field.failed)
        flags |= mutators_failed_flag;
    put_u8(out, used, static_cast<uint8_t>(flags));
    put_bytes(out, used, field.digest.data(), digest_bytes);
    put_bytes(out, used, field.layer_digest.data(), digest_bytes);
    put_u8(out, used, field.index);
    put_u8(out, used, field.total);
    put_u8(out, used, field.held);
    put_u64(out, used, field.states);
}

void write_mutators(
    uint8_t* out, std::size_t* used, const PresenceMutators& field, std::size_t keep
) noexcept {
    std::size_t value = mutators_fixed_value_bytes;
    for (std::size_t cursor = 0; cursor < keep; ++cursor)
        value += mutator_value_bytes(field.mutators[cursor]);
    put_u8(out, used, static_cast<uint8_t>(PresenceTag::mutators));
    put_u16(out, used, static_cast<uint16_t>(value));
    write_mutators_prefix(out, used, field);
    for (std::size_t cursor = 0; cursor < keep; ++cursor)
        write_mutator(out, used, field.mutators[cursor]);
}

void write_mutator_part(uint8_t* out, std::size_t* used, const PresenceMutatorPart& part) noexcept {
    put_u8(out, used, static_cast<uint8_t>(PresenceTag::mutator_part));
    put_u16(out, used, static_cast<uint16_t>(mutator_part_value_bytes(part)));
    put_bytes(out, used, part.digest.data(), digest_bytes);
    put_u8(out, used, part.first);
    for (const auto& item : part.mutators)
        write_mutator(out, used, item);
}

bool read_mutator(Window* window, PresenceMutator* out) {
    PresenceMutator item;
    const uint8_t* prefix = nullptr;
    uint8_t option_count = 0;
    if (!read_string(window, 1, mutator_id_max, ByteKind::kebab, &item.id) ||
        !read_string(window, 0, mutator_version_max, ByteKind::text, &item.version) ||
        !window->take(presence_mutator_prefix_bytes, &prefix) || !window->u8(&option_count) ||
        option_count > presence_mutator_options_max)
        return false;
    std::memcpy(item.sha256_prefix.data(), prefix, presence_mutator_prefix_bytes);
    for (uint8_t option_index = 0; option_index < option_count; ++option_index) {
        PresenceMutatorOption option;
        if (!read_string(window, 1, mutator_option_key_max, ByteKind::ascii, &option.key) ||
            !read_string(window, 0, mutator_option_value_max, ByteKind::text, &option.value))
            return false;
        item.options.push_back(std::move(option));
    }
    *out = std::move(item);
    return true;
}

bool read_mutators(Window window, PresenceMutators* out) {
    PresenceMutators field;
    uint8_t flags = 0;
    const uint8_t* digest = nullptr;
    const uint8_t* layer = nullptr;
    if (!window.u8(&flags) || !window.take(digest_bytes, &digest) ||
        !window.take(digest_bytes, &layer) || !window.u8(&field.index) ||
        !window.u8(&field.total) || !window.u8(&field.held) || !window.u64(&field.states))
        return false;
    field.built = (flags & mutators_built_flag) != 0;
    field.lacking = (flags & mutators_lacking_flag) != 0;
    field.failed = (flags & mutators_failed_flag) != 0;
    std::memcpy(field.digest.data(), digest, digest_bytes);
    std::memcpy(field.layer_digest.data(), layer, digest_bytes);
    if (!mutators_prefix_ok(
            field.built, field.layer_digest, field.index, field.total, field.held, field.states
        ))
        return false;
    while (window.size != 0) {
        PresenceMutator item;
        if (!read_mutator(&window, &item))
            return false;
        field.mutators.push_back(std::move(item));
    }
    if (field.mutators.size() > field.total)
        return false;
    *out = std::move(field);
    return true;
}

bool read_mutator_part(Window window, PresenceMutatorPart* out) {
    PresenceMutatorPart part;
    const uint8_t* digest = nullptr;
    if (!window.take(digest_bytes, &digest) || !window.u8(&part.first))
        return false;
    std::memcpy(part.digest.data(), digest, digest_bytes);
    while (window.size != 0) {
        PresenceMutator item;
        if (!read_mutator(&window, &item))
            return false;
        part.mutators.push_back(std::move(item));
    }
    if (!part_ok(part))
        return false;
    *out = std::move(part);
    return true;
}

/// Adds mutators that continue the list. Items already held are skipped.
void append_continuing(
    PresenceLoadoutAssembly& assembly, uint8_t first, const std::vector<PresenceMutator>& items
) {
    if (first > assembly.mutators.size())
        return;
    std::size_t cursor = assembly.mutators.size() - first;
    while (cursor < items.size() && assembly.mutators.size() < assembly.total &&
           assembly.mutators.size() < presence_mutators_max) {
        assembly.mutators.push_back(items[cursor]);
        ++cursor;
    }
}

/// The most computer players one brains field lists.
constexpr std::size_t brain_count_max = 10;
constexpr std::size_t brain_id_max = 64;
constexpr std::size_t brain_name_max = 32;
constexpr std::size_t brain_version_max = 16;
constexpr std::size_t brain_level_id_max = 64;
constexpr std::size_t brain_level_name_max = 24;
/// Bit 0 of a brain item's flags: the brain sees every unit. A bit past 0
/// refuses the field when it is read.
constexpr uint8_t brain_full_awareness_flag = 0x01;

/// Cuts text at a UTF-8 character boundary.
///
/// A limit that lands inside a character keeps the bytes before that character.
///
/// @param text the text
/// @param most the most bytes
/// @return the text, shortened when it was longer
std::string_view cut_on_character(std::string_view text, std::size_t most) noexcept {
    if (text.size() <= most)
        return text;
    std::size_t cut = most;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xc0u) == 0x80u)
        --cut;
    return text.substr(0, cut);
}

/// A brain item's name, version and level after the field's length limits.
struct BrainShown {
    std::string_view name{};
    std::string_view version{};
    std::string_view level{};
};

/// Tells whether one brain item can be written, and the text the field carries.
bool brain_item_ok(const PresenceBrain& item, BrainShown* shown) noexcept {
    if (!string_in_rule(item.id, 1, brain_id_max, ByteKind::kebab) ||
        !string_in_rule(item.level_id, 1, brain_level_id_max, ByteKind::kebab))
        return false;
    if (!text_bytes(reinterpret_cast<const uint8_t*>(item.name.data()), item.name.size()) ||
        !text_bytes(reinterpret_cast<const uint8_t*>(item.version.data()), item.version.size()) ||
        !text_bytes(reinterpret_cast<const uint8_t*>(item.level.data()), item.level.size()))
        return false;
    const auto name = cut_on_character(item.name, brain_name_max);
    const auto version = cut_on_character(item.version, brain_version_max);
    const auto level = cut_on_character(item.level, brain_level_name_max);
    if (!string_in_rule(name, 1, brain_name_max, ByteKind::text) ||
        !string_in_rule(version, 0, brain_version_max, ByteKind::text) ||
        !string_in_rule(level, 1, brain_level_name_max, ByteKind::text))
        return false;
    *shown = BrainShown{name, version, level};
    return true;
}

/// Bytes of one brain item, its texts already within their limits.
std::size_t brain_item_bytes(const PresenceBrain& item, BrainShown shown) noexcept {
    return 4 + string_bytes(item.id) + string_bytes(shown.name) + string_bytes(shown.version) +
           string_bytes(item.level_id) + string_bytes(shown.level) + 1;
}

/// Writes one brain item. The caller has kept room for it.
void write_brain_item(
    uint8_t* out, std::size_t* used, const PresenceBrain& item, BrainShown shown
) noexcept {
    put_u32(out, used, item.player);
    put_string(out, used, item.id);
    put_string(out, used, shown.name);
    put_string(out, used, shown.version);
    put_string(out, used, item.level_id);
    put_string(out, used, shown.level);
    put_u8(out, used, item.full_awareness ? brain_full_awareness_flag : 0);
}

} // namespace

WireError
presence_record_length(const uint8_t* bytes, std::size_t available, uint16_t* length) noexcept {
    if (bytes == nullptr || length == nullptr)
        return WireError::bad_argument;
    if (available < 3)
        return WireError::truncated;
    if (bytes[0] != presence_record_type)
        return WireError::invalid_type;
    const auto word = load_u16(bytes + 1);
    if (word < presence_record_header_bytes || word > presence_record_max_bytes)
        return WireError::length_mismatch;
    *length = word;
    return WireError::ok;
}

WireError encode_presence(
    const PresenceRecord& record, uint8_t* out, std::size_t capacity, std::size_t* written
) noexcept {
    if (out == nullptr || written == nullptr)
        return WireError::bad_argument;
    if (record.version == 0)
        return WireError::bad_argument;
    if (record.engine && !engine_ok(*record.engine))
        return WireError::bad_argument;
    if (record.mod && !mod_ok(*record.mod))
        return WireError::bad_argument;
    if (record.mod_origin && !origin_ok(*record.mod_origin))
        return WireError::bad_argument;
    if (record.developer && !developer_ok(*record.developer))
        return WireError::bad_argument;
    if (record.game_hacks && !hacks_ok(*record.game_hacks))
        return WireError::bad_argument;
    if (record.view_hacks && !hacks_ok(*record.view_hacks))
        return WireError::bad_argument;
    if (record.map_pack && !map_pack_ok(*record.map_pack))
        return WireError::bad_argument;
    if (record.bundles && !bundles_ok(*record.bundles))
        return WireError::bad_argument;
    if (record.language && !language_ok(*record.language))
        return WireError::bad_argument;
    if (record.mutators && !mutators_ok(*record.mutators))
        return WireError::bad_argument;
    if (record.mutator_modes &&
        (!record.mutators || !clash_mask_fits(*record.mutator_modes, record.mutators->total)))
        return WireError::bad_argument;
    if (record.mutator_part && !part_ok(*record.mutator_part))
        return WireError::bad_argument;

    const auto limit = capacity < presence_record_max_bytes ? capacity : presence_record_max_bytes;
    std::size_t fixed = presence_record_header_bytes;
    if (record.engine)
        fixed += presence_field_header_bytes + engine_value_bytes(*record.engine);
    if (record.content)
        fixed += presence_field_header_bytes + content_value_bytes;
    if (record.bundles)
        fixed += presence_field_header_bytes + bundles_value_bytes(*record.bundles);
    if (record.language)
        fixed += presence_field_header_bytes + string_bytes(*record.language);
    if (record.mod)
        fixed += presence_field_header_bytes + mod_value_bytes(*record.mod);
    if (record.mod_origin)
        fixed += presence_field_header_bytes + origin_value_bytes(*record.mod_origin);
    if (record.sim_hash)
        fixed += presence_field_header_bytes + digest_bytes;
    if (record.mutators)
        fixed += presence_field_header_bytes + mutators_fixed_value_bytes;
    if (record.mutator_modes)
        fixed += presence_field_header_bytes + mutator_modes_value_bytes;
    if (record.map_pack)
        fixed += presence_field_header_bytes + map_pack_value_bytes(*record.map_pack);
    if (record.mutator_part)
        fixed += presence_field_header_bytes + mutator_part_value_bytes(*record.mutator_part);
    // A list's count is fixed. Only the ids after it are cut, and only
    // after every later list's count has its room.
    constexpr std::size_t hack_prefix = presence_field_header_bytes + 2;
    constexpr std::size_t developer_prefix = presence_field_header_bytes + 1 + 2;
    if (record.developer)
        fixed += developer_prefix;
    if (record.game_hacks)
        fixed += hack_prefix;
    if (record.view_hacks)
        fixed += hack_prefix;
    if (fixed > limit)
        return WireError::buffer_too_small;

    std::size_t mutator_keep = 0;
    std::size_t dev_keep = 0;
    std::size_t game_keep = 0;
    std::size_t view_keep = 0;
    std::size_t room = limit - fixed;
    // Whole mutators take room before developer overrides and hack ids.
    if (record.mutators) {
        for (const auto& item : record.mutators->mutators) {
            const auto item_bytes = mutator_value_bytes(item);
            if (item_bytes > room)
                break;
            room -= item_bytes;
            ++mutator_keep;
        }
    }
    auto cut_overrides = [&]() noexcept {
        dev_keep = 0;
        if (!record.developer)
            return;
        for (const auto& over : record.developer->overrides) {
            const auto item = 2 + over.hack.size();
            if (item > room)
                break;
            room -= item;
            ++dev_keep;
        }
    };
    auto cut_ids = [&](const PresenceHacks* hacks, std::size_t* keep) noexcept {
        *keep = 0;
        if (hacks == nullptr)
            return;
        for (const auto& id : hacks->ids) {
            const auto item = 1 + id.size();
            if (item > room)
                break;
            room -= item;
            ++*keep;
        }
    };
    cut_overrides();
    cut_ids(record.game_hacks ? &*record.game_hacks : nullptr, &game_keep);
    cut_ids(record.view_hacks ? &*record.view_hacks : nullptr, &view_keep);

    uint8_t local[presence_record_max_bytes];
    std::size_t used = 0;
    put_u8(local, &used, presence_record_type);
    put_u16(local, &used, 0);
    put_u8(local, &used, record.version);
    if (record.engine)
        write_engine(local, &used, *record.engine);
    if (record.content)
        write_content(local, &used, *record.content);
    if (record.bundles)
        write_bundles(local, &used, *record.bundles);
    if (record.language)
        write_language(local, &used, *record.language);
    if (record.mod)
        write_mod(local, &used, *record.mod);
    if (record.mod_origin)
        write_origin(local, &used, *record.mod_origin);
    if (record.sim_hash)
        write_sim(local, &used, *record.sim_hash);
    if (record.mutators)
        write_mutators(local, &used, *record.mutators, mutator_keep);
    if (record.mutator_modes) {
        put_u8(local, &used, static_cast<uint8_t>(PresenceTag::mutator_modes));
        put_u16(local, &used, static_cast<uint16_t>(mutator_modes_value_bytes));
        put_u32(local, &used, *record.mutator_modes);
    }
    if (record.map_pack)
        write_map_pack(local, &used, *record.map_pack);
    if (record.mutator_part)
        write_mutator_part(local, &used, *record.mutator_part);
    if (record.developer) {
        std::size_t value = 1 + 2;
        for (std::size_t i = 0; i < dev_keep; ++i)
            value += 2 + record.developer->overrides[i].hack.size();
        put_u8(local, &used, static_cast<uint8_t>(PresenceTag::developer));
        put_u16(local, &used, static_cast<uint16_t>(value));
        put_u8(local, &used, record.developer->on ? developer_mode_flag : 0);
        put_u16(local, &used, record.developer->total);
        for (std::size_t i = 0; i < dev_keep; ++i) {
            const auto& over = record.developer->overrides[i];
            put_u8(local, &used, over.on ? 1 : 0);
            put_string(local, &used, over.hack);
        }
    }
    auto write_hacks = [&](PresenceTag tag, const PresenceHacks& hacks, std::size_t keep) noexcept {
        std::size_t value = 2;
        for (std::size_t i = 0; i < keep; ++i)
            value += 1 + hacks.ids[i].size();
        put_u8(local, &used, static_cast<uint8_t>(tag));
        put_u16(local, &used, static_cast<uint16_t>(value));
        put_u16(local, &used, hacks.total);
        for (std::size_t i = 0; i < keep; ++i)
            put_string(local, &used, hacks.ids[i]);
    };
    if (record.game_hacks)
        write_hacks(PresenceTag::game_hacks, *record.game_hacks, game_keep);
    if (record.view_hacks)
        write_hacks(PresenceTag::view_hacks, *record.view_hacks, view_keep);
    assert(used <= limit && used <= presence_record_max_bytes);
    store_u16(local + 1, static_cast<uint16_t>(used));
    std::memcpy(out, local, used);
    *written = used;
    return WireError::ok;
}

WireError decode_presence(const uint8_t* record, std::size_t size, PresenceRecord* out) {
    if (record == nullptr || out == nullptr)
        return WireError::bad_argument;
    const auto error = framed_record(record, size);
    if (error != WireError::ok)
        return error;

    PresenceRecord decoded;
    decoded.version = record[3];
    bool saw_engine = false;
    bool saw_mod = false;
    bool saw_origin = false;
    bool saw_sim = false;
    bool saw_game = false;
    bool saw_view = false;
    bool saw_developer = false;
    bool saw_pack = false;
    bool saw_content = false;
    bool saw_bundles = false;
    bool saw_language = false;
    bool saw_mutators = false;
    bool saw_part = false;
    bool saw_modes = false;
    std::optional<uint32_t> pending_modes;
    std::size_t at = presence_record_header_bytes;
    while (at < size) {
        assert(at + presence_field_header_bytes <= size);
        const auto tag = record[at];
        const auto value_size = static_cast<std::size_t>(load_u16(record + at + 1));
        assert(value_size <= size - at - presence_field_header_bytes);
        Window value{record + at + presence_field_header_bytes, value_size};
        at += presence_field_header_bytes + value_size;
        if (tag == static_cast<uint8_t>(PresenceTag::engine)) {
            if (saw_engine)
                continue;
            saw_engine = true;
            PresenceEngine engine;
            if (read_engine(value, &engine))
                decoded.engine = std::move(engine);
        } else if (tag == static_cast<uint8_t>(PresenceTag::mod)) {
            if (saw_mod)
                continue;
            saw_mod = true;
            PresenceMod mod;
            if (read_mod(value, &mod))
                decoded.mod = std::move(mod);
        } else if (tag == static_cast<uint8_t>(PresenceTag::mod_origin)) {
            if (saw_origin)
                continue;
            saw_origin = true;
            PresenceOrigin origin;
            if (read_origin(value, &origin))
                decoded.mod_origin = std::move(origin);
        } else if (tag == static_cast<uint8_t>(PresenceTag::sim_hash)) {
            if (saw_sim)
                continue;
            saw_sim = true;
            if (value.size == digest_bytes) {
                PresenceDigest hash{};
                std::memcpy(hash.data(), value.data, digest_bytes);
                decoded.sim_hash = hash;
            }
        } else if (tag == static_cast<uint8_t>(PresenceTag::game_hacks)) {
            if (saw_game)
                continue;
            saw_game = true;
            PresenceHacks hacks;
            if (read_hacks(value, &hacks))
                decoded.game_hacks = std::move(hacks);
        } else if (tag == static_cast<uint8_t>(PresenceTag::view_hacks)) {
            if (saw_view)
                continue;
            saw_view = true;
            PresenceHacks hacks;
            if (read_hacks(value, &hacks))
                decoded.view_hacks = std::move(hacks);
        } else if (tag == static_cast<uint8_t>(PresenceTag::developer)) {
            if (saw_developer)
                continue;
            saw_developer = true;
            PresenceDeveloper developer;
            if (read_developer(value, &developer))
                decoded.developer = std::move(developer);
        } else if (tag == static_cast<uint8_t>(PresenceTag::map_pack)) {
            if (saw_pack)
                continue;
            saw_pack = true;
            PresenceMapPack pack;
            if (read_map_pack(value, &pack))
                decoded.map_pack = std::move(pack);
        } else if (tag == static_cast<uint8_t>(PresenceTag::content)) {
            if (saw_content)
                continue;
            saw_content = true;
            // One byte of flags; a value of any other size is left out.
            if (value.size == content_value_bytes)
                decoded.content = PresenceContent{(value.data[0] & content_fetches_flag) != 0};
        } else if (tag == static_cast<uint8_t>(PresenceTag::bundles)) {
            if (saw_bundles)
                continue;
            saw_bundles = true;
            PresenceBundles bundles;
            if (read_bundles(value, &bundles))
                decoded.bundles = std::move(bundles);
        } else if (tag == static_cast<uint8_t>(PresenceTag::language)) {
            if (saw_language)
                continue;
            saw_language = true;
            std::string language;
            if (read_language(value, &language))
                decoded.language = std::move(language);
        } else if (tag == static_cast<uint8_t>(PresenceTag::mutators)) {
            if (saw_mutators)
                continue;
            saw_mutators = true;
            PresenceMutators loaded;
            if (read_mutators(value, &loaded))
                decoded.mutators = std::move(loaded);
        } else if (tag == static_cast<uint8_t>(PresenceTag::mutator_part)) {
            if (saw_part)
                continue;
            saw_part = true;
            PresenceMutatorPart part;
            if (read_mutator_part(value, &part))
                decoded.mutator_part = std::move(part);
        } else if (tag == static_cast<uint8_t>(PresenceTag::mutator_modes)) {
            if (saw_modes)
                continue;
            saw_modes = true;
            uint32_t mask = 0;
            if (value.size == mutator_modes_value_bytes && value.u32(&mask))
                pending_modes = mask;
        }
    }
    if (pending_modes && decoded.mutators &&
        clash_mask_fits(*pending_modes, decoded.mutators->total))
        decoded.mutator_modes = pending_modes;
    *out = std::move(decoded);
    return WireError::ok;
}

bool find_presence_field(
    const uint8_t* record, std::size_t size, PresenceTag tag, std::span<const uint8_t>* value
) noexcept {
    if (record == nullptr || value == nullptr || framed_record(record, size) != WireError::ok)
        return false;
    const auto want = static_cast<uint8_t>(tag);
    std::size_t at = presence_record_header_bytes;
    while (at < size) {
        assert(at + presence_field_header_bytes <= size);
        const auto value_size = static_cast<std::size_t>(load_u16(record + at + 1));
        assert(value_size <= size - at - presence_field_header_bytes);
        const auto start = at + presence_field_header_bytes;
        if (record[at] == want) {
            *value = std::span<const uint8_t>(record + start, value_size);
            return true;
        }
        at = start + value_size;
    }
    return false;
}

PresenceContentMatch presence_content_match(
    std::span<const uint8_t> first_record, std::span<const uint8_t> second_record
) noexcept {
    BundleReading first;
    BundleReading second;
    if (!bundles_of(first_record, &first) || !bundles_of(second_record, &second))
        return PresenceContentMatch::unknown;
    if (first.differs || second.differs || first.base != second.base || first.count != second.count)
        return PresenceContentMatch::differs;
    for (uint8_t index = 0; index < first.count; ++index) {
        bool found = false;
        for (uint8_t other = 0; other < second.count; ++other)
            if (second.ids[other] == first.ids[index])
                found = true;
        if (!found)
            return PresenceContentMatch::differs;
    }
    return PresenceContentMatch::same;
}

namespace {

void keep_rule_changing(const PresenceBundles& bundles, std::vector<std::string>* ids) {
    for (const auto& id : bundles.expansions) {
        if (id == presence_uncompared_expansion)
            continue;
        bool seen = false;
        for (const auto& have : *ids)
            if (have == id)
                seen = true;
        if (!seen)
            ids->push_back(id);
    }
}

} // namespace

PresenceContentGap presence_content_gap(const PresenceRecord& player, const PresenceRecord& host) {
    PresenceContentGap gap;
    if (!player.bundles || !host.bundles)
        return gap;
    gap.comparable = true;
    gap.hashes_differ = player.bundles->differs_from_known || host.bundles->differs_from_known;
    gap.player_base = player.bundles->base_game;
    gap.host_base = host.bundles->base_game;
    gap.bases_differ = gap.player_base != gap.host_base;
    std::vector<std::string> player_ids;
    std::vector<std::string> host_ids;
    keep_rule_changing(*player.bundles, &player_ids);
    keep_rule_changing(*host.bundles, &host_ids);
    for (const auto& id : host_ids) {
        bool found = false;
        for (const auto& have : player_ids)
            if (have == id)
                found = true;
        if (!found)
            gap.missing.push_back(id);
    }
    for (const auto& id : player_ids) {
        bool found = false;
        for (const auto& have : host_ids)
            if (have == id)
                found = true;
        if (!found)
            gap.extra.push_back(id);
    }
    return gap;
}

WireError presence_content_identity(
    const PresenceRecord& record, uint8_t* out, std::size_t capacity, std::size_t* written
) noexcept {
    if (out == nullptr || written == nullptr)
        return WireError::bad_argument;
    *written = 0;
    if (!record.bundles)
        return WireError::ok;
    std::string_view ids[presence_bundle_count_max];
    uint8_t count = 0;
    if (!rule_changing_ids(*record.bundles, ids, &count))
        return WireError::bad_argument;
    std::size_t need = record.bundles->base_game.size() + 1;
    for (uint8_t index = 0; index < count; ++index)
        need += ids[index].size() + 1;
    need += 2;
    if (need > capacity)
        return WireError::buffer_too_small;
    std::size_t used = 0;
    const auto put = [&](std::string_view text) noexcept {
        if (!text.empty())
            std::memcpy(out + used, text.data(), text.size());
        used += text.size();
        out[used++] = 0;
    };
    put(record.bundles->base_game);
    for (uint8_t index = 0; index < count; ++index)
        put(ids[index]);
    out[used++] = 0;
    out[used++] = record.bundles->differs_from_known ? 1 : 0;
    *written = used;
    return WireError::ok;
}

std::optional<PresenceDigest> presence_sim_hash(const uint8_t* record, std::size_t size) noexcept {
    std::span<const uint8_t> value;
    if (!find_presence_field(record, size, PresenceTag::sim_hash, &value) ||
        value.size() != digest_bytes)
        return std::nullopt;
    PresenceDigest hash{};
    std::memcpy(hash.data(), value.data(), digest_bytes);
    return hash;
}

WireError encode_presence_map_pack(
    const PresenceMapPack& pack, uint8_t* out, std::size_t capacity, std::size_t* written
) noexcept {
    if (out == nullptr || written == nullptr)
        return WireError::bad_argument;
    if (!map_pack_ok(pack))
        return WireError::bad_argument;
    const auto size = map_pack_value_bytes(pack);
    if (size > capacity)
        return WireError::buffer_too_small;
    std::size_t used = 0;
    write_map_pack_value(out, &used, pack);
    assert(used == size);
    *written = used;
    return WireError::ok;
}

WireError encode_presence_brains(
    std::span<const PresenceBrain> brains,
    uint8_t* out,
    std::size_t capacity,
    std::size_t* written,
    std::size_t* sent
) noexcept {
    if (out == nullptr || written == nullptr || sent == nullptr || brains.empty())
        return WireError::bad_argument;
    const std::size_t offered = brains.size() < brain_count_max ? brains.size() : brain_count_max;
    std::array<BrainShown, brain_count_max> shown{};
    for (std::size_t index = 0; index < offered; ++index)
        if (!brain_item_ok(brains[index], &shown[index]))
            return WireError::bad_argument;
    std::size_t value_bytes = 1;
    std::size_t count = 0;
    for (std::size_t index = 0; index < offered; ++index) {
        const auto item_bytes = brain_item_bytes(brains[index], shown[index]);
        if (value_bytes + item_bytes > capacity)
            break;
        value_bytes += item_bytes;
        ++count;
    }
    if (count == 0)
        return WireError::buffer_too_small;
    std::size_t used = 0;
    put_u8(out, &used, static_cast<uint8_t>(count));
    for (std::size_t index = 0; index < count; ++index)
        write_brain_item(out, &used, brains[index], shown[index]);
    assert(used == value_bytes);
    *written = used;
    *sent = count;
    return WireError::ok;
}

std::optional<std::vector<PresenceBrain>> decode_presence_brains(std::span<const uint8_t> value) {
    Window window{value.data(), value.size()};
    uint8_t count = 0;
    if (!window.u8(&count) || count < 1 || count > brain_count_max)
        return std::nullopt;
    std::vector<PresenceBrain> brains;
    brains.reserve(count);
    for (uint8_t index = 0; index < count; ++index) {
        PresenceBrain item;
        uint8_t flags = 0;
        if (!window.u32(&item.player) ||
            !read_string(&window, 1, brain_id_max, ByteKind::kebab, &item.id) ||
            !read_string(&window, 1, brain_name_max, ByteKind::text, &item.name) ||
            !read_string(&window, 0, brain_version_max, ByteKind::text, &item.version) ||
            !read_string(&window, 1, brain_level_id_max, ByteKind::kebab, &item.level_id) ||
            !read_string(&window, 1, brain_level_name_max, ByteKind::text, &item.level) ||
            !window.u8(&flags) || (flags & static_cast<uint8_t>(~brain_full_awareness_flag)) != 0)
            return std::nullopt;
        item.full_awareness = (flags & brain_full_awareness_flag) != 0;
        brains.push_back(std::move(item));
    }
    if (window.size != 0)
        return std::nullopt;
    return brains;
}

WireError encode_presence_mutator_part(
    const PresenceMutators& loadout,
    uint8_t first,
    uint8_t* out,
    std::size_t capacity,
    std::size_t* written,
    uint8_t* sent
) noexcept {
    if (out == nullptr || written == nullptr || sent == nullptr)
        return WireError::bad_argument;
    if (first >= loadout.mutators.size() || first >= presence_mutators_max)
        return WireError::bad_argument;
    const std::size_t end = loadout.mutators.size() < presence_mutators_max
                                ? loadout.mutators.size()
                                : static_cast<std::size_t>(presence_mutators_max);
    for (std::size_t cursor = first; cursor < end; ++cursor)
        if (!mutator_ok(loadout.mutators[cursor]))
            return WireError::bad_argument;
    const std::size_t prefix = digest_bytes + 1;
    std::size_t value_bytes = prefix;
    uint8_t count = 0;
    for (std::size_t cursor = first; cursor < end; ++cursor) {
        const auto item_bytes = mutator_value_bytes(loadout.mutators[cursor]);
        if (value_bytes + item_bytes > capacity)
            break;
        value_bytes += item_bytes;
        ++count;
    }
    if (count == 0)
        return WireError::buffer_too_small;
    std::size_t used = 0;
    put_bytes(out, &used, loadout.digest.data(), digest_bytes);
    put_u8(out, &used, first);
    for (uint8_t cursor = 0; cursor < count; ++cursor)
        write_mutator(out, &used, loadout.mutators[static_cast<std::size_t>(first) + cursor]);
    assert(used == value_bytes);
    *written = used;
    *sent = count;
    return WireError::ok;
}

std::optional<PresenceMutatorsState>
presence_mutators_state(const uint8_t* record, std::size_t size) noexcept {
    std::span<const uint8_t> value;
    if (!find_presence_field(record, size, PresenceTag::mutators, &value) ||
        value.size() < mutators_fixed_value_bytes)
        return std::nullopt;
    const uint8_t* cursor = value.data();
    const uint8_t flags = cursor[0];
    cursor += 1;
    PresenceMutatorsState state;
    state.built = (flags & mutators_built_flag) != 0;
    state.lacking = (flags & mutators_lacking_flag) != 0;
    state.failed = (flags & mutators_failed_flag) != 0;
    std::memcpy(state.digest.data(), cursor, digest_bytes);
    cursor += digest_bytes;
    std::memcpy(state.layer_digest.data(), cursor, digest_bytes);
    cursor += digest_bytes;
    state.index = cursor[0];
    state.total = cursor[1];
    state.held = cursor[2];
    state.states = load_le64(cursor + 3);
    if (!mutators_prefix_ok(
            state.built, state.layer_digest, state.index, state.total, state.held, state.states
        ))
        return std::nullopt;
    return state;
}

bool presence_assembly_take(PresenceLoadoutAssembly& assembly, const PresenceRecord& host) {
    const PresenceLoadoutAssembly previous = assembly;
    if (!host.mutators) {
        assembly = {};
        return assembly != previous;
    }
    const PresenceMutators& incoming = *host.mutators;
    if (assembly.digest != incoming.digest || assembly.total != incoming.total) {
        assembly = {};
        assembly.digest = incoming.digest;
        assembly.total = incoming.total;
    }
    append_continuing(assembly, 0, incoming.mutators);
    if (host.mutator_part && host.mutator_part->digest == assembly.digest)
        append_continuing(assembly, host.mutator_part->first, host.mutator_part->mutators);
    return assembly != previous;
}

bool presence_assembly_whole(const PresenceLoadoutAssembly& assembly) noexcept {
    return assembly.total != 0 && assembly.mutators.size() == assembly.total;
}

WireError presence_clash_modes(uint32_t mask, uint8_t count, PresenceClashMode* modes) noexcept {
    if (modes == nullptr || !clash_mask_fits(mask, count))
        return WireError::bad_argument;
    for (uint8_t position = 0; position < count; ++position) {
        const uint32_t bit = uint32_t{1} << position;
        modes[position] = (mask & bit) != 0 ? PresenceClashMode::add : PresenceClashMode::replace;
    }
    return WireError::ok;
}

WireError append_presence_field(
    uint8_t* record,
    std::size_t size,
    std::size_t capacity,
    PresenceTag tag,
    std::span<const uint8_t> value,
    std::size_t* written
) noexcept {
    if (record == nullptr || written == nullptr)
        return WireError::bad_argument;
    if (value.size() > 0xffff || (value.size() != 0 && value.data() == nullptr))
        return WireError::bad_argument;
    if (framed_record(record, size) != WireError::ok)
        return WireError::bad_argument;
    const auto added = presence_field_header_bytes + value.size();
    if (size > presence_record_max_bytes || added > presence_record_max_bytes - size)
        return WireError::buffer_too_small;
    if (capacity < size + added)
        return WireError::buffer_too_small;
    record[size] = static_cast<uint8_t>(tag);
    store_u16(record + size + 1, static_cast<uint16_t>(value.size()));
    if (!value.empty())
        std::memcpy(record + size + presence_field_header_bytes, value.data(), value.size());
    const auto total = size + added;
    store_u16(record + 1, static_cast<uint16_t>(total));
    *written = total;
    return WireError::ok;
}

bool presence_peer(const uint8_t* block) noexcept {
    return sent_by_open_annihilation(block);
}

} // namespace oa::netgame
