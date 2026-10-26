// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "oa/sim/script_state.hpp"

namespace oa::sim::script_vm {

namespace opcode {
inline constexpr uint32_t sleep = 0x10013000U;
inline constexpr uint32_t move = 0x10001000U;
inline constexpr uint32_t turn = 0x10002000U;
inline constexpr uint32_t spin = 0x10003000U;
inline constexpr uint32_t stop_spin = 0x10004000U;
inline constexpr uint32_t show = 0x10005000U;
inline constexpr uint32_t hide = 0x10006000U;
inline constexpr uint32_t cache = 0x10007000U;
inline constexpr uint32_t dont_cache = 0x10008000U;
inline constexpr uint32_t move_now = 0x1000b000U;
inline constexpr uint32_t turn_now = 0x1000c000U;
inline constexpr uint32_t shade = 0x1000d000U;
inline constexpr uint32_t dont_shade = 0x1000e000U;
inline constexpr uint32_t emit_sfx = 0x1000f000U;
inline constexpr uint32_t wait_turn = 0x10011000U;
inline constexpr uint32_t wait_move = 0x10012000U;
inline constexpr uint32_t push_constant = 0x10021001U;
inline constexpr uint32_t push_local = 0x10021002U;
inline constexpr uint32_t push_static = 0x10021004U;
inline constexpr uint32_t stack_alloc = 0x10022000U;
inline constexpr uint32_t pop_local = 0x10023002U;
inline constexpr uint32_t pop_static = 0x10023004U;
inline constexpr uint32_t pop_stack = 0x10024000U;
inline constexpr uint32_t add = 0x10031000U;
inline constexpr uint32_t subtract = 0x10032000U;
inline constexpr uint32_t multiply = 0x10033000U;
inline constexpr uint32_t divide = 0x10034000U;
inline constexpr uint32_t bit_and = 0x10035000U;
inline constexpr uint32_t bit_or = 0x10036000U;
inline constexpr uint32_t bit_xor = 0x10037000U;
inline constexpr uint32_t bit_not = 0x10038000U;
inline constexpr uint32_t random = 0x10041000U;
inline constexpr uint32_t less = 0x10051000U;
inline constexpr uint32_t less_equal = 0x10052000U;
inline constexpr uint32_t greater = 0x10053000U;
inline constexpr uint32_t greater_equal = 0x10054000U;
inline constexpr uint32_t equal = 0x10055000U;
inline constexpr uint32_t not_equal = 0x10056000U;
inline constexpr uint32_t logical_and = 0x10057000U;
inline constexpr uint32_t logical_or = 0x10058000U;
inline constexpr uint32_t logical_xor = 0x10059000U;
inline constexpr uint32_t logical_not = 0x1005a000U;
inline constexpr uint32_t discard_call = 0x10063000U;
inline constexpr uint32_t start_script = 0x10061000U;
inline constexpr uint32_t call_script = 0x10062000U;
inline constexpr uint32_t jump = 0x10064000U;
inline constexpr uint32_t return_ = 0x10065000U;
inline constexpr uint32_t jump_if_false = 0x10066000U;
inline constexpr uint32_t signal = 0x10067000U;
inline constexpr uint32_t set_signal_mask = 0x10068000U;
inline constexpr uint32_t explode = 0x10071000U;
inline constexpr uint32_t get_unit_value = 0x10042000U;
inline constexpr uint32_t get = 0x10043000U;
inline constexpr uint32_t set_unit_value = 0x10082000U;
inline constexpr uint32_t attach_unit = 0x10083000U;
inline constexpr uint32_t drop_unit = 0x10084000U;
// A piece instruction the game ignores: an inline piece and two popped values.
inline constexpr uint32_t ignored_piece_op = 0x10009000U;
inline constexpr uint32_t dont_shadow = 0x1000a000U;
inline constexpr uint32_t is_carrying_unit = 0x10044000U;
inline constexpr uint32_t carrier_unit_id = 0x10045000U;
} // namespace opcode

inline constexpr std::size_t context_count = 8;
// Child word of a CALL_SCRIPT that found no free context; no RETURN or SIGNAL
// wakes a caller waiting on it.
inline constexpr std::size_t no_child_context = 0xffffffffU;
// Stack and local slots the game's context record holds, and the slots a
// saved state keeps of each context.
inline constexpr std::size_t stack_capacity = 32;
inline constexpr std::size_t piece_axis_count = 3;

namespace limit {
inline constexpr std::size_t code_words = 1U << 20;
inline constexpr std::size_t scripts = 1U << 16;
inline constexpr std::size_t statics = 1U << 16;
inline constexpr std::size_t pieces = 4096;
// Slots a context's stack may hold. 3.1c's PUSH never checks its 32-slot
// record: a deeper expression writes over the contexts after it and runs on.
// Devolution's beacon tests a unit's type with a chain of 75 comparisons
// whose partial results all wait on the stack, 78 slots deep. A VM gives
// each context the slots its program's deepest script needs (Vm::stack_slots),
// the record's 32 at least and this many at most; a save keeps the first 32.
inline constexpr std::size_t stack_slots = 4096;
// Instructions one unit's contexts may run in one tick before the tick stops
// with instruction_limit. 3.1c has no such bound: a script may walk every unit
// slot without sleeping, and one that loops forever never returns. The bound
// lets a script visit all 65536 slots with a few hundred instructions each
// and still stops one that never sleeps.
inline constexpr std::size_t instructions_per_tick = std::size_t{1} << 24;
} // namespace limit

namespace unit {
inline constexpr int32_t milliseconds_per_second = 1000;
inline constexpr uint32_t angle_units_per_revolution = 0x10000U;
inline constexpr uint32_t angle_mask = angle_units_per_revolution - 1U;
inline constexpr uint32_t continuous_spin_target = 0xffffffffU;
} // namespace unit

struct Program {
    std::vector<uint32_t> code;
    std::vector<uint32_t> entry_points;
    std::size_t static_count = 0;
    std::size_t piece_count = 0;
    /// Slots each context is given. Zero means prepare_program has not run.
    /// A prepared program with no code uses stack_capacity.
    std::size_t stack_slots = 0;
    /// True once prepare_program has validated this program and recorded
    /// stack_slots. A prepared program is not changed again.
    bool prepared = false;
    /// Why prepare_program refused this program. Null when it was accepted.
    /// A refused program is cleared, so a VM that shares it starts no script.
    /// The text is a static message, the same ones the VM's other refusals use.
    const char* refusal = nullptr;
};

/// Validates a program, records the stack its deepest script needs, and
/// returns a program any number of VMs can share.
///
/// An empty program, one past the limit namespace, or one whose entry point
/// lies outside its code is cleared and comes back refused: its refusal says
/// why, and it starts no script. A program that was already prepared is
/// shared as it is. The code of a prepared program is never written.
///
/// @param program code, entry points and static and piece counts
/// @return a shared program VMs keep alive for as long as they run it
[[nodiscard]] std::shared_ptr<const Program> prepare_program(Program program);

/// Finds a script by exact, case-sensitive name in a COB name table.
///
/// @param script_names the COB function-name table; its length is the script count
/// @param name name looked for
/// @return index of the first match, or -1; a null query, or a null table entry
///         met before a match, also gives -1
[[nodiscard]] int32_t
find_script(std::span<const char* const> script_names, const char* name) noexcept;

class Host {
  public:

