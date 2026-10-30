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
    one("game.health-bars-mode-1", static_cast<uint32_t>('!'));
    one("game.health-bars-mode-2", static_cast<uint32_t>('#'));
    one("game.health-bars-mode-3", static_cast<uint32_t>('*'));
    one("game.health-bars-mode-4", static_cast<uint32_t>('~'));
    one("game.repeat-command", hotkey::repeat_command);
    one("debug.keys", hotkey::f11);
    one("debug.movie-capture", hotkey::control_f10);
    one("debug.refill", static_cast<uint32_t>('='));
    one("debug.kill-under-pointer", static_cast<uint32_t>(']'));
    one("debug.i", static_cast<uint32_t>('i'));
    one("debug.overlay", static_cast<uint32_t>('m'));
    one("camera.bookmark-set-1", hotkey::control_f5);
    one("camera.bookmark-set-2", hotkey::control_f5 + 1);
    one("camera.bookmark-set-3", hotkey::control_f5 + 2);
    one("camera.bookmark-set-4", hotkey::control_f5 + 3);
    one("camera.bookmark-go-1", hotkey::f5);
    one("camera.bookmark-go-2", hotkey::f5 + 1);
    one("camera.bookmark-go-3", hotkey::f5 + 2);
    one("camera.bookmark-go-4", hotkey::f5 + 3);
    one("select.builders", hotkey::control_a + static_cast<uint32_t>('B' - 'A'));
    one("select.factories", hotkey::control_a + static_cast<uint32_t>('F' - 'A'));
    one("select.aircraft", hotkey::control_a + static_cast<uint32_t>('V' - 'A'));
    one("select.category-w", hotkey::control_a + static_cast<uint32_t>('W' - 'A'));
    one("select.category-b", hotkey::control_a + static_cast<uint32_t>('B' - 'A'));
    one("select.category-f", hotkey::control_a + static_cast<uint32_t>('F' - 'A'));
    one("select.category-v", hotkey::control_a + static_cast<uint32_t>('V' - 'A'));
    OA_CHECK(!console_code("order.move").has_value());
    OA_CHECK(!console_code("select.idle-builders").has_value());
    OA_CHECK(!console_code("select.idle-factories").has_value());
    OA_CHECK(!console_code("select.category-9").has_value());
    OA_CHECK(!console_code("camera.bookmark-set-5").has_value());
    OA_CHECK(!console_code("camera.bookmark-go-0").has_value());
}

} // namespace

int main() {
    codes_match_the_console();
    return oa::test::check_exit_status();
}
