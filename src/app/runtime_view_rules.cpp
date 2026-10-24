// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The profile's display rules (the ui.* hacks) where the runtime carries
// them out: the screenshot keys and their named files.
#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/view_rules.hpp"
#include "device_state.hpp"
#include "match_models.hpp"
#include "oa/platform/files.hpp"
#include "oa/formats/objects3d.hpp"
#include "oa/present/pcx.hpp"
#include "oa/sim/messages.hpp"
#include "oa/sim/selection.hpp"
#include "oa/ui/console/console.hpp"
#include "oa/ui/frontend/savegame_dialogs.hpp"
#include "oa/sim/spatial_state/spatial.hpp"

#include <SDL3/SDL.h>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <tuple>
#include <utility>
#include <string>
#include <system_error>

namespace oa::app {

namespace {

/// A quarter turn in angle words: the heading a facing adds to a building.
constexpr uint32_t quarter_turn = 0x4000;
/// The heading a building is built at before its facing turns it: half a
/// turn, give or take half its type's build angle.
constexpr uint32_t building_heading = 0x8000;
/// The interface sound a change of build facing plays.
constexpr std::string_view rotate_sound = "MORE";
/// The preferences section the player's display-rule settings are kept in.
constexpr std::string_view view_settings_section = "Eye";
/// The most bytes a stored display-rule text setting holds.
constexpr std::size_t view_settings_text_capacity = 1024;
/// The screenshots folder below the Image Output Directory, which in the
/// player's own folder is its Screenshots folder (Runtime::game_file_path).
constexpr const char* screenshot_folder = "\\screenshots";
/// The date that begins a screenshot's name, as the C library's date for
/// the clock's local time shows it, and its separator.
constexpr const char* screenshot_date_format = "%02d/%02d/%02d - ";

/// Tells whether a key event is one of the named screenshot keys: Ctrl+F9
/// or Print Screen.
///
/// @param key the key event
/// @return true for either key
bool screenshot_key(const SDL_KeyboardEvent& key) {
    const bool f9 = key.key == SDLK_F9 || key.scancode == SDL_SCANCODE_F9;
    const bool print = key.key == SDLK_PRINTSCREEN || key.scancode == SDL_SCANCODE_PRINTSCREEN;
    return print || (f9 && (key.mod & SDL_KMOD_CTRL) != 0);
}

} // namespace

void Runtime::load_view_settings() {
    const auto* profile = mod_profile();
    const oa::data::match_rules::OrdersConPatrolGuardOptions base_builders{};
    view_rules::ViewSettingsStore store{};
    store.number = [this](std::string_view name) {
        return read_number(view_settings_section, name);
    };
    store.text = [this](std::string_view name) {
        return read_string(view_settings_section, name, view_settings_text_capacity);
    };
    view_settings_ = view_rules::read_view_settings(
        ui_rules(),
        profile != nullptr ? profile->rules.orders.con_patrol_guard_options : base_builders,
        store
    );
}

void Runtime::save_view_settings() {
    view_rules::write_view_settings(
        view_settings_,
        [this](std::string_view name, uint32_t value) {
            write_number(view_settings_section, name, value);
        },
        [this](std::string_view name, std::string_view value) {
            write_string(view_settings_section, name, value);
        }
    );
    flush_preferences();
}

bool Runtime::snap_override_held() const {
    return view_key_held(view_settings_.snap_override_key);
}

bool Runtime::view_key_held(uint32_t code) const {
    const auto key = static_cast<SDL_Keycode>(code);
    const auto modifiers = input_modifiers(ModifierUse::keyboard);
    // Either Alt, Ctrl or Shift key stands for its kind.
    if (key == SDLK_LALT || key == SDLK_RALT)
        return (modifiers & SDL_KMOD_ALT) != 0;
    if (key == SDLK_LCTRL || key == SDLK_RCTRL)
        return (modifiers & SDL_KMOD_CTRL) != 0;
    if (key == SDLK_LSHIFT || key == SDLK_RSHIFT)
        return (modifiers & SDL_KMOD_SHIFT) != 0;
    return device_state::key_held(SDL_GetScancodeFromKey(key, nullptr));
}

std::optional<Runtime::PendingBuildSite> Runtime::snapped_build_site(float x, float y) const {
    const auto& snap = ui_rules().click_snap;
    const auto radius = view_settings_.mex_snap_radius;
    if (!snap.enabled || radius <= 0 || !match_ || pending_build_type_ == 0 ||
        pending_build_type_ >= offline_type_fields_.size() || snap_override_held())
        return std::nullopt;
    const auto cursor = build_cursor_point(x, y);
    const auto site = cursor ? pending_build_site(*cursor) : std::nullopt;
    if (!site)
        return std::nullopt;
    const auto& fields = offline_type_fields_[pending_build_type_];
    const int32_t width = site->footprint_x;
    const int32_t height = site->footprint_z;
    // The cursor's cell is the footprint's middle one.
    const std::array<int32_t, 2> middle{site->cell_x + width / 2, site->cell_z + height / 2};
    const double cursor_x = static_cast<double>((*cursor)[0] >> 16);
    const double cursor_z = static_cast<double>((*cursor)[2] >> 16);
    const auto& spatial = match_->spatial();
    const auto map_width = static_cast<int32_t>(spatial.terrain_width);
    const auto map_height = static_cast<int32_t>(spatial.terrain_height);
    // A metal spot is a cell holding more metal than the map's surface metal.
    const auto metal_cells = [&](int32_t centre_x, int32_t centre_z) {
        int32_t count = 0;
        for (int32_t dx = view_rules::footprint_first_offset(width);
             dx <= view_rules::footprint_last_offset(width);
             ++dx)
            for (int32_t dz = view_rules::footprint_first_offset(height);
                 dz <= view_rules::footprint_last_offset(height);
                 ++dz) {
                const auto cell_x = centre_x + dx;
                const auto cell_z = centre_z + dz;
                if (cell_x < 0 || cell_z < 0 || cell_x >= map_width || cell_z >= map_height)
                    continue;
                const auto& plot = spatial.plots
                                       [static_cast<std::size_t>(cell_z) * spatial.terrain_width +
                                        static_cast<std::size_t>(cell_x)];
                if (static_cast<int32_t>(plot.metal) > configured_map_metal_)
                    ++count;
            }
        return count;
    };
    // The site whose middle cell is the given one.
    const auto site_at = [&](std::array<int32_t, 2> centre) {
        const auto first_x = centre[0] - width / 2;
        const auto first_z = centre[1] - height / 2;
        return pending_build_site(
            {(first_x * 2 + width) * 0x80000, 0, (first_z * 2 + height) * 0x80000}
        );
    };
    std::optional<PendingBuildSite> snapped;
    const auto* definition = fields.definition;
    if (definition != nullptr && definition->extracts_metal != 0.0F) {
        if (const auto found =
                view_rules::snap_cell(middle, radius, cursor_x, cursor_z, metal_cells)) {
            const std::array<int32_t, 2> cell{(*found)[0], (*found)[1]};
            if (cell == middle)
                snapped = site_at(cell);
            else if (
                const auto again = view_rules::snap_cell(
                    cell, std::max(width, height), cursor_x, cursor_z, metal_cells
                );
                again && (*again)[0] == cell[0] && (*again)[1] == cell[1]
            )
                // Snapping again from there must agree.
                snapped = site_at(cell);
        }
    }
    if (view_rules::yard_has_geothermal_cell(fields.yard_mask)) {
        const auto buildable = [&](int32_t centre_x, int32_t centre_z) {
            const auto placed = match_->building_site(
                pending_build_type_,
                centre_x - width / 2,
                centre_z - height / 2,
                0,
                match_local_player_
            );
            return placed ? 1 : 0;
        };
        snapped.reset();
        if (const auto found =
                view_rules::snap_cell(middle, radius, cursor_x, cursor_z, buildable)) {
            const std::array<int32_t, 2> cell{(*found)[0], (*found)[1]};
            // Only a move snaps the click.
            if (cell != middle)
                snapped = site_at(cell);
        }
    }
    return snapped;
}

std::optional<oa::sim::ground_orders::Point>
Runtime::snapped_reclaim_point(const oa::sim::ground_orders::Point& ground) const {
    const auto& snap = ui_rules().click_snap;
    const auto radius = view_settings_.wreck_snap_radius;
    if (!snap.enabled || radius <= 0 || !match_ || snap_override_held())
        return std::nullopt;
    const auto& world = match_->state();
    // A click on a feature, where it is drawn, needs no snap.
    if (match_->feature_word_under(feature_point_under_pointer(ground), nullptr, nullptr) !=
        oa::sim::spatial_state::no_feature)
        return std::nullopt;
    const auto& spatial = match_->spatial();
    const auto map_width = static_cast<int32_t>(spatial.terrain_width);
    const auto map_height = static_cast<int32_t>(spatial.terrain_height);
    const std::array<int32_t, 2> cell{ground[0] >> 20, ground[2] >> 20};
    const auto reclaimable = [&](int32_t cell_x, int32_t cell_z) {
        if (cell_x < 0 || cell_z < 0 || cell_x >= map_width || cell_z >= map_height)
            return 0;
        const oa::sim::ground_orders::Point middle{
            (cell_x * 16 + 8) << 16, 0, (cell_z * 16 + 8) << 16
        };
        const auto word = match_->feature_word_under(middle, nullptr, nullptr);
        if (word >= world.feature_def_count)
            return 0;
        const auto& def = world.feature_defs[word];
        const bool worth = def.metal > 0.0F || def.energy > 0.0F;
        return worth && (def.flags & OA_FEATURE_FLAG_RECLAIMABLE) != 0 ? 1 : 0;
    };
    const auto found = view_rules::snap_cell(
        cell,
        radius,
        static_cast<double>(ground[0] >> 16),
        static_cast<double>(ground[2] >> 16),
        reclaimable
    );
    if (!found)
        return std::nullopt;
    return feature_reclaim_point(
        {((*found)[0] * 16 + 8) << 16, ground[1], ((*found)[1] * 16 + 8) << 16}
    );
}

bool Runtime::build_tool_click(float x, float y) {
    if (!ui_rules().build_tools.enabled || !match_ || match_command_ != MatchCommand::build ||
        pending_build_type_ == 0 || !view_key_held(view_settings_.autoclick_key))
        return false;
    auto& tool = build_tool_;
    if (tool.ring) {
        give_build_tool_orders();
        return true;
    }
    const auto cursor = build_cursor_point(x, y);
    if (!cursor)
        return true;
    const int32_t cursor_x = (*cursor)[0] >> 16;
    const int32_t cursor_z = (*cursor)[2] >> 16;
    if (pending_build_type_ < spawn_types_.size()) {
        const auto footprint = pending_build_footprint();
        tool.footprint_x = footprint[0];
        tool.footprint_z = footprint[1];
    }
    if (tool.drawing_line) {
        // The line drawn so far is given, and the next starts here.
        tool.end_x = cursor_x;
        tool.end_z = cursor_z;
        lay_build_tool();
        give_build_tool_orders();
    }
    tool.drawing_line = true;
    // A snapped click starts the line at the snapped site's middle cell.
    if (const auto snapped = snapped_build_site(x, y)) {
        tool.start_x = (snapped->cell_x + snapped->footprint_x / 2) * 16;
        tool.start_z = (snapped->cell_z + snapped->footprint_z / 2) * 16;
    } else {
        tool.start_x = cursor_x;
        tool.start_z = cursor_z;
    }
    tool.end_x = cursor_x;
    tool.end_z = cursor_z;
    lay_build_tool();
    return true;
}

void Runtime::build_tool_motion(float x, float y) {
    auto& tool = build_tool_;
    if (!ui_rules().build_tools.enabled || !match_ || match_command_ != MatchCommand::build ||
        pending_build_type_ == 0) {
        tool.drawing_line = false;
        tool.ring = false;
        return;
    }
    if (tool.drawing_line) {
        if (const auto cursor = build_cursor_point(x, y)) {
            tool.end_x = (*cursor)[0] >> 16;
            tool.end_z = (*cursor)[2] >> 16;
            lay_build_tool();
        }
        return;
    }
    tool.ring = false;
    if (!view_key_held(view_settings_.autoclick_key) ||
        on_screen_units_.size() != match_->state().unit_slot_count)
        return;
    const auto unit = oa::sim::selection::unit_under_pointer(
        match_->state(), on_screen_lists(), selection_hooks()
    );
    if (unit == 0)
        return;
    tool.ring = true;
    tool.ring_unit = unit;
    if (pending_build_type_ < spawn_types_.size()) {
        const auto footprint = pending_build_footprint();
        tool.footprint_x = footprint[0];
        tool.footprint_z = footprint[1];
    }
    lay_build_tool();
}

std::array<int32_t, 2> Runtime::pending_build_footprint() const {
    std::array<int32_t, 2> footprint{1, 1};
    if (pending_build_type_ < spawn_types_.size()) {
        footprint[0] = std::max<int32_t>(spawn_types_[pending_build_type_].footprint_x, 1);
        footprint[1] = std::max<int32_t>(spawn_types_[pending_build_type_].footprint_z, 1);
    }
    // A building turned east or west lies across.
    if ((static_cast<uint8_t>(pending_build_facing()) & 1U) != 0)
        std::swap(footprint[0], footprint[1]);
    return footprint;
}

bool Runtime::order_drag_pointer(const SDL_Event& event, float x, float y) {
    if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN && event.type != SDL_EVENT_MOUSE_BUTTON_UP)
        return false;
    if (event.button.button != SDL_BUTTON_LEFT)
        return false;
    if (!ui_rules().build_tools.enabled || screen_ != Screen::match || !match_ || match_paused_ ||
        match_finished_ || !snap_override_held()) {
        order_drag_unit_ = 0;
        return false;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        order_drag_unit_ = 0;
        if (hovered_ || !battlefield_contains(x, y) ||
            on_screen_units_.size() != match_->state().unit_slot_count)
            return false;
        const auto unit = oa::sim::selection::unit_under_pointer(
            match_->state(), on_screen_lists(), selection_hooks()
        );
        // Only one of the local player's own units with a movement object.
        const auto& world = match_->state();
        const auto* picked = unit != 0 ? oa::world_unit_at(&world, unit) : nullptr;
        if (picked == nullptr || picked->movement == 0 ||
            picked->owner_index != world.game.local_player_index)
            return false;
        order_drag_unit_ = unit;
        return true;
    }
    const auto unit = std::exchange(order_drag_unit_, uint16_t{0});
    if (unit == 0)
        return false;
    // The unit goes to the terrain under the pointer ahead of its orders.
    if (const auto point = build_cursor_point(x, y))
        std::ignore = match_->send_ahead_of_orders(unit, *point);
    return true;
}

