// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The sandbox's own state. One control block belongs to one state, and the
// allocator's pointer to it does not move when the sandbox object does.
#pragma once

#include "oa/data/script_host/script_host.hpp"

#include "lua.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oa::data::script_host {

inline constexpr std::size_t path_buffer_bytes = 80;
inline constexpr std::size_t message_buffer_bytes = 512;
inline constexpr std::size_t chunk_buffer_bytes = 160;
inline constexpr std::size_t scratch_buffer_bytes = 3072;
inline constexpr std::size_t budget_message_bytes = 160;

// `bytes_per_charge`, `units_for_bytes`, `comparison_unit`, `scan_call_charge`,
// `element_charge` and `table_walk_charge` are in the public header. The
// mutator collections and rule forms charge through `charge_work` at those
// same rates.

/// Longest string the interpreter keeps one shared copy of (Lua 5.4.9's
/// short-string limit, the same on every build).
///
/// Making a string this short allocates only when no live copy exists, and
/// whether one does depends on when the collector last ran. Such strings are
/// not charged, so a script's count does not depend on the collector.
inline constexpr std::size_t short_string_limit = 40;

/// Units for one value `string.format` converts or `warn` passes on.
///
/// One conversion takes about as long as one or two instructions of the
/// interpreter.
inline constexpr uint64_t argument_unit = 2;

/// Units for one copy `setmetatable` makes of a metatable, before one unit
/// per field it copies.
///
/// Making and installing the copy takes about as long as a few dozen
/// instructions. A metatable emptied after holding many fields makes the
/// walk visit every slot it used to hold; the real-time limit, not this
/// charge, stops a loop of those.
inline constexpr uint64_t metatable_copy_charge = 64;

/// Instructions between count-hook calls.
///
/// The allowance is counted on every
/// `budget_check_interval / clock_check_interval`-th call, so the count
/// still steps by `budget_check_interval` at the same instructions. The
/// processor clock is read on that same call. The steady clock is read on
/// every call: a wait has to be noticed within these 100 instructions, and
/// that read is cheap. Reading the processor clock on every call costs
/// enough that the default bank of 60 million instructions can miss its
/// count and stop for time. One step of 1000 instructions of a tight loop
/// is a fraction of a millisecond. A single slow step is still stopped at
/// the next processor-clock read.
inline constexpr int clock_check_interval = 100;

static_assert(clock_check_interval > 1);
static_assert(budget_check_interval % clock_check_interval == 0);

/// Count hook calls that make one step of `budget_check_interval`.
inline constexpr int clock_checks_per_budget_check = budget_check_interval / clock_check_interval;

/// One Lua value, laid out as the interpreter's own value, plus its tag.
///
/// The header that names that value is private to the interpreter, so the
/// sandbox keeps this copy. `table_range_limit` is
/// `default_memory_budget / sizeof(TableSlot)`.
union ValueSlot {
    void* pointer;
    lua_CFunction function;
    lua_Integer integer;
    lua_Number number;
    unsigned char byte;
};

struct TableSlot {
    ValueSlot value;
    unsigned char tag{};
};

/// Most elements one table call may move or visit.
inline constexpr std::size_t table_range_limit = default_memory_budget / sizeof(TableSlot);

/// Which protected entry the trampoline runs.
enum class PhaseMode : uint8_t { open_libraries, run_chunk, call, host, compile };

struct Sandbox::Control {
    std::size_t memory_cap{default_memory_budget};
    std::size_t memory_in_use{};
    bool allocation_refused{};

    /// The state this block belongs to. Set before the libraries open.
    lua_State* main_state{};
    /// Set while the open measures the string header.
    bool measuring_string_header{};
    /// `nsize - length - 1` of one long string, or 0 until that measurement.
    std::size_t string_header{};
    /// A string past `string_length_limit` was refused. Sticky for the phase.
    bool string_refused{};
    uint64_t string_refused_length{};
    /// Set while a chunk is being loaded, so a literal is blamed on that chunk.
    bool loading_chunk{};
    char string_message[budget_message_bytes]{};

