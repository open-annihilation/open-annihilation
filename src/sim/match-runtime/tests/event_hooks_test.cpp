// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The match's event hooks (Match::event_hooks) only read. The synthetic
// skirmish of match-determinism plays out tick for tick the same with every
// hook set as without, to the digests that test pins; in it each kind of
// event is reported with the units and shots it names, and each death once
// while the dying unit still names its owner. A shot launched from another
// machine's report is reported with the reported aim, which its record does
// not keep; burst shots and meteors, creations and completions are reported
// where they happen. Each new unit is reported created by the unit that
// started it, or by none, right after it is created. Listeners hear each
// event after the primary table, in
// the order they were added, and the same skirmish reaches the pins with
// them set.
#include "combat_fixture.hpp"
#include "event_skirmish.hpp"

#include "oa/sim/match_runtime/event_hooks.hpp"
#include "oa/sim/weapon_execution/interceptor.hpp"
#include "oa/sim/weapon_execution/projectile_pool.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace combat_fixture;
using sim::match_runtime::EventHooks;
using sim::match_runtime::EventListenerId;
using sim::match_runtime::KillOutcome;
using sim::match_runtime::ShotSource;
using event_skirmish::pinned_final_state;
using event_skirmish::pinned_final_total;
using event_skirmish::pinned_halfway_state;
using event_skirmish::play;
using event_skirmish::Run;

// The fixture's gun, and the kinds apply_damage_event takes.
constexpr uint32_t gun_registry_index = 1;
constexpr uint8_t weapon_kind = 1;

constexpr int32_t fx(int32_t whole) {
    return static_cast<int32_t>(static_cast<uint32_t>(whole) << 16);
}

struct CreatedReport {
    uint16_t unit{};
    uint16_t type_index{};
    uint8_t owner{};
};

struct FinishedReport {
    uint16_t unit{};
    uint16_t builder{};
    bool finished_at_call{};
};

struct ShotReport {
    ShotSource source{};
    oa_ref32 def{};
    oa_ref32 origin_unit{};
    std::optional<FixedVec3> aim;
    FixedVec3 record_target{};
    uint16_t target_unit{};
};

struct DetonationReport {
    oa_ref32 def{};
    uint16_t direct_unit{};
};

struct DamageReport {
    uint16_t target{};
    uint16_t source{};
    int16_t amount{};
    uint8_t kind{};
    int16_t health_at_call{};
};

struct DeathReport {
    uint16_t unit{};
    uint8_t owner{};
    uint16_t type_index{};
    bool live_at_call{};
    bool settled_elsewhere{};
};

struct CreatedByReport {
    uint16_t unit{};
    uint16_t builder{};
};

struct EndedReport {
    uint16_t unit{};
    uint8_t kind{};
    bool done{};
};

struct CarryReport {
    uint16_t unit{};
    uint16_t carrier{};
    bool loaded{};
};

struct WreckReport {
    uint16_t unit{};
    uint16_t feature{};
    uint32_t plot{};
};

struct GivenReport {
    uint16_t unit{};
    uint16_t copy{};
    uint8_t from{};
    uint8_t to{};
    bool live_at_call{};
};

struct SeenChangeReport {
    uint8_t player{};
    std::vector<sim::detection::SightedUnit> gained;
    std::vector<sim::detection::SightedUnit> lost;
};

// Order of creation, created-by and finished reports. A created-by report
// sits immediately after its unit's created report, and a building created
// finished is reported finished after that.
enum class BirthStep : uint8_t { created, created_by, finished };

// Keeps what every hook reported.
struct Recorder {
    std::vector<CreatedReport> created;
    std::vector<CreatedByReport> created_by;
    std::vector<FinishedReport> finished;
    std::vector<ShotReport> shots;
    std::vector<DetonationReport> detonations;
    std::vector<DamageReport> damage;
    std::vector<DeathReport> deaths;
    std::vector<GivenReport> given;
    std::vector<uint16_t> idle;
    std::vector<EndedReport> ended;
    std::vector<CarryReport> carried;
    std::vector<WreckReport> wrecks;
    std::vector<BirthStep> births;
    std::vector<SeenChangeReport> seen;

