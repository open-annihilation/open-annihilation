// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// pack: a folder with one manifest becomes a .oamod, a .oalang, a .oamap,
// a .oaexp, a .oamut or a .oabrain.
// A mod or a language is written here. A map is handed to pack_map. The
// manifest is written first, then every other file in byte order of its
// path, stored or deflated by a fixed rule, with the writer's fixed times.
// The same files therefore give the same bytes. The package is written to
// a sibling .part file and renamed into place only when the archive is
// complete; a refusal deletes that result.

#include "arguments.hpp"
#include "check_brain.hpp"
#include "check_mutator.hpp"
#include "command.hpp"
#include "expansion.hpp"
#include "pack_map.hpp"
#include "pack_names.hpp"

#include "oa/app/package_install.hpp"
#include "oa/app/package_install/oamod.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/languages/language_pack.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/zip/stream.hpp"
#include "oa/formats/zip/writer.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::tool {
namespace {

namespace fs = std::filesystem;
namespace zip = oa::formats::zip;
namespace install = oa::app::package_install;
namespace languages = oa::data::languages;
namespace sha256 = oa::base::sha256;

/// Bytes read from one file and handed to the writer at a time.
constexpr std::size_t read_piece = std::size_t{1} << 20;

/// Which package a folder's manifest names.
enum class Kind : uint8_t {
    mod,      ///< oamod.yaml
    language, ///< language.yaml
};

/// How an entry the walk met is left out of the package.
enum class Omit : uint8_t {
    none,  ///< packed
    entry, ///< this entry is left out; a folder is still entered
    tree,  ///< this entry and everything under it are left out
};

using Item = detail::Item;

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

    /// Removes the path now.
    void remove() {
        if (path_.empty())
            return;
        std::error_code error;
        fs::remove(path_, error);
        path_.clear();
    }

  private:

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

/// Returns a text with its ASCII letters lowered.
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

/// Tells whether two texts are equal with ASCII letters folded.
///
/// @param left one text
/// @param right the other
/// @return true when they are equal
bool same_ascii(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](unsigned char byte) {
            return byte >= 'A' && byte <= 'Z' ? static_cast<unsigned char>(byte - 'A' + 'a') : byte;
        };
        if (fold(static_cast<unsigned char>(left[index])) !=
            fold(static_cast<unsigned char>(right[index])))
            return false;
    }
    return true;
}

/// Tells whether left sorts before right in unsigned byte order.
///
/// @param left one path
/// @param right the other
/// @return true when left comes first
bool bytes_before(std::string_view left, std::string_view right) {
    const auto* left_bytes = reinterpret_cast<const uint8_t*>(left.data());
    const auto* right_bytes = reinterpret_cast<const uint8_t*>(right.data());
    return std::lexicographical_compare(
        left_bytes, left_bytes + left.size(), right_bytes, right_bytes + right.size()
    );
}

/// Returns the name an item has in the package. A folder's ends in '/'.
///
/// @param item the item
/// @return the name
std::string entry_name(const Item& item) {
    std::string name = item.name;
    if (item.folder)
        name.push_back('/');
    return name;
}

/// Returns the first '/'-separated part of a relative path.
///
/// @param path the path
/// @return its first part
std::string_view first_part(std::string_view path) {
    return path.substr(0, path.find('/'));
}

/// Returns the last '/'-separated part of a relative path.
///
/// @param path the path
/// @return its last part
std::string_view last_part(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/// Tells whether any part of a path is `folded_name`.
///
/// @param path the path, '/' between parts
/// @param folded_name the part, already folded
/// @return true when a part is that name
bool any_part_is(std::string_view path, std::string_view folded_name) {
    std::size_t start = 0;
    while (start < path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string_view part = slash == std::string_view::npos
                                          ? path.substr(start)
                                          : path.substr(start, slash - start);
        if (folded(part) == folded_name)
            return true;
        if (slash == std::string_view::npos)
            return false;
        start = slash + 1;
    }
    return false;
}

/// Says whether a relative path is one the package leaves out.
///
/// __MACOSX and .backup are left out only as the first part, which is where
/// the installer leaves them out. .DS_Store and AppleDouble names are left
/// out at any depth. .git and .oa-origin.yaml are left out at any depth:
/// a package never carries them.
///
/// @param relative the path inside the folder
/// @return how it is left out
Omit omit_of(std::string_view relative) {
    const std::string first = folded(first_part(relative));
    if (first == "__macosx" || first == ".backup")
        return Omit::tree;
    if (any_part_is(relative, ".git") || any_part_is(relative, ".oa-origin.yaml"))
        return Omit::tree;
    const std::string last = folded(last_part(relative));
    if (last == ".ds_store" || last.starts_with("._"))
        return Omit::entry;
    return Omit::none;
}

/// Returns the path of an entry inside the folder, or refuses one that is not.
///
/// @param folder the folder being packed
/// @param path the entry's path
/// @return the relative path, '/' between parts
std::string relative_of(const fs::path& folder, const fs::path& path) {
    const std::string text = utf8_of(path.lexically_relative(folder));
    if (text.empty() || text == "." || text == ".." || text.starts_with("../") ||
        text.starts_with("/"))
        throw Failure("the folder holds a path outside it");
    return text;
}

/// Tells whether a file's extension is one that is stored, without case.
///
/// A name whose only dot is its first character has no extension.
///
/// @param name the file's path inside the package
/// @return true for the stored extensions
bool stored_extension(std::string_view name) {
    const std::string_view base = last_part(name);
    const std::size_t dot = base.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= base.size())
        return false;
    const std::string extension = folded(base.substr(dot + 1));
    static constexpr std::string_view stored[] = {
        "png",
        "jpg",
        "jpeg",
        "gif",
        "ogg",
        "mp3",
        "zip",
        "gz",
        "bz2",
        "xz",
        "7z",
        "oamod",
        "oalang",
        "oamap",
    };
    for (const std::string_view one : stored)
        if (extension == one)
            return true;
    return false;
}

