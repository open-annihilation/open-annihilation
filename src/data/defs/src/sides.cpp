// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/defs/sides.hpp"
#include "oa/data/defs/layout.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace oa::data::defs {
namespace {

constexpr std::size_t font_name_capacity = 0x100;
constexpr uint32_t reload_rect_count = 3;

struct RectKey {
    const char* section{};
    Rect32 Side::* rect{};
};

// The game's read order; it decides which missing section is reported.
constexpr RectKey rect_keys[] = {
    {"LOGO", &Side::rect_logo},
    {"ENERGYBAR", &Side::rect_energy_bar},
    {"ENERGYNUM", &Side::rect_energy_num},
    {"METALBAR", &Side::rect_metal_bar},
    {"METALNUM", &Side::rect_metal_num},
    {"TOTALUNITS", &Side::rect_total_units},
    {"TOTALTIME", &Side::rect_total_time},
    {"ENERGY0", &Side::rect_energy0},
    {"METAL0", &Side::rect_metal0},
    {"ENERGYMAX", &Side::rect_energy_max},
    {"METALMAX", &Side::rect_metal_max},
    {"ENERGYPRODUCED", &Side::rect_energy_produced},
    {"ENERGYCONSUMED", &Side::rect_energy_consumed},
    {"METALPRODUCED", &Side::rect_metal_produced},
    {"METALCONSUMED", &Side::rect_metal_consumed},
    {"LOGO2", &Side::rect_logo2},
    {"UNITNAME", &Side::rect_unit_name},
    {"DAMAGEBAR", &Side::rect_damage_bar},
    {"UNITMETALMAKE", &Side::rect_unit_metal_make},
    {"UNITMETALUSE", &Side::rect_unit_metal_use},
    {"UNITENERGYMAKE", &Side::rect_unit_energy_make},
    {"UNITENERGYUSE", &Side::rect_unit_energy_use},
    {"MISSIONTEXT", &Side::rect_mission_text},
    {"UNITNAME2", &Side::rect_unit_name2},
    {"DAMAGEBAR2", &Side::rect_damage_bar2},
    {"NAME", &Side::rect_name},
    {"DESCRIPTION", &Side::rect_description},
};

void copy_if_present(
    const formats::tdf::Block* block, const char* key, char* out, std::size_t size
) noexcept {
    char buffer[font_name_capacity];
    if (formats::tdf::get_string(block, key, buffer, size, ""))
        std::memcpy(out, buffer, std::strlen(buffer) + 1);
}

} // namespace

uint32_t side_index(const Side* side) noexcept {
    return side->side_index;
}

bool side_load_rect(
    formats::tdf::Document* sidedata,
    Rect32* rect,
    const char* section,
    const char* side_name,
    char* error,
    std::size_t error_capacity
) noexcept {
    const formats::tdf::Block* saved = formats::tdf::cursor(sidedata);
    bool found = formats::tdf::select_section(sidedata, section);
    if (found) {
        const formats::tdf::Block* block = formats::tdf::cursor(sidedata);
        rect->x1 = formats::tdf::get_int(block, "x1", 0);
        rect->y1 = formats::tdf::get_int(block, "y1", 0);
        rect->x2 = formats::tdf::get_int(block, "x2", 0);
        rect->y2 = formats::tdf::get_int(block, "y2", 0);
    } else if (error != nullptr && error_capacity != 0) {
        std::snprintf(
            error,
            error_capacity,
            "Section [%s] is missing from GAMEDATA/SIDEDATA.TDF for the %s side",
            section,
            side_name
        );
    }
    formats::tdf::set_cursor(sidedata, saved);
    return found;
}

