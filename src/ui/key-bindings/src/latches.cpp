// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Hold latches and sticky modifiers. The time is the caller's.

#include "oa/ui/key_bindings/helpers.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::key_bindings {
namespace {

/// The index of one modifier bit, or -1 when the value is not one bit.
///
/// @param bit a modifier bit
/// @return 0 to 7, or -1
int bit_index(uint8_t bit) {
    if (bit == 0 || (bit & static_cast<uint8_t>(bit - 1)) != 0)
        return -1;
    int index = 0;
    uint8_t rest = bit;
    while ((rest & 1u) == 0) {
        rest = static_cast<uint8_t>(rest >> 1);
        ++index;
    }
    return index < 8 ? index : -1;
}

} // namespace

Latches::Entry* Latches::find(std::string_view id) {
    for (Entry& entry : entries_) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

const Latches::Entry* Latches::find(std::string_view id) const {
    for (const Entry& entry : entries_) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

Latches::Entry& Latches::ensure(std::string_view id) {
    if (Entry* entry = find(id))
        return *entry;
    Entry made;
    made.id = std::string{id};
    entries_.push_back(std::move(made));
    return entries_.back();
}

void Latches::press(std::string_view id, uint64_t now_ms) {
    if (id.empty())
        return;
    Entry& entry = ensure(id);
    entry.held = true;
    entry.used_while_held = false;
    entry.pressed_ms = now_ms;
}

void Latches::release(std::string_view id, uint64_t now_ms, uint32_t long_press_ms) {
    Entry* entry = find(id);
    if (entry == nullptr || !entry->held)
        return;
    const uint64_t held_ms = now_ms >= entry->pressed_ms ? now_ms - entry->pressed_ms : 0;
    if (held_ms < long_press_ms && !entry->used_while_held) {
        entry->latched = !entry->latched;
        if (entry->latched) {
            for (std::size_t index = 0; index < shown_.size(); ++index) {
                if (shown_[index] == entry->id) {
                    shown_.erase(shown_.begin() + static_cast<std::ptrdiff_t>(index));
                    break;
                }
            }
            shown_.push_back(entry->id);
        } else {
            for (std::size_t index = 0; index < shown_.size(); ++index) {
                if (shown_[index] == entry->id) {
                    shown_.erase(shown_.begin() + static_cast<std::ptrdiff_t>(index));
                    break;
                }
            }
        }
    }
    entry->held = false;
    entry->used_while_held = false;
}

void Latches::cancel(std::string_view id) {
    Entry* entry = find(id);
    if (entry == nullptr)
        return;
    entry->held = false;
    entry->used_while_held = false;
}

void Latches::used(std::string_view id) {
    Entry* entry = find(id);
    if (entry != nullptr && entry->held)
        entry->used_while_held = true;
}

void Latches::clear() {
    entries_.clear();
    shown_.clear();
}

bool Latches::active(std::string_view id) const {
    const Entry* entry = find(id);
    return entry != nullptr && (entry->latched || entry->held);
}

bool Latches::latched(std::string_view id) const {
    const Entry* entry = find(id);
    return entry != nullptr && entry->latched;
}

bool Latches::held(std::string_view id) const {
    const Entry* entry = find(id);
    return entry != nullptr && entry->held;
}

const std::vector<std::string>& Latches::shown() const {
    return shown_;
}

void StickyModifiers::let_go(uint8_t bit) {
    const int index = bit_index(bit);
    if (index < 0)
        return;
    arms_[static_cast<std::size_t>(index)] = Arm{};
    for (std::size_t at = 0; at < shown_.size(); ++at) {
        if (shown_[at] == bit) {
            shown_.erase(shown_.begin() + static_cast<std::ptrdiff_t>(at));
            break;
        }
    }
}

void StickyModifiers::tap(uint8_t modifier, uint64_t now_ms, uint32_t double_press_ms) {
    const int index = bit_index(modifier);
    if (index < 0)
        return;
    Arm& arm = arms_[static_cast<std::size_t>(index)];
    if (arm.on && arm.locked) {
        let_go(modifier);
        return;
    }
    if (arm.on) {
        const bool in_window =
            now_ms >= arm.at_ms && now_ms - arm.at_ms <= static_cast<uint64_t>(double_press_ms);
        if (in_window) {
            arm.locked = true;
            return;
        }
        let_go(modifier);
        return;
    }
    arm.on = true;
    arm.locked = false;
    arm.at_ms = now_ms;
    shown_.push_back(modifier);
}

void StickyModifiers::consume() {
    const std::vector<uint8_t> order = shown_;
    for (const uint8_t bit : order) {
        const int index = bit_index(bit);
        if (index < 0)
            continue;
        const Arm& arm = arms_[static_cast<std::size_t>(index)];
        if (arm.on && !arm.locked)
            let_go(bit);
    }
}

uint8_t StickyModifiers::mods() const {
    uint8_t bits = 0;
    for (const uint8_t bit : shown_)
        bits = static_cast<uint8_t>(bits | bit);
    return bits;
}

uint8_t StickyModifiers::armed() const {
    uint8_t bits = 0;
    for (const uint8_t bit : shown_) {
        const int index = bit_index(bit);
        if (index < 0)
            continue;
        const Arm& arm = arms_[static_cast<std::size_t>(index)];
        if (arm.on && !arm.locked)
            bits = static_cast<uint8_t>(bits | bit);
    }
    return bits;
}

uint8_t StickyModifiers::locked() const {
    uint8_t bits = 0;
    for (const uint8_t bit : shown_) {
        const int index = bit_index(bit);
        if (index < 0)
            continue;
        if (arms_[static_cast<std::size_t>(index)].locked)
            bits = static_cast<uint8_t>(bits | bit);
    }
    return bits;
}

void StickyModifiers::clear() {
    arms_ = {};
    shown_.clear();
}

const std::vector<uint8_t>& StickyModifiers::shown() const {
    return shown_;
}

} // namespace oa::ui::key_bindings
