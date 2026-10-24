// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Match hotkeys, selection commands and overlays.
#include "oa/app/runtime.hpp"
#include "panel_first_draw.hpp"
#include "oa/app/view_rules.hpp"
#include "oa/data/languages/translation.hpp"
#include "oa/present/game_text.hpp"
#include "oa/ui/frontend_renderer/game_text.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/ui/decoded.hpp"
#include "engine_settings_state.hpp"
#include "match_models.hpp"
#include "oa/sim/speed.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/hud/chat_panel.hpp"
#include "oa/ui/hud/order_panel.hpp"
#include "oa/ui/hud/unit_info.hpp"
#include "oa/ui/gui_input/gadget_panel.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {
namespace {

// The game speed step a speed key asks for: 1 for '+', '=' and the keypad's
// '+', -1 for '-' and the keypad's '-', 0 for any other key.
int game_speed_key_step(const SDL_KeyboardEvent& key) {
    if (key.key == SDLK_MINUS || key.key == SDLK_KP_MINUS || key.scancode == SDL_SCANCODE_MINUS ||
        key.scancode == SDL_SCANCODE_KP_MINUS)
        return -1;
    if (key.key == SDLK_EQUALS || key.key == SDLK_PLUS || key.key == SDLK_KP_PLUS ||
        key.scancode == SDL_SCANCODE_EQUALS || key.scancode == SDL_SCANCODE_KP_PLUS)
        return 1;
    return 0;
}

/// Returns the way a build-page key turns the order panel's page.
///
/// @param key the key pressed
/// @return 1 for '.', -1 for ',', 0 for any other key
int page_key_step(const SDL_KeyboardEvent& key) {
    if (key.key == SDLK_PERIOD)
        return 1;
    if (key.key == SDLK_COMMA)
        return -1;
    return 0;
}

// The unit info panel's control that shows the unit's picture, and the one
// that closes it.
constexpr const char* kUnitInfoPicture = "HOTR";
constexpr const char* kUnitInfoDone = "DONE";
// The art the unit info panel's face and button come from.
constexpr const char* kUnitInfoArt = "anims/commongui.gaf";
// Height of a statistic label the unit info panel adds: one line of the GUI
// font.
constexpr int16_t kUnitInfoLabelHeight = 12;

// Quick keys are ASCII characters below this one.
constexpr SDL_Keycode kQuickKeyEnd = 0x7F;

/// Returns whether a key types the quick key of a panel's named button.
///
/// Keys with Ctrl, Alt or the system key down type no character; a letter
/// matches in either case.
///
/// @param gadgets the panel's records
/// @param name the button's name
/// @param key the key pressed
/// @return true when the button holds a quick key and the key types it
bool types_quick_key(
    const std::vector<oa::ui::gui_layout::Gadget>& gadgets,
    std::string_view name,
    const SDL_KeyboardEvent& key
) {
    if ((key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0 || key.key == 0 ||
        key.key >= kQuickKeyEnd)
        return false;
    const auto typed = std::tolower(static_cast<int>(key.key));
    return std::any_of(
        gadgets.begin(), gadgets.end(), [&](const oa::ui::gui_layout::Gadget& gadget) {
            const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
            return button != nullptr && gadget.common.name == name && button->quick_key != 0 &&
                   std::tolower(static_cast<unsigned char>(button->quick_key)) == typed;
        }
    );
}

} // namespace

bool Runtime::handle_match_hotkey(const SDL_KeyboardEvent& key) {
    if (screen_ != Screen::match || !match_)
        return false;
    // The surrender confirmation takes the keys ahead of the chat line and
    // a marker's text, which keep what was typed for after it.
    const bool question = match_question_open();
    // A held key's repeats press no hotkey but the speed keys and the page
    // keys: held '+' or '-' goes on changing the game speed, a step with each
    // repeat, as in 3.1c, and stops at the fastest or the slowest; held ','
    // or '.' goes on turning the order panel's page, a page and a
    // nextbuildmenu with each repeat, as in 3.1c, taking the order page into
    // its turn as a press does. Held Backspace goes on deleting from the chat
    // line or a marker's text being typed, as held keys go on typing in 3.1c;
    // held letters arrive as repeated text.
    const bool deletes_typed_text =
        !question && key.key == SDLK_BACKSPACE && (chat_composing_ || whiteboard_input_.editing);
    if (key.repeat && !deletes_typed_text && game_speed_key_step(key) == 0 &&
        page_key_step(key) == 0)
        return false;
    if ((!question && whiteboard_key(key)) || megamap_key(key))
        return true;
    // F4 pins the kills board out (Game.graphics_flags 0x80).
    if (key.key == SDLK_F4 || key.scancode == SDL_SCANCODE_F4)
        return resource_panel_f4_key() || handle_console_hotkey(key);
    // F1 opens the unit info panel; Shift+F1 pins the unit under the cursor
    // instead (Game.pinned_unit_a), or unpins without one.
    if (key.key == SDLK_F1 || key.scancode == SDL_SCANCODE_F1) {
        auto& game = match_->state().game;
        if ((input_modifiers(ModifierUse::keyboard) & SDL_KMOD_SHIFT) != 0) {
            game.pinned_unit_a_valid = game.cursor_unit_id != 0 ? 1U : 0U;
            if (game.cursor_unit_id != 0)
                game.pinned_unit_a = game.cursor_unit_id;
            return true;
        }
        // Without a unit to show, F1 does nothing.
        std::ignore = open_unit_info();
        return true;
    }
    if (key.key == SDLK_F2 || key.scancode == SDL_SCANCODE_F2) {
        if (match_paused_)
            resume_match_pause();
        else
            show_match_pause_menu();
        return true;
    }
    // F3 goes to the next unit the message log reported.
    if (key.key == SDLK_F3 || key.scancode == SDL_SCANCODE_F3) {
        oa::sim::messages::cycle_reported_units(match_->state(), message_hooks());
        return true;
    }
    if (key.key == SDLK_F12 || key.scancode == SDL_SCANCODE_F12) {
        oa::sim::messages::clear_messages(match_->state().game);
        return true;
    }
    // Pause flips the pause bit and opens no menu; a finished match keeps it
    // as it is.
    if (key.key == SDLK_PAUSE || key.scancode == SDL_SCANCODE_PAUSE) {
        // Pause is a console hotkey and a match is running, so it is always
        // taken.
        if (!match_finished_)
            std::ignore = handle_console_hotkey(key);
        return true;
    }
    if ((input_modifiers(ModifierUse::keyboard) & SDL_KMOD_CTRL) != 0 &&
        (key.key == SDLK_F9 || key.scancode == SDL_SCANCODE_F9)) {
        capture_screenshot();
        return true;
    }
    if (chat_composing_ && !question) {
        if (key.key == SDLK_ESCAPE) {
            close_chat_line();
            return true;
        }
        if (key.key == SDLK_RETURN) {
            submit_chat_line();
            return true;
        }
        if (key.key == SDLK_BACKSPACE && !chat_buffer_.empty()) {
            // The line is typed text: the key takes the whole last character.
            chat_buffer_.erase(oa::present::last_character_start(chat_buffer_));
            return true;
        }
        return true;
    }
    // The surrender confirmation answers Enter as No, its Enter default;
    // escape_match_menu answers its Escape.
    if (match_paused_ && key.key == SDLK_RETURN && enter_match_menu())
        return true;
    // The unit info panel's Enter and Escape defaults are both DONE, and so
    // is its quick key, OK's O.
    if (unit_info_panel_ &&
        (key.key == SDLK_RETURN || key.key == SDLK_ESCAPE ||
         (unit_info_panel_->screen &&
          types_quick_key(unit_info_panel_->screen->layout.gadgets, kUnitInfoDone, key)))) {
        press_unit_info_done();
        return true;
    }
    // The in-game menu's and the tab menu's panels, and the preferences a
    // match opens, take the keys they answer while they hold the keyboard.
    if (press_match_panel_key(key))
        return true;
    if (handle_console_hotkey(key))
        return true;
    // Escape takes back an armed command and keeps the selection; with none
    // armed it drops the selection. With the Escape opens the game menu
    // setting on and nothing selected, it opens the in-game menu as F2 does,
    // which does not stop a shared game; with the menu open, the event
    // handler closes it.
    if (key.key == SDLK_ESCAPE) {
        if (match_paused_)
            return false;
        if (match_command_ != MatchCommand::none || pending_build_type_ != 0) {
            clear_or_cancel_match_command();
            return true;
        }
        if (!match_finished_ && !has_local_selection() &&
            EngineSettingsState::escape_opens_menu(*this)) {
            show_match_pause_menu();
            return true;
        }
        clear_or_cancel_match_command();
        return true;
    }
    if (match_paused_)
        return false;
    const auto mods = input_modifiers(ModifierUse::keyboard);
    if ((mods & SDL_KMOD_CTRL) != 0 && (key.key == SDLK_D || key.scancode == SDL_SCANCODE_D) &&
        selected_match_unit_ != 0) {
        match_->toggle_self_destruct(selected_local_ids());
        status_ = "Self-destruct";
        return true;
    }
    const bool ctrl = (mods & SDL_KMOD_CTRL) != 0;
    const bool alt = (mods & SDL_KMOD_ALT) != 0;
    const bool shift = (mods & SDL_KMOD_SHIFT) != 0;
    const auto sym = key.key;
    if (const int n = squad_from_key(key); n != 0) {
        if (ctrl) {
            assign_squad(n);
            return true;
        }
        // A digit picks its squad with Alt down, or with Alt up
        // once SwitchAlt is on; otherwise it picks build page digit - 1.
        const bool switch_alt =
            (match_->state().game.graphics_flags & oa::ui::console::graphics_flag::switch_alt) != 0;
        if (alt != switch_alt)
            select_squad(n, shift);
        else
            show_match_page_by_key(n - 1);
        return true;
    }
    if (ctrl) {
        if (selection_shortcut_key(sym, shift))
            return true;
        if (sym == SDLK_A) {
            select_units_matching([](const oa::sim::unit_spawn::Slot&) { return true; });
            return true;
        }
        if (sym == SDLK_C) {
            select_and_follow_commander(shift);
            return true;
        }
        if (sym == SDLK_B) {
            select_units_matching([&](const oa::sim::unit_spawn::Slot& slot) {
                const auto* def = definition_for(slot.unit_index);
                return def && def->builder && def->bm_code != 0;
            });
            return true;
        }
        if (sym == SDLK_F) {
            select_units_matching([&](const oa::sim::unit_spawn::Slot& slot) {
                const auto* def = definition_for(slot.unit_index);
                return def && def->builder && def->bm_code == 0;
            });
            return true;
        }
        if (sym == SDLK_V) {
            select_units_matching([&](const oa::sim::unit_spawn::Slot& slot) {
                const auto* def = definition_for(slot.unit_index);
                return def && def->can_fly;
            });
            return true;
        }
        // Ctrl+S selects the local units of the on-screen list: those whose
        // box overlaps the view.
        if (sym == SDLK_S) {
            auto& world = match_->state();
            world.game.local_player_index = match_local_player_;
            oa::sim::selection::select_visible_units(world, on_screen_lists(), selection_hooks());
            adopt_selected_units();
            return true;
        }
        // Ctrl+Z adds to the selection every selectable local unit, anywhere
        // on the map, whose type is the type of any selected unit, as 3.1c
        // does. The type set holds the profile's type ids, or every type id
        // with ui.selection-shortcuts' same-type-bitset-fix.
        if (sym == SDLK_Z) {
            auto& world = match_->state();
            world.game.local_player_index = match_local_player_;
            const bool every_type =
                selection_shortcuts_on() && ui_rules().selection_shortcuts.same_type_bitset_fix;
            oa::sim::selection::select_matching_types(
                world,
                selection_hooks(),
                every_type ? oa::data::limits::highest_type_bits : limits_.unit_types.bitset_bits
            );
            adopt_selected_units();
            int count = 0;
            for (const auto& slot : match_->world().slots)
                count += slot.unit != nullptr && slot.owner_index == match_local_player_ &&
                         (slot.unit->flags & OA_UNIT_FLAG_SELECTED) != 0;
            status_ = count == 0 ? "No units" : std::to_string(count) + " selected";
            return true;
        }
        return false;
    }
    if (sym == SDLK_TAB && selected_match_unit_ != 0) {
        cycle_selected_primary(shift);
        return true;
    }
    if (sym == SDLK_T || key.scancode == SDL_SCANCODE_T) {
        cycle_match_tracking(shift);
        return true;
    }
    // Space shows the status strip and the kills board only while it is
    // held (draw_status_panel, draw_match_kill_board), as 3.1c does: its
    // press selects nothing and moves no camera.
    if (sym == SDLK_SPACE || key.scancode == SDL_SCANCODE_SPACE)
        return true;
    // Home centres the view on the selected unit, or with none selects the
    // local commander and centres on it.
    if (sym == SDLK_HOME || key.scancode == SDL_SCANCODE_HOME) {
        if (selected_match_unit_ != 0)
            center_camera_on_unit(selected_match_unit_);
        else {
            select_local_commander();
            apply_match_hud_for_selection();
            if (selected_match_unit_ != 0)
                center_camera_on_unit(selected_match_unit_);
        }
        status_ = "Center";
        return true;
    }
    // A multiplayer game's 'h' reached the console's hotkeys above; outside one
    // it does nothing.
    if (sym == SDLK_H)
        return true;
    // 'n' centres the view on the next local unit not yet visited and marks
    // the local units then on screen visited; it selects nothing.
    if (sym == SDLK_N) {
        auto& world = match_->state();
        world.game.local_player_index = match_local_player_;
        oa::sim::selection::cycle_next_unit(world, on_screen_lists(), selection_hooks());
        if (const auto next = world.game.cycle_unit_id; next != 0)
            status_ = "Next " + unit_info_name(next);
        return true;
    }
    // A multiplayer game's watcher changes no speed.
    if (const int step = game_speed_key_step(key); step != 0) {
        if ((current_extension_state() & extension_state::local_watcher) == 0)
            adjust_game_speed(step);
        return true;
    }
    // The page keys take the order page into their turn, and sound whether
    // or not a page turns; each repeat of a held one turns again.
    if (const int step = page_key_step(key); step != 0) {
        play_match_interface_sound("nextbuildmenu");
        press_match_panel_page(
            step > 0 ? oa::ui::hud::BuildPanelClick::page_forward
                     : oa::ui::hud::BuildPanelClick::page_back,
            true
        );
        return true;
    }
    if (sym == SDLK_RETURN || key.scancode == SDL_SCANCODE_RETURN) {
        open_chat_line();
        return true;
    }
    // A button whose quick key is the typed letter in either case
    // is clicked (ATTACK 'a', BLAST 'd', STOP 's', ...).
    if (match_hud_ && sym < 0x80) {
        const auto typed = std::tolower(static_cast<int>(sym));
        for (std::size_t i = 0; i < match_hud_->layout.gadgets.size(); ++i) {
            const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(
                &match_hud_->layout.gadgets[i].fields
            );
            if (button == nullptr || button->quick_key == 0)
                continue;
            if (std::tolower(static_cast<unsigned char>(button->quick_key)) == typed) {
                activate_match_hud(i);
                return true;
            }
        }
    }
    return false;
}

void Runtime::clear_or_cancel_match_command() {
    if (!match_)
        return;
    if (match_command_ != MatchCommand::none || pending_build_type_ != 0) {
        reset_match_command();
        pending_build_type_ = 0;
        oa::sim::gameplay_input::set_pointer_command(
            match_->state().game, oa::sim::gameplay_input::OrderCommand::default_order
        );
        apply_match_hud_for_selection();
        status_ = "Command cancelled";
        return;
    }
    clear_local_selection();
    apply_match_hud_for_selection();
    status_ = "Selection cleared";
}

void Runtime::select_match_unit(float x, float y, int32_t clicks) {
    if (!match_)
        return;
    update_pointer(x, y);
    auto& world = match_->state();
    world.game.local_player_index = match_local_player_;
    const auto id = hovered_match_unit_;
    const bool toggle = (input_modifiers(ModifierUse::selection) & SDL_KMOD_SHIFT) != 0;
    // The unit under the cursor, when it is a selectable local unit, is
    // selected in place of the others, or toggled with shift; the local
    // units on screen are then visited for 'n'.
    oa::sim::selection::select_cursor_unit(world, on_screen_lists(), toggle, selection_hooks());
    const auto* unit = id != 0 ? oa::world_unit_at(&world, id) : nullptr;
    if (unit == nullptr || (unit->flags & OA_UNIT_FLAG_SELECTED) == 0) {
        adopt_selected_units();
        return;
    }
    int count = 1;
    if (!toggle && clicks >= 2) {
        const auto type = unit->type_index;
        for (auto& slot : match_->world().slots) {
            if (slot.unit_index == 0 || slot.unit == nullptr || slot.unit_index == id ||
                slot.owner_index != match_local_player_ || slot.unit->type_index != type ||
                !match_->selectable(slot.unit_index))
                continue;
            slot.unit->flags |= OA_UNIT_FLAG_SELECTED;
            ++count;
        }
    }
    if (!toggle)
        selected_match_unit_ = id;
    adopt_selected_units();
    const auto maximum = match_->world().slots[id].unit->type
                             ? match_->world().slots[id].unit->type->maximum_health
                             : 0;
    status_ = count > 1 ? std::to_string(count) + " " + unit_info_name(id)
                        : unit_info_name(id) + " " +
                              std::to_string(match_->world().slots[id].unit->health) + "/" +
                              std::to_string(maximum);
}

void Runtime::adopt_selected_units() {
    if (!match_)
        return;
    uint16_t first = 0;
    bool primary_kept = false;
    for (auto& slot : match_->world().slots) {
        if (slot.unit_index == 0 || slot.unit == nullptr ||
            slot.owner_index != match_local_player_ ||
            (slot.unit->flags & OA_UNIT_FLAG_SELECTED) == 0)
            continue;
        offline_services_.refresh_selected_unit(slot);
        if (first == 0)
            first = slot.unit_index;
        if (slot.unit_index == selected_match_unit_)
            primary_kept = true;
    }
    if (!primary_kept)
        selected_match_unit_ = first;
    apply_match_hud_for_selection();
}

void Runtime::box_select_units(int x0, int y0, int x1, int y1, bool add) {
    if (!match_)
        return;
    auto& world = match_->state();
    world.game.local_player_index = match_local_player_;
    if (live_renderer_active()) {
        if (!add)
            clear_local_selection();
        const int left = std::min(x0, x1), right = std::max(x0, x1);
        const int top = std::min(y0, y1), bottom = std::max(y0, y1);
        for (const auto& slot : match_->world().slots) {
            if (!slot.unit || slot.unit_index == 0 || slot.owner_index != match_local_player_ ||
                !match_->selectable(slot.unit_index))
                continue;
            const auto bounds = live_unit_bounds(slot.unit_index);
            if (!bounds)
                continue;
            const auto centre_x = bounds->x1 + (bounds->x2 - bounds->x1) / 2;
            const auto centre_y = bounds->y1 + (bounds->y2 - bounds->y1) / 2;
            if (centre_x < left || centre_x > right || centre_y < top || centre_y > bottom)
                continue;
            if (add && (slot.unit->flags & OA_UNIT_FLAG_SELECTED) != 0)
                slot.unit->flags &= ~OA_UNIT_FLAG_SELECTED;
            else
                adopt_selection(slot.unit_index);
        }
        selection_shortcut_drag_filter();
        adopt_selected_units();
        status_ = std::to_string(selected_local_ids().size()) + " selected";
        return;
    }
    // The corners are the drag's ends on the canvas, projected from the
    // camera the view has now; the box goes back to Game.drag_start and
    // drag_end as map points at height 0.
    const auto zoom = match_zoom() == 0.0F ? 1.0 : static_cast<double>(match_zoom());
    const auto map_point = [&](int x, int y) {
        return std::array<int32_t, 3>{
            match_camera_x_ + static_cast<int32_t>(
                                  std::llround(static_cast<double>(x - match_layout_.left) / zoom)
                              ),
            0,
            match_camera_z_ + static_cast<int32_t>(
                                  std::llround(static_cast<double>(y - match_layout_.top) / zoom)
                              )
        };
    };
    const auto start = map_point(x0, y0);
    const auto end = map_point(x1, y1);
    static_assert(sizeof start == sizeof world.game.drag_start);
    static_assert(sizeof end == sizeof world.game.drag_end);
    std::memcpy(world.game.drag_start, start.data(), sizeof world.game.drag_start);
    std::memcpy(world.game.drag_end, end.data(), sizeof world.game.drag_end);
    if (!add)
        selected_match_unit_ = 0;
    const bool any =
        oa::sim::selection::select_units_in_box(world, on_screen_lists(), add, selection_hooks());
    selection_shortcut_drag_filter();
    adopt_selected_units();
    int count = 0;
    for (const auto& slot : match_->world().slots)
        if (slot.unit != nullptr && slot.owner_index == match_local_player_ &&
            (slot.unit->flags & OA_UNIT_FLAG_SELECTED) != 0)
            ++count;
    status_ = !any ? "No units" : std::to_string(count) + " selected";
}

void Runtime::area_order_units(int x0, int y0, int x1, int y1, std::string_view kind) {
    if (!match_)
        return;
    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);
    const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
    int count = 0;
    // Each selected unit takes the box's first order in place of its orders
    // (or after them, with shift held) and queues the rest behind it.
    std::vector<uint16_t> ordered;
    for (auto& slot : match_->world().slots) {
        if (slot.unit_index == 0 || slot.unit == nullptr || !match_->selectable(slot.unit_index))
            continue;
        const bool enemy = slot.owner_index != match_local_player_;
        const bool local = slot.owner_index == match_local_player_;
        if (kind == "attack" && !enemy)
            continue;
        if (kind == "repair" && !local)
            continue;
        // A unit of another machine's player is tested where the frame last
        // drawn showed it, on its playout, as the pointer picks it.
        auto screen = project_match_point(viewport, slot.unit->position);
        if (const MatchModels* models = drawn_match_models(); models != nullptr) {
            if (const auto pose = mirrored_pose(
                    *models, match_->state(), slot.unit_index, models->presentation.drawn_moment
                ))
                screen = project_match_point(
                    viewport,
                    {static_cast<uint32_t>(pose->position.x),
                     static_cast<uint32_t>(pose->position.y),
                     static_cast<uint32_t>(pose->position.z)}
                );
        }
        if (screen.x < x0 || screen.x > x1 || screen.y < y0 || screen.y > y1)
            continue;
        for_each_selected([&](uint16_t source) {
            if (source == slot.unit_index)
                return;
            const bool queue =
                queueing() || std::find(ordered.begin(), ordered.end(), source) != ordered.end();
            bool issued = true;
            try {
                if (kind == "attack")
                    issued = match_->issue_attack_command(source, slot.unit_index, queue, nullptr);
                else if (kind == "reclaim")
                    match_->issue_reclaim(source, slot.unit_index, queue);
                else if (kind == "repair")
                    issue_resume_or_repair_from(source, slot.unit_index, queue);
                else
                    issued = false;
            } catch (const std::exception&) {
                issued = false;
            }
            if (issued && !queue)
                ordered.push_back(source);
        });
        ++count;
    }
    finish_issued_command();
    status_ = std::string(kind) + " " + std::to_string(count);
}

