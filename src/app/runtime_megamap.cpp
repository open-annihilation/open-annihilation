// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// ui.megamap in a running match: its toggle and the wheel open and close it; open,
// it draws the whole map over the battlefield with small pictures of its
// indestructible features, the sight shading, the category icons of the
// units the minimap shows and the selection's sensor rings; its clicks act
// as the battlefield's do in the chosen interface type, at the map point
// they stand for. The enhanced minimap redraws the radar's picture at its
// own size. The Mouse wheel zoom setting, while on, leaves the hack off for
// this player.

#include "engine_settings_state.hpp"
#include "oa/app/runtime.hpp"
#include "oa/present/world_renderer/world_radar.hpp"
#include "oa/sim/selection/shortcuts.hpp"
#include "oa/ui/hud/megamap.hpp"
#include "oa/ui/hud/shared_views.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace oa::app {
namespace {

namespace hud = oa::ui::hud;
namespace wr = oa::present::world_renderer;

/// The icon file's place in the game folder.
constexpr const char* kIconFolder = "Icon/";
constexpr const char* kIconConfig = "Icon/iconcfg.ini";
/// The built-in icon set's files (the icon file's UseDefaultIcon).
constexpr const char* kDefaultCommanderIcon = "COMMANDER.PCX";
constexpr const char* kDefaultAirCombatIcon = "AIRCRAFTCOMBAT.PCX";
constexpr const char* kDefaultAirBuilderIcon = "AIRCONS.PCX";
constexpr const char* kDefaultBuilderIcon = "CONS.PCX";
constexpr const char* kDefaultCombatIcon = "MOBILECOMBAT.PCX";
constexpr const char* kDefaultFactoryIcon = "FACTORY.PCX";
constexpr const char* kDefaultBuildingIcon = "BUILDING.PCX";
constexpr const char* kDefaultNothingIcon = "NONE.PCX";
constexpr const char* kDefaultUnknownIcon = "UNKNOWN.PCX";
/// Interface sounds of opening and closing the megamap.
constexpr const char* kOpenSound = "Options";
constexpr const char* kCloseSound = "Previous";
/// Palette indices of one of the map's tiles, 32 by 32 pixels.
constexpr std::size_t tile_pixels = 32U * 32U;
/// Map pixels of a sight cell of the radar's grids.
constexpr int32_t kSightCellPixels = 32;
/// Battlefield pixels the pointer may move and still click.
constexpr int32_t kClickSlack = 3;
/// Palette index of the bars beside the map's picture: a dark grey.
constexpr uint8_t bar_color = 95;
/// Characters of a unit type's side the commander icons compare.
constexpr std::size_t kSideNameCompared = 8;

bool same_side(const char* side, const char* name) {
    for (std::size_t index = 0; index < kSideNameCompared; ++index) {
        const auto a = std::tolower(static_cast<unsigned char>(side[index]));
        const auto b = std::tolower(static_cast<unsigned char>(name[index]));
        if (a != b)
            return false;
        if (a == 0)
            return true;
    }
    return true;
}

bool commander_type(const oa::UnitDef& def) {
    return (def.abilities & OA_UNIT_DEF_ABILITY_SHOW_PLAYER_NAME) != 0 &&
           (def.flags & OA_UNIT_DEF_FLAG_HIDE_DAMAGE) != 0;
}

void add_type(const oa::data::defs::CategoryMask& mask, uint16_t type_id) {
    const uint32_t word = type_id >> 5;
    if (word < mask.word_count)
        mask.words[word] |= 1u << (type_id & 31u);
}

} // namespace

bool Runtime::megamap_on() const {
    // Settings not yet read hold their defaults, the wheel zoom on.
    const bool wheel_zoom = !engine_settings_ || engine_settings_->current.wheel_zoom;
    return match_ && ui_rules().megamap.enabled && !wheel_zoom;
}

bool Runtime::megamap_shown() const {
    return megamap_open_ && megamap_on();
}

MegamapIcon Runtime::load_megamap_icon(const std::string& file) {
    MegamapIcon icon;
    if (file.empty())
        return icon;
    try {
        const auto asset = assets_.read(std::string(kIconFolder) + file);
        const auto decoded = oa::decode_pcx(asset.bytes);
        if (!decoded.ok() || decoded.value->indices.empty())
            return icon;
        const auto& image = *decoded.value;
        icon.width = std::min<int32_t>(static_cast<int32_t>(image.width), kMegamapIconLimit);
        icon.height = std::min<int32_t>(static_cast<int32_t>(image.height), kMegamapIconLimit);
        icon.pixels.resize(
            static_cast<std::size_t>(icon.width) * static_cast<std::size_t>(icon.height)
        );
        for (int32_t y = 0; y < icon.height; ++y)
            for (int32_t x = 0; x < icon.width; ++x)
                icon.pixels[static_cast<std::size_t>(y * icon.width + x)] =
                    image.indices
                        [static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x)];
    } catch (const std::exception&) {
        icon = {};
    }
    return icon;
}

