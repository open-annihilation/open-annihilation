// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// HUD gadget actions, build placement, features and wrecks.
#include "oa/app/runtime.hpp"
#include "oa/core/map_plot.h"
#include "oa/sim/feature_runtime.hpp"
#include "oa/ui/gui_layout/gui_gadget.hpp"
#include "oa/ui/hud/command_buttons.hpp"
#include "oa/ui/hud/order_panel.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {

void Runtime::refresh_build_page(bool check_validity) {
    build_captions_.clear();
    if (!match_hud_ || match_build_page_ == 0 || !match_ || selected_match_unit_ == 0)
        return;
    const oa::Unit* builder = oa::world_unit_at(&match_->state(), selected_match_unit_);
    auto& gadgets = match_hud_->layout.gadgets;
    const auto* page = gadgets.empty()
                           ? nullptr
                           : std::get_if<oa::ui::gui_layout::PanelFields>(&gadgets.front().fields);
    if (builder == nullptr || page == nullptr)
        return;
    const auto count = std::clamp<int32_t>(
        page->loaded_total_gadgets, 0, static_cast<int32_t>(gadgets.size()) - 1
    );
    std::vector<oa::ui::hud::BuildPageRecord> records(static_cast<std::size_t>(count) + 1U);
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& gadget = gadgets[index];
        auto& record = records[index];
        record.name = gadget.common.name.c_str();
        record.gadget_type = static_cast<uint8_t>(gadget.common.type);
        record.common_attributes = static_cast<uint8_t>(gadget.common.common_attributes);
        if (const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields)) {
            record.grayed = button->grayed_out;
            std::snprintf(record.caption, sizeof record.caption, "%s", button->text.c_str());
        }
    }
    const auto type_for_name = [](void* user, const char* name) {
        return oa::sim::unit_spawn::find_type_index(
            static_cast<Runtime*>(user)->spawn_type_names_, name
        );
    };
    const auto queued = [](void* user, const oa::Unit& builder, uint16_t type) {
        return static_cast<Runtime*>(user)->match_->queued_build_count(builder.id, type);
    };
    oa::ui::hud::format_build_counts(records.data(), count, *builder, type_for_name, queued, this);
    if (check_validity) {
        oa::ui::hud::update_build_button_validity(records.data(), count, type_for_name, this);
        for (std::size_t index = 0; index < records.size(); ++index)
            if (auto* button =
                    std::get_if<oa::ui::gui_layout::ButtonFields>(&gadgets[index].fields))
                button->grayed_out = records[index].grayed;
    }
    build_captions_.resize(records.size());
    for (std::size_t index = 0; index < records.size(); ++index)
        build_captions_[index] = records[index].caption;
}

void Runtime::draw_build_captions() {
    // A paused menu's panel (ARMOPT.GUI, PREFS.GUI, ...) is no build page:
    // its buttons carry no build counts.
    if (pause_menu_shown())
        return;
    refresh_build_page(false);
    if (!match_hud_)
        return;
    constexpr uint8_t build_button_attributes =
        oa::ui::hud::kCommonUnitButton | oa::ui::hud::kCommonWeaponButton;
    for (std::size_t index = 0; index < build_captions_.size(); ++index) {
        const auto& gadget = match_hud_->layout.gadgets[index];
        // A hidden button shows no count.
        if (build_captions_[index].empty() || gadget.common.active == 0 ||
            (static_cast<uint8_t>(gadget.common.common_attributes) & build_button_attributes) == 0)
            continue;
        draw_hud_label(gadget.common.x + 4, gadget.common.y + 4, build_captions_[index], 255);
    }
}

