// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/selection.hpp"
#include "oa/base/text.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>

using namespace oa;
using namespace oa::sim::selection;

namespace {
int failures = 0;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

struct Recorder {
    int speech = 0;
    int multiple = 0;
    int resets = 0;
    int cleared = 0;
    int32_t camera_x = 0;
    int32_t camera_y = 0;
    int centred = 0;
    bool sees = false;
    int stops = 0;
    int squad_calls = 0;
    uint16_t squad_unit[4]{};
    int32_t squad_value[4]{};
};

Hooks hooks_for(Recorder& r) {
    Hooks h{};
    h.context = &r;
    h.player_sees_unit = [](void* c, const World&, const Player&, const Unit&) {
        return static_cast<Recorder*>(c)->sees;
    };
    h.pointer_hits_unit = [](void*, const World&, const Unit&, int32_t, int32_t) { return true; };
    h.speak = [](void* c, const Unit&, uint32_t) { ++static_cast<Recorder*>(c)->speech; };
    h.play_sound = [](void* c, const char* name) {
        if (std::strcmp(name, "SelectMultipleUnits") == 0)
            ++static_cast<Recorder*>(c)->multiple;
    };
    h.reset_command = [](void* c) { ++static_cast<Recorder*>(c)->resets; };
    h.selection_cleared = [](void* c) { ++static_cast<Recorder*>(c)->cleared; };
    h.center_camera = [](void* c, const FixedVec3& p, bool) {
        auto& rec = *static_cast<Recorder*>(c);
        rec.camera_x = p.x >> 16;
        rec.camera_y = p.z >> 16;
        ++rec.centred;
    };
    h.stop_follow = [](void* c) { ++static_cast<Recorder*>(c)->stops; };
    h.set_squad = [](void* c, Unit& unit, int32_t squad) {
        auto& rec = *static_cast<Recorder*>(c);
        if (rec.squad_calls < 4) {
            rec.squad_unit[rec.squad_calls] = unit.id;
            rec.squad_value[rec.squad_calls] = squad;
        }
        ++rec.squad_calls;
        unit.squad = squad;
    };
    return h;
}

constexpr int32_t fx(int32_t whole) {
    return static_cast<int32_t>(static_cast<uint32_t>(whole) << 16);
}

// Units 1..3 local player 0, 4..5 player 1. Type 1 for odd slots, 2 even.
World* make_world() {
    World* w = world_create();
    WorldCapacity cap{8, 4, 0};
    if (w == nullptr || !world_alloc_tables(w, &cap))
        std::abort();
    for (uint32_t t = 1; t < 4; ++t) {
        UnitDef& d = w->unit_defs[t];
        d.bounds_min_x = fx(-8);
        d.bounds_max_x = fx(8);
        d.bounds_min_z = fx(-8);
        d.bounds_max_z = fx(8);
        d.bounds_min_y = 0;
        d.model_height = fx(10);
        d.size_x = fx(16);
        d.size_y = fx(10);
        d.size_z = fx(16);
    }
    oa::base::text::copy_terminated(w->unit_defs[3].unit_name, "ARMCOM");
    for (uint32_t slot = 1; slot <= 5; ++slot) {
        Unit& u = w->units[slot];
        u.id = static_cast<uint16_t>(slot);
        u.type_index = static_cast<uint16_t>(slot % 2 ? 1 : 2);
        u.def = oa_ref_from_index(u.type_index);
        u.owner_index = slot <= 3 ? 0 : 1;
        u.flags = OA_UNIT_FLAG_SELECTABLE | OA_UNIT_FLAG_LIVE;
        u.position = {fx(static_cast<int32_t>(100 * slot)), 0, fx(100)};
    }
    w->game.players[0].first_unit = oa_unit_ref_from_slot(1);
    w->game.players[0].last_unit = oa_unit_ref_from_slot(3);
    w->game.players[1].first_unit = oa_unit_ref_from_slot(4);
    w->game.players[1].last_unit = oa_unit_ref_from_slot(5);
    w->game.players[0].base_unit_id = 1;
    w->game.players[0].last_unit_id = 3;
    w->game.players[1].base_unit_id = 4;
    w->game.players[1].last_unit_id = 5;
    w->game.local_player_index = 0;
    w->game.viewpoint_player = 0;
    Rect32 view{128, 32, 128 + 640, 32 + 480};
    w->game.battlefield_rect = view;
    w->game.viewport_width = 640;
    w->game.viewport_height = 480;
    return w;
}

bool selected(const World& w, uint32_t slot) {
    return (w.units[slot].flags & OA_UNIT_FLAG_SELECTED) != 0;
}
} // namespace

