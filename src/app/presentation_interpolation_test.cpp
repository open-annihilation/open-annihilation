// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What frames between two ticks show: tick fractions of an alpha, blends of
// 16.16 places (across the 32-bit wrap) and of angle words (the shorter
// turn), steps taken over a batch of ticks, a unit's pose noted tick by tick
// and batch by batch with its jumps, new instances and ticks run unseen, a
// unit's copies placed between two poses with the source left alone, its
// pieces between their two poses across a jump, a spin of more than half
// a revolution followed along its turn rather than back the short way,
// projectiles followed through the pool's compaction and across a batch,
// debris followed by slot, and the debug grid's random numbers taken once a
// tick however often it is drawn.
#include "presentation_interpolation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x);             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

using oa::app::whole_tick;

constexpr int32_t unit_fixed = 0x10000;
constexpr uint32_t half_tick = whole_tick / 2;
constexpr uint32_t quarter_tick = whole_tick / 4;

bool same(const oa::FixedVec3& a, const oa::FixedVec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// A two-piece model: a base and a turret on it.
std::shared_ptr<const oa::formats::objects3d::Model> turret_model() {
    auto model = std::make_shared<oa::formats::objects3d::Model>();
    oa::formats::objects3d::Object base;
    base.name = "base";
    base.vertices = {{0, 0, 0}, {4 * unit_fixed, 0, 0}, {0, 0, 4 * unit_fixed}};
    base.first_child = 1;
    oa::formats::objects3d::Object turret;
    turret.name = "turret";
    turret.vertices = {{0, 0, 0}, {2 * unit_fixed, 0, 0}, {0, 2 * unit_fixed, 0}};
    turret.parent = 0;
    model->objects.push_back(base);
    model->objects.push_back(turret);
    return model;
}

void test_tick_fraction() {
    CHECK(oa::app::tick_fraction(1.0F) == whole_tick);
    CHECK(oa::app::tick_fraction(0.0F) == 0);
    CHECK(oa::app::tick_fraction(0.5F) == half_tick);
    CHECK(oa::app::tick_fraction(0.25F) == quarter_tick);
    CHECK(oa::app::tick_fraction(2.0F) == whole_tick);
    CHECK(oa::app::tick_fraction(-1.0F) == 0);
    CHECK(oa::app::tick_fraction(__builtin_nanf("")) == whole_tick);
    // The director's sixteenth-bit fractions come back exactly.
    CHECK(oa::app::tick_fraction(static_cast<float>(12345) / whole_tick) == 12345);
}

void test_blends() {
    // A whole tick is the current value, none the previous one.
    CHECK(oa::app::blend_fixed(10 * unit_fixed, 20 * unit_fixed, whole_tick) == 20 * unit_fixed);
    CHECK(oa::app::blend_fixed(10 * unit_fixed, 20 * unit_fixed, 0) == 10 * unit_fixed);
    CHECK(oa::app::blend_fixed(10 * unit_fixed, 20 * unit_fixed, half_tick) == 15 * unit_fixed);
    // Backwards, the part rounds toward negative infinity.
    CHECK(oa::app::blend_fixed(1, 0, half_tick) == 0);
    CHECK(oa::app::blend_fixed(0, 3, half_tick) == 1);
    // Across the 32-bit wrap, the short way.
    CHECK(oa::app::blend_fixed(INT32_MAX, INT32_MIN + 1, half_tick) == INT32_MIN);
    // Even steps: a unit two pixels a tick moves half a pixel a quarter.
    for (uint32_t step = 0; step <= 4; ++step)
        CHECK(
            oa::app::blend_fixed(0, 2 * unit_fixed, step * quarter_tick) ==
            static_cast<int32_t>(step) * unit_fixed / 2
        );

    // Angles turn the shorter way, across the half turn too.
    CHECK(oa::app::blend_angle(100, 300, half_tick) == 200);
    CHECK(oa::app::blend_angle(32000, -32000, half_tick) == -32768);
    CHECK(oa::app::blend_angle(-32000, 32000, half_tick) == -32768);
    CHECK(oa::app::blend_angle(-100, 100, quarter_tick) == -50);
    CHECK(oa::app::blend_angle(5, 9, whole_tick) == 9);
    CHECK(oa::app::blend_angle(5, 9, 0) == 5);

    const oa::FixedVec3 from{0, 0, 0};
    const oa::FixedVec3 to{4 * unit_fixed, -8 * unit_fixed, 2 * unit_fixed};
    CHECK(same(
        oa::app::blend_point(from, to, quarter_tick), {unit_fixed, -2 * unit_fixed, unit_fixed / 2}
    ));
    // A step's part: the point less the rest of the step.
    CHECK(same(
        oa::app::point_along_step(to, to, 1, quarter_tick),
        {unit_fixed, -2 * unit_fixed, unit_fixed / 2}
    ));
    CHECK(same(oa::app::point_along_step(to, to, 1, whole_tick), to));
    // Over a batch of two ticks, two steps' way.
    CHECK(same(oa::app::point_along_step(to, to, 2, 0), {-to.x, -to.y, -to.z}));
    CHECK(same(oa::app::point_along_step(to, to, 2, half_tick), {0, 0, 0}));
    // Angles turned each tick: the step's part, across the half turn too.
    CHECK(oa::app::angle_along_step(300, 200, 1, half_tick) == 200);
    CHECK(oa::app::angle_along_step(300, 100, 2, quarter_tick) == 150);
    CHECK(oa::app::angle_along_step(-32700, 200, 1, 0) == 32636);
    CHECK(oa::app::angle_along_step(5, 9, 3, whole_tick) == 5);

    // Ticks from one seen to the next: within a batch, not past it.
    CHECK(oa::app::batch_ticks(10, 11) == 1);
    CHECK(oa::app::batch_ticks(10, 10 + oa::app::most_batch_ticks) == oa::app::most_batch_ticks);
    CHECK(oa::app::batch_ticks(10, 11 + oa::app::most_batch_ticks) == 0);
    CHECK(oa::app::batch_ticks(10, 10) == 0);
    CHECK(oa::app::batch_ticks(10, 9) == 0);
    CHECK(oa::app::batch_ticks(UINT32_MAX, 1) == 2);
}

oa::Unit unit_at(int32_t x, int32_t z, uint16_t heading) {
    oa::Unit unit{};
    unit.position = {x * unit_fixed, 0, z * unit_fixed};
    unit.heading = heading;
    unit.type_index = 1;
    return unit;
}

void test_instance_fits_unit() {
    const int owner = 0;
    const int other = 0;
    const auto token = reinterpret_cast<uintptr_t>(&owner);
    const auto instance = oa::sim::model_runtime::make_instance(turret_model(), token);
    CHECK(oa::app::instance_fits_unit(instance, &owner));
    // Made for another unit, or for none.
    CHECK(!oa::app::instance_fits_unit(instance, &other));
    CHECK(!oa::app::instance_fits_unit(instance, nullptr));
    CHECK(
        !oa::app::instance_fits_unit(oa::sim::model_runtime::make_instance(turret_model()), &owner)
    );
    // An instance with no pieces fits: it has no model, or one whose hierarchy was refused.
    CHECK(
        oa::app::instance_fits_unit(oa::sim::model_runtime::make_instance(nullptr, token), &owner)
    );
}

void test_unit_motion() {
    auto instance = oa::sim::model_runtime::make_instance(turret_model());
    CHECK(instance.pieces().size() == 2);
    oa::app::UnitMotion motion;

    // The first tick has no previous pose.
    oa::app::observe_unit(motion, 10, 1, unit_at(100, 100, 0), instance);
    CHECK(motion.seen && !motion.continued && !motion.moved);

    // The next tick: moved, and its turret turned.
    instance.pieces()[1].rotation.xz = 0x400;
    oa::app::observe_unit(motion, 11, 1, unit_at(104, 100, 0x200), instance);
    CHECK(motion.continued && motion.moved);
    CHECK(motion.previous.position.x == 100 * unit_fixed);
    CHECK(motion.current.position.x == 104 * unit_fixed);
    // The same tick again changes nothing.
    oa::app::observe_unit(motion, 11, 1, unit_at(999, 999, 0), instance);
    CHECK(motion.current.position.x == 104 * unit_fixed && motion.moved);

    // The copies go part of the way; the source stays as it is.
    oa::Unit record = unit_at(104, 100, 0x200);
    auto copy = instance;
    oa::app::blend_unit_pose(motion.previous, motion.current, half_tick, record, copy);
    CHECK(record.position.x == 102 * unit_fixed && record.position.z == 100 * unit_fixed);
    CHECK(record.heading == 0x100);
    CHECK(copy.pieces()[1].rotation.xz == 0x200);
    CHECK(instance.pieces()[1].rotation.xz == 0x400);
    oa::app::blend_unit_pose(motion.previous, motion.current, 0, record, copy);
    CHECK(
        record.position.x == 100 * unit_fixed && record.heading == 0 &&
        copy.pieces()[1].rotation.xz == 0
    );
    oa::app::blend_unit_pose(motion.previous, motion.current, whole_tick, record, copy);
    CHECK(
        record.position.x == 104 * unit_fixed && record.heading == 0x200 &&
        copy.pieces()[1].rotation.xz == 0x400
    );

    // Standing still: continued, but no move.
    oa::app::observe_unit(motion, 12, 1, unit_at(104, 100, 0x200), instance);
    CHECK(motion.continued && !motion.moved && !motion.pieces_moved);

    // A jump further than unit_jump_pixels shows where it lands; its turret
    // turning meanwhile still moves between the two ticks for a unit drawn
    // elsewhere than its record (on its playout).
    instance.pieces()[1].rotation.xz = 0x800;
    oa::app::observe_unit(
        motion, 13, 1, unit_at(104 + oa::app::unit_jump_pixels + 1, 100, 0x200), instance
    );
    CHECK(!motion.continued && !motion.moved && motion.pieces_moved);
    auto turning = instance;
    oa::app::blend_unit_pieces(motion.previous, motion.current, quarter_tick, turning);
    CHECK(turning.pieces()[1].rotation.xz == 0x500);
    CHECK(instance.pieces()[1].rotation.xz == 0x800);
    oa::app::observe_unit(
        motion, 14, 1, unit_at(104 + oa::app::unit_jump_pixels + 3, 100, 0x200), instance
    );
    CHECK(motion.continued && motion.moved);

    // A batch of two ticks, as above normal speed: the pose seen before the
    // batch is the previous one.
    oa::app::observe_unit(
        motion, 16, 1, unit_at(104 + oa::app::unit_jump_pixels + 9, 100, 0x200), instance
    );
    CHECK(motion.continued && motion.moved);
    CHECK(motion.previous.position.x == (104 + oa::app::unit_jump_pixels + 3) * unit_fixed);

    // Ticks run past a batch leave no previous pose.
    const uint32_t unseen = 16 + oa::app::most_batch_ticks + 1;
    oa::app::observe_unit(motion, unseen, 1, unit_at(200, 100, 0x200), instance);
    CHECK(!motion.continued && !motion.moved);

    // Loaded into a transport: a jump of its own kind.
    oa::Unit carried = unit_at(201, 100, 0x200);
    carried.attach_parent = 7;
    oa::app::observe_unit(motion, unseen + 1, 1, carried, instance);
    CHECK(!motion.continued);

    // A new instance in the slot starts afresh, its draw state with it.
    motion.state.cache.draws = 5;
    oa::app::observe_unit(motion, unseen + 2, 2, unit_at(300, 300, 0), instance);
    CHECK(motion.seen && !motion.continued && motion.state.cache.draws == 0);
    CHECK(motion.instance_generation == 2);

    // Replacing an instance without advancing the tick still takes its first pose.
    motion.state.cache.draws = 5;
    oa::app::observe_unit(motion, unseen + 2, 3, unit_at(301, 300, 0), instance);
    CHECK(motion.seen && !motion.continued && motion.state.cache.draws == 0);
    CHECK(motion.instance_generation == 3 && motion.current.position.x == 301 * unit_fixed);

    motion.instance = instance;
    CHECK(!motion.instance.pieces().empty());
    oa::app::forget_unit(motion);
    CHECK(!motion.seen);
    // The copies go with the unit.
    CHECK(motion.current.pieces.empty() && motion.previous.pieces.empty());
    CHECK(motion.instance.pieces().empty());
}

void test_fast_spin() {
    // More than half a revolution in one tick. The short way from 0 to the
    // wrapped angle turns backwards; the spin goes on forwards.
    constexpr int32_t speed = 40000;
    const auto landed = static_cast<int16_t>(static_cast<uint16_t>(speed));
    CHECK(oa::app::blend_piece_turn(0, landed, speed, 1, 0) == 0);
    CHECK(oa::app::blend_piece_turn(0, landed, speed, 1, whole_tick) == landed);
    CHECK(oa::app::blend_piece_turn(0, landed, speed, 1, half_tick) == 20000);
    CHECK(oa::app::blend_piece_turn(0, landed, speed, 1, quarter_tick) == 10000);
    CHECK(oa::app::blend_angle(0, landed, half_tick) < 0);

    // The other way.
    constexpr int32_t back = -40000;
    const auto landed_back = static_cast<int16_t>(static_cast<uint16_t>(back));
    CHECK(oa::app::blend_piece_turn(0, landed_back, back, 1, half_tick) == -20000);

    // Exactly half a revolution follows the spin, not the negative short way.
    constexpr int32_t half_turn = 32768;
    const auto half_landed = static_cast<int16_t>(static_cast<uint16_t>(half_turn));
    CHECK(oa::app::blend_piece_turn(0, half_landed, half_turn, 1, half_tick) == 16384);
    CHECK(oa::app::blend_angle(0, half_landed, half_tick) < 0);

    // A short turn whose speed accounts for it matches the short way.
    CHECK(oa::app::blend_piece_turn(0, 0x400, 0x400, 1, half_tick) == 0x200);

    // A speed that does not account for the two angles keeps the short way
    // while the turn stays within half a revolution.
    CHECK(
        oa::app::blend_piece_turn(0, 0x400, 1000, 1, half_tick) ==
        oa::app::blend_angle(0, 0x400, half_tick)
    );

    // Past half a revolution a tick, a speed that no longer accounts for the
    // two angles shows the current angle rather than turning back.
    CHECK(oa::app::blend_piece_turn(0, 100, speed, 1, 0) == 0);
    CHECK(oa::app::blend_piece_turn(0, 100, speed, 1, half_tick) == 100);

    // A batch of two ticks: halfway is one tick of the spin.
    constexpr int32_t batch_speed = 20000;
    const auto batch_landed =
        static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(batch_speed) * 2));
    CHECK(
        oa::app::blend_piece_turn(0, batch_landed, batch_speed, 2, half_tick) ==
        static_cast<int16_t>(batch_speed)
    );

    // The piece blend reads the turn off the later pose's matching axis.
    auto instance = oa::sim::model_runtime::make_instance(turret_model());
    oa::app::UnitMotion motion;
    oa::app::observe_unit(motion, 1, 1, unit_at(10, 10, 0), instance);
    instance.pieces()[1].rotation.xz = landed;
    std::array<int32_t, 6> turns{};
    turns[1 * 3 + 1] = speed;
    oa::app::observe_unit(motion, 2, 1, unit_at(10, 10, 0), instance, turns);
    auto copy = instance;
    oa::app::blend_unit_pieces(motion.previous, motion.current, half_tick, copy, 1);
    CHECK(copy.pieces()[1].rotation.xz == 20000);
    CHECK(instance.pieces()[1].rotation.xz == landed);
    CHECK(motion.current.pieces[1].turn_per_tick[1] == speed);
}

