// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// GET MAP in the battle room: what a joiner without the host's pack map is
// offered, decided from the host's presence record, the packs this machine
// has installed and what its registries offer, and the one line that says
// so. Everything here is pure; the application draws the button, its line
// and the download's bar on the OA layer, anchored to the battle room's map
// name (multiplayer_battle_room_map).
#pragma once

#include "oa/netgame/presence.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/panel.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace oa::ui::frontend_multiplayer {

/// The host's map pack, as its presence record names it (the map pack
/// field, tag 0x40).
struct HostPack {
    std::string id;           ///< the pack's id: the record's key
    std::string sha256;       ///< the release's SHA-256, 64 lower-case hex digits
    std::string registry_id;  ///< the id of the registry it was published in
    std::string registry_url; ///< the registry's descriptor URL; empty when the record has none
    std::string name;         ///< the pack's name; empty when the host could not send it
    std::string version;      ///< the pack's version; empty when the host could not send it
    int64_t release{};        ///< the catalogue release
};

/// Returns the host's pack from a presence record's map pack field.
///
/// @param pack the field, decoded
/// @return the pack: its key as the id and its SHA-256 as 64 lower-case hex digits
[[nodiscard]] HostPack host_pack_of(const netgame::PresenceMapPack& pack);

/// What this machine has installed of a map pack, by its id.
struct LocalPack {
    bool installed{};        ///< a pack of the id is installed
    std::string sha256;      ///< its SHA-256, 64 lower-case hex digits
    std::string registry_id; ///< the registry it came from; empty when it came from no catalogue
    int64_t release{};       ///< its catalogue release; 0 when it came from no catalogue
};

/// What the host pack's registry offers this machine.
struct Offer {
    bool registry_known{};     ///< the player has the registry: built in or added
    bool registry_enabled{};   ///< the registry is on and may be used
    bool downloads_on{};       ///< downloads from it are on, and the game has a downloads folder
    bool catalogue_loaded{};   ///< the registry's catalogue has been read
    bool listed{};             ///< the catalogue lists a map pack of the id
    std::string sha256;        ///< the listed release's SHA-256, 64 lower-case hex digits
    std::string pack_name;     ///< the listed pack's name
    std::string version;       ///< the listed pack's version; may be empty
    std::string registry_name; ///< the registry's name, as its descriptor gives it
    int64_t release{};         ///< the listed release
    uint64_t bytes{};          ///< the listed release's size, in bytes
    /// Why the listed release's map cannot be played in this machine's game,
    /// as its catalogue entry says (its base games, required expansions and
    /// compatible rows), in the words the map list uses for a greyed map, in
    /// the language shown. Empty when it can be played, and when the
    /// catalogue does not say, so the pack's files decide once it is fetched.
    std::string unplayable;
};

/// What GET MAP shows a joiner.
enum class GetMap : uint8_t {
    none,               ///< nothing: no pack map, no record, or the host's release installed
    get,                ///< the button: the host's release can be fetched
    add_registry_first, ///< the button: the registry must be added first
    other_release_get,  ///< the button: another release is installed; the host's can be fetched
    other_release_gone, ///< another release is installed; the host's is no longer offered
    gone,               ///< the host's release is not offered by a registry the player can use
    unplayable,    ///< the host's release is offered; its catalogue says this game cannot play it
    downloads_off, ///< the registry is off, or downloads from it are
    downloading,   ///< the bar: the host's release is on its way
    failed,        ///< the button: the last try failed, and says why
};

/// Decides what GET MAP shows a joiner.
///
/// In order: none when the host's map is not a pack map's name, there is no
/// record, the record's key is not the name's pack id, or the installed pack
/// has the host's SHA-256; then downloading and failed as given. For a pack
/// not installed: add_registry_first when the registry is unknown and the
/// record has its address (gone without one); downloads_off when the
/// registry or its downloads are off; downloading while a registry has no
/// catalogue read yet, as one just added; gone when its catalogue does not
/// list the id at the host's SHA-256; unplayable when the offer says why
/// this game cannot play it; else get. For another release installed:
/// unplayable when the registry offers the host's SHA-256 and says why this
/// game cannot play it, other_release_get when it offers it, else
/// other_release_gone.
///
/// @param host_map the host's map name, as its setup block carries it
/// @param host the host's map pack from its presence record; none without one
/// @param local this machine's pack of the id
/// @param offer what the registry offers
/// @param downloading a download of the host's release has not ended
/// @param failed the last download of it failed
/// @return the state
[[nodiscard]] GetMap get_map_state(
    std::string_view host_map,
    const std::optional<HostPack>& host,
    const LocalPack& local,
    const Offer& offer,
    bool downloading,
    bool failed
);

/// Returns the line GET MAP shows for a state, in the language shown.
///
/// The pack is named as the catalogue names it, else as the record does,
/// else by its id; the registry by its name, else by its id. The words come
/// from the interface's catalogue; a field left empty drops with the space
/// before it. Unplayable is the offer's own sentence, as it is.
///
/// @param state the state
/// @param host the host's map pack
/// @param local this machine's pack of the id
/// @param offer what the registry offers
/// @param done_bytes how much of the download has arrived, in bytes
/// @param error why the last try failed
/// @return the line; empty for none
[[nodiscard]] std::string get_map_text(
    GetMap state,
    const HostPack& host,
    const LocalPack& local,
    const Offer& offer,
    uint64_t done_bytes,
    std::string_view error
);

/// Returns the button's caption, "GET MAP", in the language shown.
///
/// @return the caption
[[nodiscard]] std::string get_map_caption();

/// Tells whether a state shows the button (get, add_registry_first,
/// other_release_get and failed).
///
/// @param state the state
/// @return true when GET MAP's button shows
[[nodiscard]] bool get_map_button_shown(GetMap state) noexcept;

/// The battle room as a joiner sees the host's map.
struct BattleRoomMap {
    std::string host_map;              ///< the host's map name, read within its field
    std::optional<HostPack> host_pack; ///< the map pack the host's stored record names
    /// The MAPNAME label's rectangle on the 640 by 480 frame: its panel
    /// control's, moved by the screen's offset.
    int32_t label_x{}, label_y{}, label_width{}, label_height{};
    bool joiner{}; ///< this machine does not host
};

/// Reads the host's map in a battle room: the host slot's map name, the map
/// pack of the record its player last sent (lobby_presence_record, decoded),
/// and the MAPNAME control's rectangle.
///
/// @param lobby the battle room's lobby
/// @param panel the battle room's panel
/// @param offset_x columns the panel's controls move on the frame
/// @param offset_y rows they move
/// @param[out] out the map; reset first
/// @return false when no host slot is found or the panel has no MAPNAME
bool battle_room_map(
    Lobby& lobby, const Panel& panel, int32_t offset_x, int32_t offset_y, BattleRoomMap* out
) noexcept;

/// Reads the host's map in the battle room that shows (battle_room_map).
///
/// @param[out] out the map; reset first; null reads nothing
/// @return false while the battle room is not shown, a dialog covers it,
///     or battle_room_map finds no host or label
bool multiplayer_battle_room_map(BattleRoomMap* out) noexcept;

} // namespace oa::ui::frontend_multiplayer
