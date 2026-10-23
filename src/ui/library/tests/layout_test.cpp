// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library's screens over the fixture of fifteen packages, laid out with
// a synthetic font at Compact, Regular and Large, and at the windows of
// 640x480 (1x), 1920x1080 (2x) and 2560x1440 (2x). At each frame: every
// part lies inside the window; no two controls meet, nor their press areas;
// every action of the selected entry is a control (Compact: on its details
// page, MORE's open); UPDATE ALL is one on the Updates tab, over its list
// beside the same filters as Mods, and CLOSE on the list; Compact's details
// page has BACK instead, and no breadcrumb. The header's
// refresh icon is present and SETTINGS is not a footer button; Tab
// follows the declared order and the arrows reach every control that takes
// the focus; the names are step 8's and unique; every text fits, and the
// Rules fact is shown whole. The Active tab is first, has no catalogue
// search and no filters, and its tiles are top-aligned, with UNLOAD MOD on
// the mod tile. Its maps search sits on the MAPS heading.
// Then the keys, the controller's
// buttons, the pointer, a finger, the wheel and typing, each changing the
// model as the screen promises, no press on a row getting anything, and a
// row's download arrow and up-arrow pressed as GET and UPDATE.
//
// The synthetic font's glyph for byte b is 2 + b % 4 columns by 8 rows.

#include "oa/formats/fnt.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/mod_questions.hpp"
#include "oa/ui/library/screen.hpp"
#include "oa/ui/library/text.hpp"
#include "screen_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <initializer_list>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace lib = oa::ui::library;
namespace renderer = oa::ui::frontend_renderer;
namespace fixture = screen_fixture;

/// The synthetic font of the kit's tests, for both roles.
///
/// @return the fonts
kit::Fonts block_fonts() {
    renderer::TextFont font;
    font.font.nominal_height = 8;
    constexpr uint8_t ink_index = 1;
    for (int byte = 0; byte < 256; ++byte) {
        const auto width = static_cast<uint16_t>(2 + byte % 4);
        constexpr uint16_t height = 8;
        const auto count = static_cast<std::size_t>(width) * height;
        std::vector<uint8_t> pixels(count, ink_index);
        std::vector<uint8_t> coverage(count, 1);
        font.font.glyphs[static_cast<std::size_t>(byte)] =
            oa::formats::fnt::Glyph{width, height, 0, 0, std::move(pixels), std::move(coverage)};
    }
    font.ink[ink_index] = static_cast<uint16_t>(renderer::blend_opaque);
    return {font, font, {}, {}};
}

/// The fonts every layout here is measured in.
///
/// @return the fonts
const kit::Fonts& fonts() {
    static const kit::Fonts loaded = block_fonts();
    return loaded;
}

/// Prints each problem a check found, and fails the check when there are any.
///
/// @param problems the problems
/// @param where the frame or case they belong to
/// @param file the check's file
/// @param line the check's line
void expect_none(
    const std::vector<std::string>& problems, const std::string& where, const char* file, int line
) {
    for (const std::string& problem : problems)
        std::fprintf(stderr, "%s:%d: %s: %s\n", file, line, where.c_str(), problem.c_str());
    if (!problems.empty())
        ++oa::test::failed_checks();
}

#define OA_EXPECT_NONE(problems, where) expect_none((problems), (where), __FILE__, __LINE__)

/// Returns the fixture's Library with Ridge, the update that changes the rules, selected.
///
/// @return the Library
lib::Library with_ridge() {
    lib::Library library = fixture::fixture_library();
    lib::select(library, fixture::ridge_id());
    return library;
}

/// Lays the Library out in the synthetic font.
///
/// @param library the model
/// @param state the screen's state
/// @param frame the frame
/// @return the display list
kit::DisplayList
laid_out(const lib::Library& library, const lib::ScreenState& state, const kit::Frame& frame) {
    return lib::library_layout(library, state, frame, fonts());
}

/// Returns a control's number by its name, or no_control.
///
/// @param list the display list
/// @param name the name
/// @return the number
kit::ControlId id_of(const kit::DisplayList& list, std::string_view name) {
    return kit::control_named(list, name);
}

/// Returns a control's centre.
///
/// @param control the control
/// @return the point
kit::Point centre_of(const kit::Control& control) {
    const kit::Rect area = fixture::press_area(control);
    return {area.x + area.width / 2, area.y + area.height / 2};
}

/// Tells whether a name starts with a prefix.
///
/// @param name the name
/// @param prefix the prefix
/// @return true when it does
bool starts(std::string_view name, std::string_view prefix) {
    return name.substr(0, prefix.size()) == prefix;
}

/// Tells whether two controls are the list and one of its rows or a row's
/// state icon, or a row and its own state icon: the rows lie in the list
/// that holds them and each icon in its row, the only controls that meet.
///
/// @param a a control
/// @param b another
/// @return true for the list and a row or an icon, and a row and its icon
bool list_and_row(const kit::Control& a, const kit::Control& b) {
    const auto pair = [](const kit::Control& outer, const kit::Control& inner) {
        if (outer.name == "library.list" && starts(inner.name, "library.row."))
            return true;
        return starts(outer.name, "library.row.") &&
               (inner.name == outer.name + ".get" || inner.name == outer.name + ".update");
    };
    return pair(a, b) || pair(b, a);
}

/// Lists the pairs of controls whose press areas meet. A clip hides the rest
/// of a control, so two rectangles that meet only outside what is shown do
/// not count.
///
/// @param list the display list
/// @return a line for each pair
std::vector<std::string> meeting_controls(const kit::DisplayList& list) {
    std::vector<std::string> problems;
    for (std::size_t a = 0; a < list.controls.size(); ++a) {
        for (std::size_t b = a + 1; b < list.controls.size(); ++b) {
            const kit::Control& first = list.controls[a];
            const kit::Control& second = list.controls[b];
            if (list_and_row(first, second))
                continue;
            if (fixture::has_room(
                    kit::intersect(fixture::press_area(first), fixture::press_area(second))
                ))
                problems.push_back(first.name + " meets " + second.name);
        }
    }
    return problems;
}

/// Returns the place a control's name gives it in the declared order: the
/// tabs, the search, the filters, UPDATE ALL over the list and the list,
/// the details' actions and a map's From link, the Registries tab's rows,
/// then the footer's buttons.
///
/// @param name the name
/// @return the place, or -1 for a name of none
int declared_place(std::string_view name) {
    if (starts(name, "library.tab."))
        return 0;
    if (name == "library.search")
        return 1;
    if (name == "library.filter" || starts(name, "library.filter.") ||
        starts(name, "library.tag.") || name == "library.tags")
        return 2;
    if (name == "library.action.update-all" || name == "library.list")
        return 3;
    if (name == "library.action" || starts(name, "library.action.") ||
        name == "library.active.unload" || name == "library.active.search" ||
        name == "library.active.change" || starts(name, "library.active.expansion.") ||
        starts(name, "library.active.from.") || starts(name, "library.detail.tag."))
        return 4;
    if (starts(name, "library.registry."))
        return 5;
    if (name == "library.refresh")
        return 0;
    if (name == "library.add-registry" || name == "library.details" || name == "library.close" ||
        name == "library.back")
        return 6;
    if (name == "library.close-x")
        return 7;
    return -1;
}

/// Lists how Tab's order differs from the declared one: every control
/// that takes the focus in it, by place, and within a place top to bottom
/// and left to right; and how the arrows, searched from the first stop,
/// fail to reach every one of them.
///
/// @param list the display list
/// @return a line for each difference
std::vector<std::string> order_problems(const kit::DisplayList& list) {
    std::vector<std::string> problems;
    std::set<kit::ControlId> takes_focus;
    for (const kit::Control& control : list.controls)
        if (control.enabled && control.focusable)
            takes_focus.insert(control.id);
    const std::set<kit::ControlId> ordered(list.tab_order.begin(), list.tab_order.end());
    if (ordered != takes_focus || ordered.size() != list.tab_order.size())
        problems.emplace_back("Tab's order is not every control that takes the focus, once");
    const kit::Control* previous = nullptr;
    for (const kit::ControlId id : list.tab_order) {
        const kit::Control* control = kit::control_of(list, id);
        if (control == nullptr) {
            problems.emplace_back("Tab stops on a control the list lacks");
            continue;
        }
        const int place = declared_place(control->name);
        if (place < 0)
            problems.push_back(control->name + " is in no declared place");
        if (previous != nullptr) {
            const int before = declared_place(previous->name);
            const bool later_place = place > before;
            const bool same_place_after =
                place == before &&
                (control->rect.y > previous->rect.y ||
                 (control->rect.y == previous->rect.y && control->rect.x > previous->rect.x));
            if (!later_place && !same_place_after)
                problems.push_back(control->name + " follows " + previous->name);
        }
        previous = control;
    }
    if (list.tab_order.empty())
        return problems;
    std::set<kit::ControlId> reached{list.tab_order.front()};
    std::deque<kit::ControlId> waiting{list.tab_order.front()};
    while (!waiting.empty()) {
        const kit::ControlId from = waiting.front();
        waiting.pop_front();
        for (const kit::Direction direction :
             {kit::Direction::up,
              kit::Direction::down,
              kit::Direction::left,
              kit::Direction::right}) {
            const kit::ControlId next = kit::focus_toward(list, from, direction);
            if (next != kit::no_control && reached.insert(next).second)
                waiting.push_back(next);
        }
    }
    for (const kit::ControlId id : takes_focus)
        if (reached.count(id) == 0)
            problems.push_back("the arrows do not reach " + kit::control_of(list, id)->name);
    return problems;
}

/// Tells whether a name is one step 8 of the screen's design names.
///
/// @param name the name
/// @return true when it is
bool designed_name(const std::string& name) {
    static const std::regex pattern(
        "library\\.(tab\\.(active|mods|maps|mutators|brains|languages|updates|registries|scroll-"
        "left|"
        "scroll-right)|"
        "search|filter|"
        "filter\\.(all|new|installed|updates|tag-[a-z0-9-]+)|tag\\.[a-z0-9-]+|tags|tags\\.[a-z0-9-]"
        "+|detail\\.tag\\.[a-z0-9-]+|"
        "list|row\\.[a-z0-9-]+(\\.[a-z0-9-]+)?(\\.(get|update))?|action|"
        "action\\.(get|update|update-all|cancel|retry|roll-back|play-now|switch-off|"
        "check-with-mod|uninstall|open-folder|homepage)|"
        "details|back|refresh|close-x|close|status|queue|note|from|rules|"
        "active\\.(mod|language|base|expansions|lock|unload|change|search|map\\.[1-9][0-9]*|"
        "from\\.[1-9][0-9]*|expansion\\.[a-z0-9-]+(\\.(get|cancel|retry))?)|"
        "add-registry|registries\\.(note|notice)|"
        "registry\\.[a-z0-9-]+(\\.(remove|add-mirror|key|refresh|address|refreshed))?)"
    );
    return std::regex_match(name, pattern);
}

/// The labels: lines a journey reads, which take nothing.
constexpr std::array<std::string_view, 12> label_names{
    "library.status",
    "library.queue",
    "library.note",
    "library.from",
    "library.rules",
    "library.active.mod",
    "library.active.language",
    "library.active.base",
    "library.active.expansions",
    "library.active.lock",
    "library.registries.note",
    "library.registries.notice",
};

/// Lists what is wrong with the controls' names: the kit's rules, step 8's
/// names, and the labels' kind.
///
/// @param list the display list
/// @return a line for each fault
std::vector<std::string> name_problems(const kit::DisplayList& list) {
    std::vector<std::string> problems;
    const std::string kit_problem = kit::name_problem(list);
    if (!kit_problem.empty())
        problems.push_back(kit_problem);
    for (const kit::Control& control : list.controls)
        if (!designed_name(control.name))
            problems.push_back(control.name + " is not one of the screen's names");
    std::set<std::string> listed;
    for (const kit::AutomationEntry& entry : kit::automation_parts(list, {}))
        if (!listed.insert(entry.name).second)
            problems.push_back(entry.name + " is listed twice to automation");
    for (const std::string_view name : label_names) {
        const kit::Control* label = fixture::named(list, name);
        if (label == nullptr)
            continue;
        if (label->kind != kit::ControlKind::area || label->enabled || label->focusable)
            problems.push_back(std::string(name) + " takes a press, a key or Tab");
    }
    for (const kit::Control& control : list.controls)
        if (control.name != "library.active.unload" && control.name != "library.active.search" &&
            control.name != "library.active.change" &&
            !starts(control.name, "library.active.from.") &&
            !starts(control.name, "library.active.expansion.") &&
            starts(control.name, "library.active.") &&
            (control.kind != kit::ControlKind::area || control.enabled || control.focusable))
            problems.push_back(control.name + " takes a press, a key or Tab");
    return problems;
}

/// Returns the fact of a label.
///
/// @param library the model
/// @param entry the entry
/// @param label the long label, in English
/// @return the fact
std::optional<lib::Fact>
fact_of(const lib::Library& library, const lib::Entry& entry, std::string_view label) {
    for (const lib::Fact& fact : lib::facts(library, entry))
        if (fact.long_label == label)
            return fact;
    return std::nullopt;
}

/// Returns the words of the text items that lie in a rectangle, top to
/// bottom, joined by spaces.
///
/// @param list the display list
/// @param box the rectangle
/// @return the words
std::string words_in(const kit::DisplayList& list, const kit::Rect& box) {
    std::string words;
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::text || !kit::wholly_in(item.rect, box))
            continue;
        if (!words.empty())
            words += ' ';
        words += item.text;
    }
    return words;
}

/// Lists how the selected entry's Rules and From facts are not shown whole.
///
/// @param library the model, with an entry selected
/// @param list the display list showing its details
/// @param long_form the details show the long values
/// @return a line for each fault
std::vector<std::string>
whole_fact_problems(const lib::Library& library, const kit::DisplayList& list, bool long_form) {
    std::vector<std::string> problems;
    const lib::Entry* entry = lib::selected_entry(library);
    if (entry == nullptr)
        return {"nothing is selected"};
    const std::optional<lib::Fact> rules = fact_of(library, *entry, "Rules");
    const kit::Control* rules_label = fixture::named(list, "library.rules");
    if (!rules || rules_label == nullptr) {
        problems.emplace_back("the Rules fact is not shown");
    } else {
        const std::string& value = long_form ? rules->long_value : rules->short_value;
        if (rules_label->text != value)
            problems.push_back(
                "library.rules reads \"" + rules_label->text + "\", not \"" + value + "\""
            );
        if (words_in(list, rules_label->rect) != value)
            problems.push_back(
                "the Rules fact shows \"" + words_in(list, rules_label->rect) + "\""
            );
    }
    const std::optional<lib::Fact> from = fact_of(library, *entry, "From");
    const kit::Control* from_label = fixture::named(list, "library.from");
    if (!from || from_label == nullptr)
        problems.emplace_back("the From fact is not shown");
    else if (from_label->text != from->long_value)
        problems.push_back("library.from reads \"" + from_label->text + "\"");
    return problems;
}

/// Counts the rows a list shows, leaving their state icons out.
///
/// @param list the display list
/// @return how many
std::size_t row_count(const kit::DisplayList& list) {
    return static_cast<std::size_t>(
        std::count_if(list.controls.begin(), list.controls.end(), [](const kit::Control& control) {
            return starts(control.name, "library.row.") &&
                   control.kind == kit::ControlKind::list_item;
        })
    );
}

/// Returns the word an action's button is named by.
///
/// @param action the action
/// @return the word
std::string_view word_of(lib::Action action) {
    switch (action) {
    case lib::Action::get:
        return "get";
    case lib::Action::update:
        return "update";
    case lib::Action::cancel:
        return "cancel";
    case lib::Action::retry:
        return "retry";
    case lib::Action::roll_back:
        return "roll-back";
    case lib::Action::play_now:
        return "play-now";
    case lib::Action::uninstall:
        return "uninstall";
    case lib::Action::open_folder:
        return "open-folder";
    case lib::Action::homepage:
        return "homepage";
    case lib::Action::use_now:
        return "use-now";
    case lib::Action::check_with_mod:
        return "check-with-mod";
    case lib::Action::use_by_default:
        return "use-by-default";
    case lib::Action::get_with:
        return "get-with";
    default:
        return "other";
    }
}

/// Checks a frame's layouts: the list page, the details at Regular and
/// Returns the first text item that shows these words.
///
/// @param list the display list
/// @param text the words
/// @return the item; null when none shows them
const kit::Item* text_item(const kit::DisplayList& list, std::string_view text) {
    for (const kit::Item& item : list.items)
        if (item.role == kit::Role::text && item.text == text)
            return &item;
    return nullptr;
}

/// Returns the smallest card face that holds a text item.
///
/// @param list the display list
/// @param text the item
/// @return the face; null when none holds it
const kit::Item* tile_around(const kit::DisplayList& list, const kit::Item& text) {
    const kit::Item* found = nullptr;
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::fill || item.colour != kit::colour::list_selected)
            continue;
        if (text.rect.x < item.rect.x || text.rect.y < item.rect.y)
            continue;
        if (text.rect.x >= item.rect.x + item.rect.width)
            continue;
        if (text.rect.y >= item.rect.y + item.rect.height)
            continue;
        if (found == nullptr || item.rect.height < found->rect.height)
            found = &item;
    }
    return found;
}

/// Tells whether a layout shows a chip with these words.
///
/// @param list the display list
/// @param text the chip's words
/// @return true when one does
bool shows_chip(const kit::DisplayList& list, std::string_view text) {
    for (const kit::Item& item : list.items)
        if (item.role == kit::Role::chip && item.text == text)
            return true;
    return false;
}

/// Tells whether a layout shows a pill with these words.
///
/// @param list the display list
/// @param text the pill's words
/// @return true when one does
bool shows_pill(const kit::DisplayList& list, std::string_view text) {
    for (const kit::Item& item : list.items)
        if (item.role == kit::Role::pill && item.text == text)
            return true;
    return false;
}

/// Large or the details page at Compact, and the Updates tab.
///
/// @param named the frame
void check_frame(const fixture::NamedFrame& named) {
    const kit::Frame& frame = named.frame;
    const bool compact = frame.size_class == kit::SizeClass::compact;
    const kit::Point window = lib::window_size(frame);
    OA_CHECK(window.x <= frame.area.width && window.y <= frame.area.height);

    lib::Library library = with_ridge();
    lib::ScreenState state;
    const kit::DisplayList list = laid_out(library, state, frame);
    OA_EXPECT_NONE(fixture::outside_window(list, window), named.name + " list");
    OA_EXPECT_NONE(meeting_controls(list), named.name + " list");
    OA_EXPECT_NONE(order_problems(list), named.name + " list");
    OA_EXPECT_NONE(name_problems(list), named.name + " list");
    OA_EXPECT_NONE(fixture::texts_not_fitting(list, fonts()), named.name + " list");
    OA_CHECK(fixture::named(list, "library.refresh") != nullptr);
    OA_CHECK(fixture::named(list, "library.settings") == nullptr);
    OA_CHECK(fixture::named(list, "library.close") != nullptr);
    OA_CHECK(fixture::named(list, "library.list") != nullptr);
    OA_CHECK(fixture::named(list, "library.queue") != nullptr);

    const lib::Entry* ridge = lib::find_entry(library, fixture::ridge_id());
    OA_CHECK(ridge != nullptr);
    if (ridge == nullptr)
        return;
    const std::vector<lib::ActionButton> actions = lib::entry_actions(library, *ridge);
    OA_CHECK(actions.size() == 6);
    if (!compact) {
        for (const lib::ActionButton& action : actions)
            OA_CHECK(
                fixture::named(list, "library.action." + std::string(word_of(action.action))) !=
                nullptr
            );
        OA_EXPECT_NONE(whole_fact_problems(library, list, true), named.name + " details");
        OA_CHECK(fixture::named(list, "library.status") != nullptr);
        OA_CHECK(fixture::named(list, "library.note") != nullptr);
    } else {
        // Compact's details are a page of their own: the first three actions,
        // and MORE ▾ holding the rest.
        lib::open_details(library);
        lib::ScreenState page_state;
        page_state.details_page = true;
        const kit::DisplayList page = laid_out(library, page_state, frame);
        const kit::Control* back = fixture::named(page, "library.back");
        OA_CHECK(back != nullptr && back->text == "BACK");
        OA_CHECK(fixture::named(page, "library.close") == nullptr);
        for (const kit::Control& control : page.controls)
            OA_CHECK(control.text.find("‹") == std::string::npos);
        OA_EXPECT_NONE(fixture::outside_window(page, window), named.name + " details page");
        OA_EXPECT_NONE(meeting_controls(page), named.name + " details page");
        OA_EXPECT_NONE(order_problems(page), named.name + " details page");
        OA_EXPECT_NONE(name_problems(page), named.name + " details page");
        OA_EXPECT_NONE(fixture::texts_not_fitting(page, fonts()), named.name + " details page");
        OA_EXPECT_NONE(whole_fact_problems(library, page, false), named.name + " details page");
        OA_CHECK(fixture::named(page, "library.status") != nullptr);
        OA_CHECK(fixture::named(page, "library.note") != nullptr);
        for (std::size_t index = 0; index < 3; ++index)
            OA_CHECK(
                fixture::named(
                    page, "library.action." + std::string(word_of(actions[index].action))
                ) != nullptr
            );
        OA_CHECK(fixture::named(page, "library.action") != nullptr);
        page_state.menu = lib::MenuKind::more;
        const kit::DisplayList more = laid_out(library, page_state, frame);
        for (const lib::ActionButton& action : actions)
            OA_CHECK(
                fixture::named(more, "library.action." + std::string(word_of(action.action))) !=
                nullptr
            );
        OA_EXPECT_NONE(name_problems(more), named.name + " details page, MORE open");
        OA_EXPECT_NONE(
            fixture::outside_window(more, window), named.name + " details page, MORE open"
        );
        OA_EXPECT_NONE(
            fixture::texts_not_fitting(more, fonts()), named.name + " details page, MORE open"
        );
    }

    lib::Library updates = with_ridge();
    lib::set_tab(updates, lib::Tab::updates);
    const kit::DisplayList updates_list = laid_out(updates, state, frame);
    const kit::Control* update_all = fixture::named(updates_list, "library.action.update-all");
    OA_CHECK(update_all != nullptr && update_all->enabled);
    // UPDATE ALL sits over the list, and the filters are the Mods tab's.
    const kit::Control* updates_pane = fixture::named(updates_list, "library.list");
    OA_CHECK(
        updates_pane != nullptr && update_all != nullptr &&
        update_all->rect.y + update_all->rect.height <= updates_pane->rect.y
    );
    if (compact) {
        OA_CHECK(fixture::named(updates_list, "library.filter") != nullptr);
    } else {
        for (const std::string_view word : {"all", "new", "installed", "updates"})
            OA_CHECK(
                fixture::named(updates_list, "library.filter." + std::string(word)) != nullptr
            );
        const kit::Control* chosen = fixture::named(updates_list, "library.filter.updates");
        OA_CHECK(chosen != nullptr && chosen->checked && chosen->text == "Updates (3)");
    }
    OA_CHECK(fixture::named(updates_list, "library.refresh") != nullptr);
    OA_CHECK(fixture::named(updates_list, "library.settings") == nullptr);
    OA_CHECK(fixture::named(updates_list, "library.close") != nullptr);
    OA_EXPECT_NONE(fixture::outside_window(updates_list, window), named.name + " updates");
    OA_EXPECT_NONE(meeting_controls(updates_list), named.name + " updates");
    OA_EXPECT_NONE(order_problems(updates_list), named.name + " updates");
    OA_EXPECT_NONE(name_problems(updates_list), named.name + " updates");
    OA_EXPECT_NONE(fixture::texts_not_fitting(updates_list, fonts()), named.name + " updates");
    const kit::Control* updates_tab = fixture::named(updates_list, "library.tab.updates");
    // Updates counts the updates that can be installed.
    const std::string updates_count = std::to_string(lib::update_count(library));
    OA_CHECK(updates_tab != nullptr && updates_tab->text == "Updates (" + updates_count + ")");

    // The Active tab: first, no catalogue search, no filters and no list.
    // Its lines are labels. Tab reaches the tabs, UNLOAD MOD, the maps
    // search, each map's From link while that link shows, and the footer's
    // buttons.
    lib::Library active = with_ridge();
    lib::set_tab(active, lib::Tab::active);
    const kit::DisplayList active_list = laid_out(active, state, frame);
    OA_EXPECT_NONE(fixture::outside_window(active_list, window), named.name + " active");
    OA_EXPECT_NONE(meeting_controls(active_list), named.name + " active");
    OA_EXPECT_NONE(order_problems(active_list), named.name + " active");
    OA_EXPECT_NONE(name_problems(active_list), named.name + " active");
    OA_EXPECT_NONE(fixture::texts_not_fitting(active_list, fonts()), named.name + " active");
    const kit::Control* active_tab = fixture::named(active_list, "library.tab.active");
    const kit::Control* mods_tab = fixture::named(active_list, "library.tab.mods");
    OA_CHECK(active_tab != nullptr && active_tab->checked && active_tab->text == "Active");
    OA_CHECK(mods_tab != nullptr && active_tab != nullptr && active_tab->rect.x < mods_tab->rect.x);
    for (const std::string_view absent :
         {"library.search",
          "library.filter",
          "library.filter.all",
          "library.list",
          "library.details",
          "library.action.update-all"})
        OA_CHECK(fixture::named(active_list, absent) == nullptr);
    for (const kit::ControlId id : active_list.tab_order) {
        const kit::Control* stop = kit::control_of(active_list, id);
        OA_CHECK(
            stop != nullptr &&
            (starts(stop->name, "library.tab.") || stop->name == "library.close" ||
             stop->name == "library.close-x" || stop->name == "library.refresh" ||
             stop->name == "library.active.unload" || stop->name == "library.active.search" ||
             stop->name == "library.active.change" ||
             starts(stop->name, "library.active.expansion.") ||
             starts(stop->name, "library.active.from."))
        );
    }
    const kit::Control* mod_line = fixture::named(active_list, "library.active.mod");
    OA_CHECK(mod_line != nullptr && mod_line->text == "Highland 0.4.0");
    const kit::Control* language_line = fixture::named(active_list, "library.active.language");
    OA_CHECK(language_line != nullptr && language_line->text == "Polski 1.0");
    // Folded title, then pack: Atoll, Ridgeline, Twin Rivers. On a small
    // window the tiles can leave the maps off the page. Scrolling brings
    // every map into view. Once the page is scrolled as far as it goes,
    // four rows fit under the heading.
    lib::ScreenState at_maps;
    kit::DisplayList maps_list = active_list;
    if (compact) {
        for (int32_t step = 0; step < 40; ++step) {
            const kit::Control* sample = fixture::named(maps_list, "library.active.map.1");
            const kit::Item* twin = text_item(maps_list, "Twin Rivers");
            if (sample != nullptr && twin != nullptr && twin->rect.y >= sample->clip.y &&
                twin->rect.y + twin->rect.height <= sample->clip.y + sample->clip.height)
                break;
            const int32_t before = at_maps.maps_scroll;
            at_maps.maps_scroll = before + kit::compact_metrics.small_line;
            maps_list = laid_out(active, at_maps, frame);
            if (at_maps.maps_scroll == before)
                break;
        }
    }
    const kit::Control* first_map = fixture::named(maps_list, "library.active.map.1");
    OA_CHECK(first_map != nullptr && first_map->text == "Atoll · 6 players · 20x20");
    if (compact && first_map != nullptr) {
        const kit::Item* atoll_line = text_item(maps_list, "Atoll");
        const kit::Item* twin_line = text_item(maps_list, "Twin Rivers");
        OA_CHECK(atoll_line != nullptr && twin_line != nullptr);
        const int32_t clip_bottom = first_map->clip.y + first_map->clip.height;
        if (atoll_line != nullptr && twin_line != nullptr) {
            OA_CHECK(atoll_line->rect.y >= first_map->clip.y);
            OA_CHECK(twin_line->rect.y + twin_line->rect.height <= clip_bottom);
        }
        // The page, once the maps are in it, holds four compact rows under the heading.
        OA_CHECK(
            first_map->clip.height >= kit::compact_metrics.field_height +
                                          kit::compact_metrics.hint_gap +
                                          4 * kit::compact_metrics.small_line
        );
    }
    lib::ScreenState at_end = at_maps;
    static_cast<void>(lib::library_key(active, at_end, maps_list, kit::Key::end));
    const kit::DisplayList end_list =
        at_end.maps_scroll == at_maps.maps_scroll ? maps_list : laid_out(active, at_end, frame);
    const kit::Control* last_map = fixture::named(end_list, "library.active.map.3");
    OA_CHECK(last_map != nullptr && last_map->text == "Twin Rivers · 2 players · 12x12");
    OA_CHECK(fixture::named(end_list, "library.active.map.4") == nullptr);
    if (compact && last_map != nullptr) {
        const kit::Item* end_heading = nullptr;
        for (const kit::Item& item : end_list.items)
            if (item.role == kit::Role::heading && item.text == "MAPS")
                end_heading = &item;
        OA_CHECK(end_heading != nullptr);
        if (end_heading != nullptr) {
            const int32_t below = last_map->clip.y + last_map->clip.height -
                                  (end_heading->rect.y + end_heading->rect.height);
            OA_CHECK(below >= 4 * kit::compact_metrics.small_line);
        }
    }
    // The three tiles: the kit's card face, the mod's facts, and the language's.
    if (mod_line != nullptr) {
        OA_CHECK(!mod_line->enabled && !mod_line->focusable);
        bool face = false;
        bool border = false;
        // The label stops above UNLOAD MOD, so the tile is at least as tall.
        for (const kit::Item& item : active_list.items) {
            if (item.rect.x != mod_line->rect.x || item.rect.y != mod_line->rect.y ||
                item.rect.width != mod_line->rect.width || item.rect.height < mod_line->rect.height)
                continue;
            if (item.role == kit::Role::fill && item.colour == kit::colour::list_selected)
                face = true;
            if (item.role == kit::Role::outline && item.colour == kit::colour::control_border)
                border = true;
        }
        OA_CHECK(face && border);
    }
    OA_CHECK(shows_pill(active_list, "Playing"));
    OA_CHECK(!shows_chip(active_list, "Playing"));
    const kit::Control* unload = fixture::named(active_list, "library.active.unload");
    OA_CHECK(unload != nullptr && unload->enabled && unload->text == "UNLOAD MOD");
    if (mod_line != nullptr && unload != nullptr)
        OA_CHECK(unload->rect.y >= mod_line->rect.y + mod_line->rect.height);
    const kit::Item* mod_title = text_item(active_list, "Highland");
    const kit::Item* language_title = text_item(active_list, "Polski");
    const kit::Item* base_name = text_item(active_list, "Total Annihilation");
    if (mod_title != nullptr && mod_line != nullptr)
        OA_CHECK(mod_title->rect.y == mod_line->rect.y + kit::compact_metrics.card_inset);
    // Row one is the base game beside the language. Row two is the mod.
    if (base_name != nullptr && language_title != nullptr)
        OA_CHECK(base_name->rect.y == language_title->rect.y);
    if (mod_title != nullptr && base_name != nullptr)
        OA_CHECK(mod_title->rect.y > base_name->rect.y);
    OA_CHECK(text_item(active_list, "by the Highland team") != nullptr);
    OA_CHECK(text_item(active_list, "0.4.0 (rev 1)") != nullptr);
    OA_CHECK(text_item(active_list, "Polish") != nullptr);
    OA_CHECK(text_item(active_list, "1.0 (rev 2)") != nullptr);
    OA_CHECK(text_item(active_list, "Coverage 92%") != nullptr);
    const kit::Item* atoll = text_item(maps_list, "Atoll");
    const kit::Item* ridgeline = text_item(maps_list, "Ridgeline");
    OA_CHECK(atoll != nullptr);
    if (ridgeline == nullptr) {
        OA_CHECK(compact && atoll != nullptr);
        if (atoll != nullptr)
            OA_CHECK(atoll->rect.height == kit::compact_metrics.small_line);
        OA_CHECK(text_item(end_list, "Twin Rivers") != nullptr);
    }
    if (atoll != nullptr && ridgeline != nullptr) {
        if (frame.size_class == kit::SizeClass::large) {
            OA_CHECK(atoll->rect.y == ridgeline->rect.y && atoll->rect.x < ridgeline->rect.x);
        } else {
            OA_CHECK(atoll->rect.y < ridgeline->rect.y && atoll->rect.x == ridgeline->rect.x);
        }
        if (compact) {
            OA_CHECK(atoll->rect.height == kit::compact_metrics.small_line);
            OA_CHECK(ridgeline->rect.height == kit::compact_metrics.small_line);
        }
    }
    // Island Hopping holds Atoll alone, so its title is the link and the
    // tile has no From line. Core Map Pack holds two, so those keep From.
    const kit::Control* from_atoll = fixture::named(maps_list, "library.active.from.1");
    const kit::Control* from_ridge = fixture::named(maps_list, "library.active.from.2");
    const kit::Control* from_twin = fixture::named(maps_list, "library.active.from.3");
    OA_CHECK(text_item(maps_list, "From Island Hopping") == nullptr);
    if (from_atoll != nullptr) {
        OA_CHECK(from_atoll->kind == kit::ControlKind::link);
        OA_CHECK(from_atoll->text == "Atoll");
        OA_CHECK(from_atoll->enabled && from_atoll->focusable);
        if (atoll != nullptr)
            OA_CHECK(from_atoll->rect.y == atoll->rect.y && from_atoll->rect.x == atoll->rect.x);
        if (first_map != nullptr) {
            if (compact) {
                // One line: the title is the link, and the players and size
                // follow it on that same line.
                OA_CHECK(
                    from_atoll->rect.y == first_map->rect.y &&
                    from_atoll->rect.x < first_map->rect.x
                );
            } else {
                OA_CHECK(from_atoll->rect.y < first_map->rect.y);
            }
        }
    }
    if (from_ridge != nullptr) {
        OA_CHECK(from_ridge->text == "From Core Map Pack");
        if (ridgeline != nullptr) {
            if (compact)
                OA_CHECK(
                    from_ridge->rect.y == ridgeline->rect.y &&
                    from_ridge->rect.x > ridgeline->rect.x
                );
            else
                OA_CHECK(from_ridge->rect.y > ridgeline->rect.y);
        }
    }
    if (from_twin != nullptr)
        OA_CHECK(from_twin->text == "From Core Map Pack");
    else {
        const kit::Control* from_end = fixture::named(end_list, "library.active.from.3");
        OA_CHECK(from_end != nullptr && from_end->text == "From Core Map Pack");
    }
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    const kit::Item* atoll_tile = atoll != nullptr ? tile_around(maps_list, *atoll) : nullptr;
    const kit::Item* ridge_tile =
        ridgeline != nullptr ? tile_around(maps_list, *ridgeline) : nullptr;
    if (atoll_tile != nullptr && ridge_tile != nullptr) {
        const int32_t side = metrics.list_row_badge + 2 * metrics.card_inset;
        const auto tile_height = [&](int32_t lines) {
            return metrics.card_inset + std::max(side, lines) + metrics.card_inset;
        };
        const int32_t atoll_height = tile_height(metrics.regular_line + metrics.small_line);
        const int32_t ridge_height = tile_height(metrics.regular_line + 2 * metrics.small_line);
        if (frame.size_class == kit::SizeClass::large) {
            // Side by side, the row takes the taller tile.
            OA_CHECK(atoll_tile->rect.height == ridge_tile->rect.height);
            OA_CHECK(atoll_tile->rect.height == ridge_height);
        } else {
            OA_CHECK(atoll_tile->rect.height == atoll_height);
            OA_CHECK(ridge_tile->rect.height == ridge_height);
            OA_CHECK(atoll_height < ridge_height);
        }
    }
    // Row one is the language. Row two is the mod, in the same column.
    if (mod_line != nullptr && language_line != nullptr) {
        OA_CHECK(
            language_line->rect.y < mod_line->rect.y && language_line->rect.x == mod_line->rect.x
        );
    }
}

