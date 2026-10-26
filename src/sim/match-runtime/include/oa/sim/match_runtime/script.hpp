// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/data/limits.hpp"
#include "oa/data/match_rules.hpp"
#include "oa/sim/match_runtime/fault.hpp"
#include "oa/sim/script_vm.hpp"
#include "oa/sim/unit_script.hpp"
#include "oa/formats/cob.hpp"
#include "oa/sim/unit_spawn/spawn.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

namespace oa::sim::match_runtime {
/// A unit-script function the engine calls by its fixed name.
using ScriptFunction = sim::unit_script::ScriptFunction;

class SharedRandom {
  public:

    /// Seeds the shared stream every unit script and simulation draw consumes.
    ///
    /// @param seed Match seed; it is scrambled with 0x66E29572 and forced odd.
    explicit SharedRandom(uint32_t seed) noexcept : state_((seed ^ 0x66e29572u) | 1u) {}

    /// Advances the shared stream and returns a value below `bound`.
    ///
    /// @param bound Exclusive upper limit of the result.
    /// @return A value in [0, bound), or 0 without advancing the stream when
    ///     the bound read as signed is below 2.
    /// @quirk The bound is tested as a signed value, so bounds of 0x80000000
    ///     and above return 0; the step itself uses unsigned multiply and
    ///     division, as 3.1c does.
    uint32_t bounded(uint32_t bound) noexcept;

    /// Returns the current stream state, for saving or comparing.
    uint32_t state() const noexcept { return state_; }

    /// Replaces the stream state with a saved one.
    void restore(uint32_t state) noexcept { state_ = state; }

  private:

    uint32_t state_;
};

/// Code and engine-function indices shared by every unit of one script.
///
/// A match builds one for each loaded unit type and keeps it for the match.
/// The interpreter program views that type's loaded script words and does not
/// copy them. Every unit of that type is given that one, so the name lookup
/// exists once per type.
struct SharedScriptProgram {
    /// Prepared interpreter program. Its code is the loaded script's own
    /// words and is never written.
    std::shared_ptr<const sim::script_vm::Program> vm_program;
    /// COB index of each engine function's exact name, or -1 when the program
    /// has none. A loaded program's names never change.
    std::array<int32_t, sim::unit_script::script_function_count> function_index{};
};

/// Prepares one type's interpreter program and resolves its engine-function names.
///
/// The caller keeps the result for as long as units of that type run, and
/// passes the same one to each of them. A null program prepares an empty
/// interpreter, which starts no script.
///
/// @param program Loaded COB program, or null when the type has no script.
/// @return The prepared program and the engine-function indices.
[[nodiscard]] SharedScriptProgram
prepare_type_script(const std::shared_ptr<const formats::cob::CobProgram>& program);

// COB scheduler ownership and the named-call wrappers. Host is a real
// model/world adapter, never an internal successful no-op implementation.
// An interpreter error, other than a start that finds no free context, is
// noted in the instance's fault record and fails the call it stopped. Each
// call by name has a form taking a ScriptFunction instead, which starts the
// script that function's exact name would, found once for the program.
class ScriptInstance {
  public:

