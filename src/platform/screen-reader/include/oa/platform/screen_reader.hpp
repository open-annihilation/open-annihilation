// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The screen-reader bridge. It wraps AccessKit's adapter for the game's SDL
// window. Nothing here runs when the game starts: a caller loads the library
// and attaches a window. The tree is built only after a screen reader asks
// for it. A build that leaves the bridge out can include this header and no
// other header of the module.
#pragma once

#include <cstdint>
#include <string_view>

struct SDL_Window;

namespace oa::platform::screen_reader {

/// How load() finished.
enum class LoadResult {
    ready,           ///< the C functions are bound
    compiled_out,    ///< this build leaves the bridge out
    missing_library, ///< Windows, and accesskit.dll is not beside the program
    old_windows,     ///< Windows, older than Windows 10, so the DLL is not loaded
    unavailable,     ///< the library was there and did not load
};

/// The preference `open-annihilation.access.screen-reader`.
///
/// Absent, or any text other than `auto`, `on` and `off`, reads as `auto`.
enum class Preference {
    automatic, ///< attach while a reader is running; build a tree only then
    on,        ///< attach even when none is running; speech runs where it cannot load
    off,       ///< create nothing
};

/// The window node's id in the tree this module builds.
inline constexpr uint64_t window_node = 1;

/// The test node's id. It is a child of the window node.
inline constexpr uint64_t test_node = 2;

/// The label the test node carries.
inline constexpr const char* test_label = "Test";

/// The preferences key. Settings does not offer a row for it yet.
inline constexpr const char* preference_key = "open-annihilation.access.screen-reader";

/// Reports whether this build includes the bridge.
///
/// @return false when this build leaves the bridge out, true otherwise
[[nodiscard]] bool compiled_in();

/// Binds AccessKit's C functions.
///
/// On Windows the DLL is loaded from the program's own folder, only when
/// the system is Windows 10 or later, and the critical-error mode is set
/// around that load. On macOS, iOS and Linux the static library is linked
/// with this module, and a caller that uses the bridge pulls it in.
///
/// @return how the bind finished
[[nodiscard]] LoadResult load();

/// Reads the preference. Any text other than `on` and `off` is `automatic`,
/// and so is an empty string.
///
/// @param text the stored value; empty when the key is absent
/// @return the preference
[[nodiscard]] Preference preference_of(std::string_view text);

/// Remembers the preference. `off` drops an adapter that was attached.
/// Changing the value lets a later attach try again.
///
/// @param mode the preference
void set_preference(Preference mode);

/// Attaches the platform adapter to a window.
///
/// The window's native handle is the SDL property for this system. Nothing
/// is attached until this is called. On Windows a window that is still hidden
/// uses AccessKit's subclassing adapter, which has to run before the window
/// is shown. A window that is already showing uses the plain adapter, and
/// this module subclasses that window so it can route `WM_GETOBJECT`,
/// `WM_SETFOCUS` and `WM_KILLFOCUS`. That path returns false when the
/// subclass cannot be installed. On macOS and iOS this returns false when
/// SDL has no native window. On Linux the adapter needs no window; with no
/// D-Bus session bus this returns false and logs that once. Attaching again
/// to the same window, while the adapter is still there, does nothing and
/// returns true.
///
/// @param window the game's window
/// @return true when an adapter is attached
[[nodiscard]] bool attach(SDL_Window* window);

/// Drops the adapter. Safe when none is attached.
void detach();

/// Reports whether an adapter is attached.
///
/// @return true while an adapter is attached
[[nodiscard]] bool attached();

/// Reports whether the adapter is attached to this window.
///
/// @param window the game's window
/// @return true when that window's adapter is still attached
[[nodiscard]] bool attached_to(SDL_Window* window);

/// Reports whether attach already failed for this window and should not be
/// tried again until the preference changes.
///
/// @param window the game's window
/// @return true when a later attach of this window is skipped
[[nodiscard]] bool attach_skipped(SDL_Window* window);

/// Records how the last attach finished, for the skip and for speech.
///
/// Speech is wanted when the preference is `on` and the adapter did not
/// attach. This does not speak.
///
/// @param window the window attach was given
/// @param succeeded true when an adapter is attached
void note_attach_finished(SDL_Window* window, bool succeeded);

/// Reports whether speech should run because the preference is `on` and the
/// bridge did not attach. Speech itself is elsewhere.
///
/// @return true when speech may run in place of the bridge
[[nodiscard]] bool speech_where_bridge_cannot();

/// Reports whether the system says a screen reader, or another assistive
/// client, is running.
///
/// The answer is cached for about half a second. Windows asks
/// `SystemParametersInfo`. macOS and iOS ask whether VoiceOver is on. Linux
/// asks AT-SPI. A build that leaves the bridge out returns false. The game
/// builds and updates the tree only while this is true.
///
/// @return true while a reader is running
[[nodiscard]] bool reader_running();

/// Reports whether a screen reader has asked for the tree.
///
/// Windows and macOS have no notice that the reader has gone, so once it has
/// asked it stays asked until detach. Linux and iOS drop it when the reader
/// disconnects. The game publishes a tree only while `reader_running` is
/// also true, so a query from something other than a screen reader does not
/// keep the tree.
///
/// @return true after a reader has asked
[[nodiscard]] bool active();

/// Tells the adapter the window's size and whether it holds the input focus,
/// when either has changed since the last call. With no adapter this does
/// nothing. It does not build a tree.
///
/// @param window the game's window
void refresh_window(SDL_Window* window);

} // namespace oa::platform::screen_reader