/// Returns how an item is stored. Empty files and the stored extensions are
/// stored; every other file is deflated. A folder is stored.
///
/// @param item the item
/// @return the method
zip::Method method_of(const Item& item) {
    if (item.folder || item.bytes == 0 || stored_extension(item.name))
        return zip::Method::stored;
    return zip::Method::deflated;
}

/// Writes bytes at an offset, seeking with the 64-bit file boundary.
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

/// Returns a zip failure's text.
///
/// @param error the error
/// @return the text
std::string zip_failure(const zip::ZipError& error) {
    std::string text = zip::zip_status_message(error.status);
    if (!error.entry.empty()) {
        text += ": ";
        text += error.entry;
    }
    return text;
}

/// Opens a file for reading and measures it.
///
/// @param path the file
/// @param[out] file receives the open file and its size
/// @return the size
uint64_t open_read(const fs::path& path, FilePos& file) {
    file.stream = oa::platform::open_file(path, "rb");
    if (file.stream == nullptr)
        throw Failure("cannot read " + utf8_of(path));
    file.files = oa::platform::stdio_files();
    auto* handle = reinterpret_cast<oa::platform::FileHandle*>(file.stream);
    if (file.files.seek(nullptr, handle, 0, oa::platform::SeekOrigin::end) != 0)
        throw Failure("cannot read " + utf8_of(path));
    const int64_t end = file.files.tell(nullptr, handle);
    if (end < 0)
        throw Failure("cannot read " + utf8_of(path));
    if (file.files.seek(nullptr, handle, 0, oa::platform::SeekOrigin::begin) != 0)
        throw Failure("cannot read " + utf8_of(path));
    return static_cast<uint64_t>(end);
}

/// Reads a whole file that is at most `most` bytes.
///
/// @param path the file
/// @param most the most bytes it may hold
/// @param what the name used when it is refused
/// @return its bytes
std::vector<uint8_t> read_capped(const fs::path& path, uint64_t bytes, std::string_view what) {
    if (bytes > oa::formats::oamod::max_input_bytes)
        throw Failure(std::string(what) + " is larger than 256 KiB");
    FilePos file;
    const uint64_t measured = open_read(path, file);
    if (measured != bytes)
        throw Failure("cannot read " + std::string(what));
    std::vector<uint8_t> buffer(static_cast<std::size_t>(measured));
    if (measured > 0 && std::fread(buffer.data(), 1, buffer.size(), file.stream) != buffer.size())
        throw Failure("cannot read " + std::string(what));
    return buffer;
}

/// Prints each diagnostic on its own line.
///
/// @param output where they are printed
/// @param diagnostics the diagnostics
void print_diagnostics(
    Output& output, const std::vector<oa::data::mod_profile::Diagnostic>& diagnostics
) {
    for (const auto& diagnostic : diagnostics)
        output.err << oa::data::mod_profile::format_diagnostic(diagnostic) << '\n';
}

