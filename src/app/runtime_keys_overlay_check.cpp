// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-keys-overlay: a skirmish with a selected builder. Primary+/ opens
// the overlay, the clock keeps running, a building being placed changes the
// title, Escape closes it, EDIT KEYS… opens the editor and the clock holds.
// Order badges, the underline, help lines, the rotate hint and a latched
// QUEUE chip are checked. --snapshot writes keys-overlay-<w>x<h>.ppm and
// key-badges-<w>x<h>.ppm at 640x480, 1280x720 and 1920x1080, on OA default,
// before any of this run's own layout is written.

#include "oa/app/runtime.hpp"
#include "oa_layer_check.hpp"
#include "pad_state.hpp"
#include "touch_state.hpp"

#include "oa/data/key_library/library.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/first_run/first_run.hpp"
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
constexpr SDL_Keymod kPrimaryModifier = SDL_KMOD_GUI;
#else
constexpr SDL_Keymod kPrimaryModifier = SDL_KMOD_CTRL;
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
void step(std::string_view what) {
    std::cout << "keys overlay: " << what << std::endl;
}

/// Writes an own layout of OA default. An empty chord unbinds the action.
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

/// Puts the settings, the language and the files this check wrote back.
struct Restore {
    Runtime& runtime;
    settings::EngineSettings saved;
    std::string language;
    fs::path own_file;
    fs::path unit_file;

    ~Restore() {
        try {
            runtime.apply_engine_settings(saved);
            runtime.set_language_choice(language);
            std::error_code error;
            fs::remove(own_file, error);
            fs::remove(unit_file, error);
            runtime.reread_key_bindings();
        } catch (const std::exception&) {
        }
    }
};

} // namespace

