// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Pointer tracking, menu activation and match unit picking.
#include "oa/app/runtime.hpp"
#include "engine_settings_state.hpp"
#include "match_models.hpp"
#include "oa/app/far_view.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/sim/spatial_state/spatial.hpp"
#include "oa/sim/weapon_execution/retaliation.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/frontend_dialogs.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

/// Moves a pick candidate of another machine's player to where the frame
/// last drawn showed it, on its playout (mirrored_pose).
///
/// @param models the match's renderer state; null before the match is drawn
/// @param world the match's World
/// @param unit the unit
/// @param[in,out] candidate the candidate, at the unit's simulated place
void pick_where_drawn(
    const MatchModels* models,
    const World& world,
    const Unit& unit,
    oa::sim::gameplay_input::PickUnit& candidate
) {
    if (models == nullptr)
        return;
    const auto pose = mirrored_pose(
        *models, world, static_cast<uint16_t>(unit.id), models->presentation.drawn_moment
    );
    if (!pose)
        return;
    candidate.position = {pose->position.x, pose->position.y, pose->position.z};
    candidate.rotation = {pose->bank, static_cast<int16_t>(pose->heading), pose->pitch};
}

} // namespace

void Runtime::update_pointer(float x, float y) {
    pointer_x_ = x;
    pointer_y_ = y;
    if (screen_ == Screen::match) {
        match_pointer_x_ = x;
        match_pointer_y_ = y;
        hovered_.reset();
        const auto hud_point = hud_source_point(x, y);
        // The HUD of a finished match takes no pointer.
        if (!match_finished_ && match_hud_) {
            const auto& gadgets = match_hud_->layout.gadgets;
            const oa::ui::gui_input::MenuObject hud{gadgets, -1};
            // A grayed order page status button still takes the click and
            // ignores it. Build page navigation is never on a paused menu.
            const auto takes_pointer = [&](const oa::ui::gui_layout::Gadget& gadget) {
                return match_gadget_state(gadget) != nullptr ||
                       !((!pause_menu_shown() && is_build_page_nav(gadget.common.name) &&
                          !build_page_nav_shown()) ||
                         !gadget_command_available(gadget));
            };
            hovered_ = oa::ui::gui_input::hit_test(hud, hud_point.x, hud_point.y);
            if (hovered_ && *hovered_ < gadgets.size() && !takes_pointer(gadgets[*hovered_])) {
                hovered_.reset();
                // A page may place two buttons in one spot, LOAD and BLAST,
                // of which a unit has one: the pointer takes the one shown.
                for (auto index = gadgets.size(); index-- > 1;) {
                    const auto& gadget = gadgets[index];
                    const auto rectangle = oa::ui::gui_input::gadget_geometry(gadgets, index);
                    if (gadget.common.active != 0 &&
                        gadget.common.type == oa::ui::gui_layout::GadgetType::button && rectangle &&
                        rectangle->contains(hud_point.x, hud_point.y) && takes_pointer(gadget)) {
                        hovered_ = index;
                        break;
                    }
                }
            }
        }
        pick_cursor_unit();
        build_tool_motion(x, y);
        if (options_.trace_input)
            std::cerr << "input match pointer x=" << x << " y=" << y
                      << " hud=" << (hovered_ ? std::to_string(*hovered_) : "none") << '\n';
        return;
    }
    if (screen_ == Screen::map_selection) {
        const auto& modal_root = resources_.layout.gadgets.front().common;
        x -= static_cast<float>((kCanvasWidth - static_cast<int>(modal_root.width)) / 2);
        y -= static_cast<float>((kCanvasHeight - static_cast<int>(modal_root.height)) / 2);
    }
    const auto origin = panel_origin();
    x -= static_cast<float>(origin.x);
    y -= static_cast<float>(origin.y);
    // The gadget hover's preselected path deliberately stops after its first
    // candidate. Hover discovery comes before selection, so this hit test runs
    // with no selection (-1) even while the button is down.
    const oa::ui::gui_input::MenuObject menu_object{resources_.layout.gadgets, -1};
    const auto previous_hover = hovered_;
    hovered_ =
        oa::ui::gui_input::hit_test(menu_object, static_cast<int32_t>(x), static_cast<int32_t>(y));
    if (hovered_ != previous_hover)
        refresh_help_text();
    if (options_.trace_input) {
        std::cerr << "input pointer screen=" << static_cast<int>(screen_) << " x=" << x
                  << " y=" << y << " hover=";
        if (hovered_)
            std::cerr << *hovered_ << ':' << resources_.layout.gadgets[*hovered_].common.name;
        else
            std::cerr << "none";
        std::cerr << " selected=" << selected_ << '\n';
    }
}

