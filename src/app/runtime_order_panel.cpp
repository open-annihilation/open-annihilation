// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The order and build pages: the selection summary, the pages and their
// download buttons, button states and clicks, through the order panel.
#include "oa/app/runtime.hpp"
#include "oa/sim/selection/shortcuts.hpp"
#include "oa/data/defs/layout.hpp"

#include "oa/sim/match_runtime/construction_orders.hpp"
#include "oa/core/weapon_def.h"
#include "oa/ui/gui_layout/gui_gadget.hpp"
#include "oa/ui/gui_layout.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/core/world.h"
#include "oa/sim/match_runtime.hpp"
#include "oa/data/mission_types.hpp"
#include "oa/ui/hud/order_panel.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>

namespace oa::app {

namespace {

namespace hud = oa::ui::hud;

// guis\<name> with its extension replaced by GUI, as the game builds it.
std::string gui_panel_path(const char* name) {
    std::string stem(name);
    if (const auto dot = stem.rfind('.'); dot != std::string::npos)
        stem.resize(dot);
    return oa::data::defs::gui_path(stem + ".GUI");
}

bool ascii_iequals(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

// Mission descriptor bit 0x200, which an order carries as bit 0x02 of its
// command flags: the mission takes a target unit.
constexpr uint32_t kMissionTakesTarget = 0x200;

// FIREORD, MOVEORD, ONOFF and CLOAK draw their status as the frame of their art.
bool shows_status_frame(const oa::ui::gui_layout::Gadget& gadget) {
    return gadget.common.type == oa::ui::gui_layout::GadgetType::button &&
           (static_cast<uint32_t>(gadget.common.attributes) &
            oa::ui::gui_layout::attribute::cycle_frames) != 0;
}

} // namespace

oa::ui::hud::UnitTable Runtime::order_panel_table() {
    oa::World& world = match_->state();
    return {
        world.units,
        static_cast<uint16_t>(std::min<uint32_t>(world.unit_slot_count, 0xffffu)),
        world.unit_defs,
        static_cast<int32_t>(world.unit_def_count)
    };
}

oa::ui::hud::PanelControls Runtime::order_panel_controls() {
    hud::PanelControls controls{};
    controls.user = this;
    // The first gadget after the panel whose name holds `name`.
    controls.find = [](void* user, const char* name) -> int32_t {
        const auto& self = *static_cast<Runtime*>(user);
        if (!self.match_hud_)
            return -1;
        const auto& gadgets = self.match_hud_->layout.gadgets;
        for (std::size_t index = 1; index < gadgets.size(); ++index)
            if (gadgets[index].common.name.find(name) != std::string::npos)
                return static_cast<int32_t>(index);
        return -1;
    };
    const auto set_status = [](void* user, int32_t index, int32_t value) {
        auto& states = static_cast<Runtime*>(user)->match_hud_states_;
        if (index >= 0 && static_cast<std::size_t>(index) < states.size())
            states[static_cast<std::size_t>(index)].status = static_cast<int16_t>(value);
    };
    controls.set_group_value = set_status;
    controls.set_value = set_status;
    // A greyed button stays drawn, in its greyed art, and takes no click.
    controls.disable = [](void* user, int32_t index) {
        auto& self = *static_cast<Runtime*>(user);
        if (index < 0 || !self.match_hud_)
            return;
        const auto at = static_cast<std::size_t>(index);
        auto& gadgets = self.match_hud_->layout.gadgets;
        if (at < gadgets.size())
            if (auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadgets[at].fields))
                button->grayed_out = true;
        if (at < self.match_hud_states_.size())
            self.match_hud_states_[at].grayed = true;
    };
    // A hidden control is neither drawn nor under the pointer.
    controls.set_active = [](void* user, int32_t index, bool shown) {
        auto& self = *static_cast<Runtime*>(user);
        if (index < 0 || !self.match_hud_ ||
            static_cast<std::size_t>(index) >= self.match_hud_->layout.gadgets.size())
            return;
        self.match_hud_->layout.gadgets[static_cast<std::size_t>(index)].common.active =
            shown ? 1 : 0;
    };
    return controls;
}

oa::ui::hud::HudEvents Runtime::order_panel_events() {
    return {
        this,
        [](void* user, const char* name) {
            static_cast<Runtime*>(user)->play_match_interface_sound(name);
        },
        [](void* user, const char* tag, int32_t value) {
            static_cast<Runtime*>(user)->apply_group_order(tag, value);
        },
    };
}

void Runtime::apply_group_order(const char* tag, int32_t value) {
    if (!match_)
        return;
    for_each_selected([&](uint16_t id) { give_state_order(id, tag, value); });
}

bool Runtime::give_state_order(uint16_t unit, const char* tag, int32_t value) {
    const auto kind = oa::data::mission_types::index_for_name(tag);
    if (!match_ || kind == oa::data::mission_types::unknown_mission)
        return false;
    const auto table = order_panel_table();
    if (unit >= table.unit_count)
        return false;
    const auto* def = hud::unit_def(table, table.units[unit]);
    if (def == nullptr || !hud::group_order_reaches(tag, *def))
        return false;
    match_->issue_state_order(unit, kind, value);
    return true;
}

void Runtime::issue_group_mission(uint8_t kind, int32_t parameter_1, int32_t parameter_2) {
    namespace runtime = oa::sim::match_runtime;
    if (!match_ || kind == oa::data::mission_types::unknown_mission ||
        kind >= runtime::mission_descriptor_table.size())
        return;
    const auto& world = match_->state();
    const bool takes_target = (runtime::mission_descriptor_table[kind] & kMissionTakesTarget) != 0;
    const uint16_t target = takes_target ? world.game.cursor_unit_id : 0;
    const char* tag = oa::data::mission_types::registered_names()[kind].data();
    const bool queue = queueing();
    const auto table = order_panel_table();
    for_each_selected([&](uint16_t id) {
        if (id >= table.unit_count || (target != 0 && id == target))
            return;
        const auto* def = hud::unit_def(table, table.units[id]);
        if (def == nullptr || !hud::group_order_reaches(tag, *def))
            return;
        try {
            match_->issue_or_cancel_order(
                id, kind, queue, target, nullptr, parameter_1, parameter_2
            );
        } catch (const std::exception& error) {
            status_ = std::string("assign: ") + error.what();
        }
    });
}

oa::ui::hud::SelectionSummary Runtime::summarize_order_panel(hud::OrderPanelState& state) {
    oa::World& world = match_->state();
    uint32_t count = 0;
    const auto* first =
        oa::world_player_units(&world, &world.game.players[match_local_player_], &count);
    const auto first_index = first != nullptr ? oa::world_unit_slot(&world, first) : 1u;
    const auto last_index = first != nullptr ? first_index + count - 1u : 0u;
    const auto summary = hud::summarize_selection(
        state,
        order_panel_table(),
        static_cast<uint16_t>(first_index),
        static_cast<uint16_t>(last_index)
    );
    state.frame_flags = summary.frame_flags;
    state.order_flags = summary.order_flags;
    state.order_flags2 = summary.order_flags2;
    return summary;
}

bool Runtime::order_command_available(std::string_view name) {
    if (!match_)
        return true;
    // The selection's abilities, as the order panel reads them; the panel's
    // own words are left as they are.
    auto state = hud::order_panel_load(match_->state().game);
    const auto summary = summarize_order_panel(state);
    if (summary.count == 0)
        return true;
    const auto open = [&summary](uint16_t bit) { return (summary.order_flags & bit) != 0; };
    const auto action = match_hud_action(name);
    if (action == "MOVE")
        return open(hud::kOrderCanMove);
    if (action == "STOP")
        return open(hud::kOrderCanStop);
    if (action == "ATTACK")
        return open(hud::kOrderCanAttack);
    if (action == "DEFEND" || action == "GUARD")
        return open(hud::kOrderCanGuard);
    if (action == "PATROL")
        return open(hud::kOrderCanPatrol);
    if (action == "RECLAIM")
        return open(hud::kOrderCanReclaim);
    if (action == "REPAIR")
        return open(hud::kOrderCanRepair);
    if (action == "CAPTURE")
        return open(hud::kOrderCanCapture);
    if (action == "LOAD" || action == "UNLOAD")
        return open(hud::kOrderCanTransport);
    if (action == "BLAST" || action == "DGUN")
        return (summary.order_flags2 & hud::kOrder2CanBlast) != 0 && !open(hud::kOrderCanTransport);
    return true;
}

std::string Runtime::match_side_name_prefix() const {
    const auto side = match_view_side();
    if (side < side_table_.count) {
        const auto& prefix =
            oa::data::defs::side_at(side_table_, static_cast<uint32_t>(side))->name_prefix;
        return std::string(prefix, strnlen(prefix, sizeof prefix));
    }
    auto prefix = match_side_prefix();
    for (auto& c : prefix)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return prefix;
}

void Runtime::bind_gadget_gaf_art() {
    if (!match_hud_)
        return;
    for (const auto& gadget : match_hud_->layout.gadgets)
        if (gadget.common.gaf_file &&
            gaf_sequence(match_hud_->sprites, gadget.common.name) == nullptr)
            append_gaf_file(match_hud_->sprites, "anims/" + gadget.common.name + "_gadget.gaf");
}

int Runtime::side_column_page_rows() {
    if (!match_)
        return kCanvasHeight;
    const oa::World& world = match_->state();
    if (side_column_measured_for_ == &world)
        return side_column_rows_;
    int rows = kCanvasHeight;
    const auto measure = [&](const char* name) {
        const auto bytes = assets_.load_file_contents(gui_panel_path(name));
        if (!bytes)
            return;
        auto parsed = oa::ui::gui_layout::parse(*bytes);
        if (!parsed.ok())
            return;
        auto& gadgets = parsed.layout->gadgets;
        if (gadgets.empty())
            return;
        // The page's panel reaches down to its authored height, and its
        // controls may reach below that.
        const auto& panel = gadgets.front().common;
        rows = std::max(rows, panel.y + panel.height);
        std::ignore = place_in_side_panel(gadgets);
        rows = std::max(rows, page_bottom_row(gadgets, true));
    };
    char name[256];
    for (uint32_t type = 1; type < world.unit_def_count; ++type) {
        const auto& definition = world.unit_defs[type];
        if (definition.unit_name[0] == '\0')
            continue;
        for (uint32_t page = 0; page < definition.gui_page_count; ++page) {
            hud::format_build_page_name(name, sizeof name, definition, page);
            measure(name);
        }
    }
    for (std::size_t side = 0; side < side_table_.count; ++side) {
        const auto& field =
            oa::data::defs::side_at(side_table_, static_cast<uint32_t>(side))->name_prefix;
        const std::string prefix(field, strnlen(field, sizeof field));
        hud::format_general_page_name(name, sizeof name, prefix.c_str());
        measure(name);
        // The page shown when nothing is selected, whose art fills the well.
        hud::format_idle_page_name(name, sizeof name, prefix.c_str());
        measure(name);
        std::snprintf(name, sizeof name, "%sDL", prefix.c_str());
        measure(name);
    }
    side_column_measured_for_ = &world;
    side_column_rows_ = rows;
    return rows;
}

oa::ui::display_layout::MatchLayout Runtime::lay_out_match(int width, int height) {
    return make_room_for_clock_line(
        oa::ui::display_layout::fit_side_column(
            oa::ui::display_layout::make_match_layout(width, height, match_chrome_most_scale()),
            side_column_page_rows()
        )
    );
}

oa::ui::hud::PanelLoader Runtime::order_panel_loader() {
    hud::PanelLoader loader{};
    loader.user = this;
    loader.close_to_root = [](void*) { return true; };
    loader.is_loaded = [](void* user, const char* name) {
        return ascii_iequals(static_cast<Runtime*>(user)->match_hud_panel_, gui_panel_path(name));
    };
    // A unit's page, and the general page, which comes without a unit, are
    // drawn in the side column as authored, scaled down when taller than it.
    loader.load = [](void* user, const char* name, const oa::Unit*, int32_t) {
        return static_cast<Runtime*>(user)->load_match_hud_layout(
            gui_panel_path(name), hud::SidePage::unit
        );
    };
    loader.downloads = unit_table_.tables.downloads.groups;
    loader.download_count = unit_table_.tables.downloads.count;
    // The linked gadget becomes a unit button named after the unit, ungreyed,
    // with its art from the unit's _gadget GAF.
    loader.link_button = [](void* user, int32_t index, const char* unit_name) {
        auto& self = *static_cast<Runtime*>(user);
        if (!self.match_hud_ || index < 0 ||
            static_cast<std::size_t>(index) >= self.match_hud_->layout.gadgets.size())
            return;
        auto& gadget = self.match_hud_->layout.gadgets[static_cast<std::size_t>(index)];
        gadget.common.name = unit_name;
        gadget.common.common_attributes = static_cast<int8_t>(hud::kCommonUnitButton);
        gadget.common.gaf_file = true;
        if (auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields))
            button->grayed_out = false;
        if (static_cast<std::size_t>(index) < self.match_hud_states_.size())
            self.match_hud_states_[static_cast<std::size_t>(index)].grayed = false;
    };
    // Buttons linked into the page's empty slots take their places in it.
    loader.redraw = [](void* user) {
        auto& self = *static_cast<Runtime*>(user);
        self.bind_gadget_gaf_art();
        self.fit_match_build_page();
    };
    loader.exists = [](void* user, const char* name) {
        return static_cast<Runtime*>(user)->assets_.file_size(gui_panel_path(name)) != 0;
    };
    loader.format_counts = [](void* user, const oa::Unit&) {
        static_cast<Runtime*>(user)->refresh_build_page(false);
    };
    loader.check_validity = [](void* user) {
        static_cast<Runtime*>(user)->refresh_build_page(true);
    };
    return loader;
}

