// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/rand15.hpp"

namespace oa::boneyards::screens {
namespace {

constexpr uint32_t kRand15Multiplier = 0x343fd;
constexpr uint32_t kRand15Increment = 0x269ec3;
constexpr int32_t kRand15Shift = 16;
constexpr uint32_t kRand15DefaultSeed = 1;

uint32_t g_holdrand = kRand15DefaultSeed;

} // namespace

int32_t rand15_step(uint32_t* holdrand) {
    *holdrand = *holdrand * kRand15Multiplier + kRand15Increment;
    return static_cast<int32_t>((*holdrand >> kRand15Shift) & static_cast<uint32_t>(kRand15Max));
}

void rand15_seed(uint32_t seed) {
    g_holdrand = seed;
}

int32_t rand15() {
    return rand15_step(&g_holdrand);
}

int32_t rand15_source(void*) {
    return rand15();
}

} // namespace oa::boneyards::screens
