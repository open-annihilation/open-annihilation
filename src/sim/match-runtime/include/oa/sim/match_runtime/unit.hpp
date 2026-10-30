// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/match_runtime/script.hpp"
#include "oa/sim/model_runtime/model_host.hpp"
#include "oa/sim/unit_spawn/spawn_runtime.hpp"

#include <cstdint>

namespace oa::sim::match_runtime {
// Effect and attachment operations a unit script reaches through its host.
struct Effects {
    virtual ~Effects() = default;
    /// Emits a COB EMIT-SFX effect from a piece.
    ///
    /// @param slot Unit running the script.
    /// @param piece COB piece index.
    /// @param effect SFX type the script passes.
    virtual void emit_sfx(sim::unit_spawn::Slot& slot, uint32_t piece, int32_t effect) = 0;
    /// Explodes a piece off the unit (COB EXPLODE).
    ///
    /// @param slot Unit running the script.
    /// @param piece COB piece index.
    /// @param flags Explode type flags the script passes.
    virtual void explode_piece(sim::unit_spawn::Slot& slot, uint32_t piece, int32_t flags) = 0;
    /// Attaches another unit to this one (COB ATTACH-UNIT).
    ///
    /// @param slot Carrier running the script.
    /// @param first Unit id to carry.
    /// @param second COB piece index to carry it on.
    /// @param third Attachment mode.
    virtual void
    attach_unit(sim::unit_spawn::Slot& slot, int32_t first, int32_t second, int32_t third) = 0;
    /// Drops a carried unit (COB DROP-UNIT).
    ///
    /// @param slot Carrier running the script.
    /// @param target Unit id to set down.
    virtual void drop_unit(sim::unit_spawn::Slot& slot, int32_t target) = 0;
};

// The unit script's host: model piece operations on the unit's own model
// instance, unit values on its world record, effects through Effects.
class UnitHost final : public sim::model_runtime::ModelHost {
  public:

    /// Binds a script host to one unit's model instance and world slot.
    ///
    /// @param instance Model instance the piece operations act on.
    /// @param slot Unit the script belongs to; its index lies inside the
    ///     world's unit table, which UnitInstance checks.
    /// @param pool Unit slot pool; its size equals the world's unit slot
    ///     count, which UnitInstance checks.
    /// @param state Canonical world holding the unit.
    /// @param values Unit-value services.
    /// @param effects Effect and attachment services.
    /// @param random Shared stream the script's RANDOM draws from.
    UnitHost(
        sim::model_runtime::Instance& instance,
        sim::unit_spawn::Slot& slot,
        std::span<sim::unit_spawn::Slot> pool,
        oa::World& state,
        UnitValueHost& values,
        Effects& effects,
        SharedRandom& random
    );
    /// Forwards EMIT-SFX to Effects::emit_sfx for this unit.
    ///
    /// @param piece COB piece index.
    /// @param effect SFX type.
    void emit_sfx(uint32_t piece, int32_t effect) override;
    /// Forwards EXPLODE to Effects::explode_piece for this unit.
    ///
    /// @param piece COB piece index.
    /// @param flags Explode type flags.
    void explode(uint32_t piece, int32_t flags) override;
    /// Reads a unit value for this unit (see sim::match_runtime::get_unit_value).
    ///
    /// @param selector Unit-value selector.
    /// @param second First extra operand.
    /// @param third Second extra operand.
    /// @param fourth Unused operand.
    /// @param fifth Unused operand.
    /// @return The selector's value, 0 for an unknown selector.
    int32_t get_unit_value(
        int32_t selector, int32_t second, int32_t third, int32_t fourth, int32_t fifth
    ) override;
    /// Writes a unit value for this unit (see sim::match_runtime::set_unit_value).
    ///
    /// @param selector Unit-value selector.
    /// @param value New value.
    void set_unit_value(int32_t selector, int32_t value) override;
    /// Forwards ATTACH-UNIT to Effects::attach_unit for this unit.
    ///
    /// @param first Unit id to carry.
    /// @param second COB piece index to carry it on.
    /// @param third Attachment mode.
    void attach_unit(int32_t first, int32_t second, int32_t third) override;
    /// Forwards DROP-UNIT to Effects::drop_unit for this unit.
    ///
    /// @param target Unit id to set down.
    void drop_unit(int32_t target) override;
    /// Does nothing: unit scripts ignore this piece operation.
    ///
    /// @param piece COB piece index.
    /// @param first First popped value (meaning unresolved).
    /// @param second Second popped value (meaning unresolved).
    /// @quirk The game's unit script host returns at once for this opcode;
    ///     it is kept as an empty body, not a placeholder.
    void ignored_piece_op(uint32_t piece, uint32_t first, uint32_t second) override;
    /// Does nothing: unit scripts ignore DONT-SHADOW.
    ///
    /// @param piece COB piece index.
    /// @quirk The game's unit script host returns at once for this opcode.
    void dont_shadow(uint32_t piece) override;
    /// Tells whether a unit with this id rides on this unit.
    ///
    /// @param value Unit id to look for.
    /// @return 1 when it is carried by this unit, else 0.
    uint32_t is_carrying_unit(uint32_t value) override;
    /// Returns the id of the unit carrying this one.
    ///
    /// @return The carrier's unit id, or 0 when nothing carries it.
    uint32_t carrier_unit_id() override;
    /// Draws from the match's shared stream for the script's RANDOM.
    ///
    /// @param bound Exclusive upper limit (see SharedRandom::bounded).
    /// @return A value below `bound`, or 0 for a bound below 2.
    uint32_t random_bounded(uint32_t bound) override;

