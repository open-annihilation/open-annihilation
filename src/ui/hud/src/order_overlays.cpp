// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/hud/order_overlays.hpp"

#include "oa/sim/unit_movement/movement.hpp"

#include <algorithm>
#include <initializer_list>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace oa::ui::hud {
namespace {

constexpr double kTwoPi = 6.28318530717958;
/// Circle segments per pixel of circumference.
constexpr double kSegmentsPerPixel = 0.125;
/// Vertical squash of the target ring.
constexpr double kRingSquash = 0.89;
constexpr int32_t kRingDefaultRadius = 0x20;
constexpr uint16_t kRingStep = 0x1000;
/// Build sites close in over this many ticks.
constexpr uint32_t kSiteAnimationTicks = 10;
/// Kamikaze blast circle pulse period and minimum radius.
constexpr uint32_t kPulseTicks = 60;
constexpr uint32_t kPulseMinimum = 8;
/// Path pips: one every 48 pixels, sliding forward over 30 ticks.
constexpr int32_t kPipSpacing = 0x300000;
constexpr int32_t kPipCycleTicks = 30;
constexpr int32_t kOneUnit = 0x10000;

int16_t high16(int32_t value) noexcept {
    return static_cast<int16_t>(static_cast<uint32_t>(value) >> 16);
}

/// Truncated length of a 16.16 vector, as the engine's vector length.
int32_t vector_length(const int32_t v[3]) noexcept {
    const double sum = static_cast<double>(v[0]) * v[0] + static_cast<double>(v[1]) * v[1] +
                       static_cast<double>(v[2]) * v[2];
    return static_cast<int32_t>(std::sqrt(sum));
}

uint8_t ui_color(const OverlayContext& context, UiColor slot) noexcept {
    return context.world->game.ui_colors[slot];
}

void line(
    const OverlayContext& context, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color
) {
    if (context.sink.line != nullptr)
        context.sink.line(context.sink.user, x0, y0, x1, y1, color);
}

uint32_t order_age(const OverlayContext& context, const OrderOverlay& order) noexcept {
    return context.world->game.tick - order.issue_tick;
}

const MissionOverlay&
mission_of(const OverlayContext& context, const OrderOverlay& order) noexcept {
    static constexpr MissionOverlay none{};
    return context.missions != nullptr ? context.missions[order.mission] : none;
}

const WeaponDef*
weapon_of(const OverlayContext& context, const Unit& unit, std::size_t slot) noexcept {
    return world_weapon_def(context.world, unit.weapons[slot].def);
}

bool weapon_enabled(const Unit& unit, std::size_t slot) noexcept {
    return (unit.weapons[slot].flags & OA_UNIT_WEAPON_ENABLED) != 0;
}

const UnitDef* type_def(const OverlayContext& context, uint32_t type) noexcept {
    const World* world = context.world;
    return type != 0 && type < world->unit_def_count ? &world->unit_defs[type] : nullptr;
}

const Unit* unit_by_id(const OverlayContext& context, uint16_t id) noexcept {
    return id != 0 ? world_unit_at(context.world, id) : nullptr;
}

bool builds_units(const OverlayContext& context, const Unit* unit) noexcept {
    if (unit == nullptr)
        return false;
    const auto* def = world_unit_def_of(context.world, unit);
    return def != nullptr && def->build_ids != 0;
}

/// Returns where a unit shows in the frame drawn.
///
/// @param context the overlay pass
/// @param unit the unit
/// @return the sink's place, or Unit.position without one
FixedVec3 shown_place(const OverlayContext& context, const Unit& unit) {
    return context.sink.place != nullptr ? context.sink.place(context.sink.user, unit)
                                         : unit.position;
}

} // namespace

ScreenPoint overlay_project(const OverlayView& view, const FixedVec3& point) noexcept {
    if (view.project != nullptr)
        return view.project(view.projection_user, point);
    return {
        high16(point.x) - view.camera_x + kBattlefieldLeft,
        high16(point.z) - (high16(point.y) >> 1) - view.camera_y + kBattlefieldTop,
    };
}

