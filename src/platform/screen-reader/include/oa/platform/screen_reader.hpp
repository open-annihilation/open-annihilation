// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The screen-reader bridge. It wraps AccessKit's adapter for the game's SDL
// window. Nothing here runs when the game starts: a caller loads the library
// and attaches a window. The tree a caller has not replaced is one window
// node and one test node.
#pragma once

#include <cstdint>

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

/// The window node's id in the tree this module builds.
inline constexpr uint64_t window_node = 1;

/// The test node's id. It is a child of the window node.
inline constexpr uint64_t test_node = 2;

/// The label the test node carries.
inline constexpr const char* test_label = "Test";

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

/// Attaches the platform adapter to a window.
///
/// The window's native handle is the SDL property for this system. Nothing
/// is attached until this is called. On Windows this returns false, and does
/// not call AccessKit, when the window is already showing: AccessKit aborts
/// if the window is visible. On macOS and iOS this returns false when SDL
/// has no native window. SDL does not report the content view or the root
/// view controller, so those stay the caller's to have ready.
///
/// @param window the game's window
/// @return true when an adapter is attached
[[nodiscard]] bool attach(SDL_Window* window);

/// Drops the adapter. Safe when none is attached.
void detach();

} // namespace oa::platform::screen_reader