void Runtime::activate() {
    if (options_.trace_input)
        std::cerr << "input activate before state=" << static_cast<int>(state_.state)
                  << " signal=" << static_cast<int>(state_.signal)
                  << " pending=" << static_cast<int>(state_.pending_signal)
                  << " selected=" << selected_ << '\n';
    const menu::Event event{{kFrontendMenuHandle}, 0};
    // These screens' handlers read the layout itself, so the release steps a
    // staged button here first, as the panel pointer handling does.
    if (screen_ == Screen::main_menu || screen_ == Screen::single_player ||
        screen_ == Screen::skirmish || screen_ == Screen::map_selection)
        step_released_button_stage();
    if (screen_ == Screen::main_menu) {
        // MULTI is the extensions' to answer (network play's, unless another
        // takes it first): one may take the game over, or let the menu step
        // into the multiplayer states. Without an answer the main menu stays
        // up and MULTI does nothing.
        if (button_result(menu::MenuHandle{kFrontendMenuHandle}, menu::Button::multiplayer) != 0 &&
            eligible_map_names_.empty()) {
            // Game data with no multiplayer map offers no multiplayer at all.
            play_menu_sound(menu::Sound::big_button);
            show_missing_content(MissingContent::multiplayer_maps);
        } else if (
            button_result(menu::MenuHandle{kFrontendMenuHandle}, menu::Button::multiplayer) != 0 &&
            refuse_incomplete_mod_start()
        ) {
            // A mod that cannot start a game says so in place of multiplayer.
            play_menu_sound(menu::Sound::big_button);
        } else if (
            button_result(menu::MenuHandle{kFrontendMenuHandle}, menu::Button::multiplayer) != 0
        ) {
            const MultiplayerSelection selection =
                call_hook_or_raise<&Extension::select_multiplayer>(extension_, *this);
            if (selection == MultiplayerSelection::frontend)
                menu::handle_event(state_, event, *this);
        } else {
            menu::handle_event(state_, event, *this);
        }
    } else if (
        screen_ == Screen::single_player &&
        button_result(entry::MenuHandle{kFrontendMenuHandle}, entry::Button::skirmish) != 0 &&
        eligible_map_names_.empty()
    ) {
        // Game data with no skirmish map says so in place of the skirmish setup.
        show_missing_content(MissingContent::skirmish_maps);
        selected_ = -1;
        return;
    } else if (screen_ == Screen::single_player)
        entry::handle_single_player_event(state_, event, *this);
    else if (
        screen_ == Screen::skirmish &&
        button_result(skirmish::MenuHandle{kFrontendMenuHandle}, skirmish::Button::start) != 0 &&
        refuse_incomplete_mod_start()
    ) {
        // A mod that cannot start a game says so, and the setup stays.
        selected_ = -1;
        return;
    } else if (screen_ == Screen::skirmish) {
        const bool started = start_skirmish_from_setup(event);
        if (started && match_ && !altitude_sight_blocked_)
            enter_match_view();
    } else if (screen_ == Screen::map_selection) {
        map_modal::handle_event(map_modal_, skirmish_settings_, event, *this);
    } else if (
        screen_ == Screen::options || screen_ == Screen::sound || screen_ == Screen::visuals ||
        screen_ == Screen::speeds || screen_ == Screen::music
    ) {
        activate_options_gadget();
        return;
    } else if (screen_ == Screen::new_campaign || screen_ == Screen::any_mission) {
        activate_campaign_gadget();
        return;
    } else if (screen_ == Screen::load_game) {
        activate_load_game_gadget();
        return;
    } else if (screen_ == Screen::campaign_end) {
        activate_campaign_end_gadget();
        return;
    } else if (screen_ == Screen::briefing) {
        activate_briefing_gadget();
        return;
    } else {
        return;
    }
    if (screen_ == Screen::match)
        return;
    // The frontend mode tick runs its unit header step before
    // each dispatcher pass.
    step(frontend::Step::reload_unit_overrides, state_);
    frontend::dispatch(state_, *this, frontend_states_);
    // A menu callback first selects the next dispatcher state; its
    // initialize signal then constructs that screen on the following
    // dispatcher pass.
    // Dispatching unconditionally every rendered frame would repeatedly
    // rebuild MAINMENU while its signal remains initialize, clearing a
    // mouse-down selection before the matching mouse-up arrives.
    step(frontend::Step::reload_unit_overrides, state_);
    frontend::dispatch(state_, *this, frontend_states_);
    if (options_.trace_input)
        std::cerr << "input activate after state=" << static_cast<int>(state_.state)
                  << " signal=" << static_cast<int>(state_.signal)
                  << " pending=" << static_cast<int>(state_.pending_signal)
                  << " screen=" << static_cast<int>(screen_) << '\n';
}

void Runtime::step_released_button_stage() {
    if (selected_ < 0 || static_cast<std::size_t>(selected_) >= resources_.layout.gadgets.size())
        return;
    const auto& gadget = resources_.layout.gadgets[static_cast<std::size_t>(selected_)];
    const auto found = widget_text_stages_.find(gadget.common.name);
    const auto stage =
        static_cast<uint8_t>(found == widget_text_stages_.end() ? 0U : found->second);
    const auto next = oa::ui::gui_input::released_button_stage(gadget, stage);
    if (next != stage)
        set_button_stage(gadget.common.name, next);
}

oa::ui::display_layout::Point Runtime::game_screen_point(float x, float y) const {
    namespace layout = oa::ui::display_layout;
    const auto column = static_cast<int>(x);
    const auto row = static_cast<int>(y);
    if (screen_ != Screen::match || !battlefield_contains(x, y))
        return layout::canvas_to_source(match_layout_, column, row);
    const auto zoom = match_zoom() == 0.0F ? 1.0 : static_cast<double>(match_zoom());
    // As the battlefield maps a canvas pixel to its map pixel
    // (world_renderer::screen_to_map_pixel), from the view as it is drawn:
    // between map pixels in the accelerated tier, and moved by the card's
    // shift (view_shift), so that the pointer picks what is drawn under
    // it. The offset never carries a point past the
    // last map pixel of Game's battlefield (Game.battlefield_rect), where
    // hover and picking would refuse it, nor before its first, which a
    // shift back can reach; a point the view on its camera's map pixel maps
    // past it maps as it always has.
    const auto mapped = [zoom](int32_t from_edge, double offset, int32_t visible) {
        const double on_camera = static_cast<double>(from_edge) / zoom;
        const auto whole = std::llround(on_camera);
        const auto drawn = std::llround(on_camera + offset);
        return static_cast<int>(
            std::clamp(drawn, 0LL, std::max(whole, static_cast<long long>(visible) - 1))
        );
    };
    const auto offset = view_offset();
    const auto shift = view_shift();
    return {
        layout::kSourceLeft +
            mapped(column - match_layout_.left, offset.x - shift[0] / zoom, visible_map_width()),
        layout::kSourceTop +
            mapped(row - match_layout_.top, offset.y - shift[1] / zoom, visible_map_height())
    };
}

