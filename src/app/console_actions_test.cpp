// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Every console action the match answers, and an id that has no key code.

#include "console_actions.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/console/hotkeys.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace {

using oa::app::console_code;
namespace hotkey = oa::ui::console::hotkey;

void one(std::string_view action, uint32_t code) {
    const std::optional<uint32_t> found = console_code(action);
    OA_CHECK(found.has_value());
    OA_CHECK(found == code);
}

void codes_match_the_console() {
    one("game.team-menu", hotkey::tab);
    one("game.share-panel", static_cast<uint32_t>('h'));
    one("game.health-bars", static_cast<uint32_t>('`'));
    one("game.repeat-command", hotkey::repeat_command);
    one("debug.keys", hotkey::f11);
    one("debug.movie-capture", hotkey::control_f10);
    one("debug.refill", static_cast<uint32_t>('='));
    one("debug.kill-under-pointer", static_cast<uint32_t>(']'));
    one("debug.i", static_cast<uint32_t>('i'));
    one("debug.overlay", static_cast<uint32_t>('m'));
    OA_CHECK(!console_code("order.move").has_value());
}

} // namespace

int main() {
    codes_match_the_console();
    return oa::test::check_exit_status();
}