/// Checks every frame.
void every_frame_holds_its_invariants() {
    for (const fixture::NamedFrame& named : fixture::frames())
        check_frame(named);
}

/// Regular shows more rows than Compact, and Large has the filter pane.
void larger_classes_show_more() {
    const lib::Library library = with_ridge();
    const lib::ScreenState state;
    const std::vector<fixture::NamedFrame> all = fixture::frames();
    const kit::DisplayList compact = laid_out(library, state, all[0].frame);
    const kit::DisplayList regular = laid_out(library, state, all[1].frame);
    const kit::DisplayList large = laid_out(library, state, all[2].frame);
    OA_CHECK(row_count(regular) > row_count(compact));
    OA_CHECK(row_count(compact) >= 1);
    const auto has_nav = [](const kit::DisplayList& list) {
        return std::any_of(list.items.begin(), list.items.end(), [](const kit::Item& item) {
            return item.role == kit::Role::nav;
        });
    };
    OA_CHECK(has_nav(large));
    OA_CHECK(!has_nav(regular) && !has_nav(compact));
    const kit::Control* all_entry = fixture::named(large, "library.filter.all");
    OA_CHECK(all_entry != nullptr && all_entry->kind == kit::ControlKind::tab);
    OA_CHECK(fixture::named(regular, "library.filter.all") != nullptr);
    OA_CHECK(fixture::named(compact, "library.filter") != nullptr);
    // The tabs keep their full names. Active is the first stop; the close
    // mark is last, after the footer.
    const kit::Control* first_stop = kit::control_of(compact, compact.tab_order.front());
    OA_CHECK(first_stop != nullptr && first_stop->name == "library.tab.active");
    const kit::Control* last_stop = kit::control_of(compact, compact.tab_order.back());
    OA_CHECK(last_stop != nullptr && last_stop->name == "library.close-x");
    const kit::Control* languages = fixture::named(compact, "library.tab.languages");
    OA_CHECK(languages != nullptr && languages->text == "Languages (2)");
    const kit::Control* updates = fixture::named(compact, "library.tab.updates");
    OA_CHECK(
        updates != nullptr &&
        updates->text == "Updates (" + std::to_string(lib::update_count(library)) + ")"
    );
}

/// Languages lists each built-in language ahead of the packs. A built-in
/// row's chip says "Built in", and the language in use says "In use".
/// USE NOW is in the tab order; a pack that is not installed has GET only.
void built_in_languages_show_on_the_list() {
    lib::Library library = fixture::fixture_library();
    lib::set_tab(library, lib::Tab::languages);
    const kit::DisplayList list = laid_out(library, {}, fixture::frames()[1].frame);
    bool deutsch = false;
    for (const kit::Item& item : list.items) {
        const auto* row = std::get_if<kit::ListRowLook>(&item.look);
        if (row == nullptr || row->title != "Deutsch")
            continue;
        bool german = false;
        for (const std::string& line : row->lines)
            german = german || line == "German";
        deutsch = row->chip.has_value() && row->chip->text == "Built in" &&
                  !row->chip->icon.has_value() && german;
    }
    OA_CHECK(deutsch);
    const kit::Control* deutsch_row = fixture::named(list, "library.row.language-de");
    OA_CHECK(deutsch_row != nullptr && deutsch_row->text == "Deutsch");

    lib::select(library, lib::built_in_language_id("de"));
    const kit::DisplayList details = laid_out(library, {}, fixture::frames()[1].frame);
    const kit::Control* use_now = fixture::named(details, "library.action.use-now");
    OA_CHECK(use_now != nullptr && use_now->text == "USE NOW" && use_now->enabled);
    if (use_now != nullptr)
        OA_CHECK(
            std::find(details.tab_order.begin(), details.tab_order.end(), use_now->id) !=
            details.tab_order.end()
        );
    bool told = false;
    for (const kit::Item& item : details.items)
        told = told || (item.role == kit::Role::text && item.text == "Built into the game");
    OA_CHECK(told);
    OA_CHECK(fixture::named(details, "library.action.get") == nullptr);
    OA_CHECK(fixture::named(details, "library.action.uninstall") == nullptr);

    lib::select(library, {std::string(fixture::added), lib::Kind::language, "pl"});
    const kit::DisplayList in_use = laid_out(library, {}, fixture::frames()[1].frame);
    bool marked = false;
    for (const kit::Item& item : in_use.items) {
        const auto* chip = std::get_if<kit::ChipLook>(&item.look);
        marked = marked || (chip != nullptr && chip->text == "In use" && !chip->icon.has_value());
    }
    OA_CHECK(marked);
    OA_CHECK(fixture::named(in_use, "library.action.use-now") == nullptr);

    lib::select(library, {std::string(fixture::core), lib::Kind::language, "pt-BR"});
    const kit::DisplayList listed = laid_out(library, {}, fixture::frames()[1].frame);
    OA_CHECK(fixture::named(listed, "library.action.get") != nullptr);
    OA_CHECK(fixture::named(listed, "library.action.use-now") == nullptr);
}

/// A row reads its entry's name, and a row's chip says its state.
void rows_name_their_entries() {
    const lib::Library library = with_ridge();
    const kit::DisplayList list = laid_out(library, {}, fixture::frames()[1].frame);
    const kit::Control* base = fixture::named(list, "library.row.base-game");
    OA_CHECK(
        base != nullptr && base->text == "No Mod" && base->kind == kit::ControlKind::list_item &&
        !base->checked
    );
    OA_CHECK(
        !library.visible.empty() && library.entries[library.visible.front()].id.key == "@base-game"
    );
    const kit::Control* ridge = fixture::named(list, "library.row.fixture-core.ridge");
    OA_CHECK(
        ridge != nullptr && ridge->text == "Ridge" && ridge->kind == kit::ControlKind::list_item
    );
    OA_CHECK(ridge != nullptr && ridge->checked);
    const kit::Control* frontier = fixture::named(list, "library.row.example-mods.frontier");
    OA_CHECK(frontier != nullptr && !frontier->checked);
    const kit::Control* pane = fixture::named(list, "library.list");
    OA_CHECK(pane != nullptr && pane->kind == kit::ControlKind::list && pane->text.empty());
    OA_CHECK(lib::badge_letters("Ridge") == "RID");
    OA_CHECK(lib::badge_letters("TA Zero") == "TZ");
    OA_CHECK(lib::badge_letters("Star of the Sea Pack") == "SOT");
    OA_CHECK(lib::badge_letters("ab") == "AB");
}

/// A mod of the player's own, which no listing describes, shows its
/// manifest's description in the details.
void an_own_mods_details_show_its_description() {
    lib::Inputs inputs = fixture::fixture_inputs();
    lib::Installed own;
    own.kind = lib::Kind::mod;
    own.key = "own-description";
    own.folder = "own-description";
    own.name = "Own Description";
    own.version = "1.0";
    own.description = "A mod of the player's own.";
    inputs.installed.push_back(own);
    lib::Library library;
    lib::refresh(library, std::move(inputs));
    lib::set_tab(library, lib::Tab::mods);
    lib::select(library, {"", lib::Kind::mod, "own-description"});
    OA_CHECK(library.selected.has_value());
    for (const fixture::NamedFrame& named : fixture::frames()) {
        if (named.frame.size_class == kit::SizeClass::compact)
            continue;
        const kit::DisplayList list = laid_out(library, {}, named.frame);
        bool shown = false;
        for (const kit::Item& item : list.items)
            shown = shown ||
                    (item.role == kit::Role::text && item.text == "A mod of the player's own.");
        if (!shown)
            std::fprintf(stderr, "%s: the description is not in the details\n", named.name.c_str());
        OA_CHECK(shown);
    }
}

/// An empty list says why, and a list the last fetch could not refresh says so above it.
void empty_and_offline_lists_say_why() {
    lib::Library library = with_ridge();
    lib::set_query(library, "zzz");
    const kit::DisplayList list = laid_out(library, {}, fixture::frames()[0].frame);
    bool found = false;
    for (const kit::Item& item : list.items)
        found = found || (item.role == kit::Role::text && item.text == "Nothing matches \"zzz\".");
    OA_CHECK(found);
    OA_CHECK(row_count(list) == 0);

    lib::Inputs inputs = fixture::fixture_inputs();
    inputs.offline = true;
    lib::Library offline;
    lib::refresh(offline, std::move(inputs));
    lib::set_tab(offline, lib::Tab::mods);
    const kit::DisplayList offline_list = laid_out(offline, {}, fixture::frames()[0].frame);
    bool noted = false;
    for (const kit::Item& item : offline_list.items)
        noted = noted ||
                (item.role == kit::Role::text &&
                 item.text == "Showing the list from 4 min ago. OA couldn't reach the registries.");
    OA_CHECK(noted);
    bool marked = false;
    for (const kit::Item& item : offline_list.items)
        marked = marked || (item.role == kit::Role::text && item.text == "offline" &&
                            item.colour == kit::colour::lock);
    OA_CHECK(marked);
    OA_EXPECT_NONE(fixture::outside_window(offline_list, {480, 324}), "offline");
    OA_EXPECT_NONE(meeting_controls(offline_list), "offline");

    // No registry and no package: Mods still starts with the base game, so
    // the empty-list line is not shown.
    lib::Library none;
    lib::refresh(none, {});
    lib::set_tab(none, lib::Tab::mods);
    const kit::DisplayList none_list = laid_out(none, {}, fixture::frames()[0].frame);
    const kit::Control* base_row = fixture::named(none_list, "library.row.base-game");
    OA_CHECK(base_row != nullptr && base_row->text == "No Mod" && base_row->checked);
    bool told = false;
    for (const kit::Item& item : none_list.items)
        told =
            told || (item.role == kit::Role::text &&
                     item.text == "Nothing here yet. Turn a registry on in Settings › Downloads.");
    OA_CHECK(!told);
}

/// Returns a state whose focus shows on a named control.
///
/// @param list the display list
/// @param name the control's name
/// @return the state
lib::ScreenState focused_on(const kit::DisplayList& list, std::string_view name) {
    lib::ScreenState state;
    state.interaction.focused = id_of(list, name);
    state.interaction.focus_shown = true;
    return state;
}

/// Enter on the list opens the details page at Compact, and moves the focus
/// to the first enabled action at Regular.
void enter_opens_details_or_reaches_the_actions() {
    const std::vector<fixture::NamedFrame> all = fixture::frames();
    lib::Library library = with_ridge();
    kit::DisplayList list = laid_out(library, {}, all[0].frame);
    lib::ScreenState state = focused_on(list, "library.list");
    lib::ScreenResult result = lib::library_key(library, state, list, kit::Key::enter);
    OA_CHECK(result.redraw && !result.action);
    OA_CHECK(library.details_open && state.details_page);
    list = laid_out(library, state, all[0].frame);
    OA_CHECK(fixture::named(list, "library.back") != nullptr);
    OA_CHECK(state.interaction.focused == id_of(list, "library.action.update"));

    lib::Library wide = with_ridge();
    kit::DisplayList wide_list = laid_out(wide, {}, all[1].frame);
    lib::ScreenState wide_state = focused_on(wide_list, "library.list");
    result = lib::library_key(wide, wide_state, wide_list, kit::Key::enter);
    OA_CHECK(result.redraw && !result.action);
    OA_CHECK(!wide.details_open);
    OA_CHECK(wide_state.interaction.focused == id_of(wide_list, "library.action.update"));
}

/// Escape clears a search that has the focus, else leaves the details page,
/// else closes.
void escape_clears_leaves_or_closes() {
    const kit::Frame compact = fixture::frames()[0].frame;
    lib::Library library = with_ridge();
    kit::DisplayList list = laid_out(library, {}, compact);
    lib::ScreenState state = focused_on(list, "library.search");
    OA_CHECK(lib::library_text(library, state, list, "ri").redraw);
    OA_CHECK(library.query == "ri" && state.search.text == "ri");
    list = laid_out(library, state, compact);
    lib::ScreenResult result = lib::library_key(library, state, list, kit::Key::escape);
    OA_CHECK(!result.action && library.query.empty() && state.search.text.empty());

    lib::select(library, fixture::ridge_id());
    lib::open_details(library);
    state = {};
    state.details_page = true;
    list = laid_out(library, state, compact);
    result = lib::library_key(library, state, list, kit::Key::escape);
    OA_CHECK(!result.action && !library.details_open && !state.details_page);

    list = laid_out(library, state, compact);
    result = lib::library_key(library, state, list, kit::Key::escape);
    OA_CHECK(result.action == lib::Action::close);
}

/// Ctrl+Tab and Ctrl+Shift+Tab, and the controller's RB and LB, change tabs
/// both ways round.
void tab_commands_change_tabs() {
    lib::Library library = with_ridge();
    lib::ScreenState state;
    const kit::Frame regular = fixture::frames()[1].frame;
    kit::DisplayList list = laid_out(library, state, regular);
    OA_CHECK(lib::library_command(library, state, list, lib::ScreenCommand::next_tab).redraw);
    OA_CHECK(library.tab == lib::Tab::maps);
    list = laid_out(library, state, regular);
    static_cast<void>(lib::library_command(library, state, list, lib::ScreenCommand::previous_tab));
    OA_CHECK(library.tab == lib::Tab::mods);
    list = laid_out(library, state, regular);
    static_cast<void>(lib::library_command(library, state, list, lib::ScreenCommand::previous_tab));
    OA_CHECK(library.tab == lib::Tab::active);
    list = laid_out(library, state, regular);
    static_cast<void>(lib::library_command(library, state, list, lib::ScreenCommand::previous_tab));
    OA_CHECK(library.tab == lib::Tab::registries);
    list = laid_out(library, state, regular);
    static_cast<void>(lib::library_command(library, state, list, lib::ScreenCommand::next_tab));
    OA_CHECK(library.tab == lib::Tab::active);
    list = laid_out(library, state, regular);
    static_cast<void>(lib::library_command(library, state, list, lib::ScreenCommand::next_tab));
    OA_CHECK(library.tab == lib::Tab::mods);
}

/// The controller's X asks for the selected entry's first action when it is
/// GET, UPDATE, CANCEL or RETRY and enabled; Y gives the search the focus;
/// B leaves the details page, else closes.
void controller_buttons() {
    const kit::Frame compact = fixture::frames()[0].frame;
    lib::Library library = with_ridge();
    lib::ScreenState state;
    kit::DisplayList list = laid_out(library, state, compact);
    lib::ScreenResult result =
        lib::library_command(library, state, list, lib::ScreenCommand::first_action);
    OA_CHECK(result.action == lib::Action::update && result.entry == fixture::ridge_id());

    lib::select(library, {std::string(fixture::added), lib::Kind::mod, "hollow"});
    result = lib::library_command(library, state, list, lib::ScreenCommand::first_action);
    OA_CHECK(!result.action);
    lib::select(library, {std::string(fixture::core), lib::Kind::mod, "bastion"});
    result = lib::library_command(library, state, list, lib::ScreenCommand::first_action);
    OA_CHECK(!result.action);
    lib::select(library, {std::string(fixture::core), lib::Kind::mod, "ta-zero"});
    result = lib::library_command(library, state, list, lib::ScreenCommand::first_action);
    OA_CHECK(result.action == lib::Action::cancel);

    result = lib::library_command(library, state, list, lib::ScreenCommand::find);
    list = laid_out(library, state, compact);
    OA_CHECK(
        state.interaction.focus_shown && state.interaction.focused == id_of(list, "library.search")
    );
    OA_CHECK(lib::focused_field(state, list).has_value());

    lib::select(library, fixture::ridge_id());
    lib::open_details(library);
    state = {};
    state.details_page = true;
    list = laid_out(library, state, compact);
    result = lib::library_command(library, state, list, lib::ScreenCommand::back);
    OA_CHECK(!result.action && !library.details_open && !state.details_page);
    list = laid_out(library, state, compact);
    result = lib::library_command(library, state, list, lib::ScreenCommand::back);
    OA_CHECK(result.action == lib::Action::close);
}

/// On the list, Up and Down move the selection and keep the focus; past the
/// last row the focus leaves the list. Page Down moves by the rows shown,
/// Home and End go to the ends, and the list scrolls to keep the selection.
void list_keys_move_the_selection() {
    const kit::Frame compact = fixture::frames()[0].frame;
    lib::Library library = fixture::fixture_library();
    kit::DisplayList list = laid_out(library, {}, compact);
    lib::ScreenState state = focused_on(list, "library.list");
    const kit::ControlId pane = state.interaction.focused;
    const auto selected_place = [&library]() {
        for (std::size_t place = 0; place < library.visible.size(); ++place)
            if (library.entries[library.visible[place]].id == *library.selected)
                return place;
        return library.visible.size();
    };
    OA_CHECK(selected_place() == 0);
    OA_CHECK(lib::library_key(library, state, list, kit::Key::down).redraw);
    OA_CHECK(selected_place() == 1 && state.interaction.focused == pane);
    const std::size_t shown_rows = row_count(list);
    list = laid_out(library, state, compact);
    static_cast<void>(lib::library_key(library, state, list, kit::Key::page_down));
    OA_CHECK(selected_place() == 1 + shown_rows);
    OA_CHECK(state.list_scroll > 0);
    list = laid_out(library, state, compact);
    static_cast<void>(lib::library_key(library, state, list, kit::Key::home));
    OA_CHECK(selected_place() == 0 && state.list_scroll == 0);
    list = laid_out(library, state, compact);
    static_cast<void>(lib::library_key(library, state, list, kit::Key::end));
    OA_CHECK(selected_place() + 1 == library.visible.size());
    list = laid_out(library, state, compact);
    OA_CHECK(
        fixture::named(list, "library.row.fixture-core.quarry") != nullptr || row_count(list) > 0
    );
    static_cast<void>(lib::library_key(library, state, list, kit::Key::down));
    OA_CHECK(selected_place() + 1 == library.visible.size());
    OA_CHECK(state.interaction.focused != pane);
    // Left and Right move by where controls sit, never along Tab's order.
    lib::select(library, fixture::ridge_id());
    list = laid_out(library, state, fixture::frames()[1].frame);
    state = focused_on(list, "library.list");
    static_cast<void>(lib::library_key(library, state, list, kit::Key::right));
    const kit::Control* focused = kit::control_of(list, state.interaction.focused);
    OA_CHECK(focused != nullptr && starts(focused->name, "library.action."));
}

/// Tab and Shift+Tab follow the declared order.
void tab_follows_the_declared_order() {
    lib::Library library = with_ridge();
    const kit::Frame large = fixture::frames()[2].frame;
    kit::DisplayList list = laid_out(library, {}, large);
    lib::ScreenState state;
    std::vector<kit::ControlId> visited;
    for (std::size_t step = 0; step < list.tab_order.size(); ++step) {
        static_cast<void>(lib::library_key(library, state, list, kit::Key::tab));
        visited.push_back(state.interaction.focused);
        list = laid_out(library, state, large);
    }
    OA_CHECK(visited == list.tab_order);
    static_cast<void>(lib::library_key(library, state, list, kit::Key::back_tab));
    OA_CHECK(state.interaction.focused == list.tab_order[list.tab_order.size() - 2]);

    // The orders, by name: Compact's list page and its details page, and
    // Regular's ends.
    const auto names_of = [](const kit::DisplayList& laid) {
        std::vector<std::string> names;
        for (const kit::ControlId id : laid.tab_order)
            names.push_back(kit::control_of(laid, id)->name);
        return names;
    };
    const kit::Frame compact = fixture::frames()[0].frame;
    const std::vector<std::string> compact_order{
        "library.tab.active",
        "library.tab.mods",
        "library.tab.maps",
        "library.tab.mutators",
        "library.tab.brains",
        "library.tab.languages",
        "library.tab.updates",
        "library.tab.registries",
        "library.search",
        "library.filter",
        "library.list",
        "library.details",
        "library.close",
        "library.close-x",
    };
    OA_CHECK(names_of(laid_out(library, {}, compact)) == compact_order);
    lib::open_details(library);
    lib::ScreenState page;
    page.details_page = true;
    const std::vector<std::string> page_order{
        "library.detail.tag.balance",
        "library.action.update",
        "library.action.play-now",
        "library.action.roll-back",
        "library.action",
        "library.back",
        "library.close-x",
    };
    OA_CHECK(names_of(laid_out(library, page, compact)) == page_order);
    lib::close_details(library);
    const std::vector<std::string> regular =
        names_of(laid_out(library, {}, fixture::frames()[1].frame));
    const std::vector<std::string> regular_end{
        "library.list",
        "library.detail.tag.balance",
        "library.action.update",
        "library.action.play-now",
        "library.action.roll-back",
        "library.action.uninstall",
        "library.action.open-folder",
        "library.action.homepage",
        "library.close",
        "library.close-x",
    };
    OA_CHECK(regular.size() > regular_end.size() + 5);
    OA_CHECK(std::equal(regular_end.rbegin(), regular_end.rend(), regular.rbegin()));
    OA_CHECK(
        regular.size() > 12 && regular[0] == "library.tab.active" &&
        regular[8] == "library.search" && regular[9] == "library.filter.all" &&
        regular[10] == "library.filter.new"
    );
}

/// Presses a control with the pointer: a press and a release at its centre.
///
/// @param library the model
/// @param state the screen's state
/// @param list the display list
/// @param name the control's name
/// @param finger_reach 0 for a mouse, else a finger's reach
/// @return what the release asked of the host
lib::ScreenResult click(
    lib::Library& library,
    lib::ScreenState& state,
    const kit::DisplayList& list,
    std::string_view name,
    int32_t finger_reach = 0
) {
    const kit::Control* control = fixture::named(list, name);
    OA_CHECK(control != nullptr);
    if (control == nullptr)
        return {};
    const kit::Point at = centre_of(*control);
    static_cast<void>(
        lib::library_pointer(library, state, list, lib::PointerKind::down, 1, at, finger_reach)
    );
    return lib::library_pointer(library, state, list, lib::PointerKind::up, 1, at, finger_reach);
}

/// A click, a double click or a tap on a row selects it and gets nothing; at
/// Compact it opens the row's details page. An action's button asks for its
/// action, and a label takes no press.
void pressing_rows_selects_and_never_gets() {
    const std::vector<fixture::NamedFrame> all = fixture::frames();
    lib::Library library = with_ridge();
    lib::ScreenState state;
    kit::DisplayList list = laid_out(library, state, all[1].frame);
    const lib::EntryId frontier{std::string(fixture::added), lib::Kind::mod, "frontier"};
    lib::ScreenResult result = click(library, state, list, "library.row.example-mods.frontier");
    OA_CHECK(!result.action && library.selected == frontier);
    list = laid_out(library, state, all[1].frame);
    result = click(library, state, list, "library.row.example-mods.frontier");
    OA_CHECK(!result.action && library.selected == frontier);
    result = click(library, state, list, "library.row.example-mods.frontier", 12);
    OA_CHECK(!result.action && library.selected == frontier && !library.details_open);

    list = laid_out(library, state, all[1].frame);
    result = click(library, state, list, "library.action.cancel");
    OA_CHECK(result.action == lib::Action::cancel && result.entry == frontier);
    const kit::Control* status = fixture::named(list, "library.status");
    OA_CHECK(status != nullptr && kit::hit(list, centre_of(*status)) != status->id);

    lib::Library small = with_ridge();
    lib::ScreenState small_state;
    kit::DisplayList small_list = laid_out(small, small_state, all[0].frame);
    const lib::EntryId cinder{std::string(fixture::core), lib::Kind::mod, "cinder"};
    result = click(small, small_state, small_list, "library.row.fixture-core.cinder");
    OA_CHECK(!result.action && small.selected == cinder);
    OA_CHECK(small.details_open && small_state.details_page);
    small_list = laid_out(small, small_state, all[0].frame);
    OA_CHECK(fixture::named(small_list, "library.back") != nullptr);
    result = click(small, small_state, small_list, "library.back");
    OA_CHECK(!result.action && !small.details_open);
    small_list = laid_out(small, small_state, all[0].frame);
    result = click(small, small_state, small_list, "library.close");
    OA_CHECK(result.action == lib::Action::close);
    // The header's refresh icon asks for nothing until a registry can be
    // asked, and nothing while the registries are being asked.
    const kit::Control* refresh_off = fixture::named(small_list, "library.refresh");
    OA_CHECK(refresh_off != nullptr && !refresh_off->enabled && refresh_off->text == "Refresh");
    result = click(small, small_state, small_list, "library.refresh");
    OA_CHECK(!result.action);
    OA_CHECK(
        lib::library_command(small, small_state, small_list, lib::ScreenCommand::refresh).action ==
        std::nullopt
    );
    small.inputs.can_check = true;
    small_list = laid_out(small, small_state, all[0].frame);
    const kit::Control* refresh_on = fixture::named(small_list, "library.refresh");
    OA_CHECK(refresh_on != nullptr && refresh_on->enabled && refresh_on->text == "Refresh");
    result = click(small, small_state, small_list, "library.refresh");
    OA_CHECK(result.action == lib::Action::check_now);
    OA_CHECK(
        lib::library_command(small, small_state, small_list, lib::ScreenCommand::refresh).action ==
        lib::Action::check_now
    );
    small.inputs.checking = true;
    small_state.refresh_turn = 2;
    small_list = laid_out(small, small_state, all[0].frame);
    const kit::Control* refreshing = fixture::named(small_list, "library.refresh");
    OA_CHECK(refreshing != nullptr && !refreshing->enabled && refreshing->text == "Refreshing");
    result = click(small, small_state, small_list, "library.refresh");
    OA_CHECK(!result.action);
    bool turning = false;
    for (const kit::Item& item : small_list.items) {
        if (item.role != kit::Role::button || refreshing == nullptr ||
            item.control != refreshing->id)
            continue;
        const auto* look = std::get_if<kit::ButtonLook>(&item.look);
        turning = look != nullptr && look->mark == kit::ButtonMark::refresh && look->turning &&
                  look->turn == 2;
    }
    OA_CHECK(turning);
}