oa::ui::display_layout::Point Runtime::game_screen_canvas(int32_t x, int32_t y) const {
    namespace layout = oa::ui::display_layout;
    const bool in_view = x >= layout::kSourceLeft && y >= layout::kSourceTop &&
                         x < layout::kSourceLeft + visible_map_width() &&
                         y < layout::kSourceTop + visible_map_height();
    if (screen_ != Screen::match || !in_view)
        return layout::source_to_canvas(match_layout_, x, y);
    const auto zoom = match_zoom() == 0.0F ? 1.0 : static_cast<double>(match_zoom());
    // Where the view as it is drawn, and moved by the card, shows the point.
    const auto offset = view_offset();
    const auto shift = view_shift();
    return {
        match_layout_.left +
            static_cast<int>(std::lround(
                (static_cast<double>(x - layout::kSourceLeft) - offset.x) * zoom + shift[0]
            )),
        match_layout_.top +
            static_cast<int>(std::lround(
                (static_cast<double>(y - layout::kSourceTop) - offset.y) * zoom + shift[1]
            ))
    };
}

oa::sim::selection::VisibleLists Runtime::on_screen_lists() {
    oa::sim::selection::VisibleLists lists{};
    lists.units = on_screen_units_.data();
    lists.unit_capacity = static_cast<uint32_t>(on_screen_units_.size());
    lists.radar = radar_state_.hot_units.data();
    lists.radar_capacity = static_cast<uint32_t>(radar_state_.hot_units.size());
    // The radar renderer places its picture in Game and returns the blips it
    // listed; selection takes both from here rather than reading them itself.
    if (match_) {
        lists.radar_count = static_cast<int32_t>(radar_state_.hot_unit_count);
        lists.radar_picture = match_->state().game.radar_picture_rect;
    }
    return lists;
}

oa::sim::selection::Hooks Runtime::selection_hooks() {
    oa::sim::selection::Hooks hooks{};
    hooks.context = this;
    hooks.player_sees_unit =
        [](void* context, const World&, const Player& player, const Unit& unit) {
            auto& self = *static_cast<Runtime*>(context);
            try {
                return self.match_ != nullptr && self.match_->unit_shown(player.index, unit.id);
            } catch (const std::exception&) {
                return false;
            }
        };
    hooks.pointer_hits_unit =
        [](void* context, const World& world, const Unit& unit, int32_t x, int32_t y) {
            auto& self = *static_cast<Runtime*>(context);
            auto* instance = self.match_ != nullptr ? self.match_->instance(unit.id) : nullptr;
            if (instance == nullptr)
                return false;
            oa::sim::gameplay_input::PickUnit candidate;
            candidate.id = unit.id;
            candidate.position = {unit.position.x, unit.position.y, unit.position.z};
            candidate.rotation = {unit.bank, static_cast<int16_t>(unit.heading), unit.pitch};
            pick_where_drawn(self.drawn_match_models(), world, unit, candidate);
            // A unit drawn as a dot is picked wherever its dot is drawn, and
            // one drawn as a model in the far view wherever its dot would
            // be, since its box there shrinks to a pixel or less; a carried
            // unit shows in its carrier and has none of its own.
            if ((self.dots_frame() || self.far_view_frame()) && unit.attach_parent == 0) {
                const auto dot = self.project_match_point(
                    self.live_viewport(
                        static_cast<int32_t>(world.game.camera_x),
                        static_cast<int32_t>(world.game.camera_y)
                    ),
                    {std::bit_cast<uint32_t>(candidate.position.x),
                     std::bit_cast<uint32_t>(candidate.position.y),
                     std::bit_cast<uint32_t>(candidate.position.z)}
                );
                const auto pointer = self.game_screen_canvas(x, y);
                if (far_view_dot_covers(dot.x, dot.y, pointer.x, pointer.y))
                    return true;
            }
            candidate.model = &instance->model().model();
            // A unit without a root object has no box to pick.
            if (candidate.model->objects.empty())
                return false;
            const oa::sim::gameplay_input::Camera camera{
                static_cast<int32_t>(world.game.camera_x), static_cast<int32_t>(world.game.camera_y)
            };
            return oa::sim::gameplay_input::hits_root_bounds(candidate, camera, {x, y});
        };
    hooks.play_sound = [](void* context, const char* name) {
        static_cast<Runtime*>(context)->play_match_interface_sound(name);
    };
    // A unit selected alone speaks its selection line to its owner.
    hooks.speak = [](void* context, const Unit& unit, uint32_t category) {
        auto& self = *static_cast<Runtime*>(context);
        if (self.match_ == nullptr || unit.id >= self.match_->world().slots.size())
            return;
        auto& slot = self.match_->world().slots[unit.id];
        if (slot.unit == nullptr)
            return;
        try {
            self.offline_services_.command_sound(slot, category);
        } catch (const std::exception&) {
            // Without the announcement runtime, or for a type outside its
            // catalog, the selection stays silent.
        }
    };
    hooks.reset_command = [](void* context) {
        auto& self = *static_cast<Runtime*>(context);
        self.reset_match_command();
        self.pending_build_type_ = 0;
    };
    hooks.selection_cleared = [](void* context) {
        static_cast<Runtime*>(context)->selected_match_unit_ = 0;
    };
    hooks.center_camera = [](void* context, const FixedVec3& position, bool) {
        auto& self = *static_cast<Runtime*>(context);
        self.set_camera_position(
            static_cast<int32_t>(static_cast<int16_t>(static_cast<uint32_t>(position.x) >> 16)) -
                self.visible_map_width() / 2,
            static_cast<int32_t>(static_cast<int16_t>(static_cast<uint32_t>(position.z) >> 16)) -
                self.visible_map_height() / 2,
            0
        );
        self.render_match_surface();
    };
    hooks.stop_follow = [](void* context) {
        static_cast<Runtime*>(context)->stop_match_tracking();
    };
    return hooks;
}

const MatchModels* Runtime::drawn_match_models() const {
    return match_models_ && match_models_->match == match_.get() ? match_models_.get() : nullptr;
}

