// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A boneyards/ file is read from a fixture archive layered over a fixture
// install: the mod archive, then the install's archive, then a file that
// exists only on the install's disk. The player's copy is read ahead of
// those. A loose file in the expansion folder is read after the game's
// files and ahead of the install's disk. A created file is written only
// under the player's folder.
#include "oa/boneyards/rover_calls.hpp"
#include "oa/boneyards/rover_session.hpp"

#include "oa/formats/hpi.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct ReadBack {
    bool opened = false;
    std::string text;
};

struct ParsedRecord {
    int count = 0;
    std::string command;
    std::string name;
};

/// Writes bytes to a file, creating its folder.
///
/// @param path the file
/// @param bytes its contents
void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!bytes.empty()) {
        out.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
    }
    OA_CHECK(static_cast<bool>(out));
}

/// The bytes of a text.
///
/// @param text the text
/// @return its bytes
std::vector<uint8_t> text_bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

/// An archive of the named files, each holding its text.
///
/// @param files path and text of each entry
/// @return the archive bytes
std::vector<uint8_t> archive(std::vector<std::pair<std::string, std::string>> files) {
    std::vector<oa::HpiWriteFile> written;
    for (auto& file : files)
        written.push_back(oa::HpiWriteFile{std::move(file.first), text_bytes(file.second)});
    return oa::write_hpi(written);
}