oa::sim::speed::Range Runtime::game_speed_range() const {
    if (!match_)
        return {};
    auto range = oa::sim::speed::range_of(match_->rules().console.game_speed_range);
    if (game_speed_lock_)
        range = oa::sim::speed::locked(
            range,
            game_speed_lock_->slowest - oa::sim::speed::lock_offset,
            game_speed_lock_->fastest - oa::sim::speed::lock_offset
        );
    return range;
}

void Runtime::lock_game_speed(oa::sim::speed::Range lock) {
    game_speed_lock_ = lock;
}

void Runtime::unlock_game_speed() {
    game_speed_lock_.reset();
}

void Runtime::read_speed_lock_line(const char* text) {
    if (!match_ || (current_extension_state() & extension_state::multiplayer) != 0)
        return;
    const auto& rule = match_->rules().console.game_speed_range;
    if (!rule.enabled || !rule.syncon)
        return;
    const auto line = oa::sim::speed::read_lock_line(text);
    switch (line.request) {
    case oa::sim::speed::LockRequest::lock: {
        game_speed_lock_ =
            oa::sim::speed::locked(oa::sim::speed::range_of(rule), line.low, line.high);
        auto& world = match_->state();
        const auto range = game_speed_range();
        const int32_t speed = world.game.requested_speed;
        if (speed >= range.slowest && speed <= range.fastest)
            break;
        std::ignore = oa::sim::speed::set_speed(world, speed, message_hooks(), range);
        match_timing_.requested_rate = world.game.requested_speed;
        match_timing_.actual_rate = world.game.current_speed;
        preferences_.current_game_speed = world.game.current_speed;
        break;
    }
    case oa::sim::speed::LockRequest::unlock:
        game_speed_lock_.reset();
        break;
    case oa::sim::speed::LockRequest::none:
        break;
    }
}

