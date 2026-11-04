// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Loose files are found when the disk spells them in another case than the
// copy name. The path that comes back uses the spelling the disk stored,
// which is what the engine's loose-file lookup returns. A join that asks
// the volume whether the requested spelling exists would hand back that
// requested spelling on a case-insensitive volume.
#include "oa/boneyards/rover_session.hpp"

#include "oa/rover/platform.h"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

/// Writes a small text file, creating its folders.
///
/// @param path the file
/// @param text its contents
void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    OA_CHECK(static_cast<bool>(out));
}

/// The placed path's components below `root` are `first`, `second` and
/// `third`, and the file is the one on disk.
///
/// @param placed the path the lookup returned
/// @param root the folder the lookup was given
/// @param first the folder directly under `root`
/// @param second the folder under `first`
/// @param third the file
void expect_spelling(
    const std::string& placed,
    const fs::path& root,
    std::string_view first,
    std::string_view second,
    std::string_view third
) {
    const fs::path path(placed);
    const fs::path parent = path.parent_path();
    const fs::path folder = parent.parent_path();
    OA_CHECK(path.filename().string() == third);
    OA_CHECK(parent.filename().string() == second);
    OA_CHECK(folder.filename().string() == first);
    std::error_code error;
    OA_CHECK(fs::is_regular_file(path, error));
    OA_CHECK(fs::equivalent(folder.parent_path(), root, error));
}

/// Same as expect_spelling for a file directly under the first folder.
///
/// @param placed the path the lookup returned
/// @param root the folder the lookup was given
/// @param folder the folder's spelling on disk
/// @param file the file's spelling on disk
void expect_pair(
    const std::string& placed, const fs::path& root, std::string_view folder, std::string_view file
) {
    const fs::path path(placed);
    OA_CHECK(path.filename().string() == file);
    OA_CHECK(path.parent_path().filename().string() == folder);
    std::error_code error;
    OA_CHECK(fs::is_regular_file(path, error));
    OA_CHECK(fs::equivalent(path.parent_path().parent_path(), root, error));
}

void check_case_paths(const fs::path& scratch) {
    const fs::path install = scratch / "install";
    write_text(install / "boneyards" / "help" / "help.def", "help");
    write_text(install / "boneyards" / "ladders" / "ladders.def", "ladders");
    write_text(install / "boneyards" / "profile" / "profile.def", "profile");
    write_text(install / "boneyards" / "images" / "i-help.png", "image");
    write_text(install / "boneyards" / "bypacific_map.gwm", "pacific");
    write_text(install / "boneyards" / "alpha_map.gwm", "alpha");

    oa::boneyards::rover_session_configure(install.string().c_str(), "");
    expect_spelling(
        oa::boneyards::rover_session_placed_path("./boneyards/Help/Help.def"),
        install,
        "boneyards",
        "help",
        "help.def"
    );
    expect_spelling(
        oa::boneyards::rover_session_placed_path("./boneyards/Ladders/Ladders.def"),
        install,
        "boneyards",
        "ladders",
        "ladders.def"
    );
    expect_spelling(
        oa::boneyards::rover_session_placed_path("./boneyards/Profile/Profile.def"),
        install,
        "boneyards",
        "profile",
        "profile.def"
    );
    expect_spelling(
        oa::boneyards::rover_session_placed_path("Boneyards/images/I-help.png"),
        install,
        "boneyards",
        "images",
        "i-help.png"
    );
    expect_pair(
        oa::boneyards::rover_session_placed_path("Boneyards\\BYPacific_map.gwm"),
        install,
        "boneyards",
        "bypacific_map.gwm"
    );
    // The war map is requested in this form when the window opens.
    expect_pair(
        oa::boneyards::rover_session_placed_path("Boneyards\\alpha_map.gwm"),
        install,
        "boneyards",
        "alpha_map.gwm"
    );

    const fs::path expansion = scratch / "expansion";
    write_text(expansion / "BONEYARDS" / "HELP" / "only.htm", "from-expansion");
    oa::boneyards::rover_session_set_expansion_folder(expansion.string().c_str());
    expect_spelling(
        oa::boneyards::rover_session_placed_path("Boneyards/Help/Only.htm"),
        expansion,
        "BONEYARDS",
        "HELP",
        "only.htm"
    );
    oa::boneyards::rover_session_set_expansion_folder(nullptr);
    expect_spelling(
        oa::boneyards::rover_session_placed_path("./boneyards/Help/Help.def"),
        install,
        "boneyards",
        "help",
        "help.def"
    );
}

