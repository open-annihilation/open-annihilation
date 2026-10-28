// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Game files screen's model: every step and sheet laid out at the review
// sizes (button heights, the safe area, text inside its box as the bundled
// fonts measure it, no overlaps), the hit test, the focus order and keys,
// every press's command, the switches, scrolling, and the texts with the
// engine's neutral words and with a platform's words. Every layout of every
// case at every review size keeps the digest layout-digests.txt records, the
// file the test is given; --write-digests prints that file's content.
#include "oa/base/sha256.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/game_files.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdint.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace game_files = oa::ui::game_files;
namespace kit = oa::ui::kit;
namespace text_font = oa::platform::text_font;

using game_files::Command;
using game_files::Control;
using game_files::ControlKind;
using game_files::Layout;
using kit::Item;
using kit::Role;
using game_files::Model;
using game_files::PartKind;
using game_files::PartRow;
using game_files::Problem;
using game_files::Sheet;
using game_files::Step;

/// Counts a failed check with a description of the case.
///
/// @param condition what must hold
/// @param what the case and the expectation, printed when it fails
/// @param line the check's line
void expect(bool condition, const std::string& what, int line) {
    if (!condition)
        oa::test::report_failed_check(__FILE__, line, what.c_str());
}

#define OA_EXPECT(condition, what) expect((condition), (what), __LINE__)

/// Returns the bundled fonts, opened once from beside the test.
///
/// @return the font stack; null when the fonts are missing
text_font::FontStack* fonts() {
    static std::unique_ptr<text_font::FontStack> stack =
        text_font::FontStack::open(text_font::bundled_font_directory());
    return stack.get();
}

/// Makes the style the screen's painter draws with.
///
/// @param size pixel size
/// @param bold bold
/// @return the style
text_font::Style style_of(int size, bool bold) {
    text_font::Style style{};
    style.pixel_size = std::clamp(size, 1, text_font::max_pixel_size);
    style.weight = bold ? text_font::Weight::bold : text_font::Weight::regular;
    style.rendering = text_font::Rendering::antialiased;
    return style;
}

/// Measures a line with the bundled fonts, as the painter does.
///
/// @param context the font stack
/// @param text the line
/// @param size pixel size
/// @param bold bold
/// @return pixels
int font_width(void* context, std::string_view text, int size, bool bold) {
    auto* stack = static_cast<text_font::FontStack*>(context);
    if (text.empty())
        return 0;
    const auto placed = stack->layout(text, style_of(size, bold));
    if (!placed || placed->empty())
        return 0;
    return placed->back().pen + placed->back().advance;
}

/// Gives a line's height with the bundled fonts, as the painter does.
///
/// @param context the font stack
/// @param size pixel size
/// @param bold bold
/// @return pixels
int font_line(void* context, int size, bool bold) {
    auto* stack = static_cast<text_font::FontStack*>(context);
    const auto metrics = stack->metrics(style_of(size, bold));
    return metrics ? metrics->ascent + metrics->descent : 0;
}

/// Returns the measure the tests lay out with: the bundled fonts, else the default.
///
/// @return the hooks
game_files::TextMeasureHooks measure() {
    game_files::TextMeasureHooks hooks{};
    if (fonts() != nullptr) {
        hooks.context = fonts();
        hooks.width = font_width;
        hooks.line_height = font_line;
    }
    return hooks;
}

/// A review size.
struct Form {
    std::string name{};          ///< for the messages
    game_files::Viewport view{}; ///< the canvas
};

/// Returns a viewport of a size in points.
///
/// @param width points
/// @param height points
/// @param scale pixels per point
/// @param left the safe area's left inset, points
/// @param right its right inset, points
/// @param bottom its bottom inset, points
/// @return the viewport
game_files::Viewport
viewport_of(int width, int height, float scale, int left = 0, int right = 0, int bottom = 0) {
    game_files::Viewport view{};
    view.width = static_cast<int>(static_cast<float>(width) * scale);
    view.height = static_cast<int>(static_cast<float>(height) * scale);
    view.px_per_point = scale;
    view.safe.left = static_cast<int>(static_cast<float>(left) * scale);
    view.safe.right = static_cast<int>(static_cast<float>(right) * scale);
    view.safe.bottom = static_cast<int>(static_cast<float>(bottom) * scale);
    return view;
}

/// The review sizes: the tablet, the phone with its safe area (at 3 and 2 pixels a point)
/// and the 640x480 window.
///
/// @return the forms
std::vector<Form> forms() {
    return {
        {"tablet 1194x834@2", viewport_of(1194, 834, 2.0f)},
        {"phone 852x393@3", viewport_of(852, 393, 3.0f, 59, 59, 21)},
        {"phone 852x393@2", viewport_of(852, 393, 2.0f, 59, 59, 21)},
        {"window 640x480@1", viewport_of(640, 480, 1.0f)},
    };
}

/// Makes a part row.
///
/// @param kind the part
/// @param files its files' note
/// @param bytes its size
/// @param has_switch it has a switch on S3
/// @return the row
PartRow part(PartKind kind, std::string files, uint64_t bytes, bool has_switch = false) {
    PartRow row{};
    row.kind = kind;
    row.files = std::move(files);
    row.bytes = bytes;
    row.has_switch = has_switch;
    return row;
}

/// The parts of the test folder as S3 shows them, a refused mod among them.
///
/// @return the rows
std::vector<PartRow> folder_parts() {
    std::vector<PartRow> rows;
    rows.push_back(part(
        PartKind::game_archives,
        "totala1.hpi, totala2.hpi, totala3.hpi, totala4.hpi, worlds.hpi",
        742'000'000
    ));
    rows.push_back(part(PartKind::update_31c, "rev31.gp3", 3'000'000));
    rows.push_back(
        part(PartKind::core_contingency, "ccdata.ccx, ccmaps.ccx, ccmiss.ccx", 131'000'000, true)
    );
    rows.push_back(
        part(PartKind::battle_tactics, "btdata.ccx, btmaps.ccx, tactics1-8.hpi", 154'000'000, true)
    );
    PartRow extra = part(PartKind::extra, {}, 18'000'000, true);
    extra.count = 11;
    rows.push_back(extra);
    PartRow music = part(PartKind::music, {}, 61'000'000, true);
    music.count = 16;
    rows.push_back(music);
    rows.push_back(part(PartKind::movies, {}, 6'000'000, true));
    PartRow mod = part(PartKind::mod, "mods/big-guns", 2'000'000, true);
    mod.name = "Big Guns 1.2";
    rows.push_back(mod);
    PartRow broken = part(PartKind::mod, "mods/old-balance", 1'000'000, true);
    broken.name = "old-balance";
    broken.mark = game_files::Mark::refused;
    broken.errors = {
        "oamod.yaml: line 3: requires names a mod that is not installed",
        "oamod.yaml: line 7: unknown key colour",
    };
    rows.push_back(broken);
    return rows;
}

/// The model every case starts from: every capability, the version, the free space.
///
/// @return the model
Model base_model() {
    Model model{};
    model.version = "v0.6";
    model.offers_pick_folder = true;
    model.offers_pick_files = true;
    model.offers_copy_yourself = true;
    model.free_bytes = 38'000'000'000;
    model.free_known = true;
    model.location = "TA-DISK › Games › Total Annihilation";
    return model;
}

/// A model of S3 for the test folder.
///
/// @return the model
Model ready_model() {
    Model model = base_model();
    model.step = Step::ready_to_copy;
    model.parts = folder_parts();
    for (int index = 0; index < 44; ++index) {
        game_files::LeftOutRow row{};
        row.path = "Setup/Files/component-" + std::to_string(index) + ".dll";
        row.bytes = 248'000;
        row.reason = static_cast<uint8_t>(index % 7);
        model.left_out.push_back(row);
    }
    model.left_out_bytes = 10'900'000;
    model.warnings = {"Music/2.mp3 and music/2.mp3 differ only in capital letters; the game reads "
                      "one of them. The first is copied."};
    model.copy_bytes = 1'117'000'000;
    model.need_bytes = 1'317'000'000;
    model.source_checked = true;
    model.replace_bytes = 1'100'000'000;
    return model;
}

/// A model of S4 part way through.
///
/// @return the model
Model copying_model() {
    Model model = ready_model();
    model.step = Step::copying;
    model.parts.resize(7);
    const std::array<game_files::Mark, 7> marks{
        game_files::Mark::done,
        game_files::Mark::done,
        game_files::Mark::done,
        game_files::Mark::copying,
        game_files::Mark::waiting,
        game_files::Mark::waiting,
        game_files::Mark::waiting,
    };
    for (std::size_t index = 0; index < marks.size(); ++index)
        model.parts[index].mark = marks[index];
    model.progress.done_bytes = 690'000'000;
    model.progress.total_bytes = 1'117'000'000;
    model.progress.current = "btmaps.ccx";
    model.progress.downloading = true;
    model.progress.seconds_left = 64;
    return model;
}

/// A model of S8 with the test folder installed.
///
/// @return the model
Model manage_model() {
    Model model = base_model();
    model.step = Step::manage;
    model.management = true;
    model.parts = folder_parts();
    model.parts.pop_back();
    for (std::size_t index = 1; index < model.parts.size(); ++index)
        model.parts[index].removable = true;
    PartRow demo = part(PartKind::demo_data, {}, 20'500'000);
    demo.mark = game_files::Mark::warning;
    model.parts.push_back(demo);
    model.installed_bytes = 1'117'000'000;
    model.free_bytes = 37'000'000'000;
    return model;
}

/// A named model.
struct Case {
    std::string name{}; ///< for the messages
    Model model{};      ///< the model
};

