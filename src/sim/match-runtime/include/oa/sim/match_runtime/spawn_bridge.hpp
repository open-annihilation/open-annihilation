// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/match_runtime/unit.hpp"

#include <cstdint>

namespace oa::sim::match_runtime {
// The match-side state of one unit slot beside its world record.
struct SlotRuntime {
    std::unique_ptr<UnitInstance> instance;
    // ? The model's cached image reference, which spawning clears once the
    // model is set up. The draw state keeps the image itself, so nothing
    // reads this word.
    uint32_t cached_image_word{};
    // Instances made for the slot so far. It tells one spawn's instance from
    // the next, which the address cannot: a unit made after a death can get
    // the freed instance's memory. Draw state is kept per generation; the
    // simulation never reads it.
    uint32_t instance_generation{};
    // Whether Match::event_hooks reported the slot's unit finished since its
    // creation was reported, so no path reports it twice; the simulation
    // never reads it.
    bool finished_reported{};
};

// Engine operations unit creation calls out to. These are required
// subsystems; implementations must use the spatial, combat, terrain,
// activation and visibility components. No default success.
struct SpawnSubsystems {
    virtual ~SpawnSubsystems() = default;
    /// Stands a weapon slot down and clears its target.
    ///
    /// @param slot Unit owning the weapon.
    /// @param index Weapon slot 0..2.
    virtual void stop_weapon(sim::unit_spawn::Slot& slot, uint32_t index) = 0;
    /// Moves the unit into a squad of its owner.
    ///
    /// @param slot Unit to move.
    /// @param group Squad number; 0xffffffff (-1) joins none.
    virtual void assign_squad(sim::unit_spawn::Slot& slot, uint32_t group) = 0;
    /// Sets up the unit's weapon slots from its type's weapons.
    ///
    /// @param slot New unit.
    /// @param runtime The unit's match-side state; its instance must exist.
    virtual void initialize_weapons(sim::unit_spawn::Slot& slot, SlotRuntime& runtime) = 0;
    /// Sets the unit's metal extraction rate from the plots under it.
    ///
    /// @param slot New unit.
    /// @param runtime The unit's match-side state, for its SetSpeed script.
    virtual void initialize_extraction_rate(sim::unit_spawn::Slot& slot, SlotRuntime& runtime) = 0;
    /// Builds the unit's movement object.
    ///
    /// @param slot New mobile unit.
    /// @return Handle of the movement object.
    virtual sim::unit_spawn::AssetHandle create_movement(sim::unit_spawn::Slot& slot) = 0;
    /// Settles a new unit's height on the terrain, sea or waterline.
    ///
    /// @param slot New unit.
    virtual void fit_spawn_height(sim::unit_spawn::Slot& slot) = 0;
    /// Registers the unit's footprint and bucket on the map.
    ///
    /// @param slot New unit.
    virtual void register_occupancy(sim::unit_spawn::Slot& slot) = 0;
    /// Tells the other players a unit was created, in a multiplayer game.
    ///
    /// @param slot New unit.
    virtual void notify_created(sim::unit_spawn::Slot& slot) = 0;
    /// Tells the other players a building was created finished, in a
    /// multiplayer game.
    ///
    /// @param slot New unit.
    virtual void notify_finished(sim::unit_spawn::Slot& slot) = 0;
    /// Sets or clears a state flag with its scripts and sounds.
    ///
    /// @param slot Unit whose flag changes.
    /// @param mask State flag bits.
    /// @param enabled True sets the bits, false clears them.
    virtual void set_activation(sim::unit_spawn::Slot& slot, uint8_t mask, bool enabled) = 0;
    /// Stamps the unit's sight on its owner's grids.
    ///
    /// @param slot New unit.
    virtual void update_sight(sim::unit_spawn::Slot& slot) = 0;
    /// Reports the unit's creation to the mission's conditions.
    ///
    /// @param slot New unit.
    virtual void notify_scenario_created(sim::unit_spawn::Slot& slot) = 0;
};

// The unit-spawn host of a match: owns each slot's model instance and
// COB interpreter and forwards the remaining engine operations to
// SpawnSubsystems. A callback that meets a broken invariant notes it in the
// match's fault record and does nothing more.
class SpawnBridge final : public sim::unit_spawn::Host {
  public:

    /// Binds the bridge to a world, its spawn tables and the loaded assets.
    ///
    /// @param state Canonical world; its type count equals the tables'.
    /// @param tables Unit-spawn type and asset tables.
    /// @param views Legacy views over `state`.
    /// @param loaded Loaded model/COB assets per type, as many as the tables'
    ///     types; when the counts differ, the refusal is noted and no asset
    ///     is found. One interpreter program is prepared for each loaded
    ///     script and kept for the match.
    /// @param values Unit-value services for the unit scripts.
    /// @param effects Effect and attachment services for the unit scripts.
    /// @param subsystems Engine operations unit creation calls out to.
    /// @param random Shared stream.
    /// @param clock_scale SLEEP multiplier for the unit scripts.
    /// @param fault The match's fault record; must outlive the bridge.
    SpawnBridge(
        oa::World& state,
        sim::unit_spawn::Tables& tables,
        sim::unit_spawn::LegacyViews& views,
        std::span<const sim::unit_spawn::LoadedType> loaded,
        UnitValueHost& values,
        Effects& effects,
        SpawnSubsystems& subsystems,
        SharedRandom& random,
        int32_t clock_scale,
        MatchFault& fault
    );
    /// Creates a unit through the unit-spawn sequence.
    ///
    /// @param request Type, player, slot and state of the new unit.
    /// @return The new unit's slot, or null for an ordinary refusal
    ///     (disabled type, exhausted limit, no free slot) or a spawn fault,
    ///     which is noted.
    sim::unit_spawn::Slot* create(const sim::unit_spawn::Request& request);
    /// Records the player's side, grants its start storage, creates the
    /// side's commander at a start marker and centres the local view on it.
    ///
    /// @param player Player index 0..9.
    /// @param setup The player's side and starting resources.
    /// @param markers The map's start markers.
    /// @param start_index Start position to use.
    /// @param local_player The viewpoint player, whose view moves there.
    /// @param viewport_width View width in pixels.
    /// @param viewport_height View height in pixels.
    /// @param start Commander type, missing-start and camera services.
    /// @return Whether the start position was found, and the commander (null
    ///     when its creation was refused or met a fault, which is noted).
    sim::unit_spawn::StartResult start_player(
        uint8_t player,
        const sim::unit_spawn::PlayerSetup& setup,
        std::span<const sim::unit_spawn::StartMarker> markers,
        int32_t start_index,
        uint8_t local_player,
        int32_t viewport_width,
        int32_t viewport_height,
        sim::unit_spawn::StartHost& start
    );
    /// Returns a slot's match-side state.
    ///
    /// @param slot A slot of this match's pool; another is noted and gets a
    ///     spare state outside the pool, cleared at each such call.
    /// @return The slot's state.
    SlotRuntime& runtime(sim::unit_spawn::Slot& slot);
    /// Returns a unit's match-side state.
    ///
    /// @param unit A unit of this match's world.
    /// @return The unit's state.
    SlotRuntime& runtime(const oa::Unit& unit);
    /// Steps one unit's script contexts.
    ///
    /// @param slot Unit; one without an instance is noted and not stepped.
    /// @param elapsed Clock time since the last step.
    void tick_script(sim::unit_spawn::Slot& slot, uint32_t elapsed);
    /// Draws from the shared stream for unit creation.
    ///
    /// @param bound Exclusive upper limit (see SharedRandom::bounded).
    /// @return A value below `bound`, or 0 for a bound below 2.
    uint32_t random_bounded(uint32_t bound) override;
    /// Points a weapon slot's target at unit 0: target_a 0 with the
    /// unit-target marker in target_b.
    ///
    /// @param unit New unit.
    /// @param index Weapon slot 0..2; another is noted and changes nothing.
    void init_weapon_target(oa::Unit& unit, uint32_t index) override;
    /// Stands an enabled weapon slot down and drops its target, unless it
    /// already stands down.
    ///
    /// @param unit Unit owning the weapon.
    /// @param index Weapon slot 0..2, or 3 for all three with slot 2 last;
    ///     another is noted and changes nothing.
    void reset_weapon_targets(oa::Unit& unit, uint8_t index) override;
    /// Clears a new unit's economy block and ties it to its owner's
    /// resources.
    ///
    /// @param unit New unit.
    /// @param owner Owning player index 0..9; another is noted and changes nothing.
    void init_unit_economy(oa::Unit& unit, uint8_t owner) override;
    /// Moves the unit into a squad (SpawnSubsystems::assign_squad).
    ///
    /// @param unit Unit to move.
    /// @param group Squad number; -1 joins none.
    void assign_squad(oa::Unit& unit, uint32_t group) override;
    /// Builds a model instance without a script for the unit being created.
    ///
    /// The instance replaces the slot's SlotRuntime::instance and advances its
    /// instance_generation.
    ///
    /// @param model Handle of the loaded model.
    /// @return Handle of the new unit instance; 0, which is noted, outside a
    ///     unit's creation or for a model that is not one of the loaded assets.
    sim::unit_spawn::AssetHandle create_model_instance(sim::unit_spawn::AssetHandle model) override;
    /// Checks that the unit's plain model instance belongs to the unit.
    ///
    /// @param unit Unit just given a plain model; a mismatch is noted.
    void model_owner(oa::Unit& unit) override;
    /// Allocates a pending script record for a unit being created.
    ///
    /// @return Handle of the pending script.
    sim::unit_spawn::AssetHandle allocate_script() override;
    /// Stages a loaded COB program on a pending script.
    ///
    /// @param script Pending script handle.
    /// @param cob Handle of the loaded COB program.
    void load_script_state(
        sim::unit_spawn::AssetHandle script, sim::unit_spawn::AssetHandle cob
    ) override;
    /// Builds a model instance with its COB interpreter for a unit.
    ///
    /// The instance replaces the slot's SlotRuntime::instance and advances its
    /// instance_generation.
    ///
    /// @param model Handle of the loaded model.
    /// @param cob Handle of the loaded COB program.
    /// @param unit Unit being created.
    /// @return Handle of the new unit instance; 0, which is noted, for a model
    ///     that is not one of the loaded assets.
    sim::unit_spawn::AssetHandle create_scripted_model(
        sim::unit_spawn::AssetHandle model, sim::unit_spawn::AssetHandle cob, oa::Unit& unit
    ) override;
    /// Records the scripted model instance on its pending script.
    ///
    /// @param script Pending script handle.
    /// @param model Handle of the unit instance; one this match does not own,
    ///     or whose COB program is not the one staged on the script, is noted
    ///     and binds nothing.
    void bind_script_model(
        sim::unit_spawn::AssetHandle script, sim::unit_spawn::AssetHandle model
    ) override;
    /// Runs Create on the instance bound to a pending script.
    ///
    /// @param script Pending script handle; one not bound yet is noted and
    ///     runs nothing.
    void call_script_create(sim::unit_spawn::AssetHandle script) override;
    /// Clears the unit's model state word.
    ///
    /// @param unit New unit.
    void model_reset(oa::Unit& unit) override;
    /// Sets up the unit's weapons (SpawnSubsystems::initialize_weapons).
    ///
    /// @param unit New unit.
    void initialize_weapons(oa::Unit& unit) override;
    /// Sets the unit's extraction rate
    /// (SpawnSubsystems::initialize_extraction_rate).
    ///
    /// @param unit New unit.
    void initialize_extraction_rate(oa::Unit& unit) override;
    /// Builds the unit's movement object (SpawnSubsystems::create_movement).
    ///
    /// @param unit New mobile unit.
    /// @return Handle of the movement object.
    sim::unit_spawn::AssetHandle create_movement(oa::Unit& unit) override;
    /// Settles the unit's height (SpawnSubsystems::fit_spawn_height).
    ///
    /// @param unit New unit.
    void fit_spawn_height(oa::Unit& unit) override;
    /// Registers the unit on the map (SpawnSubsystems::register_occupancy).
    ///
    /// @param unit New unit.
    void register_occupancy(oa::Unit& unit) override;
    /// Shares the unit's creation (SpawnSubsystems::notify_created).
    ///
    /// @param unit New unit.
    void notify_created(oa::Unit& unit) override;
    /// Shares a finished building's creation
    /// (SpawnSubsystems::notify_finished).
    ///
    /// @param unit New unit.
    void notify_finished(oa::Unit& unit) override;
    /// Sets or clears state flags (SpawnSubsystems::set_activation).
    ///
    /// @param unit New unit.
    /// @param mask True for the activation bit.
    /// @param value True sets the bits, false clears them.
    void set_activation(oa::Unit& unit, bool mask, bool value) override;
    /// Stamps the unit's sight (SpawnSubsystems::update_sight).
    ///
    /// @param unit New unit.
    void update_sight(oa::Unit& unit) override;
    /// Reports the creation to the mission
    /// (SpawnSubsystems::notify_scenario_created).
    ///
    /// @param unit New unit.
    void notify_scenario_created(oa::Unit& unit) override;

