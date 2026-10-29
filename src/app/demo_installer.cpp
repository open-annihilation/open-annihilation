// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Recognising the installer of the Total Annihilation demo (1997) and
// unpacking the game data archive it carries.
#include "oa/app/demo_installer.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/data/content_bundles/record.hpp"
#include "oa/formats/pe.hpp"
#include "oa/platform/file_clone.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace sha256 = oa::base::sha256;

/// Bytes read at a time while hashing a file.
constexpr size_t read_piece_size = size_t{1} << 16;
/// The extension of a Windows program's name, in lower case.
constexpr std::string_view program_extension = ".exe";
/// Where an earlier version unpacked the demo, under the per-user data folder.
constexpr std::string_view legacy_demo_folder = "demo-1997";
/// Bytes in a megabyte, for the sizes told to the player.
constexpr uint64_t megabyte = 1'000'000;
/// Numbers this process's temporary files.
std::atomic<unsigned long> temporary_sequence{};
/// Follows the archive's name in the name of the temporary file it is
/// unpacked to.
constexpr std::string_view temporary_marker = ".unpacking-";
/// How long a temporary file must have gone unwritten to be taken for one an
/// unpacking that stopped part way left behind, and not one being written.
constexpr auto abandoned_temporary_age = std::chrono::minutes(1);

/// The files in a folder that may be the installer.
struct InstallerCandidates {
    /// Files of the installer's size, in name order.
    std::vector<fs::path> same_size;
    /// Programs (.exe) of any other size.
    std::vector<fs::path> programs;
};

/// What find_installer() found.
struct InstallerSearch {
    fs::path installer;
    /// The installer's bytes, when it was found.
    std::vector<uint8_t> bytes;
    std::vector<fs::path> rejected;
};

/// Tests whether a file's name ends in .exe, in any case.
[[nodiscard]] bool is_program_name(const fs::path& path) {
    const auto extension = path.extension().u8string();
    if (extension.size() != program_extension.size())
        return false;
    for (size_t index = 0; index < extension.size(); ++index)
        if (std::tolower(static_cast<unsigned char>(extension[index])) != program_extension[index])
            return false;
    return true;
}

/// Hashes a file of a known size, reading it in pieces.
///
/// @return its SHA-256; nullopt when it cannot be read or its size differs
[[nodiscard]] std::optional<sha256::Digest>
file_digest(const fs::path& path, uint64_t expected_size) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    std::vector<uint8_t> piece(read_piece_size);
    sha256::Hasher hasher;
    uint64_t total = 0;
    for (;;) {
        input.read(
            reinterpret_cast<char*>(piece.data()), static_cast<std::streamsize>(piece.size())
        );
        const auto count = static_cast<size_t>(input.gcount());
        if (count == 0)
            break;
        total += count;
        if (total > expected_size)
            return std::nullopt;
        sha256::update(hasher, std::span<const uint8_t>(piece.data(), count));
    }
    if (input.bad() || total != expected_size)
        return std::nullopt;
    return sha256::finish(hasher);
}

/// Reads a whole file of a known size.
///
/// @return its bytes; nullopt when it cannot be read or its size differs
[[nodiscard]] std::optional<std::vector<uint8_t>> read_file(const fs::path& path, uint64_t size) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    std::vector<uint8_t> bytes(size);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (static_cast<uint64_t>(input.gcount()) != size ||
        input.peek() != std::ifstream::traits_type::eof())
        return std::nullopt;
    return bytes;
}

/// Lists the files directly in a folder that may be the installer.
[[nodiscard]] InstallerCandidates
list_candidates(const fs::path& folder, const DemoRelease& release) {
    InstallerCandidates candidates;
    std::error_code error;
    for (fs::directory_iterator entry(folder, error), end; !error && entry != end;
         entry.increment(error)) {
        std::error_code entry_error;
        if (!entry->is_regular_file(entry_error))
            continue;
        const auto size = entry->file_size(entry_error);
        if (entry_error)
            continue;
        if (size == release.installer_size)
            candidates.same_size.push_back(entry->path());
        else if (is_program_name(entry->path()))
            candidates.programs.push_back(entry->path());
    }
    std::sort(candidates.same_size.begin(), candidates.same_size.end());
    std::sort(candidates.programs.begin(), candidates.programs.end());
    return candidates;
}