    uint64_t instruction_allowance{default_instruction_budget};
    uint64_t instructions_used{};
    bool allowance_spent{};
    /// Count hook calls since the allowance was last counted. Reset at each phase start.
    int clock_checks{};
    /// Processor time this phase may run. Copied from `OpenOptions::time_limit`.
    std::chrono::milliseconds time_limit{default_time_limit};
    /// Steady time when this phase started. The grace compares it.
    std::chrono::steady_clock::time_point phase_started{};
    /// Processor time, in nanoseconds, when this phase started. 0 if that read failed.
    uint64_t phase_started_cpu_ns{};
    /// Latest processor-time sample, in nanoseconds. The grace uses it to see
    /// whether the allowance will run out first. 0 until a read succeeds.
    uint64_t cpu_sampled_ns{};
    /// Set once the phase has run past its time limit. Sticky until the next phase.
    bool time_expired{};
    bool sandbox_failed{};

    /// Instructions one refill adds. Zero means the profile has no bank.
    uint64_t bank_per_tick{};
    /// Most the balance may hold. The bank opens at this value.
    uint64_t bank_limit{};
    /// Instructions still available to a banked call.
    uint64_t bank_balance{};
    /// Processor time one banked call may run.
    std::chrono::milliseconds bank_call_time_limit{default_call_time_limit};
    /// Set for the duration of a banked call. The time sentence reads it.
    bool banked_call{};
    /// Profile allowance put back when a banked call ends.
    uint64_t profile_instruction_allowance{default_instruction_budget};
    /// Profile processor-time limit put back when a banked call ends.
    std::chrono::milliseconds profile_time_limit{default_time_limit};

    ErrorKind pending_kind{ErrorKind::none};
    ErrorKind raised_kind{ErrorKind::none};
    bool location_locked{};
    int32_t error_line{};
    char instruction_message[budget_message_bytes]{};
    /// Sentence a banked call raises when it spends its balance.
    char banked_instruction_message[budget_message_bytes]{};
    /// Text a spent allowance raises. A banked call points this at the banked
    /// sentence and puts it back afterwards. Every other phase leaves it on
    /// `instruction_message`.
    const char* spent_allowance_message = instruction_message;
    char time_message[budget_message_bytes]{};
    char memory_message[budget_message_bytes]{};
    char error_message[message_buffer_bytes]{};
    char error_chunk[chunk_buffer_bytes]{};
    char traceback_text[max_traceback_bytes]{};
    char scratch[scratch_buffer_bytes]{};

    lua_CFunction require_function{};

    int loading_count{};
    char loading_paths[max_require_depth][path_buffer_bytes]{};
    char module_path[path_buffer_bytes]{};
    char chunk_at_path[path_buffer_bytes]{};

    std::vector<uint8_t> read_buffer;
    std::vector<std::string> printed;
    std::size_t printed_bytes{};
    std::size_t lines_left_out{};
    bool print_closed{};
    bool omission_stored{};

    SourceHooks sources{};
    std::string package_id;
    /// Copied from `OpenOptions::random_seed` before the libraries open.
    uint64_t random_seed{};
    std::string phase;
    std::string chunk_name_storage;

    PhaseMode mode{PhaseMode::open_libraries};
    const uint8_t* chunk_data{};
    std::size_t chunk_size{};
    /// Name passed to the loader for the chunk now being loaded. Not owned.
    const char* active_chunk_name{};
    CallHooks call_hooks{};
    bool has_read{};
    void* host_context{};
    void (*host_work)(void* context, lua_State* state){};
    void* install_context{};
    void (*install)(void* context, lua_State* state){};
};

/// The private control block, named for this module's sources.
struct HostDetail {
    using Control = Sandbox::Control;
};

/// Appends text, keeping a terminating NUL inside `end`.
///
/// @param cursor first byte to write
/// @param end one past the last byte of the buffer
/// @param text bytes to copy; null copies nothing
/// @return the new cursor, on the terminating NUL
inline char* append_text(char* cursor, char* end, const char* text) {
    if (cursor == nullptr || end == nullptr || cursor >= end)
        return cursor;
    if (text == nullptr)
        return cursor;
    while (*text != '\0' && cursor + 1 < end)
        *cursor++ = *text++;
    *cursor = '\0';
    return cursor;
}