    virtual ~Host() = default;

    /// Returns a piece's current offset along one axis.
    ///
    /// @param piece piece index
    /// @param axis 0 = x, 1 = y, 2 = z
    /// @return the offset in model-position units
    virtual int32_t piece_position(uint32_t piece, uint32_t axis) const = 0;
    /// Returns a piece's current angle about one axis.
    ///
    /// @param piece piece index
    /// @param axis 0 = x, 1 = y, 2 = z
    /// @return binary angle, 65536 per turn, in the low 16 bits
    virtual int32_t piece_angle(uint32_t piece, uint32_t axis) const = 0;
    /// Returns a piece's raw visibility flag word.
    ///
    /// @param piece piece index
    /// @return nonzero when shown
    virtual uint32_t piece_visible(uint32_t piece) const = 0;
    /// Returns a piece's raw cache flag word.
    ///
    /// @param piece piece index
    /// @return nonzero when cached
    virtual uint32_t piece_cached(uint32_t piece) const = 0;
    /// Returns a piece's raw shade flag word.
    ///
    /// @param piece piece index
    /// @return nonzero when shaded
    virtual uint32_t piece_shaded(uint32_t piece) const = 0;
    /// Sets a piece's offset along one axis.
    ///
    /// @param piece piece index
    /// @param axis 0 = x, 1 = y, 2 = z
    /// @param value offset in model-position units
    virtual void set_piece_position(uint32_t piece, uint32_t axis, int32_t value) = 0;
    /// Sets a piece's angle about one axis.
    ///
    /// @param piece piece index
    /// @param axis 0 = x, 1 = y, 2 = z
    /// @param value binary angle, 65536 per turn
    virtual void set_piece_angle(uint32_t piece, uint32_t axis, uint32_t value) = 0;
    /// Sets a piece's visibility flag word (SHOW, HIDE, save restore).
    ///
    /// @param piece piece index
    /// @param visible raw flag word
    virtual void set_piece_visible(uint32_t piece, uint32_t visible) = 0;
    /// Sets a piece's cache flag word (CACHE, DONT_CACHE, save restore).
    ///
    /// @param piece piece index
    /// @param cached raw flag word
    virtual void set_piece_cached(uint32_t piece, uint32_t cached) = 0;
    /// Sets a piece's shade flag word (SHADE, DONT_SHADE, save restore).
    ///
    /// @param piece piece index
    /// @param shaded raw flag word
    virtual void set_piece_shaded(uint32_t piece, uint32_t shaded) = 0;
    /// Emits a special effect at a piece (EMIT_SFX).
    ///
    /// @param piece piece index
    /// @param effect effect type popped from the stack
    virtual void emit_sfx(uint32_t piece, int32_t effect) = 0;
    /// Explodes a piece (EXPLODE).
    ///
    /// @param piece piece index
    /// @param type explosion type flags popped from the stack
    virtual void explode(uint32_t piece, int32_t type) = 0;

