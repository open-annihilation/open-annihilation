// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The installed expansion folder is found when the disk spells Expansions,
// the base game, the bundle and the content record in another case. The
// path that comes back uses that spelling. A join of the copy names would
// hand back those names on a case-insensitive volume.
#include "expansion_folder.hpp"

#include "oa/data/content_bundles.hpp"
#include "oa/data/content_bundles/record.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;

/// Writes a text file, creating its folders.
///
/// @param path the file
/// @param text its contents
void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    OA_CHECK(static_cast<bool>(out));
}

/// A content record that names every file of the bundle.
///
/// @return the record text, or nothing when it cannot be written
std::optional<std::string> record_text() {
    namespace bundles = oa::data::content_bundles;
    const bundles::Bundle* bundle = bundles::find_bundle("boneyards");
    OA_CHECK(bundle != nullptr && bundle->files.size() > 0);
    if (bundle == nullptr)
        return std::nullopt;
    bundles::ContentRecord record;
    record.bundle_id = std::string(bundle->id);
    record.source = "fixture";
    for (const bundles::ContentFile& file : bundle->files) {
        bundles::ContentRecordFile item;
        item.copy_name = std::string(file.copy_name);
        item.size = file.size;
        record.files.push_back(std::move(item));
    }
    return bundles::format_content_record(record);
}

/// The folder's three components under `user` are `expansions`, `base` and
/// `id`, and that folder is the one on disk.
///
/// @param found the folder the lookup returned
/// @param user the player's folder
/// @param expansions the expansions folder's spelling
/// @param base_name the base game folder's spelling
/// @param bundle_name the bundle folder's spelling
void expect_folder(
    const fs::path& found,
    const fs::path& user,
    std::string_view expansions,
    std::string_view base_name,
    std::string_view bundle_name
) {
    OA_CHECK(found.filename().string() == bundle_name);
    OA_CHECK(found.parent_path().filename().string() == base_name);
    OA_CHECK(found.parent_path().parent_path().filename().string() == expansions);
    std::error_code error;
    OA_CHECK(fs::is_directory(found, error));
    OA_CHECK(fs::equivalent(found.parent_path().parent_path().parent_path(), user, error));
}

void check_expansion_folder(const fs::path& scratch) {
    const std::optional<std::string> text = record_text();
    OA_CHECK(text.has_value());
    if (!text.has_value())
        return;
    const oa::platform::preferences::Values values;

    const fs::path flipped = scratch / "flipped";
    const fs::path flipped_folder = flipped / "expansions" / "TOTALA" / "BONEYARDS";
    write_text(flipped_folder / ".OA-CONTENT", *text);
    expect_folder(
        oa::app::installed_expansion_folder(flipped, values, "totala:boneyards"),
        flipped,
        "expansions",
        "TOTALA",
        "BONEYARDS"
    );
    OA_CHECK(oa::app::installed_expansion_folder(flipped, values, "totala").empty());
    OA_CHECK(oa::app::installed_expansion_folder(flipped, values, "").empty());

    const fs::path exact = scratch / "exact";
    write_text(exact / "Expansions" / "totala" / "boneyards" / ".oa-content", *text);
    expect_folder(
        oa::app::installed_expansion_folder(exact, values, "totala:boneyards"),
        exact,
        "Expansions",
        "totala",
        "boneyards"
    );

    OA_CHECK(
        oa::app::installed_expansion_folder(scratch / "absent", values, "totala:boneyards").empty()
    );
    OA_CHECK(oa::app::installed_expansion_folder({}, values, "totala:boneyards").empty());
}

} // namespace

int main() {
    const fs::path scratch = oa::test::make_scratch_directory("oa-boneyards-expansion-folder");
    try {
        check_expansion_folder(scratch);
    } catch (const std::exception& error) {
        std::cerr << "expansion folder: " << error.what() << '\n';
        ++oa::test::failed_checks();
    }
    std::error_code remove_error;
    fs::remove_all(scratch, remove_error);
    return oa::test::check_exit_status();
}
