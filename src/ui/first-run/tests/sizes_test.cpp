// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The setup's sizes on windows of 640 by 480, 1280 by 720, 1920 by 1080 and
// 1920 by 1117 at twice the density, laid out as the pre-game window lays it
// out and drawn in the bundled fonts: the footer's buttons and the body text
// at least the sizes stated below, and nothing overlapping in English,
// German and Chinese. Nothing is drawn under the header or past the
// footer's top: a list that does not fit scrolls, clipped above the lines
// under it, and the line that says where the copies go meets nothing else.
// The header leaves out the mark and the name in a window and keeps them
// full screen. The game step's title reads the new words, and Locate
// another… holds its words with room on every side in every language. The
// question a changed install asks over the copying step, naming archives of
// the base game and of expansions together, fits the window in every
// language, with none of its text cut and nothing in it overlapping.
//
// Usage: oa-ui-first-run-sizes-test INTERFACE_TDF CHINESE_TDF, the engine's
// languages/interface.tdf and this folder's zh-Hans-setup.tdf.

#include "oa/ui/first_run/first_run.hpp"

#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/paint/painter.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

namespace first_run = oa::ui::first_run;
namespace kit = oa::ui::kit;
namespace languages = oa::data::languages;
namespace paint = oa::ui::paint;
namespace text_font = oa::platform::text_font;
using first_run::Leave;

/// A window the sizes are stated for, and the least sizes there, in the
/// window's own points. Each is the 640 by 480 floor scaled by how far the
/// window holds the setup's 960 by 540 room: four thirds at 1280 by 720,
/// twice at 1920 wide.
struct Window {
    int32_t width{};         ///< the window's width, in its points
    int32_t height{};        ///< the window's height, in its points
    int32_t density{};       ///< its pixels a point
    const char* name{};      ///< for the log
    int32_t button_width{};  ///< a footer button's least width
    int32_t button_height{}; ///< a footer button's least height
    int32_t text_points{};   ///< the body text's least size
};

constexpr Window kWindows[] = {
    {640, 480, 1, "640x480", 52, 17, 10},
    {1280, 720, 1, "1280x720", 69, 22, 13},
    {1920, 1080, 1, "1920x1080", 104, 34, 20},
    {1920, 1117, 2, "1920x1117 at 2x", 104, 34, 20},
};

/// The room kept clear inside a button, on its left and its right, in points.
constexpr int32_t kButtonSidePadding = 4;
/// The room kept clear inside a button, above and below its words, in points.
constexpr int32_t kButtonEndPadding = 2;

/// Copies a UTF-8 literal into a string.
///
/// @param text the literal
/// @return its bytes
std::string utf8(const char8_t* text) {
    return std::string(reinterpret_cast<const char*>(text));
}

/// Reads a file whole.
///
/// @param path the file
/// @return its bytes; empty when it cannot be read
std::string read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/// A smooth canvas of a window's pixels, drawn at the setup's scale, as the
/// pre-game window draws: the bundled fonts and no game fonts.
struct Drawing {
    paint::Canvas pixels;
    paint::Painter painter;
    kit::Canvas canvas{};

    Drawing(text_font::FontStack* fonts, const Window& window, const kit::Frame& frame)
        : pixels(paint::make_canvas(window.width * window.density, window.height * window.density)),
          painter(pixels) {
        canvas.smooth.painter = &painter;
        canvas.smooth.fonts = fonts;
        canvas.smooth.scale_percent = frame.scale_percent;
    }
};

/// Tells whether a character stands a whole em wide: the ideographs, kana,
/// Hangul and the full-width forms.
///
/// @param code the character
/// @return true when it is wide
bool wide(char32_t code) noexcept {
    return (code >= 0x1100 && code <= 0x115F) || (code >= 0x2E80 && code <= 0xA4CF) ||
           (code >= 0xAC00 && code <= 0xD7A3) || (code >= 0xF900 && code <= 0xFAFF) ||
           (code >= 0xFE30 && code <= 0xFE4F) || (code >= 0xFF00 && code <= 0xFF60) ||
           (code >= 0xFFE0 && code <= 0xFFE6);
}

/// A line's width as it is drawn, in points. A wide character takes one em
/// of the font, as the Simplified Chinese pack's face draws it; the bundled
/// faces measure the rest.
///
/// @param canvas the canvas, at its scale
/// @param role which font
/// @param text the line, in UTF-8
/// @return the width, in points
int32_t drawn_width(const kit::Canvas& canvas, kit::FontRole role, std::string_view text) {
    const std::vector<char32_t> characters =
        text_font::decode_utf8(text).value_or(std::vector<char32_t>{});
    int32_t width = 0;
    std::string narrow;
    std::size_t at = 0;
    for (const char32_t code : characters) {
        std::size_t bytes = 1;
        const auto lead = static_cast<unsigned char>(text[at]);
        if (lead >= 0xF0)
            bytes = 4;
        else if (lead >= 0xE0)
            bytes = 3;
        else if (lead >= 0xC0)
            bytes = 2;
        if (wide(code)) {
            width += kit::measure_text(canvas, role, narrow);
            narrow.clear();
            width += kit::smooth_text_points(role);
        } else {
            narrow.append(text.substr(at, bytes));
        }
        at += bytes;
    }
    return width + kit::measure_text(canvas, role, narrow);
}

