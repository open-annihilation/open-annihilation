// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Which actions need a hold, and a hold that finishes only while the press
// is still down.

#include "oa/test/check.hpp"
#include "oa/ui/key_bindings/helpers.hpp"
#include "oa/ui/key_bindings/layout.hpp"

#include "oa/data/input_actions/actions.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace keys = oa::ui::key_bindings;
namespace actions = oa::data::input_actions;

/// Puts a registry action on a resolved layout.
///
/// @param resolved the layout
/// @param id the action id
void add_bound(keys::Resolved& resolved, std::string_view id) {
    keys::Bound bound;
    bound.id = std::string(id);
    bound.entry = actions::find(id);
    if (bound.entry == nullptr)
        std::fprintf(stderr, "missing action %.*s\n", static_cast<int>(id.size()), id.data());
    resolved.actions.push_back(std::move(bound));
}

} // namespace

int main() {
    keys::Resolved resolved;
    add_bound(resolved, "order.self-destruct");
    add_bound(resolved, "order.attack");
    OA_CHECK(actions::find("order.self-destruct") != nullptr);
    OA_CHECK(actions::find("order.self-destruct")->confirmable);
    OA_CHECK(actions::find("order.attack") != nullptr);
    OA_CHECK(!actions::find("order.attack")->confirmable);

    OA_CHECK(!keys::needs_hold(resolved, "order.self-destruct", false));
    OA_CHECK(keys::needs_hold(resolved, "order.self-destruct", true));
    OA_CHECK(keys::needs_hold(resolved, "gui.yesorno.choice1", true));
    OA_CHECK(!keys::needs_hold(resolved, "order.attack", true));

    resolved.hold_to_confirm = std::vector<std::string>{"order.self-destruct"};
    OA_CHECK(keys::needs_hold(resolved, "order.self-destruct", true));
    OA_CHECK(!keys::needs_hold(resolved, "gui.yesorno.choice1", true));

    resolved.hold_to_confirm = std::vector<std::string>{"order.attack"};
    OA_CHECK(!keys::needs_hold(resolved, "order.self-destruct", true));
    OA_CHECK(!keys::needs_hold(resolved, "order.attack", true));

    resolved.hold_to_confirm.reset();
    OA_CHECK(keys::needs_hold(resolved, "order.self-destruct", true));

    keys::HoldConfirm hold;
    keys::HoldConfirm::Target target;
    hold.start("order.self-destruct", 0, target);
    OA_CHECK(hold.active());
    OA_CHECK(hold.tick(999, 1000) == keys::HoldState::waiting);
    OA_CHECK(hold.tick(1000, 1000) == keys::HoldState::done);
    OA_CHECK(hold.active());

    keys::HoldConfirm released;
    released.start("order.self-destruct", 0, target);
    released.release();
    OA_CHECK(!released.active());
    OA_CHECK(released.tick(900, 1000) == keys::HoldState::none);

    keys::HoldConfirm filling;
    filling.start("order.self-destruct", 0, target);
    constexpr uint64_t times[] = {0, 250, 500, 750, 1000};
    constexpr int32_t thousandths[] = {0, 250, 500, 750, 1000};
    for (int step = 0; step < 5; ++step)
        OA_CHECK(filling.progress(times[step], 1000) == thousandths[step]);
    filling.cancel();
    OA_CHECK(!filling.active());
    OA_CHECK(filling.progress(1000, 1000) == 0);

    return 0;
}
