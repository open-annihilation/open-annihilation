// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One computer seat played by a brain. The app reads the package and hands
// its bytes here. The seat copies them, opens a sandbox, and calls fits and
// start before the match's first tick.
#pragma once

#include "oa/base/sha256.hpp"
#include "oa/data/brain_facts.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/sim/brains/seat_calls.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

struct lua_State;

namespace oa::data::unit_definitions {
struct UnitDefinition;
}

namespace oa::sim::match_runtime {
class Match;
struct SetUpRecord;
} // namespace oa::sim::match_runtime

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
    /// Saved state from an earlier game, as plain-data bytes.
    ///
    /// Empty is no state: `start` receives nil. The bytes need only last for
    /// `seat_brain`. Damaged bytes fail seating.
    std::span<const uint8_t> saved{};
    /// Replaces the Classic fingerprint baked into this build, for this load.
    /// Tests set it. The app never sets it. Empty uses the baked-in value.
    std::string_view classic_fingerprint_for_tests{};
    /// Per-tick bank. The default is the brain profile's bank.
    oa::data::script_host::Bank bank{
        oa::data::script_host::default_tick_allowance,
        oa::data::script_host::default_bank_ticks,
        oa::data::script_host::default_call_time_limit
    };
    /// When true, the brain runs beside the built-in player. Its orders,
    /// squad changes and standing-bit writes are recorded and not applied.
    /// `fits` and `start` wait for the set-up record, and no difficulty is noted.
    bool shadow{};
    /// Sections to compare, comma-separated. Empty compares every section.
    /// The view need only last for `seat_brain`.
    std::string_view shadow_sections{};
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
    /// Instructions the load phase used. 0 when load did not run. The count
    /// steps by 1,000, so a short phase can report 0.
    uint64_t load_instructions{};
    /// Instructions `fits` used. 0 when `fits` did not run.
    uint64_t fits_instructions{};
    /// Instructions `start` used. 0 when `start` did not run.
    uint64_t start_instructions{};
    /// True when the load phase was entered. A short load can still leave
    /// `load_instructions` at 0.
    bool load_ran{};
    /// True when `fits` was called. A short call can still leave
    /// `fits_instructions` at 0.
    bool fits_ran{};
    /// True when `start` was called. A short call can still leave
    /// `start_instructions` at 0.
    bool start_ran{};
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

/// What asking a seated brain for its state came to.
enum class SaveOutcome : uint8_t {
    absent,  ///< no seated brain, or its table has no save function
    stored,  ///< plain data was handed to the caller
    omitted, ///< the return was not plain data, had no values, or passed 1 MiB
    failed,  ///< save raised or spent its allowance; the seat was handed back
};

/// Where a saved state is handed.
///
/// `accept` is called only after `save` returns and the bytes are plain data,
/// and only when those bytes are not empty. The span lasts for the call.
/// A null `accept` still counts as stored.
struct SaveHooks {
    /// Pointer passed to `accept`. May be null.
    void* context{};
    /// The state's bytes. Null skips the hand-off.
    void (*accept)(void* context, std::span<const uint8_t> bytes){};
};

/// Runs a seated brain's `save` and encodes the value it returns.
///
/// The call runs under a fresh phase allowance. A brain with no `save`
/// function leaves the seat as it is and returns `absent`. A return that is
/// not plain data, that has no values, or that would pass 1 MiB is left out
/// and the brain stays seated. A raise or a spent allowance hands the seat
/// back, as any other hook's failure does, and returns `failed`. The bytes
/// are accepted only after the call returns.
///
/// @param match the match
/// @param player player index, 0 to 9
/// @param hooks where the bytes go; `accept` may be null
/// @return absent, stored, omitted or failed
[[nodiscard]] SaveOutcome
save_brain_state(match_runtime::Match& match, uint8_t player, SaveHooks hooks);

/// Returns how many instructions one player's brain calls have used.
///
/// The total covers the load, fits, start and every tick that ran, including
/// a call that failed. It stays after the seat is handed back. Zero when
/// `player` is outside 0 to 9, the match has no brains state, or that player
/// never had a brain.
///
/// @param match the match
/// @param player player index, 0 to 9
/// @return the instruction total
[[nodiscard]] uint64_t
seat_instructions(const match_runtime::Match& match, uint8_t player) noexcept;

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

