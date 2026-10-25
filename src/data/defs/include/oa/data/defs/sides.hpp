// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Side identity and HUD layout from GAMEDATA\SIDEDATA.TDF.
#pragma once

#include "oa/core/side.h"
#include "oa/data/defs/files.hpp"
#include "oa/data/limits.hpp"
#include "oa/formats/tdf.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::data::defs {

inline constexpr std::size_t side_error_capacity = 256;
// Bytes of a side's panel GAF name, its terminating NUL included.
inline constexpr std::size_t side_panel_gaf_capacity = 0x1e;
// Bytes of a side's font name, its terminating NUL included.
inline constexpr std::size_t side_font_name_capacity = 0x100;

/// Sides one table can hold: the most a profile's `limits.sides` may ask for.
inline constexpr std::size_t side_table_capacity = oa::data::limits::highest_sides;
static_assert(side_table_capacity >= OA_SIDE_COUNT);

struct SideTable {
    Side sides[side_table_capacity]{};
    uint32_t count{};
    uint32_t limit{};                  // sides this load could take
    uint32_t past_limit{};             // SIDE sections from SIDE<limit> on, left out unread
    char error[side_error_capacity]{}; // set when a required rectangle is missing
    // Each side's intgaf: the GAF in anims/ that holds its PANELTOP,
    // PANELSIDE and PANELBOT.
    char panel_gaf[side_table_capacity][side_panel_gaf_capacity]{};
    // Each side's font: the FNT in fonts/ that its resource numbers and unit
    // panel are drawn in.
    char font_name[side_table_capacity][side_font_name_capacity]{};
    // Whether each side's section has an intgaf key, and a font key: a side
    // without one has no panels, or no font of its own, and one with it
    // needs the file it names, an empty name included.
    bool has_panel_gaf[side_table_capacity]{};
    bool has_font[side_table_capacity]{};
};

// A file a side's SIDEDATA section names.
enum class SideFile : uint8_t {
    panels, // its intgaf, anims/<intgaf>.GAF
    font,   // its font, fonts/<font>.FNT
};

// A file a side names that the game data lacks.
struct SideMissingFile {
    uint32_t side{};            // index of the side that names it
    SideFile file{};            // which of its files
    char path[path_capacity]{}; // the file, such as "anims/NAME.GAF"
};

// The most files the sides can name: an intgaf and a font each.
inline constexpr std::size_t side_missing_file_capacity = 2 * side_table_capacity;

// Resolves a font name to a handle stored in Side.font; may be null.
struct SideFontResolver {
    void* context{};
    oa_ref32 (*font)(void* context, const char* name) = nullptr;
};

/// Returns the slot index a side record carries in Side.side_index.
///
/// @param side side record
/// @return index of the side in its table
[[nodiscard]] uint32_t side_index(const Side* side) noexcept;

/// Reads x1/y1/x2/y2 of a named sub-section of the side section at the document cursor.
///
/// The cursor is restored afterwards. Missing keys read as 0.
///
/// @param[in,out] sidedata parsed SIDEDATA.TDF with the cursor on a SIDEn section
/// @param[out] rect rectangle to fill; untouched when the sub-section is missing
/// @param section sub-section name, e.g. "LOGO"
/// @param side_name side name used in the error message
/// @param[out] error receives "Section [<section>] is missing from
///     GAMEDATA/SIDEDATA.TDF for the <name> side" when the sub-section is
///     missing; may be null
/// @param error_capacity size of `error` in bytes
/// @return true when the sub-section exists
bool side_load_rect(
    formats::tdf::Document* sidedata,
    Rect32* rect,
    const char* section,
    const char* side_name,
    char* error,
    std::size_t error_capacity
) noexcept;

