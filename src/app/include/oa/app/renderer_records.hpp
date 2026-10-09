// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The renderer records: what the engine has seen of each render driver on
// this machine, kept in renderer-state.conf beside the preferences file, and
// the sentinel that marks the stage a start has reached, kept apart in
// renderer-sentinel.conf. A stage the game died in is a strike against its
// driver; the same strike at two starts or runs in a row becomes a record,
// failed-driver (the walk of SDL's drivers skips it), accelerated-unusable
// (the driver stays on the standard tier) or full-unusable (the driver
// draws the battlefield on the processor, in the Basic tier, while the
// graphics card still scales it). On Windows before Vista and on Linux the
// first left-over trial is already a record, since a driver fault there can
// stop the whole system. On a machine under 2 GiB, where the accelerated
// tier never runs, nothing of that tier is struck or recorded, and what a
// run with more memory left of it stays for a start from 2 GiB.
//
// Everything here is pure: the text of each key and value, the rules that
// turn what a start or a run saw into strikes and records, and the sentinel
// and the trial through a run, from `create` to `running` and each path's
// first frames. Nothing reads a file, the clock or SDL; renderer_state.hpp
// reads and writes the files. The records are advice: a value that cannot be
// read is dropped, and nothing here can stop the game.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::renderer_state {

/// A file's keys and values, as oa::platform::preferences reads and writes
/// them.
using Values = std::map<std::string, std::string>;

/// The name SDL gives its own software renderer, which is never recorded
/// failed-driver.
inline constexpr std::string_view software_driver = "software";
/// The adapter of a probe that could not read one.
inline constexpr std::string_view unknown_adapter = "unknown";

/// Key of the trial record in renderer-state.conf.
inline constexpr std::string_view trial_key = "trial";
/// Key of the adapter the probe last described.
inline constexpr std::string_view adapter_key = "adapter";
/// Key of the driver and engine version a window may open at native density
/// under.
inline constexpr std::string_view native_density_key = "native-density";
/// Start of the key of a driver's strike; the driver's name follows.
inline constexpr std::string_view strike_prefix = "strike.";
/// Start of the key of a driver's failed-driver record.
inline constexpr std::string_view failed_driver_prefix = "failed-driver.";
/// Start of the key of a driver's accelerated-unusable record.
inline constexpr std::string_view accelerated_unusable_prefix = "accelerated-unusable.";
/// Start of the key of a driver's full-unusable record, which keeps the
/// Full tier off the driver while the Basic tier stands.
inline constexpr std::string_view full_unusable_prefix = "full-unusable.";
/// Start of the key of a driver's remembered step-down rung.
inline constexpr std::string_view scale_level_prefix = "scale-level.";
/// The one key of renderer-sentinel.conf.
inline constexpr std::string_view sentinel_key = "starting";
/// The word a record ends with once the main menu's notice has shown it.
inline constexpr std::string_view told_word = "told";
/// The word in a sentinel before the framebuffer hint's list of drivers.
inline constexpr std::string_view via_word = "via";

/// Longest driver name, path name or failing call's name kept, in bytes.
inline constexpr size_t max_name_bytes = 64;
/// Longest adapter description kept, in bytes; a longer one is cut.
inline constexpr size_t max_adapter_bytes = 256;
/// Longest engine version kept, in bytes.
inline constexpr size_t max_version_bytes = 64;
/// Most drivers with a strike or a record kept; entries for further drivers
/// are dropped as the file is read.
inline constexpr size_t max_drivers = 64;
/// Most drivers a sentinel's framebuffer hint list names.
inline constexpr size_t max_via_drivers = 16;
/// Highest step-down rung a scale-level record names.
inline constexpr uint8_t max_scale_rung = 15;
/// Highest median frame interval a scale-level record holds, in thousandths
/// of the target period.
inline constexpr uint32_t max_median_thousandths = 1'000'000;

