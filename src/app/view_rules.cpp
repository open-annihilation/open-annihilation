// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/app/view_rules.hpp"
#include "oa/data/input_actions/actions.hpp"
#include "oa/present/world_renderer/world_display_modes.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_state/main_menu.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"
#include "oa/ui/key_bindings/legacy.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace oa::app::view_rules {

namespace {

/// Map pixels to cells.
constexpr double cursor_to_cells = 1.0 / 16.0;
/// The most yard map cells the geothermal test reads.
constexpr std::size_t yard_text_cells = 64;
/// The compiled yard map cell of a 'G' (geothermal) character.
constexpr uint8_t geothermal_yard_cell = 0x8f;

} // namespace

sim::match_runtime::DisplayRules
match_display_rules(const data::mod_profile::UiRules& ui) noexcept {
    sim::match_runtime::DisplayRules rules{};
    const auto& voices = ui.unit_voice_fixes;
    rules.reclaim_voice_once = voices.reclaim_voice_once;
    rules.vtol_reclaim_voice_at_start = voices.vtol_reclaim_voice_at_start;
    rules.landing_fail_voice = static_cast<uint8_t>(voices.landing_fail_voice);
    const auto& effects = ui.effects_tweaks;
    rules.end_smoke_explosion = effects.endsmoke_explosion;
    rules.explosion_smoke_column = effects.explosion_smoke_puff;
    return rules;
}

oa::ui::engine_settings::ExplosionFlash explosion_flash_drawn(
    const data::mod_profile::UiRules& ui, oa::ui::engine_settings::ExplosionFlash player
) noexcept {
    using oa::ui::engine_settings::ExplosionFlash;
    // Each level by its strength, so that the lower one wins.
    const auto strength = [](ExplosionFlash level) {
        switch (level) {
        case ExplosionFlash::off:
            return 0;
        case ExplosionFlash::reduced:
            return 1;
        case ExplosionFlash::full:
            break;
        }
        return 2;
    };
    ExplosionFlash designed = ExplosionFlash::full;
    if (ui.explosion_flash.enabled) {
        switch (ui.explosion_flash.level) {
        case data::mod_profile::UiExplosionFlashLevel::off:
            designed = ExplosionFlash::off;
            break;
        case data::mod_profile::UiExplosionFlashLevel::reduced:
            designed = ExplosionFlash::reduced;
            break;
        case data::mod_profile::UiExplosionFlashLevel::full:
            break;
        }
    }
    return strength(designed) < strength(player) ? designed : player;
}

bool victory_announcement_due(uint32_t tick, uint32_t& last_tick) noexcept {
    const bool due = tick < last_tick || tick - last_tick > victory_announcement_interval;
    last_tick = tick;
    return due;
}

MusicSource music_source(const data::mod_profile::UiRules& ui) noexcept {
    using data::mod_profile::UiAudioMusic;
    switch (ui.audio.music) {
    case UiAudioMusic::numbered_mp3:
        return MusicSource::numbered_mp3;
    case UiAudioMusic::folder_scan:
        return MusicSource::folder_scan;
    case UiAudioMusic::cd:
        break;
    }
    return MusicSource::disc;
}

int32_t minimum_mode_height(const data::mod_profile::UiRules& ui) noexcept {
    return ui.display_modes.min_height_768 ? present::world_renderer::tall_minimum_mode_height
                                           : present::world_renderer::minimum_mode_height;
}

oa::ui::frontend_state::initialization::DisplayModeSetting
display_mode_setting(const data::mod_profile::UiRules& ui) noexcept {
    namespace init = oa::ui::frontend_state::initialization;
    return ui.display_modes.min_height_768 ? init::tall_display_mode_setting
                                           : init::base_display_mode_setting;
}

std::string screenshot_safe_text(std::string_view text) {
    std::string safe{text};
    for (auto& c : safe)
        if (screenshot_forbidden_characters.find(c) != std::string_view::npos)
            c = '_';
    return safe;
}

std::string
screenshot_file_name(std::string_view date, const ScreenshotScene* scene, int32_t index) {
    std::string name = screenshot_safe_text(date);
    if (scene != nullptr) {
        name += screenshot_safe_text(scene->map);
        name += " - ";
        name += screenshot_safe_text(scene->players[0]);
        for (std::size_t slot = 1; slot < scene->players.size(); ++slot)
            if (!scene->players[slot].empty()) {
                name += ", ";
                name += screenshot_safe_text(scene->players[slot]);
            }
        name += ' ';
    } else {
        name += "SHOT";
    }
    char number[16]{};
    std::snprintf(number, sizeof number, "%.4i", static_cast<int>(index));
    name += number;
    name += ".pcx";
    return name;
}

int32_t click_snap_radius(int32_t setting, int32_t maximum) noexcept {
    const auto bound = std::min(std::max(maximum, 0), click_snap_radius_limit);
    return std::clamp(setting, 0, bound);
}

