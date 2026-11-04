// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The automation host (automation_host.hpp): each entry reads one thing the
// runtime holds, or holds a key or button down for the reads of what is
// held (device_state.hpp).
#include "oa/app/automation_host.hpp"

#include "device_state.hpp"
#include "oa_layer.hpp"
#include "oa/app/runtime.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/gui_input.hpp"
#include "oa/ui/gui_layout/gui_gadget.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace oa::app {

namespace {

namespace gui = oa::ui::gui_layout;

// How many of an extension's windows, and of their controls, one answer lists.
constexpr std::size_t kMaxExtensionWindows = 32;
constexpr std::size_t kMaxExtensionControls = 1024;
// How much of a name, a caption or a list row the answer keeps.
constexpr std::size_t kMaxControlName = 64;
constexpr std::size_t kMaxControlText = 1024;
constexpr std::size_t kMaxListItems = 256;
constexpr std::size_t kMaxItemText = 256;

/// Keeps a text within a limit, in bytes.
///
/// @param text the text
/// @param limit the most bytes kept
/// @return the text, cut off past the limit
std::string bounded_text(std::string text, std::size_t limit) {
    if (text.size() > limit)
        text.resize(limit);
    return text;
}

/// Returns the driver's kind for an extension's control.
///
/// @param kind the extension's kind
/// @return the driver's; nothing for a kind it does not name
std::optional<AutomationControlKind> mapped_kind(ExtensionControlKind kind) noexcept {
    switch (kind) {
    case ExtensionControlKind::button:
        return AutomationControlKind::button;
    case ExtensionControlKind::check_box:
        return AutomationControlKind::check_box;
    case ExtensionControlKind::list:
        return AutomationControlKind::list;
    case ExtensionControlKind::text_field:
        return AutomationControlKind::text_field;
    case ExtensionControlKind::slider:
        return AutomationControlKind::slider;
    case ExtensionControlKind::label:
        return AutomationControlKind::label;
    case ExtensionControlKind::area:
        return AutomationControlKind::area;
    case ExtensionControlKind::image:
        return AutomationControlKind::image;
    }
    return std::nullopt;
}

/// Returns the driver's kind for a screen package's control.
///
/// @param kind the package's kind
/// @return the driver's
AutomationControlKind mapped_window_kind(ScreenWindowKind kind) noexcept {
    switch (kind) {
    case ScreenWindowKind::button:
        return AutomationControlKind::button;
    case ScreenWindowKind::check_box:
        return AutomationControlKind::check_box;
    case ScreenWindowKind::list:
        return AutomationControlKind::list;
    case ScreenWindowKind::text_field:
        return AutomationControlKind::text_field;
    case ScreenWindowKind::slider:
        return AutomationControlKind::slider;
    case ScreenWindowKind::label:
        return AutomationControlKind::label;
    case ScreenWindowKind::area:
        return AutomationControlKind::area;
    case ScreenWindowKind::image:
        return AutomationControlKind::image;
    }
    return AutomationControlKind::button;
}

/// Where a panel's gadgets lie on the canvas, and what the collected
/// controls of one panel share.
struct PanelPlacement {
    int32_t x{};        ///< columns added to a gadget's panel-relative left edge
    int32_t y{};        ///< rows added to its top edge
    std::string dialog; ///< the dialog the panel is; empty for the screen's own
    int32_t focus{-1};  ///< the gadget holding the keyboard focus; -1 for none
};

/// Returns the name a dialog goes by: its GUI file's name without its
/// folder and suffix, in lower case.
///
/// @param file the GUI file, as its loader named it
/// @return the name, such as "selmap" for guis/SELMAP.GUI
std::string dialog_name(std::string_view file) {
    if (const auto folder = file.find_last_of("/\\"); folder != std::string_view::npos)
        file.remove_prefix(folder + 1);
    if (const auto suffix = file.rfind('.'); suffix != std::string_view::npos)
        file = file.substr(0, suffix);
    std::string name(file);
    for (char& character : name)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    return name;
}

/// Returns the kind of control a gadget is.
///
/// @param gadget the gadget
/// @return its kind; nothing for a record that is no control (the panel's
///         root, resources, skins and progress bars)
std::optional<AutomationControlKind> control_kind(const gui::Gadget& gadget) {
    switch (gadget.common.type) {
    case gui::GadgetType::button:
        return (static_cast<uint32_t>(gadget.common.attributes) & gui::attribute::checkbox) != 0
                   ? AutomationControlKind::check_box
                   : AutomationControlKind::button;
    case gui::GadgetType::list_box:
        return AutomationControlKind::list;
    case gui::GadgetType::text_box:
        return AutomationControlKind::text_field;
    case gui::GadgetType::scroll_bar:
        return AutomationControlKind::slider;
    case gui::GadgetType::label:
        return AutomationControlKind::label;
    case gui::GadgetType::hot_surface:
        return AutomationControlKind::area;
    case gui::GadgetType::image:
        return AutomationControlKind::image;
    default:
        return std::nullopt;
    }
}

/// Returns the text a gadget shows: a button's caption at its stage, a
/// label's text or a field's typed text.
///
/// @param gadget the gadget
/// @param stage the stage a button with stages shows
/// @return the text; empty for a gadget without one
std::string gadget_text(const gui::Gadget& gadget, std::size_t stage) {
    if (const auto* button = std::get_if<gui::ButtonFields>(&gadget.fields)) {
        if (auto translated = renderer::translated_stage_caption(*button, stage))
            return *translated;
        return std::string(renderer::staged_caption(button->text, stage));
    }
    if (const auto* label = std::get_if<gui::LabelFields>(&gadget.fields))
        return label->text;
    if (const auto* box = std::get_if<gui::TextBoxFields>(&gadget.fields))
        return box->text;
    return {};
}

/// Makes the control a gadget is, without a list's rows.
///
/// @param gadgets the panel's gadgets
/// @param index the gadget's index
/// @param kind what it is
/// @param placement where the panel lies
/// @param stage the stage a button with stages shows
/// @param grayed the game grays the gadget out beyond its record's own state
/// @return the control
AutomationControl gadget_control(
    std::span<const gui::Gadget> gadgets,
    std::size_t index,
    AutomationControlKind kind,
    const PanelPlacement& placement,
    std::size_t stage,
    bool grayed
) {
    const auto& gadget = gadgets[index];
    AutomationControl control;
    control.name = gadget.common.name;
    control.kind = kind;
    control.dialog = placement.dialog;
    if (const auto rect = oa::ui::gui_input::gadget_geometry(gadgets, index)) {
        control.x = rect->left + placement.x;
        control.y = rect->top + placement.y;
        control.width = rect->right - rect->left + 1;
        control.height = rect->bottom - rect->top + 1;
    }
    control.visible = gadget.common.active != 0;
    const auto* button = std::get_if<gui::ButtonFields>(&gadget.fields);
    const auto* bar = std::get_if<gui::ScrollBarFields>(&gadget.fields);
    control.enabled = control.visible && !grayed && (button == nullptr || !button->grayed_out) &&
                      (bar == nullptr || !bar->locked);
    control.focused = placement.focus == static_cast<int32_t>(index);
    control.checked =
        kind == AutomationControlKind::check_box && button != nullptr && button->status != 0;
    control.text = gadget_text(gadget, stage);
    return control;
}

/// A list's rows as a screen shows them.
struct ListRows {
    std::span<const std::string> items;  ///< the rows
    std::size_t first_visible{};         ///< the first row shown
    std::optional<std::size_t> selected; ///< the selected row
    /// The rows' names, when the shown text is not the name. Empty otherwise.
    std::span<const std::string> keys;
};

} // namespace

