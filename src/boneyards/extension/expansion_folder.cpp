// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The expansion folder is read ahead of the game folder only when the run
// has that expansion turned on and the copy names every one of its files.
#include "expansion_folder.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/data/content_bundles/record.hpp"
#include "oa/formats/hpi.hpp"

#include <exception>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view k_bundle_id = "boneyards";

/// The content record's path under the player's folder, as the copy names it.
///
/// @param bundle the bundle
/// @return `Expansions/<base>/<id>/.oa-content`, with '/' separators
std::string record_relative(const oa::data::content_bundles::Bundle& bundle) {
    namespace bundles = oa::data::content_bundles;
    std::string relative(bundles::managed_expansions_folder);
    relative.push_back('/');
    if (!bundle.extends.empty()) {
        relative.append(bundle.extends[0]);
        relative.push_back('/');
    }
    relative.append(bundle.id);
    relative.push_back('/');
    relative.append(bundles::content_record_name);
    return relative;
}

/// Reads a content record from the file the loose lookup returned.
///
/// A file that cannot be read, or that is larger than a record may be, is
/// present and not valid. The path is the one on disk, so a record stored
/// under another spelling of `.oa-content` is still opened.
///
/// @param path the record file
/// @return the record
oa::data::content_bundles::ContentRecord read_record_file(const fs::path& path) {
    namespace bundles = oa::data::content_bundles;
    constexpr std::size_t limit = std::size_t{1024} * 1024;
    bundles::ContentRecord record;
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) {
        record.present = true;
        record.io_error = true;
        return record;
    }
    std::string text;
    char buffer[4096];
    while (text.size() <= limit) {
        input.read(buffer, static_cast<std::streamsize>(sizeof buffer));
        const auto got = input.gcount();
        if (got > 0)
            text.append(buffer, static_cast<std::size_t>(got));
        if (got < static_cast<std::streamsize>(sizeof buffer))
            break;
    }
    if (input.bad() || text.size() > limit) {
        record.present = true;
        record.io_error = true;
        return record;
    }
    bundles::ContentRecord parsed = bundles::parse_content_record(text);
    parsed.present = true;
    return parsed;
}

/// Tells whether the choice has this expansion on.
///
/// @param choice the run's choice
/// @return true when the expansion is on
bool lists_bundle(const oa::data::content_bundles::ContentChoice& choice) {
    for (const std::string& id : choice.expansions)
        if (id == k_bundle_id)
            return true;
    return false;
}

} // namespace

fs::path installed_expansion_folder(
    const fs::path& user_folder,
    const oa::platform::preferences::Values& values,
    std::string_view content
) {
    namespace bundles = oa::data::content_bundles;
    if (user_folder.empty())
        return {};
    const bundles::Bundle* bundle = bundles::find_bundle(k_bundle_id);
    if (bundle == nullptr)
        return {};
    std::optional<fs::path> record_path;
    try {
        const oa::AssetStore store(user_folder);
        record_path = store.loose_file(record_relative(*bundle));
    } catch (const std::exception&) {
        return {};
    }
    if (!record_path.has_value())
        return {};
    const fs::path folder = record_path->parent_path();
    const bundles::ContentRecord record = read_record_file(*record_path);
    const bool complete = bundles::record_complete(*bundle, record);
    // The stored choice keeps an expansion only when it counts as copied.
    // The command line does not ask that, so the record is checked after.
    std::string copied_id;
    std::vector<std::string_view> copied;
    if (complete) {
        copied_id = std::string(bundle->id);
        copied.push_back(copied_id);
    }
    const bundles::ContentChoice choice = content_choice_from_preferences(values, copied, content);
    // No stored choice turns every copied expansion on for the managed
    // mount. A named game folder stays as it is until the expansion is
    // named, on the command line or in the stored choice.
    if (!choice.error.empty() || choice.migration || !lists_bundle(choice) || !complete)
        return {};
    std::error_code failure;
    if (!fs::is_directory(folder, failure))
        return {};
    return folder;
}

} // namespace oa::app
