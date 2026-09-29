// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// In-match HUD layout, pause, outcome and options menus.
#include "oa/data/campaign/campaign_file.hpp"
#include "oa/sim/scenario/commander_rules.hpp"
#include "oa/sim/speed.hpp"
#include "oa/ui/frontend/end_mission.hpp"
#include "oa/ui/frontend/ingame_menu.hpp"
#include "oa/ui/frontend/options.hpp"
#include "oa/ui/frontend/savegame_dialogs.hpp"
#include "oa/formats/fnt.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/gui_input/gadget_panel.hpp"
#include "oa/ui/gui_input.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/ui/screen_registry.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/present/world_renderer/world_camera.hpp"
#include "oa/app/runtime.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

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
constexpr const char* kPreferencesLayout = "guis/PREFS.GUI";

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

constexpr const char* kRestartLayout = "guis/RESTART.GUI";
// RESTART.GUI's art is the top-left of this 640x480 bitmap.
constexpr const char* kRestartBackdrop = "bitmaps/drestart.pcx";
constexpr const char* kGameSettingsLayout = "guis/GAMEOPTIONS.GUI";
constexpr const char* kGameSettingsBackdrop = "bitmaps/gamesettings.pcx";
// The Game Settings sheet centres every row it adds in its column (record attributes 2).
constexpr int32_t kSettingsRowAttributes = 2;
constexpr int16_t kSettingsRowHeight = 0xF;
constexpr uint16_t kSettingsRowColor = 0xF;

// First draw of a beside-HUD panel (flag 0x1000): centred in the source
// frame right of the HUD strip, the records moving with the root.
void place_beside_hud(oa::ui::gui_layout::Layout& layout) {
    if (layout.gadgets.empty())
        return;
    auto& root = layout.gadgets.front().common;
    const auto x =
        static_cast<int16_t>((kCanvasWidth - kBattlefieldLeft - root.width) / 2 + kBattlefieldLeft);
    const auto y = static_cast<int16_t>((kCanvasHeight - root.height) / 2);
    const int dx = x - root.x;
    const int dy = y - root.y;
    for (auto& gadget : layout.gadgets) {
        if (&gadget.common != &root && (gadget.common.width <= 0 || gadget.common.height <= 0))
            continue;
        gadget.common.x = static_cast<int16_t>(gadget.common.x + dx);
        gadget.common.y = static_cast<int16_t>(gadget.common.y + dy);
    }
}

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

// Writes stages, text, activity, gray state and knobs back to the widgets. A
// button's group value becomes its status, so the chosen member of a group
// (the open options tab) shows pressed.
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

// Load-game dialog overlay: lists the SAVEGAME directory beside the user
// preferences through the load-game handlers and fills the list and preview
// text of the built-in LOADGAME.GUI screen. In its save role
// GAMENAME takes the typed name the game is saved under.
struct LoadGameOverlay {
    bool bound = false;
    bool save_role = false;
    oa::ui::gui_layout::Layout layout;
    ui::Panel panel;
    ui::SaveDialogContext saves;
    fs::path root;
    std::optional<oa::formats::fnt::Font> font;
    std::vector<std::string> rows;    // GAMES list text
    ScreenContext* context = nullptr; // valid during a hook
};

LoadGameOverlay& load_overlay() {
    static LoadGameOverlay overlay;
    return overlay;
}

