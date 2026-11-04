// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// An expansion folder becomes a .oaexp, and a .oaexp is checked against a
// table. Files are stored. Bytes the table does not know are not packed and
// are not accepted.

#include "expansion.hpp"

#include "oa/app/package_install.hpp"
#include "oa/app/package_install/oaexp.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/content_bundles/expansion_package.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/zip/stream.hpp"
#include "oa/formats/zip/writer.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::tool {
namespace {

namespace fs = std::filesystem;
namespace bundles = oa::data::content_bundles;
namespace install = oa::app::package_install;
namespace sha = oa::base::sha256;
namespace zip = oa::formats::zip;

/// Bytes read from one file at a time.
constexpr std::size_t read_piece = std::size_t{1} << 20;

/// One file of the folder, already measured.
struct Held {
    std::string name{};
    fs::path path{};
    uint64_t size{};
    sha::Digest digest{};
};

/// A file opened for a positioned read or write.
struct FilePos {
    std::FILE* stream{};
    oa::platform::Files files{};

    FilePos() = default;
    FilePos(const FilePos&) = delete;
    FilePos& operator=(const FilePos&) = delete;

    /// Closes the file.
    ~FilePos() {
        if (stream != nullptr)
            std::fclose(stream);
    }
};

/// Removes a path unless release() was called.
class RemoveUnless {
  public:

    /// Takes a path to remove.
    ///
    /// @param path the path
    explicit RemoveUnless(fs::path path) : path_(std::move(path)) {}

    RemoveUnless(const RemoveUnless&) = delete;
    RemoveUnless& operator=(const RemoveUnless&) = delete;

    /// Removes the path, when it is still held.
    ~RemoveUnless() { remove(); }

    /// Leaves the path in place.
    void release() { path_.clear(); }

  private:

    /// Removes the path now.
    void remove() {
        if (path_.empty())
            return;
        std::error_code error;
        fs::remove(path_, error);
        path_.clear();
    }

    fs::path path_{};
};

/// Returns a path's UTF-8 spelling, with '/' between parts.
///
/// @param path the path
/// @return its UTF-8 spelling
std::string utf8_of(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

/// Returns text with its ASCII letters lowered.
///
/// @param text the text
/// @return the text
std::string folded(std::string_view text) {
    std::string lowered(text);
    for (char& character : lowered)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    return lowered;
}

/// Prints one pack failure.
///
/// @param output where it is printed
/// @param text the reason
void pack_fail(Output& output, std::string_view text) {
    output.err << "oa-tool pack: " << text << '\n';
}

/// Prints one check failure.
///
/// @param output where it is printed
/// @param text the reason
void check_fail(Output& output, std::string_view text) {
    output.err << "oa-tool check: " << text << '\n';
    output.out << "result: refused\n";
}

/// Writes bytes at an offset.
///
/// @param context a FilePos
/// @param offset where the bytes start
/// @param bytes the bytes
/// @return true when they were all written
bool write_at(void* context, uint64_t offset, std::span<const uint8_t> bytes) {
    auto& file = *static_cast<FilePos*>(context);
    auto* handle = reinterpret_cast<oa::platform::FileHandle*>(file.stream);
    if (file.stream == nullptr ||
        offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        file.files.seek(
            nullptr, handle, static_cast<int64_t>(offset), oa::platform::SeekOrigin::begin
        ) != 0)
        return false;
    std::size_t done = 0;
    while (done < bytes.size()) {
        const std::size_t wrote =
            std::fwrite(bytes.data() + done, 1, bytes.size() - done, file.stream);
        if (wrote == 0)
            return false;
        done += wrote;
    }
    return true;
}

/// Reads bytes at an offset.
///
/// @param context a FilePos
/// @param offset where the bytes start
/// @param bytes where they are read into
/// @return true when they were all read
bool read_at(void* context, uint64_t offset, std::span<uint8_t> bytes) {
    auto& file = *static_cast<FilePos*>(context);
    auto* handle = reinterpret_cast<oa::platform::FileHandle*>(file.stream);
    if (file.stream == nullptr ||
        offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        file.files.seek(
            nullptr, handle, static_cast<int64_t>(offset), oa::platform::SeekOrigin::begin
        ) != 0)
        return false;
    std::size_t done = 0;
    while (done < bytes.size()) {
        const std::size_t read =
            std::fread(bytes.data() + done, 1, bytes.size() - done, file.stream);
        if (read == 0)
            return false;
        done += read;
    }
    return true;
}

/// The sentence for a file match that failed.
///
/// @param report the fault
/// @return the sentence
std::string fault_text(const bundles::ExpansionFaultReport& report) {
    switch (report.fault) {
    case bundles::ExpansionFault::stray:
        return "the folder holds " + report.name + ", which the expansion does not have";
    case bundles::ExpansionFault::missing:
        return "the expansion is missing " + report.name;
    case bundles::ExpansionFault::size:
        return "the file " + report.name + " is not the expansion's size";
    case bundles::ExpansionFault::hash:
        return "the file " + report.name + " is not the expansion's file";
    case bundles::ExpansionFault::duplicate:
        return "the folder holds " + report.name + " twice";
    case bundles::ExpansionFault::unsafe:
        return "the folder holds a file it cannot pack: " + report.name;
    case bundles::ExpansionFault::ok:
        break;
    }
    return "the expansion's files do not match";
}

/// Hashes a file of a known size.
///
/// @param path the file
/// @param size its size
/// @param[out] digest the SHA-256
/// @param[out] why why it could not be read
/// @return true when the digest is the whole file
bool hash_file(const fs::path& path, uint64_t size, sha::Digest& digest, std::string& why) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        why = "cannot read " + utf8_of(path);
        return false;
    }
    sha::Hasher hasher{};
    std::vector<uint8_t> piece(read_piece);
    uint64_t left = size;
    while (left > 0) {
        const auto want =
            static_cast<std::size_t>(std::min(left, static_cast<uint64_t>(piece.size())));
        input.read(reinterpret_cast<char*>(piece.data()), static_cast<std::streamsize>(want));
        if (input.gcount() != static_cast<std::streamsize>(want)) {
            why = "cannot read " + utf8_of(path);
            return false;
        }
        sha::update(hasher, std::span<const uint8_t>(piece.data(), want));
        left -= want;
    }
    digest = sha::finish(hasher);
    return true;
}

