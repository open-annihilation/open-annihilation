// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The selection shortcuts of ui.selection-shortcuts in a running match: the
// double-click, Ctrl+S, Ctrl+B, Ctrl+F and the W, B and Y drag-box filters.

#include "oa/app/runtime.hpp"
#include "oa/sim/selection/shortcuts.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <tuple>

namespace oa::app {
namespace {

namespace selection = oa::sim::selection;

} // namespace

bool Runtime::selection_shortcuts_on() const {
    return match_ && ui_rules().selection_shortcuts.enabled;
}

bool Runtime::selection_shortcut_double_click(float x, float y, int32_t clicks) {
    // The second click of each pair is the double-click. With Double-click
    // selects On the map it is 3.1c's, the clicked unit's type on the whole
    // map, and the hack's own double-click stands aside.
    if (!selection_shortcuts_on() || clicks < 2 || clicks % 2 != 0 || match_paused_ ||
        megamap_open_ ||
        engine_settings().double_click_selects ==
            oa::ui::engine_settings::DoubleClickSelects::on_map)
        return false;
    if (action_held("build.line") || !battlefield_contains(x, y))
        return false;
    update_pointer(x, y);
    auto& world = match_->state();
    world.game.local_player_index = match_local_player_;
    const auto* unit = world.game.cursor_unit_id != 0
                           ? oa::world_unit_at(&world, world.game.cursor_unit_id)
                           : nullptr;
    const auto* owner = unit != nullptr ? oa::world_unit_owner(&world, unit) : nullptr;
    if (owner == nullptr || owner->index != world.game.local_player_index)
        return false;
    selected_match_unit_ = 0;
    std::ignore = selection::select_selected_types_on_screen(
        world,
        on_screen_lists(),
        unit_table_.tables.categories.words_per_mask * 32U,
        selection_hooks()
    );
    adopt_selected_units();
    return true;
}

bool Runtime::selection_shortcut_key(SelectionShortcut which, bool shift) {
    if (!selection_shortcuts_on() || shift)
        return false;
    auto& world = match_->state();
    world.game.local_player_index = match_local_player_;
    auto sets = std::make_unique<selection::ShortcutSets>();
    selection::build_shortcut_sets(
        world,
        unit_table_.tables.categories,
        unit_table_.tables.categories.words_per_mask * 32U,
        *sets
    );
    const auto hooks = selection_hooks();
    selected_match_unit_ = 0;
    if (which == SelectionShortcut::on_screen) {
        std::ignore =
            selection::select_mobile_combat_on_screen(world, on_screen_lists(), *sets, hooks);
    } else {
        selection::ShortcutHooks shortcuts{};
        shortcuts.context = match_.get();
        shortcuts.head_mission = [](void* context, const oa::Unit& unit) -> int32_t {
            auto& match = *static_cast<oa::sim::match_runtime::Match*>(context);
            std::array<oa::sim::match_runtime::Match::OrderRecordView, 1> head{};
            if (match.queue_records(unit.id, false, head.data(), head.size()) == 0)
                return -1;
            return head[0].kind;
        };
        if (which == SelectionShortcut::idle_builders)
            std::ignore =
                selection::cycle_idle_constructor(world, *sets, idle_cycle_, hooks, shortcuts);
        else
            std::ignore =
                selection::cycle_idle_factory(world, *sets, idle_cycle_, hooks, shortcuts);
    }
    adopt_selected_units();
    return true;
}

void Runtime::selection_shortcut_drag_filter() {
    if (!selection_shortcuts_on())
        return;
    const auto filter = selection::drag_filter(
        action_held("select.box-combat"),
        action_held("select.box-builders"),
        action_held("select.box-factories")
    );
    if (filter == selection::DragFilter::none)
        return;
    auto& world = match_->state();
    world.game.local_player_index = match_local_player_;
    auto sets = std::make_unique<selection::ShortcutSets>();
    selection::build_shortcut_sets(
        world,
        unit_table_.tables.categories,
        unit_table_.tables.categories.words_per_mask * 32U,
        *sets
    );
    std::ignore = selection::filter_box_selection(world, *sets, filter);
}

} // namespace oa::app
