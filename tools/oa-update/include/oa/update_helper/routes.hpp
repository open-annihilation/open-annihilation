// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Turning a plan into journal operations. This build carries out the plain
// folder route, the two Windows routes, the Linux versions folder and, on
// macOS, the application bundle, with an administrator's rights for the
// moves when the player cannot make them. The other routes produce no
// operations.
#pragma once

#include "oa/app/update/plan.hpp"
#include "oa/update_helper/journal.hpp"
#include "oa/update_helper/log.hpp"
#include "oa/update_helper/window.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace oa::update_helper {

/// What relaunching the helper with raised rights did.
struct ElevatedRelaunch {
    bool ran{false};       ///< the elevated copy started
    bool cancelled{false}; ///< the player refused the prompt
    int code{1};           ///< its exit status when `ran` is set
    std::string error;     ///< why it did not start; empty on success
};

/// The window line while macOS asks for an administrator's password.
inline constexpr std::string_view macos_asks_permission =
    "Only an administrator can replace this copy, so macOS asks for an administrator's password.";

/// The receipt's detail when the player closes the administrator prompt.
inline constexpr std::string_view permission_not_given =
    "the administrator's permission was not given";

/// What the player may do to the paths a macOS bundle swap moves.
struct MacosSwapAccess {
    bool parent_writable{false}; ///< the folder that holds the install accepts and gives up entries
    bool install_movable{false}; ///< the install may move into another folder
    bool kept_movable{true}; ///< the kept bundle, when there is one, may move into another folder
};

/// True when a macOS bundle swap, or the roll back that undoes it, needs an
/// administrator's rights.
///
/// A rename that moves a folder into another folder rewrites that folder's
/// own entry for its parent, so the folder itself must be writable, not
/// only the folders on either side. An install the player cannot write, as
/// the installer package leaves it, cannot move into `previous`, even from a
/// folder the player can write. Any one of the three missing asks.
///
/// @param access what the player may do
/// @return true when the moves need raised rights
[[nodiscard]] bool macos_swap_needs_elevation(const MacosSwapAccess& access) noexcept;

/// What this process may do to the install and the kept bundle.
///
/// The install's folder must be writable and, when it is sticky, owned by
/// this user or hold an install this user owns. The install, and the kept
/// bundle when one is there, must be writable themselves, since each moves
/// into another folder. A kept bundle that is missing does not ask. Off
/// POSIX everything is allowed.
///
/// @param install the bundle in place
/// @param kept the kept bundle; it may be missing
/// @return what is allowed
[[nodiscard]] MacosSwapAccess
macos_swap_access(const std::filesystem::path& install, const std::filesystem::path& kept);

/// The kept bundle for an install: `<updates>/previous/<install's name>`.
///
/// @param updates the data folder's updates directory
/// @param install the bundle in place
/// @return where the kept bundle sits
[[nodiscard]] std::filesystem::path
macos_kept_bundle(const std::filesystem::path& updates, const std::filesystem::path& install);

/// The program and arguments that ask macOS for an administrator's rights
/// and run the helper with them.
///
/// The program is `/usr/bin/osascript`. Its script runs one shell command
/// with administrator privileges, built from the values after it, each put
/// in single quotes by the script itself: the prompt, then the helper and
/// its arguments. The helper's own output is discarded, and the command's
/// output is the helper's exit status.
///
/// @param prompt the sentence the password prompt shows
/// @param helper the helper program
/// @param arguments the helper's arguments
/// @return the program, then its arguments
[[nodiscard]] std::vector<std::string> macos_elevation_command(
    std::string_view prompt,
    const std::filesystem::path& helper,
    const std::vector<std::string>& arguments
);