/// Returns every step, banner, problem and sheet with realistic contents.
///
/// @return the cases
std::vector<Case> every_case() {
    std::vector<Case> cases;
    Model first = base_model();
    first.location.clear();
    cases.push_back({"S1", first});
    Model two_cards = first;
    two_cards.offers_copy_yourself = false;
    cases.push_back({"S1 two cards", two_cards});
    Model one_card = first;
    one_card.offers_pick_folder = false;
    cases.push_back({"S1 copy it yourself only", one_card});
    Model resume = base_model();
    resume.banner = game_files::Banner::continue_copy;
    resume.location = "Cloud Drive › Games › Total Annihilation";
    resume.stopped_bytes = 690'000'000;
    resume.stopped_total = 1'100'000'000;
    cases.push_back({"S1 continue banner", resume});
    Model refused = first;
    refused.banner = game_files::Banner::folder_refused;
    refused.detail = "Its archives lack gamedata/sidedata.tdf.";
    cases.push_back({"S1 refused banner", refused});
    Model nothing = first;
    nothing.banner = game_files::Banner::nothing_added;
    cases.push_back({"S1 nothing added", nothing});

    Model looking = base_model();
    looking.step = Step::looking;
    cases.push_back({"S2 looking, no count yet", looking});
    looking.listed_files = 143;
    looking.listed_bytes = 1'100'000'000;
    cases.push_back({"S2 looking", looking});
    Model nested = base_model();
    nested.step = Step::nested_offer;
    nested.location = "Cloud Drive › Games";
    nested.nested = {"Games/Total Annihilation"};
    cases.push_back({"S2 nested, one", nested});
    nested.nested = {"Total Annihilation", "TA backup 1998", "Old/TA 3.1 copy"};
    cases.push_back({"S2 nested, three", nested});
    Model there = base_model();
    there.step = Step::already_there;
    cases.push_back({"S2 already there", there});

    Model ready = ready_model();
    cases.push_back({"S3", ready});
    Model replace = ready;
    replace.replace = true;
    cases.push_back({"S3 replace", replace});
    Model short_space = ready;
    short_space.space_short = true;
    short_space.free_bytes = 640'000'000;
    short_space.fitting_off = {PartKind::battle_tactics, PartKind::core_contingency};
    short_space.remote_bytes = 312'000'000;
    short_space.source_checked = false;
    short_space.source_check_skipped = true;
    short_space.parts[1].mark = game_files::Mark::missing;
    short_space.sizes_unknown = true;
    cases.push_back({"S3 short space, cloud, missing update", short_space});
    Model demo = base_model();
    demo.step = Step::ready_to_copy;
    demo.demo = true;
    demo.copy_bytes = 21'540'864;
    cases.push_back({"S3 demo", demo});

    Model copying = copying_model();
    cases.push_back({"S4", copying});
    copying.banner = game_files::Banner::copy_went_on;
    copying.progress.seconds_left.reset();
    copying.progress.downloading = false;
    cases.push_back({"S4 back from away", copying});
    Model resumed = copying_model();
    resumed.banner = game_files::Banner::copy_resumed;
    cases.push_back({"S4 resumed", resumed});

    Model checking = base_model();
    checking.step = Step::checking;
    cases.push_back({"S5 checking", checking});
    checking.demo = true;
    cases.push_back({"S5 unpacking", checking});
    Model play = base_model();
    play.step = Step::ready_to_play;
    play.ready_parts = {true, true, true, true, false, true, true, false, false, false, false};
    play.uses_bytes = 1'100'000'000;
    play.free_bytes = 37'000'000'000;
    play.skipped_archives = {"totala2.hpi"};
    play.old_folder_bytes = 1'100'000'000;
    cases.push_back({"S5 ready to play", play});
    Model play_plain = play;
    play_plain.skipped_archives.clear();
    play_plain.old_folder_bytes = 0;
    play_plain.backed_up = true;
    cases.push_back({"S5 ready to play, plain", play_plain});

    for (uint8_t value = 0; value <= static_cast<uint8_t>(Problem::staging_unwritable); ++value) {
        Model problem = base_model();
        problem.step = Step::problem;
        problem.problem = static_cast<Problem>(value);
        problem.detail = "Its archives lack guis/mainmenu.gui, gamedata/sidedata.tdf.";
        problem.file = "ccmaps.ccx";
        problem.stopped_bytes = 690'000'000;
        problem.need_bytes = 1'300'000'000;
        problem.free_bytes = 640'000'000;
        problem.copy_bytes = 48'000'000'000;
        cases.push_back({"S6 problem " + std::to_string(value), problem});
    }

    Model manage = manage_model();
    cases.push_back({"S8", manage});
    Model added = manage;
    added.banner = game_files::Banner::added;
    added.added_files = 12;
    added.added_from = "Maps from the forum";
    added.added_kept = {"totala1.hpi"};
    cases.push_back({"S8 added", added});
    Model removals = manage;
    removals.banner = game_files::Banner::next_start;
    removals.pending_removals = {"Music", "Battle Tactics"};
    cases.push_back({"S8 next start", removals});
    Model pending = manage;
    pending.pending_replacement = true;
    cases.push_back({"S8 pending replacement", pending});

    Model stop = copying_model();
    stop.sheet = Sheet::stop;
    cases.push_back({"sheet stop", stop});
    Model confirm = replace;
    confirm.sheet = Sheet::replace_confirm;
    cases.push_back({"sheet replace", confirm});
    Model left_out = ready;
    left_out.sheet = Sheet::left_out_list;
    cases.push_back({"sheet left out", left_out});
    Model why = ready;
    why.sheet = Sheet::mod_errors;
    why.sheet_part = 8;
    cases.push_back({"sheet mod errors", why});
    Model remove_old = play;
    remove_old.sheet = Sheet::remove_old_confirm;
    cases.push_back({"sheet remove old", remove_old});
    Model remove_part = manage;
    remove_part.sheet = Sheet::remove_part_confirm;
    remove_part.sheet_part = 3;
    cases.push_back({"sheet remove part", remove_part});
    Model remove_demo = manage;
    remove_demo.sheet = Sheet::remove_part_confirm;
    remove_demo.sheet_part = static_cast<uint16_t>(manage.parts.size() - 1);
    cases.push_back({"sheet remove demo data", remove_demo});
    Model remove_all = manage;
    remove_all.sheet = Sheet::remove_all_confirm;
    cases.push_back({"sheet remove all", remove_all});
    Model add = manage;
    add.sheet = Sheet::add_files;
    cases.push_back({"sheet add files", add});
    Model note = removals;
    note.sheet = Sheet::scheduled_note;
    cases.push_back({"sheet scheduled note", note});
    return cases;
}

/// Lays a model out at a form with the bundled fonts.
///
/// @param model the model
/// @param form the form
/// @return the layout
Layout lay_out(const Model& model, const Form& form) {
    return game_files::lay_out(model, form.view, measure());
}

/// Returns the part of an item that shows: its box cut by its clip.
///
/// @param item the item
/// @return the visible box; empty when none shows
game_files::Rect visible(const Item& item) {
    if (item.clip.width <= 0 || item.clip.height <= 0)
        return item.rect;
    const int left = std::max(item.rect.x, item.clip.x);
    const int top = std::max(item.rect.y, item.clip.y);
    const int right = std::min(item.rect.x + item.rect.width, item.clip.x + item.clip.width);
    const int bottom = std::min(item.rect.y + item.rect.height, item.clip.y + item.clip.height);
    if (right <= left || bottom <= top)
        return {};
    return {left, top, right - left, bottom - top};
}