void Runtime::activate_match_hud(std::size_t index, bool left_button) {
    namespace hud = oa::ui::hud;
    if (!match_hud_ || index >= match_hud_->layout.gadgets.size())
        return;
    if (match_paused_) {
        // A grayed-out button of the paused menus takes no press.
        const auto& gadget = match_hud_->layout.gadgets[index];
        if (const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
            button == nullptr || !button->grayed_out)
            activate_pause_gadget(gadget.common.name);
        return;
    }
    if (selected_match_unit_ == 0)
        return;
    const auto& gadget = match_hud_->layout.gadgets[index];
    if (!gadget_command_available(gadget))
        return;
    const std::string gadget_name = gadget.common.name;
    auto state = hud::order_panel_load(match_->state().game);
    const auto click = hud::on_build_panel_click(
        state,
        order_panel_table(),
        gadget_name.c_str(),
        left_button,
        build_panel_host(),
        order_panel_events()
    );
    switch (click.action) {
    case hud::BuildPanelClick::page_back:
    case hud::BuildPanelClick::page_forward:
        play_match_interface_sound("nextbuildmenu");
        press_match_panel_page(click.action, false);
        break;
    case hud::BuildPanelClick::orders:
    case hud::BuildPanelClick::build:
        press_match_panel_page(click.action, false);
        break;
    case hud::BuildPanelClick::place:
        match_command_ = MatchCommand::build;
        pending_build_type_ = click.type;
        status_ = "Place " + gadget_name + ": click the map";
        break;
    case hud::BuildPanelClick::queued:
    case hud::BuildPanelClick::none:
        break;
    }
}

bool Runtime::run_match_order_button(std::size_t index, std::string_view gadget_name) {
    const auto action = match_hud_action(gadget_name);
    if (action == "FIREORD" || action == "MOVEORD" || action == "ONOFF" || action == "CLOAK") {
        toggle_order_button(index);
    } else if (press_match_command_button(index)) {
    } else if (action == "SELFD" || action == "DESTRUCT") {
        if (match_)
            match_->toggle_self_destruct(selected_local_ids());
        status_ = "Self-destruct";
    } else {
        return false;
    }
    return true;
}

namespace {

namespace armed = oa::ui::hud::armed_order;

struct ArmedCommand {
    uint8_t order = 0;
    MatchCommand command{};
    const char* prompt = nullptr;
};

constexpr ArmedCommand kArmedCommands[] = {
    {armed::move, MatchCommand::move, "Move: click the battlefield"},
    {armed::attack, MatchCommand::attack, "Attack: click an enemy"},
    {armed::blast, MatchCommand::dgun, "D-Gun: click an enemy"},
    {armed::unload, MatchCommand::unload, "Unload: click the battlefield"},
    {armed::load, MatchCommand::load, "Load: click a unit"},
    {armed::defend, MatchCommand::guard, "Guard: click a unit"},
    {armed::repair, MatchCommand::repair, "Repair: click a damaged unit"},
    {armed::patrol, MatchCommand::patrol, "Patrol: click the battlefield"},
    {armed::reclaim, MatchCommand::reclaim, "Reclaim: click a unit"},
    {armed::capture, MatchCommand::capture, "Capture: click an enemy"},
};

bool is_button(const oa::ui::gui_layout::Gadget& gadget) {
    return gadget.common.type == oa::ui::gui_layout::GadgetType::button;
}

/// One command word of the order page and the order it arms.
struct OrderWord {
    std::string_view word{};
    uint8_t order = 0;
};

// The order page's command words in the order a button's name is tried, as
// the order panel tries them: UNLOAD before LOAD, which it holds.
constexpr OrderWord kOrderWords[] = {
    {"MOVE", armed::move},
    {"STOP", armed::default_order},
    {"ATTACK", armed::attack},
    {"BLAST", armed::blast},
    {"DEFEND", armed::defend},
    {"REPAIR", armed::repair},
    {"PATROL", armed::patrol},
    {"RECLAIM", armed::reclaim},
    {"CAPTURE", armed::capture},
    {"UNLOAD", armed::unload},
    {"LOAD", armed::load},
};

/// Returns the command word a name holds, as the order panel reads a button's name.
///
/// @param name an order panel name or a button's name
/// @return the first command word the name holds, or null
const OrderWord* order_word_in(std::string_view name) {
    for (const auto& word : kOrderWords)
        if (name.find(word.word) != std::string_view::npos)
            return &word;
    return nullptr;
}

/// Tells whether a button's name is one of the order page's standing order toggles, whose
/// names may hold a command word (MOVEORD holds MOVE).
///
/// @param name the button's name
/// @return true for FIREORD, MOVEORD, ONOFF and CLOAK
bool standing_order_toggle(std::string_view name) {
    for (const std::string_view toggle : {"FIREORD", "MOVEORD", "ONOFF", "CLOAK"})
        if (name.find(toggle) != std::string_view::npos)
            return true;
    return false;
}

} // namespace

