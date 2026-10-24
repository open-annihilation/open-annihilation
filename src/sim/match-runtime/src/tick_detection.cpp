// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tick_internal.hpp"

#include "oa/sim/ai.hpp"
#include "oa/sim/detection.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace oa::sim::match_runtime {
namespace {

int32_t sub_fixed(int32_t a, int32_t b) {
    return std::bit_cast<int32_t>(std::bit_cast<uint32_t>(a) - std::bit_cast<uint32_t>(b));
}

int32_t add_fixed(int32_t a, int32_t b) {
    return std::bit_cast<int32_t>(std::bit_cast<uint32_t>(a) + std::bit_cast<uint32_t>(b));
}

int32_t squared_high(int32_t delta) {
    return static_cast<int32_t>((static_cast<int64_t>(delta) * delta) >> 32);
}

int32_t whole_to_fixed(int32_t whole) {
    return static_cast<int32_t>(static_cast<uint32_t>(whole) << 16);
}

/// Gives `list` room for at least `needed` entries in each of its `parts`
/// parts. The room doubles from 64 until it is enough, so a list grows only
/// when a player sees more units at once than before. What the list held is
/// not kept.
///
/// @param list the list
/// @param room the entries each part has room for, updated
/// @param needed the entries each part must hold
/// @param parts how many parts of `room` entries the list holds
void make_seen_room(
    std::unique_ptr<sim::detection::SightedUnit[]>& list,
    uint32_t& room,
    uint32_t needed,
    std::size_t parts
) {
    if (list != nullptr && needed <= room)
        return;
    uint32_t grown = room == 0 ? 64U : room;
    while (grown < needed)
        grown *= 2U;
    list = std::make_unique<sim::detection::SightedUnit[]>(static_cast<std::size_t>(grown) * parts);
    room = grown;
}

} // namespace

uint16_t Match::participant_count() const noexcept {
    const auto recorded = simulation_.record.game.player_count;
    if (recorded != 0)
        return recorded;
    uint16_t live = 0;
    for (uint8_t player = 0; player < simulation_.players.size(); ++player)
        if (sim::simulation_state::player_slot_active(player, simulation_.players[player]))
            ++live;
    return live;
}

void Match::for_each_unit_in_radius(
    const sim::ground_orders::Point& centre,
    int32_t radius,
    void (*visit)(void* context, sim::unit_spawn::Slot& slot),
    void* context
) {
    prepare_spatial_state();
    const auto clamp = [](int32_t value, uint32_t count) -> uint32_t {
        if (static_cast<uint32_t>(value) < count)
            return static_cast<uint32_t>(value);
        return value < 0 ? 0 : count - 1;
    };
    constexpr int bucket_shift = 23;
    const auto width = spatial_.bucket_width;
    const auto height = spatial_.bucket_height;
    if (width == 0 || height == 0)
        return;
    const auto left = clamp(sub_fixed(centre[0], radius) >> bucket_shift, width);
    const auto top = clamp(sub_fixed(centre[2], radius) >> bucket_shift, height);
    const auto right = clamp(add_fixed(centre[0], radius) >> bucket_shift, width);
    const auto bottom = clamp(add_fixed(centre[2], radius) >> bucket_shift, height);
    const auto reach = squared_high(radius);
    for (auto z = top; z <= bottom; ++z)
        for (auto x = left; x <= right; ++x) {
            const auto bucket = static_cast<std::size_t>(z) * width + x;
            if (bucket >= spatial_.buckets.size())
                continue;
            auto steps = slots_.size();
            for (auto id = spatial_.buckets[bucket].head; id != 0 && steps-- > 0;
                 id = spatial_units_[id].next_in_bucket) {
                if (id >= slots_.size())
                    break;
                auto& slot = slots_[id];
                const auto dx = sub_fixed(slot.record.position.x, centre[0]);
                const auto dz = sub_fixed(slot.record.position.z, centre[2]);
                if (add_fixed(squared_high(dz), squared_high(dx)) <= reach)
                    visit(context, slot);
            }
        }
}

