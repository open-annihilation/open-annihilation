// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Replacing a file: a new file, a file written over, a name outside ASCII,
// and a folder that is left untouched when the replace is refused.

#include "oa/platform/files.hpp"
#include "oa/test/check.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

std::string read_text(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_text(const std::filesystem::path& file, std::string_view text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream output(file, std::ios::binary);
    OA_CHECK(static_cast<bool>(output));
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

/// A folder removed when the test ends.
struct Scratch {
    std::filesystem::path path{};

    Scratch() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("oa-replace-" + std::to_string(tick));
        std::filesystem::create_directories(path);
    }

    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

bool temporary_left(const std::filesystem::path& folder) {
    std::error_code failure;
    for (std::filesystem::directory_iterator cursor(folder, failure), end;
         !failure && cursor != end;
         cursor.increment(failure)) {
        if (oa::platform::path_to_utf8(cursor->path().filename()).find(".tmp-") !=
            std::string::npos)
            return true;
    }
    return false;
}

/// A file in a folder that was not there is created, and no temporary file remains.
void new_file_is_created() {
    const Scratch scratch;
    const auto file = scratch.path / "fresh" / "registries.yaml";
    const auto bytes = bytes_of("registries: []\n");
    OA_CHECK(oa::platform::replace_file(file, bytes, nullptr));
    OA_CHECK(read_text(file) == "registries: []\n");
    OA_CHECK(!temporary_left(file.parent_path()));
}

/// An existing file is replaced whole. The previous bytes do not remain.
void existing_file_is_replaced() {
    const Scratch scratch;
    const auto file = scratch.path / "registries.yaml";
    write_text(file, "old\n");
    std::string error = "unset";
    const auto bytes = bytes_of("new\n");
    OA_CHECK(oa::platform::replace_file(file, bytes, &error));
    OA_CHECK(read_text(file) == "new\n");
    OA_CHECK(!temporary_left(scratch.path));
}

/// A name outside ASCII is written and read back.
void name_outside_ascii_is_written() {
    const Scratch scratch;
    const auto file =
        scratch.path / std::filesystem::path(std::u8string(u8"registries-\u00f1.yaml"));
    const auto bytes = bytes_of("listed\n");
    std::string error;
    OA_CHECK(oa::platform::replace_file(file, bytes, &error));
    OA_CHECK(read_text(file) == "listed\n");
    OA_CHECK(!temporary_left(scratch.path));
}

/// A folder cannot be replaced. The marker inside it stays, and no temporary file is left.
void folder_target_is_left_untouched() {
    const Scratch scratch;
    const auto folder = scratch.path / "target";
    write_text(folder / "keep.txt", "keep");
    std::string error;
    OA_CHECK(!oa::platform::replace_file(folder, bytes_of("nope"), &error));
    OA_CHECK(!error.empty());
    OA_CHECK(std::filesystem::is_directory(folder));
    OA_CHECK(read_text(folder / "keep.txt") == "keep");
    OA_CHECK(!temporary_left(scratch.path));
}

} // namespace

int main() {
    new_file_is_created();
    existing_file_is_replaced();
    name_outside_ascii_is_written();
    folder_target_is_left_untouched();
    return oa::test::check_exit_status();
}
