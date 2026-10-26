// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The lobby's unit sync check: a content checksum of each unit's script, GUI
// pages and download menu that the players compare before a game starts.

#include "oa/data/unit_definitions.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa {
class AssetStore;
} // namespace oa

namespace oa::netgame::sync {

/// The installed files behind unit checksums, read from an asset store.
class StoreUnitFiles final : public data::unit_definitions::CatalogAssetReader {
  public:

    /// Reads from an asset store.
    ///
    /// @param store the store the unit files are read from; it outlives this reader
    explicit StoreUnitFiles(const AssetStore& store) noexcept : store_(store) {}

    /// Lists a directory's files.
    ///
    /// @param directory resource directory
    /// @param extension required suffix
    /// @return the store's list_effective, or what it threw as an io error
    [[nodiscard]] data::unit_definitions::Result<std::vector<std::string>>
    list_effective(std::string_view directory, std::string_view extension) const override;

    /// Reads a file's bytes.
    ///
    /// @param logical_path resource path
    /// @return the bytes, or what the store threw as an io error
    [[nodiscard]] data::unit_definitions::Result<std::string>
    read(std::string_view logical_path) const override;

  private:

    const AssetStore& store_;
};

/// Hashes one unit file into four byte lanes.
///
/// Lanes, low byte first: sum of bytes, xor of bytes, sum of (index ^ byte),
/// xor of the low 8 bits of (index + byte).
///
/// @param bytes File contents.
/// @return The hash; 0 for an empty file or one longer than INT32_MAX bytes.
/// @quirk The length test is signed, as in 3.1c, so lengths that do not fit a positive int32 hash to 0.
[[nodiscard]] uint32_t unit_file_checksum(std::string_view bytes) noexcept;

/// Computes a unit's content checksum.
///
/// A nonzero content_checksum is returned unchanged. Otherwise the result is the
/// xor of unit_file_checksum over scripts/<unit_name>.cob when that read
/// succeeds, every list_effective("guis", ".gui") entry whose filename
/// matches "<unit_name>*.gui" under the game's wildcard rules (ASCII
/// case-folded, host order), and download/<unit_name>.tdf when its size is
/// positive, then xor weapon_checksum. The game only snapshots the unit table
/// around this walk. This routine does not parse TDF keys.
///
/// @param assets Catalog the unit's files are read from.
/// @param unit_name Unit name; a name containing a slash is rejected as malformed.
/// @param content_checksum UnitDef.content_checksum.
/// @param weapon_checksum UnitDef.weapon_checksum.
/// @return The checksum, or the error of a bad name or a failed GUI listing.
[[nodiscard]] data::unit_definitions::Result<uint32_t> mix_unit_file_checksum(
    const data::unit_definitions::CatalogAssetReader& assets,
    std::string_view unit_name,
    uint32_t content_checksum,
    uint32_t weapon_checksum
);

/// Lists the GUI pages every unit's content checksum reads.
///
/// @param assets Catalog the pages are listed from.
/// @return list_effective("guis", ".gui"), or the listing's error.
[[nodiscard]] data::unit_definitions::Result<std::vector<std::string>>
list_unit_gui_pages(const data::unit_definitions::CatalogAssetReader& assets);

/// Computes a unit's content checksum from GUI pages already listed.
///
/// As mix_unit_file_checksum(assets, unit_name, content_checksum,
/// weapon_checksum), with `gui_pages` in place of the listing it makes, so
/// that the checksums of many units list the pages once.
///
/// @param assets Catalog the unit's files are read from.
/// @param unit_name Unit name; a name containing a slash is rejected as malformed.
/// @param content_checksum UnitDef.content_checksum.
/// @param weapon_checksum UnitDef.weapon_checksum.
/// @param gui_pages What list_unit_gui_pages gave for `assets`.
/// @return The checksum, or the error of a bad name.
[[nodiscard]] data::unit_definitions::Result<uint32_t> mix_unit_file_checksum(
    const data::unit_definitions::CatalogAssetReader& assets,
    std::string_view unit_name,
    uint32_t content_checksum,
    uint32_t weapon_checksum,
    std::span<const std::string> gui_pages
);

} // namespace oa::netgame::sync