    /// Builds the COB interpreter for a program against a model/world host.
    ///
    /// A null program, or one the interpreter refuses, leaves an instance that
    /// starts no script; the refusal is noted. Units of one type are given
    /// the same prepared program, so they share its code and indices.
    ///
    /// @param program Loaded COB program.
    /// @param shared Prepared program for this type. The instance keeps the
    ///     code alive; the match keeps `shared` for its own life.
    /// @param host Model and world adapter the interpreter calls for piece
    ///     and unit-value operations; must outlive the instance.
    /// @param clock_scale SLEEP multiplier supplied by the owning
    ///     application (delay = scale * ms / 1000); not derived from the
    ///     simulation frequency.
    /// @param fault Where interpreter errors are noted, or null to drop them;
    ///     must outlive the instance.
    ScriptInstance(
        std::shared_ptr<const formats::cob::CobProgram> program,
        const SharedScriptProgram& shared,
        sim::script_vm::Host& host,
        int32_t clock_scale,
        MatchFault* fault = nullptr
    );
    /// Starts the first script with this exact name without writing any
    /// locals.
    ///
    /// The started context keeps whatever its locals held. Create uses this
    /// form.
    ///
    /// @param name Script name; the lookup is case-sensitive.
    /// @param immediate Runs a full scheduler step with elapsed zero right
    ///     after the start.
    /// @param callback Receives the script's RETURN value when it finishes;
    ///     never called when the start fails.
    /// @return False when no script has the name or no context is free.
    bool call_no_arguments(
        std::string_view name, bool immediate, sim::script_vm::ReturnCallback callback = {}
    );
    /// Starts the first script with a fixed engine function's name without
    /// writing any locals, as call_no_arguments does by name.
    ///
    /// @param function Engine function whose exact COB name is started.
    /// @param immediate Runs a full scheduler step with elapsed zero right
    ///     after the start.
    /// @param callback Receives the script's RETURN value when it finishes;
    ///     never called when the start fails.
    /// @return False when no script has the name or no context is free.
    bool call_no_arguments(
        ScriptFunction function, bool immediate, sim::script_vm::ReturnCallback callback = {}
    );
    /// Starts the first script with this exact name with up to four
    /// arguments; omitted trailing locals are zeroed.
    ///
    /// @param name Script name; the lookup is case-sensitive.
    /// @param arguments At most four argument values; more starts nothing
    ///     and is noted.
    /// @param immediate Runs a full scheduler step with elapsed zero right
    ///     after the start.
    /// @param callback Receives the script's RETURN value when it finishes,
    ///     or zero when the name is missing or no context is free.
    /// @return False when no script has the name or no context is free.
    bool call(
        std::string_view name,
        std::span<const int32_t> arguments,
        bool immediate,
        sim::script_vm::ReturnCallback callback = {}
    );
    /// Starts the first script with a fixed engine function's name with up
    /// to four arguments, as call does by name.
    ///
    /// @param function Engine function whose exact COB name is started.
    /// @param arguments At most four argument values; more starts nothing
    ///     and is noted.
    /// @param immediate Runs a full scheduler step with elapsed zero right
    ///     after the start.
    /// @param callback Receives the script's RETURN value when it finishes,
    ///     or zero when the name is missing or no context is free.
    /// @return False when no script has the name or no context is free.
    bool call(
        ScriptFunction function,
        std::span<const int32_t> arguments,
        bool immediate,
        sim::script_vm::ReturnCallback callback = {}
    );
    /// Starts the first script with this exact name with all four locals
    /// written and only the first `count` of them passed as arguments; the
    /// others stay readable as locals.
    ///
    /// @param name Script name; the lookup is case-sensitive.
    /// @param locals Values of the four locals.
    /// @param count Number of locals that are arguments, 0..4; more starts
    ///     nothing and is noted.
    /// @param immediate Runs a full scheduler step with elapsed zero right
    ///     after the start.
    /// @param callback Receives the script's RETURN value when it finishes,
    ///     or zero when the name is missing or no context is free.
    /// @return False when no script has the name or no context is free.
    bool call_with_locals(
        std::string_view name,
        const std::array<int32_t, 4>& locals,
        std::size_t count,
        bool immediate,
        sim::script_vm::ReturnCallback callback = {}
    );
    /// Starts the first script with a fixed engine function's name with all
    /// four locals written, as call_with_locals does by name.
    ///
    /// @param function Engine function whose exact COB name is started.
    /// @param locals Values of the four locals.
    /// @param count Number of locals that are arguments, 0..4; more starts
    ///     nothing and is noted.
    /// @param immediate Runs a full scheduler step with elapsed zero right
    ///     after the start.
    /// @param callback Receives the script's RETURN value when it finishes,
    ///     or zero when the name is missing or no context is free.
    /// @return False when no script has the name or no context is free.
    bool call_with_locals(
        ScriptFunction function,
        const std::array<int32_t, 4>& locals,
        std::size_t count,
        bool immediate,
        sim::script_vm::ReturnCallback callback = {}
    );
    /// Runs a query script synchronously and copies its first four local
    /// slots back into `arguments`.
    ///
    /// Only the new context runs, with elapsed zero; a query that sleeps
    /// stays scheduled. A RETURN operand is not the query result.
    ///
    /// @param name Script name; the lookup is case-sensitive.
    /// @param[in,out] arguments Initial locals on entry, local slots 0..3 on
    ///     return; left unchanged when the query does not start or fails.
    /// @return False when no script has the name, no context is free, or the
    ///     interpreter fails the start or the run.
    bool query(std::string_view name, std::array<int32_t, 4>& arguments);
    /// Runs the first script with a fixed engine function's name as a query,
    /// as query does by name.
    ///
    /// @param function Engine function whose exact COB name is run.
    /// @param[in,out] arguments Initial locals on entry, local slots 0..3 on
    ///     return; left unchanged when the query does not start or fails.
    /// @return False when no script has the name, no context is free, or the
    ///     interpreter fails the start or the run.
    bool query(ScriptFunction function, std::array<int32_t, 4>& arguments);
    /// Finds the first script with this exact name, as a start looks it up.
    ///
    /// @param name Script name; the lookup is case-sensitive.
    /// @return The script's COB index, or -1 when the program has none.
    [[nodiscard]] int32_t find(std::string_view name) const noexcept;
    /// Returns the first script with a fixed engine function's exact name.
    ///
    /// @param function Engine function; an out-of-range value finds Create's
    ///     script, as sim::unit_script::engine_call maps it.
    /// @return The script's COB index, or -1 when the program has none.
    [[nodiscard]] int32_t find(ScriptFunction function) const noexcept;
    /// Steps every context in ascending order, then integrates piece motion.
    ///
    /// @param elapsed Clock time since the last step, in the units the clock
    ///     scale converts SLEEP milliseconds into.
    /// @return False when the interpreter failed the step, which is noted.
    bool tick(uint32_t elapsed);