// The runtime's side of the automation host: the one friend through which
// its entries read the runtime.
struct AutomationHostAccess {
    /// Returns the runtime an automation host's context names.
    ///
    /// @param context AutomationHost::context
    /// @return the runtime
    static const Runtime& runtime(void* context) { return *static_cast<const Runtime*>(context); }

    /// Returns the runtime an automation host's context names, for the
    /// entries that set what it copies or end its run.
    ///
    /// @param context AutomationHost::context
    /// @return the runtime
    static Runtime& capturing_runtime(void* context) { return *static_cast<Runtime*>(context); }

    /// Returns the preferences as the runtime holds them (AutomationHost::preferences).
    ///
    /// @param context AutomationHost::context
    /// @return the values
    static const std::map<std::string, std::string>* preferences(void* context) {
        return &runtime(context).preference_values_;
    }

    /// Returns the preferences file's path (AutomationHost::preferences_file).
    ///
    /// @param context AutomationHost::context
    /// @return the path
    static const std::filesystem::path* preferences_file(void* context) {
        return &runtime(context).preference_path_;
    }

    /// Returns the running match's Game block (AutomationHost::match_game).
    ///
    /// @param context AutomationHost::context
    /// @return the block, or null while no match runs
    static const oa::Game* match_game(void* context) {
        const auto& match = runtime(context).match_;
        return match ? &match->state().game : nullptr;
    }

