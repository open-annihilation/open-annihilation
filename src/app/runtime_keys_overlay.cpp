// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The keys overlay in a match: a panel beside the side column listing the
// keys of the contexts that are active now. It is not modal, so the match
// keeps running and input the panel does not use passes through.

#include "oa/app/runtime.hpp"

#include "oa_layer.hpp"

#include "oa/ui/key_editor/overlay.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace oa::app {
namespace {

namespace editor = oa::ui::key_editor;
namespace kit = oa::ui::kit;
namespace layout = oa::ui::display_layout;

/// A canvas pixel as a point at the dialog's scale.
///
/// @param pixel the pixel
/// @param scale the scale, in percent; 0 counts as 100
/// @return the point
int32_t pixel_to_point(int pixel, int32_t scale) {
    const int32_t used = scale <= 0 ? 100 : scale;
    return static_cast<int32_t>((static_cast<int64_t>(pixel) * 100) / used);
}

} // namespace

/// The keys overlay as a screen of the OA layer.
struct Runtime::KeysOverlayScreen final : LayerScreen {
    /// Makes the overlay for one runtime.
    ///
    /// @param runtime the runtime
    explicit KeysOverlayScreen(Runtime& runtime) : runtime_(runtime) {}

    /// Returns "keys-overlay".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return editor::name_keys_overlay; }