void Runtime::adjust_game_speed(int delta) {
    auto& world = match_->state();
    const auto hooks = message_hooks();
    const auto range = game_speed_range();
    // '+' sets the speed only below the fastest, '-' only above the slowest.
    const int32_t before = world.game.requested_speed;
    const bool sets = delta > 0   ? before < range.fastest
                      : delta < 0 ? before > range.slowest
                                  : false;
    if (delta > 0)
        oa::sim::speed::raise_speed(world, hooks, range);
    else if (delta < 0)
        oa::sim::speed::lower_speed(world, hooks, range);
    match_timing_.requested_rate = world.game.requested_speed;
    match_timing_.actual_rate = world.game.current_speed;
    preferences_.current_game_speed = world.game.current_speed;
    if (sets)
        call_hook_or_report<&Extension::speed_changed>(
            extension_, hook_error_report(), *this, world.game.requested_speed
        );
}

bool Runtime::open_unit_info() {
    namespace hud = oa::ui::hud;
    if (!match_)
        return false;
    auto& world = match_->state();
    if ((world.game.frame_flags & hud::kFrameUnitInfoOpen) != 0)
        return false;
    const auto type = hud::unit_info_subject(
        world,
        hovered_gadget_name(),
        [](void* user, const char* name) -> uint16_t {
            return oa::sim::unit_spawn::find_type_index(
                static_cast<Runtime*>(user)->spawn_type_names_, name
            );
        },
        [](void* user, const oa::Player& viewer, const oa::Unit& unit) {
            try {
                return static_cast<Runtime*>(user)->match_->unit_shown(viewer.index, unit.id);
            } catch (const std::exception&) {
                return false;
            }
        },
        this
    );
    if (type == 0)
        return false;
    hud::PanelLoader loader{};
    loader.user = this;
    loader.load = [](void* user, const char* name, const oa::Unit*, int32_t) {
        auto& self = *static_cast<Runtime*>(user);
        UnitInfoPanel panel;
        try {
            // The panel's art is the common GUI art: its face and its
            // button's picture, as the panel names no GAF of its own.
            panel.screen = renderer::load_screen(
                self.assets_,
                {oa::data::defs::gui_path(name), "", "palettes/guipal.pal", "", kUnitInfoArt}
            );
        } catch (const std::exception& error) {
            std::cerr << "unit info panel unavailable: " << error.what() << '\n';
            return false;
        }
        auto& gadgets = panel.screen->layout.gadgets;
        if (gadgets.empty())
            return false;
        // The panel is centred right of the HUD strip, as 3.1c places it
        // whatever position its file gives; the controls lie in it.
        auto& root = gadgets.front().common;
        oa::ui::gui_input::place_root(
            root.x,
            root.y,
            root.width,
            root.height,
            oa::ui::gui_input::panel_flag::beside_hud | oa::ui::gui_input::panel_flag::first_draw,
            kCanvasWidth,
            kCanvasHeight,
            kBattlefieldLeft
        );
        panel.root = root;
        for (std::size_t index = 1; index < gadgets.size(); ++index) {
            gadgets[index].common.x = static_cast<int16_t>(gadgets[index].common.x + panel.root.x);
            gadgets[index].common.y = static_cast<int16_t>(gadgets[index].common.y + panel.root.y);
        }
        // The panel names no picture of its own, so its face is the common
        // GUI art's BackTile, drawn in the game palette.
        auto& background = panel.screen->background;
        background.width = static_cast<uint32_t>(kCanvasWidth);
        background.height = static_cast<uint32_t>(kCanvasHeight);
        background.rgb.assign(static_cast<std::size_t>(kCanvasWidth * kCanvasHeight * 3), 0);
        if (std::any_of(self.match_palette_.begin(), self.match_palette_.end(), [](uint8_t b) {
                return b != 0;
            }))
            background.palette = self.match_palette_;
        if (const auto* tile = self.gaf_sequence(panel.screen->shared_sprites, kBackTile))
            draw_back_tile(
                background,
                panel.root,
                *tile,
                background.palette ? *background.palette : panel.screen->gui_palette
            );
        self.unit_info_panel_ = std::move(panel);
        self.match_->state().game.frame_flags =
            static_cast<uint16_t>(self.match_->state().game.frame_flags | hud::kFrameUnitInfoOpen);
        return true;
    };
    hud::PanelControls controls{};
    controls.user = this;
    controls.find = [](void* user, const char* name) {
        auto& self = *static_cast<Runtime*>(user);
        if (!self.unit_info_panel_ || !self.unit_info_panel_->screen)
            return int32_t{-1};
        const auto& gadgets = self.unit_info_panel_->screen->layout.gadgets;
        for (std::size_t index = 0; index < gadgets.size(); ++index)
            if (gadgets[index].common.name == name)
                return static_cast<int32_t>(index);
        return int32_t{-1};
    };
    controls.set_text = [](void* user, int32_t index, const char* text) {
        auto& self = *static_cast<Runtime*>(user);
        auto& gadgets = self.unit_info_panel_->screen->layout.gadgets;
        if (index < 0 || static_cast<std::size_t>(index) >= gadgets.size())
            return;
        if (auto* label = std::get_if<oa::ui::gui_layout::LabelFields>(
                &gadgets[static_cast<std::size_t>(index)].fields
            ))
            label->text = text;
    };
    hud::UnitInfoHost host{};
    host.user = this;
    // The panel's headings, labels and units in the game's language, as
    // gamedata\translate.tdf gives them.
    host.localize = [](void* user, const char* text) -> const char* {
        auto& self = *static_cast<Runtime*>(user);
        self.unit_panel_word_ = self.translate_ui(text);
        return self.unit_panel_word_.c_str();
    };
    host.add_label = [](void* user, const char* text, int16_t x, int16_t y, uint32_t) {
        auto& self = *static_cast<Runtime*>(user);
        if (!self.unit_info_panel_ || !self.unit_info_panel_->screen)
            return;
        const auto& root = self.unit_info_panel_->root;
        oa::ui::gui_layout::Gadget label;
        label.common.type = oa::ui::gui_layout::GadgetType::label;
        label.common.x = static_cast<int16_t>(root.x + x);
        label.common.y = static_cast<int16_t>(root.y + y);
        label.common.width = static_cast<int16_t>(std::max(1, root.width - x));
        label.common.height = kUnitInfoLabelHeight;
        label.common.active = 1;
        oa::ui::gui_layout::LabelFields fields;
        fields.text = text;
        label.fields = std::move(fields);
        self.unit_info_panel_->screen->layout.gadgets.push_back(std::move(label));
    };
    host.set_picture = [](void* user, const char* path) {
        auto& self = *static_cast<Runtime*>(user);
        if (!self.unit_info_panel_)
            return;
        self.unit_info_panel_->picture_path = path;
        // The picture in the language's folder when the game data has it
        // there (unitpics-German), as 3.1c looks first.
        if (const auto variant = oa::data::languages::language_folder_path(path);
            variant && self.assets_.file_size(*variant) > 0)
            self.unit_info_panel_->picture_path = *variant;
    };
    host.free_picture = [](void* user) {
        auto& self = *static_cast<Runtime*>(user);
        if (self.unit_info_panel_)
            self.unit_info_panel_->picture_path.clear();
    };
    if (!hud::open_unit_info_panel(
            world,
            type,
            static_cast<int32_t>(oa::sim::messages::ticks_per_second),
            loader,
            controls,
            host
        )) {
        unit_info_panel_.reset();
        return false;
    }
    // The panel is drawn once, its picture copied at HOTR at its own size.
    // Each button shows the quick key its caption gives it on the panel's
    // first draw, underlined: OK's O.
    auto& panel = *unit_info_panel_;
    std::vector<renderer::ButtonPresentation> buttons;
    auto& gadgets = panel.screen->layout.gadgets;
    for (std::size_t index = 0; index < gadgets.size(); ++index) {
        assign_button_quick_key(gadgets, index);
        if (const auto* button =
                std::get_if<oa::ui::gui_layout::ButtonFields>(&gadgets[index].fields)) {
            renderer::ButtonPresentation shown;
            shown.name = gadgets[index].common.name;
            shown.quick_key = static_cast<char>(button->quick_key);
            buttons.push_back(std::move(shown));
        }
    }
    try {
        panel.frame = renderer::render_screen(*panel.screen, buttons);
    } catch (const std::exception& error) {
        std::cerr << "unit info panel: " << error.what() << '\n';
        panel.frame = {};
    }
    const oa::ui::gui_layout::Gadget* picture_area = nullptr;
    for (const auto& gadget : panel.screen->layout.gadgets)
        if (gadget.common.name == kUnitInfoPicture)
            picture_area = &gadget;
    if (picture_area != nullptr && !panel.picture_path.empty() && !panel.frame.rgb.empty()) {
        auto path = panel.picture_path;
        std::replace(path.begin(), path.end(), '\\', '/');
        try {
            const auto picture =
                oa::ui::decoded::require(oa::decode_pcx(assets_.read(path).bytes), path);
            const auto& palette = panel.screen->background.palette;
            for (uint32_t row = 0; row < picture.height; ++row)
                for (uint32_t column = 0; column < picture.width; ++column) {
                    const auto x = picture_area->common.x + static_cast<int>(column);
                    const auto y = picture_area->common.y + static_cast<int>(row);
                    if (x < 0 || y < 0 || x >= static_cast<int>(panel.frame.width) ||
                        y >= static_cast<int>(panel.frame.height))
                        continue;
                    const auto at = static_cast<std::size_t>(row) * picture.width + column;
                    auto* out =
                        panel.frame.rgb.data() + (static_cast<std::size_t>(y) * panel.frame.width +
                                                  static_cast<std::size_t>(x)) *
                                                     3U;
                    // The picture's colour indices are the game palette's.
                    if (palette && at < picture.indices.size()) {
                        const auto entry = static_cast<std::size_t>(picture.indices[at]) * 4U;
                        out[0] = (*palette)[entry];
                        out[1] = (*palette)[entry + 1];
                        out[2] = (*palette)[entry + 2];
                    } else if (at * 3U + 2U < picture.rgb.size()) {
                        out[0] = picture.rgb[at * 3U];
                        out[1] = picture.rgb[at * 3U + 1];
                        out[2] = picture.rgb[at * 3U + 2];
                    }
                }
        } catch (const std::exception& error) {
            std::cerr << "unit info picture " << path << " unavailable: " << error.what() << '\n';
        }
    }
    status_ = "Unit info";
    return true;
}

