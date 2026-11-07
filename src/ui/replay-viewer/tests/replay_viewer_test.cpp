// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The playback bar: its layout at 640x480, 1280x720 and 1920x1080, marker hit
// areas, the time text, the hide and pin timing, and the fill of every button
// and card.

#include "oa/platform/text_font.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/paint/painter.hpp"
#include "oa/ui/replay_viewer/replay_viewer.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace viewer = oa::ui::replay_viewer;
namespace kit = oa::ui::kit;
namespace paint = oa::ui::paint;

/// Tells whether two rectangles share any point.
///
/// @param left one rectangle
/// @param right the other
/// @return true when they share a point
bool overlaps(const kit::Rect& left, const kit::Rect& right) {
    const kit::Rect shared = kit::intersect(left, right);
    return shared.width > 0 && shared.height > 0;
}

/// The control row, left to right. The timeline is its own row, above these.
///
/// @param layout the layout
/// @return the rectangles
std::vector<kit::Rect> row_of(const viewer::Layout& layout) {
    return {
        layout.back,
        layout.play,
        layout.forward,
        layout.time,
        layout.speed,
        layout.view,
        layout.camera,
        layout.chat,
        layout.keys,
        layout.leave,
    };
}

/// Checks one window's layout.
///
/// @param width the window's width
/// @param height its height
void check_size(int32_t width, int32_t height) {
    viewer::Model model;
    model.tick = 21000;
    model.length = 42960;
    const viewer::Layout layout = viewer::lay_out(width, height, model, true);
    OA_CHECK(layout.bar.x == 0);
    OA_CHECK(layout.bar.width == width);
    OA_CHECK(layout.bar.y + layout.bar.height == height);
    OA_CHECK(layout.bar.height > 0);
    OA_CHECK(layout.timeline.width >= width - 24);
    OA_CHECK(layout.timeline.y + layout.timeline.height <= layout.back.y);
    OA_CHECK(kit::wholly_in(layout.timeline, layout.bar));
    OA_CHECK(layout.view.width > 44);
    OA_CHECK(layout.camera.width > 44);
    const std::vector<kit::Rect> row = row_of(layout);
    for (std::size_t index = 0; index < row.size(); ++index) {
        OA_CHECK(row[index].width > 0);
        OA_CHECK(row[index].height > 0);
        OA_CHECK(kit::wholly_in(row[index], layout.bar));
        if (index > 0)
            OA_CHECK(row[index].x >= row[index - 1].x + row[index - 1].width);
    }
    for (std::size_t left = 0; left < row.size(); ++left)
        for (std::size_t right = left + 1; right < row.size(); ++right)
            OA_CHECK(!overlaps(row[left], row[right]));
    kit::DisplayList list;
    viewer::fill_controls(layout, model, list);
    bool saw_view = false;
    bool saw_back = false;
    bool saw_play = false;
    bool saw_forward = false;
    bool saw_timeline = false;
    bool saw_speed = false;
    bool saw_chat = false;
    bool saw_keys = false;
    bool saw_leave = false;
    for (const kit::Control& control : list.controls) {
        if (control.name == "replay-viewer.view" || control.name == "replay-viewer.camera")
            saw_view = true;
        saw_back = saw_back || control.name == "replay-viewer.back";
        saw_play = saw_play || control.name == "replay-viewer.play";
        saw_forward = saw_forward || control.name == "replay-viewer.forward";
        saw_timeline = saw_timeline || control.name == "replay-viewer.timeline";
        saw_speed = saw_speed || control.name == "replay-viewer.speed";
        saw_chat = saw_chat || control.name == "replay-viewer.chat";
        saw_keys = saw_keys || control.name == "replay-viewer.keys";
        saw_leave = saw_leave || control.name == "replay-viewer.leave";
    }
    OA_CHECK(!saw_view);
    OA_CHECK(saw_back && saw_play && saw_forward && saw_timeline);
    OA_CHECK(saw_speed && saw_chat && saw_keys && saw_leave);

    viewer::Model ended_model = model;
    ended_model.end_card = true;
    const viewer::Layout ended = viewer::lay_out(width, height, ended_model, true);
    constexpr int32_t kMargin = 8;
    OA_CHECK(ended.end.y >= kMargin);
    OA_CHECK(ended.end.y + ended.end.height + kMargin <= ended.bar.y);
    OA_CHECK(ended.end.height > 0);
    OA_CHECK(kit::wholly_in(ended.watch_again, ended.end));
    OA_CHECK(kit::wholly_in(ended.library, ended.end));
    OA_CHECK(kit::wholly_in(ended.export_button, ended.end));
    OA_CHECK(ended.view.width == layout.view.width);
    OA_CHECK(ended.camera.width == layout.camera.width);
}