    /// Returns the turn one axis of a piece is adding each tick.
    ///
    /// The turn is in angle words, 65536 a full turn, and negative the other
    /// way. It is the rate the piece is turning at now.
    ///
    /// @param piece Piece index.
    /// @param axis Axis 0..2.
    /// @return The turn per tick, or 0 when the piece or axis is outside the script.
    [[nodiscard]] int32_t piece_turn_speed(uint32_t piece, uint32_t axis) const noexcept;

    /// Returns the interpreter.
    sim::script_vm::Vm& vm() noexcept { return vm_; }

    /// Returns the loaded COB program.
    const formats::cob::CobProgram& program() const noexcept { return *program_; }

  private:

    /// Notes an interpreter error in the fault record, when there is one.
    ///
    /// @param what What failed.
    /// @param detail The interpreter's message or the script's name.
    void note(std::string_view what, std::string_view detail) noexcept;

    /// Starts a script without writing any locals.
    ///
    /// @param index COB script index, or -1 for a missing script.
    /// @param immediate Runs a full scheduler step with elapsed zero right
    ///     after the start.
    /// @param callback Receives the script's RETURN value; never called on
    ///     failure.
    /// @return False when the index is -1 or no context is free.
    bool start_no_arguments(int32_t index, bool immediate, sim::script_vm::ReturnCallback callback);
    /// Starts a script with up to four arguments; omitted trailing locals
    /// are zeroed.
    ///
    /// @param index COB script index, or -1 for a missing script.
    /// @param name The script's name, for the fault record.
    /// @param arguments At most four argument values; more starts nothing
    ///     and is noted.
    /// @param immediate Runs a full scheduler step with elapsed zero right
    ///     after the start.
    /// @param callback Receives the RETURN value, or zero on failure.
    /// @return False when the start fails.
    bool start_with_arguments(
        int32_t index,
        std::string_view name,
        std::span<const int32_t> arguments,
        bool immediate,
        sim::script_vm::ReturnCallback callback
    );
    /// Starts a script with all four locals written and `count` of them
    /// passed as arguments.
    ///
    /// @param index COB script index, or -1 for a missing script.
    /// @param name The script's name, for the fault record.
    /// @param locals Values of the four locals.
    /// @param count Number of locals that are arguments, 0..4; more starts
    ///     nothing and is noted.
    /// @param immediate Runs a full scheduler step with elapsed zero right
    ///     after the start.
    /// @param callback Receives the RETURN value, or zero on failure.
    /// @return False when the start fails.
    bool start_with_locals(
        int32_t index,
        std::string_view name,
        const std::array<int32_t, 4>& locals,
        std::size_t count,
        bool immediate,
        sim::script_vm::ReturnCallback callback
    );
    /// Runs a script synchronously and copies its first four locals back.
    ///
    /// @param index COB script index, or -1 for a missing script.
    /// @param[in,out] arguments Initial locals, then local slots 0..3;
    ///     unchanged when the query does not start or fails.
    /// @return False when the query does not start or fails.
    bool run_query(int32_t index, std::array<int32_t, 4>& arguments);