/// A row's state icon: a circled up-arrow pressed as UPDATE and a download
/// arrow pressed as GET, each a button named after its row with the word it
/// replaces as its text, enabled only while that button is, at every size
/// class. A press on one asks for its action for its row's entry and
/// selects it; a blocked or queued one takes no press, which reaches the
/// row instead. An installed row's circled check is no control.
void row_icons_are_get_and_update() {
    for (const fixture::NamedFrame& named : fixture::frames()) {
        lib::Library library = with_ridge();
        lib::ScreenState state;
        kit::DisplayList list = laid_out(library, state, named.frame);
        const kit::Control* row = fixture::named(list, "library.row.fixture-core.ridge");
        const kit::Control* update = fixture::named(list, "library.row.fixture-core.ridge.update");
        OA_CHECK(row != nullptr && update != nullptr);
        if (row == nullptr || update == nullptr)
            continue;
        OA_CHECK(update->kind == kit::ControlKind::button && update->enabled);
        OA_CHECK(update->text == "UPDATE" && !update->focusable);
        OA_CHECK(kit::wholly_in(update->rect, row->rect));
        OA_CHECK(update->rect.width == kit::compact_metrics.state_icon_side);
        OA_CHECK(kit::hit(list, centre_of(*update)) == update->id);
        OA_CHECK(fixture::named(list, "library.row.fixture-core.bastion.get") == nullptr);
        OA_CHECK(fixture::named(list, "library.row.fixture-core.bastion.update") == nullptr);
    }

    const kit::Frame regular = fixture::frames()[1].frame;
    lib::Library library = fixture::fixture_library();
    lib::ScreenState state;
    kit::DisplayList list = laid_out(library, state, regular);
    lib::ScreenResult result = click(library, state, list, "library.row.fixture-core.ridge.update");
    OA_CHECK(result.action == lib::Action::update && result.entry == fixture::ridge_id());
    OA_CHECK(library.selected == fixture::ridge_id() && !library.details_open);

    // A blocked update: dimmed, listed, and no press; a press there selects the row.
    const lib::EntryId ironclad{std::string(fixture::core), lib::Kind::mod, "ironclad"};
    list = laid_out(library, state, regular);
    const kit::Control* blocked = fixture::named(list, "library.row.fixture-core.ironclad.update");
    OA_CHECK(blocked != nullptr && !blocked->enabled && blocked->text == "UPDATE");
    if (blocked != nullptr) {
        const kit::Point at = centre_of(*blocked);
        static_cast<void>(
            lib::library_pointer(library, state, list, lib::PointerKind::down, 1, at, 0)
        );
        result = lib::library_pointer(library, state, list, lib::PointerKind::up, 1, at, 0);
        OA_CHECK(!result.action && library.selected == ironclad);
    }

    // GET: one that can act asks for it; one blocked by missing hacks, and
    // one already queued, take no press.
    lib::set_query(library, "quarry");
    list = laid_out(library, state, regular);
    const kit::Control* get = fixture::named(list, "library.row.fixture-core.quarry.get");
    OA_CHECK(get != nullptr && get->enabled && get->text == "GET");
    const lib::EntryId quarry{std::string(fixture::core), lib::Kind::mod, "quarry"};
    result = click(library, state, list, "library.row.fixture-core.quarry.get");
    OA_CHECK(result.action == lib::Action::get && result.entry == quarry);
    for (const std::string_view key : {"hollow", "frontier"}) {
        lib::set_query(library, key);
        list = laid_out(library, state, regular);
        const std::string name = "library.row.example-mods." + std::string(key) + ".get";
        const kit::Control* off = fixture::named(list, name);
        OA_CHECK(off != nullptr && !off->enabled);
        if (off != nullptr)
            OA_CHECK(kit::hit(list, centre_of(*off)) != off->id);
    }
}

/// The wheel scrolls the pane under the pointer; a finger's drag in the
/// list scrolls it and selects nothing.
void wheel_and_drag_scroll() {
    const kit::Frame compact = fixture::frames()[0].frame;
    lib::Library library = with_ridge();
    lib::ScreenState state;
    kit::DisplayList list = laid_out(library, state, compact);
    const kit::Control* pane = fixture::named(list, "library.list");
    OA_CHECK(pane != nullptr);
    if (pane == nullptr)
        return;
    const kit::Point inside = centre_of(*pane);
    OA_CHECK(lib::library_wheel(library, state, list, inside, -1.0F).redraw);
    OA_CHECK(state.list_scroll > 0);
    const int32_t scrolled = state.list_scroll;
    OA_CHECK(!lib::library_wheel(library, state, list, {2, 2}, -1.0F).redraw);
    OA_CHECK(state.list_scroll == scrolled);

    state = {};
    list = laid_out(library, state, compact);
    const std::optional<lib::EntryId> before = library.selected;
    static_cast<void>(
        lib::library_pointer(library, state, list, lib::PointerKind::down, 1, inside, 12)
    );
    static_cast<void>(lib::library_pointer(
        library, state, list, lib::PointerKind::move, 0, {inside.x, inside.y - 40}, 12
    ));
    OA_CHECK(state.list_scroll > 0);
    const lib::ScreenResult result = lib::library_pointer(
        library, state, list, lib::PointerKind::up, 1, {inside.x, inside.y - 40}, 12
    );
    OA_CHECK(!result.action && library.selected == before && !library.details_open);
}

/// Returns the fixture's Library with Ridge selected, with forty tags so
/// that its details scroll at every size class, and a dozen more
/// registries, so that the Registries tab scrolls too.
///
/// @return the Library
lib::Library with_long_details() {
    lib::Inputs inputs = fixture::fixture_inputs();
    for (lib::Listing& listing : inputs.listings) {
        if (listing.key != "ridge")
            continue;
        for (int32_t index = 1; index <= 40; ++index)
            listing.tags.push_back("balance-" + std::to_string(index));
    }
    const lib::Registry example = inputs.registries.back();
    for (int32_t index = 1; index <= 12; ++index) {
        lib::Registry extra = example;
        extra.id = "extra-" + std::to_string(index);
        extra.name = "Extra " + std::to_string(index);
        extra.title = extra.name;
        inputs.registries.push_back(std::move(extra));
    }
    lib::Library library;
    lib::refresh(library, std::move(inputs));
    lib::set_tab(library, lib::Tab::mods);
    lib::select(library, fixture::ridge_id());
    return library;
}

/// Returns the scroll bar of a pane: the list's lies in the list's pane, and
/// the details' is the rightmost bar.
///
/// @param list the display list
/// @param pane the list or the details
/// @return the bar's item; null while that pane shows none
const kit::Item* pane_bar(const kit::DisplayList& list, lib::Pane pane) {
    const kit::Control* rows = fixture::named(list, "library.list");
    const kit::Item* found = nullptr;
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::scroll_bar)
            continue;
        if (pane == lib::Pane::list) {
            if (rows != nullptr && kit::contains(rows->rect, {item.rect.x, item.rect.y}))
                return &item;
        } else if (found == nullptr || item.rect.x > found->rect.x) {
            found = &item;
        }
    }
    return found;
}

/// Returns a scroll bar's look.
///
/// @param bar the bar's item
/// @return the look; a look with no thumb when the item has none
kit::ScrollBarLook bar_look(const kit::Item& bar) {
    const auto* look = std::get_if<kit::ScrollBarLook>(&bar.look);
    return look != nullptr ? *look : kit::ScrollBarLook{};
}

/// Drags a pane's scroll bar the ways a player does, and checks that each
/// scrolls that pane alone, draws the bar held while it is held, and
/// presses nothing: the mouse on the thumb, down and back above the well;
/// the mouse at the well's foot, which takes the thumb to the foot; and a
/// finger on the thumb.
///
/// @param[in,out] library the model
/// @param start the screen's state, scrolled to the top
/// @param frame the frame
/// @param pane the pane whose bar is dragged
/// @param scroll how far that pane is scrolled
/// @param where what a failure names
void drag_pane_bar(
    lib::Library& library,
    const lib::ScreenState& start,
    const kit::Frame& frame,
    lib::Pane pane,
    int32_t lib::ScreenState::* scroll,
    const std::string& where
) {
    lib::ScreenState state = start;
    kit::DisplayList list = laid_out(library, state, frame);
    const kit::Item* bar = pane_bar(list, pane);
    if (bar == nullptr) {
        std::fprintf(stderr, "%s: no scroll bar\n", where.c_str());
        ++oa::test::failed_checks();
        return;
    }
    const kit::Rect well = bar->rect;
    const kit::Rect thumb = bar_look(*bar).thumb;
    OA_CHECK(!bar_look(*bar).hot);
    const std::optional<lib::EntryId> selected = library.selected;
    const bool details_open = library.details_open;
    const bool details_page = state.details_page;
    const int32_t list_scroll = state.list_scroll;
    const int32_t details_scroll = state.details_scroll;

    // The mouse on the thumb holds the bar and scrolls nothing yet.
    const kit::Point grip{thumb.x + thumb.width / 2, thumb.y + thumb.height / 2};
    static_cast<void>(
        lib::library_pointer(library, state, list, lib::PointerKind::down, 1, grip, 0)
    );
    OA_CHECK(state.bar_held == pane);
    OA_CHECK(state.*scroll == start.*scroll);
    // Down the well, the thumb follows the pointer.
    static_cast<void>(lib::library_pointer(
        library, state, list, lib::PointerKind::move, 0, {grip.x, grip.y + 20}, 0
    ));
    OA_CHECK(state.*scroll > start.*scroll);
    if (pane == lib::Pane::list)
        OA_CHECK(state.details_scroll == details_scroll);
    else
        OA_CHECK(state.list_scroll == list_scroll);
    list = laid_out(library, state, frame);
    bar = pane_bar(list, pane);
    OA_CHECK(bar != nullptr);
    if (bar == nullptr)
        return;
    OA_CHECK(bar_look(*bar).hot);
    const int32_t moved = bar_look(*bar).thumb.y - thumb.y;
    if (std::abs(moved - 20) > 1)
        std::fprintf(stderr, "%s: the thumb moved %d rows for 20\n", where.c_str(), moved);
    OA_CHECK(std::abs(moved - 20) <= 1);
    // Above the well, the pane is back at its top.
    static_cast<void>(lib::library_pointer(
        library, state, list, lib::PointerKind::move, 0, {grip.x, well.y - 40}, 0
    ));
    OA_CHECK(state.*scroll == 0);
    lib::ScreenResult result = lib::library_pointer(
        library, state, list, lib::PointerKind::up, 1, {grip.x, well.y - 40}, 0
    );
    OA_CHECK(state.bar_held == lib::Pane::none);
    OA_CHECK(!result.action);
    OA_CHECK(library.selected == selected && library.details_open == details_open);
    OA_CHECK(state.details_page == details_page);

    // The mouse at the well's foot takes the thumb to the foot.
    list = laid_out(library, state, frame);
    const kit::Point foot{grip.x, well.y + well.height - 2};
    static_cast<void>(
        lib::library_pointer(library, state, list, lib::PointerKind::down, 1, foot, 0)
    );
    result = lib::library_pointer(library, state, list, lib::PointerKind::up, 1, foot, 0);
    OA_CHECK(!result.action);
    list = laid_out(library, state, frame);
    bar = pane_bar(list, pane);
    OA_CHECK(bar != nullptr);
    if (bar == nullptr)
        return;
    const kit::Rect at_foot = bar_look(*bar).thumb;
    OA_CHECK(at_foot.y + at_foot.height == well.y + well.height - 1);
    const int32_t deepest = state.*scroll;
    OA_CHECK(deepest > 0);

    // A finger takes hold of the thumb as the mouse does.
    const kit::Point finger{at_foot.x + at_foot.width / 2, at_foot.y + at_foot.height / 2};
    static_cast<void>(
        lib::library_pointer(library, state, list, lib::PointerKind::down, 1, finger, 12)
    );
    OA_CHECK(state.bar_held == pane);
    static_cast<void>(lib::library_pointer(
        library, state, list, lib::PointerKind::move, 0, {finger.x, finger.y - 20}, 12
    ));
    result = lib::library_pointer(
        library, state, list, lib::PointerKind::up, 1, {finger.x, finger.y - 20}, 12
    );
    OA_CHECK(state.*scroll > 0 && state.*scroll < deepest);
    OA_CHECK(!result.action && library.selected == selected);
}

/// Every pane's scroll bar can be dragged, by the mouse and by a finger: the
/// list's at Compact, the details' at Regular, the Compact details page's
/// and the Registries tab's. The Active tab's maps bar is
/// active_maps_fill_scroll_and_filter's.
void pane_scroll_bars_drag() {
    const kit::Frame compact = fixture::frames()[0].frame;
    const kit::Frame regular = fixture::frames()[1].frame;
    lib::Library library = with_long_details();
    drag_pane_bar(
        library, {}, compact, lib::Pane::list, &lib::ScreenState::list_scroll, "Compact list"
    );
    drag_pane_bar(
        library,
        {},
        regular,
        lib::Pane::details,
        &lib::ScreenState::details_scroll,
        "Regular details"
    );
    lib::open_details(library);
    lib::ScreenState page;
    page.details_page = true;
    drag_pane_bar(
        library,
        page,
        compact,
        lib::Pane::details,
        &lib::ScreenState::details_scroll,
        "Compact details page"
    );
    lib::set_tab(library, lib::Tab::registries);
    drag_pane_bar(
        library,
        {},
        compact,
        lib::Pane::details,
        &lib::ScreenState::details_scroll,
        "Compact Registries"
    );
}

/// Typing searches as it is typed, and Backspace takes a character back.
void typing_searches() {
    const kit::Frame regular = fixture::frames()[1].frame;
    lib::Library library = with_ridge();
    kit::DisplayList list = laid_out(library, {}, regular);
    lib::ScreenState state = focused_on(list, "library.search");
    OA_CHECK(lib::library_text(library, state, list, "f").redraw);
    OA_CHECK(library.query == "f");
    OA_CHECK(lib::library_text(library, state, list, "r").redraw);
    OA_CHECK(library.query == "fr");
    OA_CHECK(
        !library.visible.empty() && library.entries[library.visible.front()].id.key == "frontier"
    );
    OA_CHECK(lib::library_key(library, state, list, kit::Key::backspace).redraw);
    OA_CHECK(library.query == "f");
    lib::ScreenState elsewhere;
    OA_CHECK(!lib::library_text(library, elsewhere, list, "x").redraw);
    OA_CHECK(library.query == "f");
}

/// Returns the pill a named control draws, or null.
///
/// @param list the display list
/// @param name the control's name
/// @return the pill; null when that control draws none
const kit::PillLook* pill_named(const kit::DisplayList& list, std::string_view name) {
    const kit::Control* control = fixture::named(list, name);
    if (control == nullptr)
        return nullptr;
    for (const kit::Item& item : list.items) {
        if (item.control != control->id || item.role != kit::Role::pill)
            continue;
        return std::get_if<kit::PillLook>(&item.look);
    }
    return nullptr;
}

/// Compact's filter drop-down opens, lists the filters and the tab's tags,
/// and its items show them. Regular's filter and tag pills keep their
/// counts and turn a tag on and off, and a tag under the description does too.
void filters_and_tags() {
    const std::vector<fixture::NamedFrame> all = fixture::frames();
    lib::Library library = with_ridge();
    lib::ScreenState state;
    kit::DisplayList list = laid_out(library, state, all[0].frame);
    static_cast<void>(click(library, state, list, "library.filter"));
    OA_CHECK(state.menu == lib::MenuKind::filter);
    list = laid_out(library, state, all[0].frame);
    OA_CHECK(fixture::named(list, "library.filter.installed") != nullptr);
    OA_CHECK(fixture::named(list, "library.filter.tag-balance") != nullptr);
    // The drop-down's filters carry their counts, as the chips and the column do.
    const kit::Control* all_item = fixture::named(list, "library.filter.all");
    OA_CHECK(all_item != nullptr && all_item->text == "All (11)");
    const kit::Control* new_item = fixture::named(list, "library.filter.new");
    OA_CHECK(new_item != nullptr && new_item->text == "New (3)");
    OA_EXPECT_NONE(name_problems(list), "filter menu open");
    OA_EXPECT_NONE(fixture::texts_not_fitting(list, fonts()), "filter menu open");
    static_cast<void>(click(library, state, list, "library.filter.installed"));
    OA_CHECK(
        lib::shown_filter(library) == lib::Filter::installed && state.menu == lib::MenuKind::none
    );
    // Each tab keeps its own: Updates still opens on Updates, and Mods keeps Installed.
    lib::set_tab(library, lib::Tab::updates);
    OA_CHECK(lib::shown_filter(library) == lib::Filter::updates);
    lib::set_tab(library, lib::Tab::mods);
    OA_CHECK(lib::shown_filter(library) == lib::Filter::installed);
    list = laid_out(library, state, all[0].frame);
    static_cast<void>(click(library, state, list, "library.filter"));
    list = laid_out(library, state, all[0].frame);
    // A press outside an open drop-down closes it and does nothing else.
    const lib::ScreenResult outside = click(library, state, list, "library.close");
    OA_CHECK(!outside.action && state.menu == lib::MenuKind::none);

    lib::Library wide = with_ridge();
    lib::ScreenState wide_state;
    kit::DisplayList wide_list = laid_out(wide, wide_state, all[1].frame);
    const kit::Control* all_pill = fixture::named(wide_list, "library.filter.all");
    const kit::PillLook* all_look = pill_named(wide_list, "library.filter.all");
    OA_CHECK(all_pill != nullptr && all_pill->checked && all_pill->text == "All (11)");
    OA_CHECK(all_look != nullptr && all_look->state == kit::PillState::active);
    const kit::Control* new_pill = fixture::named(wide_list, "library.filter.new");
    const kit::PillLook* new_look = pill_named(wide_list, "library.filter.new");
    OA_CHECK(new_pill != nullptr && !new_pill->checked && new_pill->text == "New (3)");
    OA_CHECK(new_look != nullptr && new_look->state == kit::PillState::inactive);
    const kit::Control* updates_pill = fixture::named(wide_list, "library.filter.updates");
    OA_CHECK(
        updates_pill != nullptr && !updates_pill->checked &&
        updates_pill->text == lib::filter_caption(wide, lib::Filter::updates)
    );
    const kit::Control* balance = fixture::named(wide_list, "library.tag.balance");
    const kit::PillLook* balance_look = pill_named(wide_list, "library.tag.balance");
    OA_CHECK(balance != nullptr && !balance->checked && balance->text == "balance");
    OA_CHECK(balance_look != nullptr && balance_look->state == kit::PillState::inactive);
    const kit::PillLook* detail_look = pill_named(wide_list, "library.detail.tag.balance");
    OA_CHECK(detail_look != nullptr && detail_look->state == kit::PillState::inactive);
    OA_CHECK(all_pill != nullptr && all_pill->kind == kit::ControlKind::button);
    if (all_pill != nullptr && balance != nullptr) {
        wide_state.interaction.focus_shown = true;
        wide_state.interaction.focused = all_pill->id;
        wide_state.interaction.hovered = balance->id;
        wide_list = laid_out(wide, wide_state, all[1].frame);
        const kit::PillLook* focused_all = pill_named(wide_list, "library.filter.all");
        const kit::PillLook* hovered_tag = pill_named(wide_list, "library.tag.balance");
        OA_CHECK(focused_all != nullptr && focused_all->focused);
        OA_CHECK(hovered_tag != nullptr && hovered_tag->hovered);
        bool ring = false;
        for (const kit::Item& item : wide_list.items)
            ring = ring || item.role == kit::Role::focus_ring;
        OA_CHECK(!ring);
        wide_state.interaction = {};
    }
    wide_list = laid_out(wide, wide_state, all[1].frame);
    static_cast<void>(click(wide, wide_state, wide_list, "library.tag.balance"));
    OA_CHECK(wide.tag == "balance");
    wide_list = laid_out(wide, wide_state, all[1].frame);
    const kit::Control* balance_on = fixture::named(wide_list, "library.tag.balance");
    const kit::PillLook* balance_on_look = pill_named(wide_list, "library.tag.balance");
    OA_CHECK(balance_on != nullptr && balance_on->checked);
    OA_CHECK(balance_on_look != nullptr && balance_on_look->state == kit::PillState::active);
    OA_CHECK(pill_named(wide_list, "library.detail.tag.balance") != nullptr);
    static_cast<void>(click(wide, wide_state, wide_list, "library.detail.tag.balance"));
    OA_CHECK(wide.tag.empty());
    wide_list = laid_out(wide, wide_state, all[1].frame);
    static_cast<void>(click(wide, wide_state, wide_list, "library.detail.tag.balance"));
    OA_CHECK(wide.tag == "balance");
    wide_list = laid_out(wide, wide_state, all[1].frame);
    static_cast<void>(click(wide, wide_state, wide_list, "library.tag.balance"));
    OA_CHECK(wide.tag.empty());
    wide_list = laid_out(wide, wide_state, all[1].frame);
    static_cast<void>(click(wide, wide_state, wide_list, "library.filter.updates"));
    OA_CHECK(lib::shown_filter(wide) == lib::Filter::updates);
    wide_list = laid_out(wide, wide_state, all[1].frame);
    const kit::Control* updates_on = fixture::named(wide_list, "library.filter.updates");
    const kit::PillLook* updates_on_look = pill_named(wide_list, "library.filter.updates");
    OA_CHECK(
        updates_on != nullptr && updates_on->checked &&
        updates_on->text == lib::filter_caption(wide, lib::Filter::updates)
    );
    OA_CHECK(updates_on_look != nullptr && updates_on_look->state == kit::PillState::active);
    static_cast<void>(click(wide, wide_state, wide_list, "library.filter.new"));
    OA_CHECK(lib::shown_filter(wide) == lib::Filter::newest);
    OA_CHECK(
        !wide.visible.empty() && wide.entries[wide.visible.front()].id.key == "dunes" &&
        wide.visible.size() == 3
    );
}

/// Tells whether a drawn text starts with a prefix.
///
/// @param list the display list
/// @param prefix the prefix
/// @return true when one text does
bool text_starts(const kit::DisplayList& list, std::string_view prefix) {
    for (const kit::Item& item : list.items) {
        if (item.role == kit::Role::text && item.text.compare(0, prefix.size(), prefix) == 0)
            return true;
    }
    return false;
}

/// With no mod played the base game and No Mod are tiles, the language says
/// it is built in, and the maps tile says there are none. Compact, Regular
/// and Large keep the same four tiles.
void active_base_game_is_a_tile() {
    const std::vector<fixture::NamedFrame> all = fixture::frames();
    lib::Inputs inputs = fixture::fixture_inputs();
    for (lib::Installed& held : inputs.installed)
        held.playing = false;
    inputs.language = {};
    inputs.map_packs.clear();
    inputs.content.source = "from GOG for macOS";
    inputs.content.files_differ = 0;
    lib::Library library;
    lib::refresh(library, std::move(inputs));
    OA_CHECK(library.tab == lib::Tab::active);
    for (const fixture::NamedFrame& named : {all[0], all[1], all[2]}) {
        lib::ScreenState state;
        const kit::DisplayList list = laid_out(library, state, named.frame);
        const kit::Point window = lib::window_size(named.frame);
        OA_EXPECT_NONE(fixture::outside_window(list, window), named.name + " base game");
        OA_EXPECT_NONE(meeting_controls(list), named.name + " base game");
        OA_EXPECT_NONE(order_problems(list), named.name + " base game");
        OA_EXPECT_NONE(name_problems(list), named.name + " base game");
        OA_EXPECT_NONE(fixture::texts_not_fitting(list, fonts()), named.name + " base game");
        const kit::Control* mod = fixture::named(list, "library.active.mod");
        OA_CHECK(mod != nullptr && mod->text == "No Mod");
        OA_CHECK(mod != nullptr && !mod->enabled && !mod->focusable);
        OA_CHECK(text_item(list, "No Mod") != nullptr);
        OA_CHECK(text_item(list, "Total Annihilation as it was made") != nullptr);
        OA_CHECK(text_item(list, "Total Annihilation") != nullptr);
        OA_CHECK(text_item(list, "TA") != nullptr);
        const kit::Control* languages = fixture::named(list, "library.tab.languages");
        const kit::Control* registries = fixture::named(list, "library.tab.registries");
        OA_CHECK(languages != nullptr && registries != nullptr);
        if (languages != nullptr)
            OA_CHECK(languages->text.starts_with("Languages ("));
        if (registries != nullptr)
            OA_CHECK(registries->text.starts_with("Registries ("));
        OA_CHECK(text_item(list, "All files match") != nullptr);
        OA_CHECK(text_starts(list, "3.1c"));
        const kit::Control* change = fixture::named(list, "library.active.change");
        OA_CHECK(change != nullptr && change->enabled && change->text == "CHANGE…");
        const kit::Control* expansions = fixture::named(list, "library.active.expansions");
        OA_CHECK(expansions != nullptr && expansions->text == "EXPANSIONS · 0 OF 6 ON");
        OA_CHECK(text_starts(list, "Core Contingency needs"));
        const kit::Control* add = fixture::named(list, "library.active.expansion.campaign");
        OA_CHECK(add != nullptr && add->text.find("ADD FROM…") != std::string::npos);
        OA_CHECK(!shows_pill(list, "Playing"));
        OA_CHECK(!shows_chip(list, "Playing"));
        OA_CHECK(!shows_chip(list, "Built in"));
        OA_CHECK(shows_pill(list, "Built in"));
        OA_CHECK(fixture::named(list, "library.active.unload") == nullptr);
        const kit::Item* game_title = text_item(list, "Total Annihilation");
        const kit::Item* language_title = text_item(list, "English");
        const kit::Item* mod_title = text_item(list, "No Mod");
        if (game_title != nullptr && language_title != nullptr)
            OA_CHECK(game_title->rect.y == language_title->rect.y);
        if (mod_title != nullptr && game_title != nullptr)
            OA_CHECK(mod_title->rect.y > game_title->rect.y);
        const kit::Control* base = fixture::named(list, "library.active.base");
        OA_CHECK(base != nullptr && base->text == "Total Annihilation · 3.1c");
        if (base != nullptr && game_title != nullptr)
            OA_CHECK(game_title->rect.y == base->rect.y + kit::compact_metrics.card_inset);
        OA_CHECK(text_item(list, "No map packs are installed.") != nullptr);
        OA_CHECK(fixture::named(list, "library.active.map.1") == nullptr);
    }
}

std::string hover_card_words(const kit::DisplayList& list);

/// CHANGE… opens the base game chooser over the Library.
void the_base_chooser_names_its_rows() {
    const kit::Frame regular = fixture::frames()[1].frame;
    lib::Library library = fixture::fixture_library();
    lib::set_tab(library, lib::Tab::active);
    OA_CHECK(lib::open_base_chooser(library).opened);
    lib::ScreenState state;
    const kit::DisplayList list = laid_out(library, state, regular);
    const kit::Control* demo = fixture::named(list, "library.base.totala-demo");
    const kit::Control* cancel = fixture::named(list, "library.base.cancel");
    const kit::Control* use = fixture::named(list, "library.base.use");
    OA_CHECK(demo != nullptr && demo->enabled);
    OA_CHECK(cancel != nullptr && cancel->text == "CANCEL");
    OA_CHECK(use != nullptr && use->text == "USE THIS GAME");
    OA_CHECK(text_item(list, "Base game") != nullptr);
    OA_CHECK(text_item(list, "Not added") != nullptr || shows_pill(list, "Not added"));
    lib::mark_base_game(library, "totala-demo");
    const kit::DisplayList marked = laid_out(library, state, regular);
    OA_CHECK(text_starts(marked, "Choosing another base game"));
    static_cast<void>(lib::library_key(library, state, marked, kit::Key::escape));
    OA_CHECK(!library.chooser.open);
}