/// The time and the speed button hold their text in every language, on the
/// battlefield the bar covers in a match: 512 by 416 points at 640x480, and
/// 725 by 416 and 832 by 476 at 1280x720 and 1920x1080, where the chrome is
/// drawn at 150 % and 200 %. Where the row has no room for the full caption,
/// the button takes a shorter form, and that form fits. The view and camera
/// slots never give up width.
///
/// @param width the battlefield's width, in points
/// @param height its height
/// @param full_expected the full caption fits a 45-minute game here
void check_speed_fit(int32_t width, int32_t height, bool full_expected) {
    const auto stack =
        oa::platform::text_font::FontStack::open(oa::platform::text_font::bundled_font_directory());
    OA_CHECK(stack != nullptr);
    if (stack == nullptr)
        return;
    kit::Fonts fonts;
    fonts.modern = stack.get();
    // The match's classic look draws the small font, and a character it lacks
    // (the multiply sign) in DejaVu at that font's 11px. Barlow at 12 is the
    // modern look. The button has to hold whichever is wider.
    const auto sans_width = [&](const std::string& caption) {
        oa::platform::text_font::Style style;
        style.pixel_size = 11;
        style.weight = oa::platform::text_font::Weight::regular;
        style.family = oa::platform::text_font::Family::sans;
        style.rendering = oa::platform::text_font::Rendering::antialiased;
        const auto placed = stack->layout(caption, style);
        if (!placed || placed->empty())
            return int32_t{0};
        return placed->back().pen + placed->back().advance;
    };
    const auto measure = [&](const std::string& text) {
        return std::max(kit::text_width(fonts, kit::FontRole::small, text), sans_width(text));
    };
    const auto widest = [&](const std::vector<std::string>& texts) {
        int32_t most = 0;
        for (const std::string& text : texts)
            most = std::max(most, measure(text));
        return most;
    };

    // The word each language puts beside the reached speed, and its Skip.
    struct Language {
        std::string before; ///< the text between the asked and the reached speed
        std::string after;  ///< the text after the reached speed
        std::string skip;   ///< the catch-up's caption
    };

    const Language languages[] = {
        {" · ", " now", "Skip"},
        {" · jetzt ", "", "Überspringen"},
        {" · ", " maintenant", "Passer"},
        {" · ", " ahora", "Omitir"},
        {" · ", " ora", "Salta"},
    };
    const std::string reached_words[] = {"9×", "0.6×", "15×"};
    // A 45-minute game and a four-and-a-half-hour one.
    for (const uint32_t length : {uint32_t{81556}, uint32_t{469856}}) {
        std::vector<std::string> times;
        for (char digit = '0'; digit <= '9'; ++digit) {
            std::string sample = viewer::time_text(length, length);
            for (char& letter : sample) {
                if (letter >= '0' && letter <= '9')
                    letter = digit;
            }
            times.push_back(std::move(sample));
        }
        for (const Language& language : languages) {
            std::vector<std::string> full;
            std::vector<std::string> brief;
            for (const std::string& reached : reached_words) {
                full.push_back("16×" + language.before + reached + language.after);
                brief.push_back("16× · " + reached);
            }
            viewer::Model model;
            model.tick = length / 2;
            model.length = length;
            const viewer::Layout unmeasured = viewer::lay_out(width, height, model, true);
            model.widths.time = widest(times);
            model.widths.speed_full = widest(full);
            model.widths.speed_brief = widest(brief);
            model.widths.speed_asked = widest({"16×", "½×", "8×"});
            model.widths.skip = measure(language.skip);
            const viewer::Layout layout = viewer::lay_out(width, height, model, true);
            OA_CHECK(layout.time.width >= model.widths.time);
            int32_t shown = model.widths.speed_asked;
            if (layout.speed_form == viewer::SpeedForm::full)
                shown = std::max(model.widths.speed_full, model.widths.skip);
            else if (layout.speed_form == viewer::SpeedForm::brief)
                shown = model.widths.speed_brief;
            OA_CHECK(layout.speed.width >= shown);
            if (full_expected && length < 100000)
                OA_CHECK(layout.speed_form == viewer::SpeedForm::full);
            OA_CHECK(layout.view.width == unmeasured.view.width);
            OA_CHECK(layout.camera.width == unmeasured.camera.width);
            const std::vector<kit::Rect> row = row_of(layout);
            for (const kit::Rect& rect : row)
                OA_CHECK(kit::wholly_in(rect, layout.bar));
            for (std::size_t left = 0; left < row.size(); ++left) {
                for (std::size_t right = left + 1; right < row.size(); ++right)
                    OA_CHECK(!overlaps(row[left], row[right]));
            }
        }
    }
}