/// An accelerated path with a first use of its own in a run.
enum class AcceleratedPath : uint8_t {
    magnify,  ///< the magnified world's scene tiles and overlay
    prescale, ///< the prescale target
    blend,    ///< the half level of the two-level zoom-out
    /// The Full tier: the battlefield drawn by the graphics card from its
    /// pages and targets. A left-over trial of it records full-unusable,
    /// which leaves the Basic tier standing.
    full,
};

/// How a platform counts a left-over trial.
enum class CrashEvidence : uint8_t {
    two_in_a_row, ///< two left-over trials in a row make a record
    first_counts, ///< the first left-over trial is a record
};

/// Returns how the platform the game runs on counts a left-over trial.
///
/// On Windows before Vista, whose display drivers have no timeout and
/// recovery, and on Linux, the Raspberry Pis included, where a fault of the
/// graphics card can lock the machine, a driver fault in a stage a trial
/// covers can stop the whole system, so one left-over trial is a record and
/// such a fault costs at most one system crash. Elsewhere a driver fault
/// usually ends only the game, and two in a row keep one killed start or
/// power cut from blaming a working driver.
///
/// @param windows_before_vista the game runs on a Windows release before Vista
/// @param linux_system the game runs on Linux
/// @return CrashEvidence::first_counts on those systems, else two_in_a_row
[[nodiscard]] CrashEvidence crash_evidence(bool windows_before_vista, bool linux_system) noexcept;

/// What the rules need to know of the machine the game runs on.
struct RecordRules {
    CrashEvidence evidence{CrashEvidence::two_in_a_row}; ///< how a left-over trial counts
    /// The machine has under 2 GiB of physical memory, or does not report how
    /// much, so the accelerated tier never runs. No trial is written then,
    /// and no strike of a probe, path or call stage and no
    /// accelerated-unusable record is made; a trial, strike or record of the
    /// accelerated tier, and the remembered rungs and native-density key, that
    /// a run with more memory left stay as they are, for a start from 2 GiB
    /// to judge.
    bool below_two_gib{};
};

/// What a strike is against.
enum class StrikeStage : uint8_t {
    none,   ///< no strike
    create, ///< a left-over create sentinel: the driver's creation
    standard, ///< a left-over standard sentinel: probe items 1 to 3, the intro or the first standard frames
    probe,   ///< a left-over probe trial: the function test and the first accelerated frames
    path,    ///< a left-over path trial: an accelerated path's first frames
    present, ///< a present error while running
    call,    ///< an accelerated-only call that failed while running
    lost,    ///< the device was lost while running
    resets,  ///< three device resets within 60 s while running
    /// A call only the Full tier makes failed while running: a page or a
    /// target the card could not make or fill, or a frame it refused.
    card,
};

/// A strike against a driver: evidence seen once, which becomes a record only
/// when the same is seen again in a row.
struct Strike {
    StrikeStage stage{StrikeStage::none};
    AcceleratedPath path{AcceleratedPath::magnify}; ///< for StrikeStage::path
    /// For StrikeStage::present, call and card: the failing call, one word
    /// as the host names it.
    std::string call{};
};

/// Tells whether two strikes are the same evidence.
///
/// @param first one strike
/// @param second the other
/// @return true when both are of the same stage, and of the same path or
///     call where their stage has one
[[nodiscard]] bool same_strike(const Strike& first, const Strike& second) noexcept;

/// What a record says failed.
enum class RecordedFailure : uint8_t {
    none,    ///< no record
    stopped, ///< the game stopped there: a left-over sentinel or trial
    present, ///< present errors
    call,    ///< accelerated-only calls
    lost,    ///< a lost device
    resets,  ///< repeated device resets
    card,    ///< calls only the Full tier makes
};

/// A failed-driver, accelerated-unusable or full-unusable record.
struct Record {
    RecordedFailure failure{RecordedFailure::none};
    bool told{}; ///< the main menu's notice has shown it
};

/// The step-down rung a driver reached, which a later start may begin one
/// rung higher than.
struct ScaleLevel {
    uint8_t
        rung{}; ///< the rung's number as the step-down ladder counts them, at most max_scale_rung
    /// The median frame interval recorded at the rung, in thousandths of the
    /// target period.
    uint32_t median_thousandths{};
};