void Runtime::rebuild_on_screen_units() {
    if (!match_)
        return;
    auto& world = match_->state();
    if (on_screen_units_.size() != world.unit_slot_count) {
        // A new match: nothing is under the cursor yet.
        on_screen_units_.assign(world.unit_slot_count, 0);
        world.game.cursor_unit_id = 0;
        world.game.cursor_feature = oa::sim::spatial_state::no_feature;
    }
    offline_services_.set_on_screen_test(
        [](const void* context, uint16_t unit) {
            auto& self = *static_cast<Runtime*>(const_cast<void*>(context));
            return self.match_ != nullptr && oa::sim::selection::unit_listed(
                                                 self.match_->state(), self.on_screen_lists(), unit
                                             );
        },
        this
    );
    if (live_renderer_active()) {
        int32_t count{};
        for (const auto& slot : match_->world().slots) {
            if (!slot.unit || slot.unit_index == 0)
                continue;
            const auto bounds = live_unit_bounds(slot.unit_index);
            if (!bounds || bounds->x2 < match_layout_.left || bounds->y2 < match_layout_.top ||
                bounds->x1 >= match_layout_.left + match_layout_.battlefield_width() ||
                bounds->y1 >= match_layout_.top + match_layout_.battlefield_height())
                continue;
            if (static_cast<std::size_t>(count) < on_screen_units_.size())
                on_screen_units_[static_cast<std::size_t>(count++)] = slot.unit_index;
        }
        world.game.hot_unit_count = count;
    } else
        oa::sim::selection::collect_visible_units(world, on_screen_lists(), selection_hooks());
}

void Runtime::refresh_on_screen_view() {
    if (!match_ || !selected_tnt_)
        return;
    // The view a frame would show: the camera held within the view's
    // limits, bound to the Game block, and the units on screen in it.
    const auto camera = view_camera();
    match_camera_x_ = camera[0];
    match_camera_z_ = camera[1];
    bind_match_view();
    rebuild_on_screen_units();
}

void Runtime::pick_cursor_unit(bool refresh_view) {
    namespace input = oa::sim::gameplay_input;
    if (!match_ || screen_ != Screen::match)
        return;
    if (refresh_view)
        refresh_on_screen_view();
    auto& game = match_->state().game;
    const auto point = game_screen_point(pointer_x_, pointer_y_);
    game.pointer_state[0] = static_cast<uint32_t>(point.x);
    game.pointer_state[1] = static_cast<uint32_t>(point.y);
    refresh_pointer_area();
    // The ground under the pointer, its cell and the feature on it, which
    // the unit panel shows when no unit is under the cursor.
    if (const auto ground = match_world_point(pointer_x_, pointer_y_)) {
        input::set_pointer_position(game, {(*ground)[0], (*ground)[1], (*ground)[2]});
        store_cursor_cell(*ground);
    }
    const auto flags = input::pointer_flags(game);
    // Placing a building tests its site over the view instead; off the view
    // and the radar the unit picked last stays.
    const bool placing =
        (flags & input::pointer_over_view) != 0 && match_command_ == MatchCommand::build;
    if (placing)
        note_build_site_under_pointer();
    if (!placing && live_renderer_active() && battlefield_contains(pointer_x_, pointer_y_))
        game.cursor_unit_id = live_unit_under(pointer_x_, pointer_y_);
    else if (
        !placing && (flags & (input::pointer_over_view | input::pointer_over_radar)) != 0 &&
        on_screen_units_.size() == match_->state().unit_slot_count
    )
        game.cursor_unit_id = oa::sim::selection::unit_under_pointer(
            match_->state(), on_screen_lists(), selection_hooks()
        );
    hovered_match_unit_ = game.cursor_unit_id;
}

oa::sim::gameplay_input::OrderCursorHooks Runtime::order_cursor_hooks() {
    oa::sim::gameplay_input::OrderCursorHooks hooks{
        this,
        // A feature counts on mapped ground under the fog too, as 3.1c does.
        [](void* context, const World&, const FixedVec3& position) {
            const auto* self = static_cast<Runtime*>(context);
            return self->match_->point_mapped(
                {static_cast<uint32_t>(position.x),
                 static_cast<uint32_t>(position.y),
                 static_cast<uint32_t>(position.z)}
            );
        },
        // A feature is found where it is drawn: a wreck sunk to the sea bed
        // where it lies below the sea the pointer meets.
        [](void*, const World& world, const FixedVec3& position) -> const oa::FeatureDef* {
            const auto drawn = oa::sim::gameplay_input::drawn_feature(world, position);
            return drawn ? drawn->def : nullptr;
        },
        [](void* context, const World&, const Unit& actor, const Unit& target) {
            try {
                return static_cast<Runtime*>(context)->match_->weapon_can_reach(
                    actor.id, target.id, 0
                );
            } catch (const std::exception&) {
                return false;
            }
        },
        [](void* context, const World& world, const Unit& actor, const FixedVec3& position) {
            const auto* self = static_cast<Runtime*>(context);
            return oa::sim::weapon_execution::slot_reaches_point(
                world,
                actor,
                position,
                0,
                self->match_ ? self->match_->rules_view() : oa::data::match_rules::MatchRulesView{}
            );
        },
    };
    if (match_)
        hooks.rules = match_->rules_view();
    const auto& fixes = ui_rules().interface_fixes;
    hooks.pad_load_cursor =
        fixes.enabled &&
        fixes.fixes.contains(oa::data::mod_profile::UiInterfaceFixesFixes::pad_cursor);
    return hooks;
}

std::string_view Runtime::match_hud_action(std::string_view name) const {
    // A page's controls carry the prefix of any side SIDEDATA lists: ARMMOVE,
    // GOKCLOAK, TALONOFF.
    const auto starts_with = [name](std::string_view prefix) {
        return !prefix.empty() && name.size() > prefix.size() &&
               std::equal(prefix.begin(), prefix.end(), name.begin(), [](char a, char b) {
                   return std::toupper(static_cast<unsigned char>(a)) ==
                          std::toupper(static_cast<unsigned char>(b));
               });
    };
    for (std::size_t side = 0; side < side_table_.count; ++side) {
        const auto& field = side_table_.sides[side].name_prefix;
        const std::string_view prefix(field, strnlen(field, sizeof field));
        if (starts_with(prefix))
            return name.substr(prefix.size());
    }
    if (starts_with("ARM") || starts_with("COR"))
        return name.substr(3);
    return name;
}

