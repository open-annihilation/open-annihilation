// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/match_runtime/keyframe.hpp"

#include "oa/core/world_image.hpp"
#include "oa/formats/cob.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/match_runtime/spawn_bridge.hpp"
#include "oa/sim/script_state.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace oa::sim::match_runtime {

namespace keyframe {

namespace {
// A section's mark in the varying region: the name's length and bytes, whether the section holds
// bytes, whether they are in the fixed region, and the section's byte count.
constexpr std::size_t mark_tail_bytes = 1 + 1 + 4;
} // namespace

void Buffer::reserve(std::size_t bytes) {
    if (bytes <= capacity_)
        return;
    // The bytes past size_ are written by append before anything reads them,
    // so the grown capacity is not zero-filled.
    auto grown = std::make_unique_for_overwrite<uint8_t[]>(bytes);
    if (size_ != 0)
        std::memcpy(grown.get(), data_.get(), size_);
    data_ = std::move(grown);
    capacity_ = bytes;
}

void Buffer::append(const void* data, std::size_t size) {
    if (size == 0)
        return;
    if (size_ + size > capacity_)
        reserve(std::max(size_ + size, capacity_ + capacity_ / 2));
    std::memcpy(data_.get() + size_, data, size);
    size_ += size;
}

void Writer::restart(Omission omission) {
    omission_ = omission;
    fixed_.clear();
    varying_.clear();
    omitting_ = false;
    current_fixed_ = false;
    omitted_ = 0;
    length_at_ = no_mark;
    section_start_ = 0;
}

void Writer::finish() {
    if (length_at_ == no_mark)
        return;
    const auto size =
        static_cast<uint32_t>((current_fixed_ ? fixed_.size() : varying_.size()) - section_start_);
    std::memcpy(varying_.bytes().data() + length_at_, &size, sizeof size);
    length_at_ = no_mark;
}

void Writer::begin(std::string_view name, bool fixed) {
    finish();
    current_fixed_ = fixed;
    omitting_ = omission_.omit != nullptr && omission_.omit(omission_.context, name);
    if (omitting_)
        ++omitted_;
    const auto length = static_cast<uint8_t>(std::min(name.size(), longest_section_name));
    const std::array<uint8_t, 1> length_byte{length};
    varying_.append(length_byte.data(), length_byte.size());
    varying_.append(name.data(), length);
    const std::array<uint8_t, 2> flags{
        omitting_ ? uint8_t{0} : uint8_t{1}, fixed ? uint8_t{1} : uint8_t{0}
    };
    varying_.append(flags.data(), flags.size());
    length_at_ = varying_.size();
    const std::array<uint8_t, sizeof(uint32_t)> size{};
    varying_.append(size.data(), size.size());
    section_start_ = fixed ? fixed_.size() : varying_.size();
}

void Writer::bytes(const void* data, std::size_t size) {
    if (omitting_ || size == 0)
        return;
    (current_fixed_ ? fixed_ : varying_).append(data, size);
}

bool Reader::close_section() {
    if (!section_open_)
        return ok();
    section_open_ = false;
    const auto consumed = (current_fixed_ ? fixed_at_ : varying_at_) - section_start_;
    if (consumed != section_size_)
        return fail("a section was not read to its end", section_name_.data());
    return ok();
}

bool Reader::begin(std::string_view name, bool fixed) {
    if (!close_section())
        return false;
    current_fixed_ = fixed;
    const auto length = static_cast<std::size_t>(std::min(name.size(), longest_section_name));
    if (varying_at_ + 1 + length + mark_tail_bytes > varying_.size())
        return fail("a keyframe ends before a section", name);
    if (varying_[varying_at_] != length ||
        std::memcmp(varying_.data() + varying_at_ + 1, name.data(), length) != 0)
        return fail("a keyframe holds another section than", name);
    const auto* mark = varying_.data() + varying_at_ + 1 + length;
    const bool present = mark[0] != 0;
    if ((mark[1] != 0) != fixed)
        return fail("a section is in the other region", name);
    uint32_t size = 0;
    std::memcpy(&size, mark + 2, sizeof size);
    varying_at_ += 1 + length + mark_tail_bytes;
    if (!present)
        return false;
    section_open_ = true;
    section_name_ = {};
    std::memcpy(section_name_.data(), name.data(), length);
    section_size_ = size;
    section_start_ = fixed ? fixed_at_ : varying_at_;
    if (section_start_ + size > (fixed ? fixed_.size() : varying_.size()))
        return fail("a section is longer than its region", name);
    return true;
}

bool Reader::bytes(void* data, std::size_t size) {
    if (!ok())
        return false;
    if (size == 0)
        return true;
    auto& at = current_fixed_ ? fixed_at_ : varying_at_;
    const auto region = current_fixed_ ? fixed_ : varying_;
    if (!section_open_ || at + size > section_start_ + section_size_)
        return fail("a section is shorter than its reader", section_name_.data());
    std::memcpy(data, region.data() + at, size);
    at += size;
    return true;
}

bool Reader::fail(std::string_view what, std::string_view detail) {
    if (!ok())
        return false;
    const auto text_bytes = std::min(what.size(), longest_error);
    std::memcpy(error_.data(), what.data(), text_bytes);
    std::size_t at = text_bytes;
    if (!detail.empty() && at + 2 < longest_error) {
        error_[at++] = ':';
        error_[at++] = ' ';
        const auto detail_bytes = std::min(detail.size(), longest_error - at);
        std::memcpy(error_.data() + at, detail.data(), detail_bytes);
        at += detail_bytes;
    }
    error_[at] = '\0';
    return false;
}

bool Reader::finish() {
    if (!close_section())
        return false;
    if (fixed_at_ != fixed_.size() || varying_at_ != varying_.size())
        return fail("a keyframe holds more than was read");
    return true;
}

} // namespace keyframe

