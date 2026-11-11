// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/hud/resource_bar.hpp"
#include "oa/base/game_math.hpp"
#include "oa/present/colour_vision/colour_vision.hpp"

#include "oa/ui/hud/game_fields.hpp"

#include "oa/formats/tdf.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace oa::ui::hud {
using base::game_math::truncate_low32;

namespace {

using oa::formats::tdf::Block;

// Energy readouts past this magnitude switch to thousands.
constexpr float kReadoutThousandsBeyond = 99999.0F;

// Off keeps the UI slot. A mode uses the indices matched for that mode.
uint8_t figure_colour(const Game& game, bool produced) noexcept {
    namespace vision = oa::present::colour_vision;
    if (vision::active_mode() == vision::Mode::off)
        return readout_color(game, produced ? kReadoutProducedColor : kReadoutConsumedColor);
    return produced ? vision::resource_produced_index() : vision::resource_consumed_index();
}

// SIDEDATA x2/y2 are inclusive.
Rect tdf_rect(const Block* side, const char* name) {
    const auto* child = oa::formats::tdf::find_child(side, name);
    if (child == nullptr)
        return {};
    const auto x1 = oa::formats::tdf::get_int(child, "x1", 0);
    const auto y1 = oa::formats::tdf::get_int(child, "y1", 0);
    const auto x2 = oa::formats::tdf::get_int(child, "x2", 0);
    const auto y2 = oa::formats::tdf::get_int(child, "y2", 0);
    return {x1, y1, x2 - x1 + 1, y2 - y1 + 1};
}

void read_point(const Block* side, const char* name, int32_t& x, int32_t& y) {
    if (const auto* child = oa::formats::tdf::find_child(side, name)) {
        x = oa::formats::tdf::get_int(child, "x1", x);
        y = oa::formats::tdf::get_int(child, "y1", y);
    }
}

void read_point(const Block* side, const char* name, Rect& rect) {
    read_point(side, name, rect.x, rect.y);
}

bool is_side(std::string_view name, int32_t side) {
    if (name.size() != 5)
        return false;
    for (std::size_t i = 0; i < 4; ++i)
        if (std::tolower(static_cast<unsigned char>(name[i])) != "side"[i])
            return false;
    return name[4] - '0' == side;
}

} // namespace

bool parse_side_layout(std::string_view sidedata, int32_t side, SideLayout& layout) {
    oa::formats::tdf::OwnedDocument parsed;
    if (!parsed.parse(sidedata))
        return false;
    const Block* s = nullptr;
    for (uint32_t index = 0; index < oa::formats::tdf::child_count(parsed.root()); ++index) {
        const auto* section = oa::formats::tdf::child_at(parsed.root(), index);
        if (is_side(section->name, side)) {
            s = section;
            break;
        }
    }
    if (s == nullptr)
        return false;
    // The bars' colours are 0 when the section has no key, as 3.1c reads them.
    layout.metal_color = static_cast<uint8_t>(oa::formats::tdf::get_int(s, "metalcolor", 0));
    layout.energy_color = static_cast<uint8_t>(oa::formats::tdf::get_int(s, "energycolor", 0));
    if (const auto bar = tdf_rect(s, "METALBAR"); bar.width > 0)
        layout.metal_bar = bar;
    if (const auto bar = tdf_rect(s, "ENERGYBAR"); bar.width > 0)
        layout.energy_bar = bar;
    read_point(s, "METALNUM", layout.metal_num_x, layout.metal_num_y);
    read_point(s, "METALMAX", layout.metal_max_x, layout.metal_max_y);
    read_point(s, "METAL0", layout.metal_zero_x, layout.metal_zero_y);
    read_point(s, "METALPRODUCED", layout.metal_produced_x, layout.metal_produced_y);
    read_point(s, "METALCONSUMED", layout.metal_consumed_x, layout.metal_consumed_y);
    read_point(s, "ENERGYNUM", layout.energy_num_x, layout.energy_num_y);
    read_point(s, "ENERGYMAX", layout.energy_max_x, layout.energy_max_y);
    read_point(s, "ENERGY0", layout.energy_zero_x, layout.energy_zero_y);
    read_point(s, "ENERGYPRODUCED", layout.energy_produced_x, layout.energy_produced_y);
    read_point(s, "ENERGYCONSUMED", layout.energy_consumed_x, layout.energy_consumed_y);
    if (const auto name = tdf_rect(s, "UNITNAME"); name.y > 0)
        layout.unit_name = name;
    if (const auto damage = tdf_rect(s, "DAMAGEBAR"); damage.width > 0)
        layout.damage_bar = damage;
    read_point(s, "UNITMETALMAKE", layout.unit_metal_make);
    read_point(s, "UNITMETALUSE", layout.unit_metal_use);
    read_point(s, "UNITENERGYMAKE", layout.unit_energy_make);
    read_point(s, "UNITENERGYUSE", layout.unit_energy_use);
    if (const auto logo = tdf_rect(s, "LOGO"); logo.width > 0)
        layout.logo = logo;
    if (const auto logo = tdf_rect(s, "LOGO2"); logo.width > 0)
        layout.logo2 = logo;
    read_point(s, "MISSIONTEXT", layout.mission_text);
    read_point(s, "UNITNAME2", layout.unit_name2);
    if (const auto damage = tdf_rect(s, "DAMAGEBAR2"); damage.width > 0)
        layout.damage_bar2 = damage;
    read_point(s, "NAME", layout.name);
    read_point(s, "DESCRIPTION", layout.description);
    return true;
}

