// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the running match keeps for ui.megamap: its icons and their unit
// sets, the terrain picture fitted to the battlefield, the features it
// draws, and the pointer's press on it.
#pragma once

#include "oa/data/defs/categories.hpp"
#include "oa/ui/hud/megamap.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace oa::app {

/// The largest icon drawn, in pixels; a larger picture is cut to it.
inline constexpr int32_t kMegamapIconLimit = 22;

/// One megamap icon: its palette indices, rows `width` apart.
struct MegamapIcon {
    int32_t width{};
    int32_t height{};
    std::vector<uint8_t> pixels;
};

/// One icon entry and the unit types it stands for.
struct MegamapIconEntry {
    MegamapIcon icon;
    const oa::data::defs::CategoryMask* types{}; ///< a category's mask, or one of `own_masks`
};

/// A feature the megamap draws on its terrain picture
/// (oa::ui::hud::megamap_draws_feature).
struct MegamapFeature {
    int32_t cell_x{}; ///< the first cell of its footprint
    int32_t cell_z{};
    uint16_t def{};          ///< its index in World.feature_defs
    uint8_t ground_height{}; ///< MapPlot.height of its first cell
};

/// One unit's icon as the megamap last drew it: the box in which the pointer
/// is on it, in battlefield pixels, right and bottom excluded.
struct MegamapMark {
    uint16_t unit{};
    int32_t left{};
    int32_t top{};
    int32_t right{};
    int32_t bottom{};
};

struct MegamapState {
    bool prepared{}; ///< icons read and features noted for this match
    oa::ui::hud::IconConfig config{};
    std::vector<MegamapIconEntry> entries; ///< side commanders' icons, then the lines'
    MegamapIcon unknown;
    MegamapIcon nothing;
    /// Unit sets made here (side commanders, the built-in icons' sets).
    std::vector<oa::data::defs::CategoryMaskStorage> own_words;
    std::vector<oa::data::defs::CategoryMask> own_masks;
    std::vector<MegamapFeature> features;
    /// The terrain picture, with the features drawn, and the layout it was
    /// made for.
    oa::ui::hud::MegamapLayout layout{};
    std::vector<uint8_t> terrain;
    /// The left button's press, in battlefield pixels, while it is held.
    bool pressed{};
    int32_t press_x{};
    int32_t press_y{};
    int32_t pointer_x{};
    int32_t pointer_y{};
    uint16_t hovered{}; ///< the unit whose icon is under the pointer
    /// The icons drawn in the last frame, in the order drawn.
    std::vector<MegamapMark> marks;

    /// Returns the unit whose icon was drawn last under a point, as the
    /// drawing finds the hovered one, or 0.
    ///
    /// @param x battlefield column
    /// @param y battlefield row
    /// @return the unit, or 0
    [[nodiscard]] uint16_t unit_at(int32_t x, int32_t y) const {
        uint16_t found = 0;
        for (const auto& mark : marks)
            if (x >= mark.left && x < mark.right && y >= mark.top && y < mark.bottom)
                found = mark.unit;
        return found;
    }
};

} // namespace oa::app