void load_overlay_bind(ScreenContext* ctx, LoadGameOverlay& overlay) {
    overlay.bound = true;
    try {
        auto parsed = oa::ui::gui_layout::parse(ctx->assets->read("guis/loadgame.gui").bytes);
        if (!parsed.ok())
            return;
        overlay.layout = std::move(*parsed.layout);
        overlay.font = oa::formats::fnt::load_gaf(*ctx->assets, "anims/hattfont12.gaf");
    } catch (const std::exception& error) {
        screen_status(ctx, error.what());
        return;
    }
    ui::panel_load_layout(overlay.panel, overlay.layout);
    // Saves are listed where the runtime writes and loads them.
    overlay.root = static_cast<const Runtime*>(ctx->host)->save_game_root();
    auto& saves = overlay.saves;
    saves.files = ui::savegame_host_files(&overlay.root);
    saves.reader = ui::savegame_persist_reader(&overlay.root);
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

// The save dialog's click on a control: CANCEL leaves through the built-in
// handler; a save writes the game and closes the dialog.
int save_overlay_click(ScreenContext* ctx, LoadGameOverlay& overlay, int32_t hit) {
    auto& panel = overlay.panel;
    panel.selected = hit;
    overlay.context = ctx;
    const auto result = ui::savegame_on_save_click(panel, overlay.saves);
    overlay.context = nullptr;
    auto& runtime = *static_cast<Runtime*>(ctx->host);
    switch (result.action) {
    case ui::SaveDialogAction::cancelled:
        return 0;
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

// Typing edits GAMENAME up to its length; Return saves under it and Escape
// leaves the dialog.
int save_overlay_key(ScreenContext* ctx, LoadGameOverlay& overlay, const ScreenInput& input) {
    const auto index = ui::panel_find(overlay.panel, "GAMENAME");
    if (index < 0)
        return 1;
    auto& control = overlay.panel.controls[static_cast<std::size_t>(index)];
    std::string text(ui::control_text(control));
    std::size_t limit = control.text.size() - 1;
    if (const auto* box = std::get_if<oa::ui::gui_layout::TextBoxFields>(
            &overlay.layout.gadgets[static_cast<std::size_t>(index)].fields
        );
        box != nullptr && box->max_characters > 0)
        limit = std::min(limit, static_cast<std::size_t>(box->max_characters));
    if (input.kind == ScreenInputKind::text && input.text != nullptr) {
        for (const char* at = input.text; *at != '\0' && text.size() < limit; ++at)
            if (static_cast<unsigned char>(*at) >= 0x20 && static_cast<unsigned char>(*at) < 0x7f)
                text.push_back(*at);
        ui::set_control_text(control, text);
        return 1;
    }
    if (input.kind != ScreenInputKind::key_down)
        return 1;
    if (input.key == SDLK_BACKSPACE && !text.empty()) {
        text.pop_back();
        ui::set_control_text(control, text);
    } else if (input.key == SDLK_RETURN || input.key == SDLK_KP_ENTER) {
        return save_overlay_click(ctx, overlay, index);
    } else if (input.key == SDLK_ESCAPE) {
        overlay.panel.selected = ui::panel_find(overlay.panel, "CANCEL");
        overlay.context = ctx;
        (void)ui::savegame_on_save_click(overlay.panel, overlay.saves);
        overlay.context = nullptr;
        static_cast<Runtime*>(ctx->host)->close_save_dialog();
    }
    return 1;
}

int load_overlay_event(ScreenContext* ctx, void*) {
    auto& overlay = load_overlay();
    const auto* input = ctx->input;
    if (overlay.bound && overlay.save_role && input != nullptr &&
        (input->kind == ScreenInputKind::text || input->kind == ScreenInputKind::key_down))
        return save_overlay_key(ctx, overlay, *input);
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
    if (overlay.save_role)
        return save_overlay_click(ctx, overlay, hit);
    overlay.context = ctx;
    panel.selected = hit;
    const auto result = ui::savegame_on_load_click(panel, overlay.saves);
    overlay.context = nullptr;
    switch (result.action) {
    case ui::SaveDialogAction::cancelled:
        overlay.bound = false; // the built-in handler leaves the screen
        return 0;
    case ui::SaveDialogAction::load:
        // Starting the save replaces this screen with the match.
        overlay.bound = false;
        static_cast<Runtime*>(ctx->host)->start_saved_game(result.path.data());
        return 1;
    case ui::SaveDialogAction::invalid:
    case ui::SaveDialogAction::refreshed:
        return 1;
    default:
        return 0;
    }
}

void load_overlay_text(
    ScreenContext* ctx, const LoadGameOverlay& overlay, std::string_view text, int32_t x, int32_t y
) {
    auto* surface = ctx->surface;
    if (surface == nullptr || !overlay.font || text.empty())
        return;
    const auto width = surface->width;
    const auto height = surface->height;
    std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height);
    std::vector<uint8_t> coverage(pixels.size());
    const oa::formats::fnt::IndexedSurface target{width, height, width, pixels, coverage};
    (void)oa::formats::fnt::raster_text(target, *overlay.font, text, x, y);
    // The font's indices are shown in the palette the dialog is drawn in.
    const auto& palette = static_cast<const Runtime*>(ctx->host)->screen_palette();
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
}

// GAMENAME is a text box, which the built-in screen leaves empty.
void load_overlay_draw(ScreenContext* ctx, void*) {
    auto& overlay = load_overlay();
    if (!overlay.bound)
        return;
    const auto origin = static_cast<const Runtime*>(ctx->host)->panel_origin();
    const auto* name = ui::panel_control(overlay.panel, "GAMENAME");
    if (const auto rect = load_overlay_rect(overlay, "GAMENAME");
        name != nullptr && name->active != 0 && rect)
        load_overlay_text(
            ctx, overlay, ui::control_text(*name), origin.x + rect->left, origin.y + rect->top
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
    layout.gadgets.front().common.width = panel.controls[0].width;
    if (auto* fields = std::get_if<oa::ui::gui_layout::PanelFields>(&layout.gadgets.front().fields))
        fields->loaded_total_gadgets = static_cast<int16_t>(layout.gadgets.size() - 1U);
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
    if (area.width <= 0 || area.height <= 0 || hud.rgb.empty() || world.rgb.empty() ||
        layout.scale <= 0.0)
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
    (void)overlay_register(registry, &load_game);
    // The match frame draws the lightbar itself (draw_options_lightbar).
    OverlayDesc lightbar{};
    lightbar.name = "options_lightbar";
    lightbar.screen = screen_id(Screen::match);
    lightbar.z = 0;
    lightbar.tick = options_lightbar_tick;
    (void)overlay_register(registry, &lightbar);
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

void Runtime::close_save_dialog() {
    auto& overlay = load_overlay();
    overlay.bound = false;
    overlay.save_role = false;
    auto context = screen_context();
    context.services->set_text_input(context.host, 0);
    leave_options_screen();
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

bool Runtime::load_match_hud_layout(const std::string& layout) {
    // A new panel's scroll bars are bound once it is placed.
    hud_scrolls_ = {};
    hud_scrolls_layout_ = nullptr;
    hud_scrolls_count_ = 0;
    const auto prefix = match_side_prefix();
    const auto panel = prefix == "cor" ? "anims/CORINT.GAF" : "anims/ARMINT.GAF";
    const auto tile = "bitmaps/" + prefix + "guisidetile.pcx";
    try {
        // Command icons live in commongui.gaf; side chrome lives in ARMINT/CORINT.
        match_hud_ = renderer::load_screen(
            assets_, {layout, tile, "palettes/guipal.pal", "anims/commongui.gaf", panel}
        );
    } catch (const std::exception& error) {
        std::cerr << "match HUD '" << layout << "' unavailable: " << error.what() << '\n';
        return false;
    }
    match_hud_panel_ = layout;
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
    append_gaf_file(match_hud_->sprites, "anims/" + stem + ".GAF");
    append_gaf_file(match_hud_->sprites, panel);
    const oa::ui::gui_layout::Gadget* header = nullptr;
    for (const auto& gadget : match_hud_->layout.gadgets) {
        if (gadget.common.name == "HEADER") {
            header = &gadget;
            break;
        }
    }
    // ARMOPT's type-0 panel is named armopt.GUI, not HEADER. Child xpos/ypos
    // still live in that (0,128) well, same as ARMGEN.
    if (header == nullptr) {
        for (const auto& gadget : match_hud_->layout.gadgets) {
            if (gadget.common.type == oa::ui::gui_layout::GadgetType::panel) {
                header = &gadget;
                break;
            }
        }
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
    // HEADER is the type-0 panel at screen (0,128). Child xpos/ypos in ARMGEN
    // and ARMCOMn live in that panel: ARMORDERS ypos=4 is not a screen row.
    // The 0x7e-pixel radar picture, drawn at Game.radar_offset_x and
    // radar_offset_y, occupies the well above.
    if (header != nullptr) {
        const auto origin_x = header->common.x;
        const auto origin_y = header->common.y;
        for (auto& gadget : match_hud_->layout.gadgets) {
            if (&gadget == header)
                continue;
            if (gadget.common.width <= 0 || gadget.common.height <= 0)
                continue;
            gadget.common.x = static_cast<int16_t>(gadget.common.x + origin_x);
            gadget.common.y = static_cast<int16_t>(gadget.common.y + origin_y);
        }
    }
    try {
        // SIDEDATA.TDF font=console for in-game metal/energy numerals.
        match_small_font_ = oa::formats::fnt::load_fnt(assets_, "fonts/CONSOLE.FNT");
    } catch (const std::exception& error) {
        std::cerr << "match CONSOLE.FNT unavailable: " << error.what() << '\n';
        match_small_font_ = small_font_;
    }
    overlay_gaf_sequence(
        match_hud_->background, match_hud_->shared_sprites, "PANELSIDE", 0, 0, icon_palette
    );
    overlay_gaf_sequence(
        match_hud_->background,
        match_hud_->shared_sprites,
        "PANELTOP",
        kBattlefieldLeft,
        0,
        icon_palette
    );
    if (const auto* top = gaf_sequence(match_hud_->shared_sprites, "PANELTOP");
        top && !top->frames.empty())
        panel_top_width_ = static_cast<int>(top->frames.front().width);
    const auto* bottom = gaf_sequence(match_hud_->shared_sprites, "PANELBOT");
    if (bottom != nullptr && !bottom->frames.empty()) {
        overlay_gaf_sequence(
            match_hud_->background,
            match_hud_->shared_sprites,
            "PANELBOT",
            kBattlefieldLeft,
            kCanvasHeight - static_cast<int>(bottom->frames.front().height),
            icon_palette
        );
    }
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
    return true;
}

void Runtime::show_match_orders_page() {
    namespace hud = oa::ui::hud;
    if (!match_)
        return;
    const auto prefix = match_side_name_prefix();
    auto& game = match_->state().game;
    auto state = hud::order_panel_load(game);
    const auto summary = summarize_order_panel(state);
    const oa::Unit* unit = summary.count == 1 ? summary.first : nullptr;
    const auto table = order_panel_table();
    const auto previous = match_hud_panel_;
    match_hud_panel_.clear();
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
    hud::order_panel_store(game, state);
    match_build_page_ = 0;
    status_ = "Orders";
    render_match_surface();
}

void Runtime::show_match_pause_menu() {
    if (match_finished_)
        return;
    match_menu_session().close_confirm = false;
    forget_team_panel();
    match_paused_ = true;
    match_command_ = MatchCommand::none;
    pending_build_type_ = 0;
    if (load_match_hud_layout("guis/ARMOPT.GUI")) {
        auto& session = match_menu_session();
        session.ingame_panel = IngamePanel::options;
        session.ingame.host = {};
        session.ingame.session = ingame_session(
            campaign_mission_, (current_extension_state() & extension_state::multiplayer) != 0
        );
        session.ingame.saved_games_offered = offers_saved_games();
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
    // Resuming over the preferences keeps what they set, as leaving them does.
    if (match_preferences_open())
        leave_options_screen();
    match_menu_session().close_confirm = false;
    match_menu_session().ingame_panel = IngamePanel::options;
    forget_team_panel();
    match_paused_ = false;
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
    if (extension_.outcome_ready != nullptr && !extension_.outcome_ready(extension_.context, *this))
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
    session.env = campaign_object_env();
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
    (void)ui::end_mission_on_click(session.panel, session.context);
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
        options_parent_ = Screen::campaign_end;
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
    if (match_finished_) {
        // No victory or defeat banner is drawn for a watcher.
        if ((current_extension_state() & extension_state::local_watcher) == 0)
            draw_igtitle(
                match_outcome_ == sim::scenario::Outcome::victory ? "igvictory" : "igdefeat"
            );
        return;
    }
    // The pause bit holds any match; a menu holds only a match played on this
    // machine alone, and a shared match's menu shows over the running game.
    const bool pause_bit =
        match_ && (match_->state().game.sim_run_flags & oa::ui::console::kSimRunPaused) != 0;
    const bool shared = (current_extension_state() & extension_state::shared_match) != 0;
    if (pause_bit || (match_paused_ && !shared))
        draw_igtitle("igpaused");
    if (match_paused_) {
        draw_battlefield_panel();
        draw_options_lightbar();
    }
}

void Runtime::draw_battlefield_panel() {
    if (!match_hud_ || match_hud_->layout.gadgets.empty() || match_hud_cpu_.rgb.empty())
        return;
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
    if (team_panel_open() || match_menu_session().ingame_panel == IngamePanel::restart ||
        match_menu_session().ingame_panel == IngamePanel::game_settings) {
        show(root);
        return;
    }
    // The preferences' sub-panel lies beside the side column, at its scale.
    // Where the bottom bar sits apart from the chrome, the panel keeps that
    // scale down to its bottom, over the battlefield, as 3.1c's panel stays
    // where it is at a larger screen.
    if (match_menu_session().ingame_panel == IngamePanel::preferences &&
        match_hud_panel_ == kPreferencesLayout) {
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
        const auto open_sub_panel = [this, &session, &context](ui::OptionsPanel which) {
            const auto file = "guis/" + std::string(ui::options_panel_file(which, true));
            oa::ui::gui_layout::Layout sub;
            try {
                auto parsed = oa::ui::gui_layout::parse(assets_.read(file).bytes);
                if (!parsed.ok() || parsed.layout->gadgets.empty())
                    throw std::runtime_error(
                        parsed.error ? parsed.error->message : file + " has no panel"
                    );
                sub = std::move(*parsed.layout);
            } catch (const std::exception& error) {
                status_ = "options panel unavailable: " + std::string(error.what());
                return;
            }
            if (!load_match_hud_layout(kPreferencesLayout))
                return;
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
            append_gaf_file(
                match_hud_->sprites, "anims/" + fs::path(file).stem().string() + ".GAF"
            );
            // The sub-panel's picture is its image record, looked up in
            // PREFS.GAF (the merged panel's own) and then the shared GAF.
            oa::formats::gaf::Archive panel_art;
            append_gaf_file(
                panel_art, "anims/" + fs::path(kPreferencesLayout).stem().string() + ".GAF"
            );
            draw_image_records(
                match_hud_->background,
                layout.gadgets,
                first,
                panel_art,
                match_hud_->sprites,
                match_hud_->background.palette ? *match_hud_->background.palette
                                               : match_hud_->gui_palette
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
        measure.host.text_width = [](void* font, const void*, const char* value) {
            return static_cast<int32_t>(oa::formats::fnt::measure_text(
                *static_cast<const oa::formats::fnt::Font*>(font), value
            ));
        };
        const auto wrapped = oa::ui::gui_input::wrap_text(measure, text, width, -1);
        return static_cast<std::size_t>(std::snprintf(out, capacity, "%s", wrapped.c_str()));
    };
    context.preferences = &preferences_;
    context.session = ingame_session(
        campaign_mission_, (current_extension_state() & extension_state::multiplayer) != 0
    );
    context.in_game = true;
    context.return_label = return_label_;
    const auto show = [this, &session](const char* layout, IngamePanel which) {
        if (!load_match_hud_layout(layout))
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
        if (show("guis/EXITMENU.GUI", IngamePanel::exit_menu)) {
            ui::ingame_enter_exit_menu(session.panel, context);
            panel_to_widgets(
                session.panel, match_hud_->layout, widget_gaf_frames_, widget_text_stages_
            );
            render_match_surface();
        }
        return;
    case ui::IngameAction::open_exit_confirm:
        if (show("guis/YESORNO.GUI", IngamePanel::exit_confirm)) {
            ui::ingame_enter_exit_confirm(session.panel, context);
            panel_to_widgets(
                session.panel, match_hud_->layout, widget_gaf_frames_, widget_text_stages_
            );
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
        match_hud_panel_ == kPreferencesLayout && !match_hud_->layout.gadgets.empty()) {
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

bool Runtime::enter_match_menu() {
    if (match_menu_session().ingame_panel != IngamePanel::exit_confirm || team_panel_open())
        return false;
    activate_pause_gadget(ui::kExitConfirmDefault);
    return true;
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
    forget_team_panel();
    match_paused_ = true;
    match_command_ = MatchCommand::none;
    pending_build_type_ = 0;
    if (!load_match_hud_layout("guis/YESORNO.GUI")) {
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
    panel_from_widgets(session.panel, match_hud_->layout, widget_text_stages_);
    ui::ingame_open_leave_confirm(session.panel, context);
    panel_to_widgets(session.panel, match_hud_->layout, widget_gaf_frames_, widget_text_stages_);
    status_ = "Exit";
    render_match_surface();
}

void Runtime::open_restart_dialog() {
    auto& session = match_menu_session();
    if (!load_match_hud_layout(kRestartLayout) || match_hud_->layout.gadgets.empty())
        return;
    session.ingame_panel = IngamePanel::restart;
    place_beside_hud(match_hud_->layout);
    try {
        draw_panel_backdrop(
            match_hud_->background,
            match_hud_->layout.gadgets.front(),
            oa::decode_pcx(assets_.read(kRestartBackdrop).bytes)
        );
    } catch (const std::exception& error) {
        std::cerr << "RESTART backdrop unavailable: " << error.what() << '\n';
    }
    panel_from_widgets(session.panel, match_hud_->layout, widget_text_stages_);
    const auto mission = session.ingame.session == ui::SessionKind::campaign
                             ? bound_mission_name()
                             : skirmish_settings_.map_name;
    ui::ingame_enter_restart(session.panel, session.ingame, mission);
    panel_to_widgets(session.panel, match_hud_->layout, widget_gaf_frames_, widget_text_stages_);
    render_match_surface();
}

void Runtime::open_game_settings_sheet() {
    auto& session = match_menu_session();
    if (!match_ || !load_match_hud_layout(kGameSettingsLayout) ||
        match_hud_->layout.gadgets.empty())
        return;
    session.ingame_panel = IngamePanel::game_settings;
    auto& layout = match_hud_->layout;
    place_beside_hud(layout);
    try {
        draw_panel_backdrop(
            match_hud_->background,
            layout.gadgets.front(),
            oa::decode_pcx(assets_.read(kGameSettingsBackdrop).bytes)
        );
    } catch (const std::exception& error) {
        std::cerr << "GAMEOPTIONS backdrop unavailable: " << error.what() << '\n';
    }
    ui::GameSettingsSheet sheet;
    ui::ingame_build_game_settings(game_settings_view(), sheet);
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
    RestartRun run{this, session.ingame.session == ui::SessionKind::campaign, {}};
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
            (void)runtime.map_player_capacity();
        });
    };
    host.apply_roster = [](void* context) {
        restart_do(context, [](RestartRun& run) { run.runtime->apply_skirmish_players(); });
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
    (void)ui::ingame_run_restart(session.ingame, state_, host);
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

void Runtime::draw_options_lightbar() {
    // Only the preferences a match opens sweep the in-game menu away; the
    // full-screen options of the frontend set the lightbar up and never draw it.
    auto& session = match_menu_session();
    if (session.ingame_panel != IngamePanel::preferences || options_parent_ != Screen::match ||
        !match_ || !match_hud_ || match_hud_panel_ != kPreferencesLayout ||
        session.options.lightbar.active == 0 || options_flip_.rgb.empty() ||
        match_hud_cpu_.rgb.empty() || match_world_cpu_.rgb.empty())
        return;
    if (session.lightbar_due) {
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
        }
        // The radar picture is drawn again with the frame, and the game marks
        // the frame drawn.
        auto& game = match_->state().game;
        game.radar_blink_flags = static_cast<uint16_t>(
            game.radar_blink_flags | oa::present::world_renderer::radar_flag_redraw
        );
        game.options_lightbar_drawn = 1;
    }
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
        if (!load_match_hud_layout("guis/ARMOPT.GUI"))
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
        match_hud_panel_ != kPreferencesLayout || match_hud_->layout.gadgets.empty())
        return {};
    const auto& root = match_hud_->layout.gadgets.front().common;
    if (root.x + root.width <= kBattlefieldLeft || root.height <= 0)
        return {};
    return {kBattlefieldLeft, root.y, root.x + root.width - kBattlefieldLeft, root.height};
}

void Runtime::place_preferences_rows(renderer::Surface& hud) {
    preferences_hud_ = {};
    const auto rows = preferences_panel_rows();
    constexpr int32_t band_top = oa::ui::display_layout::kSourceBottomBarY;
    const auto bottom = rows.y + rows.height;
    if (rows.width <= 0 || bottom <= band_top || match_layout_.scale <= 0.0 ||
        hud.width != static_cast<uint32_t>(kCanvasWidth) ||
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

void Runtime::bind_options_context() {
    auto& context = match_menu_session().options;
    context.preferences = &preferences_;
    context.state = &state_;
    // Opened from a running match, the preferences are PREFS.GUI in the side
    // column with the in-game (*RT) sub-panels; elsewhere they are the
    // full-screen frontend panels.
    const bool in_match = options_parent_ == Screen::match && screen_ == Screen::match && match_;
    context.in_game = in_match;
    if (!in_match) {
        context.realtime_panels = false;
        context.hold_game = false;
    }
    const bool multiplayer = (current_extension_state() & extension_state::multiplayer) != 0;
    context.session_kind = static_cast<uint8_t>(ingame_session(campaign_mission_, multiplayer));
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
    context.host.save_options = [](void* host) { static_cast<Runtime*>(host)->save_preferences(); };
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
    context.host.scan_display_modes = [](void*, ui::DisplayModeList& list) {
        // The native renderer scales the 640x480 canvas; offer the classic modes.
        static constexpr ui::DisplayMode modes[] = {
            {640, 480, 8}, {800, 600, 8}, {1024, 768, 8}, {1280, 1024, 8}, {1600, 1200, 8}
        };
        list.count = 0;
        for (const auto& mode : modes)
            list.modes[static_cast<std::size_t>(list.count++)] = mode;
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
            (void)oa::sim::speed::set_speed(world, speed, runtime.message_hooks());
            runtime.match_timing_.requested_rate = world.game.requested_speed;
            runtime.match_timing_.actual_rate = world.game.current_speed;
            runtime.preferences_.current_game_speed = world.game.current_speed;
            if (runtime.extension_.speed_changed != nullptr)
                runtime.extension_.speed_changed(
                    runtime.extension_.context, runtime, world.game.requested_speed
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
            if (!runtime.load_match_hud_layout(kPreferencesLayout)) {
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
        (void)static_cast<Runtime*>(host)->load_named_background(name, false, false, false);
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
                {"guis/startopt.gui",
                 "",
                 "palettes/guipal.pal",
                 "anims/commongui.gaf",
                 "anims/commongui.gaf"}
            );
            (void)load_named_background(background.c_str(), false, false, false);
            auto parsed = oa::ui::gui_layout::parse(assets_.read("guis/" + sub).bytes);
            if (!parsed.ok())
                throw std::runtime_error(
                    parsed.error ? parsed.error->message : sub + " parse failed"
                );
            auto& gadgets = resources_.layout.gadgets;
            const auto& extra = parsed.layout->gadgets;
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
        } catch (const std::exception& error) {
            status_ = "options panel unavailable: " + std::string(error.what());
            return;
        }
        // The sub-panel is merged and drawn. Its sliders bind as the first
        // draw would; its buttons are not bound, so they keep the authored
        // foreground colour the renderer draws the frame through. Only a
        // loaded panel's first draw clears it.
        bind_frontend_scrolls("guis/startopt.gui", "anims/commongui.gaf");
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
    if (name == "PrevMenu" || name == "PREVMENU") {
        load(Screen::single_player);
        return;
    }
    // The side buttons refill the Campaign list with the chosen side's
    // campaigns and, on Any Mission, the Missions list.
    if (name == "Side0" || name == "Arm") {
        set_button_status("Arm", 1);
        set_button_status("Side0", 1);
        preferences_.side = 0;
        write_number(init::general_section, "side", 0);
        flush_preferences();
        discover_campaigns();
        sync_campaign_option_widgets();
        rebuild_surface();
        return;
    }
    if (name == "Side1" || name == "Core") {
        set_button_status("Core", 1);
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
    if (name == "Start") {
        play_ui_sound("BigButton", 0);
        // Start marks every mission unplayed and loads the campaign on the
        // list's selected row; New Campaign binds its first mission, Any
        // Mission the selected one.
        entry::reset_mission_results(state_);
        if (screen_ == Screen::new_campaign)
            load_campaign_missions(selected_campaign_index_);
        show_mission_briefing();
        return;
    }
    if (name == "Campaign" || name == "Missions")
        return;
}

} // namespace oa::app