TroughColumns trough_columns(
    float shown, float capacity, float threshold, float store, int32_t left, int32_t right
) noexcept {
    TroughColumns columns{};
    if (!(capacity > 0.0F))
        return columns;
    const auto span = static_cast<double>(right - left);
    columns.fill = true;
    columns.fill_right = truncate_low32(span * shown / capacity + left);
    if (threshold > 0.0F && store > threshold) {
        columns.marker = true;
        columns.marker_left = truncate_low32(span * threshold / capacity + left);
    }
    return columns;
}

int32_t ease_toward(int32_t target, int32_t current) noexcept {
    const auto gap =
        static_cast<int32_t>(static_cast<uint32_t>(target) - static_cast<uint32_t>(current));
    int32_t step = 0;
    if (gap < 0) {
        step = gap / 8;
        if (step == 0)
            step = -1;
    } else if (gap > 0) {
        step = gap / 8;
        if (step == 0)
            step = 1;
    }
    return static_cast<int32_t>(static_cast<uint32_t>(current) + static_cast<uint32_t>(step));
}

void update_resource_readout(ResourceReadout& readout, Player& player, uint32_t tick) noexcept {
    readout.player = player.index;
    readout.energy = static_cast<float>(
        ease_toward(truncate_low32(player.energy), truncate_low32(readout.energy))
    );
    readout.metal = static_cast<float>(
        ease_toward(truncate_low32(player.metal), truncate_low32(readout.metal))
    );
    readout.energy_storage = player.energy_storage;
    readout.metal_storage = player.metal_storage;
    if (readout.energy > readout.energy_storage)
        readout.energy = readout.energy_storage;
    if (readout.metal > readout.metal_storage)
        readout.metal = readout.metal_storage;
    const auto timer = static_cast<uint32_t>(display_timer(player));
    if (timer < tick) {
        set_display_timer(player, static_cast<int32_t>(timer + kReadoutRefreshTicks));
        readout.energy_produced = oa::player_energy_produced(&player);
        readout.energy_requested = oa::player_energy_requested(&player);
        readout.metal_produced = oa::player_metal_produced(&player);
        readout.metal_requested = oa::player_metal_requested(&player);
    }
}

RateText format_energy_rate(const Game& game, float amount, bool produced) noexcept {
    RateText rate{};
    const auto whole = truncate_low32(amount);
    if (produced) {
        rate.color = figure_colour(game, true);
        if (amount > kReadoutThousandsBeyond)
            std::snprintf(rate.text, sizeof rate.text, "%dK", whole / 1000);
        else
            std::snprintf(rate.text, sizeof rate.text, "%d", whole);
    } else {
        rate.color = figure_colour(game, false);
        if (amount >= -kReadoutThousandsBeyond) {
            const auto magnitude =
                whole < 0 ? 0U - static_cast<uint32_t>(whole) : static_cast<uint32_t>(whole);
            std::snprintf(rate.text, sizeof rate.text, "%d", static_cast<int32_t>(magnitude));
        } else {
            std::snprintf(rate.text, sizeof rate.text, "%dK", whole / 1000);
        }
    }
    return rate;
}

RateText format_metal_rate(const Game& game, float amount, bool produced) noexcept {
    RateText rate{};
    rate.color = figure_colour(game, produced);
    const auto value = static_cast<double>(amount);
    std::snprintf(rate.text, sizeof rate.text, "%.1f", produced ? value : std::fabs(value));
    return rate;
}

RateText format_unit_rate(const Game& game, float amount, bool metal, bool produced) noexcept {
    RateText rate{};
    rate.color = figure_colour(game, produced);
    const auto value = amount > 0.0F ? static_cast<double>(amount) : 0.0;
    const char* format = metal ? (produced ? "+%.1f" : "-%.1f") : (produced ? "+%.0f" : "-%.0f");
    std::snprintf(rate.text, sizeof rate.text, format, value);
    return rate;
}

Rect radar_picture(int32_t map_width, int32_t map_height, int32_t size) noexcept {
    if (map_width <= 0 || map_height <= 0)
        return {0, 0, size, size};
    if (map_width < map_height) {
        const auto width = std::max(1, size * map_width / map_height);
        return {(size - width) / 2, 0, width, size};
    }
    const auto height = std::max(1, size * map_height / map_width);
    return {0, (size - height) / 2, size, height};
}

} // namespace oa::ui::hud
