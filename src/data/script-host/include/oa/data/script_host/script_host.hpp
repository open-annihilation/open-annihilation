// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One Lua state per package, with a profile, a memory cap, a fresh
// instruction allowance and a fresh real-time limit for every phase. A
// profile may also carry a bank for one call at a time. Scripts see only
// the libraries this host opens, and every error comes back as a value.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace oa::data::script_host {

/// Instructions one phase may run before that phase stops.
inline constexpr uint64_t default_instruction_budget = 50'000'000;

/// Bytes of Lua memory one sandbox may hold.
inline constexpr std::size_t default_memory_budget = std::size_t{64} << 20;

/// Instructions between checks of the running allowance.
inline constexpr int budget_check_interval = 1000;

/// Bytes of a string's length, or of a scan, that count as one instruction.
///
/// Making or walking a string byte by byte takes about as long per 32 bytes
/// as a few instructions of the interpreter. A count uses this, never the
/// size the allocator happened to reserve, so it is the same on every build.
inline constexpr std::size_t bytes_per_charge = 32;

/// How many instruction units `bytes` cost, one per `bytes_per_charge`, rounded up.
///
/// @param bytes a length the script can see, such as a key or a path
/// @return the units, 0 when `bytes` is 0
[[nodiscard]] inline uint64_t units_for_bytes(std::size_t bytes) {
    return (static_cast<uint64_t>(bytes) + bytes_per_charge - 1) / bytes_per_charge;
}

/// Units for one comparison that is not two strings, and the least two strings cost.
///
/// One comparison in `table.sort`, `pairs` or `math.max` takes about as long
/// as one or two instructions of the interpreter. Two strings cost one unit
/// per byte of the two lengths when that is more than this.
inline constexpr uint64_t comparison_unit = 2;

/// Units for one `next` call, or one walk that collects keys for `pairs`.
///
/// An emptied table can make that call visit every slot it used to hold.
/// 4096 units spends the default allowance before a loop of those calls
/// runs for long.
inline constexpr uint64_t scan_call_charge = 4096;

/// Units one element of a table, record or key list costs.
///
/// `table.move` and the other table walks charge this for each element they
/// move or visit. A host walk over records or keys uses the same rate.
inline constexpr uint64_t element_charge = 1;

/// Units for one `where` or `:change` table, before its entries.
///
/// The walk is capped at the statement limit plus one entry. An emptied
/// table that makes `next` visit every former slot is stopped by the phase's
/// real-time limit. This flat charge stays small so a plan with one block
/// per record still fits the default allowance. Each entry costs
/// `element_charge` as well, and each key's bytes cost `units_for_bytes`
/// when every key is visited.
inline constexpr uint64_t table_walk_charge = 64;

/// Real time one phase may run before that phase stops.
///
/// A steady clock is read when the phase starts. The count hook and the
/// library charges compare it with this limit. The limit stops work the
/// instruction count cannot see. It never changes a result that does come
/// back, and a script near the limit can pass on one machine and fail on
/// another.
inline constexpr std::chrono::milliseconds default_time_limit{10000};

/// Instructions a bank adds for one tick.
///
/// A banked call spends from a balance that starts at `default_bank_ticks`
/// times this allowance. The figure is a starting point, to be revised once
/// a full computer-player script has been measured.
inline constexpr uint64_t default_tick_allowance = 2'000'000;

/// Most ticks of allowance a bank may hold, the current tick included.
///
/// The balance opens at this many ticks and a refill never passes it.
inline constexpr uint32_t default_bank_ticks = 30;

/// Real time one banked call may run before that call stops.
///
/// This is a backstop for a hang. It does not change the instruction balance.
inline constexpr std::chrono::milliseconds default_call_time_limit{250};

/// Most bytes in one string. A script that would make a longer one stops.
inline constexpr std::size_t string_length_limit = std::size_t{64} << 10;

/// Most bytes string.rep may build, separators included.
inline constexpr std::size_t string_rep_limit = string_length_limit;

/// Most bytes of source one file may hold.
inline constexpr std::size_t max_source_bytes = std::size_t{1} << 20;

/// Most bytes of a require name.
inline constexpr std::size_t max_module_name_bytes = 64;

/// Most modules that may be loading inside one require chain.
inline constexpr int max_require_depth = 32;

