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
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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
        layout.photo,
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
    bool saw_camera = false;
    bool saw_photo = false;
    bool saw_back = false;
    bool saw_play = false;
    bool saw_forward = false;
    bool saw_timeline = false;
    bool saw_speed = false;
    bool saw_chat = false;
    bool saw_keys = false;
    bool saw_leave = false;
    for (const kit::Control& control : list.controls) {
        saw_view = saw_view || control.name == "replay-viewer.view";
        saw_camera = saw_camera || control.name == "replay-viewer.camera";
        saw_photo = saw_photo || control.name == "replay-viewer.photo";
        saw_back = saw_back || control.name == "replay-viewer.back";
        saw_play = saw_play || control.name == "replay-viewer.play";
        saw_forward = saw_forward || control.name == "replay-viewer.forward";
        saw_timeline = saw_timeline || control.name == "replay-viewer.timeline";
        saw_speed = saw_speed || control.name == "replay-viewer.speed";
        saw_chat = saw_chat || control.name == "replay-viewer.chat";
        saw_keys = saw_keys || control.name == "replay-viewer.keys";
        saw_leave = saw_leave || control.name == "replay-viewer.leave";
    }
    OA_CHECK(saw_view && saw_camera && saw_photo);
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
        expect_button(pixels, layout.photo, face);
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
    OA_CHECK(caption_of(list, "replay-viewer.photo") == "Photo");
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

/// A view caption wider than the row can spare does not take the slot past
/// the room left once the time has kept its text. On the 512-point
/// battlefield that room is short of a 400-point caption.
void check_wide_view_caption() {
    const auto stack =
        oa::platform::text_font::FontStack::open(oa::platform::text_font::bundled_font_directory());
    OA_CHECK(stack != nullptr);
    if (stack == nullptr)
        return;
    kit::Fonts fonts;
    fonts.modern = stack.get();
    const auto measure = [&](const std::string& text) {
        return kit::text_width(fonts, kit::FontRole::small, text);
    };
    viewer::Model bare;
    bare.length = 81556;
    bare.tick = 1000;
    const viewer::Layout unmeasured = viewer::lay_out(512, 416, bare, true);
    OA_CHECK(unmeasured.view.width == 96);
    OA_CHECK(unmeasured.camera.width == 64);
    viewer::Model model = bare;
    model.widths.time = measure("45:18 / 45:18");
    model.widths.speed_full = measure("16× · 9× now");
    model.widths.speed_brief = measure("16× · 9×");
    model.widths.speed_asked = measure("16×");
    model.widths.view = 400;
    const viewer::Layout layout = viewer::lay_out(512, 416, model, true);
    OA_CHECK(layout.view.width < model.widths.view);
    OA_CHECK(layout.view.width >= unmeasured.view.width);
    OA_CHECK(layout.camera.width == unmeasured.camera.width);
    OA_CHECK(layout.time.width >= model.widths.time);
    for (const kit::Rect& rect : row_of(layout))
        OA_CHECK(kit::wholly_in(rect, layout.bar));
}