/// Looks through a folder's candidates for the installer.
///
/// Files of the installer's size are read and hashed in name order until
/// one matches. Those that do not, and programs of any other size, are
/// rejected; files after the match are not read.
[[nodiscard]] InstallerSearch
find_installer(const InstallerCandidates& candidates, const DemoRelease& release) {
    InstallerSearch search;
    search.rejected = candidates.programs;
    for (const auto& candidate : candidates.same_size) {
        auto bytes = read_file(candidate, release.installer_size);
        if (bytes && sha256::digest_of(*bytes) == release.installer_sha256) {
            search.installer = candidate;
            search.bytes = std::move(*bytes);
            break;
        }
        search.rejected.push_back(candidate);
    }
    std::sort(search.rejected.begin(), search.rejected.end());
    return search;
}

/// Removes the temporary files that unpackings which stopped part way, such
/// as one whose process was ended, left in the archive's folder.
///
/// A temporary file is the archive's name, temporary_marker and a number;
/// one written within abandoned_temporary_age may belong to an unpacking
/// still under way and is left alone.
void remove_abandoned_temporaries(const fs::path& folder, const DemoRelease& release) {
    const auto prefix = std::string(release.archive_name) + std::string(temporary_marker);
    const auto now = fs::file_time_type::clock::now();
    std::vector<fs::path> abandoned;
    std::error_code error;
    for (fs::directory_iterator entry(folder, error), end; !error && entry != end;
         entry.increment(error)) {
        std::error_code entry_error;
        if (!path_to_utf8(entry->path().filename()).starts_with(prefix) ||
            !entry->is_regular_file(entry_error))
            continue;
        const auto written = entry->last_write_time(entry_error);
        if (!entry_error && now - written >= abandoned_temporary_age)
            abandoned.push_back(entry->path());
    }
    for (const auto& path : abandoned) {
        std::error_code ignored;
        fs::remove(path, ignored);
    }
}

/// Writes a size in megabytes, rounded to the nearest.
[[nodiscard]] std::string megabytes(uint64_t bytes) {
    return std::to_string((bytes + megabyte / 2) / megabyte) + " MB";
}

/// Tests whether the disk holding `folder` has room for `size` bytes.
///
/// @param[out] available the bytes free, when known
/// @return false only when the disk is known to lack the room
[[nodiscard]] bool has_room(const fs::path& folder, uint64_t size, uint64_t& available) {
    std::error_code error;
    const auto space = fs::space(folder, error);
    if (error)
        return true;
    available = space.available;
    return space.available >= size;
}

/// Tells whether the folder the archive goes to, or the archive's own name
/// in it, is a symbolic link or another entry that is not a plain folder or
/// file, which writing there would follow elsewhere.
///
/// @param folder the folder named release.folder_name
/// @param archive the archive named release.archive_name in it
/// @return true when either is such an entry
[[nodiscard]] bool leads_elsewhere(const fs::path& folder, const fs::path& archive) {
    std::error_code error;
    const auto folder_status = fs::symlink_status(folder, error);
    if (fs::exists(folder_status) && !fs::is_directory(folder_status))
        return true;
    const auto archive_status = fs::symlink_status(archive, error);
    return fs::exists(archive_status) && !fs::is_regular_file(archive_status);
}

