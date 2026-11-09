// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Native live-unit readouts for an explicitly staged offline battle.
#include "oa/app/runtime.hpp"

#include "battle_counts.hpp"
#include "oa/ui/hud/shared_views.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string_view>

namespace oa::app {
namespace {

/// Canvas pixels at scale one between the banner and its surrounding content.
constexpr int banner_inset = 8;
/// Source pixels inside a readout's backing.
constexpr int banner_padding = 8;
/// Source pixels between the faction label and its live total.
constexpr int banner_line_gap = 2;
/// Readouts across the battlefield; additional players continue below them.
constexpr std::size_t banner_columns = 2;
/// The backing's share of black, in 256ths.
constexpr uint32_t banner_opacity = 232;
/// Bytes in a faction label and its player number, including the terminator.
constexpr std::size_t label_bytes = 64;
/// Bytes in a live total, including the terminator.
constexpr std::size_t count_bytes = 32;
/// Bytes in each palette entry: red, green, blue and the unused fourth byte.
constexpr std::size_t palette_stride = 4;
constexpr std::array<uint8_t, 3> banner_black{0, 0, 0};
constexpr std::array<uint8_t, 3> banner_white{235, 240, 246};

/// Returns a loaded side's bounded name, or no name for an unknown side.
std::string_view faction_name(const oa::World& world, const oa::PlayerSetupInfo* info) {
    if (info == nullptr || info->side >= oa::world_side_count(&world))
        return {};
    const auto* side = oa::world_side(&world, info->side);
    if (side == nullptr)
        return {};
    const auto end = std::find(std::begin(side->name), std::end(side->name), '\0');
    return {side->name, static_cast<std::size_t>(end - side->name)};
}

} // namespace

void Runtime::draw_battle_counts() {
    if (!battle_counts_shown_ || !match_ || options_.showcase != Showcase::skirmish_battle ||
        options_.stage_file.empty() || multiplayer_session() ||
        match_session_kind() != oa::data::campaign::SessionKind::skirmish)
        return;
    const auto* font = match_label_font();
    if (font == nullptr)
        return;
    const auto& world = match_->state();
    const auto counts = battle_counts::count_live_units(world);
    std::array<uint8_t, OA_PLAYER_COUNT> owners{};
    std::size_t players = 0;
    for (uint8_t owner = 0; owner < OA_PLAYER_COUNT; ++owner) {
        const auto& player = world.game.players[owner];
        const auto* info = oa::world_player_info(&world, &player);
        if (info != nullptr && (info->options & OA_SETUP_OPTION_WATCHER) != 0)
            continue;
        // A defeated player's readout stays visible at zero.
        if (player.in_use != 0 || player.units_created != 0 || counts[owner] != 0)
            owners[players++] = owner;
    }
    if (players == 0)
        return;
    const auto side_order = [&](uint8_t owner) {
        const auto* info = oa::world_player_info(&world, &world.game.players[owner]);
        return info != nullptr ? uint32_t{info->side} : oa::world_side_count(&world);
    };
    std::sort(owners.begin(), owners.begin() + players, [&](uint8_t left, uint8_t right) {
        const auto a = side_order(left), b = side_order(right);
        return a != b ? a < b : left < right;
    });
    std::array<std::array<char, label_bytes>, OA_PLAYER_COUNT> labels{};
    std::array<std::array<char, count_bytes>, OA_PLAYER_COUNT> totals{};
    int content_width =
        static_cast<int>(oa::formats::fnt::measure_text(*font, "LIVE UNITS: 0000000000"));
    for (std::size_t index = 0; index < players; ++index) {
        const auto owner = owners[index];
        const auto* info = oa::world_player_info(&world, &world.game.players[owner]);
        const auto faction = faction_name(world, info);
        if (faction.empty())
            std::snprintf(labels[index].data(), label_bytes, "Player %u", unsigned(owner) + 1);
        else
            std::snprintf(
                labels[index].data(),
                label_bytes,
                "%.*s  P%u",
                static_cast<int>(faction.size()),
                faction.data(),
                unsigned(owner) + 1
            );
        std::snprintf(totals[index].data(), count_bytes, "LIVE UNITS: %u", counts[owner]);
        content_width = std::max(
            content_width,
            static_cast<int>(oa::formats::fnt::measure_text(*font, labels[index].data()))
        );
    }
    const int columns = static_cast<int>(std::min(players, banner_columns));
    const int rows = static_cast<int>((players + banner_columns - 1) / banner_columns);
    const int line_height = static_cast<int>(oa::formats::fnt::line_height(*font));
    const int width = content_width + banner_padding * 2;
    const int height = line_height * 2 + banner_line_gap + banner_padding * 2;
    int scale = hud_text_scale();
    const int battlefield_width = match_layout_.battlefield_width();
    const int battlefield_height = match_layout_.battlefield_height();
    while (scale > 1 && ((width + banner_inset * 2) * scale > battlefield_width / columns ||
                         (height + banner_inset) * rows * scale > battlefield_height / 2))
        --scale;
    if ((width + banner_inset * 2) * scale > battlefield_width / columns ||
        (height + banner_inset) * rows * scale > battlefield_height / 2)
        return;
    const PanelText panel(*this);
    auto& target = paint_target();
    for (std::size_t index = 0; index < players; ++index) {
        const int column = static_cast<int>(index % banner_columns);
        const int row = static_cast<int>(index / banner_columns);
        const int center =
            match_layout_.battlefield_x() + (column * 2 + 1) * battlefield_width / (columns * 2);
        const int left = center - width * scale / 2;
        const int top =
            match_layout_.battlefield_y() + (banner_inset + row * (height + banner_inset)) * scale;
        const auto at = canvas_paint(left, top);
        if (!paint_world_blend(
                at.x, at.y, width * scale, height * scale, banner_black, banner_opacity
            ))
            renderer::blend_rect(
                target, at.x, at.y, width * scale, height * scale, banner_black, banner_opacity
            );
        const auto palette =
            std::size_t{oa::ui::hud::player_dot_color(world, owners[index])} * palette_stride;
        const std::array<uint8_t, 3> color{
            match_palette_[palette], match_palette_[palette + 1], match_palette_[palette + 2]
        };
        const auto label_width = oa::formats::fnt::measure_text(*font, labels[index].data());
        const auto total_width = oa::formats::fnt::measure_text(*font, totals[index].data());
        paint_text(
            *font,
            at.x + (width - static_cast<int>(label_width)) * scale / 2,
            at.y + banner_padding * scale,
            labels[index].data(),
            color,
            scale,
            false
        );
        paint_text(
            *font,
            at.x + (width - static_cast<int>(total_width)) * scale / 2,
            at.y + (banner_padding + line_height + banner_line_gap) * scale,
            totals[index].data(),
            banner_white,
            scale,
            false
        );
    }
}

} // namespace oa::app
