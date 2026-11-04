// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Control text as the MCP server reports it (include/oa/app/automation/view.hpp).
#include "oa/app/automation/view.hpp"

#include "oa/present/game_text.hpp"

namespace oa::app::automation {

void read_control_text(std::vector<AutomationControl>& controls) {
    for (AutomationControl& control : controls) {
        if (!control.masked)
            control.text = oa::present::decode_game_text(control.text, true);
        for (std::string& item : control.items)
            item = oa::present::decode_game_text(item, true);
        for (std::string& key : control.item_keys)
            key = oa::present::decode_game_text(key, true);
    }
}

} // namespace oa::app::automation