/// Reads one game file from the store. A miss leaves `out` unchanged.
///
/// @param context the store
/// @param path relative path
/// @param[out] out receives the bytes
/// @return true when the store holds the file
bool read_store(void* context, const char* path, std::vector<uint8_t>* out) {
    const auto* assets = static_cast<const oa::AssetStore*>(context);
    if (assets == nullptr || path == nullptr || out == nullptr)
        return false;
    try {
        *out = assets->read(path).bytes;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

/// Reads a file through the placed platform.
///
/// @param path path as the game names it
/// @return whether it opened, and its bytes
ReadBack read_back(const char* path) {
    ReadBack result;
    const RoverPlatform* platform = oa::boneyards::rover_session_platform();
    if (platform == nullptr)
        return result;
    RoverFile* file = platform->file_open(path);
    if (file == nullptr)
        return result;
    result.opened = true;
    char buffer[32];
    for (;;) {
        const int32_t count =
            platform->file_read(file, buffer, static_cast<int32_t>(sizeof buffer));
        if (count < 0) {
            result.opened = false;
            break;
        }
        if (count == 0)
            break;
        result.text.append(buffer, static_cast<std::size_t>(count));
    }
    platform->file_close(file);
    return result;
}

/// Creates a text file through the placed platform.
///
/// @param path path as the game names it
/// @param text the bytes to write
/// @return true when every byte was written
bool create_text(const char* path, const char* text) {
    const RoverPlatform* platform = oa::boneyards::rover_session_platform();
    if (platform == nullptr)
        return false;
    RoverFile* file = platform->file_create(path);
    if (file == nullptr)
        return false;
    const auto length = static_cast<int32_t>(std::strlen(text));
    const bool written = platform->file_write(file, text, length) == length;
    platform->file_close(file);
    return written;
}

/// The text of a disk file.
///
/// @param path the file
/// @return its bytes, or "" when it cannot be read
std::string disk_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool ROVER_CALL take_record(
    const char* command,
    const char* /*id*/,
    const char* name,
    const char* /*sort*/,
    const char* /*category*/,
    const char* /*headline*/,
    const char* /*file*/,
    RoverPropertyBag* /*props*/,
    void* user
) {
    auto* parsed = static_cast<ParsedRecord*>(user);
    ++parsed->count;
    if (command != nullptr)
        parsed->command = command;
    if (name != nullptr)
        parsed->name = name;
    return true;
}

/// The install's loose file, the install archive beneath the mod archive, and
/// the player's folder.
///
/// @param scratch directory removed by the caller
void check_layered_files(const fs::path& scratch) {
    const fs::path install = scratch / "install";
    const fs::path player = scratch / "player";
    write_bytes(install / "boneyards" / "loose.txt", text_bytes("from-install-disk"));
    write_bytes(
        install / "by.ccx",
        archive({
            {"boneyards/shared.txt", "from-install-archive"},
            {"boneyards/archive-only.txt", "install-only"},
            {"boneyards/empty.txt", ""},
        })
    );
    write_bytes(
        scratch / "mod.ccx",
        archive({
            {"boneyards/shared.txt", "from-mod"},
            {"boneyards/notes.byd", "{[Map<S:command=sector><S:name=Hub>]}\r\n"},
        })
    );

    oa::AssetStore store(install);
    store.mount(scratch / "mod.ccx");
    store.mount(install / "by.ccx");
    oa::boneyards::rover_session_configure(install.string().c_str(), "");
    oa::boneyards::rover_session_set_virtual_files(read_store, &store);
    oa::boneyards::rover_session_set_player_folder(player.string().c_str());

    const ReadBack shared = read_back("boneyards/shared.txt");
    OA_CHECK(shared.opened && shared.text == "from-mod");
    const ReadBack shared_case = read_back("./Boneyards/Shared.TXT");
    OA_CHECK(shared_case.opened && shared_case.text == "from-mod");
    const ReadBack only = read_back("boneyards/archive-only.txt");
    OA_CHECK(only.opened && only.text == "install-only");
    const ReadBack loose = read_back("boneyards/loose.txt");
    OA_CHECK(loose.opened && loose.text == "from-install-disk");
    const ReadBack missing = read_back("boneyards/missing.txt");
    OA_CHECK(!missing.opened);
    const ReadBack empty = read_back("boneyards/empty.txt");
    OA_CHECK(empty.opened && empty.text.empty());

    std::vector<uint8_t> held{1, 2, 3};
    OA_CHECK(!oa::boneyards::rover_session_read_file("boneyards/missing.txt", &held));
    OA_CHECK(held.size() == 3 && held[0] == 1);
    OA_CHECK(oa::boneyards::rover_session_read_file("boneyards/archive-only.txt", &held));
    OA_CHECK(
        held.size() == std::strlen("install-only") &&
        std::memcmp(held.data(), "install-only", held.size()) == 0
    );
    OA_CHECK(oa::boneyards::rover_session_read_file("boneyards/empty.txt", &held));
    OA_CHECK(held.empty());

    write_bytes(player / "boneyards" / "shared.txt", text_bytes("from-player"));
    const ReadBack player_copy = read_back("boneyards/shared.txt");
    OA_CHECK(player_copy.opened && player_copy.text == "from-player");
    fs::remove(player / "boneyards" / "shared.txt");
    const ReadBack shared_again = read_back("boneyards/shared.txt");
    OA_CHECK(shared_again.opened && shared_again.text == "from-mod");

    OA_CHECK(create_text("boneyards/saved.txt", "saved"));
    OA_CHECK(disk_text(player / "Boneyards" / "saved.txt") == "saved");
    OA_CHECK(!fs::exists(install / "boneyards" / "saved.txt"));
    OA_CHECK(!fs::exists(install / "saved.txt"));

    const fs::path expansion = scratch / "expansion";
    write_bytes(expansion / "boneyards" / "only-expansion.txt", text_bytes("from-expansion"));
    write_bytes(expansion / "boneyards" / "shared.txt", text_bytes("from-expansion-shared"));
    oa::boneyards::rover_session_set_expansion_folder(expansion.string().c_str());
    const ReadBack expansion_only = read_back("boneyards/only-expansion.txt");
    OA_CHECK(expansion_only.opened && expansion_only.text == "from-expansion");
    const ReadBack shared_over_expansion = read_back("boneyards/shared.txt");
    OA_CHECK(shared_over_expansion.opened && shared_over_expansion.text == "from-mod");
    const std::string expansion_placed =
        oa::boneyards::rover_session_placed_path("boneyards/only-expansion.txt");
    std::error_code expansion_same;
    OA_CHECK(
        fs::equivalent(
            fs::path(expansion_placed).parent_path().parent_path(), expansion, expansion_same
        )
    );
    write_bytes(player / "Boneyards" / "only-expansion.txt", text_bytes("from-player-expansion"));
    const ReadBack player_over_expansion = read_back("boneyards/only-expansion.txt");
    OA_CHECK(player_over_expansion.opened && player_over_expansion.text == "from-player-expansion");
    fs::remove(player / "Boneyards" / "only-expansion.txt");
    const ReadBack expansion_again = read_back("boneyards/only-expansion.txt");
    OA_CHECK(expansion_again.opened && expansion_again.text == "from-expansion");
    oa::boneyards::rover_session_set_expansion_folder(nullptr);
    OA_CHECK(!read_back("boneyards/only-expansion.txt").opened);

    oa::boneyards::rover_session_set_virtual_files(nullptr, nullptr);
    const ReadBack loose_disk = read_back("boneyards/loose.txt");
    OA_CHECK(loose_disk.opened && loose_disk.text == "from-install-disk");
    OA_CHECK(!read_back("boneyards/archive-only.txt").opened);
    OA_CHECK(!read_back("boneyards/notes.byd").opened);

    oa::boneyards::rover_session_set_virtual_files(read_store, &store);
    oa::boneyards::rover_module_unload();
    oa::boneyards::rover_module_set_source(rover_get_interface);
    OA_CHECK(oa::boneyards::rover_module_load());
    ParsedRecord parsed;
    OA_CHECK(
        oa::boneyards::rover_call_parse_record_file("boneyards/notes.byd", take_record, &parsed)
    );
    OA_CHECK(parsed.count == 1 && parsed.command == "sector" && parsed.name == "Hub");

    oa::boneyards::rover_session_configure(install.string().c_str(), "");
    OA_CHECK(!read_back("boneyards/archive-only.txt").opened);
    OA_CHECK(read_back("boneyards/loose.txt").text == "from-install-disk");
}

} // namespace

int main() {
    const fs::path scratch = oa::test::make_scratch_directory("oa-boneyards-virtual-files");
    try {
        check_layered_files(scratch);
    } catch (const std::exception& error) {
        std::cerr << "virtual files: " << error.what() << '\n';
        ++oa::test::failed_checks();
    }
    oa::boneyards::rover_module_unload();
    rover_use_platform(nullptr);
    std::error_code remove_error;
    fs::remove_all(scratch, remove_error);
    return oa::test::check_exit_status();
}