uint8_t Runtime::pending_build_facings() const {
    if (!match_ || pending_build_type_ == 0)
        return oa::data::match_rules::build_facing::south;
    return match_->build_facings(pending_build_type_);
}

view_rules::BuildFacing Runtime::pending_build_facing() const {
    if (!ui_rules().build_preview.enabled ||
        !view_rules::facing_allowed(pending_build_facings(), build_facing_))
        return view_rules::BuildFacing::south;
    return build_facing_;
}

bool Runtime::rotate_pending_build(int32_t direction) {
    if (!ui_rules().build_preview.enabled || !match_ || match_command_ != MatchCommand::build ||
        pending_build_type_ == 0)
        return false;
    const auto next =
        view_rules::next_build_facing(pending_build_facings(), pending_build_facing(), direction);
    if (next == pending_build_facing())
        return false;
    build_facing_ = next;
    // The preview's pulse starts again.
    build_turn_phase_ = match_->simulation().tick % view_rules::build_preview_pulse_ticks;
    play_match_interface_sound(rotate_sound);
    if (!view_settings_.rotate_key_discovered) {
        view_settings_.rotate_key_discovered = true;
        save_view_settings();
    }
    lay_build_tool();
    return true;
}

bool Runtime::ready_build_preview(MatchModels& models) {
    auto& preview = models.build_preview;
    const auto& rules = ui_rules().build_preview;
    if (!rules.enabled || !match_ || match_command_ != MatchCommand::build ||
        pending_build_type_ == 0 || pending_build_type_ >= loaded_commander_types_.size() ||
        build_tool_.drawing_line || build_tool_.ring)
        return false;
    auto site = build_site_under(match_pointer_x_, match_pointer_y_);
    if (auto snapped = snapped_build_site(match_pointer_x_, match_pointer_y_))
        site = snapped;
    const auto& loaded = loaded_commander_types_[pending_build_type_];
    // Over a site the game refuses the footprint's outline shows alone.
    if (!site || !site->legal || !loaded.model)
        return false;
    auto facing = pending_build_facing();
    // A type that faces its opponent is drawn turned toward the nearest
    // enemy, as the finished defence would turn; its build order keeps the
    // facing the player chose.
    if (pending_build_type_ < unit_preview_keys_.size() &&
        unit_preview_keys_[pending_build_type_].face_opponent)
        if (const auto toward = view_rules::opponent_facing(
                match_->state(),
                site->cell_x * 16 + site->footprint_x * 8,
                site->cell_z * 16 + site->footprint_z * 8
            ))
            facing = *toward;
    if (preview.type != pending_build_type_ || preview.facing != facing) {
        // A type whose preview keys name another model shows that one.
        std::shared_ptr<const oa::formats::objects3d::Model> shown = loaded.model;
        if (pending_build_type_ < unit_preview_keys_.size() &&
            unit_preview_keys_[pending_build_type_].object[0] != '\0') {
            const auto path = "objects3d/" +
                              std::string(unit_preview_keys_[pending_build_type_].object.data()) +
                              ".3DO";
            try {
                const auto bytes = assets_.read(path).bytes;
                auto decoded = oa::formats::objects3d::load_3do(std::as_bytes(std::span(bytes)));
                if (decoded.ok())
                    shown = std::make_shared<const oa::formats::objects3d::Model>(
                        std::move(*decoded.value)
                    );
                else
                    std::cerr << "build preview model " << path << ": " << decoded.error.message
                              << '\n';
            } catch (const std::exception& error) {
                // The type's own model stays.
                std::cerr << "build preview model " << path << ": " << error.what() << '\n';
            }
        }
        preview.instance = oa::sim::model_runtime::make_instance(shown);
        preview.state = {};
        preview.type = pending_build_type_;
        preview.facing = facing;
        // The pieces the type's preview keys name for the facing, else for
        // every facing; all of them without either.
        std::string_view list;
        if (pending_build_type_ < unit_preview_keys_.size()) {
            const auto& keys = unit_preview_keys_[pending_build_type_];
            const auto& by_facing = keys.pieces_by_facing[static_cast<std::size_t>(facing)];
            list = by_facing[0] != '\0' ? std::string_view(by_facing.data())
                                        : std::string_view(keys.pieces.data());
        }
        // Without a list, every piece but the muzzle flashes and wakes the
        // unit's script would hide.
        const auto& objects = preview.instance.model().objects;
        for (auto& piece : preview.instance.pieces()) {
            const bool known = piece.object_index < objects.size();
            const bool piece_shown =
                list.empty()
                    ? !known || !view_rules::preview_skips_piece(objects[piece.object_index].name)
                    : known &&
                          view_rules::preview_lists_piece(list, objects[piece.object_index].name);
            if (!piece_shown)
                piece.flags &= static_cast<uint16_t>(
                    ~static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::visible)
                );
        }
    }
    auto& unit = preview.unit;
    unit = {};
    unit.flags = OA_UNIT_FLAG_VIEWPOINT_OWNED | OA_UNIT_FLAG_BUILDING;
    unit.flags2 = OA_UNIT_FLAG2_Z_BUFFER;
    unit.def = oa::oa_ref_from_index(pending_build_type_);
    unit.owner_index = match_local_player_;
    unit.position = {site->world[0], site->world[1], site->world[2]};
    unit.footprint_x = site->footprint_x;
    unit.footprint_z = site->footprint_z;
    // Facing the way the building is built: half a turn from heading 0,
    // then the facing's quarter turns. Its look comes from its pulse
    // (build_preview_bands); it counts as just begun.
    unit.heading =
        static_cast<oa_angle>(building_heading + static_cast<uint32_t>(facing) * quarter_turn);
    unit.build_remaining = 1.0F;
    return true;
}