void Match::scan_contacts() {
    namespace detection = sim::detection;
    auto& world = state();
    const auto viewpoint = world.game.viewpoint_player;
    if (participant_count() <= 1 || viewpoint >= OA_PLAYER_COUNT)
        return;
    auto& viewer = world.game.players[viewpoint];
    const auto* viewer_info = oa::world_player_info(&world, &viewer);
    // A slotless replay viewer looks through a recorded player's slot and
    // sees every unit as a watcher does while its full radar holds.
    const bool watching = (viewer.in_use != 0 && viewer_info != nullptr &&
                           (viewer_info->options & OA_SETUP_OPTION_WATCHER) != 0) ||
                          slotless_full_radar();
    const uint32_t count = world.unit_slot_count;
    const bool allied_vision = rules().intel.allied_los_sharing.enabled;

    for (uint32_t slot = 1; slot < count; ++slot) {
        auto& unit = world.units[slot];
        if ((unit.flags & OA_UNIT_FLAG_LIVE) == 0)
            continue;
        unit.flags &= ~OA_UNIT_FLAG_CLOAK_LOCKED;
        if (allied_vision) {
            // Every unit whose owner allies the viewer, the viewer's own
            // included, is a contact; no shared-radar role is needed.
            if (!owner_allies(unit, viewpoint) && !watching) {
                unit.flags &= ~detection::contact_bits;
                continue;
            }
        } else if (unit.owner_index != viewpoint) {
            const auto* owner = oa::world_unit_owner(&world, &unit);
            if (!(owner != nullptr && detection::shares_radar(world, viewer, *owner)) &&
                !watching) {
                unit.flags &= ~detection::contact_bits;
                continue;
            }
        }
        unit.flags |= detection::radar_contact | detection::sonar_contact;
    }

    uint32_t owned = 0;
    (void)oa::world_player_units(&world, &viewer, &owned);
    if (!allied_vision || owned == 0) {
        scan_contacts_for(viewer, owned != 0);
        return;
    }
    // Allied vision runs the passes once for each player whose row allies
    // the viewer, in player order, the viewer among them, each as the
    // viewing player: their scanners, jamming as they see it and their line
    // of sight all add to the one radar picture. Jamming in a later pass
    // clears contacts an earlier one made.
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        const auto& ally = world.game.players[player];
        if (ally.alliance[viewpoint] == 0)
            continue;
        uint32_t ally_owned = 0;
        (void)oa::world_player_units(&world, &ally, &ally_owned);
        scan_contacts_for(ally, ally_owned != 0);
    }
}

