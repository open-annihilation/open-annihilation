// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A stage camera accepts finite complete input and never partially replaces a view.
#include "stage_view.hpp"
#include "oa/test/check.hpp"

#include <sstream>

int main() {
    oa::app::StageView view;
    std::istringstream valid("5120 5120 .25 20 -53  ");
    OA_CHECK(oa::app::read_stage_view(valid, view));
    OA_CHECK(
        view.centre_x == 5120 && view.centre_z == 5120 && view.zoom == .25 && view.yaw == 20 &&
        view.pitch == -53
    );
    for (const auto* text :
         {"5120 5120 .25 20",
          "5120 5120 0 20 -53",
          "5120 5120 -1 20 -53",
          "5120 5120 .25 20 -53 extra",
          "5120 5120 .25 20 nan",
          "5120 5120 .25 20 1e300"}) {
        std::istringstream invalid(text);
        OA_CHECK(!oa::app::read_stage_view(invalid, view));
        OA_CHECK(
            view.centre_x == 5120 && view.centre_z == 5120 && view.zoom == .25 && view.yaw == 20 &&
            view.pitch == -53
        );
    }
    return oa::test::check_exit_status();
}