int main() {
    Recorder r;
    const Hooks h = hooks_for(r);
    uint16_t ids[8]{};
    VisibleLists lists{ids, 8, nullptr, 0};

    {
        World* w = make_world();
        collect_visible_units(*w, lists, h);
        // units at x 100..500 are on screen; enemies need sight.
        CHECK(w->game.hot_unit_count == 3);
        CHECK(ids[0] == 1 && ids[2] == 3);
        r.sees = true;
        collect_visible_units(*w, lists, h);
        CHECK(w->game.hot_unit_count == 5);
        w->game.camera_x = 1000;
        collect_visible_units(*w, lists, h);
        CHECK(w->game.hot_unit_count == 0);
        world_destroy(w);
    }
    {
        // The on-screen test reads the ids the last collection listed, up to
        // the count and never past the buffer.
        World* w = make_world();
        uint16_t listed[2]{};
        const VisibleLists pair{listed, 2, nullptr, 0};
        CHECK(!unit_listed(*w, pair, 1));
        const Hooks blind = [&] {
            Hooks copy = h;
            copy.player_sees_unit = nullptr;
            return copy;
        }();
        collect_visible_units(*w, lists, blind);
        CHECK(w->game.hot_unit_count == 3);
        CHECK(unit_listed(*w, lists, 1) && unit_listed(*w, lists, 3));
        CHECK(!unit_listed(*w, lists, 4) && !unit_listed(*w, lists, 0));
        listed[0] = 2;
        listed[1] = 5;
        CHECK(unit_listed(*w, pair, 5));
        CHECK(!unit_listed(*w, pair, 3));
        w->game.hot_unit_count = 1;
        CHECK(unit_listed(*w, pair, 2) && !unit_listed(*w, pair, 5));
        w->game.hot_unit_count = 0;
        CHECK(!unit_listed(*w, pair, 2));
        CHECK(!unit_listed(*w, VisibleLists{}, 2));
        world_destroy(w);
    }
    {
        World* w = make_world();
        w->units[2].capture_cooldown = 5;
        select_all(*w, h);
        CHECK(selected(*w, 1) && !selected(*w, 2) && selected(*w, 3) && !selected(*w, 4));
        CHECK((w->game.frame_flags & frame_flag_selection_changed) != 0);
        world_destroy(w);
    }
    {
        World* w = make_world();
        w->units[1].flags |= OA_UNIT_FLAG_SELECTED;
        select_matching_types(*w, h);
        CHECK(selected(*w, 1) && !selected(*w, 2) && selected(*w, 3));
        // A type id past the bitset's 512 is left out; the widest bitset holds it.
        for (const uint16_t type : {uint16_t{600}, uint16_t{65535}}) {
            w->units[1].type_index = type;
            w->units[3].type_index = type;
            w->units[3].flags &= ~OA_UNIT_FLAG_SELECTED;
            select_matching_types(*w, h);
            CHECK(!selected(*w, 3));
            select_matching_types(*w, h, data::limits::highest_type_bits);
            CHECK(selected(*w, 3));
            w->units[3].flags &= ~OA_UNIT_FLAG_SELECTED;
        }
        w->units[1].type_index = 1;
        w->units[3].type_index = 1;
        w->units[3].flags |= OA_UNIT_FLAG_SELECTED;
        data::defs::CategoryMaskStorage storage;
        TypeMask mask = data::defs::category_mask_over(storage, data::limits::base_type_bits);
        data::defs::category_mask_set(&mask, 2);
        apply_type_mask_selection(*w, mask, true, h);
        CHECK(selected(*w, 1) && selected(*w, 2));
        apply_type_mask_selection(*w, mask, false, h);
        CHECK(!selected(*w, 1) && selected(*w, 2) && !selected(*w, 3));
        CHECK(squad_has_type(*w, 0, mask));
        CHECK(!squad_has_type(*w, 3, mask));
        CHECK(!squad_has_armed_unit(*w, 0));
        w->units[3].flags |= OA_UNIT_FLAG_HAS_WEAPONS;
        CHECK(squad_has_armed_unit(*w, 0));
        world_destroy(w);
    }
    {
        // On-screen units of the mask are selected. Additive leaves the rest.
        // A unit that is not selectable, and another player's unit, stay as they are.
        World* w = make_world();
        uint16_t listed[8]{1, 2, 3, 5};
        w->game.hot_unit_count = 4;
        const VisibleLists shown{listed, 8, nullptr, 0};
        data::defs::CategoryMaskStorage storage;
        TypeMask mask = data::defs::category_mask_over(storage, data::limits::base_type_bits);
        data::defs::category_mask_set(&mask, 1);
        w->units[2].flags |= OA_UNIT_FLAG_SELECTED;
        w->units[3].capture_cooldown = 5;
        w->game.panel_unit_id = 7;
        const int resets = r.resets;
        select_type_mask_on_screen(*w, shown, mask, true, h);
        CHECK(selected(*w, 1) && selected(*w, 2));
        CHECK(!selected(*w, 3) && !selected(*w, 4) && !selected(*w, 5));
        CHECK(w->game.panel_unit_id == 0);
        CHECK(r.resets == resets + 1);
        CHECK((w->game.frame_flags & frame_flag_selection_changed) != 0);
        w->units[3].capture_cooldown = 0;
        w->units[3].flags &= ~OA_UNIT_FLAG_SELECTED;
        w->game.hot_unit_count = 2;
        select_type_mask_on_screen(*w, shown, mask, false, h);
        CHECK(selected(*w, 1) && !selected(*w, 2) && !selected(*w, 3));
        CHECK(!selected(*w, 4) && !selected(*w, 5));
        world_destroy(w);
    }
    {
        World* w = make_world();
        // Box over units 1 and 2 (x 100, 200; screen z 100 - 0).
        w->game.drag_start[0] = 50;
        w->game.drag_start[1] = 0;
        w->game.drag_start[2] = 50;
        w->game.drag_end[0] = 250;
        w->game.drag_end[1] = 0;
        w->game.drag_end[2] = 150;
        r = Recorder{};
        CHECK(select_units_in_box(*w, lists, false, h));
        CHECK(selected(*w, 1) && selected(*w, 2) && !selected(*w, 3));
        CHECK(r.multiple == 1 && r.speech == 0 && r.cleared == 1);
        // Reversed corners select the same box; toggling flips both off.
        w->game.drag_start[0] = 250;
        w->game.drag_end[0] = 50;
        CHECK(!select_units_in_box(*w, lists, true, h));
        CHECK(!selected(*w, 1) && !selected(*w, 2));
        w->game.drag_start[0] = 250;
        w->game.drag_end[0] = 150;
        CHECK(select_units_in_box(*w, lists, true, h));
        CHECK(selected(*w, 2) && r.speech == 1);
        world_destroy(w);
    }
    {
        // A box test of a view's own selects as the drag box does: cleared
        // first, the visible local units visited, one unit speaking and more
        // playing the selection sound; enemy units are never tested.
        World* w = make_world();
        collect_visible_units(*w, lists, h);
        r = Recorder{};

        struct Taken {
            uint16_t first;
            uint16_t last;
            int tested;
        } taken{2, 2, 0};

        const auto in_box = [](void* context, const Unit& unit) {
            auto& box = *static_cast<Taken*>(context);
            ++box.tested;
            return unit.id >= box.first && unit.id <= box.last;
        };
        w->units[1].flags |= OA_UNIT_FLAG_SELECTED;
        CHECK(select_units_where(*w, lists, false, h, in_box, &taken));
        CHECK(!selected(*w, 1) && selected(*w, 2) && !selected(*w, 3));
        CHECK(r.speech == 1 && r.multiple == 0 && r.cleared == 1);
        CHECK(taken.tested == 3);
        for (uint16_t id = 1; id <= 3; ++id)
            CHECK((w->units[id].flags & OA_UNIT_FLAG_CYCLE_VISITED) != 0);
        CHECK((w->game.frame_flags & frame_flag_selection_changed) != 0);
        taken = {1, 3, 0};
        CHECK(select_units_where(*w, lists, true, h, in_box, &taken));
        CHECK(selected(*w, 1) && !selected(*w, 2) && selected(*w, 3));
        CHECK(r.multiple == 1 && r.speech == 1 && r.cleared == 1);
        taken = {0, 0, 0};
        CHECK(!select_units_where(*w, lists, false, h, in_box, &taken));
        CHECK(!select_units_where(*w, lists, false, h, nullptr, nullptr));
        CHECK(!selected(*w, 1) && !selected(*w, 3));
        world_destroy(w);
    }
    {
        World* w = make_world();
        r = Recorder{};
        w->game.cursor_unit_id = 2;
        w->units[1].flags |= OA_UNIT_FLAG_SELECTED;
        select_cursor_unit(*w, lists, false, h);
        CHECK(!selected(*w, 1) && selected(*w, 2) && r.speech == 1);
        select_cursor_unit(*w, lists, true, h);
        CHECK(!selected(*w, 2) && r.speech == 1);
        w->game.cursor_unit_id = 4;
        select_cursor_unit(*w, lists, false, h);
        CHECK(!selected(*w, 4));
        world_destroy(w);
    }
    {
        World* w = make_world();
        r = Recorder{};
        collect_visible_units(*w, lists, h);
        w->game.pointer_state[0] = 300;
        w->game.pointer_state[1] = 100;
        w->unit_defs[2].size_x = fx(4);
        CHECK(unit_under_pointer(*w, lists, h) == 2);
        w->game.pointer_state[0] = 5;
        w->game.pointer_state[1] = 5;
        const RadarHotUnit blips[2] = {{4, 6, 6}, {5, 5, 6}};
        VisibleLists radar{ids, 8, blips, 2, 2, Rect32{0, 0, 100, 100}};
        CHECK(unit_under_pointer(*w, radar, h) == 5);
        world_destroy(w);
    }
    {
        World* w = make_world();
        r = Recorder{};
        Unit* first = next_unmarked_unit(*w);
        CHECK(first == &w->units[1]);
        cycle_next_unit(*w, lists, h);
        CHECK(w->game.cycle_unit_id == 1);
        CHECK(r.camera_x == 100 && r.camera_y == 100);
        // Every visible local unit was marked, so the scan wraps.
        CHECK(next_unmarked_unit(*w) == &w->units[1]);
        CHECK((w->units[2].flags & OA_UNIT_FLAG_CYCLE_VISITED) == 0);
        world_destroy(w);
    }
    {
        World* w = make_world();
        r = Recorder{};
        w->player_info[0].side = 0;
        w->game.players[0].info = oa_ref_from_index(0);
        oa::base::text::copy_terminated(w->game.sides[0].commander, "ARMCOM");
        w->units[3].def = oa_ref_from_index(3);
        find_commander(*w, true, h);
        CHECK(r.stops == 1);
        CHECK(selected(*w, 3) && !selected(*w, 1));
        CHECK(r.camera_x == 300);
        world_destroy(w);
    }
    {
        // Ctrl+C's follow: the last local unit of a Commander type, dead or
        // not; player 1's slot 5 shares the type but is never followed.
        World* w = make_world();
        data::defs::CategoryRegistry categories;
        data::defs::category_registry_init(&categories);
        w->game.follow_unit = 9;
        follow_commander(*w, categories);
        CHECK(w->game.follow_unit == 9);
        CHECK(data::defs::category_registry_find(&categories, "COMMANDER") != nullptr);
        data::defs::category_mask_set(
            data::defs::category_registry_find_or_add(&categories, "commander"), 1
        );
        w->units[3].flags = 0;
        follow_commander(*w, categories);
        CHECK(w->game.follow_unit == oa_unit_ref_from_slot(3));
        w->units[3].type_index = 2;
        follow_commander(*w, categories);
        CHECK(w->game.follow_unit == oa_unit_ref_from_slot(1));
        data::defs::category_registry_clear(&categories);
        world_destroy(w);
    }
    {
        // clear_selection masks flag byte 0 with 0x2f on every unit; clear_cycle_marks with 0x3f.
        World* w = make_world();
        r = Recorder{};
        const uint32_t marks =
            OA_UNIT_FLAG_SELECTED | OA_UNIT_FLAG_CYCLE_VISITED | OA_UNIT_FLAG_CYCLE_SKIP;
        const uint32_t low_byte = 0xffu; // the flag byte the two clears mask
        w->units[1].flags |= marks | OA_UNIT_FLAG_HAS_WEAPONS;
        w->units[4].flags |= marks;
        clear_cycle_marks(*w);
        CHECK((w->units[1].flags & low_byte) == (OA_UNIT_FLAG_SELECTED | OA_UNIT_FLAG_SELECTABLE));
        CHECK(selected(*w, 1) && selected(*w, 4) && r.cleared == 0);
        w->units[4].flags |= marks;
        clear_selection(*w, h);
        CHECK((w->units[1].flags & low_byte) == OA_UNIT_FLAG_SELECTABLE);
        CHECK((w->units[4].flags & low_byte) == OA_UNIT_FLAG_SELECTABLE);
        CHECK((w->units[1].flags & OA_UNIT_FLAG_HAS_WEAPONS) != 0 && r.cleared == 1);
        world_destroy(w);
    }
    {
        // mark_visible_local: visible-list units of the local player get 0x40 and lose 0x80.
        World* w = make_world();
        uint16_t hot[2] = {1, 4};
        VisibleLists visible{hot, 2, nullptr, 0};
        w->game.hot_unit_count = 2;
        w->units[1].flags |= OA_UNIT_FLAG_CYCLE_SKIP;
        w->units[4].flags |= OA_UNIT_FLAG_CYCLE_SKIP;
        mark_visible_local(*w, visible);
        CHECK(
            (w->units[1].flags & (OA_UNIT_FLAG_CYCLE_VISITED | OA_UNIT_FLAG_CYCLE_SKIP)) ==
            OA_UNIT_FLAG_CYCLE_VISITED
        );
        CHECK(
            (w->units[4].flags & (OA_UNIT_FLAG_CYCLE_VISITED | OA_UNIT_FLAG_CYCLE_SKIP)) ==
            OA_UNIT_FLAG_CYCLE_SKIP
        );
        world_destroy(w);
    }
    {
        // next_selected_unit over the local id range 1..3: forward scans after the
        // start to the last id, then first..start; backward mirrors it.
        World* w = make_world();
        CHECK(next_selected_unit(*w, nullptr, false) == nullptr);
        w->units[1].flags |= OA_UNIT_FLAG_SELECTED;
        w->units[3].flags |= OA_UNIT_FLAG_SELECTED;
        CHECK(next_selected_unit(*w, nullptr, false) == &w->units[3]);
        CHECK(next_selected_unit(*w, &w->units[3], false) == &w->units[1]);
        CHECK(next_selected_unit(*w, &w->units[1], true) == &w->units[3]);
        CHECK(next_selected_unit(*w, &w->units[3], true) == &w->units[1]);
        // An id outside the range starts at the first id.
        w->units[4].flags |= OA_UNIT_FLAG_SELECTED;
        CHECK(next_selected_unit(*w, &w->units[4], false) == &w->units[3]);
        CHECK(next_selected_unit(*w, &w->units[4], true) == &w->units[3]);
        // The start itself is the last candidate.
        w->units[1].flags &= ~OA_UNIT_FLAG_SELECTED;
        w->units[3].flags &= ~OA_UNIT_FLAG_SELECTED;
        w->units[2].flags |= OA_UNIT_FLAG_SELECTED;
        CHECK(next_selected_unit(*w, &w->units[2], false) == &w->units[2]);
        CHECK(next_selected_unit(*w, &w->units[2], true) == &w->units[2]);
        world_destroy(w);
    }
    {
        // follow_next_selected (t forward, T backward, and the observer pulse forward)
        // stores next_selected_unit's pick from the followed unit in Game.follow_unit.
        World* w = make_world();
        w->units[1].flags |= OA_UNIT_FLAG_SELECTED;
        w->units[3].flags |= OA_UNIT_FLAG_SELECTED;
        follow_next_selected(*w, false);
        CHECK(w->game.follow_unit == oa_unit_ref_from_slot(3));
        follow_next_selected(*w, false);
        CHECK(w->game.follow_unit == oa_unit_ref_from_slot(1));
        follow_next_selected(*w, true);
        CHECK(w->game.follow_unit == oa_unit_ref_from_slot(3));
        // Another player's unit starts the scan at the first local id.
        w->game.follow_unit = oa_unit_ref_from_slot(4);
        follow_next_selected(*w, true);
        CHECK(w->game.follow_unit == oa_unit_ref_from_slot(3));
        // Nothing selected ends the follow.
        w->units[1].flags &= ~OA_UNIT_FLAG_SELECTED;
        w->units[3].flags &= ~OA_UNIT_FLAG_SELECTED;
        follow_next_selected(*w, false);
        CHECK(w->game.follow_unit == 0);
        world_destroy(w);
    }
    {
        // select_next_viewpoint_unit over the viewpoint player's units 1..3.
        World* w = make_world();
        r = Recorder{};
        w->game.panel_unit_id = 7;
        select_next_viewpoint_unit(*w, h);
        CHECK(selected(*w, 1) && !selected(*w, 2) && r.cleared == 0);
        CHECK(w->game.panel_unit_id == 7);
        CHECK((w->game.frame_flags & frame_flag_selection_changed) != 0);
        w->units[3].flags |= OA_UNIT_FLAG_SELECTED;
        select_next_viewpoint_unit(*w, h);
        CHECK(!selected(*w, 1) && selected(*w, 2) && !selected(*w, 3) && r.cleared == 1);
        CHECK(w->game.panel_unit_id == 0);
        w->units[3].capture_cooldown = 5;
        w->game.panel_unit_id = 7;
        select_next_viewpoint_unit(*w, h);
        CHECK(selected(*w, 1) && !selected(*w, 2) && !selected(*w, 3) && r.cleared == 2);
        CHECK(w->game.panel_unit_id == 7);
        world_destroy(w);
    }
    {
        // assign_squad: selected local units join the squad, unselected members
        // go to squad 0, type-zero records are skipped.
        World* w = make_world();
        r = Recorder{};
        w->units[1].flags |= OA_UNIT_FLAG_SELECTED;
        w->units[2].squad = 5;
        w->units[3].squad = 2;
        w->units[4].flags |= OA_UNIT_FLAG_SELECTED;
        assign_squad(*w, 5, h);
        CHECK(r.squad_calls == 2);
        CHECK(r.squad_unit[0] == 1 && r.squad_value[0] == 5);
        CHECK(r.squad_unit[1] == 2 && r.squad_value[1] == 0);
        CHECK(w->units[3].squad == 2 && w->units[4].squad == 0);
        r = Recorder{};
        w->units[1].type_index = 0;
        assign_squad(*w, 5, h);
        CHECK(r.squad_calls == 0);
        world_destroy(w);
    }
    {
        // select_squad with the skip mask on type 2 (unit 2).
        World* w = make_world();
        r = Recorder{};
        data::defs::CategoryMaskStorage storage;
        TypeMask skip = data::defs::category_mask_over(storage, data::limits::base_type_bits);
        data::defs::category_mask_set(&skip, 2);
        w->units[1].squad = 4;
        w->units[2].squad = 4;
        w->units[3].squad = 1;
        w->units[3].flags |= OA_UNIT_FLAG_SELECTED;
        w->game.panel_unit_id = 9;
        CHECK(select_squad(*w, 4, false, skip, h));
        CHECK(selected(*w, 1) && selected(*w, 2) && !selected(*w, 3));
        CHECK(r.resets == 1 && w->game.panel_unit_id == 0);
        CHECK((w->game.frame_flags & frame_flag_selection_changed) != 0);
        // An armed member makes the skip types drop out.
        w->units[1].flags |= OA_UNIT_FLAG_HAS_WEAPONS;
        w->units[3].flags |= OA_UNIT_FLAG_SELECTED;
        CHECK(select_squad(*w, 4, true, skip, h));
        CHECK(selected(*w, 1) && !selected(*w, 2) && selected(*w, 3));
        // An unselectable member is left alone and arms nothing.
        w->units[1].capture_cooldown = 5;
        CHECK(select_squad(*w, 4, true, skip, h));
        CHECK(selected(*w, 1) && selected(*w, 2) && selected(*w, 3) && r.resets == 3);
        CHECK(!select_squad(*w, 7, false, skip, h));
        CHECK(selected(*w, 1) && !selected(*w, 2) && !selected(*w, 3));
        world_destroy(w);
    }
    {
        // A second recall of a squad that is already exactly the selection
        // centres on the average of the units' map positions and asks the
        // follow to stop. The first recall only selects. A later recall does
        // the same, with no double-press timer. A selection that is not the
        // squad does not move the camera.
        World* w = make_world();
        r = Recorder{};
        TypeMask skip{};
        w->units[1].squad = 2;
        w->units[2].squad = 2;
        w->units[1].position.x = fx(10);
        w->units[2].position.x = fx(13);
        w->units[1].position.z = fx(40);
        w->units[2].position.z = fx(50);
        w->units[3].flags |= OA_UNIT_FLAG_SELECTED;
        w->game.follow_unit = 9;
        CHECK(select_squad(*w, 2, false, skip, h));
        CHECK(selected(*w, 1) && selected(*w, 2) && !selected(*w, 3));
        CHECK(r.centred == 0 && r.stops == 0);
        CHECK(w->game.follow_unit == 9);
        CHECK(select_squad(*w, 2, false, skip, h));
        CHECK(selected(*w, 1) && selected(*w, 2) && !selected(*w, 3));
        CHECK(r.centred == 1 && r.stops == 1);
        CHECK(r.camera_x == 11 && r.camera_y == 45);
        CHECK(w->game.follow_unit == 9);
        w->units[3].flags |= OA_UNIT_FLAG_SELECTED;
        const int centred_before = r.centred;
        CHECK(select_squad(*w, 2, true, skip, h));
        CHECK(selected(*w, 3) && r.centred == centred_before && r.stops == 1);
        CHECK(select_squad(*w, 2, false, skip, h));
        CHECK(!selected(*w, 3) && r.centred == centred_before);
        CHECK(select_squad(*w, 2, false, skip, h));
        CHECK(r.centred == centred_before + 1 && r.stops == 2);
        CHECK(r.camera_x == 11 && r.camera_y == 45);
        world_destroy(w);
    }
    if (failures != 0)
        return 1;
    std::puts("sim-selection: ok");
    return 0;
}
