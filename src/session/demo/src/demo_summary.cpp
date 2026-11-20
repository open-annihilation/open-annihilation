// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The one `demo summary` line. `--play-demo` and a replay-viewer join both
// print it, so a recording watched both ways can be compared.
#include "oa/session/demo.hpp"

#include <cstdio>
#include <string>

namespace oa::session::demo {

namespace {

// The words and the order `--play-demo` has always printed. A change here
// changes that line.
constexpr char kSummaryFormat[] =
    "demo summary: records applied %u refused %u, created %u refused %u past table %u, record "
    "errors %u, "
    "tick errors %u, dropped frames %u, oversized %u, bad packets %u, untimed unit states "
    "%u%s%s";

} // namespace

std::string demo_summary_line(const DemoSummary& summary) {
    const char* const error_mark = summary.last_error.empty() ? "" : "; last error: ";
    const char* const error_text = summary.last_error.c_str();
    const int needed = std::snprintf(
        nullptr,
        0,
        kSummaryFormat,
        summary.records_applied,
        summary.records_refused,
        summary.units_created,
        summary.creates_refused,
        summary.creates_past_table,
        summary.record_errors,
        summary.tick_errors,
        summary.dropped_frames,
        summary.frames_oversized,
        summary.bad_packets,
        summary.untimed_unit_states,
        error_mark,
        error_text
    );
    if (needed < 0)
        return {};
    std::string line(static_cast<std::size_t>(needed), '\0');
    std::snprintf(
        line.data(),
        line.size() + 1,
        kSummaryFormat,
        summary.records_applied,
        summary.records_refused,
        summary.units_created,
        summary.creates_refused,
        summary.creates_past_table,
        summary.record_errors,
        summary.tick_errors,
        summary.dropped_frames,
        summary.frames_oversized,
        summary.bad_packets,
        summary.untimed_unit_states,
        error_mark,
        error_text
    );
    return line;
}

DemoSummary demo_summary_of_match(
    const netgame::match::NetMatch& match,
    const netgame::match::MatchBinding& binding,
    const netgame::match::NetConnection& connection
) {
    DemoSummary summary;
    summary.records_applied = match.records_applied;
    summary.records_refused = match.records_refused;
    summary.units_created = binding.created_remote;
    summary.creates_refused = binding.refused_creates;
    summary.creates_past_table = binding.creates_past_table;
    summary.record_errors = match.record_errors;
    if (connection.packets != nullptr)
        summary.dropped_frames = connection.packets->dropped_frames;
    return summary;
}

DemoSummary demo_summary_of(const DemoSession& session) {
    DemoSummary summary = demo_summary_of_match(session.net, session.binding, session.connection);
    summary.tick_errors = session.tick_errors;
    summary.frames_oversized = session.playback.stats.frames_oversized;
    summary.bad_packets = session.playback.stats.bad_packets;
    summary.untimed_unit_states = session.playback.stats.untimed_unit_states;
    summary.last_error = session.last_error;
    return summary;
}

} // namespace oa::session::demo
