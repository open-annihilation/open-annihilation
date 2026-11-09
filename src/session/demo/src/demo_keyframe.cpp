// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/session/demo.hpp"

#include "oa/base/zstd/zstd.hpp"
#include "oa/core/unit.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// A playback session's keyframe: the match's sections (oa/sim/match_runtime/keyframe.hpp), then the
// network match's bytes and values, then the playback's cursor and what it counted.
namespace oa::session::demo {
namespace {

namespace keyframe = sim::match_runtime::keyframe;

constexpr uint64_t fnv_offset = 0xcbf29ce484222325ull;
constexpr uint64_t fnv_prime = 0x100000001b3ull;
// The most a keyframe's image can be: the unit pool's tables with every plot of a large map.
constexpr std::size_t most_image_bytes = std::size_t{1} << 30;
// What the varying sections of a keyframe take, to make room for: 4 MiB covers the fixtures'.
constexpr std::size_t most_varying_bytes_hint = std::size_t{4} << 20;
// Game.sim_run_flags bit 0. The same bit netgame::match::run_flag_paused names.
constexpr uint16_t viewer_pause_bit = 0x0001;

bool fail(std::string* error, std::string message) {
    if (error != nullptr)
        *error = std::move(message);
    return false;
}

uint64_t fnv(uint64_t hash, std::span<const uint8_t> bytes) {
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= fnv_prime;
    }
    return hash;
}

/// Replaces `data` with its difference from `base`: the bytes XORed, eight at a time.
///
/// Doing it again undoes it.
///
/// @param[in,out] data The bytes.
/// @param base The bytes to difference against, at least as many.
/// @param size How many bytes.
void difference_from(uint8_t* data, const uint8_t* base, std::size_t size) {
    std::size_t at = 0;
    for (; at + sizeof(uint64_t) <= size; at += sizeof(uint64_t)) {
        uint64_t left = 0;
        uint64_t right = 0;
        std::memcpy(&left, data + at, sizeof left);
        std::memcpy(&right, base + at, sizeof right);
        left ^= right;
        std::memcpy(data + at, &left, sizeof left);
    }
    for (; at < size; ++at)
        data[at] ^= base[at];
}

static_assert(std::is_trivially_copyable_v<netgame::match::PacketLayer>);
static_assert(std::is_trivially_copyable_v<netgame::MovementRecord>);
static_assert(std::is_trivially_copyable_v<PlaybackStats>);
static_assert(std::is_trivially_copyable_v<FullRecordStats>);
static_assert(std::is_trivially_copyable_v<FullRecordSample>);
static_assert(std::is_trivially_copyable_v<EconomySample>);

/// Writes the session's own sections, after the match's.
bool write_session(DemoSession* session, keyframe::Writer* out, std::string* refusal) {
    if (const char* why = session->match->write_keyframe(out))
        return fail(refusal, why);
    auto* packets = session->connection.packets;
    if (packets == nullptr) {
        if (refusal != nullptr)
            *refusal = "the playback has no packet layer";
        return false;
    }
    out->begin("net.packets", true);
    out->bytes(packets, sizeof *packets);
    out->begin("net.binding", true);
    out->bytes(
        session->binding.movement.data(),
        session->binding.movement.size() * sizeof(netgame::MovementRecord)
    );
    out->begin("demo.slots", true);
    out->bytes(session->placed_type.data(), session->placed_type.size() * sizeof(uint16_t));
    out->bytes(session->placed_at.data(), session->placed_at.size() * sizeof(FixedVec3));
    out->begin("demo.state");
    out->value(session->connection.economy_periods);
    out->value(session->binding.created_remote);
    out->value(session->binding.refused_creates);
    out->value(session->binding.creates_past_table);
    out->value(static_cast<uint64_t>(session->playback.cursor));
    out->value(session->playback.tick);
    out->value(session->playback.stats);
    out->value(session->tick_errors);
    const auto& error = session->last_error;
    out->list(std::span<const char>(error.data(), error.size()));
    out->value(session->full_records);
    out->list(std::span<const FullRecordSample>(session->tracked));
    out->value(session->economy_samples);
    if (session->keyframe_extension.save_sections != nullptr)
        session->keyframe_extension.save_sections(session->keyframe_extension.context, out);
    return true;
}

/// Reads the playback's cursor and counters, from the section the writer began.
bool read_playback_state(DemoSession* session, keyframe::Reader* in) {
    uint64_t cursor = 0;
    if (!in->value(&session->connection.economy_periods) ||
        !in->value(&session->binding.created_remote) ||
        !in->value(&session->binding.refused_creates) ||
        !in->value(&session->binding.creates_past_table) || !in->value(&cursor) ||
        !in->value(&session->playback.tick) || !in->value(&session->playback.stats) ||
        !in->value(&session->tick_errors))
        return false;
    if (cursor > session->playback.frames.size())
        return in->fail("a keyframe's cursor is past the recording");
    session->playback.cursor = static_cast<std::size_t>(cursor);
    uint32_t count = 0;
    if (!in->list_count(&count, std::size_t{1} << 16))
        return false;
    session->last_error.resize(count);
    if (!in->bytes(session->last_error.data(), count))
        return false;
    if (!in->value(&session->full_records) || !in->list_count(&count, std::size_t{1} << 24))
        return false;
    session->tracked.resize(count);
    return in->bytes(session->tracked.data(), count * sizeof(FullRecordSample)) &&
           in->value(&session->economy_samples);
}

/// Reads the session's own sections, after the match's.
bool read_session(DemoSession* session, keyframe::Reader* in) {
    if (!session->match->read_keyframe(in))
        return false;
    auto* packets = session->connection.packets;
    if (packets == nullptr)
        return in->fail("the playback has no packet layer");
    if (in->begin("net.packets", true) && !in->bytes(packets, sizeof *packets))
        return false;
    if (in->begin("net.binding", true) &&
        !in->bytes(
            session->binding.movement.data(),
            session->binding.movement.size() * sizeof(netgame::MovementRecord)
        ))
        return false;
    if (in->begin("demo.slots", true)) {
        if (!in->bytes(
                session->placed_type.data(), session->placed_type.size() * sizeof(uint16_t)
            ) ||
            !in->bytes(session->placed_at.data(), session->placed_at.size() * sizeof(FixedVec3)))
            return false;
    }
    if (!in->ok())
        return false;
    if (in->begin("demo.state") && !read_playback_state(session, in))
        return false;
    if (!in->ok())
        return false;
    if (session->keyframe_extension.load_sections != nullptr)
        return session->keyframe_extension.load_sections(session->keyframe_extension.context, in);
    return true;
}

/// The compress block a request of `bytes` will hold.
///
/// When `held` already covers `bytes`, that block stays. Otherwise the block is
/// `bytes` plus spare room, an eighth and at least 1 MB, so the next slightly
/// larger image reuses it instead of leaving the old block behind.
std::size_t scratch_block_for(std::size_t held, std::size_t bytes) {
    if (bytes <= held)
        return held;
    const std::size_t slack = std::max(bytes / 8, std::size_t{1} << 20);
    if (bytes > std::numeric_limits<std::size_t>::max() - slack)
        return bytes;
    return bytes + slack;
}

/// Room of at least `bytes`, grown with the session and never zero-filled.
///
/// The block's size is scratch_block_for. The span is only the bytes asked for.
std::span<uint8_t> scratch_for(DemoSession* session, std::size_t bytes) {
    const std::size_t room = scratch_block_for(session->keyframe_scratch_bytes, bytes);
    if (room > session->keyframe_scratch_bytes) {
        session->keyframe_scratch = std::make_unique_for_overwrite<uint8_t[]>(room);
        session->keyframe_scratch_bytes = room;
    }
    return {session->keyframe_scratch.get(), bytes};
}

/// True when `committed`, the base, the writer's room and the compress block would pass `budget`.
///
/// The room is what the kept writer holds. The block is the compress buffer the take will
/// hold, from scratch_block_for, so a take is refused before that block grows.
bool over_budget(
    std::size_t committed,
    std::size_t base_bytes,
    std::size_t image_bytes,
    std::size_t scratch_bytes,
    std::size_t budget
) {
    std::size_t sum = 0;
    for (const std::size_t part : {committed, base_bytes, image_bytes, scratch_bytes}) {
        if (part > budget - sum)
            return true;
        sum += part;
    }
    return false;
}

} // namespace