/// How the administrator prompt's command ended.
///
/// A command that did not start asked nothing. A non-zero status whose
/// error names -128 is the prompt being closed. Any other non-zero status
/// asked nothing. A zero status carries the helper's exit status as its
/// output.
///
/// @param started the command started
/// @param status its exit status
/// @param output its standard output
/// @param errors its standard error
/// @return what the relaunch did
[[nodiscard]] ElevatedRelaunch
macos_elevation_outcome(bool started, int status, std::string_view output, std::string_view errors);

/// Whose a tree becomes after an administrator's copy moved it.
enum class MovedOwner : uint8_t {
    unchanged, ///< the tree keeps the owner it had
    player,    ///< the tree is in the updates folder, which is the player's
    install,   ///< the tree is in the install's place and takes the install's owner
};

/// Whose a tree becomes once an administrator's copy has moved it to `to`.
///
/// Inside the updates folder a tree is the player's, so nothing under the
/// player's Library is left to the administrator. In the install's place it
/// takes the owner the install had before the update began, as the
/// installer package left it. Anywhere else it keeps its owner.
///
/// @param updates the data folder's updates directory
/// @param install the bundle the route replaces
/// @param to where the tree was moved
/// @return the owner it takes
[[nodiscard]] MovedOwner moved_tree_owner(
    const std::filesystem::path& updates,
    const std::filesystem::path& install,
    const std::filesystem::path& to
);

/// The player an administrator's copy acts for.
struct ActingPlayer {
    uint32_t user{};       ///< the player's user id
    uint32_t group{};      ///< the player's group id
    bool can_raise{false}; ///< this process may take an administrator's rights again for a move
};

/// What an administrator's copy needs to move a tree: who it acts for and
/// who owned the install.
struct RaisedMoves {
    ActingPlayer player;           ///< the player
    std::filesystem::path updates; ///< a tree moved in here becomes the player's
    std::filesystem::path install; ///< a tree moved here takes the install's owner
    uint32_t install_user{};       ///< the install's owner before the update
    uint32_t install_group{};      ///< the install's group before the update
    Log* log{};                    ///< where a move notes what it did; may be null
};

/// The operations that put a staged package in place for `plan.route`.
///
/// `check-folder` moves the install's top-level entries into
/// `<updates>/previous/`, then the staged top folder's entries into the
/// install. The Windows routes build their own lists. `linux-versions`
/// converts a flat folder into `versions/` on the first update, then stages
/// the new version beside a `next` link. The caller adds the preferences
/// copy and the state file for the plain folder and the Linux route.
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

/// What one look found while waiting for a macOS application to start.
struct MacosLaunchSeen {
    bool callback_finished{false};  ///< the launch report has arrived
    bool callback_started{false};   ///< the report says the application started
    int callback_pid{0};            ///< the process id in that report; 0 names none
    bool application_listed{false}; ///< the system lists the application as running
    int listed_pid{0};              ///< the process id in that list; 0 names none
    bool version_confirmed{false};  ///< state.json confirms the expected version
};

/// How far a macOS launch has got.
enum class MacosLaunch : uint8_t {
    waiting, ///< nothing has shown a start, and the report has not refused it
    started, ///< the application is running, or the expected version is confirmed
    failed,  ///< the report refused the start, and nothing else shows it running
};

/// Whether a macOS application has started, from one look.
struct MacosLaunchDecision {
    MacosLaunch launch{MacosLaunch::waiting}; ///< how far the launch has got
    int pid{0}; ///< the process to watch; 0 when only state.json showed the start
};

/// Decides whether a macOS application has started.
///
/// A report that names a process id counts. A process the system lists as
/// running counts when the report does not. A confirmed version counts when
/// neither names a process. A report that refuses the start counts only when
/// nothing else shows the application running. A report that claims a start
/// but names no process id is ignored.
///
/// @param seen what this look found
/// @return the decision
[[nodiscard]] MacosLaunchDecision macos_launch_decision(const MacosLaunchSeen& seen);

