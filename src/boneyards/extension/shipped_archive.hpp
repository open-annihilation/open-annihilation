// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// coreprime.ccx beside the program. The new screens' layouts live there.
// The war map does not: that file is the player's copy, the expansion's, or
// one in the game folder.

#pragma once

#include <cstdint>
#include <vector>

namespace oa::boneyards {

/// Reads one file from coreprime.ccx beside the program.
///
/// The archive is opened once. A missing archive, or a path it does not
/// hold, returns false and leaves `out` unchanged.
///
/// @param path relative path, with '/' or '\\' between its parts
/// @param[out] out receives the bytes
/// @return true when the archive held the file
bool read_shipped_archive(const char* path, std::vector<uint8_t>& out);

} // namespace oa::boneyards