/// Reports whether a relative name is safe to pack.
///
/// @param name the name, '/' between parts
/// @return true when it has no empty, dot or parent part and no backslash
bool safe_relative(std::string_view name) {
    if (name.empty() || name.front() == '/' || name.back() == '/' ||
        name.find('\\') != std::string_view::npos)
        return false;
    std::size_t start = 0;
    while (start <= name.size()) {
        const auto slash = name.find('/', start);
        const std::string_view part = name.substr(start, slash - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    return true;
}

/// Walks the folder and measures every file except the manifest.
///
/// @param folder the folder
/// @param[out] manifest_path where oaexp.yaml is
/// @param[out] files the other files
/// @param[out] why why the walk stopped
/// @return true when the folder was walked
bool collect_folder(
    const fs::path& folder, fs::path& manifest_path, std::vector<Held>& files, std::string& why
) {
    std::error_code error;
    if (install::is_link_or_junction(folder)) {
        why = "the folder is a link";
        return false;
    }
    if (!fs::is_directory(folder, error) || error) {
        why = "the folder is not a folder";
        return false;
    }
    int manifests = 0;
    fs::recursive_directory_iterator cursor(folder, fs::directory_options::none, error);
    if (error) {
        why = "cannot read the folder: " + error.message();
        return false;
    }
    const fs::recursive_directory_iterator end;
    while (cursor != end) {
        const fs::directory_entry& entry = *cursor;
        const fs::path relative = entry.path().lexically_relative(folder);
        const std::string name = utf8_of(relative);
        if (install::is_link_or_junction(entry.path())) {
            why = "the folder holds a link: " + name;
            return false;
        }
        const fs::file_status status = entry.symlink_status(error);
        if (error) {
            why = "cannot read " + name + ": " + error.message();
            return false;
        }
        if (fs::is_directory(status)) {
            cursor.increment(error);
            if (error) {
                why = "cannot read the folder: " + error.message();
                return false;
            }
            continue;
        }
        if (!fs::is_regular_file(status)) {
            why = "the folder holds a special file: " + name;
            return false;
        }
        if (!safe_relative(name)) {
            why = "the folder holds a file it cannot pack: " + name;
            return false;
        }
        if (folded(name) == "oaexp.yaml") {
            if (name.find('/') != std::string::npos) {
                why = "oaexp.yaml is not at the top of the folder";
                return false;
            }
            ++manifests;
            manifest_path = entry.path();
        } else {
            const auto size = entry.file_size(error);
            if (error) {
                why = "cannot read " + name + ": " + error.message();
                return false;
            }
            Held held;
            held.name = name;
            held.path = entry.path();
            held.size = static_cast<uint64_t>(size);
            if (!hash_file(held.path, held.size, held.digest, why))
                return false;
            files.push_back(std::move(held));
        }
        cursor.increment(error);
        if (error) {
            why = "cannot read the folder: " + error.message();
            return false;
        }
    }
    if (manifests != 1) {
        why = manifests == 0 ? "the folder holds no oaexp.yaml at its top"
                             : "the folder holds more than one oaexp.yaml";
        return false;
    }
    return true;
}

/// Reads a whole file that is at most the manifest's limit.
///
/// @param path the file
/// @param[out] bytes the bytes
/// @param[out] why why it was not read
/// @return true when the bytes were read
bool read_manifest_file(const fs::path& path, std::vector<uint8_t>& bytes, std::string& why) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error) {
        why = "cannot read oaexp.yaml: " + error.message();
        return false;
    }
    if (size > oa::formats::oamod::max_input_bytes) {
        why = "oaexp.yaml is larger than 256 KiB";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    bytes.resize(static_cast<std::size_t>(size));
    if (size > 0 &&
        !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        why = "cannot read oaexp.yaml";
        return false;
    }
    return true;
}

/// Writes one stored file into the archive.
///
/// @param writer the archive
/// @param held the file
/// @param[out] why why it was not written
/// @return true when the entry was written
bool write_held(zip::StreamWriter& writer, const Held& held, std::string& why) {
    zip::ZipError error{};
    if (!writer.begin_entry(held.name, held.size, zip::Method::stored, error)) {
        why = zip::zip_status_message(error.status);
        return false;
    }
    if (held.size > 0) {
        std::ifstream input(held.path, std::ios::binary);
        std::vector<uint8_t> piece(read_piece);
        uint64_t left = held.size;
        while (left > 0) {
            const auto want =
                static_cast<std::size_t>(std::min(left, static_cast<uint64_t>(piece.size())));
            input.read(reinterpret_cast<char*>(piece.data()), static_cast<std::streamsize>(want));
            if (input.gcount() != static_cast<std::streamsize>(want) ||
                !writer.write(std::span<const uint8_t>(piece.data(), want), error)) {
                why = "cannot read " + held.name;
                return false;
            }
            left -= want;
        }
    }
    if (!writer.end_entry(error)) {
        why = zip::zip_status_message(error.status);
        return false;
    }
    return true;
}

/// Renames part onto the output. On Windows a file that exists is removed
/// first, because a rename there does not replace one.
///
/// @param part the complete package
/// @param output where it belongs
/// @param[out] why why it was not renamed
/// @return true when the output is the package
bool replace_output(const fs::path& part, const fs::path& output, std::string& why) {
#ifdef _WIN32
    std::error_code removed;
    fs::remove(output, removed);
#endif
    std::error_code error;
    fs::rename(part, output, error);
    if (error) {
        why = "cannot replace " + utf8_of(output) + ": " + error.message();
        return false;
    }
    return true;
}

/// Where the manifest sits in an archive, and the prefix stripped from names.
struct Located {
    std::string root{};
    const zip::StreamEntry* manifest{};
};

/// Finds oaexp.yaml at the top, or inside the one folder the top holds.
///
/// @param directory the archive
/// @return the manifest; null when it is not there
Located locate_manifest(const zip::StreamDirectory& directory) {
    const std::string manifest_name = folded(install::oaexp::manifest_file_name);
    Located top;
    for (const zip::StreamEntry& entry : directory.entries) {
        if (!entry.directory && folded(entry.name) == manifest_name) {
            top.manifest = &entry;
            return top;
        }
    }
    if (directory.entries.empty())
        return {};
    const auto slash = directory.entries.front().name.find('/');
    if (slash == std::string::npos || slash == 0)
        return {};
    const std::string prefix = directory.entries.front().name.substr(0, slash + 1);
    for (const zip::StreamEntry& entry : directory.entries) {
        if (entry.name.size() <= prefix.size() || !entry.name.starts_with(prefix))
            return {};
    }
    Located nested;
    nested.root = prefix;
    const std::string nested_name = folded(prefix) + manifest_name;
    for (const zip::StreamEntry& entry : directory.entries) {
        if (!entry.directory && folded(entry.name) == nested_name)
            nested.manifest = &entry;
    }
    return nested;
}

/// Takes the next piece of an entry into a hasher.
///
/// @param context a HashSink
/// @param piece the bytes
/// @return true
bool take_hash(void* context, std::span<const uint8_t> piece) {
    auto* sink = static_cast<std::pair<sha::Hasher*, uint64_t*>*>(context);
    sha::update(*sink->first, piece);
    *sink->second += piece.size();
    return true;
}

} // namespace

