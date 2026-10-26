// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// In-match HUD layout, pause, outcome and options menus.
#include "oa/data/campaign/campaign_file.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/ui/decoded.hpp"
#include "oa/sim/scenario/commander_rules.hpp"
#include "oa/sim/speed.hpp"
#include "oa/ui/frontend/end_mission.hpp"
#include "oa/ui/frontend/ingame_menu.hpp"
#include "oa/ui/frontend/options.hpp"
#include "oa/ui/frontend/savegame_dialogs.hpp"
#include "oa/formats/fnt.hpp"
#include "oa/present/blit.hpp"
#include "oa/present/game_text.hpp"
#include "oa/present/model/mesh_raster.hpp"
#include "oa/present/surface.hpp"
#include "oa/present/typed_text.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_renderer/game_text.hpp"
#include "oa/ui/hud/build_page_fit.hpp"
#include "oa/ui/gui_input/gadget_panel.hpp"
#include "oa/ui/gui_input.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/ui/screen_registry.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/console/hotkeys.hpp"
#include "oa/present/world_renderer/world_camera.hpp"
#include "oa/app/runtime.hpp"
#include "gui_actions.hpp"
#include "panel_first_draw.hpp"
#include "oa/base/text/line_break.hpp"
#include "oa/ui/hud/resource_bar.hpp"
#include "oa/app/view_rules.hpp"
#include "engine_settings_state.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
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

namespace ui = oa::ui::frontend;
using StageMap = std::map<std::string, std::size_t>;

enum class IngamePanel : uint8_t {
    options,
    exit_menu,
    exit_confirm,
    restart,
    game_settings,
    preferences, // PREFS.GUI in the side column, its open tab's sub-panel beside it
};

// The preferences a match opens: PREFS.GUI, and each tab's sub-panel merged
// into it.
constexpr const char* kPreferencesLayout = "PREFS.GUI";

/// Returns the question a profile asks before the player leaves the game.
///
/// @param profile the resolved profile; null plays 3.1c's
/// @return its strings.message.exit-confirm where that differs from 3.1c's,
///     else empty, which asks the engine's own question
std::string profile_leave_question(const oa::data::mod_profile::ModProfile* profile) {
    const char* question = view_rules::profile_texts(profile).leave_question;
    return question != nullptr ? question : "";
}

// Options/in-game menu state that outlives one click. The runtime
// shows one frontend screen at a time, so a single session suffices.
struct MatchMenuSession {
    ui::Panel panel; // the preferences' records are relative to PREFS.GUI's root
    ui::OptionsContext options;
    ui::IngameContext ingame;
    ui::OptionsPanel kind = ui::OptionsPanel::tabs;
    IngamePanel ingame_panel = IngamePanel::options;
    const void* options_bound = nullptr; // frontend layout the panel mirrors
    bool options_open = false;           // entry snapshot captured
    // The exit confirmation answers a request to close the window; CHOICE2
    // goes back to what was on screen before it.
    bool close_confirm = false;
    bool close_confirm_from_menu = false; // the in-game menu was open then
    // The lightbar steps once a frame: on the first draw after the options
    // open, then on the first draw after each frame's tick.
    bool lightbar_due = false;
    ui::OptionsLightbarStep lightbar_step{}; // the step the frame draws
    std::vector<uint8_t> lightbar_cover;     // HUD pixels the frame's lightbar covers
};

/// Returns the options and in-game menu state, created on first use.
///
/// @return the session
MatchMenuSession& match_menu_session() {
    static MatchMenuSession session;
    return session;
}

// ENDMSN.GUI: the panel handlers over the game options and the
// finished game's Game block.
struct EndMissionSession {
    ui::Panel panel;
    ui::EndMissionContext context;
    oa::data::campaign::CampaignEnv env{};
};

EndMissionSession& end_mission_session() {
    static auto* session = new EndMissionSession();
    return *session;
}

// Object state (Game.game_options) the in-game menus branch on.
ui::SessionKind ingame_session(bool campaign, bool multiplayer) {
    if (multiplayer)
        return ui::SessionKind::multiplayer;
    return campaign ? ui::SessionKind::campaign : ui::SessionKind::skirmish;
}

constexpr const char* kRestartLayout = "RESTART.GUI";
// RESTART.GUI's art is the top-left of this 640x480 bitmap.
constexpr const char* kRestartBackdrop = "bitmaps/drestart.pcx";
constexpr const char* kGameSettingsLayout = "GAMEOPTIONS.GUI";
constexpr const char* kGameSettingsBackdrop = "bitmaps/gamesettings.pcx";
// The Game Settings sheet centres every row it adds in its column (record attributes 2).
constexpr int32_t kSettingsRowAttributes = 2;
constexpr int16_t kSettingsRowHeight = 0xF;
constexpr uint16_t kSettingsRowColor = 0xF;

namespace panel_flag = oa::ui::gui_input::panel_flag;

/// Places a panel's root as the panel loader's first draw places it on the
/// 640x480 screen with the HUD strip, the records moving with the root.
///
/// @param[in,out] layout the loaded panel, its records placed on the frame
/// @param placement panel_flag::beside_hud or panel_flag::centre
void place_panel(oa::ui::gui_layout::Layout& layout, uint32_t placement) {
    if (layout.gadgets.empty())
        return;
    auto& root = layout.gadgets.front().common;
    int16_t x = root.x;
    int16_t y = root.y;
    oa::ui::gui_input::place_root(
        x,
        y,
        root.width,
        root.height,
        placement | panel_flag::first_draw,
        kCanvasWidth,
        kCanvasHeight,
        kBattlefieldLeft
    );
    const int dx = x - root.x;
    const int dy = y - root.y;
    for (auto& gadget : layout.gadgets) {
        if (&gadget.common != &root && (gadget.common.width <= 0 || gadget.common.height <= 0))
            continue;
        gadget.common.x = static_cast<int16_t>(gadget.common.x + dx);
        gadget.common.y = static_cast<int16_t>(gadget.common.y + dy);
    }
}

/// Returns the offset of a pixel in an RGB image.
///
/// @param image the image
/// @param x column
/// @param y row
/// @return the offset of its red byte
std::size_t rgb_offset(const renderer::Surface& image, int32_t x, int32_t y) {
    return (static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x)) * 3U;
}

/// Copies a rectangle of an RGB image into a new image of the rectangle's
/// size; pixels outside the source are left black.
///
/// @param source image copied from
/// @param rect rectangle copied
/// @return the copy
renderer::Surface
copy_area(const renderer::Surface& source, const oa::ui::display_layout::Rect& rect) {
    renderer::Surface area{
        static_cast<uint32_t>(std::max(rect.width, 0)),
        static_cast<uint32_t>(std::max(rect.height, 0)),
        {}
    };
    area.rgb.assign(static_cast<std::size_t>(area.width) * area.height * 3U, 0);
    for (int32_t row = 0; row < static_cast<int32_t>(area.height); ++row)
        for (int32_t column = 0; column < static_cast<int32_t>(area.width); ++column) {
            const int32_t x = rect.x + column;
            const int32_t y = rect.y + row;
            if (x < 0 || y < 0 || x >= static_cast<int32_t>(source.width) ||
                y >= static_cast<int32_t>(source.height) ||
                rgb_offset(source, x, y) + 2 >= source.rgb.size())
                continue;
            std::copy_n(
                source.rgb.begin() + static_cast<std::ptrdiff_t>(rgb_offset(source, x, y)),
                3,
                area.rgb.begin() + static_cast<std::ptrdiff_t>(rgb_offset(area, column, row))
            );
        }
    return area;
}

/// Copies an image made by copy_area() back over an RGB image, from column
/// `first_column` of the target on.
///
/// @param[in,out] target image written
/// @param area the pixels
/// @param x target column of the area's left edge
/// @param y target row of the area's top edge
/// @param first_column first target column written
void paste_area(
    renderer::Surface& target,
    const renderer::Surface& area,
    int32_t x,
    int32_t y,
    int32_t first_column
) {
    for (int32_t row = 0; row < static_cast<int32_t>(area.height); ++row)
        for (int32_t column = 0; column < static_cast<int32_t>(area.width); ++column) {
            const int32_t to_x = x + column;
            const int32_t to_y = y + row;
            if (to_x < std::max(first_column, 0) || to_y < 0 ||
                to_x >= static_cast<int32_t>(target.width) ||
                to_y >= static_cast<int32_t>(target.height) ||
                rgb_offset(target, to_x, to_y) + 2 >= target.rgb.size() ||
                rgb_offset(area, column, row) + 2 >= area.rgb.size())
                continue;
            std::copy_n(
                area.rgb.begin() + static_cast<std::ptrdiff_t>(rgb_offset(area, column, row)),
                3,
                target.rgb.begin() + static_cast<std::ptrdiff_t>(rgb_offset(target, to_x, to_y))
            );
        }
}

} // namespace

void draw_back_tile(
    oa::Image& image,
    const oa::ui::gui_layout::CommonFields& root,
    const oa::formats::gaf::Sequence& tile,
    const oa::PaletteBytes& palette
) {
    namespace skin_frame = oa::ui::gui_layout::skin_frame;
    if (tile.frames.size() < skin_frame::count || root.width <= 0 || root.height <= 0)
        return;
    std::vector<oa::formats::gaf::RenderedFrame> frames;
    for (std::size_t index = 0; index < skin_frame::count; ++index) {
        auto rendered = oa::formats::gaf::render_normal(tile.frames[index]);
        if (!rendered.ok())
            return;
        frames.push_back(std::move(*rendered.frame));
    }
    const auto stamp = [&](const oa::formats::gaf::RenderedFrame& frame, int left, int top) {
        for (int row = 0; row < static_cast<int>(frame.height); ++row)
            for (int column = 0; column < static_cast<int>(frame.width); ++column) {
                const auto at =
                    static_cast<std::size_t>(row) * frame.width + static_cast<std::size_t>(column);
                const int x = left + column;
                const int y = top + row;
                if (at >= frame.coverage.size() || frame.coverage[at] == 0 || x < 0 || y < 0 ||
                    x >= root.width || y >= root.height)
                    continue;
                const int image_x = root.x + x;
                const int image_y = root.y + y;
                if (image_x < 0 || image_y < 0 || image_x >= static_cast<int>(image.width) ||
                    image_y >= static_cast<int>(image.height))
                    continue;
                const auto colour = static_cast<std::size_t>(frame.pixels[at]) * 4U;
                const auto to = (static_cast<std::size_t>(image_y) * image.width +
                                 static_cast<std::size_t>(image_x)) *
                                3U;
                if (colour + 2 >= palette.size() || to + 2 >= image.rgb.size())
                    continue;
                std::copy_n(
                    palette.begin() + static_cast<std::ptrdiff_t>(colour),
                    3,
                    image.rgb.begin() + static_cast<std::ptrdiff_t>(to)
                );
            }
    };
    const auto tiles = oa::ui::gui_layout::skin_tiles(
        root.width,
        root.height,
        static_cast<int32_t>(frames.front().width),
        static_cast<int32_t>(frames.front().height)
    );
    for (const auto& placed : tiles)
        stamp(frames[placed.frame], placed.x, placed.y);
}

namespace {

// Copies the top-left of `art` the size of the panel root onto the panel.
void draw_panel_backdrop(
    oa::Image& background, const oa::ui::gui_layout::Gadget& root, const oa::Image& art
) {
    const auto width = std::min<int>(root.common.width, static_cast<int>(art.width));
    const auto height = std::min<int>(root.common.height, static_cast<int>(art.height));
    for (int row = 0; row < height; ++row) {
        const int y = root.common.y + row;
        if (y < 0 || y >= static_cast<int>(background.height))
            continue;
        for (int column = 0; column < width; ++column) {
            const int x = root.common.x + column;
            if (x < 0 || x >= static_cast<int>(background.width))
                continue;
            const auto from =
                (static_cast<std::size_t>(row) * art.width + static_cast<std::size_t>(column)) * 3U;
            const auto to =
                (static_cast<std::size_t>(y) * background.width + static_cast<std::size_t>(x)) * 3U;
            if (from + 2 >= art.rgb.size() || to + 2 >= background.rgb.size())
                continue;
            std::copy_n(
                art.rgb.begin() + static_cast<std::ptrdiff_t>(from),
                3,
                background.rgb.begin() + static_cast<std::ptrdiff_t>(to)
            );
        }
    }
}

// What the restart callbacks share: the first failure ends the restart.
struct RestartRun {
    Runtime* runtime = nullptr;
    bool campaign = false;
    std::string error;
    uint16_t units_per_player = 0; // the unit limit the match was started with
};

template <typename Step>
void restart_do(void* context, Step step) noexcept {
    auto& run = *static_cast<RestartRun*>(context);
    if (!run.error.empty())
        return;
    try {
        step(run);
    } catch (const std::exception& error) {
        run.error = error.what();
    }
}

template <typename Result, typename Step>
Result restart_ask(void* context, Result fallback, Step step) noexcept {
    auto& run = *static_cast<RestartRun*>(context);
    if (!run.error.empty())
        return fallback;
    try {
        return step(run);
    } catch (const std::exception& error) {
        run.error = error.what();
        return fallback;
    }
}

ui::OptionsPanel options_panel_for(Screen screen) {
    switch (screen) {
    case Screen::sound:
        return ui::OptionsPanel::sound;
    case Screen::visuals:
        return ui::OptionsPanel::visuals;
    case Screen::speeds:
        return ui::OptionsPanel::speeds;
    case Screen::music:
        return ui::OptionsPanel::music;
    default:
        return ui::OptionsPanel::tabs;
    }
}

Screen screen_for(ui::OptionsPanel panel) {
    switch (panel) {
    case ui::OptionsPanel::sound:
        return Screen::sound;
    case ui::OptionsPanel::visuals:
    case ui::OptionsPanel::select_video_mode:
        return Screen::visuals;
    case ui::OptionsPanel::speeds:
        return Screen::speeds;
    case ui::OptionsPanel::music:
        return Screen::music;
    case ui::OptionsPanel::tabs:
        break;
    }
    return Screen::options;
}

// Copies the loaded widgets into the record model, with the runtime's
// current button stages; each button's status is its group value.
void panel_from_widgets(
    ui::Panel& panel, const oa::ui::gui_layout::Layout& layout, const StageMap& stages
) {
    ui::panel_load_layout(panel, layout);
    for (int32_t index = 1; index <= panel.count; ++index) {
        auto& control = panel.controls[static_cast<std::size_t>(index)];
        const auto found = stages.find(std::string(ui::control_name(control)));
        if (control.type == ui::ControlType::button && found != stages.end())
            control.stage = static_cast<uint8_t>(found->second);
    }
}

// Writes stages, text, quick keys, activity, gray state and knobs back to the
// widgets. A button's group value becomes its status, so the chosen member of
// a group (the open options tab) shows pressed.
void panel_to_widgets(
    const ui::Panel& panel, oa::ui::gui_layout::Layout& layout, StageMap& frames, StageMap& stages
) {
    const auto count = std::min(layout.gadgets.size(), ui::kPanelControls);
    for (std::size_t index = 1; index < count; ++index) {
        const auto& control = panel.controls[index];
        auto& gadget = layout.gadgets[index];
        gadget.common.active = static_cast<int8_t>(control.active);
        const auto text = std::string(ui::control_text(control));
        if (auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields)) {
            button->status = control.group_value;
            button->grayed_out = control.grayed != 0;
            copy_button_letter(*button, control);
            if (!text.empty() || button->stages <= 1)
                button->text = text;
            const auto name = std::string(ui::control_name(control));
            frames[name] = control.stage;
            stages[name] = control.stage;
        } else if (auto* label = std::get_if<oa::ui::gui_layout::LabelFields>(&gadget.fields)) {
            label->text = text;
        } else if (
            auto* slider = std::get_if<oa::ui::gui_layout::ScrollBarFields>(&gadget.fields)
        ) {
            slider->knob_position = control.slider.knob;
            slider->range = control.slider.range;
            slider->knob_size = control.slider.knob_size;
            slider->locked = control.grayed != 0;
        } else if (auto* box = std::get_if<oa::ui::gui_layout::TextBoxFields>(&gadget.fields)) {
            box->text = text;
        }
    }
}

// Load-game dialog overlay: lists the saved games of the player's own Saves
// folder, and of the SAVEGAME folder beside the preferences that held them
// before, through the load-game handlers and fills the list and preview
// text of the built-in LOADGAME.GUI screen. In its save role
// GAMENAME takes the typed name the game is saved under.
struct LoadGameOverlay {
    bool bound = false;
    bool save_role = false;
    oa::ui::gui_layout::Layout layout;
    ui::Panel panel;
    ui::SaveDialogContext saves;
    ui::SaveRoots roots;
    std::optional<oa::formats::fnt::Font> font;
    std::vector<std::string> rows;            // GAMES list text
    std::vector<std::string> side_names;      // shown for a save's "Side" index
    std::vector<std::string_view> side_views; // saves.side_names, over side_names
    ScreenContext* context = nullptr;         // valid during a hook
    // The input method's composition shown at the end of GAMENAME, as much
    // of it as fits; it is no part of the name a save is written under.
    std::string composition;
};

LoadGameOverlay& load_overlay() {
    static LoadGameOverlay overlay;
    return overlay;
}

void load_overlay_bind(ScreenContext* ctx, LoadGameOverlay& overlay) {
    overlay.bound = true;
    overlay.composition.clear();
    try {
        auto parsed = oa::ui::gui_layout::parse(
            ctx->assets->read(oa::data::defs::gui_path("loadgame.gui")).bytes,
            oa::ui::gui_layout::game_translation_lookup()
        );
        if (!parsed.ok())
            return;
        overlay.layout = std::move(*parsed.layout);
        constexpr std::string_view overlay_font = "anims/hattfont12.gaf";
        overlay.font = oa::ui::decoded::require(
            oa::formats::fnt::load_gaf(*ctx->assets, overlay_font), overlay_font
        );
    } catch (const std::exception& error) {
        screen_status(ctx, error.what());
        return;
    }
    ui::panel_load_layout(overlay.panel, overlay.layout);
    // Saves are listed where the runtime writes and loads them.
    const auto& runtime = *static_cast<const Runtime*>(ctx->host);
    overlay.roots = runtime.save_roots();
    auto& saves = overlay.saves;
    // A dialog opens without the lists and picture a previous one kept.
    ui::savegame_release_lists(saves);
    overlay.side_names = runtime.saved_game_side_names();
    overlay.side_views.assign(overlay.side_names.begin(), overlay.side_names.end());
    saves.side_names = overlay.side_views;
    saves.difficulty_names = runtime.difficulty_names();
    saves.files = ui::savegame_host_files(&overlay.roots);
    saves.reader = ui::savegame_persist_reader(&overlay.roots);
    saves.host = {};
    saves.host.context = &overlay;
    saves.host.play_sound = [](void* state, const char* sound) {
        auto* ctx = static_cast<LoadGameOverlay*>(state)->context;
        if (ctx != nullptr)
            screen_play_sound(ctx, sound);
    };
    // The dialogs' messages open MSGBOX.GUI over them, with OK, fitted.
    saves.host.show_message = [](void* state, const char* text, int32_t width) {
        auto* ctx = static_cast<LoadGameOverlay*>(state)->context;
        if (ctx != nullptr && !oa::ui::frontend_dialogs::open_message_box(
                                  ctx, text, width, entry::message_show_ok, entry::message_fit_width
                              ))
            screen_status(ctx, text);
    };
    overlay.context = ctx;
    if (overlay.save_role) {
        ui::savegame_enter_save(overlay.panel, saves);
        oa::ui::gui_input::mark_label_shadows(overlay.layout.gadgets);
        ctx->services->set_text_input(ctx->host, 1);
    } else if (ui::savegame_enter_load(overlay.panel, saves)) {
        oa::ui::gui_input::mark_label_shadows(overlay.layout.gadgets);
    }
    overlay.context = nullptr;
}

// Preview labels filled from the selected save's Summary.
constexpr const char* kPreviewLabels[] = {"GAMETYPE", "MISSION", "TIME", "SIDE", "DIFF"};

void load_overlay_tick(ScreenContext* ctx, void*) {
    auto& overlay = load_overlay();
    if (!overlay.bound)
        load_overlay_bind(ctx, overlay);
    auto& runtime = *static_cast<Runtime*>(ctx->host);
    for (const auto* name : kPreviewLabels)
        if (const auto* control = ui::panel_control(overlay.panel, name))
            runtime.set_screen_label(name, ui::control_text(*control));
}

// LOADGAME.GUI's records are relative to its root (gadget_geometry); the
// list, the preview text and the clicks follow those rectangles from the
// root's position on the frame.
std::optional<oa::ui::gui_input::Rect>
load_overlay_rect(const LoadGameOverlay& overlay, std::string_view name) {
    const auto index = ui::panel_find(overlay.panel, name);
    if (index < 0)
        return std::nullopt;
    return oa::ui::gui_input::gadget_geometry(
        overlay.layout.gadgets, static_cast<std::size_t>(index)
    );
}

// GAMES rows are the authored item height apart, else a line and a pixel.
int32_t load_overlay_row_height(const LoadGameOverlay& overlay) {
    const auto index = ui::panel_find(overlay.panel, "GAMES");
    if (index >= 0) {
        const auto& fields = overlay.layout.gadgets[static_cast<std::size_t>(index)].fields;
        if (const auto* list = std::get_if<oa::ui::gui_layout::ListBoxFields>(&fields);
            list != nullptr && list->item_height > 0)
            return list->item_height;
    }
    return overlay.font ? oa::formats::fnt::line_height(*overlay.font) + 1 : 12;
}

// Rows start two pixels into the list.
constexpr int32_t kListRowInset = 2;

// Acts on what the save dialog's handler decided: CANCEL closes the dialog,
// and a save writes the game and closes it.
int save_overlay_result(
    ScreenContext* ctx, LoadGameOverlay& overlay, const ui::SaveDialogResult& result
) {
    auto& panel = overlay.panel;
    auto& runtime = *static_cast<Runtime*>(ctx->host);
    switch (result.action) {
    case ui::SaveDialogAction::cancelled:
        runtime.close_save_dialog();
        return 1;
    case ui::SaveDialogAction::save: {
        const auto* name = ui::panel_control(panel, "GAMENAME");
        const std::string description(
            name != nullptr ? ui::control_text(*name) : std::string_view{}
        );
        if (runtime.save_dialog_game(result.path.data(), description.c_str()))
            runtime.close_save_dialog();
        return 1;
    }
    case ui::SaveDialogAction::refreshed:
        return 1;
    default:
        return 1;
    }
}

/// The save dialog's name field and the most it holds.
struct SaveNameField {
    ui::Control* control{};
    oa::present::TypedLimits limits{};
};

/// Finds the save dialog's name field, GAMENAME.
///
/// The name is held in whole characters, no more than the field's own
/// count, and in no more bytes than the field and the GAMES list's text
/// hold, so that the list shows the whole name.
///
/// @param overlay the bound save dialog
/// @return the field; none when the dialog has no GAMENAME
std::optional<SaveNameField> save_name_field(LoadGameOverlay& overlay) {
    const auto index = ui::panel_find(overlay.panel, "GAMENAME");
    if (index < 0)
        return std::nullopt;
    auto& control = overlay.panel.controls[static_cast<std::size_t>(index)];
    SaveNameField field{&control, {}};
    field.limits.bytes = std::min(control.text.size(), ui::kSaveDescriptionBytes) - 1;
    if (const auto* box = std::get_if<oa::ui::gui_layout::TextBoxFields>(
            &overlay.layout.gadgets[static_cast<std::size_t>(index)].fields
        );
        box != nullptr && box->max_characters > 0)
        field.limits.characters = static_cast<std::size_t>(box->max_characters);
    return field;
}