/// The caption each form of the speed button draws.
void check_speed_forms() {
    viewer::Model model;
    model.asked_milli = 16000;
    model.reached_milli = 9000;
    model.reached_lower = true;
    OA_CHECK(viewer::fitted_speed_caption(model, viewer::SpeedForm::full) == "16× · 9× now");
    OA_CHECK(viewer::fitted_speed_caption(model, viewer::SpeedForm::brief) == "16× · 9×");
    OA_CHECK(viewer::fitted_speed_caption(model, viewer::SpeedForm::asked) == "16×");
    model.reached_milli = 16000;
    OA_CHECK(viewer::fitted_speed_caption(model, viewer::SpeedForm::brief) == "16×");
    model.skipping = true;
    OA_CHECK(viewer::fitted_speed_caption(model, viewer::SpeedForm::full) == "Skip");
    OA_CHECK(viewer::fitted_speed_caption(model, viewer::SpeedForm::brief) == "16×");
    OA_CHECK(viewer::fitted_speed_caption(model, viewer::SpeedForm::asked) == "16×");
}

/// The speed word, the behind caption, the pass's percent and a player's colour.
void check_words() {
    OA_CHECK(viewer::speed_word(500) == "½×");
    OA_CHECK(viewer::speed_word(600) == "0.6×");
    OA_CHECK(viewer::speed_word(1000) == "1×");
    OA_CHECK(viewer::speed_word(1400) == "1.4×");
    OA_CHECK(viewer::speed_word(400) == "0.4×");
    OA_CHECK(viewer::speed_word(9000) == "9×");
    OA_CHECK(viewer::speed_word(16000) == "16×");
    OA_CHECK(viewer::speed_caption(1000, 1000, true, false) == "1×");
    OA_CHECK(viewer::speed_caption(16000, 16000, true, false) == "16×");
    OA_CHECK(viewer::speed_caption(16000, 9000, true, false) == "16× · 9× now");
    OA_CHECK(viewer::speed_caption(16000, 600, true, false) == "16× · 0.6× now");
    OA_CHECK(viewer::speed_caption(1000, 600, false, false) == "1×");
    const std::string finding = viewer::finding_caption(40);
    OA_CHECK(finding.find("40") != std::string::npos);
    OA_CHECK(finding.find('%') != std::string::npos);
    OA_CHECK(finding.find("90") == std::string::npos);
    OA_CHECK(viewer::player_colour(0, 2, 3, 4) == 0);
    OA_CHECK(viewer::player_colour(-1, 2, 3, 4) == 2);
    OA_CHECK(viewer::player_colour(-1, -1, 3, 4) == 3);
    OA_CHECK(viewer::player_colour(-1, -1, -1, 2) == 2);
    OA_CHECK(viewer::player_colour(-1, -1, -1, 12) == 2);
    OA_CHECK(viewer::player_colour(-1, -1, -1, -1) == -1);
}

/// The centre of a rectangle.
///
/// @param rect the rectangle
/// @return its centre
kit::Point centre(const kit::Rect& rect) {
    return {rect.x + rect.width / 2, rect.y + rect.height / 2};
}

/// Tells whether a pixel is one colour.
///
/// @param pixels the canvas
/// @param x the column
/// @param y the row
/// @param ink the colour, opacity included
/// @return true when the pixel is that colour
bool pixel_is(const paint::Canvas& pixels, int x, int y, kit::Colour ink) {
    const paint::Rgba pixel = paint::pixel_at(pixels, x, y);
    return pixel.r == ink.r && pixel.g == ink.g && pixel.b == ink.b && pixel.a == ink.a;
}

/// Requires a button's interior, clear of its mark, to be one fill.
///
/// @param pixels the canvas
/// @param rect the button
/// @param ink the fill
void expect_button(const paint::Canvas& pixels, const kit::Rect& rect, kit::Colour ink) {
    constexpr int32_t kInset = 5;
    OA_CHECK(rect.width > kInset * 2);
    OA_CHECK(rect.height > kInset * 2);
    const kit::Point samples[] = {
        {rect.x + kInset, rect.y + kInset},
        {rect.x + rect.width - 1 - kInset, rect.y + kInset},
        {rect.x + kInset, rect.y + rect.height - 1 - kInset},
        {rect.x + rect.width - 1 - kInset, rect.y + rect.height - 1 - kInset},
        {rect.x + rect.width / 2, rect.y + kInset},
        {rect.x + rect.width / 2, rect.y + rect.height - 1 - kInset},
        {rect.x + kInset, rect.y + rect.height / 2},
        {rect.x + rect.width - 1 - kInset, rect.y + rect.height / 2},
    };
    for (const kit::Point& at : samples)
        OA_CHECK(pixel_is(pixels, at.x, at.y, ink));
}