/// How many characters a line holds.
///
/// @param text the line, in UTF-8
/// @return the characters
int32_t characters_of(std::string_view text) {
    int32_t count = 0;
    for (const char byte : text)
        if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U)
            ++count;
    return count;
}

/// Tells whether two rectangles share any area.
///
/// @param left a rectangle
/// @param right another
/// @return true when they share area
bool meets(const kit::Rect& left, const kit::Rect& right) {
    const kit::Rect shared = kit::intersect(left, right);
    return shared.width > 0 && shared.height > 0;
}

/// The part of a rectangle a clip leaves in view.
///
/// @param rect the rectangle
/// @param clip the clip; empty for none
/// @return the part in view; empty when none is
kit::Rect in_view(const kit::Rect& rect, const kit::Rect& clip) {
    if (clip.width <= 0 || clip.height <= 0)
        return rect;
    return kit::intersect(rect, clip);
}

/// Tells whether a rectangle has area.
///
/// @param rect the rectangle
/// @return true when it is wider and taller than nothing
bool has_area(const kit::Rect& rect) {
    return rect.width > 0 && rect.height > 0;
}

/// One thing on a step that must not run into another.
struct Box {
    kit::Rect rect{};
    std::string what{};
    enum class Kind : uint8_t { text, control, card, arrow, pill } kind{Kind::text};
};

/// The cards of the language step, with the language shown chosen.
///
/// @param tag the language shown
/// @return the session
first_run::Session language_session(std::string_view tag) {
    first_run::Session session;
    session.model.walk = first_run::Walk::setup;
    session.model.leave = first_run::Leave::quit;

    const struct {
        const char* tag;
        const char* own;
        const char* english;
    } built_in[] = {
        {"en", "English", "English"},
        {"de", "Deutsch", "German"},
        {"es", "Espa\xc3\xb1ol", "Spanish"},
        {"fr",
         "Fran\xc3\xa7"
         "ais",
         "French"},
        {"it", "Italiano", "Italian"},
    };

    for (const auto& entry : built_in) {
        first_run::Card card;
        card.tag = entry.tag;
        card.own_name = entry.own;
        card.english_name = entry.english;
        card.current = tag == entry.tag;
        session.model.cards.push_back(std::move(card));
    }
    first_run::Card chinese;
    chinese.tag = "zh-Hans";
    chinese.own_name = utf8(u8"\u7b80\u4f53\u4e2d\u6587");
    chinese.english_name = "Chinese (Simplified)";
    chinese.kind = tag == "zh-Hans" ? first_run::CardKind::installed : first_run::CardKind::pack;
    chinese.current = tag == "zh-Hans";
    chinese.size_text = "3.4 MB";
    session.model.cards.push_back(std::move(chinese));
    session.carousel.selected = 0;
    for (std::size_t at = 0; at < session.model.cards.size(); ++at)
        if (session.model.cards[at].current)
            session.carousel.selected = static_cast<int32_t>(at);
    first_run::bind(session);
    kit::carousel_settle(session.carousel);
    return session;
}

/// The game step, two installers, the fuller chosen.
///
/// @return the session
first_run::Session game_session() {
    first_run::Session session = language_session("en");
    session.model.step = first_run::Step::game;
    first_run::GameSource fuller;
    fuller.title = "setup_total_annihilation.exe";
    fuller.detail = "~/Downloads";
    fuller.mark = first_run::SourceMark::gog;
    fuller.footnote = "1.0 GB";
    fuller.chips = {
        {"Total Annihilation 3.1c", true},
        {"Campaign", true},
        {"Core Contingency", true},
        {"Battle Tactics", true},
        {"Music", true},
    };
    first_run::GameSource thinner = fuller;
    thinner.title = "setup_total_annihilation_2.exe";
    thinner.chips = {{"Total Annihilation 3.1c", true}, {"Music", true}};
    session.model.sources = {fuller, thinner};
    session.model.source = 0;
    return session;
}

/// The game step with more places than a small window holds, the last chosen.
///
/// @return the session
first_run::Session crowded_game_session() {
    first_run::Session session = game_session();
    const first_run::GameSource fuller = session.model.sources.front();
    session.model.sources.clear();

    const struct {
        const char* title;
        first_run::SourceMark mark;
        std::size_t held;
    } places[] = {
        {"setup_total_annihilation_commander_pack_3.1_(22139).exe", first_run::SourceMark::gog, 7},
        {"Total Annihilation", first_run::SourceMark::folder, 6},
        {"TA_Demo.exe", first_run::SourceMark::demo, 1},
        {"totala.iso", first_run::SourceMark::disc, 4},
        {"setup_total_annihilation_2.exe", first_run::SourceMark::gog, 5},
    };

    const char* bundles[] = {
        "Total Annihilation 3.1c",
        "Campaign",
        "Core Contingency",
        "Battle Tactics",
        "Music",
        "Cavedog units",
        "Cavedog tilesets",
    };
    for (const auto& place : places) {
        first_run::GameSource source = fuller;
        source.title = place.title;
        source.mark = place.mark;
        source.footnote = "1.0 GB · 40 GB free";
        source.chips.clear();
        for (std::size_t at = 0; at < place.held; ++at)
            source.chips.push_back({bundles[at], true});
        session.model.sources.push_back(std::move(source));
    }
    session.model.source = static_cast<int32_t>(session.model.sources.size()) - 1;
    return session;
}

