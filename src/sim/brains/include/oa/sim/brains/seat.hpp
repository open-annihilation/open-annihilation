// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One computer seat played by a brain. The app reads the package and hands
// its bytes here. The seat copies them, opens a sandbox, and calls fits and
// start before the match's first tick.
#pragma once

#include "oa/base/sha256.hpp"
#include "oa/data/brain_facts.hpp"
#include "oa/data/script_host/script_host.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace oa::sim::match_runtime {
class Match;
}

namespace oa::sim::brains {

/// Most bytes of a brain id, not counting the terminating NUL.
inline constexpr std::size_t id_bytes = 64;
/// Most bytes of a brain's name, not counting the terminating NUL.
inline constexpr std::size_t name_bytes = 128;
/// Most bytes of a version, not counting the terminating NUL.
inline constexpr std::size_t version_bytes = 32;
/// Most bytes of a level id, not counting the terminating NUL.
inline constexpr std::size_t level_bytes = 64;
/// Most bytes of every network in one brain, taken together.
inline constexpr std::size_t network_bytes_limit = std::size_t{64} << 20;

/// One file of a package, already read. The view need only last for the call
/// that receives it.
struct PackageFile {
    /// Package path, with `/` separators, matched byte for byte.
    std::string_view path{};
    /// The file's bytes.
    std::span<const uint8_t> bytes{};
};

/// One network the manifest names. The views need only last for the call.
struct NetworkFile {
    /// The name a script will ask for.
    std::string_view id{};
    /// Package path of the network file, such as `nets/scout.oann`.
    std::string_view path{};
    /// The digest the manifest states for `path`.
    base::sha256::Digest sha256{};
};

/// How one option is stored.
enum class OptionKind : uint8_t {
    toggle, ///< on or off
    number, ///< a number
    choice, ///< one of a fixed list
};

/// One option value handed to a seat.
///
/// A choice's value is `text`, or `number` when `text` is empty. A number
/// option uses `number`. A toggle uses `toggle`.
struct OptionValue {
    /// The option's id.
    std::string_view id{};
    /// Which kind of value this is.
    OptionKind kind{};
    /// A toggle's value.
    bool toggle{};
    /// A number's value, or a numeric choice's value when `text` is empty.
    double number{};
    /// A choice's text. Empty when the choice's value is `number`.
    std::string_view text{};
};

/// What the app asks a seat to load.
///
/// Every view need only last for the `seat_brain` call. `bank` is for tests
/// and tools: the app always passes the defaults.
struct SeatSpec {
    /// Engine player index, 0 to 9.
    uint8_t player{};
    /// The brain's id.
    std::string_view id{};
    /// The name players see.
    std::string_view name{};
    /// The brain's version.
    std::string_view version{};
    /// Package path of the entry script.
    std::string_view entry{};
    /// The chosen level's id.
    std::string_view level{};
    /// OA_DIFFICULTY_EASY, MEDIUM or HARD.
    int32_t difficulty{};
    /// The manifest's full-awareness flag. Later tickets read it.
    bool full_awareness{};
    /// The option values, in the manifest's order.
    std::span<const OptionValue> options{};
    /// Every file of the package.
    std::span<const PackageFile> files{};
    /// The networks the manifest names.
    std::span<const NetworkFile> networks{};
    /// Replaces the Classic fingerprint baked into this build, for this load.
    /// Tests set it. The app never sets it. Empty uses the baked-in value.
    std::string_view classic_fingerprint_for_tests{};
    /// Per-tick bank. The default is the brain profile's bank.
    oa::data::script_host::Bank bank{
        oa::data::script_host::default_tick_allowance,
        oa::data::script_host::default_bank_ticks,
        oa::data::script_host::default_call_time_limit
    };
};

/// What seating a brain came to.
enum class SeatResult : uint8_t {
    seated,       ///< fits and start ran, and the seat's hooks are installed
    refused,      ///< the seat or the package was not one a brain may take
    failed,       ///< the script failed; `error` says how
    does_not_fit, ///< fits returned a reason, and the seat was not taken
};

/// The outcome of seating one brain.
struct SeatOutcome {
    /// What seating came to.
    SeatResult result{};
    /// The script error, when `result` is `failed` from the script.
    oa::data::script_host::ScriptError error{};
    /// A refusal's sentence, or the text fits returned, cut to 255 bytes.
    /// Empty when the failure is `error` alone.
    std::array<char, 256> reason{};
};

/// Loads one brain for one computer seat and calls fits and start.
///
/// On success the seat's hooks are installed and its play skips the built-in
/// order tick. On any failure the sandbox is closed, the seat is removed and
/// no hooks are installed. No script runs before every network's digest has
/// been checked.
///
/// @param match the match; the brains' state is kept on it
/// @param spec the package, the level and the options
/// @return seated, refused, failed or does not fit
[[nodiscard]] SeatOutcome seat_brain(match_runtime::Match& match, const SeatSpec& spec);

/// Tells whether a brain is seated for one player.
///
/// @param match the match
/// @param player player index, 0 to 9
/// @return true when that seat's hooks are a seated brain's
[[nodiscard]] bool brain_seated(const match_runtime::Match& match, uint8_t player) noexcept;

/// Returns the sandbox of a seated brain.
///
/// For tests and tools. The pointer lasts until the seat is removed or the
/// match ends.
///
/// @param match the match
/// @param player player index, 0 to 9
/// @return the sandbox, or null without a seated brain
[[nodiscard]] oa::data::script_host::Sandbox*
seat_sandbox(match_runtime::Match& match, uint8_t player) noexcept;

/// Stores the facts every brain seated afterwards reads as `game`.
///
/// A seat made before any facts were stored reads a default game: base
/// `totala`, the base mod and no mutator. The facts are copied into the
/// match's brains state, so later seats keep a stable address. Taken by
/// value. A failure to store the state leaves the match unchanged.
///
/// @param match the match
/// @param facts the facts, owned by the caller until this returns
void set_match_game_facts(match_runtime::Match& match, oa::data::brain_facts::GameFacts facts);

/// Where a seat reports printed lines and a stop.
///
/// The simulation calls these during a turn. It writes no file and posts no
/// message. `printed` and `stopped` must not throw. A null function is skipped.
struct ReportHooks {
    /// Pointer passed to both functions. May be null.
    void* context{};
    /// One printed line, oldest first. The view lasts for the call.
    void (*printed)(void* context, uint8_t player, uint32_t tick, std::string_view line){};
    /// Called once when a seated turn fails. The error lasts for the call.
    void (*stopped)(
        void* context,
        uint8_t player,
        uint32_t tick,
        const oa::data::script_host::ScriptError& error
    ){};
};

/// Stores the hooks later seats report through.
///
/// The hooks are kept on the match, including before any brain is seated.
/// A failure to store the state leaves the match unchanged.
///
/// @param match the match
/// @param hooks the hooks; empty functions report nothing
void set_report_hooks(match_runtime::Match& match, ReportHooks hooks) noexcept;

} // namespace oa::sim::brains