/// Gives the characters the save dialog's name takes.
///
/// The dialog shows the name as game text, and the save's file is named by
/// it in UTF-8. While the game's text is UTF-8 the name takes any script;
/// otherwise it takes printable ASCII, which reads the same in the game's
/// code page and in UTF-8.
///
/// @return the characters the name takes
oa::present::TypedCharacters save_name_characters() {
    return oa::present::game_text_settings().utf8 ? oa::present::TypedCharacters::file_name
                                                  : oa::present::TypedCharacters::ascii_file_name;
}

/// Takes the input method's composition away from the end of the name.
///
/// @param[in,out] overlay the bound save dialog
/// @param[in,out] text the name field's text
void drop_save_composition(LoadGameOverlay& overlay, std::string& text) {
    if (!overlay.composition.empty() && text.ends_with(overlay.composition))
        text.resize(text.size() - overlay.composition.size());
    overlay.composition.clear();
}

// The save dialog's click on a control: a button acts once released over,
// and a press on the name field only keeps the keys, as
// savegame_on_save_press says. The input method's composition is no part of
// the name the click saves under.
int save_overlay_press(ScreenContext* ctx, LoadGameOverlay& overlay, int32_t hit) {
    if (const auto field = save_name_field(overlay); field && !overlay.composition.empty()) {
        std::string text(ui::control_text(*field->control));
        drop_save_composition(overlay, text);
        ui::set_control_text(*field->control, text);
    }
    overlay.context = ctx;
    const auto result = ui::savegame_on_save_press(overlay.panel, overlay.saves, hit);
    overlay.context = nullptr;
    return save_overlay_result(ctx, overlay, result);
}

// Return at the end of the name activates GAMENAME, which saves under it.
int save_overlay_enter(ScreenContext* ctx, LoadGameOverlay& overlay, int32_t name) {
    overlay.panel.selected = name;
    overlay.context = ctx;
    const auto result = ui::savegame_on_save_click(overlay.panel, overlay.saves);
    overlay.context = nullptr;
    return save_overlay_result(ctx, overlay, result);
}

// Typing edits GAMENAME up to its length, in whole characters of the
// scripts save_name_characters gives; the characters no file's name may
// hold are not taken. Backspace takes the last character away, Return saves
// under the name and Escape leaves the dialog.
int save_overlay_key(ScreenContext* ctx, LoadGameOverlay& overlay, const ScreenInput& input) {
    const auto field = save_name_field(overlay);
    if (!field)
        return 1;
    const auto index = ui::panel_find(overlay.panel, "GAMENAME");
    auto& control = *field->control;
    std::string text(ui::control_text(control));
    if (input.kind == ScreenInputKind::text && input.text != nullptr) {
        drop_save_composition(overlay, text);
        std::ignore =
            oa::present::take_typed_text(text, input.text, save_name_characters(), field->limits);
        ui::set_control_text(control, text);
        return 1;
    }
    if (input.kind != ScreenInputKind::key_down)
        return 1;
    if (input.key == SDLK_BACKSPACE && oa::present::erase_last_character(text)) {
        ui::set_control_text(control, text);
    } else if (input.key == SDLK_RETURN) {
        return save_overlay_enter(ctx, overlay, index);
    } else if (input.key == SDLK_ESCAPE) {
        overlay.panel.selected = ui::panel_find(overlay.panel, "CANCEL");
        overlay.context = ctx;
        // CANCEL only plays its sound and asks for the dialog to close, which
        // follows.
        std::ignore = ui::savegame_on_save_click(overlay.panel, overlay.saves);
        overlay.context = nullptr;
        static_cast<Runtime*>(ctx->host)->close_save_dialog();
    }
    return 1;
}

// Runs the load dialog's handler for the record `control`: CANCEL releases
// the lists, and LOAD or the chosen GAMES row starts the selected save.
ui::SaveDialogAction
load_overlay_activate(ScreenContext* ctx, LoadGameOverlay& overlay, int32_t control) {
    overlay.context = ctx;
    overlay.panel.selected = control;
    const auto result = ui::savegame_on_load_click(overlay.panel, overlay.saves);
    overlay.context = nullptr;
    if (result.action == ui::SaveDialogAction::cancelled)
        overlay.bound = false;
    if (result.action == ui::SaveDialogAction::load) {
        // Starting the save replaces this screen with the match.
        overlay.bound = false;
        static_cast<Runtime*>(ctx->host)->start_saved_game(result.path.data());
    }
    return result.action;
}

/// Clicks a button of the bound dialog, once a press on it is released over
/// it.
///
/// CANCEL leaves the dialog for the screen it was opened over. In the load
/// dialog LOAD starts the selected save; in the save dialog OK saves under
/// GAMENAME and DELETE removes the selected save.
///
/// @param[in,out] ctx the screen; its host is the Runtime
/// @param[in,out] overlay the bound dialog
/// @param control index of the clicked button, from 1 to the panel's record count
void load_overlay_click(ScreenContext* ctx, LoadGameOverlay& overlay, int32_t control) {
    if (overlay.save_role) {
        // What the click did shows in the dialog, or in the screen it left.
        std::ignore = save_overlay_press(ctx, overlay, control);
        return;
    }
    if (load_overlay_activate(ctx, overlay, control) == ui::SaveDialogAction::cancelled)
        static_cast<Runtime*>(ctx->host)->leave_load_dialog();
}

// The load dialog's keys, as LOADGAME.GUI's defaults give them: Return is OK
// (LOAD), which starts the selected save, and Escape is CANCEL, which leaves
// the dialog as the button does. A message open over the dialog takes the
// keys first.
int load_overlay_key(ScreenContext* ctx, LoadGameOverlay& overlay, const ScreenInput& input) {
    if (input.kind != ScreenInputKind::key_down || oa::ui::frontend_dialogs::dialog_count() != 0)
        return 0;
    if (input.key == SDLK_RETURN) {
        if (const auto load = ui::panel_find(overlay.panel, "LOAD"); load >= 0)
            std::ignore = load_overlay_activate(ctx, overlay, load);
        return 1;
    }
    if (input.key != SDLK_ESCAPE)
        return 0;
    if (const auto cancel = ui::panel_find(overlay.panel, "CANCEL");
        cancel >= 0 &&
        load_overlay_activate(ctx, overlay, cancel) == ui::SaveDialogAction::cancelled)
        static_cast<Runtime*>(ctx->host)->leave_load_dialog();
    return 1;
}

int load_overlay_event(ScreenContext* ctx, void*) {
    auto& overlay = load_overlay();
    const auto* input = ctx->input;
    if (overlay.bound && overlay.save_role && input != nullptr &&
        (input->kind == ScreenInputKind::text || input->kind == ScreenInputKind::key_down))
        return save_overlay_key(ctx, overlay, *input);
    if (overlay.bound && !overlay.save_role && input != nullptr &&
        input->kind == ScreenInputKind::key_down)
        return load_overlay_key(ctx, overlay, *input);
    if (!overlay.bound || input == nullptr || input->kind != ScreenInputKind::pointer_down ||
        input->button != 1)
        return 0;
    const auto origin = static_cast<const Runtime*>(ctx->host)->panel_origin();
    const auto x = static_cast<int32_t>(input->x) - origin.x;
    const auto y = static_cast<int32_t>(input->y) - origin.y;
    int32_t hit = -1;
    for (int32_t index = 1; index <= overlay.panel.count; ++index) {
        const auto rect = oa::ui::gui_input::gadget_geometry(
            overlay.layout.gadgets, static_cast<std::size_t>(index)
        );
        const auto& control = overlay.panel.controls[static_cast<std::size_t>(index)];
        if (rect && control.active != 0 && rect->left <= x && x <= rect->right && rect->top <= y &&
            y <= rect->bottom)
            hit = index;
    }
    // SLIDER and its arrows are the screen's scroll bar's to take.
    if (hit < 0 ||
        overlay.panel.controls[static_cast<std::size_t>(hit)].type == ui::ControlType::slider)
        return 0;
    overlay.context = ctx;
    auto& panel = overlay.panel;
    if (ui::control_name(panel.controls[static_cast<std::size_t>(hit)]) == "GAMES") {
        const auto rect = load_overlay_rect(overlay, "GAMES");
        // Rows count from the first row the list's scroll bar shows.
        const auto first = static_cast<Runtime*>(ctx->host)->frontend_list_first("GAMES");
        const auto row = (y - rect->top - kListRowInset) / load_overlay_row_height(overlay) +
                         static_cast<int32_t>(first.value_or(0));
        if (row >= 0 && static_cast<std::size_t>(row) < overlay.saves.list.entries.size()) {
            panel.controls[static_cast<std::size_t>(hit)].list_selection =
                static_cast<int16_t>(row);
            ui::savegame_on_games_selected(panel, overlay.saves);
        }
        overlay.context = nullptr;
        return 1;
    }
    overlay.context = nullptr;
    // The dialog's buttons are pressed and released as the screen's own are:
    // the press holds the button, and only a release over it clicks it
    // (Runtime::activate_load_game_gadget).
    if (panel.controls[static_cast<std::size_t>(hit)].type == ui::ControlType::button)
        return 0;
    // A press on the save dialog's name field only gives it the keys.
    return overlay.save_role ? save_overlay_press(ctx, overlay, hit) : 0;
}

/// Draws the save dialog's name in the dialog's font.
///
/// While the game's text is UTF-8 the name may hold characters the font
/// lacks, such as hanzi, and the modern fonts draw those in hattfont12's
/// letter colour up to the field's right edge. Otherwise the font draws the
/// name's bytes alone. The input method's composition at the name's end is
/// drawn as the chat lines draw theirs: wholly in the modern fonts, in that
/// colour, and underlined (renderer::draw_fnt_composition).
///
/// @param[in,out] ctx screen being drawn; its surface takes the name
/// @param overlay the bound dialog and its font
/// @param text the name, game text
/// @param x the pen column, in screen pixels
/// @param y the pen row, in screen pixels
/// @param right the field's last column, in screen pixels
void load_overlay_text(
    ScreenContext* ctx,
    const LoadGameOverlay& overlay,
    std::string_view text,
    int32_t x,
    int32_t y,
    int32_t right
) {
    auto* surface = ctx->surface;
    if (surface == nullptr || !overlay.font || text.empty())
        return;
    const auto width = surface->width;
    const auto height = surface->height;
    // The font's indices are shown in the palette the dialog is drawn in.
    const auto& palette = static_cast<const Runtime*>(ctx->host)->screen_palette();
    const renderer::TextClip clip{0, 0, right, static_cast<int32_t>(height) - 1};
    // The composition follows the typed name, drawn apart from it.
    std::string_view composition;
    if (!overlay.composition.empty() && text.ends_with(overlay.composition)) {
        composition = overlay.composition;
        text.remove_suffix(composition.size());
    }
    const auto compose = [&](int32_t pen) {
        if (!composition.empty())
            std::ignore = renderer::draw_fnt_composition(
                *surface,
                *overlay.font,
                composition,
                pen,
                y,
                oa::present::gui_font_color,
                palette,
                clip
            );
    };
    if (oa::present::game_text_settings().utf8 && renderer::needs_text_runs(text, false)) {
        compose(
            renderer::draw_fnt_game_text(
                *surface,
                *overlay.font,
                text,
                x,
                y,
                oa::present::gui_font_color,
                palette,
                clip,
                false
            )
        );
        return;
    }
    std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height);
    std::vector<uint8_t> coverage(pixels.size());
    const oa::formats::fnt::IndexedSurface target{width, height, width, pixels, coverage};
    const int32_t pen = oa::formats::fnt::raster_text(target, *overlay.font, text, x, y);
    for (std::size_t offset = 0; offset < pixels.size(); ++offset) {
        if (coverage[offset] == 0)
            continue;
        const auto index = static_cast<std::size_t>(pixels[offset]) * oa::palette_entry_bytes;
        const auto out = offset * 3U;
        if (out + 2 >= surface->rgb.size())
            break;
        surface->rgb[out] = palette[index];
        surface->rgb[out + 1] = palette[index + 1];
        surface->rgb[out + 2] = palette[index + 2];
    }
    compose(pen);
}

/// Draws the RADAR picture as a hot surface's image.
///
/// The picture is stretched by a quad over the record's corners that samples
/// it from one texel inside its edges, which leaves the record's last row and
/// column as they were. Its palette indices show in the palette the dialog is
/// drawn in. Nothing is drawn while RADAR is hidden or the picture is smaller
/// than kSaveRadarMinimumSize in either direction.
///
/// @param[in,out] ctx screen being drawn; its surface takes the picture
/// @param overlay the bound dialog and its radar picture
/// @param origin screen position of the dialog's top-left corner, in pixels
void load_overlay_radar(
    ScreenContext* ctx, const LoadGameOverlay& overlay, oa::ui::display_layout::Point origin
) {
    auto* surface = ctx->surface;
    const auto* radar = ui::panel_control(overlay.panel, "RADAR");
    const auto rect = load_overlay_rect(overlay, "RADAR");
    const auto& picture = overlay.saves.radar_picture;
    if (surface == nullptr || radar == nullptr || radar->active == 0 || !rect ||
        picture.surface.width < ui::kSaveRadarMinimumSize ||
        picture.surface.height < ui::kSaveRadarMinimumSize ||
        picture.pixels.size() < static_cast<std::size_t>(picture.surface.width) *
                                    static_cast<std::size_t>(picture.surface.height))
        return;
    const int32_t right = rect->right - rect->left;
    const int32_t bottom = rect->bottom - rect->top;
    if (right <= 0 || bottom <= 0)
        return;
    auto stretched = oa::present::create_surface(right + 1, bottom + 1);
    oa::Sprite texture{};
    oa::present::sprite_from_surface(texture, picture.surface);
    constexpr int32_t texel_inset = 1;
    const oa::present::PolygonVertex quad[4] = {{0, 0}, {right, 0}, {right, bottom}, {0, bottom}};
    const int32_t u = texture.width - texel_inset;
    const int32_t v = texture.height - texel_inset;
    const oa::present::model::TexturePoint uv[4] = {
        {texel_inset, texel_inset}, {u, texel_inset}, {u, v}, {texel_inset, v}
    };
    oa::present::model::texture_quad(&stretched.surface, &texture, quad, uv);
    const auto& palette = static_cast<const Runtime*>(ctx->host)->screen_palette();
    for (int32_t y = 0; y < bottom; ++y)
        for (int32_t x = 0; x < right; ++x) {
            const auto screen_x = origin.x + rect->left + x;
            const auto screen_y = origin.y + rect->top + y;
            if (screen_x < 0 || screen_y < 0 || screen_x >= static_cast<int32_t>(surface->width) ||
                screen_y >= static_cast<int32_t>(surface->height))
                continue;
            const auto index =
                stretched.pixels
                    [static_cast<std::size_t>(y) * static_cast<std::size_t>(right + 1) +
                     static_cast<std::size_t>(x)];
            const auto* colour =
                &palette[static_cast<std::size_t>(index) * oa::palette_entry_bytes];
            const auto out = (static_cast<std::size_t>(screen_y) * surface->width +
                              static_cast<std::size_t>(screen_x)) *
                             3U;
            if (out + 2 >= surface->rgb.size())
                continue;
            std::copy_n(colour, 3, surface->rgb.begin() + static_cast<std::ptrdiff_t>(out));
        }
}

// GAMENAME is a text box, which the built-in screen leaves empty; RADAR is a
// hot surface given its picture at run time.
void load_overlay_draw(ScreenContext* ctx, void*) {
    auto& overlay = load_overlay();
    if (!overlay.bound)
        return;
    const auto origin = static_cast<const Runtime*>(ctx->host)->panel_origin();
    load_overlay_radar(ctx, overlay, origin);
    const auto* name = ui::panel_control(overlay.panel, "GAMENAME");
    if (const auto rect = load_overlay_rect(overlay, "GAMENAME");
        name != nullptr && name->active != 0 && rect)
        load_overlay_text(
            ctx,
            overlay,
            ui::control_text(*name),
            origin.x + rect->left,
            origin.y + rect->top,
            origin.x + rect->right
        );
}

/// Lets the OPTIONS lightbar take its next step on the match frame's draw.
void options_lightbar_tick(ScreenContext*, void*) {
    match_menu_session().lightbar_due = true;
}

/// Moves a panel model's records from frame positions to positions relative
/// to its root, as the panel's own GUI file gives them.
///
/// @param[in,out] panel model copied from a match HUD layout
void panel_relative_to_root(ui::Panel& panel) {
    const auto& root = panel.controls[0];
    for (int32_t index = 1; index <= panel.count; ++index) {
        auto& control = panel.controls[static_cast<std::size_t>(index)];
        control.x = static_cast<int16_t>(control.x - root.x);
        control.y = static_cast<int16_t>(control.y - root.y);
    }
}

/// Appends to a loaded layout the records a merge added to its panel model.
///
/// Each record past the layout's last is placed on the frame at the model's
/// position from the root: the PANEL filler as a record of its own, the
/// others copied from the sub-panel's layout in order.
///
/// @param[in,out] layout the loaded tab panel, root first
/// @param panel the merged model, positions relative to the root
/// @param sub the sub-panel's layout, root first
void append_merged_records(
    oa::ui::gui_layout::Layout& layout,
    const ui::Panel& panel,
    const oa::ui::gui_layout::Layout& sub
) {
    if (layout.gadgets.empty())
        return;
    const auto root_x = layout.gadgets.front().common.x;
    const auto root_y = layout.gadgets.front().common.y;
    std::size_t next = 1;
    for (auto index = layout.gadgets.size();
         index <= static_cast<std::size_t>(panel.count) && index < ui::kPanelControls;
         ++index) {
        const auto& control = panel.controls[index];
        oa::ui::gui_layout::Gadget gadget;
        if (control.type == ui::ControlType::filler) {
            gadget.common.type = static_cast<oa::ui::gui_layout::GadgetType>(control.type);
            gadget.common.name = std::string(ui::control_name(control));
        } else if (next < sub.gadgets.size())
            gadget = sub.gadgets[next++];
        gadget.common.x = static_cast<int16_t>(root_x + control.x);
        gadget.common.y = static_cast<int16_t>(root_y + control.y);
        gadget.common.width = control.width;
        gadget.common.height = control.height;
        gadget.common.active = static_cast<int8_t>(control.active);
        layout.gadgets.push_back(std::move(gadget));
    }
    if (next < sub.gadgets.size())
        std::cerr << "options panel: " << (sub.gadgets.size() - next)
                  << " controls were not shown\n";
    layout.gadgets.front().common.width = panel.controls[0].width;
    if (auto* fields = std::get_if<oa::ui::gui_layout::PanelFields>(&layout.gadgets.front().fields))
        fields->loaded_total_gadgets = static_cast<int16_t>(layout.gadgets.size() - 1U);
}

/// Copies VISUALS.GUI's screen-size controls onto an in-match visuals layout.
///
/// VISUALRT.GUI draws none. They sit in the gap under the shadows control,
/// in the column's own coordinates; the merge then places them beside the tabs.
///
/// @param sub the in-match visuals layout, root first
/// @param visuals VISUALS.GUI, which draws the controls
void append_screen_size_controls(
    oa::ui::gui_layout::Layout& sub, const oa::ui::gui_layout::Layout& visuals
) {
    struct Place {
        const char* name;
        int16_t x;
        int16_t y;
        int16_t width;
        int16_t height;
    };

    // BSHADOWS ends at 172 and RESTORE starts at 269. The three controls
    // from VISUALS.GUI fill that gap, kept narrow enough for the column.
    constexpr Place places[] = {
        {"VIDTEXT", 13, 180, 120, 14},
        {"VIDVAL", 13, 196, 118, 13},
        {"VIDSLDR", 13, 212, 120, 16},
    };
    for (const Place& place : places) {
        const auto found = std::find_if(
            visuals.gadgets.begin(),
            visuals.gadgets.end(),
            [&](const oa::ui::gui_layout::Gadget& gadget) {
                return gadget.common.name == place.name;
            }
        );
        if (found == visuals.gadgets.end())
            continue;
        auto gadget = *found;
        gadget.common.x = place.x;
        gadget.common.y = place.y;
        gadget.common.width = place.width;
        gadget.common.height = place.height;
        gadget.common.active = 1;
        sub.gadgets.push_back(std::move(gadget));
    }
}

/// Copies `rect` of an RGB image into a new picture of the rect's size;
/// pixels outside the image stay black.
///
/// @param source image copied from
/// @param x left column
/// @param y top row
/// @param width picture width
/// @param height picture height
/// @return the picture
renderer::Surface
copy_picture(const renderer::Surface& source, int32_t x, int32_t y, int32_t width, int32_t height) {
    renderer::Surface picture{
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        std::vector<uint8_t>(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U
        )
    };
    for (int32_t row = 0; row < height; ++row) {
        const auto from_y = y + row;
        if (from_y < 0 || from_y >= static_cast<int32_t>(source.height))
            continue;
        for (int32_t column = 0; column < width; ++column) {
            const auto from_x = x + column;
            if (from_x < 0 || from_x >= static_cast<int32_t>(source.width))
                continue;
            const auto from = (static_cast<std::size_t>(from_y) * source.width +
                               static_cast<std::size_t>(from_x)) *
                              3U;
            const auto to = (static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                             static_cast<std::size_t>(column)) *
                            3U;
            std::copy_n(
                source.rgb.begin() + static_cast<std::ptrdiff_t>(from),
                3,
                picture.rgb.begin() + static_cast<std::ptrdiff_t>(to)
            );
        }
    }
    return picture;
}

/// Maps a picture's source quad onto a destination quad with two upright
/// edges, column by column: the top of each column lies on the line between
/// the two top corners, and the picture's columns and rows spread evenly
/// across the quad.
///
/// @param[in,out] frame RGB frame drawn on
/// @param picture RGB picture read
/// @param step the lightbar step's source and destination corners
/// @param[out] cover one byte per frame pixel, set where the picture was drawn
void blit_lightbar(
    renderer::Surface& frame,
    const renderer::Surface& picture,
    const ui::OptionsLightbarStep& step,
    std::vector<uint8_t>& cover
) {
    cover.assign(static_cast<std::size_t>(frame.width) * frame.height, 0);
    const auto& to = step.destination;
    const auto& from = step.source;
    // Corners 0 and 3 share one upright edge, 1 and 2 the other.
    const auto left = std::min(to[0].x, to[1].x);
    const auto right = std::max(to[0].x, to[1].x);
    const bool mirrored = to[0].x > to[1].x;
    const auto span = std::max(1, right - left);
    const auto source_width = std::max(1, from[1].x - from[0].x);
    const auto source_height = std::max(1, from[3].y - from[0].y);
    for (int32_t x = left; x <= right; ++x) {
        if (x < 0 || x >= static_cast<int32_t>(frame.width))
            continue;
        const auto along = mirrored ? right - x : x - left;
        const auto top_left = mirrored ? to[1].y : to[0].y;
        const auto top_right = mirrored ? to[0].y : to[1].y;
        const auto top = top_left + (top_right - top_left) * along / span;
        const auto bottom = to[2].y;
        const auto height = std::max(1, bottom - top);
        const auto source_x = from[0].x + source_width * along / span;
        if (source_x < 0 || source_x >= static_cast<int32_t>(picture.width))
            continue;
        for (int32_t y = std::max(top, 0); y <= bottom && y < static_cast<int32_t>(frame.height);
             ++y) {
            const auto source_y = from[0].y + source_height * (y - top) / height;
            if (source_y < 0 || source_y >= static_cast<int32_t>(picture.height))
                continue;
            const auto read = (static_cast<std::size_t>(source_y) * picture.width +
                               static_cast<std::size_t>(source_x)) *
                              3U;
            const auto at = static_cast<std::size_t>(y) * frame.width + static_cast<std::size_t>(x);
            std::copy_n(
                picture.rgb.begin() + static_cast<std::ptrdiff_t>(read),
                3,
                frame.rgb.begin() + static_cast<std::ptrdiff_t>(at * 3U)
            );
            cover[at] = 1;
        }
    }
}