/// The real view and camera captions, in the five built-in languages, at the
/// battlefield widths a match uses. A 20-character name is one of the view
/// captions. The slot grows into spare room and never past the caption, and
/// the time of a 45-minute game and a four-and-a-half-hour game keeps its text.
void check_caption_fit() {
    const auto stack =
        oa::platform::text_font::FontStack::open(oa::platform::text_font::bundled_font_directory());
    OA_CHECK(stack != nullptr);
    if (stack == nullptr)
        return;
    kit::Fonts fonts;
    fonts.modern = stack.get();
    const auto measure = [&](const std::string& text) {
        return kit::text_width(fonts, kit::FontRole::small, text);
    };
    const std::string views[] = {
        "All players",
        "Alle Spieler",
        "Tous les joueurs",
        "Todos los jugadores",
        "Tutti i giocatori",
        std::string(20, 'M'),
    };
    const std::string cameras[] = {
        "Free",
        "Frei",
        "Libre",
        "Libero",
        "Follow their camera",
        "Ihrer Kamera folgen",
        "Suivre sa caméra",
        "Seguir su cámara",
        "Segui la sua telecamera",
        "Follow a unit",
        "Einer Einheit folgen",
        "Suivre une unité",
        "Seguir una unidad",
        "Segui un'unità",
    };
    const int32_t sizes[][2] = {{512, 416}, {725, 416}, {832, 476}};
    const auto fits = [&](const std::string& view_caption,
                          const std::string& camera_caption,
                          int32_t width,
                          int32_t height,
                          uint32_t length) {
        viewer::Model bare;
        bare.length = length;
        bare.tick = length / 2;
        const viewer::Layout unmeasured = viewer::lay_out(width, height, bare, true);
        viewer::Model model = bare;
        const std::string time = viewer::time_text(length, length);
        model.widths.time = measure(time);
        model.widths.view = measure(view_caption) + viewer::menu_mark_points;
        model.widths.camera = measure(camera_caption) + viewer::menu_mark_points;
        const viewer::Layout layout = viewer::lay_out(width, height, model, true);
        const int32_t view_cap = std::max(unmeasured.view.width, model.widths.view + 4);
        const int32_t camera_cap = std::max(unmeasured.camera.width, model.widths.camera + 4);
        OA_CHECK(layout.view.width >= unmeasured.view.width);
        OA_CHECK(layout.view.width <= view_cap);
        OA_CHECK(layout.camera.width >= unmeasured.camera.width);
        OA_CHECK(layout.camera.width <= camera_cap);
        OA_CHECK(layout.time.width >= model.widths.time);
        const std::vector<kit::Rect> row = row_of(layout);
        for (const kit::Rect& rect : row)
            OA_CHECK(kit::wholly_in(rect, layout.bar));
        for (std::size_t left = 0; left < row.size(); ++left)
            for (std::size_t right = left + 1; right < row.size(); ++right)
                OA_CHECK(!overlaps(row[left], row[right]));
    };
    for (const int32_t (&size)[2] : sizes) {
        for (const uint32_t length : {uint32_t{81556}, uint32_t{469856}}) {
            for (const std::string& view_caption : views)
                fits(view_caption, "Free", size[0], size[1], length);
            for (const std::string& camera_caption : cameras)
                fits("All players", camera_caption, size[0], size[1], length);
        }
    }
}

/// The end card stays clear of the player list, and its buttons stay inside
/// the card, at the battlefield sizes and at the three windows.
void check_end_card_clears_the_list() {
    const int32_t sizes[][2] = {
        {512, 416}, {725, 416}, {832, 476}, {640, 480}, {1280, 720}, {1920, 1080}
    };
    for (const int32_t (&size)[2] : sizes) {
        viewer::Model model;
        model.end_card = true;
        model.length = 1000;
        model.tick = 1000;
        for (int player = 0; player < 6; ++player) {
            viewer::WatchRow row;
            row.slot = player == 0 ? -1 : player - 1;
            row.name = player == 0 ? "All players" : "Player " + std::to_string(player);
            model.watch.push_back(row);
        }
        const viewer::Layout layout = viewer::lay_out(size[0], size[1], model, true);
        OA_CHECK(kit::wholly_in(layout.watch_again, layout.end));
        OA_CHECK(kit::wholly_in(layout.library, layout.end));
        OA_CHECK(kit::wholly_in(layout.export_button, layout.end));
        OA_CHECK(!overlaps(layout.end, layout.players));
    }
}

