// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Downloads: its place in the section list before Developer, its rows for
// none, one and three registries, the link to the Library's Registries tab
// on the main menu, the labels and hints filled with the registries' names,
// an install ID shown as its first
// 8 and last 4 hex
// digits, each state's value, RESET and EMPTY disabled when they cannot
// act and locked during a game, Cancel putting the switches and the check
// back after those actions have already come out, Restore defaults, every
// part inside the dialog at Compact, Regular and Large, and the names.

#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"

#include "downloads_page.hpp"
#include "geometry.hpp"

#include <cstdio>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace settings = oa::ui::engine_settings;
namespace geometry = oa::ui::engine_settings::geometry;
namespace kit = oa::ui::kit;
namespace renderer = oa::ui::frontend_renderer;

using settings::DialogAction;
using settings::DialogKey;
using settings::Page;

/// The gap an install ID shows between its first 8 and last 4 hex digits.
const std::string kIdGap = " \u00b7 \u00b7 \u00b7 ";
/// A known 128-bit install ID.
constexpr std::string_view kWholeId = "01234567-89ab-cdef-0123-456789abcdef";
/// That ID as the page shows it, and as the short form the host stores.
const std::string kShownId = "01234567" + kIdGap + "cdef";
/// 412 mebibytes, which the size line writes as 412 MB.
constexpr uint64_t kKept = uint64_t{412} << 20;

/// One registry the offer names.
///
/// @param id the registry's id
/// @param name the name a player sees
/// @param made an ID is stored
/// @param on the switch
/// @param text the short form, or a whole ID
/// @return the row
settings::InstallIdRow
registry_row(std::string id, std::string name, bool made, bool on, std::string text) {
    settings::InstallIdRow row;
    row.registry_id = std::move(id);
    row.registry_name = std::move(name);
    row.made = made;
    row.on = on;
    row.id_text = std::move(text);
    return row;
}

/// Opens Downloads over the offer.
///
/// @param offer what the page shows
/// @param touch the dialog lists Touch
/// @param controller the dialog lists Controller
/// @param game_files the dialog lists Game files
/// @return the dialog
settings::Dialog opened(
    const settings::DownloadsOffer& offer,
    bool touch = false,
    bool controller = false,
    bool game_files = false
) {
    settings::Dialog dialog;
    settings::open_dialog(
        dialog,
        {},
        {},
        {},
        "0.8",
        Page::downloads,
        {},
        settings::highest_unit_limit,
        {},
        nullptr,
        touch,
        game_files,
        controller
    );
    settings::set_downloads_offer(dialog, offer);
    return dialog;
}

/// An offer of the named registries, with files kept and nothing downloading.
///
/// @param rows the registries
/// @return the offer
settings::DownloadsOffer offer_of(std::vector<settings::InstallIdRow> rows) {
    settings::DownloadsOffer offer;
    offer.install_ids = std::move(rows);
    offer.kept_bytes = kKept;
    return offer;
}

/// Core Prime, with the known ID stored and the switch on.
///
/// @param text the stored text
/// @return the row
settings::InstallIdRow core_prime(std::string text = kShownId) {
    return registry_row("core-prime", "Core Prime", true, true, std::move(text));
}

bool inside(const renderer::SourceRect& inner, const renderer::SourceRect& outer) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

bool overlap(const renderer::SourceRect& a, const renderer::SourceRect& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
           b.y < a.y + a.height;
}

struct Point {
    int32_t x{};
    int32_t y{};
};

Point centre(const renderer::SourceRect& rect) {
    return {rect.x + rect.width / 2, rect.y + rect.height / 2};
}

DialogAction click(settings::Dialog& dialog, Point point) {
    static_cast<void>(settings::dialog_pointer_move(dialog, point.x, point.y));
    static_cast<void>(settings::dialog_pointer_down(dialog, point.x, point.y));
    return settings::dialog_pointer_up(dialog, point.x, point.y);
}

/// Tells whether the layout draws a text.
///
/// @param dialog the dialog
/// @param text the text
/// @return true when a part shows it
bool layout_has(const settings::Dialog& dialog, std::string_view text) {
    for (const settings::LayoutPart& part : settings::dialog_layout(dialog))
        if (part.text == text)
            return true;
    return false;
}

