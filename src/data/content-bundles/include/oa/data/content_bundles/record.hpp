// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The .oa-content record of one copied bundle. The text is version 1:
// "oa-content 1", then "bundle <id>", then "source <path>", then, for an
// expansion installed from a package, "package <sha256> <release>", then
// one group per file of "file", "size", "modified" and "sha256". A record
// of any other bundle never holds a package line, and a reader that does
// not know the line skips it as a field it does not know. A file the source
// did not hold follows that group with "not-in-source", and, when the source
// held a file of that name at another size, "source-size <bytes>" after it.
// A record written before those lines existed has no such line, and the
// file reads as held.
// The source escapes backslash, newline and carriage return. A line whose
// first word is not one of those fields is kept for a later reader and
// ignored here. The parser takes bytes and returns a record; it does not
// open a file. A bundle is played only once its record names every one of
// its files.
#pragma once

#include "oa/base/sha256.hpp"
#include "oa/data/content_bundles.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::content_bundles {

/// The file name of a bundle folder's record.
inline constexpr std::string_view content_record_name = ".oa-content";

/// The folder of base games under the player's folder: `Games/<id>`.
inline constexpr std::string_view managed_games_folder = "Games";

/// The folder of expansions under the player's folder:
/// `Expansions/<base>/<id>`.
inline constexpr std::string_view managed_expansions_folder = "Expansions";

/// One file recorded in a bundle folder.
struct ContentRecordFile {
    /// The name it is stored under, relative to the bundle folder.
    std::string copy_name;
    /// Size in bytes.
    uint64_t size{};
    /// Modification time, seconds since 1970.
    int64_t modified{};
    /// SHA-256 of the bytes.
    base::sha256::Digest digest{};
    /// The source did not hold this file. A record with no such line reads
    /// this as false: the file was held.
    bool not_in_source{};
    /// When `not_in_source`, the size in bytes of the file of this name the
    /// source held instead, which is not the table's size; 0 when the source
    /// held no file of this name.
    uint64_t source_size{};
};

/// The package an expansion's files were installed from.
struct ContentRecordPackage {
    /// SHA-256 of the whole package file, as a catalogue lists it.
    base::sha256::Digest sha256{};
    /// The catalogue's release of that package, from 1; 0 for a package
    /// file the player opened.
    int64_t release{};
};

/// The highest release a package line holds, as a catalogue numbers them.
inline constexpr int64_t content_record_release_limit = 2147483647;

/// A bundle folder's record, as text or as read from the folder.
///
/// `present`, `valid` and `io_error` describe a file that was opened.
/// `parse_content_record` leaves them false and sets `valid` only.
struct ContentRecord {
    /// True when the file exists.
    bool present{};
    /// True when the bytes are a record this reader understands.
    bool valid{};
    /// True when the file exists and could not be read.
    bool io_error{};
    /// The bundle id the record names.
    std::string bundle_id;
    /// The source the files were copied from, as the player named it.
    std::string source;
    /// The package an expansion was installed from. Nothing for every other
    /// bundle, and for an expansion installed before the line was written.
    std::optional<ContentRecordPackage> package{};
    /// The files recorded so far.
    std::vector<ContentRecordFile> files;
};

/// Reads a record from its text.
///
/// A record this reader does not understand is not valid, and its fields
/// are empty. A field this reader does not know is skipped. A package line
/// that is not a SHA-256 and a release from 0 to
/// content_record_release_limit, and every package line after the first,
/// are skipped too: the record then names no package.
///
/// @param text the record's bytes, as text
/// @return the record; `valid` is false when the text is not a record
[[nodiscard]] ContentRecord parse_content_record(std::string_view text);

/// Writes a record as text.
///
/// An empty file list is still a record. A bundle id or a copy name that
/// holds a newline or a carriage return cannot be written. The package line
/// is written only when `package` is set, and a release outside 0 to
/// content_record_release_limit cannot be written.
///
/// @param record the record; `present`, `valid` and `io_error` are not written
/// @return the text, or nothing when a name cannot be stored on one line
[[nodiscard]] std::optional<std::string> format_content_record(const ContentRecord& record);

/// Reads the record in a bundle folder.
///
/// A missing file is an empty record with `present` false. A file that is
/// not a record is present and not valid. A file that cannot be read, or
/// that is larger than a record is allowed to be, sets `io_error`.
///
/// @param folder the bundle folder
/// @param[out] error why it could not be read; may be null; unset otherwise
/// @return the record
[[nodiscard]] ContentRecord
read_bundle_folder_record(const std::filesystem::path& folder, std::string* error);

/// Lists the files of a bundle that its record does not name.
///
/// The record names a file once the copy has stored it, or has recorded it
/// as not in the source. Names are compared without regard to ASCII case.
/// A copy that stopped part way has not named the files after the one it
/// stopped at, and a copy whose source holds a file under the file's name
/// at another size does not name that file.
///
/// @param bundle the bundle
/// @param record the record in the bundle's folder
/// @return the copy names of the files not named, in the bundle's order
[[nodiscard]] std::vector<std::string_view>
files_not_recorded(const Bundle& bundle, const ContentRecord& record);

/// Tells whether a record is the bundle's own and names all its files.
///
/// Only such a bundle is played. A bundle with no files never is.
///
/// @param bundle the bundle
/// @param record the record in the bundle's folder
/// @return true when the record is valid, names this bundle and names every
///     one of its files
[[nodiscard]] bool record_complete(const Bundle& bundle, const ContentRecord& record);

/// What an expansion's record says about the package a catalogue offers.
enum class PackageCheck : uint8_t {
    current,    ///< the record names that package: nothing is fetched or installed
    missing,    ///< no complete copy of the expansion: the package is fetched
    unrecorded, ///< a complete copy whose record names no package: fetched once, then recorded
    changed,    ///< the record names another package: the offered one is fetched
};

/// Compares an expansion's record with the package a catalogue offers.
///
/// Only the record's package line and the package's SHA-256 decide: the
/// SHA-256 of an archive in the folder is never the package's, since the
/// package is the whole `.oaexp` that holds it.
///
/// @param bundle the expansion
/// @param record the record in the expansion's folder
/// @param offered the SHA-256 of the package the catalogue lists
/// @return current only when the record is complete and names that package
[[nodiscard]] PackageCheck check_installed_package(
    const Bundle& bundle, const ContentRecord& record, const base::sha256::Digest& offered
);

} // namespace oa::data::content_bundles
