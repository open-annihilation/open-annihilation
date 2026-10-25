// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The shot planner on timelines built here: a map with no unit, commanders
// taking turns of varied lengths with wide drifts between them, a fight
// arrived at early and faded into, big moments interrupting with a
// checkerboard, the deciding commander death faded into, held and pulled
// back from, games without one (a replaced commander, a failed replay, a
// loss that leaves two sides) directed to their end, a commander replaced
// mid-game shown no more than before and one displaced by another unit
// lost, pans and cuts in, cloaked commanders skipped, one player's
// commander and base in turn, the switch ratio, long-range fire, armies
// with lead room, the quiet turns' sides, kinds, lengths, factories and
// memory, armies that must advance, framing around the heaviest damage and
// away from water, a battle held, a gun's launch and landing, the sea, the
// watchability rules on every plan (follow shots move a bounded step
// among them), every plan decoding and compiling cleanly, a seeded sweep of
// random timelines, and a pinned SHA-256 of one plan's YAML.

#include "oa/base/sha256.hpp"
#include "oa/core/weapon_def.h"
#include "oa/formats/oascript.hpp"
#include "oa/media/director.hpp"
#include "oa/media/director/planner.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <random>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace director = oa::media::director;
namespace oascript = oa::formats::oascript;

namespace {

int failures = 0;

/// Records a failed expectation with its line.
void expect(
    bool value,
    std::string_view what,
    const std::source_location where = std::source_location::current()
) {
    if (value)
        return;
    ++failures;
    std::cerr << where.file_name() << ':' << where.line() << ": " << what << '\n';
}

/// Unit types.
enum : uint16_t {
    commander_type = 1,
    tank_type,
    solar_type,
    gun_type,
    launcher_type,
    factory_type,
    constructor_type,
};

/// Weapons.
enum : uint8_t { laser = 1, cannon, blast, bertha, missile };

/// Death kinds (TimelineEvent::damage_kind of a death): by a weapon, and
/// the unit in a slot a new unit takes.
constexpr uint8_t weapon_death = 1;
constexpr uint8_t replaced_death = 0;
/// How long before and after a replaced commander the camera must not cut
/// to its place, ticks.
constexpr uint32_t replaced_watch_ticks = 150;

/// The player the replays are watched from.
constexpr uint8_t viewer = 9;
/// A slot counter's start for the units a fixture adds in bulk.
constexpr uint16_t bulk_slots = 100;

/// A point on the map: a column and a row.
struct Spot {
    int32_t x{};
    int32_t z{};
};

/// Builds a timeline.
class Builder {
  public:

    /// Starts a timeline of a map, from tick 0 to `end`.
    Builder(int32_t width, int32_t height, uint32_t end) {
        director::TimelineHeader& header{timeline_.header};
        header.map_name = "Test";
        header.map_width = width;
        header.map_height = height;
        header.first_tick = 0;
        header.last_tick = end - 1;
        header.viewer_player = viewer;
        timeline_.usable_end_tick = end;
        timeline_.verdict.finished = true;
        timeline_.verdict.clean = true;
        director::TimelinePlayer watching{};
        watching.index = viewer;
        watching.name = "Watcher";
        watching.watcher = true;
        watching.viewer = true;
        header.players.push_back(watching);
        const auto type{[&](uint16_t index,
                            bool mobile,
                            bool builder,
                            uint32_t metal,
                            uint32_t health,
                            uint16_t weapon) {
            director::TimelineUnitType unit{};
            unit.type_index = index;
            unit.unit_name = "TYPE" + std::to_string(index);
            unit.mobile = mobile;
            unit.builder = builder;
            unit.metal_cost = metal;
            unit.max_damage = health;
            unit.build_distance = builder ? 120 : 0;
            unit.weapons = {weapon, 0, 0};
            header.unit_types.push_back(unit);
        }};
        type(commander_type, true, true, 2500, 3000, laser);
        type(tank_type, true, false, 100, 600, cannon);
        type(solar_type, false, false, 50, 500, 0);
        type(gun_type, false, false, 3000, 4000, bertha);
        type(launcher_type, false, false, 2000, 3000, missile);
        type(factory_type, false, true, 600, 2500, 0);
        type(constructor_type, true, true, 180, 800, 0);
        const auto weapon{[&](uint8_t id,
                              int32_t range,
                              uint32_t flags,
                              int32_t area,
                              int32_t damage,
                              int32_t shake) {
            director::TimelineWeapon record{};
            record.weapon_id = id;
            record.name = "WEAPON" + std::to_string(id);
            record.range = range;
            record.flags = flags;
            record.area_of_effect = area;
            record.damage = damage;
            record.shake_magnitude = shake;
            header.weapons.push_back(record);
        }};
        weapon(laser, 300, OA_WEAPON_FLAG_LINE_OF_SIGHT, 8, 50, 0);
        weapon(cannon, 400, OA_WEAPON_FLAG_BALLISTIC, 32, 100, 0);
        weapon(blast, 0, 0, 400, 20, 10);
        weapon(bertha, 4000, OA_WEAPON_FLAG_BALLISTIC, 80, 2000, 0);
        weapon(missile, 32000, OA_WEAPON_FLAG_VLAUNCH | OA_WEAPON_FLAG_STOCKPILE, 512, 5500, 24);
    }

    /// Adds a weapon the timeline names, after the ones the constructor listed.
    void add_weapon(
        uint16_t id, int32_t range, uint32_t flags, int32_t area, int32_t damage, int32_t shake
    ) {
        director::TimelineWeapon record{};
        record.weapon_id = id;
        record.name = "WEAPON" + std::to_string(id);
        record.range = range;
        record.flags = flags;
        record.area_of_effect = area;
        record.damage = damage;
        record.shake_magnitude = shake;
        timeline_.header.weapons.push_back(record);
    }

    /// Adds a player that fights, with its start and allies.
    void player(uint8_t index, Spot start, uint16_t allies = 0) {
        director::TimelinePlayer player{};
        player.index = index;
        player.name = "P" + std::to_string(index);
        player.owned_units = true;
        player.start = director::WorldPoint{start.x, 0, start.z};
        player.allies = allies;
        auto& players{timeline_.header.players};
        players.insert(
            std::lower_bound(
                players.begin(),
                players.end(),
                index,
                [](const auto& p, uint8_t i) { return p.index < i; }
            ),
            player
        );
    }

    /// Adds a unit created on a tick at a place; a mobile one is sampled
    /// there every 15 ticks until `until` with `flags`.
    void unit(
        uint16_t slot,
        uint16_t type,
        uint8_t owner,
        uint32_t born,
        Spot at,
        bool commander = false,
        uint32_t until = 0,
        uint8_t flags = director::sample_flag::visible
    ) {
        director::TimelineEvent event{};
        event.tick = born;
        event.kind = director::EventKind::created;
        event.unit = slot;
        event.owner = owner;
        event.unit_type = type;
        event.at = director::WorldPoint{at.x, 0, at.z};
        if (commander)
            event.flags = director::event_flag::commander;
        timeline_.events.push_back(event);
        const bool mobile{type == commander_type || type == tank_type || type == constructor_type};
        if (!mobile)
            return;
        const uint32_t end{until != 0 ? until : timeline_.usable_end_tick};
        for (uint32_t tick{(born / 15 + 1) * 15}; tick < end; tick += 15)
            sample(slot, type, owner, tick, at, flags);
    }

    /// Adds a sample of a unit.
    void
    sample(uint16_t slot, uint16_t type, uint8_t owner, uint32_t tick, Spot at, uint8_t flags) {
        director::UnitSample sample{};
        sample.tick = tick;
        sample.unit = slot;
        sample.unit_type = type;
        sample.owner = owner;
        sample.flags = flags;
        sample.health = 100;
        sample.at = director::WorldPoint{at.x, 0, at.z};
        timeline_.samples.push_back(sample);
    }

    /// Adds a unit finished by a builder.
    void finished(
        uint32_t tick, uint16_t slot, uint8_t owner, uint16_t type, uint16_t builder, Spot at
    ) {
        director::TimelineEvent event{};
        event.tick = tick;
        event.kind = director::EventKind::finished;
        event.unit = slot;
        event.owner = owner;
        event.unit_type = type;
        event.other_unit = builder;
        event.at = director::WorldPoint{at.x, 0, at.z};
        timeline_.events.push_back(event);
    }

    /// Sets the water map: squares of water_square_pixels, row by row.
    void water(int32_t columns, int32_t rows, std::vector<uint8_t> percents) {
        director::TimelineHeader& header{timeline_.header};
        header.water_columns = columns;
        header.water_rows = rows;
        header.water = std::move(percents);
    }

    /// Adds damage to a unit.
    void damage(
        uint32_t tick,
        uint16_t target,
        uint8_t owner,
        uint16_t type,
        uint16_t attacker,
        uint8_t attacker_owner,
        int32_t amount,
        Spot at
    ) {
        director::TimelineEvent event{};
        event.tick = tick;
        event.kind = director::EventKind::damage;
        event.unit = target;
        event.owner = owner;
        event.unit_type = type;
        event.other_unit = attacker;
        event.other_owner = attacker_owner;
        event.amount = amount;
        event.damage_kind = 1;
        event.at = director::WorldPoint{at.x, 0, at.z};
        timeline_.events.push_back(event);
    }

    /// Adds a death, by a weapon unless `kind` names another death kind.
    void death(
        uint32_t tick,
        uint16_t slot,
        uint8_t owner,
        uint16_t type,
        Spot at,
        bool commander,
        uint8_t kind = weapon_death
    ) {
        director::TimelineEvent event{};
        event.tick = tick;
        event.kind = director::EventKind::death;
        event.unit = slot;
        event.owner = owner;
        event.unit_type = type;
        event.damage_kind = kind;
        if (commander)
            event.flags = director::event_flag::commander;
        event.at = director::WorldPoint{at.x, 0, at.z};
        timeline_.events.push_back(event);
    }