std::size_t DemoKeyframe::memory_bytes() const noexcept {
    std::size_t bytes = sizeof(DemoKeyframe) + frame.capacity();
    if (net != nullptr)
        bytes += sizeof(netgame::match::NetMatch) + net->take.kill_records.capacity() +
                 net->recorder_units.capacity() * sizeof(netgame::match::RecorderUnitView);
    return bytes;
}

std::size_t demo_keyframe_base_bytes(const DemoSession& session) noexcept {
    return session.keyframe_base.size();
}

bool demo_keyframe_take(
    DemoSession* session,
    DemoKeyframe* key,
    std::string* refusal,
    const DemoKeyframeOptions& options
) {
    if (session == nullptr || session->match == nullptr || key == nullptr)
        return fail(refusal, "the playback is not bound to a match");
    keyframe::Writer& out = session->keyframe_image;
    out.restart(options.omission);
    out.reserve(session->keyframe_base.size(), most_varying_bytes_hint);
    if (!write_session(session, &out, refusal))
        return false;
    out.finish();
    const std::span<uint8_t> fixed = out.fixed();
    const std::span<uint8_t> varying = out.varying();
    const auto* world = &session->match->state();
    *key = DemoKeyframe{};
    key->tick = world->game.tick;
    key->digest = demo_world_digest(world);
    if (options.hash_image)
        key->image_digest = fnv(fnv(fnv_offset, fixed), varying);
    key->fixed_bytes = fixed.size();
    key->varying_bytes = varying.size();
    key->net = std::make_shared<const netgame::match::NetMatch>(session->net);
    if (options.image_fixed != nullptr)
        options.image_fixed->assign(fixed.begin(), fixed.end());
    if (options.image_varying != nullptr)
        options.image_varying->assign(varying.begin(), varying.end());
    if (options.image_only)
        return true;
    if (fixed.size() > std::numeric_limits<std::size_t>::max() - varying.size())
        return fail(refusal, "a keyframe could not be compressed");
    const std::size_t raw = fixed.size() + varying.size();
    const std::size_t bound = base::zstd::compress_bound(raw);
    if (bound == 0)
        return fail(refusal, "a keyframe could not be compressed");
    // The room the writer already holds, and the compress block this take will
    // hold. Both are known before the base is stored and before the block grows.
    const std::size_t image_room = out.room();
    const std::size_t block = scratch_block_for(session->keyframe_scratch_bytes, bound);
    // Refuse before the compress buffer grows and before the first take becomes the base.
    if (options.budget_bytes != 0) {
        const std::size_t base_after =
            session->keyframe_base.empty() ? fixed.size() : session->keyframe_base.size();
        if (over_budget(
                options.committed_bytes, base_after, image_room, block, options.budget_bytes
            )) {
            session->keyframe_scratch.reset();
            session->keyframe_scratch_bytes = 0;
            session->keyframe_image_bytes = 0;
            // A refused take does not keep the image. The next one builds it again.
            session->keyframe_image = keyframe::Writer{};
            return fail(refusal, keyframe_over_budget);
        }
    }
    session->keyframe_image_bytes = image_room;

    const std::span<uint8_t> scratch = scratch_for(session, bound);
    if (options.measure_plain) {
        const auto plain = base::zstd::compress_parts(fixed, varying, keyframe_zstd_level, scratch);
        key->plain_frame_bytes = plain.ok() ? plain.bytes : 0;
    }
    // The first keyframe is the session's base. The fixed sections of a keyframe are stored as the
    // bytes' difference from the base's, so the terrain and the tables that did not change
    // compress to nothing.
    if (session->keyframe_base.empty())
        session->keyframe_base.assign(fixed.begin(), fixed.end());
    // A keyframe left a section out of is another size than the base, and is stored whole.
    key->delta = session->keyframe_base.size() == fixed.size();
    if (key->delta)
        difference_from(fixed.data(), session->keyframe_base.data(), fixed.size());
    const auto sized = base::zstd::compress_parts(fixed, varying, keyframe_zstd_level, scratch);
    if (!sized.ok())
        return fail(refusal, "a keyframe could not be compressed");
    key->frame.assign(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(sized.bytes));
    return true;
}

