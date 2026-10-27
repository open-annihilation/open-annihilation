// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A section of the settings dialog taller than the view its rows scroll
// in, which --check-engine-settings shows in place of the open section's
// rows (on Developer, of its list and the list's footer too), on the main
// menu and in a match, so that the wheel and the scroll keys have rows to
// scroll while every section of the dialog fits its view.
#pragma once

#include "oa/ui/engine_settings/dialog.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace oa::app::engine_settings_check {

/// Nine rows in one section, taller than the dialog's view.
inline constexpr std::array<oa::ui::engine_settings::Setting, 9> kTallSection{
    oa::ui::engine_settings::Setting::path_search,
    oa::ui::engine_settings::Setting::wheel_zoom,
    oa::ui::engine_settings::Setting::escape_opens_menu,
    oa::ui::engine_settings::Setting::switch_alt,
    oa::ui::engine_settings::Setting::unit_limit,
    oa::ui::engine_settings::Setting::max_frame_rate,
    oa::ui::engine_settings::Setting::anti_aliasing,
    oa::ui::engine_settings::Setting::screen_size,
    oa::ui::engine_settings::Setting::frame_stats,
};

/// Returns the tall section's rows for any section (SectionHooks::settings).
///
/// @return kTallSection
[[nodiscard]] inline std::span<const oa::ui::engine_settings::Setting>
tall_section_settings(void*, oa::ui::engine_settings::Page) noexcept {
    return kTallSection;
}

/// The hooks that show the tall section in place of every section's rows,
/// with the dialog's own locks. They live as long as the program, so a
/// dialog never holds them longer than they last.
inline constexpr oa::ui::engine_settings::SectionHooks kTallSectionHooks{
    nullptr, tall_section_settings, nullptr, nullptr
};

/// The source pixels a notch of the mouse wheel scrolls the dialog's open section.
inline constexpr int32_t kWheelStepPixels = 24;

/// Shows the tall section in place of an open dialog's rows.
///
/// @param[in,out] dialog the dialog
inline void show_tall_section(oa::ui::engine_settings::Dialog& dialog) noexcept {
    dialog.section_hooks = &kTallSectionHooks;
}

/// Shows an open dialog's own rows again: every section at its top, and no
/// turn of the wheel carried.
///
/// @param[in,out] dialog the dialog
inline void show_own_sections(oa::ui::engine_settings::Dialog& dialog) noexcept {
    dialog.section_hooks = nullptr;
    dialog.scroll = {};
    dialog.wheel_rows = 0.0F;
}

} // namespace oa::app::engine_settings_check