void Runtime::close_unit_info() {
    if (!unit_info_panel_)
        return;
    oa::ui::hud::UnitInfoHost host{};
    host.user = this;
    host.free_picture = [](void* user) {
        auto& self = *static_cast<Runtime*>(user);
        if (self.unit_info_panel_)
            self.unit_info_panel_->picture_path.clear();
    };
    // A panel closing asks for nothing more.
    if (match_)
        std::ignore = oa::ui::hud::unit_info_panel_click(match_->state().game, nullptr, host, {});
    unit_info_panel_.reset();
}

void Runtime::press_unit_info_done() {
    if (!unit_info_panel_ || !match_)
        return;
    oa::ui::hud::HudEvents events{};
    events.user = this;
    events.play_sound = [](void* user, const char* name) {
        static_cast<Runtime*>(user)->play_ui_sound(name, 0);
    };
    if (oa::ui::hud::unit_info_panel_click(match_->state().game, kUnitInfoDone, {}, events) ==
        oa::ui::hud::UnitInfoClick::done)
        close_unit_info();
}

std::optional<oa::ui::display_layout::Rect> Runtime::unit_info_area() const {
    if (!unit_info_panel_ || match_layout_.scale <= 0.0 ||
        oa::ui::display_layout::placed_mode(match_layout_))
        return std::nullopt;
    const auto& root = unit_info_panel_->root;
    if (root.width <= 0 || root.height <= 0)
        return std::nullopt;
    const auto scaled = [this](int32_t value) {
        return std::max(
            1, static_cast<int32_t>(std::lround(static_cast<double>(value) * match_layout_.scale))
        );
    };
    const auto width = scaled(root.width);
    const auto height = scaled(root.height);
    int16_t x = 0;
    int16_t y = 0;
    oa::ui::gui_input::place_root(
        x,
        y,
        width,
        height,
        oa::ui::gui_input::panel_flag::beside_hud | oa::ui::gui_input::panel_flag::first_draw,
        match_layout_.width,
        match_layout_.height,
        match_layout_.left
    );
    return oa::ui::display_layout::Rect{x, y, width, height};
}