void build_site_overlay_bounds(
    const OverlayView& view, const UnitDef& def, const FixedVec3& position, BuildSiteBounds& bounds
) noexcept {
    if (view.project != nullptr) {
        int32_t left = INT32_MAX, top = INT32_MAX, right = INT32_MIN, bottom = INT32_MIN;
        for (const auto x : {def.bounds_min_x, def.bounds_max_x})
            for (const auto z : {def.bounds_min_z, def.bounds_max_z}) {
                const auto p = overlay_project(
                    view, {position.x + x, position.y + def.bounds_min_y, position.z + z}
                );
                left = std::min(left, p.x);
                right = std::max(right, p.x);
                top = std::min(top, p.y);
                bottom = std::max(bottom, p.y);
            }
        bounds = {
            static_cast<uint32_t>(left),
            static_cast<uint32_t>(top),
            static_cast<uint32_t>(right),
            static_cast<uint32_t>(bottom)
        };
        return;
    }
    const int32_t lift = high16(position.y + def.bounds_min_y) >> 1;
    bounds.top = static_cast<uint32_t>(
        high16(position.z + def.bounds_min_z) - lift - view.camera_y + kBattlefieldTop
    );
    bounds.bottom = static_cast<uint32_t>(
        high16(position.z + def.bounds_max_z) - lift - view.camera_y + kBattlefieldTop
    );
    bounds.left = static_cast<uint32_t>(
        high16(def.bounds_min_x + position.x) - view.camera_x + kBattlefieldLeft
    );
    bounds.right = static_cast<uint32_t>(
        high16(position.x + def.bounds_max_x) - view.camera_x + kBattlefieldLeft
    );
}

void draw_build_site_overlay(
    const OverlayContext& context, const OrderOverlay& order, FixedVec3& point
) {
    const auto type = static_cast<uint16_t>(order.parameter);
    if (type == 0)
        return;
    const auto* def = type_def(context, type);
    if (def == nullptr)
        return;
    if (context.view.project != nullptr) {
        const bool selected =
            order.unit != nullptr && (order.unit->flags & OA_UNIT_FLAG_SELECTED) != 0;
        const auto color =
            ui_color(context, selected ? kUiColorSiteInnerSelected : kUiColorSiteInner);
        const FixedVec3 corners[] = {
            {order.position.x + def->bounds_min_x,
             order.position.y,
             order.position.z + def->bounds_min_z},
            {order.position.x + def->bounds_max_x,
             order.position.y,
             order.position.z + def->bounds_min_z},
            {order.position.x + def->bounds_max_x,
             order.position.y,
             order.position.z + def->bounds_max_z},
            {order.position.x + def->bounds_min_x,
             order.position.y,
             order.position.z + def->bounds_max_z}
        };
        for (int i = 0; i < 4; ++i) {
            const auto a = overlay_project(context.view, corners[i]);
            const auto b = overlay_project(context.view, corners[(i + 1) % 4]);
            line(context, a.x, a.y, b.x, b.y, color);
        }
        point = order.position;
        return;
    }
    BuildSiteBounds bounds{};
    build_site_overlay_bounds(context.view, *def, order.position, bounds);
    const auto top = bounds.top;
    const auto bottom = bounds.bottom;
    const auto left = bounds.left;
    const auto right = bounds.right;
    auto frames = order_age(context, order);
    if (frames > kSiteAnimationTicks)
        frames = kSiteAnimationTicks;
    const auto inset_x = static_cast<int32_t>((right - left) * frames) / 10;
    const auto inset_y = static_cast<int32_t>((bottom - top) * frames) / 10;
    const bool selected = order.unit != nullptr && (order.unit->flags & OA_UNIT_FLAG_SELECTED) != 0;
    const auto outer = ui_color(context, selected ? kUiColorSiteOuterSelected : kUiColorSiteOuter);
    const auto inner = ui_color(context, selected ? kUiColorSiteInnerSelected : kUiColorSiteInner);
    const auto l = static_cast<int32_t>(left);
    const auto r = static_cast<int32_t>(right);
    const auto t = static_cast<int32_t>(top);
    const auto b = static_cast<int32_t>(bottom);
    line(context, l + inset_x - 1, t - 1, l + inset_x - 1, b + 1, outer);
    line(context, r - inset_x + 1, t - 1, r - inset_x + 1, b + 1, outer);
    line(context, l - 1, t + inset_y - 1, r + 1, t + inset_y - 1, outer);
    line(context, l - 1, b - inset_y + 1, r + 1, b - inset_y + 1, outer);
    line(context, l + inset_x, t, l + inset_x, b, inner);
    line(context, r - inset_x, t, r - inset_x, b, inner);
    line(context, l, t + inset_y, r, t + inset_y, inner);
    line(context, l, b - inset_y, r, b - inset_y, inner);
    point = order.position;
}