    /// Reads a unit value (GET_UNIT_VALUE, GET).
    ///
    /// GET_UNIT_VALUE pops only the selector and passes zero for the rest;
    /// GET pops all five values, the selector deepest.
    ///
    /// @param first selector
    /// @param second first selector argument
    /// @param third second selector argument
    /// @param fourth third selector argument
    /// @param fifth fourth selector argument
    /// @return the value pushed onto the stack
    virtual int32_t
    get_unit_value(int32_t first, int32_t second, int32_t third, int32_t fourth, int32_t fifth) = 0;
    /// Writes a unit value (SET_UNIT_VALUE).
    ///
    /// @param selector unit-value selector
    /// @param value new value
    virtual void set_unit_value(int32_t selector, int32_t value) = 0;
    /// Attaches a unit to a piece of this one (ATTACH_UNIT).
    ///
    /// @param first first popped value (meaning set by the host)
    /// @param second second popped value (meaning set by the host)
    /// @param third third popped value (meaning set by the host)
    virtual void attach_unit(int32_t first, int32_t second, int32_t third) = 0;
    /// Drops an attached unit (DROP_UNIT).
    ///
    /// @param unit id of the unit to drop
    virtual void drop_unit(int32_t unit) = 0;

    /// Handles opcode 0x10009000, a piece instruction the game ignores.
    ///
    /// The unit-script host implements it as a no-op.
    ///
    /// @param piece inline piece operand
    /// @param first first popped value
    /// @param second second popped value
    virtual void ignored_piece_op(uint32_t piece, uint32_t first, uint32_t second) = 0;
    /// Handles opcode 0x1000A000, tentatively DONT_SHADOW.
    ///
    /// The unit-script host implements it as a no-op.
    ///
    /// @param piece inline piece operand
    virtual void dont_shadow(uint32_t piece) = 0;
    /// Tests whether a unit rides on this unit (opcode 0x10044000).
    ///
    /// @param unit_id Unit.id looked for
    /// @return 1 when found, else 0
    virtual uint32_t is_carrying_unit(uint32_t unit_id) = 0;
    /// Returns the id of the unit carrying this one (opcode 0x10045000).
    ///
    /// @return the carrier's Unit.id, or 0
    virtual uint32_t carrier_unit_id() = 0;

