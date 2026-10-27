// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The locator: what a folder or a file is, the places and rows laid out on a
// desktop and on a Steam Deck, and choosing one with keys and presses. The
// layout of every case at the Steam Deck and phone sizes keeps the digest
// layout-digests.txt records, the file the test is given; --write-digests
// prints that file.
#include "oa/base/sha256.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/folder_chooser.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace chooser = oa::ui::folder_chooser;
namespace kit = oa::ui::kit;

/// The desktop window the picture is drawn at, one canvas pixel a point.
chooser::Viewport desktop_viewport() {
    chooser::Viewport viewport;
    viewport.width = 1280;
    viewport.height = 720;
    viewport.px_per_point = 1.0f;
    return viewport;
}

/// The Steam Deck's screen.
chooser::Viewport deck_viewport() {
    chooser::Viewport viewport;
    viewport.width = 1280;
    viewport.height = 800;
    viewport.px_per_point = 1.0f;
    return viewport;
}

/// A phone held sideways, at three canvas pixels a point, with its safe area.
chooser::Viewport phone_viewport() {
    chooser::Viewport viewport;
    viewport.width = 852 * 3;
    viewport.height = 393 * 3;
    viewport.px_per_point = 3.0f;
    viewport.safe = {59 * 3, 0, 59 * 3, 21 * 3};
    return viewport;
}

/// A locator with two places and three rows, one of them the game.
chooser::Model sample(bool deck) {
    chooser::Model model;
    model.version = "v0.8";
    model.deck = deck;
    model.offers_dialog = !deck;
    model.permission_note = !deck;
    model.path = "/home/play/Downloads";
    model.has_parent = true;
    model.can_choose = true;
    model.choose_label = "Use this folder";
    model.places = {
        {"Home", chooser::PlaceGroup::places},
        {"Downloads", chooser::PlaceGroup::places},
        {"GOG · Commander Pack", chooser::PlaceGroup::found},
        {"Macintosh HD", chooser::PlaceGroup::volumes},
    };
    model.place = 1;
    chooser::Entry folder;
    folder.name = "notes";
    folder.what = "folder";
    folder.directory = true;
    chooser::Entry game;
    game.name = "Total Annihilation";
    game.what = "Total Annihilation";
    game.title = "Total Annihilation";
    game.account = "Holds Total Annihilation, Campaign.";
    game.bundles = {"Total Annihilation", "Campaign"};
    game.directory = true;
    game.recognised = true;
    game.choosable = true;
    chooser::Entry installer;
    installer.name = "commander.pkg";
    installer.what = "GOG installer (macOS)";
    installer.size_label = "2.4 GB";
    installer.title = "GOG installer for macOS";
    installer.recognised = true;
    installer.choosable = true;
    model.entries = {folder, game, installer};
    model.selected = 1;
    return model;
}

/// Finds a target's control.
///
/// @param layout the layout
/// @param target the target
/// @return the control; null when it was not laid out
const kit::Control* find(const chooser::Layout& layout, chooser::Target target) {
    return kit::control_of(layout.paint.list, chooser::control_id(target));
}

/// The 1997 demo installer's size, in bytes, as the app records it.
constexpr uint64_t demo_installer_bytes = 21540864;
/// totala1.hpi's size, in bytes, as the built-in table records it.
constexpr uint64_t totala_archive_bytes = 32311035;
/// TADemo.hpi's size, in bytes.
constexpr uint64_t demo_archive_bytes = 20474804;
/// worlds.hpi's size, in bytes.
constexpr uint64_t tileset_bytes = 79967143;
/// The macOS Commander Pack installer's size, in bytes.
constexpr uint64_t macos_installer_bytes = 2559350261;
/// The Windows installer's size, in bytes.
constexpr uint64_t windows_installer_bytes = 1093885504;
/// The older Mac disk image's size, in bytes.
constexpr uint64_t older_mac_installer_bytes = 1144736288;

