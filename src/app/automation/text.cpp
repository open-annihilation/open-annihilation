// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Control text as the MCP server reports it (include/oa/app/automation/view.hpp).
#include "oa/app/automation/view.hpp"

#include "oa/present/game_text.hpp"

#include <string>
#include <utility>
#include <vector>

namespace oa::app::automation {

void read_shown_focus(Observed& now, const std::vector<AutomationControl>& controls) {
    now.focus_name.clear();
    now.focus_label.clear();
    now.focus_role.clear();
    for (const AutomationControl& control : controls) {
        // A control behind a modal stays in the tree. The focus a screen
        // reader hears is the first one that is shown.
        if (!control.focused || control.hidden)
            continue;
        now.focus_name = control.name;
        now.focus_role = std::string(control_role_name(control.role));
        // The event's label is the tree's: code-page text becomes UTF-8,
        // and a masked field's label is never its text.
        std::vector<AutomationControl> shown{control};
        read_control_text(shown);
        now.focus_label = std::move(shown.front().label);
        break;
    }
}

void read_control_text(std::vector<AutomationControl>& controls) {
    for (AutomationControl& control : controls) {
        if (control.masked) {
            // The label is never the secret. A caption that is not the text is kept.
            if (!control.text.empty() && control.label == control.text)
                control.label.clear();
        } else {
            control.text = oa::present::decode_game_text(control.text, true);
        }
        if (!control.label.empty())
            control.label = oa::present::decode_game_text(control.label, true);
        if (!control.description.empty())
            control.description = oa::present::decode_game_text(control.description, true);
        if (!control.value_text.empty())
            control.value_text = oa::present::decode_game_text(control.value_text, true);
        for (std::string& item : control.items)
            item = oa::present::decode_game_text(item, true);
        for (std::string& key : control.item_keys)
            key = oa::present::decode_game_text(key, true);
    }
}

} // namespace oa::app::automation