  private:

    sim::unit_spawn::Slot& slot_;
    std::span<sim::unit_spawn::Slot> pool_;
    oa::World& state_;
    UnitValueHost& values_;
    Effects& effects_;
    SharedRandom& random_;
};

// Nonmovable because the VM and Host retain references into this owned runtime.
// Construction creates real model state and a VM; running Create is explicit
// so world registration can establish required callback links first.
class UnitInstance {
  public:

    /// Builds the unit's model instance and, when its type has a COB
    /// program, the script interpreter, and registers the interpreter in the
    /// world's script table. Create does not run yet.
    ///
    /// A slot bound to no unit, one whose index lies outside the world's unit
    /// table, or a pool whose size is not the world's unit slot count gets no
    /// interpreter, and the refusal is noted; so is a model the model runtime
    /// refuses, whose instance then has no pieces.
    ///
    /// @param loaded Loaded type: model and optional COB program.
    /// @param slot Unit slot the instance belongs to.
    /// @param pool Unit slot pool.
    /// @param state Canonical world holding the unit.
    /// @param values Unit-value services.
    /// @param effects Effect and attachment services.
    /// @param random Shared stream the script's RANDOM draws from.
    /// @param clock_scale SLEEP multiplier supplied by the owning
    ///     application (see ScriptInstance).
    /// @param type_script Prepared program for this type, kept by the match.
    ///     Null when the type has no COB program. A type that has a program
    ///     but no prepared one is noted and starts no script.
    /// @param fault Where refusals and interpreter errors are noted, or null
    ///     to drop them; must outlive the instance.
    UnitInstance(
        const sim::unit_spawn::LoadedType& loaded,
        sim::unit_spawn::Slot& slot,
        std::span<sim::unit_spawn::Slot> pool,
        oa::World& state,
        UnitValueHost& values,
        Effects& effects,
        SharedRandom& random,
        int32_t clock_scale,
        const SharedScriptProgram* type_script,
        MatchFault* fault = nullptr
    );
    /// Removes the interpreter from the world's script table.
    ~UnitInstance();
    UnitInstance(const UnitInstance&) = delete;
    UnitInstance& operator=(const UnitInstance&) = delete;

    /// Returns the unit's model instance.
    sim::model_runtime::Instance& model() noexcept { return model_; }

    /// Returns the unit's script, or null when its type has no COB program.
    ScriptInstance* script() noexcept { return script_.get(); }