/// How far a full copy has got.
enum class CopyStage : uint8_t {
    started,  ///< the first bundle copies
    underway, ///< the sixth of seven copies
    checked,  ///< every bundle is done and checked
};

/// The copying step of the fullest installer: seven bundles, more than a
/// window holds.
///
/// @param stage how far the copy has got
/// @return the session
first_run::Session full_copy_session(CopyStage stage) {
    first_run::Session session = game_session();
    session.model.step = first_run::Step::copying;
    first_run::CopyView& copy = session.model.copy;
    copy.from = "setup_total_annihilation_commander_pack_3.1_(22139).exe";
    copy.whole = 1000;
    const char* names[] = {
        "Total Annihilation 3.1c",
        "Campaign",
        "Core Contingency",
        "Battle Tactics",
        "Music",
        "Cavedog units",
        "Cavedog tilesets",
    };
    const int32_t copying = stage == CopyStage::started ? 0 : 5;
    for (int32_t index = 0; index < 7; ++index) {
        first_run::CopyBundle row;
        row.name = names[index];
        row.whole = 1000;
        const bool done = stage == CopyStage::checked || index < copying;
        if (done) {
            row.phase = first_run::CopyPhase::done;
            row.done = 1000;
            row.note = index == 4;
            row.amount = row.note ? "17 of 18 tracks" : "All files match";
        } else if (index == copying) {
            row.phase = first_run::CopyPhase::copying;
            row.file = "totala2.hpi";
            row.amount = "94.9 MB";
            row.done = 130;
        }
        copy.bundles.push_back(std::move(row));
    }
    if (stage == CopyStage::checked) {
        copy.overall = "47 of 47 · 100%";
        copy.done = 1000;
        copy.checked = true;
        copy.verdict = "All files match";
        session.copy_continue_ms = 3000;
    } else {
        copy.overall = stage == CopyStage::started ? "2 of 47 · 4%" : "39 of 47 · 83%";
        copy.remaining = "about 30 seconds left";
        copy.done = stage == CopyStage::started ? 40 : 830;
        copy.running = true;
    }
    return session;
}

/// The copying step, part way through.
///
/// @return the session
first_run::Session copying_session() {
    first_run::Session session = game_session();
    session.model.step = first_run::Step::copying;
    first_run::CopyView& copy = session.model.copy;
    copy.from = "setup_total_annihilation.exe";
    copy.overall = "12 of 38 · 31%";
    copy.remaining = "about 40 seconds left";
    copy.done = 310;
    copy.whole = 1000;
    copy.running = true;
    first_run::CopyBundle bundle;
    bundle.name = "Total Annihilation";
    bundle.file = "totala1.hpi";
    bundle.amount = "31%";
    bundle.done = 310;
    bundle.whole = 1000;
    bundle.phase = first_run::CopyPhase::copying;
    first_run::CopyBundle music;
    music.name = "Music";
    music.amount = "17 of 18 tracks";
    music.done = 1000;
    music.whole = 1000;
    music.phase = first_run::CopyPhase::done;
    music.note = true;
    copy.bundles = {bundle, music};
    return session;
}

/// Checks a button holds its words with room on every side.
///
/// @param canvas the canvas the words are measured on
/// @param rect the button, in points
/// @param caption its words
/// @param where what is being checked, for the log
void expect_words_inside(
    const kit::Canvas& canvas, const kit::Rect& rect, std::string_view caption, const char* where
) {
    const int32_t words = drawn_width(canvas, kit::FontRole::small, caption);
    const bool across = words + 2 * kButtonSidePadding <= rect.width;
    const bool down = kit::compact_metrics.small_line + 2 * kButtonEndPadding <= rect.height;
    if (!across || !down)
        std::fprintf(
            stderr,
            "%s: \"%.*s\" is %d points in a button %d by %d\n",
            where,
            static_cast<int>(caption.size()),
            caption.data(),
            words,
            rect.width,
            rect.height
        );
    OA_CHECK(across);
    OA_CHECK(down);
}

/// The width the header's left words take, from the window's left, in points.
///
/// @param canvas the canvas the words are measured on
/// @param look the header's look
/// @return where the left words end
int32_t header_left_end(const kit::Canvas& canvas, const kit::SetupChromeLook& look) {
    const auto tracked = [&](std::string_view words) {
        return drawn_width(canvas, kit::FontRole::regular, words) +
               kit::compact_metrics.heading_tracking * std::max(characters_of(words) - 1, 0);
    };
    int32_t right = kit::compact_metrics.padding;
    if (look.brand)
        right += kit::compact_metrics.mark_side + kit::compact_metrics.header_gap +
                 tracked("Open Annihilation") + kit::compact_metrics.header_gap;
    return right + tracked(look.page);
}