oa::ui::hud::BuildPanelHost Runtime::build_panel_host() {
    hud::BuildPanelHost host{};
    host.user = this;
    host.type_for_name = [](void* user, const char* name) {
        return oa::sim::unit_spawn::find_type_index(
            static_cast<Runtime*>(user)->spawn_type_names_, name
        );
    };
    host.order_click = [](void* user, const char* name) {
        auto& self = *static_cast<Runtime*>(user);
        const auto& gadgets = self.match_hud_->layout.gadgets;
        for (std::size_t index = 1; index < gadgets.size(); ++index)
            if (gadgets[index].common.name == name)
                return self.run_match_order_button(index, name);
        return false;
    };
    // x5 is the build buttons' Shift while touch controls are on.
    host.shift_down = [](void* user) {
        const auto& self = *static_cast<Runtime*>(user);
        return self.modifier_role_active("build.queue-modifier") ||
               self.virtual_shift(ModifierUse::build_button);
    };
    // ui.selection-shortcuts: with the hundred modifier held a shift-click steps by 100.
    if (ui_rules().selection_shortcuts.enabled)
        host.shift_step =
            oa::sim::selection::shift_queue_step(modifier_role_active("build.hundred-modifier"));
    host.change_queue = [](void* user, const char* name, oa::Unit& builder, int32_t count) {
        auto& self = *static_cast<Runtime*>(user);
        const auto type_for_name = [](void* context, const char* unit_name) {
            return oa::sim::unit_spawn::find_type_index(
                static_cast<Runtime*>(context)->spawn_type_names_, unit_name
            );
        };
        const auto change = hud::classify_build_queue_change(
            name,
            builder,
            self.match_view_player(),
            count,
            type_for_name,
            user,
            self.order_panel_events()
        );
        if (change.kind == hud::BuildQueueKind::none) {
            self.status_ = std::string(name) + " is not implemented yet.";
            std::cerr << "unsupported operation: " << self.status_ << '\n';
            return;
        }
        try {
            self.match_->change_queued_count(
                builder.id, oa::data::mission_types::index_for_name(change.tag), change.type, count
            );
            self.status_ = std::string("Build ") + name + " x" +
                           std::to_string(self.match_->queued_build_count(builder.id, change.type));
        } catch (const std::exception& error) {
            self.status_ = std::string("build queue: ") + error.what();
            std::cerr << "unsupported operation: " << self.status_ << '\n';
        }
    };
    host.stockpiles = [](void* user, const oa::Unit& unit) {
        const auto* weapon = oa::world_weapon_def(
            &static_cast<Runtime*>(user)->match_->state(), unit.weapons[0].def
        );
        return weapon != nullptr && (weapon->flags & OA_WEAPON_FLAG_STOCKPILE) != 0;
    };
    host.format_counts = [](void* user, const oa::Unit&) {
        static_cast<Runtime*>(user)->refresh_build_page(false);
    };
    if (const auto* profile = mod_profile())
        host.placement_by_builder = profile->rules.units.placement_by_builder.enabled;
    return host;
}