    /// Returns hooks that report into this recorder.
    ///
    /// @return every entry set, with this recorder as the context
    EventHooks hooks() {
        EventHooks hooks;
        hooks.context = this;
        hooks.unit_created = [](void* context, const World& world, uint16_t unit) {
            auto* recorder = static_cast<Recorder*>(context);
            const auto& record = world.units[unit];
            recorder->created.push_back({unit, record.type_index, record.owner_index});
            recorder->births.push_back(BirthStep::created);
        };
        hooks.unit_created_by = [](void* context, const World&, uint16_t unit, uint16_t builder) {
            auto* recorder = static_cast<Recorder*>(context);
            recorder->created_by.push_back({unit, builder});
            recorder->births.push_back(BirthStep::created_by);
        };
        hooks.unit_finished =
            [](void* context, const World& world, uint16_t unit, uint16_t builder) {
                auto* recorder = static_cast<Recorder*>(context);
                recorder->finished.push_back(
                    {unit, builder, world.units[unit].build_remaining == 0.0F}
                );
                recorder->births.push_back(BirthStep::finished);
            };
        hooks.unit_given = [](void* context,
                              const World& world,
                              uint16_t unit,
                              uint16_t copy,
                              uint8_t from,
                              uint8_t to) {
            static_cast<Recorder*>(context)->given.push_back(
                {unit, copy, from, to, (world.units[unit].flags & OA_UNIT_FLAG_LIVE) != 0}
            );
        };
        hooks.unit_idle = [](void* context, const World&, uint16_t unit) {
            static_cast<Recorder*>(context)->idle.push_back(unit);
        };
        hooks.order_ended =
            [](void* context, const World&, uint16_t unit, uint8_t kind, bool done) {
                static_cast<Recorder*>(context)->ended.push_back({unit, kind, done});
            };
        hooks.unit_carried =
            [](void* context, const World&, uint16_t unit, uint16_t carrier, bool loaded) {
                static_cast<Recorder*>(context)->carried.push_back({unit, carrier, loaded});
            };
        hooks.wreck_placed =
            [](void* context, const World&, uint16_t unit, uint16_t feature, uint32_t plot) {
                static_cast<Recorder*>(context)->wrecks.push_back({unit, feature, plot});
            };
        hooks.seen_changed = [](void* context,
                                const World&,
                                uint8_t player,
                                std::span<const sim::detection::SightedUnit> gained,
                                std::span<const sim::detection::SightedUnit> lost) {
            static_cast<Recorder*>(context)->seen.push_back(
                {player, {gained.begin(), gained.end()}, {lost.begin(), lost.end()}}
            );
        };
        hooks.shot_placed = [](void* context,
                               const World&,
                               const Projectile& shot,
                               ShotSource source,
                               const FixedVec3* aim,
                               uint16_t target_unit) {
            ShotReport report{
                source, shot.def, shot.source, std::nullopt, shot.target, target_unit
            };
            if (aim != nullptr)
                report.aim = *aim;
            static_cast<Recorder*>(context)->shots.push_back(report);
        };
        hooks.shot_detonated =
            [](void* context, const World&, const Projectile& shot, uint16_t direct_unit) {
                static_cast<Recorder*>(context)->detonations.push_back({shot.def, direct_unit});
            };
        hooks.unit_damaged = [](void* context,
                                const World& world,
                                uint16_t target,
                                uint16_t source,
                                int16_t amount,
                                uint8_t kind) {
            static_cast<Recorder*>(context)->damage.push_back(
                {target, source, amount, kind, world.units[target].health}
            );
        };
        hooks.unit_died = [](void* context,
                             const World& world,
                             uint16_t unit,
                             const KillOutcome&,
                             bool settled_elsewhere) {
            const auto& record = world.units[unit];
            static_cast<Recorder*>(context)->deaths.push_back(
                {unit,
                 record.owner_index,
                 record.type_index,
                 (record.flags & OA_UNIT_FLAG_LIVE) != 0,
                 settled_elsewhere}
            );
        };
        return hooks;
    }

    /// Counts the shots reported from one source.
    ///
    /// @param source how the shots came to be
    /// @return the number reported
    std::size_t shots_from(ShotSource source) const {
        return static_cast<std::size_t>(
            std::count_if(shots.begin(), shots.end(), [source](const ShotReport& shot) {
                return shot.source == source;
            })
        );
    }
};