/// Walks the folder, refusing a link or a special file and leaving out the
/// names a package does not carry.
///
/// @param folder the folder
/// @return the files and folders it holds, empty folders included
std::vector<Item> collect(const fs::path& folder) {
    std::error_code error;
    const fs::file_status root = fs::symlink_status(folder, error);
    if (error)
        throw Failure("cannot read the folder: " + error.message());
    if (install::is_link_or_junction(folder))
        throw Failure("the folder is a link");
    if (!fs::is_directory(root))
        throw Failure("the folder is not a folder");

    std::vector<Item> found;
    fs::recursive_directory_iterator cursor(folder, fs::directory_options::none, error);
    if (error)
        throw Failure("cannot read the folder: " + error.message());
    const fs::recursive_directory_iterator end;
    while (cursor != end) {
        const fs::directory_entry& entry = *cursor;
        const std::string relative = relative_of(folder, entry.path());
        if (install::is_link_or_junction(entry.path()))
            throw Failure("the folder holds a link: " + relative);
        const fs::file_status status = entry.symlink_status(error);
        if (error)
            throw Failure("cannot read " + relative + ": " + error.message());
        const bool directory = fs::is_directory(status);
        if (!directory && !fs::is_regular_file(status))
            throw Failure("the folder holds a special file: " + relative);
        const Omit omit = omit_of(relative);
        if (omit == Omit::tree)
            cursor.disable_recursion_pending();
        if (omit == Omit::none) {
            Item item{};
            item.name = relative;
            item.path = entry.path();
            item.folder = directory;
            if (!directory) {
                const auto size = entry.file_size(error);
                if (error)
                    throw Failure("cannot read " + relative + ": " + error.message());
                item.bytes = static_cast<uint64_t>(size);
            }
            found.push_back(std::move(item));
        }
        cursor.increment(error);
        if (error)
            throw Failure("cannot read the folder: " + error.message());
    }

    std::vector<Item> kept;
    kept.reserve(found.size());
    for (const Item& item : found) {
        if (!item.folder) {
            kept.push_back(item);
            continue;
        }
        const std::string prefix = item.name + "/";
        bool child = false;
        for (const Item& other : found) {
            if (other.name.size() > prefix.size() && other.name.starts_with(prefix)) {
                child = true;
                break;
            }
        }
        if (!child)
            kept.push_back(item);
    }
    return kept;
}

/// Renames part onto output. On Windows a file that exists is removed first,
/// because a rename there does not replace one.
///
/// @param part the complete package
/// @param output where it belongs
void replace_output(const fs::path& part, const fs::path& output) {
#ifdef _WIN32
    std::error_code removed;
    fs::remove(output, removed);
#endif
    std::error_code error;
    fs::rename(part, output, error);
    if (error)
        throw Failure("cannot replace " + utf8_of(output) + ": " + error.message());
}

/// Writes one item into the archive.
///
/// @param writer the archive
/// @param item the item
void write_item(zip::StreamWriter& writer, const Item& item) {
    zip::ZipError error{};
    const std::string name = entry_name(item);
    if (item.folder) {
        if (!writer.add_folder(name, error))
            throw Failure(zip_failure(error));
        return;
    }
    if (!writer.begin_entry(name, item.bytes, method_of(item), error))
        throw Failure(zip_failure(error));
    if (item.bytes > 0) {
        FilePos input;
        open_read(item.path, input);
        std::vector<uint8_t> piece(std::min(item.bytes, static_cast<uint64_t>(read_piece)));
        uint64_t left = item.bytes;
        while (left > 0) {
            const std::size_t want =
                static_cast<std::size_t>(std::min(left, static_cast<uint64_t>(piece.size())));
            if (std::fread(piece.data(), 1, want, input.stream) != want)
                throw Failure("cannot read " + item.name);
            if (!writer.write(std::span<const uint8_t>(piece.data(), want), error))
                throw Failure(zip_failure(error));
            left -= want;
        }
    }
    if (!writer.end_entry(error))
        throw Failure(zip_failure(error));
}

/// Writes the package to part and returns how many bytes it holds.
///
/// @param part the .part path
/// @param items the items, manifest first
/// @return the archive's size
uint64_t write_part(const fs::path& part, const std::vector<Item>& items) {
    FilePos output;
    output.stream = oa::platform::open_file(part, "wb");
    if (output.stream == nullptr)
        throw Failure("cannot write " + utf8_of(part));
    output.files = oa::platform::stdio_files();
    zip::StreamWriter writer({&output, write_at});
    for (const Item& item : items)
        write_item(writer, item);
    zip::ZipError error{};
    if (!writer.finish(error))
        throw Failure(zip_failure(error));
    if (std::fflush(output.stream) != 0)
        throw Failure("cannot write " + utf8_of(part));
    return writer.bytes_written();
}

/// Returns the SHA-256 of a file, refusing a size other than `expected`.
///
/// @param path the file
/// @param expected its size
/// @return the digest
sha256::Digest digest_file(const fs::path& path, uint64_t expected) {
    FilePos file;
    const uint64_t size = open_read(path, file);
    if (size != expected)
        throw Failure("the package's size changed while it was read");
    sha256::Hasher hasher{};
    std::vector<uint8_t> piece(read_piece);
    uint64_t left = size;
    while (left > 0) {
        const std::size_t want =
            static_cast<std::size_t>(std::min(left, static_cast<uint64_t>(piece.size())));
        if (std::fread(piece.data(), 1, want, file.stream) != want)
            throw Failure("cannot read " + utf8_of(path));
        sha256::update(hasher, std::span<const uint8_t>(piece.data(), want));
        left -= want;
    }
    return sha256::finish(hasher);
}

