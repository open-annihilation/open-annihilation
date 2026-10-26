// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Characterisation of HpiArchive::read_node, which reads a file record by
// node index: a stored entry, LZ77 entries of exactly one chunk and of
// several, a zlib entry just over one chunk, an empty entry, an encrypted
// directory, the index checks, a chunk whose checksum no longer matches, and
// entries whose claimed size the archive cannot hold; and ranges read
// through a chunk table read once.
// Archives are written by the engine's own writer into a temporary
// directory.

#include "oa/formats/hpi.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

int failures = 0;

/// Records a failed check.
///
/// @param line source line of the check
/// @param what text of the failed condition
void report_failure(int line, const char* what) {
    std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, line, what);
    ++failures;
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            report_failure(__LINE__, #condition);                                                  \
        }                                                                                          \
    } while (false)

/// Opens an archive file the test wrote, throwing when it is refused.
///
/// @param path the archive
/// @return the archive
oa::HpiArchive opened(const fs::path& path) {
    auto archive = oa::open_hpi_file(path);
    if (!archive.ok())
        throw std::runtime_error(archive.error.message);
    return std::move(*archive.value);
}

// Several 64 KiB chunks, exactly one, and a little over one.
constexpr std::size_t three_chunks = 2 * oa::formats::hpi::BlockBytes + 12345;
constexpr std::size_t one_full_chunk = oa::formats::hpi::BlockBytes;
constexpr std::size_t just_over_one_chunk = oa::formats::hpi::BlockBytes + 1;
constexpr uint8_t encrypting_header_key = 0x7D;
constexpr std::string_view chunk_marker = "SQSH";
// A payload byte some way into the first chunk, past its header, and the
// bits flipped in it.
constexpr std::size_t corrupted_payload_byte = oa::formats::hpi::SQSHHeaderSize + 7;
constexpr uint8_t corrupting_mask = 0x5A;

// Linear congruential step used to fill test entries.
constexpr uint32_t pattern_multiplier = 1103515245u;
constexpr uint32_t pattern_increment = 12345u;
constexpr uint32_t pattern_seed = 0x2F6B2A1Du;

/// Builds compressible bytes: a low-entropy pseudo-random pattern.
///
/// @param size length in bytes
/// @return the bytes
Bytes pattern_bytes(std::size_t size) {
    Bytes bytes(size);
    uint32_t state = pattern_seed;
    for (auto& byte : bytes) {
        state = state * pattern_multiplier + pattern_increment;
        byte = static_cast<uint8_t>((state >> 16) & 0x1F);
    }
    return bytes;
}

// A scratch directory removed when the test ends.
class TempDir {
  public:

    /// Creates a uniquely named directory under the system temporary directory.
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / ("oa-hpi-read-node-" + std::to_string(stamp));
        fs::create_directories(path_);
    }

    /// Removes the directory and everything in it.
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    /// Writes a file into the directory.
    ///
    /// @param name file name
    /// @param bytes file contents
    /// @return the file's path
    fs::path write(const std::string& name, const Bytes& bytes) const {
        const fs::path target = path_ / name;
        std::ofstream stream(target, std::ios::binary);
        stream.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
        return target;
    }

  private:

    fs::path path_;
};

/// Returns the entries every archive in this test holds.
///
/// @return the files, one per compression kind and size
std::vector<oa::HpiWriteFile> sample_files() {
    return {
        {"readme.txt",
         Bytes{'h', 'e', 'l', 'l', 'o'},
         static_cast<uint8_t>(oa::formats::hpi::CompressionNone)},
        {"data/lz77.bin",
         pattern_bytes(three_chunks),
         static_cast<uint8_t>(oa::formats::hpi::CompressionLZ77)},
        {"data/zlib.bin",
         pattern_bytes(just_over_one_chunk),
         static_cast<uint8_t>(oa::formats::hpi::CompressionZLib)},
        {"data/empty.bin", Bytes{}, static_cast<uint8_t>(oa::formats::hpi::CompressionLZ77)},
        {"data/single.bin",
         pattern_bytes(one_full_chunk),
         static_cast<uint8_t>(oa::formats::hpi::CompressionLZ77)},
    };
}

/// Reports whether read_node and read_node_range refuse an index as no file.
///
/// @param archive archive to read
/// @param index node index
/// @return true when both reads are refused
bool refuses(const oa::HpiArchive& archive, uint32_t index) {
    Bytes range(1);
    return archive.read_node(index).error.code == oa::base::bytes::DecodeCode::out_of_range &&
           archive.read_node_range(index, 0, range).error.code ==
               oa::base::bytes::DecodeCode::out_of_range;
}