/// Unpacks the installer's archive resource into setup.archive, checking it on the way.
///
/// @param bytes the recognised installer
/// @param release the release it is
/// @param[in,out] setup names the folder and the archive; takes the outcome
void unpack(std::span<const uint8_t> bytes, const DemoRelease& release, DemoSetup& setup) {
    const auto fail = [&setup](DemoOutcome outcome, std::string problem) {
        setup.outcome = outcome;
        setup.problem = std::move(problem);
    };
    const auto no_room = [&](uint64_t available) {
        fail(
            DemoOutcome::disk_full,
            "unpacking its game data needs about " + megabytes(release.archive_size) + " free in " +
                path_to_utf8(setup.folder) + ", and about " + megabytes(available) + " is free"
        );
    };
    const auto lookup = formats::pe::find_resource(
        bytes,
        {release.archive_resource_type,
         release.archive_resource_id,
         release.archive_resource_language}
    );
    if (lookup.error != formats::pe::Error::none)
        return fail(
            DemoOutcome::unpack_failed,
            "its game data could not be found in it: " +
                std::string(formats::pe::describe(lookup.error)) + " (at byte " +
                std::to_string(lookup.error_offset) + ")"
        );
    if (lookup.range.size != release.archive_size)
        return fail(
            DemoOutcome::unpack_failed,
            "its game data is " + std::to_string(lookup.range.size) + " bytes, where " +
                std::to_string(release.archive_size) + " were expected"
        );
    const auto archive = bytes.subspan(lookup.range.offset, lookup.range.size);

    std::error_code error;
    fs::create_directories(setup.folder, error);
    if (error)
        return fail(
            DemoOutcome::unpack_failed,
            "the folder " + path_to_utf8(setup.folder) + " could not be made: " + error.message()
        );
    uint64_t available = 0;
    if (!has_room(setup.folder, archive.size(), available))
        return no_room(available);

    const auto temporary =
        setup.folder /
        (std::string(release.archive_name) + std::string(temporary_marker) +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
         std::to_string(temporary_sequence++));
    const auto discard = [&temporary] {
        std::error_code ignored;
        fs::remove(temporary, ignored);
    };
    bool written = false;
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (output) {
            output.write(
                reinterpret_cast<const char*>(archive.data()),
                static_cast<std::streamsize>(archive.size())
            );
            output.flush();
            written = static_cast<bool>(output);
            output.close();
            written = written && !output.fail();
        }
    }
    if (!written) {
        discard();
        if (!has_room(setup.folder, archive.size(), available))
            return no_room(available);
        return fail(
            DemoOutcome::unpack_failed,
            "its game data could not be written to " + path_to_utf8(setup.folder)
        );
    }
    const auto digest = file_digest(temporary, release.archive_size);
    if (!digest || *digest != release.archive_sha256) {
        discard();
        return fail(
            DemoOutcome::unpack_failed,
            "the unpacked game data did not pass its SHA-256 check, so it was not kept"
        );
    }
    // An archive left there failed its check; the checked one replaces it.
    fs::remove(setup.archive, error);
    error.clear();
    fs::rename(temporary, setup.archive, error);
    if (error) {
        discard();
        return fail(
            DemoOutcome::unpack_failed,
            "the unpacked game data could not be put in place as " + path_to_utf8(setup.archive) +
                ": " + error.message()
        );
    }
    setup.outcome = DemoOutcome::ready;
    setup.unpacked = true;
}

/// Converts a file-clock time to whole seconds since 1970.
template <typename Clock>
[[nodiscard]] int64_t seconds_of_clock(typename Clock::time_point time) {
    const auto count = [](auto system_time) {
        return std::chrono::floor<std::chrono::seconds>(system_time).time_since_epoch().count();
    };
    if constexpr (requires { Clock::to_sys(time); })
        return static_cast<int64_t>(count(Clock::to_sys(time)));
    else {
        const auto utc = Clock::to_utc(time);
        using Utc = typename decltype(utc)::clock;
        return static_cast<int64_t>(count(Utc::to_sys(utc)));
    }
}

/// Whole seconds since 1970 of a file's modification time, as a copy's record stores it.
[[nodiscard]] int64_t seconds_since_1970(fs::file_time_type time) {
    return seconds_of_clock<fs::file_time_type::clock>(time);
}

/// Writes the base game's record beside an archive that just passed its check.
///
/// A release whose folder name is not a base game has no record. A record
/// already there is replaced with this archive.
void write_bundle_record(const DemoSetup& setup, const DemoRelease& release) {
    const oa::data::content_bundles::Bundle* bundle =
        oa::data::content_bundles::find_bundle(release.folder_name);
    if (bundle == nullptr || bundle->kind != oa::data::content_bundles::Kind::base_game)
        return;
    ContentRecord record;
    record.bundle_id = std::string(bundle->id);
    record.source = path_to_utf8(setup.installer);
    ContentRecordFile file;
    file.copy_name = std::string(release.archive_name);
    file.size = release.archive_size;
    file.digest = release.archive_sha256;
    std::error_code error;
    const auto stamp = fs::last_write_time(setup.archive, error);
    file.modified = error ? 0 : seconds_since_1970(stamp);
    record.files.push_back(std::move(file));
    const std::optional<std::string> text =
        oa::data::content_bundles::format_content_record(record);
    if (!text)
        return;
    const fs::path record_path = setup.folder / fs::path(std::string(content_record_name));
    std::ofstream output(record_path, std::ios::binary | std::ios::trunc);
    output << *text;
    if (!output)
        return;
    note_content_bundles_changed();
}

