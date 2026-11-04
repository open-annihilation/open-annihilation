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
};

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
    /// them takes every input the controls after them are listed disabled.
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
