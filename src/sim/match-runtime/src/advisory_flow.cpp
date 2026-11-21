// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "advisory_flow.hpp"
#include "advisory_flow_state.hpp"
#include "advisory_lane_codec.hpp"
#include "enhanced_state.hpp"
#include "live_route_corner.hpp"
#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/ground_orders/shared_routes.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <limits>

namespace oa::sim::match_runtime {
namespace {
namespace ground = sim::ground_orders;
constexpr size_t catalog_metadata_bytes = 320, identity_bytes = 16, frame_bytes = 96;
constexpr uint64_t catalog_storage_bound = 1'400'000;
constexpr uint32_t retained_word_bits = 64;
static_assert(sizeof(AdvisoryLaneState) <= 256 && sizeof(AdvisoryIdentity) <= 32);
static_assert(sizeof(AdvisoryFlowData) <= 65536);
static_assert(
    advisory_catalog_capacity * catalog_storage_bound + 32768ull * 640 + 65536 <
    advisory_storage_limit
);

bool overlaps(const ground::CorridorRectangle& a, const ground::CorridorRectangle& b) noexcept {
    return a.left <= b.right && b.left <= a.right && a.top <= b.bottom && b.top <= a.bottom;
}

void extend(ground::CorridorRectangle& to, const ground::CorridorRectangle& from) noexcept {
    to.left = std::min(to.left, from.left);
    to.top = std::min(to.top, from.top);
    to.right = std::max(to.right, from.right);
    to.bottom = std::max(to.bottom, from.bottom);
}

ground::CorridorRectangle proof_bounds(const ground::CorridorGeometry& geometry) noexcept {
    auto result = geometry.origins;
    extend(result, geometry.mouths[0]);
    extend(result, geometry.mouths[1]);
    if (geometry.axis == ground::CorridorAxis::x) {
        --result.top;
        ++result.bottom;
    } else {
        --result.left;
        ++result.right;
    }
    result.right = int16_t(result.right + geometry.footprint[0] - 1);
    result.bottom = int16_t(result.bottom + geometry.footprint[1] - 1);
    return result;
}

/// Rechecks the detector's complete static proof, including both transverse walls.
enum class AdvisoryProof : uint8_t { valid, invalid, deferred };

AdvisoryProof known_proof(
    const ground::CorridorGeometry& geometry,
    const ground::CorridorSampler& sampler,
    ground::CorridorBudget& budget
) noexcept {
    const bool along_x = geometry.axis == ground::CorridorAxis::x;
    const auto first = along_x ? geometry.origins.left : geometry.origins.top;
    const auto last = along_x ? geometry.origins.right : geometry.origins.bottom;
    const auto lower = along_x ? geometry.origins.top : geometry.origins.left;
    const auto upper = along_x ? geometry.origins.bottom : geometry.origins.right;
    const auto major_footprint = geometry.footprint[along_x ? 0 : 1];
    const auto columns = uint32_t(int32_t(upper) - lower + 3);
    const auto count = uint64_t(int32_t(last) - first + 1 + 2 * major_footprint) * columns;
    if (!sampler.origin)
        return AdvisoryProof::invalid;
    if (count > budget.remaining)
        return AdvisoryProof::deferred;
    const auto sample = [&](int16_t major, int16_t minor) {
        --budget.remaining;
        ++budget.used;
        return sampler.origin(
            sampler.context,
            along_x ? ground::CorridorOrigin{major, minor} : ground::CorridorOrigin{minor, major},
            geometry.footprint
        );
    };
    for (int32_t major = first; major <= last; ++major)
        for (int32_t minor = int32_t(lower) - 1; minor <= int32_t(upper) + 1; ++minor) {
            const auto expected = minor < lower || minor > upper ? ground::CorridorCell::blocked
                                                                 : ground::CorridorCell::clear;
            if (sample(int16_t(major), int16_t(minor)) != expected)
                return AdvisoryProof::invalid;
        }
    for (unsigned mouth = 0; mouth < 2; ++mouth)
        for (int32_t row = 0; row < major_footprint; ++row) {
            const auto major = int16_t(mouth == 0 ? first - 1 - row : last + 1 + row);
            bool extension = false;
            for (int32_t minor = int32_t(lower) - 1; minor <= int32_t(upper) + 1; ++minor) {
                const auto value = sample(major, int16_t(minor));
                const bool outside = minor < lower || minor > upper;
                if (value == ground::CorridorCell::unknown ||
                    (!outside && value != ground::CorridorCell::clear))
                    return AdvisoryProof::invalid;
                extension |= outside && value == ground::CorridorCell::clear;
            }
            if (!extension)
                return AdvisoryProof::invalid;
        }
    return AdvisoryProof::valid;
}

uint64_t goal_key(const ground::Goal& goal) noexcept {
    uint64_t result = 14695981039346656037ull;
    const auto add = [&result](uint32_t value) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            result ^= uint8_t(value >> (byte * 8));
            result *= 1099511628211ull;
        }
    };
    add(uint32_t(goal.shape));
    add(uint16_t(goal.cell[0]));
    add(uint16_t(goal.cell[1]));
    for (const auto value :
         {goal.tolerance,
          goal.radius_squared,
          goal.inner_range,
          goal.inner_radius_squared,
          goal.left,
          goal.right,
          goal.top,
          goal.bottom})
        add(uint32_t(value));
    return result;
}