void Runtime::lay_build_tool() {
    auto& tool = build_tool_;
    if (tool.ring && match_) {
        const auto& world = match_->state();
        const auto* unit = oa::world_unit_at(&world, tool.ring_unit);
        const auto* def = unit != nullptr ? oa::world_unit_def_of(&world, unit) : nullptr;
        if (def == nullptr) {
            tool.ring = false;
            tool.layout.slots.clear();
            return;
        }
        // Around the unit's footprint, widened by the spacing on every side;
        // a building turned east or west lies across (units.build-rotation).
        int32_t width = def->footprint_x;
        int32_t depth = def->footprint_z;
        if ((match_->unit_build_facing(*unit) & 1U) != 0)
            std::swap(width, depth);
        tool.layout.slots = view_rules::ring_build_slots(
            (unit->cell_x - tool.spacing) * 16,
            (unit->cell_z - tool.spacing) * 16,
            width + 2 * tool.spacing,
            depth + 2 * tool.spacing,
            tool.footprint_x,
            tool.footprint_z,
            view_settings_.full_rings
        );
        return;
    }
    if (!tool.drawing_line)
        return;
    // A line of more than the steps allowed keeps the one laid before.
    if (auto line = view_rules::line_build_slots(
            tool.start_x,
            tool.start_z,
            tool.end_x,
            tool.end_z,
            tool.footprint_x,
            tool.footprint_z,
            tool.spacing
        ))
        tool.layout = std::move(*line);
}

