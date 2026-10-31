// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One map's files, from its installed folder or from the mounted pack layer.
// A folder check is a check before mounting: it never reads the layer.

#include "oa/data/map_fit/map_fit.hpp"

#include "oa/data/map_pack/map_name.hpp"
#include "oa/platform/files.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::data::map_fit {

namespace map_pack = oa::data::map_pack;
namespace fs = std::filesystem;

namespace {

/// Folds a resource path the way the asset store keys one.
///
/// '\\' and '/' are one separator, repeated separators collapse, a leading or
/// trailing separator is dropped, and ASCII letters fold to lower case.
///
/// @param path the path
/// @return the folded path
std::string fold_path(std::string_view path) {
    std::string result;
    result.reserve(path.size());
    bool previous_slash = true;
    for (const char raw : path) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte == '\\' || byte == '/') {
            if (!previous_slash)
                result.push_back('/');
            previous_slash = true;
            continue;
        }
        result.push_back(
            byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a')
                                       : static_cast<char>(byte)
        );
        previous_slash = false;
    }
    if (!result.empty() && result.back() == '/')
        result.pop_back();
    return result;
}

/// Returns a host path's UTF-8 spelling.
///
/// @param path the path
/// @return its UTF-8 spelling
std::string utf8_text(const fs::path& path) {
    return oa::platform::path_to_utf8(path);
}

/// Tells whether a file, every link followed, stays inside a folder.
///
/// @param root the folder
/// @param file the file
/// @return true when the file is inside the folder
bool stays_inside(const fs::path& root, const fs::path& file) {
    std::error_code error;
    const auto rooted = fs::weakly_canonical(root, error);
    if (error)
        return false;
    const auto filed = fs::weakly_canonical(file, error);
    if (error)
        return false;
    auto root_text = oa::platform::path_to_generic_utf8(rooted);
    const auto file_text = oa::platform::path_to_generic_utf8(filed);
    if (file_text == root_text)
        return false;
    if (!root_text.empty() && root_text.back() != '/')
        root_text.push_back('/');
    return file_text.starts_with(root_text);
}

/// Resolves one source file below a folder, ignoring ASCII case.
///
/// Two names that fold alike are refused, '.' and '..' are refused, and the
/// last part must be a regular file inside the folder.
///
/// @param root the folder
/// @param source the source path, with '/' or '\\' between parts
/// @return the host file, or nothing when it cannot be taken
std::optional<fs::path> resolve_file(const fs::path& root, std::string_view source) {
    const auto key = fold_path(source);
    auto current = root;
    for (std::size_t begin = 0; begin < key.size();) {
        const auto end = key.find('/', begin);
        const auto part =
            key.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (part.empty() || part == "." || part == "..")
            return std::nullopt;
        std::error_code error;
        if (!fs::is_directory(current, error))
            return std::nullopt;
        std::optional<fs::path> match;
        for (const auto& item : fs::directory_iterator(current, error)) {
            if (fold_path(utf8_text(item.path().filename())) != part)
                continue;
            if (match)
                return std::nullopt;
            match = item.path();
        }
        if (error || !match)
            return std::nullopt;
        current = *match;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    if (!stays_inside(root, current))
        return std::nullopt;
    std::error_code error;
    if (!fs::is_regular_file(current, error))
        return std::nullopt;
    return current;
}

/// Reads a whole file.
///
/// An empty file is an empty result, not a failure. A file that cannot be
/// opened reads as nothing.
///
/// @param host the file
/// @return the bytes, or nothing when the file cannot be read
std::optional<std::vector<uint8_t>> read_host(const fs::path& host) {
    std::ifstream input(host, std::ios::binary);
    if (!input)
        return std::nullopt;
    input.seekg(0, std::ios::end);
    const auto at = input.tellg();
    if (at < 0)
        return std::nullopt;
    const auto size = static_cast<std::size_t>(at);
    input.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(size);
    if (size != 0)
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!input)
        return std::nullopt;
    return bytes;
}

/// The files of one installed map folder.
class FolderMapFiles final : public MapFiles {
  public:

    /// Keeps the shown paths and the host file each one resolved to.
    ///
    /// @param shown the paths the store would show
    /// @param hosts the host file of each path, empty when it did not resolve
    FolderMapFiles(std::vector<std::string> shown, std::vector<std::optional<fs::path>> hosts)
        : shown_(std::move(shown)), hosts_(std::move(hosts)) {}

    /// Returns the paths the store would show.
    ///
    /// @return the shown paths
    [[nodiscard]] std::vector<std::string> paths() const override { return shown_; }

    /// Reads one shown path from the host file it resolved to.
    ///
    /// @param path a resource path, matched ignoring ASCII case
    /// @return the bytes, or nothing when the map does not list it
    [[nodiscard]] std::optional<std::vector<uint8_t>> read(std::string_view path) const override {
        const auto folded = fold_path(path);
        for (std::size_t index = 0; index < shown_.size(); ++index) {
            if (fold_path(shown_[index]) != folded)
                continue;
            if (!hosts_[index])
                return std::nullopt;
            return read_host(*hosts_[index]);
        }
        return std::nullopt;
    }

    /// A folder is a check before mounting.
    ///
    /// @return false
    [[nodiscard]] bool from_layer() const noexcept override { return false; }

  private:

    std::vector<std::string> shown_{};
    std::vector<std::optional<fs::path>> hosts_{};
};

/// The files of the store's mounted pack layer.
class MountedMapFiles final : public MapFiles {
  public:

    /// Borrows the store. The store must outlive this object.
    ///
    /// @param store the store
    explicit MountedMapFiles(const oa::AssetStore& store) : store_(store) {}

    /// Returns the paths the mounted layer shows.
    ///
    /// @return the layer's paths, or none when no layer is mounted
    [[nodiscard]] std::vector<std::string> paths() const override {
        return store_.pack_layer_paths();
    }

    /// Reads one path from the mounted layer.
    ///
    /// @param path a resource path, matched ignoring ASCII case
    /// @return the bytes, or nothing when the layer has no such file
    [[nodiscard]] std::optional<std::vector<uint8_t>> read(std::string_view path) const override {
        try {
            return store_.read_pack_layer(path);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    /// These files are the mounted layer.
    ///
    /// @return true
    [[nodiscard]] bool from_layer() const noexcept override { return true; }

  private:

    const oa::AssetStore& store_;
};

} // namespace

std::vector<oa::PackLayerFile> layer_files(const map_pack::MapEntry& map, std::string_view id) {
    const auto ota_shown = "maps/" + map_pack::pack_map_name(map.stem, id) + ".ota";
    const auto tnt_shown = "maps/" + map_pack::pack_map_name(map.stem, id) + ".tnt";
    const auto ota_source = fold_path(std::string("maps/") + map.stem + ".ota");
    const auto tnt_source = fold_path(std::string("maps/") + map.stem + ".tnt");
    std::vector<oa::PackLayerFile> files;
    files.reserve(map.files.size());
    for (const auto& file : map.files) {
        const auto folded = fold_path(file);
        if (folded == ota_source)
            files.push_back({ota_shown, file});
        else if (folded == tnt_source)
            files.push_back({tnt_shown, file});
        else
            files.push_back({file, file});
    }
    return files;
}

std::unique_ptr<MapFiles> folder_map_files(
    const std::filesystem::path& folder, const map_pack::MapEntry& map, std::string_view id
) {
    const auto listed = layer_files(map, id);
    std::vector<std::string> shown;
    std::vector<std::optional<fs::path>> hosts;
    shown.reserve(listed.size());
    hosts.reserve(listed.size());
    for (const auto& file : listed) {
        shown.push_back(file.path);
        hosts.push_back(resolve_file(folder, file.source));
    }
    return std::make_unique<FolderMapFiles>(std::move(shown), std::move(hosts));
}

std::unique_ptr<MapFiles> mounted_map_files(const oa::AssetStore& store) {
    return std::make_unique<MountedMapFiles>(store);
}

} // namespace oa::data::map_fit