/// The strike, records and remembered rung of one driver.
struct DriverRecords {
    std::string driver{};
    Strike strike{};
    /// The strike was made in this run, so the same failure again in this
    /// run is not a second run in a row. Never written; a strike read from
    /// the file was made by an earlier run.
    bool struck_this_run{};
    Record failed_driver{};        ///< the walk skips the driver
    Record accelerated_unusable{}; ///< the driver stays on the standard tier
    /// The driver stays on the Basic tier where Full is asked for: the
    /// graphics card scales the battlefield but does not draw it.
    Record full_unusable{};
    std::optional<ScaleLevel> scale_level{};
};

/// The trial record: a stage only the accelerated tier runs, in which a
/// driver fault could stop the whole system, written and flushed before it
/// and erased when it passes or the run exits cleanly.
struct Trial {
    StrikeStage stage{StrikeStage::probe};          ///< StrikeStage::probe or path
    AcceleratedPath path{AcceleratedPath::magnify}; ///< for StrikeStage::path
    std::string driver{};
};

/// The driver and engine version under which a window may open at native
/// density.
struct NativeDensity {
    std::string driver{};
    std::string version{};
};

/// Everything renderer-state.conf holds.
///
/// Strikes, records and remembered rungs belong to the adapter and engine
/// build they were written under: they are written with the `adapter` key's
/// adapter and the running engine's build, its version and the commit it was
/// built from, and those of another adapter or build are dropped as the file
/// is read, so that a new build starts free of what an older one recorded.
struct Records {
    std::string adapter{};        ///< empty when the probe never described one
    std::optional<Trial> trial{}; ///< a stage under way, or left over
    std::optional<NativeDensity> native_density{};
    std::vector<DriverRecords> drivers{}; ///< in the order they were first noted
};

/// Tells whether two sets of records hold the same, the per-run marks of the
/// strikes left out.
///
/// @param first one set
/// @param second the other
/// @return true when both would be written alike
[[nodiscard]] bool same_records(const Records& first, const Records& second) noexcept;

/// Returns a driver's strike and records, or none.
///
/// @param records the records
/// @param driver the driver's name
/// @return its entry, or nullptr when it has none
[[nodiscard]] const DriverRecords*
find_driver(const Records& records, std::string_view driver) noexcept;

/// Returns a driver's strike and records, adding an empty entry when it has
/// none.
///
/// @param[in,out] records the records
/// @param driver the driver's name
/// @return its entry, which stays valid until the next entry is added
[[nodiscard]] DriverRecords& driver_entry(Records& records, std::string_view driver);

/// Tells whether the walk of SDL's drivers skips a driver.
///
/// The records are advice: the caller walks again with them ignored when
/// they would leave nothing able to present.
///
/// @param records the records
/// @param driver the driver's name
/// @return true when the driver is recorded failed-driver; never for software
[[nodiscard]] bool skips_driver(const Records& records, std::string_view driver) noexcept;

/// Lists the drivers the walk of SDL's drivers skips, as the walk takes them
/// (CreationInputs::failed_drivers in render_policy.hpp).
///
/// @param records the records
/// @return views of the names held in records, in their order: every driver
///     skips_driver skips, never software
[[nodiscard]] std::vector<std::string_view> failed_driver_list(const Records& records);

/// Tells whether a driver may use the accelerated tier, as far as the records
/// say.
///
/// @param records the records
/// @param driver the driver's name
/// @param hardware_acceleration_flag --hardware-acceleration was given, which
///     ignores accelerated-unusable records
/// @return false when the driver is recorded accelerated-unusable and the
///     flag was not given
[[nodiscard]] bool acceleration_allowed(
    const Records& records, std::string_view driver, bool hardware_acceleration_flag
) noexcept;