/// Loads SIDE0, SIDE1, ... until one is missing or `side_limit` sides are read.
///
/// Each side reads name, nameprefix, commander, intgaf (into panel_gaf, up to
/// 29 characters), font (into font_name, and through `fonts`), energycolor,
/// metalcolor, its HUD rectangles in the game's read order and
/// RELOAD1..RELOAD3; has_panel_gaf and has_font tell which of the two keys
/// its section has. The table is zeroed first and every visited slot gets its
/// index. At most side_table_capacity sides are read. `limit` stores that
/// cap. When the cap was reached, sections from `SIDE<limit>` on are counted
/// into `past_limit` and left unread: their keys and rectangles are not
/// touched, so a broken section past the limit refuses nothing.
///
/// @param[in,out] sidedata parsed SIDEDATA.TDF; its cursor is moved
/// @param[out] table table to fill
/// @param fonts font resolver; null, or a null callback, leaves Side.font 0
/// @param side_limit sides this game plays; the profile's limits.sides, or
///     base_sides when the caller passes none
/// @return false when a side the game reads lacks one of its HUD rectangles
///     (fatal in 3.1c); table->error names the first missing one and count
///     is not set
bool side_table_load(
    formats::tdf::Document* sidedata,
    SideTable* table,
    const SideFontResolver* fonts,
    uint32_t side_limit = oa::data::limits::base_sides
) noexcept;

/// Loads GAMEDATA/SIDEDATA.TDF, preferring the variant directory.
///
/// A missing or unparsable file still runs side_table_load on the empty
/// document, leaving zero sides, as 3.1c does.
///
/// @param files file boundary
/// @param[out] table table to fill
/// @param variant game-data variant suffix; null or empty for none
/// @param fonts font resolver; may be null
/// @param side_limit sides this game plays; the profile's limits.sides, or
///     base_sides when the caller passes none
/// @return true when the file loaded and every side the game reads was complete
bool load_side_data(
    const Files* files,
    SideTable* table,
    const char* variant,
    const SideFontResolver* fonts,
    uint32_t side_limit = oa::data::limits::base_sides
) noexcept;

/// Writes the line for SIDE sections left out past the limit.
///
/// One section reads `gamedata/sidedata.tdf: SIDE5 is past the 5 sides this
/// game plays (limits.sides) and is left out`. More than one reads
/// `gamedata/sidedata.tdf: SIDE5 to SIDE6 are past the 5 sides this game
/// plays (limits.sides) and are left out`, with this load's numbers.
///
/// @param table loaded side table
/// @param[out] out destination; emptied when nothing was left out
/// @param capacity size of `out` in bytes
/// @return false when `past_limit` is 0
bool side_table_past_limit_line(const SideTable& table, char* out, std::size_t capacity) noexcept;

/// Builds the path of a file a side's section names: the GAF file in anims/
/// its intgaf names or the FNT file in fonts/ its font names, from the
/// variant directory when it holds the file, as build_variant_path chooses.
///
/// @param files file boundary used for the variant's existence check
/// @param table loaded side table
/// @param side index of the side
/// @param file which of its files
/// @param variant game-data variant suffix; null or empty for none
/// @param[out] out destination buffer; "" when the side names no such file,
///     or names an empty one
/// @param capacity size of `out` in bytes
/// @return true when the side's section names the file
bool side_file_path(
    const Files* files,
    const SideTable& table,
    uint32_t side,
    SideFile file,
    const char* variant,
    char* out,
    std::size_t capacity
) noexcept;

/// Lists the files loaded sides name that the game data lacks: a side's
/// intgaf when its GAF file in anims/ is missing from both the variant and
/// the plain directory, and its font when its FNT file in fonts/ is.
///
/// @param files file boundary; null finds every named file missing
/// @param table loaded side table
/// @param variant game-data variant suffix; null or empty for none
/// @param[out] missing receives the missing files, every side's intgaf
///     before any side's font, each kind by side, as far as it holds them,
///     each path with '/' separators; side_missing_file_capacity entries
///     hold every one
/// @return how many files are missing
uint32_t side_missing_files(
    const Files* files,
    const SideTable& table,
    const char* variant,
    std::span<SideMissingFile> missing
) noexcept;

} // namespace oa::data::defs
