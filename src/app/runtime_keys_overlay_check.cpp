// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-keys-overlay: a skirmish with the commander selected. Primary+/
// opens the overlay and the clock keeps running, a building being placed
// changes its title, Escape closes it, and EDIT KEYS… opens the editor,
// which holds the clock. The order badges, the underline, the help lines,
// the build cell's badge, the rotate hint and a latched QUEUE in the On line
// follow the bindings. --snapshot writes keys-overlay-<w>x<h>.ppm and
// key-badges-<w>x<h>.ppm at 640x480, 1280x720 and 1920x1080, on OA default
// with QUEUE latched.

#include "oa/app/runtime.hpp"
#include "oa_layer_check.hpp"
#include "touch_state.hpp"

#include "oa/app/view_rules.hpp"
#include "oa/data/key_library/library.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/gui_layout.hpp"
#include "oa/ui/key_editor/overlay.hpp"
#include "oa/ui/key_editor/screen.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <limits>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace editor = oa::ui::key_editor;
namespace oakeys = oa::formats::oakeys;
namespace library = oa::data::key_library;
namespace settings = oa::ui::engine_settings;
namespace first_run = oa::ui::first_run;
namespace layout = oa::ui::display_layout;
namespace hud = oa::ui::touch_hud;

/// Primary on this platform: Command on a Mac, Ctrl elsewhere.
#ifdef SDL_PLATFORM_MACOS
constexpr SDL_Keymod kPrimaryModifier = SDL_KMOD_LGUI;
#else
constexpr SDL_Keymod kPrimaryModifier = SDL_KMOD_LCTRL;
#endif

/// Window sizes the snapshots are taken at, wide then tall.
constexpr int32_t kWidths[] = {640, 1280, 1920};
constexpr int32_t kHeights[] = {480, 720, 1080};

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("keys overlay: " + std::string(what));
}

/// Names one step on standard output.
///
/// @param what the step
void say_step(std::string_view what) {
    std::cout << "keys overlay: " << what << std::endl;
}

/// Writes an own layout of OA default in which a tap of Shift latches
/// QUEUE. An empty chord unbinds the action.
///
/// @param user_folder the player's folder
/// @param rows each action and its chord; an empty chord unbinds it
void write_own_layout(
    const fs::path& user_folder, const std::vector<std::pair<std::string, std::string>>& rows
) {
    oakeys::Manifest manifest;
    manifest.format = oakeys::format_version;
    manifest.id = "own-oa-default";
    manifest.name = "Your changes";
    manifest.version.major = 1;
    manifest.version.minor = 0;
    manifest.version.parts = 2;
    manifest.author.name = "You";
    manifest.tags = {"own"};
    manifest.base = oakeys::Base::oa_default;
    // Shift latches QUEUE, so the On line has a chip to show.
    manifest.helpers.toggle = std::vector<std::string>{"order.queue-modifier"};
    for (const auto& [action, chord] : rows) {
        oakeys::BindingEntry binding;
        binding.action = action;
        if (!chord.empty())
            binding.chords.push_back(oakeys::ChordText{chord, 0, 0});
        manifest.bindings.push_back(std::move(binding));
    }
    const fs::path own_dir = user_folder / std::string(library::keys_folder_name) /
                             library::own_folder_name("oa-default");
    std::error_code made;
    fs::create_directories(own_dir, made);
    require(!made, "the own layout folder was not made");
    const fs::path own_file = own_dir / std::string(oakeys::manifest_file);
    std::ofstream out(own_file, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(out), "the own layout was not opened");
    out << oakeys::write_manifest(manifest);
    require(static_cast<bool>(out), "the own layout was not written");
}

/// The own layout's file, whether or not it has been written.
///
/// @param user_folder the player's folder
/// @return the manifest path
fs::path own_layout_file(const fs::path& user_folder) {
    return user_folder / std::string(library::keys_folder_name) /
           library::own_folder_name("oa-default") / std::string(oakeys::manifest_file);
}

} // namespace