/// Shows part of the HUD layer over the battlefield, at the side column's
/// scale.
///
/// Each world-layer pixel is the 640x480 source point the chrome's scale
/// takes it to; one that lies in `area`, and is set in `cover` when one is
/// given, takes the HUD layer's pixel there. Source rows from the bottom
/// bar's down belong to the bottom bar, which shows the HUD layer's own, so
/// battlefield rows past the chrome's (in a window wider than 4:3) keep the
/// battlefield.
///
/// @param[in,out] world the world layer, canvas pixels from the battlefield corner
/// @param hud the HUD layer, in 640x480 source space
/// @param layout the match canvas's layout
/// @param area source rectangle shown
/// @param cover one byte per HUD pixel, set where the HUD layer shows; null
///        shows all of `area`
/// @param source_bottom source row the rows shown stop short of: the bottom
///        bar's, or a panel's own bottom for a panel that keeps the side
///        column's scale past it
void show_hud_over_battlefield(
    renderer::Surface& world,
    const renderer::Surface& hud,
    const oa::ui::display_layout::MatchLayout& layout,
    const oa::ui::display_layout::Rect& area,
    const std::vector<uint8_t>* cover,
    int32_t source_bottom = oa::ui::display_layout::kSourceBottomBarY
) {
    // On the phone layout the HUD layer's panels show through their placed
    // regions, never on the battlefield.
    if (area.width <= 0 || area.height <= 0 || hud.rgb.empty() || world.rgb.empty() ||
        layout.scale <= 0.0 || oa::ui::display_layout::placed_mode(layout))
        return;
    const auto unscaled = [&layout](int32_t value) {
        return static_cast<int32_t>(std::lround(static_cast<double>(value) / layout.scale));
    };
    const auto scaled = [&layout](int32_t value) {
        return static_cast<int32_t>(std::lround(static_cast<double>(value) * layout.scale));
    };
    const auto bottom = std::min(area.y + area.height, source_bottom);
    // A pixel either side catches the rounding of the scale.
    const auto first_x = std::max(layout.left, scaled(area.x) - 1);
    const auto last_x = std::min(layout.width - 1, scaled(area.x + area.width) + 1);
    const auto first_y = std::max(layout.top, scaled(area.y) - 1);
    const auto last_y = std::min(layout.bottom_bar_y() - 1, scaled(bottom) + 1);
    for (int32_t y = first_y; y <= last_y; ++y) {
        const auto world_y = y - layout.top;
        const auto source_y = unscaled(y);
        if (world_y < 0 || world_y >= static_cast<int32_t>(world.height) || source_y < area.y ||
            source_y >= bottom || source_y >= static_cast<int32_t>(hud.height))
            continue;
        for (int32_t x = first_x; x <= last_x; ++x) {
            const auto world_x = x - layout.left;
            const auto source_x = unscaled(x);
            if (world_x < 0 || world_x >= static_cast<int32_t>(world.width) || source_x < area.x ||
                source_x >= area.x + area.width || source_x >= static_cast<int32_t>(hud.width))
                continue;
            const auto from =
                static_cast<std::size_t>(source_y) * hud.width + static_cast<std::size_t>(source_x);
            if (cover != nullptr && (from >= cover->size() || (*cover)[from] == 0))
                continue;
            const auto to =
                static_cast<std::size_t>(world_y) * world.width + static_cast<std::size_t>(world_x);
            std::copy_n(
                hud.rgb.begin() + static_cast<std::ptrdiff_t>(from * 3U),
                3,
                world.rgb.begin() + static_cast<std::ptrdiff_t>(to * 3U)
            );
        }
    }
}

/// Stamps a GAF frame onto a picture's top-left.
///
/// @param source frame drawn; its covered pixels are stamped
/// @param palette 4 bytes per colour
/// @param[in,out] picture RGB picture stamped
void stamp_frame(
    const oa::formats::gaf::Frame& source,
    const oa::PaletteBytes& palette,
    renderer::Surface& picture
) {
    const auto rendered = oa::formats::gaf::render_normal(source);
    if (!rendered.ok())
        return;
    const auto& frame = *rendered.frame;
    for (std::size_t row = 0; row < frame.height && row < picture.height; ++row)
        for (std::size_t column = 0; column < frame.width && column < picture.width; ++column) {
            const auto at = row * frame.width + column;
            if (at >= frame.coverage.size() || frame.coverage[at] == 0)
                continue;
            const auto colour = static_cast<std::size_t>(frame.pixels[at]) * 4U;
            if (colour + 2 >= palette.size())
                continue;
            const auto to = (row * picture.width + column) * 3U;
            picture.rgb[to] = palette[colour];
            picture.rgb[to + 1] = palette[colour + 1];
            picture.rgb[to + 2] = palette[colour + 2];
        }
}

/// Finds a GAF entry by its name, compared without regard to case.
///
/// @param archive archive searched
/// @param name entry name
/// @return the first entry of that name, or null
const oa::formats::gaf::Sequence*
find_sequence(const oa::formats::gaf::Archive& archive, std::string_view name) {
    for (const auto& sequence : archive.sequences)
        if (sequence.name.size() == name.size() &&
            std::equal(name.begin(), name.end(), sequence.name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            }))
            return &sequence;
    return nullptr;
}

/// Draws a panel's image records, from `first` on, onto its background.
///
/// Each active image record shows the first frame of the GAF entry named
/// after it, from the panel's own GAF file, else the shared interface GAF,
/// with the frame's top-left on the record's position; a record found in
/// neither shows nothing.
///
/// @param[in,out] background the panel's background, in 640x480 source space
/// @param gadgets the panel's records, placed on the frame
/// @param first first record drawn
/// @param panel_art the panel's own GAF file (anims/<panel>.GAF)
/// @param shared_art the shared interface GAF
/// @param palette 4 bytes per colour
void draw_image_records(
    oa::Image& background,
    const std::vector<oa::ui::gui_layout::Gadget>& gadgets,
    std::size_t first,
    const oa::formats::gaf::Archive& panel_art,
    const oa::formats::gaf::Archive& shared_art,
    const oa::PaletteBytes& palette
) {
    for (auto index = first; index < gadgets.size(); ++index) {
        const auto& common = gadgets[index].common;
        if (common.type != oa::ui::gui_layout::GadgetType::image || common.active == 0)
            continue;
        const auto* sequence = find_sequence(panel_art, common.name);
        if (sequence == nullptr)
            sequence = find_sequence(shared_art, common.name);
        if (sequence == nullptr || sequence->frames.empty())
            continue;
        const auto rendered = oa::formats::gaf::render_normal(sequence->frames.front());
        if (!rendered.ok())
            continue;
        const auto& frame = *rendered.frame;
        for (std::size_t row = 0; row < frame.height; ++row)
            for (std::size_t column = 0; column < frame.width; ++column) {
                const auto at = row * frame.width + column;
                const auto x = common.x + static_cast<int>(column);
                const auto y = common.y + static_cast<int>(row);
                if (at >= frame.coverage.size() || frame.coverage[at] == 0 || x < 0 || y < 0 ||
                    x >= static_cast<int>(background.width) ||
                    y >= static_cast<int>(background.height))
                    continue;
                const auto colour = static_cast<std::size_t>(frame.pixels[at]) * 4U;
                if (colour + 2 >= palette.size())
                    continue;
                const auto to =
                    (static_cast<std::size_t>(y) * background.width + static_cast<std::size_t>(x)) *
                    3U;
                if (to + 2 >= background.rgb.size())
                    continue;
                background.rgb[to] = palette[colour];
                background.rgb[to + 1] = palette[colour + 1];
                background.rgb[to + 2] = palette[colour + 2];
            }
    }
}

} // namespace

/// Registers the load-game overlay, which lists the saves and fills the load and save dialogs,
/// and the match frame's tick that steps the OPTIONS lightbar of the preferences a match opens.
///
/// @param[in,out] registry registry receiving the overlays
void register_load_game_screens(ScreenRegistry* registry) {
    OverlayDesc load_game{};
    load_game.name = "load_game_list";
    load_game.screen = screen_id(Screen::load_game);
    load_game.z = 0;
    load_game.event = load_overlay_event;
    load_game.tick = load_overlay_tick;
    load_game.draw = load_overlay_draw;
    // A refused overlay is recorded in the registry, and register_screens
    // reports it once every screen and overlay is in.
    overlay_register(registry, &load_game);
    // The match frame draws the lightbar itself (draw_options_lightbar).
    OverlayDesc lightbar{};
    lightbar.name = "options_lightbar";
    lightbar.screen = screen_id(Screen::match);
    lightbar.z = 0;
    lightbar.tick = options_lightbar_tick;
    overlay_register(registry, &lightbar);
}

void Runtime::open_save_dialog(Screen parent) {
    auto& overlay = load_overlay();
    overlay.bound = false;
    overlay.save_role = true;
    options_parent_ = parent;
    if (parent == Screen::campaign_end)
        leave_end_panel_for_briefing();
    load(Screen::load_game);
}

bool Runtime::save_dialog_open() const {
    return load_overlay().save_role;
}

void Runtime::compose_save_name(std::string_view composition) {
    auto& overlay = load_overlay();
    const auto field = overlay.bound && overlay.save_role ? save_name_field(overlay) : std::nullopt;
    if (!field)
        return;
    std::string text(ui::control_text(*field->control));
    drop_save_composition(overlay, text);
    const std::size_t committed = text.size();
    std::ignore =
        oa::present::take_typed_text(text, composition, save_name_characters(), field->limits);
    overlay.composition = text.substr(committed);
    ui::set_control_text(*field->control, text);
}

void Runtime::close_save_dialog() {
    auto& overlay = load_overlay();
    overlay.bound = false;
    overlay.save_role = false;
    auto context = screen_context();
    context.services->set_text_input(context.host, 0);
    leave_options_screen();
}

void Runtime::activate_load_game_gadget() {
    if (!hovered_ || *hovered_ >= resources_.layout.gadgets.size())
        return;
    const auto name = resources_.layout.gadgets[*hovered_].common.name;
    // The bound dialog acts for its own records: a shown button is clicked,
    // and GAMES takes its rows on the press (load_overlay_event).
    auto& overlay = load_overlay();
    const auto control = overlay.bound ? ui::panel_find(overlay.panel, name) : -1;
    if (control >= 0) {
        const auto& record = overlay.panel.controls[static_cast<std::size_t>(control)];
        if (record.active != 0 && record.type == ui::ControlType::button) {
            auto context = screen_context();
            load_overlay_click(&context, overlay, control);
        }
        return;
    }
    // Without the dialog's records, there is no save to act on.
    if (name == "CANCEL" || name == "PREV" || name == "PREVMENU") {
        leave_load_dialog();
        return;
    }
    if (name == "LOAD" || name == "LOADGAME") {
        status_ = "No saved games in the preferences directory yet.";
        return;
    }
    if (name == "DELETE")
        status_ = "No saved games to delete.";
}

void Runtime::set_screen_label(std::string_view name, std::string_view text) {
    if (auto* gadget = widget(name))
        if (auto* label = std::get_if<oa::ui::gui_layout::LabelFields>(&gadget->fields))
            label->text = std::string(text);
}

const char* Runtime::load_game_background() const {
    return load_overlay().save_role ? "dsavegame2" : "dloadgame2";
}

void Runtime::present_load_game_panel(std::vector<renderer::ListPresentation>& lists) {
    auto& overlay = load_overlay();
    if (!overlay.bound)
        return;
    const auto count = std::min(resources_.layout.gadgets.size(), ui::kPanelControls);
    // SLIDER shows as its list fills, which the scroll bars decide.
    for (std::size_t index = 1;
         index < count && index <= static_cast<std::size_t>(overlay.panel.count);
         ++index)
        if (overlay.panel.controls[index].type != ui::ControlType::slider)
            resources_.layout.gadgets[index].common.active =
                static_cast<int8_t>(overlay.panel.controls[index].active);
    overlay.rows.clear();
    for (const auto& entry : overlay.saves.list.entries)
        overlay.rows.emplace_back(entry.description.data());
    std::optional<std::size_t> selected;
    if (const auto* list = ui::panel_control(overlay.panel, "GAMES");
        list != nullptr && list->list_selection >= 0 &&
        static_cast<std::size_t>(list->list_selection) < overlay.rows.size())
        selected = static_cast<std::size_t>(list->list_selection);
    // The dialog fills GAMES as a text list whenever its saves change; its
    // scroll bar then shows when they overflow it.
    if (auto* scrolls = frontend_scrolls())
        if (const auto* gadget = widget("GAMES"))
            if (const auto* list = renderer::find_layout_list(
                    *scrolls, static_cast<std::size_t>(gadget - resources_.layout.gadgets.data())
                );
                list != nullptr &&
                static_cast<std::size_t>(list->list.count) != overlay.rows.size())
                fill_frontend_list("GAMES", overlay.rows.size());
    lists.push_back({"GAMES", overlay.rows, frontend_list_first("GAMES").value_or(0), selected});
}

std::optional<std::string> Runtime::load_game_control_text(std::size_t index) const {
    const auto& overlay = load_overlay();
    if (!overlay.bound || screen_ != Screen::load_game || index == 0 ||
        index > static_cast<std::size_t>(overlay.panel.count) || index >= ui::kPanelControls)
        return std::nullopt;
    return std::string(ui::control_text(overlay.panel.controls[index]));
}

std::optional<Runtime::LoadGameRows> Runtime::load_game_rows() const {
    const auto& overlay = load_overlay();
    if (!overlay.bound || screen_ != Screen::load_game)
        return std::nullopt;
    LoadGameRows rows{&overlay.rows, std::nullopt};
    if (const auto* list = ui::panel_control(overlay.panel, "GAMES");
        list != nullptr && list->list_selection >= 0 &&
        static_cast<std::size_t>(list->list_selection) < overlay.rows.size())
        rows.selected = static_cast<std::size_t>(list->list_selection);
    return rows;
}

/// Returns the name a side's own panel art would have in its intgaf for a
/// page's panel: the GAF's stem without a trailing INT, then the page's
/// name without its first three letters, its side. RUMINT.gaf and ARMPAN2
/// give RUMPAN2. Empty when either name has no such shape.
///
/// @param side_gaf the path of the viewed side's intgaf; empty when none
/// @param page_panel the panel the page names
/// @return the side's own name for it, in the page's case; empty when none
std::string
Runtime::side_panel_art_name(const std::string& side_gaf, const std::string& page_panel) {
    if (side_gaf.empty() || page_panel.size() <= 3)
        return {};
    // The path comes '\\'-separated from the side table; the stem is its
    // last part without the extension.
    std::string stem = side_gaf.substr(side_gaf.find_last_of("/\\") + 1);
    if (const auto dot = stem.rfind('.'); dot != std::string::npos)
        stem.resize(dot);
    const auto upper = [](char c) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    };
    std::string tail = stem.size() > 3 ? stem.substr(stem.size() - 3) : std::string();
    std::transform(tail.begin(), tail.end(), tail.begin(), upper);
    if (tail != "INT")
        return {};
    stem.resize(stem.size() - 3);
    std::transform(stem.begin(), stem.end(), stem.begin(), upper);
    return stem + page_panel.substr(3);
}

Runtime::MatchHudArt* Runtime::match_hud_art(const std::string& layout) {
    auto panel = match_side_panel_gaf();
    const auto side = match_view_side();
    const auto font = match_side_font();
    // Art lent to a page went with the page when it was dropped.
    if (match_hud_art_ && match_hud_art_->lent && !match_hud_)
        match_hud_art_.reset();
    // The side tile is looked up in the archives (match_side_tile), so it is
    // found only when the art is read: the viewed side picks it.
    if (match_hud_art_ && match_hud_art_->side == side && match_hud_art_->panel == panel &&
        match_hud_art_->font == font)
        return &*match_hud_art_;
    match_hud_art_.reset();
    const auto tile = match_side_tile();
    MatchHudArt art;
    art.side = side;
    art.tile = tile;
    art.panel = panel;
    art.font = font;
    // A side whose section names no intgaf, or whose GAF lacks PANELTOP,
    // PANELSIDE or PANELBOT, is drawn without the missing pieces; 3.1c reads
    // the missing piece through a null entry as its load screen draws the
    // viewed side's panels, and faults. A GAF a mod lacks, or that cannot be
    // read, is drawn as none named.
    for (;;) {
        try {
            // The side tile is kept as read, for each page to decode the
            // background it draws its chrome over: the art's first page
            // takes the background decoded here.
            const auto tile_path = renderer::screen_background_path(assets_, tile);
            art.tile_file = assets_.read(tile_path).bytes;
            // Command icons live in commongui.gaf; the side's chrome lives in
            // the GAF its intgaf names.
            art.screen = renderer::load_screen_art(
                assets_, {{}, {}, "palettes/guipal.pal", "anims/commongui.gaf", panel}
            );
            art.screen.background =
                oa::ui::decoded::require(oa::decode_pcx(art.tile_file), tile_path);
            // The order buttons and the bars' words in the shown language.
            caption_gaf_pictures("anims/commongui.gaf", art.screen.sprites);
            if (!panel.empty())
                caption_gaf_pictures(panel, art.screen.shared_sprites);
            break;
        } catch (const std::exception& error) {
            std::cerr << "match HUD '" << layout << "' unavailable: " << error.what() << '\n';
            if (panel.empty())
                return nullptr;
            panel.clear();
        }
    }
    art.read_panel = panel;
    art.common_sequences = art.screen.sprites.sequences.size();
    // A font a mod lacks, or that cannot be read, is drawn as none named
    // (load_match_hud_layout).
    if (!font.empty())
        try {
            art.side_font =
                oa::ui::decoded::require(oa::formats::fnt::load_fnt(assets_, font), font);
        } catch (const std::exception& error) {
            std::cerr << "match font " << font << " unavailable: " << error.what() << '\n';
        }
    art.names_font = art.side_font.has_value();
    match_hud_art_ = std::move(art);
    return &*match_hud_art_;
}

renderer::ScreenResources Runtime::take_match_hud_art(MatchHudArt& art) {
    if (!art.lent) {
        art.lent = true;
        return std::move(art.screen);
    }
    // The page shown gives the art back: commongui's sequences without its
    // own GAF's before them and the side's and its gadgets' after them, and
    // the side tile decoded again, as the art read it, in place of the
    // background the page drew its chrome over.
    renderer::ScreenResources screen = std::move(*match_hud_);
    auto& sequences = screen.sprites.sequences;
    const auto first = std::min(art.page_sequences, sequences.size());
    const auto last = std::min(first + art.common_sequences, sequences.size());
    sequences.erase(sequences.begin() + static_cast<std::ptrdiff_t>(last), sequences.end());
    sequences.erase(sequences.begin(), sequences.begin() + static_cast<std::ptrdiff_t>(first));
    screen.layout = {};
    screen.background = {};
    screen.background = oa::ui::decoded::require(oa::decode_pcx(art.tile_file), art.tile);
    return screen;
}