void Runtime::check_keys_overlay() {
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the SDL presenter");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );
    require(!options_.mod_dir.empty(), "needs --mod-dir so a building can turn");

    std::string saved_language = "en";
    if (const auto found = preference_values_.find(std::string(settings::key::language));
        found != preference_values_.end())
        saved_language = found->second;
    Restore restore{
        *this,
        engine_settings(),
        saved_language,
        own_layout_file(user_folder_),
        options_.mod_dir / "units" / "armllt.fbi"
    };

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

    // A laser tower faces only south until its rules name more facings.
    // The loose file replaces the archive's for this run.
    {
        const char* names[] = {
            "units/armllt.fbi",
            "units/ARMLLT.FBI",
            "units\\armllt.fbi",
            "units\\ARMLLT.FBI",
        };
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
        fs::create_directories(restore.unit_file.parent_path(), made);
        require(!made, "the mod's units folder was not made");
        std::ofstream unit_out(restore.unit_file, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(unit_out), "the tower's rules were not opened");
        unit_out.write(text.data(), static_cast<std::streamsize>(text.size()));
        require(static_cast<bool>(unit_out), "the tower's rules were not written");
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
    const auto named = [this](std::string_view name) -> std::optional<AutomationControl> {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls)
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        return std::nullopt;
    };
    const auto list_text = [&] {
        const std::optional<AutomationControl> list = named("oa.keys-overlay.list");
        require(list.has_value(), "oa.keys-overlay.list is not shown");
        return list->text;
    };
    const auto overlay_open = [this] {
        return oa_layer().find(editor::name_keys_overlay) != nullptr;
    };
    const auto send_primary_slash = [&] {
        PadAccess::send_key(*this, SDLK_SLASH, kPrimaryModifier);
        frames(2);
    };
    const auto open_overlay = [&] {
        if (!overlay_open())
            send_primary_slash();
        require(overlay_open(), "Primary+/ did not open the keys overlay");
        require(!match_paused_, "the overlay paused the match");
    };
    const auto close_overlay = [&] {
        if (!overlay_open())
            return;
        PadAccess::send_key(*this, SDLK_ESCAPE);
        frames(1);
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
    const auto hover_gadget = [&](std::size_t index) {
        const auto& common = match_hud_->layout.gadgets[index].common;
        const auto rect = layout::source_rect_to_canvas(
            match_layout_, common.x, common.y, common.width, common.height
        );
        float window_x = 0;
        float window_y = 0;
        const float canvas_x = static_cast<float>(rect.x + rect.width / 2);
        const float canvas_y = static_cast<float>(rect.y + rect.height / 2);
        require(
            frame_to_window(sdl_.renderer, canvas_x, canvas_y, &window_x, &window_y),
            "the pointer did not map into the window"
        );
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.windowID = SDL_GetWindowID(sdl_.window);
        event.motion.x = window_x;
        event.motion.y = window_y;
        dispatch_event(event, running);
        require(running, "a pointer event ended the run");
        require(hovered_ == index, "the pointer is not over " + common.name);
    };
    const auto gadget_named = [&](std::string_view action) {
        require(match_hud_.has_value(), "the match has no order panel");
        for (std::size_t index = 0; index < match_hud_->layout.gadgets.size(); ++index)
            if (match_hud_action(match_hud_->layout.gadgets[index].common.name) == action)
                return index;
        require(false, std::string(action) + " is not on the order panel");
        return std::size_t{0};
    };
    const auto badge_for = [this](std::string_view action) -> const KeyBadgeDrawn* {
        for (const KeyBadgeDrawn& badge : key_badges_drawn_)
            if (badge.action == action)
                return &badge;
        return nullptr;
    };
    const auto quick_for = [this](std::string_view action) -> const QuickKeyApplied* {
        for (const QuickKeyApplied& applied : quick_keys_applied_)
            if (applied.action == action)
                return &applied;
        return nullptr;
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
        clear_local_selection();
        adopt_selection(commander);
        selected_match_unit_ = commander;
        apply_match_hud_for_selection();
        match_command_ = MatchCommand::none;
        pending_build_type_ = 0;
        frames(1);
        return commander;
    };
    const auto turning_type = [&] {
        uint16_t found = 0;
        const std::size_t types = spawn_type_names_.size();
        for (std::size_t index = 1; index < types; ++index) {
            if (index > static_cast<std::size_t>(std::numeric_limits<uint16_t>::max()))
                break;
            const auto type = static_cast<uint16_t>(index);
            const uint8_t facings = match_->build_facings(type);
            if (view_rules::next_build_facing(facings, view_rules::BuildFacing::south, 1) !=
                view_rules::BuildFacing::south) {
                found = type;
                break;
            }
        }
        const bool preview = ui_rules().build_preview.enabled;
        const oa::data::mod_profile::ModProfile* profile = mod_profile();
        const bool rotation = profile != nullptr && profile->rules.units.build_rotation.enabled;
        require(
            found != 0,
            std::string("no building turns (preview ") + (preview ? "on" : "off") + ", rotation " +
                (rotation ? "on" : "off") + ")"
        );
        return found;
    };
    const auto arm_building = [&](uint16_t type) {
        match_command_ = MatchCommand::build;
        pending_build_type_ = type;
        view_settings_.rotate_key_discovered = false;
    };
    const auto seek_hint = [&](std::string_view wanted) {
        const int32_t field_x = match_layout_.battlefield_x();
        const int32_t field_y = match_layout_.battlefield_y();
        const int32_t field_w = match_layout_.battlefield_width();
        const int32_t field_h = match_layout_.battlefield_height();
        require(field_w > 0 && field_h > 0, "the battlefield has no room");
        std::string last_error;
        for (int32_t row = 1; row <= 5; ++row) {
            for (int32_t column = 1; column <= 5; ++column) {
                const float x = static_cast<float>(field_x + field_w * column / 6);
                const float y = static_cast<float>(field_y + field_h * row / 6);
                try {
                    update_pointer(x, y);
                    render_match_surface();
                } catch (const std::exception& error) {
                    last_error = error.what();
                    continue;
                }
                if (rotate_hint_drawn_ == wanted)
                    return;
            }
        }
        require(
            false,
            "the rotate hint is \"" + rotate_hint_drawn_ + "\", wanted \"" + std::string(wanted) +
                "\"" + (last_error.empty() ? "" : " (" + last_error + ")")
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
    select_commander();

    step("Primary+/ opens the overlay titled Keys while playing, listing Attack with A");
    const uint32_t ticking = match_timing_.tick;
    open_overlay();
    const std::string playing = list_text();
    require(
        playing.find("Keys while playing") != std::string::npos,
        "the overlay is not titled Keys while playing"
    );
    require(
        playing.find("Attack A") != std::string::npos, "the overlay does not list Attack with A"
    );
    frames(12);
    require(match_timing_.tick > ticking, "the match clock did not keep running");
    require(!match_paused_, "the overlay paused the match");
    step("the match clock keeps running under the overlay");

    const uint16_t tower = turning_type();
    step("a building armed reads Keys while placing a building and lists Rotate");
    arm_building(tower);
    frames(2);
    const std::string placing = list_text();
    require(
        placing.find("Keys while placing a building") != std::string::npos,
        "the overlay is not titled Keys while placing a building"
    );
    require(placing.find("Rotate") != std::string::npos, "the overlay does not list Rotate");
    seek_hint("Press /, or Alt+wheel, to rotate");
    step("the rotate hint reads Press /, or Alt+wheel, to rotate");

    step("Escape closes the overlay");
    match_command_ = MatchCommand::none;
    pending_build_type_ = 0;
    close_overlay();
    frames(2);
    require(match_timing_.tick > ticking, "the clock stopped after the overlay closed");

    step("EDIT KEYS opens oa.key-editor and the clock holds");
    open_overlay();
    const uint32_t held = match_timing_.tick;
    click_named("oa.keys-overlay.edit");
    require(
        oa_layer().find(editor::screen_name) != nullptr, "EDIT KEYS… did not open oa.key-editor"
    );
    require(!overlay_open(), "EDIT KEYS… left the overlay open");
    frames(3);
    require(match_timing_.tick == held, "the clock moved while the editor was open");
    require(match_paused_ || !match_clock_steps(), "the editor did not pause the match");
    PadAccess::send_key(*this, SDLK_ESCAPE);
    frames(1);
    require(oa_layer().find(editor::screen_name) == nullptr, "Escape did not close the editor");
    resume_match_pause();
    frames(1);
    require(!match_paused_, "the match stayed paused after the editor");
    select_commander();

    step("the order panel's ATTACK button has a badge A");
    render_match_surface();
    const KeyBadgeDrawn* attack = badge_for("order.attack");
    require(attack != nullptr && attack->label == "A", "ATTACK has no badge A");
    const QuickKeyApplied* underline = quick_for("order.attack");
    require(underline != nullptr && underline->letter == 'A', "ATTACK's underline is not A");

    step("snapshots of the overlay and the badges on OA default");
    for (std::size_t size_at = 0; size_at < 3; ++size_at) {
        const int32_t width = kWidths[size_at];
        const int32_t height = kHeights[size_at];
        resize(width, height);
        require(screen_ == Screen::match && match_ != nullptr, "the resize left the match");
        select_commander();
        open_overlay();
        const std::string titled = list_text();
        require(
            titled.find("Keys while playing") != std::string::npos &&
                titled.find("Attack A") != std::string::npos,
            "the snapshot overlay is not Keys while playing with Attack A"
        );
        snapshot("keys-overlay", width, height);
        close_overlay();
        render_match_surface();
        const KeyBadgeDrawn* shown = badge_for("order.attack");
        require(shown != nullptr && shown->label == "A", "the badge snapshot has no A on ATTACK");
        snapshot("key-badges", width, height);
    }

    step("Attack rebound to W: the badge reads W and ATTACK's underline is gone");
    write_own_layout(user_folder_, {{"order.attack", "W"}, {"build.cell-1", "Z"}});
    reread_key_bindings();
    select_commander();
    render_match_surface();
    const KeyBadgeDrawn* rebound = badge_for("order.attack");
    require(rebound != nullptr && rebound->label == "W", "ATTACK's badge is not W");
    const QuickKeyApplied* moved = quick_for("order.attack");
    require(moved != nullptr && moved->letter == '\0', "ATTACK still underlines a letter");

    step("hovering ATTACK writes Attack and a description ending · W");
    const std::size_t attack_index = gadget_named("ATTACK");
    hover_gadget(attack_index);
    render_match_surface();
    require(unit_panel_hover_name_ == "Attack", "the name line is not Attack");
    require(
        unit_panel_hover_description_.size() >= 3 && unit_panel_hover_description_.ends_with("· W"),
        "the description does not end · W (" + unit_panel_hover_description_ + ")"
    );

    step("Show keys on buttons Off removes the badges");
    {
        auto chosen = engine_settings();
        chosen.show_keys_on_buttons = false;
        apply_engine_settings(chosen);
    }
    render_match_surface();
    require(key_badges_drawn_.empty(), "badges were drawn with Show keys on buttons off");
    {
        auto chosen = engine_settings();
        chosen.show_keys_on_buttons = true;
        apply_engine_settings(chosen);
    }

    step("build.cell-1 bound to Z: the first build cell wears Z and its readout ends · Z");
    select_commander();
    const std::size_t build_tab = gadget_named("BUILD");
    hover_gadget(build_tab);
    pointer_at(
        [&] {
            const auto& common = match_hud_->layout.gadgets[build_tab].common;
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
                "BUILD did not map into the window"
            );
            return layout::Point{static_cast<int32_t>(window_x), static_cast<int32_t>(window_y)};
        }(),
        SDL_EVENT_MOUSE_BUTTON_DOWN,
        SDL_BUTTON_LEFT
    );
    pointer_at(
        [&] {
            const auto& common = match_hud_->layout.gadgets[build_tab].common;
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
                "BUILD did not map into the window"
            );
            return layout::Point{static_cast<int32_t>(window_x), static_cast<int32_t>(window_y)};
        }(),
        SDL_EVENT_MOUSE_BUTTON_UP,
        SDL_BUTTON_LEFT
    );
    frames(1);
    constexpr uint8_t build_attributes =
        oa::ui::hud::kCommonUnitButton | oa::ui::hud::kCommonWeaponButton;
    int32_t cell = 0;
    std::size_t cell_index = 0;
    bool cell_found = false;
    for (std::size_t index = 0; index < match_hud_->layout.gadgets.size(); ++index) {
        const auto& common = match_hud_->layout.gadgets[index].common;
        if ((static_cast<uint8_t>(common.common_attributes) & build_attributes) == 0)
            continue;
        ++cell;
        if (cell == 1) {
            cell_index = index;
            cell_found = true;
            break;
        }
    }
    require(cell_found, "the build page has no first cell");
    require(
        match_hud_->layout.gadgets[cell_index].common.active != 0,
        "the first build cell is inactive"
    );
    render_match_surface();
    const KeyBadgeDrawn* cell_badge = badge_for("build.cell-1");
    require(
        cell_badge != nullptr && cell_badge->label == "Z", "the first build cell has no badge Z"
    );
    hover_gadget(cell_index);
    render_match_surface();
    require(
        unit_panel_hover_name_.size() >= 3 && unit_panel_hover_name_.ends_with("· Z"),
        "the cost line does not end · Z (" + unit_panel_hover_name_ + ")"
    );

    step("build.rotate bound to R with the wheel unbound reads Press R to rotate");
    write_own_layout(
        user_folder_,
        {{"order.attack", "W"},
         {"build.cell-1", "Z"},
         {"build.rotate", "R"},
         {"build.rotate-wheel", ""}}
    );
    reread_key_bindings();
    arm_building(tower);
    frames(2);
    seek_hint("Press R to rotate");

    step("a latched QUEUE shows in the overlay's On line and not on the battlefield");
    match_command_ = MatchCommand::none;
    pending_build_type_ = 0;
    select_commander();
    match_->state().game.interface_type = oa::sim::gameplay_input::interface_right_click;
    PadAccess::send_key(*this, SDLK_LSHIFT);
    frames(1);
    require(
        touch_state_if_made() != nullptr && touch_state().hud.latches.latched(hud::Latch::queue),
        "Shift did not latch QUEUE"
    );
    open_overlay();
    const std::string latched = list_text();
    require(latched.find("On:") != std::string::npos, "the overlay has no On line");
    require(latched.find("QUEUE · latched") != std::string::npos, "QUEUE is not in the On line");
    render_match_surface();
    require(
        key_latch_chips_drawn_.empty(),
        "QUEUE was drawn on the battlefield while the overlay is open"
    );
    close_overlay();
    step("done");
}

} // namespace oa::app
