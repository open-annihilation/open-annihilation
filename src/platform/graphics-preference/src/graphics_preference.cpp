// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The graphics card the game asks for on a Windows laptop with two: the
// discrete one. NVIDIA's drivers (Optimus) and AMD's (switchable graphics)
// start a program they have no profile for on the processor's own graphics,
// unless its executable exports one of these values set to 1. The player's
// own choice for the program, in Windows' graphics settings or in the
// driver's control panel, overrides it. The drivers read the values from the
// export table of the executable that started the process, so these are
// objects of open-annihilation.exe, never of a library; on a computer with
// one graphics card nothing reads them.
#include <cstdint>

extern "C" {

/// Read by NVIDIA's drivers, as a 32-bit unsigned value: 1 asks for the
/// NVIDIA graphics card.
__declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001;

/// Read by AMD's drivers: 1 asks for the high-performance graphics card.
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