/// Checks that ranges read through a chunk table read once match the whole
/// entry, and that a table is refused for a stored entry or another entry.
///
/// @param archive archive to read
/// @param files the entries it holds
void check_chunk_tables(const oa::HpiArchive& archive, const std::vector<oa::HpiWriteFile>& files) {
    const auto stored = archive.lookup("readme.txt");
    CHECK(
        stored &&
        archive.read_chunk_table(*stored).error.code == oa::base::bytes::DecodeCode::out_of_range
    );
    CHECK(archive.read_chunk_table(0).error.code == oa::base::bytes::DecodeCode::out_of_range);
    const auto single = archive.lookup("data/single.bin");
    const auto single_table = archive.read_chunk_table(single.value_or(0));
    CHECK(single_table.ok() && single_table.value->offsets.size() == 2);
    for (const auto& file : files) {
        const auto index = archive.lookup(file.path);
        if (!index || file.compression == oa::formats::hpi::CompressionNone)
            continue;
        const auto table = archive.read_chunk_table(*index);
        CHECK(table.ok());
        if (!table.ok())
            continue;
        const std::size_t chunks =
            (file.bytes.size() + oa::formats::hpi::BlockBytes - 1) / oa::formats::hpi::BlockBytes;
        CHECK(table.value->offsets.size() == chunks + 1);
        // Ranges that start inside one block and end in the next.
        constexpr std::size_t step = oa::formats::hpi::BlockBytes / 3 + 7;
        Bytes joined;
        for (std::size_t at = 0; at < file.bytes.size(); at += step) {
            Bytes part(step);
            const auto read =
                archive.read_node_range(*index, static_cast<uint32_t>(at), part, *table.value);
            CHECK(read.ok());
            if (!read.ok())
                break;
            part.resize(*read.value);
            joined.insert(joined.end(), part.begin(), part.end());
        }
        CHECK(joined == file.bytes);
        if (single && *single != *index && single_table.ok() && !file.bytes.empty()) {
            Bytes part(1);
            CHECK(
                archive.read_node_range(*index, 0, part, *single_table.value).error.code ==
                oa::base::bytes::DecodeCode::out_of_range
            );
        }
    }
}

/// Checks that every file record reads back as written and that other indices are refused.
///
/// @param path archive to open
void check_round_trip(const fs::path& path) {
    const oa::HpiArchive archive = opened(path);
    const auto files = sample_files();
    int32_t files_read = 0;
    for (const auto& file : files) {
        const auto index = archive.lookup(file.path);
        CHECK(index.has_value());
        if (!index) {
            continue;
        }
        const auto read = archive.read_node(*index);
        CHECK(read.ok());
        if (!read.ok()) {
            continue;
        }
        const Bytes& bytes = *read.value;
        CHECK(bytes == file.bytes);
        CHECK(bytes.size() == archive.nodes()[*index].size);
        CHECK(archive.read(file.path).value == bytes);
        ++files_read;
    }
    CHECK(files_read == static_cast<int32_t>(files.size()));
    const auto directory = archive.lookup("data");
    CHECK(directory.has_value() && archive.nodes()[*directory].directory());
    CHECK(refuses(archive, 0));
    if (directory) {
        CHECK(refuses(archive, *directory));
    }
    CHECK(refuses(archive, static_cast<uint32_t>(archive.nodes().size())));
    check_chunk_tables(archive, files);
}

/// Checks read_node over plain and encrypted archives.
///
/// @param scratch directory the archives are written into
void test_round_trips(const TempDir& scratch) {
    const auto files = sample_files();
    check_round_trip(scratch.write("plain.hpi", oa::write_hpi(files)));

    oa::HpiWriteOptions encrypted;
    encrypted.header_key = encrypting_header_key;
    encrypted.scramble_chunks = false;
    check_round_trip(scratch.write("encrypted.hpi", oa::write_hpi(files, encrypted)));
}