bool Runtime::click_unit_info(float x, float y) {
    if (!unit_info_panel_ || !unit_info_panel_->screen)
        return false;
    const auto& root = unit_info_panel_->root;
    const auto column = static_cast<int>(x);
    const auto row = static_cast<int>(y);
    // The point in the panel's own pixels: on the phone layout through the
    // region it shows in, elsewhere through where it shows over the
    // battlefield.
    auto point = oa::ui::display_layout::canvas_to_source(match_layout_, column, row);
    if (!oa::ui::display_layout::placed_mode(match_layout_)) {
        const auto area = unit_info_area();
        if (!area || column < area->x || row < area->y || column >= area->x + area->width ||
            row >= area->y + area->height)
            return false;
        point.x = root.x + (column - area->x) * root.width / area->width;
        point.y = root.y + (row - area->y) * root.height / area->height;
    }
    if (point.x < root.x || point.y < root.y || point.x >= root.x + root.width ||
        point.y >= root.y + root.height)
        return false;
    for (const auto& gadget : unit_info_panel_->screen->layout.gadgets) {
        const auto& control = gadget.common;
        if (control.name == kUnitInfoDone && point.x >= control.x && point.y >= control.y &&
            point.x < control.x + control.width && point.y < control.y + control.height) {
            press_unit_info_done();
            break;
        }
    }
    return true;
}