/// What a file or a folder is recognised as.
void check_recognition() {
    const chooser::SourceAccount plain =
        chooser::describe_source("notes.txt", 12, false, {}, demo_installer_bytes);
    OA_CHECK(plain.what == "file" && !plain.recognised && !plain.choosable && !plain.directory);

    const chooser::SourceAccount archive =
        chooser::describe_source("extra.hpi", 100, false, {}, demo_installer_bytes);
    OA_CHECK(archive.what == "archive" && !archive.recognised);

    const chooser::SourceAccount macos = chooser::describe_source(
        "total_annihilation__commander_pack_en_1_3_15733.pkg", macos_installer_bytes, false, {}, 0
    );
    OA_CHECK(macos.what == "GOG installer (macOS)");
    OA_CHECK(macos.title == "GOG installer for macOS");
    OA_CHECK(macos.recognised && macos.choosable && !macos.directory);
    OA_CHECK(macos.bundles.size() == 7);
    OA_CHECK(macos.bundles.front() == "Total Annihilation");

    const chooser::SourceAccount windows =
        chooser::describe_source("setup.exe", windows_installer_bytes, false, {}, 0);
    OA_CHECK(windows.what == "GOG installer (Windows)" && windows.choosable);

    const chooser::SourceAccount older =
        chooser::describe_source("pack.dmg", older_mac_installer_bytes, false, {}, 0);
    OA_CHECK(older.what == "GOG installer (older Macs)" && older.choosable);

    const chooser::SourceAccount demo_file = chooser::describe_source(
        "TADemo.exe", demo_installer_bytes, false, {}, demo_installer_bytes
    );
    OA_CHECK(demo_file.what == "TA demo installer");
    OA_CHECK(
        demo_file.bundles.size() == 1 && demo_file.bundles.front() == "Total Annihilation demo"
    );
    OA_CHECK(demo_file.choosable);
    const chooser::SourceAccount not_demo =
        chooser::describe_source("TADemo.exe", demo_installer_bytes, false, {}, 0);
    OA_CHECK(!not_demo.recognised);

    const std::vector<chooser::ChildFile> game_files{{"totala1.hpi", totala_archive_bytes}};
    const chooser::SourceAccount game =
        chooser::describe_source("Total Annihilation", 0, true, game_files, demo_installer_bytes);
    OA_CHECK(game.directory && game.recognised && game.choosable);
    OA_CHECK(game.what == "Total Annihilation");
    OA_CHECK(game.bundles.size() == 1 && game.bundles.front() == "Total Annihilation");
    OA_CHECK(game.account.find("Total Annihilation") != std::string::npos);

    const std::vector<chooser::ChildFile> demo_files{{"TADemo.hpi", demo_archive_bytes}};
    const chooser::SourceAccount demo_folder =
        chooser::describe_source("demo", 0, true, demo_files, demo_installer_bytes);
    OA_CHECK(demo_folder.what == "Total Annihilation demo" && demo_folder.choosable);

    const std::vector<chooser::ChildFile> tiles{{"worlds.hpi", tileset_bytes}};
    const chooser::SourceAccount expansion =
        chooser::describe_source("tiles", 0, true, tiles, demo_installer_bytes);
    OA_CHECK(expansion.recognised && !expansion.choosable);
    OA_CHECK(expansion.what == "Cavedog tilesets");

    const chooser::SourceAccount empty =
        chooser::describe_source("Downloads", 0, true, {}, demo_installer_bytes);
    OA_CHECK(empty.directory && empty.what == "folder" && !empty.recognised && !empty.choosable);
}

/// Every control is at least 44 points high, and the rows are the only scrolling controls.
///
/// @param layout the layout
/// @param viewport its canvas
void check_control_size(const chooser::Layout& layout, const chooser::Viewport& viewport) {
    const float scale = viewport.px_per_point > 0.0f ? viewport.px_per_point : 1.0f;
    const int32_t least = static_cast<int32_t>(44.0f * scale) - 1;
    OA_CHECK(!layout.paint.list.controls.empty());
    for (const kit::Control& control : layout.paint.list.controls) {
        OA_CHECK(control.rect.height >= least);
        const bool row = chooser::target_of(control.id).kind == chooser::TargetKind::entry;
        OA_CHECK((control.group == 0) == row);
    }
}