/// Tells whether a driver may use the Full tier, as far as the records say.
///
/// @param records the records
/// @param driver the driver's name
/// @param full_flag --hardware-acceleration=full was given, which ignores
///     full-unusable records
/// @return false when the driver is recorded full-unusable and the flag was
///     not given
[[nodiscard]] bool
full_allowed(const Records& records, std::string_view driver, bool full_flag) noexcept;

/// Tells whether a record has not been shown by the main menu's notice yet.
///
/// @param records the records
/// @return true when a failed-driver, accelerated-unusable or full-unusable
///     record lacks the told mark
[[nodiscard]] bool has_untold_record(const Records& records) noexcept;

// ---------------------------------------------------------------------------
// Text of the keys and values

/// The stage a sentinel marks.
enum class SentinelStage : uint8_t {
    create,   ///< `create <driver>`: the driver is being created
    standard, ///< `standard <driver>`: probe items 1 to 3, the intro and the first standard frames
    probe,    ///< `probe <driver>`: the function test
    accelerated, ///< `accelerated <driver>`: the first accelerated frames
    path,        ///< `path <name> <driver>`: an accelerated path's first frames
    running,     ///< `running <driver>`: the start-up stage has passed
};

/// The value of the sentinel's `starting` key.
struct Sentinel {
    SentinelStage stage{SentinelStage::create};
    AcceleratedPath path{AcceleratedPath::magnify}; ///< for SentinelStage::path
    std::string driver{};
    /// For software on a video driver without a framebuffer of its own: the
    /// framebuffer hint's list of drivers it presents through.
    std::vector<std::string> via{};
};

/// Tells whether two sentinels are the same.
///
/// @param first one sentinel
/// @param second the other
/// @return true when they would be written alike
[[nodiscard]] bool same_sentinel(const Sentinel& first, const Sentinel& second) noexcept;

/// Tells whether a name can be a driver's name in a key or a value: 1 to
/// max_name_bytes letters, digits, `_` or `-`.
///
/// @param name the name
/// @return true when it can be kept
[[nodiscard]] bool valid_driver_name(std::string_view name) noexcept;

/// Puts an adapter's description in the form the records keep: control
/// characters as spaces, runs of spaces as one, none at either end, cut to
/// max_adapter_bytes at a whole character.
///
/// @param description the probe's description
/// @return the adapter, or unknown_adapter when nothing is left
[[nodiscard]] std::string normalise_adapter(std::string_view description);

/// Writes a sentinel as the value of its key.
///
/// @param sentinel the sentinel; its driver and via names valid
/// @return `<stage> <driver>`, `path <name> <driver>` or
///     `<stage> <driver> via <first>,<second>`
[[nodiscard]] std::string format_sentinel(const Sentinel& sentinel);

/// Reads a sentinel from the value of its key.
///
/// @param text the value
/// @return the sentinel, or nullopt when the value is not one
[[nodiscard]] std::optional<Sentinel> parse_sentinel(std::string_view text);

/// Writes a trial as the value of its key.
///
/// @param trial the trial; its driver valid
/// @return `probe <driver>` or `path <name> <driver>`
[[nodiscard]] std::string format_trial(const Trial& trial);

/// Reads a trial from the value of its key.
///
/// @param text the value
/// @return the trial, or nullopt when the value is not one
[[nodiscard]] std::optional<Trial> parse_trial(std::string_view text);

/// What reading renderer-state.conf kept.
struct ParsedRecords {
    Records records{};
    /// Keys left out: unknown keys, values that could not be read, and
    /// entries of another adapter, another engine version or beyond
    /// max_drivers.
    uint32_t dropped{};
};

/// Reads the records from the keys and values of renderer-state.conf.
///
/// Every key is read on its own: one that cannot be read is dropped and the
/// rest kept. The trial and the native-density key are kept whatever the
/// adapter; strikes, records and remembered rungs only when they were written
/// under the `adapter` key's adapter (unknown_adapter when the key is absent)
/// and under `engine_version`.
///
/// @param values the file's keys and values
/// @param engine_version the running engine's build, its version and the
///     commit it was built from
/// @return the records and how many keys were dropped
[[nodiscard]] ParsedRecords parse_records(const Values& values, std::string_view engine_version);