    /// Adds a shot.
    void shot(uint32_t tick, uint16_t shooter, uint8_t owner, uint16_t weapon, Spot from) {
        director::TimelineEvent event{};
        event.tick = tick;
        event.kind = director::EventKind::shot;
        event.unit = shooter;
        event.owner = owner;
        event.weapon = weapon;
        event.at = director::WorldPoint{from.x, 20, from.z};
        timeline_.events.push_back(event);
    }

    /// Adds a detonation.
    void detonation(uint32_t tick, uint16_t shooter, uint8_t owner, uint16_t weapon, Spot at) {
        director::TimelineEvent event{};
        event.tick = tick;
        event.kind = director::EventKind::detonation;
        event.unit = shooter;
        event.owner = owner;
        event.weapon = weapon;
        event.at = director::WorldPoint{at.x, 0, at.z};
        timeline_.events.push_back(event);
    }

    /// Returns the timeline with its events and samples in order.
    director::Timeline done() const {
        director::Timeline timeline{timeline_};
        std::stable_sort(
            timeline.events.begin(), timeline.events.end(), [](const auto& a, const auto& b) {
                return a.tick < b.tick;
            }
        );
        std::stable_sort(
            timeline.samples.begin(), timeline.samples.end(), [](const auto& a, const auto& b) {
                return a.tick != b.tick ? a.tick < b.tick : a.unit < b.unit;
            }
        );
        return timeline;
    }

  private:

    director::Timeline timeline_{};
};

/// The settings every plan here uses.
director::PlannerSettings settings() {
    director::PlannerSettings value{};
    value.recording = "game.rec";
    return value;
}

/// One note of a plan: its tick and its subject.
struct Switch {
    uint32_t tick{};
    std::string key{};
};

/// Reads a plan's notes.
std::vector<Switch> switches_of(const director::Plan& plan) {
    std::vector<Switch> result{};
    for (const std::string& note : plan.notes) {
        const size_t colon{note.find(": ")};
        const size_t score{note.find(" score ")};
        if (note.rfind("tick ", 0) != 0 || colon == std::string::npos ||
            score == std::string::npos) {
            expect(false, "a note reads \"tick N: KEY score S\"");
            continue;
        }
        result.push_back(
            Switch{
                static_cast<uint32_t>(std::stoul(note.substr(5, colon - 5))),
                note.substr(colon + 2, score - colon - 2),
            }
        );
    }
    return result;
}

/// Returns a decimal's value in tenths.
int64_t tenths(oascript::Decimal value) {
    return value.places == 0 ? value.mantissa * 10 : value.mantissa;
}

/// Returns the shot that starts on a tick.
const oascript::Shot* shot_at(const director::Plan& plan, uint32_t tick) {
    for (const oascript::Shot& shot : plan.script.director.shots)
        if (shot.tick == tick)
            return &shot;
    return nullptr;
}

/// Tells whether a 1920 by 1080 camera holds a point in its middle
/// safe_area_percent.
bool holds(const oascript::CameraState& camera, Spot at) {
    const int64_t x{tenths(camera.position.x)};
    const int64_t z{tenths(camera.position.z)};
    const int64_t height{tenths(camera.position.y)};
    const int64_t across{std::abs(int64_t{at.x} * 10 - x)};
    const int64_t down{std::abs(int64_t{at.z} * 10 - z)};
    return across * 200 * 1080 <= height * 1920 * director::safe_area_percent &&
           down * 200 <= height * director::safe_area_percent;
}

/// Tells whether a transition is one the planner makes: a half-second
/// dissolve or checkerboard, a wipe of 0.6 seconds or a fade of a second.
bool transition_known(const oascript::Transition& transition) {
    const int64_t length{tenths(transition.duration)};
    switch (transition.kind) {
    case oascript::TransitionKind::dissolve:
    case oascript::TransitionKind::checkerboard:
        return length == 5;
    case oascript::TransitionKind::wipe:
        return length == 6;
    case oascript::TransitionKind::fade:
        return length == 10;
    }
    return false;
}

/// Checks what every plan must satisfy: it decodes back to itself in both
/// forms, compiles for its map without an error or a warning, is the same
/// when planned again, never takes a subject twice in a row, spaces its
/// cuts, changes zoom within the limit between follow shots, and moves a
/// follow shot at most a quarter of the view and a fifth of its height.
void check_plan(
    const director::Timeline& timeline,
    const director::Plan& plan,
    std::string_view name,
    const director::PlannerSettings& used = settings()
) {
    const int failures_before{failures};
    const oascript::Script& script{plan.script};
    for (const oascript::DocumentForm form :
         {oascript::DocumentForm::yaml, oascript::DocumentForm::json}) {
        const std::string text{oascript::write_script(script, form)};
        oascript::Script decoded{};
        oascript::DecodeReport report{};
        const bool read{oascript::read_script(
            std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(text.data()), text.size()},
            decoded,
            report
        )};
        expect(read && report.errors.empty() && report.warnings.empty(), name);
        if (read)
            expect(oascript::write_script(decoded, form) == text, name);
    }
    director::ShotList compiled{};
    director::CompileMessages messages{};
    const bool compiles{director::compile_shots(
        script,
        director::MapBounds{timeline.header.map_width, timeline.header.map_height},
        timeline.usable_end_tick,
        compiled,
        messages
    )};
    expect(compiles, name);
    expect(messages.errors.empty() && messages.warnings.empty(), name);
    for (const std::string& message : messages.errors)
        std::cerr << name << ": error: " << message << '\n';
    for (const std::string& message : messages.warnings)
        std::cerr << name << ": warning: " << message << '\n';
    const director::Plan again{director::plan_script(timeline, used)};
    expect(
        oascript::write_script(again.script, oascript::DocumentForm::yaml) ==
            oascript::write_script(script, oascript::DocumentForm::yaml),
        name
    );
    expect(again.notes == plan.notes, name);
    expect(script.director.end_tick.has_value(), name);
    expect(!script.director.shots.empty() && script.director.shots[0].camera_start, name);
    expect(
        !script.director.shots.empty() &&
            script.director.shots[0].tick == timeline.header.first_tick,
        name
    );
    const std::vector<Switch> switches{switches_of(plan)};
    for (size_t index{1}; index < switches.size(); ++index) {
        expect(switches[index].key != switches[index - 1].key, name);
        expect(switches[index].tick > switches[index - 1].tick, name);
        // The ending cuts whenever the deciding death needs it.
        if (switches[index].key.rfind("ending:", 0) == 0)
            continue;
        const bool interrupting{switches[index].key.rfind("moment:", 0) == 0};
        const uint32_t gap{switches[index].tick - switches[index - 1].tick};
        expect(
            gap >= (interrupting ? director::min_interrupt_ticks : director::min_cut_ticks), name
        );
    }
    // With more than a decision period of ticks a frame, shots merge and the
    // zoom limit no longer holds between them.
    const auto& shots{script.director.shots};
    const auto scale{[](uint32_t places) {
        int64_t value{1};
        for (uint32_t place{}; place < places; ++place)
            value *= 10;
        return value;
    }};
    const bool per_frame_ok{
        used.tickrate.mantissa * scale(used.framerate.places) <=
        int64_t{director::decision_period_ticks} * used.framerate.mantissa *
            scale(used.tickrate.places)
    };
    std::vector<uint32_t> switch_ticks{};
    for (const Switch& entry : switches)
        switch_ticks.push_back(entry.tick);
    for (size_t index{1}; index < shots.size() && per_frame_ok; ++index) {
        // A follow shot: it continues the shot before, between switches.
        const bool at_switch{
            std::find(switch_ticks.begin(), switch_ticks.end(), shots[index].tick) !=
            switch_ticks.end()
        };
        const bool last{index + 1 == shots.size()};
        if (!shots[index].camera_start && !at_switch && !last) {
            const oascript::CameraState& before{shots[index - 1].camera_end};
            const oascript::CameraState& after{shots[index].camera_end};
            const int64_t height{tenths(before.position.y)};
            const int64_t width{height * used.width / used.height};
            // A quarter of the view, and a pixel for the view kept on the map.
            expect(
                std::abs(tenths(after.position.x) - tenths(before.position.x)) * 4 <= width + 40,
                name
            );
            expect(
                std::abs(tenths(after.position.z) - tenths(before.position.z)) * 4 <= height + 40,
                name
            );
            expect(
                tenths(after.position.y) * 100 <= height * 120 + 1000 &&
                    height * 100 <= tenths(after.position.y) * 120 + 1000,
                name
            );
        }
        if (shots[index].camera_start) {
            const int64_t start{tenths(shots[index].camera_start->position.y)};
            const int64_t end{tenths(shots[index].camera_end.position.y)};
            expect(end <= start * 2 && start <= end * 2, name);
            expect(!shots[index].transition || transition_known(*shots[index].transition), name);
            continue;
        }
        const int64_t before{tenths(shots[index - 1].camera_end.position.y)};
        const int64_t after{tenths(shots[index].camera_end.position.y)};
        const bool ending_pull{
            index + 1 == shots.size() && !plan.notes.empty() &&
            plan.notes.back().find("ending:") != std::string::npos
        };
        if (!ending_pull)
            expect(after <= before * 2 && before <= after * 2, name);
    }
    if (failures != failures_before)
        for (const std::string& line : plan.notes)
            std::cerr << "  " << line << '\n';
}

/// Plans a timeline and checks the plan.
director::Plan planned(
    const director::Timeline& timeline,
    std::string_view name,
    const director::PlannerSettings& used = settings()
) {
    const director::Plan plan{director::plan_script(timeline, used)};
    check_plan(timeline, plan, name, used);
    return plan;
}