    /// Draws a bounded random number from the shared engine RNG (RANDOM).
    ///
    /// The VM does not create an independent random stream.
    ///
    /// @param bound span of the range; RANDOM adds its lower limit
    /// @return a value below bound
    virtual uint32_t random_bounded(uint32_t bound) = 0;
};

using ReturnCallback = std::function<void(int32_t)>;

enum class ContextState : uint32_t {
    stopped = 0x00000000U,
    active = 0x01000000U,
    waiting_for_turn = 0x02100000U,
    waiting_for_move = 0x02200000U,
    sleeping = 0x02400000U,
    waiting_for_child = 0x02800000U,
};

enum class ErrorCode {
    none,
    invalid_program,
    invalid_script,
    no_free_context,
    program_counter_out_of_range,
    truncated_instruction,
    unsupported_opcode,
    stack_underflow,
    stack_overflow,
    invalid_local,
    invalid_static,
    invalid_jump,
    missing_host,
    invalid_piece,
    invalid_axis,
    invalid_time_scale,
    divide_by_zero,
    signed_divide_overflow,
    instruction_limit,
    invalid_argument_count,
    invalid_state,
};

struct Error {
    ErrorCode code = ErrorCode::none;
    std::size_t context = context_count;
    uint32_t pc = 0;
    uint32_t opcode = 0;
    std::string message;
};

struct StartResult {
    std::size_t context = context_count;
    std::optional<Error> error;

    /// Returns whether no error was reported.
    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

// The synchronous query returns 1 when a context was entered and 0 when allocation
// failed. `accepted` preserves that distinction; `error` reports an interpreter
// fault after that call returns.
struct QueryResult {
    bool accepted = false;
    std::optional<Error> error;

    /// Returns whether a context was entered and ran without a fault.
    [[nodiscard]] bool ok() const noexcept { return accepted && !error.has_value(); }
};

struct TickResult {
    std::size_t instructions = 0;
    std::optional<Error> error;

    /// Returns whether no error was reported.
    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

struct StateResult {
    std::optional<sim::script_state::State> state;
    std::optional<Error> error;

    /// Returns whether a state was exported.
    [[nodiscard]] bool ok() const noexcept { return state.has_value(); }
};

struct ContextSnapshot {
    ContextState state = ContextState::stopped;
    uint32_t pc = 0;
    int32_t stack_pointer = -1;
    int32_t sleep_remaining = 0;
    uint32_t signal_mask = 1;
    std::size_t child = context_count;
    uint32_t wait_piece = 0;
    uint32_t wait_axis = 0;
    std::array<int32_t, stack_capacity> slots{};
};

class Vm {
  public:

    /// Creates a VM for a program with no host.
    ///
    /// Starts with every context stopped and none active, then allocates the
    /// statics and zeroed per-piece motion state. A program that is empty,
    /// exceeds the limit namespace or has an entry point outside its code is
    /// not taken: the VM holds an empty program, which starts no script, and
    /// program_error() says why. Host-facing instructions fail with missing_host.
    /// The VM keeps the program alive and does not write its code. Passing a
    /// program by value gives this VM its own copy; pass the shared program
    /// from prepare_program when several VMs should run one copy.
    ///
    /// @param program code, entry points and static and piece counts
    /// @param time_scale SLEEP multiplier: delay = time_scale * ms / 1000
    explicit Vm(Program program, int32_t time_scale = unit::milliseconds_per_second);
    /// Creates a VM that runs a shared program, with no host.
    ///
    /// Same as the by-value constructor. A null program, or one that
    /// prepare_program has not accepted yet, is prepared for this VM alone.
    ///
    /// @param program shared code, entry points and counts; kept alive by the VM
    /// @param time_scale SLEEP multiplier: delay = time_scale * ms / 1000
    explicit Vm(
        std::shared_ptr<const Program> program, int32_t time_scale = unit::milliseconds_per_second
    );
    /// Creates a VM for a program with a host for piece and unit calls.
    ///
    /// @param program code, entry points and static and piece counts
    /// @param host host the VM calls; must outlive the VM
    /// @param time_scale SLEEP multiplier: delay = time_scale * ms / 1000
    Vm(Program program, Host& host, int32_t time_scale = unit::milliseconds_per_second);
    /// Creates a VM that runs a shared program and calls a host.
    ///
    /// @param program shared code, entry points and counts; kept alive by the VM
    /// @param host host the VM calls; must outlive the VM
    /// @param time_scale SLEEP multiplier: delay = time_scale * ms / 1000
    Vm(std::shared_ptr<const Program> program,
       Host& host,
       int32_t time_scale = unit::milliseconds_per_second);