/// Reads the package back and refuses one whose language manifest does not.
///
/// @param path the package, a .oalang or the .part that will become one
void verify_language(const fs::path& path) {
    FilePos file;
    const uint64_t bytes = open_read(path, file);
    zip::StreamDirectory directory;
    zip::ZipError error{};
    if (!zip::read_stream_directory({&file, read_at}, bytes, {}, directory, error))
        throw Failure(zip_failure(error));
    const zip::StreamEntry* manifest = nullptr;
    for (const zip::StreamEntry& entry : directory.entries) {
        if (entry.directory || entry.name.find('/') != std::string::npos)
            continue;
        if (same_ascii(entry.name, languages::pack_manifest_file)) {
            manifest = &entry;
            break;
        }
    }
    if (manifest == nullptr)
        throw Failure("the package holds no language.yaml at its top");
    if (manifest->bytes > oa::formats::oamod::max_input_bytes)
        throw Failure("language.yaml is larger than 256 KiB");
    std::vector<uint8_t> text;
    if (!zip::read_stream_entry({&file, read_at}, bytes, *manifest, manifest->bytes, text, error))
        throw Failure(zip_failure(error));
    languages::PackManifest read{};
    std::string why;
    if (!languages::read_manifest(text, read, &why))
        throw Failure(why.empty() ? "language.yaml has errors" : why);
}

/// Reads the package back with the installer and refuses one it refuses.
///
/// @param path the .oamod
/// @param output where the manifest's diagnostics are printed
void verify_mod(const fs::path& path, Output& output) {
    const install::PackageResult result = install::open_package(path);
    if (result.package)
        return;
    for (const std::string& line : result.problem.lines)
        output.err << line << '\n';
    std::string why = "the package was refused";
    if (!result.problem.subject.empty()) {
        why += ": ";
        why += result.problem.subject;
    }
    if (!result.problem.detail.empty()) {
        why += ": ";
        why += result.problem.detail;
    }
    throw Failure(why);
}

/// Returns the package file's name when --out was not given.
///
/// @param kind which manifest the folder holds
/// @param profile the mod's profile; unused for a language
/// @param manifest the language's manifest; unused for a mod
/// @return the file name
std::string default_name(
    Kind kind,
    const oa::data::mod_profile::ModProfile& profile,
    const languages::PackManifest& manifest
) {
    if (kind == Kind::mod) {
        return profile.id + "-" + install::oamod::version_folder_part(profile.version) + "-r" +
               std::to_string(profile.packaging.revision) + ".oamod";
    }
    return manifest.tag + "-" + install::oamod::version_folder_part(manifest.version) + ".oalang";
}

/// Tells whether a file name ends in an extension, compared without case.
/// The extension includes its dot. A name that is only the extension does not.
///
/// @param name the file name
/// @param extension the extension, including its dot, already folded
/// @return true when the name ends in it and is longer
bool ends_with_extension(std::string_view name, std::string_view extension) {
    return name.size() > extension.size() && folded(name).ends_with(extension);
}

/// Prints a usage failure the way the dispatcher does.
///
/// @param output where it is printed
/// @param problem what is wrong
void report_usage(Output& output, std::string_view problem);

/// The files of a folder, read by path for the mutator checks.
struct FolderEntries {
    const std::vector<Item>* items{}; ///< the folder's files and folders
};

/// Appends every file in a folder being packed.
///
/// @param context a FolderEntries
/// @param[out] paths the file paths
void list_folder(void* context, std::vector<std::string>& paths) {
    const auto* source = static_cast<const FolderEntries*>(context);
    if (source == nullptr || source->items == nullptr)
        return;
    for (const Item& item : *source->items) {
        if (!item.folder)
            paths.push_back(item.name);
    }
}

/// Copies one file out of a folder being packed.
///
/// @param context a FolderEntries
/// @param path the file's path
/// @param limit the most bytes it may hold
/// @param[out] bytes the file; empty when it was not read
/// @param[out] too_large true when the file is larger than limit
/// @return true when the file was read
bool read_folder(
    void* context,
    std::string_view path,
    std::size_t limit,
    std::vector<uint8_t>& bytes,
    bool& too_large
) {
    bytes.clear();
    too_large = false;
    const auto* source = static_cast<const FolderEntries*>(context);
    if (source == nullptr || source->items == nullptr)
        return false;
    const Item* found = nullptr;
    for (const Item& item : *source->items) {
        if (!item.folder && item.name == path) {
            found = &item;
            break;
        }
    }
    if (found == nullptr)
        return false;
    if (found->bytes > limit) {
        too_large = true;
        return false;
    }
    FilePos file;
    const uint64_t measured = open_read(found->path, file);
    if (measured != found->bytes)
        return false;
    bytes.resize(static_cast<std::size_t>(measured));
    if (measured > 0 && std::fread(bytes.data(), 1, bytes.size(), file.stream) != bytes.size()) {
        bytes.clear();
        return false;
    }
    return true;
}

/// Reads the package back with the installer and refuses one it refuses.
///
/// The installer's sentence is printed, then each diagnostic.
///
/// @param path the package
/// @param output where the refusal is printed
void verify_mutator(const fs::path& path, Output& output) {
    const install::PackageResult result = install::open_package(path);
    if (result.package)
        return;
    const install::PackageKind* kind = install::kind_for_file(path);
    std::string sentence =
        kind != nullptr ? install::refusal_text(*kind, result.problem) : std::string{};
    if (sentence.empty())
        sentence = result.problem.detail.empty() ? std::string{"It cannot be read."}
                                                 : result.problem.detail;
    output.err << sentence << '\n';
    for (const std::string& line : result.problem.lines)
        output.err << line << '\n';
    throw Failure("the package was refused");
}