/// The archive resource of a recognised installer.
///
/// @param bytes the installer
/// @param release the release
/// @param[out] problem why the archive is not there, when the result is empty
/// @return the archive's bytes, or an empty span
[[nodiscard]] std::span<const uint8_t>
archive_bytes(std::span<const uint8_t> bytes, const DemoRelease& release, std::string& problem) {
    const auto lookup = formats::pe::find_resource(
        bytes,
        {release.archive_resource_type,
         release.archive_resource_id,
         release.archive_resource_language}
    );
    if (lookup.error != formats::pe::Error::none) {
        problem = "its game data could not be found in it: " +
                  std::string(formats::pe::describe(lookup.error));
        return {};
    }
    if (lookup.range.size != release.archive_size) {
        problem = "its game data is " + std::to_string(lookup.range.size) + " bytes, where " +
                  std::to_string(release.archive_size) + " were expected";
        return {};
    }
    return bytes.subspan(lookup.range.offset, lookup.range.size);
}

/// The earlier unpack, `data_folder`/demo-1997/release.archive_name.
[[nodiscard]] fs::path legacy_archive(const fs::path& data_folder, const DemoRelease& release) {
    return data_folder / std::string(legacy_demo_folder) /
           fs::path(std::string(release.archive_name));
}

/// Copies `source` onto `destination`, sharing blocks where the volume can.
///
/// `destination` must not exist. A share that does not then match the
/// release is removed. Otherwise the bytes are written to a temporary file
/// and renamed into place. The source is only read.
[[nodiscard]] bool
copy_archive_file(const fs::path& source, const fs::path& destination, const DemoRelease& release) {
    std::error_code error;
    fs::create_directories(destination.parent_path(), error);
    if (error)
        return false;
    std::string share_error;
    if (oa::platform::share_file_blocks(source, destination, &share_error) &&
        archive_matches(destination, release)) {
        const auto stamp = fs::last_write_time(source, error);
        if (!error)
            fs::last_write_time(destination, stamp, error);
        return true;
    }
    fs::remove(destination, error);
    uint64_t available = 0;
    if (!has_room(destination.parent_path(), release.archive_size, available))
        return false;
    const auto temporary =
        destination.parent_path() /
        (std::string(release.archive_name) + std::string(temporary_marker) +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
         std::to_string(temporary_sequence++));
    const auto discard = [&temporary] {
        std::error_code ignored;
        fs::remove(temporary, ignored);
    };
    {
        std::ifstream input(source, std::ios::binary);
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!input || !output) {
            discard();
            return false;
        }
        std::vector<char> piece(read_piece_size);
        while (input) {
            input.read(piece.data(), static_cast<std::streamsize>(piece.size()));
            const auto count = input.gcount();
            if (count > 0)
                output.write(piece.data(), count);
        }
        output.flush();
        output.close();
        if (!output || input.bad()) {
            discard();
            return false;
        }
    }
    fs::rename(temporary, destination, error);
    if (error) {
        discard();
        return false;
    }
    const auto stamp = fs::last_write_time(source, error);
    if (!error)
        fs::last_write_time(destination, stamp, error);
    if (!archive_matches(destination, release)) {
        fs::remove(destination, error);
        return false;
    }
    return true;
}

} // namespace

