// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/session/demo/clock.hpp"

#include "oa/session/demo.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::session::demo {
namespace {

struct Factor {
    uint32_t num = 1;
    uint32_t den = 1;
    uint32_t milli = 1000;
};

Factor factor_of(PlaybackSpeed speed) noexcept {
    switch (speed) {
    case PlaybackSpeed::half:
        return {1, 2, 500};
    case PlaybackSpeed::normal:
        return {1, 1, 1000};
    case PlaybackSpeed::twice:
        return {2, 1, 2000};
    case PlaybackSpeed::quadruple:
        return {4, 1, 4000};
    case PlaybackSpeed::octuple:
        return {8, 1, 8000};
    case PlaybackSpeed::sixteen:
        return {16, 1, 16000};
    }
    return {1, 1, 1000};
}

constexpr PlaybackSpeed kLadder[] = {
    PlaybackSpeed::half,
    PlaybackSpeed::normal,
    PlaybackSpeed::twice,
    PlaybackSpeed::quadruple,
    PlaybackSpeed::octuple,
    PlaybackSpeed::sixteen,
};

int ladder_index(PlaybackSpeed speed) noexcept {
    for (int index = 0; index < static_cast<int>(std::size(kLadder)); ++index)
        if (kLadder[index] == speed)
            return index;
    return 1;
}

uint32_t head_distance(uint32_t play_tick, uint32_t head) noexcept {
    return play_tick > head ? play_tick - head : head - play_tick;
}

} // namespace

uint32_t playback_speed_milli(PlaybackSpeed speed) noexcept {
    return factor_of(speed).milli;
}

PlaybackSpeed step_playback_speed(PlaybackSpeed speed, int delta) noexcept {
    const int last = static_cast<int>(std::size(kLadder)) - 1;
    int index = ladder_index(speed) + delta;
    if (index < 0)
        index = 0;
    if (index > last)
        index = last;
    return kLadder[index];
}

bool playback_speed_thins_sound(PlaybackSpeed speed) noexcept {
    return speed == PlaybackSpeed::octuple || speed == PlaybackSpeed::sixteen;
}

std::string playback_speed_line(
    uint32_t asked_milli, uint32_t reached_milli, bool reached_lower, bool skipping
) {
    if (skipping)
        return "Skip";
    const auto label = [](uint32_t milli) {
        if (milli == 500)
            return std::string("½×");
        return std::to_string((milli + 500) / 1000) + "×";
    };
    const std::string asked = label(asked_milli);
    if (!reached_lower)
        return asked;
    return asked + " · " + label(reached_milli) + " now";
}

uint32_t catch_up_percent(uint32_t origin, uint32_t play_tick, uint32_t target) noexcept {
    if (target <= origin)
        return 100;
    const uint64_t span = static_cast<uint64_t>(target) - origin;
    uint64_t done = 0;
    if (play_tick > origin)
        done = play_tick >= target ? span : static_cast<uint64_t>(play_tick) - origin;
    return static_cast<uint32_t>(done * 100 / span);
}

bool keyframe_due(uint32_t play_tick) noexcept {
    return play_tick == 0 || play_tick % keyframe_near_interval_ticks == 0;
}

bool keyframe_sparse(uint32_t play_tick) noexcept {
    return play_tick % keyframe_far_interval_ticks == 0;
}

bool keyframe_kept(uint32_t play_tick, uint32_t head) noexcept {
    if (play_tick == 0)
        return true;
    if (head_distance(play_tick, head) <= keyframe_near_window_ticks)
        return true;
    return keyframe_sparse(play_tick);
}

bool SoundGate::allow(std::string_view name, uint32_t now_ms) {
    for (Heard& heard : heard_) {
        if (heard.name != name)
            continue;
        if (static_cast<uint32_t>(now_ms - heard.at) < 1000)
            return false;
        heard.at = now_ms;
        return true;
    }
    heard_.push_back(Heard{std::string(name), now_ms});
    return true;
}

void SoundGate::clear() noexcept {
    heard_.clear();
}

KeyframeReel::KeyframeReel() noexcept : budget_(keyframe_budget_bytes) {
}

void KeyframeReel::set_base_bytes(std::size_t bytes) noexcept {
    base_bytes_ = bytes;
}

void KeyframeReel::set_budget(std::size_t bytes) noexcept {
    budget_ = bytes;
}

std::size_t KeyframeReel::budget() const noexcept {
    return budget_;
}

std::size_t KeyframeReel::base_bytes() const noexcept {
    return base_bytes_;
}

void KeyframeReel::set_working_bytes(std::size_t bytes) noexcept {
    working_bytes_ = bytes;
}

std::size_t KeyframeReel::working_bytes() const noexcept {
    return working_bytes_;
}

void KeyframeReel::forget(uint32_t play_tick) {
    for (auto found = slots_.begin(); found != slots_.end(); ++found) {
        if (found->tick != play_tick)
            continue;
        slots_.erase(found);
        return;
    }
}

