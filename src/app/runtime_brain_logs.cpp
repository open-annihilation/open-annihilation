// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The brain log and the training record. The simulation hands over lines and
// samples; this file is the only place they are written.

#include "oa/app/runtime.hpp"

#include "oa/app/brain_match.hpp"
#include "oa/core/game_state.h"
#include "oa/sim/brains/logs.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

constexpr uint64_t record_byte_cap = uint64_t{1} << 30;
constexpr char record_full_line[] = "{\"kind\":\"full\"}\n";

struct CopiedFeature {
    char name[32]{};
    uint8_t rank{};
    uint32_t dims[4]{};
    std::vector<int32_t> values;
};

struct CopiedSample {
    uint8_t player{};
    uint32_t tick{};
    uint8_t feature_count{};
    CopiedFeature features[oa::sim::brains::record_feature_cap]{};
    bool decision_tensor{};
    int64_t decision{};
    uint8_t decision_rank{};
    uint32_t decision_dims[4]{};
    std::vector<int32_t> decision_values;
};

void copy_name(char* out, const char* text) noexcept {
    std::size_t index = 0;
    if (text != nullptr) {
        while (index < 31 && text[index] != '\0') {
            out[index] = text[index];
            ++index;
        }
    }
    out[index] = '\0';
}

CopiedSample copy_sample(const oa::sim::brains::RecordSample& sample) {
    CopiedSample copied;
    copied.player = sample.player;
    copied.tick = sample.tick;
    const uint8_t count = sample.feature_count < oa::sim::brains::record_feature_cap
                              ? sample.feature_count
                              : oa::sim::brains::record_feature_cap;
    copied.feature_count = count;
    for (uint8_t index = 0; index < count; ++index) {
        const oa::sim::brains::FeatureView& feature = sample.features[index];
        CopiedFeature& held = copied.features[index];
        copy_name(held.name, feature.name);
        held.rank = feature.rank;
        for (uint8_t axis = 0; axis < 4; ++axis)
            held.dims[axis] = feature.dims[axis];
        if (feature.values.data() != nullptr)
            held.values.assign(feature.values.begin(), feature.values.end());
    }
    copied.decision_tensor = sample.decision.tensor;
    copied.decision = sample.decision.whole;
    copied.decision_rank = sample.decision.rank;
    for (uint8_t axis = 0; axis < 4; ++axis)
        copied.decision_dims[axis] = sample.decision.dims[axis];
    if (sample.decision.values.data() != nullptr)
        copied.decision_values.assign(sample.decision.values.begin(), sample.decision.values.end());
    return copied;
}

void write_all(std::ofstream& out, std::string_view text) {
    std::size_t done = 0;
    while (done < text.size()) {
        const std::size_t room = text.size() - done;
        const std::size_t step = room > (std::size_t{1} << 20) ? (std::size_t{1} << 20) : room;
        out.write(text.data() + done, static_cast<std::streamsize>(step));
        done += step;
    }
}

void append_json_string(std::string& out, std::string_view text) {
    static constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (unsigned char byte : text) {
        if (byte == '\\' || byte == '"') {
            out.push_back('\\');
            out.push_back(static_cast<char>(byte));
        } else if (byte < 0x20) {
            out.append("\\u00");
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0x0f]);
        } else {
            out.push_back(static_cast<char>(byte));
        }
    }
    out.push_back('"');
}

void append_number(std::string& out, int64_t value) {
    out.append(std::to_string(value));
}

void append_u32(std::string& out, uint32_t value) {
    out.append(std::to_string(value));
}

void append_tensor(
    std::string& out, const uint32_t* dims, uint8_t rank, const int32_t* values, std::size_t count
) {
    out.append("{\"shape\":[");
    for (uint8_t axis = 0; axis < rank; ++axis) {
        if (axis != 0)
            out.push_back(',');
        append_u32(out, dims[axis]);
    }
    out.append("],\"values\":[");
    if (values != nullptr) {
        for (std::size_t index = 0; index < count; ++index) {
            if (index != 0)
                out.push_back(',');
            append_number(out, values[index]);
        }
    }
    out.append("]}");
}

struct HeaderSeat {
    int seat{};
    bool builtin{true};
    std::string brain;
    std::string version;
    std::string level{"hard"};
};

std::string header_line(std::string_view map, uint32_t seed, const std::vector<HeaderSeat>& seats) {
    std::string line = "{\"kind\":\"header\",\"format\":1,\"map\":";
    append_json_string(line, map);
    line.append(",\"seed\":");
    append_u32(line, seed);
    line.append(",\"seats\":[");
    for (std::size_t index = 0; index < seats.size(); ++index) {
        if (index != 0)
            line.push_back(',');
        const HeaderSeat& seat = seats[index];
        line.append("{\"seat\":");
        append_number(line, seat.seat);
        line.append(",\"brain\":");
        append_json_string(
            line, seat.builtin ? std::string_view("builtin") : std::string_view(seat.brain)
        );
        if (!seat.builtin && !seat.version.empty()) {
            line.append(",\"version\":");
            append_json_string(line, seat.version);
        }
        line.append(",\"level\":");
        append_json_string(line, seat.level);
        line.push_back('}');
    }
    line.append("]}\n");
    return line;
}