/// Writes the records as the keys and values of renderer-state.conf.
///
/// Strikes, records and remembered rungs are written under the records'
/// adapter (unknown_adapter when it is empty) and `engine_version`.
///
/// @param records the records
/// @param engine_version the running engine's build, its version and the
///     commit it was built from
/// @return the keys and values
[[nodiscard]] Values format_records(const Records& records, std::string_view engine_version);

// ---------------------------------------------------------------------------
// Rules

/// How the records changed.
struct Change {
    bool changed{}; ///< something to be written changed
    bool
        new_record{}; ///< a failed-driver or accelerated-unusable record was made, which the main menu tells
};

/// What the last run left behind, as the next start finds it.
enum class LeftoverSentinel : uint8_t {
    none,       ///< no sentinel file: the last run ended cleanly
    unreadable, ///< a sentinel file that cannot be read
    read,       ///< a sentinel file that was read
};

/// What a start makes of what the last run left behind.
struct LeftoverOutcome {
    Change change{};
    std::string driver{}; ///< the driver struck or recorded; empty when none was
    bool unclean_exit{};  ///< the last run ended without a clean exit, which is logged once
    std::string
        last_driver{}; ///< the driver the last run's sentinel named, for the log; may be empty
};

/// Applies what the last run left behind to the records, at a start.
///
/// A left-over trial decides whatever the sentinel holds, or when it cannot
/// be read: it is a strike of its stage against its driver, and a record of
/// accelerated-unusable, or of full-unusable for the Full tier's path, when
/// the same strike stood already, or at once where the first counts. The
/// trial is then erased. Under 2 GiB a left-over trial
/// stays as it is and decides nothing, and the sentinel is applied as if
/// there were none. Otherwise a left-over create or standard sentinel is a
/// strike, and the same strike twice in a row records failed-driver; against
/// software it counts only for the one driver its framebuffer hint list
/// named, and for none when the list named several. A left-over sentinel of
/// the stages a trial covers, with no trial, a running sentinel and a
/// sentinel that cannot be read are only logged.
///
/// @param[in,out] records the records as read
/// @param kind whether a sentinel was left behind and could be read
/// @param sentinel the left-over sentinel, for LeftoverSentinel::read
/// @param rules how this platform counts a left-over trial, and whether the
///     machine is under 2 GiB
/// @return what changed, against which driver, and what to log
LeftoverOutcome note_leftover(
    Records& records, LeftoverSentinel kind, const Sentinel& sentinel, const RecordRules& rules
);

/// Clears the strikes of a start-up stage a start has passed cleanly: its
/// sentinel became `running`.
///
/// The create or standard strike of the sentinel's driver is cleared, and,
/// for software whose framebuffer hint list named one driver, that driver's
/// too, since a left-over sentinel of software through that list strikes
/// the driver it names.
///
/// @param[in,out] records the records
/// @param passed the start-up sentinel the start passed: its driver, and for
///     software the framebuffer hint's list
/// @param function_test_ran the start ran the function test, so it passed
///     the probe stage as well; never under 2 GiB, where a probe strike
///     stays
/// @param rules whether the machine is under 2 GiB
/// @return what changed
Change note_start_passed(
    Records& records, const Sentinel& passed, bool function_test_ran, const RecordRules& rules
);

/// Clears a driver's strike of an accelerated path whose first frames passed.
///
/// @param[in,out] records the records
/// @param driver the driver
/// @param path the path
/// @return what changed
Change note_path_passed(Records& records, std::string_view driver, AcceleratedPath path);

/// Clears a driver's strike of a failure seen while running, at the end of
/// a run on the driver that did not see it again.
///
/// Under 2 GiB, where no accelerated-only call and no call of the Full
/// tier's own is made, a call or card strike stays.
///
/// @param[in,out] records the records
/// @param driver the driver the run used
/// @param rules whether the machine is under 2 GiB
/// @return what changed
Change note_clean_run(Records& records, std::string_view driver, const RecordRules& rules);

