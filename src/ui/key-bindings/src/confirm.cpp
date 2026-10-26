// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A press held until a dangerous action may run. The caller passes the time.
#include "oa/ui/key_bindings/helpers.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/ui/key_bindings/layout.hpp"

#include <string>
#include <string_view>

namespace oa::ui::key_bindings {
namespace {

/// The Yes of YESORNO while the surrender question is up. Made gui actions
/// have no registry row, so this id is confirmable by the hold rule itself.
constexpr std::string_view yes_action = "gui.yesorno.choice1";

/// Reports whether the action may require a hold.
///
/// @param resolved the bindings in use
/// @param action the action id
/// @return true when the registry marks it confirmable, or it is Yes
bool confirmable(const Resolved& resolved, std::string_view action) {
    if (action == yes_action)
        return true;
    for (const Bound& bound : resolved.actions) {
        if (bound.id != action)
            continue;
        return bound.entry != nullptr && bound.entry->confirmable;
    }
    if (const oa::data::input_actions::Action* entry = oa::data::input_actions::find(action))
        return entry->confirmable;
    return false;
}

} // namespace

void HoldConfirm::start(std::string_view id, uint64_t now_ms, Target target) {
    id_.assign(id.begin(), id.end());
    target_ = target;
    started_ms_ = now_ms;
    started_ = true;
    released_ = false;
}

void HoldConfirm::release() {
    if (started_)
        released_ = true;
}

HoldState HoldConfirm::tick(uint64_t now_ms, uint32_t long_press_ms) {
    if (!started_)
        return HoldState::none;
    if (released_) {
        clear();
        return HoldState::none;
    }
    if (long_press_ms == 0 || now_ms - started_ms_ >= long_press_ms)
        return HoldState::done;
    return HoldState::waiting;
}

void HoldConfirm::cancel() {
    clear();
}

bool HoldConfirm::active() const {
    return started_ && !released_;
}

int32_t HoldConfirm::progress(uint64_t now_ms, uint32_t long_press_ms) const {
    if (!started_ || released_)
        return 0;
    if (long_press_ms == 0)
        return 1000;
    if (now_ms <= started_ms_)
        return 0;
    const uint64_t elapsed = now_ms - started_ms_;
    if (elapsed >= long_press_ms)
        return 1000;
    return static_cast<int32_t>((elapsed * 1000ULL) / long_press_ms);
}

const std::string& HoldConfirm::id() const {
    return id_;
}

const HoldConfirm::Target& HoldConfirm::target() const {
    return target_;
}

void HoldConfirm::clear() {
    id_.clear();
    target_ = {};
    started_ms_ = 0;
    started_ = false;
    released_ = false;
}

bool needs_hold(const Resolved& resolved, std::string_view action, bool setting_on) {
    if (!setting_on || !confirmable(resolved, action))
        return false;
    if (!resolved.hold_to_confirm.has_value())
        return true;
    for (const std::string& id : *resolved.hold_to_confirm) {
        if (id == action)
            return true;
    }
    return false;
}

} // namespace oa::ui::key_bindings
