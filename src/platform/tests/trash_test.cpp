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

} // namespace

int main() {
    const Scratch scratch;
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
