// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The random sequence the game draws from: the linear congruential
// generator x = x * 214013 + 2531011, read as bits 16-30 (15 bits). The game
// shares one stream (the Recon wind and the chat default-room pick draw from
// it); callers that keep their own seeded state step it with rand15_step.
#pragma once

#include <cstdint>

namespace oa::boneyards::screens {

inline constexpr int32_t kRand15Max = 0x7fff;

/// Advances a generator state one step.
///
/// @param[in,out] holdrand generator state
/// @return the next value, 0..kRand15Max
int32_t rand15_step(uint32_t* holdrand);

/// Seeds the process stream; before any seed it starts from 1.
///
/// @param seed new generator state
void rand15_seed(uint32_t seed);

/// Draws the next value of the process stream.
///
/// @return the next value, 0..kRand15Max
int32_t rand15();

/// Draws from the process stream for random-source hooks.
///
/// @param user ignored
/// @return the next value, 0..kRand15Max
int32_t rand15_source(void* user);

} // namespace oa::boneyards::screens