/// Tells whether a text item of the display list shows a value.
///
/// @param dialog the dialog
/// @param text the value
/// @return true when a text item shows it
bool value_has(const settings::Dialog& dialog, std::string_view text) {
    const kit::DisplayList list = geometry::dialog_list(dialog, nullptr);
    for (const kit::Item& item : list.items)
        if (item.role == kit::Role::text && item.text == text)
            return true;
    return false;
}

/// Whether a named control is listed, and whether it takes a press.
struct NamedControl {
    bool found{};   ///< the display list names it
    bool enabled{}; ///< a press reaches it
};

/// Returns a named control's presence.
///
/// @param dialog the dialog
/// @param name the control's name, without oa.
/// @return whether it is listed and enabled
NamedControl control_named(const settings::Dialog& dialog, std::string_view name) {
    const kit::DisplayList list = geometry::dialog_list(dialog, nullptr);
    for (const kit::Control& control : list.controls)
        if (control.name == name)
            return {true, control.enabled};
    return {};
}

/// Returns the rectangle of a button caption, when the layout draws one.
///
/// @param dialog the dialog
/// @param caption the caption
/// @param[out] rect the button
/// @return true when the button is drawn
bool button_rect(
    const settings::Dialog& dialog, std::string_view caption, renderer::SourceRect& rect
) {
    for (const settings::LayoutPart& part : settings::dialog_layout(dialog)) {
        if (part.text == caption && part.control != settings::no_control) {
            rect = part.rect;
            return true;
        }
    }
    return false;
}

/// The install-ID hint, with the registry's name filled in.
///
/// @param name the registry's name
/// @return the hint
std::string install_hint(std::string_view name) {
    return geometry::filled(
        "A random number that lets {name} stop automated downloads. "
        "Turned off, you can browse the Library, but downloads are off. "
        "Installed content keeps working.",
        {{"name", name}}
    );
}

/// Places Downloads' rows.
///
/// @param dialog the dialog
/// @return the kit's rows
kit::PlacedRows placed_rows(const settings::Dialog& dialog) {
    kit::PlacedRows placed;
    static_cast<void>(settings::downloads::place(dialog, dialog.locks, {}, &placed));
    return placed;
}

/// Checks that a row's hint is the text, broken as the page breaks it.
/// With no install ID, the last line is "Made at your first download".
///
/// @param view the row
/// @param hint the hint
/// @param dialog the dialog, for the line width
/// @param unmade true when the sentence is the switch's second hint
void hint_matches(
    const kit::RowView& view,
    const std::string& hint,
    const settings::Dialog& dialog,
    bool unmade = false
) {
    const int32_t room = static_cast<int32_t>(geometry::sizes_of(dialog).hint_line_characters) *
                         oa::ui::kit::estimated_character_width;
    const std::string shown(geometry::shown_text(hint));
    const std::vector<std::string> lines =
        oa::ui::kit::wrap(shown, std::max(room, int32_t{1}), &geometry::estimated_width);
    const std::size_t expect = lines.size() + (unmade ? std::size_t{1} : std::size_t{0});
    OA_CHECK(view.hints.size() == expect);
    if (unmade && !view.hints.empty())
        OA_CHECK(view.hints.back() == "Made at your first download");
    for (std::size_t index = 0; index < lines.size() && index < view.hints.size(); ++index) {
        if (view.hints[index] != lines[index]) {
            std::fprintf(
                stderr,
                "hint line %zu: '%s' against '%s'\n",
                index,
                view.hints[index].c_str(),
                lines[index].c_str()
            );
            OA_CHECK(view.hints[index] == lines[index]);
        }
    }
}