void KeyframeReel::add(uint32_t play_tick, std::size_t bytes) {
    const bool sparse = keyframe_sparse(play_tick);
    for (Slot& slot : slots_) {
        if (slot.tick != play_tick)
            continue;
        slot.bytes = bytes;
        slot.sparse = sparse;
        return;
    }
    slots_.push_back(Slot{play_tick, bytes, sparse});
    std::sort(slots_.begin(), slots_.end(), [](const Slot& earlier, const Slot& later) {
        return earlier.tick < later.tick;
    });
}

std::vector<uint32_t> KeyframeReel::thin(uint32_t head) {
    std::vector<uint32_t> dropped;
    std::vector<Slot> kept;
    kept.reserve(slots_.size());
    for (const Slot& slot : slots_) {
        if (keyframe_kept(slot.tick, head))
            kept.push_back(slot);
        else
            dropped.push_back(slot.tick);
    }
    slots_ = std::move(kept);
    return dropped;
}

std::vector<uint32_t> KeyframeReel::evict(uint32_t head) {
    std::vector<uint32_t> dropped;
    while (total_bytes() > budget_ && !slots_.empty()) {
        std::size_t victim = 0;
        for (std::size_t index = 1; index < slots_.size(); ++index) {
            const Slot& candidate = slots_[index];
            const Slot& chosen = slots_[victim];
            const uint32_t candidate_distance = head_distance(candidate.tick, head);
            const uint32_t chosen_distance = head_distance(chosen.tick, head);
            const bool further = candidate_distance > chosen_distance;
            const bool same_distance = candidate_distance == chosen_distance;
            // Equally far: the dense one goes before the sparse one, and the
            // earlier tick before the later, so the choice does not depend on order.
            const bool dense_first = same_distance && !candidate.sparse && chosen.sparse;
            const bool earlier =
                same_distance && candidate.sparse == chosen.sparse && candidate.tick < chosen.tick;
            if (further || dense_first || earlier)
                victim = index;
        }
        dropped.push_back(slots_[victim].tick);
        slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(victim));
    }
    return dropped;
}

std::optional<uint32_t> KeyframeReel::nearest_at_or_before(uint32_t play_tick) const {
    std::optional<uint32_t> found;
    for (const Slot& slot : slots_) {
        if (slot.tick <= play_tick)
            found = slot.tick;
    }
    return found;
}

std::optional<uint32_t> KeyframeReel::next_after(uint32_t play_tick) const {
    for (const Slot& slot : slots_) {
        if (slot.tick > play_tick)
            return slot.tick;
    }
    return std::nullopt;
}

SeekPlan KeyframeReel::plan_seek(uint32_t current, uint32_t target, uint32_t furthest) const {
    SeekPlan plan;
    if (target == current)
        return plan;
    plan.past_furthest = target > furthest;
    if (!plan.past_furthest) {
        const std::optional<uint32_t> key = nearest_at_or_before(target);
        if (!key) {
            plan.kind = target > current ? SeekPlan::Kind::skip : SeekPlan::Kind::stay;
            return plan;
        }
        if (*key == current) {
            plan.kind = SeekPlan::Kind::skip;
            return plan;
        }
        plan.kind = SeekPlan::Kind::restore;
        plan.restore_tick = key;
        return plan;
    }
    plan.kind = SeekPlan::Kind::skip;
    std::optional<uint32_t> ahead;
    for (const Slot& slot : slots_) {
        if (slot.tick > current && slot.tick <= furthest)
            ahead = slot.tick;
    }
    if (ahead) {
        plan.kind = SeekPlan::Kind::restore;
        plan.restore_tick = ahead;
    }
    return plan;
}

std::vector<ReadySpan> KeyframeReel::ready_spans(uint32_t furthest) const {
    std::vector<uint32_t> held;
    for (const Slot& slot : slots_) {
        if (slot.tick <= furthest)
            held.push_back(slot.tick);
    }
    std::vector<ReadySpan> spans;
    for (std::size_t index = 0; index < held.size(); ++index) {
        const uint32_t begin = held[index];
        const bool has_next = index + 1 < held.size();
        const uint32_t end = has_next ? held[index + 1] - 1 : furthest;
        if (end >= begin)
            spans.push_back(ReadySpan{begin, end});
    }
    return spans;
}

bool KeyframeReel::contains(uint32_t play_tick) const noexcept {
    for (const Slot& slot : slots_) {
        if (slot.tick == play_tick)
            return true;
    }
    return false;
}

std::size_t KeyframeReel::size() const noexcept {
    return slots_.size();
}

std::size_t KeyframeReel::total_bytes() const noexcept {
    std::size_t total = base_bytes_ + working_bytes_;
    for (const Slot& slot : slots_)
        total += slot.bytes;
    return total;
}

PlaybackSpeed PlaybackClock::speed() const noexcept {
    return speed_;
}

void PlaybackClock::set_speed(PlaybackSpeed speed) noexcept {
    speed_ = speed;
    asked_milli_ = playback_speed_milli(speed);
}

void PlaybackClock::step_speed(int delta) noexcept {
    set_speed(step_playback_speed(speed_, delta));
}

void PlaybackClock::set_recorded_speed(uint32_t recorded_speed) noexcept {
    if (recorded_speed != 0)
        recorded_speed_ = recorded_speed;
}