/// Packs a folder whose top holds oamut.yaml.
///
/// Scripts and rule files are checked before anything is written. The
/// installer then reads the package back, and a refusal deletes it.
///
/// @param items the folder's files
/// @param manifest_name the manifest's name, as the folder spells it
/// @param out_path the --out path; empty for the default name
/// @param force an existing output may be replaced
/// @param output where the result and the diagnostics are printed
/// @return exit_done
int pack_mutator(
    std::vector<Item> items,
    const std::string& manifest_name,
    const std::string& out_path,
    bool force,
    Output& output
) {
    std::sort(items.begin(), items.end(), [&](const Item& left, const Item& right) {
        const bool left_manifest = !left.folder && left.name == manifest_name;
        const bool right_manifest = !right.folder && right.name == manifest_name;
        if (left_manifest != right_manifest)
            return left_manifest;
        return bytes_before(entry_name(left), entry_name(right));
    });
    detail::check_names(items);

    const Item* manifest_item = nullptr;
    std::size_t file_count = 0;
    for (const Item& item : items) {
        if (!item.folder)
            ++file_count;
        if (!item.folder && item.name == manifest_name)
            manifest_item = &item;
    }
    if (manifest_item == nullptr)
        throw Failure("the folder holds no oamut.yaml");
    if (manifest_item->bytes > oa::formats::oamod::max_input_bytes)
        throw Failure(manifest_name + " is larger than 256 KiB");
    const std::vector<uint8_t> manifest_bytes =
        read_capped(manifest_item->path, manifest_item->bytes, manifest_name);

    std::vector<std::string> problems;
    oa::formats::oamut::Manifest manifest;
    std::vector<oa::formats::oamut::Problem> manifest_problems;
    const bool manifest_ok =
        oa::formats::oamut::read_manifest(manifest_bytes, manifest, manifest_problems);
    if (!manifest_ok) {
        for (const oa::formats::oamut::Problem& one : manifest_problems)
            problems.push_back(oa::formats::oamut::describe(one));
    }
    FolderEntries folder{&items};
    MutatorChecked checked;
    check_mutator_scripts_and_rules(
        PackageEntries{&folder, &list_folder, &read_folder}, problems, checked
    );
    if (!problems.empty()) {
        for (const std::string& problem : problems)
            output.err << problem << '\n';
        throw Failure(manifest_ok ? "the mutator has errors" : "oamut.yaml has errors");
    }

    fs::path destination;
    if (!out_path.empty()) {
        destination = fs::path(std::u8string(out_path.begin(), out_path.end()));
    } else {
        const std::string name =
            manifest.id + "-" + oa::formats::oamut::version_text(manifest.version) + ".oamut";
        if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
            !install::portable_name_problem(name).empty())
            throw Failure("the package's name is not a file name");
        destination = name;
    }
    const std::string destination_name = folded(utf8_of(destination.filename()));
    if (!ends_with_extension(destination_name, ".oamut"))
        throw Failure("a mutator folder is packed as a .oamut file");

    std::error_code error;
    if (!destination.parent_path().empty() &&
        !fs::is_directory(fs::symlink_status(destination.parent_path(), error)))
        throw Failure("cannot write " + utf8_of(destination));
    const fs::file_status existing = fs::symlink_status(destination, error);
    if (!error && fs::exists(existing)) {
        if (fs::is_directory(existing))
            throw Failure("the output is a folder: " + utf8_of(destination));
        if (!force)
            throw Failure("the output already exists: " + utf8_of(destination));
    }

    fs::path part = destination;
    part += ".part";
    RemoveUnless part_file(part);
    const uint64_t written = write_part(part, items);
    replace_output(part, destination);
    part_file.release();
    RemoveUnless output_file(destination);
    verify_mutator(destination, output);
    const sha256::Digest digest = digest_file(destination, written);
    output_file.release();

    const auto hex = sha256::to_hex(digest);
    output.out << "packed " << utf8_of(destination) << ": " << file_count << " files, " << written
               << " bytes, sha256 ";
    output.out.write(hex.data(), static_cast<std::streamsize>(hex.size()));
    output.out << '\n';
    return exit_done;
}