/// The keyframe's way into a match: it writes and reads the match's private state.
class KeyframeAccess {
  public:

    /// Counts what a match holds.
    static keyframe::Census census(const Match& match);
    /// Writes a match's sections.
    static const char* write(const Match& match, keyframe::Writer* out);
    /// Reads a match's sections.
    static bool read(Match& match, keyframe::Reader* in);

  private:

    /// Names the first state a playback session never holds that a match does.
    static const char* unsupported(const Match& match);

    /// What is stored per unit slot, so that the slot's runtime is as it was.
    struct SlotRecord {
        uint32_t generation{};
        uint32_t cached_image_word{};
        uint8_t finished_reported{};
        std::array<uint8_t, 3> weapon_slots_initialized{};
    };

    static_assert(sizeof(SlotRecord) == 12);
    /// The bits of a live unit's flags in the stream.
    static constexpr uint8_t unit_has_ground = 1;
    static constexpr uint8_t unit_has_script = 2;
    static constexpr uint8_t unit_has_instance = 4;
};

namespace {

using keyframe::Reader;
using keyframe::Writer;

/// A section's name made of a prefix and a suffix.
using SectionName = std::array<char, keyframe::longest_section_name + 1>;

/// Joins the two parts of a section's name.
///
/// @param prefix The first part.
/// @param suffix The rest.
/// @return The name, cut at the longest a section's name can be.
SectionName section_name(std::string_view prefix, std::string_view suffix) {
    SectionName name{};
    std::snprintf(
        name.data(),
        name.size(),
        "%.*s%.*s",
        static_cast<int>(prefix.size()),
        prefix.data(),
        static_cast<int>(suffix.size()),
        suffix.data()
    );
    return name;
}

template <class T>
void put_table(Writer* out, std::string_view name, const T* data, std::size_t count) {
    static_assert(std::is_trivially_copyable_v<T>);
    out->begin(name, true);
    out->bytes(data, count * sizeof(T));
}

template <class T>
bool get_table(Reader* in, std::string_view name, T* data, std::size_t count) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (!in->begin(name, true))
        return in->ok();
    return in->bytes(data, count * sizeof(T));
}

/// Writes a run of plain records as a fixed table.
template <class Records>
void put_all(Writer* out, std::string_view name, const Records& records) {
    put_table(out, name, records.data(), records.size());
}

/// Reads a run of plain records back over a run of the same length.
template <class Records>
bool get_all(Reader* in, std::string_view name, Records* records) {
    return get_table(in, name, records->data(), records->size());
}

namespace particles = sim::effect_particles;

// An emitter's bytes with its padding zero: the padding holds what the last write to the slot left
// there, which differs from one run of the same replay to the next.
static_assert(
    sizeof(particles::Emitter) == offsetof(particles::Emitter, variant) + sizeof(int32_t)
);

void put_emitters(Writer* out, const particles::Emitter* slots, std::size_t count) {
    out->begin("match.fx_emitters", true);
    for (std::size_t index = 0; index < count; ++index) {
        particles::Emitter emitter = slots[index];
        auto* bytes = reinterpret_cast<uint8_t*>(&emitter);
        std::memset(
            bytes + offsetof(particles::Emitter, kind) + sizeof(emitter.kind),
            0,
            offsetof(particles::Emitter, deadline) - offsetof(particles::Emitter, kind) -
                sizeof(emitter.kind)
        );
        std::memset(
            bytes + offsetof(particles::Emitter, particles) + sizeof(emitter.particles),
            0,
            offsetof(particles::Emitter, interval) - offsetof(particles::Emitter, particles) -
                sizeof(emitter.particles)
        );
        out->bytes(&emitter, sizeof emitter);
    }
}