void Runtime::prepare_megamap() {
    auto& state = megamap_;
    if (state.prepared || !match_)
        return;
    state.prepared = true;
    const auto& rules = ui_rules().megamap;
    std::string text;
    try {
        const auto asset = assets_.read(kIconConfig);
        text.assign(asset.bytes.begin(), asset.bytes.end());
    } catch (const std::exception&) {
        text.clear();
    }
    state.config =
        hud::parse_icon_config(text, static_cast<uint32_t>(std::max(0, rules.icon_categories)));
    const auto& world = match_->state();
    auto& categories = unit_table_.tables.categories;
    const uint32_t type_bits = categories.words_per_mask * 32U;
    // The sets made here first, so that their masks keep their places.
    const auto& game = world.game;
    const uint32_t sides = oa::world_side_count(&world);
    const bool built_in = state.config.options.default_icons;
    const std::size_t own_count = built_in ? 7 : sides;
    state.own_words.assign(own_count, {});
    state.own_masks.clear();
    state.own_masks.reserve(own_count);
    for (auto& words : state.own_words)
        state.own_masks.push_back(oa::data::defs::category_mask_over(words, type_bits));
    state.entries.clear();
    if (built_in) {
        oa::sim::selection::ShortcutSets sets;
        oa::sim::selection::build_shortcut_sets(world, categories, type_bits, sets);
        const char* files[] = {
            kDefaultCommanderIcon,
            kDefaultAirCombatIcon,
            kDefaultAirBuilderIcon,
            kDefaultBuilderIcon,
            kDefaultCombatIcon,
            kDefaultFactoryIcon,
            kDefaultBuildingIcon,
        };
        for (uint32_t index = 0; index < world.unit_def_count; ++index) {
            const auto& def = world.unit_defs[index];
            const bool flies = (def.flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0;
            const bool builds = (def.flags & OA_UNIT_DEF_FLAG_BUILDER) != 0;
            if (commander_type(def))
                add_type(state.own_masks[0], def.type_id);
            if (flies && (def.flags & OA_UNIT_DEF_FLAG_HAS_WEAPONS) != 0 && !builds)
                add_type(state.own_masks[1], def.type_id);
            if (flies && builds)
                add_type(state.own_masks[2], def.type_id);
            if (oa::data::defs::category_mask_contains(&sets.constructors, def.type_id))
                add_type(state.own_masks[3], def.type_id);
            if (oa::data::defs::category_mask_contains(&sets.mobile_combat, def.type_id))
                add_type(state.own_masks[4], def.type_id);
            if (oa::data::defs::category_mask_contains(&sets.factories, def.type_id))
                add_type(state.own_masks[5], def.type_id);
            if (def.yard_map != 0 && !commander_type(def))
                add_type(state.own_masks[6], def.type_id);
        }
        for (std::size_t index = 0; index < own_count; ++index)
            state.entries.push_back({load_megamap_icon(files[index]), &state.own_masks[index]});
        state.unknown = load_megamap_icon(kDefaultUnknownIcon);
        state.nothing = load_megamap_icon(kDefaultNothingIcon);
    } else {
        // Each side's commanders first, under the side's name.
        for (uint32_t side = 0; side < sides; ++side) {
            const oa::Side* record = oa::world_side(&world, side);
            if (record != nullptr) {
                for (uint32_t index = 0; index < world.unit_def_count; ++index) {
                    const auto& def = world.unit_defs[index];
                    if (commander_type(def) && same_side(def.side, record->name))
                        add_type(state.own_masks[side], def.type_id);
                }
            }
            const char* side_name = record != nullptr ? record->name : "";
            std::string name(side_name, ::strnlen(side_name, sizeof(oa::Side::name)));
            state.entries.push_back({load_megamap_icon(name + ".PCX"), &state.own_masks[side]});
        }
        for (const auto& line : state.config.lines) {
            // A line must not add a category: adding one can move the masks
            // earlier lines point at.
            state.entries.push_back(
                {load_megamap_icon(line.file), hud::icon_category_mask(categories, line)}
            );
        }
        state.unknown = load_megamap_icon(state.config.unknown_file);
        state.nothing = load_megamap_icon(state.config.nothing_file);
    }
    // The features the picture shows, as the map placed them: only the
    // indestructible ones that cannot be reclaimed.
    state.features.clear();
    if (rules.feature_blobs) {
        const auto width = static_cast<int32_t>(game.map_width);
        const auto cells = static_cast<std::size_t>(game.map_width) * game.map_height;
        for (std::size_t index = 0; index < cells && width > 0; ++index) {
            const auto& plot = world.plots[index];
            if (plot.feature >= OA_PLOT_FEATURE_RESERVED ||
                plot.feature >= world.feature_def_count ||
                !hud::megamap_draws_feature(world.feature_defs[plot.feature]))
                continue;
            state.features.push_back(
                {static_cast<int32_t>(index % static_cast<std::size_t>(width)),
                 static_cast<int32_t>(index / static_cast<std::size_t>(width)),
                 plot.feature,
                 plot.height}
            );
        }
    }
    state.terrain.clear();
    state.layout = {};
}

uint8_t Runtime::map_terrain_pixel(int32_t map_x, int32_t map_z) const {
    const auto& map = *selected_tnt_;
    const auto tiles_per_row = static_cast<int32_t>(map.tile_width);
    const auto tile_slot = static_cast<std::size_t>((map_z / 32) * tiles_per_row + map_x / 32);
    if (tile_slot >= map.tile_indices.size())
        return 0;
    auto tile = static_cast<std::size_t>(map.tile_indices[tile_slot]);
    if (tile >= map.tile_count)
        tile = 0;
    const auto at = tile * 1024U + static_cast<std::size_t>((map_z % 32) * 32 + map_x % 32);
    return at < map.tile_palette_indices.size() ? map.tile_palette_indices[at] : uint8_t{0};
}

std::vector<uint8_t> Runtime::downscale_megamap_terrain(const oa::ui::hud::MegamapLayout& layout) {
    if (!selected_tnt_ || layout.width <= 0 || layout.height <= 0)
        return {};
    const auto& map = *selected_tnt_;
    // The colours the map's tiles use, which the picture keeps to.
    const auto tile_bytes = std::min(
        map.tile_palette_indices.size(), static_cast<std::size_t>(map.tile_count) * tile_pixels
    );
    const auto colours = hud::perceptual_palette(
        match_palette_, std::span<const uint8_t>(map.tile_palette_indices.data(), tile_bytes)
    );
    hud::TerrainSource source{};
    source.user = this;
    source.pixel = [](void* user, int32_t x, int32_t z) {
        return static_cast<const Runtime*>(user)->map_terrain_pixel(x, z);
    };
    return hud::downscale_terrain(
        source,
        layout.map_width,
        layout.map_height,
        layout.width,
        layout.height,
        match_palette_,
        ui_rules().megamap.dither,
        &colours
    );
}

void Runtime::build_megamap_terrain(const oa::ui::hud::MegamapLayout& layout) {
    auto& state = megamap_;
    state.layout = layout;
    state.terrain = downscale_megamap_terrain(layout);
    if (state.terrain.empty() || !match_)
        return;
    const auto& world = match_->state();
    // Each type's first standing frame, shrunk once for this picture.
    std::vector<std::optional<hud::ShrunkPicture>> shrunk(world.feature_def_count);
    for (const auto& feature : state.features) {
        if (feature.def >= world.feature_def_count)
            continue;
        const auto& def = world.feature_defs[feature.def];
        const auto* frame = feature_sequence_image(def.seq_name, 0);
        const auto spot = hud::megamap_feature_spot(
            layout,
            feature.cell_x,
            feature.cell_z,
            def.footprint_x,
            def.footprint_z,
            feature.ground_height,
            frame != nullptr ? std::max<int32_t>(frame->width, frame->height) : 0
        );
        auto& picture = shrunk[feature.def];
        if (frame != nullptr && !picture)
            picture = hud::shrink_picture(
                frame->pixels,
                frame->width,
                frame->height,
                frame->transparency_index,
                match_palette_,
                spot.longest
            );
        if (picture && !picture->rgba.empty()) {
            hud::blend_picture(
                state.terrain,
                layout.width,
                layout.height,
                *picture,
                spot.x - picture->width / 2,
                hud::feature_picture_top(spot.y, frame->origin_y, frame->height, picture->height),
                match_palette_
            );
            continue;
        }
        // A feature without a picture, such as a 3DO one, takes a mark.
        const uint8_t color = hud::feature_mark_color(def);
        for (int32_t y = spot.y - 1; y < spot.y - 1 + hud::feature_mark_size; ++y)
            for (int32_t x = spot.x - 1; x < spot.x - 1 + hud::feature_mark_size; ++x)
                if (x >= 0 && y >= 0 && x < layout.width && y < layout.height)
                    state.terrain[static_cast<std::size_t>(y * layout.width + x)] = color;
    }
}

void Runtime::set_megamap_open(bool open) {
    if (open == megamap_open_ || !megamap_on())
        return;
    megamap_open_ = open;
    megamap_.pressed = false;
    // The icons drawn while it was last open have moved since.
    megamap_.marks.clear();
    play_match_interface_sound(open ? kOpenSound : kCloseSound);
}

bool Runtime::take_megamap_toggle(const SDL_KeyboardEvent& key) {
    if (!megamap_on() || key.repeat || !action_key_matches("megamap.toggle", key))
        return false;
    set_megamap_open(!megamap_open_);
    return true;
}

bool Runtime::megamap_wheel(float amount, float x, float y) {
    if (!megamap_on() || amount == 0.0F)
        return false;
    if (amount < 0.0F) {
        set_megamap_open(true);
        return true;
    }
    if (!megamap_open_)
        return false;
    // Rolled away: back to the battlefield, centred where the pointer was.
    const auto point = hud::megamap_map_point(
        megamap_.layout,
        static_cast<int32_t>(x) - match_layout_.left,
        static_cast<int32_t>(y) - match_layout_.top
    );
    set_megamap_open(false);
    if (point)
        set_camera_position(
            (*point)[0] - visible_map_width() / 2, (*point)[1] - visible_map_height() / 2, 0
        );
    return true;
}

void Runtime::draw_megamap() {
    if (!megamap_open_ || !megamap_on() || !selected_tnt_)
        return;
    prepare_megamap();
    auto& state = megamap_;
    auto& world = match_->state();
    const auto& game = world.game;
    // The map is drawn in the overlays' area (the battlefield, or with the
    // touch controls on, the part of it they leave clear), on the
    // battlefield's layer, whose origin is the battlefield's corner.
    const auto area = overlay_area();
    const auto layout = hud::megamap_layout(
        area.x - match_layout_.battlefield_x(),
        area.y - match_layout_.battlefield_y(),
        area.width,
        area.height,
        game.map_pixel_width,
        game.map_pixel_height
    );
    if (layout.width <= 0)
        return;
    if (state.terrain.empty() || layout.width != state.layout.width ||
        layout.height != state.layout.height || layout.left != state.layout.left ||
        layout.top != state.layout.top)
        build_megamap_terrain(layout);
    // The bars, across the whole battlefield, then the terrain shaded by
    // what the viewer has mapped and sees.
    const uint8_t unexplored = game.ui_colors[wr::ui_color_unexplored];
    fill_hud_rect(
        0, 0, match_layout_.battlefield_width(), match_layout_.battlefield_height(), bar_color
    );
    auto& target = paint_target();
    const auto& radar = radar_state_;
    const int grid_w = std::max(1, game.map_width / 2);
    const int grid_h = std::max(1, game.map_height / 2);
    const auto viewer_bit = static_cast<uint16_t>(1u << (match_view_player() & 0x1fu));
    // The picture's columns that lie on the target, and the sight cell's
    // column under each, the same for every row.
    const int32_t first_x = std::max(0, -layout.left);
    const int32_t end_x = std::min(layout.width, static_cast<int32_t>(target.width) - layout.left);
    std::vector<std::size_t> column_cells(static_cast<std::size_t>(std::max(0, end_x - first_x)));
    for (int32_t x = first_x; x < end_x; ++x) {
        const auto map_x =
            static_cast<int32_t>(static_cast<int64_t>(x) * layout.map_width / layout.width);
        column_cells[static_cast<std::size_t>(x - first_x)] =
            static_cast<std::size_t>(std::min(grid_w - 1, map_x / kSightCellPixels));
    }
    for (int32_t y = 0; y < layout.height; ++y) {
        const int32_t py = layout.top + y;
        if (py < 0 || py >= static_cast<int32_t>(target.height) || first_x >= end_x)
            continue;
        const auto map_z =
            static_cast<int32_t>(static_cast<int64_t>(y) * layout.map_height / layout.height);
        const int gy = std::min(grid_h - 1, map_z / kSightCellPixels);
        const auto row_cell = static_cast<std::size_t>(gy) * static_cast<std::size_t>(grid_w);
        const uint8_t* terrain = state.terrain.data() + static_cast<std::size_t>(y * layout.width);
        uint8_t* out = target.rgb.data() + (static_cast<std::size_t>(py) * target.width +
                                            static_cast<std::size_t>(layout.left + first_x)) *
                                               3U;
        for (int32_t x = first_x; x < end_x; ++x, out += 3) {
            const auto cell = row_cell + column_cells[static_cast<std::size_t>(x - first_x)];
            uint8_t color = terrain[x];
            if (cell < radar.sight_bits.size() && (radar.sight_bits[cell] & viewer_bit) == 0)
                color = unexplored;
            else if (cell < radar.coverage.size() && radar.coverage[cell] == 0)
                color = radar.gray_table[color];
            const auto pal = static_cast<std::size_t>(color) * 4U;
            out[0] = match_palette_[pal];
            out[1] = match_palette_[pal + 1];
            out[2] = match_palette_[pal + 2];
        }
    }
    // The units the minimap shows, by their icons; the hovered one is found as they are drawn.
    const bool full_radar = (game.console_flags & wr::console_flag_full_radar) != 0;
    const bool limited = (game.visibility_flags & wr::visibility_flags_radar_limited) != 0;
    const bool allied = ui_rules().allied_unit_display.enabled;
    const auto viewer = match_view_player();
    const auto pointer_x = state.pointer_x;
    const auto pointer_y = state.pointer_y;
    state.hovered = 0;
    state.marks.clear();
    std::vector<const oa::data::defs::CategoryMask*> masks;
    masks.reserve(state.entries.size());
    for (const auto& entry : state.entries)
        masks.push_back(entry.types);
    const auto draw_icon = [&](const MegamapIcon& icon,
                               int32_t cx,
                               int32_t cy,
                               uint8_t dot,
                               bool selected,
                               bool hovered) {
        if (icon.pixels.empty()) {
            // A missing picture draws a square of the owner's colour.
            fill_hud_rect(cx - 2, cy - 2, 5, 5, dot);
            return;
        }
        const int32_t left = cx - icon.width / 2, top = cy - icon.height / 2;
        for (int32_t y = 0; y < icon.height; ++y)
            for (int32_t x = 0; x < icon.width; ++x) {
                const auto shown = hud::icon_pixel(
                    state.config.options,
                    icon.pixels[static_cast<std::size_t>(y * icon.width + x)],
                    dot,
                    selected,
                    hovered
                );
                if (shown)
                    fill_hud_rect(left + x, top + y, 1, 1, *shown);
            }
    };
    for (uint32_t slot = 0; slot < world.unit_slot_count; ++slot) {
        const oa::Unit& unit = world.units[slot];
        if (unit.type_index == 0)
            continue;
        const auto* owner = oa::world_unit_owner(&world, &unit);
        const bool own = allied ? owner != nullptr && viewer < sizeof owner->alliance &&
                                      owner->alliance[viewer] != 0
                                : unit.owner_index == viewer;
        if (!full_radar && limited && (unit.flags & OA_UNIT_FLAG_RADAR_CONTACT) == 0 && !own)
            continue;
        bool visible = own;
        if (!visible) {
            try {
                visible = match_->unit_shown(viewer, unit.id);
            } catch (const std::exception&) {
                visible = false;
            }
        }
        const auto at = hud::megamap_point(layout, unit.position.x >> 16, unit.position.z >> 16);
        std::size_t line = 0;
        const auto choice = hud::choose_icon(visible, unit.type_index, masks, line);
        const MegamapIcon& icon = choice == hud::IconChoice::category  ? state.entries[line].icon
                                  : choice == hud::IconChoice::unknown ? state.unknown
                                                                       : state.nothing;
        const int32_t half_w = std::max(2, icon.width / 2), half_h = std::max(2, icon.height / 2);
        state.marks.push_back(
            {unit.id, at[0] - half_w, at[1] - half_h, at[0] + half_w, at[1] + half_h}
        );
        const bool hovered = pointer_x >= at[0] - half_w && pointer_x < at[0] + half_w &&
                             pointer_y >= at[1] - half_h && pointer_y < at[1] + half_h;
        if (hovered)
            state.hovered = unit.id;
        const bool selected = (unit.flags & OA_UNIT_FLAG_SELECTED) != 0;
        draw_icon(
            icon, at[0], at[1], hud::player_dot_color(world, unit.owner_index), selected, hovered
        );
        if (hovered && state.config.options.circle_hover)
            draw_megamap_ring(
                at[0], at[1], std::max(half_w, half_h) + 1, state.config.options.hover_color
            );
        // The selection's sensor and anti-nuke rings.
        const auto* def = oa::world_unit_def_of(&world, &unit);
        if (selected && def != nullptr) {
            std::array<hud::MegamapRing, 5> rings{};
            const auto& minimums = ui_rules().megamap.ring_minimums;
            const auto count = hud::megamap_rings(world, unit, *def, minimums, rings);
            for (uint32_t index = 0; index < count; ++index)
                draw_megamap_ring(
                    at[0],
                    at[1],
                    static_cast<int32_t>(
                        static_cast<int64_t>(rings[index].radius) * layout.width / layout.map_width
                    ),
                    rings[index].color
                );
        }
    }
    // A drag box being drawn, in the minimap's marks colour.
    const uint8_t marks = game.ui_colors[wr::ui_color_radar_marks];
    if (state.pressed && (std::abs(pointer_x - state.press_x) > kClickSlack ||
                          std::abs(pointer_y - state.press_y) > kClickSlack)) {
        const int32_t left = std::min(pointer_x, state.press_x),
                      right = std::max(pointer_x, state.press_x);
        const int32_t top = std::min(pointer_y, state.press_y),
                      bottom = std::max(pointer_y, state.press_y);
        fill_hud_rect(left, top, right - left + 1, 1, marks);
        fill_hud_rect(left, bottom, right - left + 1, 1, marks);
        fill_hud_rect(left, top, 1, bottom - top + 1, marks);
        fill_hud_rect(right, top, 1, bottom - top + 1, marks);
    }
}

void Runtime::draw_megamap_ring(int32_t x, int32_t y, int32_t radius, uint8_t color) {
    if (radius <= 0)
        return;
    // Eight points of the ring per step around a quarter.
    int32_t dx = radius, dy = 0, error = 1 - radius;
    while (dx >= dy) {
        const std::array<std::array<int32_t, 2>, 8> points{{
            {x + dx, y + dy},
            {x + dy, y + dx},
            {x - dy, y + dx},
            {x - dx, y + dy},
            {x - dx, y - dy},
            {x - dy, y - dx},
            {x + dy, y - dx},
            {x + dx, y - dy},
        }};
        for (const auto& point : points)
            fill_hud_rect(point[0], point[1], 1, 1, color);
        ++dy;
        if (error < 0) {
            error += 2 * dy + 1;
        } else {
            --dx;
            error += 2 * (dy - dx) + 1;
        }
    }
}

bool Runtime::megamap_pointer(const SDL_Event& event, float x, float y) {
    // A page opened over the match, such as the save page, takes the pointer
    // in its own pixels.
    if (screen_ != Screen::match || !megamap_open_ || !megamap_on())
        return false;
    auto& state = megamap_;
    const int32_t px = static_cast<int32_t>(x) - match_layout_.left;
    const int32_t py = static_cast<int32_t>(y) - match_layout_.top;
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        state.pointer_x = px;
        state.pointer_y = py;
        return state.pressed;
    }
    // Presses are taken in the overlays' area, where the map is drawn, as
    // the battlefield would take them: not on a touch control, a placed part
    // of the HUD or a button of a panel over the battlefield, such as the
    // exit menu's, and while the in-game menu or another panel holds the
    // match, only a right press beside the menu. The release of a press
    // taken is taken wherever it lands, and any other in the area off a
    // button.
    const auto area = overlay_area();
    const bool in_area = x >= static_cast<float>(area.x) && y >= static_cast<float>(area.y) &&
                         x < static_cast<float>(area.x + area.width) &&
                         y < static_cast<float>(area.y + area.height) && !placed_hud_covers(x, y) &&
                         !hovered_;
    const uint8_t select_button = match_select_button();
    const uint8_t command_button = match_command_button();
    const bool held_by_panel =
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && match_paused_ &&
        !(event.button.button == command_button && right_press_beside_menu(x, y));
    if ((!in_area || held_by_panel) && !(state.pressed && event.type == SDL_EVENT_MOUSE_BUTTON_UP))
        return false;
    auto& world = match_->state();
    world.game.local_player_index = match_local_player_;
    state.pointer_x = px;
    state.pointer_y = py;
    // The icon under the button is the one drawn there, though no frame has
    // drawn the pointer there yet, as for a finger's tap.
    state.hovered = state.unit_at(px, py);
    // The press's buttons and modifiers, which queueing() reads, as the
    // battlefield's.
    record_pointer_event(event);
    const auto ground = [&] { return megamap_ground(x, y); };
    if (event.button.button == command_button && command_button != select_button) {
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
            megamap_right_press(state.hovered, ground());
        return true;
    }
    if (event.button.button != select_button)
        return true;
    const bool shift = (input_modifiers(ModifierUse::selection) & SDL_KMOD_SHIFT) != 0;
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (event.button.clicks >= 2 && event.button.clicks % 2 == 0 && state.hovered != 0) {
            // A double-click on one of the viewer's units selects every unit of its type.
            const auto* unit = oa::world_unit_at(&world, state.hovered);
            if (unit != nullptr && unit->owner_index == match_local_player_) {
                const auto type = unit->type_index;
                select_units_matching([&](const oa::sim::unit_spawn::Slot& slot) {
                    return slot.unit && slot.unit->type_index == type;
                });
            }
            state.pressed = false;
            return true;
        }
        state.pressed = true;
        state.press_x = px;
        state.press_y = py;
        return true;
    }
    if (event.type != SDL_EVENT_MOUSE_BUTTON_UP || !state.pressed)
        return true;
    state.pressed = false;
    const bool dragged =
        std::abs(px - state.press_x) > kClickSlack || std::abs(py - state.press_y) > kClickSlack;
    if (!dragged) {
        megamap_click(state.hovered, ground());
        return true;
    }
    // A box selects the viewer's units in it, added to the selection with Shift.
    if (!shift)
        clear_local_selection();
    const int32_t left = std::min(px, state.press_x), right = std::max(px, state.press_x);
    const int32_t top = std::min(py, state.press_y), bottom = std::max(py, state.press_y);
    for (auto& slot : match_->world().slots) {
        if (slot.unit_index == 0 || slot.unit == nullptr ||
            slot.owner_index != match_local_player_ || !match_->selectable(slot.unit_index))
            continue;
        const auto& record = world.units[slot.unit_index];
        const auto at =
            hud::megamap_point(state.layout, record.position.x >> 16, record.position.z >> 16);
        if (at[0] >= left && at[0] <= right && at[1] >= top && at[1] <= bottom)
            slot.unit->flags |= OA_UNIT_FLAG_SELECTED;
    }
    selected_match_unit_ = 0;
    adopt_selected_units();
    // A box that leaves nothing selected drops the selection, and the unit
    // info panel closes with it, as on the battlefield.
    if (!has_local_selection())
        close_unit_info();
    return true;
}

