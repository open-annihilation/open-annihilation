// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/key_editor/screen.hpp"

#include "oa/ui/kit/input.hpp"

#include <string>

namespace oa::ui::key_editor {

std::string row_control_name(std::string_view action_id, std::size_t index) {
    const std::string named = std::string(name_row_prefix) + std::string(action_id);
    const bool long_gui = action_id.starts_with("gui.") && action_id.size() > 80;
    kit::DisplayList probe;
    kit::Control control;
    control.id = 1;
    control.name = named;
    probe.controls.push_back(std::move(control));
    if (!long_gui && named.size() <= 100 && kit::name_problem(probe).empty())
        return named;
    return std::string(name_row_prefix) + "n" + std::to_string(index);
}

} // namespace oa::ui::key_editor