bool enrollment_due(const AdvisoryFlowData& state, uint16_t unit) noexcept {
    if (unit == 0 || unit >= state.unit_count || state.unit_count <= 1)
        return false;
    return unit >= state.enrollment_begin &&
           uint32_t(unit) - state.enrollment_begin < advisory_enrollment_slots;
}

void retained_membership(AdvisoryFlowData& state, uint16_t unit, bool retained) noexcept {
    if (unit == 0 || unit >= state.unit_count)
        return;
    auto& word = state.retained_units[unit / retained_word_bits];
    const auto bit = uint64_t{1} << (unit % retained_word_bits);
    if (retained)
        word |= bit;
    else
        word &= ~bit;
}

/// Finds a retained unit in native slot order, wrapping without scanning world records.
uint32_t next_retained(const AdvisoryFlowData& state, uint32_t first) noexcept {
    const auto find = [&](uint32_t begin, uint32_t end) -> uint32_t {
        for (uint32_t at = begin; at < end;) {
            const auto index = at / retained_word_bits;
            auto word = state.retained_units[index];
            word &= std::numeric_limits<uint64_t>::max() << (at % retained_word_bits);
            if (word) {
                const auto unit = index * retained_word_bits + std::countr_zero(word);
                if (unit < end)
                    return unit;
            }
            at = (index + 1) * retained_word_bits;
        }
        return 0;
    };
    first = first > 0 && first < state.unit_count ? first : 1;
    if (const auto unit = find(first, uint32_t(state.unit_count)))
        return unit;
    return find(1, first);
}
} // namespace

void rebuild_advisory_membership(AdvisoryFlowData& state) noexcept {
    std::fill_n(state.retained_units.get(), state.retained_word_count, uint64_t{0});
    for (uint32_t unit = 1; unit < state.unit_count; ++unit)
        retained_membership(
            state, uint16_t(unit), state.guides[unit].phase != AdvisoryLanePhase::none
        );
}

