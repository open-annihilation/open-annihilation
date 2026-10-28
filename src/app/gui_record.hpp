// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Finds a record of a GUI layout by its name (runtime_main_menu.cpp).
#pragma once

#include "oa/ui/gui_layout.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace oa::app {

/// Returns a record of a GUI layout by its name, compared in either case as
/// the game's own lookup compares them.
///
/// @param gadgets the layout's records
/// @param name the name
/// @return its place among them; none without such a record
[[nodiscard]] std::optional<std::size_t>
gui_record_named(std::span<const oa::ui::gui_layout::Gadget> gadgets, std::string_view name);

} // namespace oa::app