oa::Projectile shot_at(int32_t x, int32_t index_before, uint32_t created_tick, oa::oa_ref32 def) {
    oa::Projectile shot{};
    shot.position = {x * unit_fixed, 0, 0};
    shot.origin = {0, 0, 0};
    shot.def = def;
    shot.source = 3;
    shot.created_tick = created_tick;
    shot.compact_index = static_cast<int16_t>(index_before);
    return shot;
}

void test_shots() {
    oa::app::ShotFlight flight;
    std::vector<oa::Projectile> pool{
        shot_at(10, 0, 5, 1), shot_at(20, 1, 5, 2), shot_at(30, 2, 5, 3)
    };
    oa::app::observe_shots(flight, 8, pool);
    // The next tick the first shot is gone and the pool closed up: the
    // others moved down one index and on 4 pixels; a fresh one was fired.
    std::vector<oa::Projectile> next{
        shot_at(24, 1, 5, 2), shot_at(34, 2, 5, 3), shot_at(8, 2, 9, 4)
    };
    next[2].origin = {4 * unit_fixed, 0, 0};
    oa::app::observe_shots(flight, 9, next);
    CHECK(flight.continued);
    const auto first = oa::app::presented_shot(flight, 9, 0, next[0], half_tick);
    CHECK(first.position.x == 22 * unit_fixed);
    const auto second = oa::app::presented_shot(flight, 9, 1, next[1], quarter_tick);
    CHECK(second.position.x == 31 * unit_fixed);
    // Fired this tick: from its muzzle, whatever its index before says.
    const auto fresh = oa::app::presented_shot(flight, 9, 2, next[2], half_tick);
    CHECK(fresh.position.x == 6 * unit_fixed);
    // A whole tick shows every shot as it is.
    CHECK(oa::app::presented_shot(flight, 9, 0, next[0], whole_tick).position.x == 24 * unit_fixed);
    // Another weapon at the index found before is another shot: shown as it is.
    auto stranger = shot_at(50, 0, 5, 9);
    CHECK(oa::app::presented_shot(flight, 9, 0, stranger, half_tick).position.x == 50 * unit_fixed);

    // A batch of two ticks closed the pool's gaps twice: each shot is the
    // nearest of its weapon, source and tick of creation. Two shots of one
    // burst swapped places in the pool; one fired in the batch's first tick
    // starts from its muzzle.
    std::vector<oa::Projectile> twin{
        shot_at(40, 0, 7, 5), shot_at(60, 1, 7, 5), shot_at(70, 2, 7, 6)
    };
    oa::app::observe_shots(flight, 10, twin);
    std::vector<oa::Projectile> after_batch{
        shot_at(68, 0, 7, 5), shot_at(48, 0, 7, 5), shot_at(9, 0, 11, 8)
    };
    after_batch[2].origin = {5 * unit_fixed, 0, 0};
    oa::app::observe_shots(flight, 12, after_batch);
    CHECK(flight.continued && flight.previous_tick == 10);
    CHECK(flight.previous_index[0] == 1 && flight.previous_index[1] == 0);
    CHECK(
        oa::app::presented_shot(flight, 12, 0, after_batch[0], half_tick).position.x ==
        64 * unit_fixed
    );
    CHECK(
        oa::app::presented_shot(flight, 12, 1, after_batch[1], half_tick).position.x ==
        44 * unit_fixed
    );
    CHECK(
        oa::app::presented_shot(flight, 12, 2, after_batch[2], half_tick).position.x ==
        7 * unit_fixed
    );
    // Ticks run past a batch follow no shot.
    oa::app::observe_shots(flight, 13 + oa::app::most_batch_ticks, after_batch);
    CHECK(!flight.continued);
    CHECK(
        oa::app::presented_shot(
            flight, 13 + oa::app::most_batch_ticks, 0, after_batch[0], half_tick
        )
            .position.x == 68 * unit_fixed
    );
}