/// Tells whether two points are the same.
///
/// @param a first point
/// @param b second point
/// @return true when every coordinate is equal
bool same_point(const FixedVec3& a, const FixedVec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

/// Tells whether two optional aims are the same point, or both absent.
///
/// @param a first aim
/// @param b second aim
/// @return true when both are absent or both name the same point
bool same_aim(const std::optional<FixedVec3>& a, const std::optional<FixedVec3>& b) {
    if (a.has_value() != b.has_value())
        return false;
    return !a || same_point(*a, *b);
}

/// Tells whether two creation reports name the same unit.
///
/// @param left first report
/// @param right second report
/// @return true when every field is equal
bool operator==(const CreatedReport& left, const CreatedReport& right) {
    return left.unit == right.unit && left.type_index == right.type_index &&
           left.owner == right.owner;
}

/// Tells whether two created-by reports name the same unit and builder.
///
/// @param left first report
/// @param right second report
/// @return true when every field is equal
bool operator==(const CreatedByReport& left, const CreatedByReport& right) {
    return left.unit == right.unit && left.builder == right.builder;
}

/// Tells whether two hand-over reports name the same gift.
///
/// @param left first report
/// @param right second report
/// @return true when every field is equal
bool operator==(const GivenReport& left, const GivenReport& right) {
    return left.unit == right.unit && left.copy == right.copy && left.from == right.from &&
           left.to == right.to && left.live_at_call == right.live_at_call;
}

/// Tells whether two finished reports name the same completion.
///
/// @param left first report
/// @param right second report
/// @return true when every field is equal
bool operator==(const FinishedReport& left, const FinishedReport& right) {
    return left.unit == right.unit && left.builder == right.builder &&
           left.finished_at_call == right.finished_at_call;
}

/// Tells whether two shot reports name the same placed shot.
///
/// @param left first report
/// @param right second report
/// @return true when every field is equal
bool operator==(const ShotReport& left, const ShotReport& right) {
    return left.source == right.source && left.def == right.def &&
           left.origin_unit == right.origin_unit && same_aim(left.aim, right.aim) &&
           same_point(left.record_target, right.record_target) &&
           left.target_unit == right.target_unit;
}

/// Tells whether two detonation reports name the same blast.
///
/// @param left first report
/// @param right second report
/// @return true when every field is equal
bool operator==(const DetonationReport& left, const DetonationReport& right) {
    return left.def == right.def && left.direct_unit == right.direct_unit;
}

/// Tells whether two damage reports name the same health event.
///
/// @param left first report
/// @param right second report
/// @return true when every field is equal
bool operator==(const DamageReport& left, const DamageReport& right) {
    return left.target == right.target && left.source == right.source &&
           left.amount == right.amount && left.kind == right.kind &&
           left.health_at_call == right.health_at_call;
}

/// Tells whether two death reports name the same death.
///
/// @param left first report
/// @param right second report
/// @return true when every field is equal
bool operator==(const DeathReport& left, const DeathReport& right) {
    return left.unit == right.unit && left.owner == right.owner &&
           left.type_index == right.type_index && left.live_at_call == right.live_at_call &&
           left.settled_elsewhere == right.settled_elsewhere;
}

/// Tells whether two ended-order reports name the same order.
///
/// @param left first report
/// @param right second report
/// @return true when every field is equal
bool operator==(const EndedReport& left, const EndedReport& right) {
    return left.unit == right.unit && left.kind == right.kind && left.done == right.done;
}

/// Tells whether two carry reports name the same link change.
///
/// @param left first report
/// @param right second report
/// @return true when every field is equal
bool operator==(const CarryReport& left, const CarryReport& right) {
    return left.unit == right.unit && left.carrier == right.carrier && left.loaded == right.loaded;
}

/// Tells whether two wreck reports name the same corpse.
///
/// @param left first report
/// @param right second report
/// @return true when every field is equal
bool operator==(const WreckReport& left, const WreckReport& right) {
    return left.unit == right.unit && left.feature == right.feature && left.plot == right.plot;
}

/// Tells whether two sighted lists name the same slots and generations.
///
/// @param left one list
/// @param right the other list
/// @return true when they match entry for entry
bool same_sighted(
    const std::vector<sim::detection::SightedUnit>& left,
    const std::vector<sim::detection::SightedUnit>& right
) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (left[index].unit != right[index].unit ||
            left[index].generation != right[index].generation)
            return false;
    return true;
}

/// Tells whether two recorders kept the same seen-list changes.
///
/// @param left first recorder
/// @param right second recorder
/// @return true when every seen_changed report matches
bool same_seen(const Recorder& left, const Recorder& right) {
    if (left.seen.size() != right.seen.size())
        return false;
    for (std::size_t index = 0; index < left.seen.size(); ++index) {
        if (left.seen[index].player != right.seen[index].player)
            return false;
        if (!same_sighted(left.seen[index].gained, right.seen[index].gained) ||
            !same_sighted(left.seen[index].lost, right.seen[index].lost))
            return false;
    }
    return true;
}

/// Tells whether two recorders kept the same reports, entry for entry.
///
/// @param left first recorder
/// @param right second recorder
/// @return true when every list matches
bool same_records(const Recorder& left, const Recorder& right) {
    return left.created == right.created && left.created_by == right.created_by &&
           left.finished == right.finished && left.shots == right.shots &&
           left.detonations == right.detonations && left.damage == right.damage &&
           left.deaths == right.deaths && left.given == right.given && left.idle == right.idle &&
           left.ended == right.ended && left.carried == right.carried &&
           left.wrecks == right.wrecks && left.births == right.births && same_seen(left, right);
}

/// The skirmish with every hook set plays exactly as without, to
/// match-determinism's pins.
void hooks_change_nothing() {
    Recorder recorder;
    const Run plain = play({});
    const Run hooked = play(recorder.hooks());
    CHECK(plain.totals == hooked.totals);
    CHECK(plain.halfway_state == hooked.halfway_state);
    CHECK(plain.final_state == hooked.final_state);
    CHECK(hooked.halfway_state == pinned_halfway_state);
    CHECK(hooked.final_state == pinned_final_state);
    CHECK(hooked.totals.back() == pinned_final_total);
    std::cout << "hooks change nothing passed\n";
}