void Runtime::give_build_tool_orders() {
    auto layout = build_tool_.layout;
    if (!build_tool_.ring && view_settings_.optimize_dt_rows && build_tool_.footprint_x == 2 &&
        build_tool_.footprint_z == 2)
        view_rules::optimize_dt_rows(layout);
    const auto type = pending_build_type_;
    for (const auto& slot : layout.slots) {
        // Each place is a queued build click there.
        if (pending_build_type_ != type)
            break;
        place_pending_build_at({int32_t{slot.x} << 16, 0, int32_t{slot.z} << 16}, true);
    }
}

bool Runtime::chat_backdrop_shown() const {
    return ui_rules().text_rendering.enabled && view_settings_.chat_backdrop;
}

void Runtime::run_chat_macro() {
    for (const auto& line : view_rules::chat_macro_lines(view_settings_.chat_macro))
        give_view_command(line, false);
}

void Runtime::give_view_command(const std::string& line, bool share) {
    if (!match_ || line.empty())
        return;
    auto& world = match_->state();
    const auto& speaker = world.game.players[world.game.local_player_index % OA_PLAYER_COUNT];
    // Unshared, the local-only mode keeps the line from the other players
    // and the recording.
    const uint8_t mode = world.game.chat_mode;
    if (!share)
        world.game.chat_mode = oa::sim::messages::chat_mode_local_only;
    oa::sim::messages::post_chat(
        world, speaker, line.c_str(), oa::sim::messages::kind_player_chat, nullptr, message_hooks()
    );
    world.game.chat_mode = mode;
    if (line.front() != '+')
        return;
    if (auto* console = match_console()) {
        // The mode the command's echo would go out in is not wanted: the
        // line was shown above.
        std::ignore = oa::ui::console::console_submit_chat_line(
            console, line.c_str(), world.game.chat_mode, nullptr
        );
        match_->effects().emitters_refused = console->sfx_flag;
        keep_console_carry();
    }
}

