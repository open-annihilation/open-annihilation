// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The streamed zip reader: the central directory read through a hook, the
// 64-bit extension included, and each entry's data read and inflated a
// piece at a time.

#include "oa/formats/zip/stream.hpp"

#include "oa/formats/zip/inflate.hpp"
#include "records.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zlib.h>

namespace oa::formats::zip {

using namespace detail;

namespace {

/// The bytes read from the archive, and inflated, at a time.
constexpr std::size_t piece_bytes = std::size_t{64} << 10;
/// The most bytes one byte of deflated data can inflate to (read.cpp).
constexpr uint64_t max_inflate_ratio = 1032;
/// The bytes from the archive's end the end record may start in: the record
/// and the longest comment.
constexpr uint64_t end_search_bytes = end_bytes + max_comment_bytes;
/// The length of the 64-bit end record's size field's own bytes and the
/// signature before it: the record's size counts what follows them.
constexpr uint64_t zip64_end_leading_bytes = 12;
/// The bytes of a local header's 64-bit extra field: the size, then the
/// stored size.
constexpr std::size_t local_zip64_bytes = 16;
/// The Unicode path extra field's id, its version and the bytes before its
/// name: the version and the recorded name's CRC-32.
constexpr uint16_t unicode_path_extra_id = 0x7075;
constexpr uint8_t unicode_path_version = 1;
constexpr std::size_t unicode_path_header_bytes = 5;
/// The highest code point.
constexpr uint32_t highest_code_point = 0x10FFFF;
/// The first and last UTF-16 surrogate code points, which UTF-8 never holds.
constexpr uint32_t first_surrogate = 0xD800;
constexpr uint32_t last_surrogate = 0xDFFF;

/// Code page 437's characters 0x80 to 0xFF, as Unicode code points.
constexpr std::array<uint16_t, 128> cp437_high{
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF,
    0x00EE, 0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA,
    0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557,
    0x255D, 0x255C, 0x255B, 0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559,
    0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4,
    0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

/// Records a failure.
///
/// @param[out] error the error to fill
/// @param status why the read failed
/// @param offset the byte offset of the record at fault
/// @param entry the entry's name; empty for the archive itself
/// @return false, for the caller to return
bool fail(ZipError& error, ZipStatus status, uint64_t offset, std::string_view entry = {}) {
    error = ZipError{status, offset, std::string{entry}};
    return false;
}

/// Reads bytes of the archive.
///
/// @param source the archive's hooks
/// @param offset where the bytes start
/// @param[out] bytes filled with them
/// @return true when they were all read
bool read_bytes(const SourceHooks& source, uint64_t offset, std::span<uint8_t> bytes) {
    if (bytes.empty())
        return true;
    return source.read_at != nullptr && source.read_at(source.context, offset, bytes);
}

/// Tells whether a method is one the reader takes.
///
/// @param method the recorded method
/// @return true for stored and deflated
bool method_supported(uint16_t method) noexcept {
    return method == static_cast<uint16_t>(Method::stored) ||
           method == static_cast<uint16_t>(Method::deflated);
}

/// Tells whether bytes are well-formed UTF-8: no overlong form, no
/// surrogate and nothing above U+10FFFF.
///
/// @param text the bytes
/// @return true when they are
bool valid_utf8(std::string_view text) noexcept {
    std::size_t at = 0;
    while (at < text.size()) {
        const auto lead = static_cast<unsigned char>(text[at]);
        std::size_t length = 0;
        uint32_t code = 0;
        uint32_t least = 0;
        if (lead < 0x80U) {
            ++at;
            continue;
        }
        if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
            code = lead & 0x1FU;
            least = 0x80;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
            code = lead & 0x0FU;
            least = 0x800;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
            code = lead & 0x07U;
            least = 0x10000;
        } else {
            return false;
        }
        if (text.size() - at < length)
            return false;
        for (std::size_t index = 1; index < length; ++index) {
            const auto next = static_cast<unsigned char>(text[at + index]);
            if ((next & 0xC0U) != 0x80U)
                return false;
            code = (code << 6U) | (next & 0x3FU);
        }
        if (code < least || code > highest_code_point ||
            (code >= first_surrogate && code <= last_surrogate))
            return false;
        at += length;
    }
    return true;
}

/// Appends a code point as UTF-8.
///
/// @param[in,out] out the text
/// @param code the code point, below 0x10000
void append_utf8(std::string& out, uint32_t code) {
    if (code < 0x80U) {
        out += static_cast<char>(code);
    } else if (code < 0x800U) {
        out += static_cast<char>(0xC0U | (code >> 6U));
        out += static_cast<char>(0x80U | (code & 0x3FU));
    } else {
        out += static_cast<char>(0xE0U | (code >> 12U));
        out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (code & 0x3FU));
    }
}

/// Converts a name written in code page 437 to UTF-8.
///
/// @param raw the name's bytes
/// @return the name in UTF-8; the same bytes when all are ASCII
std::string from_cp437(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (const char character : raw) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x80U)
            out += character;
        else
            append_utf8(out, cp437_high[byte - 0x80U]);
    }
    return out;
}