void Runtime::note_sticky_motion(
    float x, float y, float motion_x, float motion_y, uint64_t time_ms
) {
    if (sdl_.window == nullptr)
        return;
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSize(sdl_.window, &width, &height))
        return;
    float past_x = 0.0F;
    float past_y = 0.0F;
    // The desktop place is the push, and only when it has moved. The
    // sample's own movement is the approach, which is the flick.
    if (sticky_trust_global() && sticky_global_moved())
        sticky_global_past(past_x, past_y);
    std::ignore = sticky_edge(
        sticky_edge_,
        StickySample{x, y, motion_x, motion_y, time_ms, past_x, past_y},
        static_cast<float>(width),
        static_cast<float>(height),
        sticky_edge_limits()
    );
    restore_sticky_pointer(x, y, true);
}

bool Runtime::sticky_soft_wall() const {
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver == nullptr || std::string_view(driver) != "wayland";
}

bool Runtime::sticky_trust_global() const {
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver == nullptr)
        return false;
    const std::string_view driver_name{driver};
    return driver_name == "cocoa" || driver_name == "windows" || driver_name == "x11";
}

bool Runtime::sticky_global_moved() {
    if (!sticky_trust_global() || sdl_.window == nullptr)
        return false;
    float place_x = 0.0F;
    float place_y = 0.0F;
    std::ignore = SDL_GetGlobalMouseState(&place_x, &place_y);
    const bool moved =
        !sticky_global_known_ || place_x != sticky_global_x_ || place_y != sticky_global_y_;
    sticky_global_known_ = true;
    sticky_global_x_ = place_x;
    sticky_global_y_ = place_y;
    return moved;
}

void Runtime::sticky_global_past(float& past_x, float& past_y) const {
    past_x = 0.0F;
    past_y = 0.0F;
    if (!sticky_trust_global() || sdl_.window == nullptr)
        return;
    int origin_x = 0;
    int origin_y = 0;
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowPosition(sdl_.window, &origin_x, &origin_y) ||
        !SDL_GetWindowSize(sdl_.window, &width, &height) || width < 2 || height < 2)
        return;
    float place_x = 0.0F;
    float place_y = 0.0F;
    std::ignore = SDL_GetGlobalMouseState(&place_x, &place_y);
    const float edge_left = static_cast<float>(origin_x);
    const float edge_top = static_cast<float>(origin_y);
    const float edge_right = edge_left + static_cast<float>(width) - 1.0F;
    const float edge_bottom = edge_top + static_cast<float>(height) - 1.0F;
    if (place_x > edge_right)
        past_x = place_x - edge_right;
    else if (place_x < edge_left)
        past_x = place_x - edge_left;
    if (place_y > edge_bottom)
        past_y = place_y - edge_bottom;
    else if (place_y < edge_top)
        past_y = place_y - edge_top;
}

void Runtime::remember_sticky_global() {
    if (!sticky_trust_global() || sdl_.window == nullptr)
        return;
    float place_x = 0.0F;
    float place_y = 0.0F;
    std::ignore = SDL_GetGlobalMouseState(&place_x, &place_y);
    sticky_global_known_ = true;
    sticky_global_x_ = place_x;
    sticky_global_y_ = place_y;
}

void Runtime::drop_warp_motion(float x, float y) {
    SDL_Event pending[16];
    const int count =
        SDL_PeepEvents(pending, 16, SDL_GETEVENT, SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_MOTION);
    if (count <= 0)
        return;
    bool dropped = false;
    for (int index = 0; index < count; ++index) {
        SDL_Event& item = pending[index];
        const float dx = item.motion.x - x;
        const float dy = item.motion.y - y;
        const bool warp =
            item.motion.timestamp == 0 && dx > -0.5F && dx < 0.5F && dy > -0.5F && dy < 0.5F;
        if (!dropped && warp) {
            dropped = true;
            sticky_drop_warp_motion_ = false;
            continue;
        }
        std::ignore = SDL_PushEvent(&item);
    }
}

void Runtime::warp_pointer_to_edge(float x, float y) {
    if (sdl_.window == nullptr)
        return;
    sticky_drop_warp_motion_ = true;
    sticky_warp_x_ = x;
    sticky_warp_y_ = y;
    SDL_WarpMouseInWindow(sdl_.window, x, y);
    drop_warp_motion(x, y);
    // The warp's own motion is gone, and a leave forgot the place. The view
    // scrolls from the last pixel, which is where the pointer was put.
    pointer_x_ = x;
    pointer_y_ = y;
    if (screen_ == Screen::match) {
        match_pointer_x_ = x;
        match_pointer_y_ = y;
        match_pointer_known_ = true;
    }
}

bool Runtime::sticky_ignores_motion(const SDL_Event& event) {
    if (!sticky_drop_warp_motion_ || event.type != SDL_EVENT_MOUSE_MOTION ||
        event.motion.timestamp != 0)
        return false;
    const float dx = event.motion.x - sticky_warp_x_;
    const float dy = event.motion.y - sticky_warp_y_;
    if (!(dx > -0.5F && dx < 0.5F && dy > -0.5F && dy < 0.5F))
        return false;
    sticky_drop_warp_motion_ = false;
    return true;
}