/// Packs a folder whose top holds oabrain.yaml.
///
/// The manifest, the layout, the scripts, the networks and the icon are
/// checked before anything is written. The installer then reads the package
/// back, and a refusal deletes it. Lua is compiled and not run.
///
/// @param items the folder's files
/// @param manifest_name the manifest's name, as the folder spells it
/// @param out_path the --out path; empty for the default name
/// @param force an existing output may be replaced
/// @param output where the result and the diagnostics are printed
/// @return exit_done
int pack_brain(
    std::vector<Item> items,
    const std::string& manifest_name,
    const std::string& out_path,
    bool force,
    Output& output
) {
    std::sort(items.begin(), items.end(), [&](const Item& left, const Item& right) {
        const bool left_manifest = !left.folder && left.name == manifest_name;
        const bool right_manifest = !right.folder && right.name == manifest_name;
        if (left_manifest != right_manifest)
            return left_manifest;
        return bytes_before(entry_name(left), entry_name(right));
    });
    detail::check_names(items);

    const Item* manifest_item = nullptr;
    std::size_t file_count = 0;
    for (const Item& item : items) {
        if (!item.folder)
            ++file_count;
        if (!item.folder && item.name == manifest_name)
            manifest_item = &item;
    }
    if (manifest_item == nullptr)
        throw Failure("the folder holds no oabrain.yaml");
    if (manifest_item->bytes > oa::formats::oamod::max_input_bytes)
        throw Failure(manifest_name + " is larger than 256 KiB");
    const std::vector<uint8_t> manifest_bytes =
        read_capped(manifest_item->path, manifest_item->bytes, manifest_name);

    std::vector<std::string> problems;
    oa::formats::oabrain::Manifest manifest;
    std::vector<oa::formats::oabrain::Problem> manifest_problems;
    const bool manifest_ok =
        oa::formats::oabrain::read_manifest(manifest_bytes, manifest, manifest_problems);
    if (!manifest_ok) {
        for (const oa::formats::oabrain::Problem& one : manifest_problems)
            problems.push_back(oa::formats::oabrain::describe(one));
    } else {
        FolderEntries folder{&items};
        BrainChecked checked;
        check_brain_files(
            PackageEntries{&folder, &list_folder, &read_folder}, manifest, problems, checked
        );
    }
    if (!problems.empty()) {
        for (const std::string& problem : problems)
            output.err << problem << '\n';
        throw Failure(manifest_ok ? "the brain has errors" : "oabrain.yaml has errors");
    }

    fs::path destination;
    if (!out_path.empty()) {
        destination = fs::path(std::u8string(out_path.begin(), out_path.end()));
    } else {
        const std::string name =
            manifest.id + "-" + oa::formats::oamut::version_text(manifest.version) + ".oabrain";
        if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
            !install::portable_name_problem(name).empty())
            throw Failure("the package's name is not a file name");
        destination = name;
    }
    const std::string destination_name = folded(utf8_of(destination.filename()));
    if (!ends_with_extension(destination_name, ".oabrain"))
        throw Failure("a brain folder is packed as a .oabrain file");

    std::error_code error;
    if (!destination.parent_path().empty() &&
        !fs::is_directory(fs::symlink_status(destination.parent_path(), error)))
        throw Failure("cannot write " + utf8_of(destination));
    const fs::file_status existing = fs::symlink_status(destination, error);
    if (!error && fs::exists(existing)) {
        if (fs::is_directory(existing))
            throw Failure("the output is a folder: " + utf8_of(destination));
        if (!force)
            throw Failure("the output already exists: " + utf8_of(destination));
    }

    fs::path part = destination;
    part += ".part";
    RemoveUnless part_file(part);
    const uint64_t written = write_part(part, items);
    replace_output(part, destination);
    part_file.release();
    RemoveUnless output_file(destination);
    verify_mutator(destination, output);
    const sha256::Digest digest = digest_file(destination, written);
    output_file.release();

    const auto hex = sha256::to_hex(digest);
    output.out << "packed " << utf8_of(destination) << ": " << file_count << " files, " << written
               << " bytes, sha256 ";
    output.out.write(hex.data(), static_cast<std::streamsize>(hex.size()));
    output.out << '\n';
    return exit_done;
}