/// Checks that a chunk whose checksum no longer matches its payload fails the read.
///
/// @param scratch directory the archive is written into
void test_corrupt_chunk(const TempDir& scratch) {
    Bytes bytes = oa::write_hpi(sample_files());
    // The stored entry has no chunks, so the first marker opens lz77.bin.
    const std::string_view view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const std::size_t marker = view.find(chunk_marker);
    CHECK(marker != std::string_view::npos);
    if (marker == std::string_view::npos) {
        return;
    }
    bytes[marker + corrupted_payload_byte] ^= corrupting_mask;
    const oa::HpiArchive archive = opened(scratch.write("corrupt.hpi", bytes));
    const auto corrupted = archive.lookup("data/lz77.bin");
    const auto readme = archive.lookup("readme.txt");
    const auto zlib = archive.lookup("data/zlib.bin");
    CHECK(corrupted.has_value() && readme.has_value() && zlib.has_value());
    if (!corrupted || !readme || !zlib) {
        return;
    }
    // The chunk's error carries its SQUASHERR_* name, its status and the
    // chunk's offset in the archive.
    const auto failed = archive.read_node(*corrupted);
    CHECK(!failed.ok() && failed.error.code == oa::base::bytes::DecodeCode::malformed);
    CHECK(std::string_view(failed.error.message) == "SQUASHERR_BADCHECKSUM");
    CHECK(
        failed.error.detail == static_cast<uint16_t>(oa::formats::hpi::SquashStatus::bad_checksum)
    );
    CHECK(failed.error.offset == marker);
    // Other entries still read.
    CHECK(archive.read_node(*readme).value.value_or(Bytes{}).size() == 5);
    CHECK(archive.read_node(*zlib).value == pattern_bytes(just_over_one_chunk));
}

/// Returns where an entry's 9-byte file record lies in an archive written
/// without a header key: the entry's data offset, size and compression byte.
///
/// @param bytes the archive
/// @param node the entry as the reader resolved it
/// @return the record's offset, or bytes.size() when it is not found
std::size_t file_record_at(const Bytes& bytes, const oa::ArchiveNode& node) {
    const Bytes record{
        static_cast<uint8_t>(node.data_offset),
        static_cast<uint8_t>(node.data_offset >> 8),
        static_cast<uint8_t>(node.data_offset >> 16),
        static_cast<uint8_t>(node.data_offset >> 24),
        static_cast<uint8_t>(node.size),
        static_cast<uint8_t>(node.size >> 8),
        static_cast<uint8_t>(node.size >> 16),
        static_cast<uint8_t>(node.size >> 24),
        node.compression,
    };
    const auto found = std::search(bytes.begin(), bytes.end(), record.begin(), record.end());
    return static_cast<std::size_t>(found - bytes.begin());
}

/// Writes a 32-bit little-endian value into a byte buffer.
///
/// @param[in,out] bytes buffer written
/// @param at offset of the first byte
/// @param value value written
void put32(Bytes& bytes, std::size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[at + i] = static_cast<uint8_t>(value >> (8U * i));
}

/// Reports whether read_node fails with a code and a message containing `containing`.
///
/// @param archive archive to read
/// @param index node index
/// @param code the error code expected
/// @param containing text the error's message holds
/// @return true when the read fails so
bool fails_with(
    const oa::HpiArchive& archive,
    uint32_t index,
    oa::base::bytes::DecodeCode code,
    std::string_view containing
) {
    const auto read = archive.read_node(index);
    return !read.ok() && read.error.code == code &&
           std::string_view(read.error.message).find(containing) != std::string_view::npos;
}

/// Offset of the size field in a 9-byte file record.
constexpr std::size_t record_size_field = 4;
/// Size a damaged record may claim: 4 GiB less one byte.
constexpr uint32_t largest_claimed_size = 0xFFFF'FFFFu;
/// A compressed size under the entry limit whose chunks cannot fit in a
/// small archive: 8,176 chunks need at least 188,048 bytes.
constexpr uint32_t unfitting_compressed_size = 0x1FF0'0000u;
/// A stored entry larger than any buffer a file stream keeps.
constexpr std::size_t large_stored_entry = 4 << 20;