bool Runtime::load_match_hud_layout(const std::string& layout, oa::ui::hud::SidePage side_page) {
    // A new panel's scroll bars are bound once it is placed.
    hud_scrolls_ = {};
    hud_scrolls_layout_ = nullptr;
    hud_scrolls_count_ = 0;
    // Every page is drawn with the side's art, which is read once a match
    // (match_hud_art): a page reads only its own GUI file and GAF.
    auto* art = match_hud_art(layout);
    if (art == nullptr)
        return false;
    // The page takes the art from the page shown (take_match_hud_art), once
    // its GUI file is read: a page that cannot be read leaves that one shown.
    oa::ui::gui_layout::Layout laid_out;
    try {
        laid_out = renderer::read_screen_layout(assets_, layout);
    } catch (const std::exception& error) {
        std::cerr << "match HUD '" << layout << "' unavailable: " << error.what() << '\n';
        return false;
    }
    match_hud_ = renderer::lay_out_screen(take_match_hud_art(*art), std::move(laid_out));
    const std::string& panel = art->read_panel;
    match_hud_panel_ = layout;
    match_hud_side_page_ = side_page;
    match_hud_authored_.clear();
    match_hud_placement_ = 0;
    match_panel_under_.reset();
    match_hud_side_backdrop_ = {};
    match_dialog_pixels_ = {};
    match_hud_states_.clear();
    for (const auto& gadget : match_hud_->layout.gadgets) {
        const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
        match_hud_states_.push_back(
            button != nullptr ? MatchGadgetState{button->status, button->grayed_out}
                              : MatchGadgetState{}
        );
    }
    // In-game GAF indices are authored against PALETTE.PAL, not the sidetile PCX.
    const bool have_game_palette =
        std::any_of(match_palette_.begin(), match_palette_.end(), [](uint8_t b) { return b != 0; });
    if (have_game_palette)
        match_hud_->background.palette = match_palette_;
    else
        match_hud_->background.palette.reset();
    const auto& icon_palette =
        match_hud_->background.palette ? *match_hud_->background.palette : match_hud_->gui_palette;
    const auto stem = fs::path(layout).stem().string();
    // The page's own GAF comes before commongui's art: a button it names
    // (a commander's BLAST, say) takes the page's art.
    {
        oa::formats::gaf::Archive own;
        append_gaf_file(own, "anims/" + stem + ".GAF");
        art->page_sequences = own.sequences.size();
        auto& hud_sequences = match_hud_->sprites.sequences;
        hud_sequences.insert(
            hud_sequences.begin(),
            std::make_move_iterator(own.sequences.begin()),
            std::make_move_iterator(own.sequences.end())
        );
    }
    // The side's GAF's sequences come after commongui's, captioned as the
    // shared sprites read from the same file are.
    if (!panel.empty()) {
        const auto& side = match_hud_->shared_sprites.sequences;
        auto& hud_sequences = match_hud_->sprites.sequences;
        hud_sequences.insert(hud_sequences.end(), side.begin(), side.end());
    }
    bind_gadget_gaf_art();
    for (auto& gadget : match_hud_->layout.gadgets) {
        // A gaffile button keeps its authored size.
        if (gadget.common.type != oa::ui::gui_layout::GadgetType::button || gadget.common.gaf_file)
            continue;
        const auto* sequence = gaf_sequence(match_hud_->sprites, gadget.common.name);
        if (sequence == nullptr)
            sequence = gaf_sequence(match_hud_->shared_sprites, gadget.common.name);
        if (sequence == nullptr || sequence->frames.empty())
            continue;
        gadget.common.width = static_cast<int16_t>(sequence->frames.front().width);
        gadget.common.height = static_cast<int16_t>(sequence->frames.front().height);
    }
    const auto* header = place_in_side_panel(match_hud_->layout.gadgets);
    for (const auto& gadget : match_hud_->layout.gadgets)
        match_hud_authored_.push_back(
            {gadget.common.x, gadget.common.y, gadget.common.height, gadget.common.active}
        );
    fit_match_build_page();
    // The viewed side's SIDEDATA font, for the resource numbers, the unit
    // panel and the squad numbers. 3.1c draws a side that names none in the
    // font last made active: COMIX, which the message log makes active as it
    // draws while it shows lines, except in a frame after the side panel's
    // GUI text was drawn again, which leaves that GUI's font; with no
    // message lines, the font of the GUI text drawn last, or COMIX from the
    // load screen before any. The engine draws them in COMIX in every case.
    // A font a mod lacks, or that cannot be read, is drawn as none named.
    // The art's first page takes the side's font, and its later pages keep
    // it, so that the match holds it once.
    match_side_names_font_ = art->names_font;
    if (art->side_font)
        match_small_font_ = std::exchange(art->side_font, std::nullopt);
    else if (!match_side_names_font_)
        match_small_font_ = message_font_ ? message_font_ : small_font_;
    // The bars' rows are black from column 128 on under the side art and
    // the bars' art, as the game's screen is under its bars.
    if (auto& background = match_hud_->background;
        background.width > static_cast<uint32_t>(kBattlefieldLeft) &&
        background.height >= static_cast<uint32_t>(kCanvasHeight) &&
        background.rgb.size() >= std::size_t{background.width} * background.height * 3U) {
        const std::size_t width = background.width;
        const auto clear_rows = [&background, width](int first_row, int count) {
            for (int row = first_row; row < first_row + count; ++row)
                std::fill_n(
                    background.rgb.begin() +
                        static_cast<std::ptrdiff_t>(
                            (static_cast<std::size_t>(row) * width + kBattlefieldLeft) * 3U
                        ),
                    (width - kBattlefieldLeft) * 3U,
                    uint8_t{0}
                );
        };
        clear_rows(0, kBattlefieldTop);
        clear_rows(kCanvasHeight - kBattlefieldBottom, kBattlefieldBottom);
    }
    overlay_gaf_sequence(
        match_hud_->background, match_hud_->shared_sprites, "PANELSIDE", 0, 0, icon_palette
    );
    // A unit's page draws the art its panel names (ARMPAN and the like) at
    // the panel's place, over the column's chrome and under its controls.
    // A side whose intgaf carries panel art of its own, named by the GAF's
    // stem without INT and the page's name without its side (RUMINT.gaf
    // holding RUMPAN and RUMPAN2 for a page that names ARMPAN or ARMPAN2),
    // draws that instead: a third side's pages are download-template pages
    // named by its nameprefix, so they can only ask for Arm's or Core's.
    if (const auto* fields = header != nullptr
                                 ? std::get_if<oa::ui::gui_layout::PanelFields>(&header->fields)
                                 : nullptr;
        side_page != oa::ui::hud::SidePage::other && fields != nullptr && !fields->panel.empty()) {
        const oa::formats::gaf::Archive* source = &match_hud_->sprites;
        std::string name = fields->panel;
        if (const std::string own = side_panel_art_name(panel, fields->panel);
            !own.empty() && gaf_sequence(match_hud_->shared_sprites, own) != nullptr) {
            source = &match_hud_->shared_sprites;
            name = own;
        }
        overlay_gaf_sequence(
            match_hud_->background, *source, name, header->common.x, header->common.y, icon_palette
        );
    }
    draw_match_bars(0);
    // The panel's image records go over the chrome: ARMOPT's OPTBG and
    // PREFS's IGOPT in the command well, under the radar PANELSIDE frames.
    const auto& gadgets = match_hud_->layout.gadgets;
    if (std::any_of(gadgets.begin(), gadgets.end(), [](const auto& gadget) {
            return gadget.common.type == oa::ui::gui_layout::GadgetType::image;
        })) {
        oa::formats::gaf::Archive panel_art;
        append_gaf_file(panel_art, "anims/" + stem + ".GAF");
        draw_image_records(
            match_hud_->background, gadgets, 1, panel_art, match_hud_->sprites, icon_palette
        );
    }
    match_hud_focus_ = loaded_panel_focus();
    return true;
}

void Runtime::draw_match_bars(int from_column) {
    if (!match_hud_)
        return;
    auto& background = match_hud_->background;
    const int width = static_cast<int>(background.width);
    const int rows = static_cast<int>(background.height);
    if (width <= from_column || rows < kCanvasHeight ||
        background.rgb.size() <
            static_cast<std::size_t>(width) * static_cast<std::size_t>(rows) * 3U)
        return;
    const auto& palette = background.palette ? *background.palette : match_hud_->gui_palette;
    const auto bar_art =
        [this](std::string_view name) -> std::optional<oa::formats::gaf::RenderedFrame> {
        const auto* sequence = gaf_sequence(match_hud_->shared_sprites, name);
        if (sequence == nullptr || sequence->frames.empty())
            return std::nullopt;
        auto frame = oa::formats::gaf::render_normal(sequence->frames.front());
        if (!frame.ok())
            return std::nullopt;
        return *frame.frame;
    };
    const auto top = bar_art("PANELTOP");
    const auto bottom = bar_art("PANELBOT");
    // A piece of art, unscaled: its first `piece_rows` rows from (x, y),
    // on the layer's columns from `from_column` on.
    const auto piece =
        [&](const oa::formats::gaf::RenderedFrame& frame, int x, int y, int piece_rows) {
            const int frame_width = frame.width;
            const int shown_rows = std::min<int>(piece_rows, frame.height);
            for (int row = 0; row < shown_rows; ++row) {
                const int layer_row = y + row;
                if (layer_row < 0 || layer_row >= rows)
                    continue;
                for (int column = std::max(0, from_column - x);
                     column < frame_width && x + column < width;
                     ++column) {
                    const auto offset = static_cast<std::size_t>(row) * frame.width +
                                        static_cast<std::size_t>(column);
                    if (offset >= frame.coverage.size() || frame.coverage[offset] == 0)
                        continue;
                    const auto colour = static_cast<std::size_t>(frame.pixels[offset]) * 4U;
                    if (colour + 2 >= palette.size())
                        continue;
                    const auto at = (static_cast<std::size_t>(layer_row) * background.width +
                                     static_cast<std::size_t>(x + column)) *
                                    3U;
                    background.rgb[at] = palette[colour];
                    background.rgb[at + 1] = palette[colour + 1];
                    background.rgb[at + 2] = palette[colour + 2];
                }
            }
        };
    // Under ui.interface-fixes bar-clamp art reaching the battlefield keeps
    // one row less than its top edge.
    const auto& fixes = ui_rules().interface_fixes;
    const bool clamp =
        fixes.enabled &&
        fixes.fixes.contains(oa::data::mod_profile::UiInterfaceFixesFixes::bar_clamp);
    const int top_rows = oa::ui::hud::top_panel_rows(
        top ? static_cast<int32_t>(top->height) : kBattlefieldTop, kBattlefieldTop, clamp
    );
    // The top bar: PANELTOP from column 129, then PANELBOT at its own width
    // after it, to the layer's right edge, each from its first row.
    int x = oa::ui::display_layout::kSourceBarArtLeft;
    if (top) {
        piece(*top, x, 0, top_rows);
        x += top->width;
    }
    top_bar_pieces_ = {x, bottom ? static_cast<int32_t>(bottom->width) : 0};
    if (!bottom || bottom->width == 0)
        return;
    for (; x < width; x += bottom->width)
        piece(*bottom, x, 0, top_rows);
    // The bottom bar: PANELBOT from column 129 at its own width, its first
    // 32 rows on the bar's.
    for (x = oa::ui::display_layout::kSourceBarArtLeft; x < width; x += bottom->width)
        piece(*bottom, x, kCanvasHeight - kBattlefieldBottom, kBattlefieldBottom);
}

void Runtime::extend_match_bars(int width) {
    if (!match_hud_ || width <= static_cast<int>(match_hud_->background.width))
        return;
    auto& background = match_hud_->background;
    const std::size_t was = background.width;
    const std::size_t rows = background.height;
    if (was == 0 || rows == 0 || background.rgb.size() != was * rows * 3U)
        return;
    // Each row keeps its pixels, black after them.
    const auto widen = [rows, was, width](std::vector<uint8_t>& pixels, std::size_t bytes) {
        const auto wider_width = static_cast<std::size_t>(width);
        std::vector<uint8_t> wider(wider_width * rows * bytes, 0);
        for (std::size_t row = 0; row < rows; ++row)
            std::copy_n(
                pixels.begin() + static_cast<std::ptrdiff_t>(row * was * bytes),
                static_cast<std::ptrdiff_t>(was * bytes),
                wider.begin() + static_cast<std::ptrdiff_t>(row * wider_width * bytes)
            );
        pixels = std::move(wider);
    };
    widen(background.rgb, 3);
    if (background.indices.size() == was * rows)
        widen(background.indices, 1);
    background.width = static_cast<uint32_t>(width);
    draw_match_bars(static_cast<int>(was));
}

int Runtime::match_column_rows() const {
    // The phone layout shows no side column: its sheets place the page's
    // gadgets one by one, so pages keep the rows they were authored for.
    if (match_layout_.phone)
        return kCanvasHeight;
    const auto scale = match_layout_.column_scale > 0.0 ? match_layout_.column_scale : 1.0;
    const auto rows =
        static_cast<int>(std::floor(static_cast<double>(match_layout_.height) / scale));
    return std::max(kCanvasHeight, rows);
}

void Runtime::fit_match_build_page() {
    match_side_page_bottom_ = 0;
    if (!match_hud_ || match_hud_->layout.gadgets.empty() ||
        match_hud_side_page_ == oa::ui::hud::SidePage::other)
        return;
    // Every gadget stays where the page's file places it; the page reaches
    // as low as its lowest drawn gadget.
    const int bottom = page_bottom_row(match_hud_->layout.gadgets);
    match_side_page_bottom_ = bottom;
    // Rows past 480 exist only while a page shown needs them, and then hold
    // the whole page; the side column's blank strip below a page that ends
    // within the column stays black, as the area under the panel is.
    auto& background = match_hud_->background;
    const auto rows = static_cast<uint32_t>(bottom);
    if (bottom > kCanvasHeight && background.height < rows) {
        background.rgb.resize(static_cast<std::size_t>(background.width) * rows * 3U, 0);
        if (!background.indices.empty())
            background.indices.resize(static_cast<std::size_t>(background.width) * rows, 0);
        background.height = rows;
    }
}

oa::ui::gui_layout::Gadget*
Runtime::place_in_side_panel(std::span<oa::ui::gui_layout::Gadget> gadgets) {
    oa::ui::gui_layout::Gadget* header = nullptr;
    for (auto& gadget : gadgets) {
        if (gadget.common.name == "HEADER") {
            header = &gadget;
            break;
        }
    }
    // ARMOPT's type-0 panel is named armopt.GUI, not HEADER. Child xpos/ypos
    // still live in that (0,128) well, same as ARMGEN.
    if (header == nullptr) {
        for (auto& gadget : gadgets) {
            if (gadget.common.type == oa::ui::gui_layout::GadgetType::panel) {
                header = &gadget;
                break;
            }
        }
    }
    // HEADER is the type-0 panel at screen (0,128). Child xpos/ypos in ARMGEN
    // and ARMCOMn live in that panel: ARMORDERS ypos=4 is not a screen row.
    // The 0x7e-pixel radar picture, drawn at Game.radar_offset_x and
    // radar_offset_y, occupies the well above.
    if (header == nullptr)
        return nullptr;
    const auto origin_x = header->common.x;
    const auto origin_y = header->common.y;
    for (auto& gadget : gadgets) {
        if (&gadget == header || gadget.common.width <= 0 || gadget.common.height <= 0)
            continue;
        gadget.common.x = static_cast<int16_t>(gadget.common.x + origin_x);
        gadget.common.y = static_cast<int16_t>(gadget.common.y + origin_y);
    }
    return header;
}

int32_t
Runtime::page_bottom_row(std::span<const oa::ui::gui_layout::Gadget> gadgets, bool any_record) {
    int32_t bottom = 0;
    for (std::size_t index = 1; index < gadgets.size(); ++index) {
        const auto& common = gadgets[index].common;
        if ((any_record || common.active != 0) && common.width > 0 && common.height > 0)
            bottom = std::max(bottom, common.y + common.height);
    }
    return bottom;
}

oa::ui::hud::SidePageScale Runtime::match_side_page_scale() const {
    // The phone layout has no side column: its sheets show each of the
    // page's gadgets as authored, whatever rows the page reaches.
    if (!match_hud_ || match_hud_->layout.gadgets.empty() ||
        match_hud_side_page_ == oa::ui::hud::SidePage::other || match_side_page_bottom_ <= 0 ||
        match_layout_.phone)
        return {};
    const auto& panel = match_hud_->layout.gadgets.front().common;
    return oa::ui::hud::side_page_scale(
        panel.x, panel.y, match_side_page_bottom_, match_column_rows()
    );
}

void Runtime::show_match_orders_page() {
    namespace hud = oa::ui::hud;
    if (!match_)
        return;
    const auto prefix = match_side_name_prefix();
    auto& game = match_->state().game;
    auto state = hud::order_panel_load(game);
    const auto summary = summarize_order_panel(state);
    const auto previous = match_hud_panel_;
    match_hud_panel_.clear();
    // Nothing selected shows the side's idle page: its panel art is the
    // emblem under the minimap. A selection keeps the general order page.
    if (summary.count == 0) {
        char name[256];
        hud::format_idle_page_name(name, sizeof name, prefix.c_str());
        if (!load_match_hud_layout(oa::data::defs::gui_path(name), hud::SidePage::unit)) {
            match_hud_panel_ = previous;
            return;
        }
        state.unit_id = 0;
        state.unit_type = 0;
    } else {
        const oa::Unit* unit = summary.count == 1 ? summary.first : nullptr;
        const auto table = order_panel_table();
        hud::load_general_page(
            state,
            unit,
            unit != nullptr ? hud::unit_def(table, *unit) : nullptr,
            prefix.c_str(),
            order_panel_controls(),
            order_panel_loader()
        );
        if (match_hud_panel_.empty()) {
            match_hud_panel_ = previous;
            return;
        }
    }
    hud::order_panel_store(game, state);
    match_build_page_ = 0;
    status_ = "Orders";
    // The HUD layer takes the page at once; the frame draws the match, the
    // page with it, once.
    compose_match_hud();
}

void Runtime::show_match_pause_menu() {
    if (match_finished_)
        return;
    // Opened over the running game or from the tab menu, the menu darkens
    // the panel below it; back from its own pages it opens over none.
    if (!match_paused_ || team_panel_open())
        keep_panel_below_darkened();
    match_menu_session().close_confirm = false;
    forget_team_panel();
    match_paused_ = true;
    match_command_ = MatchCommand::none;
    pending_build_type_ = 0;
    // The in-game menu gives the match's panels the keyboard until the match
    // resumes.
    match_panels_keyboard_ = true;
    if (load_match_hud_layout(oa::data::defs::gui_path("ARMOPT.GUI"))) {
        auto& session = match_menu_session();
        session.ingame_panel = IngamePanel::options;
        session.ingame.host = {};
        session.ingame.session = ingame_session(
            campaign_mission_, (current_extension_state() & extension_state::multiplayer) != 0
        );
        session.ingame.saved_games_offered = offers_saved_games();
        session.ingame.difficulty_names = difficulty_names();
        // A match with a return label names it in the exit menus.
        session.ingame.return_label = return_label_;
        panel_from_widgets(session.panel, match_hud_->layout, widget_text_stages_);
        ui::ingame_enter_options(session.panel, session.ingame);
        panel_to_widgets(
            session.panel, match_hud_->layout, widget_gaf_frames_, widget_text_stages_
        );
        // The menu of a shared match holds nothing.
        status_ = (current_extension_state() & extension_state::shared_match) != 0 ? "Options"
                                                                                   : "Game paused";
        render_match_surface();
    } else
        status_ = "Pause menu unavailable";
}

void Runtime::resume_match_pause() {
    if (match_finished_)
        return;
    // Resume on the local game's menu releases an explicit Pause as well
    // as the menu's hold. Dismissing another dialog does not change Pause,
    // and a shared match keeps its separate, synchronized pause control.
    if (match_ && match_hud_panel_ == oa::data::defs::gui_path("ARMOPT.GUI") &&
        (current_extension_state() & extension_state::shared_match) == 0 &&
        (match_->state().game.sim_run_flags & oa::ui::console::kSimRunPaused) != 0) {
        run_console_hotkey(oa::ui::console::hotkey::pause);
    }
    // Resuming over the preferences keeps what they set, as leaving them does.
    if (match_preferences_open())
        leave_options_screen();
    match_menu_session().close_confirm = false;
    match_menu_session().ingame_panel = IngamePanel::options;
    forget_team_panel();
    match_panels_below_.clear();
    match_paused_ = false;
    match_panels_keyboard_ = false;
    if (selected_match_unit_ != 0)
        apply_match_hud_for_selection();
    else
        show_match_orders_page();
    status_ = "Resumed";
}

void Runtime::blit_gaf_source(
    const oa::formats::gaf::RenderedFrame& frame, int dest_x, int dest_y
) {
    for (std::size_t row = 0; row < frame.height; ++row) {
        for (std::size_t column = 0; column < frame.width; ++column) {
            const auto offset = row * frame.width + column;
            if (offset >= frame.coverage.size() || frame.coverage[offset] == 0)
                continue;
            fill_source_rect(
                dest_x + static_cast<int>(column),
                dest_y + static_cast<int>(row),
                1,
                1,
                frame.pixels[offset]
            );
        }
    }
}

void Runtime::ensure_match_titles() {
    if (match_titles_.sequences.empty())
        append_gaf_file(match_titles_, "anims/igtitles.gaf");
}

void Runtime::present_match_outcome() {
    if (!match_ || match_finished_)
        return;
    const auto result = match_->outcome();
    if (result == sim::scenario::Outcome::ongoing)
        return;
    match_finished_ = true;
    outcome_over_menu_ = match_paused_;
    match_paused_ = true;
    match_command_ = MatchCommand::none;
    pending_build_type_ = 0;
    match_outcome_ = result;
    ensure_match_titles();
    status_ = result == sim::scenario::Outcome::victory ? "Victory" : "Defeat";
}

void Runtime::finish_match_outcome() {
    if (extension_.outcome_ready != nullptr &&
        !call_hook_or_raise<&Extension::outcome_ready>(extension_, *this))
        return;
    keep_finished_match();
    leave_match();
    load(Screen::campaign_end);
}

const char* Runtime::campaign_end_background() {
    const auto* world = endgame_world();
    auto* options = endgame_game_options();
    const bool continuing =
        world != nullptr && oa::ui::campaign::campaign_can_continue(options, world->game);
    const char* name = continuing ? "outcome1" : "outcome0";
    // A final campaign victory leaves for the ending without opening the
    // panel, so data without the panel's bitmap, such as the Total
    // Annihilation demo (1997) without outcome0, loads none.
    const bool leaves_for_ending =
        world != nullptr && options != nullptr &&
        oa::data::campaign::campaign_kind(options) == oa::data::campaign::SessionKind::campaign &&
        (world->game.outcome_flags & sim::scenario::outcome_flag::won) != 0 &&
        !oa::data::campaign::campaign_has_next_mission(options, world->game.mission_index + 1) &&
        world->game.no_movie == 0;
    if (leaves_for_ending && assets_.file_size(std::string("bitmaps/") + name + ".pcx") == 0)
        return nullptr;
    return name;
}

void Runtime::enter_campaign_end() {
    auto& session = end_mission_session();
    session.env = campaign_dialog_env();
    auto& context = session.context;
    context = {};
    context.host.context = this;
    context.host.play_sound = [](void* host, const char* name) {
        static_cast<Runtime*>(host)->play_ui_sound(name, 0);
    };
    context.host.disc_present = [](void* host) {
        return static_cast<Runtime*>(host)->find_disc(menu::Disc::campaign) != 0;
    };
    context.host.refresh_archives = [](void* host) {
        static_cast<Runtime*>(host)->refresh_disc_archives();
    };
    context.host.show_message = [](void* host, const char* text, int32_t width) {
        auto& runtime = *static_cast<Runtime*>(host);
        runtime.show_frontend_message(
            runtime.translate_ui(text), width, entry::message_show_ok, entry::message_fit_width
        );
    };
    context.host.set_music_kind = [](void* host, int32_t) {
        static_cast<Runtime*>(host)->music_main_menu();
    };
    // A multiplayer game stopped being shared with the finished match
    // (keep_finished_match), so the closing call has no session to leave.
    context.state = &state_;
    context.frontend = this;
    // A match with a return label names it on MAIN MENU and leaves the
    // pointer's picture as it is.
    context.return_label = return_label_;
    context.campaign = endgame_game_options();
    context.env = &session.env;
    context.world = endgame_world();
    context.difficulty = static_cast<int32_t>(preferences_.difficulty);
    context.skirmish_difficulty = static_cast<int32_t>(preferences_.skirmish_difficulty);
    context.saved_games_offered = offers_saved_games();
    end_mission_rows_.clear();
    campaign_mission_first_visible_ = 0;
    start_endgame();
}

void Runtime::open_end_panel(oa::ui::campaign::ScoreLayout* reopened) {
    auto& session = end_mission_session();
    auto& context = session.context;
    panel_from_widgets(session.panel, resources_.layout, widget_text_stages_);
    if (reopened != nullptr)
        ui::end_mission_open(session.panel, context, *reopened);
    else
        ui::end_mission_enter(session.panel, context);
    panel_to_widgets(session.panel, resources_.layout, widget_gaf_frames_, widget_text_stages_);
    end_mission_rows_.clear();
    const char* row = context.missions.data();
    for (int32_t index = 0; index < context.mission_count; ++index) {
        end_mission_rows_.emplace_back(row);
        row += end_mission_rows_.back().size() + 1;
    }
    if (end_mission_rows_.empty())
        return;
    const auto* list = ui::panel_control(session.panel, "Missions");
    const auto count = static_cast<int32_t>(end_mission_rows_.size());
    const auto selected =
        std::clamp<int32_t>(list != nullptr ? list->list_selection : 0, 0, count - 1);
    selected_mission_index_ = static_cast<std::size_t>(selected);
    // The list is filled as a text list, which shows its scroll bar when the
    // rows overflow, and the list scroll puts a selection out of view at the
    // top, short of the end.
    fill_frontend_list("Missions", end_mission_rows_.size());
    select_frontend_list_row("Missions", selected_mission_index_);
    if (const auto first = frontend_list_first("Missions"))
        campaign_mission_first_visible_ = *first;
    else if (const auto* gadget = widget("Missions")) {
        int32_t item_height =
            static_cast<int32_t>(oa::formats::fnt::line_height(resources_.font)) + 1;
        if (const auto* fields = std::get_if<oa::ui::gui_layout::ListBoxFields>(&gadget->fields);
            fields != nullptr && fields->item_height > 0)
            item_height = fields->item_height;
        const int32_t rows = std::max((gadget->common.height - 2) / item_height, 1);
        const int32_t last_top = std::max(count - rows, 0);
        auto top = static_cast<int32_t>(campaign_mission_first_visible_);
        if (selected < top || selected > top + rows - 1) {
            if (last_top != 0)
                top = selected;
            top = std::min(top, last_top);
        }
        campaign_mission_first_visible_ = static_cast<std::size_t>(top);
    }
}

