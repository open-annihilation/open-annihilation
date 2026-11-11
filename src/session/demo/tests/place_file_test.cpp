// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Putting a finished file where it goes, with the rename and the link refused as they are across
// volumes (and on a file system with no links), so that the copy that takes their place is the
// way every file here is moved: the bytes come through to a file made with the permissions of any
// new file, a name that is taken is never replaced unless asked, the next name is taken when one
// is taken while the copy is made, and a copy that fails leaves no file.

#include "oa/session/demo/place_file.hpp"

#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using oa::session::demo::place_file;
using oa::session::demo::PlaceHooks;

/// A folder made for one test and removed when it ends.
struct Scratch {
    fs::path path{oa::test::make_scratch_directory("demo-place-file")};

    Scratch() = default;
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_text(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    OA_CHECK(static_cast<bool>(out));
}

/// Counts the calls the hooks refuse, and says how they refuse them.
struct Refusals {
    int renames{};
    int links{};
    /// What the rename and the link fail with: the error of a move across volumes.
    std::errc reason{std::errc::cross_device_link};
    /// A file the link makes before it fails, as another copy of the game would have made it
    /// between the check for a free name and the link; empty for none.
    fs::path made_by_another{};
};

void refuse_rename(void* context, const fs::path&, const fs::path&, std::error_code& error) {
    auto& refusals = *static_cast<Refusals*>(context);
    ++refusals.renames;
    error = std::make_error_code(refusals.reason);
}

void refuse_link(void* context, const fs::path&, const fs::path&, std::error_code& error) {
    auto& refusals = *static_cast<Refusals*>(context);
    ++refusals.links;
    if (!refusals.made_by_another.empty()) {
        write_text(refusals.made_by_another, "made by another copy of the game");
        refusals.made_by_another.clear();
    }
    error = std::make_error_code(refusals.reason);
}

/// Hooks that refuse both, so that a file can only be copied.
PlaceHooks across_volumes(Refusals& refusals) {
    PlaceHooks hooks;
    hooks.context = &refusals;
    hooks.rename = refuse_rename;
    hooks.link = refuse_link;
    return hooks;
}

/// Tells whether a file can be read and written by its owner, as a recording a player has to open
/// and may delete is.
///
/// @param file the file
/// @return true when it can
bool owner_reads_and_writes(const fs::path& file) {
#if defined(_WIN32)
    return fs::exists(file);
#else
    const fs::perms mode = fs::status(file).permissions();
    return (mode & fs::perms::owner_read) != fs::perms::none &&
           (mode & fs::perms::owner_write) != fs::perms::none;
#endif
}

/// With nothing refused, a file is renamed or linked into place: a replace takes the name, and
/// without one a taken name is kept and the next is taken.
void a_file_is_placed_on_its_own_volume() {
    const Scratch scratch;
    const fs::path destination = scratch.path / "Recordings" / "game.tad";
    fs::path placed;
    std::string error;
    write_text(scratch.path / "first.part", "first");
    OA_CHECK(place_file(scratch.path / "first.part", destination, false, placed, error));
    OA_CHECK(placed == destination && read_text(destination) == "first");
    OA_CHECK(!fs::exists(scratch.path / "first.part"));
    write_text(scratch.path / "second.part", "second");
    OA_CHECK(place_file(scratch.path / "second.part", destination, false, placed, error));
    OA_CHECK(placed == scratch.path / "Recordings" / "game (2).tad");
    OA_CHECK(read_text(destination) == "first" && read_text(placed) == "second");
    write_text(scratch.path / "third.part", "third");
    OA_CHECK(place_file(scratch.path / "third.part", destination, true, placed, error));
    OA_CHECK(placed == destination && read_text(destination) == "third");
    OA_CHECK(!fs::exists(scratch.path / "third.part"));
}

/// Across volumes, a replace copies the bytes over the name that was there, to a file its owner
/// reads and writes, and removes the original.
void a_replace_across_volumes_copies_the_bytes() {
    const Scratch scratch;
    const fs::path destination = scratch.path / "out" / "host.tad";
    write_text(destination, "an older recording, which is longer than the new one");
    write_text(scratch.path / "work.writing", "the new recording");
    Refusals refusals;
    fs::path placed;
    std::string error;
    OA_CHECK(place_file(
        scratch.path / "work.writing", destination, true, placed, error, across_volumes(refusals)
    ));
    OA_CHECK(error.empty());
    OA_CHECK(refusals.renames == 1);
    OA_CHECK(placed == destination);
    OA_CHECK(read_text(destination) == "the new recording");
    OA_CHECK(owner_reads_and_writes(destination));
    OA_CHECK(!fs::exists(scratch.path / "work.writing"));
    // A name that was free is made the same way, in a folder that was not there.
    write_text(scratch.path / "again.writing", "again");
    const fs::path deeper = scratch.path / "not" / "yet" / "there.tad";
    OA_CHECK(place_file(
        scratch.path / "again.writing", deeper, true, placed, error, across_volumes(refusals)
    ));
    OA_CHECK(placed == deeper && read_text(deeper) == "again");
    OA_CHECK(owner_reads_and_writes(deeper));
}

/// Across volumes, a name that is taken is kept, and the copy takes the next free name, each time.
void a_copy_across_volumes_takes_the_next_free_name() {
    const Scratch scratch;
    const fs::path destination = scratch.path / "out" / "host.tad";
    write_text(destination, "kept");
    Refusals refusals;
    const PlaceHooks hooks = across_volumes(refusals);
    fs::path placed;
    std::string error;
    write_text(scratch.path / "one.part", "one");
    OA_CHECK(place_file(scratch.path / "one.part", destination, false, placed, error, hooks));
    OA_CHECK(placed == scratch.path / "out" / "host (2).tad");
    OA_CHECK(read_text(placed) == "one" && owner_reads_and_writes(placed));
    OA_CHECK(!fs::exists(scratch.path / "one.part"));
    write_text(scratch.path / "two.part", "two");
    OA_CHECK(place_file(scratch.path / "two.part", destination, false, placed, error, hooks));
    OA_CHECK(placed == scratch.path / "out" / "host (3).tad");
    OA_CHECK(read_text(placed) == "two");
    // Nothing that was there was replaced, and a free name is the name itself.
    OA_CHECK(read_text(destination) == "kept");
    OA_CHECK(read_text(scratch.path / "out" / "host (2).tad") == "one");
    const fs::path free_name = scratch.path / "out" / "other.tad";
    write_text(scratch.path / "three.part", "three");
    OA_CHECK(place_file(scratch.path / "three.part", free_name, false, placed, error, hooks));
    OA_CHECK(placed == free_name && read_text(free_name) == "three");
    OA_CHECK(refusals.links == 3);
}

/// A file system with no links refuses the link in its own way, and the copy takes over the same.
void a_file_system_with_no_links_is_copied_to() {
    const Scratch scratch;
    const fs::path destination = scratch.path / "out" / "host.tad";
    Refusals refusals;
    refusals.reason = std::errc::operation_not_permitted;
    fs::path placed;
    std::string error;
    write_text(scratch.path / "work.writing", "bytes");
    OA_CHECK(place_file(
        scratch.path / "work.writing", destination, false, placed, error, across_volumes(refusals)
    ));
    OA_CHECK(placed == destination && read_text(destination) == "bytes");
    OA_CHECK(owner_reads_and_writes(destination));
    OA_CHECK(!fs::exists(scratch.path / "work.writing"));
}

/// A name another copy of the game takes while the copy is made is not replaced: the copy is
/// refused there, and the file takes the next name.
void a_name_taken_during_the_copy_is_not_replaced() {
    const Scratch scratch;
    const fs::path destination = scratch.path / "out" / "host.tad";
    fs::create_directories(destination.parent_path());
    Refusals refusals;
    refusals.made_by_another = destination;
    fs::path placed;
    std::string error;
    write_text(scratch.path / "work.writing", "mine");
    OA_CHECK(place_file(
        scratch.path / "work.writing", destination, false, placed, error, across_volumes(refusals)
    ));
    OA_CHECK(placed == scratch.path / "out" / "host (2).tad");
    OA_CHECK(read_text(destination) == "made by another copy of the game");
    OA_CHECK(read_text(placed) == "mine");
}

/// A copy that cannot be made leaves no file at the name, a file that is at the name stays, the
/// original stays, and the reason is given.
void a_copy_that_fails_leaves_no_file() {
    const Scratch scratch;
    Refusals refusals;
    const PlaceHooks hooks = across_volumes(refusals);
    fs::path placed;
    std::string error;
    // The original is not there to be copied.
    const fs::path missing = scratch.path / "missing.part";
    const fs::path free_name = scratch.path / "out" / "free.tad";
    OA_CHECK(!place_file(missing, free_name, false, placed, error, hooks));
    OA_CHECK(!error.empty());
    OA_CHECK(!fs::exists(free_name));
    error.clear();
    OA_CHECK(!place_file(missing, free_name, true, placed, error, hooks));
    OA_CHECK(!error.empty());
    OA_CHECK(!fs::exists(free_name));
    // A name that is taken is kept, and no second name is made.
    const fs::path taken = scratch.path / "out" / "taken.tad";
    write_text(taken, "kept");
    OA_CHECK(!place_file(missing, taken, false, placed, error, hooks));
    OA_CHECK(read_text(taken) == "kept");
    OA_CHECK(!fs::exists(scratch.path / "out" / "taken (2).tad"));
#if !defined(_WIN32)
    // A copy that fails after the file is made: a folder opens for reading, and reading it fails.
    const fs::path folder = scratch.path / "folder.part";
    fs::create_directories(folder);
    error.clear();
    OA_CHECK(!place_file(folder, free_name, false, placed, error, hooks));
    OA_CHECK(!error.empty());
    OA_CHECK(!fs::exists(free_name));
    OA_CHECK(!place_file(folder, free_name, true, placed, error, hooks));
    OA_CHECK(!fs::exists(free_name));
    OA_CHECK(!fs::exists(scratch.path / "out" / "free (2).tad"));
    OA_CHECK(fs::is_directory(folder));
#endif
}

} // namespace

int main() {
    a_file_is_placed_on_its_own_volume();
    a_replace_across_volumes_copies_the_bytes();
    a_copy_across_volumes_takes_the_next_free_name();
    a_file_system_with_no_links_is_copied_to();
    a_name_taken_during_the_copy_is_not_replaced();
    a_copy_that_fails_leaves_no_file();
    return oa::test::check_exit_status();
}
