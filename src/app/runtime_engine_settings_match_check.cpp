// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-engine-settings, the match's part: the OA button under Resume, the
// dialog beside the darkened column, the pause and the locks.

#include "check_host_input.hpp"
#include "engine_settings_menu_host.hpp"
#include "engine_settings_state.hpp"
#include "engine_settings_tall_section.hpp"
#include "library_host.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/runtime.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/library/library.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

/// Graphics' scroll limit in a game at Compact: its twenty-two rows, Smooth
/// motion, Fog of War and New unit shadows among them, now that User
/// Interface holds Menu scaling, Window frame, HUD scaling and Interface size.
constexpr int32_t kMatchGraphicsEnd = 1047;

namespace fs = std::filesystem;
namespace settings = oa::ui::engine_settings;
namespace library = oa::ui::library;
namespace layout = oa::ui::display_layout;
namespace artless = oa::ui::frontend_renderer;
namespace kit = oa::ui::kit;

/// How far round the pointer the software cursor may draw, in window pixels.
constexpr int kCursorReach = 64;
/// How long the check lets a paused game's clock run, in milliseconds a frame.
constexpr uint32_t kPausedFrameMs = 40;
/// How many frames the check lets a paused game's clock run.
constexpr int kPausedFrames = 3;

/// The modifier of the shortcut that opens the settings.
#ifdef SDL_PLATFORM_MACOS
constexpr SDL_Keymod kShortcutModifier = SDL_KMOD_GUI;
#else
constexpr SDL_Keymod kShortcutModifier = SDL_KMOD_CTRL;
#endif

/// An ultrawide window: 21:9.
constexpr int kUltrawideWidth = 2560;
/// An ultrawide window's height.
constexpr int kUltrawideHeight = 1080;

/// A window the in-match dialog is checked on, with HUD scaling On or Off,
/// and the size class and whole scale the dialog takes there: the points of
/// the window's whole safe area at the layer's scale, capped by the
/// column's (OaLayer::match_dialog).
struct MatchWindow {
    int width{};                                        ///< the window's width, in pixels
    int height{};                                       ///< the window's height, in pixels
    bool hud_scaling{};                                 ///< HUD scaling is On
    kit::SizeClass size_class{kit::SizeClass::compact}; ///< the dialog's class
    int32_t scale{}; ///< the window pixels a point of the dialog takes
};

/// The windows the in-match dialog is checked on: 640x480, 1280x720,
/// 1920x1080 and 2560x1080 with HUD scaling On, and 1920x1080 with it Off.
/// Only 640x480 is Compact; 1920x1080 and 2560x1080 would be Regular at 2x,
/// so Auto steps down to Large at 1x. On each the dialog clears the side
/// column: centred right of it at 640x480 and 1280x720, centred in the
/// window on the wider ones.
constexpr std::array<MatchWindow, 5> kMatchWindows{{
    {640, 480, true, kit::SizeClass::compact, 1},
    {1280, 720, true, kit::SizeClass::large, 1},
    {kDefaultWindowWidth, kDefaultWindowHeight, true, kit::SizeClass::large, 1},
    {kUltrawideWidth, kUltrawideHeight, true, kit::SizeClass::large, 1},
    {kDefaultWindowWidth, kDefaultWindowHeight, false, kit::SizeClass::large, 1},
}};

/// The rows of a side-column page taller than the game's own 480, as a
/// mod's build page may be: it narrows the side column on a window whose
/// height decides the chrome's scale.
constexpr int kTallPageRows = 540;

/// Windows the OA button is placed on without a game: 4:3, 16:10 (and the
/// same at two pixels a point) and 16:9.
constexpr std::array<std::pair<int, int>, 6> kPlacedWindows{{
    {1024, 768},
    {1440, 900},
    {1512, 982},
    {2880, 1800},
    {3024, 1964},
    {1920, 1080},
}};

/// The section whose rows a game locks: Engine Limits.
constexpr std::array<settings::Page, 1> kLockedPages{settings::Page::common_tweaks};

/// Engine Limits' rows a game locks: the unit limit, pathfinding and the
/// skirmish player count.
constexpr std::array<int32_t, 3> kLockedRows{
    settings::first_row_control,
    settings::first_row_control + 1,
    settings::first_row_control + 2,
};

/// The keys of the made-up mods that stand in the Library for the mods
/// during a game, since no mod plays in the check: the one played and
/// another.
constexpr std::string_view kPlayedStandIn = "match-check-played";
constexpr std::string_view kOtherStandIn = "match-check-other";

/// The keys that move the focus, each pressed in turn round every control
/// and back: Tab, Shift+Tab, Down and Up.
constexpr std::array<std::pair<SDL_Keycode, SDL_Keymod>, 4> kFocusKeys{{
    {SDLK_TAB, SDL_KMOD_NONE},
    {SDLK_TAB, SDL_KMOD_SHIFT},
    {SDLK_DOWN, SDL_KMOD_NONE},
    {SDLK_UP, SDL_KMOD_NONE},
}};

/// Returns the name a section's snapshots carry.
///
/// @param page Engine Limits
/// @return a short name
std::string_view locked_page_slug(settings::Page page) {
    return page == settings::Page::common_tweaks ? "tweaks" : "page";
}

/// Tells whether the dialog's layout shows a text.
///
/// @param parts the dialog's layout
/// @param text the text
/// @return true when a part draws exactly that text
bool shows_text(const std::vector<settings::LayoutPart>& parts, std::string_view text) {
    return std::any_of(parts.begin(), parts.end(), [text](const settings::LayoutPart& part) {
        return part.text == text;
    });
}

/// Returns the path of one step's snapshot: <stem>-<step>.ppm beside --snapshot.
///
/// @param snapshot the --snapshot path
/// @param step the step's name
/// @return the step's snapshot path
fs::path step_snapshot(const fs::path& snapshot, std::string_view step) {
    auto stem = snapshot;
    stem.replace_extension();
    return fs::path(stem.string() + "-" + std::string(step) + ".ppm");
}

} // namespace