std::optional<oa::sim::ground_orders::Point> Runtime::megamap_ground(float x, float y) {
    const auto point = hud::megamap_map_point(
        megamap_.layout,
        static_cast<int32_t>(x) - match_layout_.left,
        static_cast<int32_t>(y) - match_layout_.top
    );
    return point ? map_world_point((*point)[0], (*point)[1]) : std::nullopt;
}

uint16_t Runtime::megamap_unit_at(float x, float y) const {
    return megamap_.unit_at(
        static_cast<int32_t>(x) - match_layout_.left, static_cast<int32_t>(y) - match_layout_.top
    );
}

uint8_t Runtime::megamap_cursor_at(float x, float y) {
    return pick_map_cursor(megamap_unit_at(x, y), megamap_ground(x, y));
}

void Runtime::megamap_click(
    uint16_t target, const std::optional<oa::sim::ground_orders::Point>& ground
) {
    namespace input = oa::sim::gameplay_input;
    if (match_command_ == MatchCommand::build) {
        if (ground)
            place_pending_build_at(*ground);
        return;
    }
    if (match_command_ == MatchCommand::patrol) {
        std::ignore = issue_map_orders(ground, target);
        return;
    }
    // An armed MOVE on open ground, or on a unit whose cursor takes no click,
    // moves the selection to the ground there and ends; on a unit, the cursor
    // decides first, as on the battlefield.
    const bool move = match_command_ == MatchCommand::move;
    const auto move_to_ground = [&] {
        if (!ground)
            return;
        issue_selection_move(*ground, target, queueing());
        finish_issued_command();
    };
    if (move && target == 0) {
        move_to_ground();
        return;
    }
    auto& world = match_->state();
    const auto cursor = static_cast<input::OrderCursor>(pick_map_cursor(target, ground));
    const auto command = input::pointer_command(world.game);
    switch (input::click_action(world, command, cursor)) {
    case input::ClickAction::none:
        if (move)
            move_to_ground();
        return;
    case input::ClickAction::select_unit: {
        // The unit is selected in place of the others, or with Shift flipped
        // in or out of the selection, as on the battlefield.
        const bool toggle = (input_modifiers(ModifierUse::selection) & SDL_KMOD_SHIFT) != 0;
        if (!toggle)
            clear_local_selection();
        auto& slot = match_->world().slots[target];
        if (target != 0 && slot.unit != nullptr && slot.owner_index == match_local_player_ &&
            match_->selectable(target)) {
            slot.unit->flags ^= OA_UNIT_FLAG_SELECTED;
            if (!toggle)
                selected_match_unit_ = target;
        }
        adopt_selected_units();
        return;
    }
    case input::ClickAction::clear_selection:
        // The unit info panel closes with the selection, as on the
        // battlefield.
        clear_local_selection();
        close_unit_info();
        apply_match_hud_for_selection();
        return;
    case input::ClickAction::issue_command:
        break;
    }
    if (target == 0 && !ground)
        return;
    const auto issued = issue_selection_orders(command, target, ground, queueing());
    if (!issued.empty())
        status_ = std::string(issued);
    // An armed MOVE ends with the click whether or not it gave an order;
    // another command that gave none stays armed.
    if (!issued.empty() || move)
        finish_issued_command();
}

