// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Loads a brain and runs fits with no match. The load steps are the ones
// seat_brain uses. start is not called, and the sandbox is closed before
// this returns, so nothing here is left on a match.

#include "oa/sim/brains/check.hpp"

#include "internal.hpp"

#include <memory>
#include <new>
#include <utility>

namespace oa::sim::brains {

SeatOutcome check_brain(const SeatSpec& spec, const oa::data::brain_facts::GameFacts& facts) {
    SeatOutcome outcome;
    outcome.result = SeatResult::failed;
    if (!package_acceptable(spec, outcome))
        return outcome;

    std::unique_ptr<Seat> created;
    try {
        created = std::make_unique<Seat>();
    } catch (const std::bad_alloc&) {
        write_reason(outcome.reason, "the seat could not be stored");
        return outcome;
    }
    if (!copy_spec_into(*created, spec)) {
        write_reason(outcome.reason, "the seat could not be stored");
        return outcome;
    }
    created->match = nullptr;
    created->facts = &facts;
    if (!build_networks(*created, spec, outcome))
        return outcome;
    if (!open_and_fit(*created, spec, outcome))
        return outcome;
    created->sandbox.reset();
    outcome.error = {};
    outcome.result = SeatResult::seated;
    return outcome;
}

} // namespace oa::sim::brains