/// The operations of a macOS roll back that an administrator's copy carries
/// out, after the bundles have been checked.
///
/// The kept bundle is `<updates>/previous/<install's name>`, read from the
/// plan and never from a file. It must be signed as the installed bundle
/// is, by the same team and notarized, by the rule of macos_may_swap, and
/// lie on the install's volume. The installed bundle moves aside and the
/// kept one moves into its place. Off macOS this refuses the route.
///
/// @param plan the plan; its route is the macOS bundle
/// @param test_key a self-check run passed a feed key
/// @param[out] why why no operations were produced; may be null
/// @return the operations; empty when `why` is set
[[nodiscard]] std::vector<Operation>
macos_checked_restore(const oa::app::update::Plan& plan, bool test_key, std::string* why);

#if defined(__APPLE__)
/// Runs the helper again with an administrator's rights and waits for it.
///
/// macOS shows its administrator prompt with `prompt`. `waiting` is called
/// about every 50 ms while the prompt is up and while the helper runs, so a
/// window keeps drawing. The helper's exit status is returned when it ran.
///
/// @param prompt the sentence the password prompt shows
/// @param helper the helper program
/// @param arguments the helper's arguments
/// @param waiting called while it waits; may be null
/// @param context passed to `waiting`
/// @return how the relaunch ended
[[nodiscard]] ElevatedRelaunch relaunch_elevated_macos(
    std::string_view prompt,
    const std::filesystem::path& helper,
    const std::vector<std::string>& arguments,
    void (*waiting)(void* context),
    void* context
);

/// This helper's own program file. Empty when the system will not name it.
///
/// @return the program
[[nodiscard]] std::filesystem::path macos_helper_program();

/// Makes this process act as the player who owns `updates`, keeping the
/// right to take an administrator's rights again for a move.
///
/// A process whose real user is not the administrator acts as it already
/// is, and cannot raise. An administrator's copy takes the player's user,
/// groups and group for every file it touches. It refuses an updates
/// folder the administrator owns, or a plan file another user owns.
///
/// @param updates the data folder's updates directory
/// @param plan_file the plan this copy read
/// @param[out] player who it now acts as
/// @param[out] why why it refused; may be null
/// @return true when the copy may carry on
[[nodiscard]] bool act_as_player(
    const std::filesystem::path& updates,
    const std::filesystem::path& plan_file,
    ActingPlayer& player,
    std::string* why
);

/// Fills the install's owner and group, read before anything moves.
///
/// @param[in,out] moves the moves; its install is read
/// @return false when the install cannot be read
[[nodiscard]] bool read_install_owner(RaisedMoves& moves);

/// True when an administrator's copy may move `bundle`: it is signed by
/// the team that signed this helper, with a valid signature, and has the
/// bundle identifier `identifier` names. A self-check run with a feed key
/// accepts an ad hoc signature on an ad hoc helper.
///
/// @param bundle the bundle
/// @param identifier the bundle whose identifier it must share
/// @param test_key a self-check run passed a feed key
/// @param[out] why why it may not; may be null
/// @return true when it may move
[[nodiscard]] bool macos_bundle_is_ours(
    const std::filesystem::path& bundle,
    const std::filesystem::path& identifier,
    bool test_key,
    std::string* why
);

/// A JournalHooks::rename for an administrator's copy.
///
/// Takes the administrator's rights, renames, gives the moved tree the
/// owner moved_tree_owner names, and acts as the player again. The tree
/// that is given an owner is the one that was renamed: it is opened without
/// following a link and compared with what was at `from`. A process that
/// cannot raise renames as it is.
///
/// @param context the RaisedMoves
/// @param from what moves
/// @param to where it goes
/// @return true when it moved
[[nodiscard]] bool
raised_rename(void* context, const std::filesystem::path& from, const std::filesystem::path& to);