// (a) A timeline with no unit: one whole-map shot.
void test_no_units() {
    const director::Timeline timeline{Builder{3328, 1888, 1800}.done()};
    const director::Plan plan{planned(timeline, "no units")};
    const auto& shots{plan.script.director.shots};
    expect(shots.size() == 1, "one shot");
    if (shots.size() == 1) {
        const oascript::Shot& shot{shots[0]};
        expect(shot.tick == 0 && shot.camera_start.has_value(), "from the first tick");
        expect(
            shot.camera_end.position.x.mantissa == 1664 && shot.camera_end.position.x.places == 0,
            "x"
        );
        expect(shot.camera_end.position.z.mantissa == 944, "z");
        expect(shot.camera_end.position.y.mantissa == 1872, "the whole map's height");
        expect(shot.motion.kind == oascript::Motion::linear && !shot.transition, "still");
        expect(shot.camera_start->position.y.mantissa == 1872, "starts on the whole map too");
    }
    expect(plan.script.director.end_tick == 1800u, "ends at the usable end");
    expect(plan.notes == std::vector<std::string>{"tick 0: overview score 0"}, "one note");
    expect(plan.script.input.recording == "game.rec", "the recording");
    expect(plan.script.output.width == 1920 && plan.script.output.height == 1080, "the size");
    expect(!plan.script.output.show_ux, "no interface");
    expect(tenths(plan.script.output.chunking.length) == 600, "one-minute chunks");

    // A map as tall as its view centres on a half pixel, written exactly.
    const director::Timeline tall{Builder{4000, 1001, 900}.done()};
    const director::Plan half{planned(tall, "half pixel")};
    const oascript::Decimal z{half.script.director.shots[0].camera_end.position.z};
    expect(z.mantissa == 5005 && z.places == 1, "a half pixel with one place");

    // A timeline that ends before it starts still gives a valid script.
    Builder empty{2048, 2048, 1};
    director::Timeline cut_short{empty.done()};
    cut_short.usable_end_tick = 0;
    const director::Plan short_plan{director::plan_script(cut_short, settings())};
    expect(short_plan.script.director.end_tick == 1u, "at least one tick");
}

/// Adds two commanders far apart on a 4096 map.
Builder two_commanders(uint32_t end) {
    Builder builder{4096, 4096, end};
    builder.player(0, Spot{600, 600});
    builder.player(1, Spot{3400, 3400});
    builder.unit(1, commander_type, 0, 0, Spot{600, 600}, true);
    builder.unit(2, commander_type, 1, 0, Spot{3400, 3400}, true);
    return builder;
}

/// The shortest quiet turn: 6 seconds, of a subject that starts nothing.
constexpr uint32_t idle_turn = 180;
/// The longest quiet turn: 14 seconds, of one that starts something.
constexpr uint32_t busy_turn_longest = 420;

// (b) Two commanders only: the opening, then a turn each, 6 to 10 seconds
// long as they build nothing, in lengths that vary.
void test_commander_turns() {
    const director::Timeline timeline{two_commanders(3000).done()};
    const director::Plan plan{planned(timeline, "commander turns")};
    const std::vector<Switch> switches{switches_of(plan)};
    // The commanders take turns, and now and then the camera drifts wide
    // from one to the other on the way.
    expect(switches.size() >= 12, "the opening and a turn every 6 to 10 seconds");
    std::string before{};
    size_t drifts{};
    std::vector<uint32_t> lengths{};
    for (size_t index{}; index < switches.size(); ++index) {
        if (index == 1)
            expect(switches[index].tick == director::turn_ticks, "the opening's 10 seconds");
        if (index > 1) {
            const uint32_t length{switches[index].tick - switches[index - 1].tick};
            expect(length >= idle_turn && length <= idle_turn + 120, "6 to 10 seconds");
            lengths.push_back(length);
        }
        if (switches[index].key == "overview") {
            ++drifts;
            expect(index > 0 && switches[index - 1].key != "overview", "one drift at a time");
            continue;
        }
        expect(
            switches[index].key == "commander:0" || switches[index].key == "commander:1",
            "commanders"
        );
        expect(switches[index].key != before, "in turn");
        before = switches[index].key;
    }
    expect(drifts >= 1 && drifts <= 3, "a wide drift or a few");
    std::sort(lengths.begin(), lengths.end());
    expect(std::unique(lengths.begin(), lengths.end()) - lengths.begin() >= 4, "the lengths vary");
    const auto& shots{plan.script.director.shots};
    if (shots.empty())
        return;
    const oascript::Shot& opening{shots[0]};
    expect(
        opening.camera_start->position.y.mantissa == 2304, "the opening starts on the whole map"
    );
    expect(
        opening.camera_end.position.x.mantissa == 600 &&
            opening.camera_end.position.z.mantissa == 600,
        "on the first commander"
    );
    expect(opening.camera_end.position.y.mantissa == director::close_view_height, "close");
    expect(opening.motion.kind == oascript::Motion::spring, "a spring");
    expect(
        opening.motion.frequency.mantissa == 25 && opening.motion.frequency.places == 2, "0.25 Hz"
    );
    // The commanders are far apart: each turn is a cut, the first to the
    // other side with a wipe, the others from a commander with a dissolve,
    // as the next wipe may come only wipe_spacing_ticks later, and those
    // from a wide drift, which shows the commander already, pan or cut in
    // with none. A drift runs on from the view before it. Each turn drifts,
    // pushes in or pulls back linearly, at heights that vary.
    std::vector<int64_t> heights{};
    for (size_t index{1}; index < switches.size(); ++index) {
        const oascript::Shot* shot{shot_at(plan, switches[index].tick)};
        expect(shot != nullptr, "a shot");
        if (shot == nullptr)
            continue;
        if (switches[index].key == "overview") {
            expect(
                shot->camera_end.position.y.mantissa == 1400 ||
                    (shot->camera_start && shot->camera_start->position.y.mantissa == 1400),
                "a wide drift"
            );
            continue;
        }
        const bool first{index == 1};
        const bool after_drift{switches[index - 1].key == "overview"};
        expect(shot->camera_start.has_value() || after_drift, "a far turn cuts");
        expect(shot->transition.has_value() != after_drift, "a transition but after a drift");
        if (!shot->camera_start)
            continue;
        if (shot->transition) {
            expect(
                shot->transition->kind ==
                    (first ? oascript::TransitionKind::wipe : oascript::TransitionKind::dissolve),
                first ? "the first crossing wipes" : "later crossings dissolve"
            );
            expect(
                tenths(shot->transition->duration) == (first ? 6 : 5),
                first ? "0.6 seconds" : "half a second"
            );
        }
        expect(shot->motion.kind == oascript::Motion::linear, "linear");
        const oascript::CameraState& start{*shot->camera_start};
        const oascript::CameraState& end{shot->camera_end};
        expect(
            tenths(start.position.x) != tenths(end.position.x) ||
                tenths(start.position.z) != tenths(end.position.z) ||
                tenths(start.position.y) != tenths(end.position.y),
            "the camera moves"
        );
        heights.push_back(std::min(tenths(start.position.y), tenths(end.position.y)) / 10);
    }
    std::sort(heights.begin(), heights.end());
    expect(std::unique(heights.begin(), heights.end()) - heights.begin() >= 3, "three heights");
    expect(plan.script.director.end_tick == 3000u, "to the end");
}

/// The fight of fixture (c): three tanks a side, damage from tick 1500 to 1600.
constexpr uint32_t fight_tick = 1500;

/// Adds fixture (c)'s armies and fight.
void add_fight(
    Builder& builder, Spot centre, uint32_t from, uint32_t to, int32_t amount, uint16_t first_slot
) {
    for (uint16_t index{}; index < 3; ++index) {
        const Spot red{centre.x - 50 + 50 * index, centre.z - 100};
        const Spot blue{centre.x - 50 + 50 * index, centre.z + 100};
        builder.unit(static_cast<uint16_t>(first_slot + index), tank_type, 0, 1000, red);
        builder.unit(static_cast<uint16_t>(first_slot + 10 + index), tank_type, 1, 1000, blue);
    }
    for (uint32_t tick{from}, count{}; tick <= to; tick += 5, ++count) {
        const uint16_t which{static_cast<uint16_t>(count % 3)};
        builder.damage(
            tick,
            static_cast<uint16_t>(first_slot + 10 + which),
            1,
            tank_type,
            static_cast<uint16_t>(first_slot + which),
            0,
            amount,
            Spot{centre.x - 50 + 50 * which, centre.z + 100}
        );
    }
}

/// The timeline of fixtures (b) and (c) together.
director::Timeline commanders_and_fight() {
    Builder builder{two_commanders(3000)};
    add_fight(builder, Spot{2000, 2000}, fight_tick, fight_tick + 100, 60, 10);
    return builder.done();
}

// (c) A fight at tick 1500: a cut 90 ticks before it, framing it.
void test_fight() {
    const director::Timeline timeline{commanders_and_fight()};
    const director::Plan plan{planned(timeline, "fight")};
    const std::vector<Switch> switches{switches_of(plan)};
    const auto hot{std::find_if(switches.begin(), switches.end(), [](const Switch& entry) {
        return entry.key.rfind("hot:", 0) == 0;
    })};
    expect(hot != switches.end(), "a hot spot is taken");
    if (hot == switches.end())
        return;
    expect(hot->tick + director::arrive_early_ticks >= fight_tick - 15, "no earlier than 3.5 s");
    expect(hot->tick + director::arrive_early_ticks <= fight_tick + 15, "no later than 2.5 s");
    expect(hot->tick == fight_tick - director::arrive_early_ticks, "3 seconds early");
    expect(hot->key == "hot:3,3", "the fight's cell");
    // The turns before it stop in time for the cut: 150 ticks of age, 120 of spacing.
    expect(
        hot != switches.begin() && hot->tick - (hot - 1)->tick >= director::min_shot_ticks,
        "spacing"
    );
    const oascript::Shot* shot{shot_at(plan, hot->tick)};
    expect(shot != nullptr && shot->camera_start.has_value(), "a far cut");
    expect(
        shot != nullptr && shot->transition &&
            shot->transition->kind == oascript::TransitionKind::fade &&
            tenths(shot->transition->duration) == 10,
        "the first action fades in"
    );
    if (shot != nullptr && shot->camera_start) {
        for (const Spot spot :
             {Spot{1950, 2100}, Spot{2050, 2100}, Spot{1950, 1900}, Spot{2050, 1900}})
            expect(
                holds(*shot->camera_start, spot) && holds(shot->camera_end, spot),
                "in the safe area"
            );
        expect(
            shot->camera_start->position.y.mantissa <= director::max_subject_view_height, "not wide"
        );
    }
    // After the fight the armies are filmed, never for more than 20 s plus a period.
    for (size_t index{1}; index < switches.size(); ++index)
        expect(
            switches[index].tick - switches[index - 1].tick <= director::max_shot_ticks + 15 + 150,
            "at most 20 s"
        );
}