/// Finds an extra field by its id.
///
/// @param extra the extra fields
/// @param id the field's id
/// @param[out] data the field's data, when found
/// @return true when found whole
bool find_extra(std::span<const uint8_t> extra, uint16_t id, std::span<const uint8_t>& data) {
    std::size_t at = 0;
    while (extra.size() - at >= extra_header_bytes) {
        const uint16_t field = load_le16(extra.data() + at);
        const uint16_t length = load_le16(extra.data() + at + 2);
        at += extra_header_bytes;
        if (extra.size() - at < length)
            return false;
        if (field == id) {
            data = extra.subspan(at, length);
            return true;
        }
        at += length;
    }
    return false;
}

/// Returns the UTF-8 spelling of a name the Unicode path extra field gives,
/// as some writers add to a name they record in another code: the field's
/// version 1, the CRC-32 of the recorded name, which must still match, and
/// the name.
///
/// @param extra the record's extra fields
/// @param raw the recorded name
/// @return the UTF-8 name; nothing without a field that matches
std::optional<std::string> unicode_path(std::span<const uint8_t> extra, std::string_view raw) {
    std::span<const uint8_t> data;
    if (!find_extra(extra, unicode_path_extra_id, data) ||
        data.size() < unicode_path_header_bytes || data[0] != unicode_path_version)
        return std::nullopt;
    const auto raw_bytes =
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
    if (load_le32(data.data() + 1) != crc32_of(raw_bytes))
        return std::nullopt;
    const std::string_view spelled{
        reinterpret_cast<const char*>(data.data() + unicode_path_header_bytes),
        data.size() - unicode_path_header_bytes
    };
    if (spelled.empty() || spelled.size() > max_name_bytes || !valid_utf8(spelled))
        return std::nullopt;
    return std::string(spelled);
}

/// An entry's 64-bit fields, taken from its 64-bit extra field for exactly
/// the fields that hold their sentinel.
struct WideFields {
    uint64_t bytes{};
    uint64_t compressed_bytes{};
    uint64_t local_header_offset{};
    uint32_t start_disk{};
};

/// Reads the 64-bit extra field of a central record into the fields that
/// hold their sentinel, in the format's order: the size, the stored size,
/// the local header's offset and the starting disk.
///
/// @param extra the record's extra fields
/// @param[in,out] fields the 32-bit values, widened where they are sentinels
/// @return true when every sentinel field was found
bool read_wide_fields(std::span<const uint8_t> extra, WideFields& fields) {
    const bool want_bytes = fields.bytes == zip64_sentinel_32;
    const bool want_compressed = fields.compressed_bytes == zip64_sentinel_32;
    const bool want_offset = fields.local_header_offset == zip64_sentinel_32;
    const bool want_disk = fields.start_disk == zip64_sentinel_16;
    if (!want_bytes && !want_compressed && !want_offset && !want_disk)
        return true;
    std::span<const uint8_t> data;
    if (!find_extra(extra, zip64_extra_id, data))
        return false;
    std::size_t at = 0;
    const auto take64 = [&](uint64_t& value) {
        if (data.size() - at < sizeof(uint64_t))
            return false;
        value = load_le64(data.data() + at);
        at += sizeof(uint64_t);
        return true;
    };
    if (want_bytes && !take64(fields.bytes))
        return false;
    if (want_compressed && !take64(fields.compressed_bytes))
        return false;
    if (want_offset && !take64(fields.local_header_offset))
        return false;
    if (want_disk) {
        if (data.size() - at < sizeof(uint32_t))
            return false;
        fields.start_disk = load_le32(data.data() + at);
    }
    return true;
}