/// Tells whether a slot is one of the run's units.
///
/// @param run the run
/// @param unit slot to find
/// @return true when the run spawned it
bool spawned(const Run& run, uint16_t unit) {
    return std::find(run.units.begin(), run.units.end(), unit) != run.units.end();
}

/// Every kind of event of the skirmish is reported with the units it names.
void skirmish_reports_every_kind() {
    Recorder recorder;
    const Run run = play(recorder.hooks());

    // Each spawned unit once, in order, with its type and owner filled; the
    // fixture's type is mobile, so none is finished at creation.
    CHECK(recorder.created.size() == run.units.size());
    CHECK(recorder.created_by.size() == recorder.created.size());
    CHECK(recorder.births.size() == recorder.created.size() * 2);
    CHECK(recorder.given.empty());
    for (std::size_t i = 0; i < run.units.size(); ++i) {
        CHECK(
            recorder.created[i].unit == run.units[i] && recorder.created[i].type_index == 1 &&
            recorder.created[i].owner == run.owners[i]
        );
        // Created by no unit, once, immediately after unit_created.
        CHECK(recorder.births[i * 2] == BirthStep::created);
        CHECK(recorder.births[i * 2 + 1] == BirthStep::created_by);
        CHECK(recorder.created_by[i].unit == run.units[i] && recorder.created_by[i].builder == 0);
    }
    CHECK(recorder.finished.empty());
    // The fixture's type has no default mission, so a unit that runs out of
    // orders is not given one and is never reported idle. An order that does
    // end belongs to a unit the skirmish spawned.
    CHECK(recorder.idle.empty());
    for (const auto& ended : recorder.ended)
        CHECK(spawned(run, ended.unit));

    // The guns' shots, each from a spawned unit, aimed at a point and a unit.
    CHECK(recorder.shots_from(ShotSource::weapon) > 0);
    CHECK(recorder.shots.size() == recorder.shots_from(ShotSource::weapon));
    for (const auto& shot : recorder.shots)
        CHECK(
            shot.def == oa_ref_from_index(gun_registry_index) &&
            spawned(run, static_cast<uint16_t>(oa_unit_slot_from_ref(shot.origin_unit))) &&
            shot.aim && same_point(*shot.aim, shot.record_target) && spawned(run, shot.target_unit)
        );

    // Detonations of the gun, damage to spawned units by spawned units.
    CHECK(!recorder.detonations.empty());
    for (const auto& detonation : recorder.detonations)
        CHECK(detonation.def == oa_ref_from_index(gun_registry_index));
    CHECK(!recorder.damage.empty());
    for (const auto& hit : recorder.damage)
        CHECK(
            hit.kind == weapon_kind && hit.amount > 0 && spawned(run, hit.target) &&
            spawned(run, hit.source) && hit.health_at_call > 0
        );

    // Each death once, while the unit is still live and names its owner.
    CHECK(!recorder.deaths.empty());
    std::vector<uint16_t> dead;
    for (const auto& death : recorder.deaths) {
        CHECK(std::find(dead.begin(), dead.end(), death.unit) == dead.end());
        dead.push_back(death.unit);
        const auto index = static_cast<std::size_t>(
            std::find(run.units.begin(), run.units.end(), death.unit) - run.units.begin()
        );
        CHECK(index < run.units.size());
        CHECK(
            death.owner == run.owners[index] && death.type_index == 1 && death.live_at_call &&
            !death.settled_elsewhere
        );
    }
    CHECK(std::find(dead.begin(), dead.end(), run.gunner) != dead.end());
    CHECK(std::find(dead.begin(), dead.end(), run.target) != dead.end());
    std::cout << "skirmish reports every kind passed (" << recorder.shots.size() << " shots, "
              << recorder.detonations.size() << " detonations, " << recorder.damage.size()
              << " hits, " << recorder.deaths.size() << " deaths)\n";
}

/// Returns the gun's shared description for a shot of the fixture's unit.
///
/// @param f the fixture
/// @param source firing unit
/// @param target unit aimed at
/// @param aim point aimed at
/// @return the shot as another machine reports it
sim::match_runtime::ShotEvent
gun_shot(Fixture& f, uint16_t source, uint16_t target, const FixedVec3& aim) {
    sim::match_runtime::ShotEvent shot{};
    shot.start = f.match->state().units[source].position;
    shot.target = aim;
    shot.weapon_id = f.match->state().game.weapon_defs[gun_registry_index].weapon_id;
    shot.target_unit = target;
    shot.source_unit = source;
    shot.slot = 0;
    return shot;
}

