// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A gadget name maps to the registry action whose word it holds, and to the
// gui id of its file.
#include "gui_actions.hpp"

#include "oa/test/check.hpp"

#include <iostream>
#include <ostream>
#include <string_view>

namespace {

void gadget_words_follow_the_trial_order() {
    using oa::app::order_action_id;
    OA_CHECK(order_action_id("ARMATTACK") == "order.attack");
    OA_CHECK(order_action_id("CORBLAST") == "order.dgun");
    OA_CHECK(order_action_id("GOKMOVE") == "order.move");
    OA_CHECK(order_action_id("TALONOFF") == "order.on-off");
    OA_CHECK(order_action_id("MOVEORD") == "order.move-state");
    OA_CHECK(order_action_id("MOVEORD") != "order.move");
    OA_CHECK(order_action_id("MOVE") == "order.move");
    OA_CHECK(order_action_id("UNLOAD") == "order.unload");
    OA_CHECK(order_action_id("UNLOAD") != "order.load");
    OA_CHECK(order_action_id("LOAD") == "order.load");
}

void gui_ids_fold_the_stem_and_the_name() {
    using oa::app::gui_action_id;
    OA_CHECK(gui_action_id("My File", "Big_Button Name") == "gui.my-file.big-button-name");
    OA_CHECK(gui_action_id("ARMGEN", "DONE") == "gui.armgen.done");
}

} // namespace

int main() {
    gadget_words_follow_the_trial_order();
    gui_ids_fold_the_stem_and_the_name();
    std::cout << "gui actions checked\n";
    return oa::test::check_exit_status();
}