void Match::scan_contacts_for(const oa::Player& viewer, bool scan_own_units) {
    namespace detection = sim::detection;
    auto& world = state();
    const uint32_t count = world.unit_slot_count;

    // Radar reaches twice the scanner's integer altitude further; the walk
    // covers the larger of the plain radar and sonar ranges.
    struct Stamp {
        detection::ScanRecord scan;
        oa::World* world;
    };

    uint32_t owned = 0;
    auto* units = scan_own_units ? oa::world_player_units(&world, &viewer, &owned) : nullptr;
    if (units != nullptr)
        for (uint32_t i = 0; i < owned; ++i) {
            const auto& scanner = units[i];
            if (!unit_is_live_target(scanner.flags) ||
                (scanner.state_flags & OA_UNIT_STATE_ACTIVE) == 0)
                continue;
            const auto* def = oa::world_unit_def_of(&world, &scanner);
            if (def == nullptr || (def->radar_distance == 0 && def->sonar_distance == 0))
                continue;
            const int16_t radar = def->radar_distance;
            const int16_t sonar = def->sonar_distance;
            const auto altitude = static_cast<int32_t>(
                static_cast<int16_t>(static_cast<uint32_t>(scanner.position.y) >> 16)
            );
            const auto lifted = static_cast<uint32_t>(radar + altitude * 2);
            Stamp stamp{{}, &world};
            stamp.scan.radar_range_squared = static_cast<int32_t>(lifted * lifted);
            stamp.scan.sonar_range_squared = static_cast<int32_t>(sonar) * sonar;
            stamp.scan.position = scanner.position;
            const int16_t reach = radar <= sonar ? sonar : radar;
            for_each_unit_in_radius(
                {scanner.position.x, scanner.position.y, scanner.position.z},
                whole_to_fixed(reach),
                [](void* context, sim::unit_spawn::Slot& slot) {
                    const auto& s = *static_cast<const Stamp*>(context);
                    detection::stamp_contact(s.scan, slot.record, *s.world);
                },
                &stamp
            );
        }

    for (uint32_t slot = 1; slot < count; ++slot) {
        const auto& jammer = world.units[slot];
        if ((jammer.flags & OA_UNIT_FLAG_LIVE) == 0 || jammer.owner_index == viewer.index ||
            (jammer.state_flags & OA_UNIT_STATE_ACTIVE) == 0 || !jammer_jams(viewer, jammer))
            continue;
        const auto* def = oa::world_unit_def_of(&world, &jammer);
        if (def == nullptr)
            continue;
        const sim::ground_orders::Point centre{
            jammer.position.x, jammer.position.y, jammer.position.z
        };
        if (def->radar_distance_jam != 0)
            for_each_unit_in_radius(
                centre,
                whole_to_fixed(def->radar_distance_jam),
                [](void*, sim::unit_spawn::Slot& slot) { detection::jam_radar(slot.record); },
                nullptr
            );
        if (def->sonar_distance_jam != 0)
            for_each_unit_in_radius(
                centre,
                whole_to_fixed(def->sonar_distance_jam),
                [](void*, sim::unit_spawn::Slot& slot) { detection::jam_sonar(slot.record); },
                nullptr
            );
    }

    // Every locally simulated cloaker, cloaked or not, whose owner sees an
    // enemy inside its mincloakdistance.
    for (uint32_t slot = 1; slot < count; ++slot) {
        auto& unit = world.units[slot];
        if ((unit.flags & OA_UNIT_FLAG_LIVE) == 0 ||
            !sim::simulation_state::locally_simulated(world, unit))
            continue;
        const auto* def = oa::world_unit_def_of(&world, &unit);
        if (def == nullptr || (def->abilities & OA_UNIT_DEF_ABILITY_CAN_CLOAK) == 0 ||
            unit.owner_index >= sightings_.size())
            continue;
        if (detection::sighted_within(
                sightings_[unit.owner_index], world, unit.position, def->min_cloak_distance
            ))
            detection::expose_cloaker(unit, world.game.tick);
    }

    for (uint32_t slot = 1; slot < count; ++slot) {
        auto& unit = world.units[slot];
        if ((unit.flags & OA_UNIT_FLAG_LIVE) != 0 && (unit.flags & detection::radar_contact) == 0 &&
            (unit.state_flags & OA_UNIT_STATE_CLOAKED) == 0 &&
            point_seen_by(viewer.index, fixed_words(unit.position)))
            unit.flags |= detection::radar_contact;
    }
}

void Match::keep_player_contacts(uint8_t player, bool keep) {
    if (player >= OA_PLAYER_COUNT) {
        fault_.note("player contact picture is outside the ten players");
        return;
    }
    const auto stride = slots_.size();
    if (!keep) {
        if (player_contacts_ != nullptr) {
            uint8_t* row = player_contacts_.get() + static_cast<std::size_t>(player) * stride;
            for (std::size_t slot = 0; slot < stride; ++slot)
                row[slot] = static_cast<uint8_t>(0);
        }
        player_contacts_kept_[player] = static_cast<uint8_t>(0);
        player_contacts_tick_[player] = 0;
        return;
    }
    if (player_contacts_ == nullptr)
        player_contacts_ = std::make_unique<uint8_t[]>(stride * OA_PLAYER_COUNT);
    player_contacts_kept_[player] = static_cast<uint8_t>(1);
    player_contacts_tick_[player] = 0;
    auto& world = state();
    uint8_t* row = player_contacts_.get() + static_cast<std::size_t>(player) * stride;
    const auto unit_count = static_cast<std::size_t>(world.unit_slot_count);
    for (std::size_t slot = 0; slot < stride; ++slot) {
        if (slot < unit_count && (world.units[slot].flags & OA_UNIT_FLAG_LIVE) != 0)
            row[slot] = sim::detection::created_picture_bits(world.units[slot], player);
        else
            row[slot] = static_cast<uint8_t>(0);
    }
}