/// What a driver is, for the rules of failures seen while running.
struct DriverFacts {
    /// The driver loses its device in ordinary use (standby, switching users,
    /// a remote session), so nothing it does while running is recorded.
    bool loses_device_in_ordinary_use{};
};

/// Applies a failure seen while running on a driver.
///
/// A present error is a strike, and the same in the next run on the driver
/// records failed-driver. An accelerated-only call that fails is a strike,
/// and the same in the next run records accelerated-unusable; a call of the
/// Full tier's own that fails is a strike, and the same in the next run
/// records full-unusable. A lost device
/// or three resets records accelerated-unusable at once and is a strike, and
/// the same in the next run records failed-driver. The same failure again in
/// the run that struck counts for nothing more. Nothing is recorded on a
/// driver that loses its device in ordinary use, and never failed-driver
/// against software. Under 2 GiB a failed call, of either tier, is not
/// struck, and a lost device or three resets keep only their strike, with
/// failed-driver the next run as above, and no accelerated-unusable record.
///
/// @param[in,out] records the records
/// @param driver the driver
/// @param failure the failure: a strike of stage present, call, card, lost
///     or resets
/// @param facts what the driver is
/// @param rules whether the machine is under 2 GiB
/// @return what changed
Change note_running_failure(
    Records& records,
    std::string_view driver,
    const Strike& failure,
    const DriverFacts& facts,
    const RecordRules& rules
);

/// Takes note of the adapter the probe described.
///
/// An adapter that cannot be read changes nothing. A different adapter from
/// the one the records were written under replaces it and clears every
/// strike, record and remembered rung, so that the new adapter gets a fresh
/// try; an adapter where none was known before is only kept.
///
/// @param[in,out] records the records
/// @param description the probe's description of the adapter
/// @return what changed
Change note_adapter(Records& records, std::string_view description);

/// Marks every record of a driver, of the three kinds, as shown by the main
/// menu's notice.
///
/// @param[in,out] records the records
/// @param driver the driver
/// @return what changed
Change mark_told(Records& records, std::string_view driver);

/// Clears every strike, failed-driver, accelerated-unusable and
/// full-unusable record and remembered rung, as switching the setting Off
/// then On, raising it from Basic to Full and Restore defaults do, so that
/// every driver gets a fresh try.
///
/// The adapter, the native-density key and a standing trial stay: they
/// describe the machine and this start, not failures.
///
/// @param[in,out] records the records
/// @return what changed
Change clear_failures(Records& records) noexcept;

// ---------------------------------------------------------------------------
// Native density

/// Returns the driver the native-density key lets a window open at native
/// density under, for render_policy::DensityInputs::record.
///
/// @param records the records
/// @param engine_version the running engine's version
/// @return the driver the key names when it was written under
///     engine_version; empty when there is no such key
[[nodiscard]] std::optional<std::string_view>
native_density_driver(const Records& records, std::string_view engine_version) noexcept;

/// What a run that ends cleanly tells of native density.
struct DensityRunEnd {
    /// The window system honours the high-density window flag; where it
    /// does not, the key is never written.
    bool flag_honoured{};
    /// The machine's class with the run's driver has been measured at native
    /// density (render_policy::native_density_measured).
    bool class_measured{};
    /// The machine started above budget none (render_policy::start_budget).
    bool above_budget_none{};
    /// The run's function test passed on its driver.
    bool function_test_passed{};
    /// The run drew frames in the accelerated tier.
    bool accelerated{};
    /// The run dropped acceleration, for any reason: a driver's failure, the
    /// memory guard, a stall or the step-down's last rung.
    bool dropped{};
    /// The run ended at or below the step-down's magnify-off rung.
    bool ended_at_or_below_magnify_off{};
};