void test_debris() {
    oa::app::DebrisFall fall;
    std::vector<oa::sim::effect_particles::DebrisPiece> table(
        oa::sim::effect_particles::debris_capacity
    );
    table[3].live = true;
    table[3].unit = 5;
    table[3].piece = 2;
    table[3].position = {10 * unit_fixed, 20 * unit_fixed, 0};
    table[3].spin[0] = 100;
    oa::app::observe_debris(fall, 4, table);
    table[3].position = {14 * unit_fixed, 16 * unit_fixed, 0};
    table[3].spin[0] = 300;
    oa::app::observe_debris(fall, 5, table);
    const auto shown = oa::app::presented_debris(fall, 5, 3, table[3], half_tick);
    CHECK(shown.position.x == 12 * unit_fixed && shown.position.y == 18 * unit_fixed);
    CHECK(shown.spin[0] == 200);
    // A whole tick, as every frame is drawn with Smooth motion Off, shows the
    // piece where the tick left it.
    const auto whole = oa::app::presented_debris(fall, 5, 3, table[3], whole_tick);
    CHECK(whole.position.x == 14 * unit_fixed && whole.position.y == 16 * unit_fixed);
    CHECK(whole.spin[0] == 300);
    // A piece of another unit in the slot is another piece.
    oa::app::observe_debris(fall, 6, table);
    table[3].unit = 6;
    oa::app::observe_debris(fall, 7, table);
    CHECK(oa::app::presented_debris(fall, 7, 3, table[3], half_tick).position.x == 14 * unit_fixed);
}

} // namespace