/// Where the step indicator starts, from the window's left, in points.
///
/// @param canvas the canvas the words are measured on
/// @param look the header's look
/// @return its left edge
int32_t step_left(const kit::Canvas& canvas, const kit::SetupChromeLook& look) {
    kit::StepLook step;
    step.name = look.step_name;
    step.pattern = look.step_pattern;
    step.index = look.step_index;
    step.count = look.step_count;
    const std::string line = kit::step_line(step);
    const int32_t words =
        drawn_width(canvas, kit::FontRole::small, line) +
        kit::compact_metrics.heading_tracking * std::max(characters_of(line) - 1, 0);
    const int32_t segments = look.step_count * 14 + (look.step_count - 1) * 3;
    return look.width - kit::compact_metrics.edge - kit::compact_metrics.padding -
           std::max(words, segments);
}

/// Lays a step out on a window, draws it, and checks its sizes and that
/// nothing on it runs into anything else.
///
/// @param fonts the bundled fonts
/// @param window the window
/// @param session the step
/// @param language the language, for the log
void check_step(
    text_font::FontStack* fonts,
    const Window& window,
    const first_run::Session& session,
    const char* language
) {
    const kit::Frame frame = first_run::setup_frame(
        window.width * window.density, window.height * window.density, window.density * 100, true
    );
    const first_run::Scene scene = first_run::lay_out(session, frame, nullptr);
    Drawing drawing(fonts, window, frame);
    first_run::draw(drawing.canvas, scene, session);
    const std::string where = std::string(window.name) + " " + language + " " +
                              std::string(first_run::step_token(session.model.step)) +
                              (session.model.windowed ? " window" : " full screen");
    // A point of the frame in the window's own points, in hundredths.
    const int32_t window_percent = frame.scale_percent / window.density;

    // The footer's buttons at least the stated size, and their words inside them.
    const kit::SetupChromeLook* chrome = nullptr;
    for (const kit::Item& item : scene.list.items)
        if (const auto* look = std::get_if<kit::SetupChromeLook>(&item.look))
            chrome = look;
    OA_CHECK(chrome != nullptr);
    if (chrome == nullptr)
        return;

    const struct {
        kit::ControlId id;
        const std::string* caption;
    } footer[] = {
        {first_run::skip_id, &chrome->skip},
        {first_run::back_id, &chrome->back},
        {first_run::next_id, &chrome->next},
    };

    for (const auto& button : footer) {
        const kit::Control* control = kit::control_of(scene.list, button.id);
        OA_CHECK(control != nullptr);
        if (control == nullptr)
            continue;
        const int32_t wide_points = control->rect.width * window_percent / 100;
        const int32_t tall_points = control->rect.height * window_percent / 100;
        if (wide_points < window.button_width || tall_points < window.button_height)
            std::fprintf(
                stderr,
                "%s: %s is %d by %d points\n",
                where.c_str(),
                control->name.c_str(),
                wide_points,
                tall_points
            );
        OA_CHECK(wide_points >= window.button_width);
        OA_CHECK(tall_points >= window.button_height);
        expect_words_inside(drawing.canvas, control->rect, *button.caption, where.c_str());
    }
    // The body text's size, in the window's points.
    const int32_t text_points =
        kit::smooth_text_points(kit::FontRole::small) * window_percent / 100;
    if (text_points < window.text_points)
        std::fprintf(stderr, "%s: body text is %d points\n", where.c_str(), text_points);
    OA_CHECK(text_points >= window.text_points);

    // The header's words do not reach the step indicator.
    const int32_t left_end = header_left_end(drawing.canvas, *chrome);
    const int32_t indicator = step_left(drawing.canvas, *chrome);
    if (left_end + kit::compact_metrics.header_gap > indicator)
        std::fprintf(
            stderr,
            "%s: the header's words reach %d, the step %d\n",
            where.c_str(),
            left_end,
            indicator
        );
    OA_CHECK(left_end + kit::compact_metrics.header_gap <= indicator);

    // What is on the step, and where each thing's words are drawn.
    std::vector<Box> boxes;
    const kit::Rect room{0, 0, frame.area.width, frame.area.height};
    const kit::Rect body{
        0,
        kit::compact_metrics.edge + kit::compact_metrics.header_height + 1,
        frame.area.width,
        frame.area.height - 2 * kit::compact_metrics.edge - kit::compact_metrics.header_height -
            kit::compact_metrics.footer_height - 2
    };
    for (const kit::Item& item : scene.list.items) {
        if (item.role == kit::Role::text && !item.text.empty()) {
            const int32_t words = drawn_width(drawing.canvas, item.font, item.text);
            if (words > item.rect.width + 1)
                std::fprintf(
                    stderr,
                    "%s: \"%s\" is %d points on a line of %d\n",
                    where.c_str(),
                    item.text.c_str(),
                    words,
                    item.rect.width
                );
            OA_CHECK(words <= item.rect.width + 1);
            int32_t left = item.rect.x;
            if (item.align == kit::Align::centre)
                left = item.rect.x + (item.rect.width - words) / 2;
            else if (item.align == kit::Align::right)
                left = item.rect.x + item.rect.width - words;
            // A line of a list that scrolls counts where it shows.
            const kit::Rect shown =
                in_view({left, item.rect.y, words, item.rect.height}, item.clip);
            if (has_area(shown))
                boxes.push_back({shown, item.text, Box::Kind::text});
        } else if (item.role == kit::Role::pill) {
            const kit::Rect shown = in_view(item.rect, item.clip);
            if (has_area(shown))
                boxes.push_back({shown, item.text, Box::Kind::pill});
        }
    }
    if (!scene.title_text.empty())
        boxes.push_back({scene.title, scene.title_text, Box::Kind::text});
    for (const kit::Control& control : scene.list.controls) {
        Box::Kind kind = Box::Kind::control;
        if (control.name.find(".card.") != std::string::npos)
            kind = Box::Kind::card;
        else if (
            control.name.find(".previous") != std::string::npos ||
            control.name.find(".next-card") != std::string::npos
        )
            kind = Box::Kind::arrow;
        const kit::Rect shown = in_view(control.rect, control.clip);
        if (has_area(shown))
            boxes.push_back({shown, control.name, kind});
    }
    for (const Box& box : boxes) {
        const bool inside = kit::wholly_in(box.rect, room);
        if (!inside)
            std::fprintf(stderr, "%s: %s leaves the window\n", where.c_str(), box.what.c_str());
        OA_CHECK(inside);
        // Only the footer's buttons sit in the footer, and nothing in the header.
        const bool footer_button = box.what.find(".skip") != std::string::npos ||
                                   box.what.find(".back") != std::string::npos ||
                                   box.what.find(".next") != std::string::npos;
        if (!footer_button || box.what.find("next-card") != std::string::npos) {
            const bool in_body = kit::wholly_in(box.rect, body);
            if (!in_body)
                std::fprintf(
                    stderr, "%s: %s leaves the page's body\n", where.c_str(), box.what.c_str()
                );
            OA_CHECK(in_body);
        }
    }
    for (std::size_t one = 0; one < boxes.size(); ++one) {
        for (std::size_t other = one + 1; other < boxes.size(); ++other) {
            const Box& a = boxes[one];
            const Box& b = boxes[other];
            if (!meets(a.rect, b.rect))
                continue;
            // The cards of the carousel stand behind one another and behind
            // its arrows; a card's words are its own.
            const bool carousel = (a.kind == Box::Kind::card || a.kind == Box::Kind::arrow) &&
                                  (b.kind == Box::Kind::card || b.kind == Box::Kind::arrow);
            if (carousel)
                continue;
            const bool both_text = a.kind == Box::Kind::text && b.kind == Box::Kind::text;
            // A row's words and pills sit inside its card; text never meets text.
            const bool held =
                !both_text && (kit::wholly_in(a.rect, b.rect) || kit::wholly_in(b.rect, a.rect));
            if (held)
                continue;
            std::fprintf(
                stderr,
                "%s: \"%s\" runs into \"%s\"\n",
                where.c_str(),
                a.what.c_str(),
                b.what.c_str()
            );
            OA_CHECK(false);
        }
    }

    // Nothing the step draws reaches under the header or past the footer's
    // top: a list that does not fit is clipped above the lines under it.
    const std::string destination = std::string(
        languages::interface_text(
            session.model.step == first_run::Step::copying
                ? "To Documents › Open Annihilation › Games and Expansions"
                : "Copies go to Documents › Open Annihilation"
        )
    );
    std::vector<kit::Rect> lines;
    for (const kit::Item& item : scene.list.items) {
        if (item.role == kit::Role::setup_chrome)
            continue;
        const kit::Rect shown = in_view(item.rect, item.clip);
        if (!has_area(shown))
            continue;
        const bool inside = shown.y >= body.y && shown.y + shown.height <= body.y + body.height;
        if (!inside)
            std::fprintf(
                stderr,
                "%s: a part (\"%s\") is drawn from %d to %d, outside %d to %d\n",
                where.c_str(),
                item.text.c_str(),
                shown.y,
                shown.y + shown.height,
                body.y,
                body.y + body.height
            );
        OA_CHECK(inside);
        if (item.role == kit::Role::text && item.text == destination)
            lines.push_back(shown);
    }
    // The line that says where the copies go meets nothing else it draws.
    const bool says_where = session.model.step == first_run::Step::copying ||
                            session.model.step == first_run::Step::game;
    if (says_where) {
        if (lines.empty())
            std::fprintf(stderr, "%s: no line says where the copies go\n", where.c_str());
        OA_CHECK(!lines.empty());
    }
    for (const kit::Rect& line : lines) {
        for (const kit::Item& item : scene.list.items) {
            if (item.role == kit::Role::setup_chrome || item.text == destination)
                continue;
            const kit::Rect shown = in_view(item.rect, item.clip);
            if (!has_area(shown) || !meets(line, shown))
                continue;
            std::fprintf(
                stderr,
                "%s: where the copies go meets a part (\"%s\")\n",
                where.c_str(),
                item.text.c_str()
            );
            OA_CHECK(false);
        }
    }
}