int pack_expansion(
    const fs::path& folder,
    const bundles::Bundle* files,
    const std::string& out_path,
    bool force,
    Output& output
) {
    fs::path manifest_path;
    std::vector<Held> held;
    std::string why;
    if (!collect_folder(folder, manifest_path, held, why)) {
        pack_fail(output, why);
        return exit_failed;
    }
    std::vector<uint8_t> manifest_bytes;
    if (!read_manifest_file(manifest_path, manifest_bytes, why)) {
        pack_fail(output, why);
        return exit_failed;
    }
    bundles::ExpansionManifest manifest;
    if (!bundles::read_expansion_manifest(manifest_bytes, manifest, &why)) {
        pack_fail(output, why.empty() ? "oaexp.yaml has errors" : why);
        return exit_failed;
    }
    const bundles::Bundle* table = files == nullptr ? manifest.bundle : files;
    if (table == nullptr || !bundles::expansion_identity_matches(manifest, *table)) {
        pack_fail(output, "It is not one of the game's expansions.");
        return exit_failed;
    }
    std::vector<bundles::MeasuredExpansionFile> measured;
    measured.reserve(held.size());
    for (const Held& file : held)
        measured.push_back({file.name, file.size, file.digest});
    const bundles::ExpansionFaultReport report = bundles::match_expansion_files(*table, measured);
    if (report.fault != bundles::ExpansionFault::ok) {
        pack_fail(output, fault_text(report));
        return exit_failed;
    }

    fs::path destination;
    if (!out_path.empty())
        destination = fs::path(std::u8string(out_path.begin(), out_path.end()));
    else
        destination = manifest.id + ".oaexp";
    const std::string destination_name = folded(utf8_of(destination.filename()));
    if (!destination_name.ends_with(".oaexp") ||
        destination_name.size() == std::string(".oaexp").size()) {
        pack_fail(output, "the output must be a .oaexp file");
        return exit_failed;
    }
    std::error_code error;
    if (!destination.parent_path().empty() &&
        !fs::is_directory(fs::symlink_status(destination.parent_path(), error))) {
        pack_fail(output, "cannot write " + utf8_of(destination));
        return exit_failed;
    }
    const fs::file_status existing = fs::symlink_status(destination, error);
    if (!error && fs::exists(existing)) {
        if (fs::is_directory(existing) || install::is_link_or_junction(destination)) {
            pack_fail(output, "the output is a folder: " + utf8_of(destination));
            return exit_failed;
        }
        if (!force) {
            pack_fail(output, "the output already exists: " + utf8_of(destination));
            return exit_failed;
        }
    }

    std::sort(held.begin(), held.end(), [](const Held& left, const Held& right) {
        return left.name < right.name;
    });
    fs::path part = destination;
    part += ".part";
    RemoveUnless part_file(part);
    FilePos written;
    written.stream = oa::platform::open_file(part, "wb");
    written.files = oa::platform::stdio_files();
    if (written.stream == nullptr) {
        pack_fail(output, "cannot write " + utf8_of(part));
        return exit_failed;
    }
    zip::StreamWriter writer({&written, write_at});
    Held manifest_entry;
    manifest_entry.name = "oaexp.yaml";
    manifest_entry.path = manifest_path;
    manifest_entry.size = manifest_bytes.size();
    if (!write_held(writer, manifest_entry, why)) {
        pack_fail(output, why);
        return exit_failed;
    }
    for (const Held& file : held) {
        if (!write_held(writer, file, why)) {
            pack_fail(output, why);
            return exit_failed;
        }
    }
    zip::ZipError zip_error{};
    if (!writer.finish(zip_error) || std::fflush(written.stream) != 0) {
        pack_fail(output, "cannot write " + utf8_of(part));
        return exit_failed;
    }
    // Close before the rename. A rename onto an open file fails on Windows.
    // Null the pointer so the destructor does not close it again.
    if (std::fclose(written.stream) != 0) {
        written.stream = nullptr;
        pack_fail(output, "cannot write " + utf8_of(part));
        return exit_failed;
    }
    written.stream = nullptr;
    if (!replace_output(part, destination, why)) {
        pack_fail(output, why);
        return exit_failed;
    }
    part_file.release();
    output.out << "packed " << utf8_of(destination) << ": " << (held.size() + 1) << " files, "
               << writer.bytes_written() << " bytes\n";
    return exit_done;
}