    /// Returns the panel, in the match's pixels, at the match dialog's scale.
    ///
    /// @param placed_on what the screen is placed on
    /// @return the panel; empty while it is not laid out in a match
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        if (!laid_ || !placed_on.match || placed_on.match_layout == nullptr || panel_.width <= 0 ||
            panel_.height <= 0)
            return {};
        if (scale_percent_ == 0) {
            return {
                {static_cast<int>(panel_.x),
                 static_cast<int>(panel_.y),
                 static_cast<int>(panel_.width),
                 static_cast<int>(panel_.height)},
                panel_.width,
                panel_.height,
                0
            };
        }
        const kit::Rect pixels = kit::to_canvas(panel_, scale_percent_);
        return {
            {static_cast<int>(pixels.x),
             static_cast<int>(pixels.y),
             static_cast<int>(pixels.width),
             static_cast<int>(pixels.height)},
            panel_.width,
            panel_.height,
            scale_percent_
        };
    }

    /// Tells that the match under the overlay still takes input.
    ///
    /// @return false
    [[nodiscard]] bool modal() const override { return false; }

    /// Tells that the battlefield under the overlay stays as it is.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Tells that the panel is blended over the battlefield.
    ///
    /// @return false
    [[nodiscard]] bool opaque() const override { return false; }

    /// Tells that the panel changes only when its revision does.
    ///
    /// @return true
    [[nodiscard]] bool revision_covers_drawing() const override { return true; }

    /// Lays the panel out for the battlefield beside the side column.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override {
        if (!placed_on.match || placed_on.match_layout == nullptr) {
            laid_ = false;
            return;
        }
        const MatchDialogPlace place = OaLayer::match_dialog(
            *placed_on.match_layout, placed_on.interface_percent, placed_on.fit_safe_area
        );
        scale_percent_ = place.scale_percent;
        const int32_t scale = place.scale_percent == 0 ? 100 : place.scale_percent;
        const layout::Rect field = runtime_.overlay_area();
        const int32_t origin_x = pixel_to_point(field.x, scale);
        const int32_t origin_y = pixel_to_point(field.y, scale);
        const int32_t far_x = pixel_to_point(field.x + field.width, scale);
        const int32_t far_y = pixel_to_point(field.y + field.height, scale);
        room_.battlefield = {
            origin_x,
            origin_y,
            std::max(far_x - origin_x, int32_t{0}),
            std::max(far_y - origin_y, int32_t{0})
        };
        room_.column_right = origin_x;
        room_.size_class = place.size_class;
        if (relayout())
            ++revision_;
    }

    /// Draws the panel from its own top left corner.
    ///
    /// @param canvas the panel's picture
    void draw(const kit::Canvas& canvas) const override {
        editor::draw_overlay(canvas, list_, {panel_.width, panel_.height});
    }

    /// Takes a pointer on the panel. A point outside it passes to the match.
    ///
    /// @param kind the pointer event
    /// @param button the button, 1 for the left
    /// @param at the pointer, in the panel's points
    /// @param finger_reach how far a finger may lie from a control
    /// @return what the overlay did
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        const bool inside = kit::contains({0, 0, panel_.width, panel_.height}, at);
        kit::ControlId id = kit::no_control;
        if (finger_reach > 0)
            id = kit::reach(list_, at, finger_reach).control;
        else if (inside)
            id = kit::hit(list_, at);
        if (!inside && id == kit::no_control)
            return LayerAnswer::pass;
        if (kind == ScreenInputKind::pointer_down && button == 1) {
            if (id == editor::id_overlay_edit) {
                if (runtime_.open_key_editor({}))
                    return LayerAnswer::close;
                return LayerAnswer::none;
            }
            if (id == editor::id_overlay_close)
                return LayerAnswer::close;
        }
        return LayerAnswer::none;
    }

    /// Closes on Escape. Every other key passes to the match.
    ///
    /// @param pressed the key
    /// @param sdl_key the key's code, unused
    /// @return close for Escape, otherwise pass
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        if (pressed == kit::Key::escape)
            return LayerAnswer::close;
        return LayerAnswer::pass;
    }

    /// Scrolls the list. A turn outside the panel passes to the match.
    ///
    /// @param at the pointer, in the panel's points
    /// @param notches how far the wheel turned; positive toward the start
    /// @return what the overlay did
    LayerAnswer wheel(kit::Point at, float notches) override {
        if (!kit::contains({0, 0, panel_.width, panel_.height}, at))
            return LayerAnswer::pass;
        const kit::ControlId id = kit::hit(list_, at);
        if (id != editor::id_overlay_list && id != editor::id_overlay_bar)
            return LayerAnswer::none;
        const auto steps = std::lround(static_cast<double>(notches) * 16.0);
        constexpr int32_t limit = 100000;
        if (steps > limit)
            scroll_ -= limit;
        else if (steps < -limit)
            scroll_ += limit;
        else
            scroll_ -= static_cast<int32_t>(steps);
        if (!relayout())
            return LayerAnswer::none;
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Returns the panel's display list.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList display_list() const override { return list_; }

    /// Returns no hover of its own. The panel draws no press.
    ///
    /// @return an empty interaction
    [[nodiscard]] kit::Interaction interaction() const override { return {}; }

    /// Closes when the match ends, and redraws when the keys change.
    ///
    /// @return close, redraw or none
    LayerAnswer tick() override {
        if (runtime_.screen_ != Screen::match || runtime_.match_ == nullptr ||
            runtime_.match_finished_)
            return LayerAnswer::close;
        if (!laid_)
            return LayerAnswer::none;
        if (!relayout())
            return LayerAnswer::none;
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Returns the revision.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// The overlay keeps nothing of its own when it closes.
    ///
    /// @param by_key unused
    void close(bool /*by_key*/) override {}

  private:

    /// Lays the groups out again. A second call with the same keys and the
    /// same scroll changes nothing.
    ///
    /// @return true when what the panel draws changed
    bool relayout() {
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (fonts == nullptr)
            return false;
        editor::OverlayInputs inputs;
        runtime_.fill_keys_overlay(inputs);
        const std::vector<editor::OverlayGroup> groups = editor::overlay_groups(inputs);
        editor::OverlayLayout laid = editor::overlay_layout(inputs, groups, room_, *fonts, scroll_);
        scroll_ = laid.scroll;
        const std::string stamp = stamp_of(inputs);
        if (laid_ && stamp == stamp_ && panel_.width == laid.panel.width &&
            panel_.height == laid.panel.height && panel_.x == laid.panel.x &&
            panel_.y == laid.panel.y && scroll_ == laid_scroll_)
            return false;
        panel_ = laid.panel;
        list_ = std::move(laid.list);
        stamp_ = stamp;
        laid_scroll_ = scroll_;
        laid_ = true;
        return true;
    }

    /// The keys, the place and the scroll, so a later tick can tell a change.
    ///
    /// @param inputs the inputs just filled
    /// @return the stamp
    std::string stamp_of(const editor::OverlayInputs& inputs) const {
        std::string stamp;
        stamp += std::to_string(inputs.contexts.active);
        stamp.push_back('|');
        stamp += std::to_string(inputs.contexts.conditions);
        stamp.push_back('|');
        stamp += runtime_.keys_overlay_signature_;
        stamp.push_back('|');
        stamp += inputs.layout_name;
        stamp.push_back('|');
        stamp += std::to_string(inputs.changes);
        stamp.push_back('|');
        for (const std::string& chip : inputs.on) {
            stamp += chip;
            stamp.push_back('\n');
        }
        stamp.push_back('|');
        stamp += std::to_string(static_cast<int>(room_.size_class));
        stamp.push_back('|');
        stamp += std::to_string(scale_percent_);
        stamp.push_back('|');
        stamp += std::to_string(room_.battlefield.width);
        stamp.push_back('x');
        stamp += std::to_string(room_.battlefield.height);
        stamp.push_back('|');
        stamp += std::to_string(scroll_);
        return stamp;
    }

    Runtime& runtime_;
    editor::OverlayRoom room_{};
    kit::Rect panel_{};
    kit::DisplayList list_{};
    int32_t scroll_{0};
    int32_t laid_scroll_{0};
    int32_t scale_percent_{100};
    std::string stamp_{};
    uint64_t revision_{1};
    bool laid_{false};
};

void Runtime::toggle_keys_overlay_screen() {
    if (screen_ != Screen::match || match_ == nullptr || match_finished_)
        return;
    if (oa_layer().modal_shown(true))
        return;
    if (LayerScreen* open = oa_layer().find(std::string(editor::name_keys_overlay))) {
        oa_layer().close(open);
        return;
    }
    if (oa_layer().screen_fonts() == nullptr)
        return;
    oa_layer().push(std::make_unique<KeysOverlayScreen>(*this));
}

} // namespace oa::app
