// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The keys overlay in a match: a panel beside the side column listing the
// keys of the contexts that are active now, or where the player put it with
// Arrange overlays. It is not modal: the match keeps running, and what the
// panel does not take goes on to the match.

#include "oa/app/runtime.hpp"

#include "oa_layer.hpp"

#include "oa/ui/key_editor/overlay.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {
namespace {

namespace editor = oa::ui::key_editor;
namespace kit = oa::ui::kit;
namespace layout = oa::ui::display_layout;

/// Points the list moves for one notch of the wheel.
constexpr int32_t kWheelStep = 24;

/// Returns a canvas pixel as a point at a scale, rounded down.
///
/// @param pixel the pixel
/// @param scale_percent the scale, in percent, 100 or more
/// @return the point
int32_t pixel_to_point(int32_t pixel, int32_t scale_percent) {
    const int64_t scaled = static_cast<int64_t>(pixel) * 100;
    return static_cast<int32_t>(
        scaled >= 0 ? scaled / scale_percent : -((-scaled + scale_percent - 1) / scale_percent)
    );
}

} // namespace

/// The keys overlay, a screen of the OA layer of its own place and size.
struct Runtime::KeysOverlayScreen final : LayerScreen {
    /// Makes the overlay for one runtime.
    ///
    /// @param runtime the runtime
    explicit KeysOverlayScreen(Runtime& runtime) : runtime_(runtime) {}

    /// Returns "keys-overlay".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return editor::name_keys_overlay; }