std::optional<std::array<int32_t, 3>> snap_cell(
    std::array<int32_t, 2> cell,
    int32_t radius,
    double cursor_x,
    double cursor_z,
    const SnapScore& score
) {
    struct Candidate {
        double distance{};
        int32_t score{};
        int32_t dz{};
        int32_t dx{};
    };

    std::vector<Candidate> candidates;
    const double cell_x = cursor_x * cursor_to_cells;
    const double cell_z = cursor_z * cursor_to_cells;
    for (int32_t dx = -radius; dx <= radius; ++dx)
        for (int32_t dz = -radius; dz <= radius; ++dz) {
            const auto x = cell[0] + dx;
            const auto z = cell[1] + dz;
            const auto value = score(x, z);
            // From the cell's middle to the cursor, in cells.
            const double across = static_cast<double>(x) - cell_x + 0.5;
            const double down = static_cast<double>(z) - cell_z + 0.5;
            const double distance = down * down + across * across;
            if (value > 0)
                candidates.push_back({distance, value, dz, dx});
        }
    if (candidates.empty())
        return std::nullopt;
    int32_t best_score = candidates.front().score;
    for (const auto& candidate : candidates)
        best_score = std::max(best_score, candidate.score);
    const Candidate* nearest = nullptr;
    for (const auto& candidate : candidates)
        if (candidate.score == best_score &&
            (nearest == nullptr || candidate.distance < nearest->distance))
            nearest = &candidate;
    return std::array<int32_t, 3>{cell[0] + nearest->dx, cell[1] + nearest->dz, best_score};
}

int32_t footprint_first_offset(int32_t size) noexcept {
    return -(size / 2);
}

int32_t footprint_last_offset(int32_t size) noexcept {
    return size % 2 == 0 ? size / 2 - 1 : size / 2;
}

bool yard_has_geothermal_cell(std::span<const uint8_t> yard) noexcept {
    const auto cells = std::min<std::size_t>(yard.size(), yard_text_cells);
    for (std::size_t i = 0; i < cells && yard[i] != 0; ++i)
        if (yard[i] == geothermal_yard_cell)
            return true;
    return false;
}

std::optional<BuildLine> line_build_slots(
    int32_t start_x,
    int32_t start_z,
    int32_t end_x,
    int32_t end_z,
    int32_t footprint_x,
    int32_t footprint_z,
    int32_t spacing
) {
    // Cells between the two, the pixel difference divided by 16 toward zero.
    const int32_t across = (end_x - start_x) / 16;
    const int32_t down = (end_z - start_z) / 16;
    const int32_t cell_x = across < 0 ? -16 : 16;
    const int32_t cell_z = down < 0 ? -16 : 16;
    const int32_t span_x = across < 0 ? -across : across;
    const int32_t span_z = down < 0 ? -down : down;
    const int32_t step_x = footprint_x + spacing;
    const int32_t step_z = footprint_z + spacing;
    BuildLine line{};
    int32_t x = start_x;
    int32_t z = start_z;
    const auto lay = [&](int32_t steps, int32_t minor_span, auto&& advance, auto&& shift) {
        if (steps > build_line_step_limit)
            return false;
        line.slots.reserve(static_cast<std::size_t>(steps) + 1);
        int32_t error = 2 * minor_span - steps;
        for (int32_t index = 0; index <= steps; ++index) {
            line.slots.push_back({static_cast<int16_t>(x), static_cast<int16_t>(z)});
            while (error >= 0 && steps != 0) {
                shift();
                error -= 2 * steps;
            }
            error += 2 * minor_span;
            advance();
        }
        return true;
    };
    bool laid = false;
    if (span_x > span_z) {
        line.axis = LineAxis::across;
        const int32_t steps = span_x / step_x;
        laid = lay(steps, span_z, [&] { x += step_x * cell_x; }, [&] { z += cell_z; });
    } else {
        line.axis = LineAxis::down;
        const int32_t steps = span_z / step_z;
        laid = lay(steps, span_x, [&] { z += step_z * cell_z; }, [&] { x += cell_x; });
    }
    if (!laid)
        return std::nullopt;
    return line;
}

void optimize_dt_rows(BuildLine& line) noexcept {
    auto& slots = line.slots;
    const auto last = static_cast<int32_t>(slots.size()) - 1;
    if (last <= 2)
        return;
    // Along a line across, the rows across it are its z; along a line down,
    // its x.
    const auto row = [&](int32_t index) {
        const auto& slot = slots[static_cast<std::size_t>(index)];
        return line.axis == LineAxis::across ? slot.z : slot.x;
    };
    const auto swap_places = [&](int32_t a, int32_t b) {
        auto& first = slots[static_cast<std::size_t>(a)];
        auto& second = slots[static_cast<std::size_t>(b)];
        if (line.axis == LineAxis::across)
            std::swap(first.x, second.x);
        else
            std::swap(first.z, second.z);
    };
    for (int32_t index = 1; index <= last - 2;) {
        if (row(index) == row(index + 2)) {
            swap_places(index, index + 2);
            index += 3;
        } else if (row(index) == row(index + 1)) {
            swap_places(index, index + 1);
            index += 2;
        } else {
            ++index;
        }
    }
}

