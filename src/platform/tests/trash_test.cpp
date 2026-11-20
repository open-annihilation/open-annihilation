// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Moving a file in a scratch folder to the system's trash, and finding it
// gone from the folder. Where the system cannot hold the case, the test
// skips. It mounts nothing, and it removes the trash's copy when the system
// says where that copy is.

#include "oa/platform/files.hpp"
#include "oa/platform/trash.hpp"
#include "oa/test/check.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#if defined(__linux__)
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

/// A folder removed when the test ends.
struct Scratch {
    fs::path path{};

    Scratch() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("oa-r15-trash-" + std::to_string(tick));
        fs::create_directories(path);
    }

    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void write_text(const fs::path& file, std::string_view text) {
    std::ofstream output(file, std::ios::binary);
    OA_CHECK(static_cast<bool>(output));
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_text(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

#if defined(__linux__)

/// Names the device a file or folder is on.
///
/// @param path the file or folder
/// @return the device number; 0 when it cannot be told
uint64_t device_of(const fs::path& path) {
    struct stat status{};
    return ::stat(path.c_str(), &status) == 0 ? static_cast<uint64_t>(status.st_dev) : 0;
}

/// A file on another device is copied into the trash, and gone from where it was. The trash is
/// put in the scratch folder; the file is made in the shared memory folder, which is a device of
/// its own where there is one. Where there is not, nothing is checked.
void a_file_on_another_device_is_copied_into_the_trash(const fs::path& scratch) {
    const fs::path other_device = "/dev/shm";
    std::error_code failure;
    if (!fs::is_directory(other_device, failure) || device_of(other_device) == 0 ||
        device_of(other_device) == device_of(scratch))
        return;
    const fs::path data_home = scratch / "data-home";
    fs::create_directories(data_home);
    if (::setenv("XDG_DATA_HOME", data_home.c_str(), 1) != 0)
        return;
    const fs::path file = other_device / ("oa-trash-" + std::to_string(::getpid()) + ".oademo");
    write_text(file, "a replay on another device\n");
    // A file nobody else can read, so that it is plain which permissions the copy has.
    fs::permissions(
        file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, failure
    );
    const oa::platform::TrashMove moved = oa::platform::move_to_trash(file);
    OA_CHECK(moved.moved);
    OA_CHECK(!fs::exists(file, failure));
    OA_CHECK(device_of(moved.kept_at) == device_of(scratch));
    OA_CHECK(read_text(moved.kept_at) == "a replay on another device\n");
    const fs::perms kept = fs::status(moved.kept_at, failure).permissions();
    OA_CHECK((kept & fs::perms::owner_read) != fs::perms::none);
    OA_CHECK((kept & fs::perms::owner_write) != fs::perms::none);
    fs::remove(file, failure);
    ::unsetenv("XDG_DATA_HOME");
}

#endif

} // namespace

int main() {
    const Scratch scratch;
#if defined(__linux__)
    a_file_on_another_device_is_copied_into_the_trash(scratch.path);
#endif
    const fs::path file = scratch.path / "ridge-replay.oademo";
    write_text(file, "ridge replay\n");
    const oa::platform::TrashMove moved = oa::platform::move_to_trash(file);
    if (!moved.moved) {
        std::cout << "platform trash: skipped (" << moved.error << ")\n";
        return 77;
    }
    std::error_code failure;
    OA_CHECK(!fs::exists(file, failure));
    if (oa::platform::system_has_trash())
        OA_CHECK(moved.place == oa::platform::TrashPlace::trash);
    else
        OA_CHECK(moved.place == oa::platform::TrashPlace::deleted);
    if (!moved.kept_at.empty()) {
        OA_CHECK(read_text(moved.kept_at) == "ridge replay\n");
        fs::remove(moved.kept_at, failure);
    }
    if (!moved.note_at.empty())
        fs::remove(moved.note_at, failure);
    const oa::platform::TrashMove missing =
        oa::platform::move_to_trash(scratch.path / "absent.oademo");
    OA_CHECK(!missing.moved);
    OA_CHECK(fs::exists(scratch.path, failure));
    return oa::test::check_exit_status();
}
