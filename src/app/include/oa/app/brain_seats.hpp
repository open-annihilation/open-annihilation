// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// An installed brain read from the player's folder, and the seat a match
// asks for. The simulation never reads the package itself.
#pragma once

#include "oa/formats/oabrain/manifest.hpp"
#include "oa/sim/brains/seat.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

/// The folder of installed brains under the player's own folder.
inline constexpr std::string_view brains_folder_name = "Brains";

/// The most bytes of files one installed brain may hold, taken together.
inline constexpr uintmax_t installed_package_bytes = uintmax_t{128} << 20;

/// The brain, level and options one seat was given.
///
/// An empty `level` means the manifest's first level. An option that is
/// absent takes the manifest's default.
struct BrainSeatChoice {
    /// The brain's id, the folder name under `brains_folder_name`.
    std::string id;
    /// The level's id. Empty selects the manifest's first level.
    std::string level;
    /// Option id and the player's text, in the order they were given.
    /// A later pair with the same id replaces an earlier one.
    std::vector<std::pair<std::string, std::string>> options;
};

/// One brain read from disk. `files` holds every package file except the
/// installer's origin record, in byte order of the path.
struct InstalledBrain {
    /// The package folder.
    std::filesystem::path folder;
    /// The manifest.
    oa::formats::oabrain::Manifest manifest;
    /// Package path and bytes, path separators `/`.
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
};

/// One option resolved to the value a seat stores.
struct ResolvedOption {
    /// The option's id.
    std::string id;
    /// Which kind of value this is.
    oa::sim::brains::OptionKind kind{};
    /// A toggle's value.
    bool toggle{};
    /// A number's value, or a numeric choice's value when `text` is empty.
    double number{};
    /// A choice's text. Empty when the choice's value is `number`.
    std::string text;
};

/// One network the manifest names, with the digest text it stated.
struct ResolvedNetwork {
    /// The name a script will ask for.
    std::string id;
    /// Package path, such as `nets/scout.oann`.
    std::string path;
    /// 64 lower-case hex digits.
    std::string sha256;
};

/// A choice applied to one installed brain.
struct ResolvedSeat {
    /// The level's id.
    std::string level;
    /// OA_DIFFICULTY_EASY, MEDIUM or HARD.
    int32_t difficulty{};
    /// The manifest's version, as text. `seat_spec` views this string.
    std::string version;
    /// The manifest's full-awareness flag.
    bool full_awareness{};
    /// The options, in the manifest's order.
    std::vector<ResolvedOption> options;
    /// The networks, in the manifest's order.
    std::vector<ResolvedNetwork> networks;
};

/// Reads `brains_folder / id`.
///
/// Regular files only. A link, a path outside the folder, a file the
/// package may not hold, a manifest whose id is not `id`, or a folder
/// whose files total more than `installed_package_bytes` refuses the
/// package. `.oa-origin.yaml` counts toward that total and is not stored.
///
/// @param brains_folder the folder that holds one folder per brain
/// @param id the brain's id, a single folder name
/// @param[out] error a sentence when nothing is returned
/// @return the brain, or nothing when it cannot be read
[[nodiscard]] std::optional<InstalledBrain> read_installed_brain(
    const std::filesystem::path& brains_folder, std::string_view id, std::string& error
);

/// Applies `choice` to `installed`.
///
/// An unknown level or option, or a value of the wrong type for its
/// option, is an error naming it.
///
/// @param installed the brain
/// @param choice the level and the options
/// @param[out] out the resolved seat; unchanged when this returns false
/// @param[out] error the sentence, when this returns false
/// @return true when `out` was written
[[nodiscard]] bool resolve_seat(
    const InstalledBrain& installed,
    const BrainSeatChoice& choice,
    ResolvedSeat& out,
    std::string& error
);

/// Fills the views `seat_brain` takes.
///
/// `files`, `options` and `networks` are cleared and filled. The spec's
/// views point into `installed`, `resolved` and those vectors, and last
/// only as long as all of them do. The bank is the brain profile's default.
///
/// @param player engine player index, 0 to 9
/// @param installed the brain
/// @param resolved the level, the options and the networks
/// @param[out] files one entry per stored file
/// @param[out] options one entry per resolved option
/// @param[out] networks one entry per resolved network
/// @return the spec
[[nodiscard]] oa::sim::brains::SeatSpec seat_spec(
    uint8_t player,
    const InstalledBrain& installed,
    const ResolvedSeat& resolved,
    std::vector<oa::sim::brains::PackageFile>& files,
    std::vector<oa::sim::brains::OptionValue>& options,
    std::vector<oa::sim::brains::NetworkFile>& networks
);

} // namespace oa::app