/// The copying step stopped by a changed install: three archives of the base
/// game and one each of Core Contingency and Battle Tactics at other sizes,
/// or with `many` the rest of Battle Tactics' archives too, more than the
/// question names one by one.
///
/// @param many the install's every Battle Tactics archive was changed
/// @return the session
first_run::Session changed_session(bool many) {
    first_run::Session session = full_copy_session(CopyStage::started);
    first_run::CopyView& copy = session.model.copy;
    copy.running = false;
    copy.remaining.clear();
    copy.overall.clear();
    copy.done = 0;
    for (first_run::CopyBundle& row : copy.bundles) {
        row.phase = first_run::CopyPhase::waiting;
        row.file.clear();
        row.amount.clear();
        row.done = 0;
    }
    copy.changed = {
        {"rev31.gp3", "Total Annihilation 3.1c", 5481301, 5481280},
        {"totala1.hpi", "Total Annihilation 3.1c", 132311035, 32311035},
        {"totala2.hpi", "Total Annihilation 3.1c", 198009698, 98009698},
        {"ccdata.ccx", "Core Contingency", 125871597, 25871597},
        {"tactics1.hpi", "Battle Tactics", 121030743, 21030743},
    };
    if (many) {
        for (const char* name :
             {"tactics2.hpi", "tactics3.hpi", "tactics4.hpi", "tactics5.hpi", "tactics6.hpi"})
            copy.changed.push_back({name, "Battle Tactics", 128276703, 28276703});
    }
    return session;
}