void Runtime::megamap_right_press(
    uint16_t target, const std::optional<oa::sim::ground_orders::Point>& ground
) {
    namespace input = oa::sim::gameplay_input;
    auto& world = match_->state();
    std::ignore = pick_map_cursor(target, ground);
    switch (input::right_press(world.game)) {
    case input::RightPress::cancel_command:
        reset_match_command();
        pending_build_type_ = 0;
        input::set_pointer_command(world.game, input::OrderCommand::default_order);
        return;
    case input::RightPress::mouse_look:
    case input::RightPress::clear_selection:
        // The megamap has no view to look round: Control drops the selection
        // too. The unit info panel closes with it, and beside the in-game
        // menu the menu closes and the match it held runs again, as on the
        // battlefield.
        clear_local_selection();
        close_unit_info();
        if (match_paused_)
            resume_match_pause();
        else
            apply_match_hud_for_selection();
        return;
    case input::RightPress::default_order:
        break;
    case input::RightPress::none:
    case input::RightPress::radar_scroll:
    case input::RightPress::radar_jump:
        return;
    }
    if (target == 0 && !ground)
        return;
    const auto issued =
        issue_selection_orders(input::OrderCommand::default_order, target, ground, queueing());
    if (!issued.empty())
        status_ = std::string(issued);
}