/// Tells whether two boxes share any pixel.
///
/// @param a a box
/// @param b another box
/// @return true when they overlap
bool overlap(const game_files::Rect& a, const game_files::Rect& b) {
    return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0 && a.x < b.x + b.width &&
           b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

/// Tells whether a box lies inside another.
///
/// @param inner the box
/// @param outer the box it should lie in
/// @return true when it does
bool inside(const game_files::Rect& inner, const game_files::Rect& outer) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

/// Gives an item's level for the overlap check: panels, rows, or what is drawn on them.
///
/// @param role the item's role
/// @return 0 panels, 1 rows, 2 contents, -1 not checked
int level_of(Role role) {
    switch (role) {
    case Role::header_bar:
    case Role::panel:
    case Role::banner:
    case Role::sheet:
        return 0;
    case Role::row:
        return 1;
    case Role::divider:
    case Role::progress_fill:
    case Role::backdrop:
        return -1;
    default:
        return 2;
    }
}

/// Tells whether an item draws a control.
///
/// @param item the item
/// @return true when it carries a control's number
bool has_control_part(const Item& item) {
    return game_files::screen_control(item.control).kind != ControlKind::none;
}

/// Describes an item for the messages.
///
/// @param item the item
/// @return its role, control and first line
std::string describe(const Item& item) {
    const Control control = game_files::screen_control(item.control);
    std::string text = "role " + std::to_string(static_cast<int>(item.role)) + " control " +
                       std::to_string(static_cast<int>(control.kind)) + "/" +
                       std::to_string(control.index);
    if (!item.lines.empty())
        text += " '" + item.lines.front() + "'";
    text += " at " + std::to_string(item.rect.x) + "," + std::to_string(item.rect.y) + " " +
            std::to_string(item.rect.width) + "x" + std::to_string(item.rect.height);
    return text;
}

/// Checks one layout: button heights, the safe area, text inside its box, overlaps and the
/// focus order.
///
/// @param name the case and form
/// @param layout the layout
/// @param view the canvas
void check_layout(const std::string& name, const Layout& layout, const game_files::Viewport& view) {
    const game_files::Rect safe{
        view.safe.left,
        view.safe.top,
        view.width - view.safe.left - view.safe.right,
        view.height - view.safe.top - view.safe.bottom,
    };
    const float scale = view.px_per_point;
    const game_files::TextMeasureHooks hooks = measure();
    const auto width_of = [&](std::string_view text, int size, bool bold) {
        return hooks.width != nullptr
                   ? hooks.width(hooks.context, text, size, bold)
                   : static_cast<int>(0.55 * size * static_cast<double>(text.size()));
    };
    const auto line_of = [&](int size, bool bold) {
        return hooks.line_height != nullptr ? hooks.line_height(hooks.context, size, bold)
                                            : static_cast<int>(1.25 * size + 0.99);
    };
    const std::vector<Item>& items = layout.list.items;
    OA_EXPECT(!items.empty(), name + ": the layout is empty");
    std::size_t live = 0;
    for (std::size_t index = 0; index < items.size(); ++index)
        if (items[index].role == Role::backdrop)
            live = index + 1;
    for (std::size_t index = 0; index < items.size(); ++index) {
        const Item& item = items[index];
        if (item.role == Role::backdrop) {
            OA_EXPECT(
                item.rect.x == 0 && item.rect.y == 0 && item.rect.width == view.width &&
                    item.rect.height == view.height,
                name + ": the backdrop covers the canvas"
            );
            continue;
        }
        const game_files::Rect seen = visible(item);
        if (seen.width > 0)
            OA_EXPECT(inside(seen, safe), name + ": outside the safe area: " + describe(item));
        if (item.clip.width > 0)
            OA_EXPECT(
                inside(item.clip, safe), name + ": clip outside the safe area: " + describe(item)
            );
        if (has_control_part(item))
            OA_EXPECT(
                static_cast<float>(item.rect.height) + 1.0f >=
                    game_files::min_button_points * scale,
                name + ": control under 44 pt: " + describe(item)
            );
        for (const std::string& line : item.lines) {
            OA_EXPECT(!line.empty(), name + ": empty line: " + describe(item));
            OA_EXPECT(line.find('{') == std::string::npos, name + ": unfilled place: " + line);
        }
        if (item.lines.empty())
            continue;
        const int line_height = line_of(item.pixel_size, item.bold);
        switch (item.role) {
        case Role::button_main:
        case Role::button:
        case Role::button_danger: {
            OA_EXPECT(item.lines.size() == 1, name + ": a button has one line: " + describe(item));
            const int mark = item.glyph_size > 0 ? item.glyph_size : item.pixel_size;
            const int glyph = item.glyph == game_files::Glyph::none
                                  ? 0
                                  : mark + static_cast<int>(
                                               static_cast<float>(item.pixel_size) *
                                                   game_files::button_glyph_gap_em +
                                               0.5f
                                           );
            OA_EXPECT(
                mark <= item.rect.height, name + ": mark taller than its button: " + describe(item)
            );
            OA_EXPECT(
                width_of(item.lines.front(), item.pixel_size, item.bold) + glyph <= item.rect.width,
                name + ": label wider than its button: " + describe(item)
            );
            OA_EXPECT(
                line_height <= item.rect.height,
                name + ": label taller than its button: " + describe(item)
            );
            break;
        }
        case Role::toggle:
            OA_EXPECT(item.lines.size() == 2, name + ": a switch has two labels");
            for (const std::string& label : item.lines)
                OA_EXPECT(
                    width_of(label, item.pixel_size, item.bold) <= item.rect.width / 2,
                    name + ": switch label wider than its half: " + describe(item)
                );
            OA_EXPECT(line_height <= item.rect.height, name + ": switch label too tall");
            break;
        default:
            for (const std::string& line : item.lines)
                OA_EXPECT(
                    width_of(line, item.pixel_size, item.bold) <= item.rect.width,
                    name + ": line wider than its box: " + describe(item) + " line '" + line + "'"
                );
            OA_EXPECT(
                line_height * static_cast<int>(item.lines.size()) <= item.rect.height,
                name + ": lines taller than their box: " + describe(item)
            );
            break;
        }
    }
    for (std::size_t a = 0; a < items.size(); ++a) {
        const int level = level_of(items[a].role);
        if (level < 0)
            continue;
        const bool a_live = a >= live;
        for (std::size_t b = a + 1; b < items.size(); ++b) {
            if (level_of(items[b].role) != level || (b >= live) != a_live)
                continue;
            OA_EXPECT(
                !overlap(visible(items[a]), visible(items[b])),
                name + ": overlap: " + describe(items[a]) + " / " + describe(items[b])
            );
        }
    }
    for (const Control& control : layout.focus_order) {
        bool found = false;
        for (std::size_t index = live; index < items.size(); ++index)
            found = found || (game_files::screen_control(items[index].control) == control &&
                              !items[index].state.disabled);
        OA_EXPECT(found, name + ": a focused control is not on the layout");
    }
    for (std::size_t index = live; index < items.size(); ++index) {
        const Item& item = items[index];
        if (!has_control_part(item) || item.state.disabled)
            continue;
        OA_EXPECT(
            std::find(
                layout.focus_order.begin(),
                layout.focus_order.end(),
                game_files::screen_control(item.control)
            ) != layout.focus_order.end(),
            name + ": a control is missing from the focus order: " + describe(item)
        );
    }
    // The kit's controls are the live parts that draw one, the top one first, each where its
    // part is; Tab follows the focus order; and no two live parts draw the same control.
    std::vector<kit::Control> expected_controls;
    for (std::size_t index = items.size(); index-- > live;)
        if (has_control_part(items[index]))
            expected_controls.push_back(
                kit::Control{
                    items[index].control,
                    items[index].rect,
                    items[index].clip,
                    !items[index].state.disabled
                }
            );
    OA_EXPECT(
        layout.list.controls.size() == expected_controls.size(),
        name + ": one control for each live part that draws one"
    );
    for (std::size_t index = 0;
         index < std::min(layout.list.controls.size(), expected_controls.size());
         ++index) {
        const kit::Control& got = layout.list.controls[index];
        const kit::Control& wanted = expected_controls[index];
        OA_EXPECT(
            got.id == wanted.id && inside(got.rect, wanted.rect) && inside(wanted.rect, got.rect) &&
                got.clip.x == wanted.clip.x && got.clip.y == wanted.clip.y &&
                got.clip.width == wanted.clip.width && got.clip.height == wanted.clip.height &&
                got.enabled == wanted.enabled,
            name + ": a control lies where its part is"
        );
        for (std::size_t other = index + 1; other < expected_controls.size(); ++other)
            OA_EXPECT(
                expected_controls[other].id != wanted.id, name + ": two parts draw one control"
            );
    }
    OA_EXPECT(layout.list.tab_order.size() == layout.focus_order.size(), name + ": the Tab order");
    for (std::size_t index = 0;
         index < std::min(layout.list.tab_order.size(), layout.focus_order.size());
         ++index)
        OA_EXPECT(
            game_files::screen_control(layout.list.tab_order[index]) == layout.focus_order[index],
            name + ": the Tab order follows the focus order"
        );
    OA_EXPECT(layout.scroll_max_points >= 0, name + ": negative scroll");
    if (layout.scroll_max_points > 0)
        OA_EXPECT(inside(layout.rows, safe), name + ": the rows lie outside the safe area");
}

/// Every step and sheet lays out within the rules at every review size, scrolled to the top
/// and to the bottom.
void every_step_and_sheet_lays_out() {
    for (const Case& test : every_case()) {
        for (const Form& form : forms()) {
            const std::string name = test.name + " at " + form.name;
            const Layout layout = lay_out(test.model, form);
            check_layout(name, layout, form.view);
            OA_EXPECT(
                layout.device == (form.name.rfind("phone", 0) == 0
                                      ? game_files::DeviceClass::phone
                                      : game_files::DeviceClass::tablet),
                name + ": device class"
            );
            if (layout.scroll_max_points > 0) {
                Model scrolled = test.model;
                scrolled.scroll_points = layout.scroll_max_points;
                check_layout(name + " scrolled", lay_out(scrolled, form), form.view);
            }
        }
    }
}

/// The tablet and phone review sizes take their forms from their size in points.
void review_sizes_take_their_forms() {
    OA_CHECK(
        game_files::device_class(viewport_of(1194, 834, 2.0f)) == game_files::DeviceClass::tablet
    );
    OA_CHECK(
        game_files::device_class(viewport_of(852, 393, 3.0f)) == game_files::DeviceClass::phone
    );
    OA_CHECK(
        game_files::device_class(viewport_of(852, 393, 2.0f)) == game_files::DeviceClass::phone
    );
    OA_CHECK(
        game_files::device_class(viewport_of(640, 480, 1.0f)) == game_files::DeviceClass::tablet
    );
    OA_CHECK(
        game_files::device_class(viewport_of(393, 852, 3.0f)) == game_files::DeviceClass::phone
    );
    OA_CHECK(
        game_files::device_class(viewport_of(800, 459, 1.0f)) == game_files::DeviceClass::phone
    );
    OA_CHECK(
        game_files::device_class(viewport_of(800, 460, 1.0f)) == game_files::DeviceClass::tablet
    );
}

/// Returns whether a layout holds a control.
///
/// @param layout the layout
/// @param control the control
/// @return true when a live item has it
bool has_control(const Layout& layout, Control control) {
    const std::vector<Item>& items = layout.list.items;
    std::size_t live = 0;
    for (std::size_t index = 0; index < items.size(); ++index)
        if (items[index].role == Role::backdrop)
            live = index + 1;
    for (std::size_t index = live; index < items.size(); ++index)
        if (game_files::screen_control(items[index].control) == control)
            return true;
    return false;
}

/// Returns a control's item.
///
/// @param layout the layout
/// @param control the control
/// @return the last item with it (a sheet's over the step's); null when none
const Item* item_of(const Layout& layout, Control control) {
    const Item* found = nullptr;
    for (const Item& item : layout.list.items)
        if (game_files::screen_control(item.control) == control)
            found = &item;
    return found;
}

/// Makes a button item for the hit tests.
///
/// @param box its box
/// @param control its control
/// @param enabled can be pressed
/// @return the item
Item hit_item(game_files::Rect box, Control control, bool enabled = true) {
    Item item{};
    item.role = Role::button;
    item.rect = box;
    item.control = game_files::control_id(control);
    item.state.disabled = !enabled;
    return item;
}

/// Lists a hand-made layout's controls from its parts, as lay_out does.
///
/// @param layout the layout
/// @return the layout with its controls and Tab order
Layout listed(Layout layout) {
    kit::list_part_controls(layout.list);
    return layout;
}

/// The hit test takes the containing control, else the nearest within reach, skips
/// disabled controls, the clipped-off parts and everything under a backdrop.
void hit_test_finds_the_nearest_enabled_control() {
    const Control a{ControlKind::choose_folder};
    const Control b{ControlKind::choose_installer};
    const Control c{ControlKind::i_have_copied};
    Layout made{};
    made.list.items.push_back(hit_item({100, 100, 100, 50}, a));
    made.list.items.push_back(hit_item({300, 100, 100, 50}, b));
    made.list.items.push_back(hit_item({100, 300, 100, 50}, c, false));
    const Layout layout = listed(made);
    const float reach = 22.0f;
    OA_CHECK(game_files::hit_test(layout, {150, 120}, reach) == a);
    OA_CHECK(game_files::hit_test(layout, {399, 149}, reach) == b);
    OA_CHECK(game_files::hit_test(layout, {85, 120}, reach) == a);
    OA_CHECK(game_files::hit_test(layout, {150, 165}, reach) == a);
    OA_CHECK(game_files::hit_test(layout, {150, 175}, reach).kind == ControlKind::none);
    // Between the two: 51 px from a's right edge, 50 px from b's left edge.
    OA_CHECK(game_files::hit_test(layout, {250, 120}, 60.0f) == b);
    OA_CHECK(game_files::hit_test(layout, {250, 120}, reach).kind == ControlKind::none);
    // The disabled control is skipped, inside it and near it.
    OA_CHECK(game_files::hit_test(layout, {150, 320}, reach).kind == ControlKind::none);
    OA_CHECK(game_files::hit_test(layout, {150, 290}, reach).kind == ControlKind::none);
    // A clipped part does not take presses.
    Layout clipping = made;
    clipping.list.items[0].clip = {100, 100, 100, 20};
    const Layout clipped = listed(clipping);
    OA_CHECK(game_files::hit_test(clipped, {150, 140}, 5.0f).kind == ControlKind::none);
    OA_CHECK(game_files::hit_test(clipped, {150, 110}, 5.0f) == a);
    // A backdrop makes everything under it inert; the sheet's button above it answers.
    Layout covered = made;
    Item backdrop{};
    backdrop.role = Role::backdrop;
    backdrop.rect = {0, 0, 1000, 1000};
    covered.list.items.push_back(backdrop);
    const Control option{ControlKind::sheet_option, 0};
    covered.list.items.push_back(hit_item({500, 500, 100, 50}, option));
    const Layout sheet = listed(covered);
    OA_CHECK(game_files::hit_test(sheet, {150, 120}, reach).kind == ControlKind::none);
    OA_CHECK(game_files::hit_test(sheet, {510, 490}, reach) == option);
    // On a real layout: S1's main button is found at its centre and just outside it.
    const Form tablet = forms().front();
    const Layout first = lay_out(base_model(), tablet);
    const Item* choose = item_of(first, {ControlKind::choose_folder});
    OA_CHECK(choose != nullptr);
    if (choose != nullptr) {
        const game_files::Point centre{
            choose->rect.x + choose->rect.width / 2, choose->rect.y + choose->rect.height / 2
        };
        const float real_reach = game_files::pick_reach_points * tablet.view.px_per_point;
        OA_CHECK(
            game_files::hit_test(first, centre, real_reach) ==
            game_files::screen_control(choose->control)
        );
        const game_files::Point below{centre.x, choose->rect.y + choose->rect.height + 20};
        OA_CHECK(
            game_files::hit_test(first, below, real_reach) ==
            game_files::screen_control(choose->control)
        );
    }
}

/// Presses a control (down and up) and returns the outcome.
///
/// @param[in,out] model the model
/// @param form the form it is laid out at
/// @param control the control
/// @return what the app must do
game_files::Outcome press(Model& model, const Form& form, Control control) {
    game_files::Interaction interaction{};
    const Layout layout = lay_out(model, form);
    static_cast<void>(game_files::press_down(model, interaction, layout, control));
    const Item* item = item_of(layout, control);
    OA_EXPECT(
        game_files::screen_control(interaction.pressed) == control ||
            !has_control(layout, control) || (item != nullptr && item->state.disabled),
        "the press shows on the control"
    );
    return game_files::press_up(model, interaction, layout, control);
}

/// The header shows the OA mark as the Open Annihilation icon: the badge is a box with no
/// text, and OA · Aa a button with the icon beside "Aa", the icon its own size.
void the_header_shows_the_icon() {
    for (const Form& form : forms()) {
        const Layout layout = lay_out(base_model(), form);
        const float scale = form.view.px_per_point;
        int badges = 0;
        for (const Item& item : layout.list.items) {
            if (item.role != Role::badge)
                continue;
            ++badges;
            OA_EXPECT(item.lines.empty(), form.name + ": the badge holds text");
            OA_EXPECT(item.glyph == game_files::Glyph::oa, form.name + ": the badge's mark");
            OA_EXPECT(item.rect.width == item.rect.height, form.name + ": the badge is square");
        }
        OA_EXPECT(badges == 1, form.name + ": one badge");
        const Item* language = item_of(layout, {ControlKind::language});
        OA_CHECK(language != nullptr);
        if (language == nullptr)
            continue;
        OA_EXPECT(language->glyph == game_files::Glyph::oa, form.name + ": OA · Aa's mark");
        OA_EXPECT(
            language->lines == std::vector<std::string>{"Aa"}, form.name + ": OA · Aa's label"
        );
        // The phone's sizes, which a window under 600 points high takes too.
        const bool compact =
            layout.device == game_files::DeviceClass::phone ||
            static_cast<float>(std::min(form.view.width, form.view.height)) / scale < 600.0f;
        const int side = static_cast<int>(std::lround((compact ? 18.0f : 20.0f) * scale));
        OA_EXPECT(language->glyph_size == side, form.name + ": OA · Aa's icon size");
    }
    // The management state has the badge but no OA · Aa.
    const Layout managing = lay_out(manage_model(), forms().front());
    OA_CHECK(
        std::any_of(managing.list.items.begin(), managing.list.items.end(), [](const Item& item) {
            return item.role == Role::badge && item.lines.empty();
        })
    );
}

/// Every press returns its command; the UI opens and closes its sheets itself.
void presses_return_their_commands() {
    const Form tablet = forms().front();
    const Form phone = forms()[1];
    // S1.
    Model first = base_model();
    OA_CHECK(
        press(first, tablet, {ControlKind::choose_folder}).command == Command::pick_game_folder
    );
    OA_CHECK(press(first, tablet, {ControlKind::i_have_copied}).command == Command::check_copied);
    OA_CHECK(
        press(first, tablet, {ControlKind::choose_installer}).command == Command::pick_installer
    );
    OA_CHECK(press(first, tablet, {ControlKind::language}).command == Command::open_language);
    OA_CHECK(
        press(first, phone, {ControlKind::choose_folder}).command == Command::pick_game_folder
    );
    OA_CHECK(!has_control(lay_out(manage_model(), tablet), {ControlKind::language}));
    Model refused = first;
    refused.banner = game_files::Banner::folder_refused;
    OA_CHECK(press(refused, tablet, {ControlKind::banner_action}).command == Command::check_copied);
    Model resume = first;
    resume.banner = game_files::Banner::continue_copy;
    OA_CHECK(
        press(resume, tablet, {ControlKind::choose_folder, 1}).command == Command::pick_game_folder
    );
    OA_CHECK(press(resume, tablet, {ControlKind::banner_discard}).command == Command::discard);
    Model no_copy_yourself = first;
    no_copy_yourself.offers_copy_yourself = false;
    OA_CHECK(!has_control(lay_out(no_copy_yourself, tablet), {ControlKind::i_have_copied}));
    // A press slid off, or onto another control, does nothing.
    {
        Model model = base_model();
        game_files::Interaction interaction{};
        const Layout layout = lay_out(model, tablet);
        static_cast<void>(
            game_files::press_down(model, interaction, layout, {ControlKind::choose_folder})
        );
        OA_CHECK(game_files::press_up(model, interaction, layout, {}).command == Command::redraw);
        static_cast<void>(
            game_files::press_down(model, interaction, layout, {ControlKind::choose_folder})
        );
        OA_CHECK(
            game_files::press_up(model, interaction, layout, {ControlKind::choose_installer})
                .command == Command::redraw
        );
        OA_CHECK(interaction.pressed == kit::no_control);
    }
    // S2.
    Model looking = base_model();
    looking.step = Step::looking;
    OA_CHECK(press(looking, tablet, {ControlKind::cancel}).command == Command::cancel_scan);
    Model nested = base_model();
    nested.step = Step::nested_offer;
    nested.nested = {"Total Annihilation", "TA backup 1998"};
    const game_files::Outcome use = press(nested, phone, {ControlKind::nested_choice, 1});
    OA_CHECK(use.command == Command::use_nested && use.index == 1);
    OA_CHECK(
        press(nested, phone, {ControlKind::choose_folder}).command == Command::pick_game_folder
    );
    Model there = base_model();
    there.step = Step::already_there;
    OA_CHECK(press(there, tablet, {ControlKind::check_it}).command == Command::check_in_place);
    // S3: switches, Why, Show, Copy and the replace confirm.
    Model ready = ready_model();
    OA_CHECK(press(ready, tablet, {ControlKind::part_switch, 2}).command == Command::redraw);
    OA_CHECK(!ready.parts[2].on);
    OA_CHECK(press(ready, tablet, {ControlKind::part_switch, 2}).command == Command::redraw);
    OA_CHECK(ready.parts[2].on);
    OA_CHECK(!has_control(lay_out(ready, tablet), {ControlKind::part_switch, 0}));
    OA_CHECK(press(ready, tablet, {ControlKind::part_why, 8}).command == Command::redraw);
    OA_CHECK(ready.sheet == Sheet::mod_errors && ready.sheet_part == 8);
    OA_CHECK(press(ready, tablet, {ControlKind::sheet_option, 0}).command == Command::redraw);
    OA_CHECK(ready.sheet == Sheet::none);
    OA_CHECK(press(ready, tablet, {ControlKind::show_left_out}).command == Command::redraw);
    OA_CHECK(ready.sheet == Sheet::left_out_list);
    OA_CHECK(press(ready, phone, {ControlKind::sheet_option, 0}).command == Command::redraw);
    OA_CHECK(ready.sheet == Sheet::none);
    OA_CHECK(press(ready, tablet, {ControlKind::copy}).command == Command::start_copy);
    OA_CHECK(
        press(ready, tablet, {ControlKind::choose_folder}).command == Command::pick_game_folder
    );
    Model replace = ready;
    replace.replace = true;
    OA_CHECK(press(replace, tablet, {ControlKind::copy}).command == Command::redraw);
    OA_CHECK(replace.sheet == Sheet::replace_confirm);
    OA_CHECK(press(replace, tablet, {ControlKind::sheet_option, 1}).command == Command::redraw);
    OA_CHECK(replace.sheet == Sheet::none);
    OA_CHECK(press(replace, tablet, {ControlKind::copy}).command == Command::redraw);
    OA_CHECK(press(replace, tablet, {ControlKind::sheet_option, 0}).command == Command::start_copy);
    OA_CHECK(replace.sheet == Sheet::none);
    Model short_space = ready;
    short_space.space_short = true;
    OA_CHECK(press(short_space, tablet, {ControlKind::copy}).command == Command::none);
    const Layout short_layout = lay_out(short_space, tablet);
    const Item* copy = item_of(short_layout, {ControlKind::copy});
    OA_CHECK(copy != nullptr && copy->state.disabled);
    OA_CHECK(
        press(short_space, tablet, {ControlKind::check_space}).command == Command::recheck_space
    );
    Model demo = base_model();
    demo.step = Step::ready_to_copy;
    demo.demo = true;
    OA_CHECK(
        press(demo, tablet, {ControlKind::choose_installer}).command == Command::pick_installer
    );
    OA_CHECK(press(demo, tablet, {ControlKind::copy}).command == Command::start_copy);
    // S4 and its Stop sheet.
    Model copying = copying_model();
    OA_CHECK(press(copying, tablet, {ControlKind::stop}).command == Command::redraw);
    OA_CHECK(copying.sheet == Sheet::stop);
    OA_CHECK(press(copying, tablet, {ControlKind::sheet_option, 2}).command == Command::redraw);
    OA_CHECK(copying.sheet == Sheet::none);
    static_cast<void>(press(copying, phone, {ControlKind::stop}));
    OA_CHECK(press(copying, phone, {ControlKind::sheet_option, 0}).command == Command::stop_keep);
    OA_CHECK(copying.sheet == Sheet::none);
    static_cast<void>(press(copying, tablet, {ControlKind::stop}));
    OA_CHECK(
        press(copying, tablet, {ControlKind::sheet_option, 1}).command == Command::stop_discard
    );
    // Under a sheet, the step's controls do nothing.
    static_cast<void>(press(copying, tablet, {ControlKind::stop}));
    OA_CHECK(press(copying, tablet, {ControlKind::stop}).command == Command::none);
    OA_CHECK(copying.sheet == Sheet::stop);
    // S5.
    Model play = base_model();
    play.step = Step::ready_to_play;
    play.old_folder_bytes = 1'100'000'000;
    OA_CHECK(press(play, tablet, {ControlKind::play}).command == Command::play);
    OA_CHECK(press(play, tablet, {ControlKind::remove_old}).command == Command::redraw);
    OA_CHECK(play.sheet == Sheet::remove_old_confirm);
    OA_CHECK(press(play, tablet, {ControlKind::sheet_option, 0}).command == Command::remove_old);
    OA_CHECK(play.sheet == Sheet::none);
    // In the management state, Check again's report goes back to the list with Done.
    Model checked = play;
    checked.management = true;
    const Layout checked_layout = lay_out(checked, tablet);
    OA_CHECK(!has_control(checked_layout, {ControlKind::play}));
    OA_CHECK(press(checked, tablet, {ControlKind::back}).command == Command::back);
    game_files::Interaction keys{};
    OA_CHECK(
        game_files::key(checked, keys, checked_layout, game_files::Key::escape).command ==
        Command::back
    );
    // S8.
    Model manage = manage_model();
    OA_CHECK(press(manage, tablet, {ControlKind::manage_check}).command == Command::manage_check);
    OA_CHECK(
        press(manage, tablet, {ControlKind::manage_replace}).command == Command::manage_replace
    );
    OA_CHECK(press(manage, tablet, {ControlKind::manage_done}).command == Command::done);
    OA_CHECK(!has_control(lay_out(manage, tablet), {ControlKind::manage_remove, 0}));
    OA_CHECK(press(manage, tablet, {ControlKind::manage_add}).command == Command::redraw);
    OA_CHECK(manage.sheet == Sheet::add_files);
    OA_CHECK(
        press(manage, tablet, {ControlKind::sheet_option, 0}).command ==
        Command::pick_additions_folder
    );
    static_cast<void>(press(manage, tablet, {ControlKind::manage_add}));
    OA_CHECK(
        press(manage, tablet, {ControlKind::sheet_option, 1}).command == Command::pick_archives
    );
    static_cast<void>(press(manage, tablet, {ControlKind::manage_add}));
    OA_CHECK(
        press(manage, tablet, {ControlKind::sheet_option, 2}).command == Command::pick_installer
    );
    static_cast<void>(press(manage, tablet, {ControlKind::manage_add}));
    OA_CHECK(press(manage, tablet, {ControlKind::sheet_option, 3}).command == Command::redraw);
    OA_CHECK(manage.sheet == Sheet::none);
    OA_CHECK(press(manage, phone, {ControlKind::manage_remove, 5}).command == Command::redraw);
    OA_CHECK(manage.sheet == Sheet::remove_part_confirm && manage.sheet_part == 5);
    const game_files::Outcome removed = press(manage, phone, {ControlKind::sheet_option, 0});
    OA_CHECK(removed.command == Command::manage_remove && removed.index == 5);
    const auto demo_row = static_cast<uint16_t>(manage.parts.size() - 1);
    static_cast<void>(press(manage, tablet, {ControlKind::manage_remove, demo_row}));
    OA_CHECK(
        press(manage, tablet, {ControlKind::sheet_option, 0}).command == Command::remove_demo_data
    );
    OA_CHECK(press(manage, tablet, {ControlKind::manage_remove_all}).command == Command::redraw);
    OA_CHECK(manage.sheet == Sheet::remove_all_confirm);
    OA_CHECK(press(manage, tablet, {ControlKind::sheet_option, 1}).command == Command::redraw);
    static_cast<void>(press(manage, tablet, {ControlKind::manage_remove_all}));
    OA_CHECK(
        press(manage, tablet, {ControlKind::sheet_option, 0}).command == Command::manage_remove_all
    );
    Model waiting = manage_model();
    waiting.banner = game_files::Banner::next_start;
    OA_CHECK(press(waiting, tablet, {ControlKind::manage_done}).command == Command::redraw);
    OA_CHECK(waiting.sheet == Sheet::scheduled_note);
    OA_CHECK(press(waiting, tablet, {ControlKind::sheet_option, 0}).command == Command::done);
    Model pending = manage_model();
    pending.pending_replacement = true;
    OA_CHECK(
        press(pending, tablet, {ControlKind::manage_cancel_pending}).command ==
        Command::manage_cancel_pending
    );
    OA_CHECK(press(pending, tablet, {ControlKind::manage_add}).command == Command::none);
    OA_CHECK(press(pending, tablet, {ControlKind::manage_remove, 2}).command == Command::none);
    OA_CHECK(pending.sheet == Sheet::none);
}

/// Each problem shows its buttons in the design's order, each with its command.
void problems_have_their_buttons() {
    struct Expected {
        Problem problem;
        std::vector<Command> commands;
    };

    const std::vector<Expected> table{
        {Problem::not_a_game, {Command::pick_game_folder, Command::back}},
        {Problem::cannot_play,
         {Command::pick_game_folder, Command::pick_game_folder, Command::discard}},
        {Problem::not_demo_installer, {Command::pick_installer, Command::back}},
        {Problem::short_space, {Command::recheck_space, Command::copy_anyway}},
        {Problem::disk_full, {Command::continue_copy, Command::discard}},
        {Problem::source_unreadable, {Command::continue_copy, Command::discard}},
        {Problem::download_failed, {Command::continue_copy, Command::discard}},
        {Problem::access_withdrawn, {Command::pick_game_folder, Command::discard}},
        {Problem::source_changed, {Command::continue_copy, Command::start_again}},
        {Problem::too_large, {Command::copy_anyway, Command::pick_game_folder}},
        {Problem::no_game_folder_yet, {Command::check_copied, Command::back}},
        {Problem::found_misnamed, {Command::adopt, Command::back}},
        {Problem::found_loose, {Command::adopt, Command::back}},
        {Problem::picker_failed, {Command::retry, Command::back}},
        {Problem::staging_unwritable, {Command::retry, Command::back}},
    };
    for (const Form& form : forms()) {
        for (const Expected& expected : table) {
            Model model = base_model();
            model.step = Step::problem;
            model.problem = expected.problem;
            const Layout layout = lay_out(model, form);
            // The buttons, left to right and top to bottom, in the layout's order.
            std::vector<Control> buttons;
            for (const Item& item : layout.list.items) {
                const Control control = game_files::screen_control(item.control);
                if (control.kind != ControlKind::none && control.kind != ControlKind::language)
                    buttons.push_back(control);
            }
            const std::string name = "problem " +
                                     std::to_string(static_cast<int>(expected.problem)) + " at " +
                                     form.name;
            OA_EXPECT(buttons.size() == expected.commands.size(), name + ": button count");
            for (std::size_t index = 0; index < buttons.size() && index < expected.commands.size();
                 ++index) {
                Model pressed = model;
                const game_files::Outcome outcome = press(pressed, form, buttons[index]);
                OA_EXPECT(
                    outcome.command == expected.commands[index],
                    name + ": button " + std::to_string(index)
                );
                // The numbered form reaches the same button.
                Model numbered = model;
                game_files::Interaction interaction{};
                const Control by_number{ControlKind::problem_action, static_cast<uint16_t>(index)};
                const game_files::Outcome by_place =
                    has_control(layout, by_number)
                        ? game_files::press_up(numbered, interaction, layout, by_number)
                        : game_files::Outcome{expected.commands[index], 0};
                OA_EXPECT(by_place.command == expected.commands[index], name + ": numbered button");
            }
            if (!buttons.empty()) {
                const Item* first = item_of(layout, buttons.front());
                OA_EXPECT(
                    first != nullptr && (first->role == Role::button_main ||
                                         expected.problem == Problem::too_large),
                    name + ": the first button is the main one"
                );
            }
        }
    }
}

/// Tab and Shift+Tab move through the focus order, Return and Space press, Esc closes a
/// sheet or goes back, and Tab scrolls a focused row into view.
void keys_move_focus_and_press() {
    const Form tablet = forms().front();
    Model model = base_model();
    game_files::Interaction interaction{};
    Layout layout = lay_out(model, tablet);
    const std::vector<Control> expected{
        {ControlKind::language},
        {ControlKind::choose_folder},
        {ControlKind::i_have_copied},
        {ControlKind::choose_installer},
    };
    OA_CHECK(layout.focus_order == expected);
    OA_CHECK(
        game_files::key(model, interaction, layout, game_files::Key::tab).command == Command::redraw
    );
    OA_CHECK(
        interaction.focus_shown && game_files::screen_control(interaction.focused) == expected[0]
    );
    static_cast<void>(game_files::key(model, interaction, layout, game_files::Key::tab));
    OA_CHECK(game_files::screen_control(interaction.focused) == expected[1]);
    static_cast<void>(game_files::key(model, interaction, layout, game_files::Key::back_tab));
    OA_CHECK(game_files::screen_control(interaction.focused) == expected[0]);
    static_cast<void>(game_files::key(model, interaction, layout, game_files::Key::back_tab));
    OA_CHECK(game_files::screen_control(interaction.focused) == expected[3]);
    static_cast<void>(game_files::key(model, interaction, layout, game_files::Key::tab));
    OA_CHECK(game_files::screen_control(interaction.focused) == expected[0]);
    OA_CHECK(
        game_files::key(model, interaction, layout, game_files::Key::enter).command ==
        Command::open_language
    );
    static_cast<void>(game_files::key(model, interaction, layout, game_files::Key::tab));
    static_cast<void>(game_files::key(model, interaction, layout, game_files::Key::tab));
    OA_CHECK(
        game_files::key(model, interaction, layout, game_files::Key::space).command ==
        Command::check_copied
    );
    // The focus ring is marked for painting.
    kit::mark_states(layout.list, interaction);
    const Item* focused = item_of(layout, expected[2]);
    OA_CHECK(focused != nullptr && focused->state.focused);
    // Return with no focus shown presses the main button; Space does nothing.
    game_files::Interaction fresh{};
    OA_CHECK(
        game_files::key(model, fresh, layout, game_files::Key::enter).command ==
        Command::pick_game_folder
    );
    OA_CHECK(
        game_files::key(model, fresh, layout, game_files::Key::space).command == Command::none
    );
    // Esc: nothing on the first run, back elsewhere, the Stop sheet while copying, Done in
    // the management state, and a sheet closes first.
    OA_CHECK(
        game_files::key(model, fresh, layout, game_files::Key::escape).command == Command::none
    );
    Model ready = ready_model();
    OA_CHECK(
        game_files::key(ready, fresh, lay_out(ready, tablet), game_files::Key::escape).command ==
        Command::back
    );
    Model looking = base_model();
    looking.step = Step::looking;
    OA_CHECK(
        game_files::key(looking, fresh, lay_out(looking, tablet), game_files::Key::escape)
            .command == Command::cancel_scan
    );
    Model problem = base_model();
    problem.step = Step::problem;
    OA_CHECK(
        game_files::key(problem, fresh, lay_out(problem, tablet), game_files::Key::escape)
            .command == Command::back
    );
    Model copying = copying_model();
    OA_CHECK(
        game_files::key(copying, fresh, lay_out(copying, tablet), game_files::Key::escape)
            .command == Command::redraw
    );
    OA_CHECK(copying.sheet == Sheet::stop);
    const Layout stop_sheet = lay_out(copying, tablet);
    const std::vector<Control> sheet_order{
        {ControlKind::sheet_option, 0},
        {ControlKind::sheet_option, 1},
        {ControlKind::sheet_option, 2}
    };
    OA_CHECK(stop_sheet.focus_order == sheet_order);
    OA_CHECK(
        game_files::key(copying, fresh, stop_sheet, game_files::Key::enter).command ==
        Command::stop_keep
    );
    copying.sheet = Sheet::stop;
    OA_CHECK(
        game_files::key(copying, fresh, stop_sheet, game_files::Key::escape).command ==
        Command::redraw
    );
    OA_CHECK(copying.sheet == Sheet::none);
    Model manage = manage_model();
    OA_CHECK(
        game_files::key(manage, fresh, lay_out(manage, tablet), game_files::Key::escape).command ==
        Command::done
    );
    // Tab scrolls the phone's rows so the focused switch shows.
    const Form phone = forms()[1];
    Model rows = ready_model();
    game_files::Interaction tabbing{};
    bool scrolled = false;
    for (int step = 0; step < 30; ++step) {
        const Layout current = lay_out(rows, phone);
        static_cast<void>(game_files::key(rows, tabbing, current, game_files::Key::tab));
        const Layout after = lay_out(rows, phone);
        const Item* item = item_of(after, game_files::screen_control(tabbing.focused));
        if (item != nullptr && item->clip.height > 0) {
            OA_CHECK(visible(*item).height == item->rect.height);
            scrolled = scrolled || rows.scroll_points > 0;
        }
    }
    OA_CHECK(scrolled);
}

/// The rows scroll when they do not fit, clamped at both ends.
void scrolling_is_clamped() {
    const Form phone = forms()[1];
    Model model = ready_model();
    const Layout layout = lay_out(model, phone);
    OA_CHECK(layout.scroll_max_points > 0);
    OA_CHECK(layout.rows.height > 0);
    OA_CHECK(game_files::scroll(model, layout, 1'000'000.0f).command == Command::redraw);
    OA_CHECK(model.scroll_points == layout.scroll_max_points);
    OA_CHECK(game_files::scroll(model, layout, 10.0f).command == Command::none);
    OA_CHECK(game_files::scroll(model, layout, -1'000'000.0f).command == Command::redraw);
    OA_CHECK(model.scroll_points == 0);
    game_files::Interaction interaction{};
    static_cast<void>(game_files::key(model, interaction, layout, game_files::Key::down));
    OA_CHECK(model.scroll_points == std::min(40, layout.scroll_max_points));
    static_cast<void>(game_files::key(model, interaction, layout, game_files::Key::page_down));
    OA_CHECK(
        model.scroll_points > std::min(40, layout.scroll_max_points) ||
        model.scroll_points == layout.scroll_max_points
    );
    static_cast<void>(game_files::key(model, interaction, layout, game_files::Key::page_up));
    static_cast<void>(game_files::key(model, interaction, layout, game_files::Key::up));
    OA_CHECK(model.scroll_points == 0);
    // Scrolled to the end, the last row shows inside the rows region.
    model.scroll_points = layout.scroll_max_points;
    const Layout end = lay_out(model, phone);
    const Item* last = item_of(end, {ControlKind::part_why, 8});
    OA_CHECK(last != nullptr && visible(*last).height == last->rect.height);
    // The tablet's S1 does not scroll.
    Model first = base_model();
    const Layout tablet = lay_out(first, forms().front());
    OA_CHECK(tablet.scroll_max_points == 0);
    OA_CHECK(game_files::scroll(first, tablet, 100.0f).command == Command::none);
    // The left-out sheet's list scrolls on the phone, and opening it starts at the top.
    Model sheet = ready_model();
    sheet.scroll_points = 30;
    static_cast<void>(press(sheet, phone, {ControlKind::show_left_out}));
    OA_CHECK(sheet.sheet == Sheet::left_out_list && sheet.scroll_points == 0);
    const Layout list = lay_out(sheet, phone);
    OA_CHECK(list.scroll_max_points > 0);
}

/// Every text a layout shows, joined.
///
/// @param model the model
/// @return the texts
std::string all_text(const Model& model) {
    const Form wide{"wide", viewport_of(2400, 1800, 1.0f)};
    std::string text;
    for (const Item& item : lay_out(model, wide).list.items)
        for (const std::string& line : item.lines)
            text += line + "\n";
    // Joined lines read as one sentence for the searches.
    std::string joined;
    for (char character : text)
        joined += character == '\n' ? ' ' : character;
    return joined;
}

/// A platform's words in the GameFilesText order, made up for the test.
constexpr std::array<std::string_view, 8> scripted_words{
    "Slate",
    "Copy your folder into the Slate's Shared Shelf with the Shelf tool.",
    "Into the Slate's Shared Shelf",
    "on a Stick drive, on a Shelf share or on this Slate",
    "a Stick drive, a Shelf share",
    "Free space in Slate Settings",
    "In Shelf: Slate › Open Annihilation › Total Annihilation",
    "Sky Locker",
};

/// The marks of the scripted words, none of which the neutral texts may hold.
constexpr std::array<std::string_view, 4> scripted_marks{"Slate", "Shelf", "Stick", "Sky Locker"};

/// Each step's texts fill every place, with the engine's neutral words and with a platform's.
void texts_take_platform_words() {
    for (const Case& test : every_case()) {
        const std::string neutral = all_text(test.model);
        OA_EXPECT(neutral.find('{') == std::string::npos, test.name + ": an unfilled place");
        for (std::string_view mark : scripted_marks)
            OA_EXPECT(
                neutral.find(mark) == std::string::npos,
                test.name + ": a platform word in the neutral text"
            );
        Model scripted = test.model;
        for (std::size_t index = 0; index < scripted_words.size(); ++index)
            scripted.words[index] = std::string(scripted_words[index]);
        const std::string platform = all_text(scripted);
        OA_EXPECT(platform.find('{') == std::string::npos, test.name + ": an unfilled place");
    }
    const auto with_words = [](Model model) {
        for (std::size_t index = 0; index < scripted_words.size(); ++index)
            model.words[index] = std::string(scripted_words[index]);
        return model;
    };
    // S1: the device, the steps of Copy it yourself, the picker's places.
    const std::string first = all_text(with_words(base_model()));
    OA_CHECK(first.find("copied onto this Slate once") != std::string::npos);
    OA_CHECK(first.find("with the Shelf tool. Name it Total Annihilation.") != std::string::npos);
    OA_CHECK(
        first.find(
            "totala1.hpi: on a Stick drive, on a Shelf share or on this Slate. It is checked"
        ) != std::string::npos
    );
    OA_CHECK(first.find("38 GB free on this Slate.") != std::string::npos);
    const std::string neutral_first = all_text(base_model());
    OA_CHECK(neutral_first.find("copied onto this device once") != std::string::npos);
    OA_CHECK(
        neutral_first.find("with your file manager. Name it Total Annihilation.") !=
        std::string::npos
    );
    // The phone rows' one-line forms.
    const Form phone = forms()[1];
    std::string rows;
    for (const Item& item : lay_out(with_words(base_model()), phone).list.items)
        for (const std::string& line : item.lines)
            rows += line + "\n";
    OA_CHECK(
        rows.find("Into the Slate's Shared Shelf, named Total Annihilation.") != std::string::npos
    );
    OA_CHECK(
        rows.find("The folder that holds totala1.hpi: a Stick drive, a Shelf share.") !=
        std::string::npos
    );
    // S3: the cloud's name; S6: the advice and the device.
    Model cloud = ready_model();
    cloud.remote_bytes = 312'000'000;
    OA_CHECK(
        all_text(with_words(cloud))
            .find("312 MB is in Sky Locker only and is downloaded while copying.") !=
        std::string::npos
    );
    OA_CHECK(all_text(cloud).find("312 MB is in the cloud only") != std::string::npos);
    Model space = base_model();
    space.step = Step::problem;
    space.problem = Problem::short_space;
    space.need_bytes = 1'300'000'000;
    space.free_bytes = 640'000'000;
    const std::string advice = all_text(with_words(space));
    OA_CHECK(advice.find("Not enough space on this Slate") != std::string::npos);
    OA_CHECK(
        advice.find(
            "These files need 1.3 GB and this Slate has 640 MB free. Free space in Slate Settings, "
            "or turn off a part above"
        ) != std::string::npos
    );
    Model empty = base_model();
    empty.step = Step::problem;
    empty.problem = Problem::no_game_folder_yet;
    OA_CHECK(
        all_text(empty).find("There is no Total Annihilation folder on this device yet.") !=
        std::string::npos
    );
    // S5's backups line and S8's subtitle.
    Model play = base_model();
    play.step = Step::ready_to_play;
    play.uses_bytes = 1'100'000'000;
    play.free_bytes = 37'000'000'000;
    OA_CHECK(
        all_text(with_words(play)).find("Uses 1.1 GB on this Slate · 37 GB free.") !=
        std::string::npos
    );
    OA_CHECK(
        all_text(with_words(play)).find("These files are not in this Slate's backups.") !=
        std::string::npos
    );
    OA_CHECK(
        all_text(manage_model()).find("1.1 GB on this device · 37 GB free") != std::string::npos
    );
    // The S7 banner and the added banner.
    Model resume = base_model();
    resume.banner = game_files::Banner::continue_copy;
    resume.stopped_bytes = 690'000'000;
    resume.stopped_total = 1'100'000'000;
    OA_CHECK(
        all_text(resume).find(
            "stopped at 690 MB of 1.1 GB. Choose the same folder to continue; what was copied is "
            "kept."
        ) != std::string::npos
    );
    Model added = manage_model();
    added.banner = game_files::Banner::added;
    added.added_files = 12;
    added.added_kept = {"totala1.hpi"};
    OA_CHECK(
        all_text(added).find("12 files were added; totala1.hpi is there already and was kept.") !=
        std::string::npos
    );
    // The demo's installer is moved in: no "Copies 0 bytes".
    Model demo = base_model();
    demo.step = Step::ready_to_copy;
    demo.demo = true;
    OA_CHECK(all_text(demo).find("Copies") == std::string::npos);
    OA_CHECK(all_text(demo).find("38 GB free on this device.") != std::string::npos);
    // S4: the progress and the download line.
    const std::string copying = all_text(with_words(copying_model()));
    OA_CHECK(copying.find("690 MB of 1.12 GB") != std::string::npos);
    OA_CHECK(copying.find("about a minute left") != std::string::npos);
    OA_CHECK(copying.find("Downloading btmaps.ccx from Sky Locker…") != std::string::npos);
    // platform_word gives the neutral word without a platform's.
    OA_CHECK(game_files::platform_word(base_model(), 0) == "device");
    OA_CHECK(game_files::platform_word(with_words(base_model()), 7) == "Sky Locker");
    OA_CHECK(game_files::platform_word(base_model(), 8).empty());
}

/// Sizes are written in decimal units with two significant digits, or three when precise.
void size_texts() {
    struct Row {
        uint64_t bytes;
        bool precise;
        std::string_view text;
    };

    const std::array<Row, 20> table{{
        {0, false, "0 bytes"},
        {1, false, "1 byte"},
        {999, false, "999 bytes"},
        {1'000, false, "1 KB"},
        {1'500, false, "1.5 KB"},
        {2'980'000, false, "3 MB"},
        {20'000'000, false, "20 MB"},
        {131'000'000, false, "131 MB"},
        {640'000'000, false, "640 MB"},
        {742'000'000, false, "742 MB"},
        {999'600'000, false, "1 GB"},
        {1'128'000'000, false, "1.1 GB"},
        {1'120'000'000, true, "1.12 GB"},
        {1'100'000'000, true, "1.1 GB"},
        {690'000'000, true, "690 MB"},
        {9'960'000'000, false, "10 GB"},
        {11'300'000'000, true, "11.3 GB"},
        {38'000'000'000, false, "38 GB"},
        {48'000'000'000, false, "48 GB"},
        {2'500'000'000'000, false, "2.5 TB"},
    }};
    for (const Row& row : table)
        OA_EXPECT(
            game_files::size_text(row.bytes, row.precise) == row.text,
            "size_text(" + std::to_string(row.bytes) + ") = " +
                game_files::size_text(row.bytes, row.precise) + ", wanted " + std::string(row.text)
        );
}

/// The time a copy has left reads in rough words.
void time_left_texts() {
    struct Row {
        std::optional<uint32_t> seconds;
        std::string_view text;
    };

    const std::array<Row, 9> table{{
        {std::nullopt, "working out the time left"},
        {0, "a few seconds left"},
        {44, "a few seconds left"},
        {45, "about a minute left"},
        {89, "about a minute left"},
        {240, "about 4 minutes left"},
        {3000, "about 50 minutes left"},
        {4000, "about an hour left"},
        {7200, "about 2 hours left"},
    }};
    for (const Row& row : table)
        OA_EXPECT(
            game_files::time_left_text(row.seconds) == row.text,
            "time_left_text = " + game_files::time_left_text(row.seconds) + ", wanted " +
                std::string(row.text)
        );
}

/// S5's sentence and Settings' summary are built from the parts present.
void ready_and_summary_texts() {
    std::array<bool, 11> parts{};
    const auto set = [&parts](PartKind kind) { parts[static_cast<std::size_t>(kind)] = true; };
    set(PartKind::game_archives);
    OA_CHECK(game_files::ready_text(parts, false) == "Total Annihilation.");
    set(PartKind::update_31c);
    set(PartKind::music);
    OA_CHECK(game_files::ready_text(parts, false) == "Total Annihilation 3.1c with music.");
    set(PartKind::core_contingency);
    set(PartKind::battle_tactics);
    set(PartKind::movies);
    OA_CHECK(
        game_files::ready_text(parts, false) ==
        "Total Annihilation 3.1c with Core Contingency and Battle Tactics, music and movies."
    );
    OA_CHECK(game_files::ready_text(parts, true) == "The Total Annihilation demo (1997).");
    std::array<bool, 11> summary{};
    summary[static_cast<std::size_t>(PartKind::game_archives)] = true;
    summary[static_cast<std::size_t>(PartKind::update_31c)] = true;
    summary[static_cast<std::size_t>(PartKind::core_contingency)] = true;
    summary[static_cast<std::size_t>(PartKind::battle_tactics)] = true;
    summary[static_cast<std::size_t>(PartKind::music)] = true;
    OA_CHECK(
        game_files::summary_text(summary, 1, false) ==
        "3.1c · Core Contingency · Battle Tactics · music · 1 mod"
    );
    OA_CHECK(game_files::summary_text(summary, 2, false).find("2 mods") != std::string::npos);
    OA_CHECK(game_files::summary_text({}, 0, true) == "The Total Annihilation demo (1997)");
    OA_CHECK(game_files::summary_text({}, 0, false) == "No game files");
}

/// Returns the name of what a part draws, as the screen's own items named it before they
/// were the kit's parts: a panel was a card and a toggle a switch_off_on.
///
/// @param role the part's role
/// @return the name; "?" for a role the screen does not draw
std::string_view role_name(Role role) {
    switch (role) {
    case Role::header_bar:
        return "header_bar";
    case Role::badge:
        return "badge";
    case Role::header_text:
        return "header_text";
    case Role::version:
        return "version";
    case Role::title:
        return "title";
    case Role::text:
        return "text";
    case Role::panel:
        return "card";
    case Role::icon:
        return "icon";
    case Role::banner:
        return "banner";
    case Role::row:
        return "row";
    case Role::divider:
        return "divider";
    case Role::toggle:
        return "switch_off_on";
    case Role::button_main:
        return "button_main";
    case Role::button:
        return "button";
    case Role::button_danger:
        return "button_danger";
    case Role::progress_track:
        return "progress_track";
    case Role::progress_fill:
        return "progress_fill";
    case Role::progress_busy:
        return "progress_busy";
    case Role::backdrop:
        return "backdrop";
    case Role::sheet:
        return "sheet";
    default:
        return "?";
    }
}

/// Returns the name of a text's look.
///
/// @param role the text's look
/// @return its enumerator's name
std::string_view text_role_name(kit::TextStyle role) {
    using TextRole = kit::TextStyle;
    switch (role) {
    case TextRole::header:
        return "header";
    case TextRole::title:
        return "title";
    case TextRole::subtitle:
        return "subtitle";
    case TextRole::lead:
        return "lead";
    case TextRole::body:
        return "body";
    case TextRole::small:
        return "small";
    case TextRole::row_title:
        return "row_title";
    case TextRole::row_detail:
        return "row_detail";
    case TextRole::button:
        return "button";
    case TextRole::mark_text:
        return "mark_text";
    }
    return "?";
}

/// Returns the name of a mark.
///
/// @param glyph the mark
/// @return its enumerator's name
std::string_view glyph_name(game_files::Glyph glyph) {
    using game_files::Glyph;
    switch (glyph) {
    case Glyph::none:
        return "none";
    case Glyph::folder:
        return "folder";
    case Glyph::device:
        return "device";
    case Glyph::disk:
        return "disk";
    case Glyph::clock:
        return "clock";
    case Glyph::warning:
        return "warning";
    case Glyph::check:
        return "check";
    case Glyph::cross:
        return "cross";
    case Glyph::dash:
        return "dash";
    case Glyph::info:
        return "info";
    case Glyph::stop:
        return "stop";
    case Glyph::play:
        return "play";
    case Glyph::trash:
        return "trash";
    case Glyph::refresh:
        return "refresh";
    case Glyph::plus:
        return "plus";
    case Glyph::file:
        return "file";
    case Glyph::oa:
        return "oa";
    case Glyph::magnifier:
        return "magnifier";
    }
    return "?";
}

/// Returns the name of a kind of control.
///
/// @param kind the kind
/// @return its enumerator's name
std::string_view control_kind_name(ControlKind kind) {
    switch (kind) {
    case ControlKind::none:
        return "none";
    case ControlKind::language:
        return "language";
    case ControlKind::choose_folder:
        return "choose_folder";
    case ControlKind::i_have_copied:
        return "i_have_copied";
    case ControlKind::choose_installer:
        return "choose_installer";
    case ControlKind::banner_action:
        return "banner_action";
    case ControlKind::banner_discard:
        return "banner_discard";
    case ControlKind::cancel:
        return "cancel";
    case ControlKind::nested_choice:
        return "nested_choice";
    case ControlKind::check_it:
        return "check_it";
    case ControlKind::part_switch:
        return "part_switch";
    case ControlKind::part_why:
        return "part_why";
    case ControlKind::show_left_out:
        return "show_left_out";
    case ControlKind::copy:
        return "copy";
    case ControlKind::check_space:
        return "check_space";
    case ControlKind::stop:
        return "stop";
    case ControlKind::play:
        return "play";
    case ControlKind::remove_old:
        return "remove_old";
    case ControlKind::problem_action:
        return "problem_action";
    case ControlKind::back:
        return "back";
    case ControlKind::sheet_option:
        return "sheet_option";
    case ControlKind::manage_check:
        return "manage_check";
    case ControlKind::manage_add:
        return "manage_add";
    case ControlKind::manage_remove:
        return "manage_remove";
    case ControlKind::manage_replace:
        return "manage_replace";
    case ControlKind::manage_remove_all:
        return "manage_remove_all";
    case ControlKind::manage_cancel_pending:
        return "manage_cancel_pending";
    case ControlKind::manage_done:
        return "manage_done";
    }
    return "?";
}

/// Returns a colour as the name of the screen's colour token whose value it is, or as
/// #rrggbbaa when it is none of them, so that a token whose value changes keeps its name.
///
/// @param colour the colour
/// @return the token's name or the hexadecimal value
std::string colour_name(game_files::Colour colour) {
    namespace token = oa::ui::kit::screen_colour;
    const std::array<std::pair<std::string_view, game_files::Colour>, 10> tokens{{
        {"background", token::background},
        {"panel", token::panel},
        {"line", token::line},
        {"text", token::text},
        {"dim", token::dim},
        {"green", token::green},
        {"amber", token::amber},
        {"red", token::red},
        {"ink", token::ink},
        {"button_text", token::button_text},
    }};
    for (const auto& [name, value] : tokens)
        if (value == colour)
            return std::string(name);
    constexpr std::string_view digits = "0123456789abcdef";
    std::string hex = "#";
    for (const uint8_t channel : {colour.r, colour.g, colour.b, colour.a}) {
        hex += digits[channel >> 4];
        hex += digits[channel & 0xf];
    }
    return hex;
}

/// Writes a rectangle as x,y,width,height.
///
/// @param rect the rectangle
/// @return the text
std::string rect_text(const game_files::Rect& rect) {
    return std::to_string(rect.x) + "," + std::to_string(rect.y) + "," +
           std::to_string(rect.width) + "," + std::to_string(rect.height);
}

/// Writes a control as its kind's name and its index.
///
/// @param control the control
/// @return kind/index
std::string control_text(const Control& control) {
    return std::string(control_kind_name(control.kind)) + "/" + std::to_string(control.index);
}

/// Writes a number of single precision exactly, as the hexadecimal digits of its bits.
///
/// @param value the number
/// @return eight hexadecimal digits
std::string float_text(float value) {
    constexpr std::string_view digits = "0123456789abcdef";
    const auto bits = std::bit_cast<uint32_t>(value);
    std::string text;
    for (int shift = 28; shift >= 0; shift -= 4)
        text += digits[(bits >> shift) & 0xfu];
    return text;
}

/// Writes a line of text in double quotes, a quote or a backslash in it after a backslash.
///
/// @param line the line
/// @return the quoted line
std::string quoted_line(std::string_view line) {
    std::string text = "\"";
    for (const char character : line) {
        if (character == '"' || character == '\\')
            text += '\\';
        text += character;
    }
    return text + "\"";
}

/// Writes a layout in a fixed text form: one line of the layout's own fields (the form, the
/// scrolling region, how far it scrolls, the pixels a point and the focus order), then one
/// line per item in painting order with every field of the item in a fixed order. Roles,
/// text looks, marks and kinds of control are written as their enumerators' names, and
/// colours as the names of the screen's colour tokens.
///
/// @param layout the layout
/// @return the text
std::string format_layout(const Layout& layout) {
    std::string text = "layout device ";
    text += layout.device == game_files::DeviceClass::phone ? "phone" : "tablet";
    text += " rows " + rect_text(layout.rows);
    text += " scroll " + std::to_string(layout.scroll_max_points);
    text += " px " + float_text(layout.px_per_point);
    text += " focus";
    for (const Control& control : layout.focus_order)
        text += " " + control_text(control);
    text += "\n";
    for (const Item& item : layout.list.items) {
        text += role_name(item.role);
        text += " box " + rect_text(item.rect);
        text += " lines " + std::to_string(item.lines.size());
        for (const std::string& line : item.lines)
            text += " " + quoted_line(line);
        text += " text ";
        text += text_role_name(item.style);
        text += " size " + std::to_string(item.pixel_size);
        text += " bold " + std::to_string(item.bold ? 1 : 0);
        text += " colour " + colour_name(item.colour);
        text += " glyph ";
        text += glyph_name(item.glyph);
        text += " glyph_size " + std::to_string(item.glyph_size);
        text += " control " + control_text(game_files::screen_control(item.control));
        text += " enabled " + std::to_string(item.state.disabled ? 0 : 1);
        text += " on " + std::to_string(item.state.on ? 1 : 0);
        text += " focused " + std::to_string(item.state.focused ? 1 : 0);
        text += " pressed " + std::to_string(item.state.pressed ? 1 : 0);
        text += " fraction " + float_text(item.fraction);
        text += " clip " + rect_text(item.clip);
        text += "\n";
    }
    return text;
}

/// Returns the SHA-256 of a layout's text form, in hexadecimal.
///
/// @param layout the layout
/// @return 64 lower-case hexadecimal digits
std::string layout_digest(const Layout& layout) {
    namespace sha256 = oa::base::sha256;
    const std::string text = format_layout(layout);
    const auto bytes = std::as_bytes(std::span<const char>(text.data(), text.size()));
    const auto hex = sha256::to_hex(
        sha256::digest_of(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())
        )
    );
    return std::string(hex.begin(), hex.end());
}

/// Returns the digests of every case at every review size, one line each:
/// <case>\t<form>\t<hex>, the content of layout-digests.txt.
///
/// @return the lines
std::string digest_lines() {
    std::string lines;
    for (const Case& test : every_case())
        for (const Form& form : forms())
            lines += test.name + "\t" + form.name + "\t" +
                     layout_digest(lay_out(test.model, form)) + "\n";
    return lines;
}

/// Every layout of every case at every review size is the one recorded: its digest is the
/// one layout-digests.txt holds for that case and form, and the file holds no other line.
///
/// @param file layout-digests.txt; null when the test was given none
void layouts_keep_their_digests(const char* file) {
    OA_EXPECT(file != nullptr, "the test is given layout-digests.txt");
    if (file == nullptr)
        return;
    std::ifstream input(file, std::ios::binary);
    OA_EXPECT(static_cast<bool>(input), std::string("layout-digests.txt opens: ") + file);
    std::map<std::string, std::string> recorded;
    std::size_t lines = 0;
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        ++lines;
        const std::size_t tab = line.rfind('\t');
        OA_EXPECT(tab != std::string::npos, "a digest line has its fields: " + line);
        if (tab == std::string::npos)
            continue;
        recorded[line.substr(0, tab)] = line.substr(tab + 1);
    }
    std::size_t compared = 0;
    for (const Case& test : every_case()) {
        for (const Form& form : forms()) {
            const std::string key = test.name + "\t" + form.name;
            const auto found = recorded.find(key);
            OA_EXPECT(found != recorded.end(), test.name + " at " + form.name + ": a digest");
            if (found == recorded.end())
                continue;
            ++compared;
            OA_EXPECT(
                layout_digest(lay_out(test.model, form)) == found->second,
                test.name + " at " + form.name + ": the layout differs from the one recorded"
            );
        }
    }
    OA_EXPECT(
        lines == compared && recorded.size() == compared,
        "layout-digests.txt holds one line for each case and form, and no other"
    );
}

/// Prints the layouts of the cases whose names hold a text, item by item, for looking at a
/// layout while changing it.
///
/// @param wanted the text the cases' names hold
void dump(std::string_view wanted) {
    for (const Case& test : every_case()) {
        if (test.name.find(wanted) == std::string::npos)
            continue;
        for (const Form& form : forms()) {
            const Layout layout = lay_out(test.model, form);
            std::printf(
                "== %s at %s: rows %d,%d %dx%d scroll %d\n",
                test.name.c_str(),
                form.name.c_str(),
                layout.rows.x,
                layout.rows.y,
                layout.rows.width,
                layout.rows.height,
                layout.scroll_max_points
            );
            for (const Item& item : layout.list.items)
                std::printf("  %s\n", describe(item).c_str());
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--dump") {
        dump(argv[2]);
        return 0;
    }
    if (fonts() == nullptr)
        std::fprintf(stderr, "the bundled fonts are missing; text is measured roughly\n");
    if (argc == 2 && std::string_view(argv[1]) == "--write-digests") {
        if (fonts() == nullptr)
            return 1;
        std::fputs(digest_lines().c_str(), stdout);
        return 0;
    }
    OA_CHECK(fonts() != nullptr);
    layouts_keep_their_digests(argc == 2 ? argv[1] : nullptr);
    review_sizes_take_their_forms();
    every_step_and_sheet_lays_out();
    hit_test_finds_the_nearest_enabled_control();
    presses_return_their_commands();
    the_header_shows_the_icon();
    problems_have_their_buttons();
    keys_move_focus_and_press();
    scrolling_is_clamped();
    texts_take_platform_words();
    size_texts();
    time_left_texts();
    ready_and_summary_texts();
    return oa::test::check_exit_status();
}