// (d) A big blast interrupts, at least 60 ticks after the last cut.
void test_big_blast() {
    Builder builder{two_commanders(3000)};
    add_fight(builder, Spot{2000, 2000}, fight_tick, fight_tick + 100, 60, 10);
    builder.detonation(1520, 0, 0, blast, Spot{3400, 600});
    builder.detonation(2600, 0, 0, blast, Spot{600, 3400});
    const director::Timeline timeline{builder.done()};
    const director::Plan plan{planned(timeline, "big blast")};
    const std::vector<Switch> switches{switches_of(plan)};
    const auto moment_at{[&](uint32_t tick) {
        return std::find_if(switches.begin(), switches.end(), [&](const Switch& entry) {
            return entry.key == "moment:" + std::to_string(tick);
        });
    }};
    const auto early{moment_at(1520)};
    expect(early != switches.end(), "the blast during the fight interrupts");
    if (early != switches.end() && early != switches.begin()) {
        expect(
            early->tick - (early - 1)->tick == director::min_interrupt_ticks,
            "60 ticks after the cut"
        );
        expect(early->tick <= 1520, "before the blast");
        const oascript::Shot* shot{shot_at(plan, early->tick)};
        expect(shot != nullptr && shot->camera_start.has_value(), "a cut");
        expect(
            shot != nullptr && shot->transition &&
                shot->transition->kind == oascript::TransitionKind::checkerboard &&
                tenths(shot->transition->duration) == 5,
            "with a checkerboard"
        );
        if (shot != nullptr && shot->camera_start) {
            expect(
                tenths(shot->camera_start->position.y) == tenths(shot->camera_end.position.y),
                "held"
            );
            expect(
                shot->camera_end.position.y.mantissa == 2 * director::close_view_height,
                "twice close"
            );
            expect(holds(shot->camera_end, Spot{3400, 600}), "the blast in view");
        }
    }
    // No turn starts just before the second blast: it cuts 2 seconds ahead.
    const auto late{moment_at(2600)};
    expect(late != switches.end() && late->tick == 2600 - 60, "2 seconds before the blast");
    if (late != switches.end() && late != switches.begin())
        expect(late->tick - (late - 1)->tick >= director::min_shot_ticks, "no short turn");
}

// (e) The deciding commander death: held, then pulled back from; the
// cascade after it is ignored.
void test_ending() {
    Builder builder{8192, 8192, 5000};
    builder.player(0, Spot{1200, 1200});
    builder.player(1, Spot{2800, 2800});
    builder.unit(1, commander_type, 0, 0, Spot{1200, 1200}, true);
    builder.unit(2, commander_type, 1, 0, Spot{2800, 2800}, true, 2400);
    for (uint16_t index{}; index < 10; ++index) {
        const uint16_t slot{static_cast<uint16_t>(bulk_slots + index)};
        builder.unit(slot, solar_type, 1, 100, Spot{2600 + 40 * index, 2700});
        builder.death(2410 + 10 * index, slot, 1, solar_type, Spot{2600 + 40 * index, 2700}, false);
    }
    builder.damage(2300, 2, 1, commander_type, 1, 0, 500, Spot{2800, 2800});
    builder.death(2400, 2, 1, commander_type, Spot{2800, 2800}, true);
    const director::Timeline timeline{builder.done()};
    const director::Plan plan{planned(timeline, "ending")};
    const auto& shots{plan.script.director.shots};
    expect(
        plan.script.director.end_tick ==
            2400u + director::ending_hold_ticks + director::ending_pull_back_ticks,
        "end"
    );
    expect(shots.size() >= 3, "shots");
    if (shots.size() < 3)
        return;
    const oascript::Shot& hold{shots[shots.size() - 2]};
    const oascript::Shot& pull{shots.back()};
    expect(hold.tick == 2400 - 90, "held 3 seconds before the death");
    expect(hold.camera_start.has_value(), "a cut");
    expect(
        hold.transition && hold.transition->kind == oascript::TransitionKind::fade &&
            tenths(hold.transition->duration) == 10,
        "a second's fade"
    );
    expect(
        hold.camera_end.position.x.mantissa == 2800 && hold.camera_end.position.z.mantissa == 2800,
        "on the death"
    );
    expect(
        hold.camera_end.position.y.mantissa == director::close_view_height * 3 / 2,
        "one and a half times close"
    );
    expect(
        hold.camera_start &&
            hold.camera_start->position.y.mantissa == hold.camera_end.position.y.mantissa,
        "still"
    );
    expect(pull.tick == 2400 + director::ending_hold_ticks, "the pull back");
    expect(!pull.camera_start && pull.motion.kind == oascript::Motion::spring, "a spring");
    expect(pull.motion.frequency.mantissa == 2 && pull.motion.frequency.places == 1, "0.2 Hz");
    expect(
        pull.camera_end.position.x.mantissa == 2800 && pull.camera_end.position.z.mantissa == 2800,
        "same centre"
    );
    expect(pull.camera_end.position.y.mantissa == 810 * 5 / 2, "two and a half times higher");
    expect(
        !plan.notes.empty() && plan.notes.back() == "tick 2310: ending:2400 score 25000",
        "the ending's note"
    );
    const std::vector<Switch> switches{switches_of(plan)};
    for (const Switch& entry : switches)
        expect(entry.tick <= 2310, "nothing after the ending");

    // Without a later tick to pull back in, the ending holds to the end.
    Builder short_builder{8192, 8192, 2500};
    short_builder.player(0, Spot{1200, 1200});
    short_builder.player(1, Spot{2800, 2800});
    short_builder.unit(1, commander_type, 0, 0, Spot{1200, 1200}, true);
    short_builder.unit(2, commander_type, 1, 0, Spot{2800, 2800}, true, 2400);
    short_builder.death(2400, 2, 1, commander_type, Spot{2800, 2800}, true);
    const director::Timeline short_timeline{short_builder.done()};
    const director::Plan short_plan{planned(short_timeline, "short ending")};
    expect(short_plan.script.director.end_tick == 2500u, "the usable end");
    expect(short_plan.script.director.shots.back().tick == 2310, "the hold is last");

    // A commander death before the first 90 ticks makes the ending the whole video.
    Builder early_builder{8192, 8192, 1000};
    early_builder.player(0, Spot{1200, 1200});
    early_builder.player(1, Spot{2800, 2800});
    early_builder.unit(1, commander_type, 0, 0, Spot{1200, 1200}, true);
    early_builder.unit(2, commander_type, 1, 0, Spot{2800, 2800}, true, 60);
    early_builder.death(60, 2, 1, commander_type, Spot{2800, 2800}, true);
    const director::Timeline early_timeline{early_builder.done()};
    const director::Plan early_plan{planned(early_timeline, "early ending")};
    expect(early_plan.script.director.shots.size() == 2, "hold and pull back");
    expect(early_plan.script.director.shots[0].tick == 0, "held from the first tick");
}

/// Tells whether a plan ends on a deciding commander death.
bool has_ending(const director::Plan& plan) {
    return std::any_of(plan.notes.begin(), plan.notes.end(), [](const std::string& note) {
        return note.find(": ending:") != std::string::npos;
    });
}

