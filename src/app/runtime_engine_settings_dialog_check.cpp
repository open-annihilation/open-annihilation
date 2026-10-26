// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-engine-settings, the dialog's part: the dialog on the main menu
// driven through the SDL presenter's pointer and keys (every section, each
// setting changed and in effect at once, Vertical sync read back from the
// renderer, Menu scaling's whole steps in the window's presentation at once,
// Native pixel density kept for the next start, Smooth motion's Off and
// Explosion flash's Reduced in effect for the next frame drawn, the zoom's
// limits stepped by the keys on their drop-downs, View past the map's
// edge's 25% in effect at once, Window edge's strip and Edge stickiness's
// slider, locked but for Sticky, in effect at once and fitting the 640x480
// dialog in English and German,
// System's six file-type switches fitting it too, OK,
// Cancel and Restore defaults and the preferences they leave, Developer
// Mode's overrides in effect and kept), and the main menu
// with its OA button and Settings button and the dialog as the window shows
// them at several sizes, the dialog at the size class and scale each window
// gives it: the Graphics section at its top and its end, and the Developer
// section with an area of Developer Mode's list open, off and on.

#include "check_host_input.hpp"
#include "engine_settings_menu_host.hpp"
#include "engine_settings_state.hpp"
#include "engine_settings_tall_section.hpp"
#include "language_state.hpp"
#include "library_host.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/acceleration_status.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/game_files_hooks.hpp"
#include "oa/app/mod_profile_loader.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/update/checker.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/library/library.hpp"

#include <SDL3/SDL.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace fs = std::filesystem;
namespace settings = oa::ui::engine_settings;
namespace library = oa::ui::library;
namespace artless = oa::ui::frontend_renderer;
namespace layout = oa::ui::display_layout;

/// The modifier the settings' shortcut takes on this platform.
#ifdef SDL_PLATFORM_MACOS
constexpr SDL_Keymod kShortcutModifier = SDL_KMOD_GUI;
#else
constexpr SDL_Keymod kShortcutModifier = SDL_KMOD_CTRL;
#endif

/// A point of the main menu's picture over none of its buttons and outside
/// the dialog, where the pointer rests between the check's steps.
constexpr layout::Point kRestingPointer{4, 240};

/// The profile of the mod folder the check picks: one that changes nothing.
constexpr std::string_view kPickedProfile = "oamod: 1\n"
                                            "id: picked-check\n"
                                            "name: Picked folder check\n"
                                            "version: \"1.0\"\n"
                                            "requires: {base: ta-3.1c, catalogue: 1}\n"
                                            "author: {name: unknown}\n"
                                            "packaging: {revision: 1, date: 2026-10-04, "
                                            "packager: Open Annihilation}\n";

/// Where the pointer rests while a snapshot is taken: the picture's
/// bottom-right pixel, so that the cursor draws almost wholly outside it.
constexpr layout::Point kSnapshotPointer{kCanvasWidth - 1, kCanvasHeight - 1};

/// The OA button's top left corner on the main menu's picture, near its
/// bottom-right corner: one button and a quarter of a button's side left of
/// the Settings button.
constexpr layout::Point kMenuButtonCorner{520, 420};

/// The Settings button's top left corner on the main menu's picture, in its
/// bottom-right corner.
constexpr layout::Point kMenuSettingsCorner{580, 420};

/// How far round the pointer the cursor may draw, in the picture's pixels.
constexpr int32_t kCursorReach = 40;

/// The window sizes the main menu is shown at: the picture's own, a 4:3
/// one, two 16:9 sizes and an ultrawide one.
constexpr std::array<std::pair<int, int>, 5> kWindowSizes{{
    {640, 480},
    {1024, 768},
    {1280, 720},
    {1920, 1080},
    {2560, 1080},
}};

/// The size class and the scale the OA layer lays the dialog out at and
/// draws it at on a window.
struct WindowLayout {
    oa::ui::kit::SizeClass size_class{}; ///< the class of the points left over
    int32_t scale{};                     ///< window pixels a point
};

/// The class and scale each of kWindowSizes gives, in its order: the
/// design's table of windows. The check's preferences file leaves the
/// modern fonts Off, so Auto keeps whole steps.
constexpr std::array<WindowLayout, 5> kWindowLayouts{{
    {oa::ui::kit::SizeClass::compact, 1},
    {oa::ui::kit::SizeClass::regular, 1},
    {oa::ui::kit::SizeClass::large, 1},
    {oa::ui::kit::SizeClass::regular, 2},
    {oa::ui::kit::SizeClass::regular, 2},
}};
static_assert(kWindowLayouts.size() == kWindowSizes.size(), "one layout for each window size");

/// Graphics' scroll limit at Compact: its twenty-one rows, Smooth motion and
/// Fog of War among them, after Menu scaling, Window frame, HUD scaling and
/// Interface size left for User Interface.
constexpr int32_t kGraphicsScrollLimit = 972;
/// Graphics' scroll limit at Large, whose taller view holds more of its rows.
constexpr int32_t kGraphicsLargeScrollLimit = 600;
/// Interface size's place on User Interface, from the section's first row.
constexpr int32_t kInterfaceSizeRow = 3;

/// The window the Interface size is chosen on, through its drop-down.
constexpr std::pair<int, int> kInterfaceSizeWindow{1920, 1080};

/// An Interface size the check chooses, and the class and scale the OA
/// layer lays the dialog out and draws it at then on kInterfaceSizeWindow.
struct InterfaceSizeStep {
    settings::InterfaceSize size{};      ///< the size chosen
    std::string_view caption;            ///< its item in the drop-down
    oa::ui::kit::SizeClass size_class{}; ///< the class of the points left over
    int32_t scale{};                     ///< window pixels a point
};

/// The Interface sizes chosen in turn: 100% is Large at 1x; 400% is
/// Compact at 3x, as 4x does not fit Compact's 480 by 324 points in 1080
/// rows and 3x (1440 by 972) does; Auto, with the modern fonts Off, is the
/// window's own Regular at 2x.
constexpr std::array<InterfaceSizeStep, 3> kInterfaceSizeSteps{{
    {settings::InterfaceSize::size_100, "100%", oa::ui::kit::SizeClass::large, 1},
    {settings::InterfaceSize::size_400, "400%", oa::ui::kit::SizeClass::compact, 3},
    {settings::InterfaceSize::automatic, "Auto", oa::ui::kit::SizeClass::regular, 2},
}};

/// The preferences keys' common start: the keys of the Open Annihilation settings.
constexpr std::string_view kEngineKeyPrefix = "open-annihilation.";

/// The Pathfinding cycles the check chooses with the keys: 2x.
constexpr int32_t kChosenPathNodes = settings::base_path_search_nodes * 2;
/// The unit limit the check chooses with the keys: one stop above the default.
constexpr uint16_t kChosenUnitLimit = settings::default_unit_limit + settings::unit_limit_step;
/// The maximum frame rate the check chooses with the keys: one stop below the highest.
constexpr uint32_t kChosenFrameRate = settings::highest_frame_rate - settings::frame_rate_step;
/// The anti-aliasing level the check picks from the strip.
constexpr settings::AntiAliasing kChosenAntiAliasing = settings::AntiAliasing::x4;
/// The Screen size the check chooses: one the made-up 4K monitor offers.
constexpr settings::ScreenSize kChosenScreenSize{1280, 720};

/// Throws when a condition fails.
///
/// @param ok the condition
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("engine settings check: " + std::string(what));
}

#if defined(__APPLE__) && !TARGET_OS_IPHONE
/// The eight file-type switches, cleared, so a comparison can ignore them.
///
/// @param settings the settings
/// @return the same settings with every file-type switch off
settings::EngineSettings without_file_type_switches(settings::EngineSettings settings) {
    settings.file_type_oamod = false;
    settings.file_type_oamap = false;
    settings.file_type_oalang = false;
    settings.file_type_oareg = false;
    settings.file_type_oamut = false;
    settings.file_type_oabrain = false;
    settings.file_type_oakeys = false;
    settings.file_type_oalaunch = false;
    return settings;
}
#endif

/// Reports whether two settings agree. On macOS the file-type switches show
/// what the system reports, which an empty preferences file does not hold, and
/// this check never moves them.
///
/// @param left one side
/// @param right the other
/// @return true when they agree
bool same_settings(const settings::EngineSettings& left, const settings::EngineSettings& right) {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    return without_file_type_switches(left) == without_file_type_switches(right);
#else
    return left == right;
#endif
}

/// Returns the name a section's snapshots carry.
///
/// @param page the section
/// @return a short name
std::string_view page_slug(settings::Page page) {
    switch (page) {
    case settings::Page::controls:
        return "controls";
    case settings::Page::common_tweaks:
        return "tweaks";
    case settings::Page::graphics:
        return "graphics";
    case settings::Page::user_interface:
        return "user-interface";
    case settings::Page::language:
        return "language";
    case settings::Page::touch:
        return "touch";
    case settings::Page::controller:
        return "controller";
    case settings::Page::developer:
        return "developer";
    case settings::Page::game_files:
        return "game-files";
    case settings::Page::downloads:
        return "downloads";
    case settings::Page::system:
        return "system";
    case settings::Page::game_content:
        return "game-content";
    case settings::Page::updates:
        return "updates";
    case settings::Page::accessibility:
        return "accessibility";
    case settings::Page::mod_keys:
    case settings::Page::mod_patrol:
    case settings::Page::mod_guard:
    case settings::Page::mod_tools:
    case settings::Page::mod_chat:
        return "mod";
    case settings::Page::registered:
        return "registered";
    }
    return "page";
}

/// The dialog's sections, in the order its list shows them.
constexpr std::array<settings::Page, 8> kPages{
    settings::Page::controls,
    settings::Page::common_tweaks,
    settings::Page::language,
    settings::Page::graphics,
    settings::Page::user_interface,
    settings::Page::accessibility,
    settings::Page::system,
    settings::Page::developer,
};

/// The first area of Developer Mode's list, which the check opens.
constexpr std::string_view kFirstArea = "AI";
/// The area of the display (view-scope) hacks, as the list names it.
constexpr std::string_view kDisplayArea = "Interface";
/// A rule (sim-scope) hack of the first area, ai.attack-wave-size, as the
/// list names it; the check turns it on.
constexpr std::string_view kRuleHack = "Attack Wave Size";
/// The first area's first hack, which the snapshots show open and on.
constexpr std::string_view kShownHack = kRuleHack;
/// A display (view-scope) hack, ui.whiteboard, as the list names it; the
/// check turns it on.
constexpr std::string_view kDisplayHack = "Whiteboard";

/// Returns Show Active Only's label for a count of hacks that are on.
///
/// @param active the hacks that are on
/// @return "Show Active Only (X/Y)" with every standard hack as Y
std::string active_only_label(std::size_t active) {
    return "Show Active Only (" + std::to_string(active) + "/" +
           std::to_string(oa::data::mod_profile::standard_hacks().size()) + ")";
}

/// Finds the part of a hack's switch, Off or On, on the hack's own line.
///
/// @param parts the dialog's layout
/// @param hack the hack's title, as its header shows it
/// @param caption "OFF" or "ON"
/// @return the switch's half; nullptr when the hack does not show
const settings::LayoutPart* hack_switch(
    const std::vector<settings::LayoutPart>& parts, std::string_view hack, std::string_view caption
) {
    const settings::LayoutPart* header = nullptr;
    for (const auto& part : parts)
        if (part.text == hack)
            header = &part;
    if (header == nullptr)
        return nullptr;
    // The switch's halves lie one row under the header's label line's top.
    for (const auto& part : parts)
        if (part.text == caption && part.rect.y == header->rect.y + 1 &&
            part.rect.x > header->rect.x + header->rect.width)
            return &part;
    return nullptr;
}

/// Returns the label a setting's row shows.
///
/// @param setting the setting
/// @return its label, as the dialog draws it
std::string_view label_of(settings::Setting setting) {
    switch (setting) {
    case settings::Setting::path_search:
        return "Pathfinding cycles";
    case settings::Setting::wheel_zoom:
        return "Mouse wheel zoom";
    case settings::Setting::max_zoom_out:
        return "Maximum zoom out";
    case settings::Setting::max_zoom_in:
        return "Maximum zoom in";
    case settings::Setting::view_past_map_edge:
        return "View past the map's edge";
    case settings::Setting::middle_click_pan:
        return "Middle-click pans";
    case settings::Setting::escape_opens_menu:
        return "Escape opens the game menu";
    case settings::Setting::switch_alt:
        return "Select groups without Alt";
    case settings::Setting::window_edge:
        return "Window edge";
    case settings::Setting::window_edge_stickiness:
        return "Edge stickiness";
    case settings::Setting::unit_limit:
        return "Unit limit";
    case settings::Setting::max_frame_rate:
        return "Maximum frame rate";
    case settings::Setting::anti_aliasing:
        return "Enhanced anti-aliasing";
    case settings::Setting::screen_size:
        return "Screen size";
    case settings::Setting::developer_mode:
        return "Enable Developer Mode";
    case settings::Setting::frame_stats:
        return "Show performance statistics";
    case settings::Setting::hardware_acceleration:
        return "Hardware acceleration";
    case settings::Setting::vertical_sync:
        return "Vertical sync";
    case settings::Setting::smooth_motion:
        return "Smooth motion";
    case settings::Setting::menu_scaling:
        return "Menu scaling";
    case settings::Setting::native_density:
        return "Native pixel density";
    case settings::Setting::explosion_flash:
        return "Explosion flash";
    case settings::Setting::zoomed_out_units:
        return "Zoomed out units";
    case settings::Setting::zoomed_out_after:
        return "After zoom";
    case settings::Setting::window_frame:
        return "Window frame";
    case settings::Setting::hud_scaling:
        return "HUD scaling";
    case settings::Setting::interface_size:
        return "Interface size";
    case settings::Setting::modern_fonts:
        return "Use modern fonts for game text";
    case settings::Setting::text_outline:
        return "Font outline";
    case settings::Setting::text_shadow:
        return "Font shadow";
    case settings::Setting::text_background:
        return "Game text background";
    case settings::Setting::unicode_chat:
        return "Enable Unicode Multiplayer Chat";
    case settings::Setting::text_size:
        return "Text size";
    case settings::Setting::language:
        return "Language";
    case settings::Setting::user_folder:
        return "Your files";
    case settings::Setting::kit_gallery:
        return "UI kit gallery";
    case settings::Setting::file_type_registration:
        return "File Type Registration";
    case settings::Setting::file_type_oamod:
        return "Mod packages (.oamod)";
    case settings::Setting::file_type_oamap:
        return "Map packs (.oamap)";
    case settings::Setting::file_type_oalang:
        return "Language packs (.oalang)";
    case settings::Setting::file_type_oareg:
        return "Registry files (.oareg)";
    case settings::Setting::file_type_oamut:
        return "Mutator packages (.oamut)";
    case settings::Setting::file_type_oabrain:
        return "Brain packages (.oabrain)";
    case settings::Setting::file_type_oakeys:
        return "Key layout packages (.oakeys)";
    case settings::Setting::file_type_oalaunch:
        return "Launch files (.oalaunch)";
    case settings::Setting::file_types_note:
        return "File types";
    case settings::Setting::skirmish_players:
        return "Skirmish players";
    case settings::Setting::weapon_lighting:
        return "Coloured weapon lighting";
    case settings::Setting::nanolathe_lighting:
        return "Nanolathe glow";
    case settings::Setting::wreck_glow:
        return "Wreck glow";
    case settings::Setting::water_ripples:
        return "Water ripples";
    case settings::Setting::coastal_waves:
        return "Coastal waves";
    case settings::Setting::underwater_shimmer:
        return "Underwater shimmer";
    case settings::Setting::naval_reflections:
        return "Naval reflections";
    case settings::Setting::heat_distortion:
        return "Heat distortion";
    case settings::Setting::allow_full_3d:
        return "Allow full 3D mode";
    case settings::Setting::unlocked_camera:
        return "Unlocked camera";
    case settings::Setting::mod_limits_heading:
        return "Set by the mod";
    case settings::Setting::weapon_types:
        return "Weapon types";
    case settings::Setting::sides:
        return "Sides";
    case settings::Setting::key_layout:
        return "Key layout";
    case settings::Setting::key_layout_for_mod:
        return "Key layout with Ridge";
    case settings::Setting::show_keys_on_buttons:
        return "Show keys on buttons";
    case settings::Setting::welcome_setup:
        return "Welcome setup";
    case settings::Setting::fog_of_war:
        return "Fog of War";
    default:
        return {};
    }
}

/// Tells whether the renderer waits for the display, as SDL reports it.
///
/// @param renderer the renderer
/// @return true when SDL_GetRenderVSync reads 1
bool renderer_waits(SDL_Renderer* renderer) {
    int vsync = 0;
    return SDL_GetRenderVSync(renderer, &vsync) && vsync == 1;
}

/// Finds a part of the dialog's layout by its control and text.
///
/// @param parts the dialog's layout
/// @param control the part's control
/// @param text the part's text; empty for the control's first part, whatever its text
/// @return the part; nullptr when the layout has none such
const settings::LayoutPart*
find_part(const std::vector<settings::LayoutPart>& parts, int32_t control, std::string_view text) {
    for (const auto& part : parts)
        if (part.control == control && (text.empty() || part.text == text))
            return &part;
    return nullptr;
}