DemoSetup
set_up_demo(const fs::path& folder, const fs::path& player_folder, const DemoRelease& release) {
    DemoSetup setup;
    const auto candidates = list_candidates(folder, release);
    if (candidates.same_size.empty()) {
        setup.rejected = candidates.programs;
        setup.outcome =
            setup.rejected.empty() ? DemoOutcome::no_installer : DemoOutcome::unrecognised;
        return setup;
    }
    const auto unpacked_folder = demo_bundle_directory(player_folder, release);
    const auto archive = unpacked_folder / fs::path(std::string(release.archive_name));
    std::error_code games_error;
    const auto games_status =
        fs::symlink_status(player_folder / std::string(managed_games_folder), games_error);
    const bool games_linked =
        !player_folder.empty() && fs::exists(games_status) && !fs::is_directory(games_status);
    const bool linked =
        !player_folder.empty() && (games_linked || leads_elsewhere(unpacked_folder, archive));
    if (!player_folder.empty() && !linked) {
        remove_abandoned_temporaries(unpacked_folder, release);
        // An archive unpacked before that passes its check needs nothing
        // from the installer: the first file of its size is taken for it
        // unread.
        if (archive_matches(archive, release)) {
            setup.outcome = DemoOutcome::ready;
            setup.installer = candidates.same_size.front();
            setup.rejected = candidates.programs;
            setup.folder = unpacked_folder;
            setup.archive = archive;
            write_bundle_record(setup, release);
            return setup;
        }
    }
    auto search = find_installer(candidates, release);
    setup.rejected = std::move(search.rejected);
    if (search.installer.empty()) {
        setup.outcome = DemoOutcome::unrecognised;
        return setup;
    }
    setup.installer = std::move(search.installer);
    if (player_folder.empty()) {
        setup.outcome = DemoOutcome::unpack_failed;
        setup.problem = "no per-user data folder is known to unpack it to";
        return setup;
    }
    setup.folder = unpacked_folder;
    setup.archive = archive;
    if (linked) {
        setup.outcome = DemoOutcome::unpack_failed;
        setup.problem = path_to_utf8(unpacked_folder) +
                        " or the archive in it is a link, and its game data is not written "
                        "through one";
        return setup;
    }
    unpack(search.bytes, release, setup);
    if (setup.outcome == DemoOutcome::ready)
        write_bundle_record(setup, release);
    return setup;
}

DemoInstallerSource::DemoInstallerSource(fs::path installer_or_folder, const DemoRelease& release)
    : place_(std::move(installer_or_folder)), release_(&release) {
}

std::string DemoInstallerSource::origin() const {
    return path_to_utf8(place_);
}

bool DemoInstallerSource::open(std::string* error) {
    if (opened_)
        return true;
    std::error_code failure;
    if (fs::is_regular_file(place_, failure)) {
        auto bytes = read_file(place_, release_->installer_size);
        if (!bytes || sha256::digest_of(*bytes) != release_->installer_sha256) {
            if (error != nullptr)
                *error = "that file is not the installer of the Total Annihilation demo (1997)";
            return false;
        }
        installer_ = place_;
        bytes_ = std::move(*bytes);
        opened_ = true;
        return true;
    }
    if (!fs::is_directory(place_, failure)) {
        if (error != nullptr)
            *error = "the installer could not be read";
        return false;
    }
    const auto search = find_installer(list_candidates(place_, *release_), *release_);
    if (search.installer.empty()) {
        if (error != nullptr)
            *error = "that folder holds no installer of the Total Annihilation demo (1997)";
        return false;
    }
    installer_ = search.installer;
    bytes_ = search.bytes;
    opened_ = true;
    return true;
}

bool DemoInstallerSource::list(std::vector<ContentSourceFile>& files, std::string* error) {
    files.clear();
    if (!open(error))
        return false;
    ContentSourceFile listed;
    listed.name = std::string(release_->archive_name);
    listed.size = release_->archive_size;
    std::error_code failure;
    const auto stamp = fs::last_write_time(installer_, failure);
    listed.modified = failure ? 0 : seconds_since_1970(stamp);
    files.push_back(std::move(listed));
    return true;
}