    /// Collects the controls of the screen shown (AutomationHost::controls).
    ///
    /// The runtime's list and scroll readers are not const, though they
    /// change nothing; the controls are only read.
    ///
    /// @param context AutomationHost::context
    /// @param[out] controls replaced by the controls
    static void controls(void* context, std::vector<AutomationControl>* controls) {
        controls->clear();
        auto& game = *static_cast<Runtime*>(context);
        if (game.screen_ == Screen::match)
            match_controls(game, *controls);
        else if (
            screen_id(game.screen_) <= screen_id(Screen::briefing) &&
            game.screen_ != Screen::loading && !game.frame_owned_by_package()
        )
            frontend_controls(game, *controls);
        layer_controls(game, *controls);
        stacked_dialog_controls(*controls);
    }

    /// Puts the controls of Open Annihilation's own screens (the OA layer)
    /// before the others: the OA button and the screens that show take the
    /// pointer over the screen and its panels. While one of them takes every
    /// input, the controls under it are listed disabled.
    ///
    /// @param game the runtime
    /// @param[in,out] controls the screen's controls, which the layer's go before
    static void layer_controls(Runtime& game, std::vector<AutomationControl>& controls) {
        if (!game.oa_layer_)
            return;
        const OaLayer& layer = *game.oa_layer_;
        if (layer.modal_shown(game.screen_ == Screen::match))
            for (AutomationControl& control : controls)
                control.enabled = false;
        std::vector<AutomationControl> own;
        layer.automation_controls(own);
        controls.insert(controls.begin(), own.begin(), own.end());
    }

    /// Puts the controls of the top stacked dialog (a message box such as
    /// "There are no saved games to choose from", the disc prompt, the help
    /// panel) before the others: it takes the pointer and the keys over the
    /// screen and its panels while it is up.
    ///
    /// @param[in,out] controls the screen's controls, which the dialog's go before
    static void stacked_dialog_controls(std::vector<AutomationControl>& controls) {
        const auto* resources = oa::ui::frontend_dialogs::dialog_resources();
        if (resources == nullptr || resources->layout.gadgets.empty())
            return;
        const auto& gadgets = resources->layout.gadgets;
        // The root holds the dialog's place on the canvas; its records lie
        // relative to it.
        const auto& root = gadgets.front().common;
        PanelPlacement placement;
        placement.x = root.x;
        placement.y = root.y;
        placement.dialog = dialog_name(oa::ui::frontend_dialogs::dialog_layout_name());
        std::vector<AutomationControl> stacked;
        // A button with stages reports the caption of the stage it shows,
        // as HELP.GUI's Page does on each page.
        for (std::size_t index = 1; index < gadgets.size(); ++index)
            if (const auto kind = control_kind(gadgets[index]))
                stacked.push_back(gadget_control(
                    gadgets,
                    index,
                    *kind,
                    placement,
                    oa::ui::frontend_dialogs::dialog_button_stage(index),
                    false
                ));
        controls.insert(controls.begin(), stacked.begin(), stacked.end());
    }

