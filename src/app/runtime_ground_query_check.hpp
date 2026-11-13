// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

namespace oa::app::ground_query_check {

/// Windowless-check observations of the real initial ground-query caller.
/// No observation or fault simulation is active until begin() is called.
struct State {
    bool watching{};             ///< whether the dedicated native check collects observations
    bool fail_next{};            ///< simulate one false-result boundary without calling the API
    bool unchanged{true};        ///< every actual attempt preserved emitted and picking inputs
    bool retained{};             ///< the latest successful 3D frame has eligible retained geometry
    bool cache_valid{};          ///< the latest successful 3D frame has a valid query cache
    bool consumed_before_call{}; ///< the real once flag was set before the latest attempt
    uint64_t scenes{};           ///< actual new-match Scene initializations observed
    uint64_t destructions{};     ///< actual private-renderer releases observed
    uint64_t waiting_frames{};   ///< successful Enhanced frames waiting for retained geometry
    uint64_t attempts{};         ///< eligible once-only attempts, including boundary simulations
    uint64_t api_calls{};        ///< invocations of the real preparation API, including cache hits
    uint64_t successes{};        ///< real preparation API calls returning true
    uint64_t failures{};         ///< real preparation API calls returning false
    uint64_t simulated_failures{}; ///< caller-boundary simulations, never real API calls
    uint64_t index_attempts{};     ///< the latest Scene's actual index-construction attempts
    uint64_t last_attempt_scene{}; ///< the observed Scene lifetime of the latest attempt
    uint64_t ground_revision{};    ///< geometry revision of the latest successful 3D frame
    uint64_t query_revision{};     ///< revision stored in its index key, possibly stale
    uint64_t terrain_vertices{};   ///< vertices in that frame's actual ground prefix
};

/// Starts a dedicated native check with empty observations.
/// @param simulate_failure whether its next eligible attempt skips the API as a false-result simulation
void begin(bool simulate_failure = false) noexcept;

/// Stops observation and clears any unconsumed check-only fault request.
void end() noexcept;

/// Returns the current check's own counters, without reading a Runtime or World.
/// @return the observations retained by this calling thread
[[nodiscard]] State read() noexcept;

} // namespace oa::app::ground_query_check