/// A shot launched from another machine's report is reported with the
/// reported aim and target unit, also when its record keeps an earlier shot's
/// point, and exactly once.
void applied_shot_reports_its_aim() {
    Fixture f;
    Recorder recorder;
    f.match->event_hooks = recorder.hooks();
    const auto shooter = f.spawn(1, 48, 48).unit_index;
    const auto aimed = f.spawn(0, 176, 64).unit_index;
    f.match->simulation().players[1].status = OA_PLAYER_STATUS_MIRRORED;
    auto& world = f.match->state();
    CHECK(oa::world_weapon_def(&world, oa_ref_from_index(gun_registry_index)) != nullptr);

    // Ballistic: the constructor leaves the record's target as it found it.
    auto& gun = world.game.weapon_defs[gun_registry_index];
    const auto line_flags = gun.flags;
    gun.flags |= OA_WEAPON_FLAG_BALLISTIC;
    const FixedVec3 earlier{fx(7), fx(8), fx(9)};
    world.projectiles[world.game.projectile_count].target = earlier;
    const FixedVec3 aim{fx(170), fx(32), fx(60)};
    const auto count = world.game.projectile_count;
    f.match->apply_shot(gun_shot(f, shooter, aimed, aim));
    CHECK(world.game.projectile_count == count + 1);
    CHECK(same_point(world.projectiles[count].target, earlier));
    CHECK(recorder.shots.size() == 1);
    const auto& ballistic = recorder.shots.front();
    CHECK(
        ballistic.source == ShotSource::weapon && ballistic.aim && same_point(*ballistic.aim, aim)
    );
    CHECK(ballistic.target_unit == aimed);
    CHECK(ballistic.origin_unit == oa::oa_unit_ref_from_slot(shooter));

    // A line shot keeps its aim in the record too.
    gun.flags = line_flags;
    const FixedVec3 second_aim{fx(160), fx(32), fx(70)};
    f.match->apply_shot(gun_shot(f, shooter, aimed, second_aim));
    CHECK(recorder.shots.size() == 2);
    const auto& line = recorder.shots.back();
    CHECK(
        line.aim && same_point(*line.aim, second_aim) && same_point(line.record_target, second_aim)
    );

    // A report the match cannot launch reports nothing.
    f.match->apply_shot(gun_shot(f, 0, aimed, aim));
    CHECK(recorder.shots.size() == 2);
    std::cout << "applied shot reports its aim passed\n";
}

/// Meteors are reported without an aim, burst shots with the spawner's aim
/// unless a mirrored player's ballistic spawner lost it.
void meteors_and_bursts_are_reported() {
    Fixture f;
    Recorder recorder;
    f.match->event_hooks = recorder.hooks();
    auto& world = f.match->state();
    const auto gun = oa_ref_from_index(gun_registry_index);
    const FixedVec3 sky{fx(100), fx(400), fx(100)};
    const FixedVec3 fall{0, fx(-4), 0};
    CHECK(f.match->launch_meteor(gun, sky, fall, false));
    CHECK(recorder.shots.size() == 1);
    CHECK(
        recorder.shots.front().source == ShotSource::meteor && !recorder.shots.front().aim &&
        recorder.shots.front().target_unit == 0 && recorder.shots.front().def == gun
    );
    sim::weapon_execution::retire_projectile(world, world.projectiles[0]);
    sim::weapon_execution::compact_projectiles(world);

    // A spawner with two burst shots left, aimed at a point, owned by player
    // 1; each tick it copies itself as one burst shot.
    const auto spawner = [&](const FixedVec3& aim) -> Projectile& {
        auto* shot = sim::weapon_execution::allocate_projectile(world);
        CHECK(shot != nullptr);
        const FixedVec3 at{fx(128), fx(60), fx(128)};
        sim::weapon_execution::init_projectile_record(
            world, *shot, gun, at, &aim, world.game.tick, nullptr, 0
        );
        shot->owner_index = 1;
        shot->burst_remaining = 2;
        shot->lifetime_tick = world.game.tick + 200;
        return *shot;
    };
    const FixedVec3 aim{fx(20), fx(0), fx(30)};
    (void)spawner(aim);
    f.run(1);
    CHECK(recorder.shots_from(ShotSource::burst) == 1);
    CHECK(recorder.shots.back().aim && same_point(*recorder.shots.back().aim, aim));

    // The same spawner of a mirrored player's ballistic gun reports no aim.
    recorder.shots.clear();
    for (int32_t i = 0; i < world.game.projectile_count; ++i)
        sim::weapon_execution::retire_projectile(world, world.projectiles[i]);
    sim::weapon_execution::compact_projectiles(world);
    world.game.weapon_defs[gun_registry_index].flags |= OA_WEAPON_FLAG_BALLISTIC;
    f.match->simulation().players[1].status = OA_PLAYER_STATUS_MIRRORED;
    const auto* owner = oa::world_player(&world, 1);
    CHECK(owner != nullptr && owner->in_use != 0 && owner->status == OA_PLAYER_STATUS_MIRRORED);
    (void)spawner(aim);
    f.run(1);
    CHECK(recorder.shots_from(ShotSource::burst) == 1);
    CHECK(!recorder.shots.back().aim);
    std::cout << "meteors and bursts are reported passed\n";
}