/// Lays the question a changed install asks out on a window, draws it, and
/// checks it: whole in the window, none of its text cut, every line of its
/// text inside the question and as wide as it is drawn, its buttons holding
/// their words, and nothing in it running into anything else.
///
/// @param fonts the bundled fonts
/// @param window the window
/// @param session the step, with the question showing
/// @param language the language, for the log
void check_changed(
    text_font::FontStack* fonts,
    const Window& window,
    const first_run::Session& session,
    const char* language
) {
    const kit::Frame frame = first_run::setup_frame(
        window.width * window.density, window.height * window.density, window.density * 100, true
    );
    const first_run::Scene scene = first_run::lay_out(session, frame, nullptr);
    Drawing drawing(fonts, window, frame);
    first_run::draw(drawing.canvas, scene, session);
    const std::string where = std::string(window.name) + " " + language + " changed install" +
                              (session.model.windowed ? " window" : " full screen");
    const kit::Rect room{0, 0, frame.area.width, frame.area.height};
    OA_CHECK(scene.over_from > 0);
    const bool whole = has_area(scene.question) && kit::wholly_in(scene.question, room);
    if (!whole)
        std::fprintf(stderr, "%s: the question leaves the window\n", where.c_str());
    OA_CHECK(whole);
    const kit::Question question = first_run::changed_question(session.model, frame.size_class);
    const bool cut = kit::place_question(question, nullptr).cut;
    if (cut)
        std::fprintf(stderr, "%s: the question's text is cut\n", where.c_str());
    OA_CHECK(!cut);

    std::vector<Box> boxes;
    for (const kit::Item& item : scene.question_list.items) {
        if (item.role != kit::Role::text || item.text.empty())
            continue;
        const int32_t words = drawn_width(drawing.canvas, item.font, item.text);
        if (words > item.rect.width + 1)
            std::fprintf(
                stderr,
                "%s: \"%s\" is %d points on a line of %d\n",
                where.c_str(),
                item.text.c_str(),
                words,
                item.rect.width
            );
        OA_CHECK(words <= item.rect.width + 1);
        // The question's parts are in its own points.
        const kit::Rect drawn{
            scene.question.x + item.rect.x, scene.question.y + item.rect.y, words, item.rect.height
        };
        const bool inside = kit::wholly_in(drawn, scene.question);
        if (!inside)
            std::fprintf(
                stderr, "%s: \"%s\" leaves the question\n", where.c_str(), item.text.c_str()
            );
        OA_CHECK(inside);
        boxes.push_back({drawn, item.text, Box::Kind::text});
    }
    for (const kit::ControlId id : {first_run::changed_choose_id, first_run::changed_leave_id}) {
        const kit::Control* control = kit::control_of(scene.list, id);
        OA_CHECK(control != nullptr);
        if (control == nullptr)
            continue;
        OA_CHECK(kit::wholly_in(control->rect, scene.question));
        expect_words_inside(drawing.canvas, control->rect, control->text, where.c_str());
        boxes.push_back({control->rect, control->name, Box::Kind::control});
    }
    for (std::size_t one = 0; one < boxes.size(); ++one) {
        for (std::size_t other = one + 1; other < boxes.size(); ++other) {
            if (!meets(boxes[one].rect, boxes[other].rect))
                continue;
            std::fprintf(
                stderr,
                "%s: \"%s\" runs into \"%s\"\n",
                where.c_str(),
                boxes[one].what.c_str(),
                boxes[other].what.c_str()
            );
            OA_CHECK(false);
        }
    }
}