void section_list_puts_downloads_before_developer() {
    for (const bool touch : {false, true}) {
        for (const bool controller : {false, true}) {
            for (const bool game_files : {false, true}) {
                const std::span<const Page> pages = settings::dialog_pages(
                    settings::DialogKind::engine, touch, game_files, controller
                );
                const auto downloads = std::find(pages.begin(), pages.end(), Page::downloads);
                const auto updates = std::find(pages.begin(), pages.end(), Page::updates);
                const auto content = std::find(pages.begin(), pages.end(), Page::game_content);
                const auto system = std::find(pages.begin(), pages.end(), Page::system);
                const auto developer = std::find(pages.begin(), pages.end(), Page::developer);
                OA_CHECK(
                    downloads != pages.end() && updates != pages.end() && content != pages.end() &&
                    system != pages.end() && developer != pages.end()
                );
                OA_CHECK(downloads + 1 == updates);
                OA_CHECK(updates + 1 == content);
                OA_CHECK(content + 1 == system);
                OA_CHECK(system + 1 == developer);
                const auto user_interface =
                    std::find(pages.begin(), pages.end(), Page::user_interface);
                const auto accessibility =
                    std::find(pages.begin(), pages.end(), Page::accessibility);
                OA_CHECK(user_interface != pages.end() && accessibility != pages.end());
                OA_CHECK(user_interface + 1 == accessibility);
                OA_CHECK(accessibility < downloads);
                const int32_t optionals =
                    (touch ? 1 : 0) + (controller ? 1 : 0) + (game_files ? 1 : 0);
                // Accessibility makes the plain list ten flowing entries. One
                // optional section drops the gap; two or three also scroll.
                // list_item is the content's place, before that scroll.
                const int32_t stride =
                    geometry::list_item_height + (optionals >= 1 ? 0 : geometry::list_item_gap);
                const int32_t downloads_y = geometry::list_first_top + (6 + optionals) * stride;
                OA_CHECK(
                    geometry::list_item(Page::downloads, touch, game_files, controller).y ==
                    downloads_y
                );
                OA_CHECK(
                    geometry::list_item(Page::updates, touch, game_files, controller).y ==
                    downloads_y + stride
                );
                OA_CHECK(
                    geometry::list_item(Page::game_content, touch, game_files, controller).y ==
                    downloads_y + 2 * stride
                );
                OA_CHECK(
                    geometry::list_item(Page::system, touch, game_files, controller).y ==
                    downloads_y + 3 * stride
                );
                OA_CHECK(
                    geometry::list_item(Page::developer, touch, game_files, controller).y == 262
                );
                OA_CHECK(geometry::list_divider().y == 256);
                OA_CHECK(settings::page_control(Page::downloads) == 9);
                OA_CHECK(settings::page_control(Page::updates) == 12);
                OA_CHECK(settings::page_control(Page::game_content) == 11);
                OA_CHECK(settings::page_control(Page::system) == 10);
                const settings::Dialog dialog = opened({}, touch, controller, game_files);
                OA_CHECK(dialog.page == Page::downloads);
                OA_CHECK(layout_has(dialog, "Downloads"));
                OA_CHECK(layout_has(dialog, "DOWNLOADS"));
            }
        }
    }
}

void rows_follow_the_registries() {
    const settings::Dialog none = opened({});
    OA_CHECK(geometry::open_rows(none).rows.rows.size() == 2);
    OA_CHECK(settings::scroll_limit(none) == 0);
    OA_CHECK(layout_has(none, "Check for content updates"));
    OA_CHECK(layout_has(none, "Downloaded files"));
    OA_CHECK(!layout_has(none, "Install ID"));

    const settings::Dialog one = opened(offer_of({core_prime()}));
    const kit::PlacedRows one_rows = placed_rows(one);
    OA_CHECK(one_rows.rows.size() == 4);
    OA_CHECK(one_rows.rows[1].view.label == "Install ID");
    OA_CHECK(one_rows.rows[2].view.label == "Your install ID");
    OA_CHECK(one_rows.rows[2].view.value == kShownId);
    OA_CHECK(layout_has(one, "Install ID"));
    OA_CHECK(layout_has(one, "Your install ID"));
    hint_matches(one_rows.rows[1].view, install_hint("Core Prime"), one);

    settings::Dialog three = opened(offer_of({
        core_prime(),
        registry_row("ridge", "Ridge", true, true, kShownId),
        registry_row("north-archive", "North Archive", false, true, {}),
    }));
    const kit::PlacedRows three_rows = placed_rows(three);
    OA_CHECK(three_rows.rows.size() == 7);
    OA_CHECK(settings::scroll_limit(three) > 0);
    OA_CHECK(three_rows.rows[1].view.label == "Install ID \u00b7 Core Prime");
    OA_CHECK(three_rows.rows[3].view.label == "Install ID \u00b7 Ridge");
    OA_CHECK(three_rows.rows[5].view.label == "Install ID \u00b7 North Archive");
    hint_matches(
        three_rows.rows[0].view,
        "OA looks for new content and updates in the registries' lists. "
        "It never installs anything until you say so.",
        three
    );
    hint_matches(three_rows.rows[1].view, install_hint("Core Prime"), three);
    hint_matches(three_rows.rows[3].view, install_hint("Ridge"), three);
    hint_matches(three_rows.rows[5].view, install_hint("North Archive"), three, true);
    OA_CHECK(layout_has(three, "Install ID \u00b7 Core Prime"));
    three.scroll[static_cast<std::size_t>(Page::downloads)] = settings::scroll_limit(three);
    OA_CHECK(layout_has(three, "Install ID \u00b7 North Archive"));

    // Off the main menu the page has no link. On it, REGISTRIES IN THE
    // LIBRARY… is the last row and a click opens the Library on Registries.
    OA_CHECK(!control_named(none, "settings.registries-in-library").found);
    settings::Dialog linked = opened({});
    linked.library_link = true;
    const kit::PlacedRows linked_rows = placed_rows(linked);
    OA_CHECK(linked_rows.rows.size() == 3);
    OA_CHECK(layout_has(linked, "Registries are in the Library."));
    const kit::DisplayList linked_list = geometry::dialog_list(linked, nullptr);
    const kit::Control* link = kit::control_of(
        linked_list, kit::control_named(linked_list, "settings.registries-in-library")
    );
    OA_CHECK(link != nullptr && link->text == "REGISTRIES IN THE LIBRARY…");
    if (link != nullptr) {
        linked.library_tab = settings::LibraryTab::mods;
        const Point at = centre({link->rect.x, link->rect.y, link->rect.width, link->rect.height});
        OA_CHECK(click(linked, at) == DialogAction::open_library);
        OA_CHECK(linked.library_tab == settings::LibraryTab::registries);
    }
}

