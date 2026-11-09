// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Drawing the photo panel, its guides and its toast.

#include "oa/ui/photo_panel/draw.hpp"

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/photo_panel/crop.hpp"

#include <cstdint>
#include <vector>

namespace oa::ui::photo_panel {
namespace {

/// Guide ink, kept light so the battlefield still reads through it.
constexpr kit::Colour kGuide{255, 255, 255};
/// How much of each pixel a guide takes, in 256ths.
constexpr uint32_t kGuideOpacity = 170;
/// The panel, the toast, the key line and the alerts. About 95 percent, so
/// the words stay readable over light ground. The theme's ingame backdrop
/// stays as it is for the other panels.
constexpr uint32_t kBackdrop = 243;
/// The toast thumbnail's box.
constexpr int32_t kThumbW = 64;
constexpr int32_t kThumbH = 40;

/// A one-point outline blended, so a guide is not an opaque edge.
void hairline(const kit::Canvas& canvas, kit::Rect rect, kit::Colour ink, uint32_t opacity) {
    if (rect.width <= 1 || rect.height <= 1)
        return;
    kit::blend_rect(canvas, {rect.x, rect.y, rect.width, 1}, ink, opacity);
    kit::blend_rect(canvas, {rect.x, rect.y + rect.height - 1, rect.width, 1}, ink, opacity);
    kit::blend_rect(canvas, {rect.x, rect.y, 1, rect.height}, ink, opacity);
    kit::blend_rect(canvas, {rect.x + rect.width - 1, rect.y, 1, rect.height}, ink, opacity);
}

/// Thirds, the centre, the safe area, or a crop's shape, over the window.
void draw_guides(const kit::Canvas& canvas, const Model& model) {
    const int32_t width = model.width;
    const int32_t height = model.height;
    if (width <= 0 || height <= 0 || model.guide == Guide::off)
        return;
    if (model.guide == Guide::thirds) {
        const int32_t x0 = width / 3;
        const int32_t x1 = width * 2 / 3;
        const int32_t y0 = height / 3;
        const int32_t y1 = height * 2 / 3;
        kit::blend_rect(canvas, {x0, 0, 1, height}, kGuide, kGuideOpacity);
        kit::blend_rect(canvas, {x1, 0, 1, height}, kGuide, kGuideOpacity);
        kit::blend_rect(canvas, {0, y0, width, 1}, kGuide, kGuideOpacity);
        kit::blend_rect(canvas, {0, y1, width, 1}, kGuide, kGuideOpacity);
        return;
    }
    if (model.guide == Guide::centre) {
        const int32_t mid_x = width / 2;
        const int32_t mid_y = height / 2;
        kit::blend_rect(canvas, {mid_x, 0, 1, height}, kGuide, kGuideOpacity);
        kit::blend_rect(canvas, {0, mid_y, width, 1}, kGuide, kGuideOpacity);
        constexpr int32_t box = 16;
        hairline(canvas, {mid_x - box / 2, mid_y - box / 2, box, box}, kGuide, kGuideOpacity);
        return;
    }
    if (model.guide == Guide::safe) {
        const int32_t inset_x = width / 20;
        const int32_t inset_y = height / 20;
        hairline(
            canvas,
            {inset_x, inset_y, width - inset_x * 2, height - inset_y * 2},
            kGuide,
            kGuideOpacity
        );
        return;
    }
    const kit::Rect crop = crop_bounds(width, height, model.guide);
    hairline(canvas, crop, kit::colour::accent, kGuideOpacity);
}

/// The file name on two lines, the last one shortened when it still does not fit.
void draw_toast_name(const kit::Canvas& canvas, kit::Rect words, std::string_view name) {
    if (words.width <= 0 || words.height <= 0 || name.empty())
        return;
    const auto measure = [&](std::string_view part) {
        return kit::measure_text(canvas, kit::FontRole::small, part);
    };
    kit::WrapRules rules;
    rules.most_lines = 2;
    rules.shorten_last = [&](std::string_view rest) {
        return kit::fit(rest, words.width, measure);
    };
    const std::vector<std::string> lines = kit::wrap(name, words.width, measure, rules);
    const int32_t line_h = 16;
    const kit::Canvas clipped = kit::clipped(canvas, words);
    for (std::size_t index = 0; index < lines.size() && index < 2; ++index) {
        const kit::Rect line{
            words.x,
            words.y + static_cast<int32_t>(index) * line_h,
            words.width,
            line_h,
        };
        kit::box_text(
            clipped, kit::FontRole::small, lines[index], line, kit::Align::left, kit::colour::text
        );
    }
}

/// The notice after a photo, with its thumbnail when one was kept.
void draw_toast(const kit::Canvas& canvas, const Model& model, const Layout& placed) {
    if (placed.toast.width <= 0 || placed.toast.height <= 0)
        return;
    kit::blend_rect(canvas, placed.toast, kit::colour::panel, kBackdrop);
    kit::outline_rect(canvas, placed.toast, kit::colour::strong_edge);
    int32_t text_x = placed.toast.x + 8;
    if (model.toast_columns > 0 && model.toast_rows > 0 &&
        model.toast_rgb.size() == static_cast<std::size_t>(model.toast_columns) *
                                      static_cast<std::size_t>(model.toast_rows) * 3U) {
        const kit::Rect thumb{
            placed.toast.x + 6,
            placed.toast.y + (placed.toast.height - kThumbH) / 2,
            kThumbW,
            kThumbH,
        };
        std::vector<uint8_t> rgba(
            static_cast<std::size_t>(model.toast_columns) *
            static_cast<std::size_t>(model.toast_rows) * 4U
        );
        for (int32_t pixel = 0; pixel < model.toast_columns * model.toast_rows; ++pixel) {
            rgba[static_cast<std::size_t>(pixel) * 4U] =
                model.toast_rgb[static_cast<std::size_t>(pixel) * 3U];
            rgba[static_cast<std::size_t>(pixel) * 4U + 1U] =
                model.toast_rgb[static_cast<std::size_t>(pixel) * 3U + 1U];
            rgba[static_cast<std::size_t>(pixel) * 4U + 2U] =
                model.toast_rgb[static_cast<std::size_t>(pixel) * 3U + 2U];
            rgba[static_cast<std::size_t>(pixel) * 4U + 3U] = 255;
        }
        oa::ui::frontend_renderer::RgbaPicture picture;
        picture.width = static_cast<uint32_t>(model.toast_columns);
        picture.height = static_cast<uint32_t>(model.toast_rows);
        picture.pixels = rgba;
        kit::draw_picture(canvas, thumb, picture);
        text_x = thumb.x + thumb.width + 8;
    }
    const int32_t text_w = std::max(placed.toast.x + placed.toast.width - text_x - 8, int32_t{0});
    const kit::Rect words{text_x, placed.toast.y + 6, text_w, 32};
    // The file name stays inside the toast. The game font would otherwise
    // keep painting across the battlefield.
    draw_toast_name(canvas, words, model.toast_name);
    if (!model.toast_note.empty()) {
        const kit::Rect note{words.x, words.y + 32, words.width, 18};
        kit::box_text(
            kit::clipped(canvas, note),
            kit::FontRole::small,
            model.toast_note,
            note,
            kit::Align::left,
            kit::colour::accent
        );
    }
}

} // namespace

void draw_panel(
    const kit::Canvas& canvas,
    const Model& model,
    const Layout& placed,
    const kit::DisplayList& list
) {
    draw_guides(canvas, model);
    int chip_index = 0;
    bool first_chip = true;
    for (const std::string& text : model.chips) {
        if (text.empty())
            continue;
        if (chip_index >= static_cast<int>(placed.chip_box.size()))
            break;
        const kit::Rect chip = placed.chip_box[static_cast<std::size_t>(chip_index)];
        ++chip_index;
        if (chip.width <= 0 || chip.height <= 0)
            continue;
        const bool amber = first_chip && model.live_amber;
        first_chip = false;
        const kit::Colour face = amber ? kit::colour::lock : kit::colour::chip_face;
        kit::blend_rect(canvas, chip, face, 230);
        kit::outline_rect(canvas, chip, kit::colour::chip_border);
        const kit::Colour ink = amber ? kit::colour::band : kit::colour::text;
        kit::box_text(canvas, kit::FontRole::small, text, chip, kit::Align::centre, ink);
    }
    if (placed.alerts.width > 0 && !model.alerts.empty()) {
        kit::blend_rect(canvas, placed.alerts, kit::colour::panel, kBackdrop);
        kit::box_text(
            kit::clipped(canvas, placed.alerts),
            kit::FontRole::small,
            model.alerts,
            placed.alerts,
            kit::Align::left,
            kit::colour::text
        );
    }
    if (placed.keyline.width > 0 && !model.key_line.empty()) {
        kit::blend_rect(canvas, placed.keyline, kit::colour::panel, kBackdrop);
        kit::box_text(
            kit::clipped(canvas, placed.keyline),
            kit::FontRole::small,
            model.key_line,
            placed.keyline,
            kit::Align::left,
            kit::colour::text
        );
    }
    if (placed.panel.width > 0 && placed.panel.height > 0) {
        kit::blend_rect(canvas, placed.panel, kit::colour::panel, kBackdrop);
        kit::outline_rect(canvas, placed.panel, kit::colour::strong_edge);
    }
    // A centred caption wider than its row, such as the file name, would
    // paint past the panel. The clip keeps every control inside it.
    if (placed.panel.width > 0 && placed.panel.height > 0)
        kit::paint(kit::clipped(canvas, placed.panel), list);
    else
        kit::paint(canvas, list);
    draw_toast(canvas, model, placed);
}

kit::DisplayList shown_list(const Layout& placed, const kit::Interaction& interaction) {
    kit::DisplayList shown = placed.list;
    if (!kit::focus_ring_visible(interaction))
        return shown;
    const kit::Control* control = kit::control_of(shown, interaction.focused);
    if (control == nullptr)
        return shown;
    kit::add_focus_ring(shown, control->rect, true);
    return shown;
}

} // namespace oa::ui::photo_panel