void Runtime::restore_sticky_pointer(float x, float y, bool fed) {
    if (!sticky_soft_wall() || sdl_.window == nullptr || !sticky_edge_.holding)
        return;
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSize(sdl_.window, &width, &height) || width < 2 || height < 2)
        return;
    const float width_f = static_cast<float>(width);
    const float height_f = static_cast<float>(height);
    const bool outside = x < 0.0F || y < 0.0F || x >= width_f || y >= height_f;
    float past_x = 0.0F;
    float past_y = 0.0F;
    if (!fed && sticky_trust_global() && sticky_global_moved())
        sticky_global_past(past_x, past_y);
    // A sample the caller already fed keeps its push. This one is the
    // frame's, or the leave's, when the desktop pointer has gone on.
    if (!fed && ((past_x != 0.0F || past_y != 0.0F) || (!sticky_trust_global() && outside))) {
        std::ignore = sticky_edge(
            sticky_edge_,
            StickySample{x, y, 0.0F, 0.0F, static_cast<uint64_t>(SDL_GetTicks()), past_x, past_y},
            width_f,
            height_f,
            sticky_edge_limits()
        );
    }
    if (!sticky_edge_.holding)
        return;
    float raw_x = 0.0F;
    float raw_y = 0.0F;
    if (sticky_trust_global())
        sticky_global_past(raw_x, raw_y);
    if (raw_x == 0.0F && raw_y == 0.0F && !outside)
        return;
    float dest_x = std::clamp(x, 0.0F, width_f - 1.0F);
    float dest_y = std::clamp(y, 0.0F, height_f - 1.0F);
    if (raw_x > 0.0F || x >= width_f)
        dest_x = width_f - 1.0F;
    else if (raw_x < 0.0F || x < 0.0F)
        dest_x = 0.0F;
    if (raw_y > 0.0F || y >= height_f)
        dest_y = height_f - 1.0F;
    else if (raw_y < 0.0F || y < 0.0F)
        dest_y = 0.0F;
    warp_pointer_to_edge(dest_x, dest_y);
    remember_sticky_global();
}

oa::ui::engine_settings::WindowEdge Runtime::window_edge_mode() const {
    if (engine_settings_ == nullptr)
        return oa::ui::engine_settings::WindowEdge::capture;
    return engine_settings_->current.window_edge;
}

bool Runtime::battlefield_in_play() {
    if (screen_ != Screen::match || !match_ || match_finished_ || match_paused_)
        return false;
    if (engine_settings_dialog() != nullptr || oa::ui::frontend_dialogs::dialog_count() != 0)
        return false;
    return (match_->state().game.sim_run_flags & oa::ui::console::kSimRunPaused) == 0;
}

bool Runtime::window_edge_battlefield(bool require_mouse) {
    if (sdl_.window == nullptr || !battlefield_in_play())
        return false;
    const SDL_WindowFlags flags = SDL_GetWindowFlags(sdl_.window);
    // Full screen keeps its own grab. A window wants the rule with the
    // input focus, and the pointer inside when asked.
    SDL_WindowFlags mask = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_INPUT_FOCUS;
    SDL_WindowFlags need = SDL_WINDOW_INPUT_FOCUS;
    if (require_mouse) {
        mask |= SDL_WINDOW_MOUSE_FOCUS;
        need |= SDL_WINDOW_MOUSE_FOCUS;
    }
    return (flags & mask) == need;
}

bool Runtime::sticky_edge_wanted(bool require_mouse) {
    // The pointer may be past the edge while a soft wall puts it back, and
    // then the mouse focus has already gone.
    if (window_edge_mode() != oa::ui::engine_settings::WindowEdge::sticky)
        return false;
    if (engine_settings_ != nullptr && engine_settings_->current.edge_stickiness == 0)
        return false;
    return window_edge_battlefield(require_mouse);
}

StickyLimits Runtime::sticky_edge_limits() const {
    const uint32_t tenths = engine_settings_ != nullptr
                                ? engine_settings_->current.edge_stickiness
                                : oa::ui::engine_settings::default_edge_stickiness;
    return sticky_limits(
        static_cast<float>(tenths) /
        static_cast<float>(oa::ui::engine_settings::edge_stickiness_per_level)
    );
}

bool Runtime::capture_wanted(bool require_mouse) {
    return window_edge_mode() == oa::ui::engine_settings::WindowEdge::capture &&
           !capture_waits_for_click_ && window_edge_battlefield(require_mouse);
}

bool Runtime::take_window_edge_event(const SDL_Event& event, bool& force_release) {
    if (event.type == SDL_EVENT_MOUSE_MOTION && event.motion.which != SDL_TOUCH_MOUSEID &&
        !sticky_ignores_motion(event) && sticky_edge_wanted()) {
        const uint64_t time_ms = event.motion.timestamp != 0
                                     ? event.motion.timestamp / 1'000'000ULL
                                     : static_cast<uint64_t>(SDL_GetTicks());
        note_sticky_motion(
            event.motion.x, event.motion.y, event.motion.xrel, event.motion.yrel, time_ms
        );
        sticky_wanted_ = true;
    }
    const bool capture = window_edge_mode() == oa::ui::engine_settings::WindowEdge::capture;
    const bool windowed =
        sdl_.window != nullptr && (SDL_GetWindowFlags(sdl_.window) & SDL_WINDOW_FULLSCREEN) == 0;
    // When the event came, in milliseconds. A posted event has no time.
    const uint64_t event_ms = event.common.timestamp != 0 ? event.common.timestamp / 1'000'000ULL
                                                          : static_cast<uint64_t>(SDL_GetTicks());
    // The focus always lets the pointer go. On a soft wall a leave is the
    // pointer crossing the edge, and it is put back; Wayland's leave is the
    // confinement giving way, and a captured pointer's the grab giving way.
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        force_release = true;
        // A captured window waits for a click on the battlefield, and the
        // click that brings the focus back is only that.
        if (capture && windowed && match_) {
            capture_waits_for_click_ = true;
            capture_focus_click_ = true;
            capture_focus_gained_ms_ = 0;
        }
    } else if (
        event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE && !(sticky_soft_wall() && sticky_edge_.holding)
    ) {
        force_release = true;
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED && capture_focus_click_)
        capture_focus_gained_ms_ = std::max<uint64_t>(event_ms, 1);
    if (event.type == SDL_EVENT_KEY_DOWN) {
        // A key pressed with the focus back means the window is in use: the
        // next click is the player's own.
        if (capture_focus_gained_ms_ != 0)
            capture_focus_click_ = false;
        // Escape in a match lets a captured pointer go until the next click
        // on the battlefield, whatever else the key does.
        if (event.key.key == SDLK_ESCAPE && capture && screen_ == Screen::match && match_)
            capture_waits_for_click_ = true;
    }
    const bool pointer_button =
        (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
        event.button.which != SDL_TOUCH_MOUSEID;
    if (!pointer_button)
        return false;
    // The focusing click's release reaches nothing either. A new press of
    // that button means its release was lost.
    if (capture_swallowed_button_ != 0 && event.button.button == capture_swallowed_button_) {
        capture_swallowed_button_ = 0;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
            return true;
    }
    if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN)
        return false;
    // A press on a battlefield in play captures the pointer again. The input
    // focus is not asked for: a window system may send the click that
    // brings the focus back before the focus itself.
    if (capture && windowed && capture_waits_for_click_ && battlefield_in_play()) {
        capture_waits_for_click_ = false;
        // The focusing click comes with the focus: before it, as some window
        // systems send it, or within focus_click_ms after it.
        const bool with_focus = capture_focus_gained_ms_ == 0 ||
                                (event_ms >= capture_focus_gained_ms_ &&
                                 event_ms - capture_focus_gained_ms_ <= focus_click_ms);
        if (std::exchange(capture_focus_click_, false) && with_focus) {
            capture_swallowed_button_ = event.button.button;
            return true;
        }
        return false;
    }
    // Any other press, a menu's among them, was not the focusing click.
    capture_focus_click_ = false;
    return false;
}