/// The desktop offers the system picker. A Steam Deck does not, and shows the pad hints.
void check_places_and_form() {
    const chooser::Layout desktop = chooser::lay_out(sample(false), {}, desktop_viewport(), {});
    check_control_size(desktop, desktop_viewport());
    OA_CHECK(find(desktop, {chooser::TargetKind::dialog, 0}) != nullptr);
    OA_CHECK(find(desktop, {chooser::TargetKind::choose, 0}) != nullptr);
    OA_CHECK(find(desktop, {chooser::TargetKind::place, 2}) != nullptr);
    OA_CHECK(find(desktop, {chooser::TargetKind::entry, 1}) != nullptr);
    bool saw_place = false;
    bool saw_entry = false;
    for (const chooser::Target target : desktop.focus_order) {
        if (target.kind == chooser::TargetKind::entry)
            saw_entry = true;
        if (target.kind == chooser::TargetKind::place)
            OA_CHECK(!saw_entry);
        if (target.kind == chooser::TargetKind::dialog ||
            target.kind == chooser::TargetKind::choose ||
            target.kind == chooser::TargetKind::cancel)
            OA_CHECK(saw_entry);
        saw_place = saw_place || target.kind == chooser::TargetKind::place;
    }
    OA_CHECK(saw_place && saw_entry);

    const chooser::Layout deck = chooser::lay_out(sample(true), {}, deck_viewport(), {});
    check_control_size(deck, deck_viewport());
    OA_CHECK(find(deck, {chooser::TargetKind::dialog, 0}) == nullptr);
    OA_CHECK(find(deck, {chooser::TargetKind::choose, 0}) != nullptr);
    bool hinted = false;
    for (const kit::Item& part : deck.paint.list.items) {
        for (const std::string& line : part.lines) {
            if (line.find("Filter") != std::string::npos && line.find("Back") != std::string::npos)
                hinted = true;
        }
    }
    OA_CHECK(hinted);

    const chooser::Layout phone = chooser::lay_out(sample(false), {}, phone_viewport(), {});
    check_control_size(phone, phone_viewport());
    bool selected = false;
    for (const kit::Item& part : phone.paint.list.items) {
        for (const std::string& line : part.lines)
            if (line == "SELECTED")
                selected = true;
    }
    OA_CHECK(!selected);
}

/// Keys and presses select a row and choose it. Escape goes up, or cancels.
void check_selection() {
    const chooser::Viewport viewport = desktop_viewport();
    chooser::Model model = sample(false);
    chooser::UiState state;
    chooser::Layout layout = chooser::lay_out(model, state, viewport, {});
    const chooser::Outcome first = chooser::key(layout, state, chooser::Key::down);
    OA_CHECK(state.focus_shown);
    OA_CHECK(
        first.command == chooser::Command::select_entry || first.command == chooser::Command::none
    );

    int32_t guard = 0;
    while (guard < 24) {
        const chooser::Outcome step = chooser::key(layout, state, chooser::Key::tab);
        if (step.command == chooser::Command::select_entry && step.index == 2)
            break;
        ++guard;
    }
    OA_CHECK(guard < 24);
    const chooser::Target focused = layout.focus_order[static_cast<std::size_t>(state.focus)];
    OA_CHECK(focused.kind == chooser::TargetKind::entry && focused.index == 2);

    const chooser::Outcome chosen = chooser::key(layout, state, chooser::Key::choose);
    OA_CHECK(chosen.command == chooser::Command::choose);
    const chooser::Outcome filtered = chooser::key(layout, state, chooser::Key::filter);
    OA_CHECK(filtered.command == chooser::Command::toggle_filter);
    const chooser::Outcome up = chooser::key(layout, state, chooser::Key::escape);
    OA_CHECK(up.command == chooser::Command::parent_folder);

    model.has_parent = false;
    layout = chooser::lay_out(model, state, viewport, {});
    const chooser::Outcome cancel = chooser::key(layout, state, chooser::Key::escape);
    OA_CHECK(cancel.command == chooser::Command::cancel);

    model.can_choose = false;
    layout = chooser::lay_out(model, state, viewport, {});
    OA_CHECK(chooser::key(layout, state, chooser::Key::choose).command == chooser::Command::none);

    const kit::Control* row = find(layout, {chooser::TargetKind::entry, 0});
    OA_CHECK(row != nullptr);
    chooser::UiState pressed;
    chooser::press_down(layout, pressed, {row->rect.x + 4, row->rect.y + 4});
    const chooser::Outcome opened =
        chooser::press_up(layout, pressed, {row->rect.x + 4, row->rect.y + 4});
    OA_CHECK(opened.command == chooser::Command::activate_entry && opened.index == 0);

    const int32_t before = 0;
    chooser::scroll(layout, pressed, 100000);
    OA_CHECK(pressed.scroll_points == layout.scroll_max_points);
    chooser::scroll(layout, pressed, -100000);
    OA_CHECK(pressed.scroll_points == before);
}