// The smoke pool's bytes with the padding of each puff zero, as for an emitter.
void put_smoke(
    Writer* out,
    const particles::ParticlePool<particles::SmokeParticle, particles::smoke_pool_capacity>& pool
) {
    using Pool = particles::ParticlePool<particles::SmokeParticle, particles::smoke_pool_capacity>;
    out->begin("match.fx_smoke", true);
    auto copy = std::make_unique<Pool>(pool);
    for (auto& puff : copy->records)
        std::memset(
            reinterpret_cast<uint8_t*>(&puff) + offsetof(particles::SmokeParticle, remaining) +
                sizeof(puff.remaining),
            0,
            sizeof puff - offsetof(particles::SmokeParticle, remaining) - sizeof(puff.remaining)
        );
    out->bytes(copy.get(), sizeof *copy);
}

static_assert(std::is_trivially_copyable_v<sim::air::AirDriver>);
static_assert(std::is_trivially_copyable_v<sim::air::AirGoal>);
static_assert(std::is_trivially_copyable_v<sim::combat_state::UnitWeapons>);
static_assert(std::is_trivially_copyable_v<sim::spatial_state::Unit>);
static_assert(std::is_trivially_copyable_v<sim::spatial_state::Plot>);
static_assert(std::is_trivially_copyable_v<sim::spatial_state::Bucket>);
static_assert(std::is_trivially_copyable_v<sim::scenario::Controller>);
static_assert(std::is_trivially_copyable_v<sim::scenario::OutcomeState>);
static_assert(std::is_trivially_copyable_v<sim::world_environment::WindState>);
static_assert(std::is_trivially_copyable_v<sim::visibility_state::EyeballMemory>);
static_assert(std::is_trivially_copyable_v<sim::unit_movement::Unit>);
static_assert(std::is_trivially_copyable_v<sim::unit_movement::Movement>);
static_assert(std::is_trivially_copyable_v<sim::ground_orders::MirroredNavigation>);
static_assert(std::is_trivially_copyable_v<sim::effect_particles::Layer>);
static_assert(std::is_trivially_copyable_v<sim::effect_particles::Emitter>);
static_assert(std::is_trivially_copyable_v<sim::effect_particles::ExplosionRecord>);
static_assert(std::is_trivially_copyable_v<sim::effect_particles::DebrisPiece>);
static_assert(std::is_trivially_copyable_v<sim::effect_particles::ShatterFragment>);
static_assert(std::is_trivially_copyable_v<sim::detection::SightedUnit>);

/// What the match keeps of one ground unit's movement object.
struct GroundRecord {
    sim::unit_movement::Unit geometry{};
    sim::unit_movement::Movement movement{};
    sim::ground_orders::MirroredNavigation mirrored_navigation{};
    std::array<sim::ground_orders::Fixed, 3> previous_vector{};
    uint32_t occupancy_changed_tick{};
};

static_assert(std::is_trivially_copyable_v<GroundRecord>);

/// The mirrored sightings of a player as stored: the counts that matter and the lists' fronts.
struct SightingCounts {
    uint32_t seen{};
    uint32_t radar{};
    uint32_t pads{};
    uint32_t refreshed_tick{};
    uint8_t radar_fallback{};
};

/// A run of bytes of the game record that a keyframe does not carry: what the player's own
/// settings and the clock write there, and no tick of a replay reads.
struct GameRange {
    std::size_t first{};
    std::size_t end{};
};

/// The ranges of the game record a keyframe leaves as they are: the video capture's settings, the
/// times the profiler measured, and the chat ring, whose lines are what the player has been shown.
constexpr std::array<GameRange, 4> kept_game_ranges{{
    {offsetof(Game, output_directory), offsetof(Game, block_before_saved_game)},
    {offsetof(Game, profile_times), offsetof(Game, scores)},
    {offsetof(Game, chat_lines), offsetof(Game, players)},
    {offsetof(Game, chat_head), offsetof(Game, chat_tail) + sizeof(uint16_t)},
}};

} // namespace

const char* KeyframeAccess::unsupported(const Match& match) {
    if (!match.orders_.empty())
        return "the match owns orders";
    const auto& world = match.state();
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        const auto& queue = match.state_.orders[slot];
        if (queue.primary != nullptr || queue.secondary != nullptr)
            return "a unit has orders queued";
        if (world.units[slot].type_index == 0)
            continue;
        if (const auto* ground = match.movement_[slot].get()) {
            if (!ground->mirrored_driver || ground->mirrored_navigation.goal != nullptr ||
                ground->navigation.goal != nullptr || ground->navigation.count != 0)
                return "a unit is steered on this machine";
        }
        const auto& driver = match.air_drivers_[slot];
        if (driver.unit != nullptr && driver.local)
            return "an aircraft is steered on this machine";
        if (driver.goal != nullptr && (driver.goal != &match.mirrored_air_goals_[slot] ||
                                       driver.goal->order_events != nullptr))
            return "an aircraft's goal belongs to an order";
        if (driver.arrived.order_events != nullptr ||
            match.mirrored_air_goals_[slot].order_events != nullptr)
            return "an aircraft's goal belongs to an order";
    }
    for (const auto& player : world.game.players)
        if (player.in_use != 0 && player.status == OA_PLAYER_STATUS_COMPUTER)
            return "a computer player plays";
    for (const auto kept : match.player_contacts_kept_)
        if (kept != 0)
            return "a player's radar picture is kept";
    if (match.applied_shot_ != nullptr)
        return "a shot is being placed";
    return nullptr;
}