    /// Collects the controls of a frontend screen's panel, or of the dialog
    /// shown as one: the map selection, and the panels drawn over another
    /// screen.
    ///
    /// @param game the runtime
    /// @param[out] controls receives the controls
    static void frontend_controls(Runtime& game, std::vector<AutomationControl>& controls) {
        const auto& gadgets = game.resources_.layout.gadgets;
        if (gadgets.empty())
            return;
        PanelPlacement placement;
        const auto origin = game.panel_origin();
        placement.x = origin.x;
        placement.y = origin.y;
        if (game.screen_ == Screen::map_selection) {
            // The map selection is centred over the skirmish setup.
            const auto& root = gadgets.front().common;
            placement.x += (kCanvasWidth - static_cast<int32_t>(root.width)) / 2;
            placement.y += (kCanvasHeight - static_cast<int32_t>(root.height)) / 2;
        }
        if (game.screen_ == Screen::map_selection || game.panel_over_screen()) {
            const auto* desc = screen_find(&game.screens_, screen_id(game.screen_));
            placement.dialog = dialog_name(
                desc != nullptr && desc->assets.layout != nullptr ? desc->assets.layout
                                                                  : gadgets.front().common.name
            );
        }
        // The focus counts while the panel takes the keys.
        placement.focus = game.frontend_has_keyboard() ? game.frontend_focus() : -1;
        auto* scrolls = game.frontend_scrolls();
        for (std::size_t index = 1; index < gadgets.size(); ++index) {
            const auto kind = control_kind(gadgets[index]);
            if (!kind)
                continue;
            const auto& name = gadgets[index].common.name;
            const auto stage = game.widget_text_stages_.find(name);
            auto control = gadget_control(
                gadgets,
                index,
                *kind,
                placement,
                stage != game.widget_text_stages_.end() ? stage->second : 0,
                false
            );
            // The load and save dialog holds its fields' and labels' text itself.
            if (*kind == AutomationControlKind::text_field || *kind == AutomationControlKind::label)
                if (auto held = game.load_game_control_text(index))
                    control.text = std::move(*held);
            if (*kind == AutomationControlKind::list) {
                const auto* list =
                    scrolls != nullptr ? renderer::find_layout_list(*scrolls, index) : nullptr;
                int32_t row_height = scrolls != nullptr ? scrolls->line_height : 0;
                if (list != nullptr && list->list.item_height > 0)
                    row_height = list->list.item_height;
                if (const auto* fields = std::get_if<gui::ListBoxFields>(&gadgets[index].fields);
                    row_height <= 0 && fields != nullptr)
                    row_height = fields->item_height;
                control.row_height = std::max(row_height, 0);
                control.rows = control.row_height > 0 ? control.height / control.row_height : 0;
                if (const auto rows = frontend_list_rows(game, name)) {
                    control.items.assign(rows->items.begin(), rows->items.end());
                    if (rows->keys.size() == rows->items.size())
                        control.item_keys.assign(rows->keys.begin(), rows->keys.end());
                    control.first_visible = static_cast<int32_t>(rows->first_visible);
                    control.selected = rows->selected ? static_cast<int32_t>(*rows->selected) : -1;
                }
            }
            controls.push_back(std::move(control));
        }
    }