/// True when `base` has a directory whose name is exactly `name`.
bool exact_directory(const fs::path& base, std::string_view name) {
    std::error_code error;
    for (fs::directory_iterator it(base, error), last; !error && it != last; it.increment(error)) {
        std::error_code kind;
        if (!it->is_directory(kind) || kind)
            continue;
        if (it->path().filename().string() == name)
            return true;
    }
    return false;
}

/// True when creating `Aa` does not make `aa` exist.
bool volume_is_case_sensitive(const fs::path& scratch) {
    const fs::path probe = scratch / "Aa";
    fs::create_directory(probe);
    std::error_code error;
    const bool ignored = fs::exists(scratch / "aa", error);
    fs::remove(probe, error);
    return !ignored;
}

bool write_through_rover(const char* path, const char* text) {
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

/// The player's folder spells its directory Boneyards. A lowercase
/// boneyards directory is read while Boneyards is absent, and a write
/// renames it where the volume has two names.
void check_player_folder(const fs::path& scratch) {
    const fs::path player = scratch / "player";
    const fs::path install = scratch / "game";
    fs::create_directories(player);
    fs::create_directories(install);
    const bool sensitive = volume_is_case_sensitive(scratch);

    oa::boneyards::rover_session_configure(install.string().c_str(), "");
    oa::boneyards::rover_session_set_player_folder(player.string().c_str());
    const std::string destined =
        oa::boneyards::rover_session_write_path("./boneyards/serv_sav.byd");
    OA_CHECK(fs::path(destined).filename() == "serv_sav.byd");
    OA_CHECK(fs::path(destined).parent_path().filename() == "Boneyards");
    std::error_code error;
    OA_CHECK(!fs::exists(player / "boneyards", error));

    write_text(player / "boneyards" / "serv_sav.byd", "legacy");
    write_text(player / "boneyards" / "kept.txt", "kept");
    std::vector<uint8_t> bytes;
    OA_CHECK(oa::boneyards::rover_session_read_file("./boneyards/serv_sav.byd", &bytes));
    OA_CHECK(std::string(bytes.begin(), bytes.end()) == "legacy");
    if (sensitive) {
        OA_CHECK(exact_directory(player, "boneyards"));
        OA_CHECK(!exact_directory(player, "Boneyards"));
    }

    OA_CHECK(write_through_rover("./boneyards/serv_sav.byd", "saved"));
    std::ifstream saved(player / "Boneyards" / "serv_sav.byd", std::ios::binary);
    const std::string saved_text{
        std::istreambuf_iterator<char>(saved), std::istreambuf_iterator<char>()
    };
    OA_CHECK(saved_text == "saved");
    std::ifstream kept(player / "Boneyards" / "kept.txt", std::ios::binary);
    const std::string kept_text{
        std::istreambuf_iterator<char>(kept), std::istreambuf_iterator<char>()
    };
    OA_CHECK(kept_text == "kept");
    if (sensitive) {
        OA_CHECK(exact_directory(player, "Boneyards"));
        OA_CHECK(!exact_directory(player, "boneyards"));
    }
}

} // namespace

int main() {
    const fs::path scratch = oa::test::make_scratch_directory("oa-boneyards-case-paths");
    try {
        check_case_paths(scratch);
        check_player_folder(scratch);
    } catch (const std::exception& error) {
        std::cerr << "case paths: " << error.what() << '\n';
        ++oa::test::failed_checks();
    }
    rover_use_platform(nullptr);
    std::error_code remove_error;
    fs::remove_all(scratch, remove_error);
    return oa::test::check_exit_status();
}