/// Ten players scroll, a folded list is chips, and the camera menu offers
/// only the modes the recording can show.
void check_player_list() {
    viewer::Model model;
    model.length = 42960;
    model.tick = 21000;
    viewer::WatchRow everyone;
    everyone.name = "All players";
    everyone.eye = true;
    model.watch.push_back(everyone);
    for (int player = 0; player < 10; ++player) {
        viewer::WatchRow row;
        row.slot = player;
        row.name = "Player " + std::to_string(player + 1);
        row.side = player % 2 == 0 ? "Arm" : "Core";
        row.team = player % 3 == 2 ? -1 : player % 3;
        row.units = 4 + player;
        row.income = true;
        row.metal = 1.25f * static_cast<float>(player);
        row.energy = 40.f * static_cast<float>(player);
        row.colour = {static_cast<uint8_t>(40 + player * 20), 80, 140, 255};
        if (player == 3)
            row.departed = viewer::departed_caption(true, "18:02");
        model.watch.push_back(row);
    }
    OA_CHECK(viewer::income_caption(10.f, 100.f) == "+10.0 m / +100 e");
    OA_CHECK(viewer::income_caption(-2.f, 0.4f) == "+0.0 m / +0 e");
    OA_CHECK(viewer::departed_caption(true, "18:02") == "Defeated 18:02");
    OA_CHECK(viewer::departed_caption(false, "21:40") == "Left 21:40");
    const std::string armed = viewer::watch_detail(model.watch[1]);
    OA_CHECK(armed.find("Arm") != std::string::npos);
    OA_CHECK(armed.find("units") != std::string::npos);
    OA_CHECK(viewer::watch_detail(model.watch[0]).empty());
    const std::string no_team = viewer::watch_detail(model.watch[3]);
    OA_CHECK(no_team.find("units") != std::string::npos);
    OA_CHECK(no_team.find('-') == std::string::npos);
    const viewer::Layout layout = viewer::lay_out(832, 476, model, true);
    OA_CHECK(static_cast<int>(layout.player_rows.size()) == viewer::watch_visible_rows);
    OA_CHECK(layout.list_scroll == 0);
    OA_CHECK(viewer::watch_index(layout, centre(layout.player_rows[0])) == 0);
    OA_CHECK(
        viewer::watch_index(layout, centre(layout.player_rows.back())) ==
        viewer::watch_visible_rows - 1
    );
    model.list_scroll = 100;
    const viewer::Layout scrolled = viewer::lay_out(832, 476, model, true);
    OA_CHECK(
        scrolled.list_scroll == static_cast<int>(model.watch.size()) - viewer::watch_visible_rows
    );
    OA_CHECK(
        viewer::watch_index(scrolled, centre(scrolled.player_rows.back())) ==
        static_cast<int>(model.watch.size()) - 1
    );
    model.list_folded = true;
    model.camera_open = true;
    model.offer_follow_camera = true;
    model.camera = viewer::CameraMode::follow_unit;
    const viewer::Layout folded = viewer::lay_out(832, 476, model, true);
    OA_CHECK(folded.player_chips.size() == model.watch.size());
    OA_CHECK(folded.player_rows.empty());
    OA_CHECK(viewer::watch_index(folded, centre(folded.player_chips[4])) == 4);
    OA_CHECK(folded.camera_modes.size() == 2);
    OA_CHECK(folded.camera_modes[0] == viewer::CameraMode::free);
    OA_CHECK(folded.camera_modes[1] == viewer::CameraMode::follow_theirs);
    bool offered_unit = false;
    for (const viewer::CameraMode mode : folded.camera_modes)
        offered_unit = offered_unit || mode == viewer::CameraMode::follow_unit;
    OA_CHECK(!offered_unit);
    OA_CHECK(viewer::camera_button_caption(model.camera) == "Follow a unit");
    model.offer_follow_camera = false;
    const viewer::Layout no_shared = viewer::lay_out(832, 476, model, true);
    bool saw_theirs = false;
    for (const viewer::CameraMode mode : no_shared.camera_modes)
        saw_theirs = saw_theirs || mode == viewer::CameraMode::follow_theirs;
    OA_CHECK(!saw_theirs);
    OA_CHECK(no_shared.camera_modes.size() == 1);
    OA_CHECK(no_shared.camera_modes[0] == viewer::CameraMode::free);
    model.offer_director = true;
    const viewer::Layout directed = viewer::lay_out(832, 476, model, true);
    OA_CHECK(directed.camera_modes.size() == 2);
    OA_CHECK(directed.camera_modes[0] == viewer::CameraMode::free);
    OA_CHECK(directed.camera_modes[1] == viewer::CameraMode::director);
    OA_CHECK(viewer::camera_button_caption(viewer::CameraMode::director) == "Director");
    model.offer_follow_camera = true;
    model.offer_director = true;
    const viewer::Layout both = viewer::lay_out(832, 476, model, true);
    OA_CHECK(both.camera_modes.size() == 3);
    OA_CHECK(both.camera_modes[2] == viewer::CameraMode::director);
    model.offer_director = false;
    kit::DisplayList list;
    viewer::fill_controls(folded, model, list);
    bool saw_chip = false;
    bool saw_camera = false;
    for (const kit::Control& control : list.controls) {
        saw_chip = saw_chip || control.name == "replay-viewer.watch-chip.10";
        saw_camera = saw_camera || control.name == "replay-viewer.camera-item.1";
    }
    OA_CHECK(saw_chip && saw_camera);
    const viewer::Layout hidden = viewer::lay_out(832, 476, model, false);
    kit::DisplayList hidden_list;
    viewer::fill_controls(hidden, model, hidden_list);
    bool saw_row = false;
    bool saw_button = false;
    for (const kit::Control& control : hidden_list.controls) {
        saw_row = saw_row || control.name == "replay-viewer.watch-chip.0";
        saw_button = saw_button || control.name == "replay-viewer.view";
    }
    OA_CHECK(saw_row);
    OA_CHECK(!saw_button);
}