std::vector<BuildSlot> ring_build_slots(
    int32_t x,
    int32_t z,
    int32_t width,
    int32_t height,
    int32_t footprint_x,
    int32_t footprint_z,
    bool full_rings
) {
    const bool small = footprint_x < 3 && footprint_z < 3;
    const int32_t across =
        width / footprint_x + (width % footprint_x != 0 && full_rings && small ? 2 : 1);
    const int32_t down =
        height / footprint_z + (height % footprint_z != 0 && full_rings && small ? 2 : 1);
    std::vector<BuildSlot> slots;
    const auto add = [&](int32_t slot_x, int32_t slot_z) {
        slots.push_back(
            {static_cast<int16_t>(static_cast<uint16_t>(slot_x)),
             static_cast<int16_t>(static_cast<uint16_t>(slot_z))}
        );
    };
    const int32_t half_x = footprint_x * 8;
    const int32_t half_z = footprint_z * 8;
    for (int32_t i = 0; i < across; ++i)
        add(x + i * footprint_x * 16 + half_x, z - half_z);
    for (int32_t j = 0; j < down; ++j)
        add(x + width * 16 + half_x, z + half_z + j * footprint_z * 16);
    for (int32_t i = 0; i < across; ++i)
        add(x + (width - i * footprint_x) * 16 - half_x, z + height * 16 + half_z);
    for (int32_t j = 0; j < down; ++j)
        add(x - half_x, z + (height - j * footprint_z) * 16 - half_z);
    return slots;
}

bool facing_allowed(uint8_t facings, BuildFacing facing) noexcept {
    const auto index = static_cast<uint8_t>(facing) & 3U;
    return index == 0 || (facings & (1U << index)) != 0;
}

BuildFacing next_build_facing(uint8_t facings, BuildFacing current, int32_t direction) noexcept {
    int32_t allowed = 0;
    for (uint8_t index = 0; index < 4; ++index)
        if (facing_allowed(facings, static_cast<BuildFacing>(index)))
            ++allowed;
    if (allowed < 2)
        return current;
    const int32_t step = direction > 0 ? 1 : -1;
    for (int32_t turn = 1; turn <= 4; ++turn) {
        const auto next =
            static_cast<BuildFacing>((static_cast<int32_t>(current) + turn * step) & 3);
        if (facing_allowed(facings, next))
            return next;
    }
    return current;
}

BuildFacing facing_toward(int32_t dx, int32_t dz) noexcept {
    const auto across = dx < 0 ? -static_cast<int64_t>(dx) : static_cast<int64_t>(dx);
    const auto down = dz < 0 ? -static_cast<int64_t>(dz) : static_cast<int64_t>(dz);
    if (across > down)
        return dx > 0 ? BuildFacing::east : BuildFacing::west;
    return dz > 0 ? BuildFacing::south : BuildFacing::north;
}

std::optional<BuildFacing>
opponent_facing(const oa::World& world, int32_t site_x, int32_t site_z) noexcept {
    const auto local_index = static_cast<uint32_t>(world.game.local_player_index);
    if (local_index >= OA_PLAYER_COUNT)
        return std::nullopt;
    const auto& local = world.game.players[local_index];
    // A unit's whole x and z: the high halves of its 16.16 position.
    const auto whole = [](int32_t value) {
        return static_cast<int64_t>(static_cast<uint16_t>(static_cast<uint32_t>(value) >> 16));
    };
    const auto squared_distance = [&](const oa::Unit& unit) {
        const auto dx = whole(unit.position.x) - site_x;
        const auto dz = whole(unit.position.z) - site_z;
        return dx * dx + dz * dz;
    };
    const oa::Unit* first = oa::world_unit(&world, local.first_unit);
    const oa::Unit* last = oa::world_unit(&world, local.last_unit);
    bool in_reach = false;
    for (const oa::Unit* unit = first; unit != nullptr && last != nullptr && unit < last; ++unit) {
        if (unit->movement == 0 || static_cast<int16_t>(unit->type_index) <= 0 ||
            (unit->flags & OA_UNIT_FLAG_SELECTED) == 0 || unit->build_remaining != 0.0F)
            continue;
        const oa::UnitDef* def = oa::world_unit_def_of(&world, unit);
        if (def == nullptr || def->build_distance == 0)
            continue;
        const auto reach = static_cast<int64_t>(static_cast<uint16_t>(def->build_distance));
        if (squared_distance(*unit) <= reach * reach) {
            in_reach = true;
            break;
        }
    }
    if (!in_reach)
        return std::nullopt;
    const oa::Unit* nearest = nullptr;
    auto nearest_distance = std::numeric_limits<int64_t>::max();
    for (uint32_t index = 0; index < OA_PLAYER_COUNT; ++index) {
        const auto& player = world.game.players[index];
        if (index == local_index || player.in_use == 0)
            continue;
        if (const auto* info = oa::world_player_info(&world, &player);
            info != nullptr && (info->options & OA_SETUP_OPTION_WATCHER) != 0)
            continue;
        if (local.alliance[index] != 0)
            continue;
        const oa::Unit* unit = oa::world_unit(&world, player.first_unit);
        if (unit == nullptr || unit->movement == 0 || static_cast<int16_t>(unit->type_index) <= 0)
            continue;
        if (const auto distance = squared_distance(*unit); distance < nearest_distance) {
            nearest_distance = distance;
            nearest = unit;
        }
    }
    if (nearest == nullptr)
        return std::nullopt;
    return facing_toward(
        static_cast<int32_t>(whole(nearest->position.x) - site_x),
        static_cast<int32_t>(whole(nearest->position.z) - site_z)
    );
}