/// One case of the recorded layouts: a model and what the locator keeps between frames.
struct Case {
    std::string name{};       ///< what it shows
    chooser::Model model{};   ///< the model
    chooser::UiState state{}; ///< the focus, the press and the scroll
};

/// A size the cases are laid out at.
struct Form {
    std::string name{};           ///< "deck 1280x800@1"
    chooser::Viewport viewport{}; ///< the canvas
};

/// Returns the sizes every case is laid out at: the Steam Deck's screen and a phone held
/// sideways with its safe area.
///
/// @return the forms
std::vector<Form> forms() {
    return {{"deck 1280x800@1", deck_viewport()}, {"phone 852x393@3", phone_viewport()}};
}

/// Returns every model the digests record: the desktop and Deck samples, a home-relative
/// path, an empty folder, a filter, a notice, focus, a press, a scrolled list, a row that
/// cannot be chosen, and an installer.
///
/// @return the cases
std::vector<Case> every_case() {
    std::vector<Case> cases;
    const auto add = [&cases](std::string name, chooser::Model model, chooser::UiState state) {
        cases.push_back(Case{std::move(name), std::move(model), state});
    };
    const chooser::UiState rest;
    add("desktop", sample(false), rest);
    add("deck", sample(true), rest);
    chooser::Model home = sample(false);
    home.path = "~/Games";
    add("home path", home, rest);
    chooser::Model empty = sample(false);
    empty.entries.clear();
    empty.selected = -1;
    empty.can_choose = false;
    add("empty folder", empty, rest);
    chooser::Model filtered = sample(false);
    filtered.filter = "total";
    add("filter", filtered, rest);
    chooser::Model noticed = sample(false);
    noticed.notice = "This folder cannot be played.";
    add("notice", noticed, rest);
    chooser::UiState focused;
    focused.focus = 0;
    focused.focus_shown = true;
    add("focus shown", sample(false), focused);
    chooser::UiState pressed;
    pressed.pressed = 0;
    add("row pressed", sample(false), pressed);
    chooser::Model many = sample(false);
    many.entries.clear();
    for (int index = 0; index < 18; ++index) {
        chooser::Entry row;
        row.name = "folder-" + std::to_string(index);
        row.what = "folder";
        row.directory = true;
        many.entries.push_back(std::move(row));
    }
    many.selected = 0;
    chooser::UiState scrolled;
    scrolled.scroll_points = 100000;
    add("scrolled", many, scrolled);
    chooser::Model blocked = sample(false);
    blocked.can_choose = false;
    blocked.has_parent = false;
    add("cannot choose", blocked, rest);
    chooser::Model installer = sample(false);
    installer.selected = 2;
    installer.choose_label = "Use this installer";
    add("installer", installer, rest);
    return cases;
}