/// Appends `length` bytes, keeping a terminating NUL inside `end`.
///
/// @param cursor first byte to write
/// @param end one past the last byte of the buffer
/// @param text bytes to copy
/// @param length how many bytes to copy
/// @return the new cursor, on the terminating NUL
inline char* append_bytes(char* cursor, char* end, const char* text, std::size_t length) {
    if (cursor == nullptr || end == nullptr || cursor >= end)
        return cursor;
    if (text == nullptr)
        length = 0;
    for (std::size_t index = 0; index < length && cursor + 1 < end; ++index)
        *cursor++ = text[index];
    *cursor = '\0';
    return cursor;
}

/// Appends a decimal number, keeping a terminating NUL inside `end`.
///
/// @param cursor first byte to write
/// @param end one past the last byte of the buffer
/// @param value the number to write
/// @return the new cursor, on the terminating NUL
inline char* append_u64(char* cursor, char* end, uint64_t value) {
    char digits[20];
    int count = 0;
    do {
        digits[count++] = static_cast<char>('0' + static_cast<int>(value % 10));
        value /= 10;
    } while (value != 0 && count < static_cast<int>(sizeof digits));
    while (count > 0 && cursor != nullptr && end != nullptr && cursor + 1 < end)
        *cursor++ = digits[--count];
    if (cursor != nullptr && end != nullptr && cursor < end)
        *cursor = '\0';
    return cursor;
}

/// Copies bytes into a fixed buffer and terminates it.
///
/// @param destination buffer to fill
/// @param capacity bytes available at `destination`, including the NUL
/// @param text bytes to copy; null copies nothing
/// @param length how many bytes `text` holds
inline void
copy_bounded(char* destination, std::size_t capacity, const char* text, std::size_t length) {
    if (destination == nullptr || capacity == 0)
        return;
    if (text == nullptr)
        text = "";
    if (length >= capacity)
        length = capacity - 1;
    for (std::size_t index = 0; index < length; ++index)
        destination[index] = text[index];
    destination[length] = '\0';
}

/// Returns the control block the state's allocator holds.
///
/// @param state a state this host opened
/// @return that state's control block
inline HostDetail::Control* control_from(lua_State* state) {
    void* user = nullptr;
    lua_getallocf(state, &user);
    return static_cast<HostDetail::Control*>(user);
}

/// Opens the shared libraries and the replacements around them.
///
/// @param state the state being opened
void open_libraries(lua_State* state);

/// Replaces `string.find`, `match`, `gmatch` and `gsub` in the table on top
/// of the stack with the counted copies of Lua 5.4.9's matcher.
///
/// They take the same arguments and give the same results and messages as
/// Lua's own; each matcher step counts against the phase allowance, and past
/// it they raise the budget message and mark the allowance spent. `gmatch`'s
/// iterator keeps its place in a userdata value.
///
/// @param state the sandbox state, with the `string` table on top
void install_patterns(lua_State* state);

/// Replaces `string.format` in the table on top of the stack with the
/// counted copy of Lua 5.4.9's.
///
/// It takes the same arguments and gives the same results and messages as
/// Lua's own, except that it refuses `%p`, and `%a`, `%e`, `%f` and `%g` of
/// a NaN or an infinity, at the calling line. Past the allowance it raises
/// the budget message and marks the allowance spent.
///
/// @param state the sandbox state, with the `string` table on top
void install_format(lua_State* state);

/// Opens the math library. May raise.
///
/// @param state the state being opened
void open_math_library(lua_State* state);

/// Opens the fixed-point library. May raise.
///
/// @param state the state being opened
void open_fixed_library(lua_State* state);

/// Installs `pairs` for number, string and boolean keys, in a fixed order.
/// May raise.
///
/// @param state the state being opened
void install_pairs(lua_State* state);

/// Adds the package `require` and an empty module cache.
///
/// @param state the state being opened
void install_require(lua_State* state);

/// Records a loader error already on the stack and keeps that location.
///
/// @param control the state that tried to load
/// @param state the state, with the loader's message at the top
void remember_load_error(HostDetail::Control* control, lua_State* state);