/// The end record's values, widened by the 64-bit end record when there is one.
struct EndValues {
    uint64_t disk{};
    uint64_t directory_disk{};
    uint64_t disk_entry_count{};
    uint64_t entry_count{};
    uint64_t directory_bytes{};
    uint64_t directory_offset{};
    /// Where the directory must end by: the 64-bit end record, or the end record.
    uint64_t directory_end_limit{};
    bool zip64{};
};

/// Finds and reads the end record, and the 64-bit end record its locator
/// names when one comes before it.
///
/// @param source the archive's hooks
/// @param archive_bytes the archive's size
/// @param[out] values what they hold
/// @param[out] error the status when they cannot be read
/// @return true when they were read and agree
bool read_end_records(
    const SourceHooks& source, uint64_t archive_bytes, EndValues& values, ZipError& error
) {
    if (archive_bytes < end_bytes)
        return fail(error, ZipStatus::no_end_record, 0);
    const uint64_t tail_bytes = std::min(archive_bytes, end_search_bytes + zip64_locator_bytes);
    const uint64_t tail_start = archive_bytes - tail_bytes;
    std::vector<uint8_t> tail(static_cast<std::size_t>(tail_bytes));
    if (!read_bytes(source, tail_start, tail))
        return fail(error, ZipStatus::read_failed, tail_start);
    // The last signature whose comment reaches exactly to the end.
    const std::size_t last = tail.size() - end_bytes;
    const std::size_t first = last - std::min<std::size_t>(last, max_comment_bytes);
    std::size_t found = tail.size();
    for (std::size_t offset = last + 1; offset-- > first;)
        if (signature_at(tail, offset, end_signature) &&
            offset + end_bytes + load_le16(tail.data() + offset + end_comment_length) ==
                tail.size()) {
            found = offset;
            break;
        }
    if (found == tail.size())
        return fail(error, ZipStatus::no_end_record, 0);
    const uint64_t end_offset = tail_start + found;
    const uint8_t* end = tail.data() + found;
    const uint16_t disk = load_le16(end + end_disk);
    const uint16_t directory_disk = load_le16(end + end_directory_disk);
    const uint16_t disk_entry_count = load_le16(end + end_disk_entry_count);
    const uint16_t entry_count = load_le16(end + end_entry_count);
    const uint32_t directory_bytes = load_le32(end + end_directory_bytes);
    const uint32_t directory_offset = load_le32(end + end_directory_offset);
    values = EndValues{
        disk,
        directory_disk,
        disk_entry_count,
        entry_count,
        directory_bytes,
        directory_offset,
        end_offset,
        false
    };
    const bool sentinel =
        disk == zip64_sentinel_16 || directory_disk == zip64_sentinel_16 ||
        disk_entry_count == zip64_sentinel_16 || entry_count == zip64_sentinel_16 ||
        directory_bytes == zip64_sentinel_32 || directory_offset == zip64_sentinel_32;
    if (found < zip64_locator_bytes ||
        !signature_at(tail, found - zip64_locator_bytes, zip64_locator_signature)) {
        if (sentinel)
            return fail(error, ZipStatus::bad_zip64_record, end_offset);
        return true;
    }
    const uint8_t* locator = end - zip64_locator_bytes;
    const uint64_t locator_offset = end_offset - zip64_locator_bytes;
    if (load_le32(locator + zip64_locator_disk) != 0 ||
        load_le32(locator + zip64_locator_disk_count) != 1)
        return fail(error, ZipStatus::several_disks, locator_offset);
    const uint64_t record_offset = load_le64(locator + zip64_locator_end_offset);
    if (record_offset > locator_offset || locator_offset - record_offset < zip64_end_bytes)
        return fail(error, ZipStatus::bad_zip64_record, locator_offset);
    std::array<uint8_t, zip64_end_bytes> record{};
    if (!read_bytes(source, record_offset, record))
        return fail(error, ZipStatus::read_failed, record_offset);
    const uint64_t record_size = load_le64(record.data() + 4);
    if (!signature_at(record, 0, zip64_end_signature) ||
        record_size < zip64_end_bytes - zip64_end_leading_bytes ||
        record_size > locator_offset - record_offset - zip64_end_leading_bytes)
        return fail(error, ZipStatus::bad_zip64_record, record_offset);
    const EndValues wide{
        load_le32(record.data() + zip64_end_disk),
        load_le32(record.data() + zip64_end_directory_disk),
        load_le64(record.data() + zip64_end_disk_entry_count),
        load_le64(record.data() + zip64_end_entry_count),
        load_le64(record.data() + zip64_end_directory_bytes),
        load_le64(record.data() + zip64_end_directory_offset),
        record_offset,
        true
    };
    // A 32-bit value the end record holds itself must be the 64-bit one.
    const auto agrees = [](uint64_t narrow, uint64_t sentinel_value, uint64_t wide_value) {
        return narrow == sentinel_value || narrow == wide_value;
    };
    if (!agrees(disk, zip64_sentinel_16, wide.disk) ||
        !agrees(directory_disk, zip64_sentinel_16, wide.directory_disk) ||
        !agrees(disk_entry_count, zip64_sentinel_16, wide.disk_entry_count) ||
        !agrees(entry_count, zip64_sentinel_16, wide.entry_count) ||
        !agrees(directory_bytes, zip64_sentinel_32, wide.directory_bytes) ||
        !agrees(directory_offset, zip64_sentinel_32, wide.directory_offset))
        return fail(error, ZipStatus::bad_zip64_record, record_offset);
    values = wide;
    return true;
}