// A game without a deciding commander death is directed to its usable end:
// a commander replaced on the first tick is no loss, deaths after the
// replay's first error lie past the usable end, and a commander death that
// leaves two sides with commanders decides nothing.
void test_no_deciding_death() {
    // Two sides far apart on a large map; the second side's commander is
    // replaced in its slot on the first tick, before or as the first side's
    // commander comes, and dies on tick 5500.
    const auto replaced_commander{[](uint32_t first_side_born) {
        Builder builder{9216, 9216, 6000};
        builder.player(0, Spot{1800, 1000});
        builder.player(1, Spot{7800, 8400});
        builder.unit(1, commander_type, 1, 1, Spot{7800, 8400}, true, 1);
        builder.death(1, 1, 1, commander_type, Spot{7800, 8400}, true, replaced_death);
        builder.unit(1, commander_type, 1, 1, Spot{7800, 8400}, true, 5500);
        builder.unit(1501, commander_type, 0, first_side_born, Spot{1800, 1000}, true);
        builder.death(5500, 1, 1, commander_type, Spot{7800, 8400}, true);
        return builder.done();
    }};
    // The replay fails on tick 5000, before the commander dies.
    director::Timeline failed{replaced_commander(1)};
    failed.verdict.clean = false;
    failed.verdict.errors = 1;
    failed.verdict.first_error_tick = 5000;
    failed.usable_end_tick = 5000;
    const director::Plan plan{planned(failed, "replaced commander")};
    expect(plan.script.director.end_tick == 5000u, "directed to the replay's first error");
    expect(!has_ending(plan), "a replaced commander ends nothing");
    const std::vector<Switch> switches{switches_of(plan)};
    expect(
        !switches.empty() && switches.back().tick + director::max_shot_ticks >= 5000,
        "turns to the end"
    );
    // The first side's commander comes a tick after the replacement, when
    // the second side alone has one; the commander's death on tick 5500
    // decides the game.
    const director::Plan later_plan{planned(replaced_commander(2), "replaced before the other")};
    expect(
        !later_plan.notes.empty() &&
            later_plan.notes.back().find(": ending:5500 ") != std::string::npos,
        "the real loss ends the video"
    );
    expect(
        later_plan.script.director.end_tick ==
            5500u + director::ending_hold_ticks + director::ending_pull_back_ticks,
        "held and pulled back"
    );

    // Three sides: the first commander death leaves two with commanders,
    // the second one side, which decides the game.
    const auto three_sides{[](bool second_loss) {
        Builder sides{4096, 4096, 4000};
        sides.player(0, Spot{600, 600});
        sides.player(1, Spot{3400, 3400});
        sides.player(2, Spot{600, 3400});
        sides.unit(1, commander_type, 0, 0, Spot{600, 600}, true);
        sides.unit(2, commander_type, 1, 0, Spot{3400, 3400}, true, second_loss ? 3000 : 0);
        sides.unit(3, commander_type, 2, 0, Spot{600, 3400}, true, 1500);
        sides.death(1500, 3, 2, commander_type, Spot{600, 3400}, true);
        if (second_loss)
            sides.death(3000, 2, 1, commander_type, Spot{3400, 3400}, true);
        return sides.done();
    }};
    // The second side's commander leaves its slot mid-game on tick 3000,
    // to another commander of its side or to a tank: the camera does not
    // go to the replacement, which ends nothing, and the tank's arrival
    // loses the commander and decides the game.
    const auto mid_game{[](uint16_t successor) {
        Builder builder{4096, 4096, 6000};
        builder.player(0, Spot{600, 600});
        builder.player(1, Spot{3400, 3400});
        builder.unit(1, commander_type, 0, 0, Spot{600, 600}, true);
        builder.unit(2, commander_type, 1, 0, Spot{3400, 3400}, true, 3000);
        builder.death(3000, 2, 1, commander_type, Spot{3400, 3400}, true, replaced_death);
        builder.unit(2, successor, 1, 3000, Spot{3400, 3400}, successor == commander_type);
        return builder.done();
    }};
    const director::Plan replaced{planned(mid_game(commander_type), "replaced mid-game")};
    expect(replaced.script.director.end_tick == 6000u, "a replacement mid-game ends nothing");
    expect(!has_ending(replaced), "a replacement mid-game is no loss");
    for (const Switch& entry : switches_of(replaced)) {
        const bool near{
            entry.tick + replaced_watch_ticks >= 3000u && entry.tick <= 3000u + replaced_watch_ticks
        };
        expect(
            !near || (entry.key.rfind("moment:", 0) != 0 && entry.key.rfind("hot:", 0) != 0),
            "the camera does not go to a replacement"
        );
    }
    const director::Plan displaced{planned(mid_game(tank_type), "displaced by a tank")};
    expect(
        !displaced.notes.empty() &&
            displaced.notes.back().find(": ending:3000 ") != std::string::npos,
        "a commander displaced by another unit is lost"
    );

    const director::Plan undecided{planned(three_sides(false), "undecided")};
    expect(undecided.script.director.end_tick == 4000u, "directed to the recording's end");
    expect(!has_ending(undecided), "the first loss decides nothing");
    const director::Plan decided{planned(three_sides(true), "decided by the second loss")};
    expect(
        decided.script.director.end_tick ==
            3000u + director::ending_hold_ticks + director::ending_pull_back_ticks,
        "the second loss ends the video"
    );
    expect(
        !decided.notes.empty() && decided.notes.back().find(": ending:3000 ") != std::string::npos,
        "the ending is the second loss"
    );
}

// A quiet turn pans with a spring to a subject whose middle the camera
// shows in its safe area, and cuts in without a transition to one whose
// middle it shows further out.
void test_pan() {
    for (const int32_t apart : {150, 400}) {
        Builder builder{4096, 4096, 700};
        builder.player(0, Spot{1000, 1000});
        builder.player(1, Spot{1000 + apart, 1050});
        builder.unit(1, commander_type, 0, 0, Spot{1000, 1000}, true);
        builder.unit(2, commander_type, 1, 0, Spot{1000 + apart, 1050}, true);
        const director::Timeline timeline{builder.done()};
        const director::Plan plan{planned(timeline, "pan")};
        const oascript::Shot* shot{shot_at(plan, director::turn_ticks)};
        expect(shot != nullptr, "a turn");
        if (shot == nullptr)
            continue;
        expect(!shot->transition, "no transition");
        if (apart == 150) {
            expect(!shot->camera_start, "a pan continues the view");
            expect(shot->motion.kind == oascript::Motion::spring, "a spring");
            expect(
                shot->motion.frequency.mantissa == 4 && shot->motion.frequency.places == 1, "0.4 Hz"
            );
            expect(holds(shot->camera_end, Spot{1000 + apart, 1050}), "to the other commander");
        } else {
            expect(shot->camera_start.has_value(), "a cut in");
            expect(
                shot->camera_start && holds(*shot->camera_start, Spot{1000 + apart, 1050}),
                "on the other commander"
            );
        }
    }
}

// A cloaked or undrawn commander is never framed.
void test_hidden_commander() {
    Builder builder{4096, 4096, 2000};
    builder.player(0, Spot{600, 600});
    builder.player(1, Spot{3400, 3400});
    builder.player(2, Spot{600, 3400});
    builder.unit(1, commander_type, 0, 0, Spot{600, 600}, true);
    builder.unit(
        2,
        commander_type,
        1,
        0,
        Spot{3400, 3400},
        true,
        0,
        director::sample_flag::visible | director::sample_flag::cloaked
    );
    builder.unit(3, commander_type, 2, 0, Spot{600, 3400}, true, 0, 0);
    const director::Timeline timeline{builder.done()};
    const director::Plan plan{planned(timeline, "hidden commanders")};
    const std::vector<Switch> switches{switches_of(plan)};
    expect(switches.size() == 1 && switches[0].key == "commander:0", "only the drawn commander");
}

// Watchers and the viewer take no turn.
void test_watchers() {
    Builder builder{two_commanders(1500)};
    director::Timeline timeline{builder.done()};
    for (director::TimelinePlayer& player : timeline.header.players)
        if (player.index == 1)
            player.watcher = true;
    const director::Plan plan{planned(timeline, "watchers")};
    for (const Switch& entry : switches_of(plan))
        expect(entry.key == "commander:0", "a watcher's commander takes no turn");
}

// One player: its commander and its base take turns; its armed units that
// stand still are no army.
void test_single_player() {
    Builder builder{4096, 4096, 1400};
    builder.player(0, Spot{600, 600});
    builder.unit(1, commander_type, 0, 0, Spot{600, 600}, true);
    for (uint16_t index{}; index < 4; ++index)
        builder.unit(
            static_cast<uint16_t>(10 + index), tank_type, 0, 0, Spot{2000 + 60 * index, 2000}
        );
    for (uint16_t index{}; index < 14; ++index)
        builder.unit(
            static_cast<uint16_t>(bulk_slots + index),
            solar_type,
            0,
            100u * index,
            Spot{1500 + 50 * (index % 5), 1500 + 50 * (index / 5)}
        );
    const director::Timeline timeline{builder.done()};
    const director::Plan plan{planned(timeline, "single player")};
    const std::vector<Switch> switches{switches_of(plan)};
    expect(switches.size() == 5, "four turns");
    for (size_t index{}; index < switches.size(); ++index)
        expect(
            switches[index].key == (index % 2 == 0 ? "commander:0" : "base:0"), "commander, base"
        );
}

// A new subject needs one and a half times the current one's score.
void test_switch_ratio() {
    for (const int32_t amount : {78, 120}) {
        Builder builder{two_commanders(3000)};
        add_fight(builder, Spot{1000, 1000}, fight_tick, 2900, 60, 10);
        add_fight(builder, Spot{3000, 3000}, 2000, 2900, amount, 40);
        const director::Timeline timeline{builder.done()};
        const director::Plan plan{planned(timeline, "switch ratio")};
        const std::vector<Switch> switches{switches_of(plan)};
        const auto second{std::find_if(switches.begin(), switches.end(), [](const Switch& entry) {
            return entry.key.rfind("hot:", 0) == 0 && entry.key != "hot:1,1";
        })};
        expect(second != switches.end(), "the second fight is taken");
        if (second == switches.end())
            continue;
        const bool before_it{std::any_of(switches.begin(), second, [](const Switch& entry) {
            return entry.key == "hot:1,1";
        })};
        expect(before_it, "the first fight first");
        if (amount == 78)
            expect(
                second->tick == 1410 + director::max_shot_ticks,
                "1.3 times waits for the longest shot"
            );
        else
            expect(second->tick == 2000 - director::arrive_early_ticks, "twice takes over at once");
    }
}