/// Returns the name of a part's role.
///
/// @param role the role
/// @return its enumerator's name
std::string_view role_name(kit::Role role) {
    switch (role) {
    case kit::Role::fill:
        return "fill";
    case kit::Role::blend:
        return "blend";
    case kit::Role::outline:
        return "outline";
    case kit::Role::bevel:
        return "bevel";
    case kit::Role::rule:
        return "rule";
    case kit::Role::text:
        return "text";
    case kit::Role::picture:
        return "picture";
    case kit::Role::mark:
        return "mark";
    case kit::Role::button:
        return "button";
    case kit::Role::toggle:
        return "toggle";
    case kit::Role::levels:
        return "levels";
    case kit::Role::slider:
        return "slider";
    case kit::Role::choice:
        return "choice";
    case kit::Role::choice_menu:
        return "choice_menu";
    case kit::Role::lock:
        return "lock";
    case kit::Role::scroll_bar:
        return "scroll_bar";
    case kit::Role::focus_ring:
        return "focus_ring";
    case kit::Role::oa_button:
        return "oa_button";
    case kit::Role::oa_mark:
        return "oa_mark";
    case kit::Role::header:
        return "header";
    case kit::Role::footer_band:
        return "footer_band";
    case kit::Role::nav:
        return "nav";
    case kit::Role::heading:
        return "heading";
    case kit::Role::row_frame:
        return "row_frame";
    case kit::Role::locked_fade:
        return "locked_fade";
    case kit::Role::list_row:
        return "list_row";
    case kit::Role::chip:
        return "chip";
    case kit::Role::field:
        return "field";
    case kit::Role::tabs:
        return "tabs";
    case kit::Role::card:
        return "card";
    case kit::Role::hover_card:
        return "hover_card";
    case kit::Role::link:
        return "link";
    case kit::Role::progress:
        return "progress";
    case kit::Role::header_bar:
        return "header_bar";
    case kit::Role::badge:
        return "badge";
    case kit::Role::header_text:
        return "header_text";
    case kit::Role::version:
        return "version";
    case kit::Role::title:
        return "title";
    case kit::Role::panel:
        return "panel";
    case kit::Role::icon:
        return "icon";
    case kit::Role::banner:
        return "banner";
    case kit::Role::row:
        return "row";
    case kit::Role::divider:
        return "divider";
    case kit::Role::button_main:
        return "button_main";
    case kit::Role::button_danger:
        return "button_danger";
    case kit::Role::progress_track:
        return "progress_track";
    case kit::Role::progress_fill:
        return "progress_fill";
    case kit::Role::progress_busy:
        return "progress_busy";
    case kit::Role::backdrop:
        return "backdrop";
    case kit::Role::sheet:
        return "sheet";
    case kit::Role::pill:
        return "pill";
    case kit::Role::language_card:
        return "language_card";
    case kit::Role::carousel:
        return "carousel";
    case kit::Role::step_indicator:
        return "step_indicator";
    case kit::Role::setup_chrome:
        return "setup_chrome";
    case kit::Role::choice_card:
        return "choice_card";
    case kit::Role::coach_mark:
        return "coach_mark";
    case kit::Role::spotlight:
        return "spotlight";
    case kit::Role::picture_well:
        return "picture_well";
    case kit::Role::call_to_action:
        return "call_to_action";
    case kit::Role::package_badge:
        return "package_badge";
    case kit::Role::grip:
        return "grip";
    case kit::Role::keycap:
        return "keycap";
    case kit::Role::chord_chip:
        return "chord_chip";
    case kit::Role::keyboard:
        return "keyboard";
    case kit::Role::mouse_picture:
        return "mouse_picture";
    case kit::Role::hold_ring:
        return "hold_ring";
    case kit::Role::radio:
        return "radio";
    }
    return "?";
}

/// Returns the name of a text's look.
///
/// @param style the text's look
/// @return its enumerator's name
std::string_view style_name(kit::TextStyle style) {
    switch (style) {
    case kit::TextStyle::header:
        return "header";
    case kit::TextStyle::title:
        return "title";
    case kit::TextStyle::subtitle:
        return "subtitle";
    case kit::TextStyle::lead:
        return "lead";
    case kit::TextStyle::body:
        return "body";
    case kit::TextStyle::small:
        return "small";
    case kit::TextStyle::row_title:
        return "row_title";
    case kit::TextStyle::row_detail:
        return "row_detail";
    case kit::TextStyle::button:
        return "button";
    case kit::TextStyle::mark_text:
        return "mark_text";
    }
    return "?";
}