/// Writes or erases the native-density key as a run ends cleanly, so that a
/// machine that cannot keep the accelerated tier above the magnify-off rung
/// opens at the window system's density from the next start.
///
/// A run that dropped acceleration, or that drew in the accelerated tier and
/// ended at or below the magnify-off rung, erases the key. A run that drew
/// in the accelerated tier, ended above that rung, and passed the function
/// test on a hardware driver, never on software, writes the key with its
/// driver and the running engine's version, but only where the window
/// system honours the flag, the class has been measured and the machine
/// started above budget none. Any other run leaves the key as it is, a run
/// in the standard tier among them, since the key is a fact of the machine
/// and not of the setting. Under 2 GiB, where the accelerated tier never
/// runs, the key is neither written nor erased.
///
/// @param[in,out] records the records
/// @param driver the run's driver
/// @param engine_version the running engine's version
/// @param run what the run did
/// @param rules whether the machine is under 2 GiB
/// @return what changed
Change note_density_run_end(
    Records& records,
    std::string_view driver,
    std::string_view engine_version,
    const DensityRunEnd& run,
    const RecordRules& rules
);

/// Erases the native-density key, as a start whose probe rejects its
/// renderer, or whose function test fails, does; recording
/// accelerated-unusable against the key's driver erases it too. Under
/// 2 GiB, where no function test runs, the key stays.
///
/// @param[in,out] records the records
/// @param rules whether the machine is under 2 GiB
/// @return what changed
Change forget_native_density(Records& records, const RecordRules& rules) noexcept;

// ---------------------------------------------------------------------------
// The main menu's notice

/// What the main menu's notice of a new record says.
enum class NoticeKind : uint8_t {
    /// A graphics driver failed, so the game now uses another one: a
    /// failed-driver record, whatever tier the next driver runs.
    failed_driver,
    /// The graphics card could not be used, so the processor draws the game:
    /// an accelerated-unusable record.
    accelerated_unusable,
    /// The graphics card could not draw the battlefield, so it only scales
    /// it now: a full-unusable record.
    full_unusable,
};

/// A record the main menu's notice has not shown yet.
struct PendingNotice {
    std::string driver{}; ///< the driver the record is against
    NoticeKind kind{NoticeKind::failed_driver};
};

/// Returns the record the main menu tells of next.
///
/// A driver with several records has one notice: failed-driver's, since the
/// game changed driver, else accelerated-unusable's, since the processor
/// draws the game, else full-unusable's; mark_told then marks them all.
///
/// @param records the records
/// @param passed_over drivers whose notice the run has dealt with already
///     but left untold, as an unattended run does
/// @return the first driver, in the order the records noted them, with a
///     record not yet told and not passed over; nullopt when there is none
[[nodiscard]] std::optional<PendingNotice>
next_notice(const Records& records, const std::vector<std::string>& passed_over);

/// What a run does with a notice that is due.
enum class NoticeAction : uint8_t {
    show, ///< show it, then mark the record told
    /// Note the request and leave the record untold, for the next start
    /// someone watches.
    note,
    /// Note the request and mark the record told, as a shown notice would,
    /// so that a check can see the decision.
    note_and_mark,
};

/// Decides what a run does with a notice that is due.
///
/// @param unattended nobody watches the run: an unattended run, CI, or a
///     video driver that draws no window
/// @param marks_told the run is a check that marks a noted request told
///     (--check-renderer-ladder)
/// @return NoticeAction::show for a run someone watches; otherwise note,
///     or note_and_mark for the check
[[nodiscard]] NoticeAction notice_action(bool unattended, bool marks_told) noexcept;

// ---------------------------------------------------------------------------
// The sentinel and the trial through a run

/// The frames a start presents, at the menus, loading screens and matches
/// alike, before its start-up stage has passed.
inline constexpr uint32_t start_stage_frames = 60;
/// The time a start runs before its start-up stage has passed, in
/// nanoseconds.
inline constexpr uint64_t start_stage_ns = 2'000'000'000;
/// The frames an accelerated path draws before its first use has passed.
inline constexpr uint32_t path_stage_frames = 60;