    /// Returns the rows of a frontend screen's list, as the frame shows them.
    ///
    /// @param game the runtime
    /// @param name the list's name
    /// @return its rows; nothing for a list whose rows the runtime does not keep
    static std::optional<ListRows> frontend_list_rows(Runtime& game, std::string_view name) {
        const auto first = [&game, name](std::size_t kept) {
            return game.frontend_list_first(name).value_or(kept);
        };
        const auto named = [name](std::string_view list) {
            return Runtime::tdf_names_equal(name, list);
        };
        if (game.screen_ == Screen::map_selection && named("MAPNAMES")) {
            const bool labelled = game.bound_map_labels_.size() == game.bound_map_names_.size();
            return ListRows{
                labelled ? std::span<const std::string>{game.bound_map_labels_}
                         : std::span<const std::string>{game.bound_map_names_},
                game.map_first_visible(),
                static_cast<std::size_t>(std::max<int16_t>(0, game.modal_map_index_)),
                game.bound_map_names_
            };
        }
        if ((game.screen_ == Screen::any_mission || game.screen_ == Screen::new_campaign) &&
            named("Campaign")) {
            // The rows are the names shown; a campaign's file name is its key.
            const bool named_rows =
                game.campaign_shown_labels_.size() == game.campaign_labels_.size();
            return ListRows{
                named_rows ? std::span<const std::string>{game.campaign_shown_labels_}
                           : std::span<const std::string>{game.campaign_labels_},
                first(game.campaign_first_visible_),
                game.selected_campaign_index_,
                named_rows ? std::span<const std::string>{game.campaign_labels_}
                           : std::span<const std::string>{}
            };
        }
        if (game.screen_ == Screen::any_mission && named("Missions"))
            return ListRows{
                game.campaign_mission_labels_,
                first(game.campaign_mission_first_visible_),
                game.selected_mission_index_,
                {}
            };
        if (named("GAMES"))
            if (const auto saves = game.load_game_rows(); saves && saves->items != nullptr)
                return ListRows{*saves->items, first(0), saves->selected, {}};
        if (game.screen_ == Screen::campaign_end && named("Missions"))
            return ListRows{
                game.end_mission_rows_,
                game.campaign_mission_first_visible_,
                game.selected_mission_index_,
                {}
            };
        return std::nullopt;
    }

    /// Collects the controls of the match's panel, or of the menu or dialog
    /// over the match.
    ///
    /// @param game the runtime
    /// @param[out] controls receives the controls
    static void match_controls(Runtime& game, std::vector<AutomationControl>& controls) {
        if (!game.match_hud_ || game.match_hud_->layout.gadgets.empty())
            return;
        const auto& gadgets = game.match_hud_->layout.gadgets;
        PanelPlacement placement;
        if (game.match_hud_placement_ != 0 || game.match_panel_under_)
            placement.dialog = dialog_name(game.match_hud_panel_);
        // The focus counts while the match's panels take the keys.
        placement.focus = game.match_panels_keyboard_ ? game.match_hud_focus_ : -1;
        const auto area = game.placed_panel_area();
        const auto& root = gadgets.front().common;
        if (area && (root.width <= 0 || root.height <= 0))
            return;
        for (std::size_t index = 1; index < gadgets.size(); ++index) {
            const auto kind = control_kind(gadgets[index]);
            if (!kind)
                continue;
            const auto& state = index < game.match_hud_states_.size()
                                    ? game.match_hud_states_[index]
                                    : Runtime::MatchGadgetState{};
            const auto stage = game.widget_text_stages_.find(gadgets[index].common.name);
            auto control = gadget_control(
                gadgets,
                index,
                *kind,
                placement,
                stage != game.widget_text_stages_.end() ? stage->second : 0,
                state.grayed
            );
            if (*kind == AutomationControlKind::check_box)
                control.checked = state.status != 0;
            // The HUD is laid out in its 640x480 source; the canvas shows it
            // through the match's layout, the side column's scale among it,
            // and a menu or dialog placed over the paused match through the
            // area it is drawn in.
            const auto& common = gadgets[index].common;
            if (area) {
                const auto through =
                    [](int source, int start, int length, int canvas, int canvas_length) {
                        return canvas +
                               static_cast<int>(std::lround(
                                   static_cast<double>(source - start) * canvas_length / length
                               ));
                    };
                control.x = through(common.x, root.x, root.width, area->x, area->width);
                control.y = through(common.y, root.y, root.height, area->y, area->height);
                control.width =
                    through(common.x + common.width, root.x, root.width, area->x, area->width) -
                    control.x;
                control.height =
                    through(common.y + common.height, root.y, root.height, area->y, area->height) -
                    control.y;
            } else {
                const auto rect = oa::ui::display_layout::source_rect_to_canvas(
                    game.match_layout_, common.x, common.y, common.width, common.height
                );
                const auto centre = game.hud_gadget_centre(index);
                control.width = rect.width;
                control.height = rect.height;
                control.x = centre.x - rect.width / 2;
                control.y = centre.y - rect.height / 2;
            }
            controls.push_back(std::move(control));
        }
    }

