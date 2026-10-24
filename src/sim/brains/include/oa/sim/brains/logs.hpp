// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A match's brain log and the samples a brain asks to keep.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace oa::sim::match_runtime {
class Match;
}

namespace oa::sim::brains {

/// Most features one `brain.record` call keeps.
inline constexpr uint8_t record_feature_cap = 16;

/// Most elements one `brain.record` call may name.
inline constexpr uint32_t record_element_cap = 4194304;

/// One named tensor in a sample. `values` addresses the Lua tensor and lives
/// only for the call that hands the sample over.
struct FeatureView {
    /// 1 to 31 bytes of `a-z`, `0-9` or `_`, then a NUL.
    char name[32]{};
    /// 1 to 4.
    uint8_t rank{};
    /// The first `rank` entries are the shape. The rest are 0.
    uint32_t dims[4]{};
    /// Counts of 65536ths, `dims` multiplied out.
    std::span<const int32_t> values{};
};

/// The decision of one sample. An integer leaves `tensor` false.
struct DecisionView {
    /// True when the decision is a tensor.
    bool tensor{};
    /// The integer, when `tensor` is false.
    int64_t whole{};
    /// 1 to 4 when `tensor` is true.
    uint8_t rank{};
    /// The first `rank` entries are the shape.
    uint32_t dims[4]{};
    /// The tensor's counts. Empty when `tensor` is false.
    std::span<const int32_t> values{};
};

/// One `brain.record` call. `features` is in byte order of the names.
///
/// `player` is the engine player index. The spans address Lua tensors and
/// live only for the call.
struct RecordSample {
    uint8_t player{};
    uint32_t tick{};
    uint8_t feature_count{};
    FeatureView features[record_feature_cap]{};
    DecisionView decision{};
};

/// Where a match hands a brain log's lines and a brain's samples.
///
/// A null function collects nothing. `lines` receives one tick's text,
/// including the trailing newlines, or the file's first line `brain log 1`.
/// `sample` is called while the tensors it views are still alive.
struct LogHooks {
    void* context{};
    void (*lines)(void* context, std::string_view text){};
    void (*sample)(void* context, const RecordSample& sample){};
};

/// Stores the hooks a match uses for its brain log and its samples.
///
/// The first time `lines` is set, the match hands it `brain log 1` and a
/// newline. A later call keeps that line from being written again. Null
/// hooks collect nothing. A built-in seat's order tick is logged only while
/// `lines` is set.
///
/// @param match the match
/// @param hooks the functions; either may be null
void set_log_hooks(match_runtime::Match& match, LogHooks hooks) noexcept;

} // namespace oa::sim::brains