// The start of a player: its commander is the fixture's type.
struct Start : sim::unit_spawn::StartHost {
    uint16_t commander_type_for_side(uint8_t) override { return 1; }

    void report_missing_start_position(int32_t) override {
        throw std::runtime_error("unexpected missing start position");
    }

    void set_camera_position(int32_t, int32_t, uint32_t) override {}
};

/// Units created and finished are reported: a start commander, a building
/// created finished (by itself), and a frame its builder's link finishes,
/// once, even when a state record marked it finished first.
void creations_and_completions_are_reported() {
    Fixture f;
    Recorder recorder;
    f.match->event_hooks = recorder.hooks();
    auto& world = f.match->state();

    const std::array<sim::unit_spawn::StartMarker, 1> markers{{{1, 0, 96, 96}}};
    Start start;
    const auto started = f.match->start_player(0, {1, 0, 0, 100}, markers, 0, 256, 128, start);
    CHECK(started.unit != nullptr);
    const auto commander = static_cast<uint16_t>(oa::world_unit_slot(&world, started.unit));
    CHECK(recorder.created.size() == 1 && recorder.created.front().unit == commander);
    CHECK(recorder.finished.empty());
    CHECK(recorder.created_by.size() == 1 && recorder.created_by.front().unit == commander);
    CHECK(recorder.created_by.front().builder == 0);

    // A frame, then its builder's link: finished once, by the builder.
    auto* frame = f.match->create({0, 1, {fx(48), fx(32), fx(48)}, false, 1, 0});
    CHECK(frame != nullptr && frame->record.build_remaining != 0.0F);
    CHECK(recorder.created.size() == 2 && recorder.created.back().unit == frame->unit_index);
    CHECK(recorder.finished.empty());
    f.match->finish_unit(frame->unit_index, commander);
    CHECK(recorder.finished.size() == 1);
    CHECK(
        recorder.finished.front().unit == frame->unit_index &&
        recorder.finished.front().builder == commander && recorder.finished.front().finished_at_call
    );
    f.match->finish_unit(frame->unit_index, commander);
    CHECK(recorder.finished.size() == 1);

    // A frame a state record from elsewhere already marked finished is still
    // reported when its builder's link arrives.
    auto* copied = f.match->create({0, 1, {fx(80), fx(32), fx(48)}, false, 1, 0});
    CHECK(copied != nullptr);
    copied->record.build_remaining = 0.0F;
    f.match->finish_unit(copied->unit_index, commander);
    CHECK(recorder.finished.size() == 2 && recorder.finished.back().unit == copied->unit_index);

    // A building (bmcode 0) created finished names itself as its builder,
    // after its creation, and a link from elsewhere does not finish it again.
    world.unit_defs[1].bm_code = 0;
    auto* building = f.match->create({1, 1, {fx(176), fx(32), fx(176)}, true, 1, 0});
    world.unit_defs[1].bm_code = 1;
    CHECK(building != nullptr);
    CHECK(recorder.created.size() == 4 && recorder.created.back().unit == building->unit_index);
    CHECK(
        recorder.created_by.size() == 4 && recorder.created_by.back().unit == building->unit_index
    );
    CHECK(recorder.created_by.back().builder == 0);
    CHECK(recorder.finished.size() == 3);
    CHECK(
        recorder.finished.back().unit == building->unit_index &&
        recorder.finished.back().builder == building->unit_index
    );
    // Created, then created by no unit, then finished by itself.
    CHECK(recorder.births.size() >= 3);
    const auto last = recorder.births.size();
    CHECK(recorder.births[last - 3] == BirthStep::created);
    CHECK(recorder.births[last - 2] == BirthStep::created_by);
    CHECK(recorder.births[last - 1] == BirthStep::finished);
    f.match->finish_unit(building->unit_index, building->unit_index);
    CHECK(recorder.finished.size() == 3);
    std::cout << "creations and completions are reported passed\n";
}