/// UNLOAD MOD asks for SWITCH OFF of the mod being played, which is the
/// question the host asks before it plays no mod, and it is off during a game.
void unload_mod_asks_switch_off() {
    const kit::Frame regular = fixture::frames()[1].frame;
    lib::Library library = with_ridge();
    lib::set_tab(library, lib::Tab::active);
    lib::ScreenState state;
    kit::DisplayList list = laid_out(library, state, regular);
    const kit::Control* unload = fixture::named(list, "library.active.unload");
    OA_CHECK(unload != nullptr && unload->enabled);
    if (unload == nullptr)
        return;
    const lib::EntryId highland{std::string(fixture::core), lib::Kind::mod, "highland"};
    const lib::ScreenResult pressed = click(library, state, list, "library.active.unload");
    OA_CHECK(pressed.action == lib::Action::switch_off && pressed.entry == highland);
    OA_CHECK(pressed.entry != fixture::ridge_id());
    state.interaction.focus_shown = true;
    state.interaction.focused = unload->id;
    const lib::ScreenResult keyed = lib::library_key(library, state, list, kit::Key::enter);
    OA_CHECK(keyed.action == lib::Action::switch_off && keyed.entry == highland);
    const kit::Question asked = lib::switch_off_question();
    OA_CHECK(asked.title == "SWITCH MOD");
    OA_CHECK(asked.buttons.size() == 2 && asked.buttons[1].id == lib::switch_button_id);

    library.inputs.in_match = true;
    list = laid_out(library, state, regular);
    const kit::Control* during = fixture::named(list, "library.active.unload");
    OA_CHECK(during != nullptr && !during->enabled);
    if (during != nullptr) {
        OA_CHECK(
            std::find(list.tab_order.begin(), list.tab_order.end(), during->id) ==
            list.tab_order.end()
        );
    }
    const lib::ScreenResult blocked = click(library, state, list, "library.active.unload");
    OA_CHECK(!blocked.action);

    library.inputs.in_match = false;
    library.inputs.mod_from_command_line = true;
    lib::select(library, highland);
    state = {};
    list = laid_out(library, state, regular);
    const kit::Control* chosen = fixture::named(list, "library.active.unload");
    OA_CHECK(chosen != nullptr && !chosen->enabled);
    if (chosen == nullptr)
        return;
    const lib::ScreenResult refused = click(library, state, list, "library.active.unload");
    OA_CHECK(!refused.action);
    static_cast<void>(
        lib::library_pointer(library, state, list, lib::PointerKind::move, 1, centre_of(*chosen), 0)
    );
    // A disabled button is skipped by hit. Resting on UNLOAD MOD still
    // makes it the pointer's control, which is what shows its card, as a
    // rest on the disabled SWITCH OFF is what its note belongs to.
    OA_CHECK(state.interaction.hovered == chosen->id);
    list = laid_out(library, state, regular);
    OA_CHECK(
        hover_card_words(list).find("The command line chose this run's mod.") != std::string::npos
    );
}

/// The Active tab's mod and language take no press. With more maps than fit
/// under the tiles, Compact scrolls the page as one, by the wheel, a finger,
/// the page keys, the scroll bar and the right stick, until at least four
/// map rows are in view. Focusing a map link scrolls that row into view.
void active_tab_takes_nothing_and_scrolls() {
    const kit::Frame compact = fixture::frames()[0].frame;
    lib::Inputs inputs = fixture::fixture_inputs();
    lib::InstalledMaps many;
    many.name = "Many Maps";
    for (int32_t index = 1; index <= 40; ++index)
        many.maps.push_back({"Map " + std::to_string(index), 2, "16x16"});
    inputs.map_packs.push_back(std::move(many));
    lib::Library library;
    lib::refresh(library, std::move(inputs));
    OA_CHECK(library.tab == lib::Tab::active);
    lib::ScreenState state;
    kit::DisplayList list = laid_out(library, state, compact);
    OA_EXPECT_NONE(fixture::outside_window(list, {480, 324}), "active, many maps");
    OA_EXPECT_NONE(meeting_controls(list), "active, many maps");
    OA_EXPECT_NONE(name_problems(list), "active, many maps");
    OA_EXPECT_NONE(fixture::texts_not_fitting(list, fonts()), "active, many maps");
    OA_CHECK(fixture::named(list, "library.active.map.43") == nullptr);
    const bool has_bar =
        std::any_of(list.items.begin(), list.items.end(), [](const kit::Item& item) {
            return item.role == kit::Role::scroll_bar;
        });
    OA_CHECK(has_bar);

    // A press on a line does nothing.
    const kit::Control* mod_line = fixture::named(list, "library.active.mod");
    OA_CHECK(mod_line != nullptr);
    if (mod_line == nullptr)
        return;
    const kit::Point on_line = centre_of(*mod_line);
    OA_CHECK(kit::hit(list, on_line) != mod_line->id);
    static_cast<void>(
        lib::library_pointer(library, state, list, lib::PointerKind::down, 1, on_line, 0)
    );
    const lib::ScreenResult pressed =
        lib::library_pointer(library, state, list, lib::PointerKind::up, 1, on_line, 0);
    OA_CHECK(!pressed.action && !library.selected && library.tab == lib::Tab::active);

    // The wheel over the mod scrolls the page, and so does a finger, and the
    // tiles move up with it. Page Down, End and Home scroll the same page.
    const int32_t mod_top = mod_line->rect.y;
    OA_CHECK(lib::library_wheel(library, state, list, on_line, -1.0F).redraw);
    OA_CHECK(state.maps_scroll > 0);
    list = laid_out(library, state, compact);
    const kit::Control* mod_after = fixture::named(list, "library.active.mod");
    OA_CHECK(mod_after != nullptr && mod_after->rect.y < mod_top);
    state.maps_scroll = 0;
    list = laid_out(library, state, compact);
    static_cast<void>(
        lib::library_pointer(library, state, list, lib::PointerKind::down, 1, on_line, 8)
    );
    static_cast<void>(lib::library_pointer(
        library, state, list, lib::PointerKind::move, 0, {on_line.x, on_line.y - 24}, 8
    ));
    OA_CHECK(state.maps_scroll > 0);
    static_cast<void>(lib::library_pointer(
        library, state, list, lib::PointerKind::up, 1, {on_line.x, on_line.y - 24}, 8
    ));
    list = laid_out(library, state, compact);
    static_cast<void>(lib::library_key(library, state, list, kit::Key::home));
    OA_CHECK(state.maps_scroll == 0);
    list = laid_out(library, state, compact);
    int32_t shown_rows = 0;
    for (int32_t step = 0; step < 8 && shown_rows < 4; ++step) {
        if (!lib::library_key(library, state, list, kit::Key::page_down).redraw && step > 0)
            break;
        list = laid_out(library, state, compact);
        shown_rows = 0;
        for (const kit::Control& control : list.controls) {
            if (control.name.rfind("library.active.map.", 0) == 0) {
                const kit::Rect shown = kit::intersect(control.rect, control.clip);
                if (shown.height >= kit::compact_metrics.small_line)
                    ++shown_rows;
            }
        }
    }
    OA_CHECK(state.maps_scroll > 0);
    OA_CHECK(shown_rows >= 4);
    static_cast<void>(lib::library_key(library, state, list, kit::Key::end));
    const int32_t bottom = state.maps_scroll;
    list = laid_out(library, state, compact);
    OA_CHECK(fixture::named(list, "library.active.map.43") != nullptr);
    OA_CHECK(fixture::named(list, "library.active.map.1") == nullptr);
    const kit::Control* last_map = fixture::named(list, "library.active.map.43");
    OA_CHECK(last_map != nullptr);
    if (last_map == nullptr)
        return;
    OA_CHECK(!lib::library_wheel(library, state, list, centre_of(*last_map), -1.0F).redraw);
    OA_CHECK(state.maps_scroll == bottom);
    OA_EXPECT_NONE(fixture::outside_window(list, {480, 324}), "active, scrolled");
    // The right stick scrolls them too. Leaving the tab does not clear the
    // maps search, and the details scroll of the next tab starts at its top.
    OA_CHECK(lib::library_stick(library, state, list, 1.0F).redraw);
    OA_CHECK(state.maps_scroll < bottom);
    list = laid_out(library, state, compact);

    // The maps search, while only part of it shows, is scrolled wholly into
    // view when the focus moves onto it. A map link keeps the whole line, so
    // the same scroll brings that line in once the focus reaches it.
    lib::ScreenState partial;
    kit::DisplayList partial_list;
    const kit::Control* partial_search = nullptr;
    for (int32_t step = 0; step < 120 && partial_search == nullptr; ++step) {
        partial.maps_scroll = step * 4;
        partial_list = laid_out(library, partial, compact);
        const kit::Control* search = fixture::named(partial_list, "library.active.search");
        if (search == nullptr || !search->focusable)
            continue;
        const kit::Rect shown = kit::intersect(search->rect, search->clip);
        if (shown.height > 0 && shown.height < search->rect.height)
            partial_search = search;
    }
    OA_CHECK(partial_search != nullptr);
    if (partial_search != nullptr) {
        const kit::ControlId search_id = partial_search->id;
        kit::ControlId before_focus = kit::no_control;
        for (const kit::ControlId id : partial_list.tab_order) {
            if (id == search_id)
                break;
            before_focus = id;
        }
        OA_CHECK(before_focus != kit::no_control);
        partial.interaction.focus_shown = true;
        partial.interaction.focused = before_focus;
        const int32_t before_scroll = partial.maps_scroll;
        OA_CHECK(lib::library_key(library, partial, partial_list, kit::Key::tab).redraw);
        OA_CHECK(partial.interaction.focused == search_id);
        OA_CHECK(partial.maps_scroll != before_scroll);
        partial_list = laid_out(library, partial, compact);
        const kit::Control* shown_search = kit::control_of(partial_list, search_id);
        OA_CHECK(
            shown_search != nullptr && shown_search->clip.height > 0 &&
            kit::wholly_in(shown_search->rect, shown_search->clip)
        );
        kit::ControlId link = kit::no_control;
        bool passed_search = false;
        for (const kit::ControlId id : partial_list.tab_order) {
            if (!passed_search) {
                passed_search = id == search_id;
                continue;
            }
            const kit::Control* control = kit::control_of(partial_list, id);
            if (control != nullptr && control->name.rfind("library.active.from.", 0) == 0) {
                link = id;
                break;
            }
        }
        if (link != kit::no_control && shown_search != nullptr) {
            partial.interaction.focused = search_id;
            OA_CHECK(lib::library_key(library, partial, partial_list, kit::Key::tab).redraw);
            OA_CHECK(partial.interaction.focused == link);
            partial_list = laid_out(library, partial, compact);
            const kit::Control* shown_link = kit::control_of(partial_list, link);
            OA_CHECK(
                shown_link != nullptr && shown_link->clip.height > 0 &&
                kit::wholly_in(shown_link->rect, shown_link->clip)
            );
        }
    }

    static_cast<void>(click(library, state, list, "library.tab.mods"));
    OA_CHECK(library.tab == lib::Tab::mods && state.details_scroll == 0);
}

/// On Compact the expansions hint wraps, and every word of it is drawn.
void compact_expansions_hint_wraps() {
    lib::Inputs inputs;
    inputs.content.base_id = "totala";
    inputs.content.copied = {"campaign"};
    inputs.content.enabled = {"campaign"};
    lib::Library library;
    lib::refresh(library, std::move(inputs));
    lib::set_tab(library, lib::Tab::active);
    OA_CHECK(lib::press_expansion(library, "campaign").kind == lib::ContentPressKind::redraw);
    const std::string hint = lib::active_content(library).hint;
    OA_CHECK(hint == "Campaign turns off when the Library closes.");
    const kit::Frame compact = fixture::frames()[0].frame;
    lib::ScreenState state;
    const kit::DisplayList list = laid_out(library, state, compact);
    OA_EXPECT_NONE(fixture::texts_not_fitting(list, fonts()), "expansions hint");
    std::string joined;
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::text || hint.find(item.text) == std::string::npos)
            continue;
        if (!joined.empty())
            joined.push_back(' ');
        joined += item.text;
    }
    OA_CHECK(joined == hint);
}

/// On Compact's details page the page keys and the arrows never change the
/// entry it shows.
/// Compact's details page has no breadcrumb. BACK returns to the list and
/// keeps its tab, filter, search, scroll and row. The list's button is
/// CLOSE. Escape follows the button. Regular and Large keep CLOSE.
void compact_back_returns_to_the_same_list() {
    const std::vector<fixture::NamedFrame> all = fixture::frames();
    lib::Library library = with_ridge();
    lib::set_filter(library, lib::Filter::installed);
    lib::set_query(library, "ridge");
    lib::select(library, fixture::ridge_id());
    lib::ScreenState state;
    state.search.text = library.query;
    state.list_scroll = 28;
    const lib::EntryId kept = *library.selected;
    lib::open_details(library);
    state.details_page = true;
    const kit::Frame compact = all[0].frame;
    kit::DisplayList page = laid_out(library, state, compact);
    OA_CHECK(fixture::named(page, "library.close") == nullptr);
    const kit::Control* back = fixture::named(page, "library.back");
    OA_CHECK(back != nullptr && back->text == "BACK");
    for (const kit::Item& item : page.items)
        OA_CHECK(item.text.find("‹") == std::string::npos);
    lib::ScreenResult result = click(library, state, page, "library.back");
    OA_CHECK(!result.action && !library.details_open && !state.details_page);
    OA_CHECK(library.tab == lib::Tab::mods);
    OA_CHECK(lib::shown_filter(library) == lib::Filter::installed);
    OA_CHECK(library.query == "ridge" && state.search.text == "ridge");
    OA_CHECK(state.list_scroll == 28 && library.selected == kept);

    kit::DisplayList list = laid_out(library, state, compact);
    const kit::Control* close = fixture::named(list, "library.close");
    OA_CHECK(close != nullptr && close->text == "CLOSE");
    OA_CHECK(fixture::named(list, "library.back") == nullptr);
    result = lib::library_key(library, state, list, kit::Key::escape);
    OA_CHECK(result.action == lib::Action::close);
    OA_CHECK(library.query == "ridge" && library.selected == kept);

    lib::open_details(library);
    state.details_page = true;
    page = laid_out(library, state, compact);
    result = lib::library_key(library, state, page, kit::Key::escape);
    OA_CHECK(!result.action && !library.details_open && !state.details_page);
    OA_CHECK(state.list_scroll == 28 && library.selected == kept && library.query == "ridge");

    for (const fixture::NamedFrame& named : all) {
        if (named.frame.size_class == kit::SizeClass::compact)
            continue;
        lib::Library wide = with_ridge();
        lib::open_details(wide);
        lib::ScreenState wide_state;
        wide_state.details_page = true;
        const kit::DisplayList laid = laid_out(wide, wide_state, named.frame);
        const kit::Control* wide_close = fixture::named(laid, "library.close");
        OA_CHECK(wide_close != nullptr && wide_close->text == "CLOSE");
        OA_CHECK(fixture::named(laid, "library.back") == nullptr);
        result = lib::library_key(wide, wide_state, laid, kit::Key::escape);
        OA_CHECK(result.action == lib::Action::close && wide.details_open);
    }
}

void details_page_keeps_its_entry() {
    const kit::Frame compact = fixture::frames()[0].frame;
    lib::Library library = with_ridge();
    lib::open_details(library);
    lib::ScreenState state;
    state.details_page = true;
    for (const kit::Key pressed :
         {kit::Key::page_down,
          kit::Key::end,
          kit::Key::page_up,
          kit::Key::home,
          kit::Key::down,
          kit::Key::down,
          kit::Key::up}) {
        const kit::DisplayList list = laid_out(library, state, compact);
        static_cast<void>(lib::library_key(library, state, list, pressed));
        OA_CHECK(library.selected == fixture::ridge_id());
        OA_CHECK(library.details_open && state.details_page);
    }
}

/// Returns the words of every hover card, each title, aside and block line
/// joined so a line the card split still reads as one.
///
/// @param list the display list
/// @return the words; empty when no card shows
std::string hover_card_words(const kit::DisplayList& list) {
    std::string card;
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::hover_card)
            continue;
        const auto* look = std::get_if<kit::HoverCardLook>(&item.look);
        if (look == nullptr)
            continue;
        card += look->title;
        card += look->aside;
        for (const std::string& line : look->block)
            card += line;
    }
    return card;
}

/// Tells whether a registry row shows the turning refresh mark beside REFRESH.
///
/// @param list the display list
/// @return true when that mark is turning
bool row_refresh_turning(const kit::DisplayList& list) {
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::button || item.control != kit::no_control)
            continue;
        const auto* look = std::get_if<kit::ButtonLook>(&item.look);
        if (look != nullptr && look->mark == kit::ButtonMark::refresh && look->turning &&
            !look->enabled)
            return true;
    }
    return false;
}

/// The Registries tab lists every registry. ADD REGISTRY… sits at the top
/// right, above the cards. Each card shows its refresh time, a key icon and
/// REFRESH beside the switch; REMOVE and ADD MIRROR… only for one the player
/// added, and a neutral Built in pill on the title's line for one that ships
/// with the game. It has no search. The heading is the title, and the note
/// keeps the short name.
void registries_tab_lists_every_registry() {
    lib::Library library = with_ridge();
    lib::set_tab(library, lib::Tab::registries);
    const std::string core_title = "Fixture Core - Sample packages";
    const std::string core_description = "A sample registry, including the made-up mod Ridge.";
    for (lib::Registry& registry : library.inputs.registries) {
        if (registry.id != fixture::core)
            continue;
        registry.title = core_title;
        registry.description = core_description;
    }
    for (const fixture::NamedFrame& named : fixture::frames()) {
        const kit::DisplayList list = laid_out(library, {}, named.frame);
        const kit::Point window = lib::window_size(named.frame);
        OA_EXPECT_NONE(fixture::outside_window(list, window), named.name + " registries");
        OA_EXPECT_NONE(name_problems(list), named.name + " registries");
        OA_EXPECT_NONE(order_problems(list), named.name + " registries");
        OA_CHECK(fixture::named(list, "library.tab.registries") != nullptr);
        const kit::Control* registries_tab = fixture::named(list, "library.tab.registries");
        if (registries_tab != nullptr)
            OA_CHECK(registries_tab->text.starts_with("Registries ("));
        OA_CHECK(fixture::named(list, "library.search") == nullptr);
        OA_CHECK(fixture::named(list, "library.registry.fixture-core") != nullptr);
        OA_CHECK(fixture::named(list, "library.registry.fixture-core.remove") == nullptr);
        OA_CHECK(fixture::named(list, "library.registry.fixture-core.add-mirror") == nullptr);
        OA_CHECK(fixture::named(list, "library.registry.example-mods") != nullptr);
        OA_CHECK(fixture::named(list, "library.registry.example-mods.remove") != nullptr);
        OA_CHECK(fixture::named(list, "library.registry.example-mods.add-mirror") != nullptr);
        OA_CHECK(fixture::named(list, "library.add-registry") != nullptr);
        OA_CHECK(fixture::named(list, "library.registries.note") != nullptr);
        OA_CHECK(fixture::named(list, "library.details") == nullptr);
        const kit::Metrics& metrics = kit::metrics_of(named.frame.size_class);
        const kit::Control* add = fixture::named(list, "library.add-registry");
        const kit::Control* core_row = fixture::named(list, "library.registry.fixture-core");
        const kit::Control* core_key = fixture::named(list, "library.registry.fixture-core.key");
        const kit::Control* core_refresh =
            fixture::named(list, "library.registry.fixture-core.refresh");
        const kit::Control* core_when =
            fixture::named(list, "library.registry.fixture-core.refreshed");
        OA_CHECK(add != nullptr && core_row != nullptr);
        if (add != nullptr && core_row != nullptr) {
            OA_CHECK(add->rect.x + add->rect.width == window.x - metrics.edge - metrics.padding);
            OA_CHECK(add->rect.y + add->rect.height <= core_row->rect.y);
        }
        OA_CHECK(core_key != nullptr && core_key->text.empty());
        OA_CHECK(core_refresh != nullptr && core_refresh->enabled);
        OA_CHECK(core_when != nullptr && core_when->text == "never refreshed");
        OA_CHECK(fixture::named(list, "library.registry.fixture-core.address") == nullptr);
        OA_CHECK(
            core_key != nullptr && core_refresh != nullptr && core_row != nullptr &&
            !fixture::has_room(kit::intersect(core_key->rect, core_refresh->rect)) &&
            !fixture::has_room(kit::intersect(core_refresh->rect, core_row->rect))
        );
        OA_CHECK(shows_pill(list, "Built in"));
        OA_CHECK(!shows_chip(list, "Built in"));
        OA_CHECK(text_item(list, core_title) != nullptr);
        OA_CHECK(text_item(list, core_description) != nullptr);
        OA_CHECK(
            core_key != nullptr && core_key->rect.width == metrics.state_icon_side &&
            core_key->rect.height == metrics.state_icon_side
        );
        const kit::Item* built_in_pill = nullptr;
        int built_in_pills = 0;
        bool key_icon = false;
        for (const kit::Item& item : list.items) {
            if (item.role == kit::Role::pill && item.text == "Built in") {
                built_in_pill = &item;
                ++built_in_pills;
            }
            if (core_key != nullptr && item.role == kit::Role::chip &&
                item.control == core_key->id) {
                const auto* chip = std::get_if<kit::ChipLook>(&item.look);
                key_icon = chip != nullptr && chip->icon == kit::StateIcon::key;
            }
        }
        OA_CHECK(built_in_pills == 1);
        OA_CHECK(key_icon);
        const kit::Item* core_title_item = text_item(list, core_title);
        OA_CHECK(built_in_pill != nullptr && core_row != nullptr && core_title_item != nullptr);
        if (built_in_pill != nullptr && core_title_item != nullptr && core_key != nullptr) {
            const auto* pill_look = std::get_if<kit::PillLook>(&built_in_pill->look);
            OA_CHECK(pill_look != nullptr && pill_look->state == kit::PillState::neutral);
            OA_CHECK(
                built_in_pill->rect.y < core_title_item->rect.y + core_title_item->rect.height
            );
            OA_CHECK(built_in_pill->rect.y + built_in_pill->rect.height > core_title_item->rect.y);
            OA_CHECK(
                built_in_pill->rect.x >= core_title_item->rect.x + core_title_item->rect.width
            );
            OA_CHECK(built_in_pill->rect.x + built_in_pill->rect.width <= core_key->rect.x);
        }
        std::vector<kit::Rect> cards;
        for (const kit::Item& item : list.items) {
            if (item.role == kit::Role::outline && item.colour == kit::colour::control_border &&
                item.rect.width > metrics.list_row_badge)
                cards.push_back(item.rect);
        }
        std::sort(cards.begin(), cards.end(), [](const kit::Rect& left, const kit::Rect& right) {
            return left.y < right.y;
        });
        OA_CHECK(cards.size() == library.inputs.registries.size());
        for (std::size_t card_index = 1; card_index < cards.size(); ++card_index)
            OA_CHECK(
                cards[card_index].y ==
                cards[card_index - 1].y + cards[card_index - 1].height + metrics.card_gap
            );
        const kit::Control* note = fixture::named(list, "library.registries.note");
        OA_CHECK(note != nullptr && note->text.find("Fixture Core") != std::string::npos);
        OA_CHECK(note != nullptr && note->text.find(core_title) == std::string::npos);
        if (note != nullptr && !cards.empty())
            OA_CHECK(note->rect.y >= cards.back().y + cards.back().height);
        const kit::Control* added_key = fixture::named(list, "library.registry.example-mods.key");
        const kit::Control* added_refresh =
            fixture::named(list, "library.registry.example-mods.refresh");
        const kit::Control* added_when =
            fixture::named(list, "library.registry.example-mods.refreshed");
        OA_CHECK(added_key != nullptr && added_key->text.empty());
        OA_CHECK(added_refresh != nullptr && added_refresh->enabled);
        OA_CHECK(added_when != nullptr && added_when->text == "never refreshed");
    }

    const kit::Frame regular = fixture::frames()[1].frame;
    lib::ScreenState state;
    kit::DisplayList list = laid_out(library, state, regular);
    const lib::ScreenResult removed =
        click(library, state, list, "library.registry.example-mods.remove");
    OA_CHECK(removed.action == lib::Action::remove_registry && removed.registry == fixture::added);
    list = laid_out(library, state, regular);
    const lib::ScreenResult mirror =
        click(library, state, list, "library.registry.example-mods.add-mirror");
    OA_CHECK(mirror.action == lib::Action::add_mirror && mirror.registry == fixture::added);
    list = laid_out(library, state, regular);
    const lib::ScreenResult add = click(library, state, list, "library.add-registry");
    OA_CHECK(add.action == lib::Action::add_registry && add.registry.empty());

    // The switch's right half turns it on, and its left half turns it off.
    // Space toggles. A half that is already chosen asks for nothing.
    const kit::Control* core_switch = fixture::named(list, "library.registry.fixture-core");
    OA_CHECK(core_switch != nullptr);
    if (core_switch != nullptr) {
        const kit::Point on_half{
            core_switch->rect.x + core_switch->rect.width * 3 / 4, core_switch->rect.y + 1
        };
        static_cast<void>(
            lib::library_pointer(library, state, list, lib::PointerKind::down, 1, on_half, 0)
        );
        const lib::ScreenResult already =
            lib::library_pointer(library, state, list, lib::PointerKind::up, 1, on_half, 0);
        OA_CHECK(!already.action);
        const kit::Point off_half{
            core_switch->rect.x + core_switch->rect.width / 4, core_switch->rect.y + 1
        };
        static_cast<void>(
            lib::library_pointer(library, state, list, lib::PointerKind::down, 1, off_half, 0)
        );
        const lib::ScreenResult turned =
            lib::library_pointer(library, state, list, lib::PointerKind::up, 1, off_half, 0);
        OA_CHECK(
            turned.action == lib::Action::set_registry && turned.registry == fixture::core &&
            !turned.registry_on
        );
    }
    state = focused_on(list, "library.registry.fixture-core");
    const lib::ScreenResult keyed = lib::library_key(library, state, list, kit::Key::space);
    OA_CHECK(
        keyed.action == lib::Action::set_registry && keyed.registry == fixture::core &&
        !keyed.registry_on
    );

    // A registry with no key still has the icon. Resting on it shows that.
    lib::ScreenState bare_state;
    kit::DisplayList bare_list = laid_out(library, bare_state, regular);
    const kit::Control* bare_key = fixture::named(bare_list, "library.registry.fixture-core.key");
    OA_CHECK(bare_key != nullptr);
    if (bare_key != nullptr) {
        static_cast<void>(lib::library_pointer(
            library, bare_state, bare_list, lib::PointerKind::move, 0, centre_of(*bare_key), 0
        ));
        bare_list = laid_out(library, bare_state, regular);
        OA_CHECK(
            hover_card_words(bare_list).find("Signing keybuilt inNo signing key") !=
            std::string::npos
        );
        OA_EXPECT_NONE(
            fixture::outside_window(bare_list, lib::window_size(regular)), "empty key tooltip"
        );
    }

    // The address, the refresh time and the signing keys, and REFRESH.
    lib::Library with_keys = library;
    const std::string first_fingerprint = "7001 · af50 · b971 · 5bb4";
    const std::string second_fingerprint = "1111 · 2222 · 3333 · 4444";
    const std::string first_public = std::string("ed25519:") + std::string(200, 'A');
    const std::string second_public = "ed25519:BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
    for (lib::Registry& registry : with_keys.inputs.registries) {
        if (registry.id != fixture::added)
            continue;
        registry.address = "http://maps.example.org/oa/registry.yaml";
        registry.checked_ago = 5 * 60;
        lib::SigningKey first_key;
        first_key.fingerprint = first_fingerprint;
        first_key.public_key = first_public;
        lib::SigningKey second_key;
        second_key.fingerprint = second_fingerprint;
        second_key.public_key = second_public;
        registry.keys = {first_key, second_key};
    }
    lib::ScreenState key_state;
    kit::DisplayList key_list = laid_out(with_keys, key_state, regular);
    const kit::Control* address = fixture::named(key_list, "library.registry.example-mods.address");
    const kit::Control* when = fixture::named(key_list, "library.registry.example-mods.refreshed");
    const kit::Control* icon = fixture::named(key_list, "library.registry.example-mods.key");
    OA_CHECK(address != nullptr && address->text == "http://maps.example.org/oa/registry.yaml");
    OA_CHECK(when != nullptr && when->text == "refreshed 5 min ago");
    OA_CHECK(
        icon != nullptr && icon->text.find(first_fingerprint) != std::string::npos &&
        icon->text.find(second_fingerprint) != std::string::npos
    );
    if (icon != nullptr) {
        static_cast<void>(lib::library_pointer(
            with_keys, key_state, key_list, lib::PointerKind::move, 0, centre_of(*icon), 0
        ));
        key_list = laid_out(with_keys, key_state, regular);
        const std::string card = hover_card_words(key_list);
        OA_CHECK(card.find("Signing keys") != std::string::npos);
        OA_CHECK(card.find("added by you") != std::string::npos);
        OA_CHECK(card.find(first_fingerprint) != std::string::npos);
        OA_CHECK(card.find(first_public) != std::string::npos);
        OA_CHECK(card.find(second_fingerprint) != std::string::npos);
        OA_CHECK(card.find(second_public) != std::string::npos);
        OA_EXPECT_NONE(fixture::outside_window(key_list, lib::window_size(regular)), "key tooltip");
    }
    lib::ScreenState focused = focused_on(key_list, "library.registry.example-mods.key");
    focused.interaction.hovered = id_of(key_list, "library.registry.fixture-core.key");
    const std::string focused_card = hover_card_words(laid_out(with_keys, focused, regular));
    OA_CHECK(focused_card.find(first_public) != std::string::npos);
    OA_CHECK(focused_card.find("added by you") != std::string::npos);
    OA_CHECK(focused_card.find("built in") == std::string::npos);
    const lib::ScreenResult key_press =
        click(with_keys, key_state, key_list, "library.registry.example-mods.key");
    OA_CHECK(!key_press.action);
    const lib::ScreenResult asked =
        click(with_keys, key_state, key_list, "library.registry.example-mods.refresh");
    OA_CHECK(asked.action == lib::Action::refresh_registry && asked.registry == fixture::added);

    for (lib::Registry& registry : with_keys.inputs.registries) {
        if (registry.id != fixture::added)
            continue;
        registry.on = false;
    }
    kit::DisplayList off_list = laid_out(with_keys, key_state, regular);
    const kit::Control* off_refresh =
        fixture::named(off_list, "library.registry.example-mods.refresh");
    OA_CHECK(off_refresh != nullptr && !off_refresh->enabled);
    OA_EXPECT_NONE(order_problems(off_list), "registry off");
    const lib::ScreenResult off_press =
        click(with_keys, key_state, off_list, "library.registry.example-mods.refresh");
    OA_CHECK(!off_press.action);

    for (lib::Registry& registry : with_keys.inputs.registries) {
        if (registry.id != fixture::added)
            continue;
        registry.on = true;
        registry.refreshing = true;
    }
    const kit::DisplayList spinning = laid_out(with_keys, {}, regular);
    const kit::Control* busy = fixture::named(spinning, "library.registry.example-mods.refresh");
    OA_CHECK(busy != nullptr && !busy->enabled);
    OA_CHECK(row_refresh_turning(spinning));
    OA_CHECK(!row_refresh_turning(laid_out(library, {}, regular)));
    OA_EXPECT_NONE(order_problems(spinning), "registry refreshing");
    const lib::ScreenResult busy_press =
        click(with_keys, key_state, spinning, "library.registry.example-mods.refresh");
    OA_CHECK(!busy_press.action);

    library.inputs.in_match = true;
    list = laid_out(library, state, regular);
    OA_CHECK(fixture::named(list, "library.add-registry") == nullptr);
    OA_CHECK(fixture::named(list, "library.registry.example-mods.add-mirror") == nullptr);
    OA_CHECK(fixture::named(list, "library.registry.example-mods.remove") != nullptr);
}

