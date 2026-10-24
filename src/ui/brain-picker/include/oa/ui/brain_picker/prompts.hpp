// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The question a saved game asks when a brain it names is not installed.
// Pure: it reads no file, preference, clock or network, and every word
// comes from text.hpp through the Library's TextHooks. The app raises it.
#pragma once

#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/library/text.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace oa::ui::brain_picker {

/// BACK. It loads nothing. The cancel button.
inline constexpr std::string_view back_id = "back";
/// LOAD WITH THE BUILT-IN PLAYER. The primary button, and the one marked.
inline constexpr std::string_view load_built_in_id = "load-built-in";

/// One brain a saved game names that is not installed here.
struct MissingBrain {
    std::string name{};    ///< the name players see, or the id when it has none
    std::string version{}; ///< the version the save recorded
    int32_t player{};      ///< the player, 0 to 9; the question counts seats from 1
};

/// Makes "Load with the built-in player?" for the brains a save names that
/// are not installed.
///
/// One paragraph names each missing brain and the seat it played, counted
/// from 1. The next says the built-in player takes those seats from the
/// units as they stand, and that GET in the Library loads the game as it
/// was saved. BACK is the cancel button. LOAD WITH THE BUILT-IN PLAYER is
/// the primary button and the one marked.
///
/// @param missing the brains that are not installed, in seat order
/// @param hooks how texts are looked up
/// @return the question
[[nodiscard]] kit::Question
load_built_in_question(std::span<const MissingBrain> missing, const library::TextHooks& hooks = {});

} // namespace oa::ui::brain_picker
