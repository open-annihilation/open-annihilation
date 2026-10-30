// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The network rules a profile gives network play: none and the baseline
// profile give 3.1c's rules exactly, a profile with every network and
// recorder hack on gives each its rule, and every reference profile in the
// folder OA_MOD_PROFILES_DIR names (when set) gives rules that agree with
// its hacks.

#include "wire_rules_binding.hpp"

#include "oa/platform/system.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace mp = oa::data::mod_profile;
namespace ng = oa::netgame;
namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ++failures;                                                                            \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
        }                                                                                          \
    } while (0)

/// Resolves a profile's text, accepting hacks not marked implemented.
std::optional<mp::ModProfile> resolve(std::string_view text) {
    mp::ResolveOptions options{};
    options.accept_unimplemented_hacks = true;
    const auto result = mp::resolve_profile(
        {reinterpret_cast<const uint8_t*>(text.data()), text.size()}, "test", options
    );
    for (const auto& error : result.errors)
        std::fprintf(stderr, "%s\n", mp::format_diagnostic(error).c_str());
    if (!result.resolution)
        return std::nullopt;
    return result.resolution->profile;
}

void no_profile_and_baseline_are_3_1c() {
    CHECK(oa::app::netgame::wire_rules_of(nullptr) == ng::WireRules{});
    const auto baseline = resolve(mp::baseline_profile_text());
    CHECK(baseline.has_value());
    if (baseline)
        CHECK(oa::app::netgame::wire_rules_of(&*baseline) == ng::WireRules{});
}

constexpr std::string_view every_rule = R"(oamod: 1
id: every-network-rule
name: "Every network rule"
version: "1.0"
requires: {base: ta-3.1c, catalogue: 1}
author: {name: unknown}
packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}
identity:
  network-version: [7, 4]
  replay-network-version: [3, 1]
hacks:
  console.game-speed-range: true
  network.chat-extension-channel: true
  network.vercheck: periodic-challenge
  network.vote-reject: {vote-timeout-s: 45, timeout-vote-s: 80}
  network.lag-guard: 250
  network.commander-start-sync: true
  network.preserve-remote-player-colour: true
  network.session-desc-clear: true
  network.host-stays-as-watcher: true
  network.recorder-session-commands: true
  recorder.ta-demo-recorder: true
  recorder.ten-player-replay: allied-fake-player
)";

void every_rule_maps() {
    const auto profile = resolve(every_rule);
    CHECK(profile.has_value());
    if (!profile)
        return;
    const auto rules = oa::app::netgame::wire_rules_of(&*profile);
    CHECK(rules.version_major == 7 && rules.version_minor == 4);
    CHECK(rules.version_rule == ng::VersionRule::equal && !rules.launch_version_bias);
    CHECK(rules.replay_version_major == 3 && rules.replay_version_minor == 1);
    CHECK(rules.private_channel == ng::PrivateChannel::sub_id_dispatch);
    CHECK(rules.integrity_check == ng::IntegrityCheck::periodic_challenge);
    CHECK(rules.vote_reject && rules.vote_seconds == 45 && rules.timeout_vote_seconds == 80);
    CHECK(rules.lag_guard_ms == 250 && rules.commander_sync_tick == 90);
    CHECK(rules.keep_remote_colour && rules.clear_session_password && rules.host_stays_watching);
    CHECK(rules.recorder_session_commands);
    CHECK(rules.recorder_protocol == ng::recorder_protocol_current);
    CHECK(rules.ten_player_replay == ng::TenPlayerReplay::allied_fake_player);
    CHECK(rules.speed_min == 0 && rules.speed_max == 20 && rules.speed_lock);
}

// The host's speed lock follows syncon alone.
constexpr std::string_view no_speed_lock = R"(oamod: 1
id: no-speed-lock
name: "No speed lock"
version: "1.0"
requires: {base: ta-3.1c, catalogue: 1}
author: {name: unknown}
packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}
hacks:
  console.game-speed-range: {min: 5, max: 15, syncon: false}
  recorder.ta-demo-recorder: true
)";

constexpr std::string_view wide_limits = R"(oamod: 1
id: wide-limits
name: "Wide limits"
version: "1.0"
requires: {base: ta-3.1c, catalogue: 1}
author: {name: unknown}
packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}
limits: {weapon-types: 1024, sides: 10}
)";

constexpr std::string_view sides_only = R"(oamod: 1
id: sides-only
name: "Sides only"
version: "1.0"
requires: {base: ta-3.1c, catalogue: 1}
author: {name: unknown}
packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}
limits: {sides: 7}
)";

constexpr std::string_view baseline_limits = R"(oamod: 1
id: baseline-limits
name: "Baseline limits"
version: "1.0"
requires: {base: ta-3.1c, catalogue: 1}
author: {name: unknown}
packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}
limits: {weapon-types: 256, sides: 5}
)";

