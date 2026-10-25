// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// How a match is presented while a director script renders it or the
// generator analyses it (Runtime's director members): the frame it draws and
// where its sounds go.
//
// In director mode the match is drawn at the output size from the camera the
// director sets, never the player's: the battlefield fills the frame unless
// the interface is asked for, no cursor is drawn, and nothing on the wall
// clock reaches the frame. The processor draws it, or, when asked, the
// graphics card's Full tier on a renderer with no window. Debris particles start once a tick whether or not
// the tick is drawn, so a pass that draws and one that does not play out the
// same. Every sound the match plays, every player's unit announcements
// included, goes to the sound hooks, placed from the director's camera,
// instead of to a sound device.
#pragma once

#include <cstdint>

namespace oa::app {

/// What director mode draws.
struct DirectorPresentation {
    uint32_t width{};  ///< the output frame's width in pixels, even
    uint32_t height{}; ///< its height in pixels, even
    /// Draw the game's interface around and over the battlefield (showUx).
    /// Frames with it are not certain to be the same from run to run: the
    /// interface keeps timers of its own.
    bool show_interface{};
    /// Every player's unit announcements are heard, placed at the unit; the
    /// game itself announces only the viewing player's units.
    bool every_player_speaks{true};
    /// The frames are drawn by the graphics card's Full tier on a renderer
    /// of their own with no window, and read back, instead of by the
    /// processor. Its pixels differ from one driver to the next, so its
    /// frames are never pinned.
    bool full{};
};

/// A sound the match plays in director mode.
///
/// Its placement is the one the match gives a point sound: x, y and z
/// relative to the centre of the director's view, in map pixels, and the
/// distances within which it is heard at full level and beyond which it
/// fades no further.
struct DirectorSound {
    const char* resource{}; ///< the sound file's path in the archives; read at once
    int32_t volume{};       ///< hundredths of a decibel; 0 is full scale
    bool placed{};          ///< x to max_distance hold a placement; false plays unplaced
    int32_t x{};
    int32_t y{};
    int32_t z{};
    float min_distance{}; ///< map pixels
    float max_distance{}; ///< map pixels
    uint16_t unit{};      ///< the unit that speaks; 0 for a sound at a point
    uint32_t tick{};      ///< the game tick being run, or last run, as it played
};

/// Where director mode sends the match's sounds.
struct DirectorSoundHooks {
    void* context{};
    /// Takes a sound the match plays; null drops every sound.
    ///
    /// @param context DirectorSoundHooks::context
    /// @param sound the sound; valid for the call only
    void (*play)(void* context, const DirectorSound& sound){};
};

} // namespace oa::app