/// Returns the header refresh icon's look.
///
/// @param list the display list
/// @return the look; none when the icon is absent
const kit::ButtonLook* header_refresh_look(const kit::DisplayList& list) {
    const kit::Control* control = fixture::named(list, "library.refresh");
    if (control == nullptr)
        return nullptr;
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::button || item.control != control->id)
            continue;
        return std::get_if<kit::ButtonLook>(&item.look);
    }
    return nullptr;
}

/// Returns the refresh mark drawn beside a registry's REFRESH, which is not
/// itself a control.
///
/// @param list the display list
/// @return the mark; none when that registry shows the arrow with no mark
const kit::ButtonLook* row_refresh_look(const kit::DisplayList& list) {
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::button || item.control != kit::no_control)
            continue;
        const auto* look = std::get_if<kit::ButtonLook>(&item.look);
        if (look != nullptr && look->mark == kit::ButtonMark::refresh)
            return look;
    }
    return nullptr;
}

/// A refresh that reads a list shows the check, on the header and beside
/// REFRESH, then the arrow. One that fails shows the arrow at once. Another
/// press while the check shows starts a refresh again. The eighth after
/// 0.8 seconds of turning is the same at 30, 60 and 120 frames a second,
/// and a tick during the held check does not ask for another frame.
void a_finished_refresh_shows_the_check_then_the_arrow() {
    const kit::Frame regular = fixture::frames()[1].frame;
    lib::Library library = with_ridge();
    lib::set_tab(library, lib::Tab::registries);
    library.inputs.can_check = true;
    const lib::RegistryPulse busy{fixture::core, true, false};
    const lib::RegistryPulse read{fixture::core, false, false};
    const lib::RegistryPulse missed{fixture::core, false, true};
    const auto apply = [](lib::Library& model, bool checking, bool refreshing) {
        model.inputs.checking = checking;
        for (lib::Registry& registry : model.inputs.registries)
            if (registry.id == fixture::core)
                registry.refreshing = refreshing;
    };
    const auto eighth_after = [](uint32_t frame_ms) {
        lib::ScreenState spun;
        const lib::RegistryPulse turning{fixture::core, true, false};
        const std::span<const lib::RegistryPulse> one{&turning, 1};
        uint32_t elapsed = 0;
        while (elapsed < 800u) {
            const uint32_t step_ms = frame_ms < 800u - elapsed ? frame_ms : 800u - elapsed;
            static_cast<void>(lib::step_refresh_marks(spun, true, one, step_ms));
            elapsed += step_ms;
        }
        return spun.refresh_turn;
    };
    OA_CHECK(eighth_after(1000u / 30u) == eighth_after(1000u / 60u));
    OA_CHECK(eighth_after(1000u / 60u) == eighth_after(1000u / 120u));
    OA_CHECK(eighth_after(1000u / 120u) == kit::refresh_eighth(800));

    lib::ScreenState state;
    const std::span<const lib::RegistryPulse> spinning{&busy, 1};
    const std::span<const lib::RegistryPulse> finished{&read, 1};
    OA_CHECK(lib::step_refresh_marks(state, true, spinning, 0));
    apply(library, true, true);
    const kit::DisplayList turning_list = laid_out(library, state, regular);
    const kit::ButtonLook* turning_header = header_refresh_look(turning_list);
    const kit::ButtonLook* turning_row = row_refresh_look(turning_list);
    OA_CHECK(turning_header != nullptr && turning_header->turning && turning_header->check_ms == 0);
    OA_CHECK(turning_row != nullptr && turning_row->turning && turning_row->check_ms == 0);

    OA_CHECK(lib::step_refresh_marks(state, false, finished, 0));
    OA_CHECK(state.refresh_check_ms > 0);
    apply(library, false, false);
    const kit::DisplayList checked = laid_out(library, state, regular);
    const kit::ButtonLook* checked_header = header_refresh_look(checked);
    const kit::ButtonLook* checked_row = row_refresh_look(checked);
    OA_CHECK(
        checked_header != nullptr && !checked_header->turning &&
        checked_header->check_ms == state.refresh_check_ms
    );
    OA_CHECK(checked_row != nullptr && !checked_row->turning && checked_row->check_ms > 0);
    const kit::Control* refresh = fixture::named(checked, "library.refresh");
    const kit::Control* row_button =
        fixture::named(checked, "library.registry.fixture-core.refresh");
    OA_CHECK(refresh != nullptr && refresh->enabled);
    OA_CHECK(row_button != nullptr && row_button->enabled);
    lib::ScreenState header_press = state;
    const lib::ScreenResult again = click(library, header_press, checked, "library.refresh");
    OA_CHECK(again.action == lib::Action::check_now);
    lib::ScreenState row_press = state;
    const lib::ScreenResult row_again =
        click(library, row_press, checked, "library.registry.fixture-core.refresh");
    OA_CHECK(
        row_again.action == lib::Action::refresh_registry && row_again.registry == fixture::core
    );

    // Into the held check the picture stays, so the step asks for no frame.
    OA_CHECK(lib::step_refresh_marks(state, false, finished, 200));
    OA_CHECK(state.refresh_check_ms > 0);
    OA_CHECK(!lib::step_refresh_marks(state, false, finished, 200));
    OA_CHECK(lib::step_refresh_marks(state, false, finished, 800));
    OA_CHECK(state.refresh_check_ms == 0);
    const kit::DisplayList rested = laid_out(library, state, regular);
    const kit::ButtonLook* rested_header = header_refresh_look(rested);
    OA_CHECK(rested_header != nullptr && !rested_header->turning && rested_header->check_ms == 0);
    OA_CHECK(row_refresh_look(rested) == nullptr);

    // A press during the check starts the refresh again and the check goes.
    lib::ScreenState during;
    static_cast<void>(lib::step_refresh_marks(during, true, spinning, 0));
    static_cast<void>(lib::step_refresh_marks(during, false, finished, 200));
    OA_CHECK(during.refresh_check_ms > 0);
    OA_CHECK(lib::step_refresh_marks(during, true, spinning, 0));
    OA_CHECK(during.refresh_check_ms == 0);
    bool row_cleared = true;
    for (const lib::ScreenState::RegistryMark& mark : during.registry_marks)
        if (mark.check_ms != 0)
            row_cleared = false;
    OA_CHECK(row_cleared);

    lib::ScreenState failed;
    const std::span<const lib::RegistryPulse> broken{&missed, 1};
    static_cast<void>(lib::step_refresh_marks(failed, true, spinning, 0));
    static_cast<void>(lib::step_refresh_marks(failed, false, broken, 16));
    OA_CHECK(failed.refresh_check_ms == 0);
    const kit::DisplayList failed_list = laid_out(library, failed, regular);
    const kit::ButtonLook* failed_header = header_refresh_look(failed_list);
    OA_CHECK(failed_header != nullptr && !failed_header->turning && failed_header->check_ms == 0);
    OA_CHECK(row_refresh_look(failed_list) == nullptr);
}

/// MORE ▾ on Compact's details page holds the actions after the first
/// three, and choosing one asks for it.
void more_holds_the_rest() {
    const kit::Frame compact = fixture::frames()[0].frame;
    lib::Library library = with_ridge();
    lib::open_details(library);
    lib::ScreenState state;
    state.details_page = true;
    kit::DisplayList list = laid_out(library, state, compact);
    static_cast<void>(click(library, state, list, "library.action"));
    OA_CHECK(state.menu == lib::MenuKind::more);
    list = laid_out(library, state, compact);
    const lib::ScreenResult result = click(library, state, list, "library.action.homepage");
    OA_CHECK(result.action == lib::Action::homepage && result.entry == fixture::ridge_id());
    OA_CHECK(state.menu == lib::MenuKind::none);
}

/// Returns one pixel of a picture.
///
/// @param picture the picture
/// @param x the column
/// @param y the row
/// @return the pixel's red, green, blue and alpha
const uint8_t* pixel_at(const renderer::RgbaPicture& picture, uint32_t x, uint32_t y) {
    return picture.pixels.data() +
           (static_cast<std::size_t>(y) * picture.width + x) * renderer::picture_pixel_bytes;
}

/// Tells whether a pixel is one colour.
///
/// @param pixel the pixel
/// @param red red
/// @param green green
/// @param blue blue
/// @param alpha alpha
/// @return true when it matches
bool pixel_is(const uint8_t* pixel, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    return pixel[0] == red && pixel[1] == green && pixel[2] == blue && pixel[3] == alpha;
}

/// Returns the look of a named list row.
///
/// @param list the display list
/// @param name the row's name
/// @return the look; null when the row is not drawn
const kit::ListRowLook* row_look_named(const kit::DisplayList& list, std::string_view name) {
    const kit::Control* control = fixture::named(list, std::string(name));
    if (control == nullptr)
        return nullptr;
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::list_row || item.control != control->id)
            continue;
        return std::get_if<kit::ListRowLook>(&item.look);
    }
    return nullptr;
}

/// Returns the base game's badge square in a row, as the kit draws it.
///
/// @param row the row
/// @return the square
kit::Rect test_row_badge(const kit::Rect& row) {
    const int32_t inset = kit::compact_metrics.list_row_inset;
    const int32_t side =
        std::max(std::min(row.height - 2 * inset, kit::compact_metrics.list_row_badge), int32_t{0});
    return {row.x + inset, row.y + (row.height - side) / 2, side, side};
}

/// The base game's badge is the icon, nearest-neighbour and centred, and the
/// letters when there is no icon. Tabs after Active read their counts in brackets.
void base_badge_is_nearest_and_tabs_are_bracketed() {
    const kit::Frame compact = fixture::frames()[0].frame;
    lib::Inputs inputs = fixture::fixture_inputs();
    for (lib::Installed& held : inputs.installed)
        held.playing = false;
    inputs.playing_mod.reset();
    inputs.language = {};
    inputs.map_packs.clear();

    lib::Library letters;
    lib::refresh(letters, inputs);
    lib::set_tab(letters, lib::Tab::active);
    const kit::DisplayList letter_list = laid_out(letters, {}, compact);
    OA_CHECK(text_item(letter_list, "TA") != nullptr);
    OA_CHECK(text_item(letter_list, "—") != nullptr);
    const kit::Control* mods = fixture::named(letter_list, "library.tab.mods");
    OA_CHECK(
        mods != nullptr &&
        mods->text == "Mods (" + std::to_string(lib::tab_count(letters, lib::Tab::mods)) + ")" &&
        mods->name == "library.tab.mods"
    );

    std::vector<uint8_t> quarters{
        255,
        0,
        0,
        255,
        0,
        255,
        0,
        255,
        0,
        0,
        255,
        255,
        255,
        255,
        255,
        255,
    };
    inputs.base_game_badge = lib::Badge{quarters, 2, 2};
    lib::Library library;
    lib::refresh(library, inputs);
    lib::set_tab(library, lib::Tab::mods);
    const kit::DisplayList rows = laid_out(library, {}, compact);
    OA_CHECK(text_item(rows, "3.1") == nullptr);
    const kit::Control* base = fixture::named(rows, "library.row.base-game");
    const kit::ListRowLook* look = row_look_named(rows, "library.row.base-game");
    OA_CHECK(base != nullptr && look != nullptr);
    if (base != nullptr && look != nullptr) {
        const kit::Rect badge = test_row_badge(base->rect);
        OA_CHECK(badge.width == 20 && badge.height == 20);
        OA_CHECK(look->badge.width == 20 && look->badge.height == 20);
        OA_CHECK(pixel_is(pixel_at(look->badge, 0, 0), 255, 0, 0, 255));
        OA_CHECK(pixel_is(pixel_at(look->badge, 9, 0), 255, 0, 0, 255));
        OA_CHECK(pixel_is(pixel_at(look->badge, 10, 0), 0, 255, 0, 255));
        OA_CHECK(pixel_is(pixel_at(look->badge, 0, 10), 0, 0, 255, 255));
        OA_CHECK(pixel_is(pixel_at(look->badge, 10, 10), 255, 255, 255, 255));
    }

    kit::Frame doubled = compact;
    doubled.scale_percent = 200;
    const kit::DisplayList large_pixels = laid_out(library, {}, doubled);
    const kit::ListRowLook* doubled_look = row_look_named(large_pixels, "library.row.base-game");
    OA_CHECK(doubled_look != nullptr && doubled_look->badge.width == 40);
    if (doubled_look != nullptr && doubled_look->badge.width == 40) {
        OA_CHECK(pixel_is(pixel_at(doubled_look->badge, 19, 0), 255, 0, 0, 255));
        OA_CHECK(pixel_is(pixel_at(doubled_look->badge, 20, 0), 0, 255, 0, 255));
    }

    kit::Frame fractional = compact;
    fractional.scale_percent = 150;
    const kit::DisplayList smooth = laid_out(library, {}, fractional);
    const kit::Control* smooth_row = fixture::named(smooth, "library.row.base-game");
    const kit::ListRowLook* smooth_look = row_look_named(smooth, "library.row.base-game");
    OA_CHECK(smooth_row != nullptr && smooth_look != nullptr);
    if (smooth_row != nullptr && smooth_look != nullptr) {
        const kit::Rect pixels = kit::to_canvas(test_row_badge(smooth_row->rect), 150);
        OA_CHECK(smooth_look->badge.width == static_cast<uint32_t>(pixels.width));
        OA_CHECK(smooth_look->badge.height == static_cast<uint32_t>(pixels.height));
    }

    std::vector<uint8_t> icon(32U * 32U * 4U, 0);
    icon[3] = 255;
    icon[0] = 255;
    const std::size_t corner = (30U * 32U + 30U) * 4U;
    icon[corner + 1] = 255;
    icon[corner + 3] = 255;
    inputs.base_game_badge = lib::Badge{std::move(icon), 32, 32};
    lib::Library shrunk;
    lib::refresh(shrunk, inputs);
    lib::set_tab(shrunk, lib::Tab::mods);
    const kit::DisplayList down = laid_out(shrunk, {}, compact);
    const kit::ListRowLook* down_look = row_look_named(down, "library.row.base-game");
    OA_CHECK(down_look != nullptr && down_look->badge.width == 20 && down_look->badge.height == 20);
    if (down_look != nullptr && down_look->badge.width == 20 && down_look->badge.height == 20) {
        OA_CHECK(pixel_is(pixel_at(down_look->badge, 0, 0), 255, 0, 0, 255));
        OA_CHECK(pixel_is(pixel_at(down_look->badge, 19, 19), 0, 255, 0, 255));
    }

    lib::set_tab(library, lib::Tab::active);
    const kit::DisplayList active = laid_out(library, {}, compact);
    OA_CHECK(text_item(active, "3.1") == nullptr);
    const renderer::RgbaPicture* tile = nullptr;
    for (const kit::Item& item : active.items) {
        if (item.role != kit::Role::picture)
            continue;
        const auto* drawn = std::get_if<kit::PictureLook>(&item.look);
        if (drawn == nullptr || drawn->picture.pixels.data() == quarters.data())
            continue;
        tile = &drawn->picture;
        OA_CHECK(item.rect.width == 36 && item.rect.height == 36);
        break;
    }
    OA_CHECK(tile != nullptr && tile->width == 36 && tile->height == 36);
    if (tile != nullptr && tile->width == 36) {
        OA_CHECK(pixel_is(pixel_at(*tile, 0, 0), 255, 0, 0, 255));
        OA_CHECK(pixel_is(pixel_at(*tile, 17, 0), 255, 0, 0, 255));
        OA_CHECK(pixel_is(pixel_at(*tile, 18, 0), 0, 255, 0, 255));
    }
}

/// Returns the height a package row has in a size class. The mark does not add to it.
///
/// @param size_class the class
/// @return the height, in points
int32_t package_row_height(kit::SizeClass size_class) {
    const kit::Metrics& metrics = kit::metrics_of(size_class);
    switch (size_class) {
    case kit::SizeClass::compact:
        return metrics.short_list_row_height;
    case kit::SizeClass::regular:
        return metrics.list_row_height;
    case kit::SizeClass::large:
        return metrics.list_row_line_top + 2 * metrics.small_line + metrics.list_row_title_top;
    }
    return metrics.list_row_height;
}

/// A built-in Open Annihilation package shows the OA mark, and a Cavedog one
/// the bone. The same authors from an added registry, or sideloaded, show
/// none. A named author that is not a maker does not fall through. A listing
/// that names no author shows no mark, whatever its publisher says. The
/// fixture's language pack names its author. A built-in registry's card
/// shows its mark under the key, and an added registry's card does not. Row
/// heights stay the size class's.
void maker_marks_follow_the_registry() {
    OA_CHECK(lib::package_maker(" OpenAnnihilation ") == lib::PackageMaker::open_annihilation);
    OA_CHECK(lib::package_maker("oPeN aNnIhIlAtIoN") == lib::PackageMaker::open_annihilation);
    OA_CHECK(lib::package_maker("\tOA team") == lib::PackageMaker::open_annihilation);
    OA_CHECK(lib::package_maker("Cavedog Entertainment") == lib::PackageMaker::cavedog);
    OA_CHECK(lib::package_maker("  cavedog entertainment") == lib::PackageMaker::cavedog);
    OA_CHECK(lib::package_maker("Open  Annihilation") == lib::PackageMaker::none);
    OA_CHECK(lib::package_maker("the Ridge team") == lib::PackageMaker::none);

    lib::Inputs inputs = fixture::fixture_inputs();
    for (lib::Listing& listing : inputs.listings) {
        if (listing.registry == fixture::core && listing.key == "ridge")
            listing.author = " openannihilation ";
        else if (listing.registry == fixture::core && listing.key == "bastion")
            listing.author = "Cavedog Entertainment";
        else if (listing.registry == fixture::core && listing.key == "ironclad") {
            listing.publisher = "Open Annihilation";
            listing.author = "Another studio";
        } else if (listing.registry == fixture::core && listing.key == "pt-BR")
            listing.author = "OA team";
        else if (listing.registry == fixture::added && listing.key == "frontier")
            listing.author = "OpenAnnihilation";
        else if (listing.registry == fixture::core && listing.key == "quarry")
            listing.author.clear();
        else if (listing.registry == fixture::core && listing.key == "highland") {
            listing.author.clear();
            listing.publisher = "OA team";
        } else if (listing.registry == fixture::added && listing.key == "dunes") {
            listing.author.clear();
            listing.publisher = "OA team";
        }
    }
    lib::Installed kept;
    kept.kind = lib::Kind::mod;
    kept.key = "quarry";
    kept.name = "Quarry";
    kept.author = "OA team";
    kept.registry = std::string(fixture::core);
    kept.folder = "quarry";
    kept.version = "1.0";
    inputs.installed.push_back(std::move(kept));
    lib::Installed loose;
    loose.kind = lib::Kind::mod;
    loose.key = "loose-bone";
    loose.name = "Loose Bone";
    loose.author = "Cavedog Entertainment";
    loose.folder = "loose-bone";
    loose.version = "1.0";
    inputs.installed.push_back(std::move(loose));

    lib::Library library;
    lib::refresh(library, std::move(inputs));
    const kit::Frame regular = fixture::frames()[1].frame;
    const auto shown = [&](lib::Tab tab, std::string_view query) {
        lib::set_tab(library, tab);
        lib::set_query(library, query);
        return laid_out(library, {}, regular);
    };
    const auto expect_mark = [&](const kit::DisplayList& list,
                                 std::string_view name,
                                 std::optional<kit::MakerMark> mark,
                                 std::string_view text) {
        const kit::Control* row = fixture::named(list, std::string(name));
        const kit::ListRowLook* look = row_look_named(list, name);
        OA_CHECK(row != nullptr && look != nullptr);
        if (row == nullptr || look == nullptr)
            return;
        OA_CHECK(row->name == name);
        OA_CHECK(row->text == text);
        OA_CHECK(look->maker == mark);
        OA_CHECK(row->rect.height == package_row_height(kit::SizeClass::regular));
    };

    const kit::DisplayList ridge = shown(lib::Tab::mods, "ridge");
    expect_mark(
        ridge,
        "library.row.fixture-core.ridge",
        kit::MakerMark::open_annihilation,
        "Ridge · Open Annihilation"
    );
    const kit::Control* ridge_row = fixture::named(ridge, "library.row.fixture-core.ridge");
    const kit::Control* ridge_update =
        fixture::named(ridge, "library.row.fixture-core.ridge.update");
    OA_CHECK(ridge_row != nullptr && ridge_update != nullptr);
    if (ridge_row != nullptr && ridge_update != nullptr) {
        const int32_t side = kit::compact_metrics.state_icon_side;
        const int32_t offset = (ridge_row->rect.height - 2 * side) / 2;
        OA_CHECK(ridge_update->rect.y == ridge_row->rect.y + offset);
        OA_CHECK(ridge_update->rect.height == side && ridge_update->rect.width == side);
    }
    expect_mark(
        shown(lib::Tab::mods, "bastion"),
        "library.row.fixture-core.bastion",
        kit::MakerMark::bone,
        "Bastion · Cavedog Entertainment"
    );
    expect_mark(
        shown(lib::Tab::mods, "frontier"),
        "library.row.example-mods.frontier",
        std::nullopt,
        "Frontier"
    );
    expect_mark(
        shown(lib::Tab::mods, "ironclad"),
        "library.row.fixture-core.ironclad",
        std::nullopt,
        "Ironclad"
    );
    expect_mark(
        shown(lib::Tab::mods, "quarry"),
        "library.row.fixture-core.quarry",
        kit::MakerMark::open_annihilation,
        "Quarry · Open Annihilation"
    );
    expect_mark(
        shown(lib::Tab::mods, "highland"),
        "library.row.fixture-core.highland",
        std::nullopt,
        "Highland"
    );
    expect_mark(
        shown(lib::Tab::mods, "dunes"), "library.row.example-mods.dunes", std::nullopt, "Dunes"
    );
    expect_mark(
        shown(lib::Tab::mods, "loose"), "library.row.loose-bone", std::nullopt, "Loose Bone"
    );
    expect_mark(
        shown(lib::Tab::languages, "portugues"),
        "library.row.fixture-core.pt-br",
        kit::MakerMark::open_annihilation,
        "Portugues (Brasil) · Open Annihilation"
    );

    lib::set_query(library, "ridge");
    lib::set_tab(library, lib::Tab::mods);
    for (const fixture::NamedFrame& named : fixture::frames()) {
        const kit::DisplayList list = laid_out(library, {}, named.frame);
        const kit::Control* row = fixture::named(list, "library.row.fixture-core.ridge");
        OA_CHECK(row != nullptr && row->rect.height == package_row_height(named.frame.size_class));
    }

    for (lib::Registry& registry : library.inputs.registries) {
        if (registry.id == fixture::core)
            registry.author = "Open Annihilation";
        else if (registry.id == fixture::added)
            registry.author = "Cavedog Entertainment";
    }
    lib::set_tab(library, lib::Tab::registries);
    lib::set_query(library, "");
    const kit::DisplayList cards = laid_out(library, {}, regular);
    const kit::Control* core_key = fixture::named(cards, "library.registry.fixture-core.key");
    OA_CHECK(core_key != nullptr && core_key->name == "library.registry.fixture-core.key");
    int oa_marks = 0;
    int bone_marks = 0;
    if (core_key != nullptr) {
        for (const kit::Item& item : cards.items) {
            if (item.role != kit::Role::chip || item.control != kit::no_control)
                continue;
            const auto* chip = std::get_if<kit::ChipLook>(&item.look);
            if (chip == nullptr || !chip->icon)
                continue;
            if (*chip->icon == kit::StateIcon::oa) {
                ++oa_marks;
                OA_CHECK(item.rect.x == core_key->rect.x);
                OA_CHECK(item.rect.y == core_key->rect.y + core_key->rect.height);
                OA_CHECK(item.rect.width == core_key->rect.width);
            } else if (*chip->icon == kit::StateIcon::bone) {
                ++bone_marks;
            }
        }
    }
    OA_CHECK(oa_marks == 1);
    OA_CHECK(bone_marks == 0);
    OA_CHECK(fixture::named(cards, "library.registry.fixture-core.mark") == nullptr);
}

/// Returns a solid minimap. It is wider than it is tall, so a fitted badge
/// letterboxes it instead of stretching it.
///
/// @param red red
/// @param green green
/// @param blue blue
/// @return an 8 by 2 picture
lib::MapPicture solid_minimap(uint8_t red, uint8_t green, uint8_t blue) {
    lib::MapPicture picture;
    picture.width = 8;
    picture.height = 2;
    picture.rgba.assign(8U * 2U * 4U, 0);
    for (std::size_t index = 0; index + 3 < picture.rgba.size(); index += 4) {
        picture.rgba[index] = red;
        picture.rgba[index + 1] = green;
        picture.rgba[index + 2] = blue;
        picture.rgba[index + 3] = 255;
    }
    return picture;
}

/// Checks that a fitted minimap is the box's pixel size, keeps the source
/// colour in the middle, and fills the letterbox with the list's well colour.
///
/// @param picture the fitted picture
/// @param red the source's red
/// @param green the source's green
/// @param blue the source's blue
void expect_letterboxed(
    const renderer::RgbaPicture& picture, uint8_t red, uint8_t green, uint8_t blue
) {
    OA_CHECK(picture.width >= 4 && picture.height >= 4);
    OA_CHECK(picture.width == picture.height);
    OA_CHECK(pixel_is(
        pixel_at(picture, 0, 0),
        kit::colour::well.r,
        kit::colour::well.g,
        kit::colour::well.b,
        kit::colour::well.a
    ));
    OA_CHECK(
        pixel_is(pixel_at(picture, picture.width / 2, picture.height / 2), red, green, blue, 255)
    );
}

/// Checks that a row's minimap was drawn at the badge square's pixel size.
///
/// @param look the row
/// @param row the row's control
/// @param scale the frame's scale, in percent
void expect_badge_pixels(const kit::ListRowLook& look, const kit::Rect& row, int32_t scale) {
    const kit::Rect pixels = kit::to_canvas(test_row_badge(row), scale);
    OA_CHECK(look.badge.width == static_cast<uint32_t>(pixels.width));
    OA_CHECK(look.badge.height == static_cast<uint32_t>(pixels.height));
}

/// Returns the widest picture whose centre is one colour.
///
/// @param list the display list
/// @param red red
/// @param green green
/// @param blue blue
/// @return the picture's item; null when none is that colour
const kit::Item*
widest_picture(const kit::DisplayList& list, uint8_t red, uint8_t green, uint8_t blue) {
    const kit::Item* found = nullptr;
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::picture)
            continue;
        const auto* look = std::get_if<kit::PictureLook>(&item.look);
        if (look == nullptr || look->picture.width < 4 || look->picture.height < 4)
            continue;
        if (!pixel_is(
                pixel_at(look->picture, look->picture.width / 2, look->picture.height / 2),
                red,
                green,
                blue,
                255
            ))
            continue;
        if (found == nullptr || item.rect.width > found->rect.width)
            found = &item;
    }
    return found;
}

