// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tick_internal.hpp"

#include "oa/sim/ai.hpp"

#include <array>
#include <cstddef>
#include <memory>

namespace oa::sim::match_runtime {

namespace {
// Owner of an economy block as the income scaling reads it.
struct CreditOwner {
    bool present{};
    uint8_t status{};
};

// Difficulty a computer player's income uses. A mirrored owner keeps the
// game's difficulty: only the machine that runs the computer player scales
// its income, and the others copy the units.
int32_t income_difficulty(
    const oa::World& world, const std::shared_ptr<void>& computers, const oa::Player* owner
) {
    const auto fallback = world.game.difficulty;
    if (owner == nullptr || owner->in_use == 0 || owner->status != OA_PLAYER_STATUS_COMPUTER)
        return fallback;
    const auto* begin = world.game.players;
    const auto* end = begin + OA_PLAYER_COUNT;
    if (owner < begin || owner >= end || !computers)
        return fallback;
    return oa::sim::ai::computer_player_difficulty(
        static_cast<const oa::sim::ai::ComputerPlayers*>(computers.get()),
        static_cast<uint8_t>(owner - begin),
        fallback
    );
}

CreditOwner credit_owner(const oa::World& world, const oa::UnitEconomy& block) {
    const auto* owner = oa::world_player_ref(&world, block.player);
    return owner ? CreditOwner{owner->in_use != 0, owner->status} : CreditOwner{};
}

// Scales the energy and metal accumulators of one economy block by the
// player's supply and stock ratios.
void settle_economy_block(oa::UnitEconomy& block, const std::array<float, 10>& ratios) {
    auto words = economy_words(block);
    for (size_t resource = 0; resource < 2; ++resource) {
        const auto first = resource * metal_block_word;
        float values[6];
        for (size_t i = 0; i < 6; ++i)
            values[i] = std::bit_cast<float>(words[first + i]);
        sim::unit_health::scale_resource_block(values, ratios[8 + resource], ratios[6 + resource]);
        for (size_t i = 0; i < 6; ++i)
            words[first + i] = std::bit_cast<uint32_t>(values[i]);
    }
    store_economy_words(block, words);
}

// Records what one settlement paid out (Match::settled_spending), energy
// then metal. `stores_at_entry` is each store as the settlement began,
// `stores_pooled` as it went into the pool after the cloaks were paid, and
// `pool_paid` what the pool then paid toward the gated and accepted requests.
// Between two settlements a store only falls, by what was paid at once
// (shots, cloaks, gifts); a store that rose was set from outside the economy
// and paid nothing. A settlement with no stores left before it counts only
// the cloaks it paid itself.
void note_spending(
    SettledSpending& spending,
    const oa::Player& player,
    const std::array<float, 2>& stores_at_entry,
    const std::array<float, 2>& stores_pooled,
    const std::array<float, 2>& pool_paid
) noexcept {
    const std::array<float, 2> stores_since{
        spending.stores_left_known ? spending.energy_left : stores_at_entry[0],
        spending.stores_left_known ? spending.metal_left : stores_at_entry[1]
    };
    const auto paid_at_once = [&](size_t which_store) {
        return stores_since[which_store] > stores_pooled[which_store]
                   ? stores_since[which_store] - stores_pooled[which_store]
                   : 0.0F;
    };
    spending.energy = paid_at_once(0) + pool_paid[0];
    spending.metal = paid_at_once(1) + pool_paid[1];
    spending.energy_left = player.energy;
    spending.metal_left = player.metal;
    spending.stores_left_known = true;
}
} // namespace

void Match::credit_metal(
    oa::Unit& target, float amount, const sim::unit_health::ComputerIncomeScales* scales
) {
    const auto owner = credit_owner(state(), target.economy);
    auto& produced = target.economy.metal.produced;
    const auto difficulty = income_difficulty(
        state(), computer_player_state(), oa::world_player_ref(&state(), target.economy.player)
    );
    if (scales != nullptr)
        (void)sim::unit_health::credit_metal(
            produced, amount, owner.present, owner.status, difficulty, *scales
        );
    else
        (void)sim::unit_health::credit_metal(
            produced, amount, owner.present, owner.status, difficulty
        );
}

void Match::credit_energy(
    oa::Unit& target, float amount, const sim::unit_health::ComputerIncomeScales* scales
) {
    const auto owner = credit_owner(state(), target.economy);
    auto& produced = target.economy.energy.produced;
    const auto difficulty = income_difficulty(
        state(), computer_player_state(), oa::world_player_ref(&state(), target.economy.player)
    );
    if (scales != nullptr)
        (void)sim::unit_health::credit_energy(
            produced, amount, owner.present, owner.status, difficulty, *scales
        );
    else
        (void)sim::unit_health::credit_energy(
            produced, amount, owner.present, owner.status, difficulty
        );
}

void Match::update_player_economy(size_t player_index) {
    auto& world = state();
    auto& player = match_player(*this, player_index);
    const bool owner_present = player.in_use != 0;
    const auto owner_status = player.status;
    const auto difficulty = income_difficulty(world, computer_player_state(), &player);
    // Every production credit below scales a computer player's income alike,
    // energy or metal; a mod's income multipliers replace the scaling.
    const auto& income = rules().ai.income_multipliers;
    const auto* production = income.enabled ? &income.production : nullptr;
    const auto credit = [&](float& accumulator, float amount) {
        if (production != nullptr)
            (void)sim::unit_health::credit_metal(
                accumulator, amount, owner_present, owner_status, difficulty, *production
            );
        else
            (void)sim::unit_health::credit_metal(
                accumulator, amount, owner_present, owner_status, difficulty
            );
    };
    const bool cloak_after_build = rules().units.init_cloaked_after_build.enabled;
    // For settled_spending only; the settlement never reads it.
    const std::array<float, 2> stores_at_entry{player.energy, player.metal};

    player.metal_storage = 0.0F;
    player.energy_storage = 0.0F;

    // 0/1 energy/metal accepted, 2/3 gates, 4/5 requested then pools, 6/7
    // produced then stock ratios, 8/9 supply ratios.
    std::array<float, 10> totals{};

    const auto first = oa::oa_unit_slot_from_ref(player.first_unit);
    const auto last = oa::oa_unit_slot_from_ref(player.last_unit);
    for (auto index = first; player.first_unit != 0 && index <= last; ++index) {
        auto& unit = match_unit(*this, index);
        if ((unit.flags & OA_UNIT_FLAG_LIVE) == 0)
            continue;
        const auto& def = match_unit_def(*this, unit);
        auto& energy = unit.economy.energy;
        auto& metal = unit.economy.metal;
        const bool active = (unit.state_flags & active_state_bit) != 0;

        if ((unit.flags & OA_UNIT_FLAG_BUILDING) == 0) {
            if (active || (unit.flags & OA_UNIT_FLAG_MOVE_RATE_MASK) != 0) {
                const auto use = def.energy_use;
                if (use < 0.0F) {
                    credit(energy.produced, -use);
                } else {
                    energy.requested += use;
                    if (energy.gate <= 0.0F)
                        energy.accepted += use;
                }
            }
        } else if (active) {
            // A non-negative EnergyUse on an open gate powers the metal makers
            // and extractors; wind and tidal output is never gated.
            const auto use = def.energy_use;
            bool powered = false;
            if (use < 0.0F) {
                credit(energy.produced, -use);
            } else {
                energy.requested += use;
                powered = energy.gate <= 0.0F;
                if (powered)
                    energy.accepted += use;
            }
            if (def.extracts_metal > 0.0F) {
                if (powered)
                    credit(metal.produced, unit.extracted_metal);
            } else if (def.makes_metal != 0) {
                if (powered)
                    credit(
                        metal.produced, static_cast<float>(static_cast<uint8_t>(def.makes_metal))
                    );
            } else if (def.wind_generator > 0.0F) {
                credit(energy.produced, def.wind_generator * environment_wind_.normalized_strength);
            } else if (def.tidal_generator > 0.0F) {
                credit(energy.produced, def.tidal_generator * world.game.tidal_strength);
            }
        }

        if (unit.build_remaining == 0.0F) {
            credit(energy.produced, def.energy_make);
            credit(metal.produced, def.metal_make);
            player.metal_storage += def.metal_storage;
            player.energy_storage += def.energy_storage;
        }

        // Cloak upkeep is paid whole from the energy store; an
        // unpaid tick drops activation mask 4. Mirrored players skip it.
        if (!owner_present || owner_status != OA_PLAYER_STATUS_MIRRORED) {
            bool paid = false;
            // Under the cloak-after-build rule a unit with any build left
            // (any bit of its build fraction set, so -0 counts) does not cloak.
            const bool unfinished =
                cloak_after_build && std::bit_cast<uint32_t>(unit.build_remaining) != 0;
            if ((unit.flags & OA_UNIT_FLAG_CLOAK_RUNNING) != 0 &&
                (unit.flags & OA_UNIT_FLAG_CLOAK_LOCKED) == 0 && !unfinished &&
                unit.decloak_until_tick <= world.game.tick) {
                const auto cost = (unit.flags & OA_UNIT_FLAG_MOVE_RATE_MASK) != 0
                                      ? def.cloak_cost_moving
                                      : def.cloak_cost;
                paid = sim::unit_spawn::player_pay_energy(
                    player, energy.requested, static_cast<float>(truncate_low32(cost))
                );
            }
            set_activation(slots_[index], sim::unit_activation::cloaked_mask, paid);
        }

        // Re-read: the activation scripts may have changed the block.
        totals[6] += unit.economy.energy.produced;
        totals[4] += unit.economy.energy.requested;
        totals[0] += unit.economy.energy.accepted;
        totals[2] += unit.economy.energy.gate;
        totals[7] += unit.economy.metal.produced;
        totals[5] += unit.economy.metal.requested;
        totals[1] += unit.economy.metal.accepted;
        totals[3] += unit.economy.metal.gate;
    }

    // A player without a staging block settles into a spare one.
    oa::UnitEconomy spare_staging{};
    auto* staged = match_player_economy(*this, player_index);
    auto& staging = staged ? *staged : spare_staging;
    const auto energy_produced = staging.energy.produced + totals[6];
    const auto energy_requested = staging.energy.requested + totals[4];
    totals[0] += staging.energy.accepted;
    totals[2] += staging.energy.gate;
    totals[7] += staging.metal.produced;
    const auto metal_requested = staging.metal.requested + totals[5];
    totals[1] += staging.metal.accepted;
    totals[3] += staging.metal.gate;

    if ((player.resource_flags & 1) != 0) {
        player.energy_storage += player.shared_energy_storage;
        player.metal_storage += player.shared_metal_storage;
    }

    // The running totals are narrowed to float for the add, then widened.
    player.energy_produced = energy_produced;
    player.energy_requested = energy_requested;
    player.energy_produced_total =
        static_cast<double>(energy_produced + static_cast<float>(player.energy_produced_total));
    player.energy_requested_total =
        static_cast<double>(energy_requested + static_cast<float>(player.energy_requested_total));
    player.metal_produced = totals[7];
    player.metal_requested = metal_requested;
    player.metal_produced_total =
        static_cast<double>(totals[7] + static_cast<float>(player.metal_produced_total));
    player.metal_requested_total =
        static_cast<double>(metal_requested + static_cast<float>(player.metal_requested_total));

    totals[4] = energy_produced + player.energy;
    totals[5] = totals[7] + player.metal;
    // For settled_spending only: the stores pooled, and what the pool pays.
    const std::array<float, 2> stores_pooled{player.energy, player.metal};
    std::array<float, 2> pool_paid{};

    // Supply covers the gated requests first; the stock ratio then covers the
    // accepted requests from what is left.
    for (size_t resource = 0; resource < 2; ++resource) {
        const auto gate = totals[2 + resource];
        auto pool = totals[4 + resource];
        float supply = 1.0F;
        float taken = gate;
        if (pool < gate) {
            supply = pool / gate;
            taken = pool;
        }
        totals[8 + resource] = supply;
        pool -= taken;
        pool_paid[resource] = taken;
        const auto accepted = totals[resource];
        float stock = 1.0F;
        taken = accepted;
        if (pool < accepted) {
            stock = pool / accepted;
            taken = pool;
        }
        totals[6 + resource] = stock;
        totals[4 + resource] = pool - taken;
        pool_paid[resource] += taken;
    }

    player.energy = totals[4];
    if (player.energy_storage < totals[4]) {
        player.energy_wasted_total = static_cast<double>(
            (totals[4] - player.energy_storage) + static_cast<float>(player.energy_wasted_total)
        );
        player.energy = player.energy_storage;
    }
    player.metal = totals[5];
    if (player.metal_storage < totals[5]) {
        player.metal_wasted_total = static_cast<double>(
            (totals[5] - player.metal_storage) + static_cast<float>(player.metal_wasted_total)
        );
        player.metal = player.metal_storage;
    }
    if (player_index < settled_spending_.size())
        note_spending(
            settled_spending_[player_index], player, stores_at_entry, stores_pooled, pool_paid
        );

    for (auto index = first; player.first_unit != 0 && index <= last; ++index) {
        auto& unit = match_unit(*this, index);
        if ((unit.flags & OA_UNIT_FLAG_LIVE) != 0)
            settle_economy_block(unit.economy, totals);
    }
    // What the staging block took in this tick, from allies sharing, leaves
    // the produced totals; income and storage keep it. Each amount is widened
    // to double and subtracted at double precision, metal first.
    if (rules().economy.stats_exclude_shared_income.enabled) {
        player.metal_produced_total -= static_cast<double>(staging.metal.produced);
        player.energy_produced_total -= static_cast<double>(staging.energy.produced);
    }
    settle_economy_block(staging, totals);
}

} // namespace oa::sim::match_runtime