void Runtime::release_window_edge_grab() {
    if (sdl_.window == nullptr)
        return;
    if (!sticky_confined_)
        std::ignore = SDL_SetWindowMouseRect(sdl_.window, nullptr);
    if (!sticky_confined_ && !capture_held_)
        std::ignore = SDL_SetWindowMouseGrab(sdl_.window, false);
}

void Runtime::forget_sticky_edge() {
    release_sticky_edge(sticky_edge_);
    sticky_wanted_ = false;
    sticky_global_known_ = false;
    sticky_drop_warp_motion_ = false;
    // A dialog of the system's clears the grab. The next apply asks again.
    capture_held_ = false;
}

void Runtime::apply_window_edge(bool force_release) {
    SDL_Window* window = sdl_.window;
    if (window == nullptr)
        return;
    // Escape and a lost focus are forgotten with the match, and with another
    // way of Window edge.
    if (!match_ || window_edge_mode() != oa::ui::engine_settings::WindowEdge::capture) {
        capture_waits_for_click_ = false;
        capture_focus_click_ = false;
    }
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    if ((flags & SDL_WINDOW_FULLSCREEN) != 0) {
        // The rect is the sticky edge's. The grab stays full screen's.
        std::ignore = SDL_SetWindowMouseRect(window, nullptr);
        forget_sticky_edge();
        sticky_confined_ = false;
        return;
    }
    // The way that lets go comes first, so it never clears the grab the
    // other has just taken.
    if (window_edge_mode() == oa::ui::engine_settings::WindowEdge::capture) {
        apply_sticky_edge(force_release);
        apply_capture(force_release);
    } else {
        apply_capture(force_release);
        apply_sticky_edge(force_release);
    }
}

void Runtime::apply_capture(bool force_release) {
    SDL_Window* window = sdl_.window;
    // A capture starts only with the pointer inside. A captured pointer
    // cannot leave, so the mouse focus is not asked for again.
    if (force_release || !capture_wanted(!capture_held_)) {
        if (capture_held_) {
            // A focus lost while its event was dropped, as a loading screen
            // drops input, still waits for the click that brings it back.
            if ((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) == 0) {
                capture_waits_for_click_ = true;
                capture_focus_click_ = true;
                capture_focus_gained_ms_ = 0;
            }
            capture_held_ = false;
            release_window_edge_grab();
        }
        return;
    }
    // A change of the window's state clears a window's grab
    // (keep_pointer_on_screen), so it is asked for again each time. SDL
    // confines the pointer to the window: the window server's confinement
    // on macOS, the cursor clip on Windows, a pointer grab on X11 and the
    // pointer constraint on Wayland.
    capture_held_ = true;
    std::ignore = SDL_SetWindowMouseRect(window, nullptr);
    std::ignore = SDL_SetWindowMouseGrab(window, true);
}

void Runtime::apply_sticky_edge(bool force_release) {
    SDL_Window* window = sdl_.window;
    if (window == nullptr)
        return;
    const bool putting_back = sticky_soft_wall() && sticky_edge_.holding && !force_release;
    if (force_release || !sticky_edge_wanted(!putting_back)) {
        release_sticky_edge(sticky_edge_);
        sticky_wanted_ = false;
        sticky_global_known_ = false;
        sticky_drop_warp_motion_ = false;
        if (sticky_confined_) {
            sticky_confined_ = false;
            release_window_edge_grab();
        }
        return;
    }
    if (!sticky_wanted_) {
        float place_x = 0.0F;
        float place_y = 0.0F;
        std::ignore = SDL_GetMouseState(&place_x, &place_y);
        note_sticky_motion(place_x, place_y, 0.0F, 0.0F, static_cast<uint64_t>(SDL_GetTicks()));
        sticky_wanted_ = true;
    }
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSize(window, &width, &height) || width < 2 || height < 2) {
        if (sticky_confined_) {
            sticky_confined_ = false;
            release_window_edge_grab();
        }
        return;
    }
    if (!sticky_soft_wall()) {
        if (sticky_edge_.holding) {
            const SDL_Rect bounds{0, 0, width, height};
            std::ignore = SDL_SetWindowMouseRect(window, &bounds);
            std::ignore = SDL_SetWindowMouseGrab(window, true);
            sticky_confined_ = true;
            return;
        }
        if (sticky_confined_) {
            sticky_confined_ = false;
            release_window_edge_grab();
        }
        return;
    }
    // The pointer stays free. What a Wayland hold confined is cleared, and
    // the pointer is put back only while the rule still holds.
    if (sticky_confined_) {
        sticky_confined_ = false;
        release_window_edge_grab();
    }
    if (!sticky_edge_.holding)
        return;
    float place_x = 0.0F;
    float place_y = 0.0F;
    std::ignore = SDL_GetMouseState(&place_x, &place_y);
    restore_sticky_pointer(place_x, place_y, false);
    // The event that crossed the edge is handled after the warp, and it
    // records the place past the edge. The view scrolls from the last pixel.
    if (screen_ == Screen::match && match_layout_.width > 1 && match_layout_.height > 1) {
        const float max_x = static_cast<float>(match_layout_.width - 1);
        const float max_y = static_cast<float>(match_layout_.height - 1);
        if (match_pointer_x_ < 0.0F || match_pointer_x_ > max_x || match_pointer_y_ < 0.0F ||
            match_pointer_y_ > max_y) {
            match_pointer_x_ = std::clamp(match_pointer_x_, 0.0F, max_x);
            match_pointer_y_ = std::clamp(match_pointer_y_, 0.0F, max_y);
            match_pointer_known_ = true;
        }
    }
}