void draw_range_circle(
    const OverlayContext& context,
    const FixedVec3& center,
    uint32_t radius,
    uint8_t color,
    const char* label,
    int32_t label_slot
) {
    if (radius == 0)
        return;
    const auto segments =
        static_cast<int32_t>(static_cast<int32_t>(radius) * kTwoPi * kSegmentsPerPixel);
    int32_t label_x = 0;
    int32_t label_y = 0;
    int32_t end_x = static_cast<int32_t>(radius);
    int32_t end_y = static_cast<int32_t>(radius);
    if (segments == 0 && !context.segmentless_circle_label)
        return;
    if (segments > 0) {
        const int32_t step = kOneUnit / segments;
        const auto magnitude = static_cast<int32_t>(radius << 16);
        const auto on_terrain = [&](int32_t angle) {
            const auto a = static_cast<uint16_t>(angle);
            FixedVec3 p{
                center.x + sim::unit_movement::sine_scaled(a, magnitude),
                center.y,
                center.z + sim::unit_movement::cosine_scaled(a, magnitude),
            };
            const int32_t floor = high16(center.y);
            const int32_t ground = context.sink.ground_height != nullptr
                                       ? context.sink.ground_height(context.sink.user, p)
                                       : 0;
            const auto height = static_cast<int16_t>(floor > ground ? floor : ground);
            p.y = static_cast<int32_t>(
                (static_cast<uint32_t>(static_cast<uint16_t>(height)) << 16) |
                (static_cast<uint32_t>(p.y) & 0xffffu)
            );
            return overlay_project(context.view, p);
        };
        int32_t angle = 0;
        for (int32_t index = 0; index <= segments; ++index) {
            const auto from = on_terrain(angle);
            angle += step;
            const auto to = on_terrain(angle);
            end_x = to.x;
            end_y = to.y;
            line(context, from.x, from.y, to.x, to.y, color);
            if (index == label_slot * 3) {
                label_x = end_x;
                label_y = end_y;
            }
        }
    }
    if (label != nullptr) {
        if (label_x == 0 && label_y == 0) {
            label_x = end_x;
            label_y = end_y;
        }
        if (context.sink.label != nullptr)
            context.sink.label(context.sink.user, label, label_x, label_y + 4);
    }
}