void append_features_copied(std::string& line, const CopiedSample& sample) {
    line.append(",\"features\":{");
    for (uint8_t index = 0; index < sample.feature_count; ++index) {
        if (index != 0)
            line.push_back(',');
        const CopiedFeature& feature = sample.features[index];
        append_json_string(line, feature.name);
        line.push_back(':');
        append_tensor(
            line, feature.dims, feature.rank, feature.values.data(), feature.values.size()
        );
    }
    line.push_back('}');
}

void append_decision_copied(std::string& line, const CopiedSample& sample) {
    line.append(",\"decision\":");
    if (!sample.decision_tensor) {
        append_number(line, sample.decision);
        return;
    }
    append_tensor(
        line,
        sample.decision_dims,
        sample.decision_rank,
        sample.decision_values.data(),
        sample.decision_values.size()
    );
}

std::string sample_line(const CopiedSample& sample) {
    std::string line = "{\"kind\":\"sample\",\"tick\":";
    append_u32(line, sample.tick);
    line.append(",\"seat\":");
    append_number(line, static_cast<int64_t>(sample.player) + 1);
    append_features_copied(line, sample);
    append_decision_copied(line, sample);
    line.append("}\n");
    return line;
}

std::string sample_line(const oa::sim::brains::RecordSample& sample) {
    return sample_line(copy_sample(sample));
}

std::string result_line(int seat, bool winner, uint32_t tick, uint32_t units) {
    std::string line = "{\"kind\":\"result\",\"seat\":";
    append_number(line, seat);
    line.append(",\"winner\":");
    line.append(winner ? "true" : "false");
    line.append(",\"tick\":");
    append_u32(line, tick);
    line.append(",\"units\":");
    append_u32(line, units);
    line.append("}\n");
    return line;
}

} // namespace

struct Runtime::BrainTraceFiles {
    std::ofstream log;
    std::ofstream record;
    bool log_ready{};
    bool record_ready{};
    bool record_stopped{};
    bool record_noted{};
    uint64_t record_bytes{};
    std::string pending_lines;
    std::vector<CopiedSample> pending_samples;

    void begin() {
        log.close();
        record.close();
        log.clear();
        record.clear();
        log_ready = false;
        record_ready = false;
        record_stopped = false;
        record_noted = false;
        record_bytes = 0;
        pending_lines.clear();
        pending_samples.clear();
    }

    void note_record_full() {
        if (record_stopped)
            return;
        record_stopped = true;
        const std::string_view full(record_full_line);
        if (full.size() <= record_byte_cap && record_bytes <= record_byte_cap - full.size()) {
            write_all(record, full);
            record_bytes += full.size();
            record.flush();
        }
        if (!record_noted) {
            record_noted = true;
            std::cerr << "brain record is full\n";
        }
    }

    void write_record(std::string line) {
        if (!record_ready || record_stopped || line.empty())
            return;
        if (line.back() != '\n')
            line.push_back('\n');
        const uint64_t full_bytes = sizeof record_full_line - 1;
        if (line.size() > record_byte_cap ||
            record_bytes > record_byte_cap - static_cast<uint64_t>(line.size()) ||
            record_bytes + static_cast<uint64_t>(line.size()) + full_bytes > record_byte_cap) {
            note_record_full();
            return;
        }
        write_all(record, line);
        record_bytes += line.size();
        record.flush();
    }

    void take_lines(std::string_view text) {
        if (!log_ready) {
            pending_lines.append(text.data(), text.size());
            return;
        }
        write_all(log, text);
        log.flush();
    }
};

void Runtime::destroy_brain_trace_files(BrainTraceFiles* state) noexcept {
    delete state;
}

void Runtime::brain_log_lines(void* context, std::string_view text) noexcept {
    auto* runtime = static_cast<Runtime*>(context);
    if (runtime == nullptr || runtime->brain_trace_files_ == nullptr)
        return;
    try {
        runtime->brain_trace_files_->take_lines(text);
    } catch (const std::exception&) {
    }
}

void Runtime::brain_log_sample(
    void* context, const oa::sim::brains::RecordSample& sample
) noexcept {
    auto* runtime = static_cast<Runtime*>(context);
    if (runtime == nullptr || runtime->brain_trace_files_ == nullptr)
        return;
    try {
        BrainTraceFiles& files = *runtime->brain_trace_files_;
        if (files.record_ready)
            files.write_record(sample_line(sample));
        else
            files.pending_samples.push_back(copy_sample(sample));
    } catch (const std::exception&) {
    }
}