void Runtime::close_end_panel() {
    auto& session = end_mission_session();
    session.panel.selected = ui::kNoSelection;
    // With nothing selected the click closes the panel and asks for nothing
    // more.
    std::ignore = ui::end_mission_on_click(session.panel, session.context);
    end_mission_rows_.clear();
}

void Runtime::leave_campaign_end() {
    close_end_panel();
    if (!endgame_reopens())
        release_endgame();
}

void Runtime::draw_campaign_end_title() {
    // The panel set-up draws the title.
    const auto& context = end_mission_session().context;
    if (context.palette == nullptr)
        return;
    ensure_match_titles();
    const auto* sequence =
        gaf_sequence(match_titles_, context.victory_title ? "igvictory" : "igdefeat");
    if (sequence == nullptr || sequence->frames.empty())
        return;
    const auto rendered = oa::formats::gaf::render_normal(sequence->frames.front());
    if (!rendered.ok())
        return;
    // At (screen_width / 2, 0x1c) with GAF origin as hotspot.
    const int dest_x = kCanvasWidth / 2 - static_cast<int>(rendered.frame->origin_x);
    const int dest_y = 0x1c - static_cast<int>(rendered.frame->origin_y);
    const auto& pal = match_palette_.size() >= 1024 ? match_palette_ : resources_.gui_palette;
    const auto& frame = *rendered.frame;
    for (std::size_t row = 0; row < frame.height; ++row) {
        for (std::size_t column = 0; column < frame.width; ++column) {
            const auto offset = row * frame.width + column;
            if (offset >= frame.coverage.size() || frame.coverage[offset] == 0)
                continue;
            const int x = dest_x + static_cast<int>(column);
            const int y = dest_y + static_cast<int>(row);
            if (x < 0 || y < 0 || x >= static_cast<int>(surface_.width) ||
                y >= static_cast<int>(surface_.height))
                continue;
            const auto pal_i = static_cast<std::size_t>(frame.pixels[offset]) * 4U;
            if (pal_i + 2 >= pal.size())
                continue;
            const auto di =
                (static_cast<std::size_t>(y) * surface_.width + static_cast<std::size_t>(x)) * 3U;
            surface_.rgb[di] = pal[pal_i];
            surface_.rgb[di + 1] = pal[pal_i + 1];
            surface_.rgb[di + 2] = pal[pal_i + 2];
        }
    }
}

void Runtime::activate_campaign_end_gadget() {
    if (!hovered_ || *hovered_ >= resources_.layout.gadgets.size())
        return;
    click_end_panel(*hovered_);
}

void Runtime::activate_end_panel_default() {
    const auto& context = end_mission_session().context;
    const char* name =
        context.enter_control[0] != '\0' ? context.enter_control.data() : context.focus.data();
    const auto* gadget = name[0] != '\0' ? widget(name) : nullptr;
    if (gadget == nullptr || gadget->common.active == 0)
        return;
    click_end_panel(static_cast<std::size_t>(gadget - resources_.layout.gadgets.data()));
}

void Runtime::click_end_panel(std::size_t gadget) {
    auto& session = end_mission_session();
    auto& panel = session.panel;
    auto& context = session.context;
    const auto index = std::min(gadget, ui::kPanelControls - 1);
    panel_from_widgets(panel, resources_.layout, widget_text_stages_);
    panel.selected = static_cast<int32_t>(index);
    auto& control = panel.controls[index];
    control.stage =
        oa::ui::gui_input::released_button_stage(resources_.layout.gadgets[index], control.stage);
    if (auto* list = ui::panel_control(panel, "Missions"))
        list->list_selection = static_cast<int16_t>(selected_mission_index_);
    const auto action = ui::end_mission_on_click(panel, context);
    if (static_cast<uint32_t>(context.difficulty) != preferences_.difficulty) {
        preferences_.difficulty = static_cast<uint32_t>(context.difficulty);
        preferences_.skirmish_difficulty = static_cast<uint32_t>(context.skirmish_difficulty);
        write_number(init::general_section, "Difficulty", preferences_.difficulty);
        flush_preferences();
    }
    panel_to_widgets(panel, resources_.layout, widget_gaf_frames_, widget_text_stages_);
    switch (action) {
    case ui::EndMissionAction::open_load_game:
        // The panel is kept for the dialog's CANCEL to come back to.
        options_parent_ = Screen::campaign_end;
        leave_end_panel_for_briefing();
        load(Screen::load_game);
        return;
    case ui::EndMissionAction::open_save_game:
        open_save_dialog(Screen::campaign_end);
        return;
    case ui::EndMissionAction::start_mission: {
        // The briefing replaces the panel; the finished game stays for Back.
        const auto chosen = selected_mission_index_;
        close_end_panel();
        leave_end_panel_for_briefing();
        selected_mission_index_ = chosen;
        show_mission_briefing();
        return;
    }
    case ui::EndMissionAction::main_menu:
        load(Screen::main_menu);
        return;
    case ui::EndMissionAction::none:
    case ui::EndMissionAction::closed:
        break;
    }
    rebuild_surface();
}

void Runtime::draw_igtitle(std::string_view name) {
    ensure_match_titles();
    const auto* sequence = gaf_sequence(match_titles_, name);
    if (sequence == nullptr || sequence->frames.empty())
        return;
    const auto rendered = oa::formats::gaf::render_normal(sequence->frames.front());
    if (!rendered.ok())
        return;
    // At ((screen_width + 0x80) / 2, screen_height / 2) with GAF
    // origin as hotspot, the battlefield centre (384, 240 at 640x480). Here
    // the live canvas and side column give the centre, and the title scales
    // with the chrome.
    const auto& frame = *rendered.frame;
    const int hot_x = (match_layout_.width + match_layout_.left) / 2;
    const int hot_y = match_layout_.height / 2;
    const auto scaled = [this](int value) {
        return static_cast<int>(std::lround(static_cast<double>(value) * match_layout_.scale));
    };
    for (std::size_t row = 0; row < frame.height; ++row) {
        const int top = hot_y + scaled(static_cast<int>(row) - frame.origin_y);
        const int bottom = hot_y + scaled(static_cast<int>(row) + 1 - frame.origin_y);
        for (std::size_t column = 0; column < frame.width; ++column) {
            const auto offset = row * frame.width + column;
            if (offset >= frame.coverage.size() || frame.coverage[offset] == 0)
                continue;
            const int left = hot_x + scaled(static_cast<int>(column) - frame.origin_x);
            const int right = hot_x + scaled(static_cast<int>(column) + 1 - frame.origin_x);
            const auto at = canvas_paint(left, top);
            fill_hud_rect(
                at.x,
                at.y,
                std::max(1, right - left),
                std::max(1, bottom - top),
                frame.pixels[offset]
            );
        }
    }
}

void Runtime::draw_end_overlay() {
    // A placed dialog's part over the side column is drawn afresh with it.
    match_dialog_side_.width = 0;
    match_dialog_side_.height = 0;
    match_dialog_side_.rgb.clear();
    if (match_finished_) {
        // No victory or defeat banner is drawn for a watcher.
        if ((current_extension_state() & extension_state::local_watcher) == 0) {
            const bool won = match_outcome_ == sim::scenario::Outcome::victory;
            draw_igtitle(won ? "igvictory" : "igdefeat");
            if (won)
                announce_victory();
        }
        return;
    }
    // The pause bit holds any match; a menu holds only a match played on this
    // machine alone, and a shared match's menu shows over the running game.
    // The team menu and its panels hold no match (match_clock_steps).
    const bool pause_bit =
        match_ && (match_->state().game.sim_run_flags & oa::ui::console::kSimRunPaused) != 0;
    const bool shared = (current_extension_state() & extension_state::shared_match) != 0;
    if (pause_bit || (match_paused_ && !shared && !team_panel_open()))
        draw_igtitle("igpaused");
    if (match_paused_) {
        draw_battlefield_panel();
        draw_options_lightbar();
    }
}

void Runtime::draw_battlefield_panel() {
    // On the phone layout the panel shows whole through its placed region
    // (refresh_placed_hud_regions), over the touch controls.
    if (!match_hud_ || match_hud_->layout.gadgets.empty() || match_hud_cpu_.rgb.empty() ||
        oa::ui::display_layout::placed_mode(match_layout_))
        return;
    // A panel over the battlefield shows whole at one scale where it lies:
    // one that reaches the bottom bar's rows, the tab menu, on the bar
    // (display_layout::source_panel_to_canvas), where the HUD layer shows
    // the rest of it.
    const auto painted = [this](const oa::ui::display_layout::Rect& source) {
        if (hud_source_space_)
            return source;
        const auto canvas = oa::ui::display_layout::source_panel_to_canvas(
            match_layout_, source.x, source.y, source.width, source.height
        );
        const auto at = canvas_paint(canvas.x, canvas.y);
        return oa::ui::display_layout::Rect{
            at.x, at.y, std::max(1, canvas.width), std::max(1, canvas.height)
        };
    };
    // The panels kept under the HUD panel show darkened where they lie over
    // the battlefield, as they showed when the panel over each opened.
    for (const auto& below : match_panels_below_) {
        if (below.root.x + below.root.width <= kBattlefieldLeft || below.darkened.rgb.empty())
            continue;
        const auto area = painted(below.root);
        scale_blit(
            paint_target(),
            below.darkened,
            area.x,
            area.y,
            area.width,
            area.height,
            0,
            0,
            below.root.width,
            below.root.height
        );
    }
    const auto& gadgets = match_hud_->layout.gadgets;
    const auto& root = gadgets.front().common;
    if (root.x + root.width <= kBattlefieldLeft || root.width <= 0 || root.height <= 0)
        return;
    const auto show = [this](const oa::ui::gui_layout::CommonFields& area) {
        const auto top_left = hud_canvas(area.x, area.y);
        const auto bottom_right = hud_canvas(area.x + area.width, area.y + area.height);
        scale_blit(
            paint_target(),
            match_hud_cpu_,
            top_left.x,
            top_left.y,
            std::max(1, bottom_right.x - top_left.x),
            std::max(1, bottom_right.y - top_left.y),
            area.x,
            area.y,
            area.width,
            area.height
        );
    };
    if (const auto area = placed_panel_area()) {
        // The panel kept under the dialog (a team panel's) shows over the
        // battlefield from its own pixels as it showed, sampled as it was
        // when it was the match HUD.
        if (match_panel_under_) {
            const auto& under = *match_panel_under_;
            const auto& pixels = under.shaded ? under.darkened : under.pixels;
            if (under.root.x + under.root.width > kBattlefieldLeft && !pixels.rgb.empty()) {
                const auto shown = painted(under.root);
                scale_blit(
                    paint_target(),
                    pixels,
                    shown.x,
                    shown.y,
                    shown.width,
                    shown.height,
                    0,
                    0,
                    under.root.width,
                    under.root.height
                );
            }
        }
        const auto at = canvas_paint(area->x, area->y);
        scale_blit(
            paint_target(),
            match_dialog_pixels_,
            at.x,
            at.y,
            area->width,
            area->height,
            0,
            0,
            root.width,
            root.height
        );
        // The part of the dialog over the side column is drawn at the
        // canvas's pixels, sampled as the part over the battlefield is, and
        // goes over the HUD layer (compose_match_layers).
        const auto side_width = std::min(match_layout_.left, area->x + area->width) - area->x;
        if (side_width > 0) {
            match_dialog_side_.width = static_cast<uint32_t>(side_width);
            match_dialog_side_.height = static_cast<uint32_t>(area->height);
            match_dialog_side_.rgb.assign(
                static_cast<std::size_t>(side_width) * static_cast<std::size_t>(area->height) * 3U,
                0
            );
            scale_blit(
                match_dialog_side_,
                match_dialog_pixels_,
                0,
                0,
                area->width,
                area->height,
                0,
                0,
                root.width,
                root.height
            );
            match_dialog_side_at_ = {area->x, area->y};
        }
        return;
    }
    if (team_panel_open()) {
        const auto area = painted({root.x, root.y, root.width, root.height});
        scale_blit(
            paint_target(),
            match_hud_cpu_,
            area.x,
            area.y,
            area.width,
            area.height,
            root.x,
            root.y,
            root.width,
            root.height
        );
        return;
    }
    // The preferences' sub-panel lies beside the side column, at its scale.
    // Where the bottom bar sits apart from the chrome, the panel keeps that
    // scale down to its bottom, over the battlefield, as 3.1c's panel stays
    // where it is at a larger screen.
    if (match_menu_session().ingame_panel == IngamePanel::preferences &&
        match_hud_panel_ == oa::data::defs::gui_path(kPreferencesLayout)) {
        const auto rows = preferences_panel_rows();
        if (!preferences_hud_.rgb.empty())
            show_hud_over_battlefield(
                match_world_cpu_,
                preferences_hud_,
                match_layout_,
                rows,
                nullptr,
                rows.y + rows.height
            );
        else
            show_hud_over_battlefield(
                match_world_cpu_, match_hud_cpu_, match_layout_, rows, nullptr
            );
        return;
    }
    for (std::size_t index = 1; index < gadgets.size(); ++index) {
        const auto& area = gadgets[index].common;
        if (area.active != 0 && area.width > 0 && area.height > 0)
            show(area);
    }
}

void Runtime::place_match_panel(uint32_t placement, bool back_tile_face) {
    if (!match_hud_ || match_hud_->layout.gadgets.empty())
        return;
    place_panel(match_hud_->layout, placement);
    match_hud_placement_ = placement;
    match_hud_focus_ = loaded_panel_focus();
    // What the side column shows under the root where the panel does not
    // show over it: the background before the panel's face.
    const auto& placed = match_hud_->layout.gadgets.front().common;
    const auto& background = match_hud_->background;
    match_hud_side_backdrop_ = copy_area(
        renderer::Surface{background.width, background.height, background.rgb},
        {placed.x,
         placed.y,
         std::clamp(kBattlefieldLeft - placed.x, 0, static_cast<int32_t>(placed.width)),
         placed.height}
    );
    if (back_tile_face)
        draw_match_panel_back_tile();
}

void Runtime::draw_match_panel_back_tile() {
    if (!match_hud_ || match_hud_->layout.gadgets.empty())
        return;
    const auto* tile = gaf_sequence(match_hud_->sprites, kBackTile);
    if (tile == nullptr) {
        std::cerr << "match panel '" << match_hud_panel_ << "' has no BackTile face\n";
        return;
    }
    const auto& palette =
        match_hud_->background.palette ? *match_hud_->background.palette : match_hud_->gui_palette;
    draw_back_tile(
        match_hud_->background, match_hud_->layout.gadgets.front().common, *tile, palette
    );
}

bool Runtime::open_match_dialog(
    const std::string& layout,
    uint32_t placement,
    bool back_tile_face,
    std::optional<MatchPanelUnder> under
) {
    if (!load_match_hud_layout(layout))
        return false;
    place_match_panel(placement, back_tile_face);
    match_panel_under_ = std::move(under);
    return true;
}

std::optional<Runtime::MatchPanelUnder> Runtime::panel_under_dialog() {
    if (match_panel_under_)
        return match_panel_under_;
    return capture_match_hud_panel();
}

std::optional<Runtime::MatchPanelUnder> Runtime::capture_match_hud_panel() {
    if (!match_ || !selected_tnt_ || !match_hud_ || match_hud_->layout.gadgets.empty())
        return std::nullopt;
    render_match_surface();
    const auto& root = match_hud_->layout.gadgets.front().common;
    MatchPanelUnder under;
    under.root = {root.x, root.y, root.width, root.height};
    // Only the part of the root on the HUD layer, as the panel showed only
    // that part.
    under.pixels = copy_area(
        match_hud_cpu_,
        {root.x,
         root.y,
         std::clamp(static_cast<int32_t>(match_hud_cpu_.width) - root.x, 0, int32_t{root.width}),
         std::clamp(static_cast<int32_t>(match_hud_cpu_.height) - root.y, 0, int32_t{root.height})}
    );
    return under;
}

void Runtime::keep_panel_below_darkened() {
    if (match_finished_)
        return;
    // Over the running game the panel below is the side column's alone.
    if (!match_paused_)
        match_panels_below_.clear();
    auto below = capture_match_hud_panel();
    if (!below)
        return;
    below->shaded = true;
    below->darkened = below->pixels;
    renderer::shade_panel_below(
        below->darkened,
        0,
        0,
        static_cast<int>(below->darkened.width),
        static_cast<int>(below->darkened.height),
        match_palette_,
        display_.context.shade_table
    );
    match_panels_below_.push_back(std::move(*below));
}

void Runtime::compose_panels_below(renderer::Surface& hud) {
    if (match_panels_below_.empty() || !match_paused_ || match_finished_ || !match_hud_ ||
        match_hud_->layout.gadgets.empty())
        return;
    // A placed dialog keeps its own pixels first (compose_match_dialog);
    // any other HUD panel lies over them.
    std::optional<oa::ui::display_layout::Rect> over;
    if (match_hud_placement_ == 0) {
        const auto& top = match_hud_->layout.gadgets.front().common;
        over = oa::ui::display_layout::Rect{top.x, top.y, top.width, top.height};
    }
    // The layer keeps the rows of a unit's page past 480 kept under the
    // panel, so that the side column shows the page whole, as it grows for
    // the page itself (fit_match_build_page).
    auto rows = static_cast<int64_t>(hud.height);
    for (const auto& below : match_panels_below_)
        rows = std::max<int64_t>(rows, int64_t{below.root.y} + below.darkened.height);
    if (rows > static_cast<int64_t>(hud.height)) {
        hud.rgb.resize(
            static_cast<std::size_t>(hud.width) * static_cast<std::size_t>(rows) * 3U, 0
        );
        hud.height = static_cast<uint32_t>(rows);
    }
    for (const auto& below : match_panels_below_)
        for (int32_t row = 0; row < static_cast<int32_t>(below.darkened.height); ++row)
            for (int32_t column = 0; column < static_cast<int32_t>(below.darkened.width);
                 ++column) {
                const int32_t x = below.root.x + column;
                const int32_t y = below.root.y + row;
                if (x < 0 || y < 0 || x >= static_cast<int32_t>(hud.width) ||
                    y >= static_cast<int32_t>(hud.height) ||
                    (over && x >= over->x && y >= over->y && x < over->x + over->width &&
                     y < over->y + over->height))
                    continue;
                std::copy_n(
                    below.darkened.rgb.begin() +
                        static_cast<std::ptrdiff_t>(rgb_offset(below.darkened, column, row)),
                    3,
                    hud.rgb.begin() + static_cast<std::ptrdiff_t>(rgb_offset(hud, x, y))
                );
            }
}

void Runtime::compose_match_dialog(renderer::Surface& hud) {
    if (match_hud_placement_ == 0 || !match_hud_ || match_hud_->layout.gadgets.empty())
        return;
    const auto& root = match_hud_->layout.gadgets.front().common;
    const oa::ui::display_layout::Rect dialog_root{root.x, root.y, root.width, root.height};
    match_dialog_pixels_ = copy_area(hud, dialog_root);
    const auto& dialog = match_dialog_pixels_;
    // The side column shows what lies under the dialog there: the panels
    // kept under the panel it opened over, and that panel as it showed.
    paste_area(hud, match_hud_side_backdrop_, root.x, root.y, 0);
    compose_panels_below(hud);
    if (match_panel_under_) {
        auto& under = *match_panel_under_;
        if (under.shaded && under.darkened.rgb.empty()) {
            under.darkened = under.pixels;
            renderer::shade_panel_below(
                under.darkened,
                0,
                0,
                static_cast<int>(under.darkened.width),
                static_cast<int>(under.darkened.height),
                match_palette_,
                display_.context.shade_table
            );
        }
        paste_area(
            hud, under.shaded ? under.darkened : under.pixels, under.root.x, under.root.y, 0
        );
    }
    // The dialog stays over the panel under it in the layer right of the
    // side column; over the side column it is drawn at the canvas's pixels
    // (draw_battlefield_panel). On the phone layout, which shows no side
    // column, the whole dialog stays in the layer, for its placed region.
    paste_area(
        hud,
        dialog,
        root.x,
        root.y,
        oa::ui::display_layout::placed_mode(match_layout_) ? 0 : kBattlefieldLeft
    );
}

namespace {

/// Loads a match panel into the gadget engine undrawn, with its records
/// where the panel's GUI file has them: at their root's corner.
///
/// @param[out] panel the gadget engine context to load into
/// @param layout the panel as shown
/// @param name the panel's GUI file
/// @return the loaded panel, or nullptr when it does not load
oa::ui::gui_input::GadgetOwner* load_unplaced_panel(
    oa::ui::gui_input::GadgetPanel& panel, oa::ui::gui_layout::Layout layout, std::string_view name
) {
    const auto& root = layout.gadgets.front().common;
    for (std::size_t index = 1; index < layout.gadgets.size(); ++index) {
        auto& common = layout.gadgets[index].common;
        if (common.width <= 0 || common.height <= 0)
            continue;
        common.x = static_cast<int16_t>(common.x - root.x);
        common.y = static_cast<int16_t>(common.y - root.y);
    }
    oa::ui::gui_input::init_gadget_panel(panel);
    return oa::ui::gui_input::load_panel(panel, layout, name, panel_flag::no_draw);
}

} // namespace

int32_t Runtime::loaded_panel_focus() const {
    if (!match_hud_ || match_hud_->layout.gadgets.empty())
        return -1;
    oa::ui::gui_input::GadgetPanel panel;
    const auto* owner = load_unplaced_panel(panel, match_hud_->layout, match_hud_panel_);
    return owner != nullptr ? owner->focus : -1;
}

std::optional<oa::ui::display_layout::Rect> Runtime::placed_panel_area() const {
    if (match_hud_placement_ == 0 || !match_paused_ || match_finished_ || !match_hud_ ||
        match_hud_->layout.gadgets.empty() || match_layout_.scale <= 0.0)
        return std::nullopt;
    // On the phone layout the dialog is the sheet region placed for it.
    if (oa::ui::display_layout::placed_mode(match_layout_)) {
        const std::size_t count = std::min<std::size_t>(
            match_layout_.placed_count, oa::ui::display_layout::kMaxPlacedRegions
        );
        for (std::size_t index = count; index > 0; --index)
            if (match_layout_.placed[index - 1].role == oa::ui::display_layout::RegionRole::sheet)
                return match_layout_.placed[index - 1].canvas;
        return std::nullopt;
    }
    const auto& root = match_hud_->layout.gadgets.front().common;
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
        match_hud_placement_ | panel_flag::first_draw,
        match_layout_.width,
        match_layout_.height,
        match_layout_.left
    );
    return oa::ui::display_layout::Rect{x, y, width, height};
}