/// Most print lines a sandbox keeps.
inline constexpr std::size_t max_printed_lines = 256;

/// Most bytes of print text a sandbox keeps, the closing note included.
inline constexpr std::size_t max_printed_bytes = std::size_t{16} << 10;

/// Most bytes stored for one error's stack listing.
inline constexpr std::size_t max_traceback_bytes = 2048;

/// Why a phase stopped. `none` is not a failure.
enum class ErrorKind : uint8_t {
    none,
    syntax,
    runtime,
    instruction_budget,
    memory_budget,
    precompiled,
    source_too_large,
    module_name,
    module_missing,
    module_loop,
    /// A string would have passed `string_length_limit`.
    string_length,
    /// The phase ran longer than its real-time limit.
    ///
    /// The limit is `default_time_limit` unless `OpenOptions::time_limit`
    /// names another. The hook and the library charges notice it at their
    /// next check, so the phase can run a little past the limit. A script
    /// near the limit can pass on one machine and fail on another. The
    /// limit only stops the script: a result that comes back is unchanged.
    /// In multiplayer the loadout digest refuses a match whose machines
    /// disagree, so the two outcomes cannot run as one match. A dry run
    /// records this failure like any other.
    time_limit
};

/// A phase that stopped, naming the package and where in its source.
struct ScriptError {
    ErrorKind kind{};
    std::string package_id;
    std::string phase;
    std::string message;
    /// Package path of the chunk, such as `lua/util.lua`. Empty when unknown.
    std::string chunk;
    /// Line in `chunk`, or 0 when the line is unknown.
    int32_t line{};
    std::string traceback;
};

/// Writes an error as one line.
///
/// @param error the error to write
/// @return `package: phase: chunk:line: message`, leaving out `chunk:line: `
///     when the chunk is unknown
[[nodiscard]] std::string describe(const ScriptError& error);

/// How much of the allowance and the memory cap a profile gives each state.
struct Budget {
    uint64_t instructions{default_instruction_budget};
    std::size_t memory_bytes{default_memory_budget};
};

/// A per-tick instruction balance a profile may carry.
///
/// `per_tick` 0 means the profile has no bank. `ticks` below 1 counts as 1.
/// The limit is `per_tick` times `ticks`, saturating at `UINT64_MAX`. The
/// bank opens full, so the first call may spend the whole limit. That limit
/// counts the current tick.
struct Bank {
    /// Instructions added at each refill. Zero means the profile has no bank.
    uint64_t per_tick{};
    /// How many ticks of `per_tick` the bank may hold, the current tick included.
    uint32_t ticks{default_bank_ticks};
    /// Real time one banked call may run. The default is `default_call_time_limit`.
    std::chrono::milliseconds call_time_limit{default_call_time_limit};
};

/// Reads one package file by its exact path. `read` is null when every file is missing.
///
/// `read` returns false when the package has no file at that path. It matches
/// the path byte for byte and does not fold case. It does not call into the
/// state. The host clears `bytes` before the call.
struct SourceHooks {
    void* context{};
    bool (*read)(void* context, std::string_view path, std::vector<uint8_t>& bytes){};
};

/// What a use of the host adds to the shared libraries.
///
/// `name` is a literal that outlives the open call. `install` runs inside the
/// protected open, may raise, and adds that profile's globals. Null adds none.
struct Profile {
    std::string_view name{};
    Budget budget{};
    void* context{};
    void (*install)(void* context, lua_State* state){};
    /// Per-tick instruction bank. `per_tick` 0 means this profile has no bank.
    Bank bank{};
};

/// Returns the profile a mutator runs under: default budgets and no extra globals.
///
/// @return the mutator profile
[[nodiscard]] Profile mutator_profile() noexcept;

/// What `open` needs to make one sandbox.
struct OpenOptions {
    std::string package_id;
    Profile profile{};
    SourceHooks sources{};
    /// Value `math.random` starts from, and the value `math.randomseed()` with
    /// no argument returns to. The caller passes the game's seed. Zero is a
    /// defined sequence, not an extra draw.
    uint64_t random_seed{};
    /// Real time each phase of this sandbox may run. Each phase starts
    /// again from the full value. The default is `default_time_limit`.
    std::chrono::milliseconds time_limit{default_time_limit};
};