    /// Holds a key down or lets it go (AutomationHost::hold_key).
    ///
    /// @param scancode the key
    /// @param down true to hold it
    static void hold_key(void* /*context*/, uint32_t scancode, bool down) {
        if (scancode < SDL_SCANCODE_COUNT)
            device_state::hold_key(static_cast<SDL_Scancode>(scancode), down);
    }

    /// Sets the pointer buttons held down (AutomationHost::hold_buttons).
    ///
    /// @param buttons SDL_BUTTON_MASK bits
    static void hold_buttons(void* /*context*/, uint32_t buttons) {
        device_state::hold_buttons(static_cast<SDL_MouseButtonFlags>(buttons));
    }

    /// Has the runtime copy the frames it presents into a surface
    /// (AutomationHost::start_frame_capture).
    ///
    /// @param context AutomationHost::context
    /// @param[out] into the surface
    /// @return false when the runtime copies them somewhere else
    static bool start_frame_capture(void* context, oa::ui::frontend_renderer::Surface* into) {
        auto*& capture = capturing_runtime(context).capture_frame_;
        if (capture != nullptr && capture != into)
            return false;
        capture = into;
        return true;
    }

    /// Stops the runtime copying the frames it presents into a surface
    /// (AutomationHost::stop_frame_capture).
    ///
    /// @param context AutomationHost::context
    /// @param into the surface
    static void stop_frame_capture(void* context, const oa::ui::frontend_renderer::Surface* into) {
        auto*& capture = capturing_runtime(context).capture_frame_;
        if (capture == into)
            capture = nullptr;
    }

    /// Returns the running match's world (AutomationHost::match_world).
    ///
    /// @param context AutomationHost::context
    /// @return the world, or null while no match runs
    static const oa::World* match_world(void* context) {
        const auto& match = runtime(context).match_;
        return match ? &match->state() : nullptr;
    }

    /// Returns the running match's saved-state digest (AutomationHost::match_digest).
    ///
    /// @param context AutomationHost::context
    /// @return the digest, as Runtime::match_world_digest gives it
    static uint64_t match_digest(void* context) { return runtime(context).match_world_digest(); }

    /// Ends the run as the main menu's Exit does (AutomationHost::quit): the
    /// frame's end leaves a running match and quits (finish_quit_request).
    ///
    /// @param context AutomationHost::context
    static void quit(void* context) {
        Runtime& game = capturing_runtime(context);
        game.quit_requested_ = true;
        game.quit_reason_.clear();
    }

    /// Collects the windows the screen package shown reports
    /// (AutomationHost::package_windows).
    ///
    /// @param context AutomationHost::context
    /// @param[out] controls replaced by the controls
    static void package_windows(void* context, std::vector<AutomationControl>* controls) {
        controls->clear();
        // screen_context is not const. The call only reads the package shown.
        Runtime& game = capturing_runtime(context);
        const ScreenDesc* desc = screen_find(&game.screens_, screen_id(game.screen_));
        if (desc == nullptr || desc->windows == nullptr)
            return;
        ScreenContext ctx = game.screen_context();
        std::vector<ScreenPackageWindow> windows;
        desc->windows(&ctx, desc->state, &windows);
        std::size_t windows_kept = 0;
        std::size_t controls_kept = 0;
        for (const ScreenPackageWindow& window : windows) {
            if (windows_kept >= kMaxExtensionWindows)
                return;
            const std::string name = bounded_text(window.name, kMaxControlName);
            if (name.empty())
                continue;
            ++windows_kept;
            for (const ScreenWindowControl& source : window.controls) {
                if (controls_kept >= kMaxExtensionControls)
                    return;
                const std::string control_name = bounded_text(source.name, kMaxControlName);
                if (control_name.empty())
                    continue;
                AutomationControl control;
                control.name = control_name;
                control.kind = mapped_window_kind(source.kind);
                control.window = name;
                control.package_window = true;
                control.x = source.x;
                control.y = source.y;
                control.width = source.width;
                control.height = source.height;
                control.visible = source.visible;
                control.enabled = source.enabled;
                control.focused = source.focused;
                control.checked = source.checked;
                control.text = bounded_text(source.text, kMaxControlText);
                if (source.kind == ScreenWindowKind::list) {
                    const std::size_t count = std::min(source.items.size(), kMaxListItems);
                    control.items.reserve(count);
                    for (std::size_t item = 0; item < count; ++item)
                        control.items.push_back(bounded_text(source.items[item], kMaxItemText));
                    control.first_visible = std::max<int32_t>(0, source.first_visible);
                    control.row_height = source.row_height;
                    control.rows = source.rows;
                    control.selected = source.selected;
                }
                controls->push_back(std::move(control));
                ++controls_kept;
            }
        }
    }