void profile_limits_choose_the_records() {
    const auto wide = resolve(wide_limits);
    CHECK(wide.has_value());
    if (wide) {
        const auto rules = oa::app::netgame::wire_rules_of(&*wide);
        CHECK(rules.weapon_types == 1024 && rules.sides == 10);
        CHECK(ng::wide_weapon_records(rules) && ng::oa_only(rules));
    }
    const auto sides = resolve(sides_only);
    CHECK(sides.has_value());
    if (sides) {
        const auto rules = oa::app::netgame::wire_rules_of(&*sides);
        CHECK(rules.weapon_types == ng::wire_base_weapon_types && rules.sides == 7);
        CHECK(ng::oa_only(rules) && !ng::wide_weapon_records(rules));
    }
    const auto baseline = resolve(baseline_limits);
    CHECK(baseline.has_value());
    if (baseline)
        CHECK(oa::app::netgame::wire_rules_of(&*baseline) == ng::WireRules{});
}

void speed_lock_follows_syncon() {
    const auto profile = resolve(no_speed_lock);
    CHECK(profile.has_value());
    if (!profile)
        return;
    const auto rules = oa::app::netgame::wire_rules_of(&*profile);
    CHECK(rules.speed_min == 5 && rules.speed_max == 15 && !rules.speed_lock);
    CHECK(rules.recorder_protocol == ng::recorder_protocol_current);
}

// Without the channel nothing rides on it: the check and the votes stay off.
constexpr std::string_view no_channel = R"(oamod: 1
id: no-channel
name: "No channel"
version: "1.0"
requires: {base: ta-3.1c, catalogue: 1}
author: {name: unknown}
packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}
hacks:
  network.vercheck: true
  network.vote-reject: true
)";

void rules_need_their_channel() {
    const auto profile = resolve(no_channel);
    CHECK(profile.has_value());
    if (!profile)
        return;
    const auto rules = oa::app::netgame::wire_rules_of(&*profile);
    CHECK(rules.integrity_check == ng::IntegrityCheck::off && !rules.vote_reject);
    CHECK(rules.version_rule == ng::VersionRule::at_least && rules.launch_version_bias);
}

/// Checks every reference profile's rules against its hacks.
///
/// @return how many profiles were read
int reference_profiles() {
    const auto named = oa::platform::environment_value("OA_MOD_PROFILES_DIR");
    if (!named || named->empty() || !fs::is_directory(*named)) {
        std::printf(
            "netgame-wire-rules: the reference profiles are skipped; "
            "OA_MOD_PROFILES_DIR names no folder\n"
        );
        return 0;
    }
    int read = 0;
    for (const auto& directory : fs::directory_iterator{*named}) {
        const auto file = directory.path() / "oamod.yaml";
        if (!fs::is_regular_file(file))
            continue;
        std::ifstream in{file, std::ios::binary};
        const std::string text{std::istreambuf_iterator<char>{in}, {}};
        const auto profile = resolve(text);
        CHECK(profile.has_value());
        if (!profile)
            continue;
        ++read;
        const auto rules = oa::app::netgame::wire_rules_of(&*profile);
        const auto& identity = profile->identity;
        const bool base_version =
            identity.network_version[0] == 3 && identity.network_version[1] == 1;
        CHECK(rules.version_major == identity.network_version[0]);
        CHECK(rules.version_minor == identity.network_version[1]);
        CHECK((rules.version_rule == ng::VersionRule::equal) == !base_version);
        CHECK(
            (rules.recorder_protocol == ng::recorder_protocol_current) ==
            profile->recorder.ta_demo_recorder.enabled
        );
        CHECK(rules.commander_sync_tick == profile->network.commander_start_sync.tick);
        CHECK(rules.lag_guard_ms == profile->network.lag_guard.gap_ms);
        const auto& speed = profile->rules.console.game_speed_range;
        CHECK(rules.speed_lock == (speed.enabled && speed.syncon));
        std::printf(
            "%s: version %d.%d (%s), channel %d, check %d, votes %d, lag guard %d ms, start sync "
            "%d, recorder %d, ten players %d\n",
            file.string().c_str(),
            rules.version_major,
            rules.version_minor,
            rules.version_rule == ng::VersionRule::equal ? "equal" : "at least",
            static_cast<int>(rules.private_channel),
            static_cast<int>(rules.integrity_check),
            rules.vote_reject ? 1 : 0,
            rules.lag_guard_ms,
            rules.commander_sync_tick,
            rules.recorder_protocol,
            static_cast<int>(rules.ten_player_replay)
        );
    }
    CHECK(read > 0);
    return read;
}

} // namespace

int main() {
    no_profile_and_baseline_are_3_1c();
    every_rule_maps();
    rules_need_their_channel();
    speed_lock_follows_syncon();
    profile_limits_choose_the_records();
    reference_profiles();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("netgame wire rules: all tests passed");
    return 0;
}