/// Arms the count hook so the next instruction raises a spent allowance.
///
/// A `pcall` can catch the raise. With the hook set to one instruction, the
/// next check fires outside that `pcall` and the phase ends. The hook itself
/// stays in the state file. A phase past its real-time limit uses the same
/// arming.
///
/// @param state the sandbox state
void rearm_count_hook(lua_State* state);

/// How far real time may run past the limit while this thread is not scheduled.
///
/// The limit itself is this thread's processor time, so other programs do not
/// spend it. A steady clock can stop the phase once real time is past the
/// limit by this much, so a wait between two reads cannot run on. It does not
/// stop a phase whose counted rate will spend the instruction allowance
/// first: a busy machine would otherwise report the wait instead of the
/// count. The time-limit tests allow one second of real time past the limit.
inline constexpr std::chrono::milliseconds schedule_grace{250};

/// Nanoseconds of processor time this thread has used.
///
/// Returns 0 when the clock cannot be read. The count hook reads it on a
/// budget step, and `charge()` reads it on every charge. The calls between
/// budget steps do not.
///
/// @return nanoseconds, or 0 when the read fails
uint64_t thread_cpu_ns();

/// Reports whether real time is past the limit by `schedule_grace`.
///
/// Reads a steady clock. The count hook calls this on every call, and
/// `phase_past_time_limit` calls it on a budget step and from `charge()`.
///
/// @param control the phase being checked
/// @return true when the wait has reached the grace
inline bool wall_past_grace(const HostDetail::Control* control) {
    return std::chrono::steady_clock::now() - control->phase_started >=
           control->time_limit + schedule_grace;
}

/// Reports whether the allowance runs out before the processor-time limit.
///
/// Uses the instructions counted so far and the latest processor-time sample.
/// No sample, or an allowance so large the product would not fit, means the
/// clock stays in charge. A sample that failed at the start is ignored.
///
/// @param control the phase being checked
/// @return true when the count, not the clock, should end the phase
inline bool allowance_finishes_first(const HostDetail::Control* control) {
    if (control->instructions_used == 0 || control->phase_started_cpu_ns == 0)
        return false;
    if (control->cpu_sampled_ns <= control->phase_started_cpu_ns)
        return false;
    if (control->instructions_used >= control->instruction_allowance)
        return true;
    const uint64_t cpu_used = control->cpu_sampled_ns - control->phase_started_cpu_ns;
    const uint64_t remaining = control->instruction_allowance - control->instructions_used;
    if (cpu_used != 0 && remaining > UINT64_MAX / cpu_used)
        return false;
    const auto limit_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(control->time_limit).count()
    );
    if (limit_ns != 0 && control->instructions_used > UINT64_MAX / limit_ns)
        return true;
    return remaining * cpu_used <= limit_ns * control->instructions_used;
}

/// Reports whether `control`'s phase has run past its time limit.
///
/// True when this thread's processor time has reached the limit. Also true
/// when real time has reached the limit plus `schedule_grace`, unless
/// `allowance_finishes_first` says the count will run out before that
/// processor time. A processor-clock read that failed at the start is
/// ignored, so a later successful read is not the whole time since the
/// thread began. Stores the sample it reads. The count hook and `charge()`
/// are the only callers.
///
/// @param control the phase being checked
/// @return true when the phase should stop for time
inline bool phase_past_time_limit(HostDetail::Control* control) {
    const uint64_t now_ns = thread_cpu_ns();
    if (now_ns != 0)
        control->cpu_sampled_ns = now_ns;
    if (control->phase_started_cpu_ns != 0 && now_ns >= control->phase_started_cpu_ns) {
        const auto used = std::chrono::nanoseconds(now_ns - control->phase_started_cpu_ns);
        if (used >= control->time_limit)
            return true;
    }
    if (!wall_past_grace(control))
        return false;
    return !allowance_finishes_first(control);
}

/// Marks the phase past its real-time limit and raises that error.
///
/// Arms the count hook at one instruction first, so a `pcall` cannot keep
/// the phase alive. No object with a destructor may be alive across this call.
///
/// @param state the sandbox state
/// @return does not return
inline void raise_time_limit(lua_State* state) {
    HostDetail::Control* control = control_from(state);
    control->time_expired = true;
    rearm_count_hook(state);
    luaL_error(state, "%s", control->time_message);
}