/// A death settled elsewhere and a paralysis are reported as such.
void settled_deaths_and_other_kinds_are_reported() {
    Fixture f;
    Recorder recorder;
    f.match->event_hooks = recorder.hooks();
    auto& victim = f.spawn(1, 176, 64);
    auto& killer = f.spawn(0, 48, 48);
    f.match->apply_damage_event(victim, &killer, 5, paralyzer_hit, 0);
    CHECK(recorder.damage.size() == 1);
    CHECK(
        recorder.damage.front().target == victim.unit_index &&
        recorder.damage.front().source == killer.unit_index &&
        recorder.damage.front().amount == 5 && recorder.damage.front().kind == paralyzer_hit
    );
    f.match->simulation().players[1].status = OA_PLAYER_STATUS_MIRRORED;
    const KillOutcome outcome{sim::match_runtime::DeathKind::weapon, 50, 1};
    f.match->apply_kill(victim.unit_index, outcome, killer.unit_index, 0);
    CHECK(recorder.deaths.size() == 1);
    const auto& death = recorder.deaths.front();
    CHECK(
        death.unit == victim.unit_index && death.owner == 1 && death.type_index == 1 &&
        death.live_at_call && death.settled_elsewhere
    );
    // A dead unit dies once and takes no damage.
    f.match->apply_kill(victim.unit_index, outcome, killer.unit_index, 0);
    f.match->apply_damage_event(victim, &killer, 5, weapon_kind, 0);
    CHECK(recorder.deaths.size() == 1 && recorder.damage.size() == 1);
    std::cout << "settled deaths and other kinds are reported passed\n";
}

/// The skirmish with a primary recorder and two listener recorders plays as
/// without, and each listener keeps the primary's reports.
void listeners_change_nothing() {
    Recorder primary;
    Recorder first;
    Recorder second;
    const Run plain = play({});
    const EventHooks listeners[] = {first.hooks(), second.hooks()};
    const Run hooked = play(primary.hooks(), listeners);
    CHECK(plain.totals == hooked.totals);
    CHECK(plain.halfway_state == hooked.halfway_state);
    CHECK(plain.final_state == hooked.final_state);
    CHECK(hooked.halfway_state == pinned_halfway_state);
    CHECK(hooked.final_state == pinned_final_state);
    CHECK(hooked.totals.back() == pinned_final_total);
    CHECK(same_records(first, primary));
    CHECK(same_records(second, primary));
    std::cout << "listeners change nothing passed\n";
}

// Which table heard an event, and the unit it named.
struct Heard {
    int table{};
    const char* event{};
    uint16_t unit{};
};

// A table index and the log its hooks append to.
struct Ear {
    std::vector<Heard>* log{};
    int table{};
};

/// Returns hooks that append this ear's table, the event and the unit.
///
/// @param ear the listener's log and table index
/// @return creation, damage and death hooks
EventHooks hearing(Ear& ear) {
    EventHooks hooks;
    hooks.context = &ear;
    hooks.unit_created = [](void* context, const World&, uint16_t unit) {
        const auto* heard = static_cast<const Ear*>(context);
        heard->log->push_back({heard->table, "created", unit});
    };
    hooks.unit_damaged =
        [](void* context, const World&, uint16_t target, uint16_t, int16_t, uint8_t) {
            const auto* heard = static_cast<const Ear*>(context);
            heard->log->push_back({heard->table, "damaged", target});
        };
    hooks.unit_died = [](void* context, const World&, uint16_t unit, const KillOutcome&, bool) {
        const auto* heard = static_cast<const Ear*>(context);
        heard->log->push_back({heard->table, "died", unit});
    };
    return hooks;
}

/// One creation, one damage event and one death are heard by the primary
/// table, then the first listener, then the second.
void listeners_hear_in_order() {
    Fixture f;
    std::vector<Heard> log;
    std::array<Ear, 3> ears{Ear{&log, 0}, Ear{&log, 1}, Ear{&log, 2}};
    f.match->event_hooks = hearing(ears[0]);
    CHECK(f.match->add_event_listener(hearing(ears[1])) != 0);
    CHECK(f.match->add_event_listener(hearing(ears[2])) != 0);
    auto& victim = f.spawn(1, 176, 64);
    f.match->apply_damage_event(victim, nullptr, 5, weapon_kind, 0);
    const KillOutcome outcome{sim::match_runtime::DeathKind::weapon, 50, 1};
    f.match->apply_kill(victim.unit_index, outcome, 0, 0);

    CHECK(log.size() == 9);
    int created = 0;
    int damaged = 0;
    int died = 0;
    for (std::size_t index = 0; index < log.size(); index += 3) {
        const auto& primary = log[index];
        const auto& first = log[index + 1];
        const auto& second = log[index + 2];
        CHECK(primary.table == 0 && first.table == 1 && second.table == 2);
        CHECK(std::string_view{primary.event} == first.event);
        CHECK(std::string_view{primary.event} == second.event);
        CHECK(primary.unit == first.unit && primary.unit == second.unit);
        CHECK(primary.unit == victim.unit_index);
        if (std::string_view{primary.event} == "created")
            ++created;
        else if (std::string_view{primary.event} == "damaged")
            ++damaged;
        else if (std::string_view{primary.event} == "died")
            ++died;
    }
    CHECK(created == 1 && damaged == 1 && died == 1);
    std::cout << "listeners hear in order passed\n";
}