bool Runtime::toggle_order_button(std::size_t index) {
    if (!match_ || !match_hud_ || index >= match_hud_->layout.gadgets.size())
        return false;
    oa::World& world = match_->state();
    auto state = hud::order_panel_load(world.game);
    const bool toggled = hud::order_panel_toggle(
        state,
        order_panel_table(),
        order_panel_controls(),
        static_cast<int32_t>(index),
        match_hud_->layout.gadgets[index].common.name.c_str(),
        order_panel_events()
    );
    if (toggled)
        hud::order_panel_store(world.game, state);
    return toggled;
}

const Runtime::MatchGadgetState*
Runtime::match_gadget_state(const oa::ui::gui_layout::Gadget& gadget) const {
    if (!match_hud_ || !shows_status_frame(gadget))
        return nullptr;
    const auto& gadgets = match_hud_->layout.gadgets;
    if (&gadget < gadgets.data() || &gadget >= gadgets.data() + gadgets.size())
        return nullptr;
    const auto index = static_cast<std::size_t>(&gadget - gadgets.data());
    return index < match_hud_states_.size() ? &match_hud_states_[index] : nullptr;
}

std::optional<std::size_t> Runtime::match_status_frame(std::size_t index) const {
    if (!match_hud_ || index >= match_hud_->layout.gadgets.size())
        return std::nullopt;
    const auto& gadget = match_hud_->layout.gadgets[index];
    const auto* state = match_gadget_state(gadget);
    if (state == nullptr)
        return std::nullopt;
    const auto* sequence = gaf_sequence(match_hud_->sprites, gadget.common.name);
    if (sequence == nullptr)
        sequence = gaf_sequence(match_hud_->shared_sprites, gadget.common.name);
    if (sequence == nullptr || sequence->frames.empty())
        return std::nullopt;
    const auto last = sequence->frames.size() - 1U;
    if (state->grayed)
        return last;
    return std::min(static_cast<std::size_t>(std::max<int16_t>(state->status, 0)), last);
}

} // namespace oa::app