bool demo_keyframe_restore(
    DemoSession* session, const DemoKeyframe& key, const DemoKeyframe* next, std::string* error
) {
    if (session == nullptr || session->match == nullptr || session->connection.packets == nullptr)
        return fail(error, "the playback is not bound to a match");
    const auto size = base::zstd::content_size(key.frame, most_image_bytes);
    if (!size.ok() || size.bytes != key.fixed_bytes + key.varying_bytes)
        return fail(error, "a keyframe's frame does not hold its image");
    // The reader copies each section out before this buffer is used again.
    const std::span<uint8_t> image = scratch_for(session, size.bytes);
    if (!base::zstd::decompress(key.frame, image).ok())
        return fail(error, "a keyframe's frame is damaged");
    // Display figures are not in the image. A restore that moves backward would otherwise keep
    // a later rate, because a record from before that rate is ignored. The offer of a shared
    // camera stays: records before the keyframe are not played again.
    const uint32_t playing = session->playback.tick;
    const bool backward = key.tick < playing;
    std::span<uint8_t> fixed(image.data(), key.fixed_bytes);
    if (key.delta) {
        if (session->keyframe_base.size() != fixed.size())
            return fail(error, "a keyframe does not fit its session's base");
        difference_from(fixed.data(), session->keyframe_base.data(), fixed.size());
    }
    const std::span<const uint8_t> varying(image.data() + key.fixed_bytes, key.varying_bytes);
    keyframe::Reader in(fixed, varying);
    if (!read_session(session, &in) || !in.finish())
        return fail(error, in.ok() ? "a keyframe could not be read" : in.error());
    if (key.net == nullptr)
        return fail(error, "a keyframe holds no network match");
    session->net = *key.net;
    if (backward) {
        session->shown_samples = {};
        session->shown_income = {};
        session->shown_camera = {};
    }
    session->lines.clear();
    session->keyframe_faults.clear();
    session->keyframe_expected.reset();
    if (next != nullptr && next->tick > key.tick)
        session->keyframe_expected = KeyframeExpectation{next->tick, next->digest};
    return true;
}