bool Runtime::press_match_command_button(std::size_t index) {
    if (index >= match_hud_states_.size())
        return false;
    const auto& gadget = match_hud_->layout.gadgets[index];
    auto& state = match_hud_states_[index];
    const bool toggles = (static_cast<uint32_t>(gadget.common.attributes) &
                          oa::ui::gui_layout::attribute::toggle) != 0;
    const int16_t status = toggles && state.status == 0 ? 1 : 0;
    auto events = order_panel_events();
    events.apply_standing_order = [](void* user, const char*, int32_t) {
        auto& self = *static_cast<Runtime*>(user);
        self.for_each_selected([&](uint16_t id) { self.match_->issue_stop(id); });
    };
    auto& game = match_->state().game;
    if (!oa::ui::hud::order_panel_command(game, gadget.common.name.c_str(), status, events))
        return false;
    state.status = status;
    for (std::size_t other = 1; other < match_hud_states_.size(); ++other) {
        const auto& candidate = match_hud_->layout.gadgets[other];
        if (gadget.common.association != 0 && other != index && is_button(candidate) &&
            candidate.common.association == gadget.common.association)
            match_hud_states_[other].status = 0;
    }
    const auto order = oa::ui::hud::armed_order_of(game);
    match_command_ = MatchCommand::none;
    status_ = gadget.common.name.find("STOP") != std::string::npos ? "Stop" : "";
    for (const auto& armed_command : kArmedCommands)
        if (armed_command.order == order) {
            match_command_ = armed_command.command;
            status_ = armed_command.prompt;
            break;
        }
    return true;
}

bool Runtime::arm_match_command(std::string_view name, bool toggle) {
    if (!match_)
        return false;
    const auto* word = order_word_in(name);
    if (word == nullptr || word->word != name || !order_command_available(name))
        return false;
    const bool stop = word->order == armed::default_order;
    auto command = MatchCommand::none;
    for (const auto& armed_command : kArmedCommands)
        if (armed_command.order == word->order)
            command = armed_command.command;
    const bool armed_now = !stop && command != MatchCommand::none && match_command_ == command;
    if (armed_now && !toggle)
        return true;
    // The loaded page's button for the order is pressed, so it lights and
    // its association group clears as a click does.
    if (match_hud_) {
        const auto& gadgets = match_hud_->layout.gadgets;
        for (std::size_t index = 1; index < gadgets.size() && index < match_hud_states_.size();
             ++index) {
            const auto& gadget = gadgets[index];
            if (!is_button(gadget) || standing_order_toggle(gadget.common.name) ||
                order_word_in(gadget.common.name) != word)
                continue;
            auto& state = match_hud_states_[index];
            // The press turns a lit button off and an unlit one on: a button
            // left lit for an order no longer armed starts unlit, and the armed
            // order's unlit button is taken back by name below.
            if (armed_now && state.status == 0)
                break;
            if (!armed_now)
                state.status = 0;
            return press_match_command_button(index);
        }
    }
    // With no such button loaded (a build page), the order is armed by name
    // as its button would arm it.
    auto events = order_panel_events();
    events.apply_standing_order = [](void* user, const char*, int32_t) {
        auto& self = *static_cast<Runtime*>(user);
        self.for_each_selected([&](uint16_t id) { self.match_->issue_stop(id); });
    };
    auto& game = match_->state().game;
    const std::string order_name(name);
    if (!oa::ui::hud::order_panel_command(game, order_name.c_str(), armed_now ? 0 : 1, events))
        return false;
    const auto order = oa::ui::hud::armed_order_of(game);
    match_command_ = MatchCommand::none;
    status_ = stop ? "Stop" : "";
    for (const auto& armed_command : kArmedCommands)
        if (armed_command.order == order) {
            match_command_ = armed_command.command;
            status_ = armed_command.prompt;
            break;
        }
    return true;
}

bool Runtime::match_command_lit(std::size_t index) const {
    if (!match_hud_ || index >= match_hud_states_.size() ||
        index >= match_hud_->layout.gadgets.size())
        return false;
    const auto& gadget = match_hud_->layout.gadgets[index];
    const auto attributes = static_cast<uint32_t>(gadget.common.attributes);
    // A toggle shows lit while its order is armed, and a holding button,
    // such as the ORDERS or BUILD tab of the page on show, shows pressed
    // while its status is set.
    return is_button(gadget) &&
           (attributes & (oa::ui::gui_layout::attribute::toggle |
                          oa::ui::gui_layout::attribute::text_list)) != 0 &&
           (attributes & oa::ui::gui_layout::attribute::cycle_frames) == 0 &&
           match_hud_states_[index].status != 0;
}