/// Returns the name of a mark.
///
/// @param glyph the mark
/// @return its enumerator's name
std::string_view glyph_name(kit::Glyph glyph) {
    switch (glyph) {
    case kit::Glyph::none:
        return "none";
    case kit::Glyph::folder:
        return "folder";
    case kit::Glyph::device:
        return "device";
    case kit::Glyph::disk:
        return "disk";
    case kit::Glyph::clock:
        return "clock";
    case kit::Glyph::warning:
        return "warning";
    case kit::Glyph::check:
        return "check";
    case kit::Glyph::cross:
        return "cross";
    case kit::Glyph::dash:
        return "dash";
    case kit::Glyph::info:
        return "info";
    case kit::Glyph::stop:
        return "stop";
    case kit::Glyph::play:
        return "play";
    case kit::Glyph::trash:
        return "trash";
    case kit::Glyph::refresh:
        return "refresh";
    case kit::Glyph::plus:
        return "plus";
    case kit::Glyph::file:
        return "file";
    case kit::Glyph::oa:
        return "oa";
    case kit::Glyph::magnifier:
        return "magnifier";
    }
    return "?";
}

/// Returns the name of a kind of target.
///
/// @param kind the kind
/// @return its enumerator's name
std::string_view target_kind_name(chooser::TargetKind kind) {
    switch (kind) {
    case chooser::TargetKind::none:
        return "none";
    case chooser::TargetKind::place:
        return "place";
    case chooser::TargetKind::up:
        return "up";
    case chooser::TargetKind::filter:
        return "filter";
    case chooser::TargetKind::entry:
        return "entry";
    case chooser::TargetKind::dialog:
        return "dialog";
    case chooser::TargetKind::cancel:
        return "cancel";
    case chooser::TargetKind::choose:
        return "choose";
    }
    return "?";
}