bool Match::player_contacts_kept(uint8_t player) const noexcept {
    return player < OA_PLAYER_COUNT && player_contacts_kept_[player] != 0;
}

std::span<const uint8_t> Match::player_contact_bits(uint8_t player) const noexcept {
    if (player >= OA_PLAYER_COUNT || player_contacts_kept_[player] == 0 ||
        player_contacts_ == nullptr)
        return {};
    const auto stride = slots_.size();
    return std::span<const uint8_t>{
        player_contacts_.get() + static_cast<std::size_t>(player) * stride, stride
    };
}

uint32_t Match::player_contacts_tick(uint8_t player) const noexcept {
    if (player >= OA_PLAYER_COUNT)
        return 0;
    return player_contacts_tick_[player];
}

void Match::scan_player_contacts(uint8_t player) {
    namespace detection = sim::detection;
    if (player >= OA_PLAYER_COUNT || player_contacts_kept_[player] == 0 ||
        participant_count() <= 1 || player_contacts_ == nullptr)
        return;
    auto& world = state();
    const auto stride = slots_.size();
    const uint32_t unit_count = world.unit_slot_count;
    const uint32_t limit = unit_count < stride ? unit_count : static_cast<uint32_t>(stride);
    uint8_t* bits = player_contacts_.get() + static_cast<std::size_t>(player) * stride;
    const bool allied_vision = rules().intel.allied_los_sharing.enabled;
    auto& picture_player = world.game.players[player];

    // The first pass matches the viewpoint scan's, without the watcher and
    // slotless-viewer clauses: those belong to the local screen.
    for (uint32_t slot = 1; slot < limit; ++slot) {
        auto& unit = world.units[slot];
        if ((unit.flags & OA_UNIT_FLAG_LIVE) == 0)
            continue;
        if (allied_vision) {
            if (!owner_allies(unit, player)) {
                bits[slot] = static_cast<uint8_t>(0);
                continue;
            }
        } else if (unit.owner_index != player) {
            const auto* owner = oa::world_unit_owner(&world, &unit);
            if (!(owner != nullptr && detection::shares_radar(world, picture_player, *owner))) {
                bits[slot] = static_cast<uint8_t>(0);
                continue;
            }
        }
        bits[slot] =
            static_cast<uint8_t>(bits[slot] | detection::picture_radar | detection::picture_sonar);
    }

    uint32_t owned = 0;
    (void)oa::world_player_units(&world, &picture_player, &owned);
    if (!allied_vision || owned == 0) {
        scan_picture_pass(player, picture_player, owned != 0);
    } else {
        for (uint8_t ally_index = 0; ally_index < OA_PLAYER_COUNT; ++ally_index) {
            const auto& ally_player = world.game.players[ally_index];
            if (ally_player.alliance[player] == 0)
                continue;
            uint32_t ally_owned = 0;
            (void)oa::world_player_units(&world, &ally_player, &ally_owned);
            scan_picture_pass(player, ally_player, ally_owned != 0);
        }
    }
    player_contacts_tick_[player] = world.game.tick;
}