void Runtime::draw_unit_info_panel() {
    // On the phone layout the panel shows through its placed region
    // (refresh_placed_hud_regions), over the touch controls.
    if (oa::ui::display_layout::placed_mode(match_layout_))
        return;
    if (!unit_info_panel_ || unit_info_panel_->frame.rgb.empty())
        return;
    const auto area = unit_info_area();
    if (!area)
        return;
    // The panel keeps the interface's scale, centred right of the side
    // column as 3.1c centres it right of the HUD strip on its screen.
    const auto& root = unit_info_panel_->root;
    const auto at = canvas_paint(area->x, area->y);
    scale_blit(
        paint_target(),
        unit_info_panel_->frame,
        at.x,
        at.y,
        area->width,
        area->height,
        root.x,
        root.y,
        root.width,
        root.height
    );
}

void Runtime::draw_chat_overlay() {
    draw_console_clock();
    draw_match_message_log();
    draw_debug_status_line();
    draw_frame_stats();
}

void Runtime::ensure_talk_panel() {
    if (talk_layout_)
        return;
    try {
        const auto bytes = assets_.read(oa::data::defs::gui_path("talk.gui")).bytes;
        auto parsed =
            oa::ui::gui_layout::parse(bytes, oa::ui::gui_layout::game_translation_lookup());
        if (!parsed.ok())
            throw std::runtime_error(
                parsed.error ? parsed.error->message : "TALK.GUI parse failed"
            );
        talk_layout_ = std::move(*parsed.layout);
    } catch (const std::exception& error) {
        std::cerr << "TALK.GUI unavailable: " << error.what() << '\n';
        talk_layout_.emplace();
        return;
    }
    append_gaf_file(match_talk_, "anims/talk.gaf");
}

namespace {

/// The columns between the chat line's box and its text, and the rows above
/// and below the TALK field the line may take, or that a line risen over
/// the battlefield keeps clear above and below it, in source pixels.
constexpr int kChatTextInset = 2;
/// The palette colour of the chat line's text.
constexpr uint8_t kChatTextColor = 255;

} // namespace

std::optional<Runtime::HudRect> Runtime::chat_text_box() {
    ensure_talk_panel();
    if (!talk_layout_ || talk_layout_->gadgets.empty())
        return std::nullopt;
    const auto& root = talk_layout_->gadgets.front().common;
    const int origin_x = root.x;
    const int origin_y = root.y < 0 ? kCanvasHeight + root.y : root.y;
    for (const auto& gadget : talk_layout_->gadgets) {
        const auto& common = gadget.common;
        if (&common != &root && common.type == oa::ui::gui_layout::GadgetType::text_box)
            return HudRect{origin_x + common.x, origin_y + common.y, common.width, common.height};
    }
    return std::nullopt;
}

std::optional<oa::present::TextLayers> Runtime::chat_line_layers(
    int scale, int width, std::optional<oa::present::TextUnderline>* composition
) {
    if (composition != nullptr)
        composition->reset();
    const oa::formats::fnt::Font* font = match_label_font();
    if (font == nullptr)
        return std::nullopt;
    const std::string line = typed_game_text(chat_buffer_ + chat_composition_) + "_";
    const auto runs = renderer::split_game_text(line, renderer::fnt_font_characters(*font), true);
    if (runs.size() != 1 || !runs.front().modern)
        return std::nullopt;
    // The line being typed shows its end, the cursor with it, when it is
    // wider than its box; its box is its background.
    const auto& run = runs.front();
    const auto face = renderer::fnt_font_face(*font);
    const int32_t room = width - 2 * oa::present::text_border(scale, run.size);
    const std::size_t from = oa::present::modern_text_tail(run.text, face, scale, run.size, room);
    const std::string_view shown = std::string_view(run.text).substr(from);
    // The composition follows the typed text in the line, as much of it as
    // the line shows.
    if (composition != nullptr && !chat_composition_.empty()) {
        const bool utf8 = game_text_utf8();
        const std::size_t typed =
            oa::present::decode_game_text(typed_game_text(chat_buffer_), utf8).size();
        const std::size_t composed =
            typed + oa::present::decode_game_text(typed_game_text(chat_composition_), utf8).size();
        if (composed > from)
            *composition = oa::present::modern_text_underline(
                shown, face, scale, run.size, std::max(typed, from) - from, composed - from
            );
    }
    return oa::present::modern_text(shown, face, scale, run.size, false);
}

