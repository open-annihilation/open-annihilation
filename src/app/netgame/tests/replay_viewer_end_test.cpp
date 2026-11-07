// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The decision a replay viewer makes when the watched game ends: the host's
// refusal, a protocol leave, and a host that goes silent, before and after
// a record of the replay was applied.
#include "oa/app/netgame/replay_viewer_end.hpp"

#include <cstdio>

namespace view = oa::app::netgame;

namespace {
int failures = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "replay viewer end: %s\n", message);
        ++failures;
    }
}

view::ReplayViewerWatch received_watch() {
    view::ReplayViewerWatch watch;
    watch.received = true;
    return watch;
}

void host_end_reject_prints_the_summary() {
    auto watch = received_watch();
    watch.local_rejected = true;
    require(
        view::replay_viewer_end(watch) == view::ReplayViewerEnd::summary,
        "the host's end reject did not print the summary"
    );
}

void protocol_leave_after_the_replay_prints_the_summary() {
    auto watch = received_watch();
    watch.host_in_table = false;
    require(
        view::replay_viewer_end(watch) == view::ReplayViewerEnd::summary,
        "a protocol leave after the replay did not print the summary"
    );
}

void silent_host_after_the_replay_drops() {
    auto watch = received_watch();
    watch.silence_measured = true;
    watch.milliseconds_since_packet = view::replay_viewer_silence_ms;
    require(
        view::replay_viewer_end(watch) == view::ReplayViewerEnd::dropped,
        "a silent host after the replay printed a summary"
    );
}

void silent_host_before_any_record_drops() {
    view::ReplayViewerWatch watch;
    watch.silence_measured = true;
    watch.milliseconds_since_packet = view::replay_viewer_silence_ms;
    require(
        view::replay_viewer_end(watch) == view::ReplayViewerEnd::dropped,
        "a silent host before any record printed a summary"
    );
}

void reject_before_any_record_drops() {
    view::ReplayViewerWatch watch;
    watch.local_rejected = true;
    require(
        view::replay_viewer_end(watch) == view::ReplayViewerEnd::dropped,
        "a reject before any record printed a summary"
    );
}

void room_wait_just_under_the_limit_keeps_waiting() {
    view::ReplayViewerWatch watch;
    watch.room_wait_measured = true;
    watch.milliseconds_in_battle_room = view::replay_viewer_room_limit_ms - 1u;
    require(
        view::replay_viewer_end(watch) == view::ReplayViewerEnd::waiting,
        "just under five minutes in the battle room ended the watch"
    );
}

void room_wait_at_the_limit_drops() {
    view::ReplayViewerWatch watch;
    watch.room_wait_measured = true;
    watch.milliseconds_in_battle_room = view::replay_viewer_room_limit_ms;
    require(
        view::replay_viewer_end(watch) == view::ReplayViewerEnd::dropped,
        "five minutes in the battle room kept waiting"
    );
    watch.received = true;
    require(
        view::replay_viewer_end(watch) == view::ReplayViewerEnd::dropped,
        "a replay that never started printed a summary"
    );
}

view::ReplayViewerReadyFacts ready_to_send() {
    view::ReplayViewerReadyFacts facts;
    facts.headless = true;
    facts.in_battle_room = true;
    facts.seated = true;
    facts.units_reported = true;
    facts.host_map_known = true;
    return facts;
}

void join_still_in_progress_does_not_set_ready() {
    auto facts = ready_to_send();
    facts.in_battle_room = false;
    require(
        view::replay_viewer_ready(facts) == view::ReplayViewerReady::hold,
        "a join still in progress set READY"
    );
}

void room_before_units_are_reported_does_not_set_ready() {
    auto facts = ready_to_send();
    facts.units_reported = false;
    require(
        view::replay_viewer_ready(facts) == view::ReplayViewerReady::hold,
        "READY was set before this machine reported its units"
    );
}

void reported_units_set_ready() {
    require(
        view::replay_viewer_ready(ready_to_send()) == view::ReplayViewerReady::set,
        "READY was not set after this machine reported its units"
    );
}

void unknown_host_map_does_not_set_ready() {
    auto facts = ready_to_send();
    facts.host_map_known = false;
    require(
        view::replay_viewer_ready(facts) == view::ReplayViewerReady::hold,
        "READY was set before the host's map was known"
    );
}

void cleared_ready_bit_sets_again_after_a_second() {
    auto still_set = ready_to_send();
    still_set.ready_set = true;
    still_set.sent = true;
    still_set.milliseconds_since_send = view::replay_viewer_ready_gap_ms;
    require(
        view::replay_viewer_ready(still_set) == view::ReplayViewerReady::hold,
        "READY was sent again while the bit was still set"
    );
    auto cleared = ready_to_send();
    cleared.sent = true;
    cleared.milliseconds_since_send = view::replay_viewer_ready_gap_ms - 1u;
    require(
        view::replay_viewer_ready(cleared) == view::ReplayViewerReady::hold,
        "READY was sent twice within a second"
    );
    cleared.milliseconds_since_send = view::replay_viewer_ready_gap_ms;
    require(
        view::replay_viewer_ready(cleared) == view::ReplayViewerReady::set,
        "a cleared READY bit was not set again"
    );
}

void windowed_run_never_sets_ready() {
    auto facts = ready_to_send();
    facts.headless = false;
    require(
        view::replay_viewer_ready(facts) == view::ReplayViewerReady::hold,
        "a windowed run set READY"
    );
}

} // namespace

int main() {
    host_end_reject_prints_the_summary();
    protocol_leave_after_the_replay_prints_the_summary();
    silent_host_after_the_replay_drops();
    silent_host_before_any_record_drops();
    reject_before_any_record_drops();
    room_wait_just_under_the_limit_keeps_waiting();
    room_wait_at_the_limit_drops();
    join_still_in_progress_does_not_set_ready();
    room_before_units_are_reported_does_not_set_ready();
    reported_units_set_ready();
    unknown_host_map_does_not_set_ready();
    cleared_ready_bit_sets_again_after_a_second();
    windowed_run_never_sets_ready();
    if (failures != 0) {
        std::fprintf(stderr, "replay viewer end: %d failed\n", failures);
        return 1;
    }
    std::printf("replay viewer end: passed\n");
    return 0;
}
