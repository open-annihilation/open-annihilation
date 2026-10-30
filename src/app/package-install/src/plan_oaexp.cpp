// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Installing an expansion package: the manifest and the files are checked
// against a table, then the files are copied into Expansions/<game>/<id>
// with the .oa-content record. install_oaexp uses the built-in table.

#include "files.hpp"
#include "package_file.hpp"

#include "oa/app/content_copy.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/package_install/oaexp.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/content_bundles/expansion_package.hpp"
#include "oa/data/content_bundles/record.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/zip/stream.hpp"
#include "oa/platform/files.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app::package_install::oaexp {

namespace fs = std::filesystem;
namespace bundles = oa::data::content_bundles;
namespace sha = oa::base::sha256;
namespace zip = oa::formats::zip;

namespace {

/// The sentence for an id the table does not list as an expansion.
constexpr std::string_view not_an_expansion = "It is not one of the game's expansions.";

/// Bytes of the archive one step of a copy reads.
constexpr uint64_t copy_budget = uint64_t{1} << 20;

/// One file measured while it was copied.
struct HeldFile {
    std::string name{};
    uint64_t size{};
    sha::Digest digest{};
};

/// Where a copied file's bytes are written and hashed.
struct PieceWriter {
    std::ofstream* out{};
    sha::Hasher* hasher{};
    uint64_t written{};
    bool failed{};
};

/// Converts a file time to whole seconds since 1970.
///
/// A clock that can convert straight to the system clock does. The Visual
/// Studio file clock cannot: `to_utc` and then `utc_clock::to_sys` load the
/// time zone database, and a missing database aborts the process. That clock
/// counts 100 ns ticks from 1601-01-01, which is 11644473600 seconds before
/// 1970. For a file written after 2018 its own conversion adds the 27 leap
/// seconds a Windows file time skips and `to_sys` subtracts the leap seconds
/// that have elapsed, the same 27, so the whole seconds are this difference.
///
/// @param time the file time
/// @return the seconds
template <typename Clock>
int64_t seconds_of_clock(typename Clock::time_point time) {
    if constexpr (requires { Clock::to_sys(time); }) {
        return static_cast<int64_t>(
            std::chrono::floor<std::chrono::seconds>(Clock::to_sys(time)).time_since_epoch().count()
        );
    } else {
        static_assert(std::ratio_equal_v<typename Clock::period, std::ratio<1, 10'000'000>>);
        using hundred_ns = std::chrono::duration<long long, std::ratio<1, 10'000'000>>;
        // 11644473600 seconds, in 100 ns ticks: 1601-01-01 to 1970-01-01.
        constexpr hundred_ns file_epoch_to_unix{0x19DB1DED53E8000LL};
        const auto since_unix = time.time_since_epoch() - file_epoch_to_unix;
        return static_cast<int64_t>(std::chrono::floor<std::chrono::seconds>(since_unix).count());
    }
}

/// The modification time of a file, in seconds since 1970.
///
/// @param file the file
/// @return the seconds; 0 when the time cannot be read
int64_t modified_seconds(const fs::path& file) {
    std::error_code error;
    const fs::file_time_type time = fs::last_write_time(file, error);
    if (error)
        return 0;
    return seconds_of_clock<fs::file_time_type::clock>(time);
}

/// Takes the next piece of a file being copied.
///
/// @param context a PieceWriter
/// @param piece the bytes
/// @return false when they could not be written
bool take_piece(void* context, std::span<const uint8_t> piece) {
    auto& writer = *static_cast<PieceWriter*>(context);
    writer.out->write(
        reinterpret_cast<const char*>(piece.data()), static_cast<std::streamsize>(piece.size())
    );
    if (!*writer.out) {
        writer.failed = true;
        return false;
    }
    sha::update(*writer.hasher, piece);
    writer.written += piece.size();
    return true;
}

/// Removes a folder this copy made. A link is left in place.
///
/// @param folder the folder
void discard_folder(const fs::path& folder) {
    if (folder.empty() || is_link_or_junction(folder))
        return;
    std::error_code error;
    fs::remove_all(folder, error);
}

/// Reports whether an existing path from `base` down to `path` is a link.
/// The folders above `base` are the player's own (a linked Documents
/// folder, macOS's /var) and are not looked at.
///
/// @param base the folder the walk starts below
/// @param path the path, inside `base`
/// @param[out] problem the link, when one is found
/// @return true when a link was found
bool link_on_the_way(const fs::path& base, const fs::path& path, Problem& problem) {
    fs::path walked = base;
    for (const fs::path& part : path.lexically_relative(base)) {
        walked /= part;
        std::error_code error;
        if (!fs::exists(walked, error) || error)
            break;
        if (!is_link_or_junction(walked))
            continue;
        problem.refusal = Refusal::link;
        problem.subject = detail::utf8_of(walked);
        return true;
    }
    return false;
}

/// Sets a manifest refusal.
///
/// @param why what the reader said
/// @param[out] problem the refusal
void manifest_problem(const std::string& why, Problem& problem) {
    problem.refusal = Refusal::manifest_errors;
    if (why == not_an_expansion)
        problem.detail = why;
    else if (!why.empty())
        problem.lines.push_back(why);
}

/// Sets the refusal a file fault is shown as.
///
/// @param fault the fault
/// @param[out] problem the refusal
void fault_problem(const bundles::ExpansionFaultReport& fault, Problem& problem) {
    problem.subject = fault.name;
    switch (fault.fault) {
    case bundles::ExpansionFault::stray:
        problem.refusal = Refusal::stray_file;
        break;
    case bundles::ExpansionFault::missing:
        problem.refusal = Refusal::missing_entry;
        break;
    case bundles::ExpansionFault::size:
    case bundles::ExpansionFault::hash:
        problem.refusal = Refusal::damaged;
        break;
    case bundles::ExpansionFault::duplicate:
        problem.refusal = Refusal::case_clash;
        break;
    case bundles::ExpansionFault::unsafe:
        problem.refusal = Refusal::unsafe_name;
        break;
    case bundles::ExpansionFault::ok:
        break;
    }
}

/// The kind of refusal a zip status is.
///
/// @param status the status
/// @return the refusal
Refusal refusal_of(zip::ZipStatus status) noexcept {
    switch (status) {
    case zip::ZipStatus::no_end_record:
    case zip::ZipStatus::several_disks:
    case zip::ZipStatus::bad_central_record:
        return Refusal::not_zip;
    case zip::ZipStatus::encrypted:
        return Refusal::encrypted;
    case zip::ZipStatus::unsupported_method:
        return Refusal::method;
    case zip::ZipStatus::unsafe_name:
        return Refusal::unsafe_name;
    case zip::ZipStatus::duplicate_name:
        return Refusal::case_clash;
    default:
        return Refusal::damaged;
    }
}

/// The first part of an entry name, up to its first '/'.
///
/// @param name the name
/// @return the part
std::string_view first_part(std::string_view name) noexcept {
    const std::size_t slash = name.find('/');
    if (slash == std::string_view::npos)
        return name;
    return name.substr(0, slash);
}

/// Where the manifest sits, and the prefix stripped from every name.
struct LocatedManifest {
    std::string root{}; ///< empty, or the one top folder with its '/'
    const zip::StreamEntry* manifest{};
};

/// Finds oaexp.yaml at the top, or inside the one folder the top holds.
///
/// @param directory the archive
/// @return the manifest; null when it is not there
LocatedManifest locate_manifest(const zip::StreamDirectory& directory) {
    const std::string manifest_name = detail::folded(manifest_file_name);
    LocatedManifest top;
    for (const zip::StreamEntry& entry : directory.entries) {
        if (!entry.directory && detail::folded(entry.name) == manifest_name) {
            top.manifest = &entry;
            return top;
        }
    }
    if (directory.entries.empty())
        return {};
    const std::string_view folder = first_part(directory.entries.front().name);
    if (folder.empty())
        return {};
    const std::string prefix = std::string(folder) + "/";
    for (const zip::StreamEntry& entry : directory.entries) {
        if (entry.name.size() <= prefix.size() || !entry.name.starts_with(prefix))
            return {};
    }
    LocatedManifest nested;
    nested.root = prefix;
    const std::string nested_name = detail::folded(prefix) + manifest_name;
    for (const zip::StreamEntry& entry : directory.entries) {
        if (!entry.directory && detail::folded(entry.name) == nested_name) {
            nested.manifest = &entry;
            return nested;
        }
    }
    return {};
}

/// The source a record stores.
///
/// @param registry_id the registry; empty when the player named a file
/// @param package the package file
/// @param[out] problem a registry id that cannot be stored
/// @param[out] source the source
/// @return false when the registry id cannot be stored
bool source_text(
    std::string_view registry_id, const fs::path& package, Problem& problem, std::string& source
) {
    if (registry_id.empty()) {
        source = detail::utf8_of(package);
        return true;
    }
    if (registry_id.find('\n') != std::string_view::npos ||
        registry_id.find('\r') != std::string_view::npos) {
        problem.refusal = Refusal::manifest_errors;
        problem.lines.push_back("The registry id cannot be stored.");
        return false;
    }
    source = "registry ";
    source += registry_id;
    return true;
}

/// The folder an expansion is copied into.
///
/// @param user_folder the player's folder
/// @param bundle the expansion
/// @param[out] problem why it has no folder
/// @return the folder; empty when it has none
fs::path
expansion_folder(const fs::path& user_folder, const bundles::Bundle& bundle, Problem& problem) {
    if (bundle.extends.empty() || bundle.extends[0].empty() || bundle.id.empty()) {
        problem.refusal = Refusal::manifest_errors;
        problem.detail = std::string(not_an_expansion);
        return {};
    }
    const std::string game(bundle.extends[0]);
    const std::string id(bundle.id);
    if (game.find_first_of("/\\") != std::string::npos ||
        id.find_first_of("/\\") != std::string::npos) {
        problem.refusal = Refusal::unsafe_name;
        problem.subject = id;
        return {};
    }
    return user_folder / std::string(managed_expansions_folder) / game / id;
}

/// Checks measured files and sets `problem` when they are not the table's.
///
/// @param bundle the table
/// @param held the files
/// @param[out] problem the fault
/// @return true when they match
bool files_match(
    const bundles::Bundle& bundle, const std::vector<HeldFile>& held, Problem& problem
) {
    std::vector<bundles::MeasuredExpansionFile> measured;
    measured.reserve(held.size());
    for (const HeldFile& file : held)
        measured.push_back({file.name, file.size, file.digest});
    const bundles::ExpansionFaultReport fault = bundles::match_expansion_files(bundle, measured);
    if (fault.fault == bundles::ExpansionFault::ok)
        return true;
    fault_problem(fault, problem);
    return false;
}

/// The table file of a copy name, compared exactly.
///
/// @param bundle the expansion
/// @param name the copy name
/// @return the file, or null
const bundles::ContentFile*
expected_file(const bundles::Bundle& bundle, std::string_view name) noexcept {
    for (const bundles::ContentFile& file : bundle.files) {
        if (file.copy_name == name)
            return &file;
    }
    return nullptr;
}

/// Reports whether the archive's names and sizes are the table's, before anything is written.
///
/// @param directory the archive
/// @param located where the manifest sits
/// @param bundle the table
/// @param[out] problem the first file that does not fit
/// @return true when every table file is offered once, at the table's size
bool offered_files(
    const zip::StreamDirectory& directory,
    const LocatedManifest& located,
    const bundles::Bundle& bundle,
    Problem& problem
) {
    std::vector<std::string> names;
    for (const zip::StreamEntry& entry : directory.entries) {
        if (entry.directory || &entry == located.manifest)
            continue;
        if (entry.symbolic_link || entry.special_file) {
            problem.refusal = Refusal::link;
            problem.subject = entry.name;
            return false;
        }
        if (entry.name.size() < located.root.size() ||
            entry.name.compare(0, located.root.size(), located.root) != 0) {
            problem.refusal = Refusal::unsafe_name;
            problem.subject = entry.name;
            return false;
        }
        const std::string relative = entry.name.substr(located.root.size());
        if (detail::folded(relative) == detail::folded(manifest_file_name)) {
            problem.refusal = Refusal::stray_file;
            problem.subject = relative;
            return false;
        }
        const bundles::ContentFile* expected = expected_file(bundle, relative);
        if (expected == nullptr) {
            problem.refusal = Refusal::stray_file;
            problem.subject = relative;
            return false;
        }
        if (entry.bytes != expected->size) {
            problem.refusal = Refusal::damaged;
            problem.subject = relative;
            return false;
        }
        for (const std::string& earlier : names) {
            if (earlier == relative) {
                problem.refusal = Refusal::case_clash;
                problem.subject = relative;
                return false;
            }
        }
        names.push_back(relative);
    }
    for (const bundles::ContentFile& expected : bundle.files) {
        bool found = false;
        for (const std::string& name : names) {
            if (name == expected.copy_name) {
                found = true;
                break;
            }
        }
        if (!found) {
            problem.refusal = Refusal::missing_entry;
            problem.subject = std::string(expected.copy_name);
            return false;
        }
    }
    return true;
}

/// Copies the archive's files into `staging` and measures them.
///
/// The manifest is not copied. The names and sizes have already been checked.
///
/// @param archive the open package
/// @param directory its directory
/// @param located where the manifest sits
/// @param staging the folder being filled
/// @param[out] held the files that were copied
/// @param[out] problem why it stopped
/// @return true when every file was copied
bool copy_files(
    detail::PackageFile& archive,
    const zip::StreamDirectory& directory,
    const LocatedManifest& located,
    const fs::path& staging,
    std::vector<HeldFile>& held,
    Problem& problem
) {
    for (const zip::StreamEntry& entry : directory.entries) {
        if (entry.directory || &entry == located.manifest)
            continue;
        if (entry.symbolic_link || entry.special_file) {
            problem.refusal = Refusal::link;
            problem.subject = entry.name;
            return false;
        }
        if (entry.name.size() < located.root.size() ||
            entry.name.compare(0, located.root.size(), located.root) != 0) {
            problem.refusal = Refusal::unsafe_name;
            problem.subject = entry.name;
            return false;
        }
        const std::string relative = entry.name.substr(located.root.size());
        if (detail::folded(relative) == detail::folded(manifest_file_name)) {
            problem.refusal = Refusal::stray_file;
            problem.subject = relative;
            return false;
        }
        const fs::path destination = staging / detail::path_of(relative);
        if (link_on_the_way(staging, destination, problem))
            return false;
        std::error_code created;
        fs::create_directories(destination.parent_path(), created);
        if (created) {
            problem.refusal = Refusal::not_placed;
            problem.detail = created.message();
            return false;
        }
        std::ofstream out(destination, std::ios::binary | std::ios::trunc);
        if (!out) {
            problem.refusal = Refusal::not_placed;
            problem.subject = relative;
            return false;
        }
        sha::Hasher hasher{};
        PieceWriter writer{&out, &hasher};
        zip::EntryStream stream;
        zip::ZipError error;
        if (!stream.open(archive.source(), archive.bytes, entry, error)) {
            problem.refusal = refusal_of(error.status);
            problem.subject = relative;
            problem.detail = zip::zip_status_message(error.status);
            return false;
        }
        const zip::SinkHooks sink{&writer, take_piece};
        zip::StreamStep step = zip::StreamStep::more;
        while (step == zip::StreamStep::more)
            step = stream.step(sink, copy_budget, error);
        out.close();
        if (step != zip::StreamStep::done || writer.failed || writer.written != entry.bytes) {
            problem.refusal = writer.failed ? Refusal::not_placed : refusal_of(error.status);
            problem.subject = relative;
            if (!writer.failed)
                problem.detail = zip::zip_status_message(error.status);
            return false;
        }
        HeldFile file;
        file.name = relative;
        file.size = writer.written;
        file.digest = sha::finish(hasher);
        held.push_back(std::move(file));
    }
    return true;
}

/// Writes the .oa-content record into a folder that already holds the files.
///
/// @param folder the folder
/// @param bundle the expansion
/// @param source the source text
/// @param held the files, in the order they were copied
/// @param[out] problem why the record was not written
/// @return true when the record is in the folder
bool write_record(
    const fs::path& folder,
    const bundles::Bundle& bundle,
    const std::string& source,
    const std::vector<HeldFile>& held,
    Problem& problem
) {
    bundles::ContentRecord record;
    record.bundle_id = std::string(bundle.id);
    record.source = source;
    record.files.reserve(held.size());
    for (const HeldFile& file : held) {
        bundles::ContentRecordFile item;
        item.copy_name = file.name;
        item.size = file.size;
        item.modified = modified_seconds(folder / detail::path_of(file.name));
        item.digest = file.digest;
        record.files.push_back(std::move(item));
    }
    const std::optional<std::string> text = bundles::format_content_record(record);
    if (!text) {
        problem.refusal = Refusal::not_placed;
        return false;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(text->data());
    std::string why;
    if (!oa::platform::replace_file(
            folder / std::string(bundles::content_record_name), {bytes, text->size()}, &why
        )) {
        problem.refusal = Refusal::not_placed;
        problem.detail = why;
        return false;
    }
    return true;
}

/// Puts `staging` in `dest`'s place.
///
/// An existing `dest` is removed only when its record names `id`. A link is
/// refused and left in place. On a failed rename the previous folder is put
/// back.
///
/// @param staging the folder just filled
/// @param user_folder the player's folder `dest` lies in
/// @param dest where the expansion belongs
/// @param id the expansion's id
/// @param[out] problem why it was not put in place
/// @return true when `dest` is the new folder
bool put_in_place(
    const fs::path& staging,
    const fs::path& user_folder,
    const fs::path& dest,
    std::string_view id,
    Problem& problem
) {
    if (link_on_the_way(user_folder, dest, problem) || is_link_or_junction(dest)) {
        if (problem.refusal == Refusal::none)
            problem.refusal = Refusal::link;
        if (problem.subject.empty())
            problem.subject = detail::utf8_of(dest);
        return false;
    }
    std::error_code error;
    fs::create_directories(dest.parent_path(), error);
    if (error) {
        problem.refusal = Refusal::not_placed;
        problem.detail = error.message();
        return false;
    }
    const fs::path aside = dest.parent_path() / (std::string(".oaexp-old-") + std::string(id));
    if (is_link_or_junction(aside)) {
        problem.refusal = Refusal::link;
        problem.subject = detail::utf8_of(aside);
        return false;
    }
    if (fs::exists(aside, error))
        discard_folder(aside);
    const bool replacing = fs::exists(dest, error);
    if (replacing) {
        if (!fs::is_directory(dest, error) || error) {
            problem.refusal = Refusal::not_placed;
            problem.subject = detail::utf8_of(dest);
            return false;
        }
        const bundles::ContentRecord present = bundles::read_bundle_folder_record(dest, nullptr);
        if (!present.valid || present.bundle_id != id) {
            problem.refusal = Refusal::not_placed;
            problem.detail = "The folder already holds something else.";
            return false;
        }
        fs::rename(dest, aside, error);
        if (error) {
            problem.refusal = Refusal::not_placed;
            problem.detail = error.message();
            return false;
        }
    }
    fs::rename(staging, dest, error);
    if (error) {
        std::error_code restore;
        if (replacing)
            fs::rename(aside, dest, restore);
        problem.refusal = Refusal::not_placed;
        problem.detail = error.message();
        return false;
    }
    discard_folder(aside);
    return true;
}

/// Reads a package and copies it when its files match `files`.
///
/// `files` null uses the built-in bundle the manifest names.
///
/// @param package the package file
/// @param user_folder the player's folder
/// @param registry_id the registry; empty for a file the player named
/// @param files the table the bytes must match; null for the built-in one
/// @param[out] installed the folder; may be null
/// @param[out] problem why it was refused
/// @return true when the expansion is in place
bool install_checked(
    const fs::path& package,
    const fs::path& user_folder,
    std::string_view registry_id,
    const bundles::Bundle* files,
    fs::path* installed,
    Problem& problem
) {
    problem = {};
    std::string source;
    if (!source_text(registry_id, package, problem, source))
        return false;
    detail::PackageFile archive;
    if (!archive.open(package)) {
        problem.refusal = Refusal::unreadable;
        problem.detail = "the file cannot be opened";
        return false;
    }
    zip::StreamDirectory directory;
    zip::ZipError error;
    if (!zip::read_stream_directory(archive.source(), archive.bytes, {}, directory, error)) {
        problem.refusal = refusal_of(error.status);
        problem.detail = zip::zip_status_message(error.status);
        return false;
    }
    const LocatedManifest located = locate_manifest(directory);
    if (located.manifest == nullptr) {
        problem.refusal = Refusal::no_manifest;
        return false;
    }
    if (located.manifest->bytes > oa::formats::oamod::max_input_bytes) {
        problem.refusal = Refusal::manifest_too_large;
        return false;
    }
    std::vector<uint8_t> text;
    if (!zip::read_stream_entry(
            archive.source(), archive.bytes, *located.manifest, located.manifest->bytes, text, error
        )) {
        problem.refusal = Refusal::damaged;
        problem.subject = std::string(manifest_file_name);
        problem.detail = zip::zip_status_message(error.status);
        return false;
    }
    bundles::ExpansionManifest manifest;
    std::string why;
    if (!bundles::read_expansion_manifest(text, manifest, &why)) {
        manifest_problem(why, problem);
        return false;
    }
    const bundles::Bundle* table = files == nullptr ? manifest.bundle : files;
    if (table == nullptr || !bundles::expansion_identity_matches(manifest, *table)) {
        problem.refusal = Refusal::manifest_errors;
        problem.detail = std::string(not_an_expansion);
        return false;
    }
    const fs::path dest = expansion_folder(user_folder, *table, problem);
    if (dest.empty())
        return false;
    const fs::path staging =
        dest.parent_path() / (std::string(".oaexp-staging-") + std::string(table->id));
    if (is_link_or_junction(staging)) {
        problem.refusal = Refusal::link;
        problem.subject = detail::utf8_of(staging);
        return false;
    }
    if (!offered_files(directory, located, *table, problem))
        return false;
    discard_folder(staging);
    std::error_code created;
    fs::create_directories(staging, created);
    if (created) {
        problem.refusal = Refusal::not_placed;
        problem.detail = created.message();
        return false;
    }
    std::vector<HeldFile> held;
    if (!copy_files(archive, directory, located, staging, held, problem) ||
        !files_match(*table, held, problem) ||
        !write_record(staging, *table, source, held, problem) ||
        !put_in_place(staging, user_folder, dest, table->id, problem)) {
        discard_folder(staging);
        return false;
    }
    if (installed != nullptr)
        *installed = dest;
    oa::app::note_content_bundles_changed();
    return true;
}

} // namespace

bool read_manifest(
    std::span<const uint8_t> manifest,
    const ManifestContext& context,
    Package& package,
    Problem& problem
) {
    bundles::ExpansionManifest read;
    std::string why;
    if (!bundles::read_expansion_manifest(manifest, read, &why)) {
        manifest_problem(why, problem);
        return false;
    }
    if (read.bundle == nullptr) {
        problem.refusal = Refusal::manifest_errors;
        problem.detail = std::string(not_an_expansion);
        return false;
    }
    std::vector<HeldFile> held;
    for (const PackagedFile& file : package.files) {
        if (file.name.empty() || file.name.back() == '/')
            continue;
        if (detail::folded(file.name) == detail::folded(manifest_file_name))
            continue;
        if (file.entry >= package.directory.entries.size()) {
            problem.refusal = Refusal::damaged;
            problem.subject = file.name;
            return false;
        }
        const zip::StreamEntry& entry = package.directory.entries[file.entry];
        HeldFile item;
        item.name = file.name;
        item.size = entry.bytes;
        const bundles::ContentFile* expected = nullptr;
        for (const bundles::ContentFile& candidate : read.bundle->files) {
            if (candidate.copy_name == item.name) {
                expected = &candidate;
                break;
            }
        }
        if (expected != nullptr && expected->size == item.size) {
            if (context.read_entry == nullptr) {
                problem.refusal = Refusal::unreadable;
                return false;
            }
            std::vector<uint8_t> bytes;
            const EntryRead got = context.read_entry(context.reader, file.name, entry.bytes, bytes);
            if (got != EntryRead::read || bytes.size() != entry.bytes) {
                problem.refusal = Refusal::damaged;
                problem.subject = file.name;
                return false;
            }
            item.digest = sha::digest_of(bytes);
        }
        held.push_back(std::move(item));
    }
    if (!files_match(*read.bundle, held, problem))
        return false;
    package.incoming.id = read.id;
    package.incoming.name = read.name;
    package.incoming.version = read.version;
    package.incoming.revision = read.packaging.revision;
    return true;
}

InstalledPackage read_installed(const fs::path&) {
    InstalledPackage held;
    held.kind = FolderKind::other;
    return held;
}

std::optional<InstalledPackage> read_backup(const fs::path&) {
    return std::nullopt;
}

InstallPlan plan_install(const Incoming& incoming, const FolderHooks&) {
    InstallPlan plan;
    plan.kind = PlanKind::refuse;
    plan.refusal = Refusal::not_placed;
    plan.target = incoming.id;
    return plan;
}

bool install_oaexp(
    const fs::path& package,
    const fs::path& user_folder,
    std::string_view registry_id,
    fs::path* installed,
    Problem& problem
) {
    return install_checked(package, user_folder, registry_id, nullptr, installed, problem);
}

bool install_expansion_package(
    const fs::path& package,
    const fs::path& user_folder,
    std::string_view registry_id,
    const bundles::Bundle& files,
    fs::path* installed,
    Problem& problem
) {
    return install_checked(package, user_folder, registry_id, &files, installed, problem);
}

bool uninstall_expansion(const fs::path& user_folder, std::string_view id, Problem& problem) {
    problem = {};
    const bundles::Bundle* bundle = bundles::find_bundle(id);
    if (bundle == nullptr || bundle->kind != bundles::Kind::expansion) {
        problem.refusal = Refusal::manifest_errors;
        problem.detail = std::string(not_an_expansion);
        return false;
    }
    const fs::path dest = expansion_folder(user_folder, *bundle, problem);
    if (dest.empty())
        return false;
    std::error_code error;
    if (!fs::exists(dest, error))
        return true;
    if (is_link_or_junction(dest)) {
        problem.refusal = Refusal::link;
        problem.subject = detail::utf8_of(dest);
        return false;
    }
    if (!fs::is_directory(dest, error) || error) {
        problem.refusal = Refusal::not_placed;
        problem.subject = detail::utf8_of(dest);
        return false;
    }
    const bundles::ContentRecord present = bundles::read_bundle_folder_record(dest, nullptr);
    if (!present.valid || present.bundle_id != id) {
        problem.refusal = Refusal::not_placed;
        problem.detail = "The folder is not this expansion.";
        return false;
    }
    fs::remove_all(dest, error);
    if (error || fs::exists(dest, error)) {
        problem.refusal = Refusal::not_placed;
        problem.detail = error.message();
        return false;
    }
    oa::app::note_content_bundles_changed();
    return true;
}

} // namespace oa::app::package_install::oaexp