    /// Starts a script in the lowest free context with its arguments on the stack.
    ///
    /// Nothing runs until the next tick.
    ///
    /// @param script_index index in the entry-point table
    /// @param arguments initial stack slots; the stack pointer becomes their count - 1
    /// @param return_callback receives the value popped by RETURN, if set
    /// @return the claimed context, or invalid_script, stack_overflow or no_free_context
    [[nodiscard]] StartResult start(
        uint32_t script_index,
        std::span<const int32_t> arguments = {},
        ReturnCallback return_callback = {}
    );
    /// Starts a script the way the engine's argument-form call does.
    ///
    /// Writes all four locals, then sets the stack pointer to argument_count - 1.
    ///
    /// @param script_index index in the entry-point table
    /// @param locals the four local slots
    /// @param argument_count arguments the script sees, 0..4
    /// @param return_callback receives the value popped by RETURN, if set
    /// @return the claimed context, or the reason none was started
    [[nodiscard]] StartResult start_parameterized(
        uint32_t script_index,
        const std::array<int32_t, 4>& locals,
        std::size_t argument_count,
        ReturnCallback return_callback = {}
    );
    /// Runs a script synchronously and reads its first four stack slots back.
    ///
    /// The stack pointer is forced to 3 and only the new context runs, with
    /// elapsed zero. A RETURN operand is not the result.
    ///
    /// @param script_index index in the entry-point table
    /// @param[in,out] arguments values pushed into slots 0..3 and replaced by
    ///        them afterwards; null pointers push zero and are not written back
    /// @return whether a context was entered, and any interpreter fault
    /// @quirk Allocation failure leaves the values unchanged.
    [[nodiscard]] QueryResult
    query(uint32_t script_index, const std::array<int32_t*, 4>& arguments);
    /// Steps every context in ascending order, then integrates piece motion.
    ///
    /// A context that meets an instruction outside the machine's set stops
    /// there, with that instruction's state unchanged, and the tick goes on.
    ///
    /// @param elapsed scheduler ticks since the previous tick
    /// @param instruction_budget instructions allowed across all contexts; 0 is an error
    /// @return instructions executed, and the first fault: an unknown
    ///         instruction's, or any other, which stops the tick
    [[nodiscard]] TickResult
    tick(uint32_t elapsed, std::size_t instruction_budget = limit::instructions_per_tick);

    /// Wakes and interprets one context without advancing others or motion.
    ///
    /// A context waiting on a finished turn or move, or whose sleep has run
    /// out, becomes active first.
    ///
    /// @param index context index, 0..7
    /// @param elapsed scheduler ticks charged to a sleeping context
    /// @param instruction_budget instructions allowed
    /// @return instructions executed, and any fault
    [[nodiscard]] TickResult tick_context(
        std::size_t index,
        uint32_t elapsed,
        std::size_t instruction_budget = limit::instructions_per_tick
    );

    /// Captures contexts, statics, motion, transforms and piece flags for a save.
    ///
    /// Piece flags are queried before each piece's transforms.
    ///
    /// @param script_identity_token hash of the loaded COB file stored in the payload
    /// @return the state, or missing_host when pieces exist and no host is set
    [[nodiscard]] StateResult export_state(uint32_t script_identity_token) const;
    /// Restores a saved state.
    ///
    /// Validates context and program bounds before changing anything, then
    /// clears return callbacks, restores each piece's flags before its
    /// transforms, and marks every piece and the motion scheduler active.
    /// Decoding and identity checks are script-state's.
    ///
    /// @param state decoded payload
    /// @return an error when the counts, context words or host do not fit the loaded program
    [[nodiscard]] std::optional<Error> import_state(const sim::script_state::State& state);

    /// Returns why the program given to the constructor was not taken.
    ///
    /// @return an invalid_program error, or nullopt when the VM holds its program
    [[nodiscard]] const std::optional<Error>& program_error() const noexcept {
        return program_error_;
    }

    /// The program this VM runs.
    ///
    /// VMs built from one shared program run the same code. Each VM keeps its
    /// own statics, stacks and contexts. The reference stays valid while this
    /// VM is alive, because the VM keeps the program alive.
    ///
    /// @return the program given to the constructor, after it was prepared
    [[nodiscard]] const Program& program() const noexcept { return *program_; }