void Runtime::underline_typed_composition(
    const oa::formats::fnt::Font& font,
    int x,
    int y,
    std::string_view before,
    std::string_view composition,
    int scale
) {
    if (composition.empty())
        return;
    // On the line's last row, under the composition's characters.
    const int left = x + match_text_width(font, typed_game_text(before), scale);
    const int width = match_text_width(font, typed_game_text(composition), scale);
    const int row = y + (static_cast<int>(oa::formats::fnt::line_height(font)) - 1 -
                         oa::formats::fnt::row_lift(font)) *
                            scale;
    fill_hud_rect(left, row, width, scale, kChatTextColor);
}

void Runtime::draw_chat_entry() {
    if (!chat_composing_)
        return;
    // With the touch controls on, the line stands over the battlefield in
    // the overlays' area instead (draw_risen_chat_line), on a phone, whose
    // HUD shows no bottom bar, and on a tablet alike.
    if (touch_controls_active())
        return;
    ensure_talk_panel();
    if (talk_layout_->gadgets.empty())
        return;
    const auto& root = talk_layout_->gadgets.front().common;
    const int origin_x = root.x;
    const int origin_y = root.y < 0 ? kCanvasHeight + root.y : root.y;
    for (const auto& gadget : talk_layout_->gadgets) {
        const auto& common = gadget.common;
        if (&common == &root)
            continue;
        const int x = origin_x + common.x;
        const int y = origin_y + common.y;
        if (common.type == oa::ui::gui_layout::GadgetType::text_box) {
            // In the modern fonts the line is drawn at the text size,
            // centred on the box while the box and the rows above and
            // below it hold it; a taller line rises over the battlefield
            // (draw_risen_chat_line).
            // The input method's composition is underlined.
            std::optional<oa::present::TextUnderline> composition;
            if (const auto layers =
                    chat_line_layers(1, common.width - 2 * kChatTextInset, &composition)) {
                if (layers->height <= common.height + 2 * kChatTextInset) {
                    const int baseline =
                        y + (common.height - layers->height) / 2 + layers->baseline;
                    std::ignore = paint_modern_text(
                        *layers, x + kChatTextInset, baseline, palette_rgb(kChatTextColor)
                    );
                    if (composition)
                        fill_hud_rect(
                            x + kChatTextInset + composition->left,
                            baseline + composition->row,
                            composition->width,
                            composition->thickness,
                            kChatTextColor
                        );
                }
                continue;
            }
            const oa::formats::fnt::Font* font = match_label_font();
            const int text_h =
                font != nullptr ? static_cast<int>(oa::formats::fnt::line_height(*font)) : 0;
            draw_hud_label(
                x + kChatTextInset,
                y + (common.height - text_h) / 2,
                typed_game_text(chat_buffer_ + chat_composition_) + "_",
                kChatTextColor
            );
            if (font != nullptr)
                underline_typed_composition(
                    *font,
                    x + kChatTextInset,
                    y + (common.height - text_h) / 2,
                    chat_buffer_,
                    chat_composition_,
                    1
                );
            continue;
        }
        // CONSOLE is the TALK.GAF picture of the same name.
        const auto* sequence = gaf_sequence(match_talk_, common.name);
        if (sequence == nullptr || sequence->frames.empty())
            continue;
        if (const auto rendered = oa::formats::gaf::render_normal(sequence->frames.front());
            rendered.ok())
            blit_gaf_source(*rendered.frame, x, y);
    }
}

void Runtime::draw_risen_chat_line() {
    if (!chat_composing_)
        return;
    // With the touch controls on, every line stands over the battlefield
    // (draw_chat_entry leaves the TALK field alone then); without them only
    // a line taller than the TALK field and the rows round it rises.
    const bool touch = touch_controls_active();
    std::optional<HudRect> box;
    if (!touch) {
        box = chat_text_box();
        if (!box)
            return;
        const auto in_box = chat_line_layers(1, box->width - 2 * kChatTextInset);
        if (!in_box || in_box->height <= box->height + 2 * kChatTextInset)
            return;
    }
    // Across the overlays' area (the battlefield, or with the touch controls
    // on the part of it they leave clear), standing on its bottom edge, the
    // text from the column the field's text starts at, or from the area's
    // left with the touch controls on; in canvas pixels.
    namespace layout = oa::ui::display_layout;
    const int scale = hud_text_scale();
    const auto area = overlay_area();
    const int left = area.x;
    const int right = area.x + area.width;
    const int inset = kChatTextInset * scale;
    int pen = left + inset;
    if (box) {
        const auto field = layout::source_to_canvas(
            match_layout_, box->x + kChatTextInset, layout::kSourceBottomBarY
        );
        pen = std::clamp(field.x, left + inset, right);
    }
    std::optional<oa::present::TextUnderline> composition;
    const auto layers = chat_line_layers(scale, right - pen - inset, &composition);
    if (right <= left)
        return;
    const int bottom = area.y + area.height;
    if (layers) {
        const int top = std::max(bottom - layers->height - 2 * inset, area.y);
        const auto at = canvas_paint(left, top);
        fill_hud_rect(at.x, at.y, right - left, bottom - top, view_rules::chat_backdrop_color);
        const int baseline = at.y + inset + layers->baseline;
        std::ignore =
            paint_modern_text(*layers, at.x + pen - left, baseline, palette_rgb(kChatTextColor));
        // The input method's composition is underlined.
        if (composition)
            fill_hud_rect(
                at.x + pen - left + composition->left,
                baseline + composition->row,
                composition->width,
                composition->thickness,
                kChatTextColor
            );
        return;
    }
    // The game's fonts draw the line: with the touch controls on it stands
    // in the overlays' area all the same, as much of its end as fits, the
    // cursor after it, on the same box.
    const oa::formats::fnt::Font* font = touch ? match_label_font() : nullptr;
    if (font == nullptr)
        return;
    const std::string typed = chat_buffer_ + chat_composition_;
    const int room = right - pen - inset;
    std::size_t from = 0;
    std::string line = typed_game_text(typed) + "_";
    while (from < typed.size() && match_text_width(*font, line, scale) > room) {
        const auto character = oa::present::utf8_sequence(std::string_view(typed).substr(from));
        from += std::max<std::size_t>(character.bytes, 1);
        line = typed_game_text(std::string_view(typed).substr(from)) + "_";
    }
    const int text_height = static_cast<int>(oa::formats::fnt::line_height(*font)) * scale;
    const int top = std::max(bottom - text_height - 2 * inset, area.y);
    const auto at = canvas_paint(left, top);
    fill_hud_rect(at.x, at.y, right - left, bottom - top, view_rules::chat_backdrop_color);
    draw_match_text(font, at.x + pen - left, at.y + inset, line, kChatTextColor, scale);
    // The input method's composition, as much of it as the line shows, is
    // underlined.
    const std::size_t composed = std::max(chat_buffer_.size(), from);
    if (!chat_composition_.empty() && composed < typed.size())
        underline_typed_composition(
            *font,
            at.x + pen - left,
            at.y + inset,
            std::string_view(typed).substr(from, composed - from),
            std::string_view(typed).substr(composed),
            scale
        );
}

