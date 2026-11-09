// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The automation host (automation_host.hpp): each entry reads one thing the
// runtime holds, or holds a key or button down for the reads of what is
// held (device_state.hpp).
#include "oa/app/automation_host.hpp"
#include "oa/app/control_names.hpp"
#include "controls.hpp"
#include "oa/app/extension.hpp"
#include "gui_actions.hpp"

#include "device_state.hpp"
#include "oa_layer.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/defs/sides.hpp"
#include "oa/data/languages/unit_texts.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/gui_input.hpp"
#include "oa/ui/gui_layout/gui_gadget.hpp"
#include "oa/ui/hud/order_panel.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
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

/// Returns the label on the same row, to the left of a scroll bar.
///
/// The nearest label whose right edge is not past the bar's left, plus a
/// little slack, wins. A label that does not share the bar's rows is left out.
///
/// @param gadgets the panel's gadgets
/// @param bar_index the scroll bar
/// @return its text; empty when the row has none
std::string beside_label(std::span<const gui::Gadget> gadgets, std::size_t bar_index) {
    const gui::CommonFields& bar = gadgets[bar_index].common;
    const int32_t bar_top = bar.y;
    const int32_t bar_bottom = static_cast<int32_t>(bar.y) + static_cast<int32_t>(bar.height);
    const int32_t bar_left = bar.x;
    // A label that ends a few columns into the bar still belongs to its row.
    constexpr int32_t kSlack = 4;
    bool found = false;
    int32_t best_gap = 0;
    std::string caption;
    for (std::size_t other = 0; other < gadgets.size(); ++other) {
        if (other == bar_index || gadgets[other].common.type != gui::GadgetType::label)
            continue;
        const gui::CommonFields& common = gadgets[other].common;
        const int32_t top = common.y;
        const int32_t bottom = static_cast<int32_t>(common.y) + static_cast<int32_t>(common.height);
        const int32_t overlap_top = top > bar_top ? top : bar_top;
        const int32_t overlap_bottom = bottom < bar_bottom ? bottom : bar_bottom;
        if (overlap_bottom <= overlap_top)
            continue;
        const int32_t right = static_cast<int32_t>(common.x) + static_cast<int32_t>(common.width);
        if (right > bar_left + kSlack)
            continue;
        const int32_t gap = bar_left - right;
        if (found && gap >= best_gap)
            continue;
        const auto* label = std::get_if<gui::LabelFields>(&gadgets[other].fields);
        if (label == nullptr)
            continue;
        found = true;
        best_gap = gap;
        caption = label->text;
    }
    return caption;
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
    if (bar != nullptr)
        fill_scroll_range(control, bar->range, bar->knob_position, beside_label(gadgets, index));
    return control;
}

/// Folds one ASCII letter. Any other byte is unchanged.
char fold_letter(unsigned char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return static_cast<char>(byte);
}

/// Reports whether two gadget names are the same, ignoring ASCII case.
bool names_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto a = static_cast<unsigned char>(left[index]);
        const auto b = static_cast<unsigned char>(right[index]);
        if (fold_letter(a) != fold_letter(b))
            return false;
    }
    return true;
}

/// The first active, not greyed button that names each letter.
///
/// A later button with the same letter is not a shortcut: the key presses
/// the first. A digit or a mark is not a key. `extra_grayed` is the match
/// HUD's grayed flag, one per gadget; empty for a menu.
///
/// @param gadgets the panel's gadgets
/// @param extra_grayed 1 when that gadget is greyed beyond the button's own flag
/// @param[out] first the gadget index of each letter, A in slot 0; -1 for none
void first_quick_letters(
    std::span<const gui::Gadget> gadgets,
    std::span<const uint8_t> extra_grayed,
    std::array<int, 26>& first
) {
    first.fill(-1);
    for (std::size_t index = 0; index < gadgets.size(); ++index) {
        const gui::Gadget& gadget = gadgets[index];
        const auto* button = std::get_if<gui::ButtonFields>(&gadget.fields);
        if (button == nullptr || gadget.common.active == 0 || button->grayed_out)
            continue;
        if (index < extra_grayed.size() && extra_grayed[index] != 0)
            continue;
        const std::string letter = shortcut_letter(button->quick_key);
        if (letter.size() != 1)
            continue;
        const int slot = static_cast<int>(static_cast<unsigned char>(letter[0])) - 'A';
        if (slot >= 0 && slot < 26 && first[static_cast<std::size_t>(slot)] < 0)
            first[static_cast<std::size_t>(slot)] = static_cast<int>(index);
    }
}