    /// Returns the gadget the game has under the pointer (AutomationHost::hovered).
    ///
    /// @param context AutomationHost::context
    /// @return its name, or null when the game names none
    static const char* hovered(void* context) {
        const Runtime& game = runtime(context);
        if (game.screen_ == Screen::match)
            return game.hovered_gadget_name();
        if (!game.hovered_ || *game.hovered_ == 0)
            return nullptr;
        const auto& gadgets = game.resources_.layout.gadgets;
        if (*game.hovered_ >= gadgets.size())
            return nullptr;
        const std::string& name = gadgets[*game.hovered_].common.name;
        return name.empty() ? nullptr : name.c_str();
    }

    /// Collects the controls of the windows extensions show (AutomationHost::windows).
    ///
    /// @param context AutomationHost::context
    /// @param[out] controls replaced by the controls
    static void windows(void* context, std::vector<AutomationControl>* controls) {
        controls->clear();
        const auto& game = runtime(context);
        const auto sources = game.extension_window_sources_;
        std::size_t windows_kept = 0;
        std::size_t controls_kept = 0;
        for (const auto& slot : sources) {
            if (slot.source == nullptr)
                continue;
            std::vector<ExtensionWindow> listed;
            slot.source(slot.context, game, listed);
            for (const ExtensionWindow& window : listed) {
                if (windows_kept >= kMaxExtensionWindows)
                    return;
                const std::string name = bounded_text(window.name, kMaxControlName);
                if (name.empty())
                    continue;
                ++windows_kept;
                for (const ExtensionControl& source : window.controls) {
                    if (controls_kept >= kMaxExtensionControls)
                        return;
                    const auto kind = mapped_kind(source.kind);
                    const std::string control_name = bounded_text(source.name, kMaxControlName);
                    if (!kind || control_name.empty())
                        continue;
                    AutomationControl control;
                    control.name = control_name;
                    control.kind = *kind;
                    control.window = name;
                    control.x = source.x;
                    control.y = source.y;
                    control.width = source.width;
                    control.height = source.height;
                    control.visible = source.visible;
                    control.enabled = source.enabled;
                    control.focused = source.focused;
                    control.checked = source.checked;
                    control.masked = source.masked;
                    control.text_length = source.text_length > 0 ? source.text_length : 0;
                    if (source.masked)
                        control.text.clear();
                    else
                        control.text = bounded_text(source.text, kMaxControlText);
                    control.scroll_up = source.scroll_up;
                    control.scroll_down = source.scroll_down;
                    if (*kind == AutomationControlKind::list) {
                        const std::size_t count = std::min(source.items.size(), kMaxListItems);
                        control.items.reserve(count);
                        for (std::size_t index = 0; index < count; ++index)
                            control.items.push_back(
                                bounded_text(source.items[index], kMaxItemText)
                            );
                        control.first_visible = std::max<int32_t>(0, source.first_visible);
                        control.row_height = source.row_height;
                        control.rows = source.rows;
                        control.selected = source.selected;
                    }
                    controls->push_back(std::move(control));
                    ++controls_kept;
                }
            }
        }
    }
};