    /// Returns the panel's place on the match's canvas, at the class and
    /// scale the in-match dialogs take there.
    ///
    /// @param placed_on what the screen is placed on
    /// @return the panel; empty while it is not laid out over a match
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        if (!laid_ || !placed_on.match || laid_out_.panel.width <= 0 || laid_out_.panel.height <= 0)
            return {};
        const kit::Rect& panel = laid_out_.panel;
        if (scale_percent_ == 0)
            return {{panel.x, panel.y, panel.width, panel.height}, panel.width, panel.height, 0};
        const kit::Rect pixels = kit::to_canvas(panel, scale_percent_);
        return {
            {pixels.x, pixels.y, pixels.width, pixels.height},
            panel.width,
            panel.height,
            scale_percent_
        };
    }

    /// Tells that the match under the overlay still takes input.
    ///
    /// @return false
    [[nodiscard]] bool modal() const override { return false; }

    /// Tells that the battlefield under the overlay is not darkened.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Tells that the panel is laid over the battlefield by its opacity.
    ///
    /// @return false
    [[nodiscard]] bool opaque() const override { return false; }

    /// Tells that the panel changes only when its revision does.
    ///
    /// @return true
    [[nodiscard]] bool revision_covers_drawing() const override { return true; }

    /// Lays the panel out for the battlefield beside the side column, then
    /// moves it to the place the player gave it (Arrange overlays).
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override {
        if (!placed_on.match || placed_on.match_layout == nullptr) {
            laid_ = false;
            return;
        }
        const MatchDialogPlace place = OaLayer::match_dialog(
            *placed_on.match_layout,
            placed_on.interface_percent,
            placed_on.fit_safe_area,
            placed_on.window_size
        );
        scale_percent_ = place.scale_percent;
        const int32_t scale = std::max(place.scale_percent, int32_t{100});
        const layout::Rect field = runtime_.overlay_area();
        const int32_t left = pixel_to_point(field.x, scale);
        const int32_t top = pixel_to_point(field.y, scale);
        const int32_t right = pixel_to_point(field.x + field.width, scale);
        const int32_t bottom = pixel_to_point(field.y + field.height, scale);
        room_.battlefield = {
            left, top, std::max(right - left, int32_t{0}), std::max(bottom - top, int32_t{0})
        };
        // The battlefield starts where the side column ends.
        room_.column_right = left;
        room_.size_class = place.size_class;
        refresh(true);
    }

    /// Draws the panel from its own top left corner.
    ///
    /// @param canvas the panel's picture
    void draw(const kit::Canvas& canvas) const override {
        editor::OverlayLayout shown = laid_out_;
        shown.list = display_list();
        editor::draw_overlay(canvas, shown);
    }

    /// Takes the pointer over the panel; a pointer elsewhere goes on to the
    /// match. EDIT KEYS… and the close mark act on their release.
    ///
    /// @param kind the pointer event
    /// @param button the button, 1 for the left
    /// @param at the pointer, in the panel's points
    /// @param finger_reach how far a finger may lie from a control; 0 for a mouse
    /// @return what the overlay did
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        const kit::DisplayList& list = laid_out_.list;
        const bool inside =
            kit::contains({0, 0, laid_out_.panel.width, laid_out_.panel.height}, at);
        const bool held = interaction_.pressed != kit::no_control;
        if (kind == ScreenInputKind::pointer_move) {
            const kit::PointerOutcome moved = kit::pointer_move(interaction_, list, at);
            if (moved.result != kit::PointerResult::none)
                ++revision_;
            if (!inside && !held)
                return LayerAnswer::pass;
            return moved.result != kit::PointerResult::none ? LayerAnswer::redraw
                                                            : LayerAnswer::none;
        }
        if (kind == ScreenInputKind::pointer_down) {
            if (!inside && (finger_reach <= 0 ||
                            kit::reach(list, at, finger_reach).control == kit::no_control))
                return LayerAnswer::pass;
            if (button != SDL_BUTTON_LEFT)
                return LayerAnswer::none;
            const kit::PointerOutcome pressed =
                finger_reach > 0 ? kit::finger_down(interaction_, list, at, finger_reach)
                                 : kit::pointer_down(interaction_, list, at);
            if (pressed.result == kit::PointerResult::none)
                return LayerAnswer::none;
            ++revision_;
            return LayerAnswer::redraw;
        }
        if (!held)
            return inside ? LayerAnswer::none : LayerAnswer::pass;
        const kit::PointerOutcome released = kit::pointer_up(interaction_, list, at);
        ++revision_;
        if (released.result == kit::PointerResult::activated) {
            if (released.control == editor::id_overlay_close)
                return LayerAnswer::close;
            if (released.control == editor::id_overlay_edit) {
                // The editor opens over the match, which it pauses when alone.
                if (runtime_.open_key_editor({}))
                    return LayerAnswer::close;
            }
        }
        return LayerAnswer::redraw;
    }

    /// Closes on Escape. Every other key goes on to the match.
    ///
    /// @param pressed the key
    /// @return close for Escape, otherwise pass
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        if (pressed == kit::Key::escape)
            return LayerAnswer::close;
        return LayerAnswer::pass;
    }

    /// Scrolls the groups while the pointer is over the panel and they are
    /// taller than their view. Any other turn goes on to the match.
    ///
    /// @param at the pointer, in the panel's points
    /// @param notches how far the wheel turned; positive away from the player
    /// @return what the overlay did
    LayerAnswer wheel(kit::Point at, float notches) override {
        if (!kit::contains({0, 0, laid_out_.panel.width, laid_out_.panel.height}, at) ||
            laid_out_.scroll_max <= 0)
            return LayerAnswer::pass;
        const double moved = std::clamp(static_cast<double>(notches) * kWheelStep, -1.0e6, 1.0e6);
        scroll_ -= static_cast<int32_t>(std::lround(moved));
        scroll_ = std::clamp(scroll_, int32_t{0}, laid_out_.scroll_max);
        return refresh(true) ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Returns the panel's display list, its buttons showing the hover and the press.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list = laid_out_.list;
        kit::mark_states(list, interaction_);
        for (kit::Item& item : list.items) {
            if (item.control == kit::no_control || item.control != interaction_.hovered)
                continue;
            if (auto* button = std::get_if<kit::ButtonLook>(&item.look)) {
                button->hovered = true;
                button->held = interaction_.pressed == item.control;
            }
        }
        return list;
    }

    /// Returns the hover and the press.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override { return interaction_; }

    /// Closes when the match ends, and lays the panel out again when the
    /// contexts, the bindings, the latches or the layout change.
    ///
    /// @return close, redraw or none
    LayerAnswer tick() override {
        if (runtime_.screen_ != Screen::match || runtime_.match_ == nullptr ||
            runtime_.match_finished_)
            return LayerAnswer::close;
        if (!laid_)
            return LayerAnswer::none;
        return refresh(false) ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Returns the revision.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// The overlay keeps nothing of its own once it closes.
    void close(bool /*by_key*/) override {}

    /// Returns where the panel lays itself out, before the player's place
    /// for it (OaLayer::overlay_shown).
    ///
    /// @return the panel, in canvas pixels; empty before it is laid out
    [[nodiscard]] const layout::Rect& unplaced() const noexcept { return unplaced_; }

  private:

    /// Lays the groups out again when what they show has changed.
    ///
    /// @param forced lay out even when the stamp is unchanged
    /// @return true when the panel changed
    bool refresh(bool forced) {
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (fonts == nullptr)
            return false;
        editor::OverlayInputs inputs;
        const std::string stamp = runtime_.keys_overlay_stamp();
        const std::string placed =
            stamp + '|' + std::to_string(room_.battlefield.x) + ',' +
            std::to_string(room_.battlefield.y) + ',' + std::to_string(room_.battlefield.width) +
            ',' + std::to_string(room_.battlefield.height) + '|' +
            std::to_string(static_cast<int>(room_.size_class)) + '|' + std::to_string(scroll_);
        if (laid_ && !forced && placed == stamp_)
            return false;
        runtime_.fill_keys_overlay(inputs);
        const std::vector<editor::OverlayGroup> groups = editor::overlay_groups(inputs);
        editor::OverlayLayout laid = editor::overlay_layout(inputs, groups, room_, *fonts, scroll_);
        scroll_ = laid.scroll;
        // Where the panel lays itself out, then where the player put it
        // (Arrange overlays), at the in-match dialogs' scale.
        const int32_t scale = std::max(scale_percent_, int32_t{100});
        const kit::Rect unplaced =
            scale_percent_ > 0 ? kit::to_canvas(laid.panel, scale) : laid.panel;
        unplaced_ = {unplaced.x, unplaced.y, unplaced.width, unplaced.height};
        const layout::Rect shown =
            runtime_.oa_layer().overlay_shown(editor::name_keys_overlay, unplaced_);
        if (shown.x != unplaced_.x || shown.y != unplaced_.y) {
            laid.panel.x = scale_percent_ > 0 ? pixel_to_point(shown.x, scale) : shown.x;
            laid.panel.y = scale_percent_ > 0 ? pixel_to_point(shown.y, scale) : shown.y;
        }
        const bool changed = !laid_ || placed != stamp_ || laid.panel.x != laid_out_.panel.x ||
                             laid.panel.y != laid_out_.panel.y ||
                             laid.panel.width != laid_out_.panel.width ||
                             laid.panel.height != laid_out_.panel.height;
        laid_out_ = std::move(laid);
        stamp_ = placed;
        laid_ = true;
        ++revision_;
        return changed || forced;
    }

    Runtime& runtime_;
    editor::OverlayRoom room_{};       ///< the battlefield and the class
    editor::OverlayLayout laid_out_{}; ///< the panel as last laid out
    kit::Interaction interaction_{};   ///< the hover and the press
    int32_t scroll_{0};                ///< how far the groups are scrolled, in points
    int32_t scale_percent_{100};       ///< the in-match dialogs' scale; 0 fitted
    std::string stamp_{};              ///< what the panel was last laid out for
    layout::Rect unplaced_{};          ///< where the panel lays itself out, in canvas pixels
    uint64_t revision_{1};             ///< changes with what the panel draws
    bool laid_{false};                 ///< laid out over a match
};

layout::Rect OaLayer::keys_overlay_unplaced() const {
    if (screen_fonts() == nullptr)
        return {};
    const LayerView seen = view();
    if (!seen.match)
        return {};
    // The panel as it would be laid out now, whether or not it is open.
    Runtime::KeysOverlayScreen panel(runtime_);
    panel.lay_out(seen);
    return panel.unplaced();
}

void Runtime::toggle_keys_overlay_screen() {
    if (LayerScreen* open = oa_layer().find(editor::name_keys_overlay)) {
        oa_layer().close(open);
        return;
    }
    if (screen_ != Screen::match || match_ == nullptr || match_finished_)
        return;
    if (oa_layer().modal_shown(true))
        return;
    if (oa_layer().screen_fonts() == nullptr)
        return;
    oa_layer().push(std::make_unique<KeysOverlayScreen>(*this));
}

} // namespace oa::app