/// Packs one folder.
///
/// @param folder the folder
/// @param out_path the --out path; empty for the default name
/// @param force an existing output may be replaced
/// @param game_dir the game's data; only a map pack reads it
/// @param output where the result and the diagnostics are printed
/// @return exit_done, or exit_usage when --game-dir is given for another kind
int pack_folder(
    const fs::path& folder,
    const std::string& out_path,
    bool force,
    const std::optional<std::string>& game_dir,
    Output& output
) {
    std::vector<Item> items = collect(folder);
    std::string mod_name;
    std::string language_name;
    std::string map_name;
    std::string expansion_name;
    std::string mutator_name;
    std::string brain_name;
    for (const Item& item : items) {
        if (item.folder || item.name.find('/') != std::string::npos)
            continue;
        if (same_ascii(item.name, "oamod.yaml"))
            mod_name = item.name;
        else if (same_ascii(item.name, languages::pack_manifest_file))
            language_name = item.name;
        else if (same_ascii(item.name, "oamap.yaml"))
            map_name = item.name;
        else if (same_ascii(item.name, "oaexp.yaml"))
            expansion_name = item.name;
        else if (same_ascii(item.name, "oamut.yaml"))
            mutator_name = item.name;
        else if (same_ascii(item.name, "oabrain.yaml"))
            brain_name = item.name;
    }
    const int manifests = (mod_name.empty() ? 0 : 1) + (language_name.empty() ? 0 : 1) +
                          (map_name.empty() ? 0 : 1) + (expansion_name.empty() ? 0 : 1) +
                          (mutator_name.empty() ? 0 : 1) + (brain_name.empty() ? 0 : 1);
    if (manifests == 0)
        throw Failure(
            "the folder holds no oamod.yaml or language.yaml or oamap.yaml or oaexp.yaml or "
            "oamut.yaml or oabrain.yaml"
        );
    if (manifests > 1) {
        if (!brain_name.empty() && manifests == 2) {
            if (!mod_name.empty())
                throw Failure("the folder holds both oamod.yaml and oabrain.yaml");
            if (!language_name.empty())
                throw Failure("the folder holds both language.yaml and oabrain.yaml");
            if (!map_name.empty())
                throw Failure("the folder holds both oamap.yaml and oabrain.yaml");
            if (!expansion_name.empty())
                throw Failure("the folder holds both oaexp.yaml and oabrain.yaml");
            throw Failure("the folder holds both oamut.yaml and oabrain.yaml");
        }
        if (!brain_name.empty())
            throw Failure("the folder holds more than one manifest");
        if (!mutator_name.empty() && manifests == 2) {
            if (!mod_name.empty())
                throw Failure("the folder holds both oamod.yaml and oamut.yaml");
            if (!language_name.empty())
                throw Failure("the folder holds both language.yaml and oamut.yaml");
            if (!map_name.empty())
                throw Failure("the folder holds both oamap.yaml and oamut.yaml");
            throw Failure("the folder holds both oaexp.yaml and oamut.yaml");
        }
        if (!mutator_name.empty())
            throw Failure("the folder holds more than one manifest");
        if (!expansion_name.empty())
            throw Failure("the folder holds more than one manifest");
        if (!mod_name.empty() && !language_name.empty() && map_name.empty())
            throw Failure("the folder holds both oamod.yaml and language.yaml");
        if (!mod_name.empty() && language_name.empty() && !map_name.empty())
            throw Failure("the folder holds both oamod.yaml and oamap.yaml");
        if (mod_name.empty() && !language_name.empty() && !map_name.empty())
            throw Failure("the folder holds both language.yaml and oamap.yaml");
        throw Failure("the folder holds oamod.yaml, language.yaml and oamap.yaml");
    }
    if (!expansion_name.empty()) {
        if (game_dir.has_value()) {
            report_usage(output, "--game-dir is not for an expansion");
            return exit_usage;
        }
        return pack_expansion(folder, nullptr, out_path, force, output);
    }
    if (!map_name.empty()) {
        PackMapRequest request;
        request.folder = folder;
        if (!out_path.empty())
            request.out = fs::path(std::u8string(out_path.begin(), out_path.end()));
        request.force = force;
        if (game_dir.has_value())
            request.game_dir = fs::path(std::u8string(game_dir->begin(), game_dir->end()));
        return pack_map(request, output);
    }
    if (!brain_name.empty()) {
        if (game_dir.has_value()) {
            report_usage(output, "--game-dir is not for a brain");
            return exit_usage;
        }
        return pack_brain(std::move(items), brain_name, out_path, force, output);
    }
    if (game_dir.has_value()) {
        report_usage(output, "--game-dir is only for a map pack");
        return exit_usage;
    }
    if (!mutator_name.empty())
        return pack_mutator(std::move(items), mutator_name, out_path, force, output);
    const Kind kind = mod_name.empty() ? Kind::language : Kind::mod;
    const std::string& manifest_name = kind == Kind::mod ? mod_name : language_name;
    std::sort(items.begin(), items.end(), [&](const Item& left, const Item& right) {
        const bool left_manifest = !left.folder && left.name == manifest_name;
        const bool right_manifest = !right.folder && right.name == manifest_name;
        if (left_manifest != right_manifest)
            return left_manifest;
        return bytes_before(entry_name(left), entry_name(right));
    });
    detail::check_names(items);

    const Item* manifest_item = nullptr;
    std::size_t file_count = 0;
    for (const Item& item : items) {
        if (!item.folder)
            ++file_count;
        if (!item.folder && item.name == manifest_name)
            manifest_item = &item;
    }
    if (manifest_item == nullptr)
        throw Failure("the folder holds no oamod.yaml or language.yaml");
    if (manifest_item->bytes > oa::formats::oamod::max_input_bytes)
        throw Failure(manifest_name + " is larger than 256 KiB");
    const std::vector<uint8_t> manifest_bytes =
        read_capped(manifest_item->path, manifest_item->bytes, manifest_name);

    oa::data::mod_profile::ModProfile profile{};
    languages::PackManifest language{};
    if (kind == Kind::mod) {
        const auto resolved = oa::data::mod_profile::resolve_profile(manifest_bytes, manifest_name);
        print_diagnostics(output, resolved.errors);
        print_diagnostics(output, resolved.warnings);
        if (!resolved.resolution)
            throw Failure("oamod.yaml has errors");
        profile = resolved.resolution->profile;
    } else {
        std::string why;
        if (!languages::read_manifest(manifest_bytes, language, &why))
            throw Failure(why.empty() ? "language.yaml has errors" : why);
    }

    fs::path destination;
    if (!out_path.empty()) {
        destination = fs::path(std::u8string(out_path.begin(), out_path.end()));
    } else {
        const std::string name = default_name(kind, profile, language);
        if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
            !install::portable_name_problem(name).empty())
            throw Failure("the package's name is not a file name");
        destination = name;
    }
    const std::string destination_name = folded(utf8_of(destination.filename()));
    const bool oamod = ends_with_extension(destination_name, ".oamod");
    const bool oalang = ends_with_extension(destination_name, ".oalang");
    if (!oamod && !oalang)
        throw Failure("the output must be a .oamod or a .oalang file");
    if (oamod != (kind == Kind::mod))
        throw Failure(
            kind == Kind::mod ? "a mod folder is packed as a .oamod file"
                              : "a language folder is packed as a .oalang file"
        );

    std::error_code error;
    if (!destination.parent_path().empty() &&
        !fs::is_directory(fs::symlink_status(destination.parent_path(), error)))
        throw Failure("cannot write " + utf8_of(destination));
    const fs::file_status existing = fs::symlink_status(destination, error);
    if (!error && fs::exists(existing)) {
        if (fs::is_directory(existing))
            throw Failure("the output is a folder: " + utf8_of(destination));
        if (!force)
            throw Failure("the output already exists: " + utf8_of(destination));
    }

    fs::path part = destination;
    part += ".part";
    RemoveUnless part_file(part);
    const uint64_t written = write_part(part, items);
    if (oalang)
        verify_language(part);
    replace_output(part, destination);
    part_file.release();
    RemoveUnless output_file(destination);
    if (oamod)
        verify_mod(destination, output);
    const sha256::Digest digest = digest_file(destination, written);
    output_file.release();

    const auto hex = sha256::to_hex(digest);
    output.out << "packed " << utf8_of(destination) << ": " << file_count << " files, " << written
               << " bytes, sha256 ";
    output.out.write(hex.data(), static_cast<std::streamsize>(hex.size()));
    output.out << '\n';
    return exit_done;
}

