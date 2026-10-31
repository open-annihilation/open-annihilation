// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Trace stream of an offline match: at the head of every tick, before the
// unit sweep, the tick record and the section digests of
// oa/sim/trace.hpp, and optionally a text line per occupied unit slot for a
// field-level diff.
#pragma once

#include "oa/sim/trace.hpp"

#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace oa::sim::match_runtime {
class Match;

/// Fills the movement object's speed and presence and the head of the
/// primary order queue of every unit slot, from the match's side tables.
///
/// @param match Match whose units are sampled.
/// @param[out] sides One entry per unit slot.
void fill_trace_sides(const Match& match, std::span<sim::trace::UnitSide> sides);

/// Digests the state the match's rules keep outside the canonical records,
/// for the trace's total (sim::trace::tick_digest).
///
/// @param match the match
/// @param[out] digest receives Match::fold_rule_state from digest_basis and
///        the number of tables
/// @return false, leaving `digest` as it is, when the match keeps no rule
///         state and no mutator digest
bool rule_state_digest(const Match& match, sim::trace::SectionDigest& digest) noexcept;

/// Computes the section digests of the match's current tick
/// (sim::trace::tick_digest), its rule state included.
///
/// @param match the match
/// @param[out] sides scratch table of one entry per unit slot
/// @return the digests
sim::trace::TickDigest match_tick_digest(const Match& match, std::span<sim::trace::UnitSide> sides);

class TraceRecorder {
  public:

    TraceRecorder() = default;
    TraceRecorder(const TraceRecorder&) = delete;
    TraceRecorder& operator=(const TraceRecorder&) = delete;
    /// Closes both streams.
    ~TraceRecorder();

    /// Starts the stream at `path` with its header.
    ///
    /// @param path File the trace stream is written to.
    /// @param unit_path File of the per-unit text dump; empty writes none.
    /// @param seed Match random seed written in the header.
    /// @return False when either file cannot be created.
    bool
    open(const std::filesystem::path& path, const std::filesystem::path& unit_path, uint32_t seed);
    /// Appends one tick: the tick record, then the section records.
    ///
    /// @param match Match sampled at the head of its tick.
    void sample(const Match& match);

  private:

    /// Closes both stream files, if open.
    void close() noexcept;

    std::ofstream stream_;
    std::ofstream units_;
    std::vector<sim::trace::UnitSide> sides_;
};
} // namespace oa::sim::match_runtime