void Runtime::check_keys_overlay() {
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the SDL presenter");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );
    require(!options_.mod_dir.empty(), "needs --mod-dir so a building can turn");

    const fs::path own_file = own_layout_file(user_folder_);
    const fs::path unit_file = options_.mod_dir / "units" / "armllt.fbi";
    const settings::EngineSettings saved_settings = engine_settings();
    std::error_code removed;
    fs::remove(own_file, removed);
    preference_values_[std::string(first_run::preference_key)] =
        std::string(first_run::preference_done);
    preference_values_[std::string(settings::key::language)] = "en";
    preferences_dirty_ = true;
    save_preferences();
    set_language_choice("en");
    {
        auto chosen = engine_settings();
        chosen.show_keys_on_buttons = true;
        chosen.sticky_modifiers = false;
        apply_engine_settings(chosen);
    }

    // A light laser tower faces only south until its rules name more
    // facings. The mod's own copy of its rules, written into the mod's
    // folder for this run, adds them.
    {
        const char* names[] = {"units/armllt.fbi", "units/ARMLLT.FBI"};
        oa::AssetData loaded;
        bool found = false;
        for (const char* name : names) {
            try {
                loaded = assets_.read(name);
            } catch (const std::exception&) {
                continue;
            }
            if (!loaded.bytes.empty()) {
                found = true;
                break;
            }
        }
        require(found, "the game has no light laser tower to give facings");
        std::string text(loaded.bytes.begin(), loaded.bytes.end());
        if (text.find("Rotations=") == std::string::npos) {
            const std::size_t brace = text.find('{');
            require(brace != std::string::npos, "the tower's rules have no body");
            text.insert(brace + 1, "\n\tRotations=SENW;");
        }
        std::error_code made;
        fs::create_directories(unit_file.parent_path(), made);
        require(!made, "the mod's units folder was not made");
        std::ofstream unit_out(unit_file, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(unit_out), "the tower's rules were not opened");
        unit_out.write(text.data(), static_cast<std::streamsize>(text.size()));
        require(static_cast<bool>(unit_out), "the tower's rules were not written");
        unit_out.close();
        assets_.mark_loose_shadows();
    }

    bool running = true;
    const auto frames = [&](int32_t count) {
        for (int32_t frame = 0; frame < count; ++frame) {
            run_frame(running);
            require(running, "a frame ended the run");
        }
    };
    const auto resize = [&](int32_t width, int32_t height) {
        require(
            SDL_SetWindowSize(sdl_.window, width, height) && SDL_SyncWindow(sdl_.window),
            "the window did not take the size"
        );
        apply_output_mode();
        frames(2);
    };
    const auto key_event = [&](bool down, SDL_Keycode code, SDL_Scancode scan, SDL_Keymod mod) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.windowID = SDL_GetWindowID(sdl_.window);
        event.key.key = code;
        event.key.scancode = scan;
        event.key.mod = mod;
        event.key.down = down;
        dispatch_event(event, running);
        require(running, "a key ended the run");
    };
    const auto tap = [&](SDL_Keycode code, SDL_Scancode scan, SDL_Keymod mod) {
        key_event(true, code, scan, mod);
        key_event(false, code, scan, mod);
        frames(1);
    };
    const auto named = [this](std::string_view name) -> std::optional<AutomationControl> {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls)
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        return std::nullopt;
    };
    const auto overlay_open = [this] {
        return oa_layer().find(editor::name_keys_overlay) != nullptr;
    };
    const auto list_text = [&] {
        frames(1);
        const std::optional<AutomationControl> list = named("oa.keys-overlay.list");
        require(list.has_value(), "oa.keys-overlay.list is not shown");
        return list->text;
    };
    const auto press_overlay_key = [&] {
        tap(SDLK_SLASH, SDL_SCANCODE_SLASH, kPrimaryModifier);
        frames(1);
    };
    const auto open_overlay = [&] {
        if (!overlay_open())
            press_overlay_key();
        require(overlay_open(), "Primary+/ did not open the keys overlay");
        require(!match_paused_, "the overlay paused the match");
    };
    const auto close_overlay = [&] {
        if (!overlay_open())
            return;
        tap(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE);
        require(!overlay_open(), "Escape did not close the keys overlay");
        require(!match_paused_, "Escape paused the match");
    };
    const auto pointer_at = [&](layout::Point pixel, SDL_EventType type, uint8_t button) {
        SDL_Event event = window_pointer_event(sdl_.window, sdl_.renderer, type, pixel, button);
        dispatch_event(event, running);
        require(running, "a pointer event ended the run");
    };
    const auto click_named = [&](std::string_view name) {
        const std::optional<AutomationControl> control = named(name);
        require(control.has_value(), std::string(name) + " is not shown");
        const layout::Point pixel{
            control->window_x + control->window_width / 2,
            control->window_y + control->window_height / 2
        };
        pointer_at(pixel, SDL_EVENT_MOUSE_MOTION, 0);
        pointer_at(pixel, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        pointer_at(pixel, SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
        frames(1);
    };
    // A gadget's middle, in window pixels.
    const auto gadget_pixel = [&](std::size_t index) {
        const auto& common = match_hud_->layout.gadgets[index].common;
        const auto rect = layout::source_rect_to_canvas(
            match_layout_, common.x, common.y, common.width, common.height
        );
        float window_x = 0;
        float window_y = 0;
        require(
            frame_to_window(
                sdl_.renderer,
                static_cast<float>(rect.x + rect.width / 2),
                static_cast<float>(rect.y + rect.height / 2),
                &window_x,
                &window_y
            ),
            "the gadget did not map into the window"
        );
        return layout::Point{static_cast<int32_t>(window_x), static_cast<int32_t>(window_y)};
    };
    const auto hover_gadget = [&](std::size_t index) {
        const auto& common = match_hud_->layout.gadgets[index].common;
        pointer_at(gadget_pixel(index), SDL_EVENT_MOUSE_MOTION, 0);
        require(hovered_ == index, "the pointer is not over " + common.name);
    };
    const auto click_gadget = [&](std::size_t index) {
        const layout::Point pixel = gadget_pixel(index);
        pointer_at(pixel, SDL_EVENT_MOUSE_MOTION, 0);
        pointer_at(pixel, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        pointer_at(pixel, SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
        frames(1);
    };
    const auto gadget_named = [&](std::string_view word) {
        require(match_hud_.has_value(), "the match has no order panel");
        for (std::size_t index = 0; index < match_hud_->layout.gadgets.size(); ++index)
            if (match_hud_action(match_hud_->layout.gadgets[index].common.name) == word)
                return index;
        require(false, std::string(word) + " is not on the order panel");
        return std::size_t{0};
    };
    const auto badge_for = [this](std::string_view action) -> std::optional<KeyBadgeDrawn> {
        for (const KeyBadgeDrawn& badge : key_badges_drawn_)
            if (badge.action == action)
                return badge;
        return std::nullopt;
    };
    const auto quick_for = [this](std::string_view action) -> std::optional<QuickKeyApplied> {
        for (const QuickKeyApplied& applied : quick_keys_applied_)
            if (applied.action == action)
                return applied;
        return std::nullopt;
    };
    const auto select_commander = [&] {
        require(match_ != nullptr, "there is no match");
        uint16_t commander = 0;
        for (const auto& slot : match_->world().slots) {
            if (slot.unit_index == 0 || slot.unit == nullptr || slot.record.type_index == 0 ||
                slot.record.owner_index != match_local_player_)
                continue;
            if (const auto* def = definition_for(slot.unit_index);
                def != nullptr && def->can_dgun) {
                commander = slot.unit_index;
                break;
            }
        }
        require(commander != 0, "found no local commander");
        match_command_ = MatchCommand::none;
        pending_build_type_ = 0;
        clear_local_selection();
        adopt_selection(commander);
        selected_match_unit_ = commander;
        apply_match_hud_for_selection();
        frames(1);
    };
    const auto show_orders = [&] {
        if (match_hud_.has_value()) {
            for (std::size_t index = 0; index < match_hud_->layout.gadgets.size(); ++index) {
                if (match_hud_action(match_hud_->layout.gadgets[index].common.name) == "ORDERS") {
                    click_gadget(index);
                    break;
                }
            }
        }
        render_match_surface();
    };
    const auto turning_type = [&] {
        const std::size_t types = spawn_type_names_.size();
        for (std::size_t index = 1; index < types; ++index) {
            if (index > static_cast<std::size_t>(std::numeric_limits<uint16_t>::max()))
                break;
            const auto type = static_cast<uint16_t>(index);
            const uint8_t facings = match_->build_facings(type);
            if (view_rules::next_build_facing(facings, view_rules::BuildFacing::south, 1) !=
                view_rules::BuildFacing::south)
                return type;
        }
        // Say why: the hack, the tower's facings and the rules the game reads.
        std::string why = "no building turns: build rotation ";
        const oa::data::mod_profile::ModProfile* profile = mod_profile();
        why += profile != nullptr && profile->rules.units.build_rotation.enabled ? "on" : "off";
        why += ", key \"" +
               (profile != nullptr ? profile->data_keys.units_build_facings : std::string{}) +
               "\", rules for " + std::to_string(unit_type_rules_.size()) + " types";
        for (std::size_t index = 1; index < types; ++index) {
            if (index <= static_cast<std::size_t>(std::numeric_limits<uint16_t>::max()) &&
                (spawn_type_names_[index] == "ARMLLT" || spawn_type_names_[index] == "armllt"))
                why += ", ARMLLT facings " +
                       std::to_string(match_->build_facings(static_cast<uint16_t>(index)));
        }
        try {
            const oa::AssetData read = assets_.read("units/armllt.fbi");
            const std::string text(read.bytes.begin(), read.bytes.end());
            why += text.find("Rotations=") != std::string::npos ? ", rules turned"
                                                                : ", rules not turned";
        } catch (const std::exception& error) {
            why += std::string(", rules unread: ") + error.what();
        }
        require(false, why);
        return uint16_t{0};
    };
    const auto arm_building = [&](uint16_t type) {
        match_command_ = MatchCommand::build;
        pending_build_type_ = type;
        view_settings_.rotate_key_discovered = false;
        frames(1);
    };
    const auto disarm = [&] {
        match_command_ = MatchCommand::none;
        pending_build_type_ = 0;
        frames(1);
    };
    // Draws the match with the pointer over the battlefield at a grid of
    // points until the hint is drawn, since a site may not be buildable
    // everywhere.
    const auto seek_hint = [&](std::string_view wanted) {
        const int32_t field_x = match_layout_.battlefield_x();
        const int32_t field_y = match_layout_.battlefield_y();
        const int32_t field_w = match_layout_.battlefield_width();
        const int32_t field_h = match_layout_.battlefield_height();
        require(field_w > 0 && field_h > 0, "the battlefield has no room");
        for (int32_t row = 1; row <= 5; ++row) {
            for (int32_t column = 1; column <= 5; ++column) {
                update_pointer(
                    static_cast<float>(field_x + field_w * column / 6),
                    static_cast<float>(field_y + field_h * row / 6)
                );
                render_match_surface();
                if (rotate_hint_drawn_ == wanted)
                    return;
            }
        }
        require(
            false,
            "the rotate hint is \"" + rotate_hint_drawn_ + "\", wanted \"" + std::string(wanted) +
                "\""
        );
    };
    const auto snapshot = [&](std::string_view stem, int32_t width, int32_t height) {
        if (options_.snapshot.empty())
            return;
        oa_layer().read_whole_window(true);
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        const fs::path path =
            options_.snapshot.parent_path() / (std::string(stem) + '-' + std::to_string(width) +
                                               'x' + std::to_string(height) + ".ppm");
        write_ppm(path, presented);
        require(fs::is_regular_file(path), "did not write " + path.string());
    };
    const auto set_badges = [&](bool on) {
        auto chosen = engine_settings();
        chosen.show_keys_on_buttons = on;
        apply_engine_settings(chosen);
        render_match_surface();
    };

    resize(1280, 720);
    if (screen_ != Screen::main_menu)
        load(Screen::main_menu);
    frames(2);
    if (LayerScreen* welcome = oa_layer().find("first-run")) {
        oa_layer().close(welcome);
        frames(1);
    }
    start_benchmark_skirmish();
    require(screen_ == Screen::match && match_ != nullptr, "the skirmish did not start");
    require(!match_paused_, "the skirmish started paused");
    // The right-click interface, on which a tap of Shift latches QUEUE.
    match_->state().game.interface_type = oa::sim::gameplay_input::interface_right_click;
    select_commander();

    say_step("Primary+/ opens oa.keys-overlay titled Keys while playing, listing Attack with A");
    open_overlay();
    const std::string playing = list_text();
    require(
        playing.starts_with("Keys while playing\n"), "the overlay is not titled Keys while playing"
    );
    require(
        playing.find("\nAttack A\n") != std::string::npos, "the overlay does not list Attack with A"
    );
    // The check's clock is fixed, so "running" is the frame's own test:
    // idle_tick steps the match while it holds.
    frames(4);
    require(match_clock_steps(), "the match clock does not run under the overlay");
    require(!match_paused_, "the overlay paused the match");
    say_step("the match clock keeps running under the overlay");

    say_step("a building armed reads Keys while placing a building and lists Rotate");
    const uint16_t tower = turning_type();
    arm_building(tower);
    const std::string placing = list_text();
    require(
        placing.starts_with("Keys while placing a building\n"),
        "the overlay is not titled Keys while placing a building"
    );
    require(placing.find("\nRotate /") != std::string::npos, "the overlay does not list Rotate");
    say_step("the rotate hint reads Press /, or Alt+wheel, to rotate on OA default");
    seek_hint("Press /, or Alt+wheel, to rotate");
    disarm();

    say_step("Escape closes the overlay and the clock runs on");
    close_overlay();
    frames(2);
    require(match_clock_steps(), "the clock stopped after the overlay closed");

    say_step("EDIT KEYS… opens oa.key-editor and the clock holds");
    open_overlay();
    click_named("oa.keys-overlay.edit");
    require(
        oa_layer().find(editor::screen_name) != nullptr, "EDIT KEYS… did not open oa.key-editor"
    );
    require(!overlay_open(), "EDIT KEYS… left the overlay open");
    const uint32_t held = match_timing_.tick;
    frames(4);
    require(
        !match_clock_steps() && match_timing_.tick == held,
        "the clock runs while the editor is open"
    );
    tap(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE);
    require(oa_layer().find(editor::screen_name) == nullptr, "Escape did not close the editor");
    if (match_paused_)
        resume_match_pause();
    frames(1);
    require(!match_paused_, "the match stayed paused after the editor");
    select_commander();
    show_orders();

    say_step("the order panel's ATTACK button has a badge A");
    const std::size_t attack_index = gadget_named("ATTACK");
    const std::string attack_caption = [&] {
        const auto* fields = std::get_if<oa::ui::gui_layout::ButtonFields>(
            &match_hud_->layout.gadgets[attack_index].fields
        );
        return fields != nullptr ? fields->text : std::string{};
    }();
    render_match_surface();
    {
        const std::optional<KeyBadgeDrawn> attack = badge_for("order.attack");
        require(attack.has_value() && attack->label == "A", "ATTACK has no badge A");
        // The underline is the caption's own A, where the caption has one;
        // the game's order buttons are pictures with no caption to underline.
        const std::size_t letter = attack_caption.find_first_of("Aa");
        const char wanted = letter == std::string::npos ? '\0' : attack_caption[letter];
        const std::optional<QuickKeyApplied> underline = quick_for("order.attack");
        require(
            underline.has_value() && underline->letter == wanted,
            "ATTACK's underline does not follow A"
        );
    }

    say_step("Attack rebound to W: the badge reads W and ATTACK's underline is gone");
    write_own_layout(user_folder_, {{"order.attack", "W"}, {"build.cell-1", "Z"}});
    reread_key_bindings();
    render_match_surface();
    {
        const std::optional<KeyBadgeDrawn> rebound = badge_for("order.attack");
        require(rebound.has_value() && rebound->label == "W", "ATTACK's badge is not W");
        const std::optional<QuickKeyApplied> moved = quick_for("order.attack");
        require(moved.has_value() && moved->letter == '\0', "ATTACK still underlines a letter");
    }
    {
        open_overlay();
        const std::string own = list_text();
        require(
            own.find("\nAttack W\n") != std::string::npos, "the overlay does not list Attack with W"
        );
        require(
            own.find("OA default · 2 changes") != std::string::npos,
            "the overlay does not name OA default and its 2 changes"
        );
        close_overlay();
    }

    say_step("hovering ATTACK writes Attack and a description ending · W in the bottom panel");
    hover_gadget(attack_index);
    render_match_surface();
    require(unit_panel_hover_name_ == "Attack", "the name line is not Attack");
    require(
        unit_panel_hover_description_.ends_with(" · W"),
        "the description does not end · W (" + unit_panel_hover_description_ + ")"
    );

    say_step("Show keys on buttons Off removes the badges");
    set_badges(false);
    require(key_badges_drawn_.empty(), "badges were drawn with Show keys on buttons off");
    set_badges(true);
    require(!key_badges_drawn_.empty(), "Show keys on buttons On drew no badge");

    say_step("build.cell-1 bound to Z: the first build cell wears Z and its readout ends · Z");
    click_gadget(gadget_named("BUILD"));
    render_match_surface();
    const std::vector<std::size_t> cells = build_cell_gadgets();
    require(!cells.empty(), "the build page has no first cell");
    const std::size_t cell_index = cells.front();
    {
        const std::optional<KeyBadgeDrawn> cell_badge = badge_for("build.cell-1");
        require(
            cell_badge.has_value() && cell_badge->label == "Z" &&
                cell_badge->gadget == match_hud_->layout.gadgets[cell_index].common.name,
            "the first build cell has no badge Z"
        );
    }
    hover_gadget(cell_index);
    render_match_surface();
    require(
        unit_panel_hover_name_.ends_with(" · Z"),
        "the cost line does not end · Z (" + unit_panel_hover_name_ + ")"
    );

    say_step("build.rotate bound to R with the wheel unbound reads Press R to rotate");
    write_own_layout(
        user_folder_,
        {{"order.attack", "W"},
         {"build.cell-1", "Z"},
         {"build.rotate", "R"},
         {"build.rotate-wheel", ""}}
    );
    reread_key_bindings();
    select_commander();
    arm_building(tower);
    seek_hint("Press R to rotate");
    disarm();

    say_step("a latched QUEUE shows in the overlay's On line and not on the battlefield");
    select_commander();
    tap(SDLK_LSHIFT, SDL_SCANCODE_LSHIFT, SDL_KMOD_NONE);
    require(
        touch_state_if_made() != nullptr && touch_state().hud.latches.latched(hud::Latch::queue),
        "Shift did not latch QUEUE"
    );
    render_match_surface();
    require(
        key_latch_chips_drawn_.size() == 1 && key_latch_chips_drawn_.front() == "QUEUE · latched",
        "QUEUE · latched is not on the battlefield before the overlay opens"
    );
    open_overlay();
    const std::string latched = list_text();
    require(
        latched.find("\nOn: QUEUE · latched\n") != std::string::npos,
        "QUEUE is not in the overlay's On line"
    );
    render_match_surface();
    require(
        key_latch_chips_drawn_.empty(),
        "QUEUE was drawn on the battlefield while the overlay is open"
    );
    close_overlay();
    render_match_surface();
    require(!key_latch_chips_drawn_.empty(), "QUEUE did not come back to the battlefield");

    say_step("snapshots of the overlay and the badges on OA default, QUEUE latched");
    write_own_layout(user_folder_, {});
    reread_key_bindings();
    select_commander();
    show_orders();
    require(
        touch_state().hud.latches.latched(hud::Latch::queue), "QUEUE let go before the snapshots"
    );
    for (std::size_t size_at = 0; size_at < 3; ++size_at) {
        const int32_t width = kWidths[size_at];
        const int32_t height = kHeights[size_at];
        resize(width, height);
        require(screen_ == Screen::match && match_ != nullptr, "the resize left the match");
        // The pointer rests on the battlefield, so the bottom panel names no button.
        pointer_at(
            {match_layout_.battlefield_x() + match_layout_.battlefield_width() * 3 / 4,
             match_layout_.battlefield_y() + match_layout_.battlefield_height() * 3 / 4},
            SDL_EVENT_MOUSE_MOTION,
            0
        );
        render_match_surface();
        const std::optional<KeyBadgeDrawn> shown = badge_for("order.attack");
        require(shown.has_value() && shown->label == "A", "the badge snapshot has no A on ATTACK");
        snapshot("key-badges", width, height);
        open_overlay();
        const std::string titled = list_text();
        require(
            titled.starts_with("Keys while playing\n") &&
                titled.find("\nAttack A\n") != std::string::npos &&
                titled.find("\nOn: QUEUE · latched\n") != std::string::npos,
            "the snapshot overlay is not Keys while playing with Attack A and QUEUE"
        );
        snapshot("keys-overlay", width, height);
        close_overlay();
    }
    tap(SDLK_LSHIFT, SDL_SCANCODE_LSHIFT, SDL_KMOD_NONE);

    // Put back what the check changed.
    fs::remove(own_file, removed);
    fs::remove(unit_file, removed);
    reread_key_bindings();
    apply_engine_settings(saved_settings);
    say_step("done");
}

} // namespace oa::app
