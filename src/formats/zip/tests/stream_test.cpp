// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The streamed zip reader: entries read in steps of any size, the 64-bit
// extension, a sparse archive past 4 GiB, names in code page 437 and from
// MS-DOS, links marked, every refusal, and malformed archives read with
// nothing allocated beyond fixed bounds.

#include "oa/formats/zip.hpp"
#include "oa/formats/zip/stream.hpp"
#include "oa/test/raw_zip.hpp"
#include "tool_archives.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// The largest single allocation since the count was last reset, in bytes.
std::atomic<std::size_t> g_largest_allocation{0};

} // namespace

void* operator new(std::size_t size) {
    std::size_t seen = g_largest_allocation.load();
    while (size > seen && !g_largest_allocation.compare_exchange_weak(seen, size)) {
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

using namespace oa::formats::zip;
namespace raw = oa::test::raw_zip;
using raw::build_raw_archive;
using raw::RawFile;

int g_failures = 0;

/// Reports a failed check with its line.
///
/// @param condition the checked condition
/// @param what the condition's source text
/// @param line the line of the check
void check(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

/// An archive in memory, read through SourceHooks; reads at or past
/// `fail_from` fail.
struct MemorySource {
    std::span<const uint8_t> bytes{};
    uint64_t fail_from{UINT64_MAX};
};

/// Reads from a MemorySource.
///
/// @param context the source
/// @param offset where to read
/// @param out filled
/// @return true when the bytes lie in the archive and before fail_from
bool read_memory(void* context, uint64_t offset, std::span<uint8_t> out) {
    const auto& source = *static_cast<const MemorySource*>(context);
    if (offset > source.bytes.size() || source.bytes.size() - offset < out.size() ||
        offset + out.size() > source.fail_from)
        return false;
    std::copy_n(
        source.bytes.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin()
    );
    return true;
}

/// A sparse archive: zeros, then `tail` from `tail_offset` on.
struct SparseSource {
    uint64_t tail_offset{};
    std::span<const uint8_t> tail{};
};

/// Reads from a SparseSource.
///
/// @param context the source
/// @param offset where to read
/// @param out filled
/// @return true when the bytes lie in the archive
bool read_sparse(void* context, uint64_t offset, std::span<uint8_t> out) {
    const auto& source = *static_cast<const SparseSource*>(context);
    const uint64_t size = source.tail_offset + source.tail.size();
    if (offset > size || size - offset < out.size())
        return false;
    for (std::size_t index = 0; index < out.size(); ++index) {
        const uint64_t at = offset + index;
        out[index] = at < source.tail_offset ? 0 : source.tail[at - source.tail_offset];
    }
    return true;
}

/// Returns the hooks of a memory source.
///
/// @param source the source
/// @return its hooks
SourceHooks hooks_of(MemorySource& source) {
    return {&source, read_memory};
}

/// Returns the bytes of a text.
///
/// @param text the text
/// @return its bytes
std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

/// Returns deterministic bytes that do not deflate well.
///
/// @param size how many bytes
/// @param seed the generator's starting value
/// @return the bytes
std::vector<uint8_t> noise(std::size_t size, uint32_t seed) {
    std::vector<uint8_t> out(size);
    for (uint8_t& b : out) {
        seed = seed * 1103515245u + 12345u;
        b = static_cast<uint8_t>(seed >> 16);
    }
    return out;
}

/// Reads a directory from memory and returns its status.
///
/// @param bytes the archive
/// @param[out] directory the directory
/// @param limits the limits
/// @return the status
ZipStatus directory_status(
    std::span<const uint8_t> bytes, StreamDirectory& directory, const StreamLimits& limits = {}
) {
    MemorySource source{bytes};
    ZipError error{};
    const bool read =
        read_stream_directory(hooks_of(source), bytes.size(), limits, directory, error);
    CHECK(read == (error.status == ZipStatus::ok));
    if (!read)
        CHECK(directory.entries.empty());
    return error.status;
}

/// Reads a directory's status alone.
///
/// @param bytes the archive
/// @param limits the limits
/// @return the status
ZipStatus directory_status(std::span<const uint8_t> bytes, const StreamLimits& limits = {}) {
    StreamDirectory directory{};
    return directory_status(bytes, directory, limits);
}

/// What one entry's streamed read gave.
struct StreamResult {
    ZipStatus status{ZipStatus::ok};
    std::vector<uint8_t> bytes{};
    uint64_t steps{};
    uint64_t largest_step_output{};
};

/// Reads an entry in steps of a budget, collecting its data.
///
/// @param source the archive's hooks
/// @param archive_bytes its size
/// @param entry the entry
/// @param budget the input bytes each step may read
/// @param refuse_writes the sink refuses the data
/// @return the result
StreamResult stream_entry(
    const SourceHooks& source,
    uint64_t archive_bytes,
    const StreamEntry& entry,
    uint64_t budget,
    bool refuse_writes = false
) {
    StreamResult result{};

    struct Sink {
        StreamResult* result{};
        uint64_t this_step{};
        bool refuse{};
    } sink{&result, 0, refuse_writes};

    const SinkHooks hooks{
        &sink, [](void* context, std::span<const uint8_t> piece) {
            auto& into = *static_cast<Sink*>(context);
            if (into.refuse)
                return false;
            into.result->bytes.insert(into.result->bytes.end(), piece.begin(), piece.end());
            into.this_step += piece.size();
            return true;
        }
    };
    EntryStream stream;
    ZipError error{};
    if (!stream.open(source, archive_bytes, entry, error)) {
        result.status = error.status;
        return result;
    }
    while (true) {
        sink.this_step = 0;
        const uint64_t input_before = stream.input_done();
        const StreamStep step = stream.step(hooks, budget, error);
        ++result.steps;
        CHECK(stream.input_done() - input_before <= std::max<uint64_t>(budget, 1));
        result.largest_step_output = std::max(result.largest_step_output, sink.this_step);
        if (step == StreamStep::failed) {
            result.status = error.status;
            CHECK(error.status != ZipStatus::ok);
            return result;
        }
        if (step == StreamStep::done) {
            CHECK(stream.input_done() == entry.compressed_bytes);
            CHECK(stream.output_done() == entry.bytes);
            return result;
        }
    }
}

/// Reads every entry of an archive in memory, as a whole, and tells whether all read.
///
/// @param bytes the archive
/// @return true when the directory and every entry read
bool read_everything(std::span<const uint8_t> bytes) {
    MemorySource source{bytes};
    StreamDirectory directory{};
    ZipError error{};
    if (!read_stream_directory(hooks_of(source), bytes.size(), {}, directory, error))
        return false;
    for (const StreamEntry& entry : directory.entries) {
        EntryStream stream;
        if (!stream.open(hooks_of(source), bytes.size(), entry, error))
            return false;
        StreamStep step = StreamStep::more;
        while (step == StreamStep::more)
            step = stream.step({}, 4096, error);
        if (step != StreamStep::done)
            return false;
    }
    return true;
}

void test_steps() {
    const std::vector<uint8_t> plain = noise(100'003, 3);
    std::string text;
    for (int line = 0; line < 12'000; ++line)
        text += "line " + std::to_string(line % 97) + " of the profile\n";
    const std::vector<uint8_t> words = bytes_of(text);
    const std::vector<uint8_t> zeros(3'000'000, 0);
    const auto archive = build_raw_archive({
        raw::stored_file("plain.bin", plain),
        raw::deflated_file("words.txt", words),
        raw::deflated_file("zeros.bin", zeros),
        raw::stored_file("empty/", {}),
        raw::stored_file("nothing.txt", {}),
    });
    MemorySource memory{archive.bytes};
    StreamDirectory directory{};
    CHECK(directory_status(archive.bytes, directory) == ZipStatus::ok);
    CHECK(directory.entries.size() == 5);
    CHECK(!directory.zip64);
    CHECK(directory.total_bytes == plain.size() + words.size() + zeros.size());
    if (directory.entries.size() != 5)
        return;
    CHECK(directory.entries[3].directory);
    const std::vector<uint8_t>* expected[] = {&plain, &words, &zeros};
    for (const uint64_t budget : {uint64_t{1}, uint64_t{7}, uint64_t{65'536}}) {
        for (std::size_t index = 0; index < 3; ++index) {
            // Steps of one byte over three megabytes take too long to be worth it.
            if (budget == 1 && index == 2)
                continue;
            const StreamResult result = stream_entry(
                hooks_of(memory), archive.bytes.size(), directory.entries[index], budget
            );
            CHECK(result.status == ZipStatus::ok);
            CHECK(result.bytes == *expected[index]);
            // No step hands on more than four times its budget, or 256 KiB.
            CHECK(result.largest_step_output <= std::max<uint64_t>(budget, 65'536) * 4);
        }
    }
    for (std::size_t index = 3; index < 5; ++index) {
        const StreamResult result =
            stream_entry(hooks_of(memory), archive.bytes.size(), directory.entries[index], 7);
        CHECK(result.status == ZipStatus::ok);
        CHECK(result.bytes.empty());
    }
    // The whole entry into memory, within its bound or refused past it.
    std::vector<uint8_t> whole;
    ZipError error{};
    CHECK(read_stream_entry(
        hooks_of(memory), archive.bytes.size(), directory.entries[1], words.size(), whole, error
    ));
    CHECK(whole == words);
    CHECK(!read_stream_entry(
        hooks_of(memory), archive.bytes.size(), directory.entries[1], words.size() - 1, whole, error
    ));
    CHECK(error.status == ZipStatus::entry_too_large);
    CHECK(whole.empty());
}

void test_zip64() {
    // Small entries written with the 64-bit extension: sentinels in every
    // 32-bit size and offset, the 64-bit extra fields, the 64-bit end record
    // and its locator.
    const std::vector<uint8_t> first = noise(5'000, 9);
    const std::vector<uint8_t> second = bytes_of(std::string(20'000, 'z'));
    RawFile stored = raw::stored_file("first.bin", first);
    stored.zip64 = true;
    RawFile deflated = raw::deflated_file("dir/second.txt", second);
    deflated.zip64 = true;
    raw::ArchiveShape shape{};
    shape.zip64_end = true;
    const auto archive = build_raw_archive({stored, deflated}, shape);
    MemorySource memory{archive.bytes};
    StreamDirectory directory{};
    CHECK(directory_status(archive.bytes, directory) == ZipStatus::ok);
    CHECK(directory.zip64);
    CHECK(directory.entries.size() == 2);
    if (directory.entries.size() == 2) {
        CHECK(directory.entries[1].name == "dir/second.txt");
        CHECK(directory.entries[1].bytes == second.size());
        CHECK(
            stream_entry(hooks_of(memory), archive.bytes.size(), directory.entries[0], 1000)
                .bytes == first
        );
        CHECK(
            stream_entry(hooks_of(memory), archive.bytes.size(), directory.entries[1], 1000)
                .bytes == second
        );
    }

    // The zip command's archive whose local header uses the 64-bit
    // extension, which the in-memory reader refuses.
    const std::span<const uint8_t> tool{test::zip64_archive};
    MemorySource tool_memory{tool};
    CHECK(directory_status(tool, directory) == ZipStatus::ok);
    CHECK(directory.zip64);
    CHECK(directory.entries.size() == 1);
    if (directory.entries.size() == 1) {
        CHECK(directory.entries[0].name == "-");
        const StreamResult read =
            stream_entry(hooks_of(tool_memory), tool.size(), directory.entries[0], 5);
        CHECK(read.status == ZipStatus::ok);
        CHECK(read.bytes.size() == 42);
    }

    // The zip command's other archives read as the in-memory reader reads them.
    for (const std::span<const uint8_t> other :
         {std::span<const uint8_t>(test::tool_archive),
          std::span<const uint8_t>(test::descriptor_archive)}) {
        CentralDirectory whole{};
        ZipError error{};
        CHECK(read_directory(other, whole, error));
        CHECK(directory_status(other, directory) == ZipStatus::ok);
        CHECK(directory.entries.size() == whole.entries.size());
        MemorySource other_memory{other};
        for (std::size_t index = 0;
             index < std::min(directory.entries.size(), whole.entries.size());
             ++index) {
            std::vector<uint8_t> bytes;
            CHECK(read_entry(other, whole.entries[index], bytes, error));
            CHECK(directory.entries[index].name == whole.entries[index].name);
            CHECK(
                stream_entry(hooks_of(other_memory), other.size(), directory.entries[index], 3)
                    .bytes == bytes
            );
        }
    }

    // A sparse archive of 5 GiB: one small entry whose local header lies
    // past 4 GiB, its offset in the 64-bit extra field.
    const std::vector<uint8_t> small = bytes_of("past four gibibytes\n");
    RawFile far = raw::deflated_file("far.txt", small);
    far.zip64 = true;
    raw::ArchiveShape sparse_shape{};
    sparse_shape.zip64_end = true;
    sparse_shape.base_offset = (uint64_t{5} << 30) - 4096;
    const auto sparse = build_raw_archive({far}, sparse_shape);
    SparseSource sparse_source{sparse_shape.base_offset, sparse.bytes};
    const SourceHooks sparse_hooks{&sparse_source, read_sparse};
    const uint64_t sparse_size = sparse_shape.base_offset + sparse.bytes.size();
    ZipError error{};
    CHECK(read_stream_directory(sparse_hooks, sparse_size, {}, directory, error));
    CHECK(directory.entries.size() == 1);
    if (directory.entries.size() == 1) {
        CHECK(directory.entries[0].local_header_offset == sparse_shape.base_offset);
        CHECK(stream_entry(sparse_hooks, sparse_size, directory.entries[0], 64).bytes == small);
    }

    // An entry that declares 64-bit sizes above the limit.
    RawFile huge = raw::deflated_file("huge.bin", bytes_of("x"));
    huge.zip64 = true;
    huge.bytes = uint64_t{5} << 30;
    huge.data.resize(6 << 20);
    CHECK(directory_status(build_raw_archive({huge}, shape).bytes) == ZipStatus::entry_too_large);
}

void test_names() {
    // Code page 437, the format's default without the UTF-8 flag.
    const auto cp437 = build_raw_archive({raw::stored_text("caf\x82 \x9c.txt", "x")});
    StreamDirectory directory{};
    CHECK(directory_status(cp437.bytes, directory) == ZipStatus::ok);
    if (!directory.entries.empty()) {
        CHECK(directory.entries[0].name == "caf\xc3\xa9 \xc2\xa3.txt");
        CHECK(directory.entries[0].recorded_name == "caf\x82 \x9c.txt");
        MemorySource memory{cp437.bytes};
        CHECK(
            stream_entry(hooks_of(memory), cp437.bytes.size(), directory.entries[0], 9).bytes ==
            bytes_of("x")
        );
    }
    // A name the Unicode path extra field spells in UTF-8, while its CRC-32
    // still matches the recorded name.
    RawFile spelled = raw::stored_text("?ber.txt", "y");
    const std::string utf8_name = "\xc3\xbc"
                                  "ber.txt";
    std::vector<uint8_t> field;
    raw::append_le(field, 0x7075, 2);
    raw::append_le(field, 5 + utf8_name.size(), 2);
    field.push_back(1);
    raw::append_le(field, crc32_of(bytes_of("?ber.txt")), 4);
    field.insert(field.end(), utf8_name.begin(), utf8_name.end());
    spelled.central_extra = field;
    CHECK(directory_status(build_raw_archive({spelled}).bytes, directory) == ZipStatus::ok);
    if (!directory.entries.empty())
        CHECK(directory.entries[0].name == utf8_name);
    spelled.name = "?bar.txt";
    CHECK(directory_status(build_raw_archive({spelled}).bytes, directory) == ZipStatus::ok);
    if (!directory.entries.empty())
        CHECK(directory.entries[0].name == "?bar.txt");

    // MS-DOS and Windows separate folders with a backslash; Unix keeps it.
    RawFile dos = raw::stored_text("units\\armcom.fbi", "[UNITINFO]");
    const auto dos_archive = build_raw_archive({dos});
    CHECK(directory_status(dos_archive.bytes, directory) == ZipStatus::ok);
    if (!directory.entries.empty()) {
        CHECK(directory.entries[0].name == "units/armcom.fbi");
        MemorySource memory{dos_archive.bytes};
        CHECK(
            stream_entry(hooks_of(memory), dos_archive.bytes.size(), directory.entries[0], 4)
                .status == ZipStatus::ok
        );
    }
    dos.host = 3;
    CHECK(directory_status(build_raw_archive({dos}).bytes) == ZipStatus::unsafe_name);

    // Links and special files made on Unix or macOS are marked, not refused.
    RawFile link = raw::stored_text("link", "target");
    link.host = 3;
    link.external_attributes = 0120777U << 16U;
    RawFile device = raw::stored_text("device", "");
    device.host = 19;
    device.external_attributes = 0020644U << 16U;
    RawFile plain = raw::stored_text("plain", "p");
    plain.host = 3;
    plain.external_attributes = 0100644U << 16U;
    CHECK(
        directory_status(build_raw_archive({link, device, plain}).bytes, directory) == ZipStatus::ok
    );
    if (directory.entries.size() == 3) {
        CHECK(directory.entries[0].symbolic_link && !directory.entries[0].special_file);
        CHECK(directory.entries[1].special_file && !directory.entries[1].symbolic_link);
        CHECK(!directory.entries[2].symbolic_link && !directory.entries[2].special_file);
        // The recorded type and permission bits are kept as they are.
        CHECK(directory.entries[0].unix_mode == 0120777U);
        CHECK(directory.entries[1].unix_mode == 0020644U);
        CHECK(directory.entries[2].unix_mode == 0100644U);
    }
    // An entry made on MS-DOS records no Unix mode, whatever its attributes hold.
    RawFile dos_moded = raw::stored_text("dos", "d");
    dos_moded.host = 0;
    dos_moded.external_attributes = 0100755U << 16U;
    CHECK(directory_status(build_raw_archive({dos_moded}).bytes, directory) == ZipStatus::ok);
    if (directory.entries.size() == 1)
        CHECK(directory.entries[0].unix_mode == 0);
}

void test_refusals() {
    const auto two =
        build_raw_archive({raw::stored_text("a.txt", "a"), raw::stored_text("b.txt", "b")});
    CHECK(directory_status(bytes_of("")) == ZipStatus::no_end_record);
    CHECK(directory_status(noise(500, 4)) == ZipStatus::no_end_record);
    StreamLimits limits{};
    limits.max_entry_count = 1;
    CHECK(directory_status(two.bytes, limits) == ZipStatus::too_many_entries);
    limits = {};
    limits.max_directory_bytes = 10;
    CHECK(directory_status(two.bytes, limits) == ZipStatus::directory_too_large);
    limits = {};
    limits.max_archive_bytes = 10;
    CHECK(directory_status(two.bytes, limits) == ZipStatus::too_large);

    // A sentinel without the 64-bit records.
    auto sentinel = two.bytes;
    raw::put_le(sentinel, two.end_offset + 16, 0xFFFFFFFFU, 4);
    CHECK(directory_status(sentinel) == ZipStatus::bad_zip64_record);
    // A 64-bit end record that disagrees with the end record's own count.
    raw::ArchiveShape shape{};
    shape.zip64_end = true;
    auto disagree = build_raw_archive({raw::stored_text("a.txt", "a")}, shape);
    raw::put_le(disagree.bytes, disagree.end_offset + 10, 2, 2);
    CHECK(directory_status(disagree.bytes) == ZipStatus::bad_zip64_record);
    // A record with a sentinel size and no 64-bit extra field.
    auto unwidened = two.bytes;
    raw::put_le(unwidened, two.central_offsets[0] + 24, 0xFFFFFFFFU, 4);
    CHECK(directory_status(unwidened) == ZipStatus::bad_zip64_record);

    // Two records naming one local header.
    auto shared = two.bytes;
    raw::put_le(shared, two.central_offsets[1] + 42, two.local_offsets[0], 4);
    CHECK(directory_status(shared) == ZipStatus::overlapping_entries);
    // A local header whose extra field is longer than its room: its data
    // would run into the next entry.
    RawFile padded = raw::stored_text("a.txt", "abc");
    padded.local_extra.assign(8, 0);
    auto longer = build_raw_archive({padded, raw::stored_text("b.txt", "b")});
    StreamDirectory directory{};
    CHECK(directory_status(longer.bytes, directory) == ZipStatus::ok);
    raw::put_le(longer.bytes, longer.local_offsets[0] + 28, 12, 2);
    if (!directory.entries.empty()) {
        MemorySource memory{longer.bytes};
        CHECK(
            stream_entry(hooks_of(memory), longer.bytes.size(), directory.entries[0], 64).status ==
            ZipStatus::overlapping_entries
        );
    }

    RawFile encrypted = raw::stored_text("secret.txt", "s");
    encrypted.flags = 1;
    CHECK(directory_status(build_raw_archive({encrypted}).bytes) == ZipStatus::encrypted);
    RawFile bzip2 = raw::stored_text("packed.bin", "p");
    bzip2.method = 12;
    CHECK(directory_status(build_raw_archive({bzip2}).bytes) == ZipStatus::unsupported_method);
    CHECK(
        directory_status(build_raw_archive({raw::stored_text("../escape.txt", "e")}).bytes) ==
        ZipStatus::unsafe_name
    );
    CHECK(
        directory_status(build_raw_archive({raw::stored_text("same.txt", "1"),
                                            raw::stored_text("same.txt", "2")})
                             .bytes) == ZipStatus::duplicate_name
    );
    RawFile not_utf8 = raw::stored_text("bad\xff.txt", "b");
    not_utf8.flags = 1U << 11U;
    CHECK(directory_status(build_raw_archive({not_utf8}).bytes) == ZipStatus::bad_name_encoding);
    CHECK(
        directory_status(build_raw_archive({raw::stored_text(std::string(513, 'n'), "n")}).bytes) ==
        ZipStatus::name_too_long
    );

    // Data that inflates past its recorded size stops there: nothing past
    // it is handed on.
    RawFile longer_data = raw::deflated_file("long.txt", bytes_of(std::string(100'000, 'l')));
    longer_data.bytes = 50'000;
    const auto long_archive = build_raw_archive({longer_data});
    MemorySource long_memory{long_archive.bytes};
    CHECK(directory_status(long_archive.bytes, directory) == ZipStatus::ok);
    if (!directory.entries.empty()) {
        const StreamResult result = stream_entry(
            hooks_of(long_memory), long_archive.bytes.size(), directory.entries[0], 64
        );
        CHECK(result.status == ZipStatus::size_mismatch);
        CHECK(result.bytes.size() <= 50'000);
    }
    RawFile wrong_crc = raw::stored_text("crc.txt", "checked");
    wrong_crc.crc32 ^= 1;
    const auto crc_archive = build_raw_archive({wrong_crc});
    MemorySource crc_memory{crc_archive.bytes};
    CHECK(directory_status(crc_archive.bytes, directory) == ZipStatus::ok);
    if (!directory.entries.empty()) {
        CHECK(
            stream_entry(hooks_of(crc_memory), crc_archive.bytes.size(), directory.entries[0], 64)
                .status == ZipStatus::crc_mismatch
        );
    }
    // A source that cannot read the data, and a sink that refuses it.
    const std::vector<uint8_t> data = noise(200'000, 2);
    const auto read_archive = build_raw_archive({raw::deflated_file("data.bin", data)});
    CHECK(directory_status(read_archive.bytes, directory) == ZipStatus::ok);
    if (!directory.entries.empty()) {
        MemorySource failing{read_archive.bytes, 100'000};
        CHECK(
            stream_entry(hooks_of(failing), read_archive.bytes.size(), directory.entries[0], 70'000)
                .status == ZipStatus::read_failed
        );
        MemorySource memory{read_archive.bytes};
        CHECK(
            stream_entry(
                hooks_of(memory), read_archive.bytes.size(), directory.entries[0], 64, true
            )
                .status == ZipStatus::write_failed
        );
    }
    MemorySource nothing{read_archive.bytes, 0};
    ZipError error{};
    CHECK(
        !read_stream_directory(hooks_of(nothing), read_archive.bytes.size(), {}, directory, error)
    );
    CHECK(error.status == ZipStatus::read_failed);
    CHECK(!read_stream_directory({}, read_archive.bytes.size(), {}, directory, error));
    CHECK(error.status == ZipStatus::read_failed);
}

void test_status_messages() {
    for (const ZipStatus status :
         {ZipStatus::read_failed,
          ZipStatus::write_failed,
          ZipStatus::directory_too_large,
          ZipStatus::bad_zip64_record,
          ZipStatus::overlapping_entries,
          ZipStatus::bad_name_encoding})
        CHECK(std::string_view(zip_status_message(status)) != "unknown zip status");
}

void test_malformed_sweep() {
    const std::vector<uint8_t> text = bytes_of(std::string(600, 'q') + "tail\n");
    RawFile wide = raw::stored_text("wide.txt", "wide");
    wide.zip64 = true;
    raw::ArchiveShape shape{};
    shape.zip64_end = true;
    const std::vector<std::vector<uint8_t>> archives{
        build_raw_archive({raw::deflated_file("a.txt", text), raw::stored_file("b/", {})}).bytes,
        build_raw_archive({wide, raw::deflated_file("c.txt", text)}, shape).bytes,
        {test::zip64_archive, test::zip64_archive + sizeof test::zip64_archive},
    };
    g_largest_allocation = 0;
    for (const auto& archive : archives) {
        CHECK(read_everything(archive));
        // Every prefix, and every suffix, lacks an end record that fits.
        for (std::size_t size = 0; size < archive.size(); ++size) {
            CHECK(directory_status(std::span{archive}.first(size)) != ZipStatus::ok);
            CHECK(directory_status(std::span{archive}.subspan(size + 1)) != ZipStatus::ok);
        }
    }
    // Random bytes changed under a fixed seed: whatever is read is refused
    // or read whole, and nothing reads past the end.
    uint32_t seed = 54321;
    const auto next = [&seed] {
        seed = seed * 1103515245u + 12345u;
        return seed >> 8;
    };
    for (const auto& archive : archives) {
        for (int round = 0; round < 3000; ++round) {
            std::vector<uint8_t> mutated = archive;
            const uint32_t changes = 1 + next() % 4;
            for (uint32_t change = 0; change < changes; ++change)
                mutated[next() % mutated.size()] = static_cast<uint8_t>(next());
            (void)read_everything(mutated);
        }
    }
    // Nothing the reads allocated grew with what the damaged records claim:
    // the directory, the end record's search and an entry's fixed buffers.
    CHECK(g_largest_allocation.load() <= std::size_t{1} << 20);
}

} // namespace

int main() {
    test_steps();
    test_zip64();
    test_names();
    test_refusals();
    test_status_messages();
    test_malformed_sweep();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("zip stream: all checks passed\n");
    return 0;
}