void Runtime::open_match_options_page(ui::OptionsPanel which) {
    auto& session = match_menu_session();
    auto& context = session.options;
    bind_options_context();
    const auto file = oa::data::defs::gui_path(ui::options_panel_file(which, true));
    oa::ui::gui_layout::Layout sub;
    try {
        auto parsed = oa::ui::gui_layout::parse(
            assets_.read(file).bytes, oa::ui::gui_layout::game_translation_lookup()
        );
        if (!parsed.ok() || parsed.layout->gadgets.empty())
            throw std::runtime_error(parsed.error ? parsed.error->message : file + " has no panel");
        sub = std::move(*parsed.layout);
    } catch (const std::exception& error) {
        status_ = "options panel unavailable: " + std::string(error.what());
        std::cerr << status_ << '\n';
        return;
    }
    // The in-match visuals column has no screen-size control of its
    // own. The three from VISUALS.GUI are the same control, listed
    // and applied by the same path as the settings.
    if (which == ui::OptionsPanel::visuals) {
        try {
            auto visuals = oa::ui::gui_layout::parse(
                assets_.read(oa::data::defs::gui_path("VISUALS.GUI")).bytes,
                oa::ui::gui_layout::game_translation_lookup()
            );
            if (visuals.ok() && visuals.layout)
                append_screen_size_controls(sub, *visuals.layout);
            else
                std::cerr << "options panel: VISUALS.GUI was not shown: "
                          << (visuals.error ? visuals.error->message : "no panel") << '\n';
        } catch (const std::exception& error) {
            // The column still opens; it draws no screen-size control.
            std::cerr << "options panel: the screen-size controls were not shown: " << error.what()
                      << '\n';
        }
    }
    if (!load_match_hud_layout(oa::data::defs::gui_path(kPreferencesLayout))) {
        status_ = "options panel unavailable: PREFS.GUI did not load";
        std::cerr << status_ << '\n';
        return;
    }
    widget_gaf_frames_.clear();
    widget_text_stages_.clear();
    auto& layout = match_hud_->layout;
    auto& panel = session.panel;
    session.kind = which;
    panel_from_widgets(panel, layout, widget_text_stages_);
    panel_relative_to_root(panel);
    ui::options_prepare_realtime_panel(panel, context);
    ui::Panel loaded;
    ui::panel_load_layout(loaded, sub);
    ui::options_merge_realtime_panel(panel, loaded);
    const auto first = layout.gadgets.size();
    append_merged_records(layout, panel, sub);
    // The merge draws the panel as its first draw, which binds the
    // sub-panel's sliders: the sliders' handlers then read the
    // positions the bound bars have.
    bind_hud_scrolls(first);
    if (auto* scrolls = hud_scrolls())
        for (const auto& bar : scrolls->bars)
            if (bar.gadget < ui::kPanelControls) {
                auto& slider = panel.controls[bar.gadget].slider;
                slider.range = bar.bar.range;
                slider.knob_size = bar.bar.knob_size;
                slider.knob = bar.bar.knob;
            }
    for (auto index = first; index < layout.gadgets.size(); ++index) {
        const auto* button =
            std::get_if<oa::ui::gui_layout::ButtonFields>(&layout.gadgets[index].fields);
        match_hud_states_.push_back(
            button != nullptr ? MatchGadgetState{button->status, button->grayed_out}
                              : MatchGadgetState{}
        );
    }
    // The sub-panel's own art, as PREFS.GAF is the panel's:
    // MUSICRT.GAF gives CDPREV/CDSTOP/CDPLAY/CDNEXT their frames.
    append_gaf_file(match_hud_->sprites, "anims/" + fs::path(file).stem().string() + ".GAF");
    // Its buttons take the size of the frames they draw, keeping their
    // colours, so each is hit where it is drawn.
    renderer::fit_buttons_to_frames(*match_hud_, first);
    // The sub-panel's picture is its image record, looked up in
    // PREFS.GAF (the merged panel's own) and then the shared GAF.
    oa::formats::gaf::Archive panel_art;
    append_gaf_file(
        panel_art,
        "anims/" + fs::path(oa::data::defs::gui_path(kPreferencesLayout)).stem().string() + ".GAF"
    );
    const auto& palette =
        match_hud_->background.palette ? *match_hud_->background.palette : match_hud_->gui_palette;
    for (auto index = first; index < layout.gadgets.size(); ++index) {
        const auto& common = layout.gadgets[index].common;
        if (common.type != oa::ui::gui_layout::GadgetType::image || common.active == 0)
            continue;
        const auto* sequence = find_sequence(panel_art, common.name);
        if (sequence == nullptr)
            sequence = find_sequence(match_hud_->sprites, common.name);
        if (sequence == nullptr || sequence->frames.empty())
            std::cerr << "options panel: " << file << " has no picture named " << common.name
                      << '\n';
    }
    draw_image_records(
        match_hud_->background, layout.gadgets, first, panel_art, match_hud_->sprites, palette
    );
    switch (which) {
    case ui::OptionsPanel::sound:
        ui::options_enter_sound(panel, context);
        break;
    case ui::OptionsPanel::visuals:
    case ui::OptionsPanel::select_video_mode:
        ui::options_enter_visuals(panel, context, false);
        break;
    case ui::OptionsPanel::speeds:
        ui::options_enter_speeds(panel, context);
        break;
    case ui::OptionsPanel::music:
        ui::options_enter_music(panel, context);
        music_panel_entered(panel, context);
        break;
    case ui::OptionsPanel::tabs:
        ui::options_enter_tabs(panel, context);
        break;
    }
    panel_to_widgets(panel, layout, widget_gaf_frames_, widget_text_stages_);
    // Each sub-panel loader ends by giving its labels the shadow bit.
    oa::ui::gui_input::mark_label_shadows(layout.gadgets);
    // The page takes the place of the turned-over picture.
    if (context.host.release_lightbar != nullptr)
        context.host.release_lightbar(context.host.context);
}

void Runtime::activate_pause_gadget(std::string_view name) {
    auto& session = match_menu_session();
    auto& panel = session.panel;
    if (!match_hud_)
        return;
    if (team_panel_open()) {
        click_team_panel(name);
        return;
    }
    if (session.ingame_panel == IngamePanel::preferences) {
        auto& context = session.options;
        bind_options_context();
        auto& layout = match_hud_->layout;
        const auto index = ui::panel_find(panel, name);
        if (index < 0 || static_cast<std::size_t>(index) >= layout.gadgets.size())
            return;
        if (!match_)
            return;
        // The speed they show is the match's own, read afresh for each click:
        // another player's machine may have set it since they opened.
        preferences_.game_speed = match_->state().game.requested_speed;
        preferences_.current_game_speed = match_->state().game.current_speed;
        // The match takes the preferences' option fields at once, as they
        // are its own, and the speed when the click put another back (UNDO,
        // RESTORE, Cancel), which is written as it is; the GAME slider sets
        // the running speed itself.
        const auto apply_to_match = [this] {
            auto& game = match_->state().game;
            seed_match_options(game);
            if (game.requested_speed != preferences_.game_speed ||
                game.current_speed != preferences_.current_game_speed) {
                game.requested_speed = preferences_.game_speed;
                game.current_speed = preferences_.current_game_speed;
                match_timing_.requested_rate = game.requested_speed;
                match_timing_.actual_rate = game.current_speed;
            }
        };
        panel.selected = index;
        auto& control = panel.controls[static_cast<std::size_t>(index)];
        const auto& gadget = layout.gadgets[static_cast<std::size_t>(index)];
        if (control.type == ui::ControlType::slider) {
            // A slider is never clicked: its bar and arrows take the pointer
            // (preferences_bar_moved).
            panel.selected = ui::kNoSelection;
            return;
        }
        control.stage = oa::ui::gui_input::released_button_stage(gadget, control.stage);
        auto action = ui::OptionsAction::none;
        switch (session.kind) {
        case ui::OptionsPanel::sound:
            action = ui::options_on_sound_click(panel, context);
            break;
        case ui::OptionsPanel::visuals:
        case ui::OptionsPanel::select_video_mode:
            action = ui::options_on_visuals_click(panel, context);
            break;
        case ui::OptionsPanel::speeds:
            action = ui::options_on_speeds_click(panel, context);
            break;
        case ui::OptionsPanel::tabs:
        case ui::OptionsPanel::music:
            if (session.kind == ui::OptionsPanel::music && music_panel_clicked(panel, context))
                break;
            action = ui::options_on_tab_click(panel, context);
            break;
        }
        panel_to_widgets(panel, layout, widget_gaf_frames_, widget_text_stages_);
        // Each tab loads PREFS.GUI afresh, widens it and merges the tab's
        // in-game sub-panel into it, beside the tabs over the battlefield.
        const auto open_sub_panel = [this](ui::OptionsPanel which) {
            open_match_options_page(which);
        };
        switch (action) {
        case ui::OptionsAction::open_sound:
            open_sub_panel(ui::OptionsPanel::sound);
            break;
        case ui::OptionsAction::open_visuals:
            open_sub_panel(ui::OptionsPanel::visuals);
            break;
        case ui::OptionsAction::open_speeds:
            open_sub_panel(ui::OptionsPanel::speeds);
            break;
        case ui::OptionsAction::open_music:
            open_sub_panel(ui::OptionsPanel::music);
            break;
        case ui::OptionsAction::reload:
            open_sub_panel(session.kind);
            break;
        case ui::OptionsAction::close_saved:
        case ui::OptionsAction::close_restored:
            apply_to_match();
            leave_options_screen();
            return;
        case ui::OptionsAction::none:
            break;
        }
        apply_to_match();
        render_match_surface();
        return;
    }
    panel_from_widgets(panel, match_hud_->layout, widget_text_stages_);
    panel.selected = ui::panel_find(panel, name);
    // A click steps a multi-stage button before the panel handler runs.
    if (const auto index = static_cast<std::size_t>(panel.selected);
        panel.selected != ui::kNoSelection && index < match_hud_->layout.gadgets.size()) {
        auto& control = panel.controls[index];
        control.stage = oa::ui::gui_input::released_button_stage(
            match_hud_->layout.gadgets[index], control.stage
        );
    }
    auto& context = session.ingame;
    context.host = {};
    context.host.context = this;
    context.host.play_sound = [](void* host, const char* sound) {
        static_cast<Runtime*>(host)->play_ui_sound(sound, 0);
    };
    context.host.disc_present = [](void* host) {
        auto& runtime = *static_cast<Runtime*>(host);
        const auto disc =
            runtime.campaign_mission_ ? menu::Disc::campaign : menu::Disc::multiplayer;
        return runtime.find_disc(disc) != 0;
    };
    context.host.refresh_archives = [](void* host) {
        static_cast<Runtime*>(host)->refresh_disc_archives();
    };
    context.host.show_message = [](void* host, const char* text, int32_t width) {
        static_cast<Runtime*>(host)->show_frontend_message(
            text, width, entry::message_show_ok, entry::message_fit_width
        );
    };
    context.host.wrap_text = [](void* host,
                                const char* text,
                                int32_t width,
                                char* out,
                                std::size_t capacity) -> std::size_t {
        auto& runtime = *static_cast<Runtime*>(host);
        oa::ui::gui_input::GadgetPanel measure;
        measure.host.context = &runtime.match_hud_->font;
        // Measured as the name is drawn, in the modern fonts when they show
        // game text.
        measure.host.text_width = [](void* font, const void*, const char* value) {
            return oa::ui::frontend_renderer::measure_fnt_game_text(
                *static_cast<const oa::formats::fnt::Font*>(font), value, true
            );
        };
        const auto wrapped = oa::ui::gui_input::wrap_text(measure, text, width, -1);
        if (capacity == 0)
            return 0;
        // What fits of the wrapped text, less a UTF-8 character the cut
        // would split.
        const std::size_t kept = oa::base::text::whole_character_bytes(wrapped, capacity - 1);
        std::memcpy(out, wrapped.data(), kept);
        out[kept] = '\0';
        return kept;
    };
    context.preferences = &preferences_;
    context.session = ingame_session(
        campaign_mission_, (current_extension_state() & extension_state::multiplayer) != 0
    );
    context.in_game = true;
    context.return_label = return_label_;
    context.leave_question = profile_leave_question(mod_profile());
    // Opens EXITMENU or YESORNO over the in-game menu: 3.1c centres each
    // right of the HUD strip over the BackTile face, as neither names a
    // picture of its own. The exit menu darkens the menu under it; the
    // confirmation darkens nothing, and as the exit menu closes before it
    // opens the menu shows as drawn again.
    const auto show = [this, &session](const std::string& layout, IngamePanel which, bool shade) {
        auto under = panel_under_dialog();
        if (under)
            under->shaded = shade;
        if (!open_match_dialog(layout, panel_flag::beside_hud, true, std::move(under)))
            return false;
        session.ingame_panel = which;
        panel_from_widgets(session.panel, match_hud_->layout, widget_text_stages_);
        return true;
    };
    const auto back_to_options = [this, &session] {
        session.ingame_panel = IngamePanel::options;
        show_match_pause_menu();
    };
    auto action = ui::IngameAction::none;
    switch (session.ingame_panel) {
    case IngamePanel::options:
        action = ui::ingame_on_options_click(panel, context);
        break;
    case IngamePanel::exit_menu:
        action = ui::ingame_on_exit_menu_click(panel, context);
        break;
    case IngamePanel::exit_confirm:
        action = ui::ingame_on_exit_confirm_click(panel, context);
        break;
    case IngamePanel::restart:
        action = ui::ingame_on_restart_click(panel, context);
        break;
    case IngamePanel::game_settings:
        action = ui::ingame_on_game_settings_click(panel, context);
        break;
    case IngamePanel::preferences:
        // The preferences took their clicks above and returned.
        break;
    }
    switch (action) {
    case ui::IngameAction::none:
        if (session.ingame_panel == IngamePanel::options && name == "OK")
            resume_match_pause();
        if (session.ingame_panel == IngamePanel::restart) {
            panel_to_widgets(panel, match_hud_->layout, widget_gaf_frames_, widget_text_stages_);
            render_match_surface();
        }
        return;
    case ui::IngameAction::closed:
        // A close request's confirmation goes back to the running match, or
        // to the menu it was asked over.
        if (session.close_confirm && !session.close_confirm_from_menu)
            resume_match_pause();
        else
            back_to_options();
        return;
    case ui::IngameAction::open_options:
        options_parent_ = Screen::match;
        enter_options_panel();
        return;
    case ui::IngameAction::open_load_game:
        // The load-game overlay registered below fills the dialog.
        options_parent_ = Screen::match;
        load(Screen::load_game);
        return;
    case ui::IngameAction::open_save_game:
        open_save_dialog(Screen::match);
        return;
    case ui::IngameAction::open_briefing:
        show_in_game_briefing();
        return;
    case ui::IngameAction::open_game_settings:
        open_game_settings_sheet();
        return;
    case ui::IngameAction::open_help:
        open_help();
        return;
    case ui::IngameAction::open_exit_menu:
        if (show(oa::data::defs::gui_path("EXITMENU.GUI"), IngamePanel::exit_menu, true)) {
            ui::ingame_enter_exit_menu(session.panel, context);
            panel_to_widgets(
                session.panel, match_hud_->layout, widget_gaf_frames_, widget_text_stages_
            );
            render_match_surface();
        }
        return;
    case ui::IngameAction::open_exit_confirm:
        if (show(oa::data::defs::gui_path("YESORNO.GUI"), IngamePanel::exit_confirm, false)) {
            ui::ingame_enter_exit_confirm(session.panel, context);
            panel_to_widgets(
                session.panel, match_hud_->layout, widget_gaf_frames_, widget_text_stages_
            );
            match_hud_focus_ = ui::panel_find(session.panel, ui::kExitConfirmDefault);
            render_match_surface();
        }
        return;
    case ui::IngameAction::open_restart:
        open_restart_dialog();
        return;
    case ui::IngameAction::restart_mission:
        restart_match();
        return;
    case ui::IngameAction::return_to_main_menu:
        session.ingame_panel = IngamePanel::options;
        leave_match();
        load(Screen::main_menu);
        return;
    case ui::IngameAction::leave_game:
        // The run ends here rather than through a quit event, which a match
        // would answer with this confirmation again.
        session.ingame_panel = IngamePanel::options;
        session.close_confirm = false;
        leave_match();
        quit_application(nullptr);
        return;
    default:
        return;
    }
}

bool Runtime::return_to_match_for_close() {
    if (!match_ || match_finished_)
        return false;
    // The preferences a match opens stay on the match screen; only the load
    // and save pages leave it.
    const bool over_match_page = options_parent_ == Screen::match && screen_ == Screen::load_game;
    if (over_match_page) {
        if (save_dialog_open())
            close_save_dialog();
        else
            leave_options_screen();
    } else if (screen_ == Screen::briefing && briefing_from_pause_)
        click_briefing_gadget("OK");
    else
        return false;
    return screen_ == Screen::match;
}

void Runtime::escape_match_menu() {
    const auto& session = match_menu_session();
    // The surrender confirmation, asked from the menu or by closing the
    // window, answers Escape as No.
    if (session.ingame_panel == IngamePanel::exit_confirm && !team_panel_open()) {
        activate_pause_gadget(ui::kExitConfirmDefault);
        return;
    }
    // The preferences take Escape as their Escape default, PREV.
    if (session.ingame_panel == IngamePanel::preferences && match_hud_ &&
        match_hud_panel_ == oa::data::defs::gui_path(kPreferencesLayout) &&
        !match_hud_->layout.gadgets.empty()) {
        const auto* root = std::get_if<oa::ui::gui_layout::PanelFields>(
            &match_hud_->layout.gadgets.front().fields
        );
        activate_pause_gadget(
            root != nullptr && !root->escape_default.empty()
                ? std::string_view(root->escape_default)
                : std::string_view("PREV")
        );
        return;
    }
    resume_match_pause();
}

bool Runtime::match_question_open() const {
    return match_ && !match_finished_ && match_paused_ &&
           match_menu_session().ingame_panel == IngamePanel::exit_confirm && !team_panel_open();
}

bool Runtime::enter_match_menu() {
    if (match_menu_session().ingame_panel != IngamePanel::exit_confirm || team_panel_open())
        return false;
    activate_pause_gadget(ui::kExitConfirmDefault);
    return true;
}