void test_debug_grid_numbers() {
    oa::app::DebugGridRandom kept{};
    int32_t stream = 0;
    const auto draw = [&](uint32_t tick, std::size_t count) {
        oa::app::start_debug_grid_draw(kept, tick);
        std::vector<int32_t> numbers;
        for (std::size_t index = 0; index < count; ++index)
            numbers.push_back(oa::app::next_debug_grid_number(kept, [&] { return ++stream; }));
        return numbers;
    };
    // The tick's first draw takes from the stream.
    CHECK((draw(5, 3) == std::vector<int32_t>{1, 2, 3}) && stream == 3);
    // Later draws of the tick repeat the numbers in turn and take none.
    CHECK((draw(5, 3) == std::vector<int32_t>{1, 2, 3}) && stream == 3);
    CHECK((draw(5, 4) == std::vector<int32_t>{1, 2, 3, 1}) && stream == 3);
    // The next tick takes new ones, as a draw a tick did.
    CHECK((draw(6, 2) == std::vector<int32_t>{4, 5}) && stream == 5);
    CHECK((draw(6, 1) == std::vector<int32_t>{4}) && stream == 5);
    // A tick whose first draw took none gives 0 to later draws.
    CHECK((draw(7, 0).empty()) && (draw(7, 2) == std::vector<int32_t>{0, 0}) && stream == 5);
    // The grid's own generator starts alike in every match and gives 0 to 32767.
    oa::app::DebugGridRandom first{};
    oa::app::DebugGridRandom second{};
    for (int32_t step = 0; step < 64; ++step) {
        const int32_t value = first.generate();
        CHECK(value == second.generate() && value >= 0 && value <= 0x7fff);
    }
}

int main() {
    test_tick_fraction();
    test_blends();
    test_instance_fits_unit();
    test_unit_motion();
    test_fast_spin();
    test_shots();
    test_debris();
    test_debug_grid_numbers();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