    std::shared_ptr<const formats::cob::CobProgram> program_;
    sim::script_vm::Vm vm_;
    MatchFault* fault_{};
    /// The COB index of each engine function's exact name, or -1 when the
    /// program has none. Copied from the program's shared indices.
    std::array<int32_t, sim::unit_script::script_function_count> function_index_{};
};

// Services the unit-value GET/SET handlers need outside the unit record.
struct UnitValueHost {
    virtual ~UnitValueHost() = default;
    /// The rules the match plays by, which reach the GET and SET handlers
    /// (UnitValueServices::rules); unset, 3.1c's.
    data::match_rules::MatchRulesView value_rules{};
    /// The capacities the match was built with (UnitValueServices::limits); null for 3.1c's.
    const data::limits::Limits* value_limits{};
    /// Returns the world position of a script piece of the unit.
    ///
    /// @param slot Unit whose piece is asked for.
    /// @param piece COB piece index.
    /// @return Signed 16.16 x, y, z bit patterns.
    virtual std::array<uint32_t, 3>
    piece_world_position(sim::unit_spawn::Slot& slot, uint32_t piece) = 0;
    /// Returns the 16-bit heading of a horizontal vector.
    ///
    /// @param x Signed x component as a bit pattern.
    /// @param z Signed z component as a bit pattern.
    /// @return Heading in 65536ths of a turn.
    virtual uint16_t direction_to(uint32_t x, uint32_t z) = 0;
    /// Returns the horizontal length of a vector, truncated to an integer.
    ///
    /// @param x Signed x component as a bit pattern.
    /// @param z Signed z component as a bit pattern.
    /// @return Length of (x, z).
    virtual uint32_t distance(uint32_t x, uint32_t z) = 0;
    /// Returns the integer terrain height at a map position.
    ///
    /// @param x Signed 16.16 map x as a bit pattern.
    /// @param z Signed 16.16 map z as a bit pattern.
    /// @return Terrain height in whole units.
    virtual int32_t sample_terrain_height(uint32_t x, uint32_t z) = 0;
    /// Sets or clears a state flag with its side effects.
    ///
    /// @param slot Unit whose flag changes.
    /// @param mask 1 for activation, 2 for armored.
    /// @param enabled True sets the flag, false clears it.
    virtual void set_activation(sim::unit_spawn::Slot& slot, uint8_t mask, bool enabled) = 0;
    /// Opens or closes a factory yard.
    ///
    /// @param slot Factory unit.
    /// @param value Nonzero opens the yard, zero closes it.
    virtual void set_yard_open(sim::unit_spawn::Slot& slot, int32_t value) = 0;
};

/// Reads a COB unit value (GET) for a unit.
///
/// @param world Canonical world the value is read from.
/// @param slot Unit running the script; one bound to no unit reads 0.
/// @param pool Unit slot pool (unused by the lookup).
/// @param selector Unit-value selector, or an index an extension is mounted
///     at (MatchRules::script_get, through UnitValueHost::value_rules); an
///     unknown one reads 0.
/// @param second First extra operand of the selector.
/// @param third Second extra operand of the selector.
/// @param host Piece, direction, distance and terrain services.
/// @return The selector's value, 0 for an unknown selector.
int32_t get_unit_value(
    oa::World& world,
    sim::unit_spawn::Slot& slot,
    std::span<sim::unit_spawn::Slot> pool,
    int32_t selector,
    int32_t second,
    int32_t third,
    UnitValueHost& host
);
/// Writes a COB unit value (SET) for a unit, with the selector's flag
/// changes; every SET raises the unit's script-state-changed event.
///
/// @param slot Unit running the script; one bound to no unit takes no value.
/// @param selector Unit-value selector.
/// @param value New value.
/// @param host Activation and yard services the selectors reach.
void set_unit_value(
    sim::unit_spawn::Slot& slot, int32_t selector, int32_t value, UnitValueHost& host
);
} // namespace oa::sim::match_runtime
