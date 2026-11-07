// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The text a photo carries beside its picture. Pure: no window, no drawing
// and no files. The runtime supplies the facts, and keeps the returned
// strings until the PNG write returns.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oa::present::photo {

/// A tEXt chunk, whose text is Latin-1, or an uncompressed iTXt chunk, whose
/// text is UTF-8.
enum class ChunkKind : uint8_t {
    /// Latin-1 text, written as a tEXt chunk.
    latin1,
    /// UTF-8 text, written as an uncompressed iTXt chunk.
    utf8,
};

/// The kind of battle a photo was taken in.
enum class GameKind : uint8_t {
    /// A skirmish on this machine.
    skirmish,
    /// A campaign mission.
    campaign,
    /// A recording being watched.
    replay,
    /// A match shared with other machines.
    multiplayer,
};

/// The UTC date and time a photo was saved, already broken down.
struct UtcMoment {
    int year = 1970; ///< the full year, such as 1994
    int month = 1;   ///< the month, 1 to 12
    int day = 1;     ///< the day of the month, 1 to 31
    int hour = 0;    ///< the hour, 0 to 23
    int minute = 0;  ///< the minute, 0 to 59
    int second = 0;  ///< the second, 0 to 60
};

/// One player a photo names, or leaves unnamed. A team is numbered from 1.
/// Nothing means the player has no team.
struct PlayerFact {
    std::string name;        ///< the name, in the game's text; ignored when names are off
    std::string side;        ///< the side, in the game's text
    int colour = 0;          ///< the colour slot
    std::optional<int> team; ///< the team, from 1, or nothing
};

/// Where the camera was.
struct CameraFact {
    int64_t x = 0;    ///< map position, horizontal
    int64_t z = 0;    ///< map position, vertical
    double zoom = 1;  ///< the zoom
    double yaw = 0;   ///< degrees about the vertical
    double pitch = 0; ///< degrees from the classic view
};

/// The picture's size.
struct SizeFact {
    int64_t width = 0;  ///< pixels
    int64_t height = 0; ///< pixels
    int smooth = 1;     ///< the smooth step the picture was saved at
};

/// A look applied to the picture. Written only when the photo has one.
struct LookFact {
    std::string preset;            ///< the look's name
    int vignette = 0;              ///< the vignette, 0 to 100
    int grain = 0;                 ///< the grain, 0 to 100
    std::optional<int> tilt_shift; ///< the tilt, or nothing when the look has none
};

/// A replay the photo was taken from. Written only when the photo has one.
/// `file` may be a path. Only the file's own name is written, and only while
/// player names are on. With names off the recording is its digest and tick.
struct ReplayFact {
    std::string file;   ///< the recording's path or file name
    std::string sha256; ///< the recording's digest, as hex
    int64_t tick = 0;   ///< the tick inside the recording
};

/// Everything the caller knows about a photo. A folder, an account and a
/// place a machine can be reached are accepted so a caller can hand them
/// over, and are never written.
struct PhotoFacts {
    std::string map;                    ///< the map's name, in the game's text
    std::string engine;                 ///< the engine version, without the program name
    std::string mod_id;                 ///< the mod's id; empty when no mod is playing
    std::string mod_version;            ///< the mod's version; empty when no mod is playing
    GameKind kind = GameKind::skirmish; ///< the kind of battle
    uint32_t tick = 0;                  ///< the match's tick
    int speed = 0;                      ///< the match's speed
    std::vector<PlayerFact> players;    ///< the players, in slot order
    CameraFact camera;                  ///< the camera
    SizeFact size;                      ///< the picture
    UtcMoment created;                  ///< when the file was saved, in UTC
    bool player_names = true;           ///< false leaves every player's name out
    std::optional<ReplayFact> replay;   ///< set only for a photo of a replay
    std::optional<LookFact> look;       ///< set only when a look is applied
    std::string shots_folder;           ///< never written
    std::string account;                ///< never written
    std::string host;                   ///< never written
};

/// One text chunk, owning its keyword and its text.
struct PhotoText {
    ChunkKind kind = ChunkKind::utf8; ///< tEXt or uncompressed iTXt
    std::string keyword;              ///< the chunk's keyword
    std::string text;                 ///< the chunk's text
};

/// Builds the text chunks a photo stores.
///
/// The order is Title, Description, Software, Creation Time, Source and
/// OpenAnnihilation. Title and Description are UTF-8. Software, Creation
/// Time and Source are Latin-1. OpenAnnihilation is a JSON record of version
/// 1. Game text is read as UTF-8, and a byte that is not UTF-8 is read in
/// the game's code page. With `player_names` off, no player's name appears
/// in any chunk, and a replay is written as its digest and tick. A team is
/// written only for a player who has one: a real team is numbered from 1,
/// and no team is JSON null. A folder, an account, a place a machine can
/// be reached, and a replay's directory are left out. `replay` and `look`
/// are written only when present.
///
/// The returned strings own their text. The caller keeps them until the PNG
/// write that borrows them has returned.
///
/// @param facts the photo's facts
/// @return the chunks, in the order a photo writes them
[[nodiscard]] std::vector<PhotoText> photo_texts(const PhotoFacts& facts);

} // namespace oa::present::photo
