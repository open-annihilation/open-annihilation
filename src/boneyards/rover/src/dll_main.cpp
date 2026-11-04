// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Entry point of the drop-in Rover library.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "wire_text.h"

/// Succeeds for every notification.
///
/// The interface table is constant-initialised. Process attach selects Windows-1252 for text that
/// crosses the slot table, which is the encoding the game that loads this library speaks. Open
/// Annihilation calls the same slots without loading the library and leaves that mode clear.
///
/// @param instance module handle of the library; unused
/// @param reason DLL_PROCESS_ATTACH or another notification code
/// @param reserved loader context pointer; unused
/// @return TRUE
extern "C" BOOL WINAPI DllMain(HINSTANCE /*instance*/, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH)
        rover_set_interface_windows1252(true);
    return TRUE;
}
