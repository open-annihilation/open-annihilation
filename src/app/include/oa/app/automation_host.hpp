// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The table through which the automation endpoint (src/app/automation)
// reads what the running game holds beyond what the check host
// (check_host.hpp) reads: the preferences as the game holds them now, the
// file they live in, the running match's Game block, world and digest, the
// controls of the screen shown and the frames the game presents in its
// window; through which it holds keys and pointer buttons down for the
// game's reads of what is held (the input it hands the game goes through
// SDL's event queue, as a device's does); and through which it ends the run.
// The other entries only read, or have the game copy what it presents; none
// changes what the game shows, plays or simulates. The endpoint gets the table with
// automation_host() from a hook that is given the runtime, and may keep it
// while the runtime lives, on the thread that runs main(). What an entry
// returns stays valid until the runtime next runs a frame, a pass of its
// screens or a hook of the endpoint's, unless the entry says otherwise.
//
// The header names only the standard library's types and declares the
// engine's it names, so the endpoint needs nothing more of the engine's to
// read it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace oa {
struct Game;
struct World;
} // namespace oa

namespace oa::ui::frontend_renderer {
struct Surface;
}

namespace oa::app {

class Runtime;

/// What a control is, as the automation endpoint names it.
enum class AutomationControlKind : uint8_t {
    button,     ///< a push button, or one of a group of which one is chosen
    check_box,  ///< a button that flips between checked and not
    list,       ///< a list of rows, one of which may be selected
    text_field, ///< a field that takes typed text
    slider,     ///< a scroll bar
    label,      ///< text that takes no pointer
    area,       ///< a surface that takes the pointer, such as a map's picture
    image,      ///< a picture that takes no pointer
};

/// What a control is to a screen reader, beside AutomationControlKind.
///
/// `kind` keeps today's name and spelling. This names the same control the
/// way a screen reader does. `switch_control` is written "switch": that word
/// is a C++ keyword.
enum class AutomationControlRole : uint8_t {
    none,           ///< decoration, or not chosen yet
    button,         ///< a push button
    check_box,      ///< a box that is on or off
    switch_control, ///< a switch
    radio_group,    ///< a strip or a row, with one part chosen
    radio,          ///< one part of a strip or a row
    tab_list,       ///< a row of tabs
    tab,            ///< one tab
    list,           ///< a list of rows
    list_item,      ///< one row
    tree,           ///< a tree of rows
    tree_item,      ///< one row of a tree
    text_field,     ///< a field that takes typed text
    password,       ///< a masked field; its text is not given
    search,         ///< a search field
    slider,         ///< a slider or a scroll bar
    log,            ///< a log of lines
    dialog,         ///< a dialog
    alert_dialog,   ///< a message box
    group,          ///< a group of controls
    image,          ///< a picture with a meaning
    link,           ///< a link
    label,          ///< words that take no pointer
};

/// How a change of a control is announced. Off announces nothing.
enum class AutomationLive : uint8_t {
    off,       ///< a change is not announced
    polite,    ///< a change waits for a pause
    assertive, ///< a change is announced at once
};

/// One control of the screen shown, of a dialog over it, or of a window an extension shows.
struct AutomationControl {
    /// As the GUI file spells it, or as the extension spells it; for Open
    /// Annihilation's own screens, oa. and the UI kit's name.
    std::string name;
    AutomationControlKind kind{}; ///< what it is
    /// The dialog it belongs to, by its panel's name, or for Open
    /// Annihilation's own screens oa. and the screen's name; empty for the
    /// screen's own and for the OA button.
    std::string dialog;
    /// The extension window it belongs to, as the extension spells it; empty
    /// for the screen's own controls and a dialog's.
    std::string window;
    int32_t x{};      ///< its left column on the canvas
    int32_t y{};      ///< its top row on the canvas
    int32_t width{};  ///< its width in canvas pixels
    int32_t height{}; ///< its height in canvas pixels
    bool visible{};   ///< it is shown
    bool enabled{};   ///< it is shown and not grayed out
    bool focused{};   ///< it holds the keyboard focus
    bool checked{};   ///< a check box is checked
    std::string text; ///< its caption, its label's text or a field's typed text
    /// A masked field. An answer gives `text_length` and does not give the text.
    bool masked{};
    int32_t text_length{};          ///< characters a masked field holds
    std::vector<std::string> items; ///< a list's rows, the words it shows
    /// A list's names, one per `items` entry, when a row's words are not the
    /// name a choice uses. Empty for every other list.
    std::vector<std::string> item_keys;
    int32_t first_visible{}; ///< a list's first row shown
    int32_t rows{};          ///< the rows a list shows at once
    int32_t row_height{};    ///< a list's row pitch in canvas pixels
    int32_t selected{-1};    ///< a list's selected row; -1 for none
    /// A list's control that scrolls toward its start, one page of the rows
    /// it shows. Empty when the list has none: every row fits, or it has no
    /// scroll bar.
    std::string scroll_up;
    /// A list's control that scrolls toward its end, one page of the rows it shows.
    std::string scroll_down;
    /// The control is on a window a screen package reported, not on an
    /// extension's window. `window` still names that window. The protocol
    /// does not write this; the tree uses it to tell the two apart.
    bool package_window{};
    /// The control's place in the window is known exactly, in the window's
    /// own pixels (window_x to window_height), as for the controls of Open
    /// Annihilation's own screens, which are drawn in the window's pixels
    /// rather than on the canvas. Otherwise the endpoint works its window
    /// rectangle out from the canvas rectangle.
    bool window_pixels{};
    int32_t window_x{};      ///< its left column in the window, in the window's pixels
    int32_t window_y{};      ///< its top row in the window, in the window's pixels
    int32_t window_width{};  ///< its width, in the window's pixels
    int32_t window_height{}; ///< its height, in the window's pixels
    /// Set when `role` was chosen for this control. Otherwise
    /// finish_control_access fills it from `kind`. The protocol does not write this.
    bool role_set{};
    /// What a screen reader calls it. `kind` is unchanged beside it.
    AutomationControlRole role{AutomationControlRole::none};
    /// The caption a player hears. Empty when the control has none. It is
    /// never the value: a field's text, a switch's On or a slider's reading
    /// stay out of it. A masked field's label is never the text it holds.
    std::string label;
    /// More words about it. Empty when there are none.
    std::string description;
    /// Its value as words, such as a switch's On, a strip's chosen caption or
    /// a slider's "120 fps". Empty when it has none.
    std::string value_text;
    bool has_value{};      ///< `value` is set
    int32_t value{};       ///< a slider's position, or another control's number
    bool has_range{};      ///< `range_least`, `range_most` and `step` are set
    int32_t range_least{}; ///< the least value; the protocol writes it as min
    int32_t range_most{};  ///< the greatest value; the protocol writes it as max
    int32_t step{};        ///< how far one step moves the value
    /// Its place in a set, from 1. 0 when it has none. The protocol writes it as position.
    int32_t place{};
    /// How many share the set, as in "2 of 9". 0 when it has none.
    int32_t set_size{};
    /// A quick key or a bound key, as the player reads it. Empty when none is known.
    std::string shortcut;
    AutomationLive live{AutomationLive::off}; ///< how a change of it is announced
    /// It is the chosen one of a group. A list keeps that fact in `selected`.
    bool chosen{};
    bool expanded{}; ///< a tree row is open
    bool pressed{};  ///< a switch is on
    /// It is covered by a modal dialog or by another window. It stays listed,
    /// under the same name, and is not enabled.
    bool hidden{};
    bool modal{}; ///< it belongs to a dialog that covers what is behind it
    /// Where this extension window's art takes the press, on the canvas.
    /// Copied from the window onto each control it lists. Zero width or
    /// height means the window states no such place. The protocol does not
    /// write this. A screen button wholly inside it is listed hidden.
    int32_t window_press_x{};
    int32_t window_press_y{};
    int32_t window_press_width{};
    int32_t window_press_height{};
};

/// Returns the name the protocol gives a screen-reader role.
///
/// @param role the role
/// @return button, check_box, switch, radio_group, radio, tab_list, tab, list,
///         list_item, tree, tree_item, text_field, password, search, slider, log,
///         dialog, alert_dialog, group, image, link, label or none
[[nodiscard]] inline std::string_view control_role_name(AutomationControlRole role) noexcept {
    switch (role) {
    case AutomationControlRole::none:
        return "none";
    case AutomationControlRole::button:
        return "button";
    case AutomationControlRole::check_box:
        return "check_box";
    case AutomationControlRole::switch_control:
        return "switch";
    case AutomationControlRole::radio_group:
        return "radio_group";
    case AutomationControlRole::radio:
        return "radio";
    case AutomationControlRole::tab_list:
        return "tab_list";
    case AutomationControlRole::tab:
        return "tab";
    case AutomationControlRole::list:
        return "list";
    case AutomationControlRole::list_item:
        return "list_item";
    case AutomationControlRole::tree:
        return "tree";
    case AutomationControlRole::tree_item:
        return "tree_item";
    case AutomationControlRole::text_field:
        return "text_field";
    case AutomationControlRole::password:
        return "password";
    case AutomationControlRole::search:
        return "search";
    case AutomationControlRole::slider:
        return "slider";
    case AutomationControlRole::log:
        return "log";
    case AutomationControlRole::dialog:
        return "dialog";
    case AutomationControlRole::alert_dialog:
        return "alert_dialog";
    case AutomationControlRole::group:
        return "group";
    case AutomationControlRole::image:
        return "image";
    case AutomationControlRole::link:
        return "link";
    case AutomationControlRole::label:
        return "label";
    }
    return "none";
}

/// Returns the name the protocol gives a live region.
///
/// @param live how a change is announced
/// @return off, polite or assertive
[[nodiscard]] inline std::string_view live_name(AutomationLive live) noexcept {
    switch (live) {
    case AutomationLive::off:
        return "off";
    case AutomationLive::polite:
        return "polite";
    case AutomationLive::assertive:
        return "assertive";
    }
    return "off";
}

/// Fills a scroll bar's value from the gadget's range.
///
/// The knob stands at `knob`, from 0 up to one less than `range` when the
/// range is positive. One step moves it by 1. `text` is left as it is: the
/// value is `value` and `value_text`, not the caption. `beside` is the label
/// on the same row, and becomes the control's label when it has none.
///
/// @param[in,out] control the scroll bar
/// @param range how many positions the knob has; 0 or less is a single position
/// @param knob the position the knob stands at
/// @param beside the label beside it; empty when the screen has none
inline void fill_scroll_range(
    AutomationControl& control, int32_t range, int32_t knob, std::string_view beside
) {
    control.role = AutomationControlRole::slider;
    control.role_set = true;
    control.has_value = true;
    control.has_range = true;
    control.value = knob;
    control.range_least = 0;
    control.range_most = range > 0 ? range - 1 : 0;
    control.step = 1;
    if (control.label.empty() && !beside.empty())
        control.label.assign(beside);
    const std::string number = std::to_string(knob);
    if (beside.empty()) {
        control.value_text = number;
        return;
    }
    control.value_text.clear();
    control.value_text.reserve(beside.size() + 1 + number.size());
    control.value_text.assign(beside);
    control.value_text.push_back(' ');
    control.value_text += number;
}

/// Fills the role and the label a control did not set itself.
///
/// A masked field is a password and its text is never copied into the label.
/// A button, a check box, a label and an area take their caption from `text`
/// when no label was set. A text field, a slider and a list do not: their
/// caption is named elsewhere, and the text is the value.
/// A list keeps its integer `selected` and, when a row is chosen and no place
/// was set, records that place from 1 beside the row count.
///
/// @param[in,out] control the control, after its kind and text are known
inline void finish_control_access(AutomationControl& control) {
    if (control.masked) {
        control.role = AutomationControlRole::password;
        control.role_set = true;
        if (!control.text.empty() && control.label == control.text)
            control.label.clear();
    } else if (!control.role_set) {
        switch (control.kind) {
        case AutomationControlKind::button:
            control.role = AutomationControlRole::button;
            break;
        case AutomationControlKind::check_box:
            control.role = AutomationControlRole::check_box;
            break;
        case AutomationControlKind::list:
            control.role = AutomationControlRole::list;
            break;
        case AutomationControlKind::text_field:
            control.role = AutomationControlRole::text_field;
            break;
        case AutomationControlKind::slider:
            control.role = AutomationControlRole::slider;
            break;
        case AutomationControlKind::label:
            control.role = AutomationControlRole::label;
            break;
        case AutomationControlKind::area:
            control.role = AutomationControlRole::button;
            break;
        case AutomationControlKind::image:
            control.role = AutomationControlRole::image;
            break;
        }
    }
    const bool caption = control.kind == AutomationControlKind::button ||
                         control.kind == AutomationControlKind::check_box ||
                         control.kind == AutomationControlKind::label ||
                         control.kind == AutomationControlKind::area;
    if (caption && control.label.empty() && !control.masked && !control.text.empty())
        control.label = control.text;
    if (control.kind == AutomationControlKind::list && control.set_size == 0 &&
        !control.items.empty()) {
        const auto count = static_cast<int32_t>(control.items.size());
        control.set_size = count;
        if (control.selected >= 0 && control.place == 0)
            control.place = control.selected + 1;
    }
}

/// Marks every control that is not part of a modal cover as hidden and not enabled.
///
/// A cover is any control with `modal` set. With no cover, nothing changes.
/// Covered controls stay in the list under the same names.
///
/// @param[in,out] controls the screen's controls
inline void conceal_behind_modal(std::vector<AutomationControl>& controls) {
    bool covering = false;
    for (const AutomationControl& control : controls) {
        if (control.modal)
            covering = true;
    }
    if (!covering)
        return;
    for (AutomationControl& control : controls) {
        if (control.modal)
            continue;
        control.hidden = true;
        control.enabled = false;
    }
}

/// Marks extension and package windows behind a modal cover as hidden and not enabled.
///
/// The screen's own controls, and a modal dialog's, are left to the caller:
/// a screen drawn over the cover stays listed as it is. With no cover, nothing changes.
///
/// @param[in,out] controls the screen's controls, windows included
inline void conceal_windows_behind_modal(std::vector<AutomationControl>& controls) {
    bool covering = false;
    for (const AutomationControl& control : controls) {
        if (control.modal)
            covering = true;
    }
    if (!covering)
        return;
    for (AutomationControl& control : controls) {
        if (control.modal || control.window.empty())
            continue;
        control.hidden = true;
        control.enabled = false;
    }
}

/// Marks a screen button hidden when an extension window takes its press.
///
/// The extension's button is the one a player uses. The screen button on the
/// same rectangle, or contained in it when the two are close in size, stays
/// listed with `hidden` true. So does a screen button that lies wholly
/// inside the rectangle where the window's art takes the press, whatever
/// its size. `enabled` stays as the screen reported it. An extension
/// button that is hidden but enabled still covers. One that is hidden and
/// not enabled does not. A button that only falls inside a much larger
/// button is left as it is, unless that window's rectangle covers it. A
/// control measured in the window's own pixels is left as it is, and so is
/// one a package window covers. Nothing is drawn differently.
///
/// @param[in,out] controls the screen's controls, windows included
inline void conceal_screen_buttons_under_extension(std::vector<AutomationControl>& controls) {
    const auto right = [](const AutomationControl& control) {
        return static_cast<int64_t>(control.x) + static_cast<int64_t>(control.width);
    };
    const auto bottom = [](const AutomationControl& control) {
        return static_cast<int64_t>(control.y) + static_cast<int64_t>(control.height);
    };
    const auto area = [](const AutomationControl& control) {
        return static_cast<int64_t>(control.width) * static_cast<int64_t>(control.height);
    };
    const auto inside = [&](const AutomationControl& inner, const AutomationControl& outer) {
        return static_cast<int64_t>(inner.x) >= static_cast<int64_t>(outer.x) &&
               static_cast<int64_t>(inner.y) >= static_cast<int64_t>(outer.y) &&
               right(inner) <= right(outer) && bottom(inner) <= bottom(outer);
    };
    // The window's art, not one of its buttons. Edges count as inside.
    const auto inside_press = [&](const AutomationControl& inner, const AutomationControl& cover) {
        const int64_t outer_right = static_cast<int64_t>(cover.window_press_x) +
                                    static_cast<int64_t>(cover.window_press_width);
        const int64_t outer_bottom = static_cast<int64_t>(cover.window_press_y) +
                                     static_cast<int64_t>(cover.window_press_height);
        return static_cast<int64_t>(inner.x) >= static_cast<int64_t>(cover.window_press_x) &&
               static_cast<int64_t>(inner.y) >= static_cast<int64_t>(cover.window_press_y) &&
               right(inner) <= outer_right && bottom(inner) <= outer_bottom;
    };
    for (AutomationControl& screen : controls) {
        if (!screen.window.empty() || !screen.dialog.empty() || screen.window_pixels)
            continue;
        if (screen.kind != AutomationControlKind::button || screen.width <= 0 || screen.height <= 0)
            continue;
        for (const AutomationControl& cover : controls) {
            if (cover.window.empty() || cover.package_window || cover.window_pixels)
                continue;
            if (cover.window_press_width > 0 && cover.window_press_height > 0 &&
                inside_press(screen, cover)) {
                screen.hidden = true;
                break;
            }
            if (cover.kind != AutomationControlKind::button || cover.width <= 0 ||
                cover.height <= 0)
                continue;
            // Hidden and not enabled: it no longer takes the press, so it covers nothing.
            // Hidden and still enabled: it still takes the press, so it still covers.
            if (cover.hidden && !cover.enabled)
                continue;
            const int64_t screen_area = area(screen);
            const int64_t cover_area = area(cover);
            const bool close =
                screen_area <= cover_area * 4 && cover_area <= screen_area * 4 && screen_area > 0;
            if (close && (inside(screen, cover) || inside(cover, screen))) {
                screen.hidden = true;
                break;
            }
        }
    }
}

// What the automation endpoint may read of the running game.
// automation_host() sets every entry, and context, which every entry is
// passed back, is the runtime.
struct AutomationHost {
    void* context{};
    /// Returns the preferences as the game holds them now, by key: what the
    /// preferences file held when the game started, with every change the
    /// game has made since, whether it has written them to the file yet or
    /// not.
    ///
    /// @param context AutomationHost::context
    /// @return the values, each as the file writes it
    const std::map<std::string, std::string>* (*preferences)(void* context){};
    /// Returns the preferences file the game reads and writes.
    ///
    /// @param context AutomationHost::context
    /// @return its path; valid while the runtime lives
    const std::filesystem::path* (*preferences_file)(void* context){};
    /// Returns the running match's Game block.
    ///
    /// @param context AutomationHost::context
    /// @return the block, which the endpoint only reads; null while no match runs
    const oa::Game* (*match_game)(void* context){};
    /// Collects the controls of the built-in screen shown and of the dialog
    /// over it, or of the match's panel or dialog: those of the panel that
    /// takes the pointer, the dialog's while one is up, and a stacked
    /// dialog's (a message box) before them. Open Annihilation's own screens
    /// (the OA layer: the OA button, Settings, a notice or a prompt) come
    /// right after a stacked dialog's, ahead of the rest, and while one of
    /// them takes every input the controls after them are listed hidden and
    /// disabled. They stay in the list.
    /// The screens of the screen packages are not among them. A package
    /// reports the windows it shows through package_windows.
    ///
    /// @param context AutomationHost::context
    /// @param[out] controls replaced by the controls, in the panel's order
    void (*controls)(void* context, std::vector<AutomationControl>* controls){};
    /// Collects the controls of the windows extensions show over the game,
    /// from the sources the extensions registered (set_extension_window_source).
    /// They take the pointer over the screen, so the endpoint lists them
    /// ahead of the screen's own controls. Changes nothing.
    ///
    /// @param context AutomationHost::context
    /// @param[out] controls replaced by the controls, the windows in
    ///        registration order and each window's controls in its own order
    void (*windows)(void* context, std::vector<AutomationControl>* controls){};
    /// Holds a key down for the game's reads of the keys held, or lets it
    /// go, as the keyboard holds one between its key events. What the
    /// endpoint holds is kept for the process and outlives the runtime.
    ///
    /// @param context AutomationHost::context
    /// @param scancode the key, an SDL_Scancode; one SDL does not count is ignored
    /// @param down true to hold it, false to let it go
    void (*hold_key)(void* context, uint32_t scancode, bool down){};
    /// Sets the pointer buttons held down for the game's reads of the
    /// buttons held, as the mouse holds them between its button events;
    /// kept for the process as hold_key's keys are.
    ///
    /// @param context AutomationHost::context
    /// @param buttons SDL_BUTTON_MASK bits of the buttons held; 0 for none
    void (*hold_buttons)(void* context, uint32_t buttons){};
    /// Has the game copy each frame it presents in its window into a
    /// surface, from the next frame it presents until stop_frame_capture:
    /// the window's whole picture, in its own pixels, RGB with three bytes a
    /// pixel and rows from the top, read just before it is shown, cursor and
    /// all; the loading screen's frames too, which it presents as it draws
    /// them. A frame the game does not present, as while its device is
    /// lost, leaves the surface as it is. Calling it again with the same
    /// surface changes nothing.
    ///
    /// @param context AutomationHost::context
    /// @param[out] into the surface; it must live until stop_frame_capture
    /// @return false, and nothing changed, when the game already copies the
    ///         frames it presents somewhere else
    bool (*start_frame_capture)(void* context, oa::ui::frontend_renderer::Surface* into){};
    /// Stops the copying start_frame_capture began into a surface; does
    /// nothing when the game copies its frames somewhere else, or nowhere.
    ///
    /// @param context AutomationHost::context
    /// @param into the surface start_frame_capture was given
    void (*stop_frame_capture)(void* context, const oa::ui::frontend_renderer::Surface* into){};
    /// Returns the running match's world: its Game block, its players'
    /// setup blocks and its units.
    ///
    /// @param context AutomationHost::context
    /// @return the world, which the endpoint only reads; null while no match runs
    const oa::World* (*match_world)(void* context){};
    /// Returns the digest of the running match's state that a saved game
    /// carries, as the saved-game check prints it ("saveload: ... digest"):
    /// the units, players, map, camera and random state at this tick.
    /// Changes nothing; worked out only when asked, over the whole match.
    ///
    /// @param context AutomationHost::context
    /// @return the digest; that of no match while none runs
    uint64_t (*match_digest)(void* context){};
    /// Ends the run once the frame being run has been handled, as the main
    /// menu's Exit does: a running match is left first, with no question, as
    /// the surrender question's Yes leaves it, and a match being loaded is
    /// stopped. The run's exit status is left as it is.
    ///
    /// @param context AutomationHost::context
    void (*quit)(void* context){};
    /// Collects the windows the screen package shown reports
    /// (ScreenDesc::windows). A package that reports none leaves the list
    /// empty. Each control's `window` is the window's name and
    /// `package_window` is set, so the tree places the window under the
    /// package. Changes nothing.
    ///
    /// @param context AutomationHost::context
    /// @param[out] controls replaced by the controls, the windows in the
    ///        package's order and each window's controls in its own order
    void (*package_windows)(void* context, std::vector<AutomationControl>* controls){};
    /// Returns the name of the gadget the game has under the pointer: a
    /// match's gadget, or the frontend screen's. Null when the pointer is
    /// over none the game names. The screen answer uses it when no listed
    /// control's rectangle holds the pointer.
    ///
    /// @param context AutomationHost::context
    /// @return the name; valid until the runtime next runs a frame; null for none
    const char* (*hovered)(void* context){};
};

/// Returns the table through which the automation endpoint reads the running game.
///
/// @param runtime the running game, which the table's entries read
/// @return the table, every entry set and bound to `runtime`
[[nodiscard]] AutomationHost automation_host(Runtime& runtime);

} // namespace oa::app