void Match::initialize_advisory_flow() {
    if (!enhanced_ || !enhanced_features_.has(data::enhanced_mode::Feature::corridor_lanes))
        return;
    auto state = std::make_unique<AdvisoryFlowData>();
    state->unit_count = slots_.size();
    for (auto& player : state->catalog_indices)
        player.fill(-1);
    state->guides = std::make_unique<AdvisoryLaneState[]>(state->unit_count);
    state->identities = std::make_unique<AdvisoryIdentity[]>(state->unit_count);
    state->published_guides =
        std::make_unique<sim::unit_movement::LocalFlowHead[]>(state->unit_count);
    state->retained_word_count = (state->unit_count + retained_word_bits - 1) / retained_word_bits;
    state->retained_units = std::make_unique<uint64_t[]>(state->retained_word_count);
    uint64_t storage = sizeof(AdvisoryFlowData) +
                       state->unit_count * (sizeof(AdvisoryLaneState) + sizeof(AdvisoryIdentity) +
                                            sizeof(sim::unit_movement::LocalFlowHead) +
                                            sim::unit_movement::local_flow_head_bytes +
                                            advisory_lane_record_bytes + identity_bytes) +
                       frame_bytes + state->retained_word_count * sizeof(uint64_t);
    const auto mask = input_.unit_player_mask;
    for (uint32_t klass = 0; klass < movement_classes_.size(); ++klass) {
        const auto& movement = movement_classes_[klass];
        if (!movement.map || movement.record.footprint_x <= 0 || movement.record.footprint_z <= 0 ||
            movement.record.footprint_x > 8 || movement.record.footprint_z > 8)
            continue;
        for (uint32_t player = 0; player < OA_PLAYER_COUNT; ++player) {
            if ((mask != 0 && (mask & (1u << player)) == 0) ||
                state->catalog_count == state->catalogs.size())
                continue;
            auto catalog = std::make_unique<ground::CorridorCatalog>(
                spatial_.terrain_width,
                spatial_.terrain_height,
                ground::CorridorOrigin{movement.record.footprint_x, movement.record.footprint_z}
            );
            if (!catalog->status().enabled)
                continue;
            const auto bytes =
                catalog->allocated_bytes() + catalog->saved_size() + catalog_metadata_bytes;
            if (bytes > catalog_storage_bound) {
                fault_.note("Enhanced corridor allocation exceeds its fixed class bound");
                return;
            }
            storage += bytes;
            auto& slot = state->catalogs[state->catalog_count];
            slot.match = this;
            slot.player = player;
            slot.movement_class = klass;
            slot.catalog = std::move(catalog);
            state->catalog_indices[player][klass] = int16_t(state->catalog_count++);
            slot.catalog->reset(slot.revision);
            // Initial index preparation belongs to construction, before any simulation tick.
            ground::CorridorBudget initial{std::numeric_limits<uint32_t>::max(), 0};
            slot.catalog->advance(
                initial, {nullptr, [](void*, ground::CorridorOrigin, ground::CorridorOrigin) {
                              return ground::CorridorCell::unknown;
                          }}
            );
        }
    }
    state->save_size = advisory_flow_saved_size(*state);
    state->save_bytes = std::make_unique<uint8_t[]>(state->save_size);
    state->allocated_bytes = storage;
    if (state->save_size == 0 || storage > advisory_storage_limit) {
        fault_.note("Enhanced corridor storage exceeds its construction bound");
        return;
    }
    enhanced_->advisory = std::move(state);
    if (!add_rule_state(
            rule_state_,
            {"advisory-flow",
             this,
             [](void* context) { return static_cast<Match*>(context)->advisory_state_bytes(); },
             [](void* context, std::span<const uint8_t> bytes) {
                 return static_cast<Match*>(context)->restore_advisory_state(bytes);
             },
             false}
        ))
        fault_.note("Enhanced corridor state could not be registered");
}

ground::CorridorSampler Match::advisory_sampler(void* catalog) noexcept {
    return {
        catalog,
        [](void* context, ground::CorridorOrigin origin, ground::CorridorOrigin footprint) {
            auto& slot = *static_cast<AdvisoryCatalog*>(context);
            auto& match = *slot.match;
            const auto& klass = match.movement_classes_[slot.movement_class];
            if (!klass.map || slot.player >= OA_PLAYER_COUNT ||
                footprint !=
                    ground::CorridorOrigin{klass.record.footprint_x, klass.record.footprint_z} ||
                origin[0] < 0 || origin[1] < 0 ||
                uint32_t(origin[0]) + uint32_t(footprint[0]) >= match.spatial_.terrain_width ||
                uint32_t(origin[1]) + uint32_t(footprint[1]) >= match.spatial_.terrain_height)
                return ground::CorridorCell::blocked;
            ++match.enhanced_->local_flow_counters.probes;
            if (slot.recording_samples) {
                const ground::CorridorRectangle bounds{
                    origin[0],
                    origin[1],
                    int16_t(origin[0] + footprint[0] - 1),
                    int16_t(origin[1] + footprint[1] - 1)
                };
                if (slot.sampled_valid)
                    extend(slot.sampled, bounds);
                else
                    slot.sampled = bounds;
                slot.sampled_valid = true;
            }
            // player_bits is the monotonic ever-seen map, not transient current sight.
            const auto width = match.spatial_.terrain_width >> 1;
            const auto height = match.spatial_.terrain_height >> 1;
            for (int32_t z = 0; z < footprint[1]; ++z)
                for (int32_t x = 0; x < footprint[0]; ++x) {
                    const auto sx = uint32_t(origin[0] + x) >> 1;
                    const auto sz = uint32_t(origin[1] + z) >> 1;
                    const auto at = size_t(sz) * width + sx;
                    if (sx >= width || sz >= height || at >= match.sight_.player_bits.size() ||
                        (match.sight_.player_bits[at] & (1u << slot.player)) == 0)
                        return ground::CorridorCell::unknown;
                }
            return ground::static_flow_passable(klass.record, origin[0], origin[1], match.spatial_)
                       ? ground::CorridorCell::clear
                       : ground::CorridorCell::blocked;
        }
    };
}