namespace {

// A release this many game ticks or fewer after the
// press, over a box under this many map pixels across in x and z, counts as a click.
constexpr uint32_t kDragClickTicks = 0x19;
constexpr int32_t kDragClickPixels = 0x20;
// UI colours of the drag box's outer and inset outlines.
constexpr uint8_t kDragBoxOuterColor = 15;
constexpr uint8_t kDragBoxInnerColor = 0;

} // namespace

std::optional<std::array<int32_t, 3>> Runtime::match_pointer_ground(float x, float y) {
    if (live_renderer_active()) {
        const auto point = live_ground_point(x, y);
        if (!point)
            return std::nullopt;
        return std::array<int32_t, 3>{(*point)[0] >> 16, (*point)[1] >> 16, (*point)[2] >> 16};
    }
    if (!match_ || !selected_tnt_)
        return std::nullopt;
    const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
    const oa::present::world_renderer::ScreenPoint pointer{
        std::clamp(
            static_cast<int32_t>(x),
            match_layout_.left,
            match_layout_.left + match_layout_.battlefield_width() - 1
        ),
        std::clamp(
            static_cast<int32_t>(y),
            match_layout_.top,
            match_layout_.top + match_layout_.battlefield_height() - 1
        )
    };
    // The map pixel drawn under the pointer.
    const auto map = map_pixel_drawn_at(viewport, pointer);
    if (!map)
        return std::nullopt;
    const oa::sim::unit_movement::Terrain terrain(*selected_tnt_);
    const auto ground = oa::sim::gameplay_input::terrain_intersection(
        terrain,
        static_cast<int32_t>(map->x),
        static_cast<int32_t>(map->y),
        static_cast<int32_t>(selected_tnt_->attribute_width * 16U),
        static_cast<int32_t>(selected_tnt_->attribute_height * 16U)
    );
    const auto whole = [](int32_t fixed) {
        return static_cast<int32_t>(static_cast<int16_t>(fixed >> 16));
    };
    return std::array<int32_t, 3>{whole(ground.x), whole(ground.y), whole(ground.z)};
}

void Runtime::track_match_drag() {
    if (!match_drag_)
        return;
    if (const auto ground = match_pointer_ground(match_pointer_x_, match_pointer_y_))
        match_drag_->end = *ground;
}

std::array<oa::present::world_renderer::ScreenPoint, 2> Runtime::match_drag_corners(
    const oa::present::world_renderer::BattlefieldViewport& viewport
) const {
    if (live_renderer_active() && match_drag_)
        return {
            live_project_world(match_drag_->start[0], match_drag_->start[1], match_drag_->start[2]),
            live_project_world(match_drag_->end[0], match_drag_->end[1], match_drag_->end[2])
        };
    const double scale = viewport.scale == 0.0F ? 1.0 : static_cast<double>(viewport.scale);
    const auto project = [&](const std::array<int32_t, 3>& point) {
        return oa::present::world_renderer::ScreenPoint{
            viewport.destination_x +
                static_cast<int32_t>(
                    std::lround((point[0] - static_cast<int32_t>(viewport.source_x)) * scale)
                ),
            viewport.destination_y +
                static_cast<int32_t>(std::lround(
                    (point[2] - (point[1] >> 1) - static_cast<int32_t>(viewport.source_y)) * scale
                ))
        };
    };
    if (!match_drag_)
        return {};
    return {project(match_drag_->start), project(match_drag_->end)};
}

bool Runtime::match_drag_is_click() const {
    if (!match_drag_)
        return true;
    if (live_renderer_active()) {
        const auto corners = match_drag_corners(live_viewport(match_camera_x_, match_camera_z_));
        return std::abs(corners[0].x - corners[1].x) < kDragClickPixels &&
               std::abs(corners[0].y - corners[1].y) < kDragClickPixels;
    }
    const auto released = static_cast<int32_t>(frontend_tick());
    return released < static_cast<int32_t>(match_drag_->pressed_tick + kDragClickTicks) &&
           std::abs(match_drag_->start[0] - match_drag_->end[0]) < kDragClickPixels &&
           std::abs(match_drag_->start[2] - match_drag_->end[2]) < kDragClickPixels;
}

void Runtime::draw_selection_band(
    oa::present::world_renderer::Surface& destination,
    const oa::present::world_renderer::BattlefieldViewport& viewport
) {
    if (!match_drag_)
        return;
    ensure_ui_colors();
    const auto [from, to] = match_drag_corners(viewport);
    int x1 = std::min(from.x, to.x), y1 = std::min(from.y, to.y);
    int x2 = std::max(from.x, to.x), y2 = std::max(from.y, to.y);
    for (const auto color : {kDragBoxOuterColor, kDragBoxInnerColor}) {
        const auto rgb = ui_color_rgb(color);
        draw_match_line(destination, x1, y1, x2, y1, rgb);
        draw_match_line(destination, x2, y1, x2, y2, rgb);
        draw_match_line(destination, x1, y2, x2, y2, rgb);
        draw_match_line(destination, x1, y1, x1, y2, rgb);
        ++x1;
        ++y1;
        --x2;
        --y2;
    }
}

void Runtime::cycle_selected_primary(bool reverse) {
    if (!match_ || selected_match_unit_ == 0)
        return;
    std::vector<uint16_t> ids;
    for (const auto& slot : match_->world().slots) {
        if (slot.unit != nullptr && (slot.unit->flags & OA_UNIT_FLAG_SELECTED) != 0)
            ids.push_back(slot.unit_index);
    }
    if (ids.size() < 2)
        return;
    auto it = std::find(ids.begin(), ids.end(), selected_match_unit_);
    if (it == ids.end())
        it = ids.begin();
    if (reverse) {
        if (it == ids.begin())
            it = ids.end();
        --it;
    } else {
        ++it;
        if (it == ids.end())
            it = ids.begin();
    }
    selected_match_unit_ = *it;
    apply_match_hud_for_selection();
    status_ = unit_info_name(selected_match_unit_);
}

void Runtime::center_camera_on_unit(uint16_t id) {
    if (!match_ || id == 0)
        return;
    const auto* unit = match_->world().slots[id].unit;
    if (unit == nullptr)
        return;
    set_camera_position(
        static_cast<int32_t>(unit->position[0] >> 16) - visible_map_width() / 2,
        static_cast<int32_t>(unit->position[2] >> 16) - visible_map_height() / 2,
        0
    );
}

void Runtime::apply_match_hud_for_selection() {
    // An open menu, or the outcome, keeps its panel; the menu's closing shows
    // the page for the selection then.
    if (match_paused_)
        return;
    if (selected_match_unit_ == 0) {
        match_build_page_ = 0;
        show_match_orders_page();
        return;
    }
    // The panel opens on the page the unit shows, as 3.1c does: every unit
    // of a type with a build page shows its first one from its creation, a
    // missile silo's weapon page as well as a factory's, and ORDERS, BUILD
    // and the page keys change what it shows. A type with no build pages
    // shows the general page, and so do several units selected, with BUILD
    // and ORDERS greyed.
    const auto page = match_panel_page();
    if (page == 0) {
        show_match_orders_page();
        return;
    }
    // The unit's page opens as it is, not as a step from the page shown.
    open_match_build_page(page);
}

} // namespace oa::app