/// Requires a card's upper interior to be its fill. The buttons sit along the foot.
///
/// @param pixels the canvas
/// @param rect the card
void expect_card(const paint::Canvas& pixels, const kit::Rect& rect) {
    constexpr int32_t kInset = 8;
    OA_CHECK(rect.width > kInset * 2);
    OA_CHECK(rect.height > kInset * 2);
    const int32_t upper = rect.y + rect.height / 3;
    const kit::Point samples[] = {
        {rect.x + kInset, rect.y + kInset},
        {rect.x + rect.width - 1 - kInset, rect.y + kInset},
        {rect.x + rect.width / 2, rect.y + kInset},
        {rect.x + kInset, upper},
        {rect.x + rect.width - 1 - kInset, upper},
        {rect.x + rect.width / 2, upper},
    };
    for (const kit::Point& at : samples)
        OA_CHECK(pixel_is(pixels, at.x, at.y, viewer::card_fill));
}

/// Draws the bar and both cards at one size and checks each fill. The
/// shortcut card opens over the end card, so the end card is drawn on its own
/// first, then with the shortcut card on top.
///
/// @param width the canvas width
/// @param height the canvas height
void check_draw(int width, int height) {
    viewer::Model model;
    model.tick = 21000;
    model.length = 42960;
    model.paused = false;
    model.chat_shown = false;
    model.end_card = true;
    model.winner = "Arm";
    const auto draw_into =
        [](paint::Canvas& pixels, const viewer::Model& shown_model, const viewer::Layout& layout) {
            paint::Painter painter(pixels);
            kit::Canvas canvas;
            canvas.smooth.painter = &painter;
            canvas.smooth.scale_percent = 100;
            viewer::draw(canvas, layout, shown_model);
        };
    kit::Colour face = kit::hud_colour::panel;
    face.a = 255;
    {
        const viewer::Layout layout = viewer::lay_out(width, height, model, true);
        paint::Canvas pixels = paint::make_canvas(width, height);
        draw_into(pixels, model, layout);
        expect_button(pixels, layout.back, face);
        expect_button(pixels, layout.play, kit::hud_colour::lit);
        expect_button(pixels, layout.forward, face);
        expect_button(pixels, layout.speed, face);
        expect_button(pixels, layout.chat, face);
        expect_button(pixels, layout.keys, face);
        expect_button(pixels, layout.leave, face);
        expect_button(pixels, layout.watch_again, face);
        expect_button(pixels, layout.library, face);
        expect_button(pixels, layout.export_button, face);
        expect_card(pixels, layout.end);
    }
    model.keys_open = true;
    const viewer::Layout layout = viewer::lay_out(width, height, model, true);
    paint::Canvas pixels = paint::make_canvas(width, height);
    draw_into(pixels, model, layout);
    expect_button(pixels, layout.keys, kit::hud_colour::lit);
    expect_card(pixels, layout.keys_card);
    // The shortcut card is on top, and a press on it is the card's.
    OA_CHECK(viewer::hit_control(layout, centre(layout.keys_card)) == viewer::Control::keys_card);
}

/// The buttons a player hears are named from the catalogue.
void check_names() {
    viewer::Model model;
    model.paused = true;
    model.length = 100;
    const viewer::Layout layout = viewer::lay_out(1280, 720, model, true);
    const auto caption_of = [](const kit::DisplayList& list, std::string_view name) {
        for (const kit::Control& control : list.controls)
            if (control.name == name)
                return control.text == control.label ? control.text : std::string{};
        return std::string{};
    };
    kit::DisplayList list;
    viewer::fill_controls(layout, model, list);
    OA_CHECK(caption_of(list, "replay-viewer.back") == "Back 10 seconds");
    OA_CHECK(caption_of(list, "replay-viewer.play") == "Play");
    OA_CHECK(caption_of(list, "replay-viewer.forward") == "Forward 10 seconds");
    model.paused = false;
    viewer::fill_controls(layout, model, list);
    OA_CHECK(caption_of(list, "replay-viewer.play") == "Pause");
}

