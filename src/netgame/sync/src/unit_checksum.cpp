// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/sync/unit_checksum.hpp"
#include "oa/data/defs/layout.hpp"

#include "oa/formats/hpi.hpp"
#include "oa/formats/tdf.hpp"

#include <array>
#include <exception>
#include <limits>
#include <span>
#include <string>

namespace oa::netgame::sync {

using data::unit_definitions::CatalogAssetReader;
using data::unit_definitions::ErrorCode;
using data::unit_definitions::Result;

Result<std::vector<std::string>>
StoreUnitFiles::list_effective(std::string_view directory, std::string_view extension) const {
    try {
        return {store_.list_effective(directory, extension), {}};
    } catch (const std::exception& error) {
        return {{}, {ErrorCode::io, 0, error.what()}};
    }
}

Result<std::string> StoreUnitFiles::read(std::string_view logical_path) const {
    try {
        const auto bytes = store_.read(logical_path).bytes;
        return {std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), {}};
    } catch (const std::exception& error) {
        return {{}, {ErrorCode::io, 0, error.what()}};
    }
}

uint32_t unit_file_checksum(std::string_view bytes) noexcept {
    if (bytes.empty() ||
        bytes.size() > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()))
        return 0;
    return oa::formats::tdf::buffer_hash(
        reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<int32_t>(bytes.size())
    );
}

namespace {

char ascii_upper(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 'a' && u <= 'z')
        return static_cast<char>(u - ('a' - 'A'));
    return static_cast<char>(u);
}

// The game's wildcard match: active pattern indexes, capped at 100. '?' and
// '*' are matched after an ASCII upper-case fold. A trailing '*' succeeds at
// end.
bool wildcard_match(std::string_view name, std::string_view pattern) {
    std::array<int, 100> states{};
    int count = 1;
    for (const char raw : name) {
        const char folded = ascii_upper(raw);
        if (folded == '\0')
            break;
        if (count <= 0)
            continue;
        int seen = 0;
        int read = 0;
        int write = count;
        while (seen < count) {
            const int at = states[static_cast<std::size_t>(read)];
            const char pattern_char = at >= 0 && static_cast<std::size_t>(at) < pattern.size()
                                          ? ascii_upper(pattern[static_cast<std::size_t>(at)])
                                          : '\0';
            if (pattern_char == '?' || pattern_char == folded) {
                states[static_cast<std::size_t>(read)] = at + 1;
            } else if (pattern_char == '*') {
                if (count < 100) {
                    ++count;
                    states[static_cast<std::size_t>(write)] = at + 1;
                    ++write;
                }
            } else {
                --count;
                --write;
                if (count == 0)
                    return false;
                --seen;
                states[static_cast<std::size_t>(read)] = states[static_cast<std::size_t>(write)];
                --read;
            }
            ++seen;
            ++read;
        }
    }
    for (int i = 0; i < count; ++i) {
        const int at = states[static_cast<std::size_t>(i)];
        if (at < 0 || static_cast<std::size_t>(at) >= pattern.size())
            return true;
        if (pattern[static_cast<std::size_t>(at)] == '*' &&
            (static_cast<std::size_t>(at) + 1U >= pattern.size() ||
             pattern[static_cast<std::size_t>(at) + 1U] == '\0'))
            return true;
    }
    return false;
}

std::string_view filename_of(std::string_view path) noexcept {
    const auto slash = path.find_last_of("/\\");
    if (slash == std::string_view::npos)
        return path;
    return path.substr(slash + 1);
}

} // namespace

namespace {

/// Tells whether a unit name names a file in one folder.
///
/// @param unit_name the unit name
/// @param[out] result receives the malformed error when it does not
/// @return true when the name holds no path separator
bool plain_unit_name(std::string_view unit_name, Result<uint32_t>& result) {
    if (unit_name.find_first_of("/\\") == std::string_view::npos)
        return true;
    result.error = {ErrorCode::malformed, 0, "unit name contains a path separator"};
    return false;
}

/// Hashes a unit's script, its pages among the GUI pages and its download
/// menu, then mixes in its weapon checksum.
///
/// @param assets catalog the unit's files are read from
/// @param unit_name unit name, holding no path separator
/// @param weapon_checksum UnitDef.weapon_checksum
/// @param script_checksum the hash of scripts/<unit_name>.cob, 0 when it is not read
/// @param gui_pages list_effective("guis", ".gui")
/// @return the checksum
uint32_t mix_unit_files(
    const CatalogAssetReader& assets,
    std::string_view unit_name,
    uint32_t weapon_checksum,
    uint32_t script_checksum,
    std::span<const std::string> gui_pages
) {
    uint32_t mixed = script_checksum;
    const std::string pattern = std::string(unit_name) + "*.gui";
    for (const auto& path : gui_pages) {
        const auto name = filename_of(path);
        // The game's file search drops the entries "." and "..".
        if (name == "." || name == "..")
            continue;
        if (!wildcard_match(name, pattern))
            continue;
        if (auto gui = assets.read(path))
            mixed ^= unit_file_checksum(gui.value);
    }
    if (auto download = assets.read(
            oa::data::defs::data_path(
                oa::data::defs::DataDirectory::download, std::string(unit_name) + ".tdf"
            )
        )) {
        if (!download.value.empty() &&
            download.value.size() <= static_cast<std::size_t>(std::numeric_limits<int32_t>::max()))
            mixed ^= unit_file_checksum(download.value);
    }
    return mixed ^ weapon_checksum;
}

/// Hashes a unit's script.
///
/// @param assets catalog the script is read from
/// @param unit_name unit name, holding no path separator
/// @return the hash of scripts/<unit_name>.cob, 0 when it cannot be read
uint32_t script_checksum(const CatalogAssetReader& assets, std::string_view unit_name) {
    if (auto cob = assets.read(std::string("scripts/") + std::string(unit_name) + ".cob"))
        return unit_file_checksum(cob.value);
    return 0;
}

} // namespace

Result<std::vector<std::string>> list_unit_gui_pages(const CatalogAssetReader& assets) {
    return assets.list_effective(
        oa::data::defs::directory_name(oa::data::defs::DataDirectory::guis), ".gui"
    );
}

Result<uint32_t> mix_unit_file_checksum(
    const CatalogAssetReader& assets,
    std::string_view unit_name,
    uint32_t content_checksum,
    uint32_t weapon_checksum
) {
    Result<uint32_t> result;
    if (!plain_unit_name(unit_name, result))
        return result;
    if (content_checksum != 0) {
        result.value = content_checksum;
        return result;
    }
    // The script is read before the pages are listed.
    const uint32_t script = script_checksum(assets, unit_name);
    auto guis = list_unit_gui_pages(assets);
    if (!guis) {
        result.error = std::move(guis.error);
        return result;
    }
    result.value = mix_unit_files(assets, unit_name, weapon_checksum, script, guis.value);
    return result;
}

Result<uint32_t> mix_unit_file_checksum(
    const CatalogAssetReader& assets,
    std::string_view unit_name,
    uint32_t content_checksum,
    uint32_t weapon_checksum,
    std::span<const std::string> gui_pages
) {
    Result<uint32_t> result;
    if (!plain_unit_name(unit_name, result))
        return result;
    if (content_checksum != 0) {
        result.value = content_checksum;
        return result;
    }
    const uint32_t script = script_checksum(assets, unit_name);
    result.value = mix_unit_files(assets, unit_name, weapon_checksum, script, gui_pages);
    return result;
}

} // namespace oa::netgame::sync
