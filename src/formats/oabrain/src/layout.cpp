// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/oabrain/manifest.hpp"

#include <string_view>

namespace oa::formats::oabrain {
namespace {

/// The most bytes of a module name `require` can ask for.
constexpr std::size_t most_module_name_bytes = 64;

/// Tells whether two texts are the same, ignoring ASCII case.
///
/// @param left the first text
/// @param right the second text
/// @return true when every byte matches, with A-Z read as a-z
bool same_ascii_ci(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        unsigned char first = static_cast<unsigned char>(left[index]);
        unsigned char second = static_cast<unsigned char>(right[index]);
        if (first >= 'A' && first <= 'Z')
            first = static_cast<unsigned char>(first - 'A' + 'a');
        if (second >= 'A' && second <= 'Z')
            second = static_cast<unsigned char>(second - 'A' + 'a');
        if (first != second)
            return false;
    }
    return true;
}

/// Tells whether a stem is lower-case letters, digits and `_`.
///
/// @param stem the stem
/// @return true when it is non-empty and every byte is one of those
bool stem_bytes(std::string_view stem) noexcept {
    if (stem.empty())
        return false;
    for (const char character : stem) {
        const bool word = (character >= 'a' && character <= 'z') ||
                          (character >= '0' && character <= '9') || character == '_';
        if (!word)
            return false;
    }
    return true;
}

/// Tells whether a path is a Lua module a script can require.
///
/// `lua/` then parts of `[a-z0-9_]+` joined by `/`, ending `.lua`. The
/// dotted module name is at most 64 bytes.
///
/// @param path the path
/// @return true when a require of that module would open this file
bool module_file(std::string_view path) noexcept {
    constexpr std::string_view folder = "lua/";
    constexpr std::string_view suffix = ".lua";
    if (path.size() < folder.size() + 1 + suffix.size() || path.substr(0, folder.size()) != folder)
        return false;
    if (path.substr(path.size() - suffix.size()) != suffix)
        return false;
    const std::string_view body =
        path.substr(folder.size(), path.size() - folder.size() - suffix.size());
    if (body.empty())
        return false;
    std::size_t name_bytes = 0;
    std::size_t start = 0;
    bool any = false;
    while (start <= body.size()) {
        const std::size_t cut = body.find('/', start);
        const std::size_t end = cut == std::string_view::npos ? body.size() : cut;
        const std::string_view part = body.substr(start, end - start);
        if (!stem_bytes(part))
            return false;
        if (any)
            ++name_bytes;
        name_bytes += part.size();
        if (name_bytes > most_module_name_bytes)
            return false;
        any = true;
        if (cut == std::string_view::npos)
            break;
        start = cut + 1;
    }
    return any;
}

/// Tells whether a path is one data table, one folder deep.
///
/// @param path the path
/// @return true for data/<stem>.csv, .tdf, .json or .txt
bool data_file(std::string_view path) noexcept {
    constexpr std::string_view folder = "data/";
    if (path.size() <= folder.size() || path.substr(0, folder.size()) != folder)
        return false;
    const std::string_view rest = path.substr(folder.size());
    if (rest.find('/') != std::string_view::npos)
        return false;
    const std::size_t dot = rest.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == rest.size())
        return false;
    const std::string_view stem = rest.substr(0, dot);
    const std::string_view extension = rest.substr(dot + 1);
    if (extension != "csv" && extension != "tdf" && extension != "json" && extension != "txt")
        return false;
    return stem_bytes(stem) && stem.size() <= most_file_stem_bytes;
}

} // namespace

FileRole file_role(std::string_view path, const Manifest& manifest) noexcept {
    if (same_ascii_ci(path, manifest_file))
        return FileRole::manifest;
    if (path == manifest.entry)
        return FileRole::entry;
    if (module_file(path))
        return FileRole::module;
    for (const Network& network : manifest.networks) {
        if (network.file == path)
            return FileRole::network;
    }
    if (data_file(path))
        return FileRole::data;
    if (path == icon_file && manifest.icon == icon_file)
        return FileRole::icon;
    if (same_ascii_ci(path, "README.md"))
        return FileRole::readme;
    if (same_ascii_ci(path, "LICENSE"))
        return FileRole::licence;
    return FileRole::outside;
}

} // namespace oa::formats::oabrain