bool Runtime::handle_view_rule_key(const SDL_Event& event) {
    const auto& ui = ui_rules();
    const bool tools = ui.build_tools.enabled || ui.build_preview.enabled;
    if (event.type == SDL_EVENT_MOUSE_WHEEL)
        return tools && screen_ == Screen::match && match_ && handle_build_tool_key(event);
    if (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP)
        return false;
    // The screenshot keys save when released, in every screen; the game's
    // own Ctrl+F9 never sees them.
    if (ui.display_modes.enabled && screenshot_key(event.key)) {
        if (event.type == SDL_EVENT_KEY_UP)
            capture_named_screenshot();
        return true;
    }
    if (tools && screen_ == Screen::match && match_ && handle_build_tool_key(event))
        return true;
    // F11 sends the chat macro during a match.
    if (ui.options_dialog.enabled && screen_ == Screen::match && match_ &&
        (event.key.key == SDLK_F11 || event.key.scancode == SDL_SCANCODE_F11)) {
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
            run_chat_macro();
        return true;
    }
    return false;
}

bool Runtime::handle_build_tool_key(const SDL_Event& event) {
    const auto& ui = ui_rules();
    auto& tool = build_tool_;
    const auto autoclick = static_cast<SDL_Keycode>(view_settings_.autoclick_key);
    if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        if (match_command_ != MatchCommand::build)
            return false;
        const float turned =
            event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
        // The snap override key with the wheel turns the building, before
        // the autoclick key spaces a line.
        if (ui.build_preview.enabled && snap_override_held()) {
            if (turned != 0.0F)
                std::ignore = rotate_pending_build(turned > 0.0F ? 1 : -1);
            return true;
        }
        if (!ui.build_tools.enabled || !view_key_held(view_settings_.autoclick_key))
            return false;
        if (turned == 0.0F)
            return true;
        tool.spacing = std::clamp(
            tool.spacing + (turned > 0.0F ? 1 : -1), 0, view_rules::build_tool_spacing_limit
        );
        lay_build_tool();
        return true;
    }
    if (ui.build_tools.enabled && event.type == SDL_EVENT_KEY_UP && event.key.key == autoclick) {
        // Letting the key go drops the line or ring without giving it.
        tool.drawing_line = false;
        tool.ring = false;
        tool.layout.slots.clear();
        return false;
    }
    if (event.type != SDL_EVENT_KEY_DOWN || match_command_ != MatchCommand::build)
        return false;
    // The rotate key, without Ctrl, turns the building.
    if (ui.build_preview.enabled &&
        event.key.key == static_cast<SDL_Keycode>(view_settings_.rotate_build_key) &&
        (event.key.mod & SDL_KMOD_CTRL) == 0) {
        std::ignore = rotate_pending_build(1);
        return true;
    }
    if (!ui.build_tools.enabled)
        return false;
    if (event.key.key == SDLK_PAGEUP || event.key.key == SDLK_PAGEDOWN) {
        if (!view_key_held(view_settings_.autoclick_key))
            return false;
        tool.spacing = std::clamp(
            tool.spacing + (event.key.key == SDLK_PAGEUP ? 1 : -1),
            0,
            view_rules::build_tool_spacing_limit
        );
        lay_build_tool();
        return true;
    }
    // The key itself types nothing while it builds.
    return event.key.key == autoclick;
}