/// What a tool seat loads. The views need only last for `ToolSeat::open`.
///
/// The seat has no match. It reads the world, the random stream and the
/// squad calls in `calls`, and the unit definitions beside them.
struct ToolSeatSpec {
    /// The package, the level and the options. `shadow` stays false.
    SeatSpec spec{};
    /// Calls into the world this seat plays. Copied into the seat.
    SeatCalls calls{};
    /// Facts `game` is built from. Copied into the seat.
    oa::data::brain_facts::GameFacts facts{};
    /// Unit definitions indexed like the world's unit defs. May be null.
    const oa::data::unit_definitions::UnitDefinition* definitions{};
    /// How many `definitions` entries are addressable.
    std::size_t definition_count{};
    /// Set-up draws `brain.setup_random` replays. Null leaves the seat
    /// without a record.
    const match_runtime::SetUpRecord* setup{};
};

/// One brain seated with no match, for a test or a tool.
///
/// `open` loads the package, runs fits and start, and keeps the sandbox.
/// It does not take a player slot and it does not note a difficulty. `tick`
/// calls `hooks.onTick` under the per-tick bank. Destroying the seat closes
/// the sandbox.
class ToolSeat {
  public:

    /// An empty seat. `open` makes one that holds a brain.
    ToolSeat() = default;
    ToolSeat(const ToolSeat&) = delete;
    ToolSeat& operator=(const ToolSeat&) = delete;
    /// Moves the brain and leaves `other` empty.
    ToolSeat(ToolSeat&& other) noexcept;
    /// Moves the brain and leaves `other` empty.
    ToolSeat& operator=(ToolSeat&& other) noexcept;
    /// Closes the sandbox.
    ~ToolSeat();

    /// Loads `spec` and runs fits and start.
    ///
    /// On failure no seat is returned and `error` says why. A refusal uses
    /// `ErrorKind::runtime` and the refusal's sentence. A script failure
    /// copies the script error.
    ///
    /// @param spec the package and the world it plays
    /// @param[out] error set when no seat is returned
    /// @return the seat, or no value when it could not be seated
    [[nodiscard]] static std::optional<ToolSeat>
    open(const ToolSeatSpec& spec, oa::data::script_host::ScriptError& error);

    /// Runs `hooks.onTick` for `tick`.
    ///
    /// The world's tick is set to `tick` first, so `me.tick` matches the
    /// argument. A seat with no `onTick` runs nothing and succeeds.
    ///
    /// @param tick the turn, the same value `onTick` receives
    /// @param[out] error set when the call returns false
    /// @return false when this turn or an earlier phase failed
    [[nodiscard]] bool tick(uint32_t tick, oa::data::script_host::ScriptError& error);

    /// Whether this package matched the Classic fingerprint.
    ///
    /// The bit is the one `note_classic` sets. A match seat reads it through
    /// `seat_is_classic`.
    ///
    /// @return true when the seated package is Classic
    [[nodiscard]] bool classic() const noexcept;

    /// What seating came to. A moved-from seat reads an empty outcome.
    ///
    /// @return the outcome of `open`
    [[nodiscard]] const SeatOutcome& outcome() const noexcept;

    /// The sandbox, until the seat is destroyed or moved from.
    ///
    /// @return the sandbox, or null when the seat is empty
    [[nodiscard]] oa::data::script_host::Sandbox* sandbox() noexcept;

    /// Runs `work` with the seat table `hooks.seat` at the top of the stack.
    ///
    /// The stack is restored when `work` returns. `work` stores what it
    /// reads in `context`.
    ///
    /// @param context pointer passed to `work`; may be null
    /// @param work the call to make; null does nothing and succeeds
    /// @param[out] error set when the call returns false
    /// @return false when this phase or an earlier one failed
    [[nodiscard]] bool with_seat(
        void* context,
        void (*work)(void* context, lua_State* state),
        oa::data::script_host::ScriptError& error
    );

    /// Runs `work` on the sandbox, the way `Sandbox::run_host` does.
    ///
    /// @param phase name recorded on an error
    /// @param context pointer passed to `work`; may be null
    /// @param work the call to make; null does nothing and succeeds
    /// @param[out] error set when the call returns false
    /// @return false when this phase or an earlier one failed
    [[nodiscard]] bool host(
        std::string_view phase,
        void* context,
        void (*work)(void* context, lua_State* state),
        oa::data::script_host::ScriptError& error
    );

  private:

    struct Body;
    std::unique_ptr<Body> body{};
};

} // namespace oa::sim::brains