void draw_unit_ranges(const OverlayContext& context, const OrderOverlay& order) {
    const Unit* unit = order.unit;
    if (unit == nullptr)
        return;
    const auto* def = world_unit_def_of(context.world, unit);
    if (def == nullptr)
        return;
    const FixedVec3 center = shown_place(context, *unit);
    const auto cloak = def->min_cloak_distance;
    const auto& game = context.world->game;
    if (!context.show_ranges) {
        if (cloak != 0 && (unit->state_flags & kUnitStateCloaked) != 0)
            draw_range_circle(
                context,
                center,
                static_cast<uint32_t>(cloak),
                ui_color(context, kUiColorCloakRange),
                nullptr,
                0
            );
        if ((def->flags & OA_UNIT_DEF_FLAG_KAMIKAZE) == 0)
            return;
        const auto* blast = world_weapon_def(context.world, def->explode_as);
        if (blast == nullptr)
            return;
        const uint32_t half = static_cast<uint16_t>(blast->area_of_effect) >> 1;
        uint32_t pulse = ((game.tick % kPulseTicks) * half * 2) / kPulseTicks;
        if (pulse < kPulseMinimum)
            pulse = kPulseMinimum;
        if (half <= pulse)
            pulse = half;
        const auto color = ui_color(context, kUiColorRange);
        draw_range_circle(context, center, pulse, color, nullptr, 0);
        const auto reach = unit->movement == 0
                               ? static_cast<uint32_t>(static_cast<int32_t>(def->sight_distance))
                               : static_cast<uint16_t>(def->kamikaze_distance);
        draw_range_circle(context, center, reach, color, nullptr, 0);
        return;
    }

    struct Range {
        int32_t value;
        const char* label;
    };

    const Range ranges[] = {
        {def->sight_distance, "sight"},
        {def->radar_distance, "radar"},
        {def->sonar_distance, "sonar"},
        {def->radar_distance_jam, "radarjam"},
        {def->sonar_distance_jam, "sonarjam"},
        {static_cast<uint16_t>(def->build_distance), "build distance"},
        {static_cast<uint16_t>(def->maneuver_leash_length), "maneuver"},
    };
    const auto debug = ui_color(context, kUiColorDebugRange);
    int32_t slot = 0;
    if (cloak != 0)
        draw_range_circle(context, center, static_cast<uint32_t>(cloak), debug, "mincloak", slot++);
    for (const auto& range : ranges)
        if (range.value != 0)
            draw_range_circle(
                context, center, static_cast<uint32_t>(range.value), debug, range.label, slot++
            );
    if (def->kamikaze_distance != 0)
        draw_range_circle(
            context,
            center,
            static_cast<uint16_t>(def->kamikaze_distance),
            debug,
            "kamikazedistance",
            slot
        );
    const auto color =
        ui_color(context, (game.tick & 1u) == 0 ? kUiColorRange : kUiColorRangeBlink);
    static const char* const weapon_labels[] = {"weapon1 range", "weapon2 range", "weapon3 range"};
    for (std::size_t index = 0; index < 3; ++index) {
        // The third weapon's circle is gated on the first weapon's enable bit,
        // or on its own under third_ring_own_weapon.
        if (!weapon_enabled(*unit, index == 2 && !context.third_ring_own_weapon ? 0 : index))
            continue;
        const auto* weapon = weapon_of(context, *unit, index);
        if (weapon != nullptr && weapon->range != 0)
            draw_range_circle(
                context,
                center,
                static_cast<uint32_t>(weapon->range),
                color,
                weapon_labels[index],
                static_cast<int32_t>(index)
            );
    }
}