void Match::reset_advisory_unit(uint16_t unit) noexcept {
    auto* state = enhanced_ ? enhanced_->advisory.get() : nullptr;
    if (!state || unit >= state->unit_count)
        return;
    if (state->published_guides[unit].active)
        if (auto* movement = ground_runtime(unit))
            movement->navigation.flags |= ground::route_changed_flag;
    state->guides[unit] = {};
    retained_membership(*state, unit, false);
    state->published_guides[unit] = {};
    auto& identity = state->identities[unit];
    const auto generation = identity.generation + 1;
    identity = {};
    identity.generation = generation;
}

void Match::forget_advisory_order(
    uint16_t unit, const sim::simulation_state::Order* order
) noexcept {
    auto* state = enhanced_ ? enhanced_->advisory.get() : nullptr;
    if (!state || unit >= state->unit_count)
        return;
    const auto& guide = state->guides[unit];
    const auto* movement = ground_runtime(unit);
    const bool pending = state->identities[unit].rebind && guide.phase != AdvisoryLanePhase::none &&
                         movement && movement->navigation.goal &&
                         movement->navigation.goal->order == order;
    if (guide.order == order || pending)
        reset_advisory_unit(unit);
}

void Match::advisory_static_change(std::array<int16_t, 2> cell, std::array<int16_t, 2> footprint) {
    auto* state = enhanced_ ? enhanced_->advisory.get() : nullptr;
    if (!state || footprint[0] <= 0 || footprint[1] <= 0)
        return;
    const ground::CorridorRectangle changed{
        cell[0], cell[1], int16_t(cell[0] + footprint[0] - 1), int16_t(cell[1] + footprint[1] - 1)
    };
    for (uint32_t at = 0; at < state->catalog_count; ++at) {
        auto& slot = state->catalogs[at];
        bool affected = slot.sampled_valid && overlaps(changed, slot.sampled);
        for (uint16_t id = 1; !affected && id <= slot.catalog->status().corridors; ++id)
            if (const auto* geometry = slot.catalog->geometry(id))
                affected = overlaps(changed, proof_bounds(*geometry));
        if (affected) {
            slot.catalog->reset(++slot.revision);
            slot.sampled = {};
            slot.sampled_valid = false;
            slot.validated.fill(false);
        }
    }
}