void report_usage(Output& output, std::string_view problem) {
    output.err << "oa-tool pack: " << problem << '\n';
    output.err << "run 'oa-tool help pack'\n";
}

} // namespace

int run_pack(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"out", true, false},
        {"force", false, false},
        {"game-dir", true, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed) {
        report_usage(output, problem);
        return exit_usage;
    }
    if (parsed->positional.size() != 1) {
        report_usage(
            output, "expected one folder, got " + std::to_string(parsed->positional.size())
        );
        return exit_usage;
    }
    const std::string* const out = parsed->value("out");
    if (out != nullptr && out->empty()) {
        report_usage(output, "option '--out' needs a value");
        return exit_usage;
    }
    const std::string* const game_dir = parsed->value("game-dir");
    if (game_dir != nullptr && game_dir->empty()) {
        report_usage(output, "option '--game-dir' needs a value");
        return exit_usage;
    }
    std::optional<std::string> game;
    if (game_dir != nullptr)
        game = *game_dir;
    return pack_folder(
        fs::path(std::u8string(parsed->positional[0].begin(), parsed->positional[0].end())),
        out == nullptr ? std::string{} : *out,
        parsed->flags.contains("force"),
        game,
        output
    );
}

bool detail::CaseTree::add(std::string_view path, bool folder) {
    std::size_t start = 0;
    while (true) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        const bool last = end == path.size();
        const bool part_folder = !last || folder;
        const std::string_view prefix = path.substr(0, end);
        const auto [slot, added] = placed_.try_emplace(folded(prefix));
        if (added) {
            slot->second.exact.assign(prefix);
            slot->second.folder = part_folder;
            if (part_folder)
                ++folders_;
        } else if (slot->second.exact != prefix || slot->second.folder != part_folder) {
            return false;
        }
        if (last)
            return true;
        start = end + 1;
    }
}

std::size_t detail::CaseTree::folders() const noexcept {
    return folders_;
}

void detail::check_names(const std::vector<Item>& items) {
    CaseTree cases;
    uint64_t bytes = 0;
    for (const Item& item : items) {
        const std::string name = entry_name(item);
        if (const std::string problem = install::portable_name_problem(name); !problem.empty())
            throw Failure("the folder holds a name it cannot pack: " + item.name + ": " + problem);
        if (!cases.add(item.name, item.folder))
            throw Failure("the folder holds two names that differ only in case: " + item.name);
        if (cases.folders() > install::max_package_folders)
            throw Failure("the folder holds more folders than a package may");
        if (!item.folder) {
            if (item.bytes > install::max_install_bytes ||
                bytes > install::max_install_bytes - item.bytes)
                throw Failure("the folder's files take more bytes than a package may");
            bytes += item.bytes;
        }
    }
}

} // namespace oa::tool