/// Counts the pixels of a colour family in a part of the header.
///
/// @param drawing the drawn step
/// @param frame its frame
/// @param points the part, in points
/// @param test which pixels count
/// @return how many do
template <typename Test>
int32_t
count_pixels(const Drawing& drawing, const kit::Frame& frame, const kit::Rect& points, Test test) {
    const kit::Rect pixels = kit::to_canvas(points, frame.scale_percent);
    int32_t count = 0;
    for (int32_t y = pixels.y; y < pixels.y + pixels.height; ++y)
        for (int32_t x = pixels.x; x < pixels.x + pixels.width; ++x)
            if (test(paint::pixel_at(drawing.pixels, x, y)))
                ++count;
    return count;
}

/// The header full screen draws the mark and "Open Annihilation"; in a
/// window it draws neither, and the page word and the step indicator stay.
///
/// @param fonts the bundled fonts
void check_header(text_font::FontStack* fonts) {
    for (const Window& window : kWindows) {
        const kit::Frame frame = first_run::setup_frame(
            window.width * window.density,
            window.height * window.density,
            window.density * 100,
            true
        );
        int32_t accent[2]{};
        int32_t bright[2]{};
        int32_t ink[2]{};
        for (const bool windowed : {false, true}) {
            first_run::Session session = language_session("en");
            session.model.windowed = windowed;
            const first_run::Scene scene = first_run::lay_out(session, frame, nullptr);
            Drawing drawing(fonts, window, frame);
            first_run::draw(drawing.canvas, scene, session);
            const kit::Rect left_half{
                0,
                kit::compact_metrics.edge,
                frame.area.width / 2,
                kit::compact_metrics.header_height
            };
            const kit::Rect right_part{
                frame.area.width * 3 / 4,
                kit::compact_metrics.edge,
                frame.area.width / 4 - kit::compact_metrics.edge,
                kit::compact_metrics.header_height
            };
            // The OA mark's brand gold: red over green, little blue.
            const auto is_accent = [](paint::Rgba pixel) {
                return pixel.r >= 0xb0 && pixel.g >= 0x80 && pixel.r > pixel.g && pixel.b <= 0x90;
            };
            const auto is_bright = [](paint::Rgba pixel) {
                return pixel.r >= 0xc8 && pixel.g >= 0xc8 && pixel.b >= 0xc0;
            };
            const auto is_ink = [](paint::Rgba pixel) {
                return pixel.r >= 0x48 || pixel.g >= 0x48 || pixel.b >= 0x48;
            };
            const std::size_t at = windowed ? 1U : 0U;
            accent[at] = count_pixels(drawing, frame, left_half, is_accent);
            bright[at] = count_pixels(drawing, frame, left_half, is_bright);
            ink[at] = count_pixels(drawing, frame, right_part, is_ink);
            // The page word is drawn, at the left in a window.
            const int32_t page_ink = count_pixels(
                drawing,
                frame,
                {kit::compact_metrics.padding,
                 kit::compact_metrics.edge,
                 kit::compact_metrics.mark_side,
                 kit::compact_metrics.header_height},
                is_ink
            );
            OA_CHECK(page_ink > 0);
        }
        if (accent[0] == 0 || bright[0] == 0 || accent[1] != 0 || bright[1] != 0)
            std::fprintf(
                stderr,
                "%s header: full screen %d mark and %d name pixels, window %d and %d\n",
                window.name,
                accent[0],
                bright[0],
                accent[1],
                bright[1]
            );
        OA_CHECK(accent[0] > 0);
        OA_CHECK(bright[0] > 0);
        OA_CHECK(accent[1] == 0);
        OA_CHECK(bright[1] == 0);
        // The step indicator stays, the same in both.
        OA_CHECK(ink[0] > 0);
        OA_CHECK(ink[0] == ink[1]);
    }
}

