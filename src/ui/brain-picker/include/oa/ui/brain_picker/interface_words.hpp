// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// English texts the brain seat picker passes to the interface catalogue.
// oa-tool joins this list with the other modules' lists.

#pragma once

#include "oa/ui/brain_picker/text.hpp"

#include <string_view>

namespace oa::ui::brain_picker {

/// English texts this module passes to the interface catalogue
/// (oa/data/languages/interface_text.hpp). oa-tool sorts the modules'
/// lists together and keeps one of each text. Words reused from the Library
/// are named here so this list is the module's whole set; the join keeps one.
inline constexpr std::string_view interface_words[] = {
    words::title_player,
    words::context_seat,
    words::no_mod,
    words::built_in,
    words::built_in_player,
    words::built_into,
    words::brain_level,
    words::brain_version_level,
    words::every_seat,
    words::level_from_difficulty,
    words::level,
    words::version,
    words::use_every_seat,
    words::sees_every_unit,
    words::left_out,
    words::cancel,
    words::use_this,
    words::details,
    words::back,
    words::close_button,
    words::switch_off,
    words::switch_on,
    words::empty_brains,
};

} // namespace oa::ui::brain_picker