void values_show_each_state() {
    settings::DownloadsOffer offer = offer_of({core_prime(std::string(kWholeId))});
    settings::Dialog dialog = opened(offer);
    OA_CHECK(value_has(dialog, kShownId));
    OA_CHECK(value_has(dialog, "412 MB kept for resuming and repairs"));

    offer.install_ids[0].id_text = kShownId;
    dialog = opened(offer);
    OA_CHECK(value_has(dialog, kShownId));

    offer.install_ids[0].on = false;
    dialog = opened(offer);
    OA_CHECK(value_has(dialog, "Off"));

    offer.install_ids[0].on = true;
    offer.install_ids[0].made = false;
    offer.install_ids[0].id_text.clear();
    dialog = opened(offer);
    OA_CHECK(!value_has(dialog, "Made at your first download"));
    OA_CHECK(layout_has(dialog, "Made at your first download"));
    OA_CHECK(placed_rows(dialog).rows.size() == 3);

    offer.install_ids[0].made = true;
    offer.install_ids[0].id_text = "abcd";
    dialog = opened(offer);
    OA_CHECK(!value_has(dialog, "Made at your first download"));
    OA_CHECK(layout_has(dialog, "Made at your first download"));
    OA_CHECK(placed_rows(dialog).rows.size() == 3);

    offer = offer_of({core_prime()});
    offer.kept_bytes = 0;
    dialog = opened(offer);
    OA_CHECK(value_has(dialog, "Nothing kept"));

    offer.downloads_busy = true;
    dialog = opened(offer);
    OA_CHECK(value_has(dialog, "Nothing kept \u00b7 in use by downloads"));

    offer.kept_bytes = kKept;
    dialog = opened(offer);
    OA_CHECK(value_has(dialog, "412 MB \u00b7 in use by downloads"));
}

/// Presses a caption's button.
///
/// @param[in,out] dialog the dialog
/// @param caption the caption
/// @return what the release asks; none when the button is not drawn
DialogAction click_caption(settings::Dialog& dialog, std::string_view caption) {
    renderer::SourceRect rect{};
    const bool found = button_rect(dialog, caption, rect);
    OA_CHECK(found);
    if (!found)
        return DialogAction::none;
    return click(dialog, centre(rect));
}

void reset_and_empty_are_disabled_when_they_cannot_act() {
    settings::DownloadsOffer offer = offer_of({core_prime()});
    offer.install_ids[0].on = false;
    settings::Dialog dialog = opened(offer);
    const NamedControl reset = control_named(dialog, "settings.install-id.core-prime.reset");
    OA_CHECK(reset.found && !reset.enabled);
    OA_CHECK(click_caption(dialog, "RESET") != DialogAction::reset_install_id);

    offer.install_ids[0].on = true;
    offer.install_ids[0].made = false;
    dialog = opened(offer);
    OA_CHECK(!control_named(dialog, "settings.install-id.core-prime.reset").found);
    OA_CHECK(layout_has(dialog, "Made at your first download"));
    OA_CHECK(!layout_has(dialog, "Your install ID"));

    offer.kept_bytes = 0;
    dialog = opened(offer);
    const NamedControl empty = control_named(dialog, "settings.downloaded-files.empty");
    OA_CHECK(empty.found && !empty.enabled);
    OA_CHECK(click_caption(dialog, "EMPTY") != DialogAction::empty_downloads);

    offer.kept_bytes = kKept;
    offer.downloads_busy = true;
    dialog = opened(offer);
    const NamedControl busy = control_named(dialog, "settings.downloaded-files.empty");
    OA_CHECK(busy.found && !busy.enabled);
    OA_CHECK(click_caption(dialog, "EMPTY") != DialogAction::empty_downloads);
}