void Runtime::install_brain_log_hooks() {
    if (match_ == nullptr)
        return;
    if (options_.brain_trace.empty() && options_.brain_record.empty())
        return;
    if (brain_trace_files_ == nullptr)
        brain_trace_files_.reset(new BrainTraceFiles());
    brain_trace_files_->begin();
    oa::sim::brains::LogHooks hooks;
    hooks.context = this;
    if (!options_.brain_trace.empty())
        hooks.lines = &Runtime::brain_log_lines;
    if (!options_.brain_record.empty())
        hooks.sample = &Runtime::brain_log_sample;
    oa::sim::brains::set_log_hooks(*match_, hooks);
}

void Runtime::open_brain_log_files() {
    if (brain_trace_files_ == nullptr || match_ == nullptr)
        return;
    BrainTraceFiles& files = *brain_trace_files_;
    if (!options_.brain_trace.empty()) {
        files.log.open(fs::path(options_.brain_trace), std::ios::binary | std::ios::trunc);
        if (!files.log)
            throw std::runtime_error("cannot create the brain log " + options_.brain_trace);
        files.log_ready = true;
        if (!files.pending_lines.empty()) {
            write_all(files.log, files.pending_lines);
            files.log.flush();
            files.pending_lines.clear();
        }
    }
    if (options_.brain_record.empty())
        return;
    files.record.open(fs::path(options_.brain_record), std::ios::binary | std::ios::trunc);
    if (!files.record)
        throw std::runtime_error("cannot create the brain record " + options_.brain_record);
    files.record_ready = true;

    const auto level_word = [this]() {
        const int32_t difficulty = match_->state().game.difficulty;
        if (difficulty == OA_DIFFICULTY_EASY)
            return std::string("easy");
        if (difficulty == OA_DIFFICULTY_MEDIUM)
            return std::string("medium");
        return std::string("hard");
    };

    struct Named {
        bool builtin{true};
        std::string id;
        std::string version;
        std::string level;
    };

    const auto describe_choice = [this, &level_word](const BrainSeatChoice& choice) {
        Named named;
        named.level = level_word();
        fs::path brains_folder = user_folder() / std::string(brains_folder_name);
        std::string id = choice.id;
        if (!choice.folder.empty()) {
            brains_folder = choice.folder.parent_path();
            const std::string file_name = choice.folder.filename().string();
            if (!file_name.empty())
                id = file_name;
        }
        std::string error;
        const std::optional<InstalledBrain> installed =
            read_installed_brain(brains_folder, id, error);
        if (!installed)
            return named;
        ResolvedSeat resolved;
        if (!resolve_seat(*installed, choice, resolved, error))
            return named;
        named.builtin = false;
        named.id = installed->manifest.id;
        named.version = resolved.version;
        named.level = resolved.level.empty() ? named.level : resolved.level;
        return named;
    };

    std::optional<std::array<BrainMatchSeat, 2>> parsed;
    if (!options_.brain_match.empty()) {
        std::string error;
        parsed = parse_brain_match(options_.brain_match, options_.brain_levels, error);
    }
    std::vector<HeaderSeat> seats;
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        const oa::Player& record = match_->state().game.players[player];
        if (record.in_use == 0 || record.status != OA_PLAYER_STATUS_COMPUTER)
            continue;
        HeaderSeat seat;
        seat.seat = static_cast<int>(player) + 1;
        seat.level = level_word();
        if (parsed && player < 2 && brain_match_builtin((*parsed)[player].spec)) {
            const std::string& asked = (*parsed)[player].level;
            seat.level = asked.empty() ? std::string("hard") : asked;
            seats.push_back(std::move(seat));
            continue;
        }
        if (brain_seat_choices_[player].has_value()) {
            const Named named = describe_choice(*brain_seat_choices_[player]);
            if (!named.builtin) {
                seat.builtin = false;
                seat.brain = named.id;
                seat.version = named.version;
                seat.level = named.level;
            }
            seats.push_back(std::move(seat));
            continue;
        }
        for (const BrainSeat& seated : seated_brains_) {
            if (seated.player != player || seated.id.empty())
                continue;
            seat.builtin = false;
            seat.brain = seated.id;
            seat.version = seated.version;
            if (!seated.level.empty())
                seat.level = seated.level;
            break;
        }
        seats.push_back(std::move(seat));
    }
    const std::string& map_name =
        options_.brain_map.empty() ? selected_map_name_runtime_ : options_.brain_map;
    files.write_record(header_line(map_name, brain_match_seed_, seats));
    for (const CopiedSample& sample : files.pending_samples)
        files.write_record(sample_line(sample));
    files.pending_samples.clear();
}

void Runtime::write_brain_record_results(uint8_t winner, const uint32_t* live) {
    if (brain_trace_files_ == nullptr || match_ == nullptr || live == nullptr)
        return;
    if (!brain_trace_files_->record_ready)
        return;
    const uint32_t tick = match_->state().game.tick;
    for (uint8_t player = 0; player < 2; ++player) {
        const bool won = winner == player;
        brain_trace_files_->write_record(
            result_line(static_cast<int>(player) + 1, won, tick, live[player])
        );
    }
}

} // namespace oa::app