/// A one-letter chord's place, A in slot 0. A longer chord is not one.
bool plain_letter(std::string_view chord, int& slot) {
    if (chord.size() != 1)
        return false;
    const auto byte = static_cast<unsigned char>(chord[0]);
    if (byte >= 'a' && byte <= 'z') {
        slot = static_cast<int>(byte - 'a');
        return true;
    }
    if (byte >= 'A' && byte <= 'Z') {
        slot = static_cast<int>(byte - 'A');
        return true;
    }
    return false;
}

/// The GUI file's stem, without its folder or its suffix.
std::string gui_stem(std::string_view file) {
    if (const auto folder = file.find_last_of("/\\"); folder != std::string_view::npos)
        file.remove_prefix(folder + 1);
    if (const auto suffix = file.rfind('.'); suffix != std::string_view::npos && suffix > 0)
        file = file.substr(0, suffix);
    return std::string(file);
}

/// The displayed chord is the GUI file's own quick key.
///
/// A letter is compared in capitals, as a chord is shown. A digit or a mark
/// matches that one character. A longer chord is a binding of the player's.
///
/// @param chord the chord `active_key_chord` returned
/// @param quick_key the GUI file's quick-key byte
/// @return true when the chord is that byte
bool chord_is_file_key(std::string_view chord, int quick_key) {
    if (chord.empty() || quick_key <= 0 || quick_key >= 128)
        return false;
    const std::string letter = shortcut_letter(quick_key);
    if (!letter.empty())
        return chord == letter;
    return chord.size() == 1 && chord[0] == static_cast<char>(quick_key);
}

/// Tells whether a one-letter chord belongs to this button.
///
/// The letter goes to the first active button that names it. A letter no
/// button names, such as one the player bound, stays on this button.
///
/// @param chord a one-letter chord
/// @param letters the first active button that names each letter
/// @param index this gadget
/// @return true when this button is the one the letter presses
bool letter_is_this_button(std::string_view chord, const std::array<int, 26>& letters, int index) {
    int slot = 0;
    if (!plain_letter(chord, slot))
        return false;
    const int first = letters[static_cast<std::size_t>(slot)];
    return first < 0 || first == index;
}

/// The GUI file a paused match menu's gadget was loaded from.
///
/// The options pages are merged into PREFS.GUI. A gadget that panel does
/// not name keeps the page it came from: the sound, visuals, interface or
/// music page, or VISUALS.GUI for the screen-size bar the visuals page
/// copies in.
///
/// @param panel the menu's GUI file
/// @param gadget the gadget's name
/// @return that file
std::string match_menu_names_file(std::string_view panel, std::string_view gadget) {
    if (find_control_name(panel, gadget) != nullptr)
        return std::string(panel);
    if (names_equal(gui_stem(panel), "PREFS")) {
        static constexpr const char* kPages[] = {
            "SOUNDSRT.GUI",
            "VISUALRT.GUI",
            "SPEEDSRT.GUI",
            "MUSICRT.GUI",
            "VISUALS.GUI",
        };
        for (const char* file : kPages)
            if (find_control_name(file, gadget) != nullptr)
                return file;
    }
    return std::string(panel);
}

/// The page-turn action a build-menu page button presses.
std::string_view page_action(std::string_view name) {
    if (names_equal(name, "ARMPREV") || names_equal(name, "CORPREV"))
        return "build.page-previous";
    if (names_equal(name, "ARMNEXT") || names_equal(name, "CORNEXT"))
        return "build.page-next";
    return {};
}

/// The player a control's name ends with: Color0 is player 1.
///
/// @param name the control's name
/// @return the player, from 1; 0 when the name does not end with a number
int player_number(std::string_view name) {
    if (name.empty())
        return 0;
    std::size_t start = name.size();
    while (start > 0 && name[start - 1] >= '0' && name[start - 1] <= '9')
        --start;
    if (start == name.size())
        return 0;
    int number = 0;
    for (std::size_t index = start; index < name.size(); ++index)
        number = number * 10 + (name[index] - '0');
    return number + 1;
}