/// Reads one central directory record into an entry.
///
/// @param directory the whole directory
/// @param offset the record's offset in it
/// @param record_at the record's offset in the archive, for errors
/// @param limits the bounds kept
/// @param[out] entry the entry
/// @param[out] length the record's whole length
/// @param[out] error the status when the record is refused
/// @return true when the record was read and checked
bool read_record(
    std::span<const uint8_t> directory,
    std::size_t offset,
    uint64_t record_at,
    const StreamLimits& limits,
    StreamEntry& entry,
    std::size_t& length,
    ZipError& error
) {
    if (directory.size() - offset < central_bytes)
        return fail(error, ZipStatus::truncated, record_at);
    const uint8_t* record = directory.data() + offset;
    if (!signature_at(directory, offset, central_signature))
        return fail(error, ZipStatus::bad_central_record, record_at);
    const uint8_t host = static_cast<uint8_t>(load_le16(record + central_version_made_by) >> 8U);
    const uint16_t flags = load_le16(record + central_flags);
    const uint16_t method = load_le16(record + central_method);
    const std::size_t name_length = load_le16(record + central_name_length);
    const std::size_t extra_length = load_le16(record + central_extra_length);
    const std::size_t comment_length = load_le16(record + central_comment_length);
    const std::size_t tail_length = name_length + extra_length + comment_length;
    if (directory.size() - offset - central_bytes < tail_length)
        return fail(error, ZipStatus::truncated, record_at);
    length = central_bytes + tail_length;
    const std::string_view raw{reinterpret_cast<const char*>(record + central_bytes), name_length};
    if (name_length > max_name_bytes)
        return fail(error, ZipStatus::name_too_long, record_at, raw);
    WideFields wide{
        load_le32(record + central_uncompressed_bytes),
        load_le32(record + central_compressed_bytes),
        load_le32(record + central_local_header_offset),
        load_le16(record + central_start_disk),
    };
    if (!read_wide_fields(
            directory.subspan(offset + central_bytes + name_length, extra_length), wide
        ))
        return fail(error, ZipStatus::bad_zip64_record, record_at, raw);
    if (wide.start_disk != 0)
        return fail(error, ZipStatus::several_disks, record_at, raw);
    if ((flags & encryption_flags) != 0)
        return fail(error, ZipStatus::encrypted, record_at, raw);
    if (!method_supported(method))
        return fail(error, ZipStatus::unsupported_method, record_at, raw);
    std::string name;
    if ((flags & flag_utf8_name) != 0) {
        if (!valid_utf8(raw))
            return fail(error, ZipStatus::bad_name_encoding, record_at, raw);
        name = std::string(raw);
    } else if (
        const auto spelled =
            unicode_path(directory.subspan(offset + central_bytes + name_length, extra_length), raw)
    ) {
        name = *spelled;
    } else {
        name = from_cp437(raw);
    }
    // MS-DOS and Windows write their own separator.
    if (host == host_ms_dos || host == host_windows_ntfs || host == host_vfat)
        std::replace(name.begin(), name.end(), '\\', '/');
    if (!name_is_safe(name))
        return fail(error, ZipStatus::unsafe_name, record_at, name);
    entry = StreamEntry{};
    entry.method = static_cast<Method>(method);
    entry.crc32 = load_le32(record + central_crc32);
    entry.bytes = wide.bytes;
    entry.compressed_bytes = wide.compressed_bytes;
    entry.local_header_offset = wide.local_header_offset;
    entry.directory = name.back() == '/';
    if (host == host_unix || host == host_macos) {
        entry.unix_mode = load_le32(record + central_external_attributes) >> 16U;
        const uint32_t type = entry.unix_mode & unix_type_mask;
        entry.symbolic_link = type == unix_type_link;
        entry.special_file = type != 0 && type != unix_type_link && type != unix_type_regular &&
                             type != unix_type_directory;
    }
    if (entry.bytes > limits.max_entry_bytes)
        return fail(error, ZipStatus::entry_too_large, record_at, name);
    const bool consistent = entry.method == Method::stored
                                ? entry.compressed_bytes == entry.bytes
                                : entry.compressed_bytes <= UINT64_MAX / max_inflate_ratio &&
                                      entry.bytes <= entry.compressed_bytes * max_inflate_ratio;
    if (!consistent || (entry.directory && entry.bytes != 0))
        return fail(error, ZipStatus::size_mismatch, record_at, name);
    if (raw != name)
        entry.recorded_name = std::string(raw);
    entry.name = std::move(name);
    return true;
}

} // namespace