void Match::scan_picture_pass(uint8_t picture, const oa::Player& viewer, bool scan_own_units) {
    namespace detection = sim::detection;
    auto& world = state();
    const auto stride = slots_.size();
    const uint32_t unit_count = world.unit_slot_count;
    const uint32_t limit = unit_count < stride ? unit_count : static_cast<uint32_t>(stride);
    uint8_t* bits = player_contacts_.get() + static_cast<std::size_t>(picture) * stride;

    // Radar reaches twice the scanner's integer altitude further; the walk
    // covers the larger of the plain radar and sonar ranges. The stamp skips
    // the picture player's units, including during an ally's pass.
    struct Stamp {
        detection::ScanRecord scan;
        oa::World* world;
        uint8_t* bits;
        uint8_t picture_player;
        std::size_t slot_count;
    };

    uint32_t owned = 0;
    auto* units = scan_own_units ? oa::world_player_units(&world, &viewer, &owned) : nullptr;
    if (units != nullptr)
        for (uint32_t i = 0; i < owned; ++i) {
            const auto& scanner = units[i];
            if (!unit_is_live_target(scanner.flags) ||
                (scanner.state_flags & OA_UNIT_STATE_ACTIVE) == 0)
                continue;
            const auto* def = oa::world_unit_def_of(&world, &scanner);
            if (def == nullptr || (def->radar_distance == 0 && def->sonar_distance == 0))
                continue;
            const int16_t radar = def->radar_distance;
            const int16_t sonar = def->sonar_distance;
            const auto altitude = static_cast<int32_t>(
                static_cast<int16_t>(static_cast<uint32_t>(scanner.position.y) >> 16)
            );
            const auto lifted = static_cast<uint32_t>(radar + altitude * 2);
            Stamp stamp{{}, &world, bits, picture, stride};
            stamp.scan.radar_range_squared = static_cast<int32_t>(lifted * lifted);
            stamp.scan.sonar_range_squared = static_cast<int32_t>(sonar) * sonar;
            stamp.scan.position = scanner.position;
            const int16_t reach = radar <= sonar ? sonar : radar;
            for_each_unit_in_radius(
                {scanner.position.x, scanner.position.y, scanner.position.z},
                whole_to_fixed(reach),
                [](void* context, sim::unit_spawn::Slot& slot) {
                    const auto& walked = *static_cast<const Stamp*>(context);
                    if (slot.unit_index >= walked.slot_count)
                        return;
                    detection::stamp_picture(
                        walked.scan,
                        walked.picture_player,
                        slot.record,
                        *walked.world,
                        walked.bits[slot.unit_index]
                    );
                },
                &stamp
            );
        }

    struct JamRow {
        uint8_t* bits;
        std::size_t slot_count;
    };

    const bool allied_jammers_ignored = rules().intel.allied_jammers_ignored.enabled;
    JamRow row{bits, stride};
    for (uint32_t slot = 1; slot < limit; ++slot) {
        const auto& jammer = world.units[slot];
        if ((jammer.flags & OA_UNIT_FLAG_LIVE) == 0 || jammer.owner_index == viewer.index ||
            (jammer.state_flags & OA_UNIT_STATE_ACTIVE) == 0 ||
            !detection::seat_jammer_jams(allied_jammers_ignored, viewer, jammer))
            continue;
        const auto* def = oa::world_unit_def_of(&world, &jammer);
        if (def == nullptr)
            continue;
        const sim::ground_orders::Point centre{
            jammer.position.x, jammer.position.y, jammer.position.z
        };
        if (def->radar_distance_jam != 0)
            for_each_unit_in_radius(
                centre,
                whole_to_fixed(def->radar_distance_jam),
                [](void* context, sim::unit_spawn::Slot& slot) {
                    const auto& walked = *static_cast<const JamRow*>(context);
                    if (slot.unit_index >= walked.slot_count)
                        return;
                    detection::jam_picture_radar(walked.bits[slot.unit_index]);
                },
                &row
            );
        if (def->sonar_distance_jam != 0)
            for_each_unit_in_radius(
                centre,
                whole_to_fixed(def->sonar_distance_jam),
                [](void* context, sim::unit_spawn::Slot& slot) {
                    const auto& walked = *static_cast<const JamRow*>(context);
                    if (slot.unit_index >= walked.slot_count)
                        return;
                    detection::jam_picture_sonar(walked.bits[slot.unit_index]);
                },
                &row
            );
    }

    // A picture does not expose cloakers: that writes simulation state the
    // viewpoint scan already applies once.
    // Each pass reads that pass's player's sight, as the viewpoint scan's
    // pass does. A picture of the viewpoint then matches the contact bits.
    for (uint32_t slot = 1; slot < limit; ++slot) {
        const auto& unit = world.units[slot];
        if ((unit.flags & OA_UNIT_FLAG_LIVE) != 0 && (bits[slot] & detection::picture_radar) == 0 &&
            (unit.state_flags & OA_UNIT_STATE_CLOAKED) == 0 &&
            point_seen_by(viewer.index, fixed_words(unit.position)))
            bits[slot] = static_cast<uint8_t>(bits[slot] | detection::picture_radar);
    }
}

uint32_t Match::repair_pads_in_reach(const oa::Unit& aircraft) const {
    const auto& world = state();
    const auto* owner = oa::world_unit_owner(&world, &aircraft);
    if (owner == nullptr || owner->index >= sightings_.size())
        return 0;
    return sim::detection::pads_in_reach(sightings_[owner->index], world, aircraft.position);
}

