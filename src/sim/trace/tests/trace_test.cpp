// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/trace.hpp"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

using namespace oa;
using namespace oa::sim::trace;

namespace {
int failures = 0;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

constexpr uint32_t slot_count = 6;
constexpr uint32_t mobile_slot = 1, carried_slot = 2, building_slot = 3, dying_slot = 4;

// Slots 1..4 hold units of player 0: a mobile unit, one it carries, a
// building (no movement object) and a mobile unit whose live flag is clear.
World* make_world() {
    World* w = world_create();
    WorldCapacity capacity{slot_count, 2, 0};
    if (w == nullptr || !world_alloc_tables(w, &capacity))
        std::abort();
    w->game.tick = 42;
    for (uint32_t slot = 1; slot <= dying_slot; ++slot) {
        Unit& u = w->units[slot];
        u.type_index = 1;
        u.id = static_cast<uint16_t>(slot);
        u.owner = oa_ref_from_index(0);
        u.health = static_cast<int16_t>(900 + slot);
        u.build_remaining = 0.25F * static_cast<float>(slot);
        u.state_flags = 0x05;
        u.flags = OA_UNIT_FLAG_LIVE | 1u;
        u.position = {
            static_cast<int32_t>(slot) << 20, 32 << 16, -(static_cast<int32_t>(slot) << 18)
        };
        u.heading = 0x4000;
        u.pitch = -3;
        u.bank = -7;
        u.attach_piece = 0xff;
        u.weapons[0].target_a = static_cast<int16_t>(mobile_slot);
        u.weapons[0].target_b = OA_UNIT_TARGET_IS_UNIT;
        u.weapons[1].reload = 12;
    }
    w->units[carried_slot].attach_parent = oa_unit_ref_from_slot(mobile_slot);
    w->units[carried_slot].attach_piece = 3;
    w->units[dying_slot].flags &= ~OA_UNIT_FLAG_LIVE;
    w->game.projectile_count = 1;
    w->projectiles[0].def = 1;
    w->projectiles[0].position = {100 << 16, 20 << 16, 50 << 16};
    for (uint32_t index = 0; index < 2; ++index) {
        Player& p = w->game.players[index];
        p.in_use = 1;
        p.index = static_cast<uint8_t>(index);
        p.metal = 1000.0F;
        p.energy = 1000.0F;
    }
    return w;
}

struct Sides {
    UnitSide slot[slot_count]{};
};

Sides make_sides() {
    Sides sides{};
    for (uint32_t slot : {mobile_slot, carried_slot, dying_slot}) {
        sides.slot[slot].has_movement = 1;
        sides.slot[slot].movement_speed = 0x18000u + slot;
    }
    sides.slot[mobile_slot].has_order = 1;
    sides.slot[mobile_slot].order_kind = 6;
    return sides;
}

// The fields the full unit record packs, read one by one from the Unit record.
uint32_t fold_by_fields(uint32_t state, const World& w, const Unit& unit, const UnitSide& side) {
    state = tick_fold(state, unit.type_index);
    state = tick_fold(state, static_cast<uint32_t>(static_cast<int32_t>(unit.health)));
    state = tick_fold(state, std::bit_cast<uint32_t>(unit.build_remaining));
    state = tick_fold(state, unit.state_flags);
    state = tick_fold(state, unit.flags & 3u);
    const uint32_t parent = unit.attach_parent;
    state = tick_fold(state, parent != 0 ? 1u : 0u);
    if (parent != 0) {
        state = tick_fold(state, w.units[oa_unit_slot_from_ref(parent)].id);
        return tick_fold(
            state,
            static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(unit.attach_piece)))
        );
    }
    for (oa_fixed coordinate : {unit.position.x, unit.position.y, unit.position.z})
        state = tick_fold(state, static_cast<uint32_t>(coordinate));
    for (uint16_t angle :
         {unit.heading, static_cast<uint16_t>(unit.pitch), static_cast<uint16_t>(unit.bank)})
        state = tick_fold(state, angle);
    return tick_fold(state, side.movement_speed);
}

void test_folds() {
    CHECK(tick_fold(0, 0) == 0u);
    CHECK(tick_fold(0, 1) == 0x00326020u);
    CHECK(tick_fold(tick_fold(0, 1), 2) == 0xaab0ce29u);
    // 64-bit FNV-1a of the bytes "abcd".
    CHECK(digest_fold(digest_basis, 0x64636261u) == 0xfc179f83ee0724ddull);
}

