// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// English texts the photo panel passes to the interface catalogue. oa-tool
// joins this list with the other modules' lists.
#pragma once

#include "oa/ui/photo_panel/words.hpp"

#include <string_view>

namespace oa::ui::photo_panel {

/// English texts this module passes to the interface catalogue. oa-tool sorts
/// the modules' lists together and keeps one of each text.
inline constexpr std::string_view interface_words[] = {
    words::leave_head,
    words::shows_panel,
    words::photo_not_taken,
    words::show_selection,
    words::show_health,
    words::player_names,
    words::shared_game,
    words::hold_picture,
    words::thirds,
    words::safe_area,
    words::run,
    words::freeze,
    words::step,
    words::between_ticks,
    words::follow_unit,
    words::turn,
    words::tilt,
    words::speed,
    words::screen,
    words::copy_to_clipboard,
    words::far,
    words::closest,
    words::replay_paused,
    words::live,
    words::time,
    words::hide_panel,
    words::guides,
    words::leave,
    words::take_photo,
    words::copy,
    words::camera,
    words::look,
    words::photo,
    words::classic_view,
    words::zoom,
    words::slow,
    words::clock,
    words::size,
    words::file,
    words::expanded,
    words::collapsed,
    words::off,
    words::on,
    words::photo_mode,
    words::frozen,
    words::whole_map,
    words::medium,
    words::near,
    words::centre_lines,
    words::copied,
};

} // namespace oa::ui::photo_panel
