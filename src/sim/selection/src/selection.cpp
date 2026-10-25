// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/selection.hpp"

#include "oa/sim/simulation_state.hpp"

#include <cstring>

namespace oa::sim::selection {
namespace {

inline constexpr uint32_t occupancy_air = 1;

// High word of a 16.16 value, sign-extended.
int32_t whole(int32_t fixed) noexcept {
    return static_cast<int16_t>(static_cast<uint32_t>(fixed) >> 16);
}

// Inclusive point-in-rectangle test.
bool contains(const Rect32& rect, int32_t x, int32_t y) noexcept {
    return rect.x1 <= x && x <= rect.x2 && rect.y1 <= y && y <= rect.y2;
}

bool selectable(const World& world, const Unit& unit) {
    return oa::sim::simulation_state::unit_selectable(world, unit);
}

Player* local_player(World& world) noexcept {
    return world_player(&world, world.game.local_player_index);
}

// Inclusive unit range of a player as a [first, first + count) slice.
template <typename W, typename F>
void for_player_units(W& world, const Player& player, F&& visit) {
    uint32_t count = 0;
    Unit* first = world_player_units(const_cast<World*>(&world), &player, &count);
    for (uint32_t i = 0; i < count; ++i)
        if (!visit(first[i]))
            return;
}

void mark_selection_changed(World& world) noexcept {
    world.game.frame_flags =
        static_cast<uint16_t>(world.game.frame_flags | frame_flag_selection_changed);
}

void reset_command(const Hooks& hooks) {
    if (hooks.reset_command != nullptr)
        hooks.reset_command(hooks.context);
}

void speak(const Hooks& hooks, const Unit& unit) {
    if (hooks.speak != nullptr)
        hooks.speak(hooks.context, unit, speech_selected);
}

void center_camera(const Hooks& hooks, const FixedVec3& position) {
    if (hooks.center_camera != nullptr)
        hooks.center_camera(hooks.context, position, true);
}

bool is_unmarked_live(const Unit& unit) noexcept {
    return unit.type_index != 0 && (unit.flags & cycle_marks) == 0;
}

void set_squad(const Hooks& hooks, Unit& unit, int32_t squad) {
    if (hooks.set_squad != nullptr)
        hooks.set_squad(hooks.context, unit, squad);
}

// The unit an id names when it carries the selection bit. Id 0 names no unit.
Unit* selected_unit_at(World& world, uint16_t id) noexcept {
    Unit* unit = id != 0 ? world_unit_at(&world, id) : nullptr;
    return unit != nullptr && (unit->flags & OA_UNIT_FLAG_SELECTED) != 0 ? unit : nullptr;
}

} // namespace

Rect32 game_view_rect(const World& world) noexcept {
    return world.game.battlefield_rect;
}

void collect_visible_units(World& world, const VisibleLists& lists, const Hooks& hooks) {
    const Game& game = world.game;
    const Rect32 view = game_view_rect(world);
    const Player* viewer = world_player(&world, game.viewpoint_player);
    const auto camera_x = static_cast<int32_t>(game.camera_x);
    const auto camera_y = static_cast<int32_t>(game.camera_y);
    int32_t count = 0;
    for (uint32_t slot = 0; slot < world.unit_slot_count; ++slot) {
        const Unit& unit = world.units[slot];
        if (unit.type_index == 0)
            continue;
        const UnitDef* def = world_unit_def_of(&world, &unit);
        if (def == nullptr)
            continue;
        const int32_t screen_x = whole(unit.position.x) - camera_x;
        const int32_t screen_z = whole(unit.position.z) - camera_y;
        const int32_t unit_y = whole(unit.position.y);
        const int32_t left = whole(def->bounds_min_x) + screen_x;
        const int32_t right = whole(def->bounds_max_x) + screen_x;
        const int32_t top = whole(def->model_height) + unit_y;
        int32_t bottom = whole(def->bounds_min_y) + unit_y;
        const int32_t near_z = whole(def->bounds_min_z) + screen_z;
        const int32_t far_z = whole(def->bounds_max_z) + screen_z;
        if ((unit.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) != occupancy_air) {
            const MapPlot* plot = world_plot(&world, unit.position.x >> 20, unit.position.z >> 20);
            if (plot != nullptr && bottom > plot->height)
                bottom = plot->height;
        }
        if (left + view_origin_x > view.x2 || right + view_origin_x < view.x1)
            continue;
        if (near_z - (top >> 1) + view_origin_y > view.y2 ||
            far_z - (bottom >> 1) + view_origin_y < view.y1)
            continue;
        if (unit.owner_index != game.viewpoint_player &&
            (viewer == nullptr || hooks.player_sees_unit == nullptr ||
             !hooks.player_sees_unit(hooks.context, world, *viewer, unit)))
            continue;
        if (static_cast<uint32_t>(count) < lists.unit_capacity)
            lists.units[count] = unit.id;
        ++count;
    }
    world.game.hot_unit_count = count;
}

bool unit_listed(const World& world, const VisibleLists& lists, uint16_t unit) noexcept {
    if (lists.units == nullptr || world.game.hot_unit_count <= 0)
        return false;
    const auto count = static_cast<uint32_t>(world.game.hot_unit_count);
    for (uint32_t i = 0; i < count && i < lists.unit_capacity; ++i)
        if (lists.units[i] == unit)
            return true;
    return false;
}

void clear_selection(World& world, const Hooks& hooks) {
    for (uint32_t slot = 0; slot < world.unit_slot_count; ++slot)
        world.units[slot].flags &= selection_clear_keep;
    if (hooks.selection_cleared != nullptr)
        hooks.selection_cleared(hooks.context);
}

void select_all(World& world, const Hooks& hooks) {
    if (const Player* player = local_player(world)) {
        for_player_units(world, *player, [&](Unit& unit) {
            if (selectable(world, unit)) {
                unit.flags |= OA_UNIT_FLAG_SELECTED;
                world.game.panel_unit_id = 0;
            }
            return true;
        });
    }
    reset_command(hooks);
    mark_selection_changed(world);
}

void select_matching_types(World& world, const Hooks& hooks, uint32_t type_bits) {
    data::defs::CategoryMaskStorage storage;
    TypeMask mask = data::defs::category_mask_over(storage, type_bits);
    if (const Player* player = local_player(world)) {
        for_player_units(world, *player, [&](Unit& unit) {
            if ((unit.flags & OA_UNIT_FLAG_SELECTED) != 0)
                data::defs::category_mask_set(&mask, unit.type_index);
            return true;
        });
        for_player_units(world, *player, [&](Unit& unit) {
            if (selectable(world, unit) &&
                data::defs::category_mask_contains(&mask, unit.type_index))
                unit.flags |= OA_UNIT_FLAG_SELECTED;
            return true;
        });
    }
    world.game.panel_unit_id = 0;
    reset_command(hooks);
    mark_selection_changed(world);
}

void apply_type_mask_selection(
    World& world, const TypeMask& mask, bool additive, const Hooks& hooks
) {
    if (const Player* player = local_player(world)) {
        for_player_units(world, *player, [&](Unit& unit) {
            if (!selectable(world, unit))
                return true;
            if (data::defs::category_mask_contains(&mask, unit.type_index))
                unit.flags |= OA_UNIT_FLAG_SELECTED;
            else if (!additive)
                unit.flags &= ~OA_UNIT_FLAG_SELECTED;
            return true;
        });
    }
    world.game.panel_unit_id = 0;
    reset_command(hooks);
    mark_selection_changed(world);
}

void select_visible_units(World& world, const VisibleLists& lists, const Hooks& hooks) {
    bool any = false;
    clear_selection(world, hooks);
    const int32_t count = world.game.hot_unit_count;
    if (count <= 0)
        return;
    for (int32_t i = 0; i < count && static_cast<uint32_t>(i) < lists.unit_capacity; ++i) {
        Unit* unit = world_unit_at(&world, lists.units[i]);
        if (unit == nullptr || !selectable(world, *unit) ||
            unit->owner_index != world.game.local_player_index)
            continue;
        unit->flags |= OA_UNIT_FLAG_SELECTED;
        any = true;
    }
    if (any) {
        reset_command(hooks);
        world.game.panel_unit_id = 0;
        mark_selection_changed(world);
    }
}

void clear_cycle_marks(World& world) noexcept {
    for (uint32_t slot = 0; slot < world.unit_slot_count; ++slot)
        world.units[slot].flags &= ~cycle_marks;
}

Unit* next_selected_unit(World& world, const Unit* from, bool backward) noexcept {
    const Player* player = local_player(world);
    if (player == nullptr)
        return nullptr;
    const uint16_t first = player->base_unit_id;
    const uint16_t last = player->last_unit_id;
    uint16_t start = from != nullptr ? from->id : 0;
    if (start < first || start > last)
        start = first;
    if (!backward) {
        for (uint16_t id = start; id != last;)
            if (Unit* unit = selected_unit_at(world, ++id))
                return unit;
        for (uint16_t id = first;; ++id) {
            if (Unit* unit = selected_unit_at(world, id))
                return unit;
            if (id == start)
                break;
        }
    } else {
        for (uint16_t id = start; id != first;)
            if (Unit* unit = selected_unit_at(world, --id))
                return unit;
        for (uint16_t id = last;; --id) {
            if (Unit* unit = selected_unit_at(world, id))
                return unit;
            if (id == start)
                break;
        }
    }
    return nullptr;
}

void mark_visible_local(World& world, const VisibleLists& lists) noexcept {
    const auto count = static_cast<uint32_t>(world.game.hot_unit_count);
    for (uint32_t i = 0; i < count && i < lists.unit_capacity; ++i) {
        Unit* unit = world_unit_at(&world, lists.units[i]);
        if (unit != nullptr && unit->owner_index == world.game.local_player_index)
            unit->flags = (unit->flags & ~OA_UNIT_FLAG_CYCLE_SKIP) | OA_UNIT_FLAG_CYCLE_VISITED;
    }
}

namespace {

/// Selects the local player's selectable units a test takes
/// (select_units_where).
template <typename InBox>
bool select_units_taken(
    World& world, const VisibleLists& lists, bool toggle, const Hooks& hooks, InBox&& in_box
) {
    if (!toggle)
        clear_selection(world, hooks);
    bool changed = false;
    int32_t selected = 0;
    const Unit* last_selected = nullptr;
    if (const Player* player = local_player(world)) {
        for_player_units(world, *player, [&](Unit& unit) {
            if (!selectable(world, unit))
                return true;
            if (in_box(unit)) {
                if (toggle)
                    unit.flags ^= OA_UNIT_FLAG_SELECTED;
                else
                    unit.flags |= OA_UNIT_FLAG_SELECTED;
                changed = true;
            }
            if ((unit.flags & OA_UNIT_FLAG_SELECTED) != 0) {
                ++selected;
                last_selected = &unit;
            }
            return true;
        });
    }
    world.game.panel_unit_id = 0;
    clear_cycle_marks(world);
    mark_visible_local(world, lists);
    if (changed)
        mark_selection_changed(world);
    if (selected == 0)
        return false;
    if (selected == 1)
        speak(hooks, *last_selected);
    else if (hooks.play_sound != nullptr)
        hooks.play_sound(hooks.context, sound_select_multiple);
    return true;
}

} // namespace

bool select_units_where(
    World& world,
    const VisibleLists& lists,
    bool toggle,
    const Hooks& hooks,
    bool (*in_box)(void* context, const Unit& unit),
    void* context
) {
    return select_units_taken(world, lists, toggle, hooks, [&](const Unit& unit) {
        return in_box != nullptr && in_box(context, unit);
    });
}

bool select_units_in_box(World& world, const VisibleLists& lists, bool toggle, const Hooks& hooks) {
    const Game& game = world.game;
    const auto camera_x = static_cast<int32_t>(game.camera_x);
    const auto camera_y = static_cast<int32_t>(game.camera_y);
    const int32_t start_x = game.drag_start[0];
    const int32_t start_y = game.drag_start[1];
    const int32_t start_z = game.drag_start[2];
    const int32_t end_x = game.drag_end[0];
    const int32_t end_y = game.drag_end[1];
    const int32_t end_z = game.drag_end[2];
    int32_t min_x = start_x - camera_x + view_origin_x;
    int32_t max_x = end_x - camera_x + view_origin_x;
    int32_t min_y = start_z - (start_y >> 1) - camera_y + view_origin_y;
    int32_t max_y = end_z - (end_y >> 1) - camera_y + view_origin_y;
    if (max_x < min_x) {
        const int32_t swap = min_x;
        min_x = max_x;
        max_x = swap;
    }
    if (max_y < min_y) {
        const int32_t swap = min_y;
        min_y = max_y;
        max_y = swap;
    }
    return select_units_taken(world, lists, toggle, hooks, [&](const Unit& unit) {
        const int32_t x = whole(unit.position.x) - camera_x + view_origin_x;
        const int32_t y =
            whole(unit.position.z) - (whole(unit.position.y) >> 1) - camera_y + view_origin_y;
        return min_x <= x && x <= max_x && min_y <= y && y <= max_y;
    });
}

void select_cursor_unit(World& world, const VisibleLists& lists, bool toggle, const Hooks& hooks) {
    const uint16_t id = world.game.cursor_unit_id;
    if (id == 0)
        return;
    Unit* unit = world_unit_at(&world, id);
    if (unit == nullptr || unit->owner_index != world.game.local_player_index ||
        !selectable(world, *unit))
        return;
    if (!toggle) {
        clear_selection(world, hooks);
        mark_visible_local(world, lists);
        unit->flags |= OA_UNIT_FLAG_SELECTED;
        speak(hooks, *unit);
    } else {
        unit->flags ^= OA_UNIT_FLAG_SELECTED;
        if ((unit->flags & OA_UNIT_FLAG_SELECTED) != 0)
            speak(hooks, *unit);
        world.game.panel_unit_id = 0;
    }
    mark_selection_changed(world);
}

uint16_t unit_under_pointer(const World& world, const VisibleLists& lists, const Hooks& hooks) {
    const Game& game = world.game;
    const auto pointer_x = static_cast<int32_t>(game.pointer_state[0]);
    const auto pointer_y = static_cast<int32_t>(game.pointer_state[1]);
    const Rect32 view = game_view_rect(world);
    uint16_t picked = 0;
    if (contains(view, pointer_x, pointer_y)) {
        if (lists.units == nullptr)
            return 0;
        int32_t best = pick_no_score;
        for (int32_t i = 0;
             i < game.hot_unit_count && static_cast<uint32_t>(i) < lists.unit_capacity;
             ++i) {
            const Unit* unit = world_unit_at(&world, lists.units[i]);
            if (unit == nullptr || unit->type_index == 0 || hooks.pointer_hits_unit == nullptr ||
                !hooks.pointer_hits_unit(hooks.context, world, *unit, pointer_x, pointer_y))
                continue;
            const UnitDef* def = world_unit_def_of(&world, unit);
            if (def == nullptr)
                continue;
            const auto half_height =
                static_cast<int32_t>((static_cast<int64_t>(def->size_y) * 0x8000) >> 16);
            const auto score = static_cast<int32_t>(
                (static_cast<int64_t>(def->size_x) *
                 static_cast<int64_t>(def->size_z + half_height)) >>
                16
            );
            if (score < best) {
                best = score;
                picked = unit->id;
            }
        }
    } else if (contains(lists.radar_picture, pointer_x, pointer_y)) {
        int32_t best = radar_pick_no_score;
        if (lists.radar == nullptr)
            return 0;
        for (int32_t i = 0;
             i < lists.radar_count && static_cast<uint32_t>(i) < lists.radar_capacity;
             ++i) {
            const RadarHotUnit& blip = lists.radar[i];
            const int32_t dx = blip.x - pointer_x;
            const int32_t dy = blip.y - pointer_y;
            const int32_t distance = dx * dx + dy * dy;
            if (distance < radar_pick_radius_sq && distance < best) {
                best = distance;
                picked = blip.unit_id;
            }
        }
    }
    return picked;
}

Unit* next_unmarked_unit(World& world) {
    Player* player = local_player(world);
    if (player == nullptr)
        return nullptr;
    Unit* found = nullptr;
    const auto scan = [&](Unit& unit) {
        if (is_unmarked_live(unit)) {
            found = &unit;
            return false;
        }
        return true;
    };
    for_player_units(world, *player, scan);
    if (found != nullptr)
        return found;
    clear_cycle_marks(world);
    for_player_units(world, *player, scan);
    return found;
}

void cycle_next_unit(World& world, const VisibleLists& lists, const Hooks& hooks) {
    Unit* unit = next_unmarked_unit(world);
    if (unit == nullptr)
        return;
    world.game.cycle_unit_id = unit->id;
    center_camera(hooks, unit->position);
    collect_visible_units(world, lists, hooks);
    mark_visible_local(world, lists);
    unit->flags = (unit->flags & ~OA_UNIT_FLAG_CYCLE_SKIP) | OA_UNIT_FLAG_CYCLE_VISITED;
}

void find_commander(World& world, bool select, const Hooks& hooks) {
    Player* player = world_player(&world, world.game.viewpoint_player);
    if (player == nullptr)
        return;
    const PlayerSetupInfo* info = world_player_info(&world, player);
    const char* commander =
        info != nullptr && info->side < 5 ? world.game.sides[info->side].commander : "";
    Unit* found = nullptr;
    for_player_units(world, *player, [&](Unit& unit) {
        const UnitDef* def = world_unit_def_of(&world, &unit);
        if (selectable(world, unit) && def != nullptr &&
            std::strcmp(def->unit_name, commander) == 0) {
            found = &unit;
            return false;
        }
        return true;
    });
    if (found == nullptr)
        return;
    if (hooks.stop_follow != nullptr)
        hooks.stop_follow(hooks.context);
    center_camera(hooks, found->position);
    if (!select)
        return;
    reset_command(hooks);
    clear_selection(world, hooks);
    found->flags |= OA_UNIT_FLAG_SELECTED;
    mark_selection_changed(world);
}

void follow_commander(World& world, data::defs::CategoryRegistry& categories) {
    const TypeMask* commanders =
        data::defs::category_registry_find_or_add(&categories, commander_category);
    const Player* player = local_player(world);
    if (commanders == nullptr || player == nullptr)
        return;
    for_player_units(world, *player, [&](Unit& unit) {
        if (data::defs::category_mask_contains(commanders, unit.type_index))
            world.game.follow_unit = world_unit_ref(&world, &unit);
        return true;
    });
}

void follow_next_selected(World& world, bool backward) noexcept {
    const Unit* followed = world_unit(&world, world.game.follow_unit);
    world.game.follow_unit = world_unit_ref(&world, next_selected_unit(world, followed, backward));
}

void select_next_viewpoint_unit(World& world, const Hooks& hooks) {
    uint32_t count = 0;
    Player* player = world_player(&world, world.game.viewpoint_player);
    Unit* units = player != nullptr ? world_player_units(&world, player, &count) : nullptr;
    Unit* first = nullptr;
    uint32_t at = 0;
    for (; at < count; ++at) {
        if (!selectable(world, units[at]))
            continue;
        if (first == nullptr)
            first = &units[at];
        if ((units[at].flags & OA_UNIT_FLAG_SELECTED) != 0) {
            clear_selection(world, hooks);
            break;
        }
    }
    if (first != nullptr) {
        do
            ++at;
        while (at < count && !selectable(world, units[at]));
        if (at < count) {
            units[at].flags |= OA_UNIT_FLAG_SELECTED;
            world.game.panel_unit_id = 0;
        } else
            first->flags |= OA_UNIT_FLAG_SELECTED;
    }
    mark_selection_changed(world);
}

void assign_squad(World& world, int32_t squad, const Hooks& hooks) {
    const Player* player = local_player(world);
    if (player == nullptr)
        return;
    for_player_units(world, *player, [&](Unit& unit) {
        if (unit.type_index == 0)
            return true;
        if ((unit.flags & OA_UNIT_FLAG_SELECTED) != 0)
            set_squad(hooks, unit, squad);
        else if (unit.squad == squad)
            set_squad(hooks, unit, 0);
        return true;
    });
}

bool select_squad(World& world, int32_t squad, bool add, const TypeMask& skip, const Hooks& hooks) {
    const bool skip_types =
        squad_has_type(world, squad, skip) && squad_has_armed_unit(world, squad);
    // A recall of a squad that is already exactly the selection centres the
    // view on the average of the units' map positions (the signed high words,
    // divided as integers), however long after the first press, and stops
    // following a unit. 3.1c's recall leaves the camera alone; this is OA's.
    bool already = true;
    int32_t sum_x = 0;
    int32_t sum_z = 0;
    int32_t members = 0;
    if (const Player* player = local_player(world)) {
        for_player_units(world, *player, [&](const Unit& unit) {
            const bool live = selectable(world, unit);
            const bool in_squad = unit.squad == squad;
            const bool skipped = skip_types && in_squad &&
                                 data::defs::category_mask_contains(&skip, unit.type_index);
            const bool chosen = live && in_squad && !skipped;
            const bool selected_now = (unit.flags & OA_UNIT_FLAG_SELECTED) != 0;
            if (live && selected_now != chosen)
                already = false;
            if (chosen) {
                sum_x += whole(unit.position.x);
                sum_z += whole(unit.position.z);
                ++members;
            }
            return true;
        });
    } else
        already = false;
    int32_t selected = 0;
    if (const Player* player = local_player(world)) {
        for_player_units(world, *player, [&](Unit& unit) {
            if (!selectable(world, unit))
                return true;
            if (unit.squad == squad) {
                if (skip_types && data::defs::category_mask_contains(&skip, unit.type_index))
                    unit.flags &= ~OA_UNIT_FLAG_SELECTED;
                else {
                    unit.flags |= OA_UNIT_FLAG_SELECTED;
                    ++selected;
                }
            } else if (!add)
                unit.flags &= ~OA_UNIT_FLAG_SELECTED;
            return true;
        });
    }
    reset_command(hooks);
    world.game.panel_unit_id = 0;
    mark_selection_changed(world);
    if (already && members > 0) {
        if (hooks.stop_follow != nullptr)
            hooks.stop_follow(hooks.context);
        const auto pack = [](int32_t total, int32_t count) {
            return static_cast<int32_t>(static_cast<uint32_t>(total / count) << 16);
        };
        center_camera(hooks, FixedVec3{pack(sum_x, members), 0, pack(sum_z, members)});
    }
    return selected > 0;
}

bool squad_has_type(const World& world, int32_t squad, const TypeMask& mask) {
    const Player* player = world_player(&world, world.game.local_player_index);
    bool found = false;
    if (player != nullptr)
        for_player_units(world, *player, [&](const Unit& unit) {
            found = selectable(world, unit) && unit.squad == squad &&
                    data::defs::category_mask_contains(&mask, unit.type_index);
            return !found;
        });
    return found;
}

bool squad_has_armed_unit(const World& world, int32_t squad) {
    const Player* player = world_player(&world, world.game.local_player_index);
    bool found = false;
    if (player != nullptr)
        for_player_units(world, *player, [&](const Unit& unit) {
            found = selectable(world, unit) && unit.squad == squad &&
                    (unit.flags & OA_UNIT_FLAG_HAS_WEAPONS) != 0;
            return !found;
        });
    return found;
}

} // namespace oa::sim::selection