void Runtime::reset_match_command() {
    match_command_ = MatchCommand::none;
    if (!match_hud_)
        return;
    const auto& gadgets = match_hud_->layout.gadgets;
    std::size_t stop = 1;
    while (stop < gadgets.size() && gadgets[stop].common.name.find("STOP") == std::string::npos)
        ++stop;
    if (stop >= gadgets.size())
        return;
    const auto group = gadgets[stop].common.association;
    for (std::size_t index = 1; index < gadgets.size() && index < match_hud_states_.size(); ++index)
        if (is_button(gadgets[index]) && gadgets[index].common.association == group)
            match_hud_states_[index].status = 0;
}

bool Runtime::queueing() const {
    return match_ != nullptr && (match_->state().game.pointer_state[2] &
                                 oa::sim::gameplay_input::pointer_key_shift) != 0;
}

void Runtime::finish_issued_command() {
    if (!queueing())
        reset_match_command();
}

void Runtime::play_match_interface_sound(std::string_view name) {
    if (heard_interface_sounds_ != nullptr)
        heard_interface_sounds_->emplace_back(name);
    if (options_.mute)
        return;
    const auto selection =
        oa::audio::game_audio::select(audio_registry_, name, false, sound_playback_state());
    std::string error;
    if (selection.status == oa::audio::game_audio::SelectionStatus::selected &&
        !sound_found_missing(selection.sound->resource) && !audio_player_.play(selection, error))
        report_unplayed_sound(selection.sound->resource, error);
}

void Runtime::place_pending_build_at(const oa::sim::ground_orders::Point& target) {
    place_pending_build_at(target, queueing());
}

void Runtime::place_pending_build_at(const oa::sim::ground_orders::Point& target, bool queue) {
    if (!match_ || pending_build_type_ == 0 || selected_match_unit_ == 0)
        return;
    const auto site = pending_build_site(target);
    if (!site)
        return;
    if (!site->legal) {
        // A refused site only plays notoktobuild; build mode stays.
        play_match_interface_sound("notoktobuild");
        return;
    }
    issue_pending_build(site->world, queue);
}