void a_game_locks_reset_and_empty() {
    settings::Dialog dialog = opened(offer_of({core_prime()}));
    dialog.locks = settings::settings_locks(settings::GameState{true, false, false, false});
    // The locks drop under the labels, so the page scrolls. EMPTY is the
    // last row's button; the end of the scroll shows it whole.
    dialog.scroll[static_cast<std::size_t>(Page::downloads)] = settings::scroll_limit(dialog);
    OA_CHECK(layout_has(dialog, "Locked during a game"));
    OA_CHECK(!control_named(dialog, "settings.install-id.core-prime.reset").found);
    OA_CHECK(!control_named(dialog, "settings.downloaded-files.empty").found);
    OA_CHECK(!value_has(dialog, "412 MB kept for resuming and repairs"));
    OA_CHECK(layout_has(dialog, "EMPTY"));
    OA_CHECK(layout_has(dialog, "Your install ID"));
    dialog.focused = settings::first_row_control + 2;
    OA_CHECK(settings::dialog_key(dialog, DialogKey::space) != DialogAction::reset_install_id);
    dialog.focused = settings::first_row_control + 3;
    OA_CHECK(settings::dialog_key(dialog, DialogKey::space) != DialogAction::empty_downloads);

    dialog.focused = settings::first_row_control;
    OA_CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    OA_CHECK(dialog.chosen.content_check == settings::ContentCheck::when_library_opens);
    dialog.focused = settings::first_row_control + 1;
    OA_CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::redraw);
    OA_CHECK(!dialog.install_id_chosen[0].on);
}

void cancel_puts_the_choice_and_the_switches_back() {
    settings::Dialog dialog = opened(offer_of({
        core_prime(),
        registry_row("ridge", "Ridge", true, true, kShownId),
    }));
    dialog.focused = settings::first_row_control;
    OA_CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    dialog.focused = settings::first_row_control + 1;
    OA_CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::redraw);
    OA_CHECK(!dialog.install_id_chosen[0].on);
    OA_CHECK(dialog.install_id_chosen[1].on);

    dialog.focused = settings::first_row_control + 4;
    const DialogAction reset = settings::dialog_key(dialog, DialogKey::space);
    OA_CHECK(reset == DialogAction::reset_install_id);
    OA_CHECK(dialog.install_id_registry == "ridge");
    OA_CHECK(dialog.page == Page::downloads);
    dialog.focused = settings::first_row_control + 5;
    const DialogAction empty = settings::dialog_key(dialog, DialogKey::space);
    OA_CHECK(empty == DialogAction::empty_downloads);
    OA_CHECK(dialog.page == Page::downloads);

    OA_CHECK(
        click(dialog, centre(geometry::footer_button(settings::cancel_control))) ==
        DialogAction::cancelled
    );
    OA_CHECK(dialog.chosen.content_check == settings::ContentCheck::automatically);
    OA_CHECK(dialog.install_id_chosen[0].on);
    OA_CHECK(dialog.install_id_chosen[1].on);
    OA_CHECK(dialog.install_id_registry == "ridge");
    OA_CHECK(dialog.downloads.kept_bytes == kKept);
}

void restore_defaults_turns_the_check_and_the_switches_back() {
    settings::DownloadsOffer offer = offer_of({
        core_prime(),
        registry_row("ridge", "Ridge", true, false, kShownId),
    });
    settings::EngineSettings current;
    current.content_check = settings::ContentCheck::never;
    settings::Dialog dialog;
    settings::open_dialog(dialog, current, {}, {}, "0.8", Page::downloads);
    settings::set_downloads_offer(dialog, offer);
    OA_CHECK(dialog.chosen.content_check == settings::ContentCheck::never);
    dialog.install_id_chosen[0].on = false;
    OA_CHECK(
        click(dialog, centre(geometry::footer_button(settings::restore_control))) ==
        DialogAction::changed
    );
    OA_CHECK(dialog.chosen.content_check == settings::ContentCheck::automatically);
    OA_CHECK(dialog.install_id_chosen[0].on);
    OA_CHECK(dialog.install_id_chosen[1].on);
}