void PlaybackClock::pause() noexcept {
    paused_ = true;
}

void PlaybackClock::play() noexcept {
    paused_ = false;
}

bool PlaybackClock::paused() const noexcept {
    return paused_;
}

void PlaybackClock::set_skipping(bool skipping) noexcept {
    skipping_ = skipping;
}

bool PlaybackClock::skipping() const noexcept {
    return skipping_;
}

bool PlaybackClock::thins_sound() const noexcept {
    return skipping_ || playback_speed_thins_sound(speed_);
}

bool PlaybackClock::allows_unit_sound(std::string_view name, uint32_t now_ms) {
    if (!thins_sound())
        return true;
    return sounds_.allow(name, now_ms);
}

FramePlan PlaybackClock::begin_frame(uint32_t elapsed_ms, bool hold) noexcept {
    FramePlan plan;
    last_paced_ = false;
    last_planned_ = 0;
    last_elapsed_ = 0;
    last_raw_elapsed_ = 0;
    const Factor factor = factor_of(speed_);
    plan.asked_milli = factor.milli;
    asked_milli_ = factor.milli;
    if (skipping_) {
        plan.skipping = true;
        plan.asked_milli = 0;
        return plan;
    }
    if (hold || paused_)
        return plan;
    last_raw_elapsed_ = elapsed_ms;
    uint32_t elapsed = elapsed_ms;
    if (elapsed > playback_stall_cap_ms)
        elapsed = playback_stall_cap_ms;
    last_elapsed_ = elapsed;
    constexpr uint64_t kMillion = 1000000;
    // One second at the normal recorded speed owes 30 ticks. The viewer's
    // speed and the recorded speed multiply that, in millionths of a tick.
    const uint64_t owed =
        (uint64_t{elapsed} * 3 * recorded_speed_ * factor.num * 1000) / factor.den;
    fraction_ += owed;
    plan.ticks = static_cast<uint32_t>(fraction_ / kMillion);
    fraction_ %= kMillion;
    plan.draw_last = plan.ticks > 0;
    plan.undrawn = plan.ticks > 0 ? plan.ticks - 1 : 0;
    plan.paced = true;
    last_planned_ = plan.ticks;
    last_paced_ = true;
    return plan;
}

FrameResult PlaybackClock::note_frame(uint32_t ran) noexcept {
    FrameResult result;
    result.asked_milli = asked_milli_;
    result.planned = last_planned_;
    result.ran = ran;
    // A held, paused or skipping frame is not a shortfall, and it leaves the
    // speed the last paced frame reached.
    if (!last_paced_) {
        result.reached_milli = reached_milli_;
        result.reached_lower = reached_lower_;
        last_planned_ = 0;
        return result;
    }
    // A frame that comes late and runs every tick it planned kept its speed:
    // the longer gap planned more ticks. Lower the reading for ticks that did
    // not run, and for time past the cap, which is not played.
    uint64_t reached = asked_milli_;
    if (last_planned_ > 0 && ran < last_planned_)
        reached = reached * ran / last_planned_;
    if (last_raw_elapsed_ > playback_stall_cap_ms && last_elapsed_ > 0)
        reached = reached * last_elapsed_ / last_raw_elapsed_;
    result.reached_milli = static_cast<uint32_t>(reached);
    result.reached_lower = reached < asked_milli_;
    reached_milli_ = result.reached_milli;
    reached_lower_ = result.reached_lower;
    last_paced_ = false;
    last_planned_ = 0;
    return result;
}

void PlaybackClock::note_tick(uint32_t play_tick) noexcept {
    if (play_tick > furthest_)
        furthest_ = play_tick;
}

void PlaybackClock::note_length(uint32_t play_tick) noexcept {
    if (play_tick > length_known_)
        length_known_ = play_tick;
}

uint32_t PlaybackClock::furthest() const noexcept {
    return furthest_;
}

void PlaybackClock::set_catch_up(std::optional<uint32_t> percent) noexcept {
    catch_up_ = percent;
}

void PlaybackClock::set_unavailable(std::string sentence) {
    unavailable_ = std::move(sentence);
}

std::string PlaybackClock::speed_line() const {
    return playback_speed_line(asked_milli_, reached_milli_, reached_lower_, skipping_);
}

PlaybackState PlaybackClock::state(uint32_t play_tick) const {
    PlaybackState result;
    result.tick = play_tick;
    result.length_known = length_known_;
    result.furthest = furthest_;
    result.ready = reel_.ready_spans(furthest_);
    result.catch_up_percent = catch_up_;
    result.asked_milli = skipping_ ? 0 : asked_milli_;
    result.reached_milli = reached_milli_;
    result.reached_lower = reached_lower_;
    result.speed = speed_;
    result.paused = paused_;
    result.skipping = skipping_;
    result.unavailable = unavailable_;
    return result;
}

KeyframeReel& PlaybackClock::reel() noexcept {
    return reel_;
}

const KeyframeReel& PlaybackClock::reel() const noexcept {
    return reel_;
}

} // namespace oa::session::demo