/// A recorded number maps to the seat that holds it.
///
/// Two computers of one side share a name and do not share a number, so the
/// second seat is its own slot. A gap in the numbers is that seat's place,
/// not the number itself and not one before it.
void check_watch_slots() {
    const int numbers[] = {1, 2, 3};
    OA_CHECK(viewer::watch_slot(numbers, 1, 9) == 0);
    OA_CHECK(viewer::watch_slot(numbers, 2, 9) == 1);
    OA_CHECK(viewer::watch_slot(numbers, 3, 9) == 2);
    const int gapped[] = {1, 3};
    OA_CHECK(viewer::watch_slot(gapped, 1, 9) == 0);
    OA_CHECK(viewer::watch_slot(gapped, 3, 9) == 1);
    OA_CHECK(viewer::watch_slot(numbers, -1, 9) == -1);
    OA_CHECK(viewer::watch_slot(std::span<const int>{}, 4, 9) == 4);
}

/// One unit has its own word, and a departure leads the detail.
///
/// A 512-point row cuts the end of a long line. "Left 1:20", "Gegangen 1:20"
/// and "Defeated 18:02" stay, because they are written first.
void check_watch_detail() {
    const auto stack =
        oa::platform::text_font::FontStack::open(oa::platform::text_font::bundled_font_directory());
    OA_CHECK(stack != nullptr);
    if (stack == nullptr)
        return;
    kit::Fonts fonts;
    fonts.modern = stack.get();
    const kit::Measure measure = [&](std::string_view text) {
        return kit::text_width(fonts, kit::FontRole::small, text);
    };
    viewer::WatchRow one;
    one.slot = 0;
    one.side = "Arm";
    one.units = 1;
    const std::string singular = viewer::watch_detail(one);
    OA_CHECK(singular.find("1 unit") != std::string::npos);
    OA_CHECK(singular.find("units") == std::string::npos);
    viewer::WatchRow several = one;
    several.units = 4;
    OA_CHECK(viewer::watch_detail(several).find("4 units") != std::string::npos);

    viewer::WatchRow gone;
    gone.slot = 1;
    gone.side = "Core";
    gone.team = 1;
    gone.units = 12;
    gone.income = true;
    gone.metal = 10.f;
    gone.energy = 100.f;
    gone.departed = "Left 1:20";
    const std::string detail = viewer::watch_detail(gone);
    const auto left = detail.find("Left 1:20");
    const auto side = detail.find("Core");
    const auto units = detail.find("12 units");
    const auto income = detail.find("+10.0");
    OA_CHECK(left == 0);
    OA_CHECK(
        side != std::string::npos && units != std::string::npos && income != std::string::npos
    );
    OA_CHECK(left < side && side < units && units < income);

    viewer::Model model;
    model.length = 2400;
    model.tick = 1800;
    model.watch.push_back(gone);
    const viewer::Layout open = viewer::lay_out(512, 416, model, true);
    OA_CHECK(!open.player_rows.empty());
    const int32_t room = viewer::watch_detail_room(open.player_rows[0]);
    const char* leads[] = {"Left 1:20", "Gegangen 1:20", "Defeated 18:02"};
    const std::string tail = " · Core · Team 1 · 12 units · +10.0 m / +100 e · +10.0 m / +100 e";
    for (const char* lead : leads) {
        const std::string line = std::string(lead) + tail;
        const std::string fitted = kit::fit(line, room, measure);
        if (fitted.find(lead) != 0 || measure(fitted) > room) {
            std::fprintf(
                stderr,
                "detail %s fitted as %s in %d\n",
                lead,
                fitted.c_str(),
                static_cast<int>(room)
            );
        }
        OA_CHECK(fitted.find(lead) == 0);
        OA_CHECK(measure(fitted) <= room);
    }
}

