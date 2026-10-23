// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether a seated brain is the Classic brain this build was made with.
// The other Classic privileges test seat_is_classic and nothing else.
#pragma once

#include "oa/sim/brains/seat.hpp"

#include <array>
#include <span>

namespace oa::sim::brains {

/// The Classic fingerprint of a package's files.
///
/// One line per file, in byte order of the path, is
/// `<sha256 of the file in lower-case hex> <path>\n`. The fingerprint is the
/// SHA-256 of that text, written the same way. `.oa-origin.yaml` is left
/// out. The result is empty when no other file is present, or when the list
/// of paths cannot be stored.
///
/// @param files the package's files
/// @return 64 lower-case hex digits and a terminating NUL, or an empty string
[[nodiscard]] std::array<char, 65> classic_fingerprint(std::span<const PackageFile> files) noexcept;

/// Tells whether a seat is the Classic brain this engine was built with.
///
/// True only when that seat's files match the fingerprint of `brains/classic/`
/// baked into this build, and that fingerprint is not empty. The id, the
/// name, the manifest and the install path prove nothing. The other Classic
/// privileges must test this and nothing else.
///
/// @param match the match
/// @param player player index, 0 to 9
/// @return true when that seat is Classic
[[nodiscard]] bool seat_is_classic(const match_runtime::Match& match, uint8_t player) noexcept;

} // namespace oa::sim::brains