/// Returns a colour as the name of the screen's colour token whose value it is, or as
/// #rrggbbaa when it is none of them.
///
/// @param colour the colour
/// @return the token's name or the hexadecimal value
std::string colour_name(kit::Colour colour) {
    namespace token = oa::ui::kit::screen_colour;
    const std::array<std::pair<std::string_view, kit::Colour>, 10> tokens{{
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
    for (const auto& [name, value] : tokens) {
        if (value == colour)
            return std::string(name);
    }
    constexpr std::string_view digits = "0123456789abcdef";
    std::string hex = "#";
    for (const uint8_t channel : {colour.r, colour.g, colour.b, colour.a}) {
        hex += digits[channel >> 4];
        hex += digits[channel & 0x0f];
    }
    return hex;
}

/// Writes a rectangle as x,y,width,height.
///
/// @param rect the rectangle
/// @return the text
std::string rect_text(const kit::Rect& rect) {
    return std::to_string(rect.x) + "," + std::to_string(rect.y) + "," +
           std::to_string(rect.width) + "," + std::to_string(rect.height);
}

/// Writes a target as its kind's name and its index.
///
/// @param target the target
/// @return kind/index
std::string target_text(chooser::Target target) {
    return std::string(target_kind_name(target.kind)) + "/" + std::to_string(target.index);
}

/// Writes a number of single precision exactly, as the hexadecimal digits of its bits.
///
/// @param value the number
/// @return eight hexadecimal digits
std::string float_text(float value) {
    constexpr std::string_view digits = "0123456789abcdef";
    const auto bits = std::bit_cast<uint32_t>(value);
    std::string written;
    for (int shift = 28; shift >= 0; shift -= 4)
        written += digits[static_cast<uint32_t>((bits >> shift) & 0xfu)];
    return written;
}

/// Writes a line of text in double quotes, a quote or a backslash in it after a backslash.
///
/// @param line the line
/// @return the quoted line
std::string quoted_line(std::string_view line) {
    std::string quoted = "\"";
    for (const char character : line) {
        if (character == '"' || character == '\\')
            quoted += '\\';
        quoted += character;
    }
    return quoted + "\"";
}

/// Writes a layout in a fixed text form.
///
/// @param layout the layout
/// @return the text
std::string format_layout(const chooser::Layout& layout) {
    namespace gf = oa::ui::game_files;
    std::string written = "layout device ";
    written += layout.paint.device == gf::DeviceClass::phone ? "phone" : "tablet";
    written += " rows " + rect_text(layout.paint.rows);
    written += " scroll " + std::to_string(layout.scroll_max_points);
    written += " painted_scroll " + std::to_string(layout.paint.scroll_max_points);
    written += " px " + float_text(layout.paint.px_per_point);
    written += " focus";
    for (const chooser::Target target : layout.focus_order)
        written += " " + target_text(target);
    written += "\n";
    for (const kit::Item& item : layout.paint.list.items) {
        written += role_name(item.role);
        written += " box " + rect_text(item.rect);
        written += " lines " + std::to_string(item.lines.size());
        for (const std::string& line : item.lines)
            written += " " + quoted_line(line);
        written += " text ";
        written += style_name(item.style);
        written += " size " + std::to_string(item.pixel_size);
        written += " bold " + std::to_string(item.bold ? 1 : 0);
        written += " colour " + colour_name(item.colour);
        written += " glyph ";
        written += glyph_name(item.glyph);
        written += " glyph_size " + std::to_string(item.glyph_size);
        written += " enabled " + std::to_string(item.state.disabled ? 0 : 1);
        written += " on " + std::to_string(item.state.on ? 1 : 0);
        written += " focused " + std::to_string(item.state.focused ? 1 : 0);
        written += " pressed " + std::to_string(item.state.pressed ? 1 : 0);
        written += " fraction " + float_text(item.fraction);
        written += " clip " + rect_text(item.clip);
        written += "\n";
    }
    return written;
}

/// Lays a case out at a form.
///
/// @param test the case
/// @param form the form
/// @return the layout
chooser::Layout lay_out_case(const Case& test, const Form& form) {
    return chooser::lay_out(test.model, test.state, form.viewport, {});
}

/// Returns the SHA-256 of a layout's text form, in hexadecimal.
///
/// @param layout the layout
/// @return 64 lower-case hexadecimal digits
std::string layout_digest(const chooser::Layout& layout) {
    namespace sha256 = oa::base::sha256;
    const std::string written = format_layout(layout);
    const auto bytes = std::as_bytes(std::span<const char>(written.data(), written.size()));
    const auto hex = sha256::to_hex(
        sha256::digest_of(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())
        )
    );
    return std::string(hex.begin(), hex.end());
}

/// Returns the digests of every case at every form, one line each: <case>\t<form>\t<hex>.
///
/// @return the lines
std::string digest_lines() {
    std::string lines;
    for (const Case& test : every_case()) {
        for (const Form& form : forms())
            lines += test.name + "\t" + form.name + "\t" + layout_digest(lay_out_case(test, form)) +
                     "\n";
    }
    return lines;
}

/// Checks that every layout of every case at every form is the one recorded.
///
/// @param file layout-digests.txt; null when the test was given none
void check_digests(const char* file) {
    OA_CHECK(file != nullptr);
    if (file == nullptr)
        return;
    std::ifstream input(file, std::ios::binary);
    OA_CHECK(static_cast<bool>(input));
    if (!input)
        return;
    std::map<std::string, std::string> recorded;
    std::size_t lines = 0;
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        ++lines;
        const std::size_t tab = line.rfind('\t');
        OA_CHECK(tab != std::string::npos);
        if (tab == std::string::npos)
            continue;
        recorded[line.substr(0, tab)] = line.substr(tab + 1);
    }
    std::size_t compared = 0;
    for (const Case& test : every_case()) {
        for (const Form& form : forms()) {
            const std::string key = test.name + "\t" + form.name;
            const auto found = recorded.find(key);
            OA_CHECK(found != recorded.end());
            if (found == recorded.end())
                continue;
            ++compared;
            const std::string digest = layout_digest(lay_out_case(test, form));
            OA_CHECK(digest == found->second);
        }
    }
    OA_CHECK(lines == compared && recorded.size() == compared);
}