// Long-range fire: a gun's shell framed from launch to impact when one view
// shows both; a missile across a large map as a launch and an impact.
void test_long_range() {
    Builder builder{two_commanders(3000)};
    builder.unit(50, gun_type, 0, 0, Spot{1000, 2000});
    for (uint16_t index{}; index < 3; ++index)
        builder.unit(
            static_cast<uint16_t>(60 + index), tank_type, 1, 1000, Spot{2950 + 50 * index, 2000}
        );
    for (uint32_t tick{fight_tick}; tick <= fight_tick + 120; tick += 40) {
        builder.shot(tick - 60, 50, 0, bertha, Spot{1000, 2000});
        builder.detonation(tick, 50, 0, bertha, Spot{3000, 2000});
        builder.damage(tick, 61, 1, tank_type, 50, 0, 300, Spot{3000, 2000});
    }
    const director::Timeline timeline{builder.done()};
    const director::Plan plan{planned(timeline, "long range")};
    const std::vector<Switch> switches{switches_of(plan)};
    // The shelling, a hot spot where it lands or the barrage framed wide.
    const auto hot{std::find_if(switches.begin(), switches.end(), [](const Switch& entry) {
        return entry.key.rfind("hot:", 0) == 0 || entry.key == "barrage";
    })};
    expect(hot != switches.end(), "the shelling is taken");
    if (hot != switches.end()) {
        const oascript::Shot* shot{shot_at(plan, hot->tick)};
        expect(shot != nullptr && shot->camera_start.has_value(), "a cut");
        if (shot != nullptr && shot->camera_start) {
            expect(holds(*shot->camera_start, Spot{1000, 2000}), "the gun in view");
            expect(holds(*shot->camera_start, Spot{3000, 2000}), "the impact in view");
            expect(
                shot->camera_start->position.y.mantissa > director::max_subject_view_height, "wide"
            );
            if (hot->key == "barrage")
                expect(hot == switches.begin() + 5, "the first action");
        }
    }

    Builder large{16384, 16384, 3000};
    large.player(0, Spot{1000, 1000});
    large.player(1, Spot{15000, 15000});
    large.unit(1, commander_type, 0, 0, Spot{1000, 1000}, true);
    large.unit(2, commander_type, 1, 0, Spot{15000, 15000}, true);
    large.unit(50, launcher_type, 0, 0, Spot{4000, 1000});
    large.shot(2000, 50, 0, missile, Spot{4000, 1000});
    large.detonation(2300, 50, 0, missile, Spot{15000, 14800});
    const director::Timeline far{large.done()};
    const director::Plan missile_plan{planned(far, "missile")};
    const std::vector<Switch> far_switches{switches_of(missile_plan)};
    const auto has{[&](uint32_t tick, const std::string& key) {
        return std::any_of(far_switches.begin(), far_switches.end(), [&](const Switch& entry) {
            return entry.tick == tick && entry.key == key;
        });
    }};
    expect(has(1940, "moment:2000"), "the launch");
    // The impact is held from 2 to 3 seconds before it lands: its moment,
    // or the hot spot of its blast.
    const auto impact{
        std::find_if(far_switches.begin(), far_switches.end(), [](const Switch& entry) {
            return entry.tick >= 2300 - director::arrive_early_ticks && entry.tick <= 2300 - 60 &&
                   (entry.key == "moment:2300" || entry.key.rfind("hot:", 0) == 0);
        })
    };
    expect(impact != far_switches.end(), "the impact");
    if (impact != far_switches.end()) {
        const oascript::Shot* shot{shot_at(missile_plan, impact->tick)};
        expect(
            shot != nullptr && shot->camera_start && holds(*shot->camera_start, Spot{15000, 14800}),
            "the impact in view"
        );
    }
}

// A long-range shot of a weapon past 255 is matched with its detonation.
void test_wide_weapon_long_range() {
    Builder large{16384, 16384, 3000};
    large.add_weapon(300, 32000, OA_WEAPON_FLAG_VLAUNCH | OA_WEAPON_FLAG_STOCKPILE, 512, 5500, 24);
    large.player(0, Spot{1000, 1000});
    large.player(1, Spot{15000, 15000});
    large.unit(1, commander_type, 0, 0, Spot{1000, 1000}, true);
    large.unit(2, commander_type, 1, 0, Spot{15000, 15000}, true);
    large.unit(50, launcher_type, 0, 0, Spot{4000, 1000});
    large.shot(2000, 50, 0, 300, Spot{4000, 1000});
    large.detonation(2300, 50, 0, 300, Spot{15000, 14800});
    const director::Timeline far{large.done()};
    const director::Plan plan{planned(far, "wide weapon")};
    const std::vector<Switch> switches{switches_of(plan)};
    const auto has{[&](uint32_t tick, const std::string& key) {
        return std::any_of(switches.begin(), switches.end(), [&](const Switch& entry) {
            return entry.tick == tick && entry.key == key;
        });
    }};
    expect(has(1940, "moment:2000"), "the launch of weapon 300");
    const auto impact{std::find_if(switches.begin(), switches.end(), [](const Switch& entry) {
        return entry.tick >= 2300 - director::arrive_early_ticks && entry.tick <= 2300 - 60 &&
               (entry.key == "moment:2300" || entry.key.rfind("hot:", 0) == 0);
    })};
    expect(impact != switches.end(), "the impact of weapon 300");
    if (impact != switches.end()) {
        const oascript::Shot* shot{shot_at(plan, impact->tick)};
        expect(
            shot != nullptr && shot->camera_start && holds(*shot->camera_start, Spot{15000, 14800}),
            "weapon 300's impact in view"
        );
    }
}

// An army advancing on the enemy start is framed with lead room ahead of it.
void test_army_lead() {
    Builder builder{8192, 8192, 2400};
    builder.player(0, Spot{1000, 1000});
    builder.player(1, Spot{7000, 1000});
    builder.unit(1, commander_type, 0, 0, Spot{1000, 1000}, true);
    builder.unit(2, commander_type, 1, 0, Spot{7000, 1000}, true);
    // A skirmish far away engages the game early.
    builder.unit(90, tank_type, 0, 0, Spot{4000, 7000});
    builder.unit(91, tank_type, 1, 0, Spot{4100, 7000});
    builder.damage(400, 91, 1, tank_type, 90, 0, 5, Spot{4100, 7000});
    for (uint16_t index{}; index < 4; ++index)
        for (uint32_t tick{15}; tick < 2400; tick += 15)
            builder.sample(
                static_cast<uint16_t>(10 + index),
                tank_type,
                0,
                tick,
                Spot{static_cast<int32_t>(1500 + tick * 2), 1000 + 60 * index},
                director::sample_flag::visible
            );
    const director::Timeline timeline{builder.done()};
    const director::Plan plan{planned(timeline, "army lead")};
    const std::vector<Switch> switches{switches_of(plan)};
    const auto army{std::find_if(switches.begin(), switches.end(), [](const Switch& entry) {
        return entry.key == "army:0";
    })};
    expect(army != switches.end(), "the advancing army is taken");
    if (army == switches.end())
        return;
    const oascript::Shot* shot{shot_at(plan, army->tick)};
    expect(shot != nullptr, "its shot");
    if (shot == nullptr)
        return;
    const oascript::CameraState& camera{
        shot->camera_start ? *shot->camera_start : shot->camera_end
    };
    const int64_t centre{tenths(camera.position.x) / 10};
    const int64_t group{1500 + int64_t{army->tick} * 2};
    expect(centre > group, "the view leads the army toward the enemy");
    expect(holds(camera, Spot{static_cast<int32_t>(group), 1090}), "the army in the safe area");
}

/// Returns the side a subject's note names: the number after the colon,
/// before any comma; none for a subject of no side.
std::optional<int> side_of(const std::string& key) {
    for (const std::string_view kind : {"commander:", "factory:", "builder:", "base:"})
        if (key.rfind(kind, 0) == 0)
            return std::stoi(key.substr(kind.size()));
    return std::nullopt;
}

/// The timeline of the quiet turns: red's commander, a factory away from it
/// and its base, and blue's commander with its base and a construction unit
/// building away from them.
director::Timeline quiet_sides() {
    Builder builder{two_commanders(3600)};
    // Red's factory finishes a tank at tick 1400, in red's turns, and every
    // 600 ticks after.
    builder.unit(20, factory_type, 0, 0, Spot{1800, 1500});
    for (uint16_t index{}; index < 4; ++index) {
        const uint32_t done{1400u + 600u * index};
        const auto slot{static_cast<uint16_t>(21 + index)};
        builder.unit(slot, tank_type, 0, done - 200, Spot{1800, 1500});
        builder.finished(done, slot, 0, tank_type, 20, Spot{1800, 1500});
    }
    // Red builds solar collectors south of its commander, blue west of its.
    for (uint16_t index{}; index < 14; ++index) {
        const uint32_t born{50u + 240u * index};
        const int32_t across{60 * (index % 4)};
        const int32_t down{60 * (index / 4)};
        builder.unit(
            static_cast<uint16_t>(bulk_slots + index),
            solar_type,
            0,
            born,
            Spot{800 + across, 1000 + down}
        );
        builder.unit(
            static_cast<uint16_t>(bulk_slots + 20 + index),
            solar_type,
            1,
            born,
            Spot{2900 + across, 3300 + down}
        );
    }
    // Blue's construction unit starts solar collectors far from them.
    builder.unit(30, constructor_type, 1, 0, Spot{2200, 2500});
    for (uint16_t index{}; index < 10; ++index)
        builder.unit(
            static_cast<uint16_t>(bulk_slots + 40 + index),
            solar_type,
            1,
            100u + 330u * index,
            Spot{2150 + 50 * (index % 3), 2450 + 50 * (index / 3 % 2)}
        );
    return builder.done();
}

