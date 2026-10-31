// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/match_runtime/match_trace.hpp"

#include "oa/sim/match_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

namespace oa::sim::match_runtime {
namespace trace = sim::trace;

void fill_trace_sides(const Match& match, std::span<trace::UnitSide> sides) {
    const auto& world = match.state();
    for (uint32_t slot = 0; slot < sides.size(); ++slot) {
        auto& side = sides[slot] = {};
        if (slot == 0 || slot >= world.unit_slot_count || world.units[slot].type_index == 0)
            continue;
        const auto index = static_cast<uint16_t>(slot);
        if (const auto* runtime = match.ground_runtime(index)) {
            side.has_movement = 1;
            side.movement_speed = static_cast<uint32_t>(runtime->movement.speed);
        }
        if (const auto* head = match.orders(index).primary) {
            side.has_order = 1;
            side.order_kind = head->kind;
        }
    }
}

TraceRecorder::~TraceRecorder() {
    close();
}

void TraceRecorder::close() noexcept {
    if (stream_.is_open())
        stream_.close();
    if (units_.is_open())
        units_.close();
}

bool rule_state_digest(const Match& match, trace::SectionDigest& digest) noexcept {
    if (match.rule_state().count == 0 && !match.mutator_digest_held())
        return false;
    digest.value = match.fold_rule_state(trace::digest_basis);
    digest.items = match.rule_state().count;
    return true;
}

trace::TickDigest match_tick_digest(const Match& match, std::span<trace::UnitSide> sides) {
    const auto& world = match.state();
    fill_trace_sides(match, sides);
    const trace::RandomState random{match.random_state(), match.lcg_state()};
    trace::SectionDigest rules{};
    return trace::tick_digest(
        world, sides.data(), random, rule_state_digest(match, rules) ? &rules : nullptr
    );
}

bool TraceRecorder::open(
    const std::filesystem::path& path, const std::filesystem::path& unit_path, uint32_t seed
) {
    close();
    stream_.open(path, std::ios::binary);
    if (stream_.is_open() && !unit_path.empty())
        units_.open(unit_path);
    uint8_t header[trace::header_size];
    trace::encode_header(header, seed);
    if (!stream_.is_open() || (!unit_path.empty() && !units_.is_open()) ||
        !stream_.write(reinterpret_cast<const char*>(header), sizeof header)) {
        close();
        return false;
    }
    return true;
}

void TraceRecorder::sample(const Match& match) {
    if (!stream_.is_open())
        return;
    const auto& world = match.state();
    sides_.resize(world.unit_slot_count);
    fill_trace_sides(match, sides_);
    const trace::RandomState random{match.random_state(), match.lcg_state()};
    uint8_t record[trace::record_size];
    trace::encode_tick_record(record, trace::sample_tick_record(world, sides_.data(), random.core));
    stream_.write(reinterpret_cast<const char*>(record), sizeof record);
    trace::SectionDigest rules{};
    const auto digest = trace::tick_digest(
        world, sides_.data(), random, rule_state_digest(match, rules) ? &rules : nullptr
    );
    for (std::size_t index = 0; index < trace::section_count; ++index) {
        trace::encode_section_record(
            record, digest.tick, static_cast<trace::Section>(index), digest.sections[index]
        );
        stream_.write(reinterpret_cast<const char*>(record), sizeof record);
    }
    stream_.flush();
    if (!units_.is_open())
        return;
    std::array<char, 1024> line{};
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        if (world.units[slot].type_index == 0)
            continue;
        const auto length =
            trace::format_unit(world, slot, sides_.data(), digest.tick, line.data(), line.size());
        units_.write(line.data(), static_cast<std::streamsize>(std::min(length, line.size() - 1)));
    }
    units_.flush();
}

bool Match::record_trace(
    const std::filesystem::path& path, const std::filesystem::path& unit_path
) {
    auto recorder = std::make_unique<TraceRecorder>();
    if (!recorder->open(path, unit_path, input_.random_seed))
        return false;
    trace_ = std::move(recorder);
    return true;
}
} // namespace oa::sim::match_runtime