/// Adds `units` to the phase count and marks the allowance spent when it is reached.
///
/// Does not raise. The allocator uses this, because it cannot call into the
/// state. The count hook raises on its next check.
///
/// @param control the state being charged
/// @param units instruction units to add; 0 adds nothing
inline void add_charge(HostDetail::Control* control, uint64_t units) {
    if (control == nullptr || units == 0)
        return;
    const uint64_t room = UINT64_MAX - control->instructions_used;
    if (units > room)
        control->instructions_used = UINT64_MAX;
    else
        control->instructions_used += units;
    if (control->instructions_used >= control->instruction_allowance)
        control->allowance_spent = true;
}

/// Units for ordering the values at `left` and `right`.
///
/// Two strings cost one unit per byte of the two lengths, and at least
/// `comparison_unit`. Ordering a string full of zero bytes walks a segment
/// per byte, so one unit per `bytes_per_charge` would let that sort run for
/// too long. Any other pair costs `comparison_unit`. Reading the lengths does
/// not allocate.
///
/// @param state the sandbox state
/// @param left stack index of the first value
/// @param right stack index of the second value
/// @return the units to charge before the comparison
inline uint64_t comparison_charge(lua_State* state, int left, int right) {
    if (lua_type(state, left) == LUA_TSTRING && lua_type(state, right) == LUA_TSTRING) {
        std::size_t left_length = 0;
        std::size_t right_length = 0;
        lua_tolstring(state, left, &left_length);
        lua_tolstring(state, right, &right_length);
        const uint64_t bytes =
            static_cast<uint64_t>(left_length) + static_cast<uint64_t>(right_length);
        if (bytes > comparison_unit)
            return bytes;
    }
    return comparison_unit;
}

/// Adds `units` and raises, as the count hook does, when the allowance is spent.
///
/// A string refusal raises the string message instead. Past the phase's
/// real-time limit it raises that message. Each raise arms the count hook at
/// one instruction first, so a `pcall` cannot keep the phase alive. No object
/// with a destructor may be alive across this call.
///
/// @param state the sandbox state
/// @param units instruction units to add
inline void charge(lua_State* state, uint64_t units) {
    HostDetail::Control* control = control_from(state);
    if (control->string_refused) {
        rearm_count_hook(state);
        luaL_error(state, "%s", control->string_message);
    }
    if (control->time_expired) {
        raise_time_limit(state);
    }
    if (control->allowance_spent) {
        rearm_count_hook(state);
        luaL_error(state, "%s", control->spent_allowance_message);
    }
    add_charge(control, units);
    if (control->allowance_spent) {
        rearm_count_hook(state);
        luaL_error(state, "%s", control->spent_allowance_message);
    }
    if (phase_past_time_limit(control)) {
        raise_time_limit(state);
    }
}

/// Records that `length` bytes would pass the string cap, and where it happened.
///
/// The first refusal sticks. `message` null uses the standard sentence naming
/// `length` and 65536. Later calls do nothing.
///
/// @param control the state that refused the string
/// @param length how many bytes the string would have held
/// @param message the sentence to keep, or null for the standard one
void record_string_limit(HostDetail::Control* control, uint64_t length, const char* message);

/// Records the refusal and raises at the calling line.
///
/// @param state the sandbox state
/// @param length how many bytes the string would have held
/// @param message the sentence to keep; null uses the standard one
/// @return does not return
void raise_string_limit(lua_State* state, uint64_t length, const char* message);

/// Raises when `have + extra` would pass the string cap.
///
/// @param state the sandbox state
/// @param have bytes already in the buffer
/// @param extra bytes about to be added
/// @param message the early sentence naming the function
inline void
reject_long_piece(lua_State* state, std::size_t have, std::size_t extra, const char* message) {
    if (extra > string_length_limit || have > string_length_limit - extra) {
        const uint64_t length = static_cast<uint64_t>(have) + static_cast<uint64_t>(extra);
        raise_string_limit(state, length, message);
    }
}

} // namespace oa::data::script_host