// The quiet turns: each side takes at most turn_side_shots turns in a row,
// each a different kind of subject; a factory finishing a unit, a
// construction unit starting buildings and the bases being built are
// taken as well as the commanders; a factory is pushed in on with a spring;
// and no subject returns two switches after it was left.
void test_quiet_turns() {
    const director::Timeline timeline{quiet_sides()};
    const director::Plan plan{planned(timeline, "quiet turns")};
    const std::vector<Switch> switches{switches_of(plan)};
    const auto taken{[&](std::string_view prefix) {
        return std::find_if(switches.begin(), switches.end(), [&](const Switch& entry) {
            return entry.key.rfind(prefix, 0) == 0;
        });
    }};
    for (const std::string_view prefix :
         {"commander:0", "commander:1", "factory:0,", "builder:1,", "base:0", "base:1"})
        expect(taken(prefix) != switches.end(), prefix);
    size_t in_a_row{};
    std::vector<uint32_t> lengths{};
    for (size_t index{1}; index < switches.size(); ++index) {
        if (index > 1)
            lengths.push_back(switches[index].tick - switches[index - 1].tick);
        const std::optional<int> side{side_of(switches[index].key)};
        const std::optional<int> before{side_of(switches[index - 1].key)};
        in_a_row = side && side == before ? in_a_row + 1 : 1;
        expect(in_a_row <= director::turn_side_shots, "two turns a side");
        if (side && side == before)
            expect(
                switches[index].key.substr(0, switches[index].key.find(':')) !=
                    switches[index - 1].key.substr(0, switches[index - 1].key.find(':')),
                "a different kind"
            );
        if (index >= 2)
            expect(switches[index].key != switches[index - 2].key, "no quick return");
    }
    // A base is held or drifted across, never followed: no shot between its
    // switch and the next.
    for (size_t index{}; index + 1 < switches.size(); ++index) {
        if (switches[index].key.rfind("base:", 0) != 0)
            continue;
        for (const oascript::Shot& shot : plan.script.director.shots)
            expect(
                shot.tick <= switches[index].tick || shot.tick >= switches[index + 1].tick,
                "a base is not followed"
            );
    }
    // A turn lasts 6 to 14 seconds: longer when its subject starts something.
    expect(
        std::all_of(
            lengths.begin(),
            lengths.end(),
            [](uint32_t length) { return length >= idle_turn && length <= busy_turn_longest; }
        ),
        "6 to 14 seconds"
    );
    expect(
        std::any_of(lengths.begin(), lengths.end(), [](uint32_t length) { return length < 300; }),
        "short idle turns"
    );
    expect(
        std::any_of(lengths.begin(), lengths.end(), [](uint32_t length) { return length > 300; }),
        "long busy turns"
    );
    const auto factory{taken("factory:0,")};
    if (factory != switches.end()) {
        const oascript::Shot* shot{shot_at(plan, factory->tick)};
        expect(shot != nullptr && shot->motion.kind == oascript::Motion::spring, "a spring");
        if (shot != nullptr && shot->camera_start) {
            expect(
                tenths(shot->camera_start->position.y) > tenths(shot->camera_end.position.y),
                "pushing in"
            );
            expect(holds(shot->camera_end, Spot{1800, 1500}), "on the factory");
        }
    }
}

// An army advances: armed units that stand still or move away from the
// enemy's start are none; the same units moving toward it are one.
void test_army_advance() {
    for (const int32_t step : {0, -2, 2}) {
        Builder builder{8192, 8192, 2400};
        builder.player(0, Spot{1000, 1000});
        builder.player(1, Spot{7000, 1000});
        builder.unit(1, commander_type, 0, 0, Spot{1000, 1000}, true);
        builder.unit(2, commander_type, 1, 0, Spot{7000, 1000}, true);
        for (uint16_t index{}; index < 4; ++index)
            for (uint32_t tick{15}; tick < 2400; tick += 15)
                builder.sample(
                    static_cast<uint16_t>(10 + index),
                    tank_type,
                    0,
                    tick,
                    Spot{3000 + step * static_cast<int32_t>(tick), 1000 + 60 * index},
                    director::sample_flag::visible
                );
        const director::Timeline timeline{builder.done()};
        const director::Plan plan{planned(timeline, "army advance")};
        const std::vector<Switch> switches{switches_of(plan)};
        const bool army{std::any_of(switches.begin(), switches.end(), [](const Switch& entry) {
            return entry.key == "army:0";
        })};
        expect(army == (step > 0), step > 0 ? "an advance is an army" : "no advance, no army");
    }
}

// A hot spot is framed around where its heaviest damage lands; lighter
// damage and an attacker join it only when one view can hold them all.
// A subject at a shore is framed with the land rather than the water.
void test_framing() {
    Builder builder{two_commanders(3000)};
    // Blue's tank takes heavy damage at (2300, 2100), a cheap solar
    // collector light damage 900 pixels north; red's tanks fire from 300
    // and 950 pixels south.
    builder.unit(40, tank_type, 1, 1000, Spot{2300, 2100});
    builder.unit(41, solar_type, 1, 1000, Spot{2300, 1200});
    builder.unit(42, tank_type, 0, 1000, Spot{2300, 2400});
    builder.unit(43, tank_type, 0, 1000, Spot{2300, 3050});
    for (uint32_t tick{fight_tick}; tick <= fight_tick + 100; tick += 10) {
        builder.damage(tick, 40, 1, tank_type, 42, 0, 60, Spot{2300, 2100});
        builder.damage(tick + 5, 40, 1, tank_type, 43, 0, 60, Spot{2300, 2100});
        builder.damage(tick, 41, 1, solar_type, 42, 0, 5, Spot{2300, 1200});
    }
    const director::Timeline timeline{builder.done()};
    const director::Plan plan{planned(timeline, "framing")};
    const std::vector<Switch> switches{switches_of(plan)};
    const auto hot{std::find_if(switches.begin(), switches.end(), [](const Switch& entry) {
        return entry.key.rfind("hot:", 0) == 0;
    })};
    expect(hot != switches.end(), "the fight is taken");
    if (hot != switches.end()) {
        const oascript::Shot* shot{shot_at(plan, hot->tick)};
        expect(shot != nullptr && shot->camera_start.has_value(), "a cut");
        if (shot != nullptr && shot->camera_start) {
            const oascript::CameraState& camera{*shot->camera_start};
            expect(holds(camera, Spot{2300, 2100}), "the heavy damage");
            expect(holds(camera, Spot{2300, 2400}), "the near attacker");
            expect(!holds(camera, Spot{2300, 3050}), "not the far attacker");
            expect(!holds(camera, Spot{2300, 1200}), "not the light damage");
        }
    }

    // The sea east of x = 768: the opening frames red's commander with the
    // land west of it.
    Builder shore{two_commanders(600)};
    constexpr int32_t squares{4096 / director::water_square_pixels};
    std::vector<uint8_t> water(static_cast<size_t>(squares * squares), 0);
    for (int32_t row{}; row < squares; ++row)
        for (int32_t column{12}; column < squares; ++column)
            water[static_cast<size_t>(row * squares + column)] = 100;
    shore.water(squares, squares, water);
    const director::Timeline wet{shore.done()};
    const director::Plan wet_plan{planned(wet, "shore")};
    const oascript::CameraState& settled{wet_plan.script.director.shots[0].camera_end};
    expect(holds(settled, Spot{600, 600}), "the commander in the safe area");
    expect(tenths(settled.position.x) < 6000, "the view moved to the land");
    const director::Plan dry_plan{planned(two_commanders(600).done(), "dry shore")};
    expect(
        tenths(dry_plan.script.director.shots[0].camera_end.position.x) == 6000,
        "without water, centred"
    );
}

// A battle holds the camera: fire in bursts every 10 seconds keeps it on
// the fight, with no idle commander's turn between the bursts, until the
// fighting stops.
void test_battle_hold() {
    Builder builder{two_commanders(4200)};
    for (uint16_t index{}; index < 3; ++index) {
        builder.unit(
            static_cast<uint16_t>(10 + index), tank_type, 0, 1000, Spot{1950 + 50 * index, 1900}
        );
        builder.unit(
            static_cast<uint16_t>(20 + index), tank_type, 1, 1000, Spot{1950 + 50 * index, 2100}
        );
    }
    constexpr uint32_t first_burst{1500};
    constexpr uint32_t last_burst{3000};
    for (uint32_t burst{first_burst}; burst <= last_burst; burst += 300)
        for (uint32_t tick{burst}; tick < burst + 30; tick += 5) {
            const auto which{static_cast<uint16_t>(tick / 5 % 3)};
            builder.damage(
                tick,
                static_cast<uint16_t>(20 + which),
                1,
                tank_type,
                static_cast<uint16_t>(10 + which),
                0,
                300,
                Spot{1950 + 50 * which, 2100}
            );
        }
    const director::Timeline timeline{builder.done()};
    const director::Plan plan{planned(timeline, "battle hold")};
    const std::vector<Switch> switches{switches_of(plan)};
    const auto hot{std::find_if(switches.begin(), switches.end(), [](const Switch& entry) {
        return entry.key.rfind("hot:", 0) == 0;
    })};
    expect(hot != switches.end(), "the fight is taken");
    if (hot == switches.end())
        return;
    // The fight is held for its bursts, at most 25 seconds a shot.
    for (auto next{hot + 1}; next != switches.end() && next->tick < last_burst; ++next)
        expect(next->key.rfind("commander:", 0) != 0, "no idle commander in the battle");
    expect(
        hot + 1 == switches.end() || (hot + 1)->tick - hot->tick >= 600,
        "the fight held past its first burst"
    );
    // Once the fighting stops the quiet turns come back.
    expect(
        std::any_of(
            switches.begin(),
            switches.end(),
            [](const Switch& entry) {
                return entry.tick > last_burst && entry.key.rfind("commander:", 0) == 0;
            }
        ),
        "commanders after the battle"
    );
}

// A gun shelling a base too far away for one view to show both: its first
// shot is cut to as it fires, then its shell as it lands, both hard cuts;
// the next pair comes no sooner than 30 seconds later.
void test_gun_launch() {
    Builder builder{two_commanders(4000)};
    builder.unit(50, gun_type, 0, 0, Spot{200, 2000});
    for (uint16_t index{}; index < 6; ++index)
        builder.unit(
            static_cast<uint16_t>(bulk_slots + index),
            solar_type,
            1,
            0,
            Spot{3700 + 60 * (index % 3), 1950 + 60 * (index / 3)}
        );
    constexpr uint32_t flight{100};
    for (uint32_t tick{1500}; tick <= 3300; tick += 150) {
        builder.shot(tick, 50, 0, bertha, Spot{200, 2000});
        builder.detonation(tick + flight, 50, 0, bertha, Spot{3760, 2000});
    }
    const director::Timeline timeline{builder.done()};
    const director::Plan plan{planned(timeline, "gun launch")};
    const std::vector<Switch> switches{switches_of(plan)};
    const auto at{[&](const std::string& key) {
        return std::find_if(switches.begin(), switches.end(), [&](const Switch& entry) {
            return entry.key == key;
        });
    }};
    const auto launch{at("moment:1500")};
    const auto landing{at("moment:1600")};
    expect(launch != switches.end() && launch->tick == 1440, "the gun 2 seconds before it fires");
    expect(
        landing != switches.end() && landing->tick == 1540, "the shell 2 seconds before it lands"
    );
    for (const auto& entry : {launch, landing}) {
        if (entry == switches.end())
            continue;
        const oascript::Shot* shot{shot_at(plan, entry->tick)};
        expect(shot != nullptr && shot->camera_start && !shot->transition, "a hard cut");
        if (shot != nullptr && shot->camera_start)
            expect(
                holds(*shot->camera_start, entry == launch ? Spot{200, 2000} : Spot{3760, 2000}),
                "in view"
            );
    }
    // Every shot after the first lands within 30 seconds of the one shown.
    for (uint32_t tick{1650}; tick < 1500 + 900; tick += 150)
        expect(at("moment:" + std::to_string(tick)) == switches.end(), "30 seconds apart");
    expect(at("moment:2400") != switches.end(), "the next launch shown");
}

