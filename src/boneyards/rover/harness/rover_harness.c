// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

/* Loads a Rover DLL the way the game does, asks it for the interface table and
 * runs the slot 0 version handshake. Works against the 3.1c library and the
 * one built here alike.
 *
 *   rover-harness [path-to-dll]
 *
 * Exit status 0 means the DLL accepted the game's version pair. */
#include "oa/rover/interface.h"

#include <stdio.h>
#include <windows.h>

typedef void(ROVER_CALL* GetInterfaceFn)(const RoverInterface** out);

/* The pairs the handshake truth table pins down, the game's own first. */
static const struct {
    int32_t iface_version;
    int32_t build_stamp;
} kProbes[] = {
    {OA_ROVER_INTERFACE_VERSION, OA_ROVER_MINIMUM_BUILD_STAMP},
    {OA_ROVER_INTERFACE_VERSION, OA_ROVER_MINIMUM_BUILD_STAMP - 1},
    {OA_ROVER_INTERFACE_VERSION - 1, INT32_MAX},
    {OA_ROVER_INTERFACE_VERSION, INT32_MIN},
};

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "Rover";
    HMODULE module = LoadLibraryA(path);
    if (!module) {
        fprintf(stderr, "LoadLibrary(%s) failed: %lu\n", path, (unsigned long)GetLastError());
        return 2;
    }
    GetInterfaceFn get_interface =
        (GetInterfaceFn)(void (*)(void))GetProcAddress(module, "_GetInterface@4");
    if (!get_interface) {
        fprintf(stderr, "%s does not export _GetInterface@4\n", path);
        FreeLibrary(module);
        return 3;
    }
    const RoverInterface* table = NULL;
    get_interface(&table);
    printf("table %p\n", (const void*)table);
    if (!table) {
        FreeLibrary(module);
        return 4;
    }
    int32_t accepted = 0;
    for (size_t i = 0; i < sizeof kProbes / sizeof kProbes[0]; ++i) {
        int32_t result = table->check_version(kProbes[i].iface_version, kProbes[i].build_stamp);
        printf(
            "check_version(0x%x, 0x%x) = %d\n",
            (unsigned)kProbes[i].iface_version,
            (unsigned)kProbes[i].build_stamp,
            (int)result
        );
        if (i == 0)
            accepted = result & 0xff;
    }
    FreeLibrary(module);
    return accepted ? 0 : 1;
}