void Runtime::issue_pending_build(const oa::sim::ground_orders::Point& site, bool queue) {
    if (!match_ || pending_build_type_ == 0)
        return;
    try {
        for_each_selected([&](uint16_t id) {
            const auto* def = definition_for(id);
            if (def == nullptr || !def->builder || def->bm_code == 0)
                return;
            // VTOL_MobileBuild for a builder that flies; the queued-order
            // match takes off a site whatever it builds.
            const auto& state = match_->state();
            const auto* unit = oa::world_unit_at(&state, id);
            const auto* unit_def = unit != nullptr ? oa::world_unit_def_of(&state, unit) : nullptr;
            const bool flies =
                unit_def != nullptr && (unit_def->flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0;
            const auto order = flies ? oa::sim::gameplay_input::UnitOrder::vtol_mobile_build
                                     : oa::sim::gameplay_input::UnitOrder::mobile_build;
            // The building is placed facing as the player turned it
            // (units.build-rotation).
            if (!cancels_queued_order(id, order, 0, site, queue))
                match_->issue_mobile_build(
                    id,
                    pending_build_type_,
                    site,
                    queue,
                    static_cast<uint8_t>(pending_build_facing())
                );
        });
        play_match_interface_sound("oktobuild");
        status_ = "Build " + spawn_type_names_.at(pending_build_type_);
        if (!queue) {
            reset_match_command();
            pending_build_type_ = 0;
        }
    } catch (const std::exception& error) {
        status_ = std::string("build placement: ") + error.what();
        std::cerr << "unsupported operation: " << status_ << '\n';
    }
}

void Runtime::check_build_placement() {
    if (!match_)
        throw std::runtime_error("build placement check needs a running match");
    const auto mex = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMMEX");
    if (mex == 0 || mex >= spawn_types_.size())
        throw std::runtime_error("build placement check lacks ARMMEX");
    uint16_t builder = 0;
    for (const auto& slot : match_->world().slots) {
        if (slot.unit_index == 0 || slot.unit == nullptr ||
            slot.record.owner_index != match_local_player_ || slot.record.build_remaining != 0.0F)
            continue;
        const auto* def = definition_for(slot.unit_index);
        if (def != nullptr && def->builder && def->bm_code != 0) {
            builder = slot.unit_index;
            break;
        }
    }
    if (builder == 0)
        throw std::runtime_error("build placement check found no local mobile builder");
    const auto fx = spawn_types_[mex].footprint_x;
    const auto fz = spawn_types_[mex].footprint_z;
    const auto& spatial = match_->spatial();
    std::optional<std::pair<int32_t, int32_t>> clear;
    std::optional<std::pair<int32_t, int32_t>> refused;
    for (int32_t z = 1; z + fz < static_cast<int32_t>(spatial.terrain_height); ++z)
        for (int32_t x = 1; x + fx < static_cast<int32_t>(spatial.terrain_width); ++x) {
            if (!clear && match_->building_site(mex, x, z, 0, match_local_player_))
                clear = std::pair{x, z};
            if (!refused && !match_->building_site_clear(mex, x, z, 0))
                refused = std::pair{x, z};
        }
    if (!clear || !refused)
        throw std::runtime_error("build placement check found no clear and refused ARMMEX sites");
    const auto centre = [&](std::pair<int32_t, int32_t> cell) {
        return oa::sim::ground_orders::Point{
            (cell.first * 2 + fx) * 0x80000, 0, (cell.second * 2 + fz) * 0x80000
        };
    };
    const auto primary_count = [&] {
        std::size_t count = 0;
        for (const auto* order = match_->orders(builder).primary; order != nullptr;
             order = order->next)
            ++count;
        return count;
    };
    const auto saved_selection = selected_match_unit_;
    const auto saved_command = match_command_;
    const auto saved_type = pending_build_type_;
    clear_local_selection();
    adopt_selection(builder);
    selected_match_unit_ = builder;
    match_->stop_orders(builder);
    match_command_ = MatchCommand::build;
    pending_build_type_ = mex;
    const auto refused_site = pending_build_site(centre(*refused));
    if (!refused_site || refused_site->legal)
        throw std::runtime_error("build placement check: ghost accepted a refused ARMMEX site");
    place_pending_build_at(refused_site->world);
    if (primary_count() != 0 || match_command_ != MatchCommand::build)
        throw std::runtime_error("build placement check: a refused site issued an order");
    const auto clear_site = pending_build_site(centre(*clear));
    const auto height =
        match_->building_site(mex, clear->first, clear->second, 0, match_local_player_);
    if (!clear_site || !clear_site->legal || !height ||
        clear_site->world[1] != static_cast<int32_t>(static_cast<uint32_t>(*height) << 16))
        throw std::runtime_error("build placement check: ghost refused a clear ARMMEX site");
    place_pending_build_at(clear_site->world);
    const auto* order = match_->orders(builder).primary;
    if (order == nullptr || order->kind != oa::sim::match_runtime::mobile_build_kind)
        throw std::runtime_error("build placement check: a clear site did not issue MobileBuild");
    match_->stop_orders(builder);
    std::cout << "build placement check: ARMMEX refused at " << refused->first << ','
              << refused->second << ", placed at " << clear->first << ',' << clear->second
              << " height " << static_cast<int>(*height) << '\n';
    check_build_site_pointer(builder, mex);
    clear_local_selection();
    if (saved_selection != 0)
        adopt_selection(saved_selection);
    selected_match_unit_ = saved_selection;
    match_command_ = saved_command;
    pending_build_type_ = saved_type;
}

void Runtime::place_pending_build(float x, float y) {
    if (!match_ || !selected_tnt_ || pending_build_type_ == 0 || selected_match_unit_ == 0)
        return;
    if (radar_contains(x, y)) {
        std::ignore = place_pending_build_on_radar(x, y);
        return;
    }
    // The profile's click snap may move the building onto metal or a vent.
    if (const auto snapped = snapped_build_site(x, y)) {
        place_pending_build_at(snapped->world);
        return;
    }
    if (const auto site = build_site_under(x, y))
        place_pending_build_at(site->world);
}

bool Runtime::place_pending_build_on_radar(float x, float y) {
    namespace input = oa::sim::gameplay_input;
    if (!match_ || pending_build_type_ == 0)
        return false;
    const auto& game = match_->state().game;
    if ((input::pointer_flags(game) & input::pointer_build_site_clear) == 0) {
        // The battlefield's last site was refused; build mode stays.
        play_match_interface_sound("notoktobuild");
        return false;
    }
    const auto ground = radar_world_point(x, y);
    const auto site = ground ? pending_build_site(*ground) : std::nullopt;
    if (!site)
        return false;
    // The point snapped to the footprint, at the height of the battlefield's
    // last site.
    auto point = site->world;
    point[1] = static_cast<int32_t>(static_cast<uint32_t>(game.drag_start[1]) << 16);
    issue_pending_build(point, queueing());
    return true;
}

void Runtime::note_build_site_under_pointer() {
    namespace input = oa::sim::gameplay_input;
    if (!match_)
        return;
    auto& game = match_->state().game;
    auto flags =
        static_cast<uint8_t>(input::pointer_flags(game) & ~input::pointer_build_site_clear);
    if (const auto site = build_site_under(pointer_x_, pointer_y_)) {
        if (site->legal)
            flags |= input::pointer_build_site_clear;
        const int32_t height = site->world[1] >> 16;
        const int32_t left = site->cell_x * OA_MAP_CELL_PIXELS;
        const int32_t top = site->cell_z * OA_MAP_CELL_PIXELS;
        game.drag_start[0] = left;
        game.drag_start[1] = height;
        game.drag_start[2] = top;
        game.drag_end[0] = left + site->footprint_x * OA_MAP_CELL_PIXELS;
        game.drag_end[1] = height;
        game.drag_end[2] = top + site->footprint_z * OA_MAP_CELL_PIXELS;
    }
    input::set_pointer_flags(game, flags);
}

std::optional<oa::sim::ground_orders::Point>
Runtime::feature_reclaim_point(const oa::sim::ground_orders::Point& ground) const {
    const auto& world = match_->state();
    int16_t origin_x = 0;
    int16_t origin_z = 0;
    const auto word = match_->feature_word_under(ground, &origin_x, &origin_z);
    if (word >= world.feature_def_count ||
        (world.feature_defs[word].flags & OA_FEATURE_FLAG_RECLAIMABLE) == 0)
        return std::nullopt;
    // On the ground where the feature stands, so that the order's marker and
    // path end on the feature; height 0 where the map has no ground under it.
    const auto centre = oa::sim::feature_runtime::feature_center(
        world, origin_x, origin_z, world.feature_defs[word]
    );
    const oa_fixed height = centre.y;
    return oa::sim::ground_orders::Point{centre.x, std::max<oa_fixed>(height, 0), centre.z};
}

bool Runtime::try_reclaim_feature_at(float x, float y) {
    if (!match_ || selected_match_unit_ == 0)
        return false;
    const auto ground = match_world_point(x, y);
    if (!ground)
        return false;
    const auto destination = feature_reclaim_point(*ground);
    if (!destination)
        return false;
    try {
        for_each_selected([&](uint16_t id) {
            if (!cancels_queued_command(
                    id, oa::sim::gameplay_input::OrderCommand::reclaim, 0, destination, queueing()
                ))
                match_->issue_feature_reclaim(id, *destination, queueing());
        });
    } catch (const std::exception& error) {
        status_ = std::string("reclaim command: ") + error.what();
        return true;
    }
    finish_issued_command();
    status_ = "Reclaim";
    return true;
}

const oa::sim::map_runtime::NamedFeature*
Runtime::find_catalog_feature(std::string_view name) const {
    for (const auto& feature : feature_catalog_) {
        if (feature.name.size() != name.size())
            continue;
        bool same = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
            auto a = static_cast<unsigned char>(feature.name[i]);
            auto b = static_cast<unsigned char>(name[i]);
            if (a >= 'A' && a <= 'Z')
                a = static_cast<unsigned char>(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z')
                b = static_cast<unsigned char>(b + ('a' - 'A'));
            if (a != b) {
                same = false;
                break;
            }
        }
        if (same)
            return &feature;
    }
    return nullptr;
}

std::size_t Runtime::intern_gaf_feature_anim(
    const std::string& filename,
    const std::string& seqname,
    const oa::formats::gaf::Sequence& sequence,
    bool animating
) {
    const auto key = filename + "/" + seqname;
    if (const auto found = match_gaf_anim_index_.find(key); found != match_gaf_anim_index_.end())
        return found->second;
    MatchGafFeatureAnim anim;
    anim.loop = sequence.repeat_flags != 0;
    anim.animating = animating && sequence.frames.size() > 1;
    for (const auto& source : sequence.frames) {
        const auto rendered = oa::formats::gaf::render_normal(source);
        if (!rendered.ok())
            continue;
        anim.frames.push_back(std::move(*rendered.frame));
        anim.durations.push_back(source.duration);
    }
    if (anim.frames.empty())
        return static_cast<std::size_t>(-1);
    if (!anim.durations.empty())
        anim.remaining = anim.durations.front();
    const auto index = match_gaf_anims_.size();
    match_gaf_anims_.push_back(std::move(anim));
    match_gaf_anim_index_.emplace(key, index);
    return index;
}

std::size_t Runtime::intern_feature_shadow_anim(
    uint16_t feature_index, const std::string& filename, bool animating
) {
    constexpr auto none = static_cast<std::size_t>(-1);
    if (feature_index >= feature_table_.defs.size())
        return none;
    const auto ref = feature_table_.defs[feature_index].seq_name_shadow;
    if (ref == 0 || ref > feature_assets_.sequences.size() ||
        feature_assets_.sequences[ref - 1] == nullptr)
        return none;
    const auto& sequence = *feature_assets_.sequences[ref - 1];
    if (sequence.frames.empty())
        return none;
    const auto key = filename + "/" + sequence.name;
    if (const auto found = match_gaf_anim_index_.find(key); found != match_gaf_anim_index_.end())
        return found->second;
    const auto decoded = decode_feature_sequence(ref);
    if (!decoded)
        return none;
    return intern_gaf_feature_anim(filename, sequence.name, *decoded, animating);
}

void Runtime::advance_gaf_feature_anims(uint32_t tick) {
    while (gaf_feature_anim_tick_ < tick) {
        for (auto& anim : match_gaf_anims_) {
            if (!anim.animating)
                continue;
            anim.animating = oa::sim::map_runtime::step_feature_animation(
                anim.frame, anim.remaining, anim.loop, anim.durations
            );
        }
        ++gaf_feature_anim_tick_;
    }
}

void Runtime::sync_dead_feature_draws() {
    if (!match_)
        return;
    auto& world = match_->state();

    struct ChangedFeature {
        int32_t cell_x{};
        int32_t cell_z{};
        uint16_t drawn = oa::sim::feature_runtime::no_feature;
        uint16_t live = oa::sim::feature_runtime::no_feature;
    };

    std::vector<ChangedFeature> changed;
    auto consider = [&](int32_t cell_x, int32_t cell_z, uint16_t drawn) {
        const auto* plot = oa::world_plot(&world, cell_x, cell_z);
        if (drawn == oa::sim::feature_runtime::no_feature || plot == nullptr ||
            plot->feature == drawn)
            return;
        for (const auto& prior : changed)
            if (prior.cell_x == cell_x && prior.cell_z == cell_z)
                return;
        changed.push_back({cell_x, cell_z, drawn, plot->feature});
    };
    for (const auto& feature : match_features_)
        consider(feature.cell_x, feature.cell_z, feature.feature_index);
    for (const auto& feature : match_gaf_features_)
        consider(feature.cell_x, feature.cell_z, feature.feature_index);
    for (const auto& plot : changed) {
        const auto drop = [&](auto& draws) {
            std::erase_if(draws, [&](const auto& feature) {
                return feature.cell_x == plot.cell_x && feature.cell_z == plot.cell_z &&
                       feature.feature_index == plot.drawn;
            });
        };
        drop(match_features_);
        drop(match_gaf_features_);
        if (plot.live < OA_PLOT_FEATURE_RESERVED)
            place_catalog_feature_draw(plot.cell_x, plot.cell_z, plot.live);
    }
}

void Runtime::place_catalog_feature_draw(int32_t cell_x, int32_t cell_z, uint16_t feature_index) {
    if (!match_)
        return;
    auto& world = match_->state();
    const auto* plot = oa::world_plot(&world, cell_x, cell_z);
    if (plot == nullptr || feature_index >= world.feature_def_count)
        return;
    const auto& def = world.feature_defs[feature_index];
    const auto world_x = (cell_x * 16 + static_cast<int32_t>(def.footprint_x) * 8) << 16;
    const auto world_z = (cell_z * 16 + static_cast<int32_t>(def.footprint_z) * 8) << 16;
    oa::formats::objects3d::FixedVector3 position{
        world_x, static_cast<int32_t>(plot->height) << 16, world_z
    };
    if ((def.flags & OA_FEATURE_FLAG_SPRITE) == 0) {
        // An object feature's FeatureDef.animation_file holds its 3DO ref (feature_assets_).
        oa_ref32 object = 0;
        std::memcpy(&object, def.animation_file, sizeof object);
        if (object == 0 || object > feature_assets_.models.size())
            return;
        MatchFeatureDraw draw;
        if ((plot->flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0)
            if (const auto* record =
                    oa::sim::feature_runtime::feature_record(world, plot->feature_record)) {
                position = {
                    record->model.position.x, record->model.position.y, record->model.position.z
                };
                draw.rotation = {
                    record->orientation[0], record->orientation[1], record->orientation[2]
                };
            }
        draw.instance = oa::sim::model_runtime::make_instance(feature_assets_.models[object - 1]);
        draw.instance.rebuild_transforms(draw.rotation);
        draw.position = position;
        draw.cell_x = cell_x;
        draw.cell_z = cell_z;
        draw.feature_index = feature_index;
        match_features_.push_back(std::move(draw));
        return;
    }
    const auto* feature = find_catalog_feature({def.name, ::strnlen(def.name, sizeof def.name)});
    if (feature == nullptr)
        return;
    const auto& terrain = feature->terrain;
    if (terrain.filename.empty() || terrain.seqname.empty())
        return;
    try {
        // A sequence already interned is not read again.
        const auto interned = match_gaf_anim_index_.find(terrain.filename + "/" + terrain.seqname);
        auto anim = interned != match_gaf_anim_index_.end() ? interned->second
                                                            : static_cast<std::size_t>(-1);
        if (interned == match_gaf_anim_index_.end()) {
            // Only the sequence shown is decoded with its pixels.
            std::string parse_error;
            const auto gaf = feature_gaf_file(terrain.filename, parse_error);
            if (gaf.archive == nullptr)
                return;
            const auto* sequence = gaf_sequence(*gaf.archive, terrain.seqname);
            if (sequence == nullptr || sequence->frames.empty())
                return;
            const auto decoded = oa::formats::gaf::parse_sequence(
                gaf.file, static_cast<std::size_t>(sequence - gaf.archive->sequences.data())
            );
            if (!decoded.ok())
                return;
            anim = intern_gaf_feature_anim(
                terrain.filename, terrain.seqname, *decoded.sequence, terrain.animating
            );
        }
        if (anim == static_cast<std::size_t>(-1))
            return;
        match_gaf_features_.push_back(
            {anim,
             position,
             cell_x,
             cell_z,
             feature_index,
             intern_feature_shadow_anim(feature_index, terrain.filename, terrain.animating)}
        );
    } catch (const std::exception& error) {
        std::cerr << "feature sprite '" << terrain.filename << "/" << terrain.seqname
                  << "' unavailable: " << error.what() << '\n';
    }
}

void Runtime::place_match_wreck(const oa::sim::match_runtime::Match::Wreck& wreck) {
    place_catalog_feature_draw(wreck.cell_x, wreck.cell_z, wreck.feature);
}

void Runtime::sync_match_wrecks() {
    if (!match_)
        return;
    const auto wrecks = match_->wrecks();
    while (wrecks_drawn_ < wrecks.size()) {
        place_match_wreck(wrecks[wrecks_drawn_]);
        ++wrecks_drawn_;
    }
}

void Runtime::rebuild_feature_draws() {
    if (!match_)
        return;
    match_features_.clear();
    match_gaf_features_.clear();
    const auto& world = match_->state();
    const auto width = world.game.map_width;
    const auto cells =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(world.game.map_height);
    for (std::size_t index = 0; index < cells && width > 0; ++index) {
        const auto feature = world.plots[index].feature;
        if (feature < OA_PLOT_FEATURE_RESERVED)
            place_catalog_feature_draw(
                static_cast<int32_t>(index % static_cast<std::size_t>(width)),
                static_cast<int32_t>(index / static_cast<std::size_t>(width)),
                feature
            );
    }
    wrecks_drawn_ = match_->wrecks().size();
}

} // namespace oa::app