    /// Returns the number of contexts that are not stopped.
    ///
    /// @return 0..8
    [[nodiscard]] std::size_t active_count() const noexcept;
    /// Copies one context's state.
    ///
    /// @param index context index, 0..7
    /// @return the context's state, registers and stack slots; a stopped, empty
    ///         context for an index past the eight contexts
    [[nodiscard]] ContextSnapshot context(std::size_t index) const;
    /// Reads a static variable.
    ///
    /// @param index static index
    /// @return the value, or nullopt past the static table
    [[nodiscard]] std::optional<int32_t> static_value(std::size_t index) const noexcept;

    /// Returns the stack slots each context has: what the program's deepest
    /// script needs, walked once when the program is set, at least the
    /// record's 32 and at most limit::stack_slots.
    ///
    /// @return slots per context
    [[nodiscard]] std::size_t stack_slots() const noexcept { return stack_slots_; }

    /// Writes a static variable.
    ///
    /// @param index static index
    /// @param value new value
    /// @return false past the static table
    [[nodiscard]] bool set_static(std::size_t index, int32_t value) noexcept;

    /// Returns the turn one axis of a piece is adding each tick.
    ///
    /// The turn is in angle words, 65536 a full turn, and negative the other
    /// way. It is the rate after this tick's change of speed, which is the
    /// turn the axis took in the tick when the scheduler stepped one tick.
    ///
    /// @param piece piece index
    /// @param axis axis 0..2
    /// @return the turn per tick, or 0 when the piece or axis is outside the script
    [[nodiscard]] int32_t turn_speed(uint32_t piece, uint32_t axis) const noexcept;

  private:

    struct StoredContext {
        ContextState state = ContextState::stopped;
        uint32_t pc = 0;
        int32_t sp = -1;
        int32_t sleep_remaining = 0;
        std::size_t child = 0;
        uint32_t wait_piece = 0;
        uint32_t wait_axis = 0;
        uint32_t signal_mask = 1;
        // This context's stack_slots() slots of stack_block_.
        std::span<uint32_t> slots{};
        ReturnCallback return_callback;
    };

    std::shared_ptr<const Program> program_;
    std::optional<Error> program_error_;
    Host* host_ = nullptr;
    std::vector<uint32_t> statics_;
    // Every context's stack, stack_slots_ slots each, allocated once with
    // the program.
    std::unique_ptr<uint32_t[]> stack_block_;
    std::size_t stack_slots_ = stack_capacity;
    std::array<StoredContext, context_count> contexts_{};
    int32_t time_scale_;
    std::size_t active_count_ = 0;

    struct AxisMotion {
        int32_t move_target = 0;
        int32_t move_speed = 0;
        uint32_t turn_target = 0;
        int32_t turn_speed = 0;
        int32_t spin_target_speed = 0;
        int32_t spin_acceleration = 0;
    };

    std::vector<AxisMotion> motions_;
    std::vector<uint8_t> piece_active_;
    bool motions_active_ = false;

    /// Claims the lowest free context for a script.
    ///
    /// The context becomes active at the script's entry point with stack
    /// pointer -1, signal mask 1 and no return callback.
    ///
    /// @param script_index index in the entry-point table
    /// @return the context index, or nullopt for a bad index or a full pool
    [[nodiscard]] std::optional<std::size_t> allocate_context(uint32_t script_index);
    /// Stops a context and wakes every caller waiting on it.
    ///
    /// @param index context index; an already stopped context is left alone
    void stop_context(std::size_t index);
    /// Makes every context waiting on a child active again.
    ///
    /// @param child index of the child context that stopped
    void wake_callers(std::size_t child);
    /// Integrates move, turn and spin targets for active pieces.
    ///
    /// Does nothing without a host, with no motion active or with elapsed 0.
    /// Angles wrap at 16 bits; a piece stays active while any axis has not
    /// reached its target.
    ///
    /// @param elapsed scheduler ticks since the previous update
    void advance_motions(uint32_t elapsed);
    /// Interprets one active context until it stops, sleeps, waits or faults.
    ///
    /// @param index context index
    /// @param[in,out] instructions instructions executed so far this tick
    /// @param instruction_budget limit on instructions
    /// @return the fault, or nullopt
    [[nodiscard]] std::optional<Error>
    execute_context(std::size_t index, std::size_t& instructions, std::size_t instruction_budget);
};

} // namespace oa::sim::script_vm