/// The game step's title, and Locate another… with its words inside it, in
/// every language, on every window.
///
/// @param fonts the bundled fonts
/// @param catalogue the built-in languages' words
/// @param chinese the Simplified Chinese words
void check_locate(
    text_font::FontStack* fonts,
    const languages::InterfaceText& catalogue,
    const languages::InterfaceText& chinese
) {
    const std::string chinese_title =
        utf8(u8"\u5b9a\u4f4d Total Annihilation \u6e38\u620f\u6587\u4ef6");

    const struct {
        const char* tag;
        const char* title;
        const languages::InterfaceText* words;
    } spoken[] = {
        {"en", "Locate Total Annihilation Game Files", nullptr},
        {"de", "Spieldateien von Total Annihilation suchen", &catalogue},
        {"fr", nullptr, &catalogue},
        {"es", nullptr, &catalogue},
        {"it", nullptr, &catalogue},
        {"zh-Hans", chinese_title.c_str(), &chinese},
    };

    for (const auto& language : spoken) {
        const languages::Language* found = languages::find_by_tag(language.tag);
        OA_CHECK(found != nullptr);
        if (found == nullptr)
            continue;
        languages::set_interface_language(language.words, *found);
        for (const Window& window : kWindows) {
            const kit::Frame frame = first_run::setup_frame(
                window.width * window.density,
                window.height * window.density,
                window.density * 100,
                true
            );
            const first_run::Session session = game_session();
            const first_run::Scene scene = first_run::lay_out(session, frame, nullptr);
            if (language.title != nullptr) {
                if (scene.title_text != language.title)
                    std::fprintf(
                        stderr, "%s: the title is \"%s\"\n", language.tag, scene.title_text.c_str()
                    );
                OA_CHECK(scene.title_text == language.title);
            } else {
                OA_CHECK(!scene.title_text.empty());
                OA_CHECK(scene.title_text != "Locate Total Annihilation Game Files");
            }
            const kit::Control* locate = kit::control_of(scene.list, first_run::locate_id);
            const kit::Control* back = kit::control_of(scene.list, first_run::back_id);
            OA_CHECK(locate != nullptr && back != nullptr);
            if (locate == nullptr || back == nullptr)
                continue;
            // Sized like the footer's buttons, and its words inside it.
            OA_CHECK(locate->rect.height == back->rect.height);
            Drawing drawing(fonts, window, frame);
            const std::string where = std::string(language.tag) + " " + window.name + " Locate";
            expect_words_inside(drawing.canvas, locate->rect, locate->text, where.c_str());
            OA_CHECK(locate->text != "Locate another\xe2\x80\xa6" || language.words == nullptr);
        }
    }
    languages::set_interface_language(nullptr, languages::english());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: oa-ui-first-run-sizes-test INTERFACE_TDF CHINESE_TDF\n");
        return 2;
    }
    languages::InterfaceText catalogue;
    std::string error;
    const bool read = catalogue.add(read_file(argv[1]), &error);
    if (!read)
        std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
    OA_CHECK(read);
    languages::InterfaceText chinese;
    const bool read_chinese = chinese.add(read_file(argv[2]), &error);
    if (!read_chinese)
        std::fprintf(stderr, "%s: %s\n", argv[2], error.c_str());
    OA_CHECK(read_chinese);
    auto fonts = text_font::FontStack::open(text_font::bundled_font_directory());
    OA_CHECK(fonts != nullptr);
    if (fonts == nullptr)
        return oa::test::check_exit_status();

    const struct {
        const char* tag;
        const languages::InterfaceText* words;
    } spoken[] = {
        {"en", nullptr},
        {"de", &catalogue},
        {"zh-Hans", &chinese},
    };

    for (const auto& language : spoken) {
        const languages::Language* found = languages::find_by_tag(language.tag);
        OA_CHECK(found != nullptr);
        if (found == nullptr)
            continue;
        languages::set_interface_language(language.words, *found);
        for (const Window& window : kWindows) {
            for (const bool windowed : {true, false}) {
                first_run::Session steps[] = {
                    language_session(language.tag),
                    game_session(),
                    crowded_game_session(),
                    copying_session(),
                    full_copy_session(CopyStage::started),
                    full_copy_session(CopyStage::underway),
                    full_copy_session(CopyStage::checked),
                    crowded_game_session(),
                    full_copy_session(CopyStage::started),
                    language_session(language.tag),
                };
                // With a controller its hint takes the band's last line.
                steps[7].model.controller = true;
                steps[8].model.controller = true;
                steps[9].model.step = first_run::Step::online;
                for (first_run::Session& session : steps) {
                    session.model.windowed = windowed;
                    check_step(fonts.get(), window, session, language.tag);
                }
            }
        }
    }

    // The question a changed install asks, in every language the game
    // ships and in Chinese, whose pack does not have its words yet.
    const struct {
        const char* tag;
        const languages::InterfaceText* words;
    } asked[] = {
        {"en", nullptr},
        {"de", &catalogue},
        {"fr", &catalogue},
        {"es", &catalogue},
        {"it", &catalogue},
        {"zh-Hans", &chinese},
    };

    for (const auto& language : asked) {
        const languages::Language* found = languages::find_by_tag(language.tag);
        OA_CHECK(found != nullptr);
        if (found == nullptr)
            continue;
        languages::set_interface_language(language.words, *found);
        if (language.words == &catalogue) {
            const std::string choose(languages::interface_text("Choose another place"));
            OA_CHECK(choose != "Choose another place");
        }
        for (const Window& window : kWindows) {
            for (const bool windowed : {true, false}) {
                for (const Leave leave : {Leave::quit, Leave::skip}) {
                    for (const bool many : {false, true}) {
                        first_run::Session session = changed_session(many);
                        session.model.windowed = windowed;
                        session.model.leave = leave;
                        check_changed(fonts.get(), window, session, language.tag);
                    }
                }
            }
        }
    }
    languages::set_interface_language(nullptr, languages::english());
    check_header(fonts.get());
    check_locate(fonts.get(), catalogue, chinese);
    return oa::test::check_exit_status();
}