std::string demo_keyframe_difference(
    std::span<const uint8_t> first_fixed,
    std::span<const uint8_t> first_varying,
    std::span<const uint8_t> second_fixed,
    std::span<const uint8_t> second_varying
) {
    // Walk both varying regions section by section: a mark (the name, whether the section holds
    // bytes, which region, its size), then the bytes where the mark says.
    std::size_t at_first = 0;
    std::size_t at_second = 0;
    std::size_t fixed_first = 0;
    std::size_t fixed_second = 0;
    const auto read_mark = [](std::span<const uint8_t> region, std::size_t* at) {
        struct Mark {
            std::string name;
            bool present{};
            bool fixed{};
            uint32_t size{};
            bool ok{};
        } mark;
        if (*at >= region.size())
            return mark;
        const std::size_t length = region[*at];
        if (*at + 1 + length + 6 > region.size())
            return mark;
        mark.name.assign(reinterpret_cast<const char*>(region.data() + *at + 1), length);
        const auto* tail = region.data() + *at + 1 + length;
        mark.present = tail[0] != 0;
        mark.fixed = tail[1] != 0;
        std::memcpy(&mark.size, tail + 2, sizeof mark.size);
        mark.ok = true;
        *at += 1 + length + 6;
        return mark;
    };
    std::string differences;
    std::size_t listed = 0;
    const auto note = [&](const std::string& what) {
        if (listed++ < 8)
            differences += (differences.empty() ? "" : "; ") + what;
    };
    while (at_first < first_varying.size() || at_second < second_varying.size()) {
        const auto first = read_mark(first_varying, &at_first);
        const auto second = read_mark(second_varying, &at_second);
        if (!first.ok || !second.ok) {
            if (first.ok != second.ok)
                note("(the images' shapes)");
            break;
        }
        if (first.name != second.name || first.present != second.present ||
            first.fixed != second.fixed || first.size != second.size) {
            note(first.name + " (shape)");
            break;
        }
        if (!first.present)
            continue;
        const auto& data_first = first.fixed ? first_fixed : first_varying;
        const auto& data_second = first.fixed ? second_fixed : second_varying;
        auto& position_first = first.fixed ? fixed_first : at_first;
        auto& position_second = first.fixed ? fixed_second : at_second;
        if (position_first + first.size > data_first.size() ||
            position_second + second.size > data_second.size()) {
            note(first.name + " (short)");
            break;
        }
        if (std::memcmp(
                data_first.data() + position_first, data_second.data() + position_second, first.size
            ) != 0) {
            // The first byte that differs and how many do, for the one who looks for the field.
            std::size_t differing = 0;
            std::size_t first_at = first.size;
            for (std::size_t i = 0; i < first.size; ++i)
                if (data_first[position_first + i] != data_second[position_second + i]) {
                    ++differing;
                    first_at = std::min(first_at, i);
                }
            note(
                first.name + " at byte " + std::to_string(first_at) + " (" +
                std::to_string(differing) + " bytes differ)"
            );
        }
        position_first += first.size;
        position_second += second.size;
    }
    return differences;
}