void draw_order_target(const OverlayContext& context, OrderOverlay& order, FixedVec3& point) {
    const Unit* unit = order.unit;
    FixedVec3 target = order.position;
    if (order.target != nullptr) {
        const Unit& victim = *order.target;
        const Player* viewer = unit != nullptr ? world_unit_owner(context.world, unit) : nullptr;
        const bool visible = context.sink.can_see != nullptr &&
                             context.sink.can_see(context.sink.user, viewer, victim);
        if (!visible && (order.flags & kOrderTargetSeen) != 0) {
            target = {
                static_cast<int32_t>(order.seen_x) << 16,
                victim.position.y,
                static_cast<int32_t>(order.seen_z) << 16
            };
        } else {
            target = shown_place(context, victim);
            order.flags |= kOrderTargetSeen;
            order.seen_x = high16(victim.position.x);
            order.seen_z = high16(victim.position.z);
        }
    }
    const auto indicator = mission_of(context, order).indicator;
    if (indicator != 0) {
        const auto& game = context.world->game;
        if (context.show_ranges && unit != nullptr && (indicator == 1 || indicator == 2)) {
            const auto color =
                ui_color(context, (game.tick & 1u) == 0 ? kUiColorRange : kUiColorRangeBlink);
            char label[64];
            for (std::size_t index = 0; index < 3; ++index) {
                if (!weapon_enabled(*unit, index))
                    continue;
                const auto* weapon = weapon_of(context, *unit, index);
                if (weapon == nullptr)
                    continue;
                if (weapon->area_of_effect != 0) {
                    std::snprintf(
                        label, sizeof label, "weapon %d - area of effect", static_cast<int>(index)
                    );
                    draw_range_circle(
                        context,
                        target,
                        static_cast<uint16_t>(weapon->area_of_effect),
                        color,
                        label,
                        0
                    );
                }
                if (weapon->coverage != 0) {
                    std::snprintf(
                        label, sizeof label, "weapon %d - coverage", static_cast<int>(index)
                    );
                    draw_range_circle(
                        context, target, static_cast<uint32_t>(weapon->coverage), color, label, 1
                    );
                }
            }
            const auto* def = world_unit_def_of(context.world, unit);
            if (def != nullptr && def->attack_run_length != 0)
                draw_range_circle(
                    context,
                    target,
                    static_cast<uint16_t>(def->attack_run_length),
                    color,
                    "attack length",
                    2
                );
        }
        if (context.indicators != nullptr && context.sink.sprite != nullptr) {
            const auto& sequence = context.indicators[indicator];
            const uint32_t period = static_cast<uint32_t>(sequence.rate) * 2u;
            if (period != 0 && sequence.frame_count != 0) {
                const auto frame =
                    static_cast<uint16_t>((game.tick / period) % sequence.frame_count);
                const auto at = overlay_project(context.view, target);
                context.sink.sprite(
                    context.sink.user, OverlaySprite::indicator, indicator, frame, at.x, at.y
                );
            }
        }
    }
    point = target;
}

void draw_order_path(
    const OverlayContext& context, OrderOverlay& order, FixedVec3& point, bool animate
) {
    const FixedVec3 from = point;
    draw_order_target(context, order, point);
    if (!animate)
        return;
    const auto age = static_cast<int32_t>(order_age(context, order));
    const int32_t delta[3] = {point.x - from.x, point.y - from.y, point.z - from.z};
    const auto length = vector_length(delta);
    if (length < kOneUnit)
        return;
    const auto& pips = context.path_pips;
    if (pips.frame_count == 0 || context.sink.sprite == nullptr)
        return;
    int32_t offset = ((age % kPipCycleTicks) * 3 << 20) / kPipCycleTicks;
    const int32_t rate = pips.rate != 0 ? pips.rate : 1;
    auto frame = static_cast<int32_t>((age / rate) % pips.frame_count);
    while (offset < length) {
        const auto fraction = static_cast<int32_t>((static_cast<int64_t>(offset) << 16) / length);
        const auto scaled = [&](int32_t v) {
            return static_cast<int32_t>((static_cast<int64_t>(v) * fraction) >> 16);
        };
        const FixedVec3 at{
            scaled(delta[0]) + from.x, scaled(delta[1]) + from.y, scaled(delta[2]) + from.z
        };
        const auto screen = overlay_project(context.view, at);
        context.sink.sprite(
            context.sink.user,
            OverlaySprite::path_pip,
            0,
            static_cast<uint16_t>(frame),
            screen.x,
            screen.y
        );
        frame = (frame + 1) % pips.frame_count;
        offset += kPipSpacing;
    }
}