const std::string& recorded_name_of(const StreamEntry& entry) noexcept {
    return entry.recorded_name.empty() ? entry.name : entry.recorded_name;
}

bool read_stream_directory(
    const SourceHooks& source,
    uint64_t archive_bytes,
    const StreamLimits& limits,
    StreamDirectory& directory,
    ZipError& error
) {
    error = ZipError{};
    directory = StreamDirectory{};
    if (archive_bytes > limits.max_archive_bytes)
        return fail(error, ZipStatus::too_large, 0);
    EndValues end{};
    if (!read_end_records(source, archive_bytes, end, error))
        return false;
    if (end.disk != 0 || end.directory_disk != 0 || end.disk_entry_count != end.entry_count)
        return fail(error, ZipStatus::several_disks, end.directory_end_limit);
    if (end.entry_count > limits.max_entry_count)
        return fail(error, ZipStatus::too_many_entries, end.directory_end_limit);
    if (end.directory_bytes > limits.max_directory_bytes)
        return fail(error, ZipStatus::directory_too_large, end.directory_end_limit);
    if (end.directory_offset > end.directory_end_limit ||
        end.directory_bytes > end.directory_end_limit - end.directory_offset ||
        end.entry_count * central_bytes > end.directory_bytes)
        return fail(error, ZipStatus::truncated, end.directory_end_limit);
    std::vector<uint8_t> records(static_cast<std::size_t>(end.directory_bytes));
    if (!read_bytes(source, end.directory_offset, records))
        return fail(error, ZipStatus::read_failed, end.directory_offset);
    StreamDirectory read{};
    read.zip64 = end.zip64;
    read.entries.reserve(static_cast<std::size_t>(end.entry_count));
    std::vector<uint64_t> record_offsets;
    record_offsets.reserve(static_cast<std::size_t>(end.entry_count));
    std::size_t offset = 0;
    for (uint64_t index = 0; index < end.entry_count; ++index) {
        StreamEntry entry{};
        std::size_t length = 0;
        const uint64_t record_at = end.directory_offset + offset;
        if (!read_record(records, offset, record_at, limits, entry, length, error))
            return false;
        read.total_bytes += entry.bytes;
        read.total_compressed_bytes += entry.compressed_bytes;
        read.entries.push_back(std::move(entry));
        record_offsets.push_back(record_at);
        offset += length;
    }
    if (offset != records.size())
        return fail(error, ZipStatus::bad_central_record, end.directory_offset + offset);
    records = {};
    // Equal names lie side by side once sorted.
    std::vector<std::size_t> order(read.entries.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&read](std::size_t left, std::size_t right) {
        return read.entries[left].name < read.entries[right].name;
    });
    for (std::size_t index = 1; index < order.size(); ++index) {
        const StreamEntry& later = read.entries[std::max(order[index - 1], order[index])];
        if (read.entries[order[index - 1]].name == read.entries[order[index]].name)
            return fail(
                error,
                ZipStatus::duplicate_name,
                record_offsets[std::max(order[index - 1], order[index])],
                later.name
            );
    }
    // Each entry's header, name and data end before the next entry's header,
    // or the directory: no two entries share data.
    std::sort(order.begin(), order.end(), [&read](std::size_t left, std::size_t right) {
        return read.entries[left].local_header_offset < read.entries[right].local_header_offset;
    });
    for (std::size_t index = 0; index < order.size(); ++index) {
        StreamEntry& entry = read.entries[order[index]];
        const uint64_t limit = index + 1 < order.size()
                                   ? read.entries[order[index + 1]].local_header_offset
                                   : end.directory_offset;
        const uint64_t header = local_bytes + recorded_name_of(entry).size();
        if (entry.local_header_offset > limit || limit - entry.local_header_offset < header ||
            limit - entry.local_header_offset - header < entry.compressed_bytes)
            return fail(
                error, ZipStatus::overlapping_entries, entry.local_header_offset, entry.name
            );
        entry.data_limit = limit;
    }
    directory = std::move(read);
    return true;
}