std::vector<std::pair<std::string, std::size_t>>
demo_keyframe_sections(std::span<const uint8_t> varying) {
    std::vector<std::pair<std::string, std::size_t>> sections;
    std::size_t at = 0;
    while (at < varying.size()) {
        const std::size_t length = varying[at];
        if (at + 1 + length + 6 > varying.size())
            break;
        const std::string name(reinterpret_cast<const char*>(varying.data() + at + 1), length);
        const auto* tail = varying.data() + at + 1 + length;
        const bool present = tail[0] != 0;
        const bool in_fixed = tail[1] != 0;
        uint32_t size = 0;
        std::memcpy(&size, tail + 2, sizeof size);
        at += 1 + length + 6;
        sections.emplace_back(name, present ? size : 0);
        if (present && !in_fixed)
            at += size;
    }
    return sections;
}

void take_viewer_picture(const World& world, ViewerPicture* picture) {
    if (picture == nullptr)
        return;
    const Game& game = world.game;
    picture->paused = (game.sim_run_flags & viewer_pause_bit) != 0;
    picture->viewpoint_player = game.viewpoint_player;
    picture->local_player_index = game.local_player_index;
    picture->follow_unit = game.follow_unit;
    picture->follow_target = game.follow_target;
    picture->follow_point = game.follow_point;
    picture->follow_point_ticks = game.follow_point_ticks;
    std::memcpy(picture->camera_slot_x, game.camera_slot_x, sizeof picture->camera_slot_x);
    std::memcpy(picture->camera_slot_y, game.camera_slot_y, sizeof picture->camera_slot_y);
    std::memcpy(
        picture->camera_slot_valid, game.camera_slot_valid, sizeof picture->camera_slot_valid
    );
    picture->debug_overlay = game.debug_overlay;
    picture->selected.assign(world.unit_slot_count, 0);
    if (world.units == nullptr)
        return;
    for (uint32_t slot = 0; slot < world.unit_slot_count; ++slot) {
        const Unit& unit = world.units[slot];
        if (unit.type_index != 0 && (unit.flags & OA_UNIT_FLAG_SELECTED) != 0)
            picture->selected[slot] = 1;
    }
}

void give_viewer_picture(World* world, const ViewerPicture& picture) {
    if (world == nullptr)
        return;
    Game& game = world->game;
    if (picture.paused)
        game.sim_run_flags = static_cast<uint16_t>(game.sim_run_flags | viewer_pause_bit);
    else
        game.sim_run_flags =
            static_cast<uint16_t>(game.sim_run_flags & static_cast<uint16_t>(~viewer_pause_bit));
    game.viewpoint_player = picture.viewpoint_player;
    game.local_player_index = picture.local_player_index;
    game.follow_unit = picture.follow_unit;
    game.follow_target = picture.follow_target;
    game.follow_point = picture.follow_point;
    game.follow_point_ticks = picture.follow_point_ticks;
    std::memcpy(game.camera_slot_x, picture.camera_slot_x, sizeof game.camera_slot_x);
    std::memcpy(game.camera_slot_y, picture.camera_slot_y, sizeof game.camera_slot_y);
    std::memcpy(game.camera_slot_valid, picture.camera_slot_valid, sizeof game.camera_slot_valid);
    game.debug_overlay = picture.debug_overlay;
    if (world->units != nullptr) {
        for (uint32_t slot = 0; slot < world->unit_slot_count; ++slot) {
            Unit& unit = world->units[slot];
            const bool wanted = slot < picture.selected.size() && picture.selected[slot] != 0;
            const bool selected = wanted && unit.type_index != 0;
            if (selected)
                unit.flags |= OA_UNIT_FLAG_SELECTED;
            else
                unit.flags &= ~OA_UNIT_FLAG_SELECTED;
        }
    }
    std::memset(game.chat_lines, 0, sizeof game.chat_lines);
    game.chat_head = 0;
    game.chat_tail = 0;
}

} // namespace oa::session::demo