keyframe::Census KeyframeAccess::census(const Match& match) {
    keyframe::Census census;
    const auto& world = match.state();
    auto& self = const_cast<Match&>(match);
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        const auto& unit = world.units[slot];
        if (unit.type_index == 0)
            continue;
        ++census.live_units;
        if (auto* object = self.instance(static_cast<uint16_t>(slot))) {
            ++census.units_with_instances;
            if (object->script() != nullptr)
                ++census.units_with_scripts;
        }
        if (const auto* ground = match.movement_[slot].get()) {
            ++census.ground_runtimes;
            if (ground->mirrored_driver) {
                ++census.mirrored_ground;
                if (ground->mirrored_navigation.count > 1)
                    ++census.mirrored_routes;
            }
        }
        if (match.air_drivers_[slot].unit != nullptr) {
            if (match.air_drivers_[slot].local)
                ++census.air_drivers_local;
            else
                ++census.air_drivers_mirrored;
        }
        if (match.state_.orders[slot].primary != nullptr ||
            match.state_.orders[slot].secondary != nullptr)
            ++census.unit_order_queues;
    }
    census.orders = static_cast<uint32_t>(match.orders_.size());
    census.wrecks = static_cast<uint32_t>(match.wrecks_.size());
    for (const auto& player_groups : match.groups_)
        census.squads += static_cast<uint32_t>(player_groups.size());
    census.rule_state_tables = match.rule_state_.count;
    for (const auto& layer : match.effects_->layers)
        census.effect_emitters += layer.count;
    census.effect_particles = static_cast<uint32_t>(
        match.effects_->trail.live + match.effects_->nano.live + match.effects_->flame.live +
        match.effects_->wake.live + match.effects_->smoke.live
    );
    census.effect_explosions = static_cast<uint32_t>(std::max(match.effects_->explosion_count, 0));
    for (const auto& piece : match.effects_->debris)
        census.effect_debris += piece.live ? 1u : 0u;
    for (const auto& fragment : match.effects_->fragments)
        census.effect_fragments += fragment.live ? 1u : 0u;
    for (const auto& list : match.sightings_) {
        census.seen_units += list.seen_count;
        census.radar_units += list.radar_count;
    }
    for (const auto kept : match.player_contacts_kept_)
        census.contact_players += kept != 0 ? 1u : 0u;
    census.scenario_conditions =
        static_cast<uint32_t>(match.scenario_.victory_count + match.scenario_.defeat_count);
    census.remembered_sights = static_cast<uint32_t>(std::max(match.remembered_sight_.count, 0));
    for (const auto& klass : match.movement_classes_)
        census.movement_classes += klass.map.has_value() ? 1u : 0u;
    census.placed_features = world.placed_feature_count;
    census.structure_gifts = match.structure_gifts_ != nullptr;
    census.computer_state = match.computer_player_state_ != nullptr;
    census.seat_state = match.seat_state_ != nullptr;
    return census;
}