// Water: a commander under the sea takes no turn, a fight at sea is not
// cut to, a fight ashore is, and a blast at the map's edge is framed close
// with the blast off the middle only as far as the edge makes it.
void test_water() {
    Builder builder{4096, 4096, 3600};
    builder.player(0, Spot{600, 600});
    builder.player(1, Spot{1400, 3400});
    builder.unit(1, commander_type, 0, 0, Spot{600, 600}, true);
    // Blue's commander walks under the sea, east of x = 2048.
    builder.unit(2, commander_type, 1, 0, Spot{3000, 3400}, true);
    for (uint16_t index{}; index < 6; ++index)
        builder.unit(
            static_cast<uint16_t>(bulk_slots + index),
            solar_type,
            1,
            100u * index,
            Spot{1300 + 60 * (index % 3), 3300 + 60 * (index / 3)}
        );
    constexpr int32_t squares{4096 / director::water_square_pixels};
    std::vector<uint8_t> water(static_cast<size_t>(squares * squares), 0);
    for (int32_t row{}; row < squares; ++row)
        for (int32_t column{squares / 2}; column < squares; ++column)
            water[static_cast<size_t>(row * squares + column)] = 100;
    builder.water(squares, squares, water);
    // A fight at sea from tick 1200, then one ashore from tick 2400.
    add_fight(builder, Spot{3200, 1000}, 1200, 1400, 120, 10);
    add_fight(builder, Spot{1000, 2000}, 2400, 2600, 120, 40);
    builder.detonation(3300, 0, 0, blast, Spot{40, 2000});
    const director::Timeline timeline{builder.done()};
    const director::Plan plan{planned(timeline, "water")};
    const std::vector<Switch> switches{switches_of(plan)};
    for (const Switch& entry : switches) {
        expect(entry.key != "commander:1", "no commander under the sea");
        expect(entry.key != "hot:6,2" && entry.key != "hot:6,1", "no fight at sea");
    }
    expect(
        std::any_of(
            switches.begin(),
            switches.end(),
            [](const Switch& entry) {
                return entry.key == "hot:1,3" || entry.key == "hot:2,3" || entry.key == "hot:1,4" ||
                       entry.key == "hot:2,4";
            }
        ),
        "the fight ashore"
    );
    const auto edge{std::find_if(switches.begin(), switches.end(), [](const Switch& entry) {
        return entry.key == "moment:3300";
    })};
    expect(edge != switches.end(), "the blast at the edge");
    if (edge != switches.end()) {
        const oascript::Shot* shot{shot_at(plan, edge->tick)};
        expect(
            shot != nullptr && shot->camera_start &&
                shot->camera_start->position.y.mantissa == director::close_view_height,
            "framed close"
        );
        if (shot != nullptr && shot->camera_start)
            expect(tenths(shot->camera_start->position.z) == 20000, "not moved for water");
    }
}

/// Returns a random timeline: a few players with commanders, tanks wandering,
/// damage, deaths, shots and blasts, some events out of their slot's life.
director::Timeline random_timeline(std::mt19937& random) {
    const auto pick{[&](uint32_t low, uint32_t high) {
        return std::uniform_int_distribution<uint32_t>{low, high}(random);
    }};
    const int32_t width{static_cast<int32_t>(pick(0, 3) == 0 ? pick(16, 600) : pick(1000, 16000))};
    const int32_t height{static_cast<int32_t>(pick(0, 3) == 0 ? pick(16, 600) : pick(1000, 16000))};
    const uint32_t end{pick(1, 6000)};
    Builder builder{width, height, end};
    const uint32_t players{pick(0, 4)};
    const auto spot{[&]() {
        return Spot{
            static_cast<int32_t>(pick(0, static_cast<uint32_t>(width))),
            static_cast<int32_t>(pick(0, static_cast<uint32_t>(height)))
        };
    }};
    for (uint8_t player{}; player < players; ++player) {
        builder.player(player, spot(), static_cast<uint16_t>(pick(0, 1) != 0 ? pick(0, 1023) : 0));
        builder.unit(
            static_cast<uint16_t>(1 + player),
            commander_type,
            player,
            pick(0, end / 4 + 1),
            spot(),
            true,
            0,
            static_cast<uint8_t>(pick(0, 3))
        );
    }
    const uint32_t units{pick(0, 40)};
    for (uint32_t index{}; index < units && players != 0; ++index) {
        const uint8_t owner{static_cast<uint8_t>(pick(0, players - 1))};
        const uint16_t slot{static_cast<uint16_t>(bulk_slots + index)};
        const uint16_t type{static_cast<uint16_t>(pick(tank_type, launcher_type))};
        const uint32_t born{pick(0, end)};
        builder.unit(
            slot,
            type,
            owner,
            born,
            spot(),
            false,
            pick(born, end + 1),
            static_cast<uint8_t>(pick(0, 15))
        );
        if (pick(0, 2) == 0)
            builder.death(pick(born, end), slot, owner, type, spot(), false);
    }
    const uint32_t events{pick(0, 300)};
    for (uint32_t index{}; index < events; ++index) {
        const uint32_t tick{pick(0, end + 50)};
        const uint16_t target{
            static_cast<uint16_t>(pick(0, 1) != 0 ? bulk_slots + pick(0, 45) : pick(0, 6))
        };
        const uint8_t owner{static_cast<uint8_t>(pick(0, 5))};
        switch (pick(0, 4)) {
        case 0:
            builder.damage(
                tick,
                target,
                owner,
                static_cast<uint16_t>(pick(0, 6)),
                static_cast<uint16_t>(pick(0, 140)),
                static_cast<uint8_t>(pick(0, 10)),
                static_cast<int32_t>(pick(0, 3000)) - 100,
                spot()
            );
            break;
        case 1:
            builder.death(
                tick, target, owner, static_cast<uint16_t>(pick(0, 6)), spot(), pick(0, 5) == 0
            );
            break;
        case 2:
            builder.shot(tick, target, owner, static_cast<uint8_t>(pick(0, 6)), spot());
            break;
        default:
            builder.detonation(tick, target, owner, static_cast<uint8_t>(pick(0, 6)), spot());
            break;
        }
    }
    return builder.done();
}

// Random timelines always plan to scripts that decode and compile cleanly,
// at any output size and rates.
void test_random_timelines() {
    std::mt19937 random{20260930u};
    constexpr std::array<oascript::Decimal, 9> framerates{{
        {1, 0},
        {5, 0},
        {24, 0},
        {23976, 3},
        {2997, 2},
        {30, 0},
        {50, 0},
        {60, 0},
        {240, 0},
    }};
    constexpr std::array<oascript::Decimal, 5> tickrates{
        {{15, 0}, {30, 0}, {45, 0}, {60, 0}, {3000, 0}}
    };
    for (int round{}; round < 300; ++round) {
        const director::Timeline timeline{random_timeline(random)};
        director::PlannerSettings used{settings()};
        if (round % 2 == 1) {
            const auto pick{[&](uint32_t high) {
                return std::uniform_int_distribution<uint32_t>{0, high}(random);
            }};
            used.width = 16 + 2 * pick(3832);
            used.height = 16 + 2 * pick(2152);
            used.framerate = framerates[pick(static_cast<uint32_t>(framerates.size() - 1))];
            used.tickrate = tickrates[pick(static_cast<uint32_t>(tickrates.size() - 1))];
        }
        const int before{failures};
        planned(timeline, "random timeline", used);
        if (failures != before) {
            std::cerr << "random timeline " << round << " failed\n";
            return;
        }
    }
}

// (g) The same timeline gives the same YAML, pinned for fixtures (b) and (c).
void test_pinned_yaml() {
    const director::Timeline timeline{commanders_and_fight()};
    const std::string first{oascript::write_script(
        director::plan_script(timeline, settings()).script, oascript::DocumentForm::yaml
    )};
    const std::string second{oascript::write_script(
        director::plan_script(timeline, settings()).script, oascript::DocumentForm::yaml
    )};
    expect(first == second, "byte-identical twice");
    const oa::base::sha256::Digest digest{oa::base::sha256::digest_of(
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(first.data()), first.size()}
    )};
    const std::array<char, oa::base::sha256::hex_size> digits{oa::base::sha256::to_hex(digest)};
    const std::string hex{digits.begin(), digits.end()};
    constexpr std::string_view pinned{
        "77749001f31b838307377e56e88863bb3b41fb34cffe9bb0f760b386d597b45a"
    };
    expect(hex == pinned, "the pinned plan");
    if (hex != pinned)
        std::cerr << "plan SHA-256 " << hex << "\n" << first;
}

} // namespace

int main() {
    test_no_units();
    test_commander_turns();
    test_fight();
    test_big_blast();
    test_ending();
    test_no_deciding_death();
    test_pan();
    test_hidden_commander();
    test_watchers();
    test_single_player();
    test_switch_ratio();
    test_long_range();
    test_wide_weapon_long_range();
    test_army_lead();
    test_quiet_turns();
    test_army_advance();
    test_framing();
    test_battle_hold();
    test_gun_launch();
    test_water();
    test_random_timelines();
    test_pinned_yaml();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "media-director-planner: ok\n";
    return EXIT_SUCCESS;
}