void test_tick_record() {
    World* w = make_world();
    const Sides sides = make_sides();
    const TickRecord record = sample_tick_record(*w, sides.slot, 0x1234u);
    CHECK(record.tick == 42u);
    CHECK(record.random == 0x1234u);
    // The building has no movement object and the dying unit no live flag.
    CHECK(record.live_units == 2u);
    uint32_t expected = fold_by_fields(0, *w, w->units[mobile_slot], sides.slot[mobile_slot]);
    expected = fold_by_fields(expected, *w, w->units[carried_slot], sides.slot[carried_slot]);
    CHECK(record.unit_fold == expected);

    w->units[carried_slot].attach_piece = 4;
    CHECK(sample_tick_record(*w, sides.slot, 0x1234u).unit_fold != record.unit_fold);
    w->units[carried_slot].attach_piece = 3;
    w->units[building_slot].health = 1;
    CHECK(sample_tick_record(*w, sides.slot, 0x1234u).unit_fold == record.unit_fold);
    w->units[mobile_slot].bank = 0;
    const TickRecord moved = sample_tick_record(*w, sides.slot, 0x1234u);
    CHECK(moved.unit_fold != record.unit_fold);
    CHECK(moved.economy_fold == record.economy_fold);
    w->game.players[1].metal = 999.0F;
    CHECK(sample_tick_record(*w, sides.slot, 0x1234u).economy_fold != record.economy_fold);
    world_destroy(w);
}

void test_digest_stable() {
    World* a = make_world();
    World* b = make_world();
    const Sides sides = make_sides();
    const RandomState random{0x66f043f5u, 7};
    const TickDigest first = tick_digest(*a, sides.slot, random);
    const TickDigest again = tick_digest(*a, sides.slot, random);
    const TickDigest copy = tick_digest(*b, sides.slot, random);
    for (size_t index = 0; index < section_count; ++index) {
        CHECK(first.sections[index].value == again.sections[index].value);
        CHECK(first.sections[index].value == copy.sections[index].value);
        CHECK(first.sections[index].items == copy.sections[index].items);
    }
    CHECK(first.tick == 42u);
    CHECK(first.sections[static_cast<size_t>(Section::units)].items == 4u);
    CHECK(first.sections[static_cast<size_t>(Section::projectiles)].items == 1u);
    CHECK(first.sections[static_cast<size_t>(Section::players)].items == 2u);
    world_destroy(a);
    world_destroy(b);
}

void test_extended_projectile_digest() {
    World* world = make_world();
    constexpr uint32_t capacity = OA_PROJECTILE_CAPACITY + 1;
    std::free(world->projectiles);
    world->projectiles = static_cast<Projectile*>(std::calloc(capacity, sizeof(Projectile)));
    if (world->projectiles == nullptr)
        std::abort();
    world->projectile_capacity = capacity;
    world->game.projectile_count = static_cast<int32_t>(capacity);
    const Sides sides = make_sides();
    const auto section = static_cast<size_t>(Section::projectiles);
    const TickDigest before = tick_digest(*world, sides.slot, {});
    CHECK(before.sections[section].items == capacity);
    world->projectiles[capacity - 1].position.x = 1;
    const TickDigest after = tick_digest(*world, sides.slot, {});
    CHECK(before.sections[section].value != after.sections[section].value);
    world->game.projectile_count += 1;
    CHECK(
        tick_digest(*world, sides.slot, {}).sections[section].value == after.sections[section].value
    );
    world->projectile_capacity = 0;
    CHECK(tick_digest(*world, sides.slot, {}).sections[section].items == OA_PROJECTILE_CAPACITY);
    world_destroy(world);
}

// Changes one field and reports which sections moved.
template <typename Change>
uint32_t moved_sections(Change change, RandomState random = {1, 2}) {
    World* w = make_world();
    Sides sides = make_sides();
    const TickDigest before = tick_digest(*w, sides.slot, {1, 2});
    change(*w, sides);
    const TickDigest after = tick_digest(*w, sides.slot, random);
    uint32_t moved = 0;
    for (size_t index = 0; index < section_count; ++index)
        if (before.sections[index].value != after.sections[index].value)
            moved |= 1u << index;
    world_destroy(w);
    return moved;
}

constexpr uint32_t bit(Section section) {
    return 1u << static_cast<uint32_t>(section);
}

constexpr uint32_t total_and(Section section) {
    return bit(Section::total) | bit(section);
}