int check_expansion_package(const fs::path& package, const bundles::Bundle* files, Output& output) {
    if (install::is_link_or_junction(package)) {
        check_fail(output, "the file is a link");
        return exit_failed;
    }
    FilePos archive;
    archive.stream = oa::platform::open_file(package, "rb");
    archive.files = oa::platform::stdio_files();
    if (archive.stream == nullptr) {
        check_fail(output, "the file cannot be opened");
        return exit_failed;
    }
    std::error_code error;
    const auto size = fs::file_size(package, error);
    if (error) {
        check_fail(output, "the file cannot be opened");
        return exit_failed;
    }
    zip::StreamDirectory directory;
    zip::ZipError zip_error{};
    if (!zip::read_stream_directory(
            {&archive, read_at}, static_cast<uint64_t>(size), {}, directory, zip_error
        )) {
        check_fail(output, zip::zip_status_message(zip_error.status));
        return exit_failed;
    }
    const Located located = locate_manifest(directory);
    if (located.manifest == nullptr) {
        check_fail(output, "It holds no oaexp.yaml at its top, or in a single folder at its top.");
        return exit_failed;
    }
    if (located.manifest->bytes > oa::formats::oamod::max_input_bytes) {
        check_fail(output, "Its oaexp.yaml is larger than 256 KiB.");
        return exit_failed;
    }
    std::vector<uint8_t> text;
    if (!zip::read_stream_entry(
            {&archive, read_at},
            static_cast<uint64_t>(size),
            *located.manifest,
            located.manifest->bytes,
            text,
            zip_error
        )) {
        check_fail(output, "Its oaexp.yaml has errors.");
        return exit_failed;
    }
    bundles::ExpansionManifest manifest;
    std::string why;
    if (!bundles::read_expansion_manifest(text, manifest, &why)) {
        check_fail(output, why.empty() ? "Its oaexp.yaml has errors." : why);
        return exit_failed;
    }
    const bundles::Bundle* table = files == nullptr ? manifest.bundle : files;
    if (table == nullptr || !bundles::expansion_identity_matches(manifest, *table)) {
        check_fail(output, "It is not one of the game's expansions.");
        return exit_failed;
    }

    struct Measured {
        std::string name{};
        uint64_t size{};
        sha::Digest digest{};
    };

    std::vector<Measured> held;
    for (const zip::StreamEntry& entry : directory.entries) {
        if (entry.directory || &entry == located.manifest)
            continue;
        if (entry.symbolic_link || entry.special_file) {
            check_fail(output, "the package holds a link: " + entry.name);
            return exit_failed;
        }
        if (entry.name.size() < located.root.size() ||
            entry.name.compare(0, located.root.size(), located.root) != 0) {
            check_fail(output, "the package holds a file it cannot unpack: " + entry.name);
            return exit_failed;
        }
        Measured item;
        item.name = entry.name.substr(located.root.size());
        item.size = entry.bytes;
        sha::Hasher hasher{};
        uint64_t written = 0;
        std::pair<sha::Hasher*, uint64_t*> sink{&hasher, &written};
        zip::EntryStream stream;
        if (!stream.open({&archive, read_at}, static_cast<uint64_t>(size), entry, zip_error)) {
            check_fail(output, "the file " + item.name + " cannot be read");
            return exit_failed;
        }
        const zip::SinkHooks hooks{&sink, take_hash};
        zip::StreamStep step = zip::StreamStep::more;
        while (step == zip::StreamStep::more)
            step = stream.step(hooks, uint64_t{1} << 20, zip_error);
        if (step != zip::StreamStep::done || written != entry.bytes) {
            check_fail(output, "the file " + item.name + " cannot be read");
            return exit_failed;
        }
        item.digest = sha::finish(hasher);
        held.push_back(std::move(item));
    }
    std::vector<bundles::MeasuredExpansionFile> measured;
    measured.reserve(held.size());
    for (const Measured& item : held)
        measured.push_back({item.name, item.size, item.digest});
    const bundles::ExpansionFaultReport report = bundles::match_expansion_files(*table, measured);
    if (report.fault != bundles::ExpansionFault::ok) {
        check_fail(output, fault_text(report));
        return exit_failed;
    }
    output.out << "result: ok\n";
    return exit_done;
}

int run_expansions(std::span<const std::string> arguments, Output& output) {
    if (!arguments.empty()) {
        output.err << "oa-tool expansions: the command takes no arguments\n";
        output.err << "run 'oa-tool help expansions'\n";
        return exit_usage;
    }
    for (const bundles::Bundle& bundle : bundles::bundles()) {
        if (bundle.kind != bundles::Kind::expansion)
            continue;
        output.out << bundle.id << '\t' << bundle.files.size() << '\n';
    }
    return exit_done;
}

} // namespace oa::tool