bool side_table_load(
    formats::tdf::Document* sidedata,
    SideTable* table,
    const SideFontResolver* fonts,
    uint32_t side_limit
) noexcept {
    std::memset(static_cast<void*>(table), 0, sizeof *table);
    const uint32_t cap = std::min(side_limit, static_cast<uint32_t>(side_table_capacity));
    uint32_t index = 0;
    for (; index < cap; ++index) {
        Side* side = &table->sides[index];
        side->side_index = index;
        char section[32];
        std::snprintf(section, sizeof section, "SIDE%u", index);
        formats::tdf::reset_cursor(sidedata);
        if (!formats::tdf::select_section(sidedata, section))
            break;
        const formats::tdf::Block* block = formats::tdf::cursor(sidedata);
        copy_if_present(block, "name", side->name, sizeof side->name);
        copy_if_present(block, "nameprefix", side->name_prefix, sizeof side->name_prefix);
        copy_if_present(block, "commander", side->commander, sizeof side->commander);
        table->has_panel_gaf[index] = formats::tdf::get_string(
            block, "intgaf", table->panel_gaf[index], sizeof table->panel_gaf[index], ""
        );
        char* font = table->font_name[index];
        table->has_font[index] =
            formats::tdf::get_string(block, "font", font, side_font_name_capacity, "");
        if (table->has_font[index] && fonts != nullptr && fonts->font != nullptr)
            side->font = fonts->font(fonts->context, font);
        side->energy_color = static_cast<uint32_t>(formats::tdf::get_int(block, "energycolor", 0));
        side->metal_color = static_cast<uint32_t>(formats::tdf::get_int(block, "metalcolor", 0));
        for (const RectKey& key : rect_keys)
            if (!side_load_rect(
                    sidedata,
                    &(side->*key.rect),
                    key.section,
                    side->name,
                    table->error,
                    sizeof table->error
                ))
                return false;
        Rect32* reload[reload_rect_count] = {
            &side->rect_reload1, &side->rect_reload2, &side->rect_reload3
        };
        for (uint32_t slot = 0; slot < reload_rect_count; ++slot) {
            char reload_section[32];
            std::snprintf(reload_section, sizeof reload_section, "RELOAD%u", slot + 1);
            if (!side_load_rect(
                    sidedata,
                    reload[slot],
                    reload_section,
                    side->name,
                    table->error,
                    sizeof table->error
                ))
                return false;
        }
    }
    table->count = index;
    table->limit = cap;
    // A section past the cap is only looked up, never read, so a missing
    // rectangle there does not refuse the load.
    if (index == cap) {
        uint32_t past = 0;
        for (uint32_t extra = cap; extra != UINT32_MAX; ++extra) {
            char past_section[32];
            std::snprintf(past_section, sizeof past_section, "SIDE%u", extra);
            formats::tdf::reset_cursor(sidedata);
            if (!formats::tdf::select_section(sidedata, past_section))
                break;
            ++past;
        }
        table->past_limit = past;
    }
    return true;
}

bool load_side_data(
    const Files* files,
    SideTable* table,
    const char* variant,
    const SideFontResolver* fonts,
    uint32_t side_limit
) noexcept {
    char path[path_capacity];
    build_variant_path(
        files,
        path,
        sizeof path,
        directory_name(DataDirectory::gamedata),
        "sidedata",
        "tdf",
        variant
    );
    formats::tdf::Document document;
    formats::tdf::document_init(&document);
    // 3.1c ignores a failed load here and ends up with zero sides.
    const bool loaded = load_tdf_file(files, path, &document, nullptr);
    const bool ok = side_table_load(&document, table, fonts, side_limit);
    formats::tdf::document_free(&document);
    return loaded && ok;
}

bool side_file_path(
    const Files* files,
    const SideTable& table,
    uint32_t side,
    SideFile file,
    const char* variant,
    char* out,
    std::size_t capacity
) noexcept {
    if (capacity != 0)
        out[0] = '\0';
    if (side >= table.count || side >= side_table_capacity)
        return false;
    const bool panels = file == SideFile::panels;
    if (!(panels ? table.has_panel_gaf[side] : table.has_font[side]))
        return false;
    build_variant_path(
        files,
        out,
        capacity,
        panels ? "anims" : "fonts",
        panels ? table.panel_gaf[side] : table.font_name[side],
        panels ? "GAF" : "FNT",
        variant
    );
    return true;
}

uint32_t side_missing_files(
    const Files* files,
    const SideTable& table,
    const char* variant,
    std::span<SideMissingFile> missing
) noexcept {
    uint32_t count = 0;
    // Every side's intgaf before any side's font, as 3.1c loads them.
    for (const SideFile file : {SideFile::panels, SideFile::font})
        for (uint32_t index = 0; index < table.count && index < side_table_capacity; ++index) {
            char path[path_capacity];
            if (!side_file_path(files, table, index, file, variant, path, sizeof path))
                continue;
            // An empty name names no file there can be.
            if (path[0] != '\0' && files != nullptr && files->exists != nullptr &&
                files->exists(files->context, path))
                continue;
            if (count < missing.size()) {
                auto& entry = missing[count];
                entry.side = index;
                entry.file = file;
                // The plain path, which the variant falls back to.
                if (path[0] == '\0')
                    std::snprintf(
                        entry.path,
                        sizeof entry.path,
                        "%s",
                        file == SideFile::panels ? "anims/.GAF" : "fonts/.FNT"
                    );
                else
                    std::snprintf(entry.path, sizeof entry.path, "%s", path);
                std::replace(entry.path, entry.path + std::strlen(entry.path), '\\', '/');
            }
            ++count;
        }
    return count;
}

bool side_table_past_limit_line(const SideTable& table, char* out, std::size_t capacity) noexcept {
    if (out != nullptr && capacity != 0)
        out[0] = '\0';
    if (table.past_limit == 0 || out == nullptr || capacity == 0)
        return false;
    if (table.past_limit == 1) {
        std::snprintf(
            out,
            capacity,
            "gamedata/sidedata.tdf: SIDE%u is past the %u sides this game plays (limits.sides) "
            "and is left out",
            table.limit,
            table.limit
        );
    } else {
        std::snprintf(
            out,
            capacity,
            "gamedata/sidedata.tdf: SIDE%u to SIDE%u are past the %u sides this game plays "
            "(limits.sides) and are left out",
            table.limit,
            table.limit + table.past_limit - 1u,
            table.limit
        );
    }
    return true;
}

} // namespace oa::data::defs