void test_digest_sensitive() {
    CHECK(moved_sections([](World& w, Sides&) {
              w.units[building_slot].health -= 1;
          }) == total_and(Section::units));
    CHECK(moved_sections([](World& w, Sides&) {
              w.units[mobile_slot].position.y += 1;
          }) == total_and(Section::units));
    CHECK(moved_sections([](World& w, Sides&) {
              w.units[dying_slot].flags |= OA_UNIT_FLAG_LIVE;
          }) == total_and(Section::units));
    CHECK(moved_sections([](World& w, Sides&) {
              w.units[carried_slot].weapons[2].target_a = 9;
          }) == total_and(Section::weapons));
    CHECK(moved_sections([](World&, Sides& s) {
              s.slot[mobile_slot].order_kind = 7;
          }) == total_and(Section::orders));
    CHECK(moved_sections([](World& w, Sides&) {
              w.projectiles[0].velocity.x = 1;
          }) == total_and(Section::projectiles));
    CHECK(moved_sections([](World& w, Sides&) {
              w.game.players[0].metal = 1.0F;
          }) == total_and(Section::players));
    CHECK(moved_sections([](World&, Sides&) {}, {1, 3}) == total_and(Section::random));
    CHECK(moved_sections([](World& w, Sides&) { ++w.game.tick; }) == bit(Section::total));
    // A projectile past the live count is not state.
    CHECK(moved_sections([](World& w, Sides&) { w.projectiles[1].def = 2; }) == 0u);
    // Slot order counts: the same unit in another slot is another state.
    CHECK(
        moved_sections([](World& w, Sides&) {
            w.units[5] = w.units[building_slot];
            w.units[building_slot] = Unit{};
        }) ==
        (bit(Section::total) | bit(Section::units) | bit(Section::weapons) | bit(Section::orders))
    );
}

void test_format_unit() {
    World* w = make_world();
    const Sides sides = make_sides();
    char line[1024];
    const size_t length = format_unit(*w, carried_slot, sides.slot, 42, line, sizeof line);
    CHECK(length < sizeof line);
    CHECK(length == std::strlen(line));
    const char* prefix =
        "tick=42 slot=2 type=1 owner=0 flags=0x10000001 state=0x00000005 health=902 ";
    CHECK(std::strncmp(line, prefix, std::strlen(prefix)) == 0);
    CHECK(
        std::strstr(line, " parent=1 piece=3 movement=1 speed=0x00018002 order=0 order_kind=0 ") !=
        nullptr
    );
    CHECK(std::strstr(line, " w0_target_a=1 w0_target_b=-32768 w0_reload=0 ") != nullptr);
    CHECK(std::strstr(line, " w1_reload=12 ") != nullptr);
    CHECK(line[length - 1] == '\n');
    char small[16];
    CHECK(format_unit(*w, carried_slot, sides.slot, 42, small, sizeof small) == length);
    CHECK(std::strlen(small) == sizeof small - 1);
    for (size_t index = 0; index < unit_field_count; ++index) {
        const auto field = static_cast<UnitField>(index);
        CHECK(std::strcmp(unit_field_name(field), "unknown") != 0);
    }
    CHECK(unit_field_section(UnitField::order_kind) == Section::orders);
    CHECK(unit_field_section(UnitField::w2_stockpile) == Section::weapons);
    CHECK(unit_field_section(UnitField::speed) == Section::units);
    world_destroy(w);
}

void test_encoding() {
    uint8_t header[header_size];
    encode_header(header, 1234567);
    const uint8_t expected_header[header_size] = {
        'O', 'A', 'T', '1', 1, 0, 0, 0, 24, 0, 0, 0, 0x87, 0xd6, 0x12, 0
    };
    CHECK(std::memcmp(header, expected_header, header_size) == 0);
    uint8_t record[record_size];
    encode_tick_record(record, {1, 2, 3201356665u, 1495336599u, 721328935u});
    const uint8_t expected_record[record_size] = {1,    0,    0,    0,    1,    0,    0,    0,
                                                  2,    0,    0,    0,    0x79, 0xd3, 0xd0, 0xbe,
                                                  0x97, 0x06, 0x21, 0x59, 0x27, 0x9b, 0xfe, 0x2a};
    CHECK(std::memcmp(record, expected_record, record_size) == 0);
    encode_section_record(record, 9, Section::players, {0x0102030405060708ull, 3});
    const uint8_t expected_section[record_size] = {0x10, 0, 0, 0, 9, 0, 0, 0, 5, 0, 0, 0,
                                                   8,    7, 6, 5, 4, 3, 2, 1, 3, 0, 0, 0};
    CHECK(std::memcmp(record, expected_section, record_size) == 0);
}

} // namespace

int main() {
    test_folds();
    test_tick_record();
    test_digest_stable();
    test_extended_projectile_digest();
    test_digest_sensitive();
    test_format_unit();
    test_encoding();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("sim-trace: ok");
    return 0;
}