bool Runtime::press_match_panel_key(const SDL_KeyboardEvent& key) {
    namespace gui_input = oa::ui::gui_input;
    if (!match_ || match_finished_ || !match_paused_ || !match_hud_ ||
        match_hud_->layout.gadgets.empty())
        return false;
    const bool confirming =
        match_menu_session().ingame_panel == IngamePanel::exit_confirm && !team_panel_open();
    // The in-game menu and the tab menu give the panels the keyboard. The
    // surrender confirmation a close request opens over the running match
    // has no keyboard focus, but takes the keys its buttons show, as the
    // confirmation asked from the menu does.
    const bool keyboard =
        match_panels_keyboard_ || match_hud_panel_ == oa::data::defs::gui_path(kPreferencesLayout);
    if (!keyboard && !confirming)
        return false;
    namespace bindings = oa::ui::key_bindings;
    const auto down = [](const bindings::ActionEvent* fired) {
        return fired != nullptr && fired->phase == bindings::Phase::pressed;
    };
    const bool accept = down(fired_action("ui.menu-accept"));
    const bool cancel = down(fired_action("ui.menu-cancel"));
    const bool press = down(fired_action("ui.menu-press"));
    const bool menu_key = accept || cancel || press;
    const auto panel_events = match_action_events();
    bool gui_pressed = false;
    for (const bindings::ActionEvent& fed : panel_events) {
        if (fed.phase == bindings::Phase::pressed && fed.action.starts_with("gui.")) {
            gui_pressed = true;
            break;
        }
    }
    // Keys with Ctrl, Alt or the system key down type no character. A menu or
    // button action that was moved onto such a chord still answers.
    const bool modified = (key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0;
    if (modified && !menu_key && !gui_pressed)
        return false;
    // The surrender question answers Enter and Escape as No, outside this panel.
    if (confirming && (accept || cancel))
        return false;
    // Without the keyboard, Space presses nothing.
    if (!keyboard && press)
        return false;
    const auto& gadgets = match_hud_->layout.gadgets;
    // Enter, Escape and Space go to the panel's key dispatch first.
    if (menu_key) {
        gui_input::GadgetPanel panel;
        auto* owner = load_unplaced_panel(panel, match_hud_->layout, match_hud_panel_);
        if (owner == nullptr)
            return false;
        owner->focus =
            match_hud_focus_ > 0 && static_cast<std::size_t>(match_hud_focus_) < gadgets.size()
                ? match_hud_focus_
                : oa::ui::gui_layout::kNoGadget;
        const auto code = accept   ? gui_input::key_code::enter
                          : cancel ? gui_input::key_code::escape
                                   : gui_input::key_code::space;
        const auto left = gui_input::dispatch_key(panel, code);
        if (panel.activated != oa::ui::gui_layout::kNoGadget) {
            activate_match_hud(static_cast<std::size_t>(panel.activated));
            return true;
        }
        if (left == 0)
            return true;
    }
    // A key the dispatch leaves presses the first active, ungreyed button
    // whose gui action this key started. A grayed button takes no key and
    // passes it on. The typed character is kept so it reaches no chat line
    // and no marker.
    const std::string shown = shown_hud_stem();
    if (!shown.empty()) {
        for (const oa::ui::key_bindings::ActionEvent& fed : panel_events) {
            if (fed.phase != oa::ui::key_bindings::Phase::pressed ||
                !fed.action.starts_with("gui."))
                continue;
            for (std::size_t gadget_index = 1; gadget_index < gadgets.size(); ++gadget_index) {
                const auto& gadget = gadgets[gadget_index];
                const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
                if (gadget.common.active == 0 || button == nullptr || button->grayed_out)
                    continue;
                if (gui_action_id(shown, gadget.common.name) != fed.action)
                    continue;
                if (key.key > 0 && key.key < 0x7F)
                    answered_key_ = static_cast<int32_t>(std::tolower(static_cast<int>(key.key)));
                // Yes on the surrender question holds. Enter and No are answered
                // before this loop, and stay immediate.
                if (confirming && gadget.common.name == "CHOICE1" &&
                    start_yes_hold(static_cast<uint16_t>(key.scancode), false, 0, gadget_index))
                    return true;
                activate_match_hud(gadget_index);
                return true;
            }
        }
    }
    // A caption's own letter is not a chord. It presses nothing while Ctrl,
    // Alt or the system key is down.
    if (modified || shown.empty() || key.key <= 0 || key.key >= 0x7F)
        return false;
    // A caption set while the panel is open gives its button a letter the
    // file never stored, so no binding owns it. While a line is being typed
    // the dispatcher searches only that line's keys, so a button's own letter
    // is not among them either. That letter still presses the button. A
    // binding that already owns the button keeps its key once typing ends.
    const auto typed = std::tolower(static_cast<int>(key.key));
    const bool typing = input_contexts().text_entry;
    for (std::size_t index = 1; index < gadgets.size(); ++index) {
        const auto& gadget = gadgets[index];
        const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
        if (gadget.common.active == 0 || button == nullptr || button->grayed_out ||
            button->quick_key == 0 ||
            std::tolower(static_cast<unsigned char>(button->quick_key)) != typed)
            continue;
        if (!typing) {
            const std::string action_id = gui_action_id(shown, gadget.common.name);
            bool owned = false;
            for (const GuiQuickKey& quick : gui_quick_keys_) {
                if (quick.action == action_id) {
                    owned = true;
                    break;
                }
            }
            if (owned)
                continue;
        }
        answered_key_ = typed;
        activate_match_hud(index);
        return true;
    }
    return false;
}

void Runtime::request_match_close() {
    if (match_finished_ || !match_)
        return;
    auto& session = match_menu_session();
    if (match_paused_ && session.close_confirm)
        return;
    // The preferences go back to the in-game menu first, as the pages over
    // the match do, so the confirmation's CHOICE2 returns to that menu.
    if (match_preferences_open())
        leave_options_screen();
    const bool from_menu = match_paused_;
    // The confirmation opens over the panel on screen, darkening nothing: a
    // panel the exit menu darkened stays so under it.
    auto under = panel_under_dialog();
    forget_team_panel();
    match_paused_ = true;
    match_command_ = MatchCommand::none;
    pending_build_type_ = 0;
    if (!open_match_dialog(
            oa::data::defs::gui_path("YESORNO.GUI"), panel_flag::beside_hud, true, std::move(under)
        )) {
        // Without the confirmation the request is answered as a plain quit.
        std::cerr << "YESORNO.GUI unavailable; closing without confirmation\n";
        leave_match();
        quit_application(nullptr);
        return;
    }
    session.ingame_panel = IngamePanel::exit_confirm;
    session.close_confirm = true;
    session.close_confirm_from_menu = from_menu;
    auto& context = session.ingame;
    context.host = {};
    context.session = ingame_session(
        campaign_mission_, (current_extension_state() & extension_state::multiplayer) != 0
    );
    context.in_game = true;
    context.return_label = return_label_;
    context.leave_question = profile_leave_question(mod_profile());
    panel_from_widgets(session.panel, match_hud_->layout, widget_text_stages_);
    ui::ingame_open_leave_confirm(session.panel, context);
    panel_to_widgets(session.panel, match_hud_->layout, widget_gaf_frames_, widget_text_stages_);
    match_hud_focus_ = ui::panel_find(session.panel, ui::kExitConfirmDefault);
    status_ = "Exit";
    render_match_surface();
}

void Runtime::open_restart_dialog() {
    auto& session = match_menu_session();
    // The exit menu closes first, so the menu under it shows as drawn again.
    auto under = panel_under_dialog();
    if (under)
        under->shaded = false;
    if (!open_match_dialog(
            oa::data::defs::gui_path(kRestartLayout),
            panel_flag::beside_hud,
            false,
            std::move(under)
        ) ||
        match_hud_->layout.gadgets.empty())
        return;
    session.ingame_panel = IngamePanel::restart;
    try {
        draw_panel_backdrop(
            match_hud_->background,
            match_hud_->layout.gadgets.front(),
            oa::ui::decoded::require(
                oa::decode_pcx(assets_.read(kRestartBackdrop).bytes), kRestartBackdrop
            )
        );
    } catch (const std::exception& error) {
        std::cerr << "RESTART backdrop unavailable: " << error.what() << '\n';
    }
    panel_from_widgets(session.panel, match_hud_->layout, widget_text_stages_);
    const auto mission = session.ingame.session == ui::SessionKind::campaign
                             ? bound_mission_title()
                             : skirmish_settings_.map_name;
    ui::ingame_enter_restart(session.panel, session.ingame, mission);
    panel_to_widgets(session.panel, match_hud_->layout, widget_gaf_frames_, widget_text_stages_);
    render_match_surface();
}

void Runtime::open_game_settings_sheet() {
    auto& session = match_menu_session();
    if (!match_)
        return;
    // The sheet darkens the in-game menu under it.
    auto under = panel_under_dialog();
    if (under)
        under->shaded = true;
    if (!open_match_dialog(
            oa::data::defs::gui_path(kGameSettingsLayout),
            panel_flag::beside_hud,
            false,
            std::move(under)
        ) ||
        match_hud_->layout.gadgets.empty())
        return;
    session.ingame_panel = IngamePanel::game_settings;
    auto& layout = match_hud_->layout;
    try {
        draw_panel_backdrop(
            match_hud_->background,
            layout.gadgets.front(),
            oa::ui::decoded::require(
                oa::decode_pcx(assets_.read(kGameSettingsBackdrop).bytes), kGameSettingsBackdrop
            )
        );
    } catch (const std::exception& error) {
        std::cerr << "GAMEOPTIONS backdrop unavailable: " << error.what() << '\n';
    }
    // Its labels and word values in the game's language, as
    // gamedata\translate.tdf gives them.
    ui::GameSettingsSheet sheet;
    ui::ingame_build_game_settings(game_settings_view(), sheet, translation_hook, this);
    const auto& root = layout.gadgets.front().common;
    const auto root_x = root.x;
    const auto root_y = root.y;
    for (std::size_t i = 0; i < sheet.count; ++i) {
        const auto& entry = sheet.entries[i];
        oa::ui::gui_layout::Gadget row;
        row.common.type = oa::ui::gui_layout::GadgetType::label;
        row.common.x = static_cast<int16_t>(root_x + entry.x);
        row.common.y = static_cast<int16_t>(root_y + static_cast<int16_t>(entry.y));
        row.common.width = static_cast<int16_t>(entry.width);
        row.common.height = kSettingsRowHeight;
        row.common.attributes = kSettingsRowAttributes;
        row.common.foreground_color = kSettingsRowColor;
        row.common.active = 1;
        const std::string text(entry.text.data());
        row.fields = oa::ui::gui_layout::LabelFields{text, text, {}};
        layout.gadgets.push_back(std::move(row));
        match_hud_states_.push_back(MatchGadgetState{});
    }
    panel_from_widgets(session.panel, layout, widget_text_stages_);
    render_match_surface();
}

void Runtime::check_game_settings_sheet() {
    const auto require = [](bool ok, const std::string& what) {
        if (!ok)
            throw std::runtime_error("game settings sheet check: " + what);
    };
    const bool paused = match_paused_;
    const auto kept_status = status_;
    open_game_settings_sheet();
    require(
        match_hud_.has_value() && match_menu_session().ingame_panel == IngamePanel::game_settings,
        "MISSION did not open GAMEOPTIONS"
    );
    auto& layout = match_hud_->layout;
    const auto& font = match_hud_->label_font;
    // hattfont11's glyph I is 10 rows high and 3 wide (12 and 5 in hattfont12).
    require(
        font.glyphs['I'] && font.glyphs['I']->height == 10 && font.glyphs['I']->width == 3,
        "the labels are not drawn in hattfont11"
    );
    const auto find_row = [&](std::string_view text) -> oa::ui::gui_layout::Gadget* {
        for (auto& gadget : layout.gadgets) {
            const auto* label = std::get_if<oa::ui::gui_layout::LabelFields>(&gadget.fields);
            if (gadget.common.type == oa::ui::gui_layout::GadgetType::label && label != nullptr &&
                label->text == text)
                return &gadget;
        }
        return nullptr;
    };
    const auto columns = [&](oa::ui::gui_layout::Gadget& row) {
        const auto drawn = renderer::render_screen(*match_hud_, {});
        auto& fields = std::get<oa::ui::gui_layout::LabelFields>(row.fields);
        const auto text = fields.text;
        fields.text.clear();
        const auto blank = renderer::render_screen(*match_hud_, {});
        fields.text = text;
        int first = -1;
        int last = -1;
        for (int x = row.common.x; x < row.common.x + row.common.width; ++x)
            for (int y = row.common.y; y < row.common.y + row.common.height; ++y) {
                const auto offset =
                    (static_cast<std::size_t>(y) * drawn.width + static_cast<std::size_t>(x)) * 3U;
                if (offset + 2 < drawn.rgb.size() &&
                    !std::equal(
                        drawn.rgb.begin() + static_cast<std::ptrdiff_t>(offset),
                        drawn.rgb.begin() + static_cast<std::ptrdiff_t>(offset + 3),
                        blank.rgb.begin() + static_cast<std::ptrdiff_t>(offset)
                    )) {
                    if (first < 0)
                        first = x;
                    last = x;
                }
            }
        return std::pair{first, last};
    };
    for (const char* text : {"Commander Death:", "Starting Locations:"}) {
        auto* row = find_row(text);
        require(row != nullptr, std::string("no row reads ") + text);
        require(
            row->common.width == 0x6e && row->common.attributes == 2,
            std::string(text) + " is not a centred 0x6e-pixel label"
        );
        const auto width = static_cast<int>(oa::formats::fnt::measure_text(font, text));
        const auto [first, last] = columns(*row);
        const int left = row->common.x + (row->common.width - width) / 2;
        require(
            first > row->common.x && last < row->common.x + row->common.width - 1 &&
                first >= left && last < left + width,
            std::string(text) + " is cut short or off centre"
        );
    }
    match_paused_ = paused;
    if (!paused)
        resume_match_pause();
    status_ = kept_status;
    std::cout << "game settings sheet check: the rows are centred in hattfont11 and show "
                 "whole\n";
}

ui::GameSettingsView Runtime::game_settings_view() {
    const oa::World& world = match_->state();
    const oa::Game& game = world.game;
    ui::GameSettingsView view;
    view.session = ingame_session(
        campaign_mission_, (current_extension_state() & extension_state::multiplayer) != 0
    );
    view.commander_rule = static_cast<uint32_t>(game.session_rules);
    view.mapping_flags = game.visibility_flags;
    view.difficulty = static_cast<uint32_t>(game.difficulty);
    view.difficulty_names = difficulty_names();
    view.max_units = game.units_per_player;
    if (const auto* map = match_map_context())
        view.map_name = map->mission_name;
    if (view.session == ui::SessionKind::multiplayer) {
        const auto host = sim::scenario::host_player_index(world);
        const auto* info =
            host != OA_PLAYER_COUNT ? oa::world_player_info(&world, &game.players[host]) : nullptr;
        if (info != nullptr) {
            view.fixed_locations = (info->options & OA_SETUP_OPTION_FIXED_LOCATIONS) != 0 ? 1 : 0;
            view.cheats_allowed = (info->options & OA_SETUP_OPTION_CHEATS_ALLOWED) != 0;
            view.watching_allowed = (info->options & OA_SETUP_OPTION_WATCHING_ALLOWED) != 0;
            view.starting_metal = static_cast<uint32_t>(info->metal_hundreds) * 100U;
            view.starting_energy = static_cast<uint32_t>(info->energy_hundreds) * 100U;
        }
    } else {
        view.fixed_locations = preferences_.skirmish_location;
        const auto local = static_cast<std::size_t>(game.local_player_index);
        if (local < skirmish_settings_.slots.size()) {
            const auto& slot = skirmish_settings_.slots[local];
            view.starting_metal = static_cast<uint32_t>(slot.metal);
            view.starting_energy = static_cast<uint32_t>(slot.energy);
        }
    }
    return view;
}

void Runtime::restart_match() {
    auto& session = match_menu_session();
    RestartRun run{
        this,
        session.ingame.session == ui::SessionKind::campaign,
        {},
        EngineSettingsState::restart_unit_limit(*this)
    };
    ui::RestartHost host;
    host.context = &run;
    host.bound_mission = [](void* context) {
        return restart_ask(context, int32_t{}, [](RestartRun& run) {
            return run.runtime->bound_mission_index();
        });
    };
    host.end_session = [](void* context) {
        restart_do(context, [](RestartRun& run) { run.runtime->leave_match(); });
    };
    host.reload_campaign = [](void* context) {
        restart_do(context, [](RestartRun& run) { run.runtime->reload_campaign_file(); });
    };
    host.bind_mission = [](void* context, int32_t index) {
        return restart_ask(context, false, [index](RestartRun& run) {
            return run.runtime->bind_campaign_mission(index);
        });
    };
    host.select_skirmish_map = [](void* context) {
        restart_do(context, [](RestartRun& run) {
            auto& runtime = *run.runtime;
            const auto& map = runtime.skirmish_settings_.map_name;
            if (runtime.select_map(map) == 0)
                throw std::runtime_error("the skirmish map '" + map + "' is not available");
            // It keeps the map's start markers for the refilled slots; the
            // count itself is not needed.
            std::ignore = runtime.map_player_capacity();
        });
    };
    // A restart plays at the limit the match was started with, whatever the
    // run's limit is now.
    host.apply_roster = [](void* context) {
        restart_do(context, [](RestartRun& run) {
            EngineSettingsState::start_skirmish(*run.runtime, run.units_per_player, false);
        });
    };
    host.enter_frontend = [](void* context, bool in_game) {
        restart_do(context, [in_game](RestartRun& run) {
            auto& runtime = *run.runtime;
            if (!in_game)
                return;
            if (run.campaign)
                runtime.restart_campaign_mission();
            else if (runtime.match_ && !runtime.altitude_sight_blocked_)
                runtime.enter_match_view();
        });
    };
    // Which way the restart went is not needed: a restart that fails leaves
    // no match on screen, and that is reported below.
    std::ignore = ui::ingame_run_restart(session.ingame, state_, host);
    // The restart request, which the in-game menu keeps in place of
    // Game.restart_requested, clears as the restarted game loads.
    session.ingame.restart_requested = false;
    session.ingame_panel = IngamePanel::options;
    if (screen_ == Screen::match && match_)
        return;
    const auto reason = run.error.empty() ? status_ : run.error;
    load(Screen::main_menu);
    status_ = "Restart failed: " + reason;
    std::cerr << status_ << '\n';
}

void Runtime::advance_options_lightbar() {
    // Only the preferences a match opens sweep the in-game menu away; the
    // full-screen options of the frontend set the lightbar up and never draw it.
    auto& session = match_menu_session();
    if (session.ingame_panel != IngamePanel::preferences || options_parent_ != Screen::match ||
        !match_ || !match_hud_ ||
        match_hud_panel_ != oa::data::defs::gui_path(kPreferencesLayout) ||
        session.options.lightbar.active == 0 || options_flip_.rgb.empty() || !session.lightbar_due)
        return;
    session.lightbar_due = false;
    session.lightbar_step = ui::options_lightbar_step(session.options.lightbar);
    const auto& step = session.lightbar_step;
    const auto stamp = static_cast<std::size_t>(ui::kLightbarStampFrame);
    if (step.stamp_lightbar) {
        const auto* bar = gaf_sequence(match_hud_->sprites, "LIGHTBAR");
        if (bar == nullptr)
            bar = gaf_sequence(match_hud_->shared_sprites, "LIGHTBAR");
        if (bar != nullptr && bar->frames.size() > stamp)
            stamp_frame(
                bar->frames[stamp],
                match_hud_->background.palette ? *match_hud_->background.palette
                                               : match_hud_->gui_palette,
                options_flip_
            );
    }
    if (step.play_options_sound) {
        play_ui_sound("Options", 0);
        ++options_lightbar_sounds_;
        // The turned-over picture is the menu opening, not a page. Sound is
        // the first of the four pages, and it shows once the sweep ends.
        open_match_options_page(ui::OptionsPanel::sound);
    }
    // The radar picture is drawn again with the frame, and the game marks
    // the frame drawn. A page opened above has left the match in place.
    if (match_) {
        auto& game = match_->state().game;
        game.radar_blink_flags = static_cast<uint16_t>(
            game.radar_blink_flags | oa::present::world_renderer::radar_flag_redraw
        );
        game.options_lightbar_drawn = 1;
    }
}

void Runtime::draw_options_lightbar() {
    // Only the preferences a match opens sweep the in-game menu away; the
    // full-screen options of the frontend set the lightbar up and never draw it.
    // The sound page, once the sweep has opened it, draws in the menu's place.
    auto& session = match_menu_session();
    if (session.ingame_panel != IngamePanel::preferences || options_parent_ != Screen::match ||
        !match_ || !match_hud_ ||
        match_hud_panel_ != oa::data::defs::gui_path(kPreferencesLayout) ||
        session.options.lightbar.active == 0 || options_flip_.rgb.empty() ||
        match_hud_cpu_.rgb.empty() || match_world_cpu_.rgb.empty())
        return;
    const auto& step = session.lightbar_step;
    if (!step.drawn)
        return;
    // The quad is in 640x480 source space. The side column and the bottom bar
    // show the HUD layer as it is; the columns over the battlefield go onto
    // the world layer at the side column's scale.
    auto& cover = session.lightbar_cover;
    blit_lightbar(match_hud_cpu_, options_flip_, step, cover);
    int32_t right = kBattlefieldLeft - 1;
    int32_t top = kCanvasHeight;
    for (const auto& corner : step.destination) {
        right = std::max(right, corner.x);
        top = std::min(top, corner.y);
    }
    show_hud_over_battlefield(
        match_world_cpu_,
        match_hud_cpu_,
        match_layout_,
        {kBattlefieldLeft, top, right - kBattlefieldLeft + 1, kCanvasHeight - top},
        &cover
    );
}

void Runtime::leave_options_screen() {
    flush_preferences();
    auto& session = match_menu_session();
    session.options_bound = nullptr;
    session.options.lightbar.active = 0;
    session.lightbar_step = {};
    if (options_parent_ == Screen::match && match_ &&
        session.ingame_panel == IngamePanel::preferences) {
        // The preferences close to the in-game menu they were opened over;
        // the match takes their options.
        session.ingame_panel = IngamePanel::options;
        session.options_open = false;
        session.options.realtime_panels = false;
        session.options.hold_game = false;
        options_flip_ = {};
        options_backup_.clear();
        seed_match_options(match_->state().game);
        apply_output_mode();
        options_parent_ = Screen::main_menu;
        show_match_pause_menu();
        return;
    }
    if (options_parent_ == Screen::match && match_) {
        leave_load_game();
        seed_match_options(match_->state().game);
        screen_ = Screen::match;
        match_paused_ = true;
        apply_output_mode();
        if (!load_match_hud_layout(oa::data::defs::gui_path("ARMOPT.GUI")))
            resume_match_pause();
        else
            render_match_surface();
        options_parent_ = Screen::main_menu;
        return;
    }
    load(options_parent_);
    options_parent_ = Screen::main_menu;
}

oa::ui::display_layout::Rect Runtime::preferences_panel_rows() const {
    if (match_menu_session().ingame_panel != IngamePanel::preferences || !match_hud_ ||
        match_hud_panel_ != oa::data::defs::gui_path(kPreferencesLayout) ||
        match_hud_->layout.gadgets.empty())
        return {};
    const auto& root = match_hud_->layout.gadgets.front().common;
    if (root.x + root.width <= kBattlefieldLeft || root.height <= 0)
        return {};
    return {kBattlefieldLeft, root.y, root.x + root.width - kBattlefieldLeft, root.height};
}

void Runtime::place_preferences_rows(renderer::Surface& hud) {
    preferences_hud_ = {};
    // The phone layout shows the panel whole through its placed region, the
    // rows where the panel has them.
    if (oa::ui::display_layout::placed_mode(match_layout_))
        return;
    const auto rows = preferences_panel_rows();
    constexpr int32_t band_top = oa::ui::display_layout::kSourceBottomBarY;
    const auto bottom = rows.y + rows.height;
    if (rows.width <= 0 || bottom <= band_top || match_layout_.scale <= 0.0 ||
        hud.width < static_cast<uint32_t>(kCanvasWidth) ||
        hud.height != static_cast<uint32_t>(kCanvasHeight))
        return;
    const auto scaled = [this](int32_t value) {
        return static_cast<int32_t>(std::lround(static_cast<double>(value) * match_layout_.scale));
    };
    const auto unscaled = [this](int32_t value) {
        return static_cast<int32_t>(std::lround(static_cast<double>(value) / match_layout_.scale));
    };
    // The bottom bar joins the chrome where its source rows are where the
    // chrome's scale puts them: the panel's last rows then show in it.
    if (match_layout_.bottom_bar_y() == scaled(band_top))
        return;
    // Elsewhere each bottom bar row shows the panel's row the chrome's scale
    // puts at that height when the panel reaches it, and else the bar's own
    // picture.
    preferences_hud_ = hud;
    const auto& bar = match_hud_->background;
    for (int32_t row = band_top; row < kCanvasHeight; ++row) {
        const auto panel_row = unscaled(match_layout_.bottom_bar_y() + scaled(row - band_top));
        const bool panel = panel_row >= rows.y && panel_row < bottom && panel_row < kCanvasHeight;
        for (int32_t column = rows.x; column < rows.x + rows.width && column < kCanvasWidth;
             ++column) {
            const auto to =
                (static_cast<std::size_t>(row) * hud.width + static_cast<std::size_t>(column)) * 3U;
            if (panel) {
                const auto from = (static_cast<std::size_t>(panel_row) * hud.width +
                                   static_cast<std::size_t>(column)) *
                                  3U;
                std::copy_n(
                    preferences_hud_.rgb.begin() + static_cast<std::ptrdiff_t>(from),
                    3,
                    hud.rgb.begin() + static_cast<std::ptrdiff_t>(to)
                );
            } else if (
                bar.width == hud.width && bar.height == hud.height && bar.rgb.size() >= to + 3U
            ) {
                std::copy_n(
                    bar.rgb.begin() + static_cast<std::ptrdiff_t>(to),
                    3,
                    hud.rgb.begin() + static_cast<std::ptrdiff_t>(to)
                );
            }
        }
    }
}

bool Runtime::match_preferences_open() const {
    return match_menu_session().ingame_panel == IngamePanel::preferences &&
           options_parent_ == Screen::match;
}

bool Runtime::match_music_panel_open() const {
    return match_preferences_open() && match_menu_session().kind == ui::OptionsPanel::music;
}

void Runtime::forget_match_preferences() {
    auto& session = match_menu_session();
    if (session.ingame_panel != IngamePanel::preferences)
        return;
    flush_preferences();
    session.ingame_panel = IngamePanel::options;
    session.options_bound = nullptr;
    session.options_open = false;
    session.options.realtime_panels = false;
    session.options.hold_game = false;
    session.options.lightbar.active = 0;
    session.lightbar_due = false;
    session.lightbar_step = {};
    session.lightbar_cover.clear();
    options_flip_ = {};
    options_backup_.clear();
    if (options_parent_ == Screen::match)
        options_parent_ = Screen::main_menu;
}

bool Runtime::pause_menu_shown() const {
    return match_paused_ && (!match_finished_ || outcome_over_menu_);
}

bool Runtime::ingame_menu_column_shown() const {
    const auto& session = match_menu_session();
    return screen_ == Screen::match && match_ && !match_finished_ && match_paused_ && match_hud_ &&
           match_hud_panel_ == oa::data::defs::gui_path("ARMOPT.GUI") &&
           session.ingame_panel == IngamePanel::options && !session.close_confirm &&
           !team_panel_open() && oa::ui::frontend_dialogs::dialog_count() == 0;
}

bool Runtime::right_press_beside_menu(float x, float y) const {
    if (screen_ != Screen::match || !match_ || match_finished_ || !match_paused_ || !match_hud_ ||
        match_hud_->layout.gadgets.empty() || match_menu_session().close_confirm ||
        team_panel_open() || oa::ui::frontend_dialogs::dialog_count() != 0 || hovered_ ||
        placed_hud_covers(x, y))
        return false;
    // A press on the panel shown, on a button or on its face, is the panel's.
    if (const auto area = placed_panel_area(); area && x >= static_cast<float>(area->x) &&
                                               y >= static_cast<float>(area->y) &&
                                               x < static_cast<float>(area->x + area->width) &&
                                               y < static_cast<float>(area->y + area->height))
        return false;
    const auto& root = match_hud_->layout.gadgets.front().common;
    const auto point = hud_source_point(x, y);
    return point.x < root.x || point.y < root.y || point.x >= root.x + root.width ||
           point.y >= root.y + root.height;
}

void Runtime::sync_visual_option_widgets() {
    const auto flags = preferences_.graphics_flags;
    set_button_stage("SHADING", (flags & init::preference_flags::shading) != 0 ? 1 : 0);
    set_button_stage("ANTI", (flags & init::preference_flags::anti_alias) != 0 ? 1 : 0);
    set_button_stage("BSHADOWS", (flags & init::preference_flags::feature_shadows) != 0 ? 1 : 0);
}

void Runtime::sync_campaign_option_widgets() {
    set_button_stage("Difficulty", static_cast<uint8_t>(preferences_.difficulty));
}

void Runtime::toggle_graphics_flag(uint16_t mask, std::string_view key) {
    preferences_.graphics_flags ^= mask;
    const auto on = (preferences_.graphics_flags & mask) != 0 ? 1U : 0U;
    write_number(init::general_section, key, on);
    flush_preferences();
    sync_visual_option_widgets();
    rebuild_surface();
}

void Runtime::show_screen_size_in_options() {
    // The options' Screen Size opens on the size the screen is shown at,
    // which DisplaymodeWidth and DisplaymodeHeight then hold: a window's
    // own size, shown as Custom where the display offers no such size;
    // without a window, the size the setting plays at. In a match as well
    // as outside one.
    auto size = EngineSettingsState::screen_size_now(*this);
    if (size == oa::ui::engine_settings::desktop_screen_size)
        size = EngineSettingsState::screen_size_in_effect(
            *this, EngineSettingsState::offered_screen_sizes(*this)
        );
    preferences_.display_width = size.width;
    preferences_.display_height = size.height;
}

void Runtime::bind_options_context() {
    auto& context = match_menu_session().options;
    context.preferences = &preferences_;
    context.state = &state_;
    // Opened from a running match, the preferences are PREFS.GUI in the side
    // column with the in-game (*RT) sub-panels; elsewhere they are the
    // full-screen frontend panels.
    const bool in_match = options_parent_ == Screen::match && screen_ == Screen::match && match_;
    context.in_game = in_match;
    context.minimum_mode_height = view_rules::minimum_mode_height(ui_rules());
    if (!in_match) {
        context.realtime_panels = false;
        context.hold_game = false;
    }
    const bool multiplayer = (current_extension_state() & extension_state::multiplayer) != 0;
    context.session_kind = ingame_session(campaign_mission_, multiplayer);
    // A watcher's game speed slider is locked.
    context.game_speed_locked = (current_extension_state() & extension_state::local_watcher) != 0;
    context.host = {};
    context.host.context = this;
    context.host.play_sound = [](void* host, const char* sound) {
        static_cast<Runtime*>(host)->play_ui_sound(sound, 0);
    };
    context.host.apply_volumes = [](void* host) {
        static_cast<Runtime*>(host)->apply_saved_volumes();
    };
    context.host.play_wave = [](void* host, const char* path) {
        static_cast<Runtime*>(host)->play_wave_file(path);
    };
    context.host.save_options = [](void* host) {
        auto& runtime = *static_cast<Runtime*>(host);
        // RESTORE sets the Screen size setting back to its default, and a
        // Screen Size the player moved to sets it to that size. Each is
        // applied when the options close with OK, in a match as well as
        // outside one. Moving the slider only stores the size shown.
        const auto& options = match_menu_session().options;
        const auto& entry = options.snapshot;
        const auto& preferences = runtime.preferences_;
        const bool moved = preferences.display_width != entry.display_width ||
                           preferences.display_height != entry.display_height;
        if (options.screen_size_restored)
            EngineSettingsState::take_screen_size(
                runtime,
                oa::ui::engine_settings::default_settings(EngineSettingsState::inputs(runtime))
                    .screen_size
            );
        else if (moved)
            EngineSettingsState::take_screen_size(
                runtime,
                {static_cast<uint16_t>(preferences.display_width),
                 static_cast<uint16_t>(preferences.display_height)}
            );
        runtime.save_preferences();
    };
    context.host.reset_screen_size = [](void* host) {
        // RESTORE shows the Screen size setting's default, Desktop on most
        // machines, as the size it shows the screen at, rather than 3.1c's
        // 640x480: the desktop's in full screen, and in a window the
        // window's own, which Desktop leaves as it is.
        auto& runtime = *static_cast<Runtime*>(host);
        const auto setting =
            oa::ui::engine_settings::default_settings(EngineSettingsState::inputs(runtime))
                .screen_size;
        auto size = EngineSettingsState::screen_size_after(runtime, setting);
        if (size == oa::ui::engine_settings::desktop_screen_size)
            size = EngineSettingsState::screen_size_shown(
                runtime, setting, EngineSettingsState::offered_screen_sizes(runtime)
            );
        runtime.preferences_.display_width = size.width;
        runtime.preferences_.display_height = size.height;
    };
    context.host.restore_all = [](void* host) {
        auto& runtime = *static_cast<Runtime*>(host);
        init::restore_all_options(
            runtime.preferences_, runtime.state_, match_menu_session().options.snapshot, runtime
        );
    };
    context.host.restore_sound = [](void* host) {
        auto& runtime = *static_cast<Runtime*>(host);
        init::restore_sound_options(
            runtime.preferences_, match_menu_session().options.snapshot, runtime
        );
    };
    context.host.reset_sound = [](void* host) {
        auto& runtime = *static_cast<Runtime*>(host);
        auto& preferences = runtime.preferences_;
        oa::audio::game_audio::SoundOptions sound{
            preferences.fx_volume,
            preferences.sound_flags,
            preferences.unit_chat,
            static_cast<uint32_t>(runtime.sound_spatial_)
        };
        oa::audio::game_audio::restore_sound_options(sound);
        preferences.fx_volume = sound.fx_volume;
        preferences.sound_flags = sound.sound_flags;
        preferences.unit_chat = sound.unit_sound_volume;
        runtime.sound_spatial_ = static_cast<int32_t>(sound.device_mode_word);
    };
    context.host.set_spatial_sound = [](void* host, bool on) {
        static_cast<Runtime*>(host)->sound_spatial_ = on ? 1 : 0;
    };
    context.host.scan_display_modes = [](void* host, ui::DisplayModeList& list) {
        // The sizes the display offers, as the Screen size setting offers
        // them after Desktop; the game draws them in 8 bits a pixel at any
        // depth the display runs at.
        constexpr int32_t game_bits = 8;
        const auto& runtime = *static_cast<Runtime*>(host);
        list.count = 0;
        for (const auto size : EngineSettingsState::offered_screen_sizes(runtime)) {
            if (static_cast<std::size_t>(list.count) >= list.modes.size())
                break;
            list.modes[static_cast<std::size_t>(list.count++)] =
                ui::DisplayMode{size.width, size.height, game_bits};
        }
        return true;
    };
    if (in_match)
        // The GAME slider sets the running game's speed, as the speed keys
        // do, and the extension hears of it as it does of theirs.
        context.host.set_game_speed = [](void* host, uint16_t speed) {
            auto& runtime = *static_cast<Runtime*>(host);
            if (!runtime.match_)
                return;
            auto& world = runtime.match_->state();
            // The speed set is read back from the Game block below.
            std::ignore = oa::sim::speed::set_speed(
                world, speed, runtime.message_hooks(), runtime.game_speed_range()
            );
            runtime.match_timing_.requested_rate = world.game.requested_speed;
            runtime.match_timing_.actual_rate = world.game.current_speed;
            runtime.preferences_.current_game_speed = world.game.current_speed;
            call_hook_or_report<&Extension::speed_changed>(
                runtime.extension_, runtime.hook_error_report(), runtime, world.game.requested_speed
            );
        };

    // The OPTIONS lightbar: the top panel's own picture as FLIPSURFACE. Over
    // a match that is the in-game menu in the side column, from the HUD
    // image; elsewhere the frontend panel, from the frame.
    context.host.copy_top_panel =
        [](void* host, int32_t* width, int32_t* height, int32_t* y) -> oa_ref32 {
        auto& runtime = *static_cast<Runtime*>(host);
        const bool over_match = runtime.screen_ == Screen::match && runtime.match_hud_ &&
                                !runtime.match_hud_cpu_.rgb.empty();
        const auto& picture = over_match ? runtime.match_hud_cpu_ : runtime.surface_;
        const auto& gadgets =
            over_match ? runtime.match_hud_->layout.gadgets : runtime.resources_.layout.gadgets;
        *width = 0;
        *height = 0;
        *y = 0;
        runtime.options_flip_ = {};
        if (gadgets.empty() || picture.rgb.empty())
            return 0;
        const auto& root = gadgets.front().common;
        if (root.width <= 0 || root.height <= 0)
            return 0;
        runtime.options_flip_ = copy_picture(picture, root.x, root.y, root.width, root.height);
        match_menu_session().lightbar_due = true;
        match_menu_session().lightbar_step = {};
        *width = root.width;
        *height = root.height;
        *y = root.y;
        return kOptionsFlipSurface;
    };
    context.host.create_surface =
        [](void* host, const char*, int32_t width, int32_t height) -> oa_ref32 {
        auto& runtime = *static_cast<Runtime*>(host);
        runtime.options_backup_.assign(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0
        );
        return kOptionsBackupSurface;
    };
    // A tab click or a close frees the lightbar's pictures; it draws no more.
    context.host.release_lightbar = [](void* host) {
        auto& runtime = *static_cast<Runtime*>(host);
        runtime.options_flip_ = {};
        runtime.options_backup_.clear();
        match_menu_session().options.lightbar.active = 0;
    };
    context.host.load_panel = [](void* host, ui::Panel& panel) {
        auto& runtime = *static_cast<Runtime*>(host);
        auto& session = match_menu_session();
        if (session.options.in_game) {
            // PREFS.GUI takes the in-game menu's place in the side column.
            if (!runtime.load_match_hud_layout(oa::data::defs::gui_path(kPreferencesLayout))) {
                panel = {};
                return;
            }
            session.ingame_panel = IngamePanel::preferences;
            runtime.widget_gaf_frames_.clear();
            runtime.widget_text_stages_.clear();
            panel_from_widgets(panel, runtime.match_hud_->layout, runtime.widget_text_stages_);
            panel_relative_to_root(panel);
            return;
        }
        runtime.load(Screen::options);
        panel_from_widgets(panel, runtime.resources_.layout, runtime.widget_text_stages_);
    };
    context.host.load_background = [](void* host, const char* name) {
        // A bitmap that cannot be read throws; whether the backdrop changed
        // is not needed.
        std::ignore = static_cast<Runtime*>(host)->load_named_background(name, false, false, false);
    };
}

void Runtime::enter_options_panel() {
    auto& session = match_menu_session();
    auto& context = session.options;
    const bool in_match = options_parent_ == Screen::match && screen_ == Screen::match && match_;
    if (in_match) {
        // The preferences show the match's own option fields and speed.
        auto& game = match_->state().game;
        take_match_options(game);
        preferences_.game_speed = game.requested_speed;
        preferences_.current_game_speed = game.current_speed;
    }
    bind_options_context();
    show_screen_size_in_options();
    ui::options_open(session.panel, context);
    session.kind = ui::OptionsPanel::tabs;
    session.options_open = true;
    if (in_match) {
        session.options_bound = nullptr;
        if (session.ingame_panel != IngamePanel::preferences) {
            // Without PREFS.GUI the in-game menu stays as it was.
            context.host.release_lightbar(context.host.context);
            context.realtime_panels = false;
            context.hold_game = false;
            session.options_open = false;
            options_parent_ = Screen::main_menu;
            status_ = "Preferences unavailable";
            std::cerr << status_ << '\n';
            render_match_surface();
            return;
        }
        panel_to_widgets(
            session.panel, match_hud_->layout, widget_gaf_frames_, widget_text_stages_
        );
        status_ = "Options";
        render_match_surface();
        return;
    }
    session.options_bound = resources_.layout.gadgets.data();
    panel_to_widgets(session.panel, resources_.layout, widget_gaf_frames_, widget_text_stages_);
    rebuild_surface();
}

void Runtime::options_bar_moved(std::size_t index) {
    if (index >= resources_.layout.gadgets.size() || index >= ui::kPanelControls)
        return;
    auto& session = match_menu_session();
    auto& context = session.options;
    auto& panel = session.panel;
    bind_options_context();
    if (session.options_bound != resources_.layout.gadgets.data() ||
        (session.kind != options_panel_for(screen_) &&
         session.kind != ui::OptionsPanel::select_video_mode)) {
        if (!session.options_open) {
            show_screen_size_in_options();
            ui::options_capture_entry(context);
            session.options_open = true;
        }
        session.kind = options_panel_for(screen_);
        panel_from_widgets(panel, resources_.layout, widget_text_stages_);
        if (session.kind == ui::OptionsPanel::tabs)
            ui::options_enter_tabs(panel, context);
        session.options_bound = resources_.layout.gadgets.data();
    }
    auto& control = panel.controls[index];
    const auto* bar =
        std::get_if<oa::ui::gui_layout::ScrollBarFields>(&resources_.layout.gadgets[index].fields);
    if (control.type != ui::ControlType::slider || bar == nullptr)
        return;
    // The knob moved: the slider's callback reads the value it stands for.
    control.slider.knob = bar->knob_position;
    if (control.on_change != nullptr)
        control.on_change(panel, context);
    panel_to_widgets(panel, resources_.layout, widget_gaf_frames_, widget_text_stages_);
}

void Runtime::preferences_bar_moved(std::size_t index) {
    auto& session = match_menu_session();
    if (!match_ || !match_hud_ || session.ingame_panel != IngamePanel::preferences ||
        index >= match_hud_->layout.gadgets.size() || index >= ui::kPanelControls)
        return;
    auto& panel = session.panel;
    auto& context = session.options;
    bind_options_context();
    auto& layout = match_hud_->layout;
    auto& control = panel.controls[index];
    const auto* bar =
        std::get_if<oa::ui::gui_layout::ScrollBarFields>(&layout.gadgets[index].fields);
    if (control.type != ui::ControlType::slider || bar == nullptr)
        return;
    // The speed they show is the match's own, read afresh: another player's
    // machine may have set it since they opened.
    preferences_.game_speed = match_->state().game.requested_speed;
    preferences_.current_game_speed = match_->state().game.current_speed;
    control.slider.knob = bar->knob_position;
    if (control.on_change != nullptr)
        control.on_change(panel, context);
    panel_to_widgets(panel, layout, widget_gaf_frames_, widget_text_stages_);
    // The match takes the preferences' option fields at once, as they are its
    // own; the GAME slider sets the running speed itself.
    auto& game = match_->state().game;
    seed_match_options(game);
    if (game.requested_speed != preferences_.game_speed ||
        game.current_speed != preferences_.current_game_speed) {
        game.requested_speed = preferences_.game_speed;
        game.current_speed = preferences_.current_game_speed;
        match_timing_.requested_rate = game.requested_speed;
        match_timing_.actual_rate = game.current_speed;
    }
}

void Runtime::activate_options_gadget() {
    if (!hovered_ || *hovered_ >= resources_.layout.gadgets.size())
        return;
    auto& session = match_menu_session();
    auto& context = session.options;
    auto& panel = session.panel;
    bind_options_context();

    if (session.options_bound != resources_.layout.gadgets.data() ||
        (session.kind != options_panel_for(screen_) &&
         session.kind != ui::OptionsPanel::select_video_mode)) {
        if (!session.options_open) {
            show_screen_size_in_options();
            ui::options_capture_entry(context);
            session.options_open = true;
        }
        session.kind = options_panel_for(screen_);
        panel_from_widgets(panel, resources_.layout, widget_text_stages_);
        if (session.kind == ui::OptionsPanel::tabs)
            ui::options_enter_tabs(panel, context);
        session.options_bound = resources_.layout.gadgets.data();
    }

    const auto index = *hovered_;
    auto& gadget = resources_.layout.gadgets[index];
    panel.selected = static_cast<int32_t>(index);
    auto& control = panel.controls[std::min(index, ui::kPanelControls - 1)];
    if (control.type == ui::ControlType::slider) {
        // A slider is never clicked: its bar and arrows take the pointer
        // (options_bar_moved).
        panel.selected = ui::kNoSelection;
        return;
    }
    control.stage = oa::ui::gui_input::released_button_stage(gadget, control.stage);

    auto action = ui::OptionsAction::none;
    switch (session.kind) {
    case ui::OptionsPanel::sound:
        action = ui::options_on_sound_click(panel, context);
        break;
    case ui::OptionsPanel::visuals:
    case ui::OptionsPanel::select_video_mode:
        action = ui::options_on_visuals_click(panel, context);
        break;
    case ui::OptionsPanel::speeds:
        action = ui::options_on_speeds_click(panel, context);
        break;
    case ui::OptionsPanel::tabs:
    case ui::OptionsPanel::music:
        if (session.kind == ui::OptionsPanel::music && music_panel_clicked(panel, context))
            break;
        action = ui::options_on_tab_click(panel, context);
        break;
    }
    panel_to_widgets(panel, resources_.layout, widget_gaf_frames_, widget_text_stages_);
    // Sub-panels load over the tab panel: STARTOPT's records first, then the
    // sub-panel's, offset by the difference of the two panel origins.
    const auto open_panel = [this, &session, &context](ui::OptionsPanel which) {
        const auto sub = std::string(ui::options_panel_file(which, false));
        const auto background = std::string(ui::options_panel_background(which));
        load(screen_for(which));
        try {
            resources_ = renderer::load_screen(
                assets_,
                {oa::data::defs::gui_path("startopt.gui"),
                 "",
                 "palettes/guipal.pal",
                 "anims/commongui.gaf",
                 "anims/commongui.gaf"}
            );
            caption_gaf_pictures("anims/commongui.gaf", resources_.sprites);
            caption_gaf_pictures("anims/commongui.gaf", resources_.shared_sprites);
            // A bitmap that cannot be read throws, as the screen's other
            // files do; whether the backdrop changed is not needed.
            std::ignore = load_named_background(background.c_str(), false, false, false);
            auto parsed = oa::ui::gui_layout::parse(
                assets_.read(oa::data::defs::gui_path(sub)).bytes,
                oa::ui::gui_layout::game_translation_lookup()
            );
            if (!parsed.ok())
                throw std::runtime_error(
                    parsed.error ? parsed.error->message : sub + " parse failed"
                );
            auto& gadgets = resources_.layout.gadgets;
            const auto& extra = parsed.layout->gadgets;
            const auto merged_first = gadgets.size();
            if (!gadgets.empty() && !extra.empty()) {
                const auto dx = extra.front().common.x - gadgets.front().common.x;
                const auto dy = extra.front().common.y - gadgets.front().common.y;
                for (std::size_t index = 1; index < extra.size(); ++index) {
                    auto gadget = extra[index];
                    gadget.common.x = static_cast<int16_t>(gadget.common.x + dx);
                    gadget.common.y = static_cast<int16_t>(gadget.common.y + dy);
                    gadgets.push_back(std::move(gadget));
                }
            }
            // The sub-panel's own art draws its buttons; without it the shared
            // GUI GAF leaves them on its fallback frame. Only the music panels
            // ship one (MUSIC.GAF), and a missing file is remembered and
            // skipped.
            append_gaf_file(resources_.sprites, "anims/" + fs::path(sub).stem().string() + ".GAF");
            // Its buttons take the size of the frames they draw, so each is
            // hit where it is drawn (MUSIC.GUI authors the transport buttons
            // 16x16; MUSIC.GAF draws them 30x15).
            renderer::fit_buttons_to_frames(resources_, merged_first);
        } catch (const std::exception& error) {
            status_ = "options panel unavailable: " + std::string(error.what());
            return;
        }
        // The sub-panel is merged and drawn. Its sliders bind as the first
        // draw would; its buttons are fitted to their frames but keep the
        // authored foreground colour the renderer draws the frame through.
        // Only a loaded panel's first draw clears it.
        bind_frontend_scrolls(oa::data::defs::gui_path("startopt.gui"), "anims/commongui.gaf");
        set_menu_gui(oa::data::defs::gui_path("startopt.gui"));
        add_menu_gui(oa::data::defs::gui_path(sub));
        // load() gave the focus a record of the sub-panel's file alone, which
        // the merged records no longer number so.
        frontend_focus_ = -1;
        widget_gaf_frames_.clear();
        widget_text_stages_.clear();
        session.kind = which;
        auto& panel = session.panel;
        panel_from_widgets(panel, resources_.layout, widget_text_stages_);
        switch (which) {
        case ui::OptionsPanel::sound:
            ui::options_enter_sound(panel, context);
            break;
        case ui::OptionsPanel::visuals:
        case ui::OptionsPanel::select_video_mode:
            ui::options_enter_visuals(panel, context, false);
            break;
        case ui::OptionsPanel::speeds:
            ui::options_enter_speeds(panel, context);
            break;
        case ui::OptionsPanel::music:
            ui::options_enter_music(panel, context);
            music_panel_entered(panel, context);
            break;
        case ui::OptionsPanel::tabs:
            ui::options_enter_tabs(panel, context);
            break;
        }
        panel_to_widgets(panel, resources_.layout, widget_gaf_frames_, widget_text_stages_);
        // Each sub-panel loader ends by giving its labels the shadow bit;
        // the tab panel does not.
        if (which != ui::OptionsPanel::tabs)
            oa::ui::gui_input::mark_label_shadows(resources_.layout.gadgets);
        session.options_bound = resources_.layout.gadgets.data();
    };
    switch (action) {
    case ui::OptionsAction::open_sound:
        open_panel(ui::OptionsPanel::sound);
        break;
    case ui::OptionsAction::open_visuals:
        open_panel(ui::OptionsPanel::visuals);
        break;
    case ui::OptionsAction::open_speeds:
        open_panel(ui::OptionsPanel::speeds);
        break;
    case ui::OptionsAction::open_music:
        open_panel(ui::OptionsPanel::music);
        break;
    case ui::OptionsAction::reload:
        open_panel(session.kind);
        break;
    case ui::OptionsAction::close_saved:
    case ui::OptionsAction::close_restored:
        session.options_open = false;
        apply_output_mode();
        leave_options_screen();
        return;
    case ui::OptionsAction::none:
        break;
    }
    rebuild_surface();
}

void Runtime::activate_campaign_gadget() {
    if (!hovered_ || *hovered_ >= resources_.layout.gadgets.size())
        return;
    const auto name = resources_.layout.gadgets[*hovered_].common.name;
    // The pressed control takes the keyboard focus, so Up and Down leave the
    // lists alone after a button.
    campaign_setup_focus_ = name;
    if (name == "PrevMenu" || name == "PREVMENU") {
        load(Screen::single_player);
        return;
    }
    // The side buttons refill the Campaign list with the chosen side's
    // campaigns and, on Any Mission, the Missions list.
    if (name == "Side0" || name == oa::data::defs::side_name(0)) {
        set_button_status(oa::data::defs::side_name(0), 1);
        set_button_status("Side0", 1);
        preferences_.side = 0;
        write_number(init::general_section, "side", 0);
        flush_preferences();
        discover_campaigns();
        sync_campaign_option_widgets();
        rebuild_surface();
        return;
    }
    if (name == "Side1" || name == oa::data::defs::side_name(1)) {
        set_button_status(oa::data::defs::side_name(1), 1);
        set_button_status("Side1", 1);
        preferences_.side = 1;
        write_number(init::general_section, "side", 1);
        flush_preferences();
        discover_campaigns();
        sync_campaign_option_widgets();
        rebuild_surface();
        return;
    }
    if (name == "Difficulty") {
        preferences_.difficulty = (preferences_.difficulty + 1U) % 3U;
        write_number(init::general_section, "Difficulty", preferences_.difficulty);
        flush_preferences();
        sync_campaign_option_widgets();
        rebuild_surface();
        return;
    }
    if (name == "Start")
        start_campaign_setup();
}

void Runtime::start_campaign_setup() {
    play_ui_sound("BigButton", 0);
    // Start marks every mission unplayed and loads the campaign on the
    // list's selected row; New Campaign binds its first mission, Any
    // Mission the selected one.
    entry::reset_mission_results(state_);
    if (screen_ == Screen::new_campaign)
        load_campaign_missions(selected_campaign_index_);
    show_mission_briefing();
}

} // namespace oa::app