void draw_target_ring(const OverlayContext& context, const OrderOverlay& order, FixedVec3& point) {
    FixedVec3 center = order.position;
    int32_t radius = kRingDefaultRadius;
    if (order.target != nullptr) {
        const auto* def = world_unit_def_of(context.world, order.target);
        radius = def != nullptr ? high16(def->size_x) : 0;
        center = shown_place(context, *order.target);
    }
    if (context.view.project != nullptr) {
        draw_range_circle(
            context,
            center,
            static_cast<uint32_t>(radius),
            ui_color(context, kUiColorRange),
            nullptr,
            0
        );
        point = center;
        return;
    }
    const auto squashed = static_cast<int32_t>(radius * kRingSquash);
    const auto origin = overlay_project(context.view, center);
    const auto color = ui_color(context, kUiColorRange);
    int32_t last_x = radius + origin.x;
    int32_t last_y = origin.y;
    for (uint32_t angle = kRingStep; angle <= 0x10000u; angle += kRingStep) {
        const auto a = static_cast<uint16_t>(angle);
        const auto x = sim::unit_movement::cosine_scaled(a, radius) + origin.x;
        const auto y = sim::unit_movement::sine_scaled(a, squashed) + origin.y;
        line(context, last_x, last_y, x, y, color);
        last_x = x;
        last_y = y;
    }
    point = center;
}

void draw_unit_order_overlays(
    const OverlayContext& context, const Unit& unit, uint32_t mask, bool animate
) {
    if (context.sink.orders == nullptr)
        return;
    bool ranges_drawn = false;
    FixedVec3 previous = shown_place(context, unit);
    FixedVec3 point = previous;
    for (OrderOverlay* node = context.sink.orders(context.sink.user, unit, false); node != nullptr;
         node = node->next) {
        auto& order = *node;
        const auto kinds = mission_of(context, order).mask & mask;
        if ((kinds & kOverlayBuildSite) != 0) {
            point = previous;
            draw_build_site_overlay(context, order, point);
        }
        if ((kinds & kOverlayPath) != 0) {
            point = previous;
            draw_order_path(context, order, point, animate);
        }
        if ((kinds & kOverlayTargetRing) != 0) {
            point = previous;
            draw_target_ring(context, order, point);
        }
        if ((kinds & kOverlayTarget) != 0) {
            point = previous;
            draw_order_target(context, order, point);
        }
        if ((kinds & kOverlayRanges) != 0 && !ranges_drawn) {
            draw_unit_ranges(context, order);
            ranges_drawn = true;
        }
        previous = point;
    }
}

int32_t stockpile_percent(const OverlayContext& context, const Unit& unit) {
    if (context.sink.orders == nullptr)
        return 0;
    for (const OrderOverlay* order = context.sink.orders(context.sink.user, unit, true);
         order != nullptr;
         order = order->next) {
        if ((order->state & kOrderStockpileBuild) == 0)
            continue;
        if (order->parameter >= OA_UNIT_WEAPON_COUNT)
            return 0;
        const auto* weapon = weapon_of(context, unit, order->parameter);
        const int32_t reload = weapon != nullptr ? static_cast<uint16_t>(weapon->reload_time) : 0;
        return reload != 0 ? order->progress * 100 / reload : 0;
    }
    return 0;
}

void draw_selection_overlays(const OverlayContext& context) {
    World* world = context.world;
    const auto& game = world->game;
    const Player* local = world_player(world, game.local_player_index);
    if (local == nullptr)
        return;
    const Unit* focus = context.view.focus_unit;
    const Unit* panel = unit_by_id(context, game.panel_unit_id);
    const Unit* cursor = unit_by_id(context, game.cursor_unit_id);
    const bool builder_in_view = builds_units(context, focus) || builds_units(context, panel) ||
                                 builds_units(context, cursor);
    uint32_t count = 0;
    const Unit* first = world_player_units(world, local, &count);
    for (uint32_t index = 0; index < count; ++index) {
        const Unit& unit = first[index];
        if (!unit_is_live_target(unit.flags))
            continue;
        bool animate = true;
        uint32_t mask = kOverlayAll;
        if (&unit == focus || unit.id == game.panel_unit_id || unit.id == game.cursor_unit_id) {
        } else if ((unit.flags & OA_UNIT_FLAG_SELECTED) != 0) {
            animate = false;
        } else if (builder_in_view) {
            mask = kOverlayBuildSite;
        } else {
            continue;
        }
        draw_unit_order_overlays(context, unit, mask, animate);
    }
}

} // namespace oa::ui::hud