/// The text of the label whose link names a control.
///
/// @param gadgets the panel's gadgets
/// @param name the control
/// @return that label's text; empty when none links to it
std::string label_linked_to(std::span<const gui::Gadget> gadgets, std::string_view name) {
    for (const gui::Gadget& gadget : gadgets) {
        const auto* label = std::get_if<gui::LabelFields>(&gadget.fields);
        if (label == nullptr || label->link.empty() || label->text.empty())
            continue;
        if (names_equal(label->link, name))
            return label->text;
    }
    return {};
}

/// The number a neighbouring label draws for a slider.
///
/// @param gadgets the panel's gadgets
/// @param bar the slider
/// @return that label's text; empty when the slider has no such label
std::string setting_drawn(std::span<const gui::Gadget> gadgets, std::string_view bar) {
    const std::string_view label = slider_value_label(bar);
    if (label.empty())
        return {};
    for (const gui::Gadget& gadget : gadgets) {
        if (!names_equal(gadget.common.name, label))
            continue;
        return gadget_text(gadget, 0);
    }
    return {};
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
        stacked_dialog_controls(game, *controls);
    }

    /// Puts the controls of Open Annihilation's own screens (the OA layer)
    /// before the others: the OA button and the screens that show take the
    /// pointer over the screen and its panels. While one of them takes every
    /// input, the controls under it are listed hidden and disabled.
    ///
    /// @param game the runtime
    /// @param[in,out] controls the screen's controls, which the layer's go before
    static void layer_controls(Runtime& game, std::vector<AutomationControl>& controls) {
        if (!game.oa_layer_)
            return;
        const OaLayer& layer = *game.oa_layer_;
        if (layer.modal_shown(game.screen_ == Screen::match)) {
            for (AutomationControl& control : controls) {
                control.hidden = true;
                control.enabled = false;
            }
        }
        std::vector<AutomationControl> own;
        layer.automation_controls(own);
        controls.insert(controls.begin(), own.begin(), own.end());
    }

    /// Puts the controls of the top stacked dialog (a message box such as
    /// "There are no saved games to choose from", the disc prompt, the help
    /// panel) before the others: it takes the pointer and the keys over the
    /// screen and its panels while it is up.
    ///
    /// @param game the runtime
    /// @param[in,out] controls the screen's controls, which the dialog's go before
    static void stacked_dialog_controls(Runtime& game, std::vector<AutomationControl>& controls) {
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
        placement.focus = oa::ui::frontend_dialogs::dialog_keyboard_focus();
        std::vector<AutomationControl> stacked;
        std::array<int, 26> letters{};
        first_quick_letters(gadgets, {}, letters);
        const std::string gui_file(oa::ui::frontend_dialogs::dialog_layout_name());
        // A button with stages reports the caption of the stage it shows,
        // as HELP.GUI's Page does on each page.
        for (std::size_t index = 1; index < gadgets.size(); ++index) {
            if (const auto kind = control_kind(gadgets[index])) {
                AutomationControl control = gadget_control(
                    gadgets,
                    index,
                    *kind,
                    placement,
                    oa::ui::frontend_dialogs::dialog_button_stage(index),
                    false
                );
                speak_gadget(
                    &game, gadgets, index, gui_file, letters, 0, false, false, false, control
                );
                stacked.push_back(std::move(control));
            }
        }
        for (AutomationControl& control : stacked) {
            control.modal = true;
            if (control.kind == AutomationControlKind::label) {
                control.role = AutomationControlRole::alert_dialog;
                control.role_set = true;
            }
        }
        controls.insert(controls.begin(), stacked.begin(), stacked.end());
        conceal_behind_modal(controls);
    }

    /// The GUI file the frontend screen was loaded from.
    ///
    /// The last menu the keys were bound for is that file. With none yet,
    /// the screen's own layout is.
    ///
    /// @param game the runtime
    /// @return the file; empty when the screen has named neither
    static std::string frontend_gui_file(const Runtime& game) {
        if (!game.menu_gui_stems_.empty())
            return game.menu_gui_stems_.back() + ".GUI";
        const ScreenDesc* desc = screen_find(&game.screens_, screen_id(game.screen_));
        if (desc != nullptr && desc->assets.layout != nullptr)
            return std::string(desc->assets.layout);
        if (!game.resources_.layout.gadgets.empty())
            return game.resources_.layout.gadgets.front().common.name;
        return {};
    }

    /// Fills a control's label, value and shortcut from the names table.
    ///
    /// An order button, a build cell and a build page button take the key
    /// the player bound, and never the letter written in the GUI file. A
    /// paused match menu takes the chord of `gui.<shown panel>.<gadget>`,
    /// the action that menu's keys press. The options pages are merged into
    /// one panel, so that action is not the page file's. A chord that is the
    /// file's own quick key is listed only when it is a letter, and only on
    /// the first button that letter presses. Any other chord is one the
    /// player bound. A frontend control takes the chord of its
    /// `gui.<file>.<gadget>` action. A headless check, which has no
    /// bindings, keeps the GUI file's letter on that same button.
    ///
    /// @param game the runtime; null for a dialog that is not one of its screens
    /// @param gadgets the panel's gadgets
    /// @param index the gadget
    /// @param gui_file the GUI file the gadget was loaded from
    /// @param letters the first active button that names each letter
    /// @param build_cell the build cell, from 1; 0 when this is not one
    /// @param build_button the gadget is a unit or weapon picture
    /// @param match_orders the panel is the match's order panel
    /// @param match_menu the panel is a menu over a paused match
    /// @param[in,out] control the control `gadget_control` just made
    static void speak_gadget(
        Runtime* game,
        std::span<const gui::Gadget> gadgets,
        std::size_t index,
        std::string_view gui_file,
        const std::array<int, 26>& letters,
        int32_t build_cell,
        bool build_button,
        bool match_orders,
        bool match_menu,
        AutomationControl& control
    ) {
        const gui::Gadget& gadget = gadgets[index];
        const auto* button = std::get_if<gui::ButtonFields>(&gadget.fields);
        const auto* bar = std::get_if<gui::ScrollBarFields>(&gadget.fields);
        ControlReading reading;
        reading.gui.assign(gui_file);
        reading.linked_label = label_linked_to(gadgets, control.name);
        reading.drawn = setting_drawn(gadgets, control.name);
        reading.player = player_number(control.name);
        if (button != nullptr) {
            reading.quick_key = button->quick_key;
            reading.stages = button->stages;
        }
        if (bar != nullptr) {
            reading.has_slider = true;
            reading.knob = bar->knob_position;
            reading.range = bar->range;
        }
        if (game != nullptr) {
            const auto frame = game->widget_gaf_frames_.find(control.name);
            if (frame != game->widget_gaf_frames_.end()) {
                reading.alliance_frame = static_cast<int>(frame->second);
                if (frame->second < 10)
                    reading.colour = static_cast<int>(frame->second);
            }
            const auto staged = game->widget_text_stages_.find(control.name);
            if (staged != game->widget_text_stages_.end()) {
                const oa::Side* side = oa::data::defs::side_at(
                    game->side_table_, static_cast<uint32_t>(staged->second)
                );
                if (side != nullptr && side->name[0] != '\0') {
                    std::size_t length = 0;
                    while (length < sizeof side->name && side->name[length] != '\0')
                        ++length;
                    reading.side_name.assign(side->name, length);
                }
            }
        }
        if (reading.side_name.empty() && button != nullptr && !button->text.empty())
            reading.side_name = button->text;
        bool keyed = build_button;
        const std::string_view page = page_action(control.name);
        const bool headless =
            game == nullptr || (game->options_.headless_check && game->options_.snapshot.empty());
        if (!page.empty()) {
            keyed = true;
            if (game != nullptr)
                reading.shortcut = game->active_key_chord(page);
        } else if (match_orders && game != nullptr && !build_button) {
            const std::string_view action = game->order_gadget_action(index);
            if (!action.empty()) {
                keyed = true;
                reading.shortcut = game->active_key_chord(action);
            }
        } else if (build_button && game != nullptr && build_cell >= 1 && build_cell <= 12)
            reading.shortcut = game->build_cell_chord(build_cell);
        // A menu over the match is not the order panel. A common-attribute
        // bit the build page uses as "this picture builds a unit" is also
        // set on some of the menu's buttons, and that must not take their keys.
        if (match_menu)
            keyed = false;
        if (!keyed && button != nullptr && control.enabled) {
            if (match_menu && game != nullptr && !headless) {
                // The match answers gui.<shown panel>.<gadget>. When that
                // action has no binding, the button's own letter still
                // presses it, and a digit or a mark does not. The letter is
                // written here: a later step only copies a shortcut that is
                // already set.
                const std::string shown = game->shown_hud_stem();
                std::string chord;
                if (!shown.empty())
                    chord = game->active_key_chord(gui_action_id(shown, control.name));
                const int here = static_cast<int>(index);
                if (chord.empty() || chord_is_file_key(chord, reading.quick_key)) {
                    const std::string letter = shortcut_letter(reading.quick_key);
                    if (letter.size() == 1 && letter_is_this_button(letter, letters, here))
                        reading.shortcut = letter;
                } else {
                    int slot = 0;
                    if (!plain_letter(chord, slot) || letter_is_this_button(chord, letters, here))
                        reading.shortcut = std::move(chord);
                }
            } else if (game != nullptr && !headless) {
                // A merged panel, such as the options tabs, holds two files.
                // The letter is the action of the file that is on screen.
                std::string chord;
                if (!game->menu_gui_stems_.empty()) {
                    for (const std::string& stem : game->menu_gui_stems_) {
                        chord = game->active_key_chord(gui_action_id(stem, control.name));
                        if (!chord.empty())
                            break;
                    }
                } else
                    chord = game->active_key_chord(gui_action_id(gui_stem(gui_file), control.name));
                int slot = 0;
                if (!chord.empty() && plain_letter(chord, slot)) {
                    const int first = letters[static_cast<std::size_t>(slot)];
                    if (first < 0 || first == static_cast<int>(index))
                        reading.shortcut = chord;
                } else if (!chord.empty() && !chord_is_file_key(chord, reading.quick_key))
                    reading.shortcut = chord;
            } else {
                const std::string letter = shortcut_letter(reading.quick_key);
                if (letter.size() == 1) {
                    const int slot = static_cast<int>(static_cast<unsigned char>(letter[0])) - 'A';
                    if (slot >= 0 && slot < 26 &&
                        letters[static_cast<std::size_t>(slot)] == static_cast<int>(index))
                        reading.publish_letter = true;
                }
            }
        }
        apply_control_words(control, reading);
        if (build_button && control.label.empty() && game != nullptr) {
            for (const auto& definition : game->unit_definitions_) {
                if (!names_equal(definition.unit_name, control.name))
                    continue;
                const std::string shown(
                    oa::data::languages::unit_display_name(
                        definition.unit_name, definition.display_name
                    )
                );
                if (!shown.empty())
                    control.label = shown;
                break;
            }
        }
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
        // The focus the tree reports. Where the screen focuses none, it is the
        // first record of the order, and the screen's own focus stays put.
        placement.focus = game.announced_frontend_focus();
        auto* scrolls = game.frontend_scrolls();
        const std::string gui_file = frontend_gui_file(game);
        std::array<int, 26> letters{};
        first_quick_letters(gadgets, {}, letters);
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
            speak_gadget(&game, gadgets, index, gui_file, letters, 0, false, false, false, control);
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
        // The focus the tree reports while the panels have the keyboard.
        // Where the panel focuses none, it is the first record of its order.
        placement.focus = game.announced_match_focus();
        const auto area = game.placed_panel_area();
        const auto& root = gadgets.front().common;
        if (area && (root.width <= 0 || root.height <= 0))
            return;
        std::vector<uint8_t> grayed(gadgets.size(), 0);
        const std::size_t grayed_count = std::min(gadgets.size(), game.match_hud_states_.size());
        for (std::size_t at = 0; at < grayed_count; ++at)
            grayed[at] = game.match_hud_states_[at].grayed ? 1 : 0;
        std::array<int, 26> letters{};
        first_quick_letters(gadgets, grayed, letters);
        // A paused menu's letter belongs to the button whose shown-panel
        // action that key runs, which is the button the key presses. A
        // letter no such action names stays with the first button whose
        // own quick key is that letter.
        if (game.pause_menu_shown()) {
            const std::string shown = game.shown_hud_stem();
            std::array<bool, 26> claimed{};
            claimed.fill(false);
            if (!shown.empty()) {
                for (std::size_t index = 1; index < gadgets.size(); ++index) {
                    const auto* button = std::get_if<gui::ButtonFields>(&gadgets[index].fields);
                    if (button == nullptr || gadgets[index].common.active == 0 ||
                        button->grayed_out)
                        continue;
                    if (index < grayed.size() && grayed[index] != 0)
                        continue;
                    const std::string chord =
                        game.active_key_chord(gui_action_id(shown, gadgets[index].common.name));
                    int slot = 0;
                    if (!plain_letter(chord, slot) || claimed[static_cast<std::size_t>(slot)])
                        continue;
                    claimed[static_cast<std::size_t>(slot)] = true;
                    letters[static_cast<std::size_t>(slot)] = static_cast<int>(index);
                }
            }
        }
        std::vector<int32_t> cell_of(gadgets.size(), 0);
        const std::vector<std::size_t> cells = game.build_cell_gadgets();
        for (std::size_t at = 0; at < cells.size() && at < 12; ++at)
            if (cells[at] < cell_of.size())
                cell_of[cells[at]] = static_cast<int32_t>(at) + 1;
        constexpr uint8_t build_bits =
            oa::ui::hud::kCommonUnitButton | oa::ui::hud::kCommonWeaponButton;
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
            const auto attributes = static_cast<uint8_t>(common.common_attributes);
            const bool build_button = (attributes & build_bits) != 0;
            const int32_t build_cell = index < cell_of.size() ? cell_of[index] : 0;
            // A menu over the paused match is not the order panel. Its
            // gadgets are named from the GUI file each was loaded from, and
            // its keys are the menu's own.
            const bool menu = game.pause_menu_shown();
            const std::string names_file =
                menu ? match_menu_names_file(game.match_hud_panel_, common.name)
                     : game.match_hud_panel_;
            speak_gadget(
                &game,
                gadgets,
                index,
                names_file,
                letters,
                build_button ? build_cell : 0,
                build_button,
                !menu,
                menu,
                control
            );
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
                    control.holds_focus = source.holds_focus;
                    control.checked = source.checked;
                    control.masked = source.masked;
                    control.text_length = source.text_length > 0 ? source.text_length : 0;
                    if (source.masked)
                        control.text.clear();
                    else
                        control.text = bounded_text(source.text, kMaxControlText);
                    // A masked field's words are never the text it holds.
                    const auto shown_words = [&](const std::string& words) {
                        if (source.masked && !source.text.empty() && words == source.text)
                            return std::string{};
                        return bounded_text(words, kMaxControlText);
                    };
                    control.role_set = source.role_set;
                    control.role =
                        static_cast<AutomationControlRole>(static_cast<uint8_t>(source.role));
                    control.label = shown_words(source.label);
                    control.description = shown_words(source.description);
                    control.value_text = shown_words(source.value_text);
                    control.has_value = source.has_value;
                    control.value = source.value;
                    control.has_range = source.has_range;
                    control.range_least = source.range_least;
                    control.range_most = source.range_most;
                    control.step = source.step;
                    control.place = source.place;
                    control.set_size = source.set_size;
                    control.shortcut = shown_words(source.shortcut);
                    control.live = static_cast<AutomationLive>(static_cast<uint8_t>(source.live));
                    control.chosen = source.chosen;
                    control.expanded = source.expanded;
                    control.pressed = source.pressed;
                    control.hidden = source.hidden;
                    control.modal = source.modal;
                    control.scroll_up = source.scroll_up;
                    control.scroll_down = source.scroll_down;
                    if (window.press_width > 0 && window.press_height > 0) {
                        control.window_press_x = window.press_x;
                        control.window_press_y = window.press_y;
                        control.window_press_width = window.press_width;
                        control.window_press_height = window.press_height;
                    }
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

    /// Marks the focused control the family's ring is drawn around.
    ///
    /// @param context AutomationHost::context
    /// @param[in,out] controls the screen's controls
    static void stamp_family_focus(void* context, std::vector<AutomationControl>& controls) {
        static_cast<Runtime*>(context)->stamp_family_focus_shown(controls);
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
    static_cast<uint8_t>(AutomationControlRole::label) ==
        static_cast<uint8_t>(ExtensionControlRole::label),
    "automation and extension roles share one order"
);
static_assert(
    static_cast<uint8_t>(AutomationLive::assertive) ==
        static_cast<uint8_t>(ExtensionLive::assertive),
    "automation and extension live regions share one order"
);
static_assert(
    sizeof(AutomationHost) == sizeof(void*) * (1 + kAutomationHostEntryCount),
    "the automation host's entries changed: set every one of them here"
);

} // namespace

AutomationHost automation_host(Runtime& runtime) {
    // The listing marks the control the family's ring is drawn around. The
    // automation core does not name the runtime, so the mark is set here.
    automation::set_family_focus_stamp(&AutomationHostAccess::stamp_family_focus);
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
