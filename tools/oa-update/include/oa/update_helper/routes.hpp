// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Turning a plan into journal operations. This build carries out the plain
// folder route, the Linux versions folder and, on macOS, the application
// bundle. The other routes produce no operations.
#pragma once

#include "oa/app/update/plan.hpp"
#include "oa/update_helper/journal.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace oa::update_helper {

/// The operations that put a staged package in place for `plan.route`.
///
/// `check-folder` moves the install's top-level entries into
/// `<updates>/previous/`, then the staged top folder's entries into the
/// install. `linux-versions` converts a flat folder into `versions/` on the
/// first update, then stages the new version beside a `next` link. The
/// caller adds the preferences copy and the state file.
///
/// @param plan the plan
/// @param staged the staging folder, holding one top folder
/// @param[out] why why no operations were produced; may be null; cleared on success
/// @return the operations; empty when `why` is set, or when there was nothing to move
[[nodiscard]] std::vector<Operation> plan_operations(
    const oa::app::update::Plan& plan, const std::filesystem::path& staged, std::string* why
);

/// What a bundle's signature says. A test fills this in. The helper reads it
/// from the bundles themselves and never from the plan.
struct BundleSignature {
    bool valid{false};     ///< the signature check passed
    std::string team;      ///< empty for an ad hoc signature
    bool notarized{false}; ///< notarized, or Gatekeeper reports it so
};

/// Whether the staged bundle may replace the one that is running.
struct MacosDecision {
    bool go{false};  ///< the swap may proceed
    std::string why; ///< why it may not; empty when `go` is set
};

/// Whether a rename of the bundle would stay on one volume.
enum class MacosVolume : uint8_t {
    same,  ///< rename stays on the volume the bundle is already on
    exdev, ///< rename would fail because the volumes differ
};

/// Whether `staged` may replace `running`.
///
/// Same team, valid and notarized: go. Another team, an invalid signature
/// or a bundle that is not notarized: refuse. An ad hoc signature replaces
/// an ad hoc signature only when `test_key` is set, which is a self-check
/// run that was given a feed key. A release build refuses ad hoc.
///
/// @param running the bundle being replaced
/// @param staged the bundle from the package
/// @param test_key a self-check run passed a feed key
/// @return the decision
[[nodiscard]] MacosDecision
macos_may_swap(const BundleSignature& running, const BundleSignature& staged, bool test_key);

/// The one application inside a staged package.
///
/// The staging folder holds one top folder, and that folder holds one
/// application. Anything else is refused.
///
/// @param staged the staging folder
/// @param[out] application the application; unchanged on failure
/// @param[out] why why it was refused; may be null
/// @return true when there is exactly one application
[[nodiscard]] bool macos_staged_application(
    const std::filesystem::path& staged, std::filesystem::path& application, std::string* why
);

/// The operations for a macOS bundle.
///
/// When the plan names the installer, the only operation opens that package.
/// Otherwise an older kept bundle is removed first, the running bundle moves
/// to `previous/` under its own name, and the staged application moves into
/// the running bundle's place. A cross-volume move copies and then removes,
/// in both directions: the staged application lives under the updates folder,
/// so it crosses the same boundary.
///
/// @param plan the plan; its route is the macOS bundle
/// @param staged the staging folder, holding one top folder; unused for an installer
/// @param volume whether a rename would cross volumes
/// @param[out] why why no operations were produced; may be null; cleared on success
/// @return the operations; empty when `why` is set
[[nodiscard]] std::vector<Operation> macos_bundle_operations(
    const oa::app::update::Plan& plan,
    const std::filesystem::path& staged,
    MacosVolume volume,
    std::string* why
);

/// The operations for a macOS plan after the bundles have been checked.
///
/// The team is read from the bundle being replaced and from the staged
/// application, never from the plan. A failure produces no operations.
/// Off macOS this refuses the route.
///
/// @param plan the plan
/// @param staged the staging folder; unused for an installer
/// @param test_key a self-check run passed a feed key
/// @param[out] why why no operations were produced; may be null
/// @return the operations
[[nodiscard]] std::vector<Operation> macos_checked_operations(
    const oa::app::update::Plan& plan,
    const std::filesystem::path& staged,
    bool test_key,
    std::string* why
);

/// Starts the bundle through Launch Services and waits until it has started.
///
/// The arguments are the plan's, plus `--updated-from`. A failure is reported
/// and nothing else is moved. Off macOS this is not declared.
///
/// @param bundle the application, already in its place
/// @param arguments the arguments, before the update flag
/// @param from_version the version the flag names
/// @param[out] error why it did not start; may be null
/// @return true when Launch Services started it
#if defined(__APPLE__)
[[nodiscard]] bool start_macos_bundle(
    const std::filesystem::path& bundle,
    const std::vector<std::string>& arguments,
    std::string_view from_version,
    std::string* error
);
#endif

/// The operations for a Linux or Steam Deck install.
///
/// @param plan the plan; its route is the versions folder
/// @param staged the staging folder, holding one top folder
/// @param[out] why why no operations were produced; may be null; cleared on success
/// @return the operations; empty when `why` is set
[[nodiscard]] std::vector<Operation> linux_operations(
    const oa::app::update::Plan& plan, const std::filesystem::path& staged, std::string* why
);

/// The launcher script a Linux install runs, the same bytes the package ships.
///
/// @return the script
[[nodiscard]] std::string_view linux_launcher_script() noexcept;

} // namespace oa::update_helper