void Runtime::check_engine_settings_in_match() {
    const auto require = [](bool ok, const std::string& what) {
        if (!ok)
            throw std::runtime_error("engine settings check: " + what);
    };
    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);
    require(engine_settings_fonts() != nullptr, "the dialog's fonts are missing");

    // On a phone with touch controls the dialog and the OA button fit the
    // safe area: an 852x393-point landscape phone at three pixels a point,
    // its sides 59 points in and its bottom 21.
    {
        const layout::Insets insets{177, 0, 177, 63};
        const auto phone = layout::make_phone_layout(2556, 1179, 3.0, insets);
        const auto safe = OaLayer::match_safe_area(phone);
        require(
            safe.x == 177 && safe.y == 0 && safe.width == 2202 && safe.height == 1116,
            "the phone's safe area is not the canvas less its insets"
        );
        const auto inside_safe = [&safe](const layout::Rect& rect) {
            return rect.width > 0 && rect.height > 0 && rect.x >= safe.x && rect.y >= safe.y &&
                   rect.x + rect.width <= safe.x + safe.width &&
                   rect.y + rect.height <= safe.y + safe.height;
        };
        const auto fitted = OaLayer::match_dialog(phone, 0, true);
        const auto dialog_at = fitted.shown;
        // min(2202 / 480, 1116 / 324): the safe area's height decides.
        const double scale = 1116.0 / settings::dialog_height;
        require(
            fitted.size_class == kit::SizeClass::compact && fitted.scale_percent == 0 &&
                inside_safe(dialog_at) && dialog_at.height == 1116 &&
                dialog_at.width == static_cast<int>(std::lround(settings::dialog_width * scale)) &&
                std::abs(2 * dialog_at.x + dialog_at.width - (2 * safe.x + safe.width)) <= 1,
            "the dialog does not fit the phone's safe area, centred"
        );
        const auto point = layer_point(
            {dialog_at, settings::dialog_width, settings::dialog_height},
            static_cast<float>(dialog_at.x + dialog_at.width - 1),
            static_cast<float>(dialog_at.y + dialog_at.height - 1)
        );
        require(
            point.x == settings::dialog_width - 1 && point.y == settings::dialog_height - 1,
            "the fitted dialog's corner does not map to its last source pixel"
        );
        const auto button = OaLayer::match_button_rect(phone, true);
        require(inside_safe(button), "the OA button leaves the phone's safe area");
        // Unfitted, the phone's side column is at 1x, which caps the dialog.
        const auto unfitted = OaLayer::match_dialog(phone, 0);
        require(
            unfitted.scale_percent == 100 && !inside_safe(OaLayer::match_button_rect(phone)),
            "without fitting, the dialog does not keep the side column's scale"
        );
    }

    const Extension saved_extension = extension_;
    bool running = true;
    const auto send_key = [&](SDL_Keycode key, SDL_Keymod modifiers, bool down, bool repeat) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.windowID = sdl_.window != nullptr ? SDL_GetWindowID(sdl_.window) : 0;
        event.key.key = key;
        event.key.mod = modifiers;
        event.key.down = down;
        event.key.repeat = repeat;
        dispatch_event(event, running);
        require(running, "a key ended the run");
    };
    const auto tap_key = [&](SDL_Keycode key, SDL_Keymod modifiers) {
        send_key(key, modifiers, true, false);
        send_key(key, modifiers, false, false);
    };
    const auto composed = [this] {
        renderer::Surface frame;
        compose_match_frame(frame);
        return frame;
    };
    const auto shown = [this](const renderer::Surface& source) {
        auto copy = source;
        apply_gamma_rgb(copy.rgb.data(), copy.rgb.size() / 3U, 3);
        return copy;
    };
    // Each window pixel of `rect` shows the source pixel it lands on.
    const auto drawn_at = [&](const renderer::Surface& frame,
                              const renderer::Surface& source,
                              const layout::Rect& rect,
                              const std::string& what) {
        const auto expected = shown(source);
        std::size_t differing = 0;
        for (int row = 0; row < rect.height; ++row)
            for (int column = 0; column < rect.width; ++column) {
                const auto source_offset =
                    (static_cast<std::size_t>(row * static_cast<int>(source.height) / rect.height) *
                         source.width +
                     static_cast<std::size_t>(
                         column * static_cast<int>(source.width) / rect.width
                     )) *
                    3U;
                const auto frame_offset = (static_cast<std::size_t>(rect.y + row) * frame.width +
                                           static_cast<std::size_t>(rect.x + column)) *
                                          3U;
                for (std::size_t channel = 0; channel < 3; ++channel)
                    if (frame.rgb[frame_offset + channel] !=
                        expected.rgb[source_offset + channel]) {
                        ++differing;
                        break;
                    }
            }
        require(differing == 0, what + ": " + std::to_string(differing) + " pixels differ");
    };
    // `rect` shows the source pixel for pixel: as large as it, each window
    // pixel the source's pixel at the same place, its corners rounded as the
    // layer rounds the placement's (round_layer_corners). Outside the
    // rounded shape, and where its edge blends with what lies behind, the
    // window shows the game, which is not compared.
    const auto copied_at = [&](const renderer::Surface& frame,
                               const renderer::Surface& source,
                               const layout::Rect& rect,
                               const LayerPlacement& placed,
                               const std::string& what) {
        require(
            static_cast<int>(source.width) == rect.width &&
                static_cast<int>(source.height) == rect.height && rect.x >= 0 && rect.y >= 0 &&
                rect.x + rect.width <= static_cast<int>(frame.width) &&
                rect.y + rect.height <= static_cast<int>(frame.height),
            what + " is not drawn as large as its place"
        );
        LayerDrawing rounded;
        rounded.picture = source;
        round_layer_corners(rounded, placed.corner_radius, placed.scale_percent);
        const auto expected = shown(rounded.picture);
        std::size_t differing = 0;
        for (int row = 0; row < rect.height; ++row)
            for (int column = 0; column < rect.width; ++column) {
                const auto source_pixel =
                    static_cast<std::size_t>(row) * source.width + static_cast<std::size_t>(column);
                if (!rounded.opacity.empty() && rounded.opacity[source_pixel] != 255)
                    continue;
                const auto source_offset = source_pixel * 3U;
                const auto frame_offset = (static_cast<std::size_t>(rect.y + row) * frame.width +
                                           static_cast<std::size_t>(rect.x + column)) *
                                          3U;
                for (std::size_t channel = 0; channel < 3; ++channel)
                    if (frame.rgb[frame_offset + channel] !=
                        expected.rgb[source_offset + channel]) {
                        ++differing;
                        break;
                    }
            }
        require(differing == 0, what + ": " + std::to_string(differing) + " pixels differ");
    };
    // The dialog's header and the in-game OA button, which opens Settings,
    // show the settings icon.
    const auto gears = EngineSettingsMenuHost::settings_icon(*this);
    require(
        renderer::picture_drawable(gears),
        "the dialog and the in-game OA button have no icon to show"
    );
    const auto button_face = [&](settings::ButtonLook look, bool darkened, const auto& fonts) {
        return OaLayer::match_button_face(match_layout_, look, darkened, fonts, gears);
    };
    // The dialog drawn offscreen at a size class and a whole scale: the
    // class's dialog points at the scale, crisp. Compact at 1x is 0.7.3's.
    const auto dialog_face_at = [&gears](
                                    const settings::Dialog& dialog,
                                    const auto& fonts,
                                    kit::SizeClass size_class,
                                    int32_t scale
                                ) {
        settings::Dialog sized = dialog;
        sized.size_class = size_class;
        const kit::Metrics& class_metrics = kit::metrics_of(size_class);
        renderer::Surface face;
        face.width = static_cast<uint32_t>(class_metrics.dialog_width * scale);
        face.height = static_cast<uint32_t>(class_metrics.dialog_height * scale);
        face.rgb.assign(static_cast<std::size_t>(face.width) * face.height * 3U, 0);
        settings::draw_dialog(face, {0, 0, scale}, sized, fonts, gears);
        return face;
    };
    const auto centre = [](const layout::Rect& rect) {
        return layout::Point{rect.x + rect.width / 2, rect.y + rect.height / 2};
    };

    // Clicks the middle of a part of the dialog that shows beside the column,
    // as the dialog draws it at its scale.
    const auto click_dialog = [&](const artless::SourceRect& part) {
        const LayerPlacement placed = oa_layer().placement_of("settings");
        require(placed.scale_percent > 0, "the dialog does not show at a scale of its own");
        const kit::Rect drawn =
            kit::to_canvas({part.x, part.y, part.width, part.height}, placed.scale_percent);
        const layout::Point point{
            placed.shown.x + drawn.x + drawn.width / 2, placed.shown.y + drawn.y + drawn.height / 2
        };
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, point, 0);
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, point, SDL_BUTTON_LEFT);
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_UP, point, SDL_BUTTON_LEFT);
    };
    // Shows a section of the open dialog through its entry in the list.
    const auto show_page = [&](settings::Page page, const std::string& on) {
        auto* dialog = engine_settings_dialog();
        require(dialog != nullptr, "the dialog closed" + on);
        // The Game files section belongs to the main menu's dialog alone.
        require(!dialog->game_files, "the in-game dialog lists Game files" + on);
        for (const auto& part : settings::dialog_layout(*dialog))
            if (part.control == settings::page_control(page)) {
                click_dialog(part.rect);
                break;
            }
        require(dialog->page == page, "a click on a section's entry did not show it" + on);
    };
    // Writes the composed frame as a step's snapshot when --snapshot names one.
    const auto snapshot = [&](std::string_view step, const std::string& size) {
        if (options_.snapshot.empty())
            return;
        send_check_pointer(
            SDL_EVENT_MOUSE_MOTION, {match_layout_.width - 1, match_layout_.height - 1}, 0
        );
        render();
        write_ppm(step_snapshot(options_.snapshot, std::string(step) + '-' + size), composed());
    };
    // Each locked row says why, and neither a press on its slider nor its
    // keys change it.
    const auto expect_locks = [&](std::string_view lock_text,
                                  std::string_view step,
                                  const std::string& size,
                                  const std::string& on) {
        // Each section says why its row is locked, shown before a key has
        // shown the keyboard focus.
        for (const auto page : kLockedPages) {
            show_page(page, on);
            require(
                shows_text(settings::dialog_layout(*engine_settings_dialog()), lock_text),
                std::string(locked_page_slug(page)) + " does not say \"" + std::string(lock_text) +
                    '"' + on
            );
            snapshot(std::string(step) + '-' + std::string(locked_page_slug(page)), size);
        }
        for (const auto page : kLockedPages) {
            show_page(page, on);
            const auto kept = engine_settings();
            // The slider's place, as the row shows it unlocked.
            auto unlocked = *engine_settings_dialog();
            unlocked.locks = {};
            for (const auto& part : settings::dialog_layout(unlocked))
                if (std::find(kLockedRows.begin(), kLockedRows.end(), part.control) !=
                        kLockedRows.end() &&
                    part.text.empty()) {
                    click_dialog(
                        {part.rect.x + part.rect.width - 2, part.rect.y, 1, part.rect.height}
                    );
                    click_dialog({part.rect.x + 1, part.rect.y, 1, part.rect.height});
                }
            for (const auto key : {SDLK_DOWN, SDLK_RIGHT, SDLK_RIGHT, SDLK_LEFT})
                tap_key(key, SDL_KMOD_NONE);
            require(
                engine_settings_dialog() != nullptr && engine_settings() == kept,
                "a locked row changed under a press or a key" + on
            );
        }
    };

    // During a game the mod played stays as it is: the in-game dialog lists
    // no Mods section, the Library does not open, and in its model, as the
    // run's feed gives it in a game, PLAY NOW and SWITCH OFF are off and ask
    // nothing, with nothing changed. Two made-up mods stand in for the mods,
    // one played, since no mod plays in the check.
    const auto expect_mod_locked = [&](const std::string& on) {
        const auto kept = engine_settings();
        auto* dialog = engine_settings_dialog();
        require(dialog != nullptr, "the dialog closed" + on);
        const auto parts = settings::dialog_layout(*dialog);
        require(
            !shows_text(parts, "Mods") && !shows_text(parts, "OPEN MODS FOLDER"),
            "the in-game dialog lists a Mods section" + on
        );
        open_library(std::nullopt);
        require(
            oa_layer().find(library_screen_name) == nullptr, "the Library opened in a game" + on
        );
        library::Inputs inputs = library_inputs();
        require(inputs.in_match, "the Library's feed does not know a game is played" + on);
        for (const std::string_view key : {kPlayedStandIn, kOtherStandIn}) {
            library::Installed stand_in;
            stand_in.kind = library::Kind::mod;
            stand_in.key = std::string(key);
            stand_in.name = std::string(key);
            stand_in.folder = user_folder_ / std::string(key);
            stand_in.playing = key == kPlayedStandIn;
            inputs.installed.push_back(std::move(stand_in));
        }
        LibraryHost& host = library_host();
        library::refresh(host.model, std::move(inputs));
        for (const std::string_view key : {kPlayedStandIn, kOtherStandIn}) {
            const library::EntryId id{{}, library::Kind::mod, std::string(key)};
            const library::Entry* entry = library::find_entry(host.model, id);
            require(entry != nullptr, "the Library does not list a made-up mod" + on);
            const library::Action wanted =
                key == kPlayedStandIn ? library::Action::switch_off : library::Action::play_now;
            bool off = false;
            for (const library::ActionButton& button : library::entry_actions(host.model, *entry))
                off = off || (button.action == wanted && !button.enabled);
            require(off, "PLAY NOW or SWITCH OFF can be pressed during a game" + on);
            library_entry_action(wanted, id);
        }
        // The feed builds the model afresh once the main menu shows again.
        host.refreshed = false;
        require(
            engine_settings_dialog() != nullptr &&
                dynamic_cast<const QuestionScreen*>(oa_layer().top()) == nullptr &&
                !soft_restart_requested() && engine_settings() == kept,
            "PLAY NOW or SWITCH OFF asked to switch the mod or changed a setting during a game" + on
        );
        // The keys move the focus round the in-game dialog, never onto a row
        // the game locks, and change nothing.
        show_page(settings::Page::common_tweaks, on);
        for (const auto& [key, modifiers] : kFocusKeys) {
            for (int press = 0; press < 12; ++press) {
                tap_key(key, modifiers);
                const auto* now = engine_settings_dialog();
                require(
                    now != nullptr &&
                        std::find(kLockedRows.begin(), kLockedRows.end(), now->focused) ==
                            kLockedRows.end(),
                    "a key moved the focus onto a row a game locks" + on
                );
            }
        }
        require(
            !soft_restart_requested() && engine_settings() == kept,
            "a key switched the mod or changed a setting during a game" + on
        );
    };

    // The HUD scaling each window takes, put back after the last.
    const bool kept_hud_scaling = engine_settings_state().current.hud_scaling;
    for (const MatchWindow& checked : kMatchWindows) {
        const int width = checked.width;
        const int height = checked.height;
        const std::string size = std::to_string(width) + 'x' + std::to_string(height);
        const std::string on =
            " on the " + size + " window" + (checked.hud_scaling ? "" : " with HUD scaling Off");
        // What the window's reports and snapshots are named by: its size, and
        // with HUD scaling Off that too.
        const std::string hud_suffix = checked.hud_scaling ? "" : "-hud-off";
        const std::string shot = size + hud_suffix;
        if (match_)
            leave_match();
        load(Screen::main_menu);
        // The game is laid out at the HUD scaling in effect when it starts.
        engine_settings_state().current.hud_scaling = checked.hud_scaling;
        if (!SDL_SetWindowSize(sdl_.window, width, height) || !SDL_SyncWindow(sdl_.window))
            throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
        start_benchmark_skirmish();
        require(
            match_layout_.width == width && match_layout_.height == height,
            "the match canvas is not the window's size" + on
        );
        const auto& fonts = *engine_settings_fonts();
        const auto button = OaLayer::match_button_rect(match_layout_);
        // The dialog at its class and scale, its class's points at the scale
        // centred in the window, or centred right of the column where the
        // window's centre would cover part of it and the area beside it
        // holds the dialog.
        const auto in_match =
            OaLayer::match_dialog(match_layout_, oa_layer().view().interface_percent);
        const auto& dialog_at = in_match.shown;
        const kit::Metrics& class_metrics = kit::metrics_of(checked.size_class);
        const int face_width = class_metrics.dialog_width * checked.scale;
        const int face_height = class_metrics.dialog_height * checked.scale;
        require(
            in_match.size_class == checked.size_class &&
                in_match.scale_percent == checked.scale * 100,
            "the dialog is " + std::string(size_class_name(in_match.size_class)) + " at " +
                std::to_string(in_match.scale_percent) + "%, not " +
                std::string(size_class_name(checked.size_class)) + " at " +
                std::to_string(checked.scale) + "x" + on
        );
        const int beside_width = width - match_layout_.left;
        const int centred_x = (width - face_width) / 2;
        const int expected_x = centred_x < match_layout_.left && beside_width >= face_width
                                   ? match_layout_.left + (beside_width - face_width) / 2
                                   : centred_x;
        require(
            dialog_at.width == face_width && dialog_at.height == face_height &&
                dialog_at.x == expected_x && dialog_at.y == (height - face_height) / 2,
            "the dialog is not its class's points at its scale centred in the window, or right "
            "of the column" +
                on
        );
        require(
            button.y + button.height <= match_layout_.hud_height && button.x >= 0 &&
                button.x + button.width <= match_layout_.left,
            "the OA button leaves the side column" + on
        );
        require(
            dialog_at.x >= match_layout_.left && dialog_at.y >= 0 &&
                dialog_at.x + dialog_at.width <= width && dialog_at.y + dialog_at.height <= height,
            "the dialog covers the side column" + on
        );

        // In play the column and the button are hidden, and a press on the
        // button's place opens nothing.
        render();
        require(!OaLayer::match_button_shown(*this), "the OA button shows in play" + on);
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, centre(button), 0);
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, centre(button), SDL_BUTTON_LEFT);
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_UP, centre(button), SDL_BUTTON_LEFT);
        require(engine_settings_dialog() == nullptr, "a press in play opened the dialog" + on);

        // The in-game menu shows the button under Resume.
        show_match_pause_menu();
        require(ingame_menu_column_shown(), "the in-game menu's column does not show" + on);
        // The button lies in the menu's panel, at the same place in it on
        // every window: this window, and the others with the game's pages and
        // with a page that narrows the side column.
        const auto& panel_record = match_hud_->layout.gadgets.front().common;
        const layout::Rect menu_panel{
            panel_record.x, panel_record.y, panel_record.width, panel_record.height
        };
        const auto check_in_menu = [&](const layout::MatchLayout& match, const std::string& where) {
            const auto panel = layout::source_rect_to_canvas(
                match, menu_panel.x, menu_panel.y, menu_panel.width, menu_panel.height
            );
            const auto placed = OaLayer::match_button_rect(match);
            const double scale = match.column_narrowed() ? match.column_scale : match.scale;
            const auto at = [scale](int source) {
                return static_cast<int>(std::lround(static_cast<double>(source) * scale));
            };
            require(
                placed.x >= panel.x && placed.y >= panel.y &&
                    placed.x + placed.width <= panel.x + panel.width &&
                    placed.y + placed.height <= panel.y + panel.height,
                "the OA button leaves the in-game menu" + where
            );
            require(
                std::abs(placed.x - panel.x - at(OaLayer::match_button_source_x - menu_panel.x)) <=
                        1 &&
                    std::abs(
                        placed.y - panel.y - at(OaLayer::match_button_source_y - menu_panel.y)
                    ) <= 1,
                "the OA button is not under Resume" + where
            );
        };
        check_in_menu(match_layout_, on);
        for (const auto& [placed_width, placed_height] : kPlacedWindows) {
            const auto plain = layout::make_match_layout(placed_width, placed_height);
            const std::string window =
                " on a " + std::to_string(placed_width) + 'x' + std::to_string(placed_height);
            check_in_menu(plain, window + " window");
            check_in_menu(
                layout::fit_side_column(plain, kTallPageRows),
                window + " window whose side column a taller page narrows"
            );
        }
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, {width - 1, height - 1}, 0);
        render();
        const auto menu = composed();
        write_ppm(
            report_directory /
                ("native-engine-settings-menu-" + std::to_string(width) + hud_suffix + ".ppm"),
            menu
        );
        if (!options_.snapshot.empty())
            write_ppm(step_snapshot(options_.snapshot, "match-menu-" + shot), menu);
        drawn_at(
            menu,
            button_face(settings::ButtonLook::idle, false, fonts),
            button,
            "the OA button" + on
        );
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, centre(button), 0);
        render();
        drawn_at(
            composed(),
            button_face(settings::ButtonLook::hovered, false, fonts),
            button,
            "the hovered OA button" + on
        );
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, centre(button), SDL_BUTTON_LEFT);
        require(
            engine_settings_dialog() == nullptr, "a press opened the dialog before its release"
        );
        render();
        drawn_at(
            composed(),
            button_face(settings::ButtonLook::pressed, false, fonts),
            button,
            "the pressed OA button" + on
        );

        // Its release opens the dialog beside the darkened column; a game
        // played alone stays paused.
        const uint32_t tick = match_timing_.tick;
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_UP, centre(button), SDL_BUTTON_LEFT);
        auto* dialog = engine_settings_dialog();
        require(dialog != nullptr, "a click on the OA button did not open the dialog" + on);
        {
            // Laid out, placed and taking input at its class and scale.
            const LayerPlacement placed = oa_layer().placement_of("settings");
            require(
                dialog->size_class == checked.size_class && placed.shown.x == dialog_at.x &&
                    placed.shown.y == dialog_at.y && placed.shown.width == dialog_at.width &&
                    placed.shown.height == dialog_at.height &&
                    placed.points_width == class_metrics.dialog_width &&
                    placed.points_height == class_metrics.dialog_height &&
                    placed.scale_percent == checked.scale * 100,
                "the open dialog is not laid out and placed at its class and scale" + on
            );
        }
        require(
            match_paused_ && ingame_menu_column_shown(),
            "the in-game menu did not stay under the dialog" + on
        );
        require(
            dialog->locks.path_search == settings::Lock::in_game &&
                dialog->locks.unit_limit == settings::Lock::in_game && !dialog->locks.shared_game,
            "a game played alone does not lock Pathfinding cycles and Unit limit" + on
        );
        for (int frame = 0; frame < kPausedFrames; ++frame) {
            SDL_Delay(kPausedFrameMs);
            idle_tick();
        }
        require(
            match_timing_.tick == tick && engine_settings_dialog() != nullptr,
            "the game ran on under the dialog" + on
        );
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, {width - 1, height - 1}, 0);
        render();
        const auto open = composed();
        write_ppm(
            report_directory /
                ("native-engine-settings-match-" + std::to_string(width) + hud_suffix + ".ppm"),
            open
        );
        // Drawn at its class and scale, copied pixel for pixel: at 640x480
        // Compact at 1x, 0.7.3's dialog.
        copied_at(
            open,
            dialog_face_at(*dialog, fonts, checked.size_class, checked.scale),
            dialog_at,
            oa_layer().placement_of("settings"),
            "the dialog" + on
        );
        drawn_at(
            open,
            button_face(settings::ButtonLook::idle, true, fonts),
            button,
            "the darkened OA button" + on
        );
        // A point in the column, above the button, is darkened.
        const layout::Point column_point{button.x, button.y - button.height};
        const auto column_offset = (static_cast<std::size_t>(column_point.y) * open.width +
                                    static_cast<std::size_t>(column_point.x)) *
                                   3U;
        renderer::Surface backdrop;
        backdrop.width = 1;
        backdrop.height = 1;
        backdrop.rgb = {
            settings::backdrop_color[0], settings::backdrop_color[1], settings::backdrop_color[2]
        };
        backdrop = shown(backdrop);
        const unsigned alpha = (settings::ingame_backdrop_opacity * 255U + 128U) / 256U;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const unsigned under = menu.rgb[column_offset + channel];
            require(
                open.rgb[column_offset + channel] ==
                    (backdrop.rgb[channel] * alpha + under * (255U - alpha)) / 255U,
                "the in-game menu's column is not darkened under the dialog" + on
            );
        }
        // The presented frame is the composed one.
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        const auto again = composed();
        require(
            presented.width == again.width && presented.height == again.height,
            "the presented frame is not the composed frame's size" + on
        );
        std::size_t differing = 0;
        const auto near_cursor = [this](int x, int y) {
            const auto near = [x, y](float cursor_x, float cursor_y) {
                return std::abs(x - static_cast<int>(cursor_x)) < kCursorReach &&
                       std::abs(y - static_cast<int>(cursor_y)) < kCursorReach;
            };
            return near(pointer_x_, pointer_y_) || near(match_pointer_x_, match_pointer_y_);
        };
        for (int y = 0; y < static_cast<int>(again.height); ++y)
            for (int x = 0; x < static_cast<int>(again.width); ++x) {
                if (near_cursor(x, y))
                    continue;
                const auto offset =
                    (static_cast<std::size_t>(y) * again.width + static_cast<std::size_t>(x)) * 3U;
                for (std::size_t channel = 0; channel < 3; ++channel)
                    if (presented.rgb[offset + channel] != again.rgb[offset + channel]) {
                        ++differing;
                        break;
                    }
            }
        if (differing != 0) {
            write_ppm(report_directory / "native-engine-settings-composed.ppm", again);
            write_ppm(report_directory / "native-engine-settings-presented.ppm", presented);
        }
        require(
            differing == 0,
            std::to_string(differing) + " presented pixels differ from the composed frame" + on
        );
        // A click on a section's entry, at its middle as the dialog draws it,
        // shows that section.
        {
            const settings::Page was = engine_settings_dialog()->page;
            const settings::Page other = was == settings::Page::graphics
                                             ? settings::Page::common_tweaks
                                             : settings::Page::graphics;
            show_page(other, on);
            require(
                engine_settings_dialog()->page != was,
                "a click on a section's entry did not change the section" + on
            );
        }
        std::cout << "engine settings check: in a match the dialog is "
                  << size_class_name(checked.size_class) << ", " << checked.scale << "x, on the "
                  << size << " window\n";
        if (!checked.hud_scaling) {
            // With HUD scaling Off the window checks the dialog's class and
            // scale, its drawing and its clicks. The parts below expect every
            // setting at its default, which HUD scaling Off is not; the
            // windows above check them. Escape is Cancel.
            tap_key(SDLK_ESCAPE, SDL_KMOD_NONE);
            require(engine_settings_dialog() == nullptr, "Escape did not close the dialog" + on);
            resume_match_pause();
            std::cout << "engine settings check: the in-game menu's OA button at " << button.x
                      << ',' << button.y << " and the dialog at " << dialog_at.x << ','
                      << dialog_at.y << on << '\n';
            continue;
        }
        expect_locks("Locked during a game", "match-dialog-alone", shot, on);
        expect_mod_locked(on);
        require(
            !shows_text(
                settings::dialog_layout(*engine_settings_dialog()), "Shared game - still running"
            ),
            "a game played alone says it is shared" + on
        );
        // Graphics at its end in a game played alone: neither Hardware
        // acceleration nor Vertical sync locked by the game.
        show_page(settings::Page::graphics, on);
        tap_key(SDLK_END, SDL_KMOD_NONE);
        {
            const auto* graphics = engine_settings_dialog();
            // Its end: at Compact kMatchGraphicsEnd; a larger class's view
            // shows more and scrolls less.
            const int32_t limit = graphics != nullptr ? settings::scroll_limit(*graphics) : -1;
            require(
                graphics != nullptr &&
                    graphics->scroll[static_cast<std::size_t>(settings::Page::graphics)] == limit &&
                    (graphics->size_class != kit::SizeClass::compact || limit == kMatchGraphicsEnd),
                "End did not scroll Graphics to its end (" + std::to_string(limit) + ")" + on
            );
            require(
                graphics->locks.hardware_acceleration != settings::Lock::in_game &&
                    graphics->locks.vertical_sync != settings::Lock::in_game,
                "a game played alone locks Hardware acceleration or Vertical sync" + on
            );
        }
        // No snapshot here: a frame drawn while the game is paused would move
        // the later shared game's clock, and with it the snapshots taken there.
        tap_key(SDLK_HOME, SDL_KMOD_NONE);
        // Window frame and HUD scaling are on User Interface in a game too.
        show_page(settings::Page::user_interface, on);
        {
            const auto parts = settings::dialog_layout(*engine_settings_dialog());
            require(
                shows_text(parts, "Window frame") && shows_text(parts, "HUD scaling"),
                "User Interface does not show Window frame and HUD scaling" + on
            );
        }
        show_page(settings::Page::graphics, on);

        // The wheel over the dialog reaches it and not the battlefield: given
        // a section taller than its view, a notch scrolls it, as far as its
        // end, the zoom stays and the dialog is drawn again; Home scrolls it
        // back. A view that holds the nine rows whole scrolls nothing, and
        // the zoom still stays.
        {
            namespace tall = engine_settings_check;

            // The dialog shows its own sections again however the block
            // ends, and the next frame draws them.
            struct OwnSections {
                Runtime& runtime;

                ~OwnSections() {
                    if (auto* shown = runtime.engine_settings_dialog()) {
                        tall::show_own_sections(*shown);
                        runtime.oa_layer().redraw();
                    }
                }
            } own_sections{*this};

            tall::show_tall_section(*engine_settings_dialog());
            const auto settings_revision = [this] {
                const auto* screen = oa_layer().find("settings");
                return screen != nullptr ? screen->revision() : uint64_t{0};
            };
            const auto offset = [this] {
                const auto* open = engine_settings_dialog();
                return open->scroll[static_cast<std::size_t>(open->page)];
            };
            const int32_t tall_limit = settings::scroll_limit(*engine_settings_dialog());
            const bool scrolls = tall_limit > 0;
            const float zoom = match_zoom_target_;
            const uint64_t revision = settings_revision();
            const auto over = centre(dialog_at);
            SDL_Event wheel =
                check_host_input::wheel_event(sdl_.renderer, sdl_.window, over.x, over.y, -1.0F);
            dispatch_event(wheel, running);
            require(running, "the wheel in the dialog ended the run" + on);
            require(
                offset() == std::min(tall::kWheelStepPixels, tall_limit),
                scrolls ? "a notch of the wheel did not scroll the dialog" + on
                        : "a notch of the wheel scrolled a section that fits its view" + on
            );
            require(
                match_zoom_target_ == zoom, "the wheel over the dialog zoomed the battlefield" + on
            );
            require(
                !scrolls || settings_revision() != revision,
                "a scroll did not draw the dialog again" + on
            );
            tap_key(SDLK_HOME, SDL_KMOD_NONE);
            require(offset() == 0, "Home did not scroll the dialog back to its top" + on);
        }

        // A press beside the dialog does nothing; Escape is Cancel and puts
        // the opened settings back, and back to the in-game menu; while it is
        // held the menu does not see it.
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, column_point, SDL_BUTTON_LEFT);
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_UP, column_point, SDL_BUTTON_LEFT);
        require(
            engine_settings_dialog() != nullptr && ingame_menu_column_shown(),
            "a press beside the dialog reached the in-game menu" + on
        );
        const auto opened = engine_settings_dialog()->opened;
        engine_settings_dialog()->chosen.wheel_zoom = !opened.wheel_zoom;
        (void)take_engine_settings_action(settings::DialogAction::changed);
        send_key(SDLK_ESCAPE, SDL_KMOD_NONE, true, false);
        require(engine_settings_dialog() == nullptr, "Escape did not close the dialog" + on);
        require(
            engine_settings().wheel_zoom == opened.wheel_zoom,
            "Escape did not put the opened settings back" + on
        );
        require(ingame_menu_column_shown(), "Escape did not go back to the in-game menu" + on);
        send_key(SDLK_ESCAPE, SDL_KMOD_NONE, true, true);
        require(
            ingame_menu_column_shown() && match_paused_,
            "a held Escape reached the in-game menu" + on
        );
        send_key(SDLK_ESCAPE, SDL_KMOD_NONE, false, false);

        // The shortcut opens it from the menu, and Enter is OK.
        send_key(SDLK_COMMA, kShortcutModifier, true, false);
        require(engine_settings_dialog() != nullptr, "the shortcut did not open the dialog" + on);
        send_key(SDLK_COMMA, kShortcutModifier, false, false);
        tap_key(SDLK_RETURN, SDL_KMOD_NONE);
        require(
            engine_settings_dialog() == nullptr && ingame_menu_column_shown(),
            "Enter did not close the dialog back to the in-game menu" + on
        );

        // From play the comma alone opens nothing; the shortcut opens the
        // in-game menu with the dialog over it, and pauses.
        resume_match_pause();
        require(!match_paused_, "the in-game menu did not resume" + on);
        tap_key(SDLK_COMMA, SDL_KMOD_NONE);
        require(engine_settings_dialog() == nullptr, "the comma alone opened the dialog" + on);
        send_key(SDLK_COMMA, kShortcutModifier, true, false);
        send_key(SDLK_COMMA, kShortcutModifier, false, false);
        require(
            engine_settings_dialog() != nullptr && ingame_menu_column_shown() && match_paused_,
            "the shortcut in play did not open the dialog over the paused in-game menu" + on
        );
        tap_key(SDLK_ESCAPE, SDL_KMOD_NONE);
        require(engine_settings_dialog() == nullptr, "Escape did not close the dialog" + on);

        // The dialog closes when the menu no longer shows under it.
        send_key(SDLK_COMMA, kShortcutModifier, true, false);
        send_key(SDLK_COMMA, kShortcutModifier, false, false);
        require(engine_settings_dialog() != nullptr, "the shortcut did not open the dialog" + on);
        resume_match_pause();
        tick_screen_packages();
        require(
            engine_settings_dialog() == nullptr,
            "the dialog stayed open without the in-game menu" + on
        );

        // A shared game runs on under the dialog, which says so, and the host
        // sets Pathfinding cycles and Unit limit. The match turns shared
        // here rather than at its loading screen, so the tier is told so
        // as a shared game's bootstrap tells it.
        extension_.state = [](void*, const Runtime&) -> uint32_t {
            return extension_state::multiplayer | extension_state::shared_match;
        };
        begin_render_tier_match(render_policy::MatchKind::shared_game);
        show_match_pause_menu();
        send_key(SDLK_COMMA, kShortcutModifier, true, false);
        send_key(SDLK_COMMA, kShortcutModifier, false, false);
        dialog = engine_settings_dialog();
        require(dialog != nullptr, "the shortcut did not open the dialog in a shared game" + on);
        require(
            dialog->locks.path_search == settings::Lock::set_by_host &&
                dialog->locks.unit_limit == settings::Lock::set_by_host &&
                dialog->locks.shared_game,
            "a shared game does not leave Pathfinding cycles and Unit limit to the host" + on
        );
        require(match_clock_steps(), "a shared game stopped under the dialog" + on);
        require(
            shows_text(settings::dialog_layout(*dialog), "Shared game - still running"),
            "the dialog does not say the shared game is still running" + on
        );
        expect_locks("Set by the host", "match-dialog-shared", shot, on);
        expect_mod_locked(on);
        // Graphics in a shared game, a page down to show both rows at
        // Compact, whose view a larger class's holds from its top: Vertical
        // sync is locked during the game, its value set before the game kept
        // in effect; Hardware acceleration can still be set, and Basic or
        // Full waits for the game to end, so the processor keeps drawing.
        // Escape then puts Off back.
        show_page(settings::Page::graphics, on);
        if (engine_settings_dialog()->size_class == kit::SizeClass::compact)
            tap_key(SDLK_PAGEDOWN, SDL_KMOD_NONE);
        dialog = engine_settings_dialog();
        require(
            dialog->locks.vertical_sync ==
                (options_.force_capable ? settings::Lock::in_game : settings::Lock::unavailable),
            "a shared game does not lock Vertical sync" + on
        );
        require(
            dialog->locks.hardware_acceleration == settings::Lock::none ||
                dialog->locks.hardware_acceleration == settings::Lock::unavailable,
            "a shared game locks Hardware acceleration" + on
        );
        require(
            shows_text(settings::dialog_layout(*dialog), "Locked during a game") ||
                !options_.force_capable,
            "Vertical sync does not say it is locked during the game" + on
        );
        if (dialog->locks.hardware_acceleration == settings::Lock::none) {
            // Each level of the strip, Full then Basic, waits for the game's
            // end and says which level then takes effect.
            const auto choose = [&](std::string_view caption,
                                    settings::HardwareAcceleration level,
                                    std::string_view waiting) {
                // The layout outlives the loop: segment points into it.
                const auto strip_layout = settings::dialog_layout(*dialog);
                const settings::LayoutPart* segment = nullptr;
                for (const auto& part : strip_layout)
                    if (part.control == settings::first_row_control + 3 && part.text == caption)
                        segment = &part;
                require(
                    segment != nullptr,
                    "Hardware acceleration's " + std::string(caption) + " does not show" + on
                );
                const auto rect = segment->rect;
                click_dialog(rect);
                tick_screen_packages();
                dialog = engine_settings_dialog();
                require(
                    dialog != nullptr && engine_settings().hardware_acceleration == level,
                    "Hardware acceleration was not set to " + std::string(caption) +
                        " in a shared game" + on
                );
                require(
                    dialog->acceleration.state == settings::AccelerationState::waiting_for_game_end,
                    "Hardware acceleration " + std::string(caption) +
                        " in a shared game does not wait for its end" + on
                );
                require(
                    shows_text(settings::dialog_layout(*dialog), waiting),
                    "the status does not say " + std::string(caption) + " waits for the next game" +
                        on
                );
            };
            choose(
                "Full",
                settings::HardwareAcceleration::full,
                "Off for this game: in a shared game, Full"
            );
            choose(
                "Basic",
                settings::HardwareAcceleration::basic,
                "Off for this game: in a shared game, Basic"
            );
            snapshot("match-dialog-shared-graphics", shot);
        }
        tap_key(SDLK_ESCAPE, SDL_KMOD_NONE);
        require(
            engine_settings().hardware_acceleration == settings::HardwareAcceleration::off,
            "Escape did not put Hardware acceleration back Off" + on
        );
        require(engine_settings_dialog() == nullptr, "Escape did not close the dialog" + on);
        extension_ = saved_extension;
        begin_render_tier_match(render_policy::MatchKind::none);
        resume_match_pause();
        std::cout << "engine settings check: the in-game menu's OA button at " << button.x << ','
                  << button.y << " and the dialog at " << dialog_at.x << ',' << dialog_at.y << on
                  << '\n';
    }
    // HUD scaling as it was, kept and in effect: the game is laid out again.
    engine_settings_state().current.hud_scaling = kept_hud_scaling;
    require(!EngineSettingsState::flush(*this), "HUD scaling was not written back");
    apply_output_mode();
    // A Screen size chosen in a game applies when OK is pressed: the window
    // takes it at once and the match is laid out again at it, the game
    // keeping its tick and its world; the window and the setting then go
    // back as they were.
    {
        require(match_ != nullptr, "no game is left for the Screen size");
        const auto pump_window_events = [&] {
            SDL_SyncWindow(sdl_.window);
            SDL_Event event{};
            while (SDL_PollEvent(&event))
                dispatch_event(event, running);
            require(running, "a window event ended the run");
        };
        // The world's digest, the camera aside, which a new layout may move.
        const auto world_digest = [this] {
            const auto kept_x = match_camera_x_;
            const auto kept_z = match_camera_z_;
            match_camera_x_ = 0;
            match_camera_z_ = 0;
            const auto digest = match_world_digest();
            match_camera_x_ = kept_x;
            match_camera_z_ = kept_z;
            return digest;
        };
        int before_width = 0;
        int before_height = 0;
        SDL_GetWindowSize(sdl_.window, &before_width, &before_height);
        const auto kept_values = preference_values_;
        const auto kept_setting = engine_settings_state().current.screen_size;
        constexpr settings::ScreenSize kChosenSize{1280, 720};
        require(
            before_width != kChosenSize.width || before_height != kChosenSize.height,
            "the window is already 1280x720"
        );
        show_match_pause_menu();
        const auto button = OaLayer::match_button_rect(match_layout_);
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, centre(button), 0);
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, centre(button), SDL_BUTTON_LEFT);
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_UP, centre(button), SDL_BUTTON_LEFT);
        require(engine_settings_dialog() != nullptr, "the OA button did not open the dialog");
        const uint32_t tick = match_timing_.tick;
        const auto digest = world_digest();
        show_page(settings::Page::graphics, " for the Screen size");
        for (int press = 0;
             press < 16 && engine_settings_dialog()->focused != settings::first_row_control + 2;
             ++press)
            tap_key(SDLK_DOWN, SDL_KMOD_NONE);
        for (int press = 0;
             press < 64 && engine_settings_dialog()->chosen.screen_size != kChosenSize;
             ++press)
            tap_key(SDLK_LEFT, SDL_KMOD_NONE);
        require(
            engine_settings_dialog()->chosen.screen_size == kChosenSize,
            "Screen size never reached 1280x720 in a game"
        );
        tap_key(SDLK_RETURN, SDL_KMOD_NONE);
        pump_window_events();
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(sdl_.window, &width, &height);
        require(
            engine_settings_dialog() == nullptr && width == kChosenSize.width &&
                height == kChosenSize.height && match_layout_.width == kChosenSize.width &&
                match_layout_.height == kChosenSize.height,
            "OK on a Screen size of 1280x720 in a game left the window at " +
                std::to_string(width) + 'x' + std::to_string(height) + " and the match at " +
                std::to_string(match_layout_.width) + 'x' + std::to_string(match_layout_.height)
        );
        require(
            match_ && match_timing_.tick == tick && world_digest() == digest,
            "a Screen size applied in a game changed its tick or its world"
        );
        std::cout << "engine settings check: a Screen size of 1280x720 chosen in a game resized "
                     "the window from "
                  << before_width << 'x' << before_height << " at tick " << tick << '\n';
        resume_match_pause();
        preference_values_ = kept_values;
        engine_settings_state().current.screen_size = kept_setting;
        require(!EngineSettingsState::flush(*this), "the preferences were not written back");
        SDL_SetWindowSize(sdl_.window, before_width, before_height);
        pump_window_events();
    }
    // Interface size Auto at the start of a game is the scale a resize to
    // the same window leaves. A size chosen in the dialog redraws at once,
    // and Cancel puts back the size the dialog opened with.
    {
        const auto pump_resize = [&] {
            SDL_SyncWindow(sdl_.window);
            SDL_Event event{};
            while (SDL_PollEvent(&event))
                dispatch_event(event, running);
            require(running, "a window event ended the run");
        };
        const auto preference_of = [this] {
            const auto stored = preference_values_.find(std::string(settings::key::interface_size));
            return stored == preference_values_.end() ? std::string{} : stored->second;
        };
        const auto read_dialog = [this] {
            return OaLayer::match_dialog(match_layout_, oa_layer().view().interface_percent);
        };
        leave_match();
        load(Screen::main_menu);
        engine_settings_state().current.hud_scaling = true;
        engine_settings_state().current.interface_size = settings::InterfaceSize::automatic;
        constexpr int kWide = 2103;
        constexpr int kTall = 1181;
        if (!SDL_SetWindowSize(sdl_.window, kWide, kTall) || !SDL_SyncWindow(sdl_.window))
            throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
        start_benchmark_skirmish();
        const auto started = read_dialog();
        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(sdl_.window, &window_width, &window_height);
        std::cout << "engine settings check: Interface size Auto at the start on a " << window_width
                  << 'x' << window_height << " window, canvas " << match_layout_.width << 'x'
                  << match_layout_.height << ", density " << match_layout_.px_per_point << ", "
                  << size_class_name(started.size_class) << " at " << started.scale_percent
                  << "%, dialog " << started.shown.width << 'x' << started.shown.height << '\n';
        require(
            window_width == kWide && window_height == kTall && match_layout_.width == kWide &&
                match_layout_.height == kTall && started.size_class == kit::SizeClass::large &&
                started.scale_percent == 100 && started.shown.width == 960 &&
                started.shown.height == 600,
            "Auto at the start of a 2103x1181 game is not Large at 100%, dialog 960x600"
        );
        // The modern fonts are Off with the check's named preferences file, so
        // the lines above are whole steps: 2x would leave 1051 by 590 points,
        // Regular, and Auto steps down to Large at 1x. On, 1181 lines is 150%
        // and the window's 1402 by 787 points Large. Off again, the
        // whole-step scale returns.
        engine_settings_state().current.modern_fonts = true;
        require(oa_layer().smooth_fonts() != nullptr, "the modern fonts did not open");
        const auto fractional = read_dialog();
        require(
            fractional.size_class == kit::SizeClass::large && fractional.scale_percent == 150 &&
                fractional.shown.width == 1440 && fractional.shown.height == 900,
            "Auto with the modern fonts On at the start of a 2103x1181 game is not Large at "
            "150%, dialog 1440x900"
        );
        std::cout << "engine settings check: Interface size Auto with the modern fonts On is "
                  << size_class_name(fractional.size_class) << " at " << fractional.scale_percent
                  << "%, dialog " << fractional.shown.width << 'x' << fractional.shown.height
                  << '\n';
        engine_settings_state().current.modern_fonts = false;
        const auto whole_again = read_dialog();
        require(
            whole_again.size_class == started.size_class &&
                whole_again.scale_percent == started.scale_percent &&
                whole_again.shown.width == started.shown.width &&
                whole_again.shown.height == started.shown.height,
            "Auto with the modern fonts Off is not Large at 100% again"
        );
        // A different size, reported and not yet handled: the scale after
        // following the window equals the scale once the resize is handled.
        constexpr int kAwayWidth = 1280;
        constexpr int kAwayHeight = 720;
        if (!SDL_SetWindowSize(sdl_.window, kAwayWidth, kAwayHeight))
            throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
        follow_window_output();
        const auto followed = read_dialog();
        pump_resize();
        const auto pumped = read_dialog();
        require(
            followed.size_class == kit::SizeClass::large && followed.scale_percent == 100 &&
                followed.size_class == pumped.size_class &&
                followed.scale_percent == pumped.scale_percent &&
                followed.shown.width == pumped.shown.width &&
                followed.shown.height == pumped.shown.height,
            "Auto after a resize was not the scale the resize leaves once handled"
        );
        if (!SDL_SetWindowSize(sdl_.window, kWide, kTall))
            throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
        follow_window_output();
        const auto returned = read_dialog();
        pump_resize();
        const auto returned_handled = read_dialog();
        require(
            returned.size_class == started.size_class &&
                returned.scale_percent == started.scale_percent &&
                returned.shown.width == started.shown.width &&
                returned.shown.height == started.shown.height &&
                returned_handled.size_class == started.size_class &&
                returned_handled.scale_percent == started.scale_percent &&
                returned_handled.shown.width == started.shown.width &&
                returned_handled.shown.height == started.shown.height,
            "Auto after a resize back to 2103x1181 is not the scale at the start"
        );
        std::cout << "engine settings check: Interface size Auto after a resize back to " << kWide
                  << 'x' << kTall << " is " << size_class_name(returned.size_class) << " at "
                  << returned.scale_percent << "%, dialog " << returned.shown.width << 'x'
                  << returned.shown.height << '\n';
        // 200%, two steps on from Auto past 100% (150% is not offered with
        // the modern fonts Off), redraws the open dialog at once: Regular at
        // 2x, 1051 by 590 points. The page, the focus and the scroll stay,
        // and the preference is kept for OK. Escape puts Auto back.
        const std::string preference_before = preference_of();
        open_engine_settings_in_match();
        require(engine_settings_dialog() != nullptr, "the settings did not open in the game");
        show_page(settings::Page::user_interface, " for Interface size");
        // Interface size, the fourth row of User Interface, which Tab reaches.
        const int32_t size_row = settings::first_row_control + 3;
        for (int press = 0; press < 64 && engine_settings_dialog()->focused != size_row; ++press)
            tap_key(SDLK_TAB, SDL_KMOD_NONE);
        auto* dialog = engine_settings_dialog();
        require(
            dialog != nullptr && dialog->focused == size_row,
            "Interface size's row did not take the focus in a game"
        );
        const auto scroll_index = static_cast<std::size_t>(settings::Page::user_interface);
        const int32_t scroll_before = dialog->scroll[scroll_index];
        tap_key(SDLK_RIGHT, SDL_KMOD_NONE);
        tap_key(SDLK_RIGHT, SDL_KMOD_NONE);
        dialog = engine_settings_dialog();
        const auto chosen_place = read_dialog();
        require(dialog != nullptr, "choosing Interface size 200% closed the dialog");
        const int32_t limit_now = settings::scroll_limit(*dialog);
        const int32_t shown_scroll =
            std::clamp(dialog->scroll[scroll_index], int32_t{0}, limit_now);
        require(
            dialog->page == settings::Page::user_interface && dialog->focused == size_row &&
                dialog->chosen.interface_size == settings::InterfaceSize::size_200 &&
                engine_settings().interface_size == settings::InterfaceSize::size_200 &&
                dialog->size_class == kit::SizeClass::regular &&
                chosen_place.size_class == kit::SizeClass::regular &&
                chosen_place.scale_percent == 200 && chosen_place.shown.width == 1440 &&
                chosen_place.shown.height == 972 &&
                (dialog->scroll[scroll_index] == scroll_before ||
                 shown_scroll == std::min(scroll_before, limit_now)) &&
                preference_of() == preference_before,
            "Interface size 200% in a game did not take effect at once on its row"
        );
        std::cout << "engine settings check: Interface size 200% in a game takes effect at once, "
                     "dialog "
                  << chosen_place.shown.width << 'x' << chosen_place.shown.height << '\n';
        tap_key(SDLK_ESCAPE, SDL_KMOD_NONE);
        const auto restored = read_dialog();
        require(
            engine_settings_dialog() == nullptr &&
                engine_settings().interface_size == settings::InterfaceSize::automatic &&
                preference_of() == preference_before && restored.size_class == started.size_class &&
                restored.scale_percent == started.scale_percent &&
                restored.shown.width == started.shown.width &&
                restored.shown.height == started.shown.height,
            "Escape did not put the game's Interface size back to Auto"
        );
    }
    leave_match();
    load(Screen::main_menu);
    // Back on the main menu the Library's PLAY NOW and SWITCH OFF can be
    // pressed again, and the dialog still lists no Mods section.
    send_key(SDLK_COMMA, kShortcutModifier, true, false);
    send_key(SDLK_COMMA, kShortcutModifier, false, false);
    {
        const auto* dialog = engine_settings_dialog();
        require(
            dialog != nullptr && !shows_text(settings::dialog_layout(*dialog), "Mods"),
            "the main menu's dialog lists a Mods section after a game"
        );
        library::Inputs inputs = library_inputs();
        for (const std::string_view key : {kPlayedStandIn, kOtherStandIn}) {
            library::Installed stand_in;
            stand_in.kind = library::Kind::mod;
            stand_in.key = std::string(key);
            stand_in.name = std::string(key);
            stand_in.folder = user_folder_ / std::string(key);
            stand_in.playing = key == kPlayedStandIn;
            inputs.installed.push_back(std::move(stand_in));
        }
        LibraryHost& host = library_host();
        library::refresh(host.model, std::move(inputs));
        bool on = !host.model.inputs.in_match;
        for (const std::string_view key : {kPlayedStandIn, kOtherStandIn}) {
            const library::Entry* entry =
                library::find_entry(host.model, {{}, library::Kind::mod, std::string(key)});
            const library::Action wanted =
                key == kPlayedStandIn ? library::Action::switch_off : library::Action::play_now;
            bool enabled = false;
            if (entry != nullptr)
                for (const library::ActionButton& button :
                     library::entry_actions(host.model, *entry))
                    enabled = enabled || (button.action == wanted && button.enabled);
            on = on && enabled;
        }
        host.refreshed = false;
        require(on, "PLAY NOW or SWITCH OFF cannot be pressed on the main menu after a game");
    }
    tap_key(SDLK_ESCAPE, SDL_KMOD_NONE);
    require(engine_settings_dialog() == nullptr, "Escape did not close the main menu's dialog");
    if (!SDL_SetWindowSize(sdl_.window, kDefaultWindowWidth, kDefaultWindowHeight) ||
        !SDL_SyncWindow(sdl_.window))
        throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
    std::cout << "engine settings check: the in-game menu\n";
}

} // namespace oa::app