  private:

    struct PendingScript {
        std::shared_ptr<const formats::cob::CobProgram> program;
        UnitInstance* bound{};
    };

    /// Returns the pool slot of a unit.
    sim::unit_spawn::Slot& slot(const oa::Unit& unit) noexcept { return views_.slot(unit); }

    /// Notes a spawn fault in the match's fault record.
    ///
    /// @param fault The fault, or none.
    void note(sim::unit_spawn::SpawnFault fault) noexcept;

    oa::World& state_;
    sim::unit_spawn::Tables& tables_;
    sim::unit_spawn::LegacyViews& views_;
    std::span<const sim::unit_spawn::LoadedType> loaded_;
    UnitValueHost& values_;
    Effects& effects_;
    SpawnSubsystems& subsystems_;
    SharedRandom& random_;
    int32_t clock_scale_;
    MatchFault& fault_;
    sim::unit_spawn::Slot* active_{};
    std::vector<SlotRuntime> runtime_;
    SlotRuntime spare_runtime_; // what runtime() gives for a slot outside the pool
    std::vector<std::unique_ptr<PendingScript>> scripts_;
    /// Prepared interpreter programs, one per loaded type, in type order.
    /// A type with no script, or one that shares another type's COB program,
    /// holds that shared program (or none). Lives as long as the match.
    std::unique_ptr<SharedScriptProgram[]> type_scripts_;
    std::size_t type_script_count_ = 0;
    /// Prepares one interpreter program for each loaded type.
    void prepare_type_scripts();
    /// Finds the prepared program for a loaded COB program.
    ///
    /// @param cob COB program a loaded type holds, or null.
    /// @return That type's prepared program, or null when this match did not
    ///     load it.
    const SharedScriptProgram* script_for(const formats::cob::CobProgram* cob) const noexcept;
    /// Finds a pending script by its handle.
    ///
    /// @param handle Handle allocate_script returned.
    /// @return The pending script, or null, which is noted, for another handle.
    PendingScript* pending(sim::unit_spawn::AssetHandle handle) noexcept;
    /// Finds the loaded type whose model a handle names.
    ///
    /// @param handle Model handle.
    /// @return The loaded type, or null, which is noted, for one not loaded.
    const sim::unit_spawn::LoadedType*
    asset_model(sim::unit_spawn::AssetHandle handle) const noexcept;
    /// Finds the loaded COB program a handle names.
    ///
    /// @param handle COB handle.
    /// @return The program, or null, which is noted, for one not loaded.
    std::shared_ptr<const formats::cob::CobProgram>
    asset_script(sim::unit_spawn::AssetHandle handle) const;
};
} // namespace oa::sim::match_runtime
