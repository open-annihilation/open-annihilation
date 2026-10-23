// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The runtime's seat chat: the three sources that feed it, and the flush
// after a frame's ticks.

#include "oa/app/runtime.hpp"

#include <string_view>

namespace oa::app {

void Runtime::flush_seat_chat() {
    if (match_)
        seat_chat_.flush(match_->state(), message_hooks());
}

seat_chat::Said
Runtime::say_for_seat(uint8_t seat, std::string_view text, uint8_t mode, uint8_t target) {
    if (!match_)
        return seat_chat::Said::not_a_seat;
    return seat_chat_.say(match_->state(), seat, text, mode, target);
}

void Runtime::set_seat_chat_listener(const seat_chat::Listener& listener) noexcept {
    seat_chat_.set_listener(listener);
}

void Runtime::hear_seat_chat_remote(
    uint8_t from, uint8_t addressee, bool broadcast, const char* text
) {
    if (!match_ || text == nullptr)
        return;
    seat_chat_.hear_remote(match_->state(), from, addressee, broadcast, text);
}

} // namespace oa::app