const char* KeyframeAccess::write(const Match& match, keyframe::Writer* out) {
    if (const char* why = unsupported(match))
        return why;
    auto& self = const_cast<Match&>(match);
    auto& world = self.state();
    const auto slots = static_cast<std::size_t>(world.unit_slot_count);

    // The world's tables.
    for (const auto& table : world_image_tables(&world)) {
        out->begin(section_name("world.", table.name).data(), true);
        if (table.data != &world.game) {
            out->bytes(table.data, table.bytes);
            continue;
        }
        // The game record without the fields a keyframe leaves alone, which are zero in the image.
        auto game = std::make_unique<Game>(world.game);
        for (const auto& range : kept_game_ranges)
            std::memset(
                reinterpret_cast<uint8_t*>(game.get()) + range.first, 0, range.end - range.first
            );
        out->bytes(game.get(), sizeof *game);
    }

    // Each live unit's movement object and script.
    out->begin("match.units");
    uint32_t live = 0;
    for (std::size_t slot = 1; slot < slots; ++slot)
        live += world.units[slot].type_index != 0 ? 1u : 0u;
    out->value(live);
    for (std::size_t slot = 1; slot < slots; ++slot) {
        if (world.units[slot].type_index == 0)
            continue;
        out->value(static_cast<uint16_t>(slot));
        const auto* ground = match.movement_[slot].get();
        auto* instance = self.instance(static_cast<uint16_t>(slot));
        const bool scripted = instance != nullptr && instance->script() != nullptr;
        const uint8_t flags = static_cast<uint8_t>(
            (ground != nullptr ? unit_has_ground : 0) |
            (instance != nullptr ? unit_has_instance : 0) | (scripted ? unit_has_script : 0)
        );
        out->value(flags);
        if (ground != nullptr) {
            const GroundRecord record{
                ground->geometry,
                ground->movement,
                ground->mirrored_navigation,
                ground->previous_vector,
                ground->occupancy_changed_tick
            };
            out->value(record);
        }
        if (scripted) {
            const auto exported = instance->script()->vm().export_state(
                oa::formats::cob::script_identity(instance->script()->program())
            );
            if (!exported.ok())
                return "a unit's script state cannot be exported";
            const auto bytes = oa::sim::script_state::encode(*exported.state);
            if (bytes.empty())
                return "a unit's script state cannot be encoded";
            out->list(std::span<const uint8_t>(bytes));
        }
    }

    // What each slot's runtime keeps beside the unit's record.
    auto records = std::make_unique<SlotRecord[]>(slots);
    for (std::size_t slot = 1; slot < slots; ++slot) {
        auto& runtime = self.bridge_->runtime(self.slots_[slot]);
        records[slot].generation = runtime.instance_generation;
        records[slot].cached_image_word = runtime.cached_image_word;
        records[slot].finished_reported = runtime.finished_reported ? 1 : 0;
        for (std::size_t w = 0; w < 3; ++w)
            records[slot].weapon_slots_initialized[w] =
                self.state_.assets[slot].weapon_slots_initialized[w] ? 1 : 0;
    }
    put_table(out, "match.slots", records.get(), slots);
    put_all(out, "match.air_drivers", match.air_drivers_);
    put_all(out, "match.air_goals", match.mirrored_air_goals_);
    put_all(out, "match.weapons", match.weapons_);
    put_all(out, "match.spatial_units", match.spatial_units_);
    put_all(out, "match.spatial_plots", match.spatial_.plots);
    put_all(out, "match.spatial_buckets", match.spatial_.buckets);
    put_all(out, "match.sight_coverage", match.sight_.coverage);
    put_all(out, "match.sight_players", match.sight_.player_bits);
    for (std::size_t player = 0; player < match.other_player_coverage_.size(); ++player) {
        const std::array<char, 2> digit{static_cast<char>('0' + player), '\0'};
        put_all(
            out,
            section_name("match.sight_other_", digit.data()).data(),
            match.other_player_coverage_[player]
        );
    }
    const auto cells = static_cast<std::size_t>(world.game.map_width) *
                       static_cast<std::size_t>(world.game.map_height);
    put_table(out, "match.plot_metal", match.plot_metal_.get(), match.plot_metal_ ? cells : 0);

    // The effects: they draw from the stream the simulation shares, so the next tick reads them.
    const auto& effects = *match.effects_;
    put_table(out, "match.fx_layers", effects.layers, std::size(effects.layers));
    put_emitters(out, effects.emitter_block.slots.get(), effects.emitter_block.count);
    put_table(out, "match.fx_trail", &effects.trail, 1);
    put_table(out, "match.fx_nano", &effects.nano, 1);
    put_table(out, "match.fx_flame", &effects.flame, 1);
    put_table(out, "match.fx_wake", &effects.wake, 1);
    put_smoke(out, effects.smoke);
    put_table(out, "match.fx_explosions", effects.explosions, std::size(effects.explosions));
    put_table(out, "match.fx_debris", effects.debris, std::size(effects.debris));
    put_table(out, "match.fx_fragments", effects.fragments, std::size(effects.fragments));

    // The rest of the match.
    out->begin("match.state");
    out->value(match.random_.state());
    out->value(match.lcg_seed_);
    out->value(match.outcome_state_);
    out->value(static_cast<uint8_t>(match.outcome_result_));
    out->value(static_cast<uint8_t>(match.local_game_ended_ ? 1 : 0));
    out->value(static_cast<uint8_t>(match.load_finished_ ? 1 : 0));
    out->value(static_cast<uint8_t>(match.load_pass_waiting_ ? 1 : 0));
    out->value(match.load_tick_);
    out->value(match.wind_);
    out->value(match.environment_wind_);
    out->value(match.weapon_sweep_cursor_);
    out->value(match.scenario_);
    out->value(match.remembered_sight_);
    out->value(static_cast<uint8_t>(match.occupants_projected_ ? 1 : 0));
    out->value(effects.explosion_count);
    out->value(effects.pooled_emitters);
    for (const auto& alliances : match.player_alliances_) {
        out->value(static_cast<uint8_t>(alliances.has_value() ? 1 : 0));
        if (alliances)
            out->value(*alliances);
    }
    out->value(match.spatial_.outside_bucket);
    out->value(match.spatial_.written_occupants);
    out->value(match.spatial_.written_occupant_count);
    out->value(static_cast<uint8_t>(match.spatial_.written_occupants_lost ? 1 : 0));
    out->value(match.spatial_.tick);
    out->value(match.spatial_.fill_round);
    out->value(match.spatial_.last_filled);
    out->list(std::span<const Match::Wreck>(match.wrecks_));
    for (const auto& player_groups : match.groups_) {
        out->value(static_cast<uint32_t>(player_groups.size()));
        for (const auto& [group, members] : player_groups) {
            out->value(group);
            out->list(std::span<const uint16_t>(members));
        }
    }
    for (std::size_t player = 0; player < match.sightings_.size(); ++player) {
        const auto& list = match.sightings_[player];
        const SightingCounts counts{
            list.seen_count,
            list.radar_count,
            list.pad_count,
            list.refreshed_tick,
            list.radar_fallback
        };
        out->value(counts);
        out->bytes(list.seen, counts.seen * sizeof(uint16_t));
        out->bytes(list.radar, counts.radar * sizeof(uint16_t));
        out->bytes(list.pads, counts.pads * sizeof(uint16_t));
        out->value(match.seen_before_count_[player]);
        out->bytes(
            match.seen_before_[player].get(),
            match.seen_before_count_[player] * sizeof(sim::detection::SightedUnit)
        );
    }
    out->value(static_cast<uint8_t>(match.rule_state_.count));
    for (uint8_t index = 0; index < match.rule_state_.count; ++index) {
        const auto& table = match.rule_state_.tables[index];
        const std::string_view name(table.name);
        out->value(static_cast<uint8_t>(name.size()));
        out->bytes(name.data(), name.size());
        const auto bytes =
            table.bytes != nullptr ? table.bytes(table.context) : std::span<const uint8_t>{};
        out->list(bytes);
    }
    return nullptr;
}