/// Folders come before files, names fold A–Z, and a filter matches the name only.
/// The layout says what to choose and shows the filter, the file's mark and its size.
void check_order_and_filter() {
    chooser::Entry file;
    file.name = "zeta.txt";
    file.size_label = "12 B";
    chooser::Entry folder;
    folder.name = "Alpha";
    folder.directory = true;
    chooser::Entry later;
    later.name = "beta";
    std::vector<chooser::Entry> rows{file, later, folder};
    chooser::order_entries(rows);
    OA_CHECK(rows.size() == 3);
    OA_CHECK(rows[0].name == "Alpha" && rows[0].directory);
    OA_CHECK(rows[1].name == "beta");
    OA_CHECK(rows[2].name == "zeta.txt");

    chooser::Entry upper;
    upper.name = "Notes";
    chooser::Entry lower;
    lower.name = "notes";
    std::vector<chooser::Entry> ties{upper, lower};
    chooser::order_entries(ties);
    OA_CHECK(ties[0].name == "Notes" && ties[1].name == "notes");
    OA_CHECK(chooser::entry_before(folder, file));
    OA_CHECK(!chooser::entry_before(file, folder));

    OA_CHECK(chooser::name_matches("Total Annihilation", ""));
    OA_CHECK(chooser::name_matches("Total Annihilation", "total"));
    OA_CHECK(chooser::name_matches("Notes", "notes"));
    OA_CHECK(!chooser::name_matches("commander.pkg", "total"));

    const chooser::Layout shown = chooser::lay_out(sample(false), {}, desktop_viewport(), {});
    bool title = false;
    bool qualify = false;
    bool file_glyph = false;
    bool folder_glyph = false;
    bool size_shown = false;
    bool placeholder = false;
    bool magnifier = false;
    for (const kit::Item& part : shown.paint.list.items) {
        if (part.role == kit::Role::icon && part.glyph == kit::Glyph::file)
            file_glyph = true;
        if (part.role == kit::Role::icon && part.glyph == kit::Glyph::folder)
            folder_glyph = true;
        if (part.role == kit::Role::icon && part.glyph == kit::Glyph::magnifier)
            magnifier = true;
        for (const std::string& line : part.lines) {
            if (line.find("Choose a folder or an installer") != std::string::npos)
                title = true;
            if (line.find("totala1.hpi") != std::string::npos)
                qualify = true;
            if (line == "2.4 GB")
                size_shown = true;
            if (line == "Filter names")
                placeholder = true;
        }
    }
    const kit::Control* filter = find(shown, {chooser::TargetKind::filter, 0});
    const kit::Control* up = find(shown, {chooser::TargetKind::up, 0});
    OA_CHECK(title);
    OA_CHECK(placeholder);
    OA_CHECK(magnifier);
    OA_CHECK(filter != nullptr && up != nullptr && filter->rect.y > up->rect.y);
    OA_CHECK(filter != nullptr && filter->rect.width > up->rect.width);
    OA_CHECK(qualify);
    OA_CHECK(file_glyph);
    OA_CHECK(folder_glyph);
    OA_CHECK(size_shown);

    chooser::Model filtered = sample(false);
    filtered.filter = "commander";
    filtered.entries.clear();
    const chooser::Layout narrowed = chooser::lay_out(filtered, {}, desktop_viewport(), {});
    bool filter_shown = false;
    bool none = false;
    for (const kit::Item& part : narrowed.paint.list.items) {
        for (const std::string& line : part.lines) {
            if (line.find("commander") != std::string::npos)
                filter_shown = true;
            if (line == "No names match.")
                none = true;
        }
    }
    OA_CHECK(filter_shown);
    OA_CHECK(none);
}

} // namespace

/// Runs the checks, or with --write-digests prints the digests of every case's layouts.
///
/// @param argc the arguments' count
/// @param argv the arguments: layout-digests.txt, or --write-digests
/// @return 0 when every check passed
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--write-digests") {
        std::fputs(digest_lines().c_str(), stdout);
        return 0;
    }
    check_digests(argc == 2 ? argv[1] : nullptr);
    check_recognition();
    check_places_and_form();
    check_selection();
    check_order_and_filter();
    return oa::test::check_exit_status();
}
