// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A release a replay names, unpacked into its own folder beside the one the
// player uses. The installer's own folders are not touched.

#include "files.hpp"
#include "package_file.hpp"

#include "oa/app/package_install/oamod.hpp"
#include "oa/app/package_install/replay_release.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/map_pack/manifest.hpp"
#include "oa/formats/zip/stream.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app::package_install {

namespace fs = std::filesystem;
namespace zip = oa::formats::zip;

namespace {

/// Archive bytes one step of an entry may read.
constexpr uint64_t read_piece_bytes = uint64_t{1} << 20;

/// Sets a refusal.
///
/// @param error the caller's reason; may be null
/// @param reason why
void set_error(std::string* error, std::string reason) {
    if (error != nullptr)
        *error = std::move(reason);
}

/// Removes a folder this install made, and leaves a failure as it is.
///
/// @param folder the folder
void discard_folder(const fs::path& folder) {
    std::error_code ignored;
    fs::remove_all(folder, ignored);
}

/// Tells whether a digest's text is 64 lower-case hex digits.
///
/// @param text the text
/// @return true when it is
bool digest_name(std::string_view text) {
    if (text.size() != 64)
        return false;
    for (const char character : text) {
        const bool digit = character >= '0' && character <= '9';
        const bool letter = character >= 'a' && character <= 'f';
        if (!digit && !letter)
            return false;
    }
    return true;
}

/// The folder Replay releases/<sha>, when the text is a digest.
///
/// @param player_folder the player's own folder
/// @param sha256 the digest text
/// @return the folder; nothing when the text is not a digest
std::optional<fs::path> release_path(const fs::path& player_folder, std::string_view sha256) {
    if (player_folder.empty() || !digest_name(sha256))
        return std::nullopt;
    return player_folder / std::string(replay_releases_folder_name) / std::string(sha256);
}

/// Writes one entry into the staging folder.
///
/// @param file the package
/// @param package the package, already read
/// @param packaged the file
/// @param staging where the release is unpacked
/// @param[out] error why it was not written
/// @return true when the file is in place
bool write_entry(
    detail::PackageFile& file,
    const Package& package,
    const PackagedFile& packaged,
    const fs::path& staging,
    std::string* error
) {
    if (packaged.entry >= package.directory.entries.size()) {
        set_error(error, "the package names a file it does not hold");
        return false;
    }
    const zip::StreamEntry& entry = package.directory.entries[packaged.entry];
    if (entry.directory)
        return true;
    if (entry.symbolic_link || entry.special_file) {
        set_error(error, "the package holds a link");
        return false;
    }
    const fs::path path = staging / detail::path_of(packaged.name);
    std::error_code failure;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), failure);
    if (failure) {
        set_error(error, "cannot make a folder for the release");
        return false;
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        set_error(error, "cannot write the release");
        return false;
    }
    zip::ZipError zip_error{};
    zip::EntryStream reader;
    if (!reader.open(file.source(), file.bytes, entry, zip_error)) {
        set_error(error, "the package could not be read");
        return false;
    }

    struct Sink {
        std::ofstream* stream{};

        /// Writes the next piece.
        ///
        /// @param bytes the piece
        /// @return true when it was written
        bool take(std::span<const uint8_t> bytes) {
            stream->write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size())
            );
            return static_cast<bool>(*stream);
        }
    };

    Sink sink{&stream};
    const zip::SinkHooks hooks{&sink, [](void* context, std::span<const uint8_t> bytes) {
                                   return static_cast<Sink*>(context)->take(bytes);
                               }};
    for (;;) {
        const zip::StreamStep step = reader.step(hooks, read_piece_bytes, zip_error);
        if (step == zip::StreamStep::failed) {
            set_error(error, "the package could not be read");
            return false;
        }
        if (step == zip::StreamStep::done)
            break;
    }
    stream.flush();
    if (!stream) {
        set_error(error, "cannot write the release");
        return false;
    }
    return true;
}

/// Moves a folder that does not verify out of the way of a new release.
///
/// @param from the folder that is in the way
/// @param aside where it goes; an earlier aside is removed first
/// @return true when `from` has been moved
bool move_aside(const fs::path& from, const fs::path& aside) {
    discard_folder(aside);
    std::error_code failure;
    fs::rename(from, aside, failure);
    return !failure;
}

/// True when `folder` still reads as the mod or the map pack its origin names.
///
/// @param folder the release's folder
/// @return true when a mod profile or a pack manifest can be read
bool release_is_readable(const fs::path& folder) {
    const InstalledPackage read = oamod::read_installed_mod(folder);
    if (read.kind == FolderKind::package && !read.id.empty())
        return true;
    return installed_pack_manifest(folder).has_value();
}

} // namespace