struct EntryStream::State {
    SourceHooks source{};
    StreamEntry entry{};
    uint64_t data_offset{};
    uint64_t input_done{};
    uint64_t output_done{};
    uLong crc{};
    bool finished{};
    Inflater inflater{};
    std::size_t in_position{}; ///< the next byte of `input` to inflate
    std::size_t in_length{};   ///< the bytes `input` holds
    std::array<uint8_t, piece_bytes> input{};
    std::array<uint8_t, piece_bytes> output{};
};

EntryStream::EntryStream() = default;
EntryStream::EntryStream(EntryStream&& other) noexcept = default;
EntryStream& EntryStream::operator=(EntryStream&& other) noexcept = default;
EntryStream::~EntryStream() = default;

bool EntryStream::open(
    const SourceHooks& source, uint64_t archive_bytes, const StreamEntry& entry, ZipError& error
) {
    error = ZipError{};
    state_.reset();
    const uint64_t header = entry.local_header_offset;
    if (header > archive_bytes || archive_bytes - header < local_bytes)
        return fail(error, ZipStatus::truncated, header, entry.name);
    std::array<uint8_t, local_bytes> record{};
    if (!read_bytes(source, header, record))
        return fail(error, ZipStatus::read_failed, header, entry.name);
    if (!signature_at(record, 0, local_signature))
        return fail(error, ZipStatus::bad_local_record, header, entry.name);
    const std::size_t name_length = load_le16(record.data() + local_name_length);
    const std::size_t extra_length = load_le16(record.data() + local_extra_length);
    const uint64_t data = header + local_bytes + name_length + extra_length;
    if (data > archive_bytes)
        return fail(error, ZipStatus::truncated, header, entry.name);
    std::vector<uint8_t> tail(name_length + extra_length);
    if (!read_bytes(source, header + local_bytes, tail))
        return fail(error, ZipStatus::read_failed, header, entry.name);
    const std::string_view name{reinterpret_cast<const char*>(tail.data()), name_length};
    if (name != recorded_name_of(entry))
        return fail(error, ZipStatus::bad_local_record, header, entry.name);
    const uint16_t flags = load_le16(record.data() + local_flags);
    if ((flags & encryption_flags) != 0)
        return fail(error, ZipStatus::encrypted, header, entry.name);
    if (load_le16(record.data() + local_method) != static_cast<uint16_t>(entry.method))
        return fail(error, ZipStatus::bad_local_record, header, entry.name);
    const bool deferred = (flags & flag_data_descriptor) != 0;
    const uint32_t crc = load_le32(record.data() + local_crc32);
    const uint32_t compressed = load_le32(record.data() + local_compressed_bytes);
    const uint32_t bytes = load_le32(record.data() + local_uncompressed_bytes);
    // A local field agrees when it holds the central value, or zero under
    // the data-descriptor flag.
    const auto agrees = [deferred](uint64_t local, uint64_t central) {
        return local == central || (deferred && local == 0);
    };
    if (!agrees(crc, entry.crc32))
        return fail(error, ZipStatus::bad_local_record, header, entry.name);
    if (compressed == zip64_sentinel_32 || bytes == zip64_sentinel_32) {
        // The local 64-bit extra field holds both sizes.
        std::span<const uint8_t> wide;
        if (!find_extra(
                std::span<const uint8_t>(tail).subspan(name_length), zip64_extra_id, wide
            ) ||
            wide.size() < local_zip64_bytes)
            return fail(error, ZipStatus::bad_zip64_record, header, entry.name);
        if (!agrees(load_le64(wide.data()), entry.bytes) ||
            !agrees(load_le64(wide.data() + sizeof(uint64_t)), entry.compressed_bytes))
            return fail(error, ZipStatus::bad_local_record, header, entry.name);
    } else if (!agrees(compressed, entry.compressed_bytes) || !agrees(bytes, entry.bytes)) {
        return fail(error, ZipStatus::bad_local_record, header, entry.name);
    }
    const uint64_t limit = entry.data_limit != 0 ? entry.data_limit : archive_bytes;
    if (data > limit || limit - data < entry.compressed_bytes)
        return fail(error, ZipStatus::overlapping_entries, header, entry.name);
    if (archive_bytes - data < entry.compressed_bytes)
        return fail(error, ZipStatus::truncated, header, entry.name);
    auto state = std::make_unique<State>();
    state->source = source;
    state->entry = entry;
    state->data_offset = data;
    state->crc = crc32(0L, Z_NULL, 0);
    if (entry.method == Method::deflated && !state->inflater.start())
        return fail(error, ZipStatus::inflate_failed, header, entry.name);
    state_ = std::move(state);
    return true;
}