/// A chat marker's hover names the player once.
void check_chat_caption() {
    viewer::Marker marker;
    marker.kind = viewer::MarkerKind::chat;
    marker.tick = 30;
    marker.name = "Arm";
    marker.line = "<Arm> hello";
    OA_CHECK(viewer::marker_caption(marker) == "0:01 · Arm: hello");
}

/// Export outside the Library draws, and a press there does nothing.
void check_export_disabled() {
    viewer::Model model;
    model.end_card = true;
    model.can_export = false;
    model.length = 100;
    const viewer::Layout layout = viewer::lay_out(1280, 720, model, true);
    OA_CHECK(!layout.export_enabled);
    OA_CHECK(viewer::hit_control(layout, centre(layout.export_button)) == viewer::Control::none);
    kit::DisplayList list;
    viewer::fill_controls(layout, model, list);
    bool saw = false;
    for (const kit::Control& control : list.controls) {
        if (control.name != "replay-viewer.export")
            continue;
        saw = true;
        OA_CHECK(!control.enabled);
    }
    OA_CHECK(saw);
}

} // namespace

int main() {
    check_size(640, 480);
    check_size(1280, 720);
    check_size(1920, 1080);
    check_speed_fit(512, 416, false);
    check_speed_fit(725, 416, true);
    check_speed_fit(832, 476, true);
    check_speed_forms();
    check_words();
    check_draw(640, 480);
    check_draw(1280, 720);
    check_draw(1920, 1080);
    check_names();
    check_chat_caption();
    check_export_disabled();

    OA_CHECK(viewer::time_text(21000, 42960) == "11:40 / 23:52");
    OA_CHECK(viewer::time_text(0, 30) == "0:00 / 0:01");
    OA_CHECK(viewer::clock_text(0) == "0:00");

    viewer::Model model;
    model.length = 30000;
    model.tick = 3000;
    viewer::Marker early;
    early.kind = viewer::MarkerKind::commander_death;
    early.tick = 3000;
    early.name = "Arm";
    viewer::Marker late;
    late.kind = viewer::MarkerKind::chat;
    late.tick = 24000;
    late.name = "Core";
    late.line = "hello";
    model.markers.push_back(early);
    model.markers.push_back(late);
    const viewer::Layout layout = viewer::lay_out(1280, 720, model, true);
    OA_CHECK(layout.marker_hits.size() == 2);
    OA_CHECK(viewer::marker_index(layout, centre(layout.marker_hits[0])) == 0);
    OA_CHECK(viewer::marker_index(layout, centre(layout.marker_hits[1])) == 1);
    const kit::Point away{centre(layout.marker_hits[0]).x - 24, centre(layout.marker_hits[0]).y};
    OA_CHECK(viewer::marker_index(layout, away) == -1);
    OA_CHECK(viewer::hit_control(layout, centre(layout.play)) == viewer::Control::play);
    OA_CHECK(viewer::hit_control(layout, centre(layout.view)) == viewer::Control::none);
    OA_CHECK(viewer::hit_control(layout, centre(layout.camera)) == viewer::Control::none);
    OA_CHECK(viewer::tick_at(layout.timeline, layout.timeline.x, model.length) == 0);
    OA_CHECK(
        viewer::tick_at(
            layout.timeline, layout.timeline.x + layout.timeline.width - 1, model.length
        ) == model.length
    );

    const viewer::Layout hidden = viewer::lay_out(1280, 720, model, false);
    OA_CHECK(viewer::hit_control(hidden, centre(hidden.play)) == viewer::Control::none);
    kit::DisplayList hidden_list;
    viewer::fill_controls(hidden, model, hidden_list);
    OA_CHECK(hidden_list.controls.empty());

    viewer::BarVisibility bar;
    OA_CHECK(bar.shown());
    OA_CHECK(!bar.pinned());
    const int32_t top = layout.bar.y;
    bar.advance(2999, 0, 720, top);
    OA_CHECK(bar.shown());
    bar.advance(1, 0, 720, top);
    OA_CHECK(!bar.shown());
    bar.show();
    OA_CHECK(bar.shown());
    bar.advance(3000, 0, 720, top);
    OA_CHECK(!bar.shown());
    bar.show();
    bar.toggle_pin();
    OA_CHECK(bar.pinned());
    bar.advance(10000, 0, 720, top);
    OA_CHECK(bar.shown());
    bar.toggle_pin();
    OA_CHECK(!bar.pinned());
    bar.advance(2999, 0, 720, top);
    OA_CHECK(bar.shown());
    bar.advance(1, 0, 720, top);
    OA_CHECK(!bar.shown());
    bar.advance(0, 700, 720, top);
    OA_CHECK(bar.shown());

    return oa::test::check_exit_status();
}