/// Eight listeners take distinct ids, a ninth is refused, and removing one
/// leaves the rest in order. A later id is new, and 0 removes nothing.
void listener_list_is_bounded() {
    Fixture f;
    std::vector<Heard> log;
    std::array<Ear, 8> ears{};
    std::array<EventListenerId, 8> ids{};
    for (int table = 0; table < 8; ++table) {
        ears[static_cast<std::size_t>(table)] = Ear{&log, table};
        ids[static_cast<std::size_t>(table)] =
            f.match->add_event_listener(hearing(ears[static_cast<std::size_t>(table)]));
        CHECK(ids[static_cast<std::size_t>(table)] != 0);
    }
    for (int table = 0; table < 8; ++table)
        for (int other = table + 1; other < 8; ++other)
            CHECK(ids[static_cast<std::size_t>(table)] != ids[static_cast<std::size_t>(other)]);
    CHECK(f.match->add_event_listener(hearing(ears[0])) == 0);
    CHECK(f.match->event_listener_count() == 8);

    const auto removed = ids[2];
    CHECK(f.match->remove_event_listener(removed));
    CHECK(!f.match->remove_event_listener(removed));
    CHECK(!f.match->remove_event_listener(0));
    CHECK(f.match->event_listener_count() == 7);

    log.clear();
    (void)f.spawn(0, 48, 48);
    const int order[] = {0, 1, 3, 4, 5, 6, 7};
    CHECK(log.size() == 7);
    for (std::size_t index = 0; index < log.size(); ++index)
        CHECK(log[index].table == order[index] && std::string_view{log[index].event} == "created");

    Ear later{&log, 8};
    const auto added = f.match->add_event_listener(hearing(later));
    CHECK(added != 0);
    for (const auto id : ids)
        CHECK(added != id);
    std::cout << "listener list is bounded passed\n";
}

/// Replacing the primary table and then clearing it, as a shot counter and
/// the director do, leaves every listener hearing every event.
void primary_swap_keeps_listeners() {
    Fixture f;
    Recorder primary;
    Recorder replacement;
    Recorder first;
    Recorder second;
    f.match->event_hooks = primary.hooks();
    CHECK(f.match->add_event_listener(first.hooks()) != 0);
    CHECK(f.match->add_event_listener(second.hooks()) != 0);

    const auto saved = f.match->event_hooks;
    f.match->event_hooks = replacement.hooks();
    const auto during = f.spawn(0, 48, 48).unit_index;
    f.match->event_hooks = saved;
    const auto restored = f.spawn(0, 80, 48).unit_index;
    f.match->event_hooks = {};
    const auto cleared = f.spawn(0, 112, 48).unit_index;

    CHECK(replacement.created.size() == 1 && replacement.created.front().unit == during);
    CHECK(primary.created.size() == 1 && primary.created.front().unit == restored);
    CHECK(first.created.size() == 3 && second.created.size() == 3);
    CHECK(first.created[0].unit == during && first.created[1].unit == restored);
    CHECK(first.created[2].unit == cleared);
    CHECK(same_records(first, second));
    std::cout << "primary swap keeps listeners passed\n";
}

/// Listeners with an all-null primary table reach the pins, and each records
/// every created unit and every death.
void listeners_alone_change_nothing() {
    Recorder first;
    Recorder second;
    Recorder third;
    const EventHooks listeners[] = {first.hooks(), second.hooks(), third.hooks()};
    const Run run = play({}, listeners);
    CHECK(run.halfway_state == pinned_halfway_state);
    CHECK(run.final_state == pinned_final_state);
    CHECK(run.totals.back() == pinned_final_total);
    CHECK(same_records(first, second));
    CHECK(same_records(second, third));
    CHECK(first.created.size() == run.units.size());
    for (std::size_t index = 0; index < run.units.size(); ++index)
        CHECK(first.created[index].unit == run.units[index]);
    bool gunner = false;
    bool target = false;
    CHECK(!first.deaths.empty());
    for (const auto& death : first.deaths) {
        CHECK(spawned(run, death.unit));
        if (death.unit == run.gunner)
            gunner = true;
        if (death.unit == run.target)
            target = true;
    }
    CHECK(gunner && target);
    std::cout << "listeners alone change nothing passed\n";
}

} // namespace

int main() {
    try {
        hooks_change_nothing();
        skirmish_reports_every_kind();
        applied_shot_reports_its_aim();
        meteors_and_bursts_are_reported();
        creations_and_completions_are_reported();
        settled_deaths_and_other_kinds_are_reported();
        listeners_change_nothing();
        listeners_hear_in_order();
        listener_list_is_bounded();
        primary_swap_keeps_listeners();
        listeners_alone_change_nothing();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