StreamStep EntryStream::step(const SinkHooks& sink, uint64_t input_budget, ZipError& error) {
    if (!state_) {
        fail(error, ZipStatus::read_failed, 0);
        return StreamStep::failed;
    }
    State& state = *state_;
    const StreamEntry& entry = state.entry;
    const auto stop = [&](ZipStatus status) {
        fail(error, status, entry.local_header_offset, entry.name);
        state.finished = true;
        return StreamStep::failed;
    };
    if (state.finished)
        return StreamStep::done;
    const uint64_t budget = std::max<uint64_t>(input_budget, 1);
    const uint64_t output_budget = std::max<uint64_t>(budget, piece_bytes) * 4;
    uint64_t read_now = 0;
    uint64_t produced_now = 0;
    // Reads the next piece of the data, within the step's budget.
    const auto read_piece = [&]() -> bool {
        const uint64_t remaining = entry.compressed_bytes - state.input_done;
        const auto length = static_cast<std::size_t>(
            std::min<uint64_t>({piece_bytes, remaining, budget - read_now})
        );
        if (!read_bytes(
                state.source,
                state.data_offset + state.input_done,
                std::span<uint8_t>(state.input).first(length)
            ))
            return false;
        state.in_position = 0;
        state.in_length = length;
        state.input_done += length;
        read_now += length;
        return true;
    };
    // Hands inflated or stored bytes on, keeping the CRC-32.
    const auto hand_on = [&](std::span<const uint8_t> bytes) -> bool {
        state.crc = crc32(state.crc, bytes.data(), static_cast<uInt>(bytes.size()));
        state.output_done += bytes.size();
        produced_now += bytes.size();
        return sink.write == nullptr || sink.write(sink.context, bytes);
    };
    const auto finish = [&]() {
        if (state.output_done != entry.bytes)
            return stop(ZipStatus::size_mismatch);
        if (static_cast<uint32_t>(state.crc) != entry.crc32)
            return stop(ZipStatus::crc_mismatch);
        state.finished = true;
        if (entry.method == Method::deflated)
            state.inflater.finish();
        return StreamStep::done;
    };
    while (read_now < budget && produced_now < output_budget) {
        if (entry.method == Method::stored) {
            if (state.input_done == entry.compressed_bytes)
                return finish();
            if (!read_piece())
                return stop(ZipStatus::read_failed);
            if (!hand_on(std::span<const uint8_t>(state.input).first(state.in_length)))
                return stop(ZipStatus::write_failed);
            state.in_position = state.in_length;
            continue;
        }
        if (state.in_position == state.in_length && state.input_done < entry.compressed_bytes &&
            !read_piece())
            return stop(ZipStatus::read_failed);
        const auto room =
            static_cast<std::size_t>(std::min<uint64_t>(piece_bytes, output_budget - produced_now));
        const Inflater::Pass pass = state.inflater.pass(
            std::span<const uint8_t>(state.input)
                .subspan(state.in_position, state.in_length - state.in_position),
            std::span<uint8_t>(state.output).first(room)
        );
        state.in_position += pass.consumed;
        if (pass.failed)
            return stop(ZipStatus::inflate_failed);
        if (pass.produced > 0) {
            // Data past the recorded size is never handed on.
            if (pass.produced > entry.bytes - state.output_done)
                return stop(ZipStatus::size_mismatch);
            if (!hand_on(std::span<const uint8_t>(state.output).first(pass.produced)))
                return stop(ZipStatus::write_failed);
        }
        if (pass.ended) {
            // The stream must use all of the recorded data.
            if (state.in_position != state.in_length || state.input_done != entry.compressed_bytes)
                return stop(ZipStatus::size_mismatch);
            return finish();
        }
        if (pass.consumed == 0 && pass.produced == 0) {
            // The data ended before the stream did.
            if (state.in_position == state.in_length && state.input_done == entry.compressed_bytes)
                return stop(ZipStatus::size_mismatch);
            if (state.in_position != state.in_length)
                return stop(ZipStatus::inflate_failed);
        }
    }
    return StreamStep::more;
}