/// A one-map pack wears its minimap, fitted, on Maps and on Updates. A pack
/// of several keeps its badge, or the first map's minimap on the Maps tab
/// when it has none. The details show one large picture or a grid: under one
/// map's picture its players and size come first, with no name, and under
/// each map of several its name and then those lines. A minimap that has not
/// arrived yet keeps the row's letters, then the picture.
void map_previews_fit_their_boxes() {
    const lib::MapPicture red = solid_minimap(255, 0, 0);
    const lib::MapPicture blue = solid_minimap(0, 0, 255);
    const lib::MapPicture green = solid_minimap(0, 255, 0);
    const std::string shore_note(800, 'x');
    lib::Inputs inputs = fixture::fixture_inputs();
    for (lib::Listing& listing : inputs.listings) {
        if (listing.key == "core-maps" && !listing.maps.empty())
            listing.maps[0].preview = red;
        else if (listing.key == "desert-pack" && listing.maps.size() == 2) {
            listing.maps[0].preview = blue;
            listing.maps[0].description = "Dry and flat.";
            listing.maps[1].preview = green;
            listing.maps[1].description = "A table of stone.";
        }
    }
    lib::Listing shore = fixture::listing(
        fixture::core,
        lib::Kind::map_pack,
        "shore",
        "Shore Pack",
        "1.0",
        fixture::mib,
        "One cove",
        {}
    );
    lib::MapLine cove;
    cove.name = "Cove";
    cove.players = 3;
    cove.size = "10x10";
    cove.description = "A long cove.";
    cove.stem = "cove";
    cove.preview_url = "https://example.test/cove.png";
    shore.maps.push_back(std::move(cove));
    inputs.listings.push_back(std::move(shore));
    inputs.map_packs[1].maps[0].preview = red;
    inputs.map_packs[1].maps[0].description = shore_note;

    lib::Library library;
    lib::refresh(library, std::move(inputs));
    const kit::Frame compact = fixture::frames()[0].frame;
    lib::set_tab(library, lib::Tab::maps);
    lib::set_query(library, "shore");
    const kit::DisplayList waiting = laid_out(library, {}, compact);
    const kit::ListRowLook* placeholder = row_look_named(waiting, "library.row.fixture-core.shore");
    OA_CHECK(placeholder != nullptr);
    if (placeholder != nullptr) {
        OA_CHECK(!renderer::picture_drawable(placeholder->badge));
        OA_CHECK(placeholder->badge_text == "SP");
    }

    for (lib::Listing& listing : library.inputs.listings) {
        if (listing.key == "shore" && !listing.maps.empty())
            listing.maps[0].preview = red;
    }
    lib::refresh(library, std::move(library.inputs));
    lib::set_tab(library, lib::Tab::maps);
    lib::set_query(library, "shore");
    const kit::DisplayList shore_rows = laid_out(library, {}, compact);
    const kit::Control* shore_row = fixture::named(shore_rows, "library.row.fixture-core.shore");
    const kit::ListRowLook* shore_look =
        row_look_named(shore_rows, "library.row.fixture-core.shore");
    OA_CHECK(shore_row != nullptr && shore_look != nullptr);
    if (shore_row != nullptr && shore_look != nullptr) {
        expect_letterboxed(shore_look->badge, 255, 0, 0);
        expect_badge_pixels(*shore_look, shore_row->rect, compact.scale_percent);
    }

    lib::set_tab(library, lib::Tab::updates);
    lib::set_filter(library, lib::Filter::all);
    lib::set_query(library, "shore");
    const kit::DisplayList shore_updates = laid_out(library, {}, compact);
    const kit::Control* shore_update =
        fixture::named(shore_updates, "library.row.fixture-core.shore");
    const kit::ListRowLook* shore_update_look =
        row_look_named(shore_updates, "library.row.fixture-core.shore");
    OA_CHECK(shore_update != nullptr && shore_update_look != nullptr);
    if (shore_update != nullptr && shore_update_look != nullptr) {
        expect_letterboxed(shore_update_look->badge, 255, 0, 0);
        expect_badge_pixels(*shore_update_look, shore_update->rect, compact.scale_percent);
    }

    lib::set_tab(library, lib::Tab::maps);
    lib::set_query(library, "core map");
    const kit::DisplayList core_rows = laid_out(library, {}, compact);
    const kit::ListRowLook* core_look =
        row_look_named(core_rows, "library.row.fixture-core.core-maps");
    OA_CHECK(core_look != nullptr && core_look->badge.width == 16 && core_look->badge.height == 16);
    if (core_look != nullptr && core_look->badge.width == 16)
        OA_CHECK(pixel_is(pixel_at(core_look->badge, 0, 0), 0x40, 0x80, 0xc0, 255));

    lib::set_query(library, "desert pack");
    const kit::DisplayList desert_rows = laid_out(library, {}, compact);
    const kit::Control* desert_row =
        fixture::named(desert_rows, "library.row.example-mods.desert-pack");
    const kit::ListRowLook* desert_look =
        row_look_named(desert_rows, "library.row.example-mods.desert-pack");
    OA_CHECK(desert_row != nullptr && desert_look != nullptr);
    if (desert_row != nullptr && desert_look != nullptr) {
        expect_letterboxed(desert_look->badge, 0, 0, 255);
        expect_badge_pixels(*desert_look, desert_row->rect, compact.scale_percent);
    }

    lib::set_tab(library, lib::Tab::updates);
    lib::set_filter(library, lib::Filter::all);
    lib::set_query(library, "desert pack");
    const kit::DisplayList desert_update_rows = laid_out(library, {}, compact);
    const kit::ListRowLook* desert_updates =
        row_look_named(desert_update_rows, "library.row.example-mods.desert-pack");
    OA_CHECK(desert_updates != nullptr);
    if (desert_updates != nullptr) {
        OA_CHECK(!renderer::picture_drawable(desert_updates->badge));
        OA_CHECK(desert_updates->badge_text == "DP");
    }
    lib::set_query(library, "core map");
    const kit::DisplayList core_update_rows = laid_out(library, {}, compact);
    const kit::ListRowLook* core_updates =
        row_look_named(core_update_rows, "library.row.fixture-core.core-maps");
    OA_CHECK(core_updates != nullptr && core_updates->badge.width == 16);

    const kit::Frame large = fixture::frames()[2].frame;
    lib::set_tab(library, lib::Tab::maps);
    lib::set_filter(library, lib::Filter::all);
    lib::set_query(library, "");
    lib::select(library, {std::string(fixture::core), lib::Kind::map_pack, "shore"});
    const kit::DisplayList one = laid_out(library, {}, large);
    const kit::Item* one_large = widest_picture(one, 255, 0, 0);
    OA_CHECK(one_large != nullptr && one_large->rect.width == 160 && one_large->rect.height == 160);
    if (one_large != nullptr) {
        const auto* drawn = std::get_if<kit::PictureLook>(&one_large->look);
        OA_CHECK(drawn != nullptr);
        if (drawn != nullptr)
            expect_letterboxed(drawn->picture, 255, 0, 0);
    }
    int header_squares = 0;
    for (const kit::Item& item : one.items) {
        if (item.role == kit::Role::picture && item.rect.width == 40 && item.rect.height == 40)
            ++header_squares;
    }
    OA_CHECK(header_squares == 1);
    OA_CHECK(text_item(one, "Shore Pack") != nullptr);
    OA_CHECK(text_item(one, "Cove") == nullptr);
    const kit::Item* one_meta = text_item(one, "3 players · 10x10");
    OA_CHECK(one_meta != nullptr);
    const kit::Item* one_description = text_item(one, "A long cove.");
    OA_CHECK(one_description != nullptr);
    if (one_meta != nullptr && one_description != nullptr)
        OA_CHECK(one_meta->rect.y < one_description->rect.y);

    lib::select(library, {std::string(fixture::added), lib::Kind::map_pack, "desert-pack"});
    const kit::DisplayList grid = laid_out(library, {}, large);
    int cells = 0;
    bool saw_blue = false;
    bool saw_green = false;
    for (const kit::Item& item : grid.items) {
        if (item.role != kit::Role::picture || item.rect.width != 72 || item.rect.height != 72)
            continue;
        ++cells;
        const auto* drawn = std::get_if<kit::PictureLook>(&item.look);
        if (drawn == nullptr || drawn->picture.width < 4)
            continue;
        OA_CHECK(pixel_is(
            pixel_at(drawn->picture, 0, 0),
            kit::colour::well.r,
            kit::colour::well.g,
            kit::colour::well.b,
            kit::colour::well.a
        ));
        const uint8_t* mid =
            pixel_at(drawn->picture, drawn->picture.width / 2, drawn->picture.height / 2);
        if (pixel_is(mid, 0, 0, 255, 255))
            saw_blue = true;
        if (pixel_is(mid, 0, 255, 0, 255))
            saw_green = true;
    }
    OA_CHECK(cells == 2);
    OA_CHECK(saw_blue && saw_green);
    const kit::Item* dry_lake = text_item(grid, "Dry Lake");
    const kit::Item* dry_lake_meta = text_item(grid, "2 players · 8x8");
    OA_CHECK(dry_lake != nullptr);
    OA_CHECK(text_item(grid, "Mesa") != nullptr);
    OA_CHECK(dry_lake_meta != nullptr);
    OA_CHECK(text_item(grid, "4 players · 16x16") != nullptr);
    if (dry_lake != nullptr && dry_lake_meta != nullptr)
        OA_CHECK(dry_lake->rect.y < dry_lake_meta->rect.y);
    const kit::Item* dry_lake_cell = nullptr;
    if (dry_lake != nullptr) {
        for (const kit::Item& item : grid.items) {
            if (item.role == kit::Role::picture && item.rect.width == 72 &&
                item.rect.height == 72 && item.rect.x == dry_lake->rect.x) {
                dry_lake_cell = &item;
                break;
            }
        }
    }
    OA_CHECK(dry_lake_cell != nullptr);
    // One map's players and size take the place a map's name has in a grid.
    if (dry_lake != nullptr && dry_lake_cell != nullptr && one_meta != nullptr &&
        one_large != nullptr)
        OA_CHECK(
            one_meta->rect.y - (one_large->rect.y + one_large->rect.height) ==
            dry_lake->rect.y - (dry_lake_cell->rect.y + dry_lake_cell->rect.height)
        );
    OA_CHECK(text_item(grid, "Dry and flat.") != nullptr);
    OA_CHECK(text_item(grid, "A table of stone.") != nullptr);
    OA_CHECK(text_item(grid, "DP") != nullptr);

    lib::set_tab(library, lib::Tab::active);
    const kit::DisplayList active = laid_out(library, {}, large);
    const kit::Control* atoll = fixture::named(active, "library.active.map.1");
    const kit::Control* from = fixture::named(active, "library.active.from.1");
    const kit::Item* atoll_title = text_item(active, "Atoll");
    OA_CHECK(atoll != nullptr && atoll->text == "Atoll · 6 players · 20x20");
    OA_CHECK(from != nullptr && from->kind == kit::ControlKind::link && from->text == "Atoll");
    if (atoll != nullptr && from != nullptr && atoll_title != nullptr) {
        OA_CHECK(from->rect.y == atoll_title->rect.y);
        OA_CHECK(from->rect.y < atoll->rect.y);
    }
    OA_CHECK(text_item(active, "From Island Hopping") == nullptr);
    const kit::Control* from_ridge = fixture::named(active, "library.active.from.2");
    OA_CHECK(
        from_ridge != nullptr && from_ridge->kind == kit::ControlKind::link &&
        from_ridge->text == "From Core Map Pack"
    );
    const kit::Item* tile_picture = nullptr;
    if (atoll_title != nullptr) {
        for (const kit::Item& item : active.items) {
            if (item.role != kit::Role::picture || item.rect.y != atoll_title->rect.y ||
                item.rect.x >= atoll_title->rect.x)
                continue;
            tile_picture = &item;
            break;
        }
    }
    OA_CHECK(tile_picture != nullptr);
    if (tile_picture != nullptr) {
        const auto* drawn = std::get_if<kit::PictureLook>(&tile_picture->look);
        OA_CHECK(drawn != nullptr);
        if (drawn != nullptr)
            expect_letterboxed(drawn->picture, 255, 0, 0);
    }
    OA_CHECK(text_item(active, shore_note) == nullptr);
    bool cut_note = false;
    for (const kit::Item& item : active.items) {
        if (item.role != kit::Role::text || item.text.size() >= shore_note.size())
            continue;
        if (item.text.size() < 3 || item.text.front() != 'x')
            continue;
        if (item.text.compare(item.text.size() - 3, 3, "...") != 0)
            continue;
        cut_note = item.rect.height == kit::compact_metrics.small_line;
    }
    OA_CHECK(cut_note);
    OA_CHECK(text_item(active, "6 players · 20x20") != nullptr);
}

/// Returns the fixture plus Desert Pack installed as a map, and enough other
/// map packs that Desert Pack's row starts below the list.
///
/// @return the inputs
lib::Inputs packs_past_the_fold() {
    lib::Inputs inputs = fixture::fixture_inputs();
    inputs.map_packs.push_back({"Desert Pack", {{"Dry Lake", 2, "8x8"}}});
    for (int32_t index = 1; index <= 30; ++index) {
        const std::string number = (index < 10 ? "0" : "") + std::to_string(index);
        inputs.listings.push_back(
            fixture::listing(
                fixture::added,
                lib::Kind::map_pack,
                "aaa-" + number,
                "Aaa " + number,
                "1.0",
                fixture::mib,
                "A filler map pack",
                {}
            )
        );
    }
    return inputs;
}

/// Checks that following Desert Pack's From link opens Maps with that pack
/// selected and its row in view.
///
/// @param library the model, just switched
/// @param state the screen's state
/// @param frame the frame
void expect_desert_in_view(
    lib::Library& library, lib::ScreenState& state, const kit::Frame& frame
) {
    OA_CHECK(library.tab == lib::Tab::maps);
    OA_CHECK(
        library.selected && library.selected->key == "desert-pack" &&
        library.selected->registry == fixture::added
    );
    OA_CHECK(state.reveal_row >= 0);
    const kit::DisplayList list = laid_out(library, state, frame);
    OA_CHECK(state.reveal_row == -1);
    const kit::Control* row = fixture::named(list, "library.row.example-mods.desert-pack");
    const kit::Control* pane = fixture::named(list, "library.list");
    OA_CHECK(row != nullptr && pane != nullptr);
    if (row != nullptr && pane != nullptr) {
        OA_CHECK(row->rect.y >= pane->rect.y);
        OA_CHECK(row->rect.y + row->rect.height <= pane->rect.y + pane->rect.height);
    }
}

/// A click, Enter or Space on a map's From link opens the Maps tab on that pack.
void from_link_opens_the_pack() {
    const kit::Frame large = fixture::frames()[2].frame;
    lib::Library clicked;
    lib::refresh(clicked, packs_past_the_fold());
    lib::set_tab(clicked, lib::Tab::active);
    lib::ScreenState state;
    kit::DisplayList list = laid_out(clicked, state, large);
    const kit::Control* from = fixture::named(list, "library.active.from.2");
    OA_CHECK(from != nullptr && from->text == "Dry Lake" && from->enabled);
    OA_CHECK(text_item(list, "From Desert Pack") == nullptr);
    if (from == nullptr)
        return;
    const lib::ScreenResult pressed = click(clicked, state, list, "library.active.from.2");
    OA_CHECK(!pressed.action);
    expect_desert_in_view(clicked, state, large);

    for (const kit::Key key : {kit::Key::enter, kit::Key::space}) {
        lib::Library library;
        lib::refresh(library, packs_past_the_fold());
        lib::set_tab(library, lib::Tab::active);
        lib::ScreenState keyed_state;
        const kit::DisplayList active = laid_out(library, keyed_state, large);
        const kit::Control* link = fixture::named(active, "library.active.from.2");
        OA_CHECK(link != nullptr);
        if (link == nullptr)
            continue;
        keyed_state.interaction.focus_shown = true;
        keyed_state.interaction.focused = link->id;
        const lib::ScreenResult keyed = lib::library_key(library, keyed_state, active, key);
        OA_CHECK(!keyed.action);
        expect_desert_in_view(library, keyed_state, large);
        const kit::DisplayList maps = laid_out(library, keyed_state, large);
        const kit::Control* pane = fixture::named(maps, "library.list");
        OA_CHECK(pane != nullptr && keyed_state.interaction.focused == pane->id);
    }
}

/// The title bar's close mark sits to the right of the refresh icon at
/// every size class. The title and the version stop before it, a press
/// asks for the same close as CLOSE, and the footer CLOSE stays.
void the_title_bar_holds_a_close_mark() {
    const kit::Metrics& compact = kit::compact_metrics;
    const std::vector<fixture::NamedFrame> frames = fixture::frames();
    for (std::size_t index = 0; index < 3; ++index) {
        const fixture::NamedFrame& named = frames[index];
        lib::Library library = with_ridge();
        library.inputs.version_text = "v0.8.0-a-very-long-version";
        lib::ScreenState state;
        const kit::DisplayList list = laid_out(library, state, named.frame);
        const kit::Control* close = fixture::named(list, "library.close-x");
        const kit::Control* refresh = fixture::named(list, "library.refresh");
        const kit::Control* footer = fixture::named(list, "library.close");
        OA_CHECK(close != nullptr && refresh != nullptr && footer != nullptr);
        if (close == nullptr || refresh == nullptr || footer == nullptr)
            continue;
        OA_CHECK(close->text == "Close");
        OA_CHECK(close->rect.width == refresh->rect.width);
        OA_CHECK(close->rect.height == refresh->rect.height);
        OA_CHECK(close->rect.x == refresh->rect.x + refresh->rect.width + compact.header_gap);
        OA_CHECK(close->rect.y == refresh->rect.y);
        OA_CHECK(
            close->rect.x + close->rect.width ==
            named.frame.area.width - compact.edge - compact.padding
        );
        bool mark = false;
        const kit::HeaderLook* header = nullptr;
        for (const kit::Item& item : list.items) {
            if (item.role == kit::Role::button && item.control == close->id) {
                const auto* look = std::get_if<kit::ButtonLook>(&item.look);
                mark = look != nullptr && look->mark == kit::ButtonMark::close;
            }
            if (item.role == kit::Role::header)
                header = std::get_if<kit::HeaderLook>(&item.look);
        }
        OA_CHECK(mark);
        OA_CHECK(header != nullptr);
        if (header != nullptr) {
            const int32_t title_left = compact.padding + compact.mark_side + compact.header_gap;
            const int32_t title_right = title_left + header->title_width;
            const int32_t version_right =
                named.frame.area.width - compact.edge - compact.padding - header->right_inset;
            OA_CHECK(title_right <= close->rect.x);
            OA_CHECK(version_right <= close->rect.x);
        }
        lib::Library pressed = library;
        const lib::ScreenResult result =
            click(pressed, state, list, "library.close-x", index == 0 ? 12 : 0);
        OA_CHECK(result.action == lib::Action::close);
        const lib::ScreenResult footer_close = click(pressed, state, list, "library.close");
        OA_CHECK(footer_close.action == lib::Action::close);
    }
    lib::Library checking = with_ridge();
    checking.inputs.can_check = true;
    lib::ScreenState state;
    const kit::DisplayList list = laid_out(checking, state, frames[1].frame);
    const auto place = [&](std::string_view name) {
        std::size_t at = 0;
        for (const kit::ControlId id : list.tab_order) {
            const kit::Control* control = kit::control_of(list, id);
            if (control != nullptr && control->name == name)
                return at;
            ++at;
        }
        return list.tab_order.size();
    };
    const std::size_t refresh_at = place("library.refresh");
    const std::size_t close_at = place("library.close-x");
    OA_CHECK(refresh_at < close_at);
    OA_CHECK(close_at + 1 == list.tab_order.size());
}

/// Returns the MAPS heading.
///
/// @param list the display list
/// @return the heading; null when the tab does not show it
const kit::Item* maps_heading(const kit::DisplayList& list) {
    for (const kit::Item& item : list.items)
        if (item.role == kit::Role::heading && item.text == "MAPS")
            return &item;
    return nullptr;
}

/// The maps pane fills the room under the MAPS heading at every size class,
/// five hundred maps scroll, and the search filters by map and by pack.
void active_maps_fill_scroll_and_filter() {
    lib::Library library = with_ridge();
    lib::set_tab(library, lib::Tab::active);
    for (const fixture::NamedFrame& named : fixture::frames()) {
        lib::ScreenState state;
        const kit::DisplayList list = laid_out(library, state, named.frame);
        const kit::Control* search = fixture::named(list, "library.active.search");
        const kit::Control* mod = fixture::named(list, "library.active.mod");
        const kit::Control* language = fixture::named(list, "library.active.language");
        const kit::Control* map = fixture::named(list, "library.active.map.1");
        const kit::Control* close = fixture::named(list, "library.close");
        const kit::Item* heading = maps_heading(list);
        OA_CHECK(search != nullptr && heading != nullptr && close != nullptr);
        OA_CHECK(mod != nullptr && language != nullptr);
        if (search == nullptr || heading == nullptr || close == nullptr || mod == nullptr ||
            language == nullptr)
            continue;
        OA_CHECK(search->rect.x > heading->rect.x);
        OA_CHECK(heading->rect.y >= search->rect.y);
        OA_CHECK(heading->rect.y < search->rect.y + search->rect.height);
        // Compact can leave the maps below the page until it scrolls. The
        // other frames keep a maps pane under the tiles.
        if (map == nullptr) {
            OA_CHECK(named.frame.size_class == kit::SizeClass::compact);
            continue;
        }
        const int32_t pane_bottom = map->clip.y + map->clip.height;
        OA_CHECK(pane_bottom <= close->rect.y);
        OA_CHECK(close->rect.y - pane_bottom <= 40);
        const bool page = map->clip.y <= mod->rect.y;
        if (page) {
            OA_CHECK(named.frame.size_class == kit::SizeClass::compact);
            OA_CHECK(
                map->clip.height >= kit::compact_metrics.field_height +
                                        kit::compact_metrics.hint_gap +
                                        4 * kit::compact_metrics.small_line
            );
        } else {
            OA_CHECK(map->clip.y >= search->rect.y + search->rect.height);
            OA_CHECK(mod->rect.y + mod->rect.height <= map->clip.y);
            OA_CHECK(language->rect.y + language->rect.height <= map->clip.y);
        }
        OA_CHECK(text_item(list, "1 of 3") == nullptr);
        OA_CHECK(text_item(list, "No installed map matches.") == nullptr);
    }

    const kit::Frame regular = fixture::frames()[1].frame;
    lib::ScreenState state;
    kit::DisplayList list = laid_out(library, state, regular);
    state = focused_on(list, "library.active.search");
    OA_CHECK(lib::library_command(library, state, list, lib::ScreenCommand::find).redraw);
    OA_CHECK(state.interaction.focused == id_of(list, "library.active.search"));
    OA_CHECK(lib::focused_field(state, list).has_value());
    OA_CHECK(lib::library_text(library, state, list, "atoll").redraw);
    list = laid_out(library, state, regular);
    OA_CHECK(text_item(list, "1 of 3") != nullptr);
    OA_CHECK(fixture::named(list, "library.active.map.1") != nullptr);
    OA_CHECK(fixture::named(list, "library.active.map.2") == nullptr);
    OA_CHECK(fixture::named(list, "library.active.mod") != nullptr);
    OA_CHECK(fixture::named(list, "library.active.language") != nullptr);

    state.maps_search = {};
    state.maps_scroll = 0;
    list = laid_out(library, state, regular);
    state = focused_on(list, "library.active.search");
    OA_CHECK(lib::library_text(library, state, list, "CORE").redraw);
    list = laid_out(library, state, regular);
    OA_CHECK(text_item(list, "2 of 3") != nullptr);
    OA_CHECK(fixture::named(list, "library.active.map.1") == nullptr);
    OA_CHECK(fixture::named(list, "library.active.map.2") != nullptr);
    OA_CHECK(fixture::named(list, "library.active.map.3") != nullptr);
    OA_CHECK(fixture::named(list, "library.active.mod") != nullptr);

    state.maps_search = {};
    list = laid_out(library, state, regular);
    state = focused_on(list, "library.active.search");
    OA_CHECK(lib::library_text(library, state, list, "river").redraw);
    list = laid_out(library, state, regular);
    OA_CHECK(text_item(list, "1 of 3") != nullptr);
    OA_CHECK(
        fixture::named(list, "library.active.map.3") != nullptr &&
        fixture::named(list, "library.active.map.3")->text == "Twin Rivers · 2 players · 12x12"
    );
    OA_CHECK(fixture::named(list, "library.active.map.1") == nullptr);

    state.maps_search = {};
    list = laid_out(library, state, regular);
    state = focused_on(list, "library.active.search");
    OA_CHECK(lib::library_text(library, state, list, "zzz").redraw);
    list = laid_out(library, state, regular);
    OA_CHECK(text_item(list, "0 of 3") != nullptr);
    OA_CHECK(text_item(list, "No installed map matches.") != nullptr);
    OA_CHECK(fixture::named(list, "library.active.map.1") == nullptr);
    OA_CHECK(fixture::named(list, "library.active.mod") != nullptr);
    OA_CHECK(fixture::named(list, "library.active.language") != nullptr);
    OA_CHECK(lib::library_key(library, state, list, kit::Key::escape).redraw);
    OA_CHECK(state.maps_search.text.empty());
    list = laid_out(library, state, regular);
    OA_CHECK(text_item(list, "0 of 3") == nullptr);
    OA_CHECK(fixture::named(list, "library.active.map.1") != nullptr);
    OA_CHECK(fixture::named(list, "library.active.map.3") != nullptr);

    state.maps_search.text = "river";
    state.maps_search.caret = state.maps_search.text.size();
    list = laid_out(library, state, regular);
    static_cast<void>(click(library, state, list, "library.tab.mods"));
    list = laid_out(library, state, regular);
    static_cast<void>(click(library, state, list, "library.tab.active"));
    OA_CHECK(state.maps_search.text == "river");
    list = laid_out(library, state, regular);
    OA_CHECK(fixture::named(list, "library.active.map.3") != nullptr);
    OA_CHECK(fixture::named(list, "library.active.map.1") == nullptr);
    OA_CHECK(text_item(list, "1 of 3") != nullptr);

    lib::Inputs inputs = fixture::fixture_inputs();
    lib::InstalledMaps bulk;
    bulk.name = "Bulk Sheets";
    for (int32_t index = 1; index <= 500; ++index)
        bulk.maps.push_back({"Sheet " + std::to_string(index), 2, "8x8"});
    inputs.map_packs.push_back(std::move(bulk));
    lib::Library many;
    lib::refresh(many, std::move(inputs));
    lib::set_tab(many, lib::Tab::active);
    const std::size_t count = lib::active_setup(many).maps.size();
    OA_CHECK(count >= 500);
    lib::ScreenState scrolled;
    kit::DisplayList packed = laid_out(many, scrolled, regular);
    const std::string last_name = "library.active.map." + std::to_string(count);
    const kit::Control* mod_before = fixture::named(packed, "library.active.mod");
    OA_CHECK(mod_before != nullptr);
    OA_CHECK(fixture::named(packed, "library.active.map.1") != nullptr);
    OA_CHECK(fixture::named(packed, last_name) == nullptr);
    if (mod_before == nullptr)
        return;
    const int32_t mod_top = mod_before->rect.y;
    const kit::Control* top_map = fixture::named(packed, "library.active.map.1");
    OA_CHECK(top_map != nullptr);
    if (top_map != nullptr) {
        const kit::Point on_map = centre_of(*top_map);
        static_cast<void>(
            lib::library_pointer(many, scrolled, packed, lib::PointerKind::down, 1, on_map, 12)
        );
        static_cast<void>(lib::library_pointer(
            many, scrolled, packed, lib::PointerKind::move, 0, {on_map.x, on_map.y - 40}, 12
        ));
        OA_CHECK(scrolled.maps_scroll > 0);
        static_cast<void>(lib::library_pointer(
            many, scrolled, packed, lib::PointerKind::up, 1, {on_map.x, on_map.y - 40}, 12
        ));
    }
    scrolled.maps_scroll = 0;
    packed = laid_out(many, scrolled, regular);
    static_cast<void>(lib::library_key(many, scrolled, packed, kit::Key::end));
    packed = laid_out(many, scrolled, regular);
    OA_CHECK(fixture::named(packed, last_name) != nullptr);
    OA_CHECK(fixture::named(packed, "library.active.map.1") == nullptr);
    const kit::Control* mod_after = fixture::named(packed, "library.active.mod");
    OA_CHECK(mod_after != nullptr && mod_after->rect.y == mod_top);
    const kit::Item* bar = nullptr;
    for (const kit::Item& item : packed.items)
        if (item.role == kit::Role::scroll_bar)
            bar = &item;
    OA_CHECK(bar != nullptr);
    if (bar != nullptr) {
        scrolled.maps_scroll = 0;
        packed = laid_out(many, scrolled, regular);
        const kit::Item* rested = nullptr;
        for (const kit::Item& item : packed.items)
            if (item.role == kit::Role::scroll_bar)
                rested = &item;
        OA_CHECK(rested != nullptr);
        if (rested != nullptr) {
            const kit::Point on_bar{
                rested->rect.x + rested->rect.width / 2,
                rested->rect.y + rested->rect.height - 2,
            };
            static_cast<void>(
                lib::library_pointer(many, scrolled, packed, lib::PointerKind::down, 1, on_bar, 0)
            );
            OA_CHECK(scrolled.maps_scroll > 0);
            static_cast<void>(
                lib::library_pointer(many, scrolled, packed, lib::PointerKind::up, 1, on_bar, 0)
            );
            OA_CHECK(scrolled.bar_held == lib::Pane::none);
        }
    }
}

/// An expansion a registry offers draws the download arrow inside its pill.
/// The whole pill is the press.
void expansion_download_arrow_sits_inside_the_pill() {
    const kit::Frame regular = fixture::frames()[1].frame;
    lib::Inputs inputs = fixture::fixture_inputs();
    lib::DownloadableExpansion units;
    units.id = "cavedog-units";
    units.registry = std::string(fixture::core);
    units.name = "Cavedog units";
    units.extends = {"totala"};
    inputs.downloadable_expansions.push_back(std::move(units));
    lib::Library library;
    lib::refresh(library, std::move(inputs));
    lib::set_tab(library, lib::Tab::active);
    lib::ScreenState state;
    const kit::DisplayList list = laid_out(library, state, regular);
    OA_EXPECT_NONE(meeting_controls(list), "expansion arrow");
    OA_EXPECT_NONE(order_problems(list), "expansion arrow");
    OA_EXPECT_NONE(name_problems(list), "expansion arrow");
    OA_EXPECT_NONE(fixture::texts_not_fitting(list, fonts()), "expansion arrow");
    const kit::Control* pill = fixture::named(list, "library.active.expansion.cavedog-units");
    const kit::Control* separate =
        fixture::named(list, "library.active.expansion.cavedog-units.get");
    const kit::Control* add = fixture::named(list, "library.active.expansion.campaign");
    OA_CHECK(pill != nullptr && separate == nullptr && add != nullptr);
    OA_CHECK(pill != nullptr && pill->text.find("ADD FROM") == std::string::npos);
    OA_CHECK(pill != nullptr && pill->enabled && pill->focusable);
    OA_CHECK(add != nullptr && add->text.find("ADD FROM") != std::string::npos);
    const kit::Item* pill_item = nullptr;
    if (pill != nullptr) {
        for (const kit::Item& item : list.items) {
            if (item.control == pill->id && item.role == kit::Role::pill)
                pill_item = &item;
        }
    }
    OA_CHECK(pill_item != nullptr);
    if (pill_item != nullptr && pill != nullptr) {
        const auto* look = std::get_if<kit::PillLook>(&pill_item->look);
        OA_CHECK(look != nullptr && look->icon == kit::StateIcon::download_arrow);
        OA_CHECK(pill_item->rect.x == pill->rect.x && pill_item->rect.width == pill->rect.width);
    }
    const lib::ScreenResult result =
        click(library, state, list, "library.active.expansion.cavedog-units");
    OA_CHECK(result.action == lib::Action::get);
    OA_CHECK(result.expansion == "cavedog-units");
    OA_CHECK(result.registry == fixture::core);
    OA_CHECK(result.entry.key.empty());
    OA_CHECK(library.content.downloads.size() == 1);
    OA_CHECK(!library.content.edited);
}

/// Returns the item that draws a control as a pill.
///
/// @param list the display list
/// @param control the control
/// @return the item; null when none draws it
const kit::Item* pill_item_of(const kit::DisplayList& list, const kit::Control& control) {
    for (const kit::Item& item : list.items)
        if (item.control == control.id && item.role == kit::Role::pill)
            return &item;
    return nullptr;
}

/// Tells whether the kit's locked fade covers a rectangle exactly.
///
/// @param list the display list
/// @param rect the rectangle
/// @return true when a fade covers it
bool faded(const kit::DisplayList& list, const kit::Rect& rect) {
    return std::any_of(list.items.begin(), list.items.end(), [&rect](const kit::Item& item) {
        return item.role == kit::Role::locked_fade && item.rect.x == rect.x &&
               item.rect.y == rect.y && item.rect.width == rect.width &&
               item.rect.height == rect.height;
    });
}

/// Counts the padlocks drawn.
///
/// @param list the display list
/// @return how many
std::size_t padlock_count(const kit::DisplayList& list) {
    return static_cast<std::size_t>(
        std::count_if(list.items.begin(), list.items.end(), [](const kit::Item& item) {
            const auto* look = std::get_if<kit::MarkLook>(&item.look);
            return item.role == kit::Role::mark && look != nullptr &&
                   look->mark == kit::Mark::padlock && look->colour == kit::colour::lock;
        })
    );
}