void Runtime::megamap_wheel_zoom_changed() {
    // Closed without its sound: the setting closes it, not the player's key.
    if (!megamap_on()) {
        megamap_open_ = false;
        megamap_.pressed = false;
    }
    const auto& rules = ui_rules().megamap;
    if (!rules.enabled || !rules.enhanced_minimap || radar_state_.surfaces.picture == nullptr)
        return;
    enhance_radar_picture();
    radar_state_.reset_sight = true;
}

void Runtime::enhance_radar_picture() {
    if (!match_ || !selected_tnt_ || !ui_rules().megamap.enabled ||
        !ui_rules().megamap.enhanced_minimap)
        return;
    auto* picture = radar_state_.surfaces.picture;
    const auto& game = match_->state().game;
    if (picture == nullptr || picture->pixels == nullptr || game.map_pixel_width <= 0 ||
        game.map_pixel_height <= 0)
        return;
    // The game's own picture, kept as the radar drew it, comes back while the
    // hack is off for this player.
    auto& kept = radar_state_.game_picture;
    const auto size =
        static_cast<std::size_t>(picture->pitch) * static_cast<std::size_t>(picture->height);
    if (kept.empty())
        kept.assign(picture->pixels, picture->pixels + size);
    if (!megamap_on()) {
        if (kept.size() == size)
            std::copy(kept.begin(), kept.end(), picture->pixels);
        return;
    }
    const auto& map = *selected_tnt_;
    hud::TerrainSource source{};
    int32_t source_w = game.map_pixel_width, source_h = game.map_pixel_height;

    struct Minimap {
        const uint8_t* pixels;
        int32_t stride;
    } minimap{nullptr, 0};

    if (map.minimap.has_value() && map.minimap->width > 0 && map.minimap->height > 0 &&
        map.minimap->palette_indices.size() >=
            static_cast<std::size_t>(map.minimap->width) * map.minimap->height) {
        // The map file's own minimap, as far as it shows the map.
        minimap = {map.minimap->palette_indices.data(), static_cast<int32_t>(map.minimap->width)};
        source_w = 2 * game.radar_width;
        source_h = 2 * game.radar_height;
        source.user = &minimap;
        source.pixel = [](void* user, int32_t x, int32_t z) {
            const auto& from = *static_cast<const Minimap*>(user);
            return from.pixels
                [static_cast<std::size_t>(z) * static_cast<std::size_t>(from.stride) +
                 static_cast<std::size_t>(x)];
        };
    } else {
        source.user = this;
        source.pixel = [](void* user, int32_t x, int32_t z) {
            return static_cast<const Runtime*>(user)->map_terrain_pixel(x, z);
        };
    }
    const auto pixels = hud::downscale_terrain(
        source,
        source_w,
        source_h,
        picture->width,
        picture->height,
        match_palette_,
        ui_rules().megamap.dither,
        nullptr
    );
    if (pixels.empty())
        return;
    for (int32_t y = 0; y < picture->height; ++y)
        std::memcpy(
            picture->pixels + y * picture->pitch,
            pixels.data() + static_cast<std::size_t>(y) * picture->width,
            static_cast<std::size_t>(picture->width)
        );
}

} // namespace oa::app