bool Runtime::gadget_command_available(const oa::ui::gui_layout::Gadget& gadget) const {
    // The order panel greys the buttons the selection cannot use; every
    // other button is live.
    if (const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
        button != nullptr && button->grayed_out)
        return false;
    if (const auto* state = match_gadget_state(gadget))
        return !state->grayed;
    return true;
}

bool Runtime::middle_pan_take(const SDL_Event& event) {
    const bool button_up = event.type == SDL_EVENT_MOUSE_BUTTON_UP;
    const bool swallow_up = button_up && middle_pan_swallow_button_ != 0 &&
                            event.button.button == middle_pan_swallow_button_;
    const bool live =
        screen_ == Screen::match && match_ != nullptr && !match_paused_ && !match_finished_;
    if (!live) {
        if (middle_pan_.phase != middle_pan::Phase::idle || middle_pan_swallow_button_ != 0)
            middle_pan_ = {};
        middle_pan_swallow_button_ = 0;
        return swallow_up;
    }
    // The release of the press that ended the pan gives no order. A left
    // order is given when the button comes up.
    if (swallow_up) {
        middle_pan_swallow_button_ = 0;
        return true;
    }

    float x = 0.0F;
    float y = 0.0F;
    middle_pan::Gesture gesture = middle_pan::Gesture::other;
    bool button_down = false;
    uint8_t button = 0;
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (middle_pan_.phase == middle_pan::Phase::idle)
            return false;
        gesture = middle_pan::Gesture::move;
        x = event.motion.x;
        y = event.motion.y;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        button_down = true;
        button = event.button.button;
        x = event.button.x;
        y = event.button.y;
        gesture = button == SDL_BUTTON_MIDDLE ? middle_pan::Gesture::middle_down
                                              : middle_pan::Gesture::other;
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button != SDL_BUTTON_MIDDLE)
            return false;
        gesture = middle_pan::Gesture::middle_up;
        x = event.button.x;
        y = event.button.y;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        gesture = middle_pan::Gesture::other;
        break;
    default:
        return false;
    }
    if (middle_pan_.phase == middle_pan::Phase::idle && gesture != middle_pan::Gesture::middle_down)
        return false;

    const bool enabled = engine_settings().middle_click_pan;
    const bool can_start = battlefield_contains(x, y) && !megamap_open_ && !hovered_ &&
                           !placed_hud_covers(x, y) && !radar_contains(x, y);
    const auto result = middle_pan::apply(middle_pan_, gesture, x, y, enabled, can_start);
    middle_pan_ = result.state;
    if (button_down && result.swallow) {
        const bool starting =
            button == SDL_BUTTON_MIDDLE && result.state.phase != middle_pan::Phase::idle;
        if (!starting)
            middle_pan_swallow_button_ = button;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && result.swallow)
        middle_pan_drop_text_ = true;
    // The pointer moving still hovers. Losing focus is not a press to consume.
    if (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
        return false;
    return result.swallow;
}

bool Runtime::middle_pan_shows_direction() const {
    if (middle_pan_.phase == middle_pan::Phase::idle)
        return false;
    const float dx = match_pointer_x_ - middle_pan_.anchor_x;
    const float dy = match_pointer_y_ - middle_pan_.anchor_y;
    return dx * dx + dy * dy > 1.0F;
}

void Runtime::draw_middle_pan_marks() {
    if (middle_pan_.phase == middle_pan::Phase::idle || screen_ != Screen::match)
        return;
    const auto stamp = [&](int x, int y, int width, int height, uint8_t index) {
        const auto at = canvas_paint(x, y);
        fill_hud_rect(at.x, at.y, width, height, index);
    };
    const auto plus = [&](int x, int y, int arm, uint8_t index) {
        const int span = arm * 2 + 1;
        stamp(x - arm, y, span, 1, index);
        stamp(x, y - arm, 1, span, index);
    };
    const int anchor_x = static_cast<int>(std::lround(middle_pan_.anchor_x));
    const int anchor_y = static_cast<int>(std::lround(middle_pan_.anchor_y));
    plus(anchor_x, anchor_y, 5, 0);
    plus(anchor_x, anchor_y, 4, 255);
    if (!middle_pan_shows_direction())
        return;
    const float dx = match_pointer_x_ - middle_pan_.anchor_x;
    const float dy = match_pointer_y_ - middle_pan_.anchor_y;
    constexpr double kEighth = 3.141592653589793 / 4.0;
    int sector = static_cast<int>(
        std::lround(std::atan2(static_cast<double>(dy), static_cast<double>(dx)) / kEighth)
    );
    sector = (sector % 8 + 8) % 8;
    constexpr int kDirX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
    constexpr int kDirY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    const int dir_x = kDirX[sector];
    const int dir_y = kDirY[sector];
    const int tip_x = static_cast<int>(std::lround(match_pointer_x_));
    const int tip_y = static_cast<int>(std::lround(match_pointer_y_));
    const auto arrow = [&](uint8_t index, int spread) {
        for (int step = 0; step <= 8; ++step)
            stamp(tip_x - dir_x * step, tip_y - dir_y * step, spread, spread, index);
        const int wing_x = tip_x - dir_x * 2;
        const int wing_y = tip_y - dir_y * 2;
        stamp(wing_x + dir_y * 3, wing_y - dir_x * 3, spread, spread, index);
        stamp(wing_x - dir_y * 3, wing_y + dir_x * 3, spread, spread, index);
    };
    arrow(0, 3);
    arrow(255, 1);
}

} // namespace oa::app