void Match::EnhancedState::prepare_advisory_tick(Match& match) {
    auto* state = advisory.get();
    if (!state)
        return;
    if (!state->tick_valid || state->tick != match.simulation_.tick) {
        state->tick_valid = true;
        state->tick = match.simulation_.tick;
        state->construction = {std::min(advisory_construction_work, state->budget_override), 0};
        state->query = {std::min(advisory_query_work, state->budget_override), 0};
        state->active = {std::min(advisory_active_work, state->budget_override), 0};
        auto next_active = next_retained(*state, state->active_cursor);
        // A unit whose owner no longer runs callbacks must not pin the service
        // cursor. Each stale member is removed once under the shared work cap.
        while (next_active && state->construction.remaining) {
            --state->construction.remaining;
            ++state->construction.used;
            const auto& unit = match.slots_[next_active];
            const auto* ground = match.ground_runtime(uint16_t(next_active));
            const bool eligible = unit.unit && unit.record.type_index != 0 &&
                                  (unit.record.flags & OA_UNIT_FLAG_LIVE) != 0 &&
                                  sim::simulation_state::locally_simulated(*unit.unit) && ground &&
                                  !ground->mirrored_driver && unit.record.attach_parent == 0 &&
                                  match.air_drivers_[next_active].unit == nullptr;
            if (eligible)
                break;
            match.reset_advisory_unit(uint16_t(next_active));
            next_active = next_retained(*state, next_active);
        }
        state->active_begin = state->active_cursor = next_active ? next_active : 1;
        state->active_stopped = false;
        state->enrollment_begin = state->enrollment_next;
        if (state->unit_count > 1) {
            const auto after_window = state->enrollment_begin + advisory_enrollment_slots;
            state->enrollment_next = after_window < state->unit_count ? after_window : 1;
        }
        uint32_t idle = 0;
        while (state->construction.remaining && state->catalog_count &&
               idle < state->catalog_count) {
            auto& slot = state->catalogs[state->catalog_cursor];
            state->catalog_cursor = (state->catalog_cursor + 1) % state->catalog_count;
            const auto before = slot.catalog->status();
            if (!before.building && !before.clearing && before.pending_seeds == 0) {
                ++idle;
                continue;
            }
            idle = 0;
            ground::CorridorBudget one{1, 0};
            slot.recording_samples = true;
            slot.catalog->advance(one, match.advisory_sampler(&slot));
            slot.recording_samples = false;
            state->construction.remaining -= one.used;
            state->construction.used += one.used;
            const auto after = slot.catalog->status();
            if (after.corridors != before.corridors ||
                after.rejected_seeds != before.rejected_seeds || !after.building) {
                slot.sampled = {};
                slot.sampled_valid = false;
            }
        }
    }
}