    /// Starts the Create script and runs one scheduler step with elapsed zero.
    ///
    /// @return False when the unit has no script, no Create script or no
    ///     free context.
    bool create();
    /// The piece query_weapon_piece gives when it cannot ask the script;
    /// piece_world places it at the unit position.
    static constexpr uint32_t no_piece = 0xffff'ffffU;

    /// Returns the slot's firing piece from QueryPrimary/Secondary/Tertiary.
    ///
    /// @param slot Weapon slot 0..2.
    /// @return COB piece index; 0 when the script leaves the argument;
    ///     no_piece, which is noted, for another slot or a unit without a script.
    uint32_t query_weapon_piece(uint8_t slot);
    /// Returns the slot's firing piece in world space.
    ///
    /// This is the form without a known piece; a caller holding one goes
    /// straight to piece_world.
    ///
    /// @param slot Weapon slot 0..2; another is at the unit position.
    /// @return Signed 16.16 x, y, z bit patterns.
    std::array<uint32_t, 3> query_weapon_world(uint8_t slot);
    /// Returns the AimFromPrimary/Secondary/Tertiary piece in world space, or
    /// the firing piece when the script leaves the -1 it is given.
    ///
    /// @param slot Weapon slot 0..2; another, or a unit without a script, is
    ///     at the unit position, which is noted.
    /// @return Signed 16.16 x, y, z bit patterns.
    std::array<uint32_t, 3> aim_from_world(uint8_t slot);
    /// Returns the QueryNanoPiece piece in world space.
    ///
    /// @return Signed 16.16 x, y, z bit patterns; piece 0 when the script
    ///     leaves the argument, the unit position when the unit has no script.
    std::array<uint32_t, 3> query_nano_world();
    /// Returns where other units aim: the box centre of the SweetSpot piece.
    ///
    /// @return Signed 16.16 x, y, z bit patterns; piece 0 when the script
    ///     leaves the argument or the unit has no script.
    std::array<uint32_t, 3> sweet_spot_world();
    /// Starts SetMaxReloadTime asynchronously; nothing happens without a
    /// script.
    ///
    /// @param milliseconds Longest weapon reload time.
    void set_max_reload_time(int32_t milliseconds);
    /// Steps the unit's script contexts; nothing happens without a script.
    ///
    /// @param elapsed Clock time since the last step (see ScriptInstance::tick).
    void tick(uint32_t elapsed);
    /// Returns the piece's attachment offset added to the unit position.
    ///
    /// @param piece COB piece index; a negative one or one outside the model
    ///     adds nothing.
    /// @return Signed 16.16 x, y, z bit patterns; the origin for a slot bound
    ///     to no unit.
    std::array<uint32_t, 3> piece_world(uint32_t piece) const;
    /// Returns the unit position plus the centre of the box around the
    /// piece's vertices, transformed from the unit's pieces and its bank,
    /// heading and pitch as they are now; what a draw last left in the model
    /// instance plays no part.
    ///
    /// @param piece COB piece index; one outside the model gives the unit
    ///     position.
    /// @return Signed 16.16 x, y, z bit patterns; the origin for a slot bound
    ///     to no unit.
    /// @quirk The box starts at the piece origin, so it always holds (0,0,0).
    std::array<uint32_t, 3> piece_box_center(uint32_t piece) const;
    /// Returns the attitude of a unit carried on the piece: the piece's
    /// rotation words plus this unit's bank, heading and pitch.
    ///
    /// @param piece COB piece index.
    /// @return Bank, heading and pitch in 65536ths of a turn.
    sim::model_runtime::RotationWords piece_attitude(uint32_t piece) const;

  private:

    /// Notes a refusal in the fault record, when there is one.
    ///
    /// @param what What was refused.
    /// @param detail Why, when known.
    void note(std::string_view what, std::string_view detail = {}) noexcept;

    sim::unit_spawn::Slot& slot_;
    oa::World& world_;
    sim::model_runtime::Instance model_;
    UnitHost host_;
    std::unique_ptr<ScriptInstance> script_;
    MatchFault* fault_{};
};
} // namespace oa::sim::match_runtime