/// Pushes a call and reads it, both inside the protected phase.
///
/// `push` pushes the function and its arguments and returns how many arguments
/// it pushed. `read` reads the values the call returned. `results` is how
/// many of those values sit at the top of the stack. Either may raise.
/// A null `read` leaves the results unread. Neither call holds a C++ object
/// with a destructor across a raising call, and neither throws.
struct CallHooks {
    void* context{};
    int (*push)(void* context, lua_State* state){};
    void (*read)(void* context, lua_State* state, int results){};
};

/// One package's state. Movable, not copyable. Destroying it closes the state.
class Sandbox {
  public:

    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;
    /// Moves the state from another sandbox and leaves that sandbox empty.
    Sandbox(Sandbox&&) noexcept;
    /// Moves the state from another sandbox and closes the state held here.
    Sandbox& operator=(Sandbox&&) noexcept;
    /// Closes the state.
    ~Sandbox();

    /// Opens a state, loads the shared libraries and runs the profile's install.
    ///
    /// @param options package, profile and file reader
    /// @param[out] error set when no sandbox is returned
    /// @return the sandbox, or no value when the state cannot be made
    [[nodiscard]] static std::optional<Sandbox>
    open(const OpenOptions& options, ScriptError& error);

    /// Runs the caller's work on the state inside the phase's allowance.
    ///
    /// No script runs. This is how a caller registers functions, userdata
    /// metatables and globals before any script, or reads a table a script left.
    /// A failure here fails the sandbox the same way a script failure does.
    ///
    /// @param phase name recorded on an error
    /// @param context pointer passed to `work`; may be null
    /// @param work the call to make; null does nothing and succeeds
    /// @param[out] error set when the call returns false
    /// @return false when this phase or an earlier one failed
    [[nodiscard]] bool run_host(
        std::string_view phase,
        void* context,
        void (*work)(void* context, lua_State* state),
        ScriptError& error
    );

    /// Loads `path` as a text chunk and runs it as the phase.
    ///
    /// The bytes come from the sources given to `open`. `results` may be null.
    /// When it is not, only its `read` is called, with the values the chunk
    /// returned.
    ///
    /// @param path package path of the chunk
    /// @param phase name recorded on an error
    /// @param results where to read the chunk's results; may be null
    /// @param[out] error set when the call returns false
    /// @return false when this phase or an earlier one failed
    [[nodiscard]] bool run_file(
        std::string_view path, std::string_view phase, const CallHooks* results, ScriptError& error
    );

    /// Calls the function `hooks.push` pushes, under a fresh allowance.
    ///
    /// @param phase name recorded on an error
    /// @param hooks push and read for this call
    /// @param[out] error set when the call returns false
    /// @return false when this phase or an earlier one failed
    [[nodiscard]] bool call(std::string_view phase, const CallHooks& hooks, ScriptError& error);

    /// Adds one tick's allowance to the bank, up to its limit.
    ///
    /// Does nothing when the profile has no bank or this sandbox was moved from.
    /// The balance is a count. This call does not read a clock.
    void refill_bank() noexcept;

    /// Calls the function `hooks.push` pushes, under the bank's balance.
    ///
    /// The instruction allowance for this call is the balance, and the
    /// real-time limit is the bank's. When the call returns, the instructions
    /// it used are taken off the balance, and the profile's own allowance and
    /// real-time limit are put back. A profile with no bank returns false,
    /// runs nothing and leaves the sandbox usable. A balance of zero fails
    /// the sandbox without running the call.
    ///
    /// @param phase name recorded on an error
    /// @param hooks push and read for this call
    /// @param[out] error set when the call returns false
    /// @return false when this call or an earlier phase failed, or when the
    ///     profile has no bank
    [[nodiscard]] bool
    call_banked(std::string_view phase, const CallHooks& hooks, ScriptError& error);

    /// Returns the instructions the bank still holds.
    ///
    /// @return the balance, or 0 when the profile has no bank or this sandbox
    ///     was moved from
    [[nodiscard]] uint64_t bank_balance() const noexcept;

    /// Returns the most instructions the bank may hold.
    ///
    /// The limit counts the current tick. The bank opens holding this many.
    ///
    /// @return the limit, or 0 when the profile has no bank or this sandbox
    ///     was moved from
    [[nodiscard]] uint64_t bank_limit() const noexcept;