char facing_letter(BuildFacing facing) noexcept {
    constexpr std::string_view letters = "SENW";
    return letters[static_cast<std::size_t>(facing) & 3U];
}

std::string rotate_hint(std::string_view key, std::string_view modifier) {
    std::string hint = "Press ";
    hint += key;
    hint += ", or ";
    hint += modifier;
    hint += "+wheel, to rotate";
    return hint;
}

bool preview_lists_piece(std::string_view list, std::string_view piece) noexcept {
    const auto same = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    };
    while (!list.empty()) {
        const auto comma = list.find(',');
        auto name = list.substr(0, comma);
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
            name.remove_prefix(1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
            name.remove_suffix(1);
        if (!name.empty() && same(name, piece))
            return true;
        if (comma == std::string_view::npos)
            break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

bool preview_skips_piece(std::string_view piece) noexcept {
    constexpr std::array<std::string_view, 6> flashes{
        "flare", "flash", "muzzle", "fire", "flame", "wake"
    };
    return std::any_of(flashes.begin(), flashes.end(), [&](std::string_view name) {
        return name.size() == piece.size() &&
               std::equal(name.begin(), name.end(), piece.begin(), [](char a, char b) {
                   return a == std::tolower(static_cast<unsigned char>(b));
               });
    });
}

BuildPreviewLook build_preview_look(uint32_t pulse_tick, bool fill, int32_t height) noexcept {
    // The walk takes 32 steps over the pulse; at the pulse's start the
    // outline is 23 ticks of it on, 24 steps into it.
    constexpr uint32_t walk_steps = 32;
    constexpr uint32_t walk_start = 23;
    constexpr uint32_t half_walk = walk_steps / 2;
    const auto shade = [](uint32_t step) {
        constexpr uint8_t bright = 0xa0;
        constexpr uint8_t dark = 0xaf;
        constexpr uint32_t steps_one_way = 0xf;
        return static_cast<uint8_t>(
            (step & half_walk) != 0 ? dark - (step & steps_one_way)
                                    : bright + (step & steps_one_way)
        );
    };
    const uint32_t at = pulse_tick % build_preview_pulse_ticks;
    const uint32_t step =
        (at + walk_start) % build_preview_pulse_ticks * walk_steps / build_preview_pulse_ticks;
    BuildPreviewLook look;
    look.outline = shade(step);
    if (fill)
        look.fill = shade(step + half_walk);
    if (at >= build_preview_scan_ticks)
        return look;
    // Places up the model run from 1 at its lowest point to 255 at its
    // highest; the scanline is at 1 on the climb's first tick and at 255 on
    // its last.
    constexpr int32_t places = 254;
    constexpr int32_t reach = 2;
    const auto climb = static_cast<int32_t>(build_preview_scan_ticks - 1);
    const int32_t scan = 1 + static_cast<int32_t>(at) * places / climb;
    const int32_t span = std::max(height, 1);
    for (int32_t up = 0; up <= std::max(height, 0); ++up) {
        const int32_t place = (up * places + span / 2) / span + 1;
        if (std::abs(place - scan) > reach)
            continue;
        if (!look.scanning) {
            look.scanning = true;
            look.scan_low = up;
        }
        look.scan_high = up;
    }
    return look;
}

SourceBox
chat_backdrop_rect(bool has_logo, int32_t y, int32_t text_width, int32_t line_height) noexcept {
    // The log's text starts at column 0x8a, 4 right of the backdrop's edge;
    // the backdrop reaches 4 past the text.
    constexpr int32_t log_left = 0x8a;
    constexpr int32_t margin = 4;
    const int32_t left = log_left - margin;
    const int32_t right = log_left + margin + (has_logo ? line_height : 0) + text_width;
    return {left, y - 1, right - left, line_height + 2};
}

std::vector<std::string> chat_macro_lines(std::string_view macro) {
    std::vector<std::string> lines;
    while (!macro.empty()) {
        const auto end = macro.find_first_of("\r\n");
        const auto line = macro.substr(0, end);
        if (!line.empty())
            lines.emplace_back(line);
        if (end == std::string_view::npos)
            break;
        macro.remove_prefix(end + 1);
    }
    return lines;
}

uint8_t lobby_button_bits(const data::mod_profile::UiRules& ui) noexcept {
    using data::mod_profile::UiShareDialogAndLobbyButtonsLobbyButtons;
    namespace button = oa::ui::frontend_multiplayer::lobby_button;
    const auto& rules = ui.share_dialog_and_lobby_buttons;
    if (!rules.enabled)
        return 0;
    uint8_t bits = 0;
    const auto add = [&](UiShareDialogAndLobbyButtonsLobbyButtons value, uint8_t bit) {
        if (rules.lobby_buttons.contains(value))
            bits = static_cast<uint8_t>(bits | bit);
    };
    add(UiShareDialogAndLobbyButtonsLobbyButtons::autoteam, button::autoteam);
    add(UiShareDialogAndLobbyButtonsLobbyButtons::autopause, button::autopause);
    add(UiShareDialogAndLobbyButtonsLobbyButtons::randomteam, button::randomteam);
    add(UiShareDialogAndLobbyButtonsLobbyButtons::crcreport, button::crcreport);
    return bits;
}

namespace {

/// Truncates a single toward zero; out of range gives the lowest int.
int32_t truncate_single(float value) noexcept {
    if (!(value > -2147483904.0F && value < 2147483648.0F))
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(value);
}

} // namespace

int16_t share_threshold_knob(float threshold, float storage, int16_t range) noexcept {
    const int32_t whole = truncate_single(storage);
    if (whole == 0)
        return 0;
    const float share = threshold / static_cast<float>(whole);
    return static_cast<int16_t>(truncate_single(share * static_cast<float>(range)));
}

int32_t share_threshold_value(int16_t knob, int16_t range, float storage) noexcept {
    float share = static_cast<float>(knob) / static_cast<float>(int32_t{range} - 1);
    // Held to 0 to 1; a share that is no number stays one.
    share = 1.0F < share ? 1.0F : share;
    share = 0.0F > share ? 0.0F : share;
    return truncate_single(share * static_cast<float>(truncate_single(storage)));
}

namespace {

/// The names the settings are stored under.
namespace setting_name {
constexpr std::string_view snap_override_key = "ClickSnapOverrideKey";
constexpr std::string_view autoclick_key = "KeyCode";
[[maybe_unused]] constexpr std::string_view whiteboard_key = "WhiteboardKey";
[[maybe_unused]] constexpr std::string_view megamap_key = "MegamapKey";
constexpr std::string_view rotate_build_key = "RotateBuildKey";
constexpr std::string_view rotate_key_discovered = "RotateBuildKeyDiscovered";
constexpr std::string_view build_menu_rotation = "BuildMenuRotationOverlay";
constexpr std::array<std::string_view, 3> patrol{
    "ConUnitsPatrolHoldPosOption", "ConUnitsPatrolManeuverOption", "ConUnitsPatrolRoamOption"
};
constexpr std::array<std::string_view, 3> guard{
    "ConUnitsGuardHoldPosOption", "ConUnitsGuardManeuverOption", "ConUnitsGuardRoamOption"
};
constexpr std::string_view mex_snap_radius = "MexSnapRadius";
constexpr std::string_view wreck_snap_radius = "WreckSnapRadius";
constexpr std::string_view chat_macro = "ShareText";
constexpr std::string_view panel_background = "BackGround";
constexpr std::string_view optimize_dt_rows = "OptimizeDT";
constexpr std::string_view full_rings = "FullRings";
constexpr std::string_view chat_backdrop = "ChatBackdrop";
constexpr std::string_view vsync = "VSync";
} // namespace setting_name

/// The position name of an option key a place follows, or empty for a modifier.
///
/// @param code the slider's key code
/// @return the UI Events code, such as KeyX
std::string_view place_name(uint32_t code) noexcept {
    struct Place {
        uint32_t code;
        std::string_view name;
    };

    static constexpr Place places[] = {
        {'\t', "Tab"},
        {'b', "KeyB"},
        {'c', "KeyC"},
        {'e', "KeyE"},
        {'f', "KeyF"},
        {'g', "KeyG"},
        {'q', "KeyQ"},
        {'r', "KeyR"},
        {'v', "KeyV"},
        {'x', "KeyX"},
        {'z', "KeyZ"},
        {'/', "Slash"},
        {'\\', "Backslash"},
    };
    for (const Place& place : places) {
        if (place.code == code)
            return place.name;
    }
    return {};
}

/// The position of an option key, or 0 when the key is a modifier or unknown.
///
/// @param code the slider's key code
/// @return the position number
uint16_t place_scancode(uint32_t code) noexcept {
    const std::string_view name = place_name(code);
    if (name.empty())
        return 0;
    const std::optional<uint16_t> scan = oa::ui::key_bindings::scancode_of(name);
    return scan ? *scan : uint16_t{0};
}

/// The key code a slider shows when the chord is not one of the option keys.
///
/// @return Alt, the first option key
uint32_t alt_code() noexcept {
    return oa::ui::engine_settings::option_keys[0].code;
}

/// The most characters the chat macro keeps.
constexpr std::size_t chat_macro_limit = 512;

} // namespace

uint32_t slider_value(std::string_view action_id, std::span<const std::string_view> chords) {
    namespace keys = oa::ui::key_bindings;
    namespace actions = oa::data::input_actions;
    const uint32_t alt = alt_code();
    const actions::Action* action = actions::find(action_id);
    if (action == nullptr || chords.empty())
        return alt;
    keys::ChordError error;
    const std::optional<keys::Chord> first = keys::parse_chord(chords.front(), error);
    if (!first)
        return alt;
    for (const oa::ui::engine_settings::OptionKey& key : oa::ui::engine_settings::option_keys) {
        const std::optional<keys::Chord> offered =
            keys::legacy_key_chord(key.code, place_scancode(key.code), action->by);
        if (offered && *offered == *first)
            return key.code;
    }
    return alt;
}

std::vector<std::string> slider_chords(std::string_view action_id, uint32_t code) {
    namespace keys = oa::ui::key_bindings;
    namespace actions = oa::data::input_actions;
    const actions::Action* action = actions::find(action_id);
    if (action == nullptr)
        return {};
    const std::optional<keys::Chord> chord =
        keys::legacy_key_chord(code, place_scancode(code), action->by);
    if (!chord)
        return {};
    if (chord->kind == keys::TriggerKind::modifier_alone && action->kind != actions::Kind::modifier)
        return {};
    const std::vector<keys::Chord> list = keys::imported_list(*action, *chord);
    std::vector<std::string> texts;
    texts.reserve(list.size());
    for (const keys::Chord& item : list)
        texts.push_back(keys::chord_text(item));
    return texts;
}

std::optional<ImportedKey> imported_key(
    std::string_view action_id,
    const std::function<std::optional<uint32_t>(std::string_view)>& number
) {
    struct Row {
        std::string_view action;
        std::string_view setting;
        uint32_t fallback;
    };

    static constexpr Row rows[] = {
        {"build.snap-override", setting_name::snap_override_key, 0x400000e2U},
        {"build.line", setting_name::autoclick_key, static_cast<uint32_t>('x')},
        {"build.rotate", setting_name::rotate_build_key, static_cast<uint32_t>('/')},
    };
    const Row* matched = nullptr;
    for (const Row& one : rows) {
        if (one.action == action_id) {
            matched = &one;
            break;
        }
    }
    if (matched == nullptr)
        return std::nullopt;
    uint32_t stored_code = matched->fallback;
    if (number) {
        if (const std::optional<uint32_t> stored = number(matched->setting))
            stored_code = *stored;
    }
    return ImportedKey{matched->setting, stored_code};
}

ViewSettings read_view_settings(
    const data::mod_profile::UiRules& ui,
    const data::match_rules::OrdersConPatrolGuardOptions& builders,
    const ViewSettingsStore& store
) {
    const auto number = [&](std::string_view name) -> std::optional<uint32_t> {
        return store.number ? store.number(name) : std::nullopt;
    };
    const auto flag = [&](std::string_view name, bool fallback) {
        const auto value = number(name);
        return value ? *value != 0 : fallback;
    };
    ViewSettings settings{};
    settings.rotate_key_discovered = flag(setting_name::rotate_key_discovered, false);
    settings.build_menu_rotation = flag(setting_name::build_menu_rotation, false);
    const std::array<PatrolOption, 3> patrol_defaults{
        static_cast<PatrolOption>(builders.patrol_hold_position),
        static_cast<PatrolOption>(builders.patrol_maneuver),
        static_cast<PatrolOption>(builders.patrol_roam)
    };
    const std::array<GuardOption, 3> guard_defaults{
        static_cast<GuardOption>(builders.guard_hold_position),
        static_cast<GuardOption>(builders.guard_maneuver),
        static_cast<GuardOption>(builders.guard_roam)
    };
    for (std::size_t order = 0; order < settings.patrol.size(); ++order) {
        const auto patrol = number(setting_name::patrol[order]);
        settings.patrol[order] =
            patrol && *patrol <= static_cast<uint32_t>(PatrolOption::assist_only)
                ? static_cast<PatrolOption>(*patrol)
                : patrol_defaults[order];
        const auto guard = number(setting_name::guard[order]);
        settings.guard[order] = guard && *guard <= static_cast<uint32_t>(GuardOption::scatter)
                                    ? static_cast<GuardOption>(*guard)
                                    : guard_defaults[order];
    }
    const auto& snap = ui.click_snap;
    const auto radius = [&](std::string_view name, int32_t fallback, int32_t maximum) {
        const auto value = number(name);
        const auto chosen = value && *value <= static_cast<uint32_t>(click_snap_radius_limit)
                                ? static_cast<int32_t>(*value)
                                : fallback;
        return click_snap_radius(chosen, maximum);
    };
    settings.mex_snap_radius =
        radius(setting_name::mex_snap_radius, snap.mex_default, snap.mex_max);
    settings.wreck_snap_radius =
        radius(setting_name::wreck_snap_radius, snap.wreck_default, snap.wreck_max);
    auto macro = store.text ? store.text(setting_name::chat_macro) : std::nullopt;
    settings.chat_macro =
        macro ? macro->substr(0, chat_macro_limit) : std::string(default_chat_macro);
    const auto background = number(setting_name::panel_background);
    settings.panel_background =
        background && *background <= static_cast<uint32_t>(PanelBackground::solid)
            ? static_cast<PanelBackground>(*background)
            : PanelBackground::text;
    settings.optimize_dt_rows = flag(setting_name::optimize_dt_rows, true);
    settings.full_rings = flag(setting_name::full_rings, true);
    settings.chat_backdrop = flag(setting_name::chat_backdrop, ui.text_rendering.chat_backdrop);
    settings.vsync = flag(setting_name::vsync, false);
    return settings;
}

void write_view_settings(
    const ViewSettings& settings,
    const data::mod_profile::UiRules& ui,
    const std::function<void(std::string_view name, uint32_t value)>& number,
    const std::function<void(std::string_view name, std::string_view value)>& text
) {
    number(setting_name::rotate_key_discovered, settings.rotate_key_discovered ? 1U : 0U);
    number(setting_name::build_menu_rotation, settings.build_menu_rotation ? 1U : 0U);
    for (std::size_t order = 0; order < settings.patrol.size(); ++order) {
        number(setting_name::patrol[order], static_cast<uint32_t>(settings.patrol[order]));
        number(setting_name::guard[order], static_cast<uint32_t>(settings.guard[order]));
    }
    // A radius the profile holds at 0 is not the player's choice.
    const auto locks = dialog_option_locks(ui);
    if (locks.mex_snap == oa::ui::engine_settings::Lock::none)
        number(setting_name::mex_snap_radius, static_cast<uint32_t>(settings.mex_snap_radius));
    if (locks.wreck_snap == oa::ui::engine_settings::Lock::none)
        number(setting_name::wreck_snap_radius, static_cast<uint32_t>(settings.wreck_snap_radius));
    text(setting_name::chat_macro, settings.chat_macro);
    number(setting_name::panel_background, static_cast<uint32_t>(settings.panel_background));
    number(setting_name::optimize_dt_rows, settings.optimize_dt_rows ? 1U : 0U);
    number(setting_name::full_rings, settings.full_rings ? 1U : 0U);
    number(setting_name::chat_backdrop, settings.chat_backdrop ? 1U : 0U);
    number(setting_name::vsync, settings.vsync ? 1U : 0U);
}

void apply_builder_options(
    const ViewSettings& settings, data::match_rules::OrdersConPatrolGuardOptions& builders
) noexcept {
    using data::match_rules::OrdersConPatrolGuardOptionsGuardHoldPosition;
    using data::match_rules::OrdersConPatrolGuardOptionsGuardManeuver;
    using data::match_rules::OrdersConPatrolGuardOptionsGuardRoam;
    using data::match_rules::OrdersConPatrolGuardOptionsPatrolHoldPosition;
    using data::match_rules::OrdersConPatrolGuardOptionsPatrolManeuver;
    using data::match_rules::OrdersConPatrolGuardOptionsPatrolRoam;
    if (!builders.enabled)
        return;
    builders.patrol_hold_position =
        static_cast<OrdersConPatrolGuardOptionsPatrolHoldPosition>(settings.patrol[0]);
    builders.patrol_maneuver =
        static_cast<OrdersConPatrolGuardOptionsPatrolManeuver>(settings.patrol[1]);
    builders.patrol_roam = static_cast<OrdersConPatrolGuardOptionsPatrolRoam>(settings.patrol[2]);
    builders.guard_hold_position =
        static_cast<OrdersConPatrolGuardOptionsGuardHoldPosition>(settings.guard[0]);
    builders.guard_maneuver =
        static_cast<OrdersConPatrolGuardOptionsGuardManeuver>(settings.guard[1]);
    builders.guard_roam = static_cast<OrdersConPatrolGuardOptionsGuardRoam>(settings.guard[2]);
}

oa::ui::engine_settings::ModOptions
dialog_options(const ViewSettings& settings, const data::mod_profile::UiRules& ui) noexcept {
    oa::ui::engine_settings::ModOptions options{};
    for (std::size_t order = 0; order < settings.patrol.size(); ++order) {
        options.patrol[order] = static_cast<uint8_t>(settings.patrol[order]);
        options.guard[order] = static_cast<uint8_t>(settings.guard[order]);
    }
    const auto& snap = ui.click_snap;
    options.mex_snap_most =
        snap.enabled ? std::clamp(snap.mex_max, 0, oa::ui::engine_settings::most_snap_radius) : 0;
    options.wreck_snap_most =
        snap.enabled ? std::clamp(snap.wreck_max, 0, oa::ui::engine_settings::most_snap_radius) : 0;
    options.mex_snap_radius = std::clamp(settings.mex_snap_radius, 0, options.mex_snap_most);
    options.wreck_snap_radius = std::clamp(settings.wreck_snap_radius, 0, options.wreck_snap_most);
    options.optimize_dt_rows = settings.optimize_dt_rows;
    options.full_rings = settings.full_rings;
    options.chat_backdrop = settings.chat_backdrop;
    options.panel_background = static_cast<uint8_t>(settings.panel_background);
    return options;
}

void apply_dialog_options(
    const oa::ui::engine_settings::ModOptions& options,
    const data::mod_profile::UiRules& ui,
    ViewSettings& settings
) noexcept {
    for (std::size_t order = 0; order < settings.patrol.size(); ++order) {
        settings.patrol[order] = static_cast<PatrolOption>(std::min<uint8_t>(
            options.patrol[order], static_cast<uint8_t>(PatrolOption::assist_only)
        ));
        settings.guard[order] = static_cast<GuardOption>(
            std::min<uint8_t>(options.guard[order], static_cast<uint8_t>(GuardOption::scatter))
        );
    }
    settings.mex_snap_radius = click_snap_radius(options.mex_snap_radius, ui.click_snap.mex_max);
    settings.wreck_snap_radius =
        click_snap_radius(options.wreck_snap_radius, ui.click_snap.wreck_max);
    settings.optimize_dt_rows = options.optimize_dt_rows;
    settings.full_rings = options.full_rings;
    settings.chat_backdrop = options.chat_backdrop;
    settings.panel_background = static_cast<PanelBackground>(
        std::min<uint8_t>(options.panel_background, static_cast<uint8_t>(PanelBackground::solid))
    );
}

oa::ui::engine_settings::Locks dialog_option_locks(const data::mod_profile::UiRules& ui) noexcept {
    oa::ui::engine_settings::Locks locks{};
    const auto& snap = ui.click_snap;
    if (!snap.enabled || snap.mex_max <= 0)
        locks.mex_snap = oa::ui::engine_settings::Lock::set_by_mod;
    if (!snap.enabled || snap.wreck_max <= 0)
        locks.wreck_snap = oa::ui::engine_settings::Lock::set_by_mod;
    return locks;
}

ProfileTexts profile_texts(const data::mod_profile::ModProfile* profile) noexcept {
    ProfileTexts texts{};
    if (profile == nullptr)
        return texts;
    const auto changed = [](const std::string& value, const std::string& original) {
        return value != original ? value.c_str() : nullptr;
    };
    // 3.1c's texts, built once: the unit panel asks every frame.
    static const data::mod_profile::Strings baseline{};
    const auto& strings = profile->strings;
    texts.nanolathing_status = changed(strings.status.nanolathing, baseline.status.nanolathing);
    texts.paralyzed_status = changed(strings.status.paralyzed, baseline.status.paralyzed);
    texts.leave_question = changed(strings.message.exit_confirm, baseline.message.exit_confirm);
    texts.kill_lead = changed(strings.message.kill_lead, baseline.message.kill_lead);
    for (std::size_t index = 0; index < texts.elimination_endings.size(); ++index)
        texts.elimination_endings[index] =
            changed(strings.message.elimination[index], baseline.message.elimination[index]);
    return texts;
}

std::string_view credits_gadget(const data::mod_profile::ModProfile* profile) noexcept {
    namespace menu = oa::ui::frontend_state::main_menu;
    return profile != nullptr ? std::string_view(profile->strings.gadget.credits)
                              : menu::resource_name(menu::Button::credits);
}

} // namespace oa::app::view_rules