/// A pill the command line's mod needs is drawn locked at every size: its
/// face kept under the kit's locked fade, out of Tab's order and taking no
/// press, with the padlock and the reason in the lock colour under the
/// pills, which a label reads whole. Pills the player can turn keep their
/// look, and without the command line nothing is locked.
void locked_pill_shows_why() {
    lib::Inputs inputs = fixture::fixture_inputs();
    inputs.content.base_id = "totala";
    inputs.content.copied = {"totala", "campaign", "core-contingency", "music"};
    inputs.content.enabled = {"campaign", "core-contingency"};
    inputs.content.mod_required = {"campaign"};
    inputs.mod_from_command_line = true;
    const std::string reason = "The command line chose this run's mod, which needs Campaign.";

    for (const fixture::NamedFrame& named : fixture::frames()) {
        lib::Library library;
        lib::refresh(library, inputs);
        lib::set_tab(library, lib::Tab::active);
        lib::ScreenState state;
        const kit::DisplayList list = laid_out(library, state, named.frame);
        const std::string where = named.name + " locked pill";
        const kit::Point window = lib::window_size(named.frame);
        OA_EXPECT_NONE(fixture::outside_window(list, window), where);
        OA_EXPECT_NONE(meeting_controls(list), where);
        OA_EXPECT_NONE(order_problems(list), where);
        OA_EXPECT_NONE(name_problems(list), where);
        OA_EXPECT_NONE(fixture::texts_not_fitting(list, fonts()), where);

        const kit::Control* campaign = fixture::named(list, "library.active.expansion.campaign");
        const kit::Control* contingency =
            fixture::named(list, "library.active.expansion.core-contingency");
        const kit::Control* music = fixture::named(list, "library.active.expansion.music");
        OA_CHECK(campaign != nullptr && contingency != nullptr && music != nullptr);
        if (campaign == nullptr || contingency == nullptr || music == nullptr)
            continue;
        OA_CHECK(!campaign->enabled && !campaign->focusable);
        OA_CHECK(
            std::find(list.tab_order.begin(), list.tab_order.end(), campaign->id) ==
            list.tab_order.end()
        );
        const kit::Item* locked = pill_item_of(list, *campaign);
        OA_CHECK(locked != nullptr);
        if (locked != nullptr) {
            const auto* look = std::get_if<kit::PillLook>(&locked->look);
            OA_CHECK(look != nullptr && look->state == kit::PillState::active && !look->disabled);
            OA_CHECK(locked->state.locked);
            OA_CHECK(faded(list, locked->rect));
        }
        for (const kit::Control* turns : {contingency, music}) {
            OA_CHECK(turns->enabled && turns->focusable);
            const kit::Item* turning = pill_item_of(list, *turns);
            OA_CHECK(turning != nullptr && !turning->state.locked && !faded(list, turning->rect));
            if (turning != nullptr) {
                const auto* look = std::get_if<kit::PillLook>(&turning->look);
                OA_CHECK(look != nullptr && !look->disabled);
            }
        }

        const kit::Control* lock = fixture::named(list, "library.active.lock");
        OA_CHECK(lock != nullptr);
        if (lock == nullptr)
            continue;
        OA_CHECK(lock->text == reason);
        OA_CHECK(lock->rect.y >= campaign->rect.y + campaign->rect.height);
        OA_CHECK(padlock_count(list) == 1);
        std::string shown;
        for (const kit::Item& item : list.items) {
            if (item.role != kit::Role::text || item.colour != kit::colour::lock ||
                !kit::wholly_in(item.rect, lock->rect))
                continue;
            if (!shown.empty())
                shown.push_back(' ');
            shown += item.text;
        }
        OA_CHECK(shown == reason);

        const lib::ScreenResult pressed =
            click(library, state, list, "library.active.expansion.campaign");
        OA_CHECK(!pressed.action.has_value() && pressed.expansion.empty());
        OA_CHECK(!library.content.edited && !library.content.asking);
    }

    // Chosen in the game, the same choice asks before Campaign goes off, so
    // no pill is locked and nothing is faded.
    lib::Inputs in_game = inputs;
    in_game.mod_from_command_line = false;
    lib::Library library;
    lib::refresh(library, std::move(in_game));
    lib::set_tab(library, lib::Tab::active);
    const kit::DisplayList list = laid_out(library, {}, fixture::frames()[0].frame);
    OA_CHECK(fixture::named(list, "library.active.lock") == nullptr);
    OA_CHECK(padlock_count(list) == 0);
    OA_CHECK(std::none_of(list.items.begin(), list.items.end(), [](const kit::Item& item) {
        return item.role == kit::Role::locked_fade || item.state.locked;
    }));
    const kit::Control* campaign = fixture::named(list, "library.active.expansion.campaign");
    OA_CHECK(campaign != nullptr && campaign->enabled && campaign->focusable);
}

/// Returns the strip of tabs, or null when the layout has none.
///
/// @param list the display list
/// @return the item
const kit::Item* tab_strip(const kit::DisplayList& list) {
    for (const kit::Item& item : list.items)
        if (item.role == kit::Role::tabs)
            return &item;
    return nullptr;
}

/// Tells whether the open tab lies wholly in the strip's view.
///
/// @param list the display list
/// @return true when it does
bool open_tab_is_shown(const kit::DisplayList& list) {
    const kit::Item* strip = tab_strip(list);
    if (strip == nullptr)
        return false;
    const auto* look = std::get_if<kit::TabsLook>(&strip->look);
    if (look == nullptr)
        return false;
    const kit::TabStrip placed = kit::place_tabs(fonts(), strip->rect, *look);
    return placed.selected < placed.tabs.size() &&
           kit::wholly_in(placed.tabs[placed.selected], placed.view);
}

/// The tabs keep their full names. At Compact the strip scrolls, the open
/// tab stays in view, and an arrow or the wheel moves it by one tab. Where
/// the names fit there are no arrows. A download draws its bar at every size.
void tabs_scroll_and_the_queue_shows_its_bar() {
    const std::vector<fixture::NamedFrame> all = fixture::frames();
    lib::Library library = with_ridge();
    for (const fixture::NamedFrame& named : all) {
        lib::ScreenState state;
        const kit::DisplayList list = laid_out(library, state, named.frame);
        const bool compact = named.frame.size_class == kit::SizeClass::compact;
        const kit::Control* languages = fixture::named(list, "library.tab.languages");
        const kit::Control* updates = fixture::named(list, "library.tab.updates");
        const kit::Control* registries = fixture::named(list, "library.tab.registries");
        OA_CHECK(languages != nullptr && languages->text.starts_with("Languages ("));
        OA_CHECK(updates != nullptr && updates->text.starts_with("Updates ("));
        OA_CHECK(registries != nullptr && registries->text.starts_with("Registries ("));
        OA_CHECK(open_tab_is_shown(list));
        const kit::Item* strip = tab_strip(list);
        const kit::Control* left = fixture::named(list, "library.tab.scroll-left");
        const kit::Control* right = fixture::named(list, "library.tab.scroll-right");
        bool overflows = false;
        if (strip != nullptr) {
            if (const auto* look = std::get_if<kit::TabsLook>(&strip->look))
                overflows = kit::place_tabs(fonts(), strip->rect, *look).overflows;
        }
        if (overflows && compact) {
            OA_CHECK(state.tab_scroll == 0);
            OA_CHECK(left == nullptr && right != nullptr && !right->focusable);
        } else {
            OA_CHECK(left == nullptr && right == nullptr);
        }
        const kit::Control* queue = fixture::named(list, "library.queue");
        const kit::Item* bar = nullptr;
        for (const kit::Item& item : list.items)
            if (item.role == kit::Role::progress)
                bar = &item;
        const kit::Metrics& metrics = kit::metrics_of(named.frame.size_class);
        OA_CHECK(queue != nullptr && bar != nullptr);
        if (queue != nullptr && bar != nullptr) {
            OA_CHECK(bar->rect.height == metrics.progress_bar_height);
            OA_CHECK(bar->rect.y >= queue->rect.y + queue->rect.height);
            OA_CHECK(bar->rect.x == queue->rect.x);
            OA_CHECK(bar->rect.width == queue->rect.width);
        }
    }

    lib::set_tab(library, lib::Tab::mods);
    for (const fixture::NamedFrame& named : {all[1], all[2]}) {
        const kit::DisplayList list = laid_out(library, {}, named.frame);
        OA_CHECK(fixture::named(list, "library.tab.scroll-left") == nullptr);
        OA_CHECK(fixture::named(list, "library.tab.scroll-right") == nullptr);
        OA_CHECK(open_tab_is_shown(list));
    }

    // Narrower than the fixture's Compact, so the full names overflow the
    // synthetic font the same way they overflow a Compact window in the
    // game's font.
    kit::Frame narrow;
    narrow.area = {0, 0, 280, 480};
    narrow.size_class = kit::SizeClass::compact;
    narrow.scale_percent = 100;
    lib::set_tab(library, lib::Tab::registries);
    lib::ScreenState at_end;
    const kit::DisplayList end = laid_out(library, at_end, narrow);
    const kit::Item* end_strip = tab_strip(end);
    bool end_overflows = false;
    if (end_strip != nullptr) {
        if (const auto* look = std::get_if<kit::TabsLook>(&end_strip->look))
            end_overflows = kit::place_tabs(fonts(), end_strip->rect, *look).overflows;
    }
    OA_CHECK(end_overflows);
    OA_CHECK(at_end.tab_scroll > 0);
    OA_CHECK(fixture::named(end, "library.tab.scroll-left") != nullptr);
    OA_CHECK(fixture::named(end, "library.tab.scroll-right") == nullptr);
    OA_CHECK(open_tab_is_shown(end));

    lib::set_tab(library, lib::Tab::updates);
    lib::ScreenState scrolling;
    const kit::DisplayList updates = laid_out(library, scrolling, narrow);
    const kit::Item* strip = tab_strip(updates);
    OA_CHECK(strip != nullptr && open_tab_is_shown(updates));
    if (strip != nullptr) {
        const kit::Point over{
            strip->rect.x + strip->rect.width / 2, strip->rect.y + strip->rect.height / 2
        };
        const int32_t before_scroll = scrolling.tab_scroll;
        const int32_t stepped = kit::tab_scroll_step(updates, 1);
        const lib::ScreenResult turned =
            lib::library_wheel(library, scrolling, updates, over, -1.0F);
        OA_CHECK(stepped != before_scroll);
        OA_CHECK(turned.redraw);
        OA_CHECK(scrolling.tab_scroll == stepped);
        OA_CHECK(scrolling.list_scroll == 0);
        OA_CHECK(open_tab_is_shown(laid_out(library, scrolling, narrow)));

        lib::ScreenState by_arrow;
        const kit::DisplayList before = laid_out(library, by_arrow, narrow);
        const int32_t by_step = kit::tab_scroll_step(before, 1);
        click(library, by_arrow, before, "library.tab.scroll-right");
        OA_CHECK(by_arrow.tab_scroll == by_step);
        OA_CHECK(open_tab_is_shown(laid_out(library, by_arrow, narrow)));
    }

    library = with_ridge();
    lib::ScreenState commands;
    kit::DisplayList shown = laid_out(library, commands, narrow);
    for (int32_t step = 0; library.tab != lib::Tab::registries && step < 8; ++step)
        lib::library_command(library, commands, shown, lib::ScreenCommand::next_tab);
    shown = laid_out(library, commands, narrow);
    OA_CHECK(library.tab == lib::Tab::registries);
    OA_CHECK(open_tab_is_shown(shown));
    OA_CHECK(commands.tab_scroll > 0);
}

} // namespace

/// Answers the tabs' names in a wordier language, as Ukrainian's run longer
/// than English's.
///
/// @param english the English text
/// @return the longer words, or the English
std::string_view wordy_tabs(void*, std::string_view english) {
    if (english == lib::words::tab_active)
        return "Aktivnistyyy";
    if (english == lib::words::tab_mods)
        return "Modyfikatsiyi";
    if (english == lib::words::tab_maps)
        return "Mapyyyyyyy";
    if (english == lib::words::tab_mutators)
        return "Mutatoryyyyyy";
    if (english == lib::words::tab_brains)
        return "Mozkyyy";
    if (english == lib::words::tab_languages)
        return "Movyyyyyyyy";
    if (english == lib::words::tab_updates)
        return "Onovlennyayyy";
    if (english == lib::words::tab_registries)
        return "Reyestryyyyy";
    return english;
}

/// Returns a control's rectangle that a press reaches: within its clip.
///
/// @param control the control
/// @return the rectangle; empty when the clip hides it
kit::Rect pressed_area(const kit::Control& control) {
    if (control.clip.width <= 0 || control.clip.height <= 0)
        return control.rect;
    return kit::intersect(control.rect, control.clip);
}

/// Where a catalogue tab's names do not fit beside the search, at Regular
/// and Large, the tabs take the whole row and the search a row under them:
/// every tab is shown whole and none lies under the search, so a press on
/// a tab, Registries among them, opens it. English fits beside the search
/// at Large. At Regular the eight captions take the row.
void wordy_tabs_take_the_row_over_the_search() {
    for (const fixture::NamedFrame& named : fixture::frames()) {
        if (named.frame.size_class == kit::SizeClass::compact)
            continue;
        for (const bool wordy : {false, true}) {
            lib::Library library = with_ridge();
            if (wordy)
                library.text.shown = wordy_tabs;
            lib::set_tab(library, lib::Tab::mods);
            const kit::DisplayList list = laid_out(library, {}, named.frame);
            const kit::Item* strip = tab_strip(list);
            const kit::Control* search = fixture::named(list, "library.search");
            OA_CHECK(strip != nullptr && search != nullptr);
            if (strip == nullptr || search == nullptr)
                continue;
            const auto* look = std::get_if<kit::TabsLook>(&strip->look);
            OA_CHECK(look != nullptr && !kit::place_tabs(fonts(), strip->rect, *look).overflows);
            const bool beside = search->rect.y < strip->rect.y + strip->rect.height;
            if (!wordy) {
                if (named.frame.size_class == kit::SizeClass::large)
                    OA_CHECK(beside);
                if (named.frame.size_class == kit::SizeClass::regular)
                    OA_CHECK(!beside);
            }
            for (const std::string_view word :
                 {"active",
                  "mods",
                  "maps",
                  "mutators",
                  "brains",
                  "languages",
                  "updates",
                  "registries"}) {
                const kit::Control* tab = fixture::named(list, "library.tab." + std::string(word));
                OA_CHECK(tab != nullptr);
                if (tab == nullptr)
                    continue;
                const kit::Rect area = pressed_area(*tab);
                OA_CHECK(kit::wholly_in(tab->rect, strip->rect));
                const kit::Rect shared = kit::intersect(area, search->rect);
                OA_CHECK(area.width > 0 && (shared.width == 0 || shared.height == 0));
            }
            if (wordy) {
                // The words were chosen to need the whole row at Regular.
                if (named.name == "regular")
                    OA_CHECK(!beside);
                if (!beside)
                    OA_CHECK(search->rect.y >= strip->rect.y + strip->rect.height);
            }
            // Registries opens with a press at its centre.
            lib::ScreenState state;
            click(library, state, list, "library.tab.registries");
            OA_CHECK(library.tab == lib::Tab::registries);
        }
    }
}

/// A Library of the five made-up mutators, on the Mutators tab.
///
/// @return the Library
lib::Library mutators_library() {
    lib::Inputs inputs;
    lib::Registry built_in;
    built_in.id = "fixture-core";
    built_in.name = "Fixture Core";
    built_in.built_in = true;
    inputs.registries.push_back(built_in);
    inputs.engine = {0, 8, 0};
    inputs.version_text = "v0.8.0";
    const auto add = [&](std::string key, std::string name, std::vector<std::string> tags) {
        lib::Listing made;
        made.registry = "fixture-core";
        made.kind = lib::Kind::mutator;
        made.key = std::move(key);
        made.name = std::move(name);
        made.version = "1.0";
        made.author = "Fixture Author";
        made.summary = "A made-up mutator.";
        made.size = 1024;
        lib::MutatorFacts facts;
        facts.tags = std::move(tags);
        facts.works_with.push_back({"*", {}, {}});
        made.mutator = std::move(facts);
        inputs.listings.push_back(std::move(made));
    };
    add("fast-builders", "Fast Builders", {"economy", "statistics"});
    add("gatehouse", "Gatehouse", {"gameplay", "new-units"});
    add("gatehouse-keep", "Gatehouse Keep", {"economy"});
    add("ridge-walls", "Ridge Walls", {"commanders"});
    add("grounded", "Grounded", {"disable", "ai", "maps"});
    lib::Library library;
    lib::refresh(library, std::move(inputs));
    lib::set_tab(library, lib::Tab::mutators);
    return library;
}

/// The search field's placeholder, or empty when the layout has no search.
///
/// @param list the display list
/// @return the placeholder
std::string search_placeholder(const kit::DisplayList& list) {
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::field)
            continue;
        if (const auto* look = std::get_if<kit::SearchLook>(&item.look))
            return look->placeholder;
    }
    return {};
}

/// The Mutators tab lays out at every frame: its parts stay inside the
/// window, its controls do not meet, Tab's order and the names hold, and
/// every text fits. The open tab stays wholly in view, including on a
/// strip narrower than Compact.
void mutators_tab_holds_its_invariants() {
    const lib::Library library = mutators_library();
    const std::string full(lib::words::search_mutators);
    for (const fixture::NamedFrame& named : fixture::frames()) {
        const kit::DisplayList list = laid_out(library, {}, named.frame);
        const kit::Point window = lib::window_size(named.frame);
        OA_EXPECT_NONE(fixture::outside_window(list, window), named.name + " mutators");
        OA_EXPECT_NONE(meeting_controls(list), named.name + " mutators");
        OA_EXPECT_NONE(order_problems(list), named.name + " mutators");
        OA_EXPECT_NONE(name_problems(list), named.name + " mutators");
        OA_EXPECT_NONE(fixture::texts_not_fitting(list, fonts()), named.name + " mutators");
        const kit::Control* tab = fixture::named(list, "library.tab.mutators");
        OA_CHECK(tab != nullptr && tab->text.starts_with("Mutators ("));
        OA_CHECK(open_tab_is_shown(list));
        const std::string placeholder = search_placeholder(list);
        const bool whole = placeholder == full;
        const bool cut = placeholder.size() >= 3 && placeholder.ends_with("...") &&
                         full.starts_with(placeholder.substr(0, placeholder.size() - 3));
        OA_CHECK(whole || cut);
        if (named.frame.size_class != kit::SizeClass::compact)
            OA_CHECK(whole);
    }

    kit::Frame narrow;
    narrow.area = {0, 0, 280, 480};
    narrow.size_class = kit::SizeClass::compact;
    narrow.scale_percent = 100;
    const kit::DisplayList narrow_list = laid_out(library, {}, narrow);
    OA_CHECK(open_tab_is_shown(narrow_list));
    OA_EXPECT_NONE(fixture::texts_not_fitting(narrow_list, fonts()), "narrow mutators");
}

/// Where a named mutator row sits in the visible list, or -1.
///
/// @param library the Library
/// @param key the mutator's key
/// @return the place
int32_t mutator_place(const lib::Library& library, std::string_view key) {
    for (int32_t place = 0; place < static_cast<int32_t>(library.visible.size()); ++place) {
        const lib::Entry& entry = library.entries[library.visible[static_cast<std::size_t>(place)]];
        if (entry.id.key == key)
            return place;
    }
    return -1;
}

/// Lays the list out with one mutator row at the top, and returns its look.
///
/// @param library the Library
/// @param frame the frame
/// @param key the mutator's key
/// @param[out] list the display list
/// @return the row's look; null when the row is not there
const kit::ListRowLook* reveal_mutator(
    const lib::Library& library,
    const kit::Frame& frame,
    std::string_view key,
    kit::DisplayList& list
) {
    lib::ScreenState state;
    state.reveal_row = mutator_place(library, key);
    list = laid_out(library, state, frame);
    return row_look_named(list, "library.row.fixture-core." + std::string(key));
}

/// The side of a row's badge, as the kit draws it.
///
/// @param row the row
/// @return the side, in points
int32_t shown_badge_side(const kit::Rect& row) {
    const int32_t inset = kit::compact_metrics.list_row_inset;
    return std::max(
        std::min(row.height - 2 * inset, kit::compact_metrics.list_row_badge), int32_t{0}
    );
}

/// The details' tag badge, when the selected mutator shows one.
///
/// @param list the display list
/// @return the badge; null when the details show a picture or letters instead
const kit::BadgeLook* detail_tag_badge(const kit::DisplayList& list) {
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::package_badge || item.rect.width != 40 ||
            item.rect.height != 40)
            continue;
        return std::get_if<kit::BadgeLook>(&item.look);
    }
    return nullptr;
}

/// Tells whether a fade covers a row.
///
/// @param list the display list
/// @param row the row
/// @return true when a locked fade meets the row
bool row_is_dimmed(const kit::DisplayList& list, const kit::Rect& row) {
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::locked_fade)
            continue;
        const kit::Rect shared = kit::intersect(item.rect, row);
        if (shared.width > 0 && shared.height > 0)
            return true;
    }
    return false;
}

/// A Library of made-up mutators that exercise badges, chips and tag icons.
///
/// Signal is selected. Played with no mod.
///
/// @return the Library
lib::Library badge_library() {
    lib::Inputs inputs;
    lib::Registry built_in;
    built_in.id = "fixture-core";
    built_in.name = "Fixture Core";
    built_in.built_in = true;
    inputs.registries.push_back(built_in);
    inputs.engine = {0, 8, 0};
    inputs.version_text = "v0.8.0";
    const auto add = [&](std::string key, std::string name, std::vector<std::string> tags) {
        lib::Listing made;
        made.registry = "fixture-core";
        made.kind = lib::Kind::mutator;
        made.key = std::move(key);
        made.name = std::move(name);
        made.version = "1.0";
        made.author = "Fixture Author";
        made.summary = "A made-up mutator.";
        made.size = 1024;
        lib::MutatorFacts facts;
        facts.tags = std::move(tags);
        made.mutator = std::move(facts);
        inputs.listings.push_back(std::move(made));
    };
    add("beacon", "Beacon", {"gameplay"});
    inputs.listings.back().badge = fixture::gradient_badge();
    add("signal", "Signal", {"economy", "statistics"});
    add("plain-mark", "Plain Mark", {});
    add("odd-mark", "Odd Mark", {"custom"});
    add("gatehouse-keep", "Gatehouse Keep", {"economy"});
    inputs.listings.back().mutator->needs.push_back({"gatehouse", {}, "Gatehouse"});
    add("ridge-walls", "Ridge Walls", {"commanders"});
    inputs.listings.back().mutator->works_with.push_back({"ridge", {}, "Ridge"});
    add("both-marks", "Both Marks", {"economy"});
    inputs.listings.back().mutator->needs.push_back({"north-tower", {}, "North Tower"});
    inputs.listings.back().mutator->works_with.push_back({"ridge", {}, "Ridge"});
    add("wide-claim",
        "Wide Claim",
        {"balance", "balance-extra", "balance-further", "balance-again", "balance-more"});
    inputs.listings.back().mutator->needs.push_back({"far-beacon", {}, "Far Beacon"});
    inputs.listings.back().mutator->works_with.push_back({"ridge", {}, "Ridge"});
    add("kept-icon", "Kept Icon", {});
    add("kept-mark", "Kept Mark", {});
    inputs.listings.back().mutator.reset();
    add("listed-first", "Listed First", {"economy"});
    const auto install = [&](std::string_view key, std::vector<std::string> tags, bool picture) {
        const lib::Listing* from = nullptr;
        for (const lib::Listing& listing : inputs.listings)
            if (listing.key == key)
                from = &listing;
        lib::Installed copy = fixture::installed(*from, "1.0", 1);
        if (picture)
            copy.badge = fixture::gradient_badge();
        lib::MutatorFacts facts;
        facts.tags = std::move(tags);
        copy.mutator = std::move(facts);
        inputs.installed.push_back(std::move(copy));
    };
    install("kept-icon", {}, true);
    install("kept-mark", {"commanders"}, false);
    install("listed-first", {"commanders"}, false);
    lib::Library library;
    lib::refresh(library, std::move(inputs));
    lib::set_tab(library, lib::Tab::mutators);
    lib::select(library, {"fixture-core", lib::Kind::mutator, "signal"});
    lib::open_details(library);
    return library;
}

/// Mutator rows show a picture, else the first tag's icon, else letters.
/// Large carries tag chips and the lock-coloured state chips. Compact names
/// a missing mutator or one that does not fit after the title. Tag pills and
/// the filter pane carry the eight icons. An incompatible row stays bright
/// and its GET stays on.
void mutator_rows_show_badges_chips_and_icons() {
    const lib::Library library = badge_library();
    const lib::Library mods = with_ridge();
    for (const fixture::NamedFrame& named : fixture::frames()) {
        const kit::DisplayList list = laid_out(library, {}, named.frame);
        OA_EXPECT_NONE(fixture::texts_not_fitting(list, fonts()), named.name + " mutator badges");
        const bool compact = named.frame.size_class == kit::SizeClass::compact;
        const bool regular = named.frame.size_class == kit::SizeClass::regular;
        const bool large = named.frame.size_class == kit::SizeClass::large;
        const int32_t badge_side = compact ? 20 : 28;

        kit::DisplayList shown;
        const kit::ListRowLook* beacon = reveal_mutator(library, named.frame, "beacon", shown);
        const kit::Control* beacon_row = fixture::named(shown, "library.row.fixture-core.beacon");
        OA_CHECK(beacon != nullptr && beacon_row != nullptr);
        if (beacon != nullptr && beacon_row != nullptr) {
            OA_CHECK(renderer::picture_drawable(beacon->badge));
            OA_CHECK(!beacon->badge_tag.has_value());
            OA_CHECK(shown_badge_side(beacon_row->rect) == badge_side);
            OA_CHECK(beacon->badge.width == 16 && beacon->badge.height == 16);
        }

        const kit::ListRowLook* signal = reveal_mutator(library, named.frame, "signal", shown);
        const kit::Control* signal_row = fixture::named(shown, "library.row.fixture-core.signal");
        OA_CHECK(signal != nullptr && signal_row != nullptr);
        if (signal != nullptr && signal_row != nullptr) {
            OA_CHECK(signal->badge_tag == kit::TagIcon::economy);
            OA_CHECK(!renderer::picture_drawable(signal->badge));
            OA_CHECK(shown_badge_side(signal_row->rect) == badge_side);
        }

        const kit::ListRowLook* plain = reveal_mutator(library, named.frame, "plain-mark", shown);
        OA_CHECK(plain != nullptr);
        if (plain != nullptr) {
            OA_CHECK(!plain->badge_tag.has_value());
            OA_CHECK(plain->badge_text == lib::badge_letters("Plain Mark"));
        }
        const kit::ListRowLook* odd = reveal_mutator(library, named.frame, "odd-mark", shown);
        OA_CHECK(odd != nullptr);
        if (odd != nullptr) {
            OA_CHECK(!odd->badge_tag.has_value());
            OA_CHECK(odd->badge_text == lib::badge_letters("Odd Mark"));
        }

        const kit::ListRowLook* kept_icon =
            reveal_mutator(library, named.frame, "kept-icon", shown);
        OA_CHECK(kept_icon != nullptr && renderer::picture_drawable(kept_icon->badge));
        OA_CHECK(kept_icon == nullptr || !kept_icon->badge_tag.has_value());
        const kit::ListRowLook* kept_mark =
            reveal_mutator(library, named.frame, "kept-mark", shown);
        OA_CHECK(kept_mark != nullptr && kept_mark->badge_tag == kit::TagIcon::commanders);
        const kit::ListRowLook* listed =
            reveal_mutator(library, named.frame, "listed-first", shown);
        OA_CHECK(listed != nullptr && listed->badge_tag == kit::TagIcon::economy);

        lib::ScreenState details;
        details.details_page = compact;
        const kit::DisplayList detail_list = laid_out(library, details, named.frame);
        OA_EXPECT_NONE(
            fixture::texts_not_fitting(detail_list, fonts()), named.name + " mutator details"
        );
        // Every size class shows the 40-point tag badge, Compact's details
        // page included, beside the name.
        const kit::BadgeLook* badge = detail_tag_badge(detail_list);
        OA_CHECK(badge != nullptr && badge->tag == kit::TagIcon::economy);
        OA_CHECK(badge == nullptr || badge->face == kit::colour::hint);
        const kit::PillLook* detail_economy = pill_named(detail_list, "library.detail.tag.economy");
        OA_CHECK(detail_economy != nullptr && detail_economy->tag == kit::TagIcon::economy);
        const kit::PillLook* detail_statistics =
            pill_named(detail_list, "library.detail.tag.statistics");
        OA_CHECK(
            detail_statistics != nullptr && detail_statistics->tag == kit::TagIcon::statistics
        );

        if (large) {
            const kit::ListRowLook* keep =
                reveal_mutator(library, named.frame, "gatehouse-keep", shown);
            OA_CHECK(keep != nullptr && keep->tags.size() >= 2);
            if (keep != nullptr && keep->tags.size() >= 2) {
                OA_CHECK(keep->tags[0].text == "Economy");
                OA_CHECK(keep->tags[0].state == kit::ChipState::filter);
                OA_CHECK(keep->tags[1].text == "Needs Gatehouse");
                OA_CHECK(keep->tags[1].state == kit::ChipState::update);
            }
            const kit::ListRowLook* walls =
                reveal_mutator(library, named.frame, "ridge-walls", shown);
            const kit::Control* walls_row =
                fixture::named(shown, "library.row.fixture-core.ridge-walls");
            const kit::Control* walls_get =
                fixture::named(shown, "library.row.fixture-core.ridge-walls.get");
            OA_CHECK(walls != nullptr && walls->tags.size() >= 2);
            if (walls != nullptr && walls->tags.size() >= 2) {
                OA_CHECK(walls->tags[0].text == "Commanders");
                OA_CHECK(walls->tags[1].text == "Not compatible with this mod");
                OA_CHECK(walls->tags[1].state == kit::ChipState::update);
                OA_CHECK(walls->tags[1].state != kit::ChipState::problem);
            }
            OA_CHECK(walls_get != nullptr && walls_get->enabled);
            OA_CHECK(walls != nullptr && walls->chip.has_value() && !walls->chip->disabled);
            OA_CHECK(walls_row != nullptr && !row_is_dimmed(shown, walls_row->rect));
            const kit::ListRowLook* both =
                reveal_mutator(library, named.frame, "both-marks", shown);
            OA_CHECK(both != nullptr && both->tags.size() >= 3);
            if (both != nullptr && both->tags.size() >= 3) {
                OA_CHECK(both->tags[0].text == "Economy");
                OA_CHECK(both->tags[1].text == "Needs North Tower");
                OA_CHECK(both->tags[1].state == kit::ChipState::update);
                OA_CHECK(both->tags[2].text == "Not compatible with this mod");
                OA_CHECK(both->tags[2].state == kit::ChipState::update);
            }
            const kit::ListRowLook* wide =
                reveal_mutator(library, named.frame, "wide-claim", shown);
            OA_CHECK(wide != nullptr && wide->tags.size() >= 2);
            if (wide != nullptr && wide->tags.size() >= 2) {
                OA_CHECK(wide->tags[0].text == "Needs Far Beacon");
                OA_CHECK(wide->tags[0].state == kit::ChipState::update);
                OA_CHECK(wide->tags[1].text == "Not compatible with this mod");
                OA_CHECK(wide->tags[1].state == kit::ChipState::update);
                for (const kit::ChipLook& chip : wide->tags) {
                    if (chip.state == kit::ChipState::filter)
                        OA_CHECK(chip.text != wide->tags[0].text);
                }
            }
            const kit::NavLook* nav = nullptr;
            for (const kit::Item& item : list.items) {
                if (item.role != kit::Role::nav)
                    continue;
                if (const auto* look = std::get_if<kit::NavLook>(&item.look);
                    look != nullptr && !look->entries.empty())
                    nav = look;
            }
            OA_CHECK(nav != nullptr);
            if (nav != nullptr) {
                bool economy = false;
                bool balance = false;
                for (const kit::NavEntry& entry : nav->entries) {
                    if (entry.caption.starts_with("Economy")) {
                        economy = true;
                        OA_CHECK(entry.tag == kit::TagIcon::economy);
                    }
                    if (entry.caption.starts_with("balance")) {
                        balance = true;
                        OA_CHECK(!entry.tag.has_value());
                    }
                    if (entry.caption.starts_with("custom"))
                        OA_CHECK(!entry.tag.has_value());
                }
                OA_CHECK(economy);
                OA_CHECK(balance);
            }
        }
        if (compact) {
            const kit::ListRowLook* keep =
                reveal_mutator(library, named.frame, "gatehouse-keep", shown);
            OA_CHECK(keep != nullptr);
            if (keep != nullptr) {
                OA_CHECK(keep->by == "· needs Gatehouse");
                OA_CHECK(keep->by_colour == kit::colour::lock);
                OA_CHECK(!keep->lines.empty());
            }
            const kit::ListRowLook* walls =
                reveal_mutator(library, named.frame, "ridge-walls", shown);
            const kit::Control* walls_row =
                fixture::named(shown, "library.row.fixture-core.ridge-walls");
            const kit::Control* walls_get =
                fixture::named(shown, "library.row.fixture-core.ridge-walls.get");
            OA_CHECK(walls != nullptr);
            if (walls != nullptr) {
                OA_CHECK(walls->by == "· not compatible with this mod");
                OA_CHECK(walls->by_colour == kit::colour::lock);
                OA_CHECK(!walls->lines.empty());
                OA_CHECK(walls->chip.has_value() && !walls->chip->disabled);
            }
            OA_CHECK(walls_get != nullptr && walls_get->enabled);
            OA_CHECK(walls_row != nullptr && !row_is_dimmed(shown, walls_row->rect));
            const kit::ListRowLook* both =
                reveal_mutator(library, named.frame, "both-marks", shown);
            OA_CHECK(both != nullptr && both->by == "· needs North Tower");
            OA_CHECK(both == nullptr || both->by_colour == kit::colour::lock);
        }
        if (regular) {
            const kit::PillLook* economy = pill_named(list, "library.tag.economy");
            OA_CHECK(economy != nullptr && economy->tag == kit::TagIcon::economy);
            const kit::PillLook* commanders = pill_named(list, "library.tag.commanders");
            OA_CHECK(commanders != nullptr && commanders->tag == kit::TagIcon::commanders);
            const kit::PillLook* custom = pill_named(list, "library.tag.custom");
            if (custom != nullptr)
                OA_CHECK(!custom->tag.has_value());
            const kit::PillLook* balance = pill_named(list, "library.tag.balance");
            if (balance != nullptr)
                OA_CHECK(!balance->tag.has_value());
        }
    }

    lib::Library odd = library;
    lib::select(odd, {"fixture-core", lib::Kind::mutator, "odd-mark"});
    const kit::DisplayList odd_list = laid_out(odd, {}, fixture::frames()[1].frame);
    const kit::PillLook* custom = pill_named(odd_list, "library.detail.tag.custom");
    OA_CHECK(custom != nullptr && !custom->tag.has_value());

    const kit::Frame regular = fixture::frames()[1].frame;
    lib::ScreenState mods_state;
    mods_state.reveal_row = mutator_place(mods, "quarry");
    const kit::DisplayList mods_list = laid_out(mods, mods_state, regular);
    for (const kit::Item& item : mods_list.items) {
        if (item.role != kit::Role::pill)
            continue;
        const kit::Control* control = kit::control_of(mods_list, item.control);
        if (control == nullptr || !control->name.starts_with("library.tag."))
            continue;
        const auto* look = std::get_if<kit::PillLook>(&item.look);
        OA_CHECK(look != nullptr && !look->tag.has_value());
    }
    const kit::ListRowLook* quarry = row_look_named(mods_list, "library.row.fixture-core.quarry");
    OA_CHECK(quarry != nullptr && !quarry->badge_tag.has_value());
}