/// A layout made for the call would be gone before its part is read.
const settings::LayoutPart* find_part(
    std::vector<settings::LayoutPart>&& parts, int32_t control, std::string_view text
) = delete;

/// Tells whether the dialog's layout shows a text.
///
/// @param parts the dialog's layout
/// @param text the text
/// @return true when a part draws exactly that text
bool shows_text(const std::vector<settings::LayoutPart>& parts, std::string_view text) {
    for (const auto& part : parts)
        if (part.text == text)
            return true;
    return false;
}

/// Tells whether the layout shows a sentence as one part or as the wrapped
/// lines of it, joined by single spaces.
///
/// @param parts the dialog's layout
/// @param sentence the sentence
/// @return true when a part is the sentence, or consecutive parts join to it
bool shows_sentence(const std::vector<settings::LayoutPart>& parts, std::string_view sentence) {
    if (shows_text(parts, sentence))
        return true;
    for (std::size_t start = 0; start < parts.size(); ++start) {
        std::string joined;
        for (std::size_t index = start; index < parts.size(); ++index) {
            if (parts[index].text.empty())
                break;
            if (!joined.empty())
                joined.push_back(' ');
            joined += parts[index].text;
            if (joined == sentence)
                return true;
            if (joined.size() >= sentence.size() || !sentence.starts_with(joined) ||
                sentence[joined.size()] != ' ')
                break;
        }
    }
    return false;
}

/// Returns the Open Annihilation settings' keys a preferences file holds.
///
/// @param values the file's preferences
/// @return each key under kEngineKeyPrefix with its value
std::map<std::string, std::string> engine_keys(const oa::platform::preferences::Values& values) {
    std::map<std::string, std::string> keys;
    for (const auto& [key, value] : values)
        if (key.starts_with(kEngineKeyPrefix))
            keys.emplace(key, value);
    return keys;
}