bool Match::advisory_navigation(
    sim::unit_spawn::Slot& unit,
    ground::GroundRuntime& movement,
    std::array<uint8_t, 3>& weapon_flags,
    ground::Navigation& steering
) {
    auto* state = enhanced_ ? enhanced_->advisory.get() : nullptr;
    if (!state || unit.unit_index >= state->unit_count)
        return false;
    auto& guide = state->guides[unit.unit_index];
    auto& published = state->published_guides[unit.unit_index];
    auto& navigation = movement.navigation;
    const auto previous_published = published;
    published = {};
    const auto ordinary_fallback = [&](bool retain) {
        const bool was_steering = guide.steering_active;
        if (retain)
            guide.steering_active = false;
        else
            guide = {};
        retained_membership(*state, unit.unit_index, guide.phase != AdvisoryLanePhase::none);
        if (was_steering || previous_published.active) {
            auto& head = enhanced_->local_flow_heads[unit.unit_index];
            if (head.active || previous_published.active)
                navigation.flags |= ground::route_changed_flag;
            head.active = false;
            head.count = 0;
        }
        steering = navigation;
        return false;
    };
    const bool eligible = unit.unit && sim::simulation_state::locally_simulated(*unit.unit) &&
                          !movement.mirrored_driver && unit.record.attach_parent == 0 &&
                          air_drivers_[unit.unit_index].unit == nullptr &&
                          (movement.movement.flags & sim::unit_movement::occupancy_mask) == 1;
    if (!eligible)
        return ordinary_fallback(false);
    enhanced_->prepare_advisory_tick(*this);
    const auto* klass = movement_class_map(movement.movement_class);
    const auto player = unit.record.owner_index;
    const auto class_index =
        klass ? size_t(klass - movement_classes_.data()) : movement_classes_.size();
    const auto catalog_index = player < OA_PLAYER_COUNT && class_index < movement_classes_.size()
                                   ? state->catalog_indices[player][class_index]
                                   : int16_t{-1};
    if (catalog_index < 0 ||
        movement.geometry.footprint !=
            ground::CorridorOrigin{klass->record.footprint_x, klass->record.footprint_z}) {
        return ordinary_fallback(false);
    }
    auto& slot = state->catalogs[size_t(catalog_index)];
    auto sampler = advisory_sampler(&slot);
    auto& identity = state->identities[unit.unit_index];
    bool identity_allowed = true;
    if (guide.phase != AdvisoryLanePhase::none) {
        identity_allowed = navigation.goal && navigation.goal->order &&
                           identity.unit_type == unit.record.type_index &&
                           identity.owner == player &&
                           identity.order_issue_tick == navigation.goal->order->issue_tick &&
                           identity.order_kind == navigation.goal->order->kind &&
                           guide.goal_geometry == goal_key(*navigation.goal);
        if (!identity_allowed)
            ++identity.generation;
        if (identity.rebind && identity_allowed) {
            guide.goal = navigation.goal;
            guide.order = navigation.goal->order;
        }
    }
    identity.rebind = false;
    const bool retained = guide.phase != AdvisoryLanePhase::none;
    const auto* retained_geometry = retained ? slot.catalog->geometry(guide.corridor.id) : nullptr;
    if (retained &&
        (!identity_allowed || guide.goal != navigation.goal || !navigation.goal ||
         guide.order != navigation.goal->order || guide.generation != identity.generation ||
         guide.corridor.footprint != movement.geometry.footprint ||
         ground::goal_contains_unit(*navigation.goal, movement.geometry)))
        return ordinary_fallback(false);
    const auto corridor_axis = guide.corridor.axis == ground::CorridorAxis::x ? 0u : 1u;
    const auto corridor_last =
        corridor_axis == 0 ? guide.corridor.plots.right : guide.corridor.plots.bottom;
    const auto corridor_first =
        corridor_axis == 0 ? guide.corridor.plots.left : guide.corridor.plots.top;
    const bool completed = retained && guide.entered &&
                           (guide.direction == ground::CorridorDirection::increasing
                                ? movement.geometry.cell[corridor_axis] > corridor_last
                                : int32_t(movement.geometry.cell[corridor_axis]) +
                                          movement.geometry.footprint[corridor_axis] - 1 <
                                      corridor_first);
    if (retained && !completed &&
        (!retained_geometry || retained_geometry->revision != guide.corridor.revision ||
         retained_geometry->footprint != guide.corridor.footprint))
        return ordinary_fallback(false);
    const bool active_service = retained && !state->active_stopped &&
                                unit.unit_index >= state->active_begin && state->active.remaining;
    if (retained && !completed && !active_service)
        return ordinary_fallback(true);
    const bool full_allowance = retained && state->active.used == 0;
    const auto advance_service = [&] {
        const auto next = next_retained(*state, uint32_t(unit.unit_index) + 1);
        state->active_cursor = next ? next : 1;
    };
    const auto budget_deferred = [&] {
        if (retained) {
            if (full_allowance) {
                advance_service();
                if (state->active.remaining == 0)
                    state->active_stopped = true;
            } else {
                state->active_cursor = unit.unit_index;
                state->active_stopped = true;
            }
        }
        return ordinary_fallback(retained);
    };
    const bool enroll = enrollment_due(*state, unit.unit_index);
    if (!retained && enroll && state->query.remaining && navigation.goal && navigation.count >= 2 &&
        ground::route_present(navigation)) {
        // Resume after the last serviced slot, rather than starving the tail
        // when earlier units spend the shared allowance before the window ends.
        state->enrollment_next = static_cast<std::size_t>(unit.unit_index + 1) < state->unit_count
                                     ? uint32_t(unit.unit_index) + 1
                                     : 1;
        const auto& point = navigation.points[1];
        live_corner_detail::Segment segment{};
        bool valid = true;
        for (unsigned axis = 0; axis < 2; ++axis) {
            const auto coordinate = axis * 2;
            valid &= live_corner_detail::origin(
                movement.geometry.position[coordinate],
                movement.geometry.footprint[axis],
                segment.from[axis],
                segment.first[axis]
            );
            valid &= live_corner_detail::origin(
                int64_t(point[axis]) * 65536,
                movement.geometry.footprint[axis],
                segment.to[axis],
                segment.last[axis]
            );
        }
        if (valid) {
            const auto axis = std::abs(segment.to[0] - segment.from[0]) >=
                                      std::abs(segment.to[1] - segment.from[1])
                                  ? ground::CorridorAxis::x
                                  : ground::CorridorAxis::z;
            uint32_t visited{};
            (void)live_corner_detail::visit_origins(
                segment,
                16,
                [&](int16_t x, int16_t z) {
                    if (!state->query.remaining)
                        return false;
                    --state->query.remaining;
                    ++state->query.used;
                    (void)slot.catalog->request({x, z}, axis);
                    return true;
                },
                visited
            );
        }
    }
    ground::CorridorBudget no_release_work{};
    auto& budget = completed && !active_service ? no_release_work
                   : retained                   ? state->active
                                                : state->query;
    std::array<ground::CorridorApproach, 64> contacts{};
    ground::CorridorQuery query{};
    if (!completed && (retained || enroll) && !(retained && guide.entered) &&
        slot.catalog->status().corridors && budget.remaining)
        query = slot.catalog->upcoming(movement.geometry, navigation, contacts, budget);
    if (!completed && retained && !guide.entered && !query.complete && budget.remaining == 0)
        return budget_deferred();
    const ground::CorridorApproach* approach = nullptr;
    const ground::CorridorGeometry* geometry = nullptr;
    if (retained)
        geometry = retained_geometry;
    if (query.complete)
        for (uint32_t at = 0; at < query.count; ++at) {
            const auto& contact = contacts[at];
            const auto* candidate = slot.catalog->geometry(contact.id);
            if (candidate &&
                ((retained && contact.id == guide.corridor.id) ||
                 (!retained && contact.exits_other_end && candidate->lane_count == 2))) {
                approach = &contact;
                geometry = candidate;
                break;
            }
        }
    if (!completed && geometry) {
        const auto at = size_t(geometry->id - 1);
        if (!slot.validated[at] || slot.validated_tick[at] != simulation_.tick) {
            const auto proof = known_proof(*geometry, sampler, budget);
            if (proof == AdvisoryProof::deferred)
                return budget_deferred();
            slot.validated[at] = proof == AdvisoryProof::valid;
            slot.validated_tick[at] = simulation_.tick;
        }
        if (!slot.validated[at])
            geometry = nullptr;
    }
    if (!identity_allowed)
        geometry = nullptr;
    // Release uses the retained departure plane without granting stale geometry
    // any steering rights, including when a static edit just invalidated the catalog.
    const auto departed_geometry = guide.corridor;
    if (completed)
        geometry = &departed_geometry;
    const auto prior_guide = guide;
    const auto helper_work = budget.used;
    const auto decision = choose_advisory_lane(
        movement.geometry,
        navigation,
        identity.generation,
        approach,
        geometry,
        sampler,
        budget,
        guide
    );
    if (retained && !completed && !decision.active && !decision.released &&
        guide.phase != AdvisoryLanePhase::none && budget.used == helper_work) {
        guide = prior_guide;
        return budget_deferred();
    }
    if (!retained && !decision.active)
        guide = {};
    retained_membership(*state, unit.unit_index, guide.phase != AdvisoryLanePhase::none);
    if (active_service)
        advance_service();
    if (guide.phase != AdvisoryLanePhase::none && navigation.goal && navigation.goal->order) {
        identity.unit_type = unit.record.type_index;
        identity.owner = player;
        identity.order_issue_tick = navigation.goal->order->issue_tick;
        identity.order_kind = navigation.goal->order->kind;
    }
    if (decision.reset_local_head) {
        auto& head = enhanced_->local_flow_heads[unit.unit_index];
        if (head.active)
            navigation.flags |= ground::route_changed_flag;
        head.active = false;
        head.count = 0;
    }
    if (decision.released) {
        const auto retry = navigation.last_search_tick;
        search_.controller.cancel(navigation);
        const bool failed =
            decision.rejoin_count == 0 ||
            ground::accept_path(
                {*unit.unit, movement.geometry, movement.movement, navigation, false, weapon_flags},
                {decision.rejoin.data(), decision.rejoin_count}
            );
        if (failed) {
            ground::advance_path(navigation, navigation.count);
            navigation.flags |= ground::search_pending_flag | ground::route_changed_flag;
        }
        navigation.last_search_tick = retry;
        steering = navigation;
    } else
        steering = decision.steering;
    if (decision.active) {
        std::copy_n(decision.steering.points.begin(), 3, published.points.begin());
        if (previous_published.active && !decision.reset_local_head &&
            previous_published.points[1] == published.points[1] &&
            previous_published.points[2] == published.points[2])
            published.points[0] = previous_published.points[0];
        published.count = 3;
        published.active = true;
        published.goal_key = guide.goal_geometry;
        published.tick = simulation_.tick;
        steering.points[0] = published.points[0];
    }
    if (previous_published.active != published.active ||
        (published.active && (previous_published.points != published.points ||
                              previous_published.count != published.count)))
        navigation.flags |= ground::route_changed_flag;
    return decision.active;
}

} // namespace oa::sim::match_runtime