/// Tells whether a start-up stage has passed.
///
/// @param frames the frames presented since the stage began
/// @param elapsed_ns the time since it began, in nanoseconds
/// @return true after start_stage_frames frames and start_stage_ns both
[[nodiscard]] bool start_stage_passed(uint32_t frames, uint64_t elapsed_ns) noexcept;

/// What moves a run's sentinel or trial.
enum class LifeEvent : uint8_t {
    creating,                ///< before each attempt to create a renderer
    probing,                 ///< before probe items 1 to 3
    function_test,           ///< before the function test
    function_test_done,      ///< after the function test
    first_accelerated_frame, ///< before the first accelerated frame
    start_passed,            ///< the start-up stage passed (start_stage_passed)
    path_first_use,          ///< before a path's first use in the run
    path_passed,             ///< the path drew path_stage_frames frames
};

/// Where a run's sentinel and trial stand.
struct SentinelLife {
    bool kept{}; ///< the run keeps a sentinel: SDL_RENDER_DRIVER is not set
    /// The sentinel's stage; none before the run's first event.
    std::optional<SentinelStage> stage{};
    AcceleratedPath path{AcceleratedPath::magnify}; ///< for SentinelStage::path
    bool trial{};                                   ///< a trial stands
    /// The stage that stood before the trial was written, which stands again
    /// when it could not be.
    std::optional<SentinelStage> before_trial{};
};

/// What to write for a life event, in this order: the trial, then the
/// sentinel, then the trial's erasure.
struct SentinelWrite {
    /// Write the trial, flushed, before anything else
    /// (RendererState::write_trial). When it cannot be written, as under
    /// 2 GiB where none is, the stage is skipped: nothing else is written,
    /// and the host calls note_trial_unwritten.
    bool write_trial{};
    StrikeStage trial_stage{StrikeStage::probe};             ///< the trial's stage: probe or path
    AcceleratedPath trial_path{AcceleratedPath::magnify};    ///< the trial's path
    bool set_sentinel{};                                     ///< rewrite the sentinel
    SentinelStage sentinel{SentinelStage::create};           ///< its new stage
    AcceleratedPath sentinel_path{AcceleratedPath::magnify}; ///< for SentinelStage::path
    bool erase_trial{}; ///< erase the trial, flushed, as its stage has passed
};

/// Starts a run's sentinel.
///
/// @param render_driver_named SDL_RENDER_DRIVER is set, which keeps no
///     sentinel and no trial
/// @return the life, before its first event
[[nodiscard]] SentinelLife start_sentinel_life(bool render_driver_named) noexcept;

/// Moves a run's sentinel and trial for an event.
///
/// `create <driver>` stands before each attempt to create a renderer and
/// `standard <driver>` around probe items 1 to 3, through the intro and the
/// first standard frames. The function test writes the trial `probe` and
/// stands under `probe <driver>`, then `standard <driver>` again; the first
/// accelerated frame sets `accelerated <driver>`; once the start-up stage
/// passes, the sentinel becomes `running <driver>` and the trial is erased.
/// A path first used after that writes its trial and `path <name> <driver>`,
/// and after path_stage_frames frames goes back to `running <driver>` and
/// erases the trial. A path first used while another stage stands writes
/// nothing of its own: that stage's sentinel and trial cover it. Under
/// SDL_RENDER_DRIVER nothing is written.
///
/// @param[in,out] life the run's sentinel
/// @param event the event
/// @param path the path, for LifeEvent::path_first_use and path_passed
/// @return what to write; nothing when the event moves nothing
[[nodiscard]] SentinelWrite
sentinel_step(SentinelLife& life, LifeEvent event, AcceleratedPath path) noexcept;

/// Notes that the trial of the last step could not be written: its stage is
/// skipped, and the sentinel stands where it stood before the step.
///
/// @param[in,out] life the run's sentinel
void note_trial_unwritten(SentinelLife& life) noexcept;

} // namespace oa::app::renderer_state