void parts_stay_inside_at_every_class() {
    const std::array<kit::SizeClass, 3> classes{
        kit::SizeClass::compact, kit::SizeClass::regular, kit::SizeClass::large
    };
    for (const kit::SizeClass size_class : classes) {
        settings::Dialog dialog = opened(offer_of({
            core_prime(),
            registry_row("ridge", "Ridge", true, true, kShownId),
            registry_row("north-archive", "North Archive", true, true, kShownId),
        }));
        dialog.size_class = size_class;
        const geometry::Sizes& sized = geometry::sizes_of(dialog);
        const renderer::SourceRect face{
            geometry::edge,
            geometry::edge,
            sized.dialog_width - 2 * geometry::edge,
            sized.dialog_height - 2 * geometry::edge,
        };
        const std::vector<settings::LayoutPart> parts = settings::dialog_layout(dialog);
        OA_CHECK(!parts.empty());
        for (std::size_t index = 0; index < parts.size(); ++index) {
            OA_CHECK(parts[index].rect.width > 0 && parts[index].rect.height > 0);
            OA_CHECK(inside(parts[index].rect, face));
            if (overlap(parts[index].rect, sized.view))
                OA_CHECK(inside(parts[index].rect, sized.view));
            for (std::size_t other = index + 1; other < parts.size(); ++other)
                OA_CHECK(!overlap(parts[index].rect, parts[other].rect));
        }
    }
}

/// Returns every automation name the dialog lists, and checks each is unique.
///
/// @param dialog the dialog
/// @return the names
std::set<std::string> names_of(const settings::Dialog& dialog) {
    const kit::DisplayList list = geometry::dialog_list(dialog, nullptr);
    OA_CHECK(kit::name_problem(list).empty());
    kit::Interaction interaction;
    interaction.focused = dialog.focused;
    interaction.focus_shown = dialog.focused != settings::no_control;
    std::set<std::string> names;
    for (const kit::AutomationEntry& entry : kit::automation_parts(list, interaction))
        OA_CHECK(names.insert(entry.name).second);
    return names;
}

void names_are_unique() {
    settings::Dialog dialog = opened(offer_of({
        core_prime(),
        registry_row("ridge", "Ridge", true, true, kShownId),
    }));
    const std::set<std::string> names = names_of(dialog);
    OA_CHECK(names.count("settings.nav.downloads") == 1);
    OA_CHECK(names.count("settings.content-updates") == 1);
    OA_CHECK(names.count("settings.install-id.core-prime.off") == 1);
    OA_CHECK(names.count("settings.install-id.core-prime.on") == 1);
    OA_CHECK(names.count("settings.install-id.ridge.off") == 1);
    OA_CHECK(names.count("settings.install-id.core-prime.reset") == 1);
    OA_CHECK(names.count("settings.install-id.ridge.reset") == 1);
    OA_CHECK(names.count("settings.downloaded-files.empty") == 1);

    dialog.focused = settings::first_row_control;
    OA_CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::redraw);
    const std::set<std::string> open = names_of(dialog);
    OA_CHECK(open.count("settings.content-updates.automatically") == 1);
    OA_CHECK(open.count("settings.content-updates.library") == 1);
    OA_CHECK(open.count("settings.content-updates.never") == 1);

    const settings::Dialog empty = opened({});
    static_cast<void>(names_of(empty));
    OA_CHECK(control_named(empty, "settings.content-updates").found);
    const NamedControl files = control_named(empty, "settings.downloaded-files.empty");
    OA_CHECK(files.found && !files.enabled);
}

} // namespace

int main() {
    section_list_puts_downloads_before_developer();
    rows_follow_the_registries();
    values_show_each_state();
    reset_and_empty_are_disabled_when_they_cannot_act();
    a_game_locks_reset_and_empty();
    cancel_puts_the_choice_and_the_switches_back();
    restore_defaults_turns_the_check_and_the_switches_back();
    parts_stay_inside_at_every_class();
    names_are_unique();
    return oa::test::check_exit_status();
}