void Runtime::capture_named_screenshot() {
    present::DisplayContext capture{};
    present::SurfaceBuffer frame{};
    if (!indexed_frame(capture, frame)) {
        status_ = "error writing screenshot";
        return;
    }
    // The local date as the C library's "%x - " writes it: month, day and
    // the year's last two digits.
    char date[100]{};
    SDL_Time now{};
    SDL_DateTime local{};
    if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &local, true))
        std::snprintf(
            date,
            sizeof date,
            screenshot_date_format,
            local.month,
            local.day,
            ((local.year % 100) + 100) % 100
        );
    std::optional<view_rules::ScreenshotScene> scene;
    if (screen_ == Screen::match && match_) {
        scene.emplace();
        scene->map = skirmish_settings_.map_name;
        const auto& game = match_->state().game;
        for (std::size_t slot = 0; slot < scene->players.size(); ++slot) {
            const auto& player = game.players[slot];
            if (player.in_use != 0)
                scene->players[slot] =
                    std::string(player.name, strnlen(player.name, sizeof player.name));
        }
    }
    // Where Ctrl+F9's screenshots go: the Image Output Directory's
    // screenshots folder, the match's own when the console changed it.
    std::string output = preferences_.image_output_directory;
    if (screen_ == Screen::match && match_) {
        const oa::ui::console::Console* con = match_console();
        output = oa::ui::console::output_directory(
            match_->state().game, con != nullptr ? con->host : nullptr
        );
    }
    const auto folder =
        game_file_path(output + screenshot_folder, ui::frontend::SavePathUse::write);
    // An output directory that would leave the save root has no host path.
    if (folder.empty()) {
        status_ = "error writing screenshot";
        return;
    }
    std::error_code error;
    fs::create_directories(folder, error);
    fs::path path;
    for (int32_t index = 0;; ++index) {
        path = folder / view_rules::screenshot_file_name(date, scene ? &*scene : nullptr, index);
        if (!fs::exists(path, error))
            break;
    }
    std::FILE* file = oa::platform::open_file(path, "wb");
    if (file == nullptr) {
        status_ = "error writing screenshot";
        return;
    }
    present::ByteStream stream{};
    stream.user = file;
    stream.write = [](void* user, const void* data, int32_t size) {
        return static_cast<int32_t>(
            std::fwrite(data, 1, static_cast<std::size_t>(size), static_cast<std::FILE*>(user))
        );
    };
    stream.seek = [](void* user, int32_t position) {
        return static_cast<int32_t>(std::fseek(static_cast<std::FILE*>(user), position, SEEK_SET));
    };
    stream.tell = [](void* user) {
        return static_cast<int32_t>(std::ftell(static_cast<std::FILE*>(user)));
    };
    const auto saved = present::save_pcx_surface(stream, capture, *capture.active_surface);
    const bool closed = std::fclose(file) == 0;
    status_ = saved == present::PcxStatus::ok && closed ? path_to_utf8(path.filename())
                                                        : std::string("error writing screenshot");
}

} // namespace oa::app
