// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The renderer records' text and rules, by table: how each platform counts a
// left-over trial; every key and value read and written, with the values
// that cannot be read and the entries of another adapter or engine version
// dropped; the sentinel and the trial; a left-over sentinel or trial and a
// failure while running making a strike, and a record only the second time
// in a row, or at once for a left-over trial where the first counts; clean
// passes clearing strikes, software's through a list of one included; under
// 2 GiB nothing of the accelerated tier made and what a larger machine left
// kept; the native-density key, written only by a run that kept the
// accelerated tier above the magnify-off rung where the flag does something,
// and erased by any drop and by a run that ended at or below that rung; the
// adapter, the told mark and clearing; the main menu's notice: which record
// it tells of next and what a run does with it; and the sentinel and the
// trial through a run, with none under SDL_RENDER_DRIVER; and the Full
// tier's own path, strike and record, full-unusable, which leaves the Basic
// tier standing. The drivers are named "alpha", "beta" and "gamma", and
// SDL's own "software".
#include "oa/app/renderer_records.hpp"

#include "oa/test/check.hpp"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace rs = oa::app::renderer_state;

namespace {

constexpr std::string_view version = "0.6.2";
constexpr std::string_view adapter = "Example Graphics 3000";
/// A machine from 2 GiB where two left-over trials in a row make a record.
constexpr rs::RecordRules from_two_gib{rs::CrashEvidence::two_in_a_row, false};
/// A machine under 2 GiB, on a platform where the first left-over trial would
/// count.
constexpr rs::RecordRules below_two_gib{rs::CrashEvidence::first_counts, true};

/// Returns records written under the test's adapter.
rs::Records adapter_records() {
    rs::Records records;
    records.adapter = std::string(adapter);
    return records;
}

/// Returns a strike of a stage with no path or call.
rs::Strike strike_of(rs::StrikeStage stage) {
    rs::Strike strike;
    strike.stage = stage;
    return strike;
}

/// Returns a strike of a stage with a call.
rs::Strike call_strike(rs::StrikeStage stage, std::string call) {
    rs::Strike strike;
    strike.stage = stage;
    strike.call = std::move(call);
    return strike;
}

/// Returns a sentinel of a stage and driver.
rs::Sentinel sentinel_of(rs::SentinelStage stage, std::string driver) {
    rs::Sentinel sentinel;
    sentinel.stage = stage;
    sentinel.driver = std::move(driver);
    return sentinel;
}

/// Returns the failure a driver is recorded failed-driver for.
rs::RecordedFailure failed_driver(const rs::Records& records, std::string_view driver) {
    const rs::DriverRecords* entry = rs::find_driver(records, driver);
    return entry != nullptr ? entry->failed_driver.failure : rs::RecordedFailure::none;
}

/// Returns the failure a driver is recorded accelerated-unusable for.
rs::RecordedFailure accelerated_unusable(const rs::Records& records, std::string_view driver) {
    const rs::DriverRecords* entry = rs::find_driver(records, driver);
    return entry != nullptr ? entry->accelerated_unusable.failure : rs::RecordedFailure::none;
}

/// Returns the failure a driver is recorded full-unusable for.
rs::RecordedFailure full_unusable(const rs::Records& records, std::string_view driver) {
    const rs::DriverRecords* entry = rs::find_driver(records, driver);
    return entry != nullptr ? entry->full_unusable.failure : rs::RecordedFailure::none;
}

/// Returns the stage of a driver's strike.
rs::StrikeStage strike_stage(const rs::Records& records, std::string_view driver) {
    const rs::DriverRecords* entry = rs::find_driver(records, driver);
    return entry != nullptr ? entry->strike.stage : rs::StrikeStage::none;
}

/// Writes records and reads them back, as a later start reads the file.
rs::Records next_start(const rs::Records& records) {
    const rs::ParsedRecords parsed =
        rs::parse_records(rs::format_records(records, version), version);
    OA_CHECK(parsed.dropped == 0);
    return parsed.records;
}

void crash_evidence_counts_the_first_trial_before_vista_and_on_linux() {
    struct Row {
        bool windows_before_vista;
        bool linux_system;
        rs::CrashEvidence expected;
    };

    const Row rows[] = {
        {false, false, rs::CrashEvidence::two_in_a_row},
        {true, false, rs::CrashEvidence::first_counts},
        {false, true, rs::CrashEvidence::first_counts},
        {true, true, rs::CrashEvidence::first_counts},
    };
    for (const Row& row : rows)
        OA_CHECK(rs::crash_evidence(row.windows_before_vista, row.linux_system) == row.expected);
}

void every_key_round_trips() {
    rs::Records records = adapter_records();
    records.trial = rs::Trial{rs::StrikeStage::path, rs::AcceleratedPath::prescale, "beta"};
    records.native_density = rs::NativeDensity{"alpha", std::string(version)};
    rs::DriverRecords& alpha = rs::driver_entry(records, "alpha");
    alpha.strike = call_strike(rs::StrikeStage::present, "present-frame");
    alpha.accelerated_unusable = rs::Record{rs::RecordedFailure::lost, true};
    alpha.scale_level = rs::ScaleLevel{3, 550};
    rs::DriverRecords& beta = rs::driver_entry(records, "beta");
    beta.failed_driver = rs::Record{rs::RecordedFailure::stopped, false};
    beta.strike.stage = rs::StrikeStage::path;
    beta.strike.path = rs::AcceleratedPath::blend;
    rs::DriverRecords& gamma = rs::driver_entry(records, "gamma");
    gamma.accelerated_unusable = rs::Record{rs::RecordedFailure::call, false};
    gamma.failed_driver = rs::Record{rs::RecordedFailure::present, true};

    const rs::Values values = rs::format_records(records, version);
    const rs::Values expected{
        {"accelerated-unusable.alpha", "lost Example Graphics 3000 0.6.2 told"},
        {"accelerated-unusable.gamma", "call Example Graphics 3000 0.6.2"},
        {"adapter", "Example Graphics 3000"},
        {"failed-driver.beta", "stopped Example Graphics 3000 0.6.2"},
        {"failed-driver.gamma", "present Example Graphics 3000 0.6.2 told"},
        {"native-density", "alpha 0.6.2"},
        {"scale-level.alpha", "3 Example Graphics 3000 0.6.2 0.550"},
        {"strike.alpha", "present present-frame Example Graphics 3000 0.6.2"},
        {"strike.beta", "path blend Example Graphics 3000 0.6.2"},
        {"trial", "path prescale beta"},
    };
    OA_CHECK(values == expected);
    const rs::ParsedRecords parsed = rs::parse_records(values, version);
    OA_CHECK(parsed.dropped == 0);
    OA_CHECK(rs::same_records(parsed.records, records));
    OA_CHECK(rs::format_records(parsed.records, version) == values);

    // Every stage of a strike has its own text.
    struct StrikeRow {
        rs::Strike strike;
        const char* text;
    };

    rs::Strike magnify = strike_of(rs::StrikeStage::path);
    const StrikeRow strikes[] = {
        {strike_of(rs::StrikeStage::create), "create"},
        {strike_of(rs::StrikeStage::standard), "standard"},
        {strike_of(rs::StrikeStage::probe), "probe"},
        {magnify, "path magnify"},
        {call_strike(rs::StrikeStage::present, "p1"), "present p1"},
        {call_strike(rs::StrikeStage::call, "upload"), "call upload"},
        {strike_of(rs::StrikeStage::lost), "lost"},
        {strike_of(rs::StrikeStage::resets), "resets"},
        {call_strike(rs::StrikeStage::card, "page"), "card page"},
    };
    for (const StrikeRow& row : strikes) {
        rs::Records one = adapter_records();
        rs::driver_entry(one, "alpha").strike = row.strike;
        const rs::Values written = rs::format_records(one, version);
        OA_CHECK(
            written.at("strike.alpha") == std::string(row.text) + " Example Graphics 3000 0.6.2"
        );
        const rs::ParsedRecords read = rs::parse_records(written, version);
        OA_CHECK(read.dropped == 0);
        OA_CHECK(rs::same_strike(read.records.drivers.at(0).strike, row.strike));
    }

    // With no adapter key the records are written under "unknown".
    rs::Records unknown;
    rs::driver_entry(unknown, "alpha").strike = strike_of(rs::StrikeStage::create);
    const rs::Values unknown_values = rs::format_records(unknown, version);
    OA_CHECK(unknown_values.count("adapter") == 0);
    OA_CHECK(unknown_values.at("strike.alpha") == "create unknown 0.6.2");
    OA_CHECK(rs::parse_records(unknown_values, version).dropped == 0);
    // Entries with nothing in them write nothing.
    rs::Records empty;
    (void)rs::driver_entry(empty, "alpha");
    OA_CHECK(rs::format_records(empty, version).empty());
}

void a_new_build_starts_free_of_an_older_ones_failures() {
    // The records are written under the engine's build, its version and the
    // commit it was built from: another commit of the same version reads
    // none of them, nor does the version alone.
    constexpr std::string_view written_under = "0.7.3+0123456789ab";
    rs::Records records;
    records.adapter = std::string(adapter);
    rs::DriverRecords& entry = rs::driver_entry(records, "metal");
    entry.strike = {
        rs::StrikeStage::card, rs::AcceleratedPath::magnify, "the-card-refused-the-frame"
    };
    entry.full_unusable = {rs::RecordedFailure::card, false};
    const rs::Values values = rs::format_records(records, written_under);
    const rs::ParsedRecords same = rs::parse_records(values, written_under);
    OA_CHECK(same.dropped == 0 && same.records.drivers.size() == 1);
    for (const std::string_view other : {"0.7.3+ba9876543210", "0.7.3"}) {
        const rs::ParsedRecords read = rs::parse_records(values, other);
        OA_CHECK(read.records.drivers.empty() && read.dropped == 2);
        OA_CHECK(rs::full_allowed(read.records, "metal", false));
    }
}

void values_that_cannot_be_read_are_dropped_alone() {
    struct Row {
        const char* key;
        const char* value;
        bool kept;
    };

    const Row rows[] = {
        {"strike.alpha", "create Example Graphics 3000 0.6.2", true},
        {"strike.alpha", "", false},
        {"strike.alpha", "create", false},
        {"strike.alpha", "create 0.6.2", false}, // no adapter
        {"strike.alpha", "explode Example Graphics 3000 0.6.2", false},
        {"strike.alpha", "path Example Graphics 3000 0.6.2", false}, // no path name
        {"strike.alpha", "path zoom Example Graphics 3000 0.6.2", false},
        {"strike.alpha", "present p1 Example Graphics 3000 0.6.2", true},
        // The call takes the adapter's first word, leaving another adapter.
        {"strike.alpha", "present Example Graphics 3000 0.6.2", false},
        {"strike.alpha", "present", false},
        {"strike.alpha", "create  Example Graphics 3000 0.6.2", false}, // two spaces
        {"strike.alpha", " create Example Graphics 3000 0.6.2", false},
        {"strike.alpha", "create Example Graphics 3000 0.6.2 ", false},
        {"strike.alpha", "create Example\tGraphics 3000 0.6.2", false},
        {"strike.alpha", "create Example Graphics 3000 0.6.1", false}, // another version
        {"strike.alpha", "create Other Graphics 0.6.2", false},        // another adapter
        {"strike.al pha", "create Example Graphics 3000 0.6.2", false},
        {"strike.", "create Example Graphics 3000 0.6.2", false},
        {"strike.alpha.beta", "create Example Graphics 3000 0.6.2", false},
        {"failed-driver.alpha", "stopped Example Graphics 3000 0.6.2", true},
        {"failed-driver.alpha", "stopped Example Graphics 3000 0.6.2 told", true},
        {"failed-driver.alpha", "call Example Graphics 3000 0.6.2", false},
        {"failed-driver.alpha", "stopped Example Graphics 3000 0.6.2 told told", false},
        {"failed-driver.software", "stopped Example Graphics 3000 0.6.2", false},
        {"accelerated-unusable.alpha", "call Example Graphics 3000 0.6.2", true},
        {"accelerated-unusable.alpha", "present Example Graphics 3000 0.6.2", false},
        {"accelerated-unusable.software", "stopped Example Graphics 3000 0.6.2", true},
        {"scale-level.alpha", "2 Example Graphics 3000 0.6.2 0.5", true},
        {"scale-level.alpha", "2 Example Graphics 3000 0.6.2", false},
        {"scale-level.alpha", "16 Example Graphics 3000 0.6.2 0.5", false},
        {"scale-level.alpha", "x Example Graphics 3000 0.6.2 0.5", false},
        {"scale-level.alpha", "2 Example Graphics 3000 0.6.2 0.5555", false},
        {"scale-level.alpha", "2 Example Graphics 3000 0.6.2 .5", false},
        {"scale-level.alpha", "2 Example Graphics 3000 0.6.2 1001", false},
        {"trial", "probe alpha", true},
        {"trial", "path magnify alpha", true},
        {"trial", "probe", false},
        {"trial", "standard alpha", false},
        {"trial", "path alpha", false},
        {"trial", "probe al/pha", false},
        {"native-density", "alpha 0.6.2", true},
        {"native-density", "alpha", false},
        {"another-key", "anything", false},
        {"strike.alpha", "none Example Graphics 3000 0.6.2", false},
        {"failed-driver.alpha", "none Example Graphics 3000 0.6.2", false},
        {"failed-driver.alpha", "told Example Graphics 3000 0.6.2", false},
        {"failed-driver.alpha", "stopped 0.6.2", false}, // no adapter
        {"accelerated-unusable.alpha", "stopped told", false},
        {"scale-level.alpha", "2 0.6.2 0.5", false}, // no adapter
        {"scale-level.alpha", "2 Example Graphics 3000 0.6.2 half", false},
        {"scale-level.alpha", "2 Example Graphics 3000 0.6.2 0.", false},
        {"scale-level.alpha", "2 Example Graphics 3000 0.6.2 0.5x", false},
        {"scale-level.alpha", "2 Example Graphics 3000 0.6.2 99999999.5", false},
        {"trial", "", false},
        {"trial", "probe alpha beta", false},
        {"trial", "create alpha", false},
        {"native-density", "alpha 0.6.2 extra", false},
        {"crash.alpha", "create Example Graphics 3000 0.6.2", false},
        {"starting", "create alpha", false}, // the sentinel's key, in its own file
    };
    for (const Row& row : rows) {
        rs::Values values{
            {"adapter", std::string(adapter)}, {"strike.beta", "lost Example Graphics 3000 0.6.2"}
        };
        values[row.key] = row.value;
        const rs::ParsedRecords parsed = rs::parse_records(values, version);
        const bool kept = parsed.dropped == 0;
        if (kept != row.kept)
            std::fprintf(stderr, "key %s value \"%s\"\n", row.key, row.value);
        OA_CHECK(kept == row.kept);
        // The other key is kept either way.
        OA_CHECK(strike_stage(parsed.records, "beta") == rs::StrikeStage::lost);
    }
    // A scale-level record's median keeps its thousandths.
    const rs::Values scale{
        {"adapter", std::string(adapter)},
        {"scale-level.alpha", "2 Example Graphics 3000 0.6.2 0.55"}
    };
    const rs::ParsedRecords parsed = rs::parse_records(scale, version);
    OA_CHECK(parsed.records.drivers.at(0).scale_level->median_thousandths == 550);
    OA_CHECK(
        rs::format_records(parsed.records, version).at("scale-level.alpha") ==
        "2 Example Graphics 3000 0.6.2 0.550"
    );

    // The median is a fraction of the target period, read to the thousandth.
    struct Median {
        const char* text;
        uint32_t thousandths;
    };

    for (const Median& median :
         {Median{"0.5", 500},
          Median{"0.55", 550},
          Median{"0.559", 559},
          Median{"1", 1000},
          Median{"2.00", 2000},
          Median{"0.05", 50},
          Median{"1000", rs::max_median_thousandths}}) {
        const rs::Values one{
            {"adapter", std::string(adapter)},
            {"scale-level.alpha", std::string("2 Example Graphics 3000 0.6.2 ") + median.text}
        };
        const rs::ParsedRecords read = rs::parse_records(one, version);
        OA_CHECK(read.dropped == 0);
        OA_CHECK(read.records.drivers.at(0).scale_level->median_thousandths == median.thousandths);
    }

    // A value that cannot be read leaves the driver's other values.
    const rs::Values mixed{
        {"adapter", std::string(adapter)},
        {"failed-driver.alpha", "stopped Example Graphics 3000 0.6.2 told"},
        {"strike.alpha", "explode Example Graphics 3000 0.6.2"},
        {"accelerated-unusable.alpha", "present Example Graphics 3000 0.6.2"},
    };
    const rs::ParsedRecords mixed_read = rs::parse_records(mixed, version);
    OA_CHECK(mixed_read.dropped == 2);
    OA_CHECK(mixed_read.records.drivers.size() == 1);
    OA_CHECK(failed_driver(mixed_read.records, "alpha") == rs::RecordedFailure::stopped);
    OA_CHECK(rs::find_driver(mixed_read.records, "alpha")->failed_driver.told);
    OA_CHECK(strike_stage(mixed_read.records, "alpha") == rs::StrikeStage::none);
    OA_CHECK(accelerated_unusable(mixed_read.records, "alpha") == rs::RecordedFailure::none);

    // An adapter whose last word is the told mark is still the adapter.
    for (const bool told : {false, true}) {
        const rs::Values told_adapter{
            {"adapter", "Card told"},
            {"failed-driver.alpha", std::string("stopped Card told 0.6.2") + (told ? " told" : "")}
        };
        const rs::ParsedRecords read = rs::parse_records(told_adapter, version);
        OA_CHECK(read.dropped == 0);
        OA_CHECK(failed_driver(read.records, "alpha") == rs::RecordedFailure::stopped);
        OA_CHECK(rs::find_driver(read.records, "alpha")->failed_driver.told == told);
        OA_CHECK(rs::format_records(read.records, version) == told_adapter);
    }
}

void entries_beyond_the_driver_limit_are_dropped() {
    rs::Values values;
    for (size_t index = 0; index < rs::max_drivers + 3; ++index)
        values["strike.driver" + std::to_string(1000 + index)] = "create unknown 0.6.2";
    const rs::ParsedRecords parsed = rs::parse_records(values, version);
    OA_CHECK(parsed.records.drivers.size() == rs::max_drivers);
    OA_CHECK(parsed.dropped == 3);
    // A long name is not a driver's.
    const std::string long_name(rs::max_name_bytes + 1, 'a');
    OA_CHECK(!rs::valid_driver_name(long_name));
    OA_CHECK(rs::valid_driver_name(std::string(rs::max_name_bytes, 'a')));
    OA_CHECK(
        rs::parse_records({{"strike." + long_name, "create unknown 0.6.2"}}, version).dropped == 1
    );
}

void sentinels_and_trials_read_and_write() {
    struct Row {
        const char* text;
        bool valid;
    };

    const Row rows[] = {
        {"create alpha", true},
        {"standard alpha", true},
        {"probe alpha", true},
        {"accelerated alpha", true},
        {"running alpha", true},
        {"path magnify alpha", true},
        {"path prescale alpha", true},
        {"path blend alpha", true},
        {"path full alpha", true},
        {"standard software via alpha,beta", true},
        {"create software via alpha", true},
        {"", false},
        {"running", false},
        {"starting alpha", false},
        {"path alpha", false},
        {"path zoom alpha", false},
        {"path magnify alpha via beta", false},
        {"standard software via", false},
        {"standard software through alpha", false},
        {"standard software via alpha,,beta", false},
        {"standard software via alpha,", false},
        {"standard software via ,alpha", false},
        {"create alpha beta", false},
        {"create al.pha", false},
        {"create alpha\n", false},
    };
    for (const Row& row : rows) {
        const auto sentinel = rs::parse_sentinel(row.text);
        OA_CHECK(sentinel.has_value() == row.valid);
        if (sentinel)
            OA_CHECK(rs::format_sentinel(*sentinel) == row.text);
    }
    const auto via = rs::parse_sentinel("standard software via alpha,beta");
    OA_CHECK(via && via->via == (std::vector<std::string>{"alpha", "beta"}));
    std::string many = "create software via d0";
    for (size_t index = 1; index < rs::max_via_drivers; ++index)
        many += ",d" + std::to_string(index);
    OA_CHECK(rs::parse_sentinel(many).has_value());
    OA_CHECK(!rs::parse_sentinel(many + ",extra").has_value());

    const auto probe = rs::parse_trial("probe alpha");
    OA_CHECK(probe && probe->stage == rs::StrikeStage::probe && probe->driver == "alpha");
    const auto path = rs::parse_trial("path blend beta");
    OA_CHECK(
        path && path->stage == rs::StrikeStage::path && path->path == rs::AcceleratedPath::blend
    );
    OA_CHECK(rs::format_trial(*path) == "path blend beta");
    OA_CHECK(!rs::parse_trial("probe alpha beta").has_value());
}

void adapters_are_kept_in_one_form() {
    struct Row {
        const char* description;
        const char* kept;
    };

    const Row rows[] = {
        {"Example Graphics 3000", "Example Graphics 3000"},
        {"  Example   Graphics\t3000\n", "Example Graphics 3000"},
        {"", "unknown"},
        {" \t ", "unknown"},
    };
    for (const Row& row : rows)
        OA_CHECK(rs::normalise_adapter(row.description) == row.kept);
    // A long description is cut at a whole character.
    std::string long_description(rs::max_adapter_bytes - 1, 'a');
    long_description += "\xc3\xa9 more"; // é straddles the limit
    const std::string cut = rs::normalise_adapter(long_description);
    OA_CHECK(cut == std::string(rs::max_adapter_bytes - 1, 'a'));
}

void a_leftover_makes_a_strike_then_a_record() {
    // Each row is what one start finds left over, applied to the records the
    // start before it wrote: first to empty records, then once more to what
    // that start wrote, as at two starts in a row.
    struct Row {
        const char* name;
        const char* trial_driver; // a left-over trial's driver, or "" for none
        rs::StrikeStage trial_stage;
        rs::LeftoverSentinel kind;
        rs::SentinelStage stage;
        const char* driver;
        std::vector<std::string> via;
        rs::CrashEvidence evidence;
        const char* struck;               // the driver struck or recorded, or ""
        rs::StrikeStage first_strike;     // its strike after the first start
        rs::RecordedFailure first_failed; // its failed-driver record after the first start
        rs::RecordedFailure first_unusable;
        rs::RecordedFailure second_failed; // after the second start in a row
        rs::RecordedFailure second_unusable;
    };

    using SS = rs::SentinelStage;
    using K = rs::StrikeStage;
    using F = rs::RecordedFailure;
    using L = rs::LeftoverSentinel;
    const auto two = rs::CrashEvidence::two_in_a_row;
    const auto first = rs::CrashEvidence::first_counts;
    const Row rows[] = {
        {"create sentinel",
         "",
         K::probe,
         L::read,
         SS::create,
         "alpha",
         {},
         two,
         "alpha",
         K::create,
         F::none,
         F::none,
         F::stopped,
         F::none},
        {"standard sentinel",
         "",
         K::probe,
         L::read,
         SS::standard,
         "alpha",
         {},
         two,
         "alpha",
         K::standard,
         F::none,
         F::none,
         F::stopped,
         F::none},
        {"standard sentinel where the first trial counts",
         "",
         K::probe,
         L::read,
         SS::standard,
         "alpha",
         {},
         first,
         "alpha",
         K::standard,
         F::none,
         F::none,
         F::stopped,
         F::none},
        {"probe trial",
         "alpha",
         K::probe,
         L::read,
         SS::accelerated,
         "alpha",
         {},
         two,
         "alpha",
         K::probe,
         F::none,
         F::none,
         F::none,
         F::stopped},
        {"probe trial, sentinel lost",
         "alpha",
         K::probe,
         L::none,
         SS::create,
         "",
         {},
         two,
         "alpha",
         K::probe,
         F::none,
         F::none,
         F::none,
         F::stopped},
        {"probe trial, sentinel unreadable",
         "alpha",
         K::probe,
         L::unreadable,
         SS::create,
         "",
         {},
         two,
         "alpha",
         K::probe,
         F::none,
         F::none,
         F::none,
         F::stopped},
        {"probe trial, sentinel of another stage",
         "alpha",
         K::probe,
         L::read,
         SS::running,
         "alpha",
         {},
         two,
         "alpha",
         K::probe,
         F::none,
         F::none,
         F::none,
         F::stopped},
        {"probe trial where the first counts",
         "alpha",
         K::probe,
         L::read,
         SS::probe,
         "alpha",
         {},
         first,
         "alpha",
         K::none,
         F::none,
         F::stopped,
         F::none,
         F::stopped},
        {"path trial where the first counts",
         "alpha",
         K::path,
         L::none,
         SS::create,
         "",
         {},
         first,
         "alpha",
         K::none,
         F::none,
         F::stopped,
         F::none,
         F::stopped},
        {"path trial",
         "alpha",
         K::path,
         L::read,
         SS::path,
         "alpha",
         {},
         two,
         "alpha",
         K::path,
         F::none,
         F::none,
         F::none,
         F::stopped},
        {"software probe trial",
         "software",
         K::probe,
         L::read,
         SS::probe,
         "software",
         {},
         two,
         "software",
         K::probe,
         F::none,
         F::none,
         F::none,
         F::stopped},
        {"software via one driver",
         "",
         K::probe,
         L::read,
         SS::standard,
         "software",
         {"beta"},
         two,
         "beta",
         K::standard,
         F::none,
         F::none,
         F::stopped,
         F::none},
        {"software via two drivers",
         "",
         K::probe,
         L::read,
         SS::standard,
         "software",
         {"beta", "gamma"},
         two,
         "",
         K::none,
         F::none,
         F::none,
         F::none,
         F::none},
        {"software with no list",
         "",
         K::probe,
         L::read,
         SS::create,
         "software",
         {},
         two,
         "",
         K::none,
         F::none,
         F::none,
         F::none,
         F::none},
        {"probe sentinel with no trial",
         "",
         K::probe,
         L::read,
         SS::probe,
         "alpha",
         {},
         first,
         "",
         K::none,
         F::none,
         F::none,
         F::none,
         F::none},
        {"accelerated sentinel with no trial",
         "",
         K::probe,
         L::read,
         SS::accelerated,
         "alpha",
         {},
         first,
         "",
         K::none,
         F::none,
         F::none,
         F::none,
         F::none},
        {"path sentinel with no trial",
         "",
         K::probe,
         L::read,
         SS::path,
         "alpha",
         {},
         first,
         "",
         K::none,
         F::none,
         F::none,
         F::none,
         F::none},
        {"running sentinel",
         "",
         K::probe,
         L::read,
         SS::running,
         "alpha",
         {},
         first,
         "",
         K::none,
         F::none,
         F::none,
         F::none,
         F::none},
        {"unreadable sentinel",
         "",
         K::probe,
         L::unreadable,
         SS::create,
         "",
         {},
         first,
         "",
         K::none,
         F::none,
         F::none,
         F::none,
         F::none},
        {"clean exit",
         "",
         K::probe,
         L::none,
         SS::create,
         "",
         {},
         first,
         "",
         K::none,
         F::none,
         F::none,
         F::none,
         F::none},
    };
    for (const Row& row : rows) {
        const int failures_before = oa::test::failed_checks();
        const std::string_view trial_driver = row.trial_driver;
        const auto leftover = [&](rs::Records& records) {
            if (!trial_driver.empty())
                records.trial = rs::Trial{
                    row.trial_stage, rs::AcceleratedPath::magnify, std::string(trial_driver)
                };
            rs::Sentinel sentinel = sentinel_of(row.stage, row.driver);
            sentinel.via = row.via;
            return rs::note_leftover(
                records, row.kind, sentinel, rs::RecordRules{row.evidence, false}
            );
        };
        rs::Records records = adapter_records();
        const rs::LeftoverOutcome first_outcome = leftover(records);
        const std::string_view struck = row.struck;
        OA_CHECK(first_outcome.driver == struck);
        OA_CHECK(first_outcome.unclean_exit == (!trial_driver.empty() || row.kind != L::none));
        OA_CHECK(!records.trial.has_value());
        const bool first_record = row.first_failed != F::none || row.first_unusable != F::none;
        OA_CHECK(first_outcome.change.new_record == first_record);
        OA_CHECK(first_outcome.change.changed == (!struck.empty() || !trial_driver.empty()));
        if (!struck.empty()) {
            OA_CHECK(strike_stage(records, struck) == row.first_strike);
            OA_CHECK(failed_driver(records, struck) == row.first_failed);
            OA_CHECK(accelerated_unusable(records, struck) == row.first_unusable);
        } else {
            OA_CHECK(records.drivers.empty());
        }
        // The next start reads what this one wrote and finds the same left
        // over again.
        records = next_start(records);
        const rs::LeftoverOutcome second_outcome = leftover(records);
        if (!struck.empty()) {
            OA_CHECK(failed_driver(records, struck) == row.second_failed);
            OA_CHECK(accelerated_unusable(records, struck) == row.second_unusable);
            const bool second_record =
                !first_record && (row.second_failed != F::none || row.second_unusable != F::none);
            OA_CHECK(second_outcome.change.new_record == second_record);
            if (row.second_failed != F::none || row.second_unusable != F::none) {
                OA_CHECK(strike_stage(records, struck) == K::none);
                OA_CHECK(rs::has_untold_record(records));
            }
        } else {
            OA_CHECK(records.drivers.empty());
        }
        OA_CHECK(failed_driver(records, rs::software_driver) == F::none);
        if (oa::test::failed_checks() != failures_before)
            std::fprintf(stderr, "in the row \"%s\"\n", row.name);
    }
}

void a_clean_pass_between_two_leftovers_clears_the_strike() {
    rs::Records records = adapter_records();
    const rs::Sentinel standard = sentinel_of(rs::SentinelStage::standard, "alpha");
    (void)rs::note_leftover(records, rs::LeftoverSentinel::read, standard, from_two_gib);
    OA_CHECK(strike_stage(records, "alpha") == rs::StrikeStage::standard);
    // The next start passes its start-up cleanly on the driver.
    records = next_start(records);
    OA_CHECK(rs::note_start_passed(records, standard, false, from_two_gib).changed);
    OA_CHECK(strike_stage(records, "alpha") == rs::StrikeStage::none);
    // So the start after that, left over at the same stage, strikes again
    // rather than recording.
    records = next_start(records);
    const auto outcome =
        rs::note_leftover(records, rs::LeftoverSentinel::read, standard, from_two_gib);
    OA_CHECK(!outcome.change.new_record);
    OA_CHECK(failed_driver(records, "alpha") == rs::RecordedFailure::none);

    // A different left-over replaces the strike.
    records = next_start(records);
    (void)rs::note_leftover(
        records,
        rs::LeftoverSentinel::read,
        sentinel_of(rs::SentinelStage::create, "alpha"),
        from_two_gib
    );
    OA_CHECK(strike_stage(records, "alpha") == rs::StrikeStage::create);
    OA_CHECK(failed_driver(records, "alpha") == rs::RecordedFailure::none);

    // A probe strike is cleared only by a start that ran the function test.
    rs::Records probe = adapter_records();
    rs::driver_entry(probe, "alpha").strike = strike_of(rs::StrikeStage::probe);
    OA_CHECK(!rs::note_start_passed(probe, standard, false, from_two_gib).changed);
    OA_CHECK(strike_stage(probe, "alpha") == rs::StrikeStage::probe);
    OA_CHECK(rs::note_start_passed(probe, standard, true, from_two_gib).changed);
    OA_CHECK(strike_stage(probe, "alpha") == rs::StrikeStage::none);
    // A path strike is cleared by its own path's first frames alone.
    rs::Records path = adapter_records();
    rs::Strike prescale = strike_of(rs::StrikeStage::path);
    prescale.path = rs::AcceleratedPath::prescale;
    rs::driver_entry(path, "alpha").strike = prescale;
    OA_CHECK(!rs::note_path_passed(path, "alpha", rs::AcceleratedPath::magnify).changed);
    OA_CHECK(!rs::note_start_passed(path, standard, true, from_two_gib).changed);
    OA_CHECK(rs::note_path_passed(path, "alpha", rs::AcceleratedPath::prescale).changed);
    OA_CHECK(strike_stage(path, "alpha") == rs::StrikeStage::none);

    // A clean pass the way a left-over sentinel struck clears its strike:
    // software through a list of one strikes the driver the list names.
    struct Pass {
        const char* name;
        rs::Sentinel passed;
        bool clears;
    };

    rs::Sentinel via_beta = sentinel_of(rs::SentinelStage::standard, "software");
    via_beta.via = {"beta"};
    rs::Sentinel via_both = via_beta;
    via_both.via = {"beta", "gamma"};
    const Pass passes[] = {
        {"software through the struck driver", via_beta, true},
        {"the struck driver itself", sentinel_of(rs::SentinelStage::standard, "beta"), true},
        {"software through a list of two", via_both, false},
        {"software with no list", sentinel_of(rs::SentinelStage::standard, "software"), false},
        {"another driver", sentinel_of(rs::SentinelStage::standard, "gamma"), false},
    };
    for (const Pass& pass : passes) {
        const int failures_before = oa::test::failed_checks();
        rs::Records via = adapter_records();
        (void)rs::note_leftover(via, rs::LeftoverSentinel::read, via_beta, from_two_gib);
        OA_CHECK(strike_stage(via, "beta") == rs::StrikeStage::standard);
        via = next_start(via);
        OA_CHECK(
            rs::note_start_passed(via, pass.passed, false, from_two_gib).changed == pass.clears
        );
        // The same left-over at the start after: only a strike after a clean
        // pass, else the record.
        via = next_start(via);
        const rs::LeftoverOutcome again =
            rs::note_leftover(via, rs::LeftoverSentinel::read, via_beta, from_two_gib);
        OA_CHECK(again.change.new_record == !pass.clears);
        OA_CHECK(
            strike_stage(via, "beta") ==
            (pass.clears ? rs::StrikeStage::standard : rs::StrikeStage::none)
        );
        OA_CHECK(
            failed_driver(via, "beta") ==
            (pass.clears ? rs::RecordedFailure::none : rs::RecordedFailure::stopped)
        );
        if (oa::test::failed_checks() != failures_before)
            std::fprintf(stderr, "in the pass \"%s\"\n", pass.name);
    }
    // Passing on a driver with nothing recorded changes nothing.
    OA_CHECK(!rs::note_start_passed(
                  path, sentinel_of(rs::SentinelStage::standard, "gamma"), true, from_two_gib
    )
                  .changed);
}

void a_failure_while_running_is_recorded_in_the_next_run() {
    // Each row is a failure seen in one run, then the same in the next run on
    // the same driver.
    struct Row {
        const char* name;
        rs::Strike failure;
        const char* driver;
        bool loses_device;
        rs::RecordedFailure first_failed; // after the first run
        rs::RecordedFailure first_unusable;
        rs::RecordedFailure second_failed; // after the second run
        rs::RecordedFailure second_unusable;
    };

    using F = rs::RecordedFailure;
    using K = rs::StrikeStage;
    const Row rows[] = {
        {"present error",
         call_strike(K::present, "present-frame"),
         "alpha",
         false,
         F::none,
         F::none,
         F::present,
         F::none},
        {"accelerated-only call",
         call_strike(K::call, "upload"),
         "alpha",
         false,
         F::none,
         F::none,
         F::none,
         F::call},
        {"lost device", strike_of(K::lost), "alpha", false, F::none, F::lost, F::lost, F::lost},
        {"three resets",
         strike_of(K::resets),
         "alpha",
         false,
         F::none,
         F::resets,
         F::resets,
         F::resets},
        {"present error on software",
         call_strike(K::present, "present-frame"),
         "software",
         false,
         F::none,
         F::none,
         F::none,
         F::none},
        {"lost device on software",
         strike_of(K::lost),
         "software",
         false,
         F::none,
         F::lost,
         F::none,
         F::lost},
        {"lost device where devices are lost in ordinary use",
         strike_of(K::lost),
         "alpha",
         true,
         F::none,
         F::none,
         F::none,
         F::none},
        {"present error where devices are lost in ordinary use",
         call_strike(K::present, "present-frame"),
         "alpha",
         true,
         F::none,
         F::none,
         F::none,
         F::none},
        {"a start-up stage is no running failure",
         strike_of(K::standard),
         "alpha",
         false,
         F::none,
         F::none,
         F::none,
         F::none},
    };
    for (const Row& row : rows) {
        const int failures_before = oa::test::failed_checks();
        rs::Records records = adapter_records();
        const rs::DriverFacts facts{row.loses_device};
        (void)rs::note_running_failure(records, row.driver, row.failure, facts, from_two_gib);
        // The same failure again in the run that struck counts for nothing
        // more.
        const rs::Change again =
            rs::note_running_failure(records, row.driver, row.failure, facts, from_two_gib);
        OA_CHECK(!again.changed);
        OA_CHECK(failed_driver(records, row.driver) == row.first_failed);
        OA_CHECK(accelerated_unusable(records, row.driver) == row.first_unusable);
        // The next run, on the same driver.
        records = next_start(records);
        (void)rs::note_running_failure(records, row.driver, row.failure, facts, from_two_gib);
        OA_CHECK(failed_driver(records, row.driver) == row.second_failed);
        OA_CHECK(accelerated_unusable(records, row.driver) == row.second_unusable);
        if (oa::test::failed_checks() != failures_before)
            std::fprintf(stderr, "in the row \"%s\"\n", row.name);
    }

    // Another call failing in the next run is a new strike, not a record.
    rs::Records records = adapter_records();
    (void)rs::note_running_failure(
        records, "alpha", call_strike(K::present, "first"), {}, from_two_gib
    );
    records = next_start(records);
    const rs::Change other = rs::note_running_failure(
        records, "alpha", call_strike(K::present, "second"), {}, from_two_gib
    );
    OA_CHECK(other.changed && !other.new_record);
    OA_CHECK(failed_driver(records, "alpha") == F::none);
    OA_CHECK(rs::find_driver(records, "alpha")->strike.call == "second");

    // A run that ends cleanly clears a strike from an earlier run, but keeps
    // the one it made itself for the next run to see.
    rs::Records clean = adapter_records();
    (void)rs::note_running_failure(clean, "alpha", strike_of(K::lost), {}, from_two_gib);
    OA_CHECK(!rs::note_clean_run(clean, "alpha", from_two_gib).changed);
    OA_CHECK(strike_stage(clean, "alpha") == K::lost);
    clean = next_start(clean);
    OA_CHECK(rs::note_clean_run(clean, "alpha", from_two_gib).changed);
    OA_CHECK(strike_stage(clean, "alpha") == K::none);
    // The record a lost device made at once stays.
    OA_CHECK(accelerated_unusable(clean, "alpha") == F::lost);
    // A clean run leaves a start-up strike to the start-up's own pass.
    rs::Records startup = adapter_records();
    rs::driver_entry(startup, "alpha").strike = strike_of(K::create);
    OA_CHECK(!rs::note_clean_run(startup, "alpha", from_two_gib).changed);
}

void under_two_gib_nothing_of_the_accelerated_tier_is_made() {
    // Each row starts from what a run with more memory left: a probe trial of
    // alpha and its native-density key, beta's accelerated-unusable record,
    // remembered rung and probe strike, and gamma's call strike. Then one
    // start or run under 2 GiB, or two in a row.
    struct Row {
        const char* name;
        rs::Change (*apply)(rs::Records& records);
        bool twice;                       // applied again at the next start or run
        rs::StrikeStage alpha_strike;     // alpha's strike afterwards
        rs::RecordedFailure alpha_failed; // alpha's failed-driver record afterwards
        bool new_record;                  // the last application made a record
    };

    using K = rs::StrikeStage;
    using F = rs::RecordedFailure;
    const Row rows[] = {
        {"left-over trial and its probe sentinel",
         [](rs::Records& records) {
             return rs::note_leftover(
                        records,
                        rs::LeftoverSentinel::read,
                        sentinel_of(rs::SentinelStage::probe, "alpha"),
                        below_two_gib
             )
                 .change;
         },
         true,
         K::none,
         F::none,
         false},
        {"left-over trial with its sentinel lost",
         [](rs::Records& records) {
             return rs::note_leftover(
                        records, rs::LeftoverSentinel::none, rs::Sentinel{}, below_two_gib
             )
                 .change;
         },
         true,
         K::none,
         F::none,
         false},
        {"left-over trial and a standard sentinel",
         [](rs::Records& records) {
             return rs::note_leftover(
                        records,
                        rs::LeftoverSentinel::read,
                        sentinel_of(rs::SentinelStage::standard, "alpha"),
                        below_two_gib
             )
                 .change;
         },
         true,
         K::none,
         F::stopped,
         true},
        {"lost device",
         [](rs::Records& records) {
             return rs::note_running_failure(
                 records, "alpha", strike_of(K::lost), {}, below_two_gib
             );
         },
         false,
         K::lost,
         F::none,
         false},
        {"lost device in the next run too",
         [](rs::Records& records) {
             return rs::note_running_failure(
                 records, "alpha", strike_of(K::lost), {}, below_two_gib
             );
         },
         true,
         K::none,
         F::lost,
         true},
        {"three resets in the next run too",
         [](rs::Records& records) {
             return rs::note_running_failure(
                 records, "alpha", strike_of(K::resets), {}, below_two_gib
             );
         },
         true,
         K::none,
         F::resets,
         true},
        {"accelerated-only call",
         [](rs::Records& records) {
             return rs::note_running_failure(
                 records, "alpha", call_strike(K::call, "upload"), {}, below_two_gib
             );
         },
         true,
         K::none,
         F::none,
         false},
        {"present error in the next run too",
         [](rs::Records& records) {
             return rs::note_running_failure(
                 records, "alpha", call_strike(K::present, "present-frame"), {}, below_two_gib
             );
         },
         true,
         K::none,
         F::present,
         true},
        {"start-up passed on beta as if the function test ran",
         [](rs::Records& records) {
             return rs::note_start_passed(
                 records, sentinel_of(rs::SentinelStage::standard, "beta"), true, below_two_gib
             );
         },
         true,
         K::none,
         F::none,
         false},
        {"clean run on gamma",
         [](rs::Records& records) { return rs::note_clean_run(records, "gamma", below_two_gib); },
         true,
         K::none,
         F::none,
         false},
    };
    for (const Row& row : rows) {
        const int failures_before = oa::test::failed_checks();
        rs::Records records = adapter_records();
        records.trial = rs::Trial{K::probe, rs::AcceleratedPath::magnify, "alpha"};
        records.native_density = rs::NativeDensity{"alpha", std::string(version)};
        rs::DriverRecords& beta = rs::driver_entry(records, "beta");
        beta.accelerated_unusable = rs::Record{F::call, true};
        beta.scale_level = rs::ScaleLevel{2, 450};
        beta.strike = strike_of(K::probe);
        rs::driver_entry(records, "gamma").strike = call_strike(K::call, "upload");
        records = next_start(records);
        const rs::Change first = row.apply(records);
        OA_CHECK(!first.new_record);
        if (row.twice) {
            records = next_start(records);
            OA_CHECK(row.apply(records).new_record == row.new_record);
        }
        // What the larger machine left stays as it was.
        OA_CHECK(records.trial && records.trial->driver == "alpha");
        OA_CHECK(records.native_density && records.native_density->driver == "alpha");
        OA_CHECK(accelerated_unusable(records, "beta") == F::call);
        OA_CHECK(strike_stage(records, "beta") == K::probe);
        const rs::DriverRecords* kept = rs::find_driver(records, "beta");
        OA_CHECK(kept != nullptr && kept->scale_level && kept->scale_level->rung == 2);
        kept = rs::find_driver(records, "gamma");
        OA_CHECK(kept != nullptr && kept->strike.stage == K::call && kept->strike.call == "upload");
        // Nothing of the accelerated tier is made: only a strike or
        // failed-driver against alpha.
        OA_CHECK(accelerated_unusable(records, "alpha") == F::none);
        OA_CHECK(strike_stage(records, "alpha") == row.alpha_strike);
        OA_CHECK(failed_driver(records, "alpha") == row.alpha_failed);
        if (oa::test::failed_checks() != failures_before)
            std::fprintf(stderr, "in the row \"%s\"\n", row.name);
    }

    // A left-over trial alone is no unclean exit there, since it stays for
    // every start.
    rs::Records records = adapter_records();
    records.trial = rs::Trial{K::path, rs::AcceleratedPath::prescale, "alpha"};
    const rs::LeftoverOutcome outcome =
        rs::note_leftover(records, rs::LeftoverSentinel::none, rs::Sentinel{}, below_two_gib);
    OA_CHECK(!outcome.unclean_exit && !outcome.change.changed && outcome.driver.empty());
    OA_CHECK(records.trial.has_value());
}

void accelerated_unusable_takes_away_native_density() {
    rs::Records records = adapter_records();
    records.native_density = rs::NativeDensity{"alpha", std::string(version)};
    (void)rs::note_running_failure(
        records, "beta", strike_of(rs::StrikeStage::lost), {}, from_two_gib
    );
    OA_CHECK(records.native_density.has_value());
    const rs::Change change = rs::note_running_failure(
        records, "alpha", strike_of(rs::StrikeStage::lost), {}, from_two_gib
    );
    OA_CHECK(change.changed && change.new_record);
    OA_CHECK(!records.native_density.has_value());
}

/// A run that writes the native-density key as it ends: it drew in the
/// accelerated tier above the magnify-off rung, on a window system that
/// honours the flag, in a measured class above budget none.
rs::DensityRunEnd density_run() {
    rs::DensityRunEnd run;
    run.flag_honoured = true;
    run.class_measured = true;
    run.above_budget_none = true;
    run.function_test_passed = true;
    run.accelerated = true;
    return run;
}

void the_native_density_key_follows_the_runs() {
    // Written by a run that ends above the magnify-off rung, read back under
    // the same engine version and not under another.
    rs::Records records = adapter_records();
    rs::Change change =
        rs::note_density_run_end(records, "alpha", version, density_run(), from_two_gib);
    OA_CHECK(change.changed && !change.new_record);
    OA_CHECK(rs::native_density_driver(records, version) == std::string_view("alpha"));
    OA_CHECK(!rs::native_density_driver(records, "0.6.3").has_value());
    OA_CHECK(!rs::native_density_driver(adapter_records(), version).has_value());
    const rs::ParsedRecords parsed =
        rs::parse_records(rs::format_records(records, version), version);
    OA_CHECK(rs::native_density_driver(parsed.records, version) == std::string_view("alpha"));
    // The same run again changes nothing.
    change = rs::note_density_run_end(records, "alpha", version, density_run(), from_two_gib);
    OA_CHECK(!change.changed);

    // Never written where the flag does nothing, in a class not measured,
    // at budget none, without a passed function test, on software, under a
    // name that cannot be kept or by a run in the standard tier.
    const auto not_written = [](void (*change_run)(rs::DensityRunEnd&), std::string_view driver) {
        rs::Records fresh = adapter_records();
        rs::DensityRunEnd run = density_run();
        change_run(run);
        const rs::Change made = rs::note_density_run_end(fresh, driver, version, run, from_two_gib);
        return !made.changed && !fresh.native_density.has_value();
    };
    OA_CHECK(not_written([](rs::DensityRunEnd& run) { run.flag_honoured = false; }, "alpha"));
    OA_CHECK(not_written([](rs::DensityRunEnd& run) { run.class_measured = false; }, "alpha"));
    OA_CHECK(not_written([](rs::DensityRunEnd& run) { run.above_budget_none = false; }, "alpha"));
    OA_CHECK(
        not_written([](rs::DensityRunEnd& run) { run.function_test_passed = false; }, "alpha")
    );
    OA_CHECK(not_written([](rs::DensityRunEnd& run) { run.accelerated = false; }, "alpha"));
    OA_CHECK(not_written([](rs::DensityRunEnd&) {}, "software"));
    OA_CHECK(not_written([](rs::DensityRunEnd&) {}, "not a name"));

    // A run in the standard tier, with nothing dropped, leaves the key: it
    // is a fact of the machine, not of the setting.
    rs::DensityRunEnd standard_run = density_run();
    standard_run.accelerated = false;
    standard_run.ended_at_or_below_magnify_off = true;
    change = rs::note_density_run_end(records, "alpha", version, standard_run, from_two_gib);
    OA_CHECK(!change.changed);
    OA_CHECK(records.native_density.has_value());

    // Erased by a drop, the memory guard's and the last rung's included, and
    // by a run that ends at or below the magnify-off rung, whatever else
    // holds.
    const auto erased = [](void (*change_run)(rs::DensityRunEnd&)) {
        rs::Records kept = adapter_records();
        kept.native_density = rs::NativeDensity{"alpha", std::string(version)};
        rs::DensityRunEnd run = density_run();
        change_run(run);
        const rs::Change made = rs::note_density_run_end(kept, "beta", version, run, from_two_gib);
        return made.changed && !kept.native_density.has_value();
    };
    OA_CHECK(erased([](rs::DensityRunEnd& run) { run.dropped = true; }));
    OA_CHECK(erased([](rs::DensityRunEnd& run) {
        run.dropped = true;
        run.accelerated = false;
        run.flag_honoured = false;
    }));
    OA_CHECK(erased([](rs::DensityRunEnd& run) { run.ended_at_or_below_magnify_off = true; }));

    // A start whose probe rejects the renderer, or whose function test
    // fails, forgets it; a start with none changes nothing.
    OA_CHECK(rs::forget_native_density(records, from_two_gib).changed);
    OA_CHECK(!records.native_density.has_value());
    OA_CHECK(!rs::forget_native_density(records, from_two_gib).changed);

    // Under 2 GiB, where the accelerated tier never runs, the key a larger
    // machine left stays, and none is written.
    rs::Records small = adapter_records();
    small.native_density = rs::NativeDensity{"alpha", std::string(version)};
    rs::DensityRunEnd dropped = density_run();
    dropped.dropped = true;
    OA_CHECK(!rs::note_density_run_end(small, "alpha", version, dropped, below_two_gib).changed);
    OA_CHECK(!rs::forget_native_density(small, below_two_gib).changed);
    OA_CHECK(small.native_density.has_value());
    rs::Records none = adapter_records();
    change = rs::note_density_run_end(none, "alpha", version, density_run(), below_two_gib);
    OA_CHECK(!change.changed);
    OA_CHECK(!none.native_density.has_value());
}

void the_adapter_clears_records_when_it_changes() {
    rs::Records records;
    rs::driver_entry(records, "alpha").failed_driver =
        rs::Record{rs::RecordedFailure::stopped, false};
    // One that cannot be read changes nothing.
    OA_CHECK(!rs::note_adapter(records, "").changed);
    OA_CHECK(!rs::note_adapter(records, "unknown").changed);
    // The first adapter known is kept, and the records with it.
    OA_CHECK(rs::note_adapter(records, " Example  Graphics 3000 ").changed);
    OA_CHECK(records.adapter == adapter);
    OA_CHECK(failed_driver(records, "alpha") == rs::RecordedFailure::stopped);
    // The same adapter again changes nothing.
    OA_CHECK(!rs::note_adapter(records, "Example Graphics 3000").changed);
    OA_CHECK(!rs::note_adapter(records, "unknown").changed);
    OA_CHECK(records.adapter == adapter);
    // Another adapter clears every failure, but not the native-density key
    // or a standing trial.
    records.native_density = rs::NativeDensity{"alpha", std::string(version)};
    records.trial = rs::Trial{rs::StrikeStage::probe, rs::AcceleratedPath::magnify, "alpha"};
    rs::driver_entry(records, "beta").scale_level = rs::ScaleLevel{1, 400};
    const rs::Change change = rs::note_adapter(records, "Other Graphics 9");
    OA_CHECK(change.changed && !change.new_record);
    OA_CHECK(records.adapter == "Other Graphics 9");
    OA_CHECK(records.drivers.empty());
    OA_CHECK(records.native_density.has_value());
    OA_CHECK(records.trial.has_value());
    // Records written under the old adapter are dropped as the file is read.
    rs::Values values = rs::format_records(adapter_records(), version);
    values["failed-driver.alpha"] = "stopped Example Graphics 3000 0.6.2";
    values["adapter"] = "Other Graphics 9";
    OA_CHECK(rs::parse_records(values, version).records.drivers.empty());
    // And those of another engine version.
    values["adapter"] = std::string(adapter);
    OA_CHECK(rs::parse_records(values, version).records.drivers.size() == 1);
    OA_CHECK(rs::parse_records(values, "0.7.0").records.drivers.empty());
}

void the_told_mark_and_clearing() {
    rs::Records records = adapter_records();
    rs::DriverRecords& alpha = rs::driver_entry(records, "alpha");
    alpha.failed_driver = rs::Record{rs::RecordedFailure::stopped, false};
    alpha.accelerated_unusable = rs::Record{rs::RecordedFailure::lost, false};
    alpha.strike = strike_of(rs::StrikeStage::lost);
    OA_CHECK(rs::has_untold_record(records));
    OA_CHECK(!rs::mark_told(records, "beta").changed);
    OA_CHECK(rs::mark_told(records, "alpha").changed);
    OA_CHECK(!rs::mark_told(records, "alpha").changed);
    OA_CHECK(!rs::has_untold_record(records));
    records = next_start(records);
    OA_CHECK(!rs::has_untold_record(records));
    OA_CHECK(rs::find_driver(records, "alpha")->failed_driver.told);

    // The records are advice to the walk and the tier.
    OA_CHECK(rs::skips_driver(records, "alpha"));
    OA_CHECK(!rs::skips_driver(records, "beta"));
    OA_CHECK(!rs::acceleration_allowed(records, "alpha", false));
    OA_CHECK(rs::acceleration_allowed(records, "alpha", true)); // --hardware-acceleration
    OA_CHECK(rs::acceleration_allowed(records, "beta", false));
    rs::Records software = adapter_records();
    rs::driver_entry(software, "software").failed_driver =
        rs::Record{rs::RecordedFailure::stopped, false};
    OA_CHECK(!rs::skips_driver(software, "software"));

    // Off then On and Restore defaults clear the strikes, both records and the
    // remembered rungs, and keep the adapter, the native-density key and a
    // standing trial.
    records.native_density = rs::NativeDensity{"beta", std::string(version)};
    records.trial = rs::Trial{rs::StrikeStage::probe, rs::AcceleratedPath::magnify, "beta"};
    rs::driver_entry(records, "beta").scale_level = rs::ScaleLevel{2, 700};
    const rs::Change cleared = rs::clear_failures(records);
    OA_CHECK(cleared.changed && !cleared.new_record);
    OA_CHECK(records.drivers.empty());
    OA_CHECK(records.adapter == adapter);
    OA_CHECK(records.native_density.has_value());
    OA_CHECK(records.trial.has_value());
    OA_CHECK(!rs::skips_driver(records, "alpha"));
    OA_CHECK(rs::acceleration_allowed(records, "alpha", false));
    OA_CHECK(!rs::clear_failures(records).changed);
}

void the_notice_tells_each_new_record_once() {
    rs::Records records = adapter_records();
    const std::vector<std::string> none;
    OA_CHECK(!rs::next_notice(records, none));
    // A strike shows no notice.
    rs::driver_entry(records, "alpha").strike = strike_of(rs::StrikeStage::create);
    OA_CHECK(!rs::next_notice(records, none));
    // An accelerated-unusable record, then a failed-driver one, in the order
    // the drivers were noted; a driver with both has failed-driver's.
    rs::driver_entry(records, "beta").accelerated_unusable =
        rs::Record{rs::RecordedFailure::stopped, false};
    rs::DriverRecords& gamma = rs::driver_entry(records, "gamma");
    gamma.failed_driver = rs::Record{rs::RecordedFailure::lost, false};
    gamma.accelerated_unusable = rs::Record{rs::RecordedFailure::lost, false};
    auto notice = rs::next_notice(records, none);
    OA_CHECK(
        notice && notice->driver == "beta" && notice->kind == rs::NoticeKind::accelerated_unusable
    );
    // A notice passed over in this run, as a run nobody watches notes it,
    // leaves the next.
    notice = rs::next_notice(records, {"beta"});
    OA_CHECK(notice && notice->driver == "gamma" && notice->kind == rs::NoticeKind::failed_driver);
    OA_CHECK(!rs::next_notice(records, {"beta", "gamma"}));
    // Told records show nothing again; one told mark covers both of a
    // driver's records.
    OA_CHECK(rs::mark_told(records, "beta").changed);
    OA_CHECK(rs::mark_told(records, "gamma").changed);
    OA_CHECK(!rs::next_notice(records, none));
    // A new record of a told driver is told again.
    rs::driver_entry(records, "beta").failed_driver =
        rs::Record{rs::RecordedFailure::stopped, false};
    notice = rs::next_notice(records, none);
    OA_CHECK(notice && notice->driver == "beta" && notice->kind == rs::NoticeKind::failed_driver);

    // A run someone watches shows it; one nobody watches notes it and
    // leaves it untold; the ladder check notes it and marks it told.
    OA_CHECK(rs::notice_action(false, false) == rs::NoticeAction::show);
    OA_CHECK(rs::notice_action(false, true) == rs::NoticeAction::show);
    OA_CHECK(rs::notice_action(true, false) == rs::NoticeAction::note);
    OA_CHECK(rs::notice_action(true, true) == rs::NoticeAction::note_and_mark);
}

void the_full_tier_has_its_own_path_strike_and_record() {
    using F = rs::RecordedFailure;
    using K = rs::StrikeStage;
    const auto full = rs::AcceleratedPath::full;

    // The key, the trial, the sentinel and the strike each have their text,
    // and read back as written.
    rs::Records records = adapter_records();
    records.trial = rs::Trial{K::path, full, "alpha"};
    rs::DriverRecords& alpha = rs::driver_entry(records, "alpha");
    alpha.full_unusable = rs::Record{F::card, false};
    alpha.strike = call_strike(K::card, "page");
    rs::driver_entry(records, "beta").full_unusable = rs::Record{F::stopped, true};
    rs::driver_entry(records, "beta").accelerated_unusable = rs::Record{F::lost, false};
    const rs::Values values = rs::format_records(records, version);
    const rs::Values expected{
        {"accelerated-unusable.beta", "lost Example Graphics 3000 0.6.2"},
        {"adapter", "Example Graphics 3000"},
        {"full-unusable.alpha", "card Example Graphics 3000 0.6.2"},
        {"full-unusable.beta", "stopped Example Graphics 3000 0.6.2 told"},
        {"strike.alpha", "card page Example Graphics 3000 0.6.2"},
        {"trial", "path full alpha"},
    };
    OA_CHECK(values == expected);
    const rs::ParsedRecords parsed = rs::parse_records(values, version);
    OA_CHECK(parsed.dropped == 0);
    OA_CHECK(rs::same_records(parsed.records, records));
    OA_CHECK(full_unusable(parsed.records, "alpha") == F::card);
    OA_CHECK(rs::find_driver(parsed.records, "beta")->full_unusable.told);
    const auto sentinel = rs::parse_sentinel("path full alpha");
    OA_CHECK(sentinel && sentinel->stage == rs::SentinelStage::path && sentinel->path == full);
    const auto trial = rs::parse_trial("path full alpha");
    OA_CHECK(trial && trial->stage == K::path && trial->path == full && trial->driver == "alpha");
    // Each kind of record says only the failures it records: full-unusable a
    // stop or a card call, and the other two never a card call.
    for (const char* key :
         {"full-unusable.alpha", "accelerated-unusable.alpha", "failed-driver.alpha"}) {
        for (const char* word : {"stopped", "present", "call", "lost", "resets", "card"}) {
            const rs::Values one{
                {"adapter", std::string(adapter)},
                {key, std::string(word) + " Example Graphics 3000 0.6.2"}
            };
            const std::string_view kind = key;
            const std::string_view failure = word;
            const bool kept = kind.starts_with("full-unusable")
                                  ? failure == "stopped" || failure == "card"
                              : kind.starts_with("accelerated-unusable")
                                  ? failure != "present" && failure != "card"
                                  : failure != "call" && failure != "card";
            OA_CHECK(rs::parse_records(one, version).dropped == (kept ? 0U : 1U));
        }
    }
    // A card strike without its call is dropped.
    OA_CHECK(
        rs::parse_records(
            rs::Values{
                {"adapter", std::string(adapter)},
                {"strike.alpha", "card Example Graphics 3000 0.6.2"}
            },
            version
        )
            .dropped == 1
    );

    // A left-over Full trial is a strike, and the second in a row records
    // full-unusable, which leaves accelerated-unusable and the
    // native-density key alone; where the first counts, at once.
    for (const rs::CrashEvidence evidence :
         {rs::CrashEvidence::two_in_a_row, rs::CrashEvidence::first_counts}) {
        rs::Records left = adapter_records();
        left.native_density = rs::NativeDensity{"alpha", std::string(version)};
        const rs::RecordRules rules{evidence, false};
        const auto leave = [&]() {
            left.trial = rs::Trial{K::path, full, "alpha"};
            return rs::note_leftover(
                left,
                rs::LeftoverSentinel::read,
                sentinel_of(rs::SentinelStage::path, "alpha"),
                rules
            );
        };
        const rs::LeftoverOutcome first = leave();
        const bool at_once = evidence == rs::CrashEvidence::first_counts;
        OA_CHECK(first.driver == "alpha" && first.unclean_exit && first.change.changed);
        OA_CHECK(first.change.new_record == at_once);
        OA_CHECK(!left.trial.has_value());
        if (at_once) {
            OA_CHECK(full_unusable(left, "alpha") == F::stopped);
            OA_CHECK(strike_stage(left, "alpha") == K::none);
        } else {
            OA_CHECK(full_unusable(left, "alpha") == F::none);
            const rs::DriverRecords* struck = rs::find_driver(left, "alpha");
            OA_CHECK(
                struck != nullptr && struck->strike.stage == K::path && struck->strike.path == full
            );
            left = next_start(left);
            const rs::LeftoverOutcome second = leave();
            OA_CHECK(second.change.new_record);
            OA_CHECK(full_unusable(left, "alpha") == F::stopped);
            OA_CHECK(strike_stage(left, "alpha") == K::none);
        }
        OA_CHECK(accelerated_unusable(left, "alpha") == F::none);
        OA_CHECK(failed_driver(left, "alpha") == F::none);
        OA_CHECK(left.native_density.has_value());
        OA_CHECK(rs::has_untold_record(left));
        OA_CHECK(!rs::full_allowed(left, "alpha", false));
        OA_CHECK(rs::full_allowed(left, "alpha", true)); // --hardware-acceleration=full
        OA_CHECK(rs::full_allowed(left, "beta", false));
        OA_CHECK(rs::acceleration_allowed(left, "alpha", false));
        // The notice tells of it, after the other kinds, and one told mark
        // covers it.
        const auto notice = rs::next_notice(left, {});
        OA_CHECK(
            notice && notice->driver == "alpha" && notice->kind == rs::NoticeKind::full_unusable
        );
        OA_CHECK(rs::mark_told(left, "alpha").changed);
        OA_CHECK(!rs::has_untold_record(left));
        OA_CHECK(rs::find_driver(next_start(left), "alpha")->full_unusable.told);
    }
    // A Full path's first frames passing clear its strike, as any path's do.
    rs::Records passing = adapter_records();
    passing.trial = rs::Trial{K::path, full, "alpha"};
    (void)rs::note_leftover(passing, rs::LeftoverSentinel::none, rs::Sentinel{}, from_two_gib);
    OA_CHECK(strike_stage(passing, "alpha") == K::path);
    OA_CHECK(!rs::note_path_passed(passing, "alpha", rs::AcceleratedPath::magnify).changed);
    OA_CHECK(rs::note_path_passed(passing, "alpha", full).changed);
    OA_CHECK(strike_stage(passing, "alpha") == K::none);
    // Under 2 GiB a left-over Full trial stays for a larger machine to judge.
    rs::Records small = adapter_records();
    small.trial = rs::Trial{K::path, full, "alpha"};
    OA_CHECK(!rs::note_leftover(small, rs::LeftoverSentinel::none, rs::Sentinel{}, below_two_gib)
                  .change.changed);
    OA_CHECK(small.trial.has_value() && small.drivers.empty());

    // A card call that fails is a strike, and the same in the next run
    // records full-unusable; another call is a new strike; on software too,
    // since the Full tier draws there under --force-capable; nothing where
    // the device is lost in ordinary use; and nothing under 2 GiB.
    for (const char* driver : {"alpha", "software"}) {
        rs::Records failing = adapter_records();
        const rs::Strike page = call_strike(K::card, "page");
        rs::Change change = rs::note_running_failure(failing, driver, page, {}, from_two_gib);
        OA_CHECK(change.changed && !change.new_record);
        OA_CHECK(!rs::note_running_failure(failing, driver, page, {}, from_two_gib).changed);
        OA_CHECK(strike_stage(failing, driver) == K::card);
        OA_CHECK(full_unusable(failing, driver) == F::none);
        OA_CHECK(accelerated_unusable(failing, driver) == F::none);
        failing = next_start(failing);
        change = rs::note_running_failure(
            failing, driver, call_strike(K::card, "target"), {}, from_two_gib
        );
        OA_CHECK(change.changed && !change.new_record);
        OA_CHECK(rs::find_driver(failing, driver)->strike.call == "target");
        failing = next_start(failing);
        change = rs::note_running_failure(
            failing, driver, call_strike(K::card, "target"), {}, from_two_gib
        );
        OA_CHECK(change.changed && change.new_record);
        OA_CHECK(full_unusable(failing, driver) == F::card);
        OA_CHECK(strike_stage(failing, driver) == K::none);
        OA_CHECK(accelerated_unusable(failing, driver) == F::none);
        OA_CHECK(failed_driver(failing, driver) == F::none);
        OA_CHECK(!rs::full_allowed(failing, driver, false));
    }
    rs::Records ordinary = adapter_records();
    OA_CHECK(
        !rs::note_running_failure(
             ordinary, "alpha", call_strike(K::card, "page"), rs::DriverFacts{true}, from_two_gib
        )
             .changed
    );
    rs::Records small_run = adapter_records();
    OA_CHECK(!rs::note_running_failure(
                  small_run, "alpha", call_strike(K::card, "page"), {}, below_two_gib
    )
                  .changed);
    OA_CHECK(small_run.drivers.empty());
    // A card strike without its call counts for nothing.
    OA_CHECK(
        !rs::note_running_failure(small_run, "alpha", strike_of(K::card), {}, from_two_gib).changed
    );
    // A clean run clears a card strike from an earlier run, from 2 GiB
    // alone.
    rs::Records clean = adapter_records();
    rs::driver_entry(clean, "alpha").strike = call_strike(K::card, "page");
    rs::Records clean_small = clean;
    OA_CHECK(!rs::note_clean_run(clean_small, "alpha", below_two_gib).changed);
    OA_CHECK(rs::note_clean_run(clean, "alpha", from_two_gib).changed);
    OA_CHECK(strike_stage(clean, "alpha") == K::none);
    // Off then On, a raise to Full and Restore defaults clear the record.
    rs::Records cleared = adapter_records();
    rs::driver_entry(cleared, "alpha").full_unusable = rs::Record{F::card, true};
    OA_CHECK(rs::clear_failures(cleared).changed);
    OA_CHECK(rs::full_allowed(cleared, "alpha", false));
    // The strikes of the two card tiers and the paths are each their own.
    OA_CHECK(!rs::same_strike(call_strike(K::card, "page"), call_strike(K::call, "page")));
    OA_CHECK(rs::same_strike(call_strike(K::card, "page"), call_strike(K::card, "page")));
    OA_CHECK(!rs::same_strike(call_strike(K::card, "page"), call_strike(K::card, "target")));
    rs::Strike magnify = strike_of(K::path);
    rs::Strike full_path = strike_of(K::path);
    full_path.path = full;
    OA_CHECK(!rs::same_strike(magnify, full_path));
}

void a_left_over_trial_decides_alone() {
    // The trial decides whatever the sentinel holds, one that names another
    // driver included: only the trial's driver is struck.
    struct Leftover {
        const char* name;
        rs::LeftoverSentinel kind;
        rs::SentinelStage stage;
    };

    const Leftover leftovers[] = {
        {"create sentinel of another driver",
         rs::LeftoverSentinel::read,
         rs::SentinelStage::create},
        {"standard sentinel of another driver",
         rs::LeftoverSentinel::read,
         rs::SentinelStage::standard},
        {"running sentinel of another driver",
         rs::LeftoverSentinel::read,
         rs::SentinelStage::running},
        {"unreadable sentinel", rs::LeftoverSentinel::unreadable, rs::SentinelStage::create},
        {"no sentinel", rs::LeftoverSentinel::none, rs::SentinelStage::create},
    };
    for (const Leftover& leftover : leftovers) {
        const int failures_before = oa::test::failed_checks();
        rs::Records records = adapter_records();
        records.trial = rs::Trial{rs::StrikeStage::probe, rs::AcceleratedPath::magnify, "alpha"};
        const rs::LeftoverOutcome outcome = rs::note_leftover(
            records, leftover.kind, sentinel_of(leftover.stage, "beta"), from_two_gib
        );
        OA_CHECK(outcome.driver == "alpha");
        OA_CHECK(outcome.unclean_exit);
        OA_CHECK(!records.trial.has_value());
        OA_CHECK(strike_stage(records, "alpha") == rs::StrikeStage::probe);
        OA_CHECK(rs::find_driver(records, "beta") == nullptr);
        if (oa::test::failed_checks() != failures_before)
            std::fprintf(stderr, "in the left-over \"%s\"\n", leftover.name);
    }

    // A path's trial is a strike of that path; another path's trial left
    // over at the next start replaces it, and is no record.
    rs::Records records = adapter_records();
    records.trial = rs::Trial{rs::StrikeStage::path, rs::AcceleratedPath::magnify, "alpha"};
    (void)rs::note_leftover(records, rs::LeftoverSentinel::none, rs::Sentinel{}, from_two_gib);
    records.trial = rs::Trial{rs::StrikeStage::path, rs::AcceleratedPath::prescale, "alpha"};
    records = next_start(records);
    OA_CHECK(records.trial && records.trial->path == rs::AcceleratedPath::prescale);
    const rs::LeftoverOutcome other =
        rs::note_leftover(records, rs::LeftoverSentinel::none, rs::Sentinel{}, from_two_gib);
    OA_CHECK(other.change.changed && !other.change.new_record);
    OA_CHECK(accelerated_unusable(records, "alpha") == rs::RecordedFailure::none);
    const rs::DriverRecords* alpha = rs::find_driver(records, "alpha");
    OA_CHECK(alpha != nullptr && alpha->strike.stage == rs::StrikeStage::path);
    OA_CHECK(alpha != nullptr && alpha->strike.path == rs::AcceleratedPath::prescale);
}

void a_failure_repeated_in_one_run_counts_once() {
    using K = rs::StrikeStage;
    using F = rs::RecordedFailure;
    // The same failure again and again in one run, as when a rebuild returns
    // to the driver, is that run's alone: its clean end keeps the strike, and
    // the same failure in the next run records.
    for (const rs::Strike& failure :
         {call_strike(K::present, "present-frame"),
          call_strike(K::call, "upload"),
          strike_of(K::lost),
          strike_of(K::resets)}) {
        rs::Records records = adapter_records();
        for (int again = 0; again < 3; ++again)
            (void)rs::note_running_failure(records, "alpha", failure, {}, from_two_gib);
        const rs::DriverRecords* alpha = rs::find_driver(records, "alpha");
        OA_CHECK(alpha != nullptr && alpha->struck_this_run);
        OA_CHECK(strike_stage(records, "alpha") == failure.stage);
        OA_CHECK(failed_driver(records, "alpha") == F::none);
        if (failure.stage == K::call)
            OA_CHECK(accelerated_unusable(records, "alpha") == F::none);
        OA_CHECK(!rs::note_clean_run(records, "alpha", from_two_gib).changed);
        records = next_start(records);
        OA_CHECK(rs::note_running_failure(records, "alpha", failure, {}, from_two_gib).new_record);
        if (failure.stage == K::call)
            OA_CHECK(accelerated_unusable(records, "alpha") == F::call);
        else
            OA_CHECK(failed_driver(records, "alpha") != F::none);
    }

    // A lost device, then three resets in the next run, are not the same
    // evidence.
    rs::Records records = adapter_records();
    (void)rs::note_running_failure(records, "alpha", strike_of(K::lost), {}, from_two_gib);
    records = next_start(records);
    (void)rs::note_running_failure(records, "alpha", strike_of(K::resets), {}, from_two_gib);
    OA_CHECK(failed_driver(records, "alpha") == F::none);
    OA_CHECK(strike_stage(records, "alpha") == K::resets);
    // Nor are two lost devices with a clean run between them.
    records = adapter_records();
    (void)rs::note_running_failure(records, "alpha", strike_of(K::lost), {}, from_two_gib);
    records = next_start(records);
    OA_CHECK(rs::note_clean_run(records, "alpha", from_two_gib).changed);
    records = next_start(records);
    (void)rs::note_running_failure(records, "alpha", strike_of(K::lost), {}, from_two_gib);
    OA_CHECK(failed_driver(records, "alpha") == F::none);
    OA_CHECK(strike_stage(records, "alpha") == K::lost);
}

void strikes_compare_and_drivers_keep_their_order() {
    using K = rs::StrikeStage;
    OA_CHECK(rs::same_strike(rs::Strike{}, rs::Strike{}));
    OA_CHECK(!rs::same_strike(call_strike(K::call, "upload"), call_strike(K::call, "draw")));
    OA_CHECK(!rs::same_strike(call_strike(K::present, "upload"), call_strike(K::call, "upload")));
    rs::Strike blend = strike_of(K::path);
    blend.path = rs::AcceleratedPath::blend;
    OA_CHECK(!rs::same_strike(strike_of(K::path), blend));
    // Only a stage with a path or a call compares it.
    rs::Strike lost = strike_of(K::lost);
    lost.path = rs::AcceleratedPath::blend;
    lost.call = "upload";
    OA_CHECK(rs::same_strike(strike_of(K::lost), lost));

    // Entries keep the order they were first noted in, and the walk skips
    // the drivers recorded failed-driver, never software.
    rs::Records records = adapter_records();
    rs::driver_entry(records, "alpha").failed_driver =
        rs::Record{rs::RecordedFailure::stopped, false};
    (void)rs::driver_entry(records, "beta");
    rs::driver_entry(records, "software").failed_driver =
        rs::Record{rs::RecordedFailure::stopped, false};
    rs::driver_entry(records, "gamma").failed_driver =
        rs::Record{rs::RecordedFailure::present, true};
    OA_CHECK(records.drivers.size() == 4);
    OA_CHECK(&rs::driver_entry(records, "beta") == &records.drivers[1]);
    OA_CHECK(rs::failed_driver_list(records) == (std::vector<std::string_view>{"alpha", "gamma"}));
    OA_CHECK(rs::failed_driver_list(adapter_records()).empty());

    // Entries that hold nothing go without counting as a change.
    rs::Records empty = adapter_records();
    (void)rs::driver_entry(empty, "alpha");
    OA_CHECK(!rs::clear_failures(empty).changed);
    OA_CHECK(empty.drivers.empty());
}

/// Moves a run's sentinel and writes down what the step writes: `trial` and
/// the trial's value, the sentinel's new value for the driver `d`, and
/// `erase` for the trial's erasure, separated by `; `; `-` for nothing.
///
/// @param[in,out] life the run's sentinel
/// @param event the event
/// @param path the path, for the path events
/// @return what the step writes
std::string step(
    rs::SentinelLife& life,
    rs::LifeEvent event,
    rs::AcceleratedPath path = rs::AcceleratedPath::magnify
) {
    const rs::SentinelWrite write = rs::sentinel_step(life, event, path);
    std::string text;
    const auto add = [&text](const std::string& part) {
        if (!text.empty())
            text += "; ";
        text += part;
    };
    if (write.write_trial)
        add("trial " + rs::format_trial(rs::Trial{write.trial_stage, write.trial_path, "d"}));
    if (write.set_sentinel) {
        rs::Sentinel sentinel = sentinel_of(write.sentinel, "d");
        sentinel.path = write.sentinel_path;
        add(rs::format_sentinel(sentinel));
    }
    if (write.erase_trial)
        add("erase");
    return text.empty() ? std::string("-") : text;
}

void the_sentinel_and_the_trial_through_a_run() {
    using E = rs::LifeEvent;
    using P = rs::AcceleratedPath;
    // A start that runs the function test and passes its start-up stage;
    // a path first used under the start-up sentinel writes nothing of its
    // own, and each later path brackets its first frames.
    rs::SentinelLife life = rs::start_sentinel_life(false);
    OA_CHECK(!life.stage.has_value());
    OA_CHECK(step(life, E::path_first_use, P::magnify) == "-");
    OA_CHECK(step(life, E::creating) == "create d");
    OA_CHECK(step(life, E::creating) == "create d");
    OA_CHECK(step(life, E::probing) == "standard d");
    OA_CHECK(step(life, E::function_test) == "trial probe d; probe d");
    OA_CHECK(step(life, E::function_test_done) == "standard d");
    OA_CHECK(step(life, E::path_first_use, P::prescale) == "-");
    OA_CHECK(step(life, E::first_accelerated_frame) == "accelerated d");
    OA_CHECK(step(life, E::first_accelerated_frame) == "-");
    OA_CHECK(step(life, E::path_passed, P::prescale) == "-");
    OA_CHECK(step(life, E::start_passed) == "running d; erase");
    OA_CHECK(step(life, E::start_passed) == "-");
    OA_CHECK(step(life, E::path_first_use, P::magnify) == "trial path magnify d; path magnify d");
    OA_CHECK(step(life, E::path_passed, P::prescale) == "-");
    OA_CHECK(step(life, E::path_first_use, P::blend) == "-");
    OA_CHECK(step(life, E::path_passed, P::magnify) == "running d; erase");
    OA_CHECK(step(life, E::path_first_use, P::blend) == "trial path blend d; path blend d");
    OA_CHECK(step(life, E::path_passed, P::blend) == "running d; erase");

    // A start with no function test: standard, then running, with no trial
    // to erase.
    life = rs::start_sentinel_life(false);
    OA_CHECK(step(life, E::creating) == "create d");
    OA_CHECK(step(life, E::probing) == "standard d");
    OA_CHECK(step(life, E::function_test_done) == "-");
    OA_CHECK(step(life, E::start_passed) == "running d");
    // The function test run later, when the player turns the setting On:
    // its trial stands through the first accelerated frames.
    OA_CHECK(step(life, E::function_test) == "trial probe d; probe d");
    OA_CHECK(step(life, E::function_test_done) == "standard d");
    OA_CHECK(step(life, E::first_accelerated_frame) == "accelerated d");
    OA_CHECK(step(life, E::start_passed) == "running d; erase");

    // A trial that cannot be written, as under 2 GiB where none is, skips
    // its stage: the sentinel stands where it stood, and no trial is erased
    // later.
    life = rs::start_sentinel_life(false);
    (void)step(life, E::creating);
    (void)step(life, E::probing);
    OA_CHECK(step(life, E::function_test) == "trial probe d; probe d");
    rs::note_trial_unwritten(life);
    OA_CHECK(life.stage == rs::SentinelStage::standard && !life.trial);
    OA_CHECK(step(life, E::function_test_done) == "-");
    OA_CHECK(step(life, E::start_passed) == "running d");
    OA_CHECK(step(life, E::path_first_use, P::magnify) == "trial path magnify d; path magnify d");
    rs::note_trial_unwritten(life);
    OA_CHECK(life.stage == rs::SentinelStage::running && !life.trial);
    OA_CHECK(step(life, E::path_passed, P::magnify) == "-");
    rs::note_trial_unwritten(life);
    OA_CHECK(life.stage == rs::SentinelStage::running);
    // A function test before any other stage that cannot write its trial
    // leaves no sentinel standing.
    life = rs::start_sentinel_life(false);
    OA_CHECK(step(life, E::function_test) == "trial probe d; probe d");
    rs::note_trial_unwritten(life);
    OA_CHECK(!life.stage.has_value() && !life.trial);

    // Under SDL_RENDER_DRIVER nothing is written at all.
    life = rs::start_sentinel_life(true);
    for (const rs::LifeEvent event :
         {E::creating,
          E::probing,
          E::function_test,
          E::function_test_done,
          E::first_accelerated_frame,
          E::start_passed,
          E::path_first_use,
          E::path_passed})
        OA_CHECK(step(life, event) == "-");

    // The start-up stage passes after 60 frames and 2 s, both.
    OA_CHECK(!rs::start_stage_passed(rs::start_stage_frames - 1, 3 * rs::start_stage_ns));
    OA_CHECK(!rs::start_stage_passed(10 * rs::start_stage_frames, rs::start_stage_ns - 1));
    OA_CHECK(rs::start_stage_passed(rs::start_stage_frames, rs::start_stage_ns));

    // The trial a step writes is the records' trial, which the next start
    // reads back, until the step that erases it.
    life = rs::start_sentinel_life(false);
    (void)rs::sentinel_step(life, E::probing, P::magnify);
    const rs::SentinelWrite before = rs::sentinel_step(life, E::function_test, P::magnify);
    OA_CHECK(before.write_trial);
    rs::Records records = adapter_records();
    records.trial = rs::Trial{before.trial_stage, before.trial_path, "alpha"};
    records = next_start(records);
    OA_CHECK(records.trial && records.trial->stage == rs::StrikeStage::probe);
    OA_CHECK(records.trial && records.trial->driver == "alpha");
    (void)rs::sentinel_step(life, E::function_test_done, P::magnify);
    OA_CHECK(rs::sentinel_step(life, E::start_passed, P::magnify).erase_trial);
}

} // namespace

int main() {
    crash_evidence_counts_the_first_trial_before_vista_and_on_linux();
    every_key_round_trips();
    values_that_cannot_be_read_are_dropped_alone();
    a_new_build_starts_free_of_an_older_ones_failures();
    entries_beyond_the_driver_limit_are_dropped();
    sentinels_and_trials_read_and_write();
    adapters_are_kept_in_one_form();
    a_leftover_makes_a_strike_then_a_record();
    a_clean_pass_between_two_leftovers_clears_the_strike();
    a_failure_while_running_is_recorded_in_the_next_run();
    under_two_gib_nothing_of_the_accelerated_tier_is_made();
    accelerated_unusable_takes_away_native_density();
    the_native_density_key_follows_the_runs();
    the_adapter_clears_records_when_it_changes();
    the_told_mark_and_clearing();
    the_notice_tells_each_new_record_once();
    the_full_tier_has_its_own_path_strike_and_record();
    a_left_over_trial_decides_alone();
    a_failure_repeated_in_one_run_counts_once();
    strikes_compare_and_drivers_keep_their_order();
    the_sentinel_and_the_trial_through_a_run();
    return oa::test::check_exit_status();
}
