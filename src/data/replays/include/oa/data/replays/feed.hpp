// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Core Prime's replay list, as svc-content publishes it. The game reads the
// JSON. It sends no GraphQL. Nothing here fetches: the caller asks
// `{origin}/content/v1/replays` and reads the answer with these functions.
#pragma once

#include "oa/data/replays/replays.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::replays {

/// The address the game asks when no other address was given.
inline constexpr std::string_view core_prime_feed_origin = "https://api.coreprime.net";

/// The path of the list, under the origin.
inline constexpr std::string_view replay_feed_path = "/content/v1/replays";

/// How many replays one page asks for, unless the caller says otherwise.
inline constexpr int32_t replay_page_size = 48;

/// The most replays one page may ask for.
inline constexpr int32_t replay_page_most = 100;

/// How many characters follow `r_` in a replay id.
inline constexpr std::size_t replay_id_body = 22;

/// What a feed answer was.
enum class FeedStatus : uint8_t {
    page,       ///< a list page
    entry,      ///< one replay
    error,      ///< an error object
    unreadable, ///< empty, damaged, or `replays` other than 1
    refused,    ///< an id that is not `r_` and 22 base64url characters
};

/// One player, as the feed lists them.
struct FeedPlayer {
    std::string name{};             ///< the player's name
    std::string side{};             ///< the side
    int32_t colour{};               ///< the colour, as a number
    std::optional<int32_t> team{};  ///< the team, when named
    std::string result{};           ///< won, lost, or empty
    std::optional<int64_t> kills{}; ///< units destroyed, when named
};

/// One file the feed names.
struct FeedFile {
    std::string sha256{}; ///< the file's SHA-256, as published
    uint64_t size{};      ///< the file's size, in bytes
};

/// A mod the battle was played with. Absent for the base game.
struct FeedMod {
    std::string id{};                  ///< the mod's id
    std::string name{};                ///< the name a player sees
    std::string version{};             ///< the version
    std::optional<int64_t> revision{}; ///< the revision, when named
};

/// One replay in the feed.
struct FeedEntry {
    std::string id{};          ///< `r_` and 22 base64url characters
    int64_t revision{};        ///< the revision a download asks for
    std::string title{};       ///< the title the feed gives
    std::string source{};      ///< `upload` or `capture`
    std::string visibility{};  ///< who can see it
    std::string recorded_at{}; ///< the time as published
    int64_t recorded{};        ///< that time, in seconds since 1970 (UTC); 0 when it did not read
    uint64_t length_ms{};      ///< the battle's length, in milliseconds
    std::string game_kind{};   ///< game.kind
    std::string game_id{};     ///< game.game_id, when named
    std::string planet{};      ///< game.planet, when named
    std::string sector{};      ///< game.sector, when named
    std::string map_name{};    ///< map.name
    std::vector<FeedPlayer> players{}; ///< the players, in the feed's order
    std::optional<FeedMod> mod{};      ///< the mod; none for the base game
    std::string engine_needs{};        ///< the oldest engine that plays it
    std::string uploader{};            ///< the uploader's handle; empty for a capture
    int64_t downloads{};               ///< how many times it has been downloaded
    FeedFile oademo{};                 ///< the `.oademo`
    FeedFile tad{};                    ///< the `.tad`; the game does not download it
    bool needs_present{};              ///< the answer included `needs`
    /// The same replay as the Library compares content for. Filled from the
    /// entry, and from `needs` when the answer included them.
    Replay replay{};
};

/// One page of the list.
struct FeedPage {
    int64_t total{};                ///< how many replays the filter matches
    std::string next{};             ///< the cursor of the next page; empty on the last page
    std::vector<FeedEntry> items{}; ///< the replays on this page
};

/// An error the feed named.
struct FeedFailure {
    std::string error{};   ///< the code
    std::string message{}; ///< one sentence
};

/// A list answer.
struct FeedPageResult {
    FeedStatus status{FeedStatus::unreadable}; ///< page, error or unreadable
    FeedPage page{};                           ///< set for a page
    FeedFailure failure{};                     ///< set for an error
};

/// One replay's answer.
struct FeedEntryResult {
    FeedStatus status{FeedStatus::unreadable}; ///< entry, error, unreadable or refused
    FeedEntry entry{};                         ///< set for an entry
    FeedFailure failure{};                     ///< set for an error
};

/// What a list request asks for. An empty string is left out of the query.
struct FeedQuery {
    std::string sort{"newest"};       ///< `newest`, `longest` or `downloads`
    std::string kind{};               ///< game.kind
    std::string source{};             ///< `upload` or `capture`
    std::string planet{};             ///< the planet
    std::string map{};                ///< the map's name
    std::string player{};             ///< a player's name
    std::string mod{};                ///< a mod's id
    std::optional<int32_t> players{}; ///< how many players
    std::string after{};              ///< the cursor of the page before this one
    int32_t first{replay_page_size};  ///< how many to ask for, kept from 1 to replay_page_most
};

/// Tells whether an id may be asked for.
///
/// An id is `r_` and then exactly 22 characters from `A-Z`, `a-z`, `0-9`,
/// `_` and `-`. Any other text is refused, and is never put in a request.
///
/// @param id the id
/// @return true when it is that form
[[nodiscard]] bool replay_id_ok(std::string_view id) noexcept;

/// Reads a list answer.
///
/// A page has `replays` equal to 1. An item whose id is not accepted is left
/// out. A member this version does not know is ignored. An empty body, a body
/// that is not JSON, and `replays` other than 1 are unreadable. An object
/// with `error` and no `replays` is an error.
///
/// @param text the body
/// @return the page, the error, or unreadable
[[nodiscard]] FeedPageResult parse_replay_page(std::string_view text);

/// Reads one replay's answer.
///
/// The body is the entry itself. `needs` is applied onto the replay. An id
/// that is not accepted is refused. An empty body, a body that is not one
/// object, and a body without an id are unreadable.
///
/// @param text the body
/// @return the entry, the error, unreadable, or refused
[[nodiscard]] FeedEntryResult parse_replay_entry(std::string_view text);

/// Writes the list's query, without a leading `?`.
///
/// `sort` is `newest` when it is empty. `first` is kept from 1 to
/// replay_page_most, and is replay_page_size when it is not a positive number.
/// An empty filter is left out.
///
/// @param query what the list asks for
/// @return the query
[[nodiscard]] std::string feed_query_text(const FeedQuery& query);

/// Joins an origin and the list's path and query.
///
/// A trailing `/` on the origin is dropped. The origin is used as given:
/// the caller passes Core Prime's origin, or a loopback address.
///
/// @param origin the origin, with its scheme
/// @param query what the list asks for
/// @return the address
[[nodiscard]] std::string feed_list_url(std::string_view origin, const FeedQuery& query);

/// Joins an origin and one replay's path.
///
/// @param origin the origin, with its scheme
/// @param id the replay's id
/// @return the address, or empty when the id is not accepted
[[nodiscard]] std::string feed_entry_url(std::string_view origin, std::string_view id);

} // namespace oa::data::replays