/// The end card's three buttons and its export note, in the five languages.
///
/// At 512, 725 and 832 points the buttons stay wide enough for each caption,
/// and the note stays inside the card. A 512-point battlefield hides the
/// list, chips included, while the card shows. A wider battlefield keeps the
/// list clear of the card. At 1280 the list stays at its own width.
void check_end_card_captions() {
    const auto stack =
        oa::platform::text_font::FontStack::open(oa::platform::text_font::bundled_font_directory());
    OA_CHECK(stack != nullptr);
    if (stack == nullptr)
        return;
    kit::Fonts fonts;
    fonts.modern = stack.get();
    const auto measure = [&](std::string_view text) {
        return kit::text_width(fonts, kit::FontRole::small, text);
    };

    struct Language {
        const char* watch;
        const char* library;
        const char* export_caption;
        const char* note;
    };

    const Language languages[] = {
        {"Watch again",
         "Back to the Library",
         "Export…",
         "Only a replay in the Library can be exported"},
        {"Nochmal ansehen",
         "Zurück zur Bibliothek",
         "Exportieren…",
         "Nur ein Replay in der Bibliothek kann exportiert werden"},
        {"Revoir",
         "Retour à la Bibliothèque",
         "Exporter…",
         "Seul un replay dans la Bibliothèque peut être exporté"},
        {"Ver de nuevo",
         "Volver a la Biblioteca",
         "Exportar…",
         "Solo se puede exportar una repetición de la Biblioteca"},
        {"Guarda di nuovo",
         "Torna alla Biblioteca",
         "Esporta…",
         "Si può esportare solo un replay della Biblioteca"},
    };
    const int32_t sizes[][2] = {{512, 416}, {725, 416}, {832, 476}};
    for (const int32_t (&size)[2] : sizes) {
        for (const bool folded : {false, true}) {
            viewer::Model model;
            model.end_card = true;
            model.list_folded = folded;
            model.length = 1000;
            model.tick = 1000;
            for (int player = 0; player < 4; ++player) {
                viewer::WatchRow row;
                row.slot = player == 0 ? -1 : player - 1;
                row.name = player == 0 ? "All players" : "Core";
                model.watch.push_back(row);
            }
            const viewer::Layout layout = viewer::lay_out(size[0], size[1], model, true);
            const int32_t button = layout.watch_again.width;
            OA_CHECK(button >= 144);
            if (size[0] == 512)
                OA_CHECK(button == 144);
            OA_CHECK(layout.library.width == button);
            OA_CHECK(layout.export_button.width == button);
            OA_CHECK(kit::wholly_in(layout.watch_again, layout.end));
            OA_CHECK(kit::wholly_in(layout.library, layout.end));
            OA_CHECK(kit::wholly_in(layout.export_button, layout.end));
            // A 512-point battlefield hides the list, chips included, while
            // the card shows. A wider battlefield keeps the list, clear of
            // the card, so a departed row stays on show.
            const bool hidden = layout.players.width == 0 &&
                                (folded ? layout.player_chips.empty() : layout.player_rows.empty());
            if (size[0] <= 512) {
                if (!hidden) {
                    std::fprintf(
                        stderr,
                        "end card %dx%d folded %d left a list %d wide\n",
                        static_cast<int>(size[0]),
                        static_cast<int>(size[1]),
                        folded ? 1 : 0,
                        static_cast<int>(layout.players.width)
                    );
                }
                OA_CHECK(hidden);
            } else {
                if (hidden || overlaps(layout.end, layout.players) || layout.players.width < 168) {
                    std::fprintf(
                        stderr,
                        "end card %dx%d folded %d list %d wide, card x %d w %d\n",
                        static_cast<int>(size[0]),
                        static_cast<int>(size[1]),
                        folded ? 1 : 0,
                        static_cast<int>(layout.players.width),
                        static_cast<int>(layout.end.x),
                        static_cast<int>(layout.end.width)
                    );
                }
                OA_CHECK(!hidden);
                OA_CHECK(!overlaps(layout.end, layout.players));
                OA_CHECK(layout.players.width >= 168);
            }
            const int32_t note_room = layout.end.width - 32;
            for (const Language& language : languages) {
                const int32_t watch = measure(language.watch);
                const int32_t library = measure(language.library);
                const int32_t export_width = measure(language.export_caption);
                const int32_t note = measure(language.note);
                if (watch > button || library > button || export_width > button ||
                    note > note_room) {
                    std::fprintf(
                        stderr,
                        "end card %dx%d %s is %d, %s is %d, %s is %d, note %d in %d\n",
                        static_cast<int>(size[0]),
                        static_cast<int>(size[1]),
                        language.watch,
                        static_cast<int>(watch),
                        language.library,
                        static_cast<int>(library),
                        language.export_caption,
                        static_cast<int>(export_width),
                        static_cast<int>(note),
                        static_cast<int>(note_room)
                    );
                }
                OA_CHECK(watch <= button);
                OA_CHECK(library <= button);
                OA_CHECK(export_width <= button);
                OA_CHECK(note <= note_room);
            }
        }
    }
    viewer::Model wide;
    wide.end_card = true;
    wide.length = 1000;
    wide.tick = 1000;
    for (int player = 0; player < 4; ++player) {
        viewer::WatchRow row;
        row.slot = player == 0 ? -1 : player - 1;
        row.name = player == 0 ? "All players" : "Core";
        if (player == 2)
            row.departed = "Left 1:20";
        wide.watch.push_back(row);
    }
    const viewer::Layout kept = viewer::lay_out(1280, 720, wide, true);
    OA_CHECK(kept.players.width > 0);
    OA_CHECK(!kept.player_rows.empty());
    OA_CHECK(!overlaps(kept.end, kept.players));
    OA_CHECK(kept.watch_again.width >= 144);
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
    check_wide_view_caption();
    check_caption_fit();
    check_end_card_clears_the_list();
    check_end_card_captions();
    check_watch_slots();
    check_watch_detail();
    check_player_list();

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
    OA_CHECK(viewer::hit_control(layout, centre(layout.view)) == viewer::Control::view);
    OA_CHECK(viewer::hit_control(layout, centre(layout.camera)) == viewer::Control::camera);
    OA_CHECK(viewer::hit_control(layout, centre(layout.photo)) == viewer::Control::photo);
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