/// Tells whether a pixel lies in a rectangle.
///
/// @param rect the rectangle
/// @param x the pixel's column
/// @param y the pixel's row
/// @return true inside it
bool inside(const artless::SourceRect& rect, int32_t x, int32_t y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

/// Counts the pixels in which two frames of one size differ outside a rectangle.
///
/// @param first one frame
/// @param second the other frame
/// @param left_out the rectangle not compared
/// @return the differing pixels; every pixel when the sizes differ
std::size_t differing_outside(
    const artless::Surface& first,
    const artless::Surface& second,
    const artless::SourceRect& left_out
) {
    if (first.width != second.width || first.height != second.height)
        return static_cast<std::size_t>(first.width) * first.height;
    std::size_t differing = 0;
    for (uint32_t y = 0; y < first.height; ++y)
        for (uint32_t x = 0; x < first.width; ++x) {
            if (inside(left_out, static_cast<int32_t>(x), static_cast<int32_t>(y)))
                continue;
            const std::size_t at = (static_cast<std::size_t>(y) * first.width + x) * 3U;
            if (first.rgb[at] != second.rgb[at] || first.rgb[at + 1] != second.rgb[at + 1] ||
                first.rgb[at + 2] != second.rgb[at + 2])
                ++differing;
        }
    return differing;
}

/// Counts the picture's pixels a letterboxed window frame does not show:
/// each picture pixel is compared with the window pixel at its centre,
/// leaving out the pixels round the pointer, where the cursor draws. A
/// window larger than the picture draws the menu's buttons from a larger
/// icon, over the window rectangles the caller names, so a sample that
/// lands in one of those is left out. The picture's own 48-pixel squares
/// are not those rectangles once the picture is scaled.
///
/// @param presented the window's frame as presented
/// @param picture the picture, at the display gamma
/// @param area where the picture lands in the window
/// @param window_width the window's width, in pixels
/// @param pointer the pointer, in the picture's pixels
/// @param sharp_buttons window rectangles the larger icons cover; empty at
///        the picture's own size
/// @return the pixels that differ; every pixel when the frame misses the area
std::size_t letterbox_differences(
    const artless::Surface& presented,
    const artless::Surface& picture,
    const SDL_FRect& area,
    int window_width,
    layout::Point pointer,
    std::span<const layout::Rect> sharp_buttons = {}
) {
    // The read-back holds the whole window, or the letterboxed area alone.
    const bool whole_window = static_cast<int>(presented.width) == window_width;
    const float left = whole_window ? area.x : 0.0F;
    const float top = whole_window ? area.y : 0.0F;
    const artless::SourceRect cursor{
        pointer.x - kCursorReach, pointer.y - kCursorReach, 2 * kCursorReach, 2 * kCursorReach
    };
    std::size_t differing = 0;
    for (uint32_t y = 0; y < picture.height; ++y)
        for (uint32_t x = 0; x < picture.width; ++x) {
            if (inside(cursor, static_cast<int32_t>(x), static_cast<int32_t>(y)))
                continue;
            const auto window_x = static_cast<std::size_t>(
                left + (static_cast<float>(x) + 0.5F) * area.w / static_cast<float>(picture.width)
            );
            const auto window_y = static_cast<std::size_t>(
                top + (static_cast<float>(y) + 0.5F) * area.h / static_cast<float>(picture.height)
            );
            if (window_x >= presented.width || window_y >= presented.height)
                return static_cast<std::size_t>(picture.width) * picture.height;
            bool sharp = false;
            for (const layout::Rect& button : sharp_buttons) {
                if (static_cast<int>(window_x) >= button.x &&
                    static_cast<int>(window_y) >= button.y &&
                    static_cast<int>(window_x) < button.x + button.width &&
                    static_cast<int>(window_y) < button.y + button.height) {
                    sharp = true;
                    break;
                }
            }
            if (sharp)
                continue;
            const auto* shown = presented.rgb.data() + (window_y * presented.width + window_x) * 3U;
            const auto* expected =
                picture.rgb.data() + (static_cast<std::size_t>(y) * picture.width + x) * 3U;
            if (shown[0] != expected[0] || shown[1] != expected[1] || shown[2] != expected[2])
                ++differing;
        }
    return differing;
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

void Runtime::check_engine_settings_dialog() {
    std::cout << "engine settings check: the dialog's sections and controls\n";
    const auto previous_tick = fake_frontend_tick_;
    fake_frontend_tick_ = 1000U;
    load(Screen::main_menu);
    require(screen_ == Screen::main_menu, "the main menu did not open");
    // The dialog starts from an empty preferences file: every setting at its default.
    preference_values_.clear();
    oa::platform::preferences::save(preference_path_, preference_values_);
    init::load_preferences(state_, skirmish_settings_, preferences_, *this);
    load_engine_settings();
    const auto defaults = settings::default_settings(EngineSettingsState::inputs(*this));
    require(
        same_settings(engine_settings(), defaults), "the settings did not start at their defaults"
    );

    // Where the OA layer's settings screen shows on the main menu's picture,
    // while it is open.
    const auto placement = [this] {
        const auto* screen = oa_layer().find("settings");
        require(screen != nullptr, "the settings screen is not on the OA layer");
        return screen->placement(oa_layer().view()).shown;
    };
    const auto point = [this](SDL_EventType type, layout::Point at) {
        send_check_pointer(type, at, type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT);
    };
    const auto rest = [&] { point(SDL_EVENT_MOUSE_MOTION, kRestingPointer); };
    // Clicks a point of the dialog, in its points, at the window pixel the
    // OA layer shows it at: the window takes another size on the way.
    const auto click_at = [&](int32_t x, int32_t y) {
        require(
            oa_layer().find("settings") != nullptr, "the settings screen is not on the OA layer"
        );
        const layout::Point pixel = layer_window_pixel(oa_layer().placement_of("settings"), {x, y});
        for (const SDL_EventType type :
             {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event = window_pointer_event(
                sdl_.window,
                sdl_.renderer,
                type,
                pixel,
                type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
            );
            bool running = true;
            dispatch_event(event, running);
            require(running, "a click in the dialog ended the run");
        }
    };
    // Clicks the middle of the part a control with a caption draws.
    const auto click = [&](int32_t control, std::string_view text, std::string_view what) {
        auto* dialog = engine_settings_dialog();
        require(dialog != nullptr, "the dialog closed before " + std::string(what));
        const auto parts = settings::dialog_layout(*dialog);
        const auto* part = find_part(parts, control, text);
        require(part != nullptr, "the dialog shows no " + std::string(what));
        click_at(part->rect.x + part->rect.width / 2, part->rect.y + part->rect.height / 2);
    };
    // Sends a key's press and release; false when it ended the run.
    const auto tap = [this](SDL_Keycode code, SDL_Keymod modifiers = SDL_KMOD_NONE) {
        bool running = true;
        for (const bool down : {true, false}) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.windowID = SDL_GetWindowID(sdl_.window);
            event.key.key = code;
            event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
            event.key.mod = modifiers;
            event.key.down = down;
            dispatch_event(event, running);
        }
        require(running, "a key in the dialog ended the run");
    };
    const auto open = [&](std::string_view what) {
        tap(SDLK_COMMA, kShortcutModifier);
        auto* dialog = engine_settings_dialog();
        require(
            dialog != nullptr && oa_layer().find("settings") != nullptr,
            "the shortcut did not open the dialog " + std::string(what)
        );
        rest();
        return dialog;
    };
    // Moves the keyboard focus onto a control with Tab, at most once round
    // the declared order: the section's rows, the footer's buttons and the
    // sections' entries, and one more press to show the focus. The arrows
    // move by where the controls lie and stop at the dialog's edges.
    // A row's control, by the name the dialog gives it (settings.<id>), so a
    // new row above it does not move the press onto another setting.
    const auto control_named = [&](std::string_view name) {
        auto* dialog = engine_settings_dialog();
        require(dialog != nullptr, "the dialog closed before " + std::string(name));
        const oa::ui::kit::DisplayList list = settings::geometry::dialog_list(*dialog, nullptr);
        const int32_t control = oa::ui::kit::control_named(list, name);
        require(control != oa::ui::kit::no_control, "the dialog has no " + std::string(name));
        return control;
    };
    const auto focus = [&](int32_t control, std::string_view what) {
        const auto order =
            settings::page_settings(engine_settings_dialog()->page).size() +
            static_cast<std::size_t>(settings::ok_control - settings::restore_control + 1) +
            settings::page_count;
        for (std::size_t presses = 0; presses <= order; ++presses) {
            if (engine_settings_dialog()->focused == control)
                return;
            tap(SDLK_TAB);
        }
        require(false, "Tab never reached " + std::string(what));
    };
    const auto sparks = menu_sparks_;
    const auto frame = [&] {
        menu_sparks_ = sparks;
        return frame_without_cursor();
    };
    // Every section through its entry in the list: its rows, and nothing
    // outside the dialog changes.
    const auto* menu_dialog = open("from the main menu");
    const artless::SourceRect dialog_area{
        placement().x, placement().y, settings::dialog_width, settings::dialog_height
    };
    // The main menu's dialog lists Game files only where the platform brings
    // game files in; this game lists it exactly then.
    const bool game_files = game_files_import_offered(game_files_hooks());
    require(
        menu_dialog->game_files == game_files &&
            shows_text(settings::dialog_layout(*menu_dialog), "Game files") == game_files,
        game_files ? "the main menu's dialog does not list Game files"
                   : "the main menu's dialog lists Game files where no game files are brought in"
    );
    auto before = frame();
    for (const auto page : kPages) {
        const auto name = std::string(page_slug(page));
        click(settings::page_control(page), {}, name + "'s entry in the list");
        auto* dialog = engine_settings_dialog();
        require(dialog->page == page, "a click on " + name + "'s entry did not show it");
        auto parts = settings::dialog_layout(*dialog);
        // A section taller than its view shows the rest of its rows a page
        // at a time, down to its end.
        if (dialog->scroll[static_cast<std::size_t>(page)] == 0) {
            for (int32_t last = -1; dialog->scroll[static_cast<std::size_t>(page)] != last;) {
                last = dialog->scroll[static_cast<std::size_t>(page)];
                tap(SDLK_PAGEDOWN);
                const auto lower = settings::dialog_layout(*dialog);
                parts.insert(parts.end(), lower.begin(), lower.end());
            }
            tap(SDLK_HOME);
        }
        const auto rows = settings::page_settings(page);
        for (std::size_t row = 0; row < rows.size(); ++row)
            require(
                shows_text(parts, label_of(rows[row])),
                name + " does not show " + std::string(label_of(rows[row]))
            );
        // The list names Language & Text; the row is still Language. Only
        // the open section's own parts count, so the list's entries are
        // dropped before the labels are compared.
        std::erase_if(parts, [](const settings::LayoutPart& part) {
            return part.control >= settings::first_page_control &&
                   part.control < settings::restore_control;
        });
        for (const auto other : kPages)
            if (other != page)
                for (const auto setting : settings::page_settings(other))
                    require(
                        !shows_text(parts, label_of(setting)),
                        name + " shows " + std::string(label_of(setting))
                    );
        rest();
        const auto shown = frame();
        require(
            differing_outside(shown, before, dialog_area) == 0,
            "showing " + name + " changed the menu outside the dialog"
        );
        before = shown;
        require(same_settings(engine_settings(), defaults), "showing a section changed a setting");
    }

    // The wheel and the scroll keys reach the dialog. Given a section taller
    // than its view, a notch towards the player scrolls it 24 pixels, Page
    // Down 200 more and End to its end, Page Up 200 back and Home to its top;
    // a notch away from the player at the top, a notch towards the player at
    // the end, and a notch outside the dialog, do nothing.
    {
        namespace tall = engine_settings_check;

        // The dialog shows its own sections again however the block ends.
        struct OwnSections {
            Runtime& runtime;

            ~OwnSections() {
                if (auto* shown = runtime.engine_settings_dialog())
                    tall::show_own_sections(*shown);
            }
        } own_sections{*this};

        tall::show_tall_section(*engine_settings_dialog());
        const auto turn_wheel = [&](layout::Point at, float notches) {
            SDL_Event wheel =
                check_host_input::wheel_event(sdl_.renderer, sdl_.window, at.x, at.y, notches);
            bool running = true;
            dispatch_event(wheel, running);
            require(running, "the wheel in the dialog ended the run");
        };
        const auto offset = [this] {
            const auto* open = engine_settings_dialog();
            return open->scroll[static_cast<std::size_t>(open->page)];
        };
        // The dialog's middle, over its rows.
        const layout::Point over_rows{
            placement().x + settings::dialog_width / 2, placement().y + settings::dialog_height / 2
        };
        turn_wheel(over_rows, 1.0F);
        require(offset() == 0, "the wheel scrolled the dialog above its top");
        turn_wheel(over_rows, -1.0F);
        require(
            offset() == tall::kWheelStepPixels, "a notch of the wheel did not scroll the dialog"
        );
        turn_wheel(kRestingPointer, -1.0F);
        require(offset() == tall::kWheelStepPixels, "the wheel outside the dialog scrolled it");
        tap(SDLK_PAGEDOWN);
        require(
            offset() == tall::kWheelStepPixels + tall::kPageStepPixels,
            "Page Down did not scroll the dialog"
        );
        tap(SDLK_END);
        const int32_t end = offset();
        require(
            end > tall::kWheelStepPixels + tall::kPageStepPixels, "End did not scroll to the end"
        );
        turn_wheel(over_rows, -1.0F);
        require(offset() == end, "the wheel scrolled the dialog past its end");
        tap(SDLK_PAGEUP);
        require(offset() == end - tall::kPageStepPixels, "Page Up did not scroll the dialog");
        tap(SDLK_HOME);
        require(offset() == 0, "Home did not scroll back to the top");
        require(
            engine_settings_dialog()->focused == settings::no_control &&
                same_settings(engine_settings(), defaults),
            "scrolling showed the focus or changed a setting"
        );
    }
    rest();

    // Each setting through the pointer or the keys, in effect at once, but
    // Screen size, which applies when OK is pressed. In a window the dialog
    // shows Screen size at the window's own size.
    auto chosen = defaults;
    int opened_width = 0;
    int opened_height = 0;
    SDL_GetWindowSize(sdl_.window, &opened_width, &opened_height);
    const auto window_size_text = [this] {
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(sdl_.window, &width, &height);
        return std::to_string(width) + 'x' + std::to_string(height);
    };
    // The window's own events (its new size among them) reach the game as
    // its loop pumps them.
    const auto pump_window_events = [this] {
        SDL_SyncWindow(sdl_.window);
        SDL_Event event{};
        bool running = true;
        while (SDL_PollEvent(&event))
            dispatch_event(event, running);
        require(running, "a window event ended the run");
    };
    auto shown_screen_size = engine_settings_dialog()->chosen.screen_size;
    {
        const auto& window_size = engine_settings_dialog()->window_screen_size;
        require(
            window_size && settings::screen_size_text(*window_size) == window_size_text() &&
                shows_text(
                    settings::dialog_layout(*engine_settings_dialog()),
                    std::to_string(window_size->width) + " x " + std::to_string(window_size->height)
                ) == (engine_settings_dialog()->page == settings::Page::graphics),
            "Screen size did not open on the window's size, " + window_size_text()
        );
    }
    const auto expect = [&](std::string_view what) {
        require(engine_settings_dialog() != nullptr, "the dialog closed on " + std::string(what));
        auto shown = chosen;
        shown.screen_size = shown_screen_size;
        require(
            same_settings(engine_settings_dialog()->chosen, shown) &&
                same_settings(engine_settings(), chosen),
            std::string(what) + " did not take effect at once"
        );
    };
    click(settings::page_control(settings::Page::common_tweaks), {}, "Engine Limits' entry");
    focus(settings::first_row_control + 1, "Pathfinding cycles");
    tap(SDLK_RIGHT);
    chosen.path_search_nodes = kChosenPathNodes;
    expect("Pathfinding cycles at 2x");
    require(
        shows_text(settings::dialog_layout(*engine_settings_dialog()), "2x"),
        "Pathfinding cycles does not show 2x"
    );

    click(settings::page_control(settings::Page::controls), {}, "Controls' entry");
    // Key layout and Show keys on buttons sit above the wheel. The wheel and
    // the limits keep their own names, so those rows do not move the press.
    click(control_named("settings.wheel-zoom"), "OFF", "Mouse wheel zoom's Off");
    chosen.wheel_zoom = false;
    expect("Mouse wheel zoom Off");
    // The zoom's limits, on their drop-downs: Right steps a choice on and
    // Left one back.
    focus(control_named("settings.max-zoom-out"), "Maximum zoom out");
    tap(SDLK_RIGHT);
    chosen.max_zoom_out = settings::ZoomOutLimit::whole_map;
    expect("Maximum zoom out Whole map");
    focus(control_named("settings.max-zoom-in"), "Maximum zoom in");
    tap(SDLK_LEFT);
    chosen.max_zoom_in = settings::ZoomInLimit::three_times;
    expect("Maximum zoom in 3x");
    // The rows under them, scrolled into view as the focus reaches them.
    // View past the map's edge: 25% is the share the view's limits take.
    focus(control_named("settings.view-past-map-edge"), "View past the map's edge");
    click(control_named("settings.view-past-map-edge"), "25%", "View past the map's edge's 25%");
    chosen.view_past_map_edge = settings::ViewPastMapEdge::one_quarter;
    expect("View past the map's edge 25%");
    require(
        past_map_edge_share() == 0.25,
        "View past the map's edge 25% is not the view's limits' share"
    );
    // Window edge opens on Capture, with Edge stickiness under it locked at
    // 1.0: Tab passes it by and its keys change nothing. Sticky frees it,
    // and a step of it is the sticky edge's firmness at once. Free locks it
    // again and keeps its value.
    {
        const int32_t window_edge = control_named("settings.window-edge");
        // The slider is the row under the strip. Locked, it names no control.
        const int32_t stickiness = window_edge + 1;
        require(
            engine_settings_dialog()->chosen.window_edge == settings::WindowEdge::capture,
            "Window edge did not open on Capture"
        );
        // Both rows at the section's end, scrolled into view.
        const auto shows = [&](std::string_view text) {
            auto* shown = engine_settings_dialog();
            shown->scroll[static_cast<std::size_t>(settings::Page::controls)] =
                std::numeric_limits<int32_t>::max();
            return shows_text(settings::dialog_layout(*shown), text);
        };
        focus(window_edge, "Window edge");
        require(
            shows("Capture") && shows("Sticky") && shows("Free") && shows("Needs Sticky") &&
                shows("1.0"),
            "Window edge's strip or Edge stickiness's locked slider does not show"
        );
        tap(SDLK_TAB);
        require(
            engine_settings_dialog()->focused != stickiness,
            "Tab reached Edge stickiness while Window edge is Capture"
        );
        engine_settings_dialog()->focused = stickiness;
        tap(SDLK_RIGHT);
        expect("a key on the locked Edge stickiness");
        focus(window_edge, "Window edge");
        click(window_edge, "Sticky", "Window edge's Sticky");
        chosen.window_edge = settings::WindowEdge::sticky;
        expect("Window edge Sticky");
        require(!shows("Needs Sticky"), "Sticky did not free Edge stickiness");
        require(
            control_named("settings.window-edge-stickiness") == stickiness,
            "Edge stickiness is not the row under Window edge"
        );
        focus(stickiness, "Edge stickiness");
        tap(SDLK_RIGHT);
        tap(SDLK_RIGHT);
        chosen.edge_stickiness = settings::default_edge_stickiness + 2;
        expect("Edge stickiness 1.2");
        require(shows("1.2"), "Edge stickiness does not show 1.2");
        require(
            sticky_edge_limits().push_points == sticky_limits(1.2F).push_points &&
                sticky_edge_limits().flick_points_per_s == sticky_limits(1.2F).flick_points_per_s,
            "Edge stickiness 1.2 is not the sticky edge's firmness"
        );
        focus(window_edge, "Window edge");
        click(window_edge, "Free", "Window edge's Free");
        chosen.window_edge = settings::WindowEdge::free;
        expect("Window edge Free");
        require(shows("Needs Sticky") && shows("1.2"), "Free did not lock Edge stickiness at 1.2");
        focus(window_edge, "Window edge");
        click(window_edge, "Sticky", "Window edge's Sticky");
        chosen.window_edge = settings::WindowEdge::sticky;
        expect("Window edge Sticky again");
        require(!shows("Needs Sticky") && shows("1.2"), "Edge stickiness lost 1.2");
    }

    // Escape opens the game menu, the last row of User Interface.
    click(settings::page_control(settings::Page::user_interface), {}, "User Interface's entry");
    focus(settings::first_row_control + 4, "Escape opens the game menu");
    click(settings::first_row_control + 4, "ON", "Escape opens the game menu's On");
    chosen.escape_opens_menu = true;
    expect("Escape opens the game menu On");

    click(settings::page_control(settings::Page::common_tweaks), {}, "Engine Limits' entry");
    focus(settings::first_row_control, "Unit limit");
    tap(SDLK_RIGHT);
    chosen.unit_limit = kChosenUnitLimit;
    expect("Unit limit one stop up");
    require(
        frontend_game().max_units_setting == kChosenUnitLimit,
        "Unit limit did not set the next game's"
    );

    click(settings::page_control(settings::Page::graphics), {}, "Graphics' entry");
    focus(settings::first_row_control, "Maximum frame rate");
    tap(SDLK_LEFT);
    chosen.max_frame_rate = kChosenFrameRate;
    expect("Maximum frame rate one stop down");
    if (!options_.max_frames_per_second_given)
        require(
            options_.max_frames_per_second == kChosenFrameRate,
            "Maximum frame rate did not set the frame rate"
        );
    click(settings::first_row_control + 1, "4x", "Enhanced anti-aliasing's 4x");
    chosen.anti_aliasing = kChosenAntiAliasing;
    expect("Enhanced anti-aliasing at 4x");
    require(
        unit_supersampling_ == oa::present::model::UnitSupersampling::x4,
        "Enhanced anti-aliasing at 4x did not draw units finer"
    );
    // Screen size offers Desktop, then every size the display offers (the
    // made-up monitor's with --display-modes), the narrower first; Right
    // steps to the largest and stops there, and Left back to 1280x720. The
    // window keeps its size until OK.
    {
        const auto offered = EngineSettingsState::offered_screen_sizes(*this);
        std::vector<settings::ScreenSize> stops{settings::desktop_screen_size};
        stops.insert(stops.end(), offered.begin(), offered.end());
        require(
            engine_settings_dialog()->offered_screen_sizes == stops,
            "Screen size does not offer Desktop and the display's sizes"
        );
        require(
            std::find(offered.begin(), offered.end(), kChosenScreenSize) != offered.end(),
            "the display does not offer 1280x720"
        );
        focus(settings::first_row_control + 2, "Screen size");
        for (std::size_t step = 0; step <= offered.size(); ++step)
            tap(SDLK_RIGHT);
        shown_screen_size = offered.back();
        expect("Screen size at the display's largest");
        const std::string largest =
            std::to_string(offered.back().width) + " x " + std::to_string(offered.back().height);
        require(
            shows_text(settings::dialog_layout(*engine_settings_dialog()), largest),
            "Screen size at its last stop does not show " + largest
        );
        for (std::size_t step = 0;
             step <= offered.size() &&
             engine_settings_dialog()->chosen.screen_size != kChosenScreenSize;
             ++step)
            tap(SDLK_LEFT);
        shown_screen_size = kChosenScreenSize;
        expect("Screen size at 1280x720");
        require(
            window_size_text() ==
                std::to_string(opened_width) + 'x' + std::to_string(opened_height),
            "Screen size moved the window before OK"
        );
        std::cout << "engine settings check: Screen size offers Desktop and " << offered.size()
                  << " sizes, up to " << largest << '\n';
    }
    // Hardware acceleration, Off with a named preferences file, says the
    // processor draws, or, under 2 GiB, that the machine needs more memory;
    // it is never switched here, so that every frame the check compares is
    // drawn as without the setting. It is locked under 2 GiB, and on SDL's
    // software renderer or the environment's driver unless --force-capable
    // lifts them; a renderer nothing has looked at leaves it unlocked.
    auto* dialog = engine_settings_dialog();
    const auto facts = acceleration_facts();
    const bool memory = enough_memory_for_acceleration(facts.physical_memory);
    require(
        dialog->acceleration.state == (memory ? settings::AccelerationState::off_by_setting
                                              : settings::AccelerationState::needs_memory),
        "Hardware acceleration's status does not say it is Off, or why not under 2 GiB"
    );
    const bool ruled_out =
        !memory || (!facts.force_capable && (facts.software_renderer || facts.environment_driver));
    require(
        dialog->locks.hardware_acceleration ==
            (ruled_out ? settings::Lock::unavailable : settings::Lock::none),
        "Hardware acceleration is not locked as the renderer and the memory give it"
    );
    // Vertical sync: Down to it scrolls Graphics to show it whole, and Right
    // turns it On, in effect at once: the renderer waits for the display. On
    // SDL's software renderer it is out of reach unless --force-capable lifts
    // it, as the command that registers this check does.
    const bool vertical_sync = dialog->locks.vertical_sync == settings::Lock::none;
    require(
        vertical_sync == options_.force_capable,
        "Vertical sync is within reach otherwise than as --force-capable gives it"
    );
    if (vertical_sync) {
        require(!renderer_waits(sdl_.renderer), "the renderer waits for the display at Off");
        focus(settings::first_row_control + 4, "Vertical sync");
        require(
            dialog->scroll[static_cast<std::size_t>(settings::Page::graphics)] == 72,
            "the focus on Vertical sync did not scroll Graphics to show it"
        );
        require(
            shows_text(
                settings::dialog_layout(*dialog),
                memory ? "Off: the processor draws and scales the view."
                       : "Not in use: it needs at least 2 GB of memory."
            ),
            "Graphics at its end does not show Hardware acceleration's status"
        );
        tap(SDLK_RIGHT);
        chosen.vertical_sync = true;
        expect("Vertical sync On");
        require(renderer_waits(sdl_.renderer), "Vertical sync On did not reach the renderer");
    } else {
        require(
            dialog->locks.vertical_sync == settings::Lock::unavailable,
            "Vertical sync is locked other than on the software renderer"
        );
    }
    // Menu scaling: Whole steps sets the window's presentation at once, in
    // whole steps where the window holds the menu, else letterboxed.
    const auto presentation = [this] {
        int width = 0;
        int height = 0;
        SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
        require(
            SDL_GetRenderLogicalPresentation(sdl_.renderer, &width, &height, &mode),
            "the renderer has no logical presentation"
        );
        return mode;
    };
    const auto held_whole = [this] {
        int width = 0;
        int height = 0;
        require(
            SDL_GetRenderOutputSize(sdl_.renderer, &width, &height), "the renderer has no size"
        );
        return render_policy::frame_fit(
                   render_policy::MenuScaling::whole_steps,
                   width,
                   height,
                   kCanvasWidth,
                   kCanvasHeight
               ) == render_policy::FrameFit::whole_steps;
    };
    require(
        presentation() == SDL_LOGICAL_PRESENTATION_LETTERBOX,
        "Sharp does not letterbox the main menu"
    );
    // Smooth motion: Off draws the next frame, and every frame after it, at
    // the latest tick (present_frame_between_ticks), and its hint says so.
    focus(settings::first_row_control + 5, "Smooth motion");
    click(settings::first_row_control + 5, "OFF", "Smooth motion's Off");
    chosen.smooth_motion = false;
    expect("Smooth motion Off");
    require(
        !smooth_motion() &&
            shows_sentence(
                settings::dialog_layout(*dialog),
                "Units and shots move 30 times a second, as the game drew them; fast spins may "
                "flicker."
            ),
        "Smooth motion Off is not what the frames draw, or its hint does not say what Off does"
    );
    // Native pixel density: On is kept for the next start, and the window
    // keeps the density it opened at.
    const bool dense = native_density_window();
    focus(settings::first_row_control + 6, "Native pixel density");
    click(settings::first_row_control + 6, "ON", "Native pixel density's On");
    chosen.native_density = true;
    expect("Native pixel density On");
    require(
        native_density_window() == dense &&
            shows_sentence(
                settings::dialog_layout(*dialog),
                "The display's own pixel density (Retina on a Mac). Applies from the next start."
            ),
        "Native pixel density On changed the open window, or does not say when it applies"
    );
    // Explosion flash: Reduced is the level the next frame draws.
    focus(settings::first_row_control + 7, "Explosion flash");
    click(settings::first_row_control + 7, "Reduced", "Explosion flash's Reduced");
    chosen.explosion_flash = settings::ExplosionFlash::reduced;
    expect("Explosion flash Reduced");
    require(
        explosion_flash() == settings::ExplosionFlash::reduced,
        "Explosion flash Reduced is not the level the frames draw"
    );
    // Fog of War: Classic is the fog the next frame draws, and its hint says so.
    focus(settings::first_row_control + 8, "Fog of War");
    click(settings::first_row_control + 8, "Classic", "Fog of War's Classic");
    chosen.fog_of_war = settings::FogOfWar::classic;
    expect("Fog of War Classic");
    require(
        fog_of_war() == settings::FogOfWar::classic &&
            shows_sentence(
                settings::dialog_layout(*dialog),
                "Hard edges that step from cell to cell, as the game drew the fog."
            ),
        "Fog of War Classic is not the fog the frames draw, or its hint does not say what it is"
    );
    // Menu scaling, the first row of User Interface.
    click(settings::page_control(settings::Page::user_interface), {}, "User Interface's entry");
    focus(settings::first_row_control, "Menu scaling");
    click(settings::first_row_control, "Whole steps", "Menu scaling's Whole steps");
    chosen.menu_scaling = settings::MenuScaling::whole_steps;
    expect("Menu scaling Whole steps");
    require(
        presentation() == (held_whole() ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE
                                        : SDL_LOGICAL_PRESENTATION_LETTERBOX),
        "Menu scaling Whole steps did not set the window's presentation at once"
    );

    // The text drawing reads the Language section at once. Text size
    // waits for the modern fonts, Off with a named preferences file: locked,
    // it says so; with them On, Right raises it a stop.
    click(settings::page_control(settings::Page::language), {}, "Language's entry");
    require(
        !chosen.modern_fonts &&
            shows_text(settings::dialog_layout(*engine_settings_dialog()), "Needs modern fonts"),
        "Text size is not locked while the modern fonts are Off"
    );
    // The Language drop-down: a click on its field opens its list, a click
    // on an item chooses the language at once, and Left steps it back.
    require(
        chosen.language == "en" && game_language() != nullptr &&
            std::string_view(game_language()) == "English",
        "a named preferences file does not play in English"
    );
    click(settings::first_row_control, "English", "Language's field");
    require(
        engine_settings_dialog()->open_list == settings::first_row_control,
        "a click on Language's field did not open its list"
    );
    click(settings::no_control, "Deutsch", "Language's Deutsch");
    chosen.language = "de";
    expect("Language Deutsch");
    require(
        shown_language().tag == "de" && game_language() != nullptr &&
            std::string_view(game_language()) == "German" &&
            engine_settings_dialog()->open_list == settings::no_control,
        "Language Deutsch did not put German in effect at once"
    );
    focus(settings::first_row_control, "Language");
    tap(SDLK_LEFT);
    chosen.language = "en";
    expect("Language English");
    require(shown_language().tag == "en", "Language English did not put English back");
    // On the main menu MORE IN THE LIBRARY… stands right under Language's
    // row, and the rows under it come one control later.
    const int32_t library_link = engine_settings_dialog()->library_link ? 1 : 0;
    click(
        settings::first_row_control + 1 + library_link, "ON", "Use modern fonts for game text's On"
    );
    chosen.modern_fonts = true;
    expect("Use modern fonts for game text On");
    require(
        !shows_text(settings::dialog_layout(*engine_settings_dialog()), "Needs modern fonts"),
        "Text size stayed locked with the modern fonts On"
    );
    focus(settings::first_row_control + 2 + library_link, "Text size");
    tap(SDLK_RIGHT);
    chosen.text_size = settings::default_text_size + settings::text_size_step;
    expect("Text size a stop up");
    require(
        text_style().size == chosen.text_size &&
            shows_text(settings::dialog_layout(*engine_settings_dialog()), "90%"),
        "Text size a stop up did not reach the text style"
    );
    // Font shadow lies under the view's edge: the focus brings it in.
    focus(settings::first_row_control + 4 + library_link, "Font shadow");
    tap(SDLK_LEFT);
    chosen.text_shadow = false;
    expect("Font shadow Off");
    require(
        text_style() == settings::text_style(chosen) && !text_style().shadow &&
            text_style().outline,
        "Font shadow Off did not reach the text style"
    );

    click(settings::page_control(settings::Page::developer), {}, "Developer's entry");
    click(settings::first_row_control + 1, "ON", "Show performance statistics' On");
    chosen.frame_stats = true;
    expect("Show performance statistics On");
    require(frame_stats_shown_, "Show performance statistics did not show the statistics");

    // OK keeps them and saves exactly the keys that changed, and the dialog
    // opens again on them; the window takes the Screen size at once, and the
    // main menu is drawn to the whole of it.
    click(settings::ok_control, "OK", "OK");
    require(
        engine_settings_dialog() == nullptr && oa_layer().find("settings") == nullptr,
        "OK did not close the dialog"
    );
    chosen.screen_size = kChosenScreenSize;
    require(same_settings(engine_settings(), chosen), "OK did not keep the settings chosen");
    pump_window_events();
    {
        int output_width = 0;
        int output_height = 0;
        SDL_GetRenderOutputSize(sdl_.renderer, &output_width, &output_height);
        require(
            window_size_text() == "1280x720" && output_width == kChosenScreenSize.width &&
                output_height == kChosenScreenSize.height,
            "OK on a Screen size of 1280x720 left the window at " + window_size_text()
        );
    }
    std::map<std::string, std::string> expected_keys{
        {std::string(settings::key::path_search_nodes), std::to_string(kChosenPathNodes)},
        {std::string(settings::key::wheel_zoom), "0"},
        {std::string(settings::key::max_zoom_out), "whole-map"},
        {std::string(settings::key::max_zoom_in), "3"},
        {std::string(settings::key::view_past_map_edge), "25"},
        {std::string(settings::key::window_edge), "sticky"},
        {std::string(settings::key::edge_stickiness), "1.2"},
        {std::string(settings::key::escape_opens_menu), "1"},
        {std::string(settings::key::unit_limit), std::to_string(kChosenUnitLimit)},
        {std::string(settings::key::max_frame_rate), std::to_string(kChosenFrameRate)},
        {std::string(settings::key::anti_aliasing),
         std::to_string(static_cast<int>(kChosenAntiAliasing))},
        {std::string(settings::key::frame_stats), "1"},
        {std::string(settings::key::modern_fonts), "1"},
        {std::string(settings::key::text_shadow), "0"},
        {std::string(settings::key::text_size), std::to_string(chosen.text_size)},
        {std::string(settings::key::screen_size), settings::screen_size_text(chosen.screen_size)},
    };
    if (vertical_sync)
        expected_keys.emplace(std::string(settings::key::vertical_sync), "1");
    expected_keys.emplace(std::string(settings::key::smooth_motion), "0");
    expected_keys.emplace(std::string(settings::key::menu_scaling), "whole-steps");
    expected_keys.emplace(std::string(settings::key::native_density), "1");
    expected_keys.emplace(std::string(settings::key::explosion_flash), "reduced");
    expected_keys.emplace(std::string(settings::key::fog_of_war), "classic");
    const auto saved = oa::platform::preferences::load(preference_path_);
    require(engine_keys(saved) == expected_keys, "OK did not save exactly the settings changed");
    // The preferences load writes SwitchAlt's default, 0, into 3.1c's own
    // key before the dialog opens. Controls no longer shows that switch, so
    // OK leaves the key as the load stored it.
    require(
        saved_general_number("SwitchAlt") == 0,
        "OK changed SwitchAlt though Controls no longer shows it"
    );
    dialog = open("again");
    require(
        same_settings(dialog->opened, chosen) && dialog->page == settings::Page::developer,
        "the dialog did not open again on the settings kept and the last section"
    );

    // Cancel (its button) puts back what the dialog opened with and saves nothing.
    click(settings::first_row_control + 1, "OFF", "Show performance statistics' Off");
    click(settings::page_control(settings::Page::controls), {}, "Controls' entry");
    click(control_named("settings.wheel-zoom"), "ON", "Mouse wheel zoom's On");
    require(
        !engine_settings().frame_stats && engine_settings().wheel_zoom && !frame_stats_shown_,
        "the changes before Cancel did not take effect at once"
    );
    click(settings::cancel_control, "CANCEL", "Cancel");
    require(engine_settings_dialog() == nullptr, "Cancel did not close the dialog");
    require(
        same_settings(engine_settings(), chosen) && frame_stats_shown_,
        "Cancel did not put back the settings the dialog opened with"
    );
    require(
        oa::platform::preferences::load(preference_path_) == saved,
        "Cancel changed the preferences file"
    );

    // Languages a catalogue offers. A hook records the download in place of
    // the queue. Cancel puts the opened language back and leaves the download.
    {
        std::string recorded;
        language_download_hook_context_ = &recorded;
        language_download_hook_ = [](void* context, std::string_view tag, std::string&) {
            *static_cast<std::string*>(context) = std::string(tag);
            return true;
        };
        const std::string simplified = "\347\256\200\344\275\223\344\270\255\346\226\207";
        const std::string portuguese = "Portugu\u00EAs (Brasil)";
        std::array<CatalogueLanguage, 2> offered{};
        offered[0].registry = "oa";
        offered[0].reviewed = true;
        offered[0].entry.tag = "zh-Hans";
        offered[0].entry.endonym = simplified;
        offered[0].entry.english_name = "Chinese (Simplified)";
        offered[0].entry.word = "Chinese";
        offered[0].entry.locales = {"zh-Hans", "zh-CN", "zh"};
        offered[0].entry.needs = oa::data::languages::TextNeeds::modern_fonts;
        offered[0].publisher = "OA team";
        offered[0].bytes = 4012345;
        offered[1].registry = "ridge";
        offered[1].reviewed = true;
        offered[1].entry.tag = "pt-BR";
        offered[1].entry.endonym = portuguese;
        offered[1].entry.english_name = "Portuguese (Brazil)";
        offered[1].entry.word = "Portuguese";
        offered[1].entry.locales = {"pt-BR"};
        offered[1].entry.needs = oa::data::languages::TextNeeds::modern_fonts;
        offered[1].publisher = "Ridge";
        offered[1].coverage = 0.92;
        offered[1].bytes = 1100000;
        refresh_available_languages(offered);
        const std::string opened_language = engine_settings().language;
        open("for languages available to download");
        click(settings::page_control(settings::Page::language), {}, "Language's entry");
        click(settings::first_row_control, {}, "Language's field");
        auto* listing = engine_settings_dialog();
        require(
            listing != nullptr && listing->open_list == settings::first_row_control,
            "Language's list did not open for the packs to download"
        );
        int32_t installed = 1;
        for (const oa::data::languages::Language* language : oa::data::languages::known_languages())
            if (oa::data::languages::playable(*language))
                ++installed;
        const int32_t total =
            installed + 1 + static_cast<int32_t>(listing->available_languages.size());
        constexpr int32_t shown_rows = 8;
        listing->list_first = std::max(total - shown_rows, 0);
        require(listing->available_languages.size() == 2, "the dialog does not list both packs");
        const auto shown_name = [](const settings::AvailableLanguage& pack) {
            if (pack.endonym_drawable && !pack.endonym.empty())
                return pack.endonym;
            if (!pack.english_name.empty())
                return pack.english_name;
            return pack.tag;
        };
        const std::string simplified_shown = shown_name(listing->available_languages[0]);
        const std::string portuguese_shown = shown_name(listing->available_languages[1]);
        const auto parts = settings::dialog_layout(*listing);
        require(shows_text(parts, "AVAILABLE TO DOWNLOAD"), "the language list has no divider");
        require(shows_text(parts, simplified_shown), "the language list has no Simplified Chinese");
        require(
            shows_text(parts, portuguese_shown), "the language list has no Portuguese (Brazil)"
        );
        require(shows_text(parts, "4.0 MB \u21E9"), "Simplified Chinese shows no size");
        require(
            shows_text(parts, "community \u00B7 92% \u00B7 1.1 MB"),
            "Portuguese (Brazil) shows no coverage"
        );
        for (const settings::LayoutPart& part : parts) {
            if (part.text.empty())
                continue;
            require(modern_fonts_draw(part.text), "a settings part does not draw: " + part.text);
        }
        std::cout << "engine settings check: injected row " << simplified << '\n';
        std::cout << "engine settings check: injected row " << portuguese << '\n';
        if (!options_.snapshot.empty()) {
            point(SDL_EVENT_MOUSE_MOTION, kSnapshotPointer);
            renderer::Surface presented;
            capture_frame_ = &presented;
            render();
            capture_frame_ = nullptr;
            write_ppm(
                step_snapshot(options_.snapshot, "menu-dialog-language-available"), presented
            );
            point(SDL_EVENT_MOUSE_MOTION, kRestingPointer);
        }
        click(settings::no_control, simplified_shown, "Language's Simplified Chinese");
        listing = engine_settings_dialog();
        require(
            recorded == "zh-Hans" && listing != nullptr &&
                listing->open_list == settings::no_control,
            "choosing Simplified Chinese did not record its download"
        );
        const settings::AvailableLanguage* waiting = nullptr;
        for (const settings::AvailableLanguage& pack : listing->available_languages)
            if (pack.tag == "zh-Hans")
                waiting = &pack;
        require(
            waiting != nullptr && waiting->state == settings::AvailableLanguage::State::waiting &&
                shows_text(settings::dialog_layout(*listing), simplified_shown + " \u00B7 waiting"),
            "Simplified Chinese is not waiting after its download was recorded"
        );
        std::cout << "engine settings check: recorded download " << recorded << '\n';
        click(settings::cancel_control, "CANCEL", "Cancel");
        require(engine_settings_dialog() == nullptr, "Cancel did not close the language dialog");
        require(
            engine_settings().language == opened_language && shown_language().tag == "en" &&
                same_settings(engine_settings(), chosen),
            "Cancel did not put the opened language back"
        );
        language_download_hook_ = nullptr;
        language_download_hook_context_ = nullptr;
        release_available_languages();
    }

    // Restore defaults resets every setting at once, Cancel undoes it, and
    // OK after it erases every key it reset.
    open("for Restore defaults");
    click(settings::restore_control, "RESTORE DEFAULTS", "Restore defaults");
    // Screen size, applied when OK is pressed, stays as it was until then.
    auto restored_now = defaults;
    restored_now.screen_size = chosen.screen_size;
    require(
        engine_settings_dialog() != nullptr && same_settings(engine_settings(), restored_now) &&
            !frame_stats_shown_ &&
            unit_supersampling_ == oa::present::model::UnitSupersampling::off,
        "Restore defaults did not reset every setting at once"
    );
    require(!renderer_waits(sdl_.renderer), "Restore defaults did not stop the renderer waiting");
    require(
        presentation() == SDL_LOGICAL_PRESENTATION_LETTERBOX,
        "Restore defaults did not letterbox the main menu again"
    );
    tap(SDLK_ESCAPE);
    require(
        engine_settings_dialog() == nullptr && same_settings(engine_settings(), chosen),
        "Escape after Restore defaults did not put the settings back"
    );
    require(
        renderer_waits(sdl_.renderer) == vertical_sync,
        "Escape after Restore defaults did not put Vertical sync back"
    );
    open("for Restore defaults and OK");
    click(settings::restore_control, "RESTORE DEFAULTS", "Restore defaults");
    tap(SDLK_RETURN);
    require(
        engine_settings_dialog() == nullptr && same_settings(engine_settings(), defaults),
        "Enter after Restore defaults did not keep the defaults"
    );
    const auto restored = oa::platform::preferences::load(preference_path_);
    require(engine_keys(restored).empty(), "Restore defaults and OK left settings in the file");
    // Desktop in a window leaves its size as it is; the window then goes
    // back to its size for the steps that follow.
    pump_window_events();
    require(window_size_text() == "1280x720", "Desktop resized the window");
    require(
        SDL_SetWindowSize(sdl_.window, opened_width, opened_height),
        "the window was not resized back"
    );
    pump_window_events();
    require(!renderer_waits(sdl_.renderer), "Restore defaults and OK left the renderer waiting");
    require(saved_general_number("SwitchAlt") == 0, "Restore defaults did not save SwitchAlt off");

    // Developer Mode: off, its list shows the profile and takes no change;
    // on, an override of a rule (sim-scope) hack plays at once with no
    // match running and one of a display (view-scope) hack shows at once;
    // OK keeps them under the base game's id, Restore profile values clears
    // them, and Off plays the profile as it ships.
    {
        namespace profiles = oa::data::mod_profile;
        const std::string overrides_key =
            std::string(settings::key::hack_overrides) + std::string(profiles::base_game_id);
        const std::string mode_key{settings::key::developer_mode};
        const auto parts_now = [&] { return settings::dialog_layout(*engine_settings_dialog()); };
        // Clicks the middle of a part with a text, whatever its control.
        const auto click_text = [&](std::string_view text, std::string_view what) {
            const auto parts = parts_now();
            const settings::LayoutPart* found = nullptr;
            for (const auto& part : parts)
                if (part.text == text)
                    found = &part;
            require(found != nullptr, "Developer Mode shows no " + std::string(what));
            click_at(found->rect.x + found->rect.width / 2, found->rect.y + found->rect.height / 2);
        };
        const auto click_hack = [&](std::string_view hack, std::string_view caption) {
            const auto parts = parts_now();
            const auto* half = hack_switch(parts, hack, caption);
            require(half != nullptr, "Developer Mode shows no switch of " + std::string(hack));
            click_at(half->rect.x + half->rect.width / 2, half->rect.y + half->rect.height / 2);
        };
        // Scrolls the list a page at a time until it shows a text. A wrapped
        // hint over the list shortens each page, so the walk continues until
        // the list will not move, not for a fixed count of pages.
        const auto reveal = [&](std::string_view text) {
            tap(SDLK_HOME);
            for (int32_t pages = 0; pages < 400; ++pages) {
                if (shows_text(parts_now(), text))
                    return true;
                auto* scrolling = engine_settings_dialog();
                require(scrolling != nullptr, "the dialog closed while its list scrolled");
                const int32_t before =
                    scrolling->scroll[static_cast<std::size_t>(settings::Page::developer)];
                tap(SDLK_PAGEDOWN);
                scrolling = engine_settings_dialog();
                require(scrolling != nullptr, "the dialog closed while its list scrolled");
                if (scrolling->scroll[static_cast<std::size_t>(settings::Page::developer)] ==
                    before)
                    break;
            }
            return shows_text(parts_now(), text);
        };
        open("for Developer Mode");
        click(settings::page_control(settings::Page::developer), {}, "Developer's entry");
        dialog = engine_settings_dialog();
        require(dialog->page == settings::Page::developer, "Developer did not show");
        auto parts = parts_now();
        require(
            shows_text(parts, "Enable Developer Mode") &&
                shows_text(parts, "Show performance statistics") &&
                shows_text(parts, active_only_label(0)),
            "Developer does not show its switches with no hack on"
        );
        require(!shows_text(parts, kRuleHack), "an area of Developer Mode starts open");
        click_text(kFirstArea, "first area");
        require(reveal(kRuleHack), "a click on an area did not open it");
        click_hack(kRuleHack, "ON");
        require(
            engine_settings_dialog()->chosen.hack_overrides.empty() && mod_profile() == nullptr,
            "a hack's switch took a change while Developer Mode was off"
        );
        click(settings::developer_mode_control, "ON", "Enable Developer Mode's On");
        require(developer_mode(), "Enable Developer Mode did not turn it on at once");
        click_hack(kRuleHack, "ON");
        const auto* rules = mod_profile();
        require(
            rules != nullptr && rules->rules.ai.attack_wave_size.enabled &&
                shows_text(parts_now(), active_only_label(1)),
            "a rule hack turned on did not play at once with no match running"
        );
        require(reveal(kDisplayArea), "Developer Mode does not list the display area");
        click_text(kDisplayArea, "display area");
        require(reveal(kDisplayHack), "the display area did not open");
        click_hack(kDisplayHack, "ON");
        require(
            ui_rules().whiteboard.enabled &&
                engine_settings_dialog()->chosen.hack_overrides.size() == 2,
            "a display hack turned on did not show at once"
        );
        click(settings::ok_control, "OK", "OK");
        const auto kept = oa::platform::preferences::load(preference_path_);
        require(
            kept.contains(mode_key) && kept.at(mode_key) == "1" && kept.contains(overrides_key) &&
                profiles::read_overrides(kept.at(overrides_key)).size() == 2,
            "OK did not keep Developer Mode and its overrides under the base game's id"
        );
        dialog = open("for Restore profile values");
        require(dialog->page == settings::Page::developer, "the dialog did not open on Developer");
        click(
            settings::restore_profile_control, "RESTORE PROFILE VALUES", "Restore profile values"
        );
        require(
            engine_settings_dialog()->chosen.hack_overrides.empty() && mod_profile() == nullptr &&
                !ui_rules().whiteboard.enabled,
            "Restore profile values did not put the profile's values back at once"
        );
        click(settings::developer_mode_control, "OFF", "Enable Developer Mode's Off");
        require(!developer_mode(), "Enable Developer Mode did not turn it off at once");
        click(settings::ok_control, "OK", "OK");
        const auto cleared = oa::platform::preferences::load(preference_path_);
        require(
            !cleared.contains(overrides_key) && cleared.contains(mode_key) &&
                cleared.at(mode_key) == "0",
            "Restore profile values and Off did not leave the overrides out of the file"
        );
        // Restore defaults and OK leave no key of the settings behind, and the
        // list opens closed again.
        open("after Developer Mode");
        click(settings::restore_control, "RESTORE DEFAULTS", "Restore defaults");
        tap(SDLK_RETURN);
        require(
            engine_keys(oa::platform::preferences::load(preference_path_)).empty() &&
                same_settings(engine_settings(), defaults),
            "Restore defaults and OK after Developer Mode left settings in the file"
        );
        engine_settings_state().last_developer_list.reset();
        engine_settings_state().last_page = settings::Page::common_tweaks;
    }

    // With touch controls the dialog lists Touch between Graphics and
    // Developer, with its five rows at their defaults; a finger's press near
    // a switch takes it, and a mouse press there does nothing.
    {
        // The touch controls go back off however the block ends.
        struct TouchControlsOn {
            Runtime& runtime;
            bool before;

            ~TouchControlsOn() {
                runtime.options_.touch_controls = before;
                runtime.engine_settings_state().finger_pointer = false;
                runtime.engine_settings_state().last_page = settings::Page::common_tweaks;
            }
        } touch_on{*this, options_.touch_controls};

        options_.touch_controls = true;
        // A finger's click as the touch controls send it: on the picture's
        // pixels, from no window, from the touch mouse.
        const auto finger = [this](SDL_EventType type, layout::Point at) {
            SDL_Event event{};
            event.type = type;
            if (type == SDL_EVENT_MOUSE_MOTION) {
                event.motion.which = SDL_TOUCH_MOUSEID;
                event.motion.x = static_cast<float>(at.x);
                event.motion.y = static_cast<float>(at.y);
            } else {
                event.button.which = SDL_TOUCH_MOUSEID;
                event.button.button = SDL_BUTTON_LEFT;
                event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                event.button.clicks = 1;
                event.button.x = static_cast<float>(at.x);
                event.button.y = static_cast<float>(at.y);
            }
            bool running = true;
            dispatch_event(event, running);
            require(running, "a finger in the dialog ended the run");
        };
        const auto finger_click = [&](layout::Point at) {
            finger(SDL_EVENT_MOUSE_MOTION, at);
            finger(SDL_EVENT_MOUSE_BUTTON_DOWN, at);
            finger(SDL_EVENT_MOUSE_BUTTON_UP, at);
        };
        dialog = open("with touch controls");
        require(dialog->touch, "the dialog opened with touch controls does not list Touch");
        auto parts = settings::dialog_layout(*dialog);
        const auto* graphics_entry =
            find_part(parts, settings::page_control(settings::Page::graphics), {});
        const auto* touch_entry =
            find_part(parts, settings::page_control(settings::Page::touch), {});
        const auto* developer_entry =
            find_part(parts, settings::page_control(settings::Page::developer), {});
        require(
            graphics_entry != nullptr && touch_entry != nullptr && developer_entry != nullptr &&
                touch_entry->text == "Touch" && graphics_entry->rect.y < touch_entry->rect.y &&
                touch_entry->rect.y < developer_entry->rect.y,
            "Touch is not listed between Graphics and Developer"
        );
        click(settings::page_control(settings::Page::touch), {}, "Touch's entry");
        require(engine_settings_dialog()->page == settings::Page::touch, "Touch did not show");
        parts = settings::dialog_layout(*engine_settings_dialog());
        for (const std::string_view text :
             {"One-finger drag",
              "Automatic",
              "Hold delay",
              "350 ms",
              "QUEUE and ADD",
              "Stay on",
              "Haptics"})
            require(shows_text(parts, text), "Touch does not show " + std::string(text));
        // Haptics, the fourth row, is On; a press left of its switch, two
        // thirds of the touch controls' reach from its Off half, takes it.
        const auto* haptics_off = find_part(parts, settings::first_row_control + 3, "OFF");
        require(haptics_off != nullptr && engine_settings().touch_haptics, "Haptics is not On");
        const int32_t reach = EngineSettingsState::finger_reach(*this, 1.0);
        const layout::Point beside{
            placement().x + haptics_off->rect.x - 1 - reach * 2 / 3,
            placement().y + haptics_off->rect.y + haptics_off->rect.height / 2
        };
        click_at(beside.x - placement().x, beside.y - placement().y);
        require(engine_settings().touch_haptics, "a mouse press beside Haptics' switch took it");
        finger_click({beside.x - reach, beside.y});
        require(engine_settings().touch_haptics, "a finger's press out of reach took Haptics");
        finger_click(beside);
        require(
            !engine_settings().touch_haptics,
            "a finger's press beside Haptics' switch did not take it"
        );
        tap(SDLK_ESCAPE);
        require(
            engine_settings_dialog() == nullptr && same_settings(engine_settings(), defaults),
            "Escape did not put Haptics back"
        );
    }
    // Settings has no Mods section: the mods are chosen in the Library, as
    // the run's own feed lists them. No mod plays, so none is PLAYING; the
    // mods are listed by title as their oamod.yaml names them, a folder
    // without one by its name, and a folder an earlier version's Pick
    // Folder... stored while it is the mod stored and still a folder, each
    // with PLAY NOW, which the main menu offers. --base-game turns PLAY NOW
    // off and says so; SWITCH OFF on the picked folder played asks the
    // Switch Mod question, which Escape and CANCEL put away with nothing
    // changed, and SWITCH plays no mod and forgets the folder, as a start
    // does a picked folder that is not the mod stored; a stored mod folder
    // that is gone reads as no mod played, and OK replaces it.
    {
        auto& state = engine_settings_state();
        const fs::path base = fs::absolute(preference_path_).parent_path();
        const fs::path empty_folder = (base / "picked-empty").lexically_normal();
        const fs::path mod_folder = (base / "picked-mod").lexically_normal();
        fs::create_directories(empty_folder);
        fs::create_directories(mod_folder);
        {
            std::ofstream profile(mod_folder / "oamod.yaml", std::ios::binary);
            profile << kPickedProfile;
        }
        const std::string picked = path_to_utf8(mod_folder);
        const std::string empty = path_to_utf8(empty_folder);
        const std::string mod_key{settings::key::mod_directory};
        const std::string picked_key{settings::key::picked_mod_directory};
        preference_values_[mod_key] = picked;
        preference_values_[picked_key] = picked;
        list_offered_mods();
        const auto parts_now = [&] { return settings::dialog_layout(*engine_settings_dialog()); };
        open("without Mods");
        {
            const auto listed = parts_now();
            require(
                !shows_text(listed, "Mods") && !shows_text(listed, "OPEN MODS FOLDER") &&
                    !shows_text(listed, "No Mod"),
                "Settings still lists a Mods section"
            );
        }
        click(settings::cancel_control, "CANCEL", "Cancel");
        // The Library over the mod folders the run lists.
        LibraryHost& host = library_host();
        // A Library opens on Active: the mods are chosen on its Mods tab.
        const auto library_now = [&] {
            list_offered_mods();
            host.rebuild(*this);
            library::set_tab(host.model, library::Tab::mods);
        };
        const auto entry_at = [&host](const std::string& folder) -> const library::Entry* {
            for (const library::Entry& each : host.model.entries)
                if (each.id.kind == library::Kind::mod && each.installed != nullptr &&
                    path_to_utf8(fs::absolute(each.installed->folder).lexically_normal()) == folder)
                    return &each;
            return nullptr;
        };
        const auto offers = [&host](const library::Entry& entry, library::Action wanted) {
            for (const library::ActionButton& button : library::entry_actions(host.model, entry))
                if (button.action == wanted)
                    return button.enabled ? 1 : 0;
            return -1;
        };
        const auto none_playing = [&host] {
            return std::none_of(
                host.model.entries.begin(),
                host.model.entries.end(),
                [](const library::Entry& each) {
                    return each.installed != nullptr && each.installed->playing;
                }
            );
        };
        // Answers the question on top of the OA layer with a key, as the layer does.
        const auto answer = [this](oa::ui::kit::Key key, uint32_t sdl_key) {
            LayerScreen* top = oa_layer().top();
            require(top != nullptr, "no question shows to answer");
            oa_layer().take_answer(*top, top->key(key, sdl_key), sdl_key);
            oa_layer().forget_latched_key();
        };
        const auto switch_question = [this]() -> const QuestionScreen* {
            const auto* asked = dynamic_cast<const QuestionScreen*>(oa_layer().top());
            if (asked == nullptr || asked->question().title != "SWITCH MOD")
                return nullptr;
            return asked;
        };
        library_now();
        {
            const library::Entry* listed = entry_at(picked);
            require(
                none_playing() && listed != nullptr &&
                    listed->installed->name == "Picked folder check" &&
                    offers(*listed, library::Action::play_now) == 1 &&
                    offers(*listed, library::Action::switch_off) == -1,
                "the Library does not list the stored picked folder by its title with PLAY NOW, "
                "no mod played"
            );
        }
        // --base-game sets the Mod setting aside for the run: PLAY NOW is
        // off, the details say why, and PLAY NOW asks nothing.
        options_.base_game = true;
        library_now();
        {
            const library::Entry* listed = entry_at(picked);
            require(listed != nullptr, "the Library does not list the picked folder");
            const library::EntryId id = listed->id;
            library::select(host.model, id);
            require(
                offers(*listed, library::Action::play_now) == 0 &&
                    library::playing_note(host.model) == "The command line chose this run's mod.",
                "--base-game does not turn PLAY NOW off and say so"
            );
            library_entry_action(library::Action::play_now, id);
            require(
                !soft_restart_requested() && engine_settings().mod_folder.empty() &&
                    switch_question() == nullptr,
                "PLAY NOW switched the mod or asked under --base-game"
            );
        }
        options_.base_game = false;
        // SWITCH OFF from the picked folder played to no mod erases both
        // keys, and the next start, which plays no mod, no longer lists the
        // folder.
        {
            const auto lists_picked = [&] {
                return std::find(state.mod_folders.begin(), state.mod_folders.end(), picked) !=
                       state.mod_folders.end();
            };
            const auto game_folders = options_.game_folders;
            const fs::path saved_layered = options_.layered_mod_folder;
            const fs::path game_folder =
                game_folders.empty() ? options_.game_dir : game_folders.back();
            options_.game_folders = {mod_folder, game_folder};
            options_.layered_mod_folder = mod_folder;
            load_engine_settings();
            require(
                state.playing_mod_folder == picked && engine_settings().mod_folder == picked &&
                    preference_values_.contains(picked_key),
                "the picked folder stored does not play"
            );
            library_now();
            const library::Entry* played = entry_at(picked);
            require(
                played != nullptr && played->installed->playing &&
                    offers(*played, library::Action::switch_off) == 1 &&
                    offers(*played, library::Action::play_now) == -1,
                "the picked folder played offers no SWITCH OFF"
            );
            const library::EntryId id = played->id;
            library_entry_action(library::Action::switch_off, id);
            {
                const QuestionScreen* asked = switch_question();
                require(
                    asked != nullptr && asked->question().buttons.size() == 2 &&
                        asked->question().buttons[0].id == "cancel" &&
                        asked->question().buttons[1].id == "switch" &&
                        asked->question().buttons[1].caption == "SWITCH" &&
                        asked->question().marked == 0,
                    "SWITCH OFF did not ask the Switch Mod question"
                );
            }
            if (!options_.snapshot.empty())
                write_ppm(step_snapshot(options_.snapshot, "menu-switch-off-question"), frame());
            answer(oa::ui::kit::Key::escape, SDLK_ESCAPE);
            require(
                switch_question() == nullptr && !soft_restart_requested() &&
                    engine_settings().mod_folder == picked,
                "Escape on the question did not put it away and keep the Mod as it was"
            );
            library_entry_action(library::Action::switch_off, id);
            require(switch_question() != nullptr, "a second SWITCH OFF did not ask again");
            answer(oa::ui::kit::Key::enter, SDLK_RETURN);
            require(
                switch_question() == nullptr && !soft_restart_requested() &&
                    engine_settings().mod_folder == picked,
                "CANCEL changed the Mod"
            );
            library_entry_action(library::Action::switch_off, id);
            answer(oa::ui::kit::Key::yes, SDLK_Y);
            const auto switched = oa::platform::preferences::load(preference_path_);
            require(
                soft_restart_requested() && engine_settings_dialog() == nullptr &&
                    !switched.contains(mod_key) && !switched.contains(picked_key),
                "SWITCH away from the picked folder did not forget it"
            );
            soft_restart_requested_ = false;
            exit_requested_ = false;
            options_.game_folders = game_folders;
            options_.layered_mod_folder = saved_layered;
            load_engine_settings();
            library_now();
            require(
                !lists_picked() && entry_at(picked) == nullptr,
                "the Library lists the picked folder after a switch away from it"
            );
            // A picked folder's key that names a folder other than the mod
            // stored is erased at start, and the folder is not listed.
            preference_values_[picked_key] = picked;
            load_engine_settings();
            require(
                !preference_values_.contains(picked_key) && !lists_picked(),
                "a start kept a picked folder that is not the mod stored"
            );
        }
        // A stored mod folder that is gone, which the start dropped: no mod
        // plays, and OK replaces the stored folder.
        const std::string gone = path_to_utf8((base / "gone-mod").lexically_normal());
        preference_values_[mod_key] = gone;
        oa::platform::preferences::save(preference_path_, preference_values_);
        load_engine_settings();
        require(
            engine_settings().mod_folder.empty() && state.dropped_mod_folder == gone,
            "a gone mod folder stored did not read as No Mod"
        );
        library_now();
        require(none_playing(), "the Library over a gone mod folder shows a mod played");
        open("over a gone mod folder");
        click(settings::ok_control, "OK", "OK");
        const auto saved_mod = oa::platform::preferences::load(preference_path_);
        require(
            !saved_mod.contains(mod_key) && state.dropped_mod_folder.empty(),
            "OK over a gone mod folder did not erase it"
        );
        // A start that plays the folder without a profile layers it over
        // the game folder with no profile, by 3.1c's own rules, and keeps
        // Developer Mode's overrides under the folder's own id.
        {
            const auto game_folders = options_.game_folders;
            const fs::path saved_layered = options_.layered_mod_folder;
            const fs::path game_folder =
                game_folders.empty() ? options_.game_dir : game_folders.back();
            options_.game_folders = {empty_folder, game_folder};
            options_.layered_mod_folder = empty_folder;
            load_engine_settings();
            require(
                mod_profile() == nullptr && state.profile_id == folder_overrides_id(empty_folder) &&
                    state.profile_id != oa::data::mod_profile::base_game_id &&
                    state.playing_mod_folder == empty,
                "a mod folder without a profile does not play by 3.1c's rules under its own "
                "overrides' id"
            );
            options_.game_folders = game_folders;
            options_.layered_mod_folder = saved_layered;
            load_engine_settings();
        }
        // The settings as the check found them.
        preference_values_.erase(picked_key);
        oa::platform::preferences::save(preference_path_, preference_values_);
        state.current.picked_mod_folder.clear();
        state.last_page = settings::Page::common_tweaks;
        std::error_code ignored;
        fs::remove_all(empty_folder, ignored);
        fs::remove_all(mod_folder, ignored);
    }
    rest();
    oa_layer().forget_latched_key();
    fake_frontend_tick_ = previous_tick;
    std::cout << "engine settings check: every section, each setting through the dialog's "
                 "pointer and keys, OK, Cancel and Restore defaults, the keys they save, "
                 "Developer Mode's overrides in effect and kept, and the Touch section with a "
                 "finger's press taking the nearest control\n";
}

void Runtime::check_engine_settings_window_sizes() {
    std::cout << "engine settings check: the main menu as the window shows it\n";
    const auto previous_tick = fake_frontend_tick_;
    fake_frontend_tick_ = 1000U;
    // Where the OA layer's settings screen shows in the window, while it is open.
    const auto placed = [this] {
        require(
            oa_layer().find("settings") != nullptr, "the settings screen is not on the OA layer"
        );
        return oa_layer().placement_of("settings");
    };
    const auto point = [this](SDL_EventType type, layout::Point at) {
        send_check_pointer(type, at, type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT);
    };
    const auto click = [&](layout::Point at) {
        point(SDL_EVENT_MOUSE_MOTION, at);
        point(SDL_EVENT_MOUSE_BUTTON_DOWN, at);
        point(SDL_EVENT_MOUSE_BUTTON_UP, at);
    };
    // Clicks a window pixel, as the pointer there would.
    const auto click_window = [this](layout::Point pixel) {
        for (const SDL_EventType type :
             {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event = window_pointer_event(
                sdl_.window,
                sdl_.renderer,
                type,
                pixel,
                type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
            );
            bool running = true;
            dispatch_event(event, running);
            require(running, "a click in the window ended the run");
        }
    };
    // Clicks the middle of one of the dialog's parts, where the window shows it.
    const auto click_part = [&](const settings::LayoutPart* part, std::string_view what) {
        require(part != nullptr, "the dialog shows no " + std::string(what));
        click_window(layer_window_pixel(
            placed(), {part->rect.x + part->rect.width / 2, part->rect.y + part->rect.height / 2}
        ));
    };
    const auto tap = [this](SDL_Keycode code, SDL_Keymod modifiers = SDL_KMOD_NONE) {
        bool running = true;
        for (const bool down : {true, false}) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.windowID = SDL_GetWindowID(sdl_.window);
            event.key.key = code;
            event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
            event.key.mod = modifiers;
            event.key.down = down;
            dispatch_event(event, running);
        }
        require(running, "a key ended the run");
    };
    // Writes the window's frame as a step's snapshot when --snapshot names
    // one, the pointer moved out of the way.
    const auto snapshot = [&](const std::string& step) {
        if (options_.snapshot.empty())
            return;
        point(SDL_EVENT_MOUSE_MOTION, kSnapshotPointer);
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        write_ppm(step_snapshot(options_.snapshot, step), presented);
        point(SDL_EVENT_MOUSE_MOTION, kRestingPointer);
    };
    const auto sparks = menu_sparks_;
    // The window's frame and the picture it should show, drawn from the
    // same sparks, the picture at the display gamma.
    const auto present = [&](renderer::Surface& presented, renderer::Surface& picture) {
        menu_sparks_ = sparks;
        picture = frame_without_cursor();
        apply_gamma_rgb(picture.rgb.data(), picture.rgb.size() / 3U, 3);
        menu_sparks_ = sparks;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
    };
    // The window's frame alone, drawn from the same sparks.
    const auto presented_frame = [&] {
        renderer::Surface presented;
        menu_sparks_ = sparks;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        return presented;
    };
    // The backdrop darkens the window's frame itself, so the frame expected
    // under the dialog is the closed one darkened.
    require(
        gamma_identity_,
        "the window-size check compares frames at a display gamma that changes no colour"
    );

    // Every frame read back holds the whole window, the letterbox round the
    // picture with it, which the dialog's backdrop darkens too.
    struct WholeWindow {
        OaLayer& layer;

        ~WholeWindow() { layer.read_whole_window(false); }
    } whole_window{oa_layer()};

    oa_layer().read_whole_window(true);

    for (std::size_t size_index = 0; size_index < kWindowSizes.size(); ++size_index) {
        const auto [width, height] = kWindowSizes[size_index];
        const WindowLayout& expected_layout = kWindowLayouts[size_index];
        const std::string size = std::to_string(width) + 'x' + std::to_string(height);
        const std::string on = " on the " + size + " window";
        if (!SDL_SetWindowSize(sdl_.window, width, height) || !SDL_SyncWindow(sdl_.window))
            throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
        load(Screen::main_menu);
        require(screen_ == Screen::main_menu, "the main menu did not open" + on);
        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSizeInPixels(sdl_.window, &window_width, &window_height);
        require(window_width == width && window_height == height, "the window is not " + size);
        SDL_FRect area{};
        require(
            SDL_GetRenderLogicalPresentationRect(sdl_.renderer, &area) &&
                area.w >= static_cast<float>(kCanvasWidth) &&
                area.h >= static_cast<float>(kCanvasHeight),
            "the main menu is not letterboxed at 640x480 or more" + on
        );
        point(SDL_EVENT_MOUSE_MOTION, kRestingPointer);

        // The main menu with the OA button, as the window shows it.
        renderer::Surface presented;
        renderer::Surface picture;
        present(presented, picture);
        const auto button = EngineSettingsMenuHost::button_rect(*this);
        const auto settings_button = EngineSettingsMenuHost::settings_button_rect(*this);
        require(
            button.x == kMenuButtonCorner.x && button.y == kMenuButtonCorner.y &&
                settings_button.x == kMenuSettingsCorner.x &&
                settings_button.y == kMenuSettingsCorner.y &&
                EngineSettingsMenuHost::button_shown(*this),
            "the OA button and the Settings button are not in the picture's bottom-right "
            "corner" +
                on
        );
        // Above the picture's size the window draws each button from a larger
        // icon, over the window rectangle, which is not the picture's
        // 48-pixel square. A sample is left out only when it lands in that
        // rectangle. At the picture's size the window shows the picture,
        // buttons included.
        const auto windows = oa_layer().menu_button_windows();
        std::array<layout::Rect, 2> sharp_buttons{};
        std::size_t sharp_count = 0;
        if (windows.library.width != button.width || windows.library.height != button.height)
            sharp_buttons[sharp_count++] = windows.library;
        if (windows.settings.width != settings_button.width ||
            windows.settings.height != settings_button.height)
            sharp_buttons[sharp_count++] = windows.settings;
        const auto differing = letterbox_differences(
            presented,
            picture,
            area,
            window_width,
            kRestingPointer,
            std::span<const layout::Rect>(sharp_buttons.data(), sharp_count)
        );
        require(
            differing == 0,
            "the window does not show the main menu with its OA button" + on + ": " +
                std::to_string(differing) + " pixels differ"
        );
        snapshot("menu-" + size);
        // The window's frame with no OA screen open, which the dialog's
        // backdrop darkens.
        const renderer::Surface closed = presented;

        // A click on the button where the window shows it opens the Library,
        // which Escape closes; the shortcut opens the dialog, laid out at the
        // class and drawn at the scale this window gives.
        click({button.x + button.width / 2, button.y + button.height / 2});
        require(
            oa_layer().find("library") != nullptr && engine_settings_dialog() == nullptr,
            "a click on the OA button did not open the Library" + on
        );
        tap(SDLK_ESCAPE);
        require(oa_layer().find("library") == nullptr, "Escape did not close the Library" + on);
        // A click on the Settings button opens Settings with no Library
        // under it, which Escape closes.
        click(
            {settings_button.x + settings_button.width / 2,
             settings_button.y + settings_button.height / 2}
        );
        require(
            engine_settings_dialog() != nullptr && oa_layer().find("settings") != nullptr &&
                oa_layer().find("library") == nullptr,
            "a click on the Settings button did not open Settings alone" + on
        );
        tap(SDLK_ESCAPE);
        require(
            engine_settings_dialog() == nullptr && oa_layer().find("settings") == nullptr,
            "Escape did not close Settings" + on
        );
        tap(SDLK_COMMA, kShortcutModifier);
        auto* dialog = engine_settings_dialog();
        require(
            dialog != nullptr && oa_layer().find("settings") != nullptr,
            "the shortcut did not open the dialog" + on
        );
        point(SDL_EVENT_MOUSE_MOTION, kRestingPointer);
        const LayerView seen = oa_layer().view();
        require(
            oa_layer().screen_class() == expected_layout.size_class &&
                oa_layer().screen_scale() == expected_layout.scale &&
                dialog->size_class == expected_layout.size_class,
            "the dialog is not laid out at " +
                std::string(size_class_name(expected_layout.size_class)) + ", " +
                std::to_string(expected_layout.scale) + "x" + on
        );
        {
            // Centred in the window at the class's size times the scale.
            const auto& sized = oa::ui::kit::metrics_of(expected_layout.size_class);
            const LayerPlacement at = placed();
            const int32_t shown_width = sized.dialog_width * expected_layout.scale;
            const int32_t shown_height = sized.dialog_height * expected_layout.scale;
            require(
                at.points_width == sized.dialog_width && at.points_height == sized.dialog_height &&
                    at.shown.width == shown_width && at.shown.height == shown_height &&
                    at.shown.x == (width - shown_width) / 2 &&
                    at.shown.y == (height - shown_height) / 2,
                "the dialog is not centred in the window at its class's size" + on
            );
        }
        // The window as it shows the dialog now: the closed frame darkened,
        // the dialog drawn at its class and a scale at its place: at a whole
        // scale by draw_dialog, and at 150% offscreen on the kit canvas's
        // smooth form in the modern fonts.
        const auto shows_dialog_at = [&](std::string_view what, int32_t drawn_percent) {
            point(SDL_EVENT_MOUSE_MOTION, kRestingPointer);
            const LayerPlacement at = placed();
            renderer::Surface drawing;
            if (drawn_percent % 100 == 0) {
                drawing.width = static_cast<uint32_t>(at.shown.width);
                drawing.height = static_cast<uint32_t>(at.shown.height);
                drawing.rgb.assign(
                    static_cast<std::size_t>(drawing.width) * drawing.height * 3U, 0
                );
                settings::draw_dialog(
                    drawing,
                    {0, 0, drawn_percent / 100},
                    *engine_settings_dialog(),
                    *engine_settings_fonts(),
                    EngineSettingsMenuHost::settings_icon(*this)
                );
            } else {
                const settings::Dialog& shown_dialog = *engine_settings_dialog();
                drawing = layer_drawing(
                    at.points_width,
                    at.points_height,
                    drawn_percent,
                    engine_settings_fonts(),
                    oa_layer().smooth_fonts(),
                    EngineSettingsMenuHost::settings_icon(*this),
                    [&shown_dialog](const oa::ui::kit::Canvas& canvas) {
                        settings::draw_dialog(canvas, shown_dialog);
                    }
                );
                require(
                    drawing.width == static_cast<uint32_t>(at.shown.width) &&
                        drawing.height == static_cast<uint32_t>(at.shown.height),
                    "the dialog drawn at " + std::to_string(drawn_percent) +
                        "% is not as large as its place" + on
                );
            }
            apply_gamma_rgb(drawing.rgb.data(), drawing.rgb.size() / 3U, 3);
            auto expected = expected_layer_frame(closed, drawing, at);
            const auto shown = presented_frame();
            // The closed frame holds the larger icons. Darkening that frame
            // is not what the window draws when the dialog is open: the icon
            // is darkened and blended on its own, over its window rectangle.
            // Those pixels are left as the window shows them. The dialog
            // covers its own place, which stays in the compare.
            const auto windows_now = oa_layer().menu_button_windows();
            const auto keep_icon = [&](const layout::Rect& window, int32_t source_side) {
                if (window.width == source_side && window.height == source_side)
                    return;
                for (int row = window.y; row < window.y + window.height; ++row) {
                    for (int column = window.x; column < window.x + window.width; ++column) {
                        if (column >= at.shown.x && row >= at.shown.y &&
                            column < at.shown.x + at.shown.width &&
                            row < at.shown.y + at.shown.height)
                            continue;
                        if (column < 0 || row < 0 ||
                            static_cast<uint32_t>(column) >= expected.width ||
                            static_cast<uint32_t>(row) >= expected.height)
                            continue;
                        const std::size_t at_pixel =
                            (static_cast<std::size_t>(row) * expected.width +
                             static_cast<std::size_t>(column)) *
                            3U;
                        expected.rgb[at_pixel] = shown.rgb[at_pixel];
                        expected.rgb[at_pixel + 1] = shown.rgb[at_pixel + 1];
                        expected.rgb[at_pixel + 2] = shown.rgb[at_pixel + 2];
                    }
                }
            };
            keep_icon(windows_now.library, button.width);
            keep_icon(windows_now.settings, settings_button.width);
            const auto differing_pixels =
                layer_differences(shown, expected, window_pointer(seen, kRestingPointer));
            if (differing_pixels != 0 && !options_.snapshot.empty()) {
                write_ppm(step_snapshot(options_.snapshot, "expected-" + size), expected);
                write_ppm(step_snapshot(options_.snapshot, "presented-" + size), shown);
            }
            require(
                differing_pixels == 0,
                "the window does not show " + std::string(what) + on + ": " +
                    std::to_string(differing_pixels) + " pixels differ"
            );
        };
        // The window as it shows the dialog at this window's own scale.
        const auto shows_dialog = [&](std::string_view what) {
            shows_dialog_at(what, expected_layout.scale * 100);
        };
        // Each section, through its entry where the window shows it.
        for (const auto page : kPages) {
            const auto parts = settings::dialog_layout(*dialog);
            click_part(
                find_part(parts, settings::page_control(page), {}),
                std::string(page_slug(page)) + "'s entry"
            );
            require(dialog->page == page, "a click on a section's entry did not show it" + on);
            shows_dialog("the dialog's " + std::string(page_slug(page)));
            snapshot("menu-dialog-" + std::string(page_slug(page)) + '-' + size);
            if (page == settings::Page::developer) {
                // Developer with the first area of Developer Mode's list
                // open, off and then on, and with a rule hack of it open and
                // on, as the window shows them; then closed and off again as
                // it was.
                // Clicks the part that shows a text.
                const auto click_text = [&](std::string_view text) {
                    const auto parts_now = settings::dialog_layout(*engine_settings_dialog());
                    const settings::LayoutPart* found = nullptr;
                    for (const auto& part : parts_now)
                        if (part.text == text)
                            found = &part;
                    click_part(found, text);
                };
                click_text(kFirstArea);
                shows_dialog("Developer Mode with an area open");
                snapshot("menu-dialog-developer-area-" + size);
                {
                    const auto area_parts = settings::dialog_layout(*engine_settings_dialog());
                    click_part(
                        find_part(area_parts, settings::developer_mode_control, "ON"),
                        "Enable Developer Mode's On"
                    );
                }
                shows_dialog("Developer Mode on");
                snapshot("menu-dialog-developer-on-" + size);
                click_text(kShownHack);
                {
                    const auto hack_parts = settings::dialog_layout(*engine_settings_dialog());
                    click_part(hack_switch(hack_parts, kShownHack, "ON"), "rule hack's switch");
                }
                shows_dialog("Developer Mode with a rule hack open and on");
                snapshot("menu-dialog-developer-hack-" + size);
                click_text(kShownHack);
                click_text(kFirstArea);
            }
            if (page != settings::Page::graphics)
                continue;
            // Graphics scrolls to its end, the class's own. A wider class wraps
            // each description onto fewer lines and its view is taller, so it
            // scrolls less: Compact's end is its own, Regular scrolls less than
            // that, and Large least of all.
            tap(SDLK_END);
            const int32_t end_limit = settings::scroll_limit(*dialog);
            const int32_t scrolled = dialog->scroll[static_cast<std::size_t>(page)];
            const bool compact_end =
                expected_layout.size_class == oa::ui::kit::SizeClass::compact &&
                end_limit == kGraphicsScrollLimit;
            const bool regular_end =
                expected_layout.size_class == oa::ui::kit::SizeClass::regular && end_limit > 0 &&
                end_limit < kGraphicsScrollLimit;
            const bool large_end = expected_layout.size_class == oa::ui::kit::SizeClass::large &&
                                   end_limit == kGraphicsLargeScrollLimit;
            require(
                scrolled == end_limit && (compact_end || regular_end || large_end),
                "End did not scroll Graphics to its end (" + std::to_string(scrolled) + " of " +
                    std::to_string(end_limit) + ")" + on
            );
            shows_dialog("Graphics at its end");
            snapshot("menu-dialog-graphics-end-" + size);
            tap(SDLK_HOME);
        }
        // Updates, on the three windows the ticket names. The mode is chosen
        // through its drop-down and read back after OK. Never then hides
        // CHECK NOW and starts nothing. CHECK NOW is pressed once the mode
        // is Automatically again, against a feed this check names, and the
        // dialog is left open for the Escape below. The preferences, the
        // feed and the settings are put back on every path, including a
        // failure.
        if ((width == 640 && height == 480) || (width == 1280 && height == 720) ||
            (width == 1920 && height == 1080)) {
            struct RestoreUpdates {
                Runtime& runtime;
                oa::platform::preferences::Values values;
                std::optional<std::string> feed;
                settings::EngineSettings settings;

                ~RestoreUpdates() {
                    runtime.preference_values_ = values;
                    runtime.preferences_dirty_ = true;
                    try {
                        runtime.flush_preferences();
                    } catch (const std::exception& failure) {
                        std::cerr << "engine settings check: Updates could not restore "
                                     "preferences: "
                                  << failure.what() << '\n';
                    }
                    runtime.options_.update_feed = std::move(feed);
                    runtime.apply_engine_settings(settings);
                    runtime.cancel_update_check();
                }
            } restore{*this, preference_values_, options_.update_feed, engine_settings()};

            const auto click_updates =
                [&](int32_t control, std::string_view text, std::string_view what) {
                    const auto parts_now = settings::dialog_layout(*engine_settings_dialog());
                    click_part(find_part(parts_now, control, text), what);
                };
            const oa::ui::kit::DisplayList named =
                settings::geometry::dialog_list(*dialog, nullptr);
            const int32_t updates_nav = oa::ui::kit::control_named(named, "settings.nav.updates");
            require(
                updates_nav == settings::page_control(settings::Page::updates),
                "oa.settings.nav.updates is not Updates' entry" + on
            );
            click_updates(updates_nav, {}, "oa.settings.nav.updates");
            require(
                engine_settings_dialog() != nullptr &&
                    engine_settings_dialog()->page == settings::Page::updates,
                "oa.settings.nav.updates did not show Updates" + on
            );
            snapshot("settings-updates-" + size);
            std::cout << "engine settings check: Updates (oa.settings.nav.updates) on the " << size
                      << " window\n";

            const int32_t mode_row = settings::first_row_control + 1;
            click_updates(mode_row, {}, "Check for updates");
            require(
                engine_settings_dialog()->open_list == mode_row,
                "Check for updates did not open" + on
            );
            click_updates(settings::no_control, "Only when I ask", "Only when I ask");
            require(
                engine_settings_dialog()->chosen.update_check ==
                        settings::UpdateCheck::when_asked &&
                    engine_settings_dialog()->open_list == settings::no_control,
                "Only when I ask was not chosen" + on
            );
            click_updates(settings::ok_control, "OK", "OK");
            require(engine_settings_dialog() == nullptr, "OK did not close Updates" + on);
            {
                const auto stored =
                    preference_values_.find(std::string(settings::key::update_check));
                require(
                    stored != preference_values_.end() && stored->second == "when-asked",
                    "OK did not store Check for updates as when-asked" + on
                );
            }
            std::cout << "engine settings check: Updates stored Only when I ask on the " << size
                      << " window\n";

            preference_values_ = restore.values;
            preferences_dirty_ = true;
            flush_preferences();
            apply_engine_settings(restore.settings);
            preference_values_[std::string(oa::ui::first_run::preference_key)] =
                std::string(oa::ui::first_run::preference_done);
            options_.update_feed = "http://127.0.0.1:9/v1/engine/stable.json";

            tap(SDLK_COMMA, kShortcutModifier);
            require(engine_settings_dialog() != nullptr, "Updates did not open again" + on);
            click_updates(
                settings::page_control(settings::Page::updates), {}, "oa.settings.nav.updates"
            );
            require(
                engine_settings_dialog() != nullptr &&
                    engine_settings_dialog()->page == settings::Page::updates,
                "Updates did not show again" + on
            );
            // The previous window's CHECK NOW may still be finishing. Let it
            // leave checking before Never is judged, so its result is not a
            // request Never made.
            cancel_update_check();
            const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < drain_deadline) {
                const oa::app::update::CheckState state = update_snapshot().state;
                tick_update_check();
                if (update_snapshot().state == state &&
                    state != oa::app::update::CheckState::checking)
                    break;
                SDL_Delay(10);
            }
            click_updates(mode_row, {}, "Check for updates");
            require(
                engine_settings_dialog()->open_list == mode_row,
                "Check for updates did not open again" + on
            );
            click_updates(settings::no_control, "Never", "Never");
            require(
                engine_settings_dialog()->chosen.update_check == settings::UpdateCheck::never &&
                    engine_settings_dialog()->open_list == settings::no_control,
                "Never was not chosen" + on
            );
            {
                const auto never_parts = settings::dialog_layout(*engine_settings_dialog());
                require(
                    !shows_text(never_parts, "CHECK NOW") &&
                        !shows_text(never_parts, "UPDATE\u2026"),
                    "CHECK NOW was shown with Check for updates at Never" + on
                );
            }
            const uint64_t generation_never = update_snapshot().generation;
            tick_update_check();
            require(
                update_snapshot().generation == generation_never &&
                    update_snapshot().state != oa::app::update::CheckState::checking,
                "Never started a check" + on
            );
            std::cout << "engine settings check: Updates Never made no request on the " << size
                      << " window\n";
            click_updates(mode_row, {}, "Check for updates");
            require(
                engine_settings_dialog()->open_list == mode_row,
                "Check for updates did not open after Never" + on
            );
            click_updates(settings::no_control, "Automatically", "Automatically");
            require(
                engine_settings_dialog()->chosen.update_check ==
                        settings::UpdateCheck::automatically &&
                    engine_settings_dialog()->open_list == settings::no_control,
                "Automatically was not chosen after Never" + on
            );

            const uint64_t generation = update_snapshot().generation;
            // The button is the status row's control, not a caption with no control.
            click_updates(settings::first_row_control, "CHECK NOW", "CHECK NOW");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            bool started = false;
            while (std::chrono::steady_clock::now() < deadline) {
                tick_update_check();
                const oa::app::update::CheckSnapshot& snap = update_snapshot();
                if (snap.state != oa::app::update::CheckState::idle ||
                    snap.generation != generation) {
                    started = true;
                    break;
                }
                SDL_Delay(10);
            }
            require(started, "CHECK NOW did not start a check" + on);
            std::cout << "engine settings check: Updates CHECK NOW started on the " << size
                      << " window\n";

            // Accessibility, on the three windows the ticket names. Each row
            // is changed through its named control and applies at once. OK
            // writes the preferences, which are read back and then put back,
            // with the settings, on every path. CHOOSE… opens the key editor
            // on Latchable over the dialog, and Escape leaves the dialog open.
            struct RestoreAccess {
                Runtime& runtime;
                oa::platform::preferences::Values values;
                settings::EngineSettings settings;

                ~RestoreAccess() {
                    runtime.preference_values_ = values;
                    runtime.preferences_dirty_ = true;
                    try {
                        runtime.flush_preferences();
                    } catch (const std::exception& failure) {
                        std::cerr << "engine settings check: Accessibility could not restore "
                                     "preferences: "
                                  << failure.what() << '\n';
                    }
                    runtime.apply_engine_settings(settings);
                }
            } restore_access{*this, preference_values_, engine_settings()};

            const auto named_control = [&](std::string_view name) {
                auto* open = engine_settings_dialog();
                require(open != nullptr, std::string(name) + " was asked with no dialog" + on);
                const oa::ui::kit::DisplayList list =
                    settings::geometry::dialog_list(*open, nullptr);
                const int32_t control = oa::ui::kit::control_named(list, name);
                require(
                    control != oa::ui::kit::no_control, std::string(name) + " is not shown" + on
                );
                return control;
            };
            const auto focus_access = [&](int32_t control, std::string_view what) {
                const auto order =
                    settings::page_settings(settings::Page::accessibility).size() +
                    static_cast<std::size_t>(settings::ok_control - settings::restore_control + 1) +
                    settings::page_count;
                for (std::size_t presses = 0; presses <= order; ++presses) {
                    if (engine_settings_dialog()->focused == control)
                        return;
                    tap(SDLK_TAB);
                }
                require(false, "Tab never reached " + std::string(what) + on);
            };
            const auto require_now = [&](bool ok, std::string_view what) {
                require(
                    ok && engine_settings_dialog() != nullptr &&
                        same_settings(engine_settings(), engine_settings_dialog()->chosen),
                    std::string(what) + " did not apply at once" + on
                );
            };

            const int32_t access_nav = named_control("settings.nav.accessibility");
            require(
                access_nav == settings::page_control(settings::Page::accessibility),
                "oa.settings.nav.accessibility is not Accessibility's entry" + on
            );
            click_updates(access_nav, {}, "oa.settings.nav.accessibility");
            require(
                engine_settings_dialog() != nullptr &&
                    engine_settings_dialog()->page == settings::Page::accessibility,
                "oa.settings.nav.accessibility did not show Accessibility" + on
            );
            snapshot("settings-accessibility-" + size);
            std::cout << "engine settings check: Accessibility (oa.settings.nav.accessibility) on "
                         "the "
                      << size << " window\n";

            const int32_t sticky = named_control("settings.sticky-modifiers");
            focus_access(sticky, "Sticky modifiers");
            click_updates(sticky, "ON", "Sticky modifiers' On");
            require_now(engine_settings_dialog()->chosen.sticky_modifiers, "Sticky modifiers On");
            const int32_t hold = named_control("settings.hold-to-confirm");
            focus_access(hold, "Hold to confirm");
            click_updates(hold, "ON", "Hold to confirm's On");
            require_now(engine_settings_dialog()->chosen.hold_to_confirm, "Hold to confirm On");
            const int32_t long_press = named_control("settings.long-press");
            focus_access(long_press, "Long press");
            tap(SDLK_RIGHT);
            require_now(engine_settings_dialog()->chosen.long_press_ms == 550, "Long press 550 ms");
            const int32_t slow = named_control("settings.slow-keys");
            focus_access(slow, "Slow keys");
            tap(SDLK_RIGHT);
            require_now(engine_settings_dialog()->chosen.slow_keys_ms == 50, "Slow keys 50 ms");
            const int32_t bounce = named_control("settings.bounce-keys");
            focus_access(bounce, "Bounce keys");
            tap(SDLK_RIGHT);
            require_now(engine_settings_dialog()->chosen.bounce_keys_ms == 50, "Bounce keys 50 ms");
            const int32_t wheel = named_control("settings.order-wheel");
            focus_access(wheel, "Order wheel");
            click_updates(wheel, "Hold right", "Order wheel's Hold right");
            require_now(
                engine_settings_dialog()->chosen.order_wheel == settings::OrderWheel::hold_right,
                "Order wheel Hold right"
            );
            const int32_t pointer = named_control("settings.pointer-keys");
            focus_access(pointer, "Pointer keys");
            click_updates(pointer, "ON", "Pointer keys' On");
            require_now(engine_settings_dialog()->chosen.pointer_keys, "Pointer keys On");
            const int32_t doublet = named_control("settings.double-press");
            focus_access(doublet, "Double press");
            tap(SDLK_RIGHT);
            require_now(
                engine_settings_dialog()->chosen.double_press_ms == 200, "Double press 200 ms"
            );
            const int32_t repeat_delay = named_control("settings.repeat-delay");
            focus_access(repeat_delay, "Key repeat delay");
            tap(SDLK_RIGHT);
            require_now(
                engine_settings_dialog()->chosen.repeat_delay_ms == 150, "Key repeat delay 150 ms"
            );
            const int32_t repeat_rate = named_control("settings.repeat-rate");
            focus_access(repeat_rate, "Key repeat rate");
            tap(SDLK_RIGHT);
            require_now(engine_settings_dialog()->chosen.repeat_rate == 2, "Key repeat rate 2");
            const int32_t drag = named_control("settings.drag-threshold");
            focus_access(drag, "Drag threshold");
            tap(SDLK_RIGHT);
            require_now(
                engine_settings_dialog()->chosen.drag_threshold == 40, "Drag threshold 40 px"
            );
            const int32_t edge = named_control("settings.edge-scroll");
            focus_access(edge, "Edge scrolling");
            click_updates(edge, "OFF", "Edge scrolling's Off");
            require_now(!engine_settings_dialog()->chosen.edge_scroll, "Edge scrolling Off");
            std::cout << "engine settings check: Accessibility changed each row on the " << size
                      << " window\n";

            click_updates(settings::ok_control, "OK", "OK");
            require(engine_settings_dialog() == nullptr, "OK did not close Accessibility" + on);
            const auto stored_as = [&](std::string_view key, std::string_view text) {
                const auto found = preference_values_.find(std::string(key));
                require(
                    found != preference_values_.end() && found->second == text,
                    "OK did not store " + std::string(key) + " as " + std::string(text) + on
                );
            };
            stored_as(settings::key::sticky_modifiers, "1");
            stored_as(settings::key::hold_to_confirm, "1");
            stored_as(settings::key::access_long_press, "550");
            stored_as(settings::key::access_slow_keys, "50");
            stored_as(settings::key::access_bounce_keys, "50");
            stored_as(settings::key::order_wheel, "hold-right");
            stored_as(settings::key::pointer_keys, "1");
            stored_as(settings::key::access_double_press, "200");
            stored_as(settings::key::access_repeat_delay, "150");
            stored_as(settings::key::access_repeat_rate, "2");
            stored_as(settings::key::access_drag_threshold, "40");
            stored_as(settings::key::edge_scroll, "0");
            std::cout << "engine settings check: Accessibility stored each row on the " << size
                      << " window\n";

            preference_values_ = restore_access.values;
            preferences_dirty_ = true;
            flush_preferences();
            apply_engine_settings(restore_access.settings);
            tap(SDLK_COMMA, kShortcutModifier);
            require(engine_settings_dialog() != nullptr, "Accessibility did not open again" + on);
            click_updates(
                settings::page_control(settings::Page::accessibility),
                {},
                "oa.settings.nav.accessibility"
            );
            require(
                engine_settings_dialog() != nullptr &&
                    engine_settings_dialog()->page == settings::Page::accessibility,
                "Accessibility did not show again" + on
            );
            const int32_t choose = named_control("settings.latching.choose");
            focus_access(choose, "CHOOSE…");
            click_updates(choose, "CHOOSE\u2026", "oa.settings.latching.choose");
            require(
                oa_layer().find("key-editor") != nullptr,
                "oa.settings.latching.choose did not open the key editor" + on
            );
            bool latchable_on = false;
            {
                std::vector<AutomationControl> controls;
                oa_layer().automation_controls(controls);
                for (const AutomationControl& control : controls) {
                    if (control.name == "oa.key-editor.filter.latchable" && control.visible &&
                        control.checked)
                        latchable_on = true;
                }
            }
            require(latchable_on, "the key editor did not open on its Latchable filter" + on);
            std::cout << "engine settings check: Accessibility CHOOSE\u2026 opened the key editor "
                         "on Latchable on the "
                      << size << " window\n";
            tap(SDLK_ESCAPE);
            require(
                oa_layer().find("key-editor") == nullptr && engine_settings_dialog() != nullptr,
                "Escape did not leave Settings under the key editor" + on
            );
        }
        tap(SDLK_ESCAPE);
        require(engine_settings_dialog() == nullptr, "Escape did not close the dialog" + on);
        std::cout << "engine settings check: the dialog's sections at "
                  << size_class_name(expected_layout.size_class) << ", " << expected_layout.scale
                  << "x, on the " << size << " window\n";
        if (std::pair<int, int>{width, height} != kInterfaceSizeWindow)
            continue;

        // Clicks the part of the open dialog that a control with a text draws.
        const auto click_control =
            [&](int32_t control, std::string_view text, std::string_view what) {
                const auto parts_now = settings::dialog_layout(*engine_settings_dialog());
                click_part(find_part(parts_now, control, text), what);
            };
        // Interface size, chosen through its drop-down on User Interface: it
        // takes effect as it is chosen, the dialog staying open at the class
        // and scale it gives. OK stores it, and the dialog opens again drawn
        // as the dialog drawn at that class and scale.
        for (const InterfaceSizeStep& size_step : kInterfaceSizeSteps) {
            const std::string chosen_size = "Interface size " + std::string(size_step.caption) + on;
            // The shortcut opens the dialog; the OA button opens the Library.
            tap(SDLK_COMMA, kShortcutModifier);
            auto* sized = engine_settings_dialog();
            require(sized != nullptr, "the shortcut did not open the dialog" + on);
            click_control(
                settings::page_control(settings::Page::user_interface), {}, "User Interface's entry"
            );
            require(
                sized->page == settings::Page::user_interface,
                "a click on User Interface's entry did not show it" + on
            );
            tap(SDLK_END);
            const int32_t row_control = settings::first_row_control + kInterfaceSizeRow;
            click_control(row_control, {}, "Interface size's field");
            require(
                sized->open_list == row_control,
                "a click on Interface size's field did not open its list" + on
            );
            click_control(
                settings::no_control,
                size_step.caption,
                "Interface size's " + std::string(size_step.caption)
            );
            require(
                sized->chosen.interface_size == size_step.size &&
                    sized->open_list == settings::no_control,
                "a click on " + chosen_size + " did not choose it"
            );
            // As it is chosen, before OK: the dialog redraws at the class and
            // scale the size gives, and stays open.
            require(
                engine_settings_dialog() == sized &&
                    oa_layer().screen_class() == size_step.size_class &&
                    oa_layer().screen_scale() == size_step.scale &&
                    sized->size_class == size_step.size_class,
                chosen_size + " did not take effect as it was chosen"
            );
            click_control(settings::ok_control, "OK", "OK");
            require(
                engine_settings_dialog() == nullptr &&
                    engine_settings().interface_size == size_step.size,
                "OK did not keep " + chosen_size
            );
            {
                const auto stored =
                    preference_values_.find(std::string(settings::key::interface_size));
                require(
                    stored != preference_values_.end() &&
                        stored->second == settings::interface_size_text(size_step.size),
                    "OK did not store " + chosen_size
                );
            }
            // At once after OK: the dialog opens again at the size's class
            // and scale, centred, and the window shows it so.
            tap(SDLK_COMMA, kShortcutModifier);
            const auto* reopened = engine_settings_dialog();
            require(reopened != nullptr, "the dialog did not open again" + on);
            point(SDL_EVENT_MOUSE_MOTION, kRestingPointer);
            require(
                oa_layer().screen_class() == size_step.size_class &&
                    oa_layer().screen_scale() == size_step.scale &&
                    reopened->size_class == size_step.size_class,
                chosen_size + " is not " + std::string(size_class_name(size_step.size_class)) +
                    ", " + std::to_string(size_step.scale) + "x"
            );
            {
                const auto& step_metrics = oa::ui::kit::metrics_of(size_step.size_class);
                const LayerPlacement at = placed();
                const int32_t shown_width = step_metrics.dialog_width * size_step.scale;
                const int32_t shown_height = step_metrics.dialog_height * size_step.scale;
                require(
                    at.shown.width == shown_width && at.shown.height == shown_height &&
                        at.shown.x == (width - shown_width) / 2 &&
                        at.shown.y == (height - shown_height) / 2,
                    chosen_size + " does not centre the dialog at its class's size"
                );
            }
            shows_dialog_at("the dialog at " + chosen_size, size_step.scale * 100);
            snapshot(
                "menu-dialog-interface-size-" +
                std::string(settings::interface_size_text(size_step.size)) + '-' + size
            );
            tap(SDLK_ESCAPE);
            require(engine_settings_dialog() == nullptr, "Escape did not close the dialog" + on);
            if (size_step.size != settings::InterfaceSize::automatic)
                std::cout << "engine settings check: Interface size " << size_step.caption
                          << " on the " << size << " window is "
                          << size_class_name(size_step.size_class) << ", " << size_step.scale
                          << "x\n";
        }

        // Choosing a size redraws at once and leaves the dialog on User
        // Interface, its scroll as near as the new size allows, and the focus
        // on Interface size. Escape puts back the size the dialog opened with.
        // The preference stays as it was until OK.
        {
            const std::string chosen_now = "Interface size 100%" + on;
            tap(SDLK_COMMA, kShortcutModifier);
            auto* sized = engine_settings_dialog();
            require(sized != nullptr, "the shortcut did not open the dialog" + on);
            click_control(
                settings::page_control(settings::Page::user_interface), {}, "User Interface's entry"
            );
            const int32_t size_row = settings::first_row_control + kInterfaceSizeRow;
            // Tab walks the section's rows to Interface size.
            for (int press = 0; press < 64 && sized->focused != size_row; ++press)
                tap(SDLK_TAB);
            require(sized->focused == size_row, "Interface size's row did not take the focus" + on);
            const auto scroll_index = static_cast<std::size_t>(settings::Page::user_interface);
            const int32_t scroll_before = sized->scroll[scroll_index];
            const auto stored_before =
                preference_values_.find(std::string(settings::key::interface_size));
            const std::string preference_before =
                stored_before == preference_values_.end() ? std::string{} : stored_before->second;
            tap(SDLK_RIGHT);
            const int32_t limit_now = settings::scroll_limit(*sized);
            const int32_t shown_scroll =
                std::clamp(sized->scroll[scroll_index], int32_t{0}, limit_now);
            const auto stored_now =
                preference_values_.find(std::string(settings::key::interface_size));
            const std::string preference_now =
                stored_now == preference_values_.end() ? std::string{} : stored_now->second;
            require(
                engine_settings_dialog() == sized &&
                    sized->page == settings::Page::user_interface && sized->focused == size_row &&
                    sized->chosen.interface_size == settings::InterfaceSize::size_100 &&
                    engine_settings().interface_size == settings::InterfaceSize::size_100 &&
                    oa_layer().screen_class() == oa::ui::kit::SizeClass::large &&
                    oa_layer().screen_scale() == 1 &&
                    sized->size_class == oa::ui::kit::SizeClass::large &&
                    (sized->scroll[scroll_index] == scroll_before ||
                     shown_scroll == std::min(scroll_before, limit_now)) &&
                    preference_now == preference_before,
                chosen_now + " did not take effect at once on its row"
            );
            tap(SDLK_ESCAPE);
            const auto stored_after =
                preference_values_.find(std::string(settings::key::interface_size));
            const std::string preference_after =
                stored_after == preference_values_.end() ? std::string{} : stored_after->second;
            require(
                engine_settings_dialog() == nullptr &&
                    engine_settings().interface_size == settings::InterfaceSize::automatic &&
                    oa_layer().screen_class() == expected_layout.size_class &&
                    oa_layer().screen_scale() == expected_layout.scale &&
                    preference_after == preference_before,
                "Escape did not put Interface size back" + on
            );
            std::cout << "engine settings check: Interface size 100% on the " << size
                      << " window takes effect as it is chosen, and Escape puts Auto back\n";
        }

        // 150%, which the drop-down offers only with Use modern fonts for
        // game text On: they are turned On through the dialog, 150% chosen
        // and OK pressed. The dialog opens again at Large, laid out for the
        // points at 1.5x and drawn on the kit canvas's smooth form, and the
        // window shows it as the dialog drawn offscreen there. Turning the
        // modern fonts Off shows 100% again, which OK puts in effect at
        // Large, 1x; then Auto goes back.
        const std::string fractional = "Interface size 150%" + on;
        const int32_t size_row = settings::first_row_control + kInterfaceSizeRow;
        // Opens Interface size's drop-down on User Interface, at its end,
        // where the field shows whole.
        const auto open_sizes = [&](settings::Dialog& shown_dialog) {
            click_control(
                settings::page_control(settings::Page::user_interface), {}, "User Interface's entry"
            );
            tap(SDLK_END);
            click_control(size_row, {}, "Interface size's field");
            require(
                shown_dialog.open_list == size_row,
                "a click on Interface size's field did not open its list" + on
            );
        };
        require(
            !engine_settings().modern_fonts &&
                engine_settings().interface_size == settings::InterfaceSize::automatic,
            "the modern fonts are not Off and Auto not in effect before 150% is chosen" + on
        );
        // The shortcut opens the dialog; the OA button opens the Library.
        tap(SDLK_COMMA, kShortcutModifier);
        auto* fraction_dialog = engine_settings_dialog();
        require(fraction_dialog != nullptr, "the shortcut did not open the dialog" + on);
        // On the main menu MORE IN THE LIBRARY… stands right under
        // Language's row, and the rows under it come one control later.
        const int32_t fonts_row =
            settings::first_row_control + 1 + (fraction_dialog->library_link ? 1 : 0);
        open_sizes(*fraction_dialog);
        require(
            !shows_text(settings::dialog_layout(*fraction_dialog), "150%"),
            "Interface size offers 150% while the modern fonts are Off" + on
        );
        tap(SDLK_ESCAPE);
        require(
            engine_settings_dialog() == fraction_dialog &&
                fraction_dialog->open_list == settings::no_control,
            "Escape did not close Interface size's list" + on
        );
        click_control(settings::page_control(settings::Page::language), {}, "Language's entry");
        click_control(fonts_row, "ON", "Use modern fonts for game text's On");
        require(
            fraction_dialog->chosen.modern_fonts,
            "a click on Use modern fonts for game text's On did not turn them on" + on
        );
        // Auto is still the size in effect. On this 1920x1080 window, with
        // the modern fonts on, it draws Large at 150%.
        require(
            engine_settings().interface_size == settings::InterfaceSize::automatic &&
                oa_layer().screen_class() == oa::ui::kit::SizeClass::large &&
                oa_layer().screen_scale_percent() == oa::ui::kit::fractional_scale_percent &&
                fraction_dialog->size_class == oa::ui::kit::SizeClass::large,
            "Auto with the modern fonts On is not Large at 150%" + on
        );
        {
            // Large's 960 by 600 points at 1.5x: 1440 by 900 pixels, centred.
            const LayerPlacement at = placed();
            require(
                at.shown.x == 240 && at.shown.y == 90 && at.shown.width == 1440 &&
                    at.shown.height == 900,
                "Auto with the modern fonts On does not show the dialog at 1440x900 at (240, 90)" +
                    on
            );
        }
        std::cout << "engine settings check: Interface size Auto with the modern fonts On on the "
                  << size << " window is Large, 1.5x\n";
        open_sizes(*fraction_dialog);
        click_control(settings::no_control, "150%", "Interface size's 150%");
        require(
            fraction_dialog->chosen.interface_size == settings::InterfaceSize::size_150 &&
                fraction_dialog->open_list == settings::no_control,
            "a click on " + fractional + " did not choose it"
        );
        require(
            engine_settings_dialog() == fraction_dialog &&
                oa_layer().screen_class() == oa::ui::kit::SizeClass::large &&
                oa_layer().screen_scale_percent() == oa::ui::kit::fractional_scale_percent &&
                fraction_dialog->size_class == oa::ui::kit::SizeClass::large,
            fractional + " did not take effect as it was chosen"
        );
        click_control(settings::ok_control, "OK", "OK");
        require(
            engine_settings_dialog() == nullptr &&
                engine_settings().interface_size == settings::InterfaceSize::size_150 &&
                engine_settings().modern_fonts,
            "OK did not keep " + fractional + " with the modern fonts On"
        );
        {
            const auto stored = preference_values_.find(std::string(settings::key::interface_size));
            require(
                stored != preference_values_.end() && stored->second == "150",
                "OK did not store " + fractional
            );
        }
        tap(SDLK_COMMA, kShortcutModifier);
        const auto* fraction_reopened = engine_settings_dialog();
        require(fraction_reopened != nullptr, "the dialog did not open again" + on);
        point(SDL_EVENT_MOUSE_MOTION, kRestingPointer);
        require(
            oa_layer().screen_class() == oa::ui::kit::SizeClass::large &&
                oa_layer().screen_scale_percent() == oa::ui::kit::fractional_scale_percent &&
                fraction_reopened->size_class == oa::ui::kit::SizeClass::large,
            fractional + " is not Large, 1.5x"
        );
        {
            // Large's 960 by 600 points at 1.5x: 1440 by 900 pixels, centred.
            const LayerPlacement at = placed();
            require(
                at.points_width == oa::ui::kit::large_metrics.dialog_width &&
                    at.points_height == oa::ui::kit::large_metrics.dialog_height &&
                    at.shown.x == 240 && at.shown.y == 90 && at.shown.width == 1440 &&
                    at.shown.height == 900,
                fractional + " does not show the dialog at 1440x900 at (240, 90)"
            );
        }
        shows_dialog_at("the dialog at " + fractional, oa::ui::kit::fractional_scale_percent);
        snapshot("menu-dialog-interface-size-150-" + size);
        std::cout << "engine settings check: Interface size 150% on the " << size
                  << " window is Large, 1.5x\n";
        // The modern fonts turned Off take 150% back to 100%, shown at once.
        click_control(settings::page_control(settings::Page::language), {}, "Language's entry");
        click_control(fonts_row, "OFF", "Use modern fonts for game text's Off");
        require(
            !fraction_reopened->chosen.modern_fonts &&
                fraction_reopened->chosen.interface_size == settings::InterfaceSize::size_100,
            "turning the modern fonts Off did not show Interface size 100%" + on
        );
        click_control(
            settings::page_control(settings::Page::user_interface), {}, "User Interface's entry"
        );
        tap(SDLK_END);
        {
            const auto parts_now = settings::dialog_layout(*fraction_reopened);
            require(
                find_part(parts_now, size_row, "100%") != nullptr,
                "Interface size does not show 100% with the modern fonts Off" + on
            );
        }
        click_control(settings::ok_control, "OK", "OK");
        require(
            engine_settings_dialog() == nullptr &&
                engine_settings().interface_size == settings::InterfaceSize::size_100 &&
                !engine_settings().modern_fonts,
            "OK did not keep 100% with the modern fonts Off" + on
        );
        tap(SDLK_COMMA, kShortcutModifier);
        require(engine_settings_dialog() != nullptr, "the dialog did not open again" + on);
        point(SDL_EVENT_MOUSE_MOTION, kRestingPointer);
        require(
            oa_layer().screen_class() == oa::ui::kit::SizeClass::large &&
                oa_layer().screen_scale_percent() == 100 &&
                engine_settings_dialog()->size_class == oa::ui::kit::SizeClass::large,
            "Interface size 100% with the modern fonts Off is not Large, 1x" + on
        );
        // Auto goes back.
        open_sizes(*engine_settings_dialog());
        click_control(settings::no_control, "Auto", "Interface size's Auto");
        click_control(settings::ok_control, "OK", "OK");
        {
            const auto stored = preference_values_.find(std::string(settings::key::interface_size));
            require(
                engine_settings_dialog() == nullptr &&
                    engine_settings().interface_size == settings::InterfaceSize::automatic &&
                    stored != preference_values_.end() && stored->second == "automatic" &&
                    oa_layer().screen_class() == expected_layout.size_class &&
                    oa_layer().screen_scale() == expected_layout.scale,
                "Interface size Auto did not go back" + on
            );
        }
    }
    check_window_edge_rows_fit();
    check_file_type_rows_fit();
    if (!SDL_SetWindowSize(sdl_.window, kDefaultWindowWidth, kDefaultWindowHeight) ||
        !SDL_SyncWindow(sdl_.window))
        throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
    load(Screen::main_menu);
    oa_layer().forget_latched_key();
    fake_frontend_tick_ = previous_tick;
}

void Runtime::check_window_edge_rows_fit() {
    // Window edge's strip and Edge stickiness's slider on the 640x480
    // window, Compact, in English and German, with each way of the strip:
    // no text of theirs is wider than its box or drawn across another.
    // Simplified Chinese draws from its pack, and --check-text-fit shows
    // every section in it.
    if (!SDL_SetWindowSize(sdl_.window, 640, 480) || !SDL_SyncWindow(sdl_.window))
        throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
    load(Screen::main_menu);
    require(screen_ == Screen::main_menu, "the main menu did not open on the 640x480 window");
    const std::string kept_language = language_state().choice;
    constexpr std::array<std::string_view, 10> words{
        "Window edge",
        "Capture",
        "Sticky",
        "Free",
        "The pointer stays in the window while you play.",
        "The pointer rests on the edge so the view scrolls.",
        "The pointer leaves a window's edge, as before.",
        "Edge stickiness",
        "How firmly Sticky holds the pointer at the edge.",
        "Needs Sticky",
    };
    for (const std::string_view tag : {std::string_view{"en"}, std::string_view{"de"}}) {
        set_language_choice(tag);
        require(
            shown_language().tag == tag,
            "the check could not show the settings in " + std::string(tag)
        );
        for (const auto way : settings::window_edge_choices) {
            open_engine_settings_from_menu();
            settings::Dialog* dialog = engine_settings_dialog();
            require(dialog != nullptr, "Settings did not open on the 640x480 window");
            dialog->page = settings::Page::controls;
            dialog->chosen.window_edge = way;
            dialog->scroll[static_cast<std::size_t>(settings::Page::controls)] =
                std::numeric_limits<int32_t>::max();
            const std::string screen = std::string("settings/controls/") +
                                       (way == settings::WindowEdge::capture  ? "capture"
                                        : way == settings::WindowEdge::sticky ? "sticky"
                                                                              : "free");
            oa::ui::kit::set_text_fit_screen(screen);
            bool running = true;
            // The first frame settles the size class; the second is read.
            for (int frame = 0; frame < 2; ++frame) {
                oa::ui::kit::begin_text_fit_frame();
                run_frame(running);
                require(running, "a frame ended the run");
            }
            for (const oa::ui::kit::TextFitRecord& noted : oa::ui::kit::text_fit_records()) {
                if ((!noted.wider && !noted.across) || noted.text.empty())
                    continue;
                for (const std::string_view word : words) {
                    const std::string shown{oa::data::languages::interface_text(word)};
                    require(
                        shown.find(noted.text) == std::string::npos,
                        screen + " in " + std::string(tag) + ": '" + noted.text + "' is " +
                            (noted.wider ? "wider than its box" : "drawn across other text")
                    );
                }
            }
            take_settings_screen_action(settings::DialogAction::cancelled);
            require(engine_settings_dialog() == nullptr, "Cancel did not close Settings");
        }
        std::cout << "engine settings check: Window edge and Edge stickiness fit the 640x480 "
                     "dialog in "
                  << tag << '\n';
    }
    set_language_choice(kept_language);
}

void Runtime::check_file_type_rows_fit() {
    // System's File Type Registration and its eight switches, the launch
    // file's last, on the 640x480 window, Compact, in English and German, at
    // the section's top and at its end: no text of theirs is wider than its
    // box or drawn across another. Simplified Chinese draws from its pack,
    // and --check-text-fit shows every section in it.
    if (!settings::file_types_switchable)
        return;
    if (!SDL_SetWindowSize(sdl_.window, 640, 480) || !SDL_SyncWindow(sdl_.window))
        throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
    load(Screen::main_menu);
    require(screen_ == Screen::main_menu, "the main menu did not open on the 640x480 window");
    const std::string kept_language = language_state().choice;
    constexpr std::array<std::string_view, 10> words{
        "File Type Registration",
        "Open these files with the game from your file manager.",
        "Mod packages (.oamod)",
        "Map packs (.oamap)",
        "Language packs (.oalang)",
        "Registry files (.oareg)",
        "Mutator packages (.oamut)",
        "Brain packages (.oabrain)",
        "Key layout packages (.oakeys)",
        "Launch files (.oalaunch)",
    };
    for (const std::string_view tag : {std::string_view{"en"}, std::string_view{"de"}}) {
        set_language_choice(tag);
        require(
            shown_language().tag == tag,
            "the check could not show the settings in " + std::string(tag)
        );
        bool launch_shown = false;
        for (const int32_t scroll : {int32_t{0}, std::numeric_limits<int32_t>::max()}) {
            open_engine_settings_from_menu();
            settings::Dialog* dialog = engine_settings_dialog();
            require(dialog != nullptr, "Settings did not open on the 640x480 window");
            require(dialog->file_type_switches, "System does not list the file-type switches");
            dialog->page = settings::Page::system;
            dialog->scroll[static_cast<std::size_t>(settings::Page::system)] = scroll;
            const std::string screen =
                std::string("settings/system/") + (scroll == 0 ? "top" : "end");
            oa::ui::kit::set_text_fit_screen(screen);
            bool running = true;
            // The first frame settles the size class; the second is read.
            for (int frame = 0; frame < 2; ++frame) {
                oa::ui::kit::begin_text_fit_frame();
                run_frame(running);
                require(running, "a frame ended the run");
            }
            // The launch file's switch is among the rows drawn, in either
            // language's words.
            const std::string launch_label{
                oa::data::languages::interface_text("Launch files (.oalaunch)")
            };
            const auto parts = settings::dialog_layout(*dialog);
            for (const settings::LayoutPart& part : parts)
                if (part.text == launch_label || part.text == "Launch files (.oalaunch)")
                    launch_shown = true;
            for (const oa::ui::kit::TextFitRecord& noted : oa::ui::kit::text_fit_records()) {
                if ((!noted.wider && !noted.across) || noted.text.empty())
                    continue;
                for (const std::string_view word : words) {
                    const std::string shown{oa::data::languages::interface_text(word)};
                    require(
                        shown.find(noted.text) == std::string::npos,
                        screen + " in " + std::string(tag) + ": '" + noted.text + "' is " +
                            (noted.wider ? "wider than its box" : "drawn across other text")
                    );
                }
            }
            take_settings_screen_action(settings::DialogAction::cancelled);
            require(engine_settings_dialog() == nullptr, "Cancel did not close Settings");
        }
        require(
            launch_shown,
            "System at its end does not show Launch files (.oalaunch) in " + std::string(tag)
        );
        std::cout << "engine settings check: File Type Registration and its six switches fit "
                     "the 640x480 dialog in "
                  << tag << '\n';
    }
    set_language_choice(kept_language);
}

} // namespace oa::app