    /// Returns the state a hook may use while a phase is running.
    ///
    /// @return the state, or null after the sandbox has been moved from
    [[nodiscard]] lua_State* state() const noexcept;

    /// Reports whether a phase has already failed.
    ///
    /// @return true after a failed phase, until the sandbox is destroyed
    [[nodiscard]] bool failed() const noexcept;

    /// Returns the error of the phase that failed.
    ///
    /// @return that error; a default error when no phase has failed
    [[nodiscard]] const ScriptError& failure() const noexcept;

    /// Returns the instruction count recorded for the last phase.
    ///
    /// The count steps by `budget_check_interval`. A short phase can report 0.
    ///
    /// @return instructions counted in the last phase, or 0 after a move
    [[nodiscard]] uint64_t instructions_used() const noexcept;

    /// Returns the interpreter's string header, measured when the state opened.
    ///
    /// Every sandbox measures the same size. The string cap uses it so a
    /// string's length, not the size of its header, is what the cap compares.
    ///
    /// @return that size in bytes, or 0 after a move
    [[nodiscard]] std::size_t string_header_bytes() const noexcept;

    /// Returns the bytes the state currently holds.
    ///
    /// This is the Lua allocation only, not text the host has copied out.
    ///
    /// @return bytes in use, never above the sandbox's cap, or 0 after a move
    [[nodiscard]] std::size_t memory_in_use() const noexcept;

    /// Returns the lines `print` kept, including a closing note when lines were dropped.
    ///
    /// The span is valid until the next `print` in this sandbox or until it is destroyed.
    ///
    /// @return the kept lines, empty after a move
    [[nodiscard]] std::span<const std::string> printed() const noexcept;

  private:

    struct Control;

    /// Lets this module's own sources name `Control`.
    friend struct HostDetail;

    /// Builds a sandbox that already owns its control block and state.
    Sandbox(std::unique_ptr<Control> control, lua_State* state) noexcept;

    /// Closes the state and leaves this sandbox empty.
    void close() noexcept;
    /// Rejects a moved-from sandbox or one whose phase has already failed.
    ///
    /// @param[out] error set when the call returns false
    /// @return false when no phase can run
    [[nodiscard]] bool prepare(ScriptError& error);
    /// Runs one protected phase.
    ///
    /// @param phase name recorded on an error
    /// @param[out] error set when the call returns false
    /// @return false when this phase or an earlier one failed
    [[nodiscard]] bool enter(std::string_view phase, ScriptError& error);
    /// Records `error` and marks the sandbox failed.
    ///
    /// @param error the failure later phases return
    void fail(const ScriptError& error);

    /// Records that the host ran out of memory while finishing a phase.
    ///
    /// @param[out] error set to a memory-budget error
    /// @return false
    [[nodiscard]] bool fail_allocation(ScriptError& error);

    std::unique_ptr<Control> control_;
    lua_State* state_{};
    bool failed_{};
    ScriptError failure_{};

    /// Loads a chunk in a throwaway state and does not run it.
    friend bool
    compile_check(std::span<const uint8_t> source, std::string_view path, ScriptError& error);
};

/// Loads a chunk in a throwaway state and does not run it.
///
/// The state uses the default memory cap. A chunk that would fail or print
/// when run is still accepted. Precompiled bytes and a file past
/// `max_source_bytes` are refused before the loader sees them.
///
/// @param source the chunk's bytes
/// @param path package path recorded on the chunk and on an error
/// @param[out] error set when the call returns false; phase is `compile`
/// @return false when the bytes are refused or the chunk does not load
[[nodiscard]] bool
compile_check(std::span<const uint8_t> source, std::string_view path, ScriptError& error);

/// Adds `units` to the running phase and raises when the allowance is spent.
///
/// This is the count the libraries use. `units` must come from element
/// counts and byte lengths, so one script spends the same count on every
/// build and every run. A spent allowance raises the instruction-budget
/// error, as the count hook does, and arms that hook so a protected call
/// cannot keep the phase alive. A string refusal or a phase past its
/// real-time limit raises that error instead. No C++ object with a
/// destructor may be alive across this call: the raise leaves such an
/// object unrun.
///
/// @param state a sandbox state inside a phase
/// @param units instruction units to add; 0 adds nothing and still raises
///     when the allowance is already spent
void charge_work(lua_State* state, uint64_t units);

} // namespace oa::data::script_host