/// Starts the bundle and waits until it has started.
///
/// The arguments are the plan's. `--updated-from` is added when `from_version`
/// is not empty. The application counts as started when the launch report
/// names it, when the system lists it running, or when `state_file` changes
/// from unconfirmed to confirmed for `expect_version`. A version that was
/// already confirmed when the wait began does not count. An empty `state_file`
/// or `expect_version` skips that look. The wait does not run out the launch
/// report once one of those has happened. A failure is reported and nothing
/// else is moved.
///
/// @param bundle the application, already in its place
/// @param arguments the arguments, before the update flag
/// @param from_version the version the flag names; empty adds no `--updated-from`
/// @param state_file the state file; empty skips the confirmation look
/// @param expect_version the version a confirmation must name; empty skips it
/// @param[out] error why it did not start; may be null
/// @param[out] pid the process id when the start succeeded; 0 when only the state file showed it
/// @return true when the application has started
[[nodiscard]] bool start_macos_bundle(
    const std::filesystem::path& bundle,
    const std::vector<std::string>& arguments,
    std::string_view from_version,
    const std::filesystem::path& state_file,
    std::string_view expect_version,
    std::string* error,
    int* pid
);
#endif

/// The operations that put a kept version back.
///
/// The list comes from `state.kept_route` and `state.kept_path`. It moves
/// only the kept program files and the install they return to. It does not
/// copy preferences, rewrite the state file, or touch a save, a mod, the
/// user folder or installed content. The caller adds those bookkeeping steps.
///
/// @param state the record the swap wrote
/// @param install the folder or bundle the route replaces
/// @param bad_version the version now in place, for a step that records it
/// @param[out] why why no operations were produced; may be null; cleared on success
/// @return the operations; empty when `why` is set
[[nodiscard]] std::vector<Operation> restore_operations(
    const oa::app::update::State& state,
    const std::filesystem::path& install,
    std::string_view bad_version,
    std::string* why
);

/// The operations for a Linux or Steam Deck install.
///
/// @param plan the plan; its route is the versions folder
/// @param staged the staging folder, holding one top folder
/// @param[out] why why no operations were produced; may be null; cleared on success
/// @return the operations; empty when `why` is set
[[nodiscard]] std::vector<Operation> linux_operations(
    const oa::app::update::Plan& plan, const std::filesystem::path& staged, std::string* why
);

/// Points `current` at the kept version and moves `update-starts` aside.
///
/// @param state the record; its kept path is `versions/<kept>` under the install
/// @param install the install folder
/// @param[out] why why no operations were produced; may be null
/// @return the operations
[[nodiscard]] std::vector<Operation> linux_restore_operations(
    const oa::app::update::State& state, const std::filesystem::path& install, std::string* why
);

/// Puts the kept version back and, when `start_game` is set, starts it.
///
/// Preferences become the copy taken at the swap, with the bad release
/// skipped when that copy can be read. The state file then names the kept
/// version, confirmed, with nothing kept. When the install folder cannot be
/// written on Windows and `may_elevate` is set, an elevated copy does the
/// restore and this process only starts the game.
///
/// @param plan the plan, an install or a roll back
/// @param updates the updates folder, holding state.json
/// @param plan_path the plan file an elevated copy reads; may be null when not elevating
/// @param log the log
/// @param window the window; closed before the start
/// @param hooks journal hooks
/// @param reason why the roll back is happening
/// @param may_elevate true when this process may relaunch itself elevated
/// @param start_game false for the elevated copy, which restores and exits
/// @return an exit status
[[nodiscard]] int restore_kept(
    const oa::app::update::Plan& plan,
    const std::filesystem::path& updates,
    const std::filesystem::path* plan_path,
    Log& log,
    ProgressWindow& window,
    JournalHooks& hooks,
    oa::app::update::RollBackReason reason,
    bool may_elevate,
    bool start_game
);

/// The launcher script a Linux install runs, the same bytes the package ships.
///
/// @return the script
[[nodiscard]] std::string_view linux_launcher_script() noexcept;

} // namespace oa::update_helper