bool KeyframeAccess::read(Match& match, keyframe::Reader* in) {
    if (const char* why = unsupported(match))
        return in->fail(why);
    auto& world = match.state();
    const auto slots = static_cast<std::size_t>(world.unit_slot_count);
    auto before = std::make_unique<uint16_t[]>(slots);
    for (std::size_t slot = 0; slot < slots; ++slot)
        before[slot] = world.units[slot].type_index;

    for (const auto& table : world_image_tables(&world)) {
        if (!in->begin(section_name("world.", table.name).data(), true)) {
            if (!in->ok())
                return false;
            continue;
        }
        if (table.data != &world.game) {
            if (!in->bytes(table.data, table.bytes))
                return false;
            continue;
        }
        // The game record comes back but for the fields it was written without.
        std::size_t kept_bytes = 0;
        for (const auto& range : kept_game_ranges)
            kept_bytes += range.end - range.first;
        auto kept = std::make_unique<uint8_t[]>(kept_bytes);
        auto* game_bytes = reinterpret_cast<uint8_t*>(&world.game);
        std::size_t at = 0;
        for (const auto& range : kept_game_ranges) {
            std::memcpy(kept.get() + at, game_bytes + range.first, range.end - range.first);
            at += range.end - range.first;
        }
        if (!in->bytes(table.data, table.bytes))
            return false;
        at = 0;
        for (const auto& range : kept_game_ranges) {
            std::memcpy(game_bytes + range.first, kept.get() + at, range.end - range.first);
            at += range.end - range.first;
        }
    }

    // Each live unit's movement object and script, and the units that are gone since.
    if (in->begin("match.units")) {
        uint32_t live = 0;
        if (!in->value(&live))
            return false;
        if (live >= slots)
            return in->fail("a keyframe holds more units than the pool");
        auto listed = std::make_unique<uint8_t[]>(slots);
        uint16_t previous = 0;
        for (uint32_t i = 0; i < live; ++i) {
            uint16_t slot = 0;
            uint8_t flags = 0;
            if (!in->value(&slot) || !in->value(&flags))
                return false;
            if (slot <= previous || slot >= slots || world.units[slot].type_index == 0)
                return in->fail("a keyframe lists a unit that its world does not hold");
            previous = slot;
            listed[slot] = 1;
            auto& view = match.slots_[slot];
            const bool retyped = before[slot] != world.units[slot].type_index;
            if ((flags & unit_has_ground) != 0) {
                GroundRecord record{};
                if (!in->value(&record))
                    return false;
                auto& ground = match.movement_[slot];
                if (ground == nullptr || retyped) {
                    ground.reset();
                    view.movement_object = match.create_movement(view);
                }
                if (ground == nullptr)
                    return in->fail("a unit's movement object cannot be made");
                ground->geometry = record.geometry;
                ground->movement = record.movement;
                ground->mirrored_navigation = record.mirrored_navigation;
                ground->previous_vector = record.previous_vector;
                ground->occupancy_changed_tick = record.occupancy_changed_tick;
            } else if (match.movement_[slot] != nullptr) {
                match.movement_[slot].reset();
                view.movement_object = 0;
            }
            auto& runtime = match.bridge_->runtime(view);
            if ((flags & unit_has_instance) != 0) {
                if (runtime.instance == nullptr || retyped) {
                    runtime.instance.reset();
                    if (!match.bridge_->rebuild_instance(view))
                        return in->fail("a unit's model cannot be made");
                }
                if ((flags & unit_has_script) != 0) {
                    uint32_t size = 0;
                    if (!in->list_count(&size, std::size_t{1} << 24))
                        return false;
                    auto bytes = std::make_unique<uint8_t[]>(size);
                    if (!in->bytes(bytes.get(), size))
                        return false;
                    auto* script =
                        runtime.instance != nullptr ? runtime.instance->script() : nullptr;
                    if (script == nullptr)
                        return in->fail("a unit that ran a script has none now");
                    const auto& program = script->vm().program();
                    const auto decoded = oa::sim::script_state::decode(
                        std::span<const uint8_t>(bytes.get(), size),
                        oa::formats::cob::script_identity(script->program()),
                        program.static_count,
                        program.piece_count
                    );
                    if (!decoded.ok() || script->vm().import_state(*decoded.state).has_value())
                        return in->fail("a unit's script state does not fit its script");
                }
            } else {
                runtime.instance.reset();
            }
        }
        for (std::size_t slot = 1; slot < slots; ++slot) {
            if (listed[slot] != 0)
                continue;
            match.movement_[slot].reset();
            match.slots_[slot].movement_object = 0;
            match.bridge_->runtime(match.slots_[slot]).instance.reset();
        }
    } else if (!in->ok()) {
        return false;
    }

    auto records = std::make_unique<SlotRecord[]>(slots);
    if (in->begin("match.slots", true)) {
        if (!in->bytes(records.get(), slots * sizeof(SlotRecord)))
            return false;
        for (std::size_t slot = 1; slot < slots; ++slot) {
            auto& runtime = match.bridge_->runtime(match.slots_[slot]);
            runtime.instance_generation = records[slot].generation;
            runtime.cached_image_word = records[slot].cached_image_word;
            runtime.finished_reported = records[slot].finished_reported != 0;
            for (std::size_t w = 0; w < 3; ++w)
                match.state_.assets[slot].weapon_slots_initialized[w] =
                    records[slot].weapon_slots_initialized[w] != 0;
        }
    } else if (!in->ok()) {
        return false;
    }
    if (!get_all(in, "match.air_drivers", &match.air_drivers_) ||
        !get_all(in, "match.air_goals", &match.mirrored_air_goals_) ||
        !get_all(in, "match.weapons", &match.weapons_) ||
        !get_all(in, "match.spatial_units", &match.spatial_units_) ||
        !get_all(in, "match.spatial_plots", &match.spatial_.plots) ||
        !get_all(in, "match.spatial_buckets", &match.spatial_.buckets) ||
        !get_all(in, "match.sight_coverage", &match.sight_.coverage) ||
        !get_all(in, "match.sight_players", &match.sight_.player_bits))
        return false;
    for (std::size_t player = 0; player < match.other_player_coverage_.size(); ++player) {
        const std::array<char, 2> digit{static_cast<char>('0' + player), '\0'};
        if (!get_all(
                in,
                section_name("match.sight_other_", digit.data()).data(),
                &match.other_player_coverage_[player]
            ))
            return false;
    }
    const auto cells = static_cast<std::size_t>(world.game.map_width) *
                       static_cast<std::size_t>(world.game.map_height);
    if (!get_table(in, "match.plot_metal", match.plot_metal_.get(), match.plot_metal_ ? cells : 0))
        return false;

    auto& effects = *match.effects_;
    if (!get_table(in, "match.fx_layers", effects.layers, std::size(effects.layers)) ||
        !get_table(
            in, "match.fx_emitters", effects.emitter_block.slots.get(), effects.emitter_block.count
        ) ||
        !get_table(in, "match.fx_trail", &effects.trail, 1) ||
        !get_table(in, "match.fx_nano", &effects.nano, 1) ||
        !get_table(in, "match.fx_flame", &effects.flame, 1) ||
        !get_table(in, "match.fx_wake", &effects.wake, 1) ||
        !get_table(in, "match.fx_smoke", &effects.smoke, 1) ||
        !get_table(in, "match.fx_explosions", effects.explosions, std::size(effects.explosions)) ||
        !get_table(in, "match.fx_debris", effects.debris, std::size(effects.debris)) ||
        !get_table(in, "match.fx_fragments", effects.fragments, std::size(effects.fragments)))
        return false;

    if (!in->begin("match.state"))
        return in->ok();
    uint32_t random_state = 0;
    uint8_t flag = 0;
    if (!in->value(&random_state) || !in->value(&match.lcg_seed_))
        return false;
    match.random_.restore(random_state);
    if (!in->value(&match.outcome_state_) || !in->value(&flag))
        return false;
    match.outcome_result_ = static_cast<sim::scenario::Outcome>(flag);
    if (!in->value(&flag))
        return false;
    match.local_game_ended_ = flag != 0;
    if (!in->value(&flag))
        return false;
    match.load_finished_ = flag != 0;
    if (!in->value(&flag))
        return false;
    match.load_pass_waiting_ = flag != 0;
    if (!in->value(&match.load_tick_) || !in->value(&match.wind_) ||
        !in->value(&match.environment_wind_) || !in->value(&match.weapon_sweep_cursor_) ||
        !in->value(&match.scenario_) || !in->value(&match.remembered_sight_) || !in->value(&flag))
        return false;
    match.occupants_projected_ = flag != 0;
    if (!in->value(&effects.explosion_count) || !in->value(&effects.pooled_emitters))
        return false;
    for (auto& alliances : match.player_alliances_) {
        if (!in->value(&flag))
            return false;
        if (flag == 0) {
            alliances.reset();
            continue;
        }
        std::array<uint8_t, 10> row{};
        if (!in->value(&row))
            return false;
        alliances = row;
    }
    if (!in->value(&match.spatial_.outside_bucket) ||
        !in->value(&match.spatial_.written_occupants) ||
        !in->value(&match.spatial_.written_occupant_count) || !in->value(&flag))
        return false;
    match.spatial_.written_occupants_lost = flag != 0;
    if (!in->value(&match.spatial_.tick) || !in->value(&match.spatial_.fill_round) ||
        !in->value(&match.spatial_.last_filled))
        return false;
    uint32_t count = 0;
    if (!in->list_count(&count, std::size_t{1} << 20))
        return false;
    match.wrecks_.resize(count);
    if (!in->bytes(match.wrecks_.data(), count * sizeof(Match::Wreck)))
        return false;
    for (auto& player_groups : match.groups_) {
        player_groups.clear();
        if (!in->value(&count))
            return false;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t group = 0;
            uint32_t members = 0;
            if (!in->value(&group) || !in->list_count(&members, slots))
                return false;
            auto& squad = player_groups[group];
            squad.resize(members);
            if (!in->bytes(squad.data(), members * sizeof(uint16_t)))
                return false;
        }
    }
    for (std::size_t player = 0; player < match.sightings_.size(); ++player) {
        auto& list = match.sightings_[player];
        SightingCounts counts{};
        if (!in->value(&counts))
            return false;
        if (counts.seen > list.capacity || counts.radar > list.capacity ||
            counts.pads > list.capacity)
            return in->fail("a sighting list is longer than its storage");
        list.seen_count = counts.seen;
        list.radar_count = counts.radar;
        list.pad_count = counts.pads;
        list.refreshed_tick = counts.refreshed_tick;
        list.radar_fallback = counts.radar_fallback;
        if (!in->bytes(list.seen, counts.seen * sizeof(uint16_t)) ||
            !in->bytes(list.radar, counts.radar * sizeof(uint16_t)) ||
            !in->bytes(list.pads, counts.pads * sizeof(uint16_t)))
            return false;
        uint32_t known = 0;
        if (!in->value(&known))
            return false;
        if (known > slots)
            return in->fail("a seen list is longer than the unit pool");
        if (match.seen_before_room_[player] < known) {
            match.seen_before_[player] = std::make_unique<sim::detection::SightedUnit[]>(known);
            match.seen_before_room_[player] = known;
        }
        match.seen_before_count_[player] = known;
        if (!in->bytes(
                match.seen_before_[player].get(), known * sizeof(sim::detection::SightedUnit)
            ))
            return false;
    }
    uint8_t tables = 0;
    if (!in->value(&tables))
        return false;
    if (tables != match.rule_state_.count)
        return in->fail("a keyframe holds other rule-state tables than the match");
    for (uint8_t index = 0; index < tables; ++index) {
        const auto& table = match.rule_state_.tables[index];
        uint8_t length = 0;
        if (!in->value(&length))
            return false;
        std::array<char, 256> name{};
        if (!in->bytes(name.data(), length))
            return false;
        uint32_t size = 0;
        if (!in->list_count(&size, std::size_t{1} << 26))
            return false;
        auto bytes = std::make_unique<uint8_t[]>(size);
        if (!in->bytes(bytes.get(), size))
            return false;
        if (std::string_view(name.data(), length) != table.name)
            return in->fail("a keyframe holds another rule-state table than", table.name);
        if (table.restore != nullptr &&
            !table.restore(table.context, std::span<const uint8_t>(bytes.get(), size)))
            return in->fail("a rule-state table does not fit", table.name);
    }
    return in->ok();
}

keyframe::Census Match::keyframe_census() const {
    return KeyframeAccess::census(*this);
}

const char* Match::write_keyframe(keyframe::Writer* out) const {
    return KeyframeAccess::write(*this, out);
}

bool Match::read_keyframe(keyframe::Reader* in) {
    return KeyframeAccess::read(*this, in);
}

} // namespace oa::sim::match_runtime