oa::Unit* Match::repair_pad_in_reach(const oa::Unit& aircraft, uint32_t index) {
    auto& world = state();
    const auto* owner = oa::world_unit_owner(&world, &aircraft);
    if (owner == nullptr || owner->index >= sightings_.size())
        return nullptr;
    const auto slot =
        sim::detection::pad_in_reach(sightings_[owner->index], world, aircraft.position, index);
    return slot != 0 ? oa::world_unit_at(&world, slot) : nullptr;
}

void Match::refresh_player_knowledge(uint8_t player) {
    namespace detection = sim::detection;
    auto& world = state();
    const auto& viewer = world.game.players[player];
    // The knowledge record is built together with the controller.
    if (!sim::ai::player_has_controller(viewer))
        return;
    auto& sightings = sightings_.at(player);
    const auto tick = world.game.tick;
    if (!detection::sightings_due(sightings, tick))
        return;
    auto* computers = sim::ai::match_computer_players(*this);
    auto* knowledge = sim::ai::computer_player_knowledge(computers, player);
    sim::ai::KnowledgeTally tally;
    detection::clear_sightings(sightings);
    if (knowledge != nullptr)
        sim::ai::computer_knowledge_clear(computers, *knowledge);
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        const auto& unit = world.units[slot];
        const bool seen = detection::sighting_candidate(world, viewer, unit) &&
                          unit_visible(player, static_cast<uint16_t>(slot));
        if (detection::file_sighting(sightings, world, viewer, unit, seen) && knowledge != nullptr)
            sim::ai::computer_knowledge_count(computers, *knowledge, unit, tally);
    }
    if (knowledge != nullptr)
        sim::ai::computer_knowledge_settle(*knowledge, tally);
    sightings.refreshed_tick = tick;
    const uint32_t before_count = seen_before_count_[player];
    make_seen_room(
        seen_work_,
        seen_work_room_,
        sightings.seen_count > before_count ? sightings.seen_count : before_count,
        3
    );
    const auto seen_stride = static_cast<std::size_t>(seen_work_room_);
    detection::SightedUnit* const now_list = seen_work_.get();
    uint32_t now_count = 0;
    for (uint32_t filed = 0; filed < sightings.seen_count; ++filed) {
        const auto unit_slot = sightings.seen[filed];
        now_list[now_count] = detection::SightedUnit{
            unit_slot, bridge_->runtime(slots_.at(unit_slot)).instance_generation
        };
        ++now_count;
    }
    if (event_wanted<&EventHooks::seen_changed>()) {
        detection::SightedUnit* const gained_list = now_list + seen_stride;
        detection::SightedUnit* const lost_list = gained_list + seen_stride;
        uint32_t gained_count = 0;
        uint32_t lost_count = 0;
        detection::diff_sighted(
            std::span<const detection::SightedUnit>{
                seen_before_[player].get(), static_cast<std::size_t>(before_count)
            },
            std::span<const detection::SightedUnit>{now_list, static_cast<std::size_t>(now_count)},
            [&](const detection::SightedUnit& entry) {
                lost_list[lost_count] = entry;
                ++lost_count;
            },
            [&](const detection::SightedUnit& entry) {
                gained_list[gained_count] = entry;
                ++gained_count;
            }
        );
        if (gained_count != 0 || lost_count != 0) {
            emit_event<&EventHooks::seen_changed>(
                world,
                player,
                std::span<const detection::SightedUnit>{
                    gained_list, static_cast<std::size_t>(gained_count)
                },
                std::span<const detection::SightedUnit>{
                    lost_list, static_cast<std::size_t>(lost_count)
                }
            );
        }
    }
    make_seen_room(seen_before_[player], seen_before_room_[player], now_count, 1);
    auto* const stored = seen_before_[player].get();
    for (uint32_t copied = 0; copied < now_count; ++copied)
        stored[copied] = now_list[copied];
    seen_before_count_[player] = now_count;
    if (random_.bounded(detection::sighting_period) == 0)
        refresh_strategic_state(player);
}

} // namespace oa::sim::match_runtime
