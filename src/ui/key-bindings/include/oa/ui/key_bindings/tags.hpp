// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The action a Category word selects. The scan, resolution and the editor
// share this mapping.
#pragma once

#include <string>
#include <string_view>

namespace oa::ui::key_bindings {

/// The action id a Category word selects, or empty when the word makes no id.
///
/// `CTRL_<letter>` is `select.builders`, `select.commander`, `select.factories`
/// or `select.aircraft` for B, C, F and V. Any other `CTRL_<letter>` whose
/// Ctrl chord no permanent action binds in play is `select.category-<letter>`.
/// Every other word that folds to a tag id is `select.tag-<word>`. Folding
/// lower-cases ASCII and turns spaces and underscores into hyphens. A word
/// that still breaks the id grammar, or whose suffix is longer than 85 bytes,
/// makes no action. With the selection-shortcut hack on, the editor follows
/// the action that replaces the base id; this function still returns the base.
/// The word is the data's spelling; the caller keeps it.
///
/// @param word the Category word, as the data spells it
/// @return the action id, or empty
[[nodiscard]] std::string tag_action_id(std::string_view word);

} // namespace oa::ui::key_bindings