namespace {

// Every entry of the automation host; automation_host() binds a copy to a runtime.
constexpr AutomationHost kAutomationHostEntries{
    nullptr,
    AutomationHostAccess::preferences,
    AutomationHostAccess::preferences_file,
    AutomationHostAccess::match_game,
    AutomationHostAccess::controls,
    AutomationHostAccess::windows,
    AutomationHostAccess::hold_key,
    AutomationHostAccess::hold_buttons,
    AutomationHostAccess::start_frame_capture,
    AutomationHostAccess::stop_frame_capture,
    AutomationHostAccess::match_world,
    AutomationHostAccess::match_digest,
    AutomationHostAccess::quit,
    AutomationHostAccess::package_windows,
    AutomationHostAccess::hovered
};

// The entries above, one for each of the table's after its context.
constexpr std::size_t kAutomationHostEntryCount = 14;
static_assert(
    sizeof(AutomationHost) == sizeof(void*) * (1 + kAutomationHostEntryCount),
    "the automation host's entries changed: set every one of them here"
);

} // namespace

AutomationHost automation_host(Runtime& runtime) {
    AutomationHost host = kAutomationHostEntries;
    host.context = &runtime;
    return host;
}

void set_extension_window_source(Runtime& runtime, void* context, ExtensionWindowSource source) {
    // How many extensions may show windows at once.
    constexpr std::size_t kMaxSources = 16;
    auto& sources = runtime.extension_window_sources_;
    const auto found = std::find_if(sources.begin(), sources.end(), [&](const auto& slot) {
        return slot.context == context;
    });
    if (source == nullptr) {
        if (found != sources.end())
            sources.erase(found);
        return;
    }
    if (found != sources.end()) {
        found->source = source;
        return;
    }
    if (sources.size() >= kMaxSources)
        return;
    sources.push_back({context, source});
}

namespace {

/// Copies a note into a fixed buffer, cutting it so the buffer stays terminated.
///
/// @param[out] into the buffer
/// @param capacity its size in bytes, including the terminator
/// @param from the text; null is empty
void copy_note(char* into, std::size_t capacity, const char* from) {
    if (capacity == 0)
        return;
    if (from == nullptr) {
        into[0] = '\0';
        return;
    }
    std::size_t index = 0;
    for (; index + 1 < capacity && from[index] != '\0'; ++index)
        into[index] = from[index];
    into[index] = '\0';
}

} // namespace

void post_extension_note(Runtime& runtime, const char* kind, const char* text) {
    if (kind == nullptr || kind[0] == '\0')
        return;
    if (runtime.extension_note_count_ == Runtime::k_extension_note_cap) {
        runtime.extension_note_read_ =
            (runtime.extension_note_read_ + 1) % Runtime::k_extension_note_cap;
        --runtime.extension_note_count_;
    }
    Runtime::ExtensionNote& note = runtime.extension_notes_[runtime.extension_note_write_];
    copy_note(note.kind, sizeof note.kind, kind);
    copy_note(note.text, sizeof note.text, text);
    runtime.extension_note_write_ =
        (runtime.extension_note_write_ + 1) % Runtime::k_extension_note_cap;
    ++runtime.extension_note_count_;
}

void take_extension_notes(
    void* runtime, std::vector<std::string>& kinds, std::vector<std::string>& texts
) {
    if (runtime == nullptr) {
        kinds.clear();
        texts.clear();
        return;
    }
    take_extension_notes(*static_cast<Runtime*>(runtime), kinds, texts);
}

void take_extension_notes(
    Runtime& runtime, std::vector<std::string>& kinds, std::vector<std::string>& texts
) {
    kinds.clear();
    texts.clear();
    kinds.reserve(runtime.extension_note_count_);
    texts.reserve(runtime.extension_note_count_);
    while (runtime.extension_note_count_ > 0) {
        const Runtime::ExtensionNote& note = runtime.extension_notes_[runtime.extension_note_read_];
        kinds.emplace_back(note.kind);
        texts.emplace_back(note.text);
        runtime.extension_note_read_ =
            (runtime.extension_note_read_ + 1) % Runtime::k_extension_note_cap;
        --runtime.extension_note_count_;
    }
    runtime.extension_note_read_ = 0;
    runtime.extension_note_write_ = 0;
}

uint32_t extension_clock(const Runtime& runtime) {
    if (runtime.options_.fixed_clock)
        return runtime.clock_milliseconds();
    if (runtime.options_.remote_controlled)
        return runtime.extension_clock_frame_ * kFixedClockMsPerTick;
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()
    )
                                     .count());
}

} // namespace oa::app
