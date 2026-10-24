// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/app/runtime.hpp"
#include "match_models.hpp"
#include "oa/ui/hud/health_bar.hpp"

#include <string>
#include <vector>

namespace oa::app {

void Runtime::draw_live_unit_overlays() {
    if (!live_renderer_active() || !match_ || match_world_cpu_.rgb.empty())
        return;
    drawn_health_bars_.clear();
    paint_on(PaintLayer::battlefield);
    const auto& world = match_->state();
    const auto& slots = match_->world().slots;
    const auto size = oa::ui::hud::health_bar_size(match_zoom());
    constexpr float fixed_scale = 65536.0F;
    constexpr int label_left = 4;
    constexpr int squad_gap = 2;
    constexpr int countdown_gap = 10;
    std::vector<bool> painted(slots.size());
    for (const auto& draw : match_models().draws.models) {
        if (draw.on_map || draw.stand_in >= 0 || draw.model.unit == nullptr)
            continue;
        const auto id = draw.model.unit->id;
        // Model bounds validate the current slot and visibility before any label is drawn.
        if (id == 0 || id >= slots.size() || painted[id] || !live_unit_bounds(id))
            continue;
        const auto& unit = slots[id].record;
        const auto position = draw.shown_position.value_or(unit.position);
        const auto frame = live_project_world(
            position.x / fixed_scale, position.y / fixed_scale, position.z / fixed_scale
        );
        const int x = frame.x - match_layout_.left;
        const int y = frame.y - match_layout_.top + size.below_unit;
        painted[id] = true;
        if (oa::ui::hud::draws_health_bar(world, unit)) {
            const auto* definition = oa::world_unit_def_of(&world, &unit);
            oa::ui::hud::HealthBar bar{};
            if (definition != nullptr &&
                oa::ui::hud::unit_health_bar(world.game, unit, *definition, x, y, bar, size)) {
                const auto fill = [this](const oa::Rect32& rect, uint8_t color) {
                    fill_hud_rect(
                        rect.x1, rect.y1, rect.x2 - rect.x1 + 1, rect.y2 - rect.y1 + 1, color
                    );
                };
                fill(bar.trough, bar.trough_color);
                fill(bar.fill, bar.fill_color);
                drawn_health_bars_.push_back(bar);
            }
        }
        const PanelText labels(*this);
        if (oa::ui::hud::draws_squad_digit(world, unit))
            draw_match_label(
                x - label_left,
                y + size.half_height + squad_gap,
                std::string(1, oa::ui::hud::squad_digit(unit)),
                255
            );
        if (const auto count = match_->self_destruct_remaining(id); count != 0)
            draw_match_label(
                x - label_left, y - size.half_height - countdown_gap, std::to_string(count), 1
            );
    }
}

} // namespace oa::app