/// An installed, updatable mutator whose check came back incompatible.
///
/// @return the Library, on Mutators, with Fast Builders selected
lib::Library fast_builders_library() {
    lib::Inputs inputs;
    lib::Registry registry;
    registry.id = std::string(fixture::core);
    registry.name = "Fixture Core";
    registry.built_in = true;
    inputs.registries.push_back(std::move(registry));
    inputs.engine = {0, 8, 0};
    inputs.version_text = "v0.8.0";
    lib::Listing made = fixture::listing(
        fixture::core,
        lib::Kind::mutator,
        "fast-builders",
        "Fast Builders",
        "2.0",
        1024,
        "Builds faster.",
        {"economy"}
    );
    made.homepage = "https://example.test/fast";
    lib::MutatorFacts facts;
    facts.works_with.push_back({"*", {}, {}});
    facts.layers = {"units"};
    made.mutator = facts;
    lib::Installed copy = fixture::installed(made, "1.0", 1);
    copy.mutator = facts;
    lib::Verdict verdict;
    verdict.state = lib::Verdict::State::incompatible;
    verdict.reason = "clashes";
    copy.mutator->verdict = std::move(verdict);
    inputs.listings.push_back(std::move(made));
    inputs.installed.push_back(std::move(copy));
    lib::UpdateNote note;
    note.registry = std::string(fixture::core);
    note.kind = lib::Kind::mutator;
    note.key = "fast-builders";
    note.to_release = 2;
    note.to_version = "2.0";
    note.to_revision = 1;
    note.size = 1024;
    inputs.updates.push_back(std::move(note));
    lib::Library library;
    lib::refresh(library, std::move(inputs));
    lib::set_tab(library, lib::Tab::mutators);
    lib::select(library, {std::string(fixture::core), lib::Kind::mutator, "fast-builders"});
    lib::open_details(library);
    return library;
}

/// Tells whether a layout draws the incompatible reason in the danger colour.
///
/// @param list the display list
/// @param reason the verdict's reason
/// @return true when a text in the danger colour contains it
bool shows_danger(const kit::DisplayList& list, std::string_view reason) {
    for (const kit::Item& item : list.items)
        if (item.colour == kit::colour::danger && item.text.find(reason) != std::string::npos)
            return true;
    return false;
}

/// Tells whether a layout draws these words in the lock colour.
///
/// @param list the display list
/// @param piece the words
/// @return true when a text in the lock colour contains them
bool shows_lock(const kit::DisplayList& list, std::string_view piece) {
    for (const kit::Item& item : list.items)
        if (item.colour == kit::colour::lock && item.text.find(piece) != std::string::npos)
            return true;
    return false;
}

/// Every action of an updatable mutator is a control at each class. Compact
/// shows them on the details page, the rest inside MORE ▾. The Rules fact is
/// whole, and the incompatible verdict is drawn in the danger colour.
void mutator_actions_rules_and_danger() {
    const lib::Library library = fast_builders_library();
    const lib::Entry* fast = lib::selected_entry(library);
    OA_CHECK(fast != nullptr);
    if (fast == nullptr)
        return;
    const std::vector<lib::ActionButton> actions = lib::entry_actions(library, *fast);
    OA_CHECK(actions.size() == 5);
    if (actions.size() != 5)
        return;
    for (const fixture::NamedFrame& named : fixture::frames()) {
        const kit::Frame& frame = named.frame;
        const bool compact = frame.size_class == kit::SizeClass::compact;
        const kit::Point window = lib::window_size(frame);
        if (!compact) {
            lib::ScreenState state;
            const kit::DisplayList list = laid_out(library, state, frame);
            OA_EXPECT_NONE(fixture::outside_window(list, window), named.name + " mutator actions");
            OA_EXPECT_NONE(meeting_controls(list), named.name + " mutator actions");
            OA_EXPECT_NONE(name_problems(list), named.name + " mutator actions");
            OA_EXPECT_NONE(
                fixture::texts_not_fitting(list, fonts()), named.name + " mutator actions"
            );
            OA_EXPECT_NONE(
                whole_fact_problems(library, list, true), named.name + " mutator actions"
            );
            for (const lib::ActionButton& action : actions)
                OA_CHECK(
                    fixture::named(list, "library.action." + std::string(word_of(action.action))) !=
                    nullptr
                );
            OA_CHECK(shows_danger(list, "clashes"));
        } else {
            lib::ScreenState page_state;
            page_state.details_page = true;
            const kit::DisplayList page = laid_out(library, page_state, frame);
            OA_EXPECT_NONE(
                fixture::outside_window(page, window), named.name + " mutator details page"
            );
            OA_EXPECT_NONE(meeting_controls(page), named.name + " mutator details page");
            OA_EXPECT_NONE(name_problems(page), named.name + " mutator details page");
            OA_EXPECT_NONE(
                fixture::texts_not_fitting(page, fonts()), named.name + " mutator details page"
            );
            OA_EXPECT_NONE(
                whole_fact_problems(library, page, false), named.name + " mutator details page"
            );
            OA_CHECK(shows_danger(page, "clashes"));
            for (std::size_t index = 0; index < 3; ++index)
                OA_CHECK(
                    fixture::named(
                        page, "library.action." + std::string(word_of(actions[index].action))
                    ) != nullptr
                );
            OA_CHECK(fixture::named(page, "library.action") != nullptr);
            page_state.menu = lib::MenuKind::more;
            const kit::DisplayList more = laid_out(library, page_state, frame);
            for (const lib::ActionButton& action : actions)
                OA_CHECK(
                    fixture::named(more, "library.action." + std::string(word_of(action.action))) !=
                    nullptr
                );
            OA_EXPECT_NONE(
                fixture::outside_window(more, window), named.name + " mutator details, MORE open"
            );
            OA_EXPECT_NONE(name_problems(more), named.name + " mutator details, MORE open");
            OA_EXPECT_NONE(
                fixture::texts_not_fitting(more, fonts()),
                named.name + " mutator details, MORE open"
            );
        }
    }
}

/// GET WITH is a control at every class, and the first action at Compact.
void get_with_is_a_control() {
    lib::Inputs inputs;
    lib::Registry registry;
    registry.id = std::string(fixture::core);
    registry.name = "Fixture Core";
    registry.built_in = true;
    inputs.registries.push_back(std::move(registry));
    inputs.engine = {0, 8, 0};
    inputs.version_text = "v0.8.0";
    lib::Listing gate = fixture::listing(
        fixture::core,
        lib::Kind::mutator,
        "gatehouse",
        "Gatehouse",
        "4.1",
        1024,
        "A gate.",
        {"gameplay"}
    );
    lib::MutatorFacts gate_facts;
    gate_facts.tags = {"gameplay"};
    gate.mutator = gate_facts;
    lib::Listing keep = fixture::listing(
        fixture::core,
        lib::Kind::mutator,
        "gatehouse-keep",
        "Gatehouse Keep",
        "2.0",
        1024,
        "Above the gate.",
        {"gameplay"}
    );
    lib::MutatorFacts keep_facts;
    keep_facts.tags = {"gameplay"};
    keep_facts.needs.push_back({"gatehouse", ">= 4.0", "Gatehouse"});
    keep.mutator = std::move(keep_facts);
    keep.homepage = "https://example.test/keep";
    inputs.listings.push_back(std::move(gate));
    inputs.listings.push_back(std::move(keep));
    lib::Library library;
    lib::refresh(library, std::move(inputs));
    lib::set_tab(library, lib::Tab::mutators);
    lib::select(library, {std::string(fixture::core), lib::Kind::mutator, "gatehouse-keep"});
    lib::open_details(library);
    const lib::Entry* selected = lib::selected_entry(library);
    OA_CHECK(selected != nullptr);
    if (selected == nullptr)
        return;
    const std::vector<lib::ActionButton> actions = lib::entry_actions(library, *selected);
    OA_CHECK(
        !actions.empty() && actions.front().action == lib::Action::get_with &&
        actions.front().enabled
    );
    for (const fixture::NamedFrame& named : fixture::frames()) {
        const bool compact = named.frame.size_class == kit::SizeClass::compact;
        lib::ScreenState state;
        if (compact)
            state.details_page = true;
        const kit::DisplayList list = laid_out(library, state, named.frame);
        OA_CHECK(fixture::named(list, "library.action.get-with") != nullptr);
        const kit::Control* first_action = nullptr;
        for (const kit::Control& control : list.controls) {
            if (control.name.starts_with("library.action.")) {
                first_action = &control;
                break;
            }
        }
        if (compact)
            OA_CHECK(first_action != nullptr && first_action->name == "library.action.get-with");
    }
}

/// An installed, updatable brain that is not the default, sees every unit,
/// and has a verdict and a homepage.
///
/// @return the Library, on Brains, with Steady Commander selected
lib::Library steady_commander_library() {
    lib::Inputs inputs;
    lib::Registry registry;
    registry.id = std::string(fixture::core);
    registry.name = "Fixture Core";
    registry.built_in = true;
    inputs.registries.push_back(std::move(registry));
    inputs.engine = {0, 8, 0};
    inputs.version_text = "v0.8.0";
    lib::Listing made = fixture::listing(
        fixture::core,
        lib::Kind::brain,
        "steady-commander",
        "Steady Commander",
        "2.0",
        1024,
        "Holds the line.",
        {"classic"}
    );
    made.homepage = "https://example.test/steady-commander";
    lib::BrainFacts facts;
    facts.tags = {"classic"};
    facts.full_awareness = true;
    facts.works_with.push_back({"*", {}, {}});
    facts.levels.push_back({"steady", "Steady", lib::BrainDifficulty::easy});
    made.brain = facts;
    lib::Installed copy = fixture::installed(made, "1.0", 1);
    lib::Verdict verdict;
    verdict.state = lib::Verdict::State::compatible;
    verdict.summary = "it fits this game";
    facts.verdict = std::move(verdict);
    copy.brain = std::move(facts);
    inputs.listings.push_back(std::move(made));
    inputs.installed.push_back(std::move(copy));
    lib::UpdateNote note;
    note.registry = std::string(fixture::core);
    note.kind = lib::Kind::brain;
    note.key = "steady-commander";
    note.to_release = 2;
    note.to_version = "2.0";
    note.to_revision = 1;
    note.size = 1024;
    inputs.updates.push_back(std::move(note));
    lib::Library library;
    lib::refresh(library, std::move(inputs));
    lib::set_tab(library, lib::Tab::brains);
    lib::select(library, {std::string(fixture::core), lib::Kind::brain, "steady-commander"});
    lib::open_details(library);
    return library;
}

/// Every action of an updatable brain is a control at each class. Compact
/// shows them on the details page, the rest inside MORE ▾. Full awareness
/// is drawn in the lock colour.
void brain_actions_and_full_awareness() {
    const lib::Library library = steady_commander_library();
    const lib::Entry* steady = lib::selected_entry(library);
    OA_CHECK(steady != nullptr);
    if (steady == nullptr)
        return;
    const std::vector<lib::ActionButton> actions = lib::entry_actions(library, *steady);
    OA_CHECK(actions.size() == 6);
    if (actions.size() != 6)
        return;
    for (const fixture::NamedFrame& named : fixture::frames()) {
        const kit::Frame& frame = named.frame;
        const bool compact = frame.size_class == kit::SizeClass::compact;
        const kit::Point window = lib::window_size(frame);
        if (!compact) {
            lib::ScreenState state;
            const kit::DisplayList list = laid_out(library, state, frame);
            OA_EXPECT_NONE(fixture::outside_window(list, window), named.name + " brain actions");
            OA_EXPECT_NONE(meeting_controls(list), named.name + " brain actions");
            OA_EXPECT_NONE(name_problems(list), named.name + " brain actions");
            OA_EXPECT_NONE(
                fixture::texts_not_fitting(list, fonts()), named.name + " brain actions"
            );
            OA_EXPECT_NONE(whole_fact_problems(library, list, true), named.name + " brain actions");
            for (const lib::ActionButton& action : actions)
                OA_CHECK(
                    fixture::named(list, "library.action." + std::string(word_of(action.action))) !=
                    nullptr
                );
            OA_CHECK(shows_lock(list, "unit on the map"));
        } else {
            lib::ScreenState page_state;
            page_state.details_page = true;
            const kit::DisplayList page = laid_out(library, page_state, frame);
            OA_EXPECT_NONE(
                fixture::outside_window(page, window), named.name + " brain details page"
            );
            OA_EXPECT_NONE(meeting_controls(page), named.name + " brain details page");
            OA_EXPECT_NONE(name_problems(page), named.name + " brain details page");
            OA_EXPECT_NONE(
                fixture::texts_not_fitting(page, fonts()), named.name + " brain details page"
            );
            OA_EXPECT_NONE(
                whole_fact_problems(library, page, false), named.name + " brain details page"
            );
            OA_CHECK(shows_lock(page, "unit on the map"));
            for (std::size_t index = 0; index < 3; ++index)
                OA_CHECK(
                    fixture::named(
                        page, "library.action." + std::string(word_of(actions[index].action))
                    ) != nullptr
                );
            OA_CHECK(fixture::named(page, "library.action") != nullptr);
            page_state.menu = lib::MenuKind::more;
            const kit::DisplayList more = laid_out(library, page_state, frame);
            for (const lib::ActionButton& action : actions)
                OA_CHECK(
                    fixture::named(more, "library.action." + std::string(word_of(action.action))) !=
                    nullptr
                );
            OA_EXPECT_NONE(
                fixture::outside_window(more, window), named.name + " brain details, MORE open"
            );
            OA_EXPECT_NONE(name_problems(more), named.name + " brain details, MORE open");
            OA_EXPECT_NONE(
                fixture::texts_not_fitting(more, fonts()), named.name + " brain details, MORE open"
            );
        }
    }
}

/// A Library of made-up brains, on the Brains tab. The built-in player is
/// the default. Wide Watch sees everything. Ridge Raider is made for Ridge.
///
/// @return the Library
lib::Library brains_library() {
    lib::Inputs inputs;
    lib::Registry built_in;
    built_in.id = "fixture-core";
    built_in.name = "Fixture Core";
    built_in.built_in = true;
    inputs.registries.push_back(built_in);
    inputs.engine = {0, 8, 0};
    inputs.version_text = "v0.8.0";
    const auto add = [&](std::string key, std::string name, lib::BrainFacts facts) {
        lib::Listing made;
        made.registry = "fixture-core";
        made.kind = lib::Kind::brain;
        made.key = std::move(key);
        made.name = std::move(name);
        made.version = "1.0";
        made.author = "Fixture Author";
        made.summary = "A made-up brain.";
        made.size = 1024;
        made.brain = facts;
        inputs.listings.push_back(std::move(made));
    };
    lib::BrainFacts wide_facts;
    wide_facts.tags = {"economy"};
    wide_facts.full_awareness = true;
    wide_facts.works_with.push_back({"*", {}, {}});
    add("wide-watch", "Wide Watch", wide_facts);
    lib::Installed wide = fixture::installed(inputs.listings.back(), "1.0", 1);
    wide.brain = wide_facts;
    inputs.installed.push_back(std::move(wide));
    lib::BrainFacts ridge_facts;
    ridge_facts.tags = {"rush"};
    ridge_facts.works_with.push_back({"ridge", {}, "Ridge"});
    add("ridge-raider", "Ridge Raider", ridge_facts);
    lib::BrainFacts steady_facts;
    steady_facts.tags = {"classic"};
    steady_facts.works_with.push_back({"*", {}, {}});
    add("steady-commander", "Steady Commander", steady_facts);
    lib::Library library;
    lib::refresh(library, std::move(inputs));
    lib::set_tab(library, lib::Tab::brains);
    return library;
}

/// Where a named brain row sits in the visible list, or -1.
///
/// @param library the Library
/// @param key the brain's key
/// @return the place
int32_t brain_place(const lib::Library& library, std::string_view key) {
    for (int32_t place = 0; place < static_cast<int32_t>(library.visible.size()); ++place) {
        const lib::Entry& entry = library.entries[library.visible[static_cast<std::size_t>(place)]];
        if (entry.id.key == key)
            return place;
    }
    return -1;
}

/// Lays the list out with one brain row at the top, and returns its look.
///
/// @param library the Library
/// @param frame the frame
/// @param key the brain's key
/// @param[out] list the display list
/// @return the row's look; null when the row is not there
const kit::ListRowLook* reveal_brain(
    const lib::Library& library,
    const kit::Frame& frame,
    std::string_view key,
    kit::DisplayList& list
) {
    lib::ScreenState state;
    state.reveal_row = brain_place(library, key);
    list = laid_out(library, state, frame);
    std::string name = "library.row.";
    if (key != lib::built_in_brain_key)
        name += "fixture-core.";
    name += std::string(key == lib::built_in_brain_key ? "built-in" : key);
    return row_look_named(list, name);
}

/// Tells whether a row's chips include a text.
///
/// @param row the row
/// @param text the chip's words
/// @return the chip; null when the row has none
const kit::ChipLook* chip_named(const kit::ListRowLook& row, std::string_view text) {
    for (const kit::ChipLook& chip : row.tags)
        if (chip.text == text)
            return &chip;
    return nullptr;
}

/// The Brains tab lays out at every frame. Its rows carry Default, Full
/// awareness and the tags at Regular and Large, and the unfit words in the
/// lock colour. Compact writes those words after the title. Tag pills and
/// the filter pane label the tags and draw no mutator icon. An unfit row
/// stays bright and its GET stays on.
void brains_tab_holds_its_invariants() {
    const lib::Library library = brains_library();
    const std::string full(lib::words::search_brains);
    for (const fixture::NamedFrame& named : fixture::frames()) {
        const kit::DisplayList list = laid_out(library, {}, named.frame);
        const kit::Point window = lib::window_size(named.frame);
        OA_EXPECT_NONE(fixture::outside_window(list, window), named.name + " brains");
        OA_EXPECT_NONE(meeting_controls(list), named.name + " brains");
        OA_EXPECT_NONE(order_problems(list), named.name + " brains");
        OA_EXPECT_NONE(name_problems(list), named.name + " brains");
        OA_EXPECT_NONE(fixture::texts_not_fitting(list, fonts()), named.name + " brains");
        const kit::Control* tab = fixture::named(list, "library.tab.brains");
        OA_CHECK(tab != nullptr && tab->text.starts_with("Brains ("));
        OA_CHECK(open_tab_is_shown(list));
        const std::string placeholder = search_placeholder(list);
        const bool whole = placeholder == full;
        const bool cut = placeholder.size() >= 3 && placeholder.ends_with("...") &&
                         full.starts_with(placeholder.substr(0, placeholder.size() - 3));
        OA_CHECK(whole || cut);
        if (named.frame.size_class != kit::SizeClass::compact)
            OA_CHECK(whole);

        const bool compact = named.frame.size_class == kit::SizeClass::compact;
        const bool regular = named.frame.size_class == kit::SizeClass::regular;
        const bool large = named.frame.size_class == kit::SizeClass::large;
        kit::DisplayList shown;
        const kit::ListRowLook* built_in =
            reveal_brain(library, named.frame, lib::built_in_brain_key, shown);
        OA_CHECK(built_in != nullptr && built_in->title == "Built-in computer player");
        if (built_in != nullptr) {
            if (compact) {
                OA_CHECK(!built_in->lines.empty() && built_in->lines.front() == "Built in");
            } else {
                OA_CHECK(
                    std::find(built_in->aside.begin(), built_in->aside.end(), "Built in") !=
                    built_in->aside.end()
                );
                const kit::ChipLook* chip = chip_named(*built_in, "Default");
                OA_CHECK(chip != nullptr && chip->state == kit::ChipState::installed);
            }
        }

        const kit::ListRowLook* wide = reveal_brain(library, named.frame, "wide-watch", shown);
        OA_CHECK(wide != nullptr);
        if (wide != nullptr && !compact) {
            const kit::ChipLook* awareness = chip_named(*wide, "Full awareness");
            const kit::ChipLook* economy = chip_named(*wide, "Economy");
            OA_CHECK(awareness != nullptr && awareness->state == kit::ChipState::update);
            OA_CHECK(economy != nullptr && economy->state == kit::ChipState::filter);
            OA_CHECK(chip_named(*wide, "Default") == nullptr);
        }

        const kit::ListRowLook* raider = reveal_brain(library, named.frame, "ridge-raider", shown);
        const kit::Control* raider_row =
            fixture::named(shown, "library.row.fixture-core.ridge-raider");
        const kit::Control* raider_get =
            fixture::named(shown, "library.row.fixture-core.ridge-raider.get");
        OA_CHECK(raider != nullptr && raider_get != nullptr && raider_get->enabled);
        OA_CHECK(raider != nullptr && raider->chip.has_value() && !raider->chip->disabled);
        OA_CHECK(raider_row != nullptr && !row_is_dimmed(shown, raider_row->rect));
        if (raider != nullptr && compact) {
            OA_CHECK(raider->by == "· Made for Ridge");
            OA_CHECK(raider->by_colour == kit::colour::lock);
        }
        if (raider != nullptr && !compact) {
            const kit::ChipLook* unfit = chip_named(*raider, "Made for Ridge");
            const kit::ChipLook* rush = chip_named(*raider, "Rush");
            OA_CHECK(unfit != nullptr && unfit->state == kit::ChipState::update);
            OA_CHECK(rush != nullptr && rush->state == kit::ChipState::filter);
        }

        if (regular) {
            const kit::PillLook* economy = pill_named(list, "library.tag.economy");
            const kit::PillLook* rush = pill_named(list, "library.tag.rush");
            OA_CHECK(economy != nullptr && economy->text == "Economy" && !economy->tag.has_value());
            OA_CHECK(rush != nullptr && rush->text == "Rush" && !rush->tag.has_value());
        }
        if (large) {
            const kit::NavLook* nav = nullptr;
            for (const kit::Item& item : list.items) {
                if (item.role != kit::Role::nav)
                    continue;
                if (const auto* look = std::get_if<kit::NavLook>(&item.look);
                    look != nullptr && !look->entries.empty())
                    nav = look;
            }
            OA_CHECK(nav != nullptr);
            if (nav != nullptr) {
                bool economy = false;
                for (const kit::NavEntry& entry : nav->entries) {
                    if (!entry.caption.starts_with("Economy"))
                        continue;
                    economy = true;
                    OA_CHECK(!entry.tag.has_value());
                }
                OA_CHECK(economy);
            }
        }
    }

    kit::Frame narrow;
    narrow.area = {0, 0, 280, 480};
    narrow.size_class = kit::SizeClass::compact;
    narrow.scale_percent = 100;
    const kit::DisplayList narrow_list = laid_out(library, {}, narrow);
    OA_CHECK(open_tab_is_shown(narrow_list));
    OA_EXPECT_NONE(fixture::texts_not_fitting(narrow_list, fonts()), "narrow brains");
}

int main() {
    every_frame_holds_its_invariants();
    mutators_tab_holds_its_invariants();
    brains_tab_holds_its_invariants();
    mutator_rows_show_badges_chips_and_icons();
    mutator_actions_rules_and_danger();
    brain_actions_and_full_awareness();
    get_with_is_a_control();
    wordy_tabs_take_the_row_over_the_search();
    larger_classes_show_more();
    built_in_languages_show_on_the_list();
    rows_name_their_entries();
    empty_and_offline_lists_say_why();
    an_own_mods_details_show_its_description();
    enter_opens_details_or_reaches_the_actions();
    escape_clears_leaves_or_closes();
    tab_commands_change_tabs();
    controller_buttons();
    list_keys_move_the_selection();
    tab_follows_the_declared_order();
    pressing_rows_selects_and_never_gets();
    the_title_bar_holds_a_close_mark();
    row_icons_are_get_and_update();
    wheel_and_drag_scroll();
    pane_scroll_bars_drag();
    typing_searches();
    filters_and_tags();
    active_tab_takes_nothing_and_scrolls();
    compact_expansions_hint_wraps();
    active_maps_fill_scroll_and_filter();
    active_base_game_is_a_tile();
    expansion_download_arrow_sits_inside_the_pill();
    locked_pill_shows_why();
    tabs_scroll_and_the_queue_shows_its_bar();
    the_base_chooser_names_its_rows();
    base_badge_is_nearest_and_tabs_are_bracketed();
    unload_mod_asks_switch_off();
    registries_tab_lists_every_registry();
    maker_marks_follow_the_registry();
    map_previews_fit_their_boxes();
    from_link_opens_the_pack();
    a_finished_refresh_shows_the_check_then_the_arrow();
    more_holds_the_rest();
    compact_back_returns_to_the_same_list();
    details_page_keeps_its_entry();
    return oa::test::check_exit_status();
}