std::optional<oa::data::map_pack::Manifest> installed_pack_manifest(const fs::path& folder) {
    std::ifstream manifest_in(
        folder / std::string(oa::data::map_pack::manifest_file), std::ios::binary
    );
    const std::vector<uint8_t> yaml{
        std::istreambuf_iterator<char>(manifest_in), std::istreambuf_iterator<char>()
    };
    if (!manifest_in && !manifest_in.eof())
        return std::nullopt;
    oa::data::map_pack::Manifest manifest;
    std::vector<oa::data::map_pack::Problem> problems;
    if (!oa::data::map_pack::read_manifest(
            yaml, oa::data::map_pack::ManifestUse::package, manifest, problems
        ) ||
        manifest.id.empty())
        return std::nullopt;
    return manifest;
}

std::optional<fs::path>
replay_release_folder(const fs::path& player_folder, std::string_view sha256) {
    const std::optional<fs::path> folder = release_path(player_folder, sha256);
    if (!folder)
        return std::nullopt;
    const std::optional<Origin> recorded = read_origin(*folder);
    if (!recorded || !recorded->sha256)
        return std::nullopt;
    const auto hex = oa::base::sha256::to_hex(*recorded->sha256);
    if (std::string_view(hex.data(), hex.size()) != sha256)
        return std::nullopt;
    return folder;
}

std::optional<fs::path> install_replay_release(
    const fs::path& player_folder,
    const fs::path& package_file,
    const Origin& origin,
    std::string* error
) {
    if (!origin.sha256) {
        set_error(error, "the release has no digest");
        return std::nullopt;
    }
    const auto hex = oa::base::sha256::to_hex(*origin.sha256);
    const std::string name(hex.data(), hex.size());
    const std::optional<fs::path> destination = release_path(player_folder, name);
    if (!destination) {
        set_error(error, "the release has no folder");
        return std::nullopt;
    }
    if (replay_release_folder(player_folder, name) && release_is_readable(*destination))
        return destination;
    const PackageResult opened = open_package(package_file);
    if (!opened.package) {
        set_error(
            error,
            opened.problem.detail.empty() ? std::string("the package was refused")
                                          : opened.problem.detail
        );
        return std::nullopt;
    }
    const fs::path root = player_folder / std::string(replay_releases_folder_name);
    const fs::path staging = root / (name + ".staging");
    discard_folder(staging);
    std::error_code failure;
    fs::create_directories(staging, failure);
    if (failure) {
        set_error(error, "cannot make a folder for the release");
        return std::nullopt;
    }
    detail::PackageFile archive;
    if (!archive.open(package_file)) {
        discard_folder(staging);
        set_error(error, "the package could not be read");
        return std::nullopt;
    }
    for (const std::string& nested : opened.package->folders) {
        fs::create_directories(staging / detail::path_of(nested), failure);
        if (failure) {
            discard_folder(staging);
            set_error(error, "cannot make a folder for the release");
            return std::nullopt;
        }
    }
    for (const PackagedFile& packaged : opened.package->files) {
        if (!write_entry(archive, *opened.package, packaged, staging, error)) {
            discard_folder(staging);
            return std::nullopt;
        }
    }
    Origin record = origin;
    if (record.installed.empty())
        record.installed = utc_date_text(std::chrono::system_clock::now());
    const std::string text = origin_text(record);
    if (text.empty()) {
        discard_folder(staging);
        set_error(error, "the origin record cannot be written");
        return std::nullopt;
    }
    const fs::path origin_path = staging / std::string(origin_file_name);
    std::ofstream origin_stream(origin_path, std::ios::binary | std::ios::trunc);
    origin_stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    origin_stream.flush();
    if (!origin_stream) {
        discard_folder(staging);
        set_error(error, "the origin record cannot be written");
        return std::nullopt;
    }
    origin_stream.close();
    std::error_code present;
    fs::path aside;
    if (fs::exists(*destination, present)) {
        // A folder that verifies, and still reads, appeared while this copy was unpacked.
        if (replay_release_folder(player_folder, name) && release_is_readable(*destination)) {
            discard_folder(staging);
            return destination;
        }
        aside = root / (name + ".old");
        if (!move_aside(*destination, aside)) {
            discard_folder(staging);
            set_error(error, "the release's folder is already there");
            return std::nullopt;
        }
    }
    std::error_code renamed;
    fs::rename(staging, *destination, renamed);
    if (renamed) {
        discard_folder(staging);
        if (!aside.empty()) {
            std::error_code restore;
            fs::rename(aside, *destination, restore);
        }
        set_error(error, "cannot put the release in place");
        return std::nullopt;
    }
    if (!aside.empty())
        discard_folder(aside);
    detail::log_line("kept a replay's release in " + detail::utf8_of(*destination));
    return destination;
}

} // namespace oa::app::package_install