bool DemoInstallerSource::stream_file(
    const StreamRequest& request, StreamResult& result, std::string* error
) {
    result = {};
    if (!open(error))
        return false;
    if (request.name != release_->archive_name) {
        if (error != nullptr)
            *error = "the installer does not hold that file";
        return false;
    }
    std::string problem;
    const std::span<const uint8_t> archive = archive_bytes(bytes_, *release_, problem);
    if (archive.empty()) {
        if (error != nullptr)
            *error = problem.empty() ? "its game data could not be found in it" : problem;
        return false;
    }
    uint64_t prefix = 0;
    {
        std::error_code failure;
        if (fs::is_regular_file(request.destination, failure)) {
            const auto existing = fs::file_size(request.destination, failure);
            if (!failure)
                prefix = static_cast<uint64_t>(existing);
        }
    }
    if (prefix > archive.size())
        prefix = 0;
    if (prefix > 0) {
        const auto kept = read_file(request.destination, prefix);
        if (!kept || !std::equal(
                         kept->begin(),
                         kept->end(),
                         archive.begin(),
                         archive.begin() + static_cast<std::ptrdiff_t>(prefix)
                     ))
            prefix = 0;
    }
    if (prefix == 0) {
        std::error_code failure;
        fs::remove(request.destination, failure);
    }
    std::error_code parent_error;
    fs::create_directories(request.destination.parent_path(), parent_error);
    const auto mode =
        prefix == 0 ? std::ios::binary | std::ios::trunc : std::ios::binary | std::ios::app;
    std::ofstream output(request.destination, mode);
    if (!output) {
        if (error != nullptr)
            *error = "its game data could not be written";
        return false;
    }
    if (prefix < archive.size()) {
        output.write(
            reinterpret_cast<const char*>(archive.data() + prefix),
            static_cast<std::streamsize>(archive.size() - prefix)
        );
    }
    output.flush();
    output.close();
    if (!output) {
        result.no_space = true;
        if (error != nullptr)
            *error = "its game data could not be written";
        return false;
    }
    const auto digest = file_digest(request.destination, release_->archive_size);
    if (!digest || *digest != release_->archive_sha256) {
        std::error_code failure;
        fs::remove(request.destination, failure);
        if (error != nullptr)
            *error = "the unpacked game data did not pass its SHA-256 check";
        return false;
    }
    result.digest = *digest;
    result.bytes = release_->archive_size;
    return true;
}

bool adopt_legacy_demo(
    const fs::path& data_folder, const fs::path& player_folder, const DemoRelease& release
) {
    if (data_folder.empty() || player_folder.empty())
        return false;
    const fs::path folder = demo_bundle_directory(player_folder, release);
    const fs::path archive = folder / fs::path(std::string(release.archive_name));
    std::error_code games_error;
    const auto games_status =
        fs::symlink_status(player_folder / std::string(managed_games_folder), games_error);
    const bool games_linked = fs::exists(games_status) && !fs::is_directory(games_status);
    if (games_linked || leads_elsewhere(folder, archive))
        return false;
    const fs::path earlier = legacy_archive(data_folder, release);
    const fs::path record = folder / fs::path(std::string(content_record_name));
    // A copy with its record is the content check's to watch, by size and
    // time; it is not hashed again at every start.
    std::error_code record_error;
    if (fs::is_regular_file(record, record_error) && fs::is_regular_file(archive, record_error))
        return true;
    if (archive_matches(archive, release)) {
        if (!fs::is_regular_file(record, record_error)) {
            DemoSetup setup;
            setup.folder = folder;
            setup.archive = archive;
            setup.installer = fs::is_regular_file(earlier, record_error) ? earlier : archive;
            write_bundle_record(setup, release);
        }
        return true;
    }
    if (!archive_matches(earlier, release))
        return false;
    std::error_code remove_error;
    if (fs::exists(archive, remove_error))
        fs::remove(archive, remove_error);
    if (fs::exists(archive, remove_error))
        return false;
    if (!copy_archive_file(earlier, archive, release))
        return false;
    DemoSetup setup;
    setup.folder = folder;
    setup.archive = archive;
    setup.installer = earlier;
    write_bundle_record(setup, release);
    return archive_matches(archive, release) && archive_matches(earlier, release);
}

bool archive_matches(const fs::path& archive, const DemoRelease& release) {
    std::error_code error;
    if (!fs::is_regular_file(archive, error) ||
        fs::file_size(archive, error) != release.archive_size || error)
        return false;
    const auto digest = file_digest(archive, release.archive_size);
    return digest && *digest == release.archive_sha256;
}

std::string describe_ready(const DemoSetup& setup) {
    return "mounted the Total Annihilation demo (1997) from " + path_to_utf8(setup.archive) +
           (setup.unpacked ? ", unpacked now from " : ", unpacked earlier and checked, from ") +
           path_to_utf8(setup.installer);
}

} // namespace oa::app