uint64_t EntryStream::input_done() const noexcept {
    return state_ ? state_->input_done : 0;
}

uint64_t EntryStream::output_done() const noexcept {
    return state_ ? state_->output_done : 0;
}

bool read_stream_entry(
    const SourceHooks& source,
    uint64_t archive_bytes,
    const StreamEntry& entry,
    uint64_t most_bytes,
    std::vector<uint8_t>& bytes,
    ZipError& error
) {
    bytes.clear();
    error = ZipError{};
    if (entry.bytes > most_bytes)
        return fail(error, ZipStatus::entry_too_large, entry.local_header_offset, entry.name);
    EntryStream stream;
    if (!stream.open(source, archive_bytes, entry, error))
        return false;
    std::vector<uint8_t> read;
    read.reserve(static_cast<std::size_t>(entry.bytes));
    const SinkHooks sink{&read, [](void* context, std::span<const uint8_t> piece) {
                             auto& into = *static_cast<std::vector<uint8_t>*>(context);
                             into.insert(into.end(), piece.begin(), piece.end());
                             return true;
                         }};
    while (true) {
        const StreamStep step = stream.step(sink, piece_bytes, error);
        if (step == StreamStep::failed)
            return false;
        if (step == StreamStep::done)
            break;
    }
    bytes = std::move(read);
    return true;
}

} // namespace oa::formats::zip
