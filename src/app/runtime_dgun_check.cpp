// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Headless check of the commander's D-gun order.
#include "oa/app/runtime.hpp"
#include "match_fault.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace oa::app {

void Runtime::check_dgun_order() {
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    if (screen_ != Screen::match || !match_ || !selected_tnt_)
        throw std::runtime_error("D-gun check: Start did not enter a match");
    auto& world = match_->state();
    auto& slots = match_->world().slots;
    uint16_t commander = 0;
    for (const auto& slot : slots) {
        if (slot.unit_index == 0 || slot.unit == nullptr || slot.record.type_index == 0 ||
            slot.record.owner_index != match_local_player_)
            continue;
        if (const auto* def = definition_for(slot.unit_index); def != nullptr && def->can_dgun) {
            commander = slot.unit_index;
            break;
        }
    }
    if (commander == 0)
        throw std::runtime_error("D-gun check found no local commander");
    const auto* commander_def = definition_for(commander);
    const auto laser_ref = slots[commander].record.weapons[0].def;
    const auto dgun_ref = slots[commander].record.weapons[2].def;
    const auto* dgun = oa::world_weapon_def(&world, dgun_ref);
    if (dgun == nullptr || (dgun->flags & OA_WEAPON_FLAG_COMMAND_FIRE) == 0)
        throw std::runtime_error("D-gun check: the commander's third weapon is not commandfire");

    // Two sturdy enemies toward the middle of the map: one in the laser's
    // reach, one beyond the D-gun's.
    const auto enemy_player = static_cast<uint8_t>(match_local_player_ == 0 ? 1 : 0);
    const auto enemy_type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "CORGOL");
    if (enemy_type == 0)
        throw std::runtime_error("D-gun check lacks CORGOL");
    const auto home_x = static_cast<int32_t>(slots[commander].unit->position[0] >> 16);
    const auto home_z = static_cast<int32_t>(slots[commander].unit->position[2] >> 16);
    const int32_t toward_x =
        home_x < static_cast<int32_t>(selected_tnt_->tile_width * 16U) ? 1 : -1;
    const int32_t toward_z =
        home_z < static_cast<int32_t>(selected_tnt_->tile_height * 16U) ? 1 : -1;
    const auto spawn_enemy = [&](int32_t dx, int32_t dz) {
        const auto x = home_x + toward_x * dx;
        const auto z = home_z + toward_z * dz;
        oa::sim::unit_spawn::Request request;
        request.player = enemy_player;
        request.type = enemy_type;
        request.finished = true;
        request.state = kGroundOccupancyState;
        request.position = {
            static_cast<uint32_t>(x) << 16,
            static_cast<uint32_t>(
                match_->map_height(static_cast<uint32_t>(x) << 16, static_cast<uint32_t>(z) << 16)
            ) << 16,
            static_cast<uint32_t>(z) << 16
        };
        auto* spawned = match_->create(request);
        if (spawned == nullptr || spawned->unit == nullptr)
            throw std::runtime_error("D-gun check could not spawn an enemy");
        return spawned->unit_index;
    };
    const auto near_enemy = spawn_enemy(150, 0);
    const auto far_enemy = spawn_enemy(200, 180);

    const auto tick = [&] {
        ++match_timing_.tick;
        match_->simulation().tick = match_timing_.tick;
        tick_or_raise(*match_);
    };
    const auto shots_of = [&](oa_ref32 weapon) {
        int32_t count = 0;
        for (int32_t i = 0; i < world.game.projectile_count; ++i)
            if (world.projectiles[i].def == weapon &&
                world.projectiles[i].source == oa::oa_unit_ref_from_slot(commander))
                ++count;
        return count;
    };
    // The frame clamps the camera to the map, as each presented frame does.
    const auto view_commander = [&] {
        center_camera_on_unit(commander);
        render_match_surface();
    };
    const auto point_at = [&](uint16_t id) {
        const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
        const auto screen = project_match_point(viewport, slots[id].unit->position);
        const auto x = static_cast<float>(screen.x);
        const auto y = static_cast<float>(screen.y);
        update_pointer(x, y);
        if (hovered_match_unit_ != id)
            throw std::runtime_error(
                "D-gun check: the pointer over unit " + std::to_string(id) + " found unit " +
                std::to_string(hovered_match_unit_)
            );
        return std::pair{x, y};
    };
    auto& owner = world.game.players[match_local_player_];
    constexpr auto attack_cursor =
        static_cast<uint8_t>(oa::sim::gameplay_input::OrderCursor::attack);
    const auto fail = [](const char* how, const std::string& what) {
        throw std::runtime_error(std::string("D-gun check: ") + how + ' ' + what);
    };
    const auto live_shot = [&]() -> const oa::Projectile* {
        for (int32_t i = 0; i < world.game.projectile_count; ++i) {
            const auto& shot = world.projectiles[i];
            if (shot.def == dgun_ref && shot.source == oa::oa_unit_ref_from_slot(commander) &&
                (shot.flags & OA_PROJECTILE_FLAG_RETIRED) == 0)
                return &shot;
        }
        return nullptr;
    };
    // The match plot under a 16.16 point, or null off the map.
    const auto plot_under = [&](int32_t x, int32_t z) -> const oa::sim::spatial_state::Plot* {
        const auto& spatial = match_->spatial();
        int32_t cell_x = 0;
        int32_t cell_z = 0;
        if (!oa::sim::spatial_state::position_to_cell(
                x,
                z,
                static_cast<int32_t>(spatial.terrain_width),
                static_cast<int32_t>(spatial.terrain_height),
                cell_x,
                cell_z
            ))
            return nullptr;
        const auto index = static_cast<std::size_t>(cell_z) * spatial.terrain_width +
                           static_cast<std::size_t>(cell_x);
        return index < spatial.plots.size() ? &spatial.plots[index] : nullptr;
    };
    // Whether a blast went off at exactly `at`, where the ball is this tick.
    const auto blast_at = [&](const oa::FixedVec3& at) {
        const auto& effects = match_->effects();
        for (int32_t i = 0; i < effects.explosion_count; ++i) {
            const auto& blast = effects.explosions[i].position;
            if (blast.x == at.x && blast.y == at.y && blast.z == at.z)
                return true;
        }
        return false;
    };
    // Follows the D-gun's ball from the tick it left until after its range
    // ran out, looking at the midpoint of the commander and `target` as it
    // left. The ball flies on through whatever it strikes, leaving the map
    // or reaching the end of its range, and every tick it spends below the
    // ground bursts where it is. With --snapshot, frames of it go beside the
    // snapshot as <stem>-dgun-<label>-<tick>.ppm. Returns the ticks it burst
    // below the ground.
    const auto follow_shot =
        [&](const std::array<uint32_t, 3>& target, const char* how, const char* label) {
            constexpr std::array<int32_t, 9> frame_ticks{2, 6, 10, 14, 18, 24, 30, 36, 44};
            const auto* shot = live_shot();
            if (shot == nullptr)
                fail(how, "left no ball in flight");
            const auto expiry = shot->lifetime_tick;
            auto last = *shot;
            const auto& from = slots[commander].unit->position;
            const auto middle = [](uint32_t a, uint32_t b) {
                return (static_cast<int32_t>(a >> 16) + static_cast<int32_t>(b >> 16)) / 2;
            };
            const auto look_x = middle(from[0], target[0]) - visible_map_width() / 2;
            const auto look_z = middle(from[2], target[2]) - visible_map_height() / 2;
            bool left_map = false;
            int32_t trail = 0;
            std::size_t next_frame = 0;
            for (int32_t step = 1; step <= frame_ticks.back(); ++step) {
                tick();
                shot = live_shot();
                if (world.game.tick >= expiry) {
                    if (shot != nullptr)
                        fail(how, "kept its ball past its range");
                } else if (shot == nullptr) {
                    left_map = left_map || plot_under(
                                               last.position.x + last.velocity.x,
                                               last.position.z + last.velocity.z
                                           ) == nullptr;
                    if (!left_map)
                        fail(
                            how,
                            "lost its ball " + std::to_string(step) +
                                " ticks after the shot, before its range ran out"
                        );
                } else {
                    last = *shot;
                    const auto* plot = plot_under(shot->position.x, shot->position.z);
                    if (plot != nullptr &&
                        static_cast<int16_t>(shot->position.y >> 16) < plot->low_height) {
                        if (!blast_at(shot->position))
                            fail(
                                how,
                                "had its ball below the ground without a blast " +
                                    std::to_string(step) + " ticks after the shot"
                            );
                        ++trail;
                    }
                }
                if (options_.snapshot.empty() || step != frame_ticks[next_frame])
                    continue;
                ++next_frame;
                set_camera_position(look_x, look_z, 0);
                render_match_surface();
                const auto& snapshot = options_.snapshot;
                fs::path file = snapshot.parent_path() / snapshot.stem();
                file += std::string("-dgun-") + label + '-' + std::to_string(step) + ".ppm";
                write_ppm(file, surface_);
            }
            return trail;
        };

    struct DgunShot {
        int32_t ticks{};
        int32_t trail{};
    };

    // Issues the armed BLAST on `target`, runs until the D-gun's shot and
    // follows its ball; returns the ticks to the shot and the ball's blasts
    // below the ground.
    const auto blast = [&](uint16_t target, const char* how, const char* label) {
        view_commander();
        const std::array<uint32_t, 3> aim = slots[target].unit->position;
        const auto [x, y] = point_at(target);
        if (const auto cursor = pick_match_cursor(); cursor != attack_cursor)
            fail(how, "shows cursor " + std::to_string(cursor) + " over the enemy");
        handle_match_left_click(x, y, 1);
        const auto* head = match_->orders(commander).primary;
        if (head == nullptr || head->kind != oa::sim::match_runtime::attack_special_kind)
            fail(how, "did not issue AttackSpecial");
        if (match_command_ != MatchCommand::none)
            fail(how, "stayed armed after the click");
        owner.energy = owner.energy_storage;
        const auto before = shots_of(dgun_ref);
        for (int32_t step = 1; step <= 600; ++step) {
            const auto energy = owner.energy;
            tick();
            if (shots_of(dgun_ref) == before)
                continue;
            // The shot's tick may also bring a second's income.
            const auto spent = energy - owner.energy;
            if (spent < dgun->energy_per_shot - commander_def->energy_make ||
                spent > dgun->energy_per_shot)
                fail(how, "shot spent " + std::to_string(spent) + " energy");
            return DgunShot{step, follow_shot(aim, how, label)};
        }
        fail(how, "never had the D-gun fire");
        return DgunShot{};
    };

    clear_local_selection();
    match_command_ = MatchCommand::none;
    match_->stop_orders(commander);
    view_commander();
    {
        const auto [x, y] = point_at(commander);
        handle_match_left_click(x, y, 1);
    }
    if (selected_match_unit_ != commander ||
        (slots[commander].unit->flags & OA_UNIT_FLAG_SELECTED) == 0)
        throw std::runtime_error("D-gun check: the click did not select the commander");
    int32_t laser_step = 0;
    for (int32_t step = 1; step <= 300 && laser_step == 0; ++step) {
        tick();
        if (shots_of(laser_ref) != 0)
            laser_step = step;
    }
    if (laser_step == 0)
        throw std::runtime_error("D-gun check: the commander's laser never fired");

    bool running = true;
    SDL_Event key{};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.key = SDLK_D;
    key.key.scancode = SDL_SCANCODE_D;
    handle_sdl_event(key, running);
    if (match_command_ != MatchCommand::dgun)
        throw std::runtime_error("D-gun check: 'd' did not arm BLAST");
    const auto keyed = blast(far_enemy, "the 'd' key", "key");

    for (int step = 0; step < 60; ++step)
        tick();
    view_commander();
    std::optional<std::size_t> button;
    for (std::size_t i = 0; match_hud_ && i < match_hud_->layout.gadgets.size(); ++i)
        if (match_hud_action(match_hud_->layout.gadgets[i].common.name) == "BLAST")
            button = i;
    if (!button)
        throw std::runtime_error("D-gun check: the commander's panel has no BLAST button");
    const auto& gadget = match_hud_->layout.gadgets[*button].common;
    const auto rect = oa::ui::display_layout::source_rect_to_canvas(
        match_layout_, gadget.x, gadget.y, gadget.width, gadget.height
    );
    update_pointer(
        static_cast<float>(rect.x) + static_cast<float>(rect.width) / 2.0F,
        static_cast<float>(rect.y) + static_cast<float>(rect.height) / 2.0F
    );
    if (hovered_ != button)
        throw std::runtime_error("D-gun check: the pointer missed the BLAST button");
    activate_match_hud(*hovered_);
    if (match_command_ != MatchCommand::dgun)
        throw std::runtime_error("D-gun check: the BLAST button did not arm the D-gun");
    const auto pressed = blast(near_enemy, "the BLAST button", "button");
    // Past the near enemy the ball runs into the ground well inside its range.
    if (pressed.trail == 0)
        fail("the BLAST button", "left no trail of blasts along the ground");

    std::cout << "D-gun check: with the laser firing, 'd' and a click fired the D-gun "
              << keyed.ticks << " ticks later, the BLAST button and a click " << pressed.ticks
              << " ticks later; the balls flew their whole range, bursting on " << keyed.trail
              << " and " << pressed.trail << " ticks below the ground\n";
    clear_local_selection();
    match_command_ = MatchCommand::none;
    return_to_skirmish_menu();
}

} // namespace oa::app
