// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Copying a file's bytes into a new file: the bytes and a readable file come through, a taken
// name is kept or replaced as asked, the source is left as it was, and a copy that fails leaves
// no file at the destination.

#include "oa/platform/file_copy.hpp"

#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using oa::platform::CopyOnto;
using oa::platform::copy_file_contents;

/// A folder made for one test and removed when it ends.
struct Scratch {
    fs::path path{oa::test::make_scratch_directory("platform-file-copy")};

    Scratch() = default;
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

/// Makes bytes that no chunk of a copy repeats: every value, and runs of zeros.
///
/// @param count how many bytes
/// @return the bytes
std::vector<char> sample_bytes(std::size_t count) {
    std::vector<char> bytes(count);
    uint32_t state = 12345;
    for (std::size_t index = 0; index < count; ++index) {
        state = state * 1664525U + 1013904223U;
        bytes[index] = (index / 7000) % 3 == 0 ? char(0) : static_cast<char>(state >> 24);
    }
    return bytes;
}

void write_bytes(const fs::path& file, const std::vector<char>& bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    OA_CHECK(static_cast<bool>(out));
}

void write_text(const fs::path& file, std::string_view text) {
    write_bytes(file, std::vector<char>(text.begin(), text.end()));
}

std::vector<char> read_bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string read_text(const fs::path& file) {
    const std::vector<char> bytes = read_bytes(file);
    return {bytes.begin(), bytes.end()};
}

/// Tells whether a copy was refused.
///
/// @param result what copy_file_contents returned
/// @return true when it carries an error
bool failed(const std::error_code& result) {
    return static_cast<bool>(result);
}

/// A copy across several chunks comes through whole, to a name that is free, and the source stays.
void bytes_come_through_whole() {
    const Scratch scratch;
    const fs::path from = scratch.path / "source.bin";
    const fs::path to = scratch.path / "made" / "copy.bin";
    fs::create_directories(to.parent_path());
    // More than one chunk, and not a multiple of one.
    const std::vector<char> bytes = sample_bytes(5 * 32 * 1024 + 1234);
    write_bytes(from, bytes);
    OA_CHECK(!failed(copy_file_contents(from, to, CopyOnto::must_be_new)));
    OA_CHECK(read_bytes(to) == bytes);
    OA_CHECK(read_bytes(from) == bytes);
    // An empty file is a file, too.
    const fs::path empty = scratch.path / "empty.bin";
    write_text(empty, "");
    OA_CHECK(!failed(
        copy_file_contents(empty, scratch.path / "made" / "empty.bin", CopyOnto::must_be_new)
    ));
    OA_CHECK(fs::exists(scratch.path / "made" / "empty.bin"));
    OA_CHECK(fs::file_size(scratch.path / "made" / "empty.bin") == 0);
}

/// A name outside ASCII is made and read back.
void a_name_outside_ascii_is_copied() {
    const Scratch scratch;
    const fs::path from = scratch.path / fs::path(std::u8string(u8"source-ñ.tad"));
    const fs::path to = scratch.path / fs::path(std::u8string(u8"copy-ñ.tad"));
    write_text(from, "carried");
    OA_CHECK(!failed(copy_file_contents(from, to, CopyOnto::must_be_new)));
    OA_CHECK(read_text(to) == "carried");
}

#if !defined(_WIN32)

/// The copy has the permissions of any new file, not the source's: a source its owner cannot
/// write, as a read-only drive may report, still gives a file its owner reads and writes.
void the_copy_is_made_with_the_permissions_of_a_new_file() {
    const Scratch scratch;
    const fs::path from = scratch.path / "source.bin";
    const fs::path to = scratch.path / "copy.bin";
    write_text(from, "bytes");
    std::error_code unchanged;
    fs::permissions(from, fs::perms::owner_read, fs::perm_options::replace, unchanged);
    if (unchanged)
        return; // a folder that takes no permissions has no source mode to leave behind
    OA_CHECK(!failed(copy_file_contents(from, to, CopyOnto::must_be_new)));
    const fs::perms source_mode = fs::status(from).permissions();
    const fs::perms made_mode = fs::status(to).permissions();
    OA_CHECK((source_mode & fs::perms::owner_write) == fs::perms::none);
    OA_CHECK((made_mode & fs::perms::owner_read) != fs::perms::none);
    OA_CHECK((made_mode & fs::perms::owner_write) != fs::perms::none);
    OA_CHECK(read_text(to) == "bytes");
    fs::permissions(from, fs::perms::owner_all, fs::perm_options::replace, unchanged);
}

#endif

/// A name that is taken is refused, and what is there is left as it was.
void a_taken_name_is_refused() {
    const Scratch scratch;
    const fs::path from = scratch.path / "source.bin";
    const fs::path to = scratch.path / "taken.bin";
    write_text(from, "new bytes");
    write_text(to, "the file that was there first");
    OA_CHECK(copy_file_contents(from, to, CopyOnto::must_be_new) == std::errc::file_exists);
    OA_CHECK(read_text(to) == "the file that was there first");
    OA_CHECK(read_text(from) == "new bytes");
}

/// A folder where the name is wanted is a taken name, too, and a replace does not remove it.
void a_folder_at_the_name_is_refused() {
    const Scratch scratch;
    const fs::path from = scratch.path / "source.bin";
    const fs::path to = scratch.path / "folder";
    write_text(from, "bytes");
    fs::create_directories(to);
    OA_CHECK(copy_file_contents(from, to, CopyOnto::must_be_new) == std::errc::file_exists);
    OA_CHECK(fs::is_directory(to));
    OA_CHECK(failed(copy_file_contents(from, to, CopyOnto::replace)));
    OA_CHECK(fs::is_directory(to));
}

/// A name that is taken is written over whole when asked: a shorter file leaves no tail of the old.
void a_taken_name_is_replaced_when_asked() {
    const Scratch scratch;
    const fs::path from = scratch.path / "source.bin";
    const fs::path to = scratch.path / "taken.bin";
    write_text(from, "short");
    write_text(to, "a much longer file that was there first, with a tail");
    OA_CHECK(!failed(copy_file_contents(from, to, CopyOnto::replace)));
    OA_CHECK(read_text(to) == "short");
    // A name that is free is made, the same way.
    const fs::path free_name = scratch.path / "free.bin";
    OA_CHECK(!failed(copy_file_contents(from, free_name, CopyOnto::replace)));
    OA_CHECK(read_text(free_name) == "short");
}

/// A file is never copied onto itself: that would empty it before it was read.
void a_file_is_not_copied_onto_itself() {
    const Scratch scratch;
    const fs::path file = scratch.path / "only.bin";
    write_text(file, "these bytes stay");
    OA_CHECK(copy_file_contents(file, file, CopyOnto::replace) == std::errc::file_exists);
    OA_CHECK(copy_file_contents(file, file, CopyOnto::must_be_new) == std::errc::file_exists);
    OA_CHECK(read_text(file) == "these bytes stay");
}

/// A copy that cannot start leaves no file, and a file that was at the name stays.
void a_copy_that_cannot_start_leaves_no_file() {
    const Scratch scratch;
    const fs::path missing = scratch.path / "missing.bin";
    const fs::path to = scratch.path / "copy.bin";
    OA_CHECK(failed(copy_file_contents(missing, to, CopyOnto::must_be_new)));
    OA_CHECK(!fs::exists(to));
    OA_CHECK(failed(copy_file_contents(missing, to, CopyOnto::replace)));
    OA_CHECK(!fs::exists(to));
    // A folder that is not there for the destination is not made.
    const fs::path from = scratch.path / "source.bin";
    write_text(from, "bytes");
    const fs::path no_folder = scratch.path / "no-such-folder" / "copy.bin";
    OA_CHECK(failed(copy_file_contents(from, no_folder, CopyOnto::must_be_new)));
    OA_CHECK(failed(copy_file_contents(from, no_folder, CopyOnto::replace)));
    OA_CHECK(!fs::exists(no_folder.parent_path()));
    // A file that was at the name is not touched by a copy that never began.
    write_text(to, "kept");
    OA_CHECK(failed(copy_file_contents(missing, to, CopyOnto::replace)));
    OA_CHECK(read_text(to) == "kept");
}

#if !defined(_WIN32)

/// A copy that fails once the destination is made removes it, for a free name and for one that
/// was taken and written over: a folder opens for reading, and reading it fails.
void a_copy_that_fails_part_way_leaves_no_file() {
    const Scratch scratch;
    const fs::path source_folder = scratch.path / "source-folder";
    fs::create_directories(source_folder);
    const fs::path to = scratch.path / "copy.bin";
    OA_CHECK(failed(copy_file_contents(source_folder, to, CopyOnto::must_be_new)));
    OA_CHECK(!fs::exists(to));
    OA_CHECK(failed(copy_file_contents(source_folder, to, CopyOnto::replace)));
    OA_CHECK(!fs::exists(to));
    // A file written over and then lost is no better than the partial file: neither is kept.
    write_text(to, "the old bytes");
    OA_CHECK(failed(copy_file_contents(source_folder, to, CopyOnto::replace)));
    OA_CHECK(!fs::exists(to));
    // The source is still there, whole.
    OA_CHECK(fs::is_directory(source_folder));
}

#endif

} // namespace

int main() {
    bytes_come_through_whole();
    a_name_outside_ascii_is_copied();
#if !defined(_WIN32)
    the_copy_is_made_with_the_permissions_of_a_new_file();
#endif
    a_taken_name_is_refused();
    a_folder_at_the_name_is_refused();
    a_taken_name_is_replaced_when_asked();
    a_file_is_not_copied_onto_itself();
    a_copy_that_cannot_start_leaves_no_file();
#if !defined(_WIN32)
    a_copy_that_fails_part_way_leaves_no_file();
#endif
    return oa::test::check_exit_status();
}