/// Checks that an entry claiming more than the archive holds fails before
/// its buffer is allocated, while ranged reads keep returning what is there.
///
/// @param scratch directory the archives are written into
void test_claimed_sizes(const TempDir& scratch) {
    const Bytes original = oa::write_hpi(sample_files());
    const oa::HpiArchive plain = opened(scratch.write("sizes.hpi", original));
    const auto readme = plain.lookup("readme.txt");
    const auto lz77 = plain.lookup("data/lz77.bin");
    CHECK(readme.has_value() && lz77.has_value());
    if (!readme || !lz77) {
        return;
    }
    const oa::ArchiveNode stored = plain.nodes()[*readme];
    const oa::ArchiveNode chunked = plain.nodes()[*lz77];
    const std::size_t stored_record = file_record_at(original, stored);
    const std::size_t chunked_record = file_record_at(original, chunked);
    CHECK(stored_record < original.size() && chunked_record < original.size());
    if (stored_record >= original.size() || chunked_record >= original.size()) {
        return;
    }

    // A stored entry claiming 4 GiB is refused by the entry limit.
    Bytes huge = original;
    put32(huge, stored_record + record_size_field, largest_claimed_size);
    const oa::HpiArchive huge_archive = opened(scratch.write("huge.hpi", huge));
    CHECK(fails_with(
        huge_archive, *readme, oa::base::bytes::DecodeCode::limit_exceeded, "entry size limit"
    ));

    // A stored entry one byte longer than the rest of the archive is refused
    // as a whole, but a ranged read returns the bytes that are there.
    Bytes past_end = original;
    const auto rest = static_cast<uint32_t>(original.size() - stored.data_offset);
    put32(past_end, stored_record + record_size_field, rest + 1);
    const oa::HpiArchive past_end_archive = opened(scratch.write("past-end.hpi", past_end));
    CHECK(fails_with(
        past_end_archive,
        *readme,
        oa::base::bytes::DecodeCode::truncated,
        "past the end of the archive"
    ));
    Bytes range(rest + 1);
    CHECK(past_end_archive.read_node_range(*readme, 0, range).value == rest);

    // A compressed entry one byte over the entry limit is refused by it.
    Bytes over_limit = original;
    put32(
        over_limit,
        chunked_record + record_size_field,
        static_cast<uint32_t>(oa::formats::hpi::EntryByteLimit + 1)
    );
    const oa::HpiArchive over_limit_archive = opened(scratch.write("over-limit.hpi", over_limit));
    CHECK(fails_with(
        over_limit_archive, *lz77, oa::base::bytes::DecodeCode::limit_exceeded, "entry size limit"
    ));

    // A compressed entry of a small archive whose chunks cannot all fit is
    // refused too.
    const std::vector<oa::HpiWriteFile> small_files{
        {"small.bin", pattern_bytes(100), static_cast<uint8_t>(oa::formats::hpi::CompressionLZ77)}
    };
    Bytes unfitting = oa::write_hpi(small_files);
    const oa::HpiArchive small = opened(scratch.write("small.hpi", unfitting));
    const auto small_entry = small.lookup("small.bin");
    CHECK(small_entry.has_value() && unfitting.size() < std::size_t{188'048});
    if (!small_entry) {
        return;
    }
    const std::size_t small_record = file_record_at(unfitting, small.nodes()[*small_entry]);
    CHECK(small_record < unfitting.size());
    if (small_record >= unfitting.size()) {
        return;
    }
    put32(unfitting, small_record + record_size_field, unfitting_compressed_size);
    const oa::HpiArchive unfitting_archive = opened(scratch.write("unfitting.hpi", unfitting));
    CHECK(fails_with(
        unfitting_archive,
        *small_entry,
        oa::base::bytes::DecodeCode::truncated,
        "past the end of the archive"
    ));

    // An archive cut short after it was opened: a large stored entry now
    // reads short, which a whole read reports and a ranged read returns. The
    // entry is large so that its bytes are read from the file, not from what
    // the stream kept from opening the archive.
    const Bytes large = pattern_bytes(large_stored_entry);
    const std::vector<oa::HpiWriteFile> large_files{
        {"large.bin", large, static_cast<uint8_t>(oa::formats::hpi::CompressionNone)}
    };
    const auto cut_path = scratch.write("cut.hpi", oa::write_hpi(large_files));
    const oa::HpiArchive cut = opened(cut_path);
    const auto entry = cut.lookup("large.bin");
    CHECK(entry.has_value());
    if (!entry) {
        return;
    }
    const uint32_t data_offset = cut.nodes()[*entry].data_offset;
    std::error_code resize_error;
    fs::resize_file(cut_path, data_offset + large_stored_entry / 2, resize_error);
    if (!resize_error) {
        CHECK(
            fails_with(cut, *entry, oa::base::bytes::DecodeCode::truncated, "truncated HPI entry")
        );
        Bytes partial(large_stored_entry);
        const uint32_t got = cut.read_node_range(*entry, 0, partial).value.value_or(0);
        CHECK(got > 0 && got < large_stored_entry);
        CHECK(got > 0 && std::equal(large.begin(), large.begin() + got, partial.begin()));
    }
}

} // namespace

int main() {
    const TempDir scratch;
    test_round_trips(scratch);
    test_corrupt_chunk(scratch);
    test_claimed_sizes(scratch);
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
