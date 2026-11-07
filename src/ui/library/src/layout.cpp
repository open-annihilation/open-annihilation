// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library's layout at Compact, Regular and Large, made of the UI kit's
// components: the window's face, header and footer, the section list, the
// heading and its search, the list and the details, the Active tab's setup,
// and the Registries tab's cards, every control named for automation.
// It reads the model and decides none of its rules.
#include "oa/ui/library/screen.hpp"

#include "oa/base/text/line_break.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/keys.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/library/uninstall.hpp"
#include "oa/ui/library/use_question.hpp"
#include "screen_controls.hpp"
#include "visible.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oa::ui::library {

namespace {

// The Library's own sizes, from the design; every other size is a kit metric.

/// The search field's width on the heading row, when the row has room.
constexpr int32_t wide_search_width = 240;
/// The Active tab's maps search, at the right of the MAPS heading, when the row has room.
constexpr int32_t maps_search_width = 148;
/// How many map rows the Active page keeps in view, once scrolled to them,
/// before it scrolls the tiles and the maps as one page.
constexpr int32_t maps_rows_kept = 4;
/// The list pane's width at Large, while the details keep their floor.
constexpr int32_t large_list_width = 400;
/// The side of the details' badge at Regular and Large.
constexpr int32_t details_badge_side = 40;
/// The most lines of the summary on Compact's details page.
constexpr std::size_t compact_summary_lines = 3;
/// The most lines of the summary in the details pane.
constexpr std::size_t wide_summary_lines = 6;
/// The most lines of a fact's value on Compact's details page; Rules and From take all they need.
constexpr std::size_t compact_fact_lines = 2;
/// The most lines of a fact's value in the details pane; Rules and From take all they need.
constexpr std::size_t wide_fact_lines = 3;
/// The most lines of the status line beside the details' badge.
constexpr std::size_t wide_status_lines = 2;
/// How many actions Compact's details page shows as buttons, before MORE.
constexpr std::size_t compact_action_buttons = 3;
/// The most letters a badge shows.
constexpr std::size_t badge_letter_count = 3;
/// The face behind a badge's letters when its package has no picture. The
/// kit's row draws the letters in the ink for the accent (colour::on_accent),
/// which reads on this light grey at every size class.
constexpr kit::Colour badge_face = kit::colour::hint;
/// The most bytes one word of an automation name keeps.
constexpr std::size_t most_name_word_bytes = 40;
/// The letters on the No Mod tile, and on the Mods row, when the icon is absent.
constexpr std::string_view base_badge_name = "\u2014";
/// A full progress bar, in the bar's own units.
constexpr int32_t progress_whole = 1000;
/// The prefix of every control's name.
constexpr std::string_view name_prefix = "library.";

/// The tabs, in the order they are shown.
constexpr std::array<Tab, tab_total> tab_order{
    Tab::active,
    Tab::replays,
    Tab::mods,
    Tab::maps,
    Tab::mutators,
    Tab::brains,
    Tab::keys,
    Tab::languages,
    Tab::updates,
    Tab::registries
};
/// The words the tabs are named by, in that order.
constexpr std::array<std::string_view, tab_total> tab_words{
    "active",
    "replays",
    "mods",
    "maps",
    "mutators",
    "brains",
    "keys",
    "languages",
    "updates",
    "registries"
};
/// The filters, in the order they are shown.
constexpr std::array<Filter, filter_total> filter_order{
    Filter::all, Filter::newest, Filter::installed, Filter::updates
};
/// The words the filters are named by, in that order.
constexpr std::array<std::string_view, filter_total> filter_words{
    "all", "new", "installed", "updates"
};
/// The Replays section's filters, in the order they are shown.
constexpr std::array<ReplayFilter, replay_filter_total> replay_filter_order{
    ReplayFilter::this_month, ReplayFilter::older, ReplayFilter::kept, ReplayFilter::imported
};
/// The words the Replays section's filters are named by, in that order.
constexpr std::array<std::string_view, replay_filter_total> replay_filter_words{
    "this-month", "older", "kept", "imported"
};
/// The sorts, in the order the sort menu shows them.
constexpr std::array<ReplaySort, replay_sort_total> replay_sort_order{
    ReplaySort::newest, ReplaySort::longest, ReplaySort::map, ReplaySort::players
};
/// The words the sorts are named by, in that order.
constexpr std::array<std::string_view, replay_sort_total> replay_sort_words{
    "newest", "longest", "map", "players"
};

/// Returns the tags a row or the details show.
///
/// A mutator's come from its facts. Any other kind uses its listing's tags.
///
/// @param entry the entry
/// @return the tags, or null when it has none
const std::vector<std::string>* shown_tags(const Entry& entry) {
    if (entry.id.kind == Kind::mutator)
        return mutator_tag_list(entry);
    if (entry.id.kind == Kind::brain) {
        if (const BrainFacts* facts = brain_facts(entry))
            return &facts->tags;
        return nullptr;
    }
    if (entry.id.kind == Kind::key_layout) {
        if (const KeyLayoutFacts* facts = key_facts(entry))
            return &facts->tags;
        return nullptr;
    }
    if (entry.listing != nullptr)
        return &entry.listing->tags;
    return nullptr;
}

/// Returns the words a tag shows in one place.
///
/// A mutator's chips and detail pills, and the Mutators tab's filter pills,
/// pane and menu, use the eight ids' labels. A world tag shows its world's
/// name (world_name). Every other tag is the id as written, so a mod's or a
/// map pack's own tag does not change.
///
/// @param library the Library
/// @param tag the catalogue id
/// @param labelled true for a mutator's tags, or for the Mutators tab's filters
/// @return the words; a view of `tag` when this place does not label it
std::string_view tag_caption(const Library& library, std::string_view tag, bool labelled) {
    if (world_tag(tag))
        return world_name(tag);
    if (!labelled)
        return tag;
    if (library.tab == Tab::brains)
        return brain_tag_label(tag, library.text);
    if (library.tab == Tab::keys)
        return key_tag_label(tag, library.text);
    return tag_label(tag, library.text);
}

/// Returns the word an action's button is named by.
///
/// @param action the action
/// @return the word, such as "roll-back"
std::string_view action_word(Action action) {
    switch (action) {
    case Action::get:
        return "get";
    case Action::update:
        return "update";
    case Action::update_all:
        return "update-all";
    case Action::cancel:
        return "cancel";
    case Action::retry:
        return "retry";
    case Action::roll_back:
        return "roll-back";
    case Action::play_now:
        return "play-now";
    case Action::switch_off:
        return "switch-off";
    case Action::open_folder:
        return "open-folder";
    case Action::homepage:
        return "homepage";
    case Action::details:
        return "details";
    case Action::back:
        return "back";
    case Action::check_now:
        return "refresh";
    case Action::settings:
        return "settings";
    case Action::close:
        return "close";
    case Action::add_registry:
        return "add-registry";
    case Action::add_mirror:
        return "add-mirror";
    case Action::remove_registry:
        return "remove";
    case Action::set_registry:
        return "registry";
    case Action::refresh_registry:
        return "refresh-registry";
    case Action::uninstall:
        return "uninstall";
    case Action::use_now:
        return "use-now";
    case Action::change_base_game:
        return "change";
    case Action::copy_base_game:
        return "copy";
    case Action::add_expansion:
        return "add-from";
    case Action::ask_expansion_off:
        return "turn-off";
    case Action::turn_on:
        return "turn-on";
    case Action::check_with_mod:
        return "check-with-mod";
    case Action::use_by_default:
        return "use-by-default";
    case Action::get_with:
        return "get-with";
    case Action::use_layout:
        return "use";
    case Action::edit_keys:
        return "edit-keys";
    case Action::watch_replay:
        return "watch";
    case Action::keep_replays:
        return "keep";
    case Action::open_replay_folder:
        return "open-folder";
    case Action::turn_on_recording:
        return "turn-on-recording";
    case Action::open_replay_file:
        return "open-replay-file";
    case Action::delete_replays:
        return "delete";
    case Action::confirm_delete_replays:
        return "delete-confirm";
    case Action::export_replays:
        return "export";
    case Action::choose_export_folder:
        return "choose-export-folder";
    case Action::run_export:
        return "export";
    case Action::show_export_folder:
        return "show-folder";
    case Action::export_browse_up:
        return "up";
    case Action::export_browse_open:
        return "folder";
    case Action::export_browse_use:
        return "use-folder";
    }
    return "action";
}

/// Returns a text as one word of an automation name: letters a to z and
/// digits kept, capitals made small, every other run of characters one
/// hyphen, none at either end, at most most_name_word_bytes.
///
/// @param text the text, in UTF-8
/// @return the word; "x" when nothing of the text is kept
std::string name_word(std::string_view text) {
    std::string word;
    bool hyphen = false;
    for (const char character : text) {
        const bool small = character >= 'a' && character <= 'z';
        const bool capital = character >= 'A' && character <= 'Z';
        const bool digit = character >= '0' && character <= '9';
        if (small || digit || capital) {
            if (hyphen && !word.empty())
                word += '-';
            hyphen = false;
            word += capital ? static_cast<char>(character - 'A' + 'a') : character;
        } else {
            hyphen = true;
        }
        if (word.size() >= most_name_word_bytes)
            break;
    }
    if (word.empty())
        word = "x";
    return word;
}

/// Returns the word a registry's controls are named by.
///
/// @param id the registry's id
/// @return the word; "registry" when the id keeps none
std::string registry_word(std::string_view id) {
    std::string word = name_word(id);
    if (word == "x")
        return "registry";
    return word;
}

/// Returns a name with the prefix every control's name starts with.
///
/// @param words_after the words after it, joined by dots
/// @return the name
std::string named(std::string_view words_after) {
    return std::string(name_prefix) + std::string(words_after);
}

/// The window's rows and columns at a size class.
struct Window {
    int32_t width{};   ///< the window's width, in points
    int32_t height{};  ///< the window's height, in points
    int32_t padding{}; ///< Kit 2's window padding: the dialogs over the Library keep it
    /// The shell's margin: the columns between the window's edges and its
    /// parts, and between its panes, two spaces.
    int32_t gutter{};
    int32_t left{};        ///< the content's left column
    int32_t right{};       ///< the column after the content's last
    int32_t body_top{};    ///< the first row under the header's rule
    int32_t footer_rule{}; ///< the row of the line over the footer
    int32_t footer_top{};  ///< the footer's first row
    int32_t button_top{};  ///< the footer's buttons' top row
    int32_t gap{};         ///< the rows between stacked parts
    int32_t bottom_gap{}; ///< the rows between the shell's last part and the footer's rule: a space
    /// The footer's height: Kit 2's window footer, or at Compact its controls
    /// with a space above and below, so the list keeps its room.
    int32_t footer_height{};
    int32_t button_height{}; ///< a footer's or a bar's button: Kit 2's control height
    /// The search on the heading's row: Kit 2's control height, as Settings'
    /// buttons and fields have it.
    int32_t field_height{};
    int32_t header_height{}; ///< the header's band: Kit 2's window header, Compact's at Compact
};

/// Returns the window's rows and columns for a frame, as the settings
/// dialog's frame places them.
///
/// @param frame the frame
/// @return the window
Window window_of(const kit::Frame& frame) {
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    const bool compact = frame.size_class == kit::SizeClass::compact;
    const kit::Point size = window_size(frame);
    Window window;
    window.width = size.x;
    window.height = size.y;
    // Kit 2's sizes for the shell's controls: its window header, fields,
    // controls and footer. The shell keeps two spaces round
    // its parts and between its panes, a space less than Kit 2's window
    // padding, so the lists have the room. Compact's stacked parts sit a
    // space apart and its footer holds its controls with a space above and
    // below, so the list keeps its room.
    window.padding = metrics.window_padding;
    window.gutter = 2 * metrics.space;
    window.left = window.gutter;
    window.right = size.x - metrics.edge - window.gutter;
    window.button_height = metrics.control_height;
    window.field_height = metrics.control_height;
    window.footer_height =
        compact ? metrics.control_height + 2 * metrics.space : metrics.window_footer_height;
    // Regular and Large take Kit 2's window header. Compact keeps the kit's
    // Compact band: ten points more would cost its list a row and leave the
    // Active page's lowest controls over the footer's, where no arrow
    // passes them.
    window.header_height = compact ? metrics.header_height : metrics.window_header_height;
    window.body_top = metrics.edge + window.header_height + 1;
    window.footer_rule = size.y - metrics.edge - window.footer_height - 1;
    window.footer_top = window.footer_rule + 1;
    window.button_top = window.footer_top + (window.footer_height - window.button_height) / 2;
    window.gap = compact ? metrics.space : metrics.row_gap;
    window.bottom_gap = metrics.space;
    return window;
}

/// A drop-down whose menu is open, drawn over everything else.
struct OpenMenu {
    MenuKind menu{MenuKind::none};           ///< which
    kit::Rect field{};                       ///< its field
    std::vector<controls::MenuItem> items{}; ///< its items
    int32_t chosen{-1};                      ///< the item shown as chosen, or -1
    std::string name{};                      ///< the field's control's name
};

/// What a layout is built into: the items in drawing order, and the
/// controls in groups that are tried in a fixed order.
struct Building {
    const Library* library{};      ///< the model
    const ScreenState* state{};    ///< the screen's state
    const kit::Fonts* fonts{};     ///< the fonts texts are measured in
    const kit::Metrics* metrics{}; ///< the class's metrics
    kit::SizeClass size_class{};   ///< the class
    int32_t scale_percent{100};    ///< the frame's scale, in percent; the base badge is sized to it
    kit::DisplayList list{}; ///< the items, in drawing order; its controls are moved to the groups
    std::vector<kit::Control> menu{};   ///< an open drop-down's items, tried first
    std::vector<kit::Control> top{};    ///< the tabs, the search and the filters
    std::vector<kit::Control> rows{};   ///< the list's rows that show
    std::vector<kit::Control> panes{};  ///< the list pane, tried after its rows
    std::vector<kit::Control> detail{}; ///< the details' controls and labels
    std::vector<kit::Control> footer{}; ///< the footer's buttons and label
    /// The base game chooser's controls, tried before the Library's, so a
    /// press on the dialog never reaches what it covers.
    std::vector<kit::Control> overlay{};
    std::vector<kit::ControlId> order{}; ///< Tab's order, built in the order it is declared
    /// Tab's order while the chooser is open: its rows and buttons only.
    std::vector<kit::ControlId> chooser_order{};
    /// Tab's order while USE's question is open: its choices and buttons only.
    std::vector<kit::ControlId> question_order{};
    /// Tab's order while Export is open: its choices and buttons only.
    std::vector<kit::ControlId> export_order{};
    std::unordered_set<std::string> names{}; ///< the names given so far
    /// For each name wanted, the first count after it not yet tried.
    std::unordered_map<std::string, int32_t> next_count{};
    std::optional<OpenMenu> open{}; ///< the open drop-down
};

/// Returns a name no control of the layout has yet, the name itself or with
/// "-2", "-3" and so on after it, and records it.
///
/// @param[in,out] building the layout
/// @param name the name wanted
/// @return the name given
std::string unique_name(Building& building, std::string name) {
    if (building.names.insert(name).second)
        return name;
    // Names are never taken back, so every count below the one recorded is
    // still taken.
    int32_t& count = building.next_count.try_emplace(name, 2).first->second;
    std::string given = name + "-" + std::to_string(count);
    while (!building.names.insert(given).second)
        given = name + "-" + std::to_string(++count);
    ++count;
    return given;
}

/// Returns a text's width in the layout's fonts.
///
/// @param building the layout
/// @param role the font
/// @param piece the text, in UTF-8
/// @return the width, in points
int32_t width_of(const Building& building, kit::FontRole role, std::string_view piece) {
    return kit::text_width(*building.fonts, role, piece);
}

/// Returns a text cut with "..." to a width in the layout's fonts.
///
/// @param building the layout
/// @param role the font
/// @param piece the text, in UTF-8
/// @param width the room, in points
/// @return the text, whole when it fits
std::string
cut(const Building& building, kit::FontRole role, std::string_view piece, int32_t width) {
    return kit::cut_to_width(*building.fonts, role, piece, std::max(width, int32_t{0}));
}

/// Breaks a text into lines no wider than a width, as the kit wraps.
///
/// @param building the layout
/// @param role the font
/// @param piece the text, in UTF-8
/// @param width the room, in points
/// @param most_lines the most lines kept, the last cut with "..." when more
///     would follow; 0 keeps every line
/// @return the lines
std::vector<std::string> wrapped(
    const Building& building,
    kit::FontRole role,
    std::string_view piece,
    int32_t width,
    std::size_t most_lines
) {
    const kit::Fonts* fonts = building.fonts;
    const kit::Measure measure = [fonts, role](std::string_view measured) {
        return kit::text_width(*fonts, role, measured);
    };
    kit::WrapRules rules;
    rules.newlines = true;
    rules.most_lines = most_lines;
    if (most_lines != 0) {
        const int32_t room = std::max(width, int32_t{0});
        // The line and the words dropped after it are wider than the room,
        // so the cut always ends with the kit's ellipsis.
        rules.shorten_last = [fonts, role, room](std::string_view last) {
            return kit::cut_to_width(*fonts, role, last, room);
        };
    }
    return kit::wrap(piece, std::max(width, int32_t{1}), measure, rules);
}

/// Joins lines with single spaces, as they read.
///
/// @param lines the lines
/// @return the text
std::string joined(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) {
        if (!text.empty())
            text += ' ';
        text += line;
    }
    return text;
}

/// Moves the controls a kit function appended to the list into a group.
///
/// @param[in,out] list the display list
/// @param from how many controls the list held before
/// @param[in,out] group the group they go to
void move_controls(kit::DisplayList& list, std::size_t from, std::vector<kit::Control>& group) {
    for (std::size_t index = from; index < list.controls.size(); ++index)
        group.push_back(std::move(list.controls[index]));
    list.controls.resize(from);
}

/// Appends an item.
///
/// @param[in,out] list the display list
/// @param role what it draws
/// @param rect where
/// @param clip what drawing may touch; empty for the window
/// @return the item, to fill in
kit::Item&
add_item(kit::DisplayList& list, kit::Role role, const kit::Rect& rect, const kit::Rect& clip) {
    kit::Item& added = list.items.emplace_back();
    added.role = role;
    added.rect = rect;
    added.clip = clip;
    return added;
}

/// Appends a text in a box.
///
/// @param[in,out] building the layout
/// @param role the font
/// @param drawn the text, as drawn
/// @param box the box
/// @param align where it sits along the box
/// @param ink its colour
/// @param clip what drawing may touch; empty for the window
void add_text(
    Building& building,
    kit::FontRole role,
    std::string drawn,
    const kit::Rect& box,
    kit::Align align,
    kit::Colour ink,
    const kit::Rect& clip
) {
    if (drawn.empty())
        return;
    kit::Item& added = add_item(building.list, kit::Role::text, box, clip);
    added.text = std::move(drawn);
    added.font = role;
    added.align = align;
    added.colour = ink;
}

/// Appends a label: a control of kind area that takes no press, no finger,
/// no key and no Tab, whose text is the words shown, so automation lists it
/// as a line a journey reads.
///
/// @param group the group it goes to
/// @param id its number
/// @param rect where its words lie
/// @param name its name
/// @param read the words shown
/// @param clip the part of it that shows; empty for all
/// @param scroll_group its scroll area, or -1
void add_label(
    std::vector<kit::Control>& group,
    kit::ControlId id,
    const kit::Rect& rect,
    std::string name,
    std::string read,
    const kit::Rect& clip,
    int32_t scroll_group
) {
    kit::Control label;
    label.id = id;
    label.rect = rect;
    label.clip = clip;
    label.enabled = false;
    label.focusable = false;
    label.name = std::move(name);
    label.kind = kit::ControlKind::area;
    label.group = scroll_group;
    label.text = std::move(read);
    group.push_back(std::move(label));
}

/// Adds a control to Tab's order when it takes the focus. One clipped
/// wholly out of its scroll area does not: the arrows cannot reach it,
/// and it joins the order once a scroll brings it into view.
///
/// @param[in,out] building the layout
/// @param[in,out] control the control; its focus is cleared when none shows
void tab_stop(Building& building, kit::Control& control) {
    // A tab scrolled off the strip stays in Tab's order. Its clip hides the
    // press; the arrows still move to it, and the strip follows.
    if (control.kind != kit::ControlKind::tab && control.clip.width > 0 &&
        control.clip.height > 0) {
        const kit::Rect shown = kit::intersect(control.rect, control.clip);
        if (shown.width <= 0 || shown.height <= 0)
            control.focusable = false;
    }
    if (control.enabled && control.focusable)
        building.order.push_back(control.id);
}

/// Tells whether the pointer is over a control.
///
/// @param building the layout
/// @param id the control
/// @return true while it is hovered
bool hovered(const Building& building, kit::ControlId id) {
    return building.state->interaction.hovered == id;
}

/// Tells whether a press is held on a control with the pointer over it.
///
/// @param building the layout
/// @param id the control
/// @return true while it is held
bool held(const Building& building, kit::ControlId id) {
    const kit::Interaction& interaction = building.state->interaction;
    return interaction.pressed == id && interaction.hovered == id;
}

/// Tells whether the keys' focus shows on a control.
///
/// @param building the layout
/// @param id the control
/// @return true while it has the focus
bool focused(const Building& building, kit::ControlId id) {
    const kit::Interaction& interaction = building.state->interaction;
    return interaction.focus_shown && interaction.focused == id;
}

/// Returns a button's width as Settings sizes its buttons (Cancel, OK,
/// Change layout…): its caption in the regular font with two spaces either
/// side, and at least twice the control height.
///
/// @param building the layout
/// @param caption the caption
/// @return the width, in points
int32_t button_width(const Building& building, std::string_view caption) {
    const kit::Metrics& metrics = *building.metrics;
    return std::max(
        width_of(building, kit::FontRole::regular, caption) + 4 * metrics.space,
        2 * metrics.control_height
    );
}

/// Returns a small button's width: its caption in the small font and the
/// room a question's button keeps either side, and at least OK's width at
/// Compact. The Registries tab's cards and the dialogs over the Library
/// keep these buttons.
///
/// @param building the layout
/// @param caption the caption
/// @return the width, in points
int32_t small_button_width(const Building& building, std::string_view caption) {
    const kit::Metrics& metrics = *building.metrics;
    return std::max(
        metrics.button_width,
        width_of(building, kit::FontRole::small, caption) + metrics.prompt_button_padding
    );
}

/// The gap Settings keeps between two buttons side by side: two spaces.
///
/// @param metrics the class's metrics
/// @return the columns, in points
int32_t button_spacing(const kit::Metrics& metrics) {
    return 2 * metrics.space;
}

/// Tells whether the words are drawn in the modern fonts, where Settings
/// keeps a hint's lines tall_hint_line_gap further apart.
///
/// @param building the layout
/// @return true in the modern look
bool tall_lines(const Building& building) {
    return kit::modern_look(*building.fonts);
}

/// Returns the rows from one line of small text to the next, as Settings
/// steps a hint's lines: the small font's line, and tall_hint_line_gap more
/// in the modern fonts, whose letters stand as tall as that line.
///
/// @param building the layout
/// @return the rows, in points
int32_t hint_step(const Building& building) {
    const kit::Metrics& metrics = *building.metrics;
    return metrics.small_line + (tall_lines(building) ? metrics.tall_hint_line_gap : 0);
}

/// Returns the rows lines of small text take, stepped as hint_step steps them.
///
/// @param building the layout
/// @param count how many lines
/// @return the rows, in points; 0 for none
int32_t hint_lines_height(const Building& building, std::size_t count) {
    if (count == 0)
        return 0;
    return static_cast<int32_t>(count - 1) * hint_step(building) + building.metrics->small_line;
}

/// Returns a chip's height: a tag, a state, an expansion or the pill on a
/// tile, each a word on a rounded ground with room above and below it: the
/// small font's line and half a space above and below it.
///
/// @param metrics the class's metrics
/// @return the height, in points
int32_t chip_height(const kit::Metrics& metrics) {
    return metrics.small_line + metrics.space;
}

/// Returns the columns a chip keeps either side of its words: the columns
/// Settings keeps clear beside a row's label (Metrics::label_gap).
///
/// @param metrics the class's metrics
/// @return the columns, in points
int32_t chip_side(const kit::Metrics& metrics) {
    return metrics.label_gap;
}

/// Returns the rows between two parts of a card, as Settings' cards keep
/// between their parts: two spaces.
///
/// @param metrics the class's metrics
/// @return the rows, in points
int32_t part_gap(const kit::Metrics& metrics) {
    return 2 * metrics.space;
}

/// Returns a pill's width at chip_height: its words with a chip's side
/// either side of them (chip_side), and for each icon the room the kit
/// draws it in, as far in from the pill's round end as from its top.
///
/// @param building the layout
/// @param look the pill
/// @return the width, in points
int32_t pill_width_of(const Building& building, const kit::PillLook& look) {
    const kit::Metrics& metrics = *building.metrics;
    kit::PillLook plain;
    plain.text = look.text;
    // The kit's own width with and without the icons tells what each icon
    // and its gap take; the icon's side is the kit's pill height less two.
    const int32_t icons_room =
        kit::pill_width(*building.fonts, look) - kit::pill_width(*building.fonts, plain);
    const int32_t icon_inset = (chip_height(metrics) - (kit::pill_height() - 2)) / 2;
    const int32_t icons = (look.tag.has_value() ? 1 : 0) + (look.icon.has_value() ? 1 : 0);
    return width_of(building, kit::FontRole::small, look.text) + 2 * chip_side(metrics) +
           icons_room + icons * icon_inset;
}

/// Returns a tag chip's width in a row: its state icon's side, or its words
/// with a chip's side either side of them, as the kit's row draws it with
/// the Library's spacing (row_spacing).
///
/// @param building the layout
/// @param chip the chip
/// @return the width, in points
int32_t tag_width(const Building& building, const kit::ChipLook& chip) {
    const kit::Metrics& metrics = *building.metrics;
    if (chip.icon.has_value())
        return metrics.state_icon_side;
    return width_of(building, kit::FontRole::small, chip.text) + 2 * chip_side(metrics);
}

/// Returns the spacing of a list row's lines and tags, as Settings spaces a
/// row's label and its hints: the first line hint_gap under the title, the
/// lines a hint's step apart, the tags slider_gap under them (where
/// Settings puts a control under a row's hints), each chip_height high.
///
/// @param building the layout
/// @return the spacing
kit::RowSpacing row_spacing(const Building& building) {
    const kit::Metrics& metrics = *building.metrics;
    kit::RowSpacing spacing;
    spacing.title_gap = metrics.hint_gap;
    spacing.line_step = hint_step(building);
    spacing.tags_gap = metrics.slider_gap;
    spacing.tag_height = chip_height(metrics);
    spacing.tag_padding = chip_side(metrics);
    spacing.tag_gap = metrics.chip_gap;
    return spacing;
}

/// Appends a button in a font.
///
/// @param[in,out] building the layout
/// @param group the group its control goes to
/// @param rect where it lies
/// @param caption its words, looked up
/// @param accent drawn as the main button while it is enabled
/// @param enabled it takes a press
/// @param id its number
/// @param name its name
/// @param clip the part of it that shows; empty for all
/// @param scroll_group its scroll area, or -1
/// @param font the caption's font
/// @param in_tab_order false draws it and leaves Tab's order for the caller
/// @param danger drawn in the danger look while it is enabled
void add_button_in(
    Building& building,
    std::vector<kit::Control>& group,
    const kit::Rect& rect,
    std::string_view caption,
    bool accent,
    bool enabled,
    kit::ControlId id,
    std::string name,
    const kit::Rect& clip,
    int32_t scroll_group,
    kit::FontRole font,
    bool in_tab_order,
    bool danger = false
) {
    kit::ButtonLook look;
    look.caption = cut(building, font, caption, rect.width - 2);
    look.style = danger && enabled   ? kit::ButtonStyle::danger
                 : accent && enabled ? kit::ButtonStyle::accent
                                     : kit::ButtonStyle::plain;
    look.hovered = hovered(building, id);
    look.held = held(building, id);
    look.enabled = enabled;
    look.font = font;
    const std::size_t before = building.list.controls.size();
    kit::add_button(building.list, rect, look, id, unique_name(building, std::move(name)));
    building.list.items.back().clip = clip;
    move_controls(building.list, before, group);
    group.back().clip = clip;
    group.back().group = scroll_group;
    group.back().text = std::string(caption);
    if (in_tab_order)
        tab_stop(building, group.back());
}

/// Appends one of Settings' buttons: its caption in the regular font, as
/// Settings' Cancel, OK and Change layout… have theirs. The rectangle is
/// the caller's, at the control height.
///
/// @param[in,out] building the layout
/// @param group the group its control goes to
/// @param rect where it lies
/// @param caption its words, looked up
/// @param accent drawn as the main button while it is enabled
/// @param enabled it takes a press
/// @param id its number
/// @param name its name
/// @param clip the part of it that shows; empty for all
/// @param scroll_group its scroll area, or -1
/// @param in_tab_order false draws it and leaves Tab's order for the caller
/// @param danger drawn in the danger look while it is enabled
void add_button(
    Building& building,
    std::vector<kit::Control>& group,
    const kit::Rect& rect,
    std::string_view caption,
    bool accent,
    bool enabled,
    kit::ControlId id,
    std::string name,
    const kit::Rect& clip,
    int32_t scroll_group,
    bool in_tab_order = true,
    bool danger = false
) {
    add_button_in(
        building,
        group,
        rect,
        caption,
        accent,
        enabled,
        id,
        std::move(name),
        clip,
        scroll_group,
        kit::FontRole::regular,
        in_tab_order,
        danger
    );
}

/// Appends a small button, its caption in the small font: the Registries
/// tab's cards keep these.
///
/// @param[in,out] building the layout
/// @param group the group its control goes to
/// @param rect where it lies
/// @param caption its words, looked up
/// @param enabled it takes a press
/// @param id its number
/// @param name its name
/// @param clip the part of it that shows; empty for all
/// @param scroll_group its scroll area, or -1
void add_small_button(
    Building& building,
    std::vector<kit::Control>& group,
    const kit::Rect& rect,
    std::string_view caption,
    bool enabled,
    kit::ControlId id,
    std::string name,
    const kit::Rect& clip,
    int32_t scroll_group
) {
    add_button_in(
        building,
        group,
        rect,
        caption,
        false,
        enabled,
        id,
        std::move(name),
        clip,
        scroll_group,
        kit::FontRole::small,
        true
    );
}

/// Returns the chip a state is shown with.
///
/// @param state the entry's state
/// @return the chip's state
kit::ChipState chip_state_of(State state) {
    switch (state) {
    case State::get:
        return kit::ChipState::get;
    case State::installed:
        return kit::ChipState::installed;
    case State::update:
        return kit::ChipState::update;
    case State::playing:
        return kit::ChipState::playing;
    }
    return kit::ChipState::get;
}

/// Returns the kit mark a maker shows.
///
/// @param maker the maker
/// @return the mark; none when the maker is none
std::optional<kit::MakerMark> shown_maker(PackageMaker maker) noexcept {
    switch (maker) {
    case PackageMaker::open_annihilation:
        return kit::MakerMark::open_annihilation;
    case PackageMaker::cavedog:
        return kit::MakerMark::bone;
    case PackageMaker::none:
        return std::nullopt;
    }
    return std::nullopt;
}

/// Returns a row's read text: its name, and the maker when a mark shows.
///
/// @param name the package's name
/// @param maker the maker the row shows; none adds nothing
/// @return the text
std::string maker_reading(std::string name, PackageMaker maker) {
    if (maker == PackageMaker::open_annihilation)
        name += " · Open Annihilation";
    else if (maker == PackageMaker::cavedog)
        name += " · Cavedog Entertainment";
    return name;
}

/// Returns the state icon a state is shown with: a download arrow for GET,
/// a circled up-arrow for UPDATE, and a circled check for INSTALLED and
/// PLAYING.
///
/// @param state the entry's state
/// @return the icon
kit::StateIcon icon_of(State state) {
    switch (state) {
    case State::get:
        return kit::StateIcon::download_arrow;
    case State::update:
        return kit::StateIcon::circled_up_arrow;
    case State::installed:
    case State::playing:
        return kit::StateIcon::circled_check;
    }
    return kit::StateIcon::circled_check;
}

/// Returns an entry's chip: its state's icon, dimmed when GET or UPDATE
/// cannot act, its state's word kept as its text for automation.
///
/// @param library the model
/// @param entry the entry
/// @return the chip
kit::ChipLook entry_chip(const Library& library, const Entry& entry) {
    kit::ChipLook chip;
    // A built-in language shows its words, not a download icon. The one in
    // use says so in place of USE NOW.
    if (is_built_in_language(entry.id)) {
        const bool in_use = entry.state == State::playing;
        chip.text = std::string(
            shown(library.text, in_use ? words::chip_in_use : words::registry_built_in)
        );
        chip.state = in_use ? kit::ChipState::playing : kit::ChipState::installed;
        return chip;
    }
    // A key layout says its one state in words: In use, Built in, Update,
    // Installed, or Get with the download's size. The layout in use is
    // filled with the accent, as PLAYING is.
    if (entry.id.kind == Kind::key_layout) {
        if (layout_in_use(library, entry)) {
            chip.text = std::string(shown(library.text, words::chip_in_use));
            chip.state = kit::ChipState::playing;
            return chip;
        }
        if (is_built_in_layout(entry.id)) {
            chip.text = std::string(shown(library.text, words::registry_built_in));
            chip.state = kit::ChipState::installed;
            return chip;
        }
        chip.state = chip_state_of(entry.state);
        chip.disabled = !entry.can_act;
        switch (entry.state) {
        case State::update:
            chip.text = std::string(shown(library.text, words::layout_state_update));
            break;
        case State::get:
            chip.text = entry.listing != nullptr && entry.listing->size > 0
                            ? filled(
                                  library.text,
                                  words::layout_state_get_size,
                                  {{"size", size_text(entry.listing->size, library.text)}}
                              )
                            : std::string(shown(library.text, words::layout_state_get));
            break;
        case State::installed:
        case State::playing:
            chip.text = std::string(shown(library.text, words::layout_state_installed));
            chip.disabled = false;
            break;
        }
        return chip;
    }
    // An installed language pack in use says so too, unless an update is
    // waiting: that arrow stays, and USE NOW is still withheld.
    if (entry.id.kind == Kind::language && entry.installed != nullptr && entry.installed->in_use &&
        entry.state != State::get && entry.state != State::update) {
        chip.text = std::string(shown(library.text, words::chip_in_use));
        chip.state = kit::ChipState::playing;
        return chip;
    }
    chip.text = std::string(state_text(entry.state, library.text));
    chip.state = chip_state_of(entry.state);
    chip.disabled = !entry.can_act;
    chip.icon = icon_of(entry.state);
    return chip;
}

/// Returns an entry's version as its row shows it: the installed copy's,
/// else the listing's.
///
/// @param library the model
/// @param entry the entry
/// @return the version; empty when neither names one
std::string entry_version(const Library& library, const Entry& entry) {
    if (is_built_in_brain(entry.id) || is_built_in_layout(entry.id))
        return std::string(shown(library.text, words::registry_built_in));
    if (entry.installed != nullptr)
        return version_text(*entry.installed, library.text);
    if (entry.listing != nullptr)
        return version_text(*entry.listing, library.text);
    return {};
}

/// Returns an entry's size as its row shows it: its listing's.
///
/// @param library the model
/// @param entry the entry
/// @return the size; empty without a listing
std::string entry_size(const Library& library, const Entry& entry) {
    if (entry.listing == nullptr)
        return {};
    return size_text(entry.listing->size, library.text);
}

/// Returns a badge's picture.
///
/// @param badge the badge; may be null
/// @return the picture, which points into the badge; empty without one
oa::ui::frontend_renderer::RgbaPicture picture_of(const Badge* badge) {
    oa::ui::frontend_renderer::RgbaPicture picture;
    if (badge == nullptr)
        return picture;
    picture.width = badge->width;
    picture.height = badge->height;
    picture.pixels = std::span<const uint8_t>(badge->rgba);
    return picture;
}

/// Returns a registry's icon, when the app has one.
///
/// @param registry the registry
/// @return the picture, which points into the icon; empty without one
oa::ui::frontend_renderer::RgbaPicture registry_picture(const Registry& registry) {
    if (!registry.icon)
        return {};
    return picture_of(&*registry.icon);
}

/// Returns an entry's badge picture: its own, else its listing's, else its
/// installed copy's.
///
/// @param entry the entry
/// @return the picture; empty without one
oa::ui::frontend_renderer::RgbaPicture badge_picture(const Entry& entry) {
    const Badge* badge = entry.badge;
    if (badge == nullptr && entry.listing != nullptr && entry.listing->badge)
        badge = &*entry.listing->badge;
    else if (badge == nullptr && entry.installed != nullptr && entry.installed->badge)
        badge = &*entry.installed->badge;
    return picture_of(badge);
}

/// Returns the icon a mutator's badge shows when it has no picture.
///
/// The first tag is the listing's catalogue tags when that list is not
/// empty, and otherwise the installed manifest's. An id outside the eight
/// shows no icon, and the letters stay. A later tag is not tried.
///
/// @param entry the entry
/// @return the icon, or none
std::optional<kit::TagIcon> mutator_badge_icon(const Entry& entry) {
    if (entry.id.kind != Kind::mutator)
        return std::nullopt;
    const std::vector<std::string>* tags = nullptr;
    if (entry.listing != nullptr && entry.listing->mutator && !entry.listing->mutator->tags.empty())
        tags = &entry.listing->mutator->tags;
    else if (
        entry.installed != nullptr && entry.installed->mutator &&
        !entry.installed->mutator->tags.empty()
    )
        tags = &entry.installed->mutator->tags;
    if (tags == nullptr)
        return std::nullopt;
    return kit::tag_icon_of(tags->front());
}

/// Returns a row's name: library.row., then its registry's word and its
/// key's, or its key's alone for a package of the player's own.
///
/// @param entry the entry
/// @return the name
std::string row_name(const Entry& entry) {
    std::string name = named("row.");
    if (!entry.id.registry.empty())
        name += name_word(entry.id.registry) + ".";
    name += name_word(entry.id.key);
    return name;
}

/// How a row is shown: Compact's short row, Regular's or Large's.
enum class RowForm : uint8_t {
    compact, ///< the name, then its version and size; Kit 2's row height
    regular, ///< the name and byline, then the summary; the version over the size; a key
             ///< layout's chips on a third line
    large,   ///< as regular, with the tags as chips on a third line
};

/// Returns the rows a list row keeps above and below its words: Settings'
/// row padding, and a space at Compact, whose short list keeps its rows.
///
/// @param building the layout
/// @return the rows, in points
int32_t row_padding_of(const Building& building) {
    const kit::Metrics& metrics = *building.metrics;
    return building.size_class == kit::SizeClass::compact ? metrics.space : metrics.row_padding;
}

/// Returns a row's height in a form: Kit 2's row height (Metrics::row_height),
/// and at least the lines the form holds, spaced as Settings spaces a row's
/// label and hint (row_spacing), with the row's padding above and below.
///
/// @param building the layout
/// @param form the form
/// @return the height, in points
int32_t row_height(const Building& building, RowForm form) {
    const kit::Metrics& metrics = *building.metrics;
    const kit::RowSpacing spacing = row_spacing(building);
    // The title's line and a line under it, the gap between them, and the
    // padding above and below the words.
    const int32_t two_lines = metrics.regular_line + spacing.title_gap + metrics.small_line +
                              2 * row_padding_of(building);
    switch (form) {
    case RowForm::compact:
        return std::max({metrics.short_list_row_height, metrics.row_height, two_lines});
    case RowForm::regular:
        return std::max(two_lines, metrics.row_height);
    case RowForm::large:
        // The title's line, a line under it and the tags under that.
        return std::max(two_lines + spacing.tags_gap + spacing.tag_height, metrics.row_height);
    }
    return metrics.row_height;
}

/// Returns the columns a row's chip line may use, as the kit's row draws it.
///
/// The kit lays every row out with Compact's inset, badge and gap, including
/// a taller Regular or Large row. The state's icon, or the maker's mark when
/// the row has no icon, and the version and size, take their columns from
/// the right. What remains is the chip line.
///
/// @param building the layout
/// @param look the row, with its icon, mark and aside already set
/// @param row_width the row's width, in points
/// @param row_height the row's height, in points
/// @return the columns, in points
int32_t chip_line_room(
    const Building& building, const kit::ListRowLook& look, int32_t row_width, int32_t row_height
) {
    const kit::Metrics& metrics = kit::compact_metrics;
    const int32_t inset = metrics.list_row_inset;
    const int32_t side =
        std::max(std::min(row_height - 2 * inset, metrics.list_row_badge), int32_t{0});
    const int32_t text_left = inset + side + inset;
    int32_t right = row_width - inset;
    const bool stacked = look.maker.has_value() && look.chip.has_value() &&
                         look.chip->icon.has_value() && row_height >= 2 * metrics.state_icon_side;
    if (look.chip.has_value()) {
        const int32_t used = stacked || look.chip->icon.has_value()
                                 ? metrics.state_icon_side
                                 : tag_width(building, *look.chip);
        right = row_width - inset - used - metrics.list_row_gap;
    } else if (look.maker.has_value()) {
        right = row_width - inset - metrics.state_icon_side - metrics.list_row_gap;
    }
    if (!look.aside.empty()) {
        int32_t aside_width = 0;
        for (const std::string& aside_line : look.aside)
            aside_width =
                std::max(aside_width, width_of(building, kit::FontRole::small, aside_line));
        aside_width = std::min(aside_width, std::max(right - text_left, int32_t{0}));
        right -= aside_width + metrics.list_row_gap;
    }
    return std::max(right - text_left, int32_t{0});
}

/// Tells whether a chip line ends at or before a column, as the kit draws it.
///
/// The kit stops at the first chip that would pass the column, so a line
/// fits only when every chip does.
///
/// @param building the layout
/// @param chips the chips, in the order they would be drawn
/// @param room the columns the line may use
/// @return true when every chip fits
bool chips_fit(const Building& building, const std::vector<kit::ChipLook>& chips, int32_t room) {
    const int32_t gap = row_spacing(building).tag_gap;
    int32_t left = 0;
    for (const kit::ChipLook& chip : chips) {
        const int32_t width = tag_width(building, chip);
        if (left + width > room)
            return false;
        left += width + gap;
    }
    return true;
}

/// Returns how many lines a chip row takes when a chip that does not fit wraps.
///
/// A chip wider than the line takes a line of its own and is still counted,
/// so the row can grow for it. No chips take no line.
///
/// @param building the layout
/// @param chips the chips, in the order they are drawn
/// @param room the columns one line may use
/// @return the lines
int32_t
chip_lines_in(const Building& building, const std::vector<kit::ChipLook>& chips, int32_t room) {
    if (chips.empty())
        return 0;
    const int32_t gap = row_spacing(building).tag_gap;
    int32_t lines = 1;
    int32_t left = 0;
    for (const kit::ChipLook& chip : chips) {
        const int32_t width = tag_width(building, chip);
        if (left > 0 && left + width > room) {
            ++lines;
            left = 0;
        }
        left += width + gap;
    }
    return lines;
}

/// Returns a mutator's state chip, drawn in the lock colour.
///
/// @param text the chip's words, looked up
/// @return the chip
kit::ChipLook state_chip(std::string text) {
    kit::ChipLook chip;
    chip.text = std::move(text);
    chip.state = kit::ChipState::update;
    return chip;
}

/// Returns a tag chip: quiet, outlined, its words in the hint colour.
///
/// @param text the chip's words, looked up
/// @return the chip
kit::ChipLook tag_chip(std::string text) {
    kit::ChipLook chip;
    chip.text = std::move(text);
    chip.state = kit::ChipState::tag;
    return chip;
}

/// Joins two pieces with a space. An empty piece is left out.
///
/// @param left the piece on the left
/// @param right the piece on the right
/// @return the joined text
std::string joined_line(std::string_view left, std::string_view right) {
    if (left.empty())
        return std::string(right);
    if (right.empty())
        return std::string(left);
    std::string text;
    text.reserve(left.size() + right.size() + 1);
    text.append(left);
    text.push_back(' ');
    text.append(right);
    return text;
}

/// Returns a Compact key layout's one line under its name.
///
/// The summary stays. "Made for {mod}" and the own-changes count share that
/// line. A tag's word follows only while the line still holds it, and the
/// tags are the ones left off, as a chip line drops them.
///
/// @param building the layout
/// @param summary the summary; empty when the row has none
/// @param made "Made for {mod}"; empty when the layout is not made for this mod
/// @param own the own-changes line; empty when the player has none
/// @param tags the tag words, in the order they would be drawn
/// @param room the columns the line may use
/// @return the line
std::string compact_key_line(
    const Building& building,
    std::string summary,
    std::string_view made,
    std::string_view own,
    const std::vector<std::string>& tags,
    int32_t room
) {
    const auto text_width = [&](std::string_view text) {
        return width_of(building, kit::FontRole::small, text);
    };
    const std::string notes = joined_line(made, own);
    std::string line = std::move(summary);
    if (!notes.empty()) {
        const int32_t notes_width = text_width(std::string(" ") + notes);
        if (line.empty() || notes_width >= room) {
            line = notes;
        } else {
            const int32_t head_room = room - notes_width;
            if (text_width(line) > head_room)
                line = cut(building, kit::FontRole::small, line, head_room);
            const std::string both = joined_line(line, notes);
            line = text_width(both) <= room ? both : notes;
        }
    }
    for (const std::string& tag : tags) {
        const std::string trial = joined_line(line, tag);
        if (text_width(trial) > room)
            break;
        line = trial;
    }
    return line;
}

/// Returns what an entry's row shows.
///
/// @param building the layout
/// @param entry the entry
/// @param form the row's form
/// @param row_width the row's width, in points; a Large row's chips are kept to it
/// @param row_height the row's height, in points
/// @return the row's look
kit::ListRowLook row_look(
    const Building& building,
    const Entry& entry,
    RowForm form,
    int32_t row_width,
    int32_t row_height
) {
    const Library& library = *building.library;
    kit::ListRowLook look;
    look.badge = badge_picture(entry);
    look.badge_text =
        is_base_game(entry.id) ? std::string(base_badge_name) : badge_letters(entry_name(entry));
    look.badge_colour = badge_face;
    // A mutator with no picture wears its first known tag's icon. The letters
    // stay for a tag outside the eight, and for a mutator with no tag.
    if (entry.id.kind == Kind::mutator && !oa::ui::frontend_renderer::picture_drawable(look.badge))
        look.badge_tag = mutator_badge_icon(entry);
    look.title = std::string(entry_name(entry));
    look.chip = entry_chip(library, entry);
    look.maker = shown_maker(entry_maker(entry));
    const std::string version = entry_version(library, entry);
    const std::string size = entry_size(library, entry);
    const ContentLines lines = content_lines(library, entry);
    const bool named = !lines.row.empty();
    if (form == RowForm::compact) {
        // The version stays on the row's second line. A missing mutator, or
        // one that does not fit this mod, is named after the title.
        if (entry.id.kind == Kind::mutator && !entry.needs_missing.empty()) {
            look.by = filled(library.text, words::row_needs, {{"name", entry.needs_missing}});
            look.by_colour = kit::colour::lock;
        } else if (entry.id.kind == Kind::mutator && entry.not_compatible) {
            look.by = std::string(shown(library.text, words::row_not_compatible));
            look.by_colour = kit::colour::lock;
        } else if (entry.id.kind == Kind::brain && entry.not_compatible) {
            if (const BrainFacts* facts = brain_facts(entry)) {
                const std::string reason = brain_unfit_reason(library.inputs, *facts, library.text);
                if (!reason.empty()) {
                    look.by = filled(library.text, words::row_brain_unfit, {{"reason", reason}});
                    look.by_colour = kit::colour::lock;
                }
            }
        }
        if (named && entry.id.kind != Kind::key_layout) {
            look.lines.push_back(lines.row);
            return look;
        }
        std::string line = version;
        if (is_built_in_language(entry.id)) {
            const std::string_view english = entry_summary(entry);
            if (!english.empty())
                line = std::string(english);
        } else if (!version.empty() && !size.empty()) {
            line = filled(
                library.text, words::row_version_size, {{"version", version}, {"size", size}}
            );
        } else if (version.empty()) {
            line = size;
        }
        if (entry.not_reviewed && !entry.registry_name.empty())
            line = filled(
                library.text,
                words::row_from_registry,
                {{"line", line}, {"registry", entry.registry_name}}
            );
        if (entry.id.kind == Kind::key_layout) {
            // One line under the name: the summary, then Made for and the
            // own-changes count on that same line. The row is not tall
            // enough for a chip line, so a tag's word follows only while
            // the line holds it.
            std::string about;
            if (named)
                about = lines.row;
            else
                about = std::string(entry_summary(entry));
            if (about.empty())
                about = line;
            const std::string made = layout_made_for_line(library, entry);
            const KeyLayoutFacts* facts = key_facts(entry);
            const std::string own =
                own_changes_line(facts != nullptr ? facts->own_changes : 0, library.text);
            std::vector<std::string> tag_words;
            if (const std::vector<std::string>* tags = shown_tags(entry)) {
                tag_words.reserve(tags->size());
                for (const std::string& tag : *tags)
                    tag_words.emplace_back(tag_caption(library, tag, true));
            }
            const int32_t room = chip_line_room(building, look, row_width, row_height);
            line = compact_key_line(building, std::move(about), made, own, tag_words, room);
        }
        look.lines.push_back(std::move(line));
        return look;
    }
    look.by = byline(entry, library.text);
    // A key layout installed from its own file has no listing: its row says
    // what it does and who made it from its manifest, as Compact's line does.
    if (entry.id.kind == Kind::key_layout && look.by.empty() && entry.installed != nullptr &&
        !entry.installed->author.empty() && !is_built_in_layout(entry.id))
        look.by = filled(library.text, words::byline, {{"author", entry.installed->author}});
    if (named)
        look.lines.push_back(lines.row);
    else if (entry.listing != nullptr && !entry.listing->summary.empty())
        look.lines.push_back(entry.listing->summary);
    else if (entry.id.kind == Kind::key_layout && !entry_summary(entry).empty())
        look.lines.push_back(std::string(entry_summary(entry)));
    else if (
        is_base_game(entry.id) || is_built_in_language(entry.id) || is_built_in_brain(entry.id) ||
        is_built_in_layout(entry.id)
    ) {
        const std::string_view about = entry_summary(entry);
        if (!about.empty())
            look.lines.push_back(std::string(about));
    }
    // A key layout's row says what it does and its one state; its version
    // and size are in its details. OA default's chip says Built in, as a
    // built-in language's does.
    if (entry.id.kind != Kind::key_layout) {
        if (!version.empty())
            look.aside.push_back(version);
        if (!size.empty())
            look.aside.push_back(size);
    }
    if (form == RowForm::large && entry.id.kind == Kind::brain) {
        // Default, then Full awareness, then the tags, then why it does not
        // fit. When the line cannot hold them, the tags are the ones dropped.
        std::vector<kit::ChipLook> kept_chips;
        if (entry.is_default) {
            kit::ChipLook chip;
            chip.text = std::string(shown(library.text, words::chip_default));
            chip.state = kit::ChipState::installed;
            kept_chips.push_back(std::move(chip));
        }
        const BrainFacts* facts = brain_facts(entry);
        if (facts != nullptr && facts->full_awareness) {
            kit::ChipLook chip;
            chip.text = std::string(shown(library.text, words::chip_full_awareness));
            chip.state = kit::ChipState::update;
            kept_chips.push_back(std::move(chip));
        }
        std::vector<kit::ChipLook> labels;
        if (facts != nullptr) {
            for (const std::string& tag : facts->tags)
                labels.push_back(tag_chip(std::string(brain_tag_label(tag, library.text))));
        }
        std::vector<kit::ChipLook> unfit;
        if (entry.not_compatible && facts != nullptr) {
            const std::string reason = brain_unfit_reason(library.inputs, *facts, library.text);
            if (!reason.empty())
                unfit.push_back(state_chip(reason));
        }
        std::vector<kit::ChipLook> preferred = kept_chips;
        preferred.insert(preferred.end(), labels.begin(), labels.end());
        preferred.insert(preferred.end(), unfit.begin(), unfit.end());
        const int32_t room = chip_line_room(building, look, row_width, row_height);
        if (chips_fit(building, preferred, room)) {
            look.tags = std::move(preferred);
        } else {
            look.tags = std::move(kept_chips);
            look.tags.insert(look.tags.end(), unfit.begin(), unfit.end());
            look.tags.insert(look.tags.end(), labels.begin(), labels.end());
        }
    } else if (entry.id.kind == Kind::key_layout) {
        // The summary is the line under the name. Made for, the own-changes
        // count and the tags are chips under that, at Regular and Large.
        // Made for is this chip and not a line as well. A chip that does not
        // fit the line wraps under it, so the order stays and none is dropped.
        const std::string made = layout_made_for_line(library, entry);
        std::string own;
        if (const KeyLayoutFacts* facts = key_facts(entry);
            facts != nullptr && facts->own_changes > 0)
            own = facts->own_changes == 1 ? std::string(shown(library.text, words::own_count_one))
                                          : filled(
                                                library.text,
                                                words::own_count_many,
                                                {{"count", std::to_string(facts->own_changes)}}
                                            );
        std::vector<kit::ChipLook> kept;
        if (!made.empty())
            kept.push_back(tag_chip(made));
        if (!own.empty())
            kept.push_back(state_chip(std::move(own)));
        std::vector<kit::ChipLook> labels;
        if (const std::vector<std::string>* tags = shown_tags(entry)) {
            labels.reserve(tags->size());
            for (const std::string& tag : *tags)
                labels.push_back(tag_chip(std::string(tag_caption(library, tag, true))));
        }
        kept.insert(kept.end(), labels.begin(), labels.end());
        look.tags = std::move(kept);
    } else if (form == RowForm::large) {
        std::vector<kit::ChipLook> labels;
        if (const std::vector<std::string>* tags = shown_tags(entry)) {
            for (const std::string& tag : *tags)
                labels.push_back(
                    tag_chip(std::string(tag_caption(library, tag, entry.id.kind == Kind::mutator)))
                );
        }
        if (entry.id.kind != Kind::mutator) {
            look.tags = std::move(labels);
        } else {
            // Needs, then the note that it does not fit this mod. Both are
            // information in the lock colour. When the row cannot hold the
            // tags and these, the two stay and the tags are the ones dropped.
            std::vector<kit::ChipLook> states;
            if (!entry.needs_missing.empty())
                states.push_back(state_chip(
                    filled(library.text, words::chip_needs, {{"name", entry.needs_missing}})
                ));
            if (entry.not_compatible)
                states.push_back(
                    state_chip(std::string(shown(library.text, words::chip_not_compatible)))
                );
            std::vector<kit::ChipLook> tags_first = labels;
            tags_first.insert(tags_first.end(), states.begin(), states.end());
            const int32_t room = chip_line_room(building, look, row_width, row_height);
            if (chips_fit(building, tags_first, room)) {
                look.tags = std::move(tags_first);
            } else {
                look.tags = std::move(states);
                look.tags.insert(look.tags.end(), labels.begin(), labels.end());
            }
        }
    }
    return look;
}

/// Appends the header: the OA mark, the title and its second word, and the
/// version at the right. right_inset keeps columns clear inside the padding
/// for the refresh icon, the close mark and, when the last refresh failed,
/// the offline mark.
///
/// @param[in,out] building the layout
/// @param window the window
/// @param title the title, cut to fit when it is an entry's name
/// @param second the word after the title; empty for none
/// @param right the version at the right, cut to fit
/// @param right_inset the columns kept clear inside the padding
void add_header(
    Building& building,
    const Window& window,
    std::string_view title,
    std::string_view second,
    std::string_view right,
    int32_t right_inset
) {
    const kit::Metrics& compact = kit::compact_metrics;
    const int32_t tracking = building.metrics->heading_tracking;
    // The kit's header keeps Compact's places from the window's edges.
    const int32_t title_left = compact.padding + compact.mark_side + compact.header_gap;
    const int32_t right_edge = window.width - compact.edge - compact.padding - right_inset;
    const int32_t second_width =
        second.empty()
            ? 0
            : kit::tracked_width(*building.fonts, kit::FontRole::regular, second, tracking);
    const int32_t second_room = second.empty() ? 0 : compact.header_gap + second_width;
    const int32_t right_width = width_of(building, kit::FontRole::small, right);
    const int32_t title_room = std::max(
        right_edge - title_left - second_room - compact.header_gap - right_width, int32_t{0}
    );

    kit::HeaderLook look;
    look.width = window.width;
    look.height = window.header_height;
    look.tracking = tracking;
    look.right_inset = right_inset;
    look.title = std::string(title);
    if (kit::tracked_width(*building.fonts, kit::FontRole::regular, title, tracking) > title_room)
        look.title =
            cut(building,
                kit::FontRole::regular,
                title,
                title_room - tracking * static_cast<int32_t>(kit::character_count(title)));
    look.title_width =
        kit::tracked_width(*building.fonts, kit::FontRole::regular, look.title, tracking);
    look.second = std::string(second);
    look.second_width = second_width;
    const int32_t version_room = std::max(
        right_edge - (title_left + look.title_width + second_room + compact.header_gap), int32_t{0}
    );
    look.version = cut(building, kit::FontRole::small, right, version_room);
    look.version_width = width_of(building, kit::FontRole::small, look.version);
    const int32_t header_rule = compact.edge + window.header_height;
    add_item(building.list, kit::Role::header, {0, 0, window.width, header_rule + 1}, {}).look =
        std::move(look);
}

/// Returns the header's close mark, the right-hand square.
///
/// @param window the window
/// @return the button
kit::Rect header_close_rect(const Window& window) {
    const kit::Metrics& compact = kit::compact_metrics;
    const int32_t side = compact.button_height;
    return {
        window.width - compact.edge - compact.padding - side,
        compact.edge + (window.header_height - side) / 2,
        side,
        side,
    };
}

/// Returns the header's refresh icon, the same square, one gap left of the
/// close mark.
///
/// @param window the window
/// @return the button
kit::Rect header_refresh_rect(const Window& window) {
    const kit::Rect close = header_close_rect(window);
    const int32_t gap = kit::compact_metrics.header_gap;
    return {close.x - gap - close.width, close.y, close.width, close.height};
}

/// Returns the columns the header keeps clear for the refresh icon, the
/// close mark and, when the last refresh could not reach a registry, the
/// offline mark.
///
/// @param building the layout
/// @return the columns, inside the header's padding
int32_t header_reserve(const Building& building) {
    const kit::Metrics& compact = kit::compact_metrics;
    int32_t reserve = 2 * (compact.button_height + compact.header_gap);
    if (building.library->inputs.offline) {
        const std::string_view mark = shown(building.library->text, words::offline_mark);
        reserve += width_of(building, kit::FontRole::small, mark) + compact.header_gap;
    }
    return reserve;
}

/// Returns how long a registry's refresh mark has been showing its check.
///
/// @param state the marks
/// @param id the registry
/// @return the milliseconds; 0 while it shows the arrow or is turning
uint16_t registry_check_ms(const ScreenState& state, std::string_view id) noexcept {
    for (const ScreenState::RegistryMark& mark : state.registry_marks)
        if (mark.id == id)
            return mark.check_ms;
    return 0;
}

/// Appends the header's refresh icon, at the top right, the same square at
/// every size class. Its words are "Refresh", or "Refreshing" while the
/// registries are being asked. It can be pressed only when a registry can
/// be asked, including while the check shows.
///
/// @param[in,out] building the layout
/// @param window the window
void add_refresh_button(Building& building, const Window& window) {
    const Library& library = *building.library;
    const kit::Rect rect = header_refresh_rect(window);
    const bool turning = library.inputs.checking;
    kit::ButtonLook look;
    look.style = kit::ButtonStyle::quiet;
    look.hovered = hovered(building, controls::refresh);
    look.held = held(building, controls::refresh);
    look.enabled = can_check_now(library);
    look.mark = kit::ButtonMark::refresh;
    look.turn = building.state->refresh_turn;
    look.turning = turning;
    look.check_ms = turning ? uint16_t{0} : building.state->refresh_check_ms;
    const std::size_t before = building.list.controls.size();
    kit::add_button(
        building.list, rect, look, controls::refresh, unique_name(building, named("refresh"))
    );
    move_controls(building.list, before, building.top);
    building.top.back().text = std::string(
        shown(library.text, turning ? words::action_refreshing : words::action_refresh)
    );
    tab_stop(building, building.top.back());
}

/// Appends the header's close mark, the same square as the refresh icon and
/// one gap to its right. Pressing it asks for the same close as CLOSE.
///
/// @param[in,out] building the layout
/// @param window the window
void add_close_button(Building& building, const Window& window) {
    const Library& library = *building.library;
    const kit::Rect rect = header_close_rect(window);
    kit::ButtonLook look;
    look.style = kit::ButtonStyle::quiet;
    look.hovered = hovered(building, controls::close_x);
    look.held = held(building, controls::close_x);
    look.mark = kit::ButtonMark::close;
    const std::size_t before = building.list.controls.size();
    kit::add_button(
        building.list, rect, look, controls::close_x, unique_name(building, named("close-x"))
    );
    move_controls(building.list, before, building.top);
    building.top.back().text = std::string(shown(library.text, words::action_close_x));
    // Tab's order gains it in finish, after every other stop, so it stays
    // last in the title bar (the refresh icon, when it takes focus, is earlier).
}

/// Appends the word "offline" in the lock colour, immediately left of the
/// refresh icon, when the last refresh could not reach a registry.
///
/// @param[in,out] building the layout
/// @param window the window
void add_offline_mark(Building& building, const Window& window) {
    const Library& library = *building.library;
    if (!library.inputs.offline)
        return;
    const kit::Metrics& compact = kit::compact_metrics;
    const std::string mark(shown(library.text, words::offline_mark));
    const int32_t mark_width = width_of(building, kit::FontRole::small, mark);
    const int32_t icon_left = header_refresh_rect(window).x;
    const kit::Rect box{
        icon_left - compact.header_gap - mark_width,
        compact.edge,
        std::max(mark_width, int32_t{0}),
        window.header_height,
    };
    add_text(building, kit::FontRole::small, mark, box, kit::Align::right, kit::colour::lock, {});
}

/// Appends the header, the offline mark, the refresh icon and the close mark.
///
/// @param[in,out] building the layout
/// @param window the window
/// @param title the title
/// @param second the word after the title; empty for none
/// @param version the version at the right
void add_library_header(
    Building& building,
    const Window& window,
    std::string_view title,
    std::string_view second,
    std::string_view version
) {
    add_header(building, window, title, second, version, header_reserve(building));
    add_offline_mark(building, window);
    add_refresh_button(building, window);
    add_close_button(building, window);
}

/// Returns a tab's title, with its count in brackets after every tab but Active.
///
/// @param library the model
/// @param tab the tab
/// @param compact true uses Compact's short name, where the tab has one
/// @return the title, such as "Mods (13)" or "Active"
std::string tab_caption(const Library& library, Tab tab, bool compact) {
    const std::string title(tab_text(tab, compact, library.text));
    if (tab == Tab::active)
        return title;
    return filled(
        library.text,
        words::tab_with_count,
        {{"tab", title}, {"count", std::to_string(tab_count(library, tab))}}
    );
}

/// Returns the search field's words while it is empty, for the open tab.
///
/// @param library the model
/// @return the words, looked up; none on Active and Registries, which have no search
std::string_view placeholder_of(const Library& library) {
    switch (library.tab) {
    case Tab::active:
    case Tab::registries:
        return {};
    case Tab::mods:
        return shown(library.text, words::search_mods);
    case Tab::maps:
        return shown(library.text, words::search_maps);
    case Tab::mutators:
        return shown(library.text, words::search_mutators);
    case Tab::brains:
        return shown(library.text, words::search_brains);
    case Tab::keys:
        return shown(library.text, words::search_keys);
    case Tab::languages:
        return shown(library.text, words::search_languages);
    case Tab::updates:
        return shown(library.text, words::search_updates);
    case Tab::replays:
        return shown(library.text, words::search_replays);
    }
    return {};
}

/// Appends the search field.
///
/// @param[in,out] building the layout
/// @param rect the field
void add_search(Building& building, const kit::Rect& rect) {
    kit::SearchLook look;
    look.field = building.state->search;
    look.focused = focused(building, controls::search);
    look.hovered = hovered(building, controls::search);
    look.placeholder =
        cut(building,
            kit::FontRole::regular,
            placeholder_of(*building.library),
            kit::field_text_rect(rect, look).width);
    const std::size_t before = building.list.controls.size();
    kit::add_search(
        building.list, rect, look, controls::search, unique_name(building, named("search"))
    );
    move_controls(building.list, before, building.top);
    building.top.back().label = std::string(placeholder_of(*building.library));
    tab_stop(building, building.top.back());
}

/// Appends a drop-down's field and, while it is open, keeps its menu to
/// draw over everything else.
///
/// @param[in,out] building the layout
/// @param group the group its control goes to
/// @param rect the field
/// @param menu which drop-down
/// @param caption the words the field shows
/// @param items its items
/// @param id its control's number
/// @param name its control's name
/// @param wide_menu true opens the menu as wide as its widest item, wider than
///     its field when it needs to be, as far as the window's right edge
void add_drop_down(
    Building& building,
    std::vector<kit::Control>& group,
    const kit::Rect& rect,
    MenuKind menu,
    std::string_view caption,
    std::vector<controls::MenuItem> items,
    kit::ControlId id,
    std::string name,
    bool wide_menu = false
) {
    const kit::Metrics& metrics = *building.metrics;
    kit::ChoiceLook look;
    look.text =
        cut(building,
            kit::FontRole::regular,
            caption,
            rect.width - metrics.choice_text_inset - metrics.choice_arrow_room);
    look.hovered = hovered(building, id) || building.state->interaction.pressed == id;
    look.open = building.state->menu == menu;
    const std::string given = unique_name(building, std::move(name));
    const std::size_t before = building.list.controls.size();
    kit::add_choice(building.list, rect, look, id, given);
    move_controls(building.list, before, group);
    kit::Control& added = group.back();
    // The arrows move the focus off a drop-down; Space and Enter open it.
    added.steps = false;
    for (const controls::MenuItem& item : items)
        added.parts.push_back(item.word);
    tab_stop(building, added);
    if (look.open) {
        OpenMenu opened;
        opened.menu = menu;
        opened.field = rect;
        if (wide_menu) {
            // The items are whole: the menu takes the widest one's room.
            int32_t widest = 0;
            for (const controls::MenuItem& item : items)
                widest = std::max(widest, width_of(building, kit::FontRole::regular, item.caption));
            const int32_t needed =
                widest + metrics.choice_item_text_inset + metrics.list_row_inset + 2;
            const int32_t window_width =
                building.list.items.empty() ? 0 : building.list.items.front().rect.width;
            const int32_t room = std::max(window_width - metrics.edge - rect.x, rect.width);
            opened.field.width = std::max(rect.width, std::min(needed, room));
        }
        opened.chosen = controls::chosen_item(*building.library, menu, items);
        opened.items = std::move(items);
        opened.name = given;
        building.open = std::move(opened);
    }
}

/// Returns the width a drop-down's field needs for its caption and, open,
/// for its widest item.
///
/// @param building the layout
/// @param caption the field's words
/// @param items its items
/// @return the width, in points
int32_t drop_down_width(
    const Building& building, std::string_view caption, const std::vector<controls::MenuItem>& items
) {
    const kit::Metrics& metrics = *building.metrics;
    int32_t width = width_of(building, kit::FontRole::regular, caption) +
                    metrics.choice_text_inset + metrics.choice_arrow_room;
    for (const controls::MenuItem& item : items)
        width = std::max(
            width,
            width_of(building, kit::FontRole::regular, item.caption) +
                metrics.choice_item_text_inset + metrics.choice_arrow_room
        );
    return width;
}

/// Appends the Updates tab's line over its list: "{n} updates" at its
/// left, counting the updates the list shows, and UPDATE ALL at its right.
///
/// @param[in,out] building the layout
/// @param row the row
void add_updates_line(Building& building, const kit::Rect& row) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    const std::string_view caption = action_text(Action::update_all, library.text);
    const int32_t width = std::min(button_width(building, caption), row.width);
    const kit::Rect button{row.x + row.width - width, row.y, width, row.height};
    const std::size_t shown_count = updates_shown(library);
    const std::string count = filled(
        library.text,
        shown_count == 1 ? words::update_count_one : words::update_count_many,
        {{"count", std::to_string(shown_count)}}
    );
    const int32_t count_room = std::max(button.x - metrics.label_gap - row.x, int32_t{0});
    const kit::Rect words_box{row.x, row.y, count_room, row.height};
    add_text(
        building,
        kit::FontRole::small,
        cut(building, kit::FontRole::small, count, count_room),
        words_box,
        kit::Align::left,
        kit::colour::hint,
        {}
    );
    add_button(
        building,
        building.top,
        button,
        caption,
        false,
        update_count(library) > 0,
        controls::update_all,
        named("action.update-all"),
        {},
        -1
    );
}

/// The shapes the Replays section draws as pictures of its own: the section's
/// film frame, a kept replay's star and a player's colour dot.
enum class ReplayIcon : uint8_t {
    film, ///< a strip of film, the section's icon
    star, ///< a five-pointed star, a kept replay
    dot,  ///< a round dot, a player's colour
    edge, ///< the ring round a dot's rim, light, so that a dark colour shows on a dark ground
};

/// Appends one of the Replays section's icons; its definition is with the
/// section's layout.
void add_replay_icon(
    Building& building,
    ReplayIcon icon,
    const kit::Rect& rect,
    kit::Colour ink,
    const kit::Rect& clip
);

/// Settings' section list, copied from the settings dialog's NavMetrics.
/// The Library does not include that module. Compact, Regular, then Large:
/// an entry's height, the gap under it, the rows above and below a divider,
/// the rows kept above the first entry, and the icon's column, side and the
/// caption's column.
struct SectionNav {
    int32_t entry_height{};
    int32_t entry_gap{};
    int32_t rule_margin{};
    int32_t margin{};
    int32_t icon_left{};
    int32_t icon_side{};
    int32_t text_left{};
};

/// The columns an entry keeps clear of its right, as the settings list does.
constexpr int32_t section_text_margin = 4;
/// The columns between the list's sides and a rule between its groups.
constexpr int32_t section_divider_inset = 4;
/// How far a filter or a tag sits in from a tab, so the lime bar moves with it.
constexpr int32_t section_filter_indent = 12;
/// The least the list keeps when the nav is folded back on a narrow window.
constexpr int32_t least_list_width = 96;
/// The details pane keeps this much at Regular and Large, and the list gives
/// way, so an action's words are not cut.
constexpr int32_t details_floor = 280;

/// Returns the section list's sizes at a class.
///
/// @param size_class the class
/// @return the sizes
SectionNav section_nav_of(kit::SizeClass size_class) {
    static constexpr std::array<SectionNav, 3> metrics{{
        {19, 0, 3, 6, 7, 9, 19},
        {25, 1, 3, 6, 12, 14, 33},
        {32, 1, 5, 10, 14, 16, 38},
    }};
    const auto index = static_cast<std::size_t>(size_class);
    return index < metrics.size() ? metrics[index] : metrics.back();
}

/// Drops the icons when the widest tab's word does not fit beside them, as
/// Settings drops its section icons. The names then start at the icon's column.
///
/// @param[in,out] nav the sizes
/// @param building the layout
/// @param entry_width an entry's width
void fit_section_icons(SectionNav& nav, const Building& building, int32_t entry_width) {
    int32_t widest = 0;
    for (const Tab tab : tab_order) {
        const std::string word(tab_text(tab, false, building.library->text));
        widest = std::max(widest, width_of(building, kit::FontRole::regular, word));
    }
    const int32_t room = entry_width - nav.text_left - section_text_margin;
    if (widest > room) {
        nav.text_left = nav.icon_left;
        nav.icon_side = 0;
    }
}

/// Returns the tag icon a tab draws. Keys and Registries use a mark instead,
/// and Replays its film (add_replay_icon).
///
/// @param tab the tab
/// @return the icon; none for Replays, Keys and Registries
std::optional<kit::TagIcon> tab_tag_icon(Tab tab) {
    switch (tab) {
    case Tab::active:
        return kit::TagIcon::commanders;
    case Tab::mods:
        return kit::TagIcon::new_units;
    case Tab::maps:
        return kit::TagIcon::maps;
    case Tab::mutators:
        return kit::TagIcon::gameplay;
    case Tab::brains:
        return kit::TagIcon::ai;
    case Tab::languages:
        return kit::TagIcon::statistics;
    case Tab::updates:
        return kit::TagIcon::economy;
    case Tab::replays:
    case Tab::keys:
    case Tab::registries:
        return std::nullopt;
    }
    return std::nullopt;
}

/// Returns the mark a tab draws beside its name when the icons show.
///
/// @param tab the tab
/// @return the mark; none when the tab uses a tag icon
std::optional<kit::Mark> tab_mark(Tab tab) {
    if (tab == Tab::keys)
        return kit::Mark::key;
    if (tab == Tab::registries)
        return kit::Mark::padlock;
    return std::nullopt;
}

/// Returns a heading in capitals. Only ASCII letters change, so a translated
/// title that is already capitals is left as it is.
///
/// @param title the open tab's name
/// @return the heading
std::string heading_of(std::string_view title) {
    std::string heading(title);
    for (char& letter : heading) {
        if (letter >= 'a' && letter <= 'z')
            letter = static_cast<char>(letter - 'a' + 'A');
    }
    return heading;
}

/// Returns the word that says which entry the nav shows as chosen: the tab,
/// the filter and the tag.
///
/// @param library the model
/// @return the word
std::string filter_choice_word(const Library& library) {
    if (library.tab == Tab::replays)
        return std::to_string(static_cast<int32_t>(library.tab)) + "/" +
               std::to_string(static_cast<int32_t>(library.replays.filter)) + "/";
    return std::to_string(static_cast<int32_t>(library.tab)) + "/" +
           std::to_string(static_cast<int32_t>(shown_filter(library))) + "/" + library.tag;
}

/// Returns the offset nearest another that shows a part whole, or its top
/// when it is taller than the view.
///
/// @param offset the offset now
/// @param top the part's top when the nav is not scrolled
/// @param height the part's height
/// @param view_height the view's height
/// @param limit the greatest offset
/// @return the offset
int32_t
shown_offset(int32_t offset, int32_t top, int32_t height, int32_t view_height, int32_t limit) {
    const int32_t lowest = top + height - view_height;
    return std::clamp(
        std::min(std::max(offset, lowest), top), int32_t{0}, std::max(limit, int32_t{0})
    );
}

/// One row of the section list, placed from the top of its content.
struct NavPart {
    bool heading{};                     ///< TAGS or WORLD, not an entry
    int32_t top{};                      ///< its top, from the content's top
    int32_t height{};                   ///< its height
    int32_t indent{};                   ///< columns in from a tab
    std::string word{};                 ///< the word that is drawn
    std::string count{};                ///< the digits after it; empty draws none
    std::string reading{};              ///< the control's text, with the count in brackets
    bool selected{};                    ///< the open tab, the chosen filter or the chosen tag
    kit::ControlId id{kit::no_control}; ///< an entry's control
    std::string name{};                 ///< an entry's name
    std::optional<kit::TagIcon> tag{};  ///< an entry's tag icon
    std::optional<kit::Mark> mark{};    ///< a tab's mark, when it has no tag icon
    bool film{};                        ///< Replays' film, drawn as a picture of its own
    bool in_filters{};                  ///< a filter or a tag: the pane's scroll group
    /// The word takes the room it needs and the count follows it only when
    /// both fit, instead of the word giving way to the count.
    bool whole_word{};
};

/// Appends the section list down the left: the tabs, then, under a divider,
/// the open tab's filters and tags. The whole column scrolls. The tabs are
/// stops here; the filters and tags are stopped once the search has been,
/// so Tab's order stays tabs, search, filters.
///
/// @param[in,out] building the layout
/// @param column the list's face
void add_section_nav(Building& building, const kit::Rect& column) {
    const Library& library = *building.library;
    const ScreenState& state = *building.state;
    const kit::Metrics& metrics = *building.metrics;
    SectionNav nav = section_nav_of(building.size_class);
    // Settings' list_item_left is edge + 6. The face starts on the window's
    // edge, so an entry starts six columns in and keeps six clear of the rule.
    const int32_t entry_width = std::max(column.width - 12, int32_t{0});
    const int32_t item_left = column.x + 6;

    kit::NavLook face;
    face.area = column;
    face.rule_column = column.x + column.width - 1;
    add_item(building.list, kit::Role::nav, column, {}).look = face;

    const kit::Rect view{
        column.x,
        column.y + nav.margin,
        column.width,
        std::max(column.height - 2 * nav.margin, int32_t{0}),
    };

    std::vector<NavPart> parts;
    int32_t cursor = 0;
    const auto push = [&](NavPart part) {
        part.top = cursor;
        parts.push_back(std::move(part));
        cursor += parts.back().height;
    };
    for (std::size_t index = 0; index < tab_order.size(); ++index) {
        const Tab tab = tab_order[index];
        NavPart part;
        part.height = nav.entry_height;
        part.word = std::string(tab_text(tab, false, library.text));
        part.reading = tab_caption(library, tab, false);
        if (tab != Tab::active)
            part.count = std::to_string(tab_count(library, tab));
        part.selected = tab == library.tab;
        part.id = controls::tab_control(tab);
        part.name = named("tab." + std::string(tab_words[index]));
        part.tag = tab_tag_icon(tab);
        part.mark = tab_mark(tab);
        part.film = tab == Tab::replays;
        push(std::move(part));
        if (index + 1 < tab_order.size())
            cursor += nav.entry_gap;
    }

    const bool catalogue =
        library.tab != Tab::active && library.tab != Tab::registries && library.tab != Tab::replays;
    int32_t divider_top = -1;
    if (library.tab == Tab::replays) {
        // Replays' own filters, under the divider as a tab's filters are.
        cursor += nav.rule_margin;
        divider_top = cursor;
        cursor += 1 + nav.rule_margin;
        for (std::size_t index = 0; index < replay_filter_order.size(); ++index) {
            const ReplayFilter filter = replay_filter_order[index];
            NavPart part;
            part.height = nav.entry_height;
            // At Compact the column is narrow: the filters sit nearer the edge so
            // that each word is whole (the longest, "Dieser Monat", needs the room).
            part.indent = building.size_class == kit::SizeClass::compact ? section_filter_indent / 2
                                                                         : section_filter_indent;
            part.word = replay_filter_title(library, filter);
            part.count = std::to_string(replay_filter_count(library, filter));
            part.reading = filled(
                library.text,
                words::filter_with_count,
                {{"filter", part.word}, {"count", part.count}}
            );
            part.selected = library.replays.filter == filter;
            part.id = controls::replay_filter_control(filter);
            part.name = named("filter." + std::string(replay_filter_words[index]));
            part.in_filters = true;
            part.whole_word = true;
            push(std::move(part));
            cursor += nav.entry_gap;
        }
    }
    if (catalogue) {
        cursor += nav.rule_margin;
        divider_top = cursor;
        cursor += 1 + nav.rule_margin;
        for (std::size_t index = 0; index < filter_order.size(); ++index) {
            const Filter filter = filter_order[index];
            NavPart part;
            part.height = nav.entry_height;
            part.indent = section_filter_indent;
            part.word = std::string(filter_text(filter, library.text));
            part.count = std::to_string(filter_count(library, filter));
            part.reading = filter_caption(library, filter);
            part.selected = shown_filter(library) == filter;
            part.id = controls::filter_control(filter);
            part.name = named("filter." + std::string(filter_words[index]));
            part.in_filters = true;
            push(std::move(part));
            cursor += nav.entry_gap;
        }
        const bool labelled =
            library.tab == Tab::mutators || library.tab == Tab::brains || library.tab == Tab::keys;
        const std::vector<std::pair<std::string, std::size_t>> tags = tags_in_tab(library);
        bool tags_headed = false;
        bool worlds_headed = false;
        for (std::size_t index = 0; index < tags.size(); ++index) {
            const std::string& tag = tags[index].first;
            if (world_tag(tag) && !worlds_headed) {
                cursor += metrics.paragraph_gap;
                NavPart heading;
                heading.heading = true;
                heading.height = metrics.small_line;
                heading.indent = section_filter_indent;
                heading.word = std::string(shown(library.text, words::world_heading));
                push(std::move(heading));
                cursor += metrics.hint_gap;
                worlds_headed = true;
            } else if (!world_tag(tag) && !tags_headed) {
                cursor += metrics.paragraph_gap;
                NavPart heading;
                heading.heading = true;
                heading.height = metrics.small_line;
                heading.indent = section_filter_indent;
                heading.word = std::string(shown(library.text, words::tags_menu));
                push(std::move(heading));
                cursor += metrics.hint_gap;
                tags_headed = true;
            }
            NavPart part;
            part.height = nav.entry_height;
            part.indent = section_filter_indent;
            part.word = std::string(tag_caption(library, tag, labelled));
            part.count = std::to_string(tags[index].second);
            part.reading = filled(
                library.text, words::tag_with_count, {{"tag", part.word}, {"count", part.count}}
            );
            part.selected = library.tag == tag;
            part.id = controls::first_tag + static_cast<kit::ControlId>(index);
            part.name = named("tag." + name_word(tag));
            part.tag = library.tab == Tab::mutators ? kit::tag_icon_of(tag) : std::nullopt;
            part.in_filters = true;
            push(std::move(part));
            cursor += nav.entry_gap;
        }
    }
    const int32_t content_height = parts.empty() ? 0 : parts.back().top + parts.back().height;
    const int32_t limit = kit::ScrollArea::limit_for(content_height, view.height);
    const int32_t fitted_width =
        limit > 0 ? std::max(
                        entry_width - (metrics.scroll_thumb_width + 2 + 4 + metrics.button_gap),
                        int32_t{0}
                    )
                  : entry_width;
    fit_section_icons(nav, building, fitted_width);
    const int32_t well_width = metrics.scroll_thumb_width + 2;
    const kit::Rect well{view.x + view.width - well_width - 4, view.y, well_width, view.height};

    const std::string choice = filter_choice_word(library);
    const auto slash = choice.find('/');
    const auto old_slash = state.filters_chosen.find('/');
    const bool choice_changed = choice != state.filters_chosen;
    // Empty is the first layout, not a change of tab: a tag already chosen,
    // including one a fresh state opens on, is brought into the view. A later
    // change of tab shows the open tab from the top.
    const bool tab_changed = !state.filters_chosen.empty() && slash != std::string::npos &&
                             old_slash != std::string::npos &&
                             choice.compare(0, slash, state.filters_chosen, 0, old_slash) != 0;
    if (choice_changed) {
        if (tab_changed) {
            for (const NavPart& part : parts) {
                if (part.heading || part.in_filters || !part.selected)
                    continue;
                state.filters_scroll = shown_offset(0, part.top, part.height, view.height, limit);
                break;
            }
        } else {
            for (auto part_at = parts.rbegin(); part_at != parts.rend(); ++part_at) {
                const NavPart& part = *part_at;
                if (part.heading || !part.selected)
                    continue;
                state.filters_scroll =
                    shown_offset(state.filters_scroll, part.top, part.height, view.height, limit);
                break;
            }
        }
        state.filters_chosen = choice;
        state.filters_stop = kit::no_control;
    }
    // The focus brings its entry into view once, when it moves onto it. The
    // wheel, the bar and a finger then scroll past it and the view stays.
    const kit::ControlId focused_id = state.interaction.focused;
    if (focused_id != state.filters_focused) {
        for (const NavPart& part : parts) {
            if (part.heading || part.id != focused_id)
                continue;
            state.filters_scroll =
                shown_offset(state.filters_scroll, part.top, part.height, view.height, limit);
            break;
        }
        state.filters_focused = focused_id;
    }
    const int32_t offset = std::clamp(state.filters_scroll, int32_t{0}, limit);
    state.filters_scroll = offset;

    const auto room_for = [&](int32_t width, const NavPart& part) {
        int32_t visual = width - nav.text_left - section_text_margin;
        if (part.whole_word) {
            // The word takes the room; the kit draws the count after it when it fits.
            const int32_t check =
                width - kit::compact_metrics.padding - kit::compact_metrics.list_row_inset;
            return std::max(std::min(visual, check), int32_t{0});
        }
        if (!part.count.empty()) {
            const int32_t count_width = width_of(building, kit::FontRole::small, part.count);
            const int32_t gap = width_of(building, kit::FontRole::small, " ");
            const int32_t with_count = visual - count_width - gap;
            if (with_count >= metrics.regular_line)
                visual = with_count;
        }
        const int32_t check =
            width - kit::compact_metrics.padding - kit::compact_metrics.list_row_inset;
        return std::max(std::min(visual, check), int32_t{0});
    };

    kit::NavLook list_look;
    if (divider_top >= 0) {
        list_look.divider = kit::Rect{
            item_left + section_divider_inset,
            view.y - offset + divider_top,
            std::max(entry_width - 2 * section_divider_inset, int32_t{0}),
            1,
        };
    }
    std::vector<kit::ControlId> ids;
    std::vector<std::string> names;
    std::vector<std::string> readings;
    std::vector<NavPart> marks;
    const int32_t scroll_right = limit > 0 ? well.x - metrics.button_gap : item_left + entry_width;
    for (NavPart& part : parts) {
        const int32_t row = view.y - offset + part.top;
        const int32_t left = item_left + part.indent;
        const int32_t width =
            std::max(std::min(item_left + entry_width, scroll_right) - left, int32_t{0});
        if (part.heading) {
            int32_t room = width;
            std::string drawn =
                cut(building, kit::FontRole::small, part.word, std::max(room, int32_t{0}));
            while (room > 0 &&
                   kit::tracked_width(
                       *building.fonts, kit::FontRole::small, drawn, metrics.heading_tracking
                   ) > width) {
                --room;
                drawn = cut(building, kit::FontRole::small, part.word, room);
            }
            kit::Item& item =
                add_item(building.list, kit::Role::heading, {left, row, width, part.height}, view);
            item.text = drawn;
            item.look = kit::HeadingLook{drawn};
            continue;
        }
        kit::NavEntry entry;
        entry.rect = {left, row, width, part.height};
        entry.caption = cut(building, kit::FontRole::regular, part.word, room_for(width, part));
        // The count follows the word where the kit draws it, and only when it fits.
        entry.count = part.count;
        entry.icon_column = nav.icon_left;
        entry.icon_side = nav.icon_side;
        entry.text_column = nav.text_left;
        if (nav.icon_side > 0)
            entry.tag = part.tag;
        entry.selected = part.selected;
        entry.hovered =
            hovered(building, part.id) || building.state->interaction.pressed == part.id;
        entry.focused = focused(building, part.id);
        list_look.entries.push_back(std::move(entry));
        ids.push_back(part.id);
        names.push_back(unique_name(building, std::move(part.name)));
        readings.push_back(std::move(part.reading));
        if (nav.icon_side > 0 && (part.mark.has_value() || part.film))
            marks.push_back(part);
    }
    const std::size_t first_item = building.list.items.size();
    const std::size_t before = building.list.controls.size();
    const std::size_t first_control = building.top.size();
    if (!list_look.entries.empty())
        kit::add_nav(building.list, list_look, ids, names);
    for (std::size_t index = first_item; index < building.list.items.size(); ++index) {
        kit::Item& item = building.list.items[index];
        item.clip = view;
        // The entry draws its own focus ring. The item carries the control so
        // the screen's ring is not drawn over that one.
        const std::size_t entry = index - first_item;
        if (entry < ids.size())
            item.control = ids[entry];
    }
    move_controls(building.list, before, building.top);
    for (std::size_t index = 0;
         index < readings.size() && first_control + index < building.top.size();
         ++index) {
        kit::Control& control = building.top[first_control + index];
        control.clip = view;
        control.text = readings[index];
    }
    for (std::size_t index = first_control; index < building.top.size(); ++index) {
        kit::Control& control = building.top[index];
        const bool in_filters = control.name.find(".filter.") != std::string::npos ||
                                control.name.find(".tag.") != std::string::npos;
        if (in_filters)
            control.group = controls::filters_group;
        else
            tab_stop(building, control);
    }
    for (const NavPart& part : marks) {
        if (part.film) {
            const int32_t row = view.y - offset + part.top;
            const int32_t left = item_left + part.indent;
            const int32_t film_side = nav.icon_side;
            add_replay_icon(
                building,
                ReplayIcon::film,
                {left + nav.icon_left, row + (part.height - film_side) / 2, film_side, film_side},
                part.selected ? kit::colour::text : kit::colour::list_text,
                view
            );
            // The film is the Replays entry's own icon.
            if (!building.list.items.empty() &&
                building.list.items.back().role == kit::Role::picture)
                building.list.items.back().control = part.id;
            continue;
        }
        const kit::Point pixels = kit::mark_size(*part.mark);
        const int32_t row = view.y - offset + part.top;
        const int32_t left = item_left + part.indent;
        const int32_t x = left + nav.icon_left + (nav.icon_side - pixels.x) / 2;
        const int32_t y = row + (part.height - pixels.y) / 2;
        const kit::Colour ink = part.selected ? kit::colour::text : kit::colour::list_text;
        kit::Item& item =
            add_item(building.list, kit::Role::mark, {x, y, pixels.x, pixels.y}, view);
        item.look = kit::MarkLook{*part.mark, ink};
    }
    if (limit > 0) {
        kit::ScrollArea scroll;
        scroll.view = view;
        scroll.well = well;
        scroll.content_height = content_height;
        scroll.limit = limit;
        scroll.offset = offset;
        add_item(building.list, kit::Role::scroll_bar, well, {}).look = kit::ScrollBarLook{
            scroll.thumb(metrics.least_thumb_height), state.bar_held == Pane::filters
        };
    }
}

/// Stops on the filters and tags, after the search, so Tab reaches them in
/// the order they are drawn and then the list.
///
/// @param[in,out] building the layout
void stop_on_filters(Building& building) {
    for (kit::Control& control : building.top)
        if (control.group == controls::filters_group)
            tab_stop(building, control);
}

/// Appends the open tab's heading, and on a catalogue tab the search at the
/// row's right, at the control's height.
///
/// @param[in,out] building the layout
/// @param row the row
/// @param with_search true draws the search
void add_section_heading(Building& building, const kit::Rect& row, bool with_search) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    const std::string heading = heading_of(tab_text(library.tab, false, library.text));
    int32_t search_width = 0;
    if (with_search) {
        const int32_t gap = metrics.space;
        const int32_t words = kit::tracked_width(
            *building.fonts, kit::FontRole::small, heading, metrics.heading_tracking
        );
        const int32_t spare = std::max(row.width - words - gap, int32_t{0});
        search_width = std::min(wide_search_width, spare);
        const int32_t least = std::min(wide_search_width, std::max(row.width / 2, int32_t{0}));
        if (search_width < least)
            search_width =
                std::min(least, std::max(row.width - metrics.small_line - gap, int32_t{0}));
    }
    const int32_t gap = search_width > 0 ? metrics.space : 0;
    const int32_t heading_width = std::max(row.width - search_width - gap, int32_t{0});
    int32_t room = heading_width;
    std::string drawn = cut(building, kit::FontRole::small, heading, std::max(room, int32_t{0}));
    while (room > 0 && kit::tracked_width(
                           *building.fonts, kit::FontRole::small, drawn, metrics.heading_tracking
                       ) > heading_width) {
        --room;
        drawn = cut(building, kit::FontRole::small, heading, room);
    }
    const int32_t text_height = metrics.small_line;
    const int32_t text_y = row.y + std::max(row.height - text_height, int32_t{0}) / 2;
    kit::Item& item = add_item(
        building.list, kit::Role::heading, {row.x, text_y, heading_width, text_height}, {}
    );
    item.text = drawn;
    item.look = kit::HeadingLook{drawn};
    if (search_width > 0)
        add_search(building, {row.x + row.width - search_width, row.y, search_width, row.height});
}

/// Returns the nav's face and the content to its right.
///
/// @param window the window
/// @param metrics the class's metrics
/// @return the nav, then the content
std::pair<kit::Rect, kit::Rect> section_columns(const Window& window, const kit::Metrics& metrics) {
    int32_t nav_width = metrics.nav_width;
    int32_t content_left = metrics.edge + nav_width + window.gutter;
    int32_t content_width = window.right - content_left;
    if (content_width < least_list_width) {
        const int32_t room = std::max(window.right - metrics.edge - window.gutter, int32_t{0});
        content_width = std::min(least_list_width, room);
        nav_width = std::max(room - content_width, int32_t{0});
        content_left = metrics.edge + nav_width + window.gutter;
        content_width = std::max(window.right - content_left, int32_t{0});
    }
    const int32_t top = window.body_top;
    const int32_t height = std::max(window.footer_rule - top, int32_t{0});
    const kit::Rect nav{metrics.edge, top, nav_width, height};
    const kit::Rect content{content_left, top, content_width, height};
    return {nav, content};
}

/// Returns the words an empty list shows.
///
/// @param library the model
/// @return the words, looked up
std::string empty_words(const Library& library) {
    if (library.inputs.registries.empty())
        return std::string(shown(library.text, words::empty_no_registries));
    if (!library.query.empty())
        return filled(library.text, words::empty_matches, {{"query", library.query}});
    if (shown_filter(library) == Filter::updates)
        return std::string(shown(library.text, words::empty_updates));
    return std::string(shown(library.text, words::empty_tab));
}

/// The most bytes one scaled base-game badge may take. A larger square keeps
/// the icon as it was read, and the kit scales that.
constexpr uint64_t most_scaled_badge_bytes = 8U * 1024U * 1024U;

/// Returns the square a list row's badge is drawn in. The kit's row uses
/// Compact's inset and badge side at every size class.
///
/// @param row the row
/// @return the square, inside the row
kit::Rect row_badge_square(const kit::Rect& row) noexcept {
    const kit::Metrics& metrics = kit::compact_metrics;
    const int32_t inset = metrics.list_row_inset;
    const int32_t side =
        std::max(std::min(row.height - 2 * inset, metrics.list_row_badge), int32_t{0});
    return {row.x + inset, row.y + (row.height - side) / 2, side, side};
}

/// Tells whether a badge is the base game's icon.
///
/// @param library the model
/// @param badge the badge; may be null
/// @return true when it is Inputs::base_game_badge
bool base_badge(const Library& library, const Badge* badge) noexcept {
    return library.inputs.base_game_badge.has_value() && badge == &*library.inputs.base_game_badge;
}

/// Tells whether a picture is the base game's icon, still at the size it was read.
///
/// @param library the model
/// @param picture the picture
/// @return true when its pixels are Inputs::base_game_badge
bool base_picture(
    const Library& library, const oa::ui::frontend_renderer::RgbaPicture& picture
) noexcept {
    if (!library.inputs.base_game_badge)
        return false;
    const Badge& badge = *library.inputs.base_game_badge;
    return picture.width == badge.width && picture.height == badge.height &&
           picture.pixels.data() == badge.rgba.data();
}

/// Returns how many surface pixels a badge square covers at this layout's scale.
/// A whole scale is that many times the square; any other scale rounds each
/// edge on its own, as the canvas does. The Library's canvas starts at the origin.
///
/// @param building the layout
/// @param rect the badge's square, in points
/// @return the pixels; empty when the square covers none
kit::Point badge_pixel_size(const Building& building, const kit::Rect& rect) noexcept {
    if (rect.width <= 0 || rect.height <= 0)
        return {};
    const kit::Rect drawn = kit::to_canvas(rect, building.scale_percent);
    if (drawn.width <= 0 || drawn.height <= 0)
        return {};
    return {drawn.width, drawn.height};
}

/// Scales an icon to a pixel square, nearest-neighbour, whole pixels, and
/// centres it. A scale of one or more copies each source pixel into a block;
/// a smaller scale keeps the icon's shape and samples one source pixel for
/// each drawn pixel. The square around it is clear.
///
/// @param picture the icon
/// @param dest_columns the square's columns
/// @param dest_rows the square's rows
/// @return the square, dest_columns by dest_rows, or empty when it would be too large
std::vector<uint8_t> scale_nearest(
    const oa::ui::frontend_renderer::RgbaPicture& picture, uint32_t dest_columns, uint32_t dest_rows
) {
    if (dest_columns == 0 || dest_rows == 0 || dest_rows > most_scaled_badge_bytes / 4U ||
        uint64_t{dest_columns} > most_scaled_badge_bytes / 4U / dest_rows)
        return {};
    std::vector<uint8_t> scaled(static_cast<std::size_t>(dest_columns) * dest_rows * 4U, 0);
    const uint32_t source_columns = picture.width;
    const uint32_t source_rows = picture.height;
    if (source_columns == 0 || source_rows == 0)
        return scaled;
    uint32_t drawn_columns = dest_columns;
    uint32_t drawn_rows = dest_rows;
    uint32_t block = 0;
    if (dest_columns / source_columns >= 1U && dest_rows / source_rows >= 1U) {
        block = std::min(dest_columns / source_columns, dest_rows / source_rows);
        drawn_columns = source_columns * block;
        drawn_rows = source_rows * block;
    } else if (uint64_t{source_columns} * dest_rows >= uint64_t{source_rows} * dest_columns) {
        drawn_columns = dest_columns;
        drawn_rows = static_cast<uint32_t>(uint64_t{dest_columns} * source_rows / source_columns);
    } else {
        drawn_rows = dest_rows;
        drawn_columns = static_cast<uint32_t>(uint64_t{dest_rows} * source_columns / source_rows);
    }
    if (drawn_columns == 0)
        drawn_columns = 1;
    if (drawn_rows == 0)
        drawn_rows = 1;
    if (drawn_columns > dest_columns)
        drawn_columns = dest_columns;
    if (drawn_rows > dest_rows)
        drawn_rows = dest_rows;
    const uint32_t origin_column = (dest_columns - drawn_columns) / 2U;
    const uint32_t origin_row = (dest_rows - drawn_rows) / 2U;
    const uint8_t* source = picture.pixels.data();
    for (uint32_t row = 0; row < drawn_rows; ++row) {
        uint32_t source_row = block >= 1U ? row / block : row * source_rows / drawn_rows;
        if (source_row >= source_rows)
            source_row = source_rows - 1U;
        for (uint32_t column = 0; column < drawn_columns; ++column) {
            uint32_t source_column =
                block >= 1U ? column / block : column * source_columns / drawn_columns;
            if (source_column >= source_columns)
                source_column = source_columns - 1U;
            const uint8_t* from =
                source + (static_cast<std::size_t>(source_row) * source_columns + source_column) *
                             oa::ui::frontend_renderer::picture_pixel_bytes;
            uint8_t* to =
                scaled.data() + (static_cast<std::size_t>(origin_row + row) * dest_columns +
                                 origin_column + column) *
                                    oa::ui::frontend_renderer::picture_pixel_bytes;
            to[0] = from[0];
            to[1] = from[1];
            to[2] = from[2];
            to[3] = from[3];
        }
    }
    return scaled;
}

/// Returns the base game's icon scaled to the pixels of a badge square.
/// The pixels live on the Library until the next layout.
///
/// @param building the layout
/// @param picture the icon, at the size it was read
/// @param rect the badge's square, in points
/// @return the scaled picture, or `picture` when this square is not drawn
oa::ui::frontend_renderer::RgbaPicture fitted_base_picture(
    Building& building, const oa::ui::frontend_renderer::RgbaPicture& picture, const kit::Rect& rect
) {
    const kit::Point pixels = badge_pixel_size(building, rect);
    if (pixels.x <= 0 || pixels.y <= 0)
        return picture;
    const auto columns = static_cast<uint32_t>(pixels.x);
    const auto rows = static_cast<uint32_t>(pixels.y);
    std::vector<uint8_t> scaled = scale_nearest(picture, columns, rows);
    if (scaled.empty())
        return picture;
    std::vector<uint8_t>& kept =
        building.library->scaled_badge_pixels.emplace_back(std::move(scaled));
    oa::ui::frontend_renderer::RgbaPicture fitted;
    fitted.width = columns;
    fitted.height = rows;
    fitted.pixels = kept;
    return fitted;
}

/// Returns a map picture as the painter takes one.
///
/// @param picture the minimap
/// @return the picture, which points into it
oa::ui::frontend_renderer::RgbaPicture picture_of_map(const MapPicture& picture) {
    oa::ui::frontend_renderer::RgbaPicture drawn;
    drawn.width = picture.width;
    drawn.height = picture.height;
    drawn.pixels = std::span<const uint8_t>(picture.rgba);
    return drawn;
}

/// Fits a minimap into a box, nearest-neighbour, and fills the letterbox and
/// every transparent pixel with the list's well colour. The result is the
/// box's pixel size, so drawing it into that box does not stretch it. The
/// pixels live on the Library until the next layout.
///
/// @param building the layout
/// @param picture the minimap
/// @param rect the box, in points
/// @return the fitted picture; empty when the box covers no pixels
oa::ui::frontend_renderer::RgbaPicture fitted_well_picture(
    Building& building, const oa::ui::frontend_renderer::RgbaPicture& picture, const kit::Rect& rect
) {
    const kit::Point pixels = badge_pixel_size(building, rect);
    if (pixels.x <= 0 || pixels.y <= 0)
        return {};
    const auto columns = static_cast<uint32_t>(pixels.x);
    const auto rows = static_cast<uint32_t>(pixels.y);
    std::vector<uint8_t> scaled = scale_nearest(picture, columns, rows);
    if (scaled.empty())
        return {};
    const kit::Colour well = kit::colour::well;
    for (std::size_t index = 0; index + 3 < scaled.size(); index += 4) {
        if (scaled[index + 3] != 0)
            continue;
        scaled[index] = well.r;
        scaled[index + 1] = well.g;
        scaled[index + 2] = well.b;
        scaled[index + 3] = well.a;
    }
    std::vector<uint8_t>& kept =
        building.library->scaled_badge_pixels.emplace_back(std::move(scaled));
    oa::ui::frontend_renderer::RgbaPicture fitted;
    fitted.width = columns;
    fitted.height = rows;
    fitted.pixels = kept;
    return fitted;
}

/// Returns a map line's minimap when its pixels are there.
///
/// @param line the map
/// @return the minimap; null without one
const MapPicture* map_pixels(const MapLine& line) noexcept {
    if (!line.preview || line.preview->rgba.empty() || line.preview->width == 0 ||
        line.preview->height == 0)
        return nullptr;
    return &*line.preview;
}

/// Returns the installed pack an entry is, by its key and registry, or by
/// its name when the pack names no key.
///
/// @param library the model
/// @param entry the entry
/// @return the pack; null when the entry is not an installed map pack
const InstalledMaps* installed_pack(const Library& library, const Entry& entry) {
    if (entry.id.kind != Kind::map_pack)
        return nullptr;
    const InstalledMaps* by_name = nullptr;
    for (const InstalledMaps& pack : library.inputs.map_packs) {
        if (!pack.key.empty() && pack.key == entry.id.key && pack.registry == entry.id.registry)
            return &pack;
        if (pack.key.empty() && pack.name == entry_name(entry))
            by_name = &pack;
    }
    return by_name;
}

/// Returns the installed map that is the same as a listed one: the stem
/// when both name one, else the title.
///
/// @param pack the installed pack
/// @param listed the catalogue's map
/// @return the installed map; null when none matches
const MapLine* installed_map(const InstalledMaps& pack, const MapLine& listed) {
    if (!listed.stem.empty()) {
        for (const MapLine& map : pack.maps)
            if (map.stem == listed.stem)
                return &map;
    }
    for (const MapLine& map : pack.maps)
        if (map.name == listed.name)
            return &map;
    return nullptr;
}

/// One map as the details and a row show it. The words point into the
/// Library's inputs until the next refresh.
struct ShownMap {
    std::string_view name{};
    int32_t players{};
    std::string_view size{};
    std::string_view description{};
    const MapPicture* preview{}; ///< null while it has not been decoded
};

/// Returns the maps an entry shows. A listing's maps come first, and an
/// installed map's minimap and description fill in where the listing has
/// none. An installed minimap wins when both have pixels. With no listing
/// the installed pack's maps are the ones shown.
///
/// @param library the model
/// @param entry the entry
/// @return the maps; empty when the entry is not a map pack
std::vector<ShownMap> maps_shown(const Library& library, const Entry& entry) {
    std::vector<ShownMap> result;
    if (entry.id.kind != Kind::map_pack)
        return result;
    const InstalledMaps* pack = installed_pack(library, entry);
    if (entry.listing != nullptr && !entry.listing->maps.empty()) {
        for (const MapLine& listed : entry.listing->maps) {
            const MapLine* installed = pack != nullptr ? installed_map(*pack, listed) : nullptr;
            ShownMap map;
            map.name = listed.name;
            map.players = listed.players;
            map.size = listed.size.empty() && installed != nullptr
                           ? std::string_view{installed->size}
                           : std::string_view{listed.size};
            map.description = listed.description.empty() && installed != nullptr
                                  ? std::string_view{installed->description}
                                  : std::string_view{listed.description};
            const MapPicture* from_installed =
                installed != nullptr ? map_pixels(*installed) : nullptr;
            const MapPicture* from_listed = map_pixels(listed);
            map.preview = from_installed != nullptr ? from_installed : from_listed;
            result.push_back(map);
        }
        return result;
    }
    if (pack == nullptr)
        return result;
    for (const MapLine& map_line : pack->maps) {
        ShownMap map;
        map.name = map_line.name;
        map.players = map_line.players;
        map.size = map_line.size;
        map.description = map_line.description;
        map.preview = map_pixels(map_line);
        result.push_back(map);
    }
    return result;
}

/// What a map pack's row shows in place of its badge.
struct RowPreview {
    const MapPicture* picture{}; ///< the minimap to fit; null to keep the badge
    bool placeholder{};          ///< the minimap is not here yet: the row keeps its letters
};

/// Returns the minimap a map pack's row shows.
///
/// One map always shows its minimap, or the row's letters while it is not
/// decoded. Several maps on the Maps tab show the pack's badge when it has
/// a picture, and otherwise the first map's minimap. Everywhere else a pack
/// of several keeps its badge or its letters.
///
/// @param library the model
/// @param entry the entry
/// @return what the row shows; both empty keeps the badge the row already has
RowPreview row_preview(const Library& library, const Entry& entry) {
    RowPreview choice;
    if (entry.id.kind != Kind::map_pack)
        return choice;
    const std::vector<ShownMap> maps = maps_shown(library, entry);
    if (maps.size() == 1) {
        choice.picture = maps.front().preview;
        choice.placeholder = choice.picture == nullptr;
        return choice;
    }
    if (maps.size() > 1 && library.tab == Tab::maps &&
        !oa::ui::frontend_renderer::picture_drawable(badge_picture(entry))) {
        choice.picture = maps.front().preview;
        choice.placeholder = choice.picture == nullptr;
    }
    return choice;
}

/// The side of one map's large preview, and of one cell in a grid of several.
constexpr int32_t map_preview_large = 160;
constexpr int32_t map_preview_cell = 72;
/// How many lines of a map's description a grid cell keeps.
constexpr std::size_t map_preview_lines = 3;

/// Returns the Keys tab's row height: Large's row, grown for each chip line
/// past the first. The kit wraps a row's tags in the fonts the layout
/// measured them in (kit::RowSpacing), so they wrap where they are counted
/// here.
///
/// @param building the layout
/// @param rows_width a row's width, in points
/// @return the height, in points
int32_t keys_chip_row_height(const Building& building, int32_t rows_width) {
    const int32_t nominal = row_height(building, RowForm::large);
    int32_t lines = 1;
    const Library& library = *building.library;
    for (const std::size_t place : library.visible) {
        const Entry& entry = library.entries[place];
        if (entry.id.kind != Kind::key_layout)
            continue;
        const kit::ListRowLook look =
            row_look(building, entry, RowForm::large, rows_width, nominal);
        const int32_t room = chip_line_room(building, look, rows_width, nominal);
        lines = std::max(lines, chip_lines_in(building, look.tags, room));
    }
    if (lines <= 1)
        return nominal;
    const kit::RowSpacing spacing = row_spacing(building);
    return nominal + (lines - 1) * (spacing.tag_height + spacing.tag_gap);
}

/// Tells whether an entry's Large row shows chips under its lines, as
/// row_look gives them: a brain's Default, Full awareness, tags and why it
/// does not fit, a mutator's tags, what it needs and that it does not fit
/// this mod, and any other entry's tags.
///
/// @param library the model
/// @param entry the entry
/// @return true when the row shows a chip under its lines
bool shows_row_chips(const Library& library, const Entry& entry) {
    if (entry.id.kind == Kind::brain) {
        const BrainFacts* facts = brain_facts(entry);
        if (entry.is_default)
            return true;
        if (facts == nullptr)
            return false;
        return facts->full_awareness || !facts->tags.empty() ||
               (entry.not_compatible &&
                !brain_unfit_reason(library.inputs, *facts, library.text).empty());
    }
    if (entry.id.kind == Kind::mutator && (!entry.needs_missing.empty() || entry.not_compatible))
        return true;
    const std::vector<std::string>* tags = shown_tags(entry);
    return tags != nullptr && !tags->empty();
}

/// Appends the list pane: the note on a list the last fetch could not
/// bring up to date, the rows that show, an empty list's words, the scroll
/// bar and the list's own control.
///
/// @param[in,out] building the layout
/// @param pane the pane
/// @param form how its rows are shown
void add_list(Building& building, const kit::Rect& pane, RowForm form) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    kit::Rect area = pane;
    if (library.inputs.offline) {
        const std::string note = filled(
            library.text,
            words::offline_list,
            {{"age", age_text(library.inputs.list_age_seconds, library.text)}}
        );
        add_text(
            building,
            kit::FontRole::small,
            cut(building, kit::FontRole::small, note, area.width),
            {area.x, area.y, area.width, metrics.small_line},
            kit::Align::left,
            kit::colour::lock,
            {}
        );
        area.y += metrics.small_line + metrics.hint_gap;
        area.height -= metrics.small_line + metrics.hint_gap;
    }
    const int32_t well_width = metrics.scroll_thumb_width + 2;
    const int32_t rows_width = std::max(area.width - well_width - metrics.button_gap, int32_t{0});
    // Keys keeps every chip, wrapping onto further lines, so its rows grow
    // together. Every other list keeps the form's one height, and a Large
    // list none of whose rows shows a chip under its lines keeps Regular's,
    // so its rows hold no empty line.
    int32_t height = form == RowForm::large && library.tab == Tab::keys
                         ? keys_chip_row_height(building, rows_width)
                         : row_height(building, form);
    if (form == RowForm::large && library.tab != Tab::keys &&
        std::none_of(library.visible.begin(), library.visible.end(), [&library](std::size_t place) {
            return shows_row_chips(library, library.entries[place]);
        }))
        height = row_height(building, RowForm::regular);
    // The rows lie a step apart, a gap under each (controls::list_row_step),
    // and the view holds whole steps.
    const int32_t step = controls::list_row_step(height);
    const int32_t view_height = std::max(area.height, int32_t{0}) / step * step;
    const kit::Rect view{area.x, area.y, area.width, view_height};
    const kit::Rect well{view.x + view.width - well_width, view.y, well_width, view.height};
    const auto content = static_cast<int32_t>(library.visible.size()) * step;
    const int32_t limit = kit::ScrollArea::limit_for(content, view.height);
    // A From link names the row to put at the top of the list. The write is
    // through the state's mutable scroll: the layout is handed a const state,
    // and a temporary state discards it.
    if (building.state->reveal_row >= 0 && !library.visible.empty()) {
        const int32_t last = static_cast<int32_t>(library.visible.size()) - 1;
        const int32_t place = std::clamp(building.state->reveal_row, int32_t{0}, last);
        const int64_t raw = static_cast<int64_t>(place) * static_cast<int64_t>(step);
        const int64_t furthest = static_cast<int64_t>(std::max(limit, int32_t{0}));
        building.state->list_scroll = static_cast<int32_t>(std::clamp(raw, int64_t{0}, furthest));
        building.state->reveal_row = -1;
    }
    const int32_t offset = std::clamp(building.state->list_scroll, int32_t{0}, limit);

    if (library.visible.empty())
        add_text(
            building,
            kit::FontRole::small,
            cut(building, kit::FontRole::small, empty_words(library), rows_width),
            {view.x, view.y, rows_width, metrics.small_line},
            kit::Align::left,
            kit::colour::hint,
            {}
        );
    const kit::RowSpacing spacing = row_spacing(building);
    for (std::size_t place = 0; place < library.visible.size(); ++place) {
        const int32_t top = view.y + static_cast<int32_t>(place) * step - offset;
        if (top + height <= view.y || top >= view.y + view.height)
            continue;
        // A hairline between two rows, as Settings separates its rows, in
        // the middle of the gap over each row but the first.
        if (place > 0) {
            const int32_t rule = top - (step - height) + (step - height - 1) / 2;
            add_item(building.list, kit::Role::rule, {view.x, rule, rows_width, 1}, view).colour =
                kit::colour::rule;
        }
        const Entry& entry = library.entries[library.visible[place]];
        const kit::ControlId id = controls::first_row + static_cast<kit::ControlId>(place);
        kit::ListRowLook look = row_look(building, entry, form, rows_width, height);
        look.spacing = spacing;
        look.selected = library.selected.has_value() && *library.selected == entry.id;
        look.selection_bar = true;
        look.hovered = hovered(building, id);
        const kit::Rect rect{view.x, top, rows_width, height};
        const RowPreview preview = row_preview(library, entry);
        if (preview.picture != nullptr || preview.placeholder) {
            // A minimap that has not arrived leaves the letters a row with
            // no picture shows. An empty square is not drawn in its place.
            oa::ui::frontend_renderer::RgbaPicture fitted;
            if (preview.picture != nullptr)
                fitted = fitted_well_picture(
                    building, picture_of_map(*preview.picture), row_badge_square(rect)
                );
            if (oa::ui::frontend_renderer::picture_drawable(fitted)) {
                look.badge = fitted;
                look.badge_text.clear();
            } else {
                look.badge = {};
            }
        } else if (
            base_badge(library, entry.badge) &&
            oa::ui::frontend_renderer::picture_drawable(look.badge)
        ) {
            look.badge = fitted_base_picture(building, look.badge, row_badge_square(rect));
        }
        if (look.chip && (entry.state == State::get || entry.state == State::update)) {
            // The download arrow is named get and the up-arrow update. A press
            // asks for what row_icon_action returns: GET, GET WITH or UPDATE.
            const kit::ControlId icon_id =
                controls::first_row_icon + static_cast<kit::ControlId>(place);
            const Action action = entry.state == State::get ? Action::get : Action::update;
            kit::Control icon;
            icon.id = icon_id;
            icon.rect = kit::list_row_icon_rect(*building.fonts, rect, look);
            icon.clip = view;
            icon.enabled = controls::row_icon_action(library, entry).has_value();
            icon.focusable = false;
            icon.name =
                unique_name(building, row_name(entry) + "." + std::string(action_word(action)));
            icon.kind = kit::ControlKind::button;
            icon.group = controls::list_group;
            icon.text = look.chip->text;
            look.chip->hovered = icon.enabled && hovered(building, icon_id);
            building.rows.push_back(std::move(icon));
        }
        const std::size_t before = building.list.controls.size();
        kit::add_list_row(building.list, rect, look, id, unique_name(building, row_name(entry)));
        building.list.items.back().clip = view;
        move_controls(building.list, before, building.rows);
        kit::Control& row = building.rows.back();
        row.clip = view;
        row.group = controls::list_group;
        row.focusable = false;
        row.text = maker_reading(std::string(entry_name(entry)), entry_maker(entry));
    }
    if (limit > 0) {
        kit::ScrollArea scroll;
        scroll.view = view;
        scroll.well = well;
        scroll.page_step = view.height;
        scroll.content_height = content;
        scroll.limit = limit;
        scroll.offset = offset;
        add_item(building.list, kit::Role::scroll_bar, well, {}).look = kit::ScrollBarLook{
            scroll.thumb(metrics.least_thumb_height), building.state->bar_held == Pane::list
        };
    }
    kit::Control pane_control;
    pane_control.id = controls::list;
    pane_control.rect = view;
    pane_control.name = unique_name(building, named("list"));
    pane_control.kind = kit::ControlKind::list;
    building.panes.push_back(std::move(pane_control));
    tab_stop(building, building.panes.back());
}

/// Appends a badge as the details show one: its picture, a known tag's icon
/// on the hint ground, or a name's letters on that ground.
///
/// A picture and the letters are the same items as before, so a mod, a map
/// pack and a language keep those pixels. A tag's icon is the kit's badge.
///
/// @param[in,out] building the layout
/// @param picture the picture; empty for the icon or the letters
/// @param name the name whose letters show without a picture or a known tag
/// @param rect the badge's square
/// @param clip what drawing may touch
/// @param tag the icon, when a mutator with no picture has a known first tag
void add_badge_of(
    Building& building,
    const oa::ui::frontend_renderer::RgbaPicture& picture,
    std::string_view name,
    const kit::Rect& rect,
    const kit::Rect& clip,
    std::optional<kit::TagIcon> tag = std::nullopt
) {
    oa::ui::frontend_renderer::RgbaPicture drawn = picture;
    if (base_picture(*building.library, picture))
        drawn = fitted_base_picture(building, picture, rect);
    if (oa::ui::frontend_renderer::picture_drawable(drawn)) {
        add_item(building.list, kit::Role::picture, rect, clip).look = kit::PictureLook{drawn};
        add_item(building.list, kit::Role::outline, rect, clip).colour =
            kit::colour::control_border;
        return;
    }
    if (tag.has_value()) {
        kit::BadgeLook badge;
        badge.face = badge_face;
        badge.tag = tag;
        badge.letters = badge_letters(name);
        kit::add_badge(building.list, rect, badge);
        building.list.items.back().clip = clip;
        return;
    }
    add_item(building.list, kit::Role::fill, rect, clip).colour = badge_face;
    const std::string letters = badge_letters(name);
    add_text(
        building,
        kit::FontRole::small,
        cut(building, kit::FontRole::small, letters, rect.width),
        rect,
        kit::Align::centre,
        kit::colour::on_accent,
        clip
    );
}

/// Appends a square filled with the list's well colour and outlined, the
/// place a minimap takes while it has not been decoded.
///
/// @param[in,out] building the layout
/// @param rect the square
/// @param clip what drawing may touch
void add_well_square(Building& building, const kit::Rect& rect, const kit::Rect& clip) {
    add_item(building.list, kit::Role::fill, rect, clip).colour = kit::colour::well;
    add_item(building.list, kit::Role::outline, rect, clip).colour = kit::colour::control_border;
}

/// Appends a minimap fitted into a square, or the well while it is not decoded.
///
/// @param[in,out] building the layout
/// @param picture the minimap; null for the well
/// @param rect the square
/// @param clip what drawing may touch
void add_fitted_preview(
    Building& building, const MapPicture* picture, const kit::Rect& rect, const kit::Rect& clip
) {
    if (picture != nullptr) {
        const oa::ui::frontend_renderer::RgbaPicture fitted =
            fitted_well_picture(building, picture_of_map(*picture), rect);
        if (oa::ui::frontend_renderer::picture_drawable(fitted)) {
            add_item(building.list, kit::Role::picture, rect, clip).look = kit::PictureLook{fitted};
            add_item(building.list, kit::Role::outline, rect, clip).colour =
                kit::colour::control_border;
            return;
        }
    }
    add_well_square(building, rect, clip);
}

/// Appends an entry's badge as the details show it. A pack of one map wears
/// that map's minimap. A pack of several keeps its picture or its letters.
///
/// @param[in,out] building the layout
/// @param entry the entry
/// @param rect the badge's square
/// @param clip what drawing may touch
void add_badge(
    Building& building, const Entry& entry, const kit::Rect& rect, const kit::Rect& clip
) {
    // A pack of one map wears that map's minimap everywhere, including the
    // details. A pack of several keeps its badge or its letters.
    if (entry.id.kind == Kind::map_pack) {
        const std::vector<ShownMap> maps = maps_shown(*building.library, entry);
        if (maps.size() == 1) {
            add_fitted_preview(building, maps.front().preview, rect, clip);
            return;
        }
    }
    const std::string_view letters = is_base_game(entry.id) ? base_badge_name : entry_name(entry);
    const oa::ui::frontend_renderer::RgbaPicture picture = badge_picture(entry);
    std::optional<kit::TagIcon> tag;
    if (entry.id.kind == Kind::mutator && !oa::ui::frontend_renderer::picture_drawable(picture))
        tag = mutator_badge_icon(entry);
    add_badge_of(building, picture, letters, rect, clip, tag);
}

/// Returns a map's players and size as one line: "4 players · 16x16".
///
/// @param building the layout
/// @param players how many players it is for
/// @param size its size, as written
/// @return the line; the size alone when the count is empty
std::string players_and_size(const Building& building, int32_t players, std::string_view size) {
    const std::string count = filled(
        building.library->text,
        players == 1 ? words::active_players_one : words::active_players_many,
        {{"count", std::to_string(players)}}
    );
    if (size.empty())
        return count;
    if (count.empty())
        return std::string(size);
    return filled(
        building.library->text,
        words::map_players_size,
        {{"players", count}, {"size", std::string(size)}}
    );
}

/// Appends lines of a text in the small font, one under another, a hint's
/// step apart (hint_step).
///
/// @param[in,out] building the layout
/// @param lines the lines
/// @param left their left column
/// @param top the first line's top row
/// @param width their width
/// @param ink their colour
/// @param clip what drawing may touch
/// @return the row under the last line
int32_t add_lines(
    Building& building,
    const std::vector<std::string>& lines,
    int32_t left,
    int32_t top,
    int32_t width,
    kit::Colour ink,
    const kit::Rect& clip
);

/// Appends a map pack's previews: one large picture for one map, a grid for
/// several, each with its players, size and description. Each map of several
/// has its name over those lines; one map leaves its name out, as the pack's
/// title already heads the details.
///
/// @param[in,out] building the layout
/// @param entry the entry
/// @param left the column
/// @param top the row the first picture starts on
/// @param width the columns they take
/// @param clip what drawing may touch
/// @return the row under the last map, or `top` when the entry is not a map pack
int32_t add_map_previews(
    Building& building,
    const Entry& entry,
    int32_t left,
    int32_t top,
    int32_t width,
    const kit::Rect& clip
) {
    if (entry.id.kind != Kind::map_pack || width <= 0)
        return top;
    const std::vector<ShownMap> maps = maps_shown(*building.library, entry);
    if (maps.empty())
        return top;
    const kit::Metrics& metrics = *building.metrics;
    const bool one = maps.size() == 1;
    const int32_t gap = metrics.card_gap;
    const int32_t columns = one || width < map_preview_cell * 2 + gap ? int32_t{1} : int32_t{2};
    const int32_t cell = one ? std::min(map_preview_large, width) : map_preview_cell;
    const int32_t across =
        columns == 1 ? cell : std::min(cell, std::max((width - gap) / 2, int32_t{0}));
    if (across <= 0)
        return top;
    int32_t row_top = top;
    for (std::size_t index = 0; index < maps.size();) {
        const std::size_t row_count =
            std::min(static_cast<std::size_t>(columns), maps.size() - index);
        int32_t block_height = across;
        std::vector<std::vector<std::string>> descriptions(row_count);
        for (std::size_t place = 0; place < row_count; ++place) {
            const ShownMap& map = maps[index + place];
            std::size_t line_count = one ? 0 : 1;
            if (!map.size.empty() || map.players > 0)
                ++line_count;
            if (!map.description.empty()) {
                descriptions[place] = wrapped(
                    building,
                    kit::FontRole::small,
                    map.description,
                    across,
                    one ? wide_summary_lines : map_preview_lines
                );
                line_count += descriptions[place].size();
            }
            block_height = std::max(
                block_height, across + metrics.hint_gap + hint_lines_height(building, line_count)
            );
        }
        for (std::size_t place = 0; place < row_count; ++place) {
            const ShownMap& map = maps[index + place];
            const int32_t column = left + static_cast<int32_t>(place) * (across + gap);
            add_fitted_preview(building, map.preview, {column, row_top, across, across}, clip);
            int32_t text_top = row_top + across + metrics.hint_gap;
            if (!one) {
                const std::string title = cut(building, kit::FontRole::small, map.name, across);
                if (!title.empty() && width_of(building, kit::FontRole::small, title) <= across) {
                    add_text(
                        building,
                        kit::FontRole::small,
                        title,
                        {column, text_top, across, metrics.small_line},
                        kit::Align::left,
                        kit::colour::text,
                        clip
                    );
                }
                text_top += hint_step(building);
            }
            if (!map.size.empty() || map.players > 0) {
                const std::string meta = players_and_size(building, map.players, map.size);
                const std::string meta_drawn = cut(building, kit::FontRole::small, meta, across);
                if (!meta_drawn.empty() &&
                    width_of(building, kit::FontRole::small, meta_drawn) <= across)
                    add_text(
                        building,
                        kit::FontRole::small,
                        meta_drawn,
                        {column, text_top, across, metrics.small_line},
                        kit::Align::left,
                        kit::colour::hint,
                        clip
                    );
                text_top += hint_step(building);
            }
            if (!descriptions[place].empty())
                add_lines(
                    building,
                    descriptions[place],
                    column,
                    text_top,
                    across,
                    kit::colour::list_text,
                    clip
                );
        }
        index += row_count;
        row_top += block_height;
        if (index < maps.size())
            row_top += gap;
    }
    return row_top + metrics.paragraph_gap;
}

/// A trigger as a W3C code, when the keyboard picture has that key.
///
/// A leading "@" is dropped. One letter becomes KeyQ and the like. A code
/// the picture does not draw, including a mouse or pad code, is left out.
///
/// @param trigger the catalogue's trigger, or a chord's
/// @param shape the keyboard the picture draws
/// @return the code; empty when the picture has no such key
std::string marked_key_code(std::string_view trigger, kit::KeyboardShape shape) {
    if (!trigger.empty() && trigger.front() == '@')
        trigger.remove_prefix(1);
    std::string code(trigger);
    if (code.size() == 1) {
        const char letter = code.front();
        const bool small = letter >= 'a' && letter <= 'z';
        const bool capital = letter >= 'A' && letter <= 'Z';
        if (small || capital) {
            code = "Key";
            code.push_back(small ? static_cast<char>(letter - 'a' + 'A') : letter);
        }
    }
    for (const kit::KeyPlace& place : kit::keyboard_places(shape)) {
        if (place.code == code)
            return code;
    }
    return {};
}

/// The keyboard a picture draws: the layout's, unless that is empty or
/// "any", then the player's, then ANSI.
///
/// @param keyboard the layout's keyboard
/// @param player the player's keyboard
/// @return the shape
kit::KeyboardShape keyboard_shape_of(std::string_view keyboard, std::string_view player) {
    std::string_view which = keyboard;
    if (which.empty() || which == "any")
        which = player;
    if (which == "iso")
        return kit::KeyboardShape::iso;
    if (which == "jis")
        return kit::KeyboardShape::jis;
    return kit::KeyboardShape::ansi;
}

/// Appends the keyboard picture of what a layout changes, and an area whose
/// text names the keys it marks: the main block alone while every key it
/// changes lies in that block, else the whole keyboard. Compact and the wide
/// details both call this before the facts.
///
/// @param[in,out] building the layout
/// @param entry the entry
/// @param left the picture's left column
/// @param top the picture's top row
/// @param width the columns it may use
/// @param clip what drawing may touch
/// @return the row under the picture, or `top` when the entry is not a layout
int32_t add_key_picture(
    Building& building,
    const Entry& entry,
    int32_t left,
    int32_t top,
    int32_t width,
    const kit::Rect& clip
) {
    if (entry.id.kind != Kind::key_layout || width <= 0)
        return top;
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    const KeyLayoutFacts* facts = key_facts(entry);
    const std::string_view keyboard = facts != nullptr ? facts->keyboard : std::string_view{};
    const kit::KeyboardShape shape = keyboard_shape_of(keyboard, library.inputs.player_keyboard);
    kit::KeyboardLook look;
    look.shape = shape;
    std::string named_keys;
    if (facts != nullptr) {
        for (const std::string& trigger : facts->triggers) {
            const std::string code = marked_key_code(trigger, shape);
            if (code.empty())
                continue;
            bool seen = false;
            for (const kit::KeyboardKey& key : look.keys) {
                if (key.code == code) {
                    seen = true;
                    break;
                }
            }
            if (seen)
                continue;
            kit::KeyboardKey key;
            key.code = code;
            key.cap.mark = kit::KeyMark::changed;
            look.keys.push_back(std::move(key));
            if (!named_keys.empty())
                named_keys.push_back(' ');
            named_keys += code;
        }
    }
    // The main block alone, as wide as the details, so its keys' legends can
    // be read, at every class: the layout's own keys are hardest to read
    // where the screen is smallest. The whole keyboard when a key the layout
    // changes lies outside that block.
    bool inside = true;
    for (const kit::KeyboardKey& key : look.keys) {
        for (const kit::KeyPlace& place : kit::keyboard_places(shape)) {
            if (place.code != key.code)
                continue;
            kit::KeyboardLook block;
            block.main_block = true;
            inside = inside && kit::keyboard_draws(place, block);
        }
    }
    look.main_block = inside;
    // The main block's keys are large enough to read, so a changed key is
    // filled with the accent under its legend, as the design marks it,
    // rather than wearing a corner mark over the legend.
    if (inside)
        for (kit::KeyboardKey& key : look.keys) {
            key.cap.mark = kit::KeyMark::none;
            key.cap.held = true;
        }
    const kit::Point unit = kit::keyboard_size(look);
    const int32_t picture_width = width;
    const int32_t picture_height =
        unit.x > 0 ? std::max(picture_width * unit.y / unit.x, int32_t{1}) : int32_t{25};
    const kit::Rect rect{left, top, picture_width, picture_height};
    const std::size_t first_item = building.list.items.size();
    const std::size_t first_control = building.list.controls.size();
    kit::add_keyboard(
        building.list, rect, look, controls::first_keys_picture_key, named("keys.picture")
    );
    for (std::size_t index = first_item; index < building.list.items.size(); ++index)
        building.list.items[index].clip = clip;
    // The picture is one area. The per-key buttons are not controls the
    // player reaches, and they are not named beside the area.
    if (building.list.controls.size() > first_control)
        building.list.controls.resize(first_control);
    add_label(
        building.detail,
        controls::keys_picture,
        kit::intersect(rect, clip),
        unique_name(building, named("keys.picture")),
        named_keys,
        clip,
        controls::details_group
    );
    return top + picture_height + metrics.paragraph_gap;
}

/// Tells whether a fact is the Rules fact.
///
/// @param library the model
/// @param fact the fact
/// @return true for Rules
bool rules_fact(const Library& library, const Fact& fact) {
    return fact.long_label == shown(library.text, words::fact_rules);
}

/// Tells whether a fact is the From fact.
///
/// @param library the model
/// @param fact the fact
/// @return true for From
bool from_fact(const Library& library, const Fact& fact) {
    return fact.long_label == shown(library.text, words::fact_from);
}

/// Appends an entry's facts as label and value columns, from a row.
///
/// Rules and From show their whole value; the others take at most a number
/// of lines, the last cut with "...". A value's lines lie a hint's step
/// apart (hint_step) and the facts a space further, so a wrapped value
/// stands apart from the fact under it. A value that is met ends with the
/// kit's check in the accent, a warning is drawn in the lock colour, and a
/// bad mark in the danger colour.
///
/// @param[in,out] building the layout
/// @param entry the entry
/// @param area the columns the facts take, from their top
/// @param long_form the long labels and values, else the short
/// @param most_lines the most lines of a value other than Rules and From
/// @param clip what drawing may touch
/// @return the row under the last fact
int32_t add_facts(
    Building& building,
    const Entry& entry,
    const kit::Rect& area,
    bool long_form,
    std::size_t most_lines,
    const kit::Rect& clip
) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    const std::vector<Fact> list = facts(library, entry);
    int32_t label_width = 0;
    for (const Fact& fact : list)
        label_width = std::max(
            label_width,
            width_of(building, kit::FontRole::small, long_form ? fact.long_label : fact.short_label)
        );
    const int32_t value_left = area.x + label_width + metrics.label_gap;
    const int32_t value_width = std::max(area.x + area.width - value_left, int32_t{0});
    const std::string_view met = shown(library.text, words::fact_met);
    const int32_t met_width = width_of(building, kit::FontRole::small, met);
    const int32_t space_width = width_of(building, kit::FontRole::small, " ");
    int32_t top = area.y;
    std::size_t fact_index = 0;
    for (const Fact& fact : list) {
        const std::string& label = long_form ? fact.long_label : fact.short_label;
        const std::string& value = long_form ? fact.long_value : fact.short_value;
        const bool whole = rules_fact(library, fact) || from_fact(library, fact);
        const bool good = fact.mark == Mark::good;
        const int32_t room = good ? value_width - met_width - space_width : value_width;
        int32_t text_left = value_left;
        int32_t text_room = room;
        // The From line draws the registry's icon beside the name once the
        // picture is here. While it is missing, the line stays text.
        if (from_fact(library, fact)) {
            const Registry* from_registry = nullptr;
            for (const Registry& candidate : library.inputs.registries) {
                if (candidate.id == entry.id.registry) {
                    from_registry = &candidate;
                    break;
                }
            }
            const oa::ui::frontend_renderer::RgbaPicture from_picture =
                from_registry != nullptr ? registry_picture(*from_registry)
                                         : oa::ui::frontend_renderer::RgbaPicture{};
            if (oa::ui::frontend_renderer::picture_drawable(from_picture)) {
                const int32_t mark_side = metrics.small_line;
                add_item(
                    building.list, kit::Role::picture, {value_left, top, mark_side, mark_side}, clip
                )
                    .look = kit::PictureLook{from_picture};
                const int32_t shift = mark_side + metrics.list_row_gap;
                text_left += shift;
                text_room = text_room > shift ? text_room - shift : 0;
            }
        }
        const std::vector<std::string> lines =
            wrapped(building, kit::FontRole::small, value, text_room, whole ? 0 : most_lines);
        add_text(
            building,
            kit::FontRole::small,
            cut(building, kit::FontRole::small, label, label_width),
            {area.x, top, label_width, metrics.small_line},
            kit::Align::left,
            kit::colour::hint,
            clip
        );
        kit::Colour ink = kit::colour::text;
        if (fact.opens)
            ink = kit::colour::accent;
        else if (fact.mark == Mark::warn)
            ink = kit::colour::lock;
        else if (fact.mark == Mark::bad)
            ink = kit::colour::danger;
        const int32_t step = hint_step(building);
        int32_t line_top = top;
        for (const std::string& line : lines) {
            add_text(
                building,
                kit::FontRole::small,
                line,
                {text_left, line_top, text_room, metrics.small_line},
                kit::Align::left,
                ink,
                clip
            );
            line_top += step;
        }
        if (good) {
            const int32_t last_width =
                lines.empty() ? 0 : width_of(building, kit::FontRole::small, lines.back());
            const int32_t met_top = lines.empty() ? top : line_top - step;
            add_text(
                building,
                kit::FontRole::small,
                std::string(met),
                {text_left + last_width + space_width, met_top, met_width, metrics.small_line},
                kit::Align::left,
                kit::colour::accent,
                clip
            );
        }
        const int32_t lines_height =
            hint_lines_height(building, std::max(lines.size(), std::size_t{1}));
        const int32_t box_width = std::max(value_left + value_width - text_left, int32_t{0});
        const kit::Rect value_box{text_left, top, box_width, lines_height};
        if (rules_fact(library, fact))
            add_label(
                building.detail,
                controls::rules,
                value_box,
                unique_name(building, named("rules")),
                value,
                clip,
                controls::details_group
            );
        else if (from_fact(library, fact))
            add_label(
                building.detail,
                controls::from,
                value_box,
                unique_name(building, named("from")),
                fact.long_value,
                clip,
                controls::details_group
            );
        if (fact.opens && fact_index < controls::most_fact_links) {
            kit::Control link;
            link.id = controls::first_fact_link + static_cast<kit::ControlId>(fact_index);
            link.rect = value_box;
            link.clip = clip;
            link.enabled = true;
            link.focusable = true;
            link.kind = kit::ControlKind::link;
            link.group = controls::details_group;
            link.name = unique_name(building, named("fact.keys." + name_word(fact.long_value)));
            link.text = fact.long_value;
            building.detail.push_back(std::move(link));
            tab_stop(building, building.detail.back());
        }
        ++fact_index;
        top += lines_height + metrics.space;
    }
    return list.empty() ? top : top - metrics.space;
}

/// Appends lines of a text in the small font, one under another, a hint's
/// step apart (hint_step).
///
/// @param[in,out] building the layout
/// @param lines the lines
/// @param left their left column
/// @param top the first line's top row
/// @param width their width
/// @param ink their colour
/// @param clip what drawing may touch
/// @return the row under the last line
int32_t add_lines(
    Building& building,
    const std::vector<std::string>& lines,
    int32_t left,
    int32_t top,
    int32_t width,
    kit::Colour ink,
    const kit::Rect& clip
) {
    const int32_t step = hint_step(building);
    int32_t row = top;
    for (const std::string& line : lines) {
        add_text(
            building,
            kit::FontRole::small,
            line,
            {left, row, width, building.metrics->small_line},
            kit::Align::left,
            ink,
            clip
        );
        row += step;
    }
    return top + hint_lines_height(building, lines.size());
}

/// Returns the colour an entry's status line is drawn in: the lock colour
/// while an update waits or GET or UPDATE cannot act, else the hint.
///
/// @param entry the entry
/// @return the colour
kit::Colour status_colour(const Entry& entry) {
    return entry.state == State::update || !entry.can_act ? kit::colour::lock : kit::colour::hint;
}

/// Appends the details' note (details_note), when the model has one, under a row.
///
/// @param[in,out] building the layout
/// @param left its left column
/// @param top its top row
/// @param width its width
/// @param clip what drawing may touch
/// @param scroll_group its label's scroll area
/// @return the row under it
int32_t add_note(
    Building& building,
    int32_t left,
    int32_t top,
    int32_t width,
    const kit::Rect& clip,
    int32_t scroll_group
) {
    const std::string note = details_note(*building.library);
    if (note.empty())
        return top;
    const std::vector<std::string> lines = wrapped(building, kit::FontRole::small, note, width, 0);
    const int32_t bottom = add_lines(building, lines, left, top, width, kit::colour::hint, clip);
    add_label(
        building.detail,
        controls::note,
        {left, top, width, bottom - top},
        unique_name(building, named("note")),
        note,
        clip,
        scroll_group
    );
    return bottom;
}

/// Appends a package's tags as tag pills, quiet as the tags under a row
/// and as tall, on a wrapping row. Pressing one chooses that tag. Nothing
/// is added when the package has no tags.
///
/// @param[in,out] building the layout
/// @param entry the entry
/// @param left the row's left
/// @param top the row's top
/// @param width the columns the row may use
/// @param clip the part that shows
/// @param scroll_group the scroll area
/// @return the bottom of the row, or top when there are no tags
int32_t add_detail_tags(
    Building& building,
    const Entry& entry,
    int32_t left,
    int32_t top,
    int32_t width,
    const kit::Rect& clip,
    int32_t scroll_group
) {
    const std::vector<std::string>* tags = shown_tags(entry);
    if (tags == nullptr || tags->empty() || width <= 0)
        return top;
    const kit::Metrics& metrics = *building.metrics;
    const int32_t height = chip_height(metrics);
    int32_t x = left;
    int32_t y = top;
    const std::size_t count = std::min(tags->size(), controls::most_detail_tags);
    for (std::size_t index = 0; index < count; ++index) {
        const std::string& tag = (*tags)[index];
        kit::PillLook look;
        look.text = std::string(tag_caption(
            *building.library,
            tag,
            entry.id.kind == Kind::mutator || entry.id.kind == Kind::key_layout
        ));
        if (entry.id.kind == Kind::mutator)
            look.tag = kit::tag_icon_of(tag);
        look.state = kit::PillState::tag;
        const kit::ControlId id = controls::first_detail_tag + static_cast<kit::ControlId>(index);
        look.hovered = hovered(building, id);
        look.focused = focused(building, id);
        const int32_t across = pill_width_of(building, look);
        if (index > 0 && x > left && x + across > left + width) {
            x = left;
            y += height + metrics.chip_gap;
        }
        const kit::Rect rect{x, y, across, height};
        const std::size_t before = building.list.controls.size();
        kit::add_pill(
            building.list,
            rect,
            look,
            id,
            unique_name(building, named("detail.tag." + name_word(tag)))
        );
        building.list.items.back().clip = clip;
        move_controls(building.list, before, building.detail);
        building.detail.back().clip = clip;
        building.detail.back().group = scroll_group;
        tab_stop(building, building.detail.back());
        x += across + metrics.chip_gap;
    }
    return y + height;
}

/// Appends the details pane's parts for an entry, scrolled up by an offset.
///
/// @param[in,out] building the layout
/// @param entry the entry
/// @param view the pane, which clips them
/// @param width the columns they take from the pane's left
/// @param offset how far they are scrolled up, in points
/// @return the content's height, in points
int32_t add_details_content(
    Building& building, const Entry& entry, const kit::Rect& view, int32_t width, int32_t offset
) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    const int32_t first = view.y - offset;
    const int32_t left = view.x;
    const std::size_t first_item = building.list.items.size();

    add_badge(building, entry, {left, first, details_badge_side, details_badge_side}, view);
    const int32_t text_left = left + details_badge_side + metrics.list_row_gap;
    const int32_t text_width = std::max(left + width - text_left, int32_t{0});
    // The state is said once: a key layout's chip says it in words, where it
    // is used while it is the layout in use, and the status line under the
    // name is left out when it would say the same.
    kit::ChipLook chip = entry_chip(library, entry);
    const std::string status_words = status_line(library, entry);
    if (entry.id.kind == Kind::key_layout && !is_built_in_layout(entry.id) &&
        layout_in_use(library, entry))
        chip.text = status_words;
    const bool status_on_chip = !chip.icon && chip.text == status_words;
    const int32_t chip_width = tag_width(building, chip);
    const int32_t chip_tall = chip.icon ? metrics.state_icon_side : chip_height(metrics);
    const std::string name =
        cut(building,
            kit::FontRole::regular,
            entry_name(entry),
            text_width - chip_width - metrics.list_row_gap);
    const int32_t name_width = width_of(building, kit::FontRole::regular, name);
    add_text(
        building,
        kit::FontRole::regular,
        name,
        {text_left, first, name_width, metrics.regular_line},
        kit::Align::left,
        kit::colour::text,
        view
    );
    const kit::Rect chip_rect{
        text_left + name_width + metrics.list_row_gap,
        first + (metrics.regular_line - chip_tall) / 2,
        chip_width,
        chip_tall,
    };
    kit::add_chip(building.list, chip_rect, chip, kit::no_control, {});
    building.list.items.back().clip = view;
    const std::vector<std::string> status =
        status_on_chip
            ? std::vector<std::string>{}
            : wrapped(building, kit::FontRole::small, status_words, text_width, wide_status_lines);
    const int32_t status_top = first + metrics.regular_line + metrics.hint_gap;
    const int32_t status_bottom =
        add_lines(building, status, text_left, status_top, text_width, status_colour(entry), view);
    // Automation reads the status where it is shown: the line, or the chip.
    add_label(
        building.detail,
        controls::status,
        status_on_chip ? chip_rect
                       : kit::Rect{text_left,
                                   status_top,
                                   text_width,
                                   std::max(status_bottom - status_top, metrics.small_line)},
        unique_name(building, named("status")),
        status_on_chip ? status_words : joined(status),
        view,
        controls::details_group
    );
    const int32_t gap = part_gap(metrics);
    int32_t top = first + std::max(details_badge_side, status_bottom - first) + gap;

    // The listing's summary, or a mod of the player's own's description.
    if (const std::string_view about = entry_summary(entry); !about.empty()) {
        const std::vector<std::string> summary =
            wrapped(building, kit::FontRole::small, about, width, wide_summary_lines);
        top = add_lines(building, summary, left, top, width, kit::colour::list_text, view) + gap;
    }
    const int32_t tags_bottom =
        add_detail_tags(building, entry, left, top, width, view, controls::details_group);
    if (tags_bottom != top)
        top = tags_bottom + gap;
    top = add_map_previews(building, entry, left, top, width, view);
    top = add_key_picture(building, entry, left, top, width, view);
    top = add_facts(building, entry, {left, top, width, 0}, true, wide_fact_lines, view) + gap;

    static_cast<void>(add_note(building, left, top, width, view, controls::details_group));
    const std::optional<controls::Extent> extent =
        controls::clipped_extent(building.list, view, first_item);
    return extent ? extent->bottom - first : 0;
}

/// Where the details' actions lie in their bar, as Settings lays out a
/// card's buttons.
struct ActionPlan {
    std::vector<kit::Rect> buttons{}; ///< each action's button, from the bar's top left
    int32_t height{};                 ///< the bar's rows; 0 with no action
};

/// Returns where the details' actions lie in a bar of a width, as Settings
/// lays out its Updates card's buttons: left to right, each as wide as its
/// caption needs (button_width) and at the control height, two spaces
/// apart, and one that does not fit starting a line a space under the one
/// before. One wider than the bar takes the bar.
///
/// @param building the layout
/// @param actions the actions, in the order they are shown
/// @param width the bar's width, in points
/// @return the plan
ActionPlan
plan_actions(const Building& building, const std::vector<ActionButton>& actions, int32_t width) {
    const kit::Metrics& metrics = *building.metrics;
    ActionPlan plan;
    if (actions.empty())
        return plan;
    const int32_t gap = button_spacing(metrics);
    const int32_t button_height = metrics.control_height;
    int32_t x = 0;
    int32_t y = 0;
    for (const ActionButton& action : actions) {
        const int32_t across =
            std::min(button_width(building, action.text), std::max(width, int32_t{0}));
        if (x > 0 && x + across > width) {
            x = 0;
            y += button_height + metrics.space;
        }
        plan.buttons.push_back({x, y, across, button_height});
        x += across + gap;
    }
    plan.height = y + button_height;
    return plan;
}

/// Returns the rows the details' actions take at the pane's foot.
///
/// @param building the layout
/// @param entry the entry
/// @param width the pane's width, in points
/// @return the rows; 0 when it has no action
int32_t detail_actions_height(const Building& building, const Entry& entry, int32_t width) {
    return plan_actions(building, entry_actions(*building.library, entry), width).height;
}

/// Appends the details' actions, as plan_actions places them, in a bar at
/// the pane's foot that does not scroll.
///
/// @param[in,out] building the layout
/// @param entry the entry
/// @param bar the bar
void add_detail_actions(Building& building, const Entry& entry, const kit::Rect& bar) {
    const std::vector<ActionButton> actions = entry_actions(*building.library, entry);
    const ActionPlan plan = plan_actions(building, actions, bar.width);
    for (std::size_t index = 0; index < actions.size() && index < plan.buttons.size(); ++index) {
        const ActionButton& action = actions[index];
        const kit::Rect& place = plan.buttons[index];
        add_button(
            building,
            building.detail,
            {bar.x + place.x, bar.y + place.y, place.width, place.height},
            action.text,
            // UNINSTALL is never drawn as the main button.
            index == 0 && action.action != Action::uninstall,
            action.enabled,
            controls::action_control(action.action),
            named("action." + std::string(action_word(action.action))),
            {},
            -1
        );
    }
}

/// Appends a card's face, as Settings' side card: the inset button's fill
/// and border, and no control, so it never takes a press from the actions.
///
/// @param[in,out] building the layout
/// @param rect the card
void add_card_face(Building& building, const kit::Rect& rect) {
    kit::ButtonLook face;
    face.style = kit::ButtonStyle::inset;
    face.enabled = true;
    add_item(building.list, kit::Role::button, rect, {}).look = std::move(face);
}

/// Appends the details pane of the selected entry in a bordered card. Its
/// parts scroll when they are taller than the room above the actions, and
/// the actions sit at the card's foot, which does not scroll.
///
/// @param[in,out] building the layout
/// @param whole the card
void add_details_pane(Building& building, const kit::Rect& whole) {
    const Entry* entry = selected_entry(*building.library);
    if (entry == nullptr)
        return;
    const kit::Metrics& metrics = *building.metrics;
    const int32_t pad = 2 * metrics.space;
    add_card_face(building, whole);
    const kit::Rect inner{
        whole.x + pad,
        whole.y + pad,
        std::max(whole.width - 2 * pad, int32_t{0}),
        std::max(whole.height - 2 * pad, int32_t{0}),
    };
    const int32_t actions_height = detail_actions_height(building, *entry, inner.width);
    kit::Rect pane = inner;
    if (actions_height > 0)
        pane.height = std::max(inner.height - actions_height - metrics.row_gap, int32_t{0});
    // Laid out once at the pane's width to learn its height, then again
    // narrower when that is taller than the pane.
    Building trial = building;
    trial.list = {};
    trial.detail.clear();
    trial.order.clear();
    const int32_t whole_height = add_details_content(trial, *entry, pane, pane.width, 0);
    const int32_t well_width = metrics.scroll_thumb_width + 2;
    const bool scrolls = whole_height > pane.height;
    const int32_t width = scrolls ? pane.width - well_width - metrics.button_gap : pane.width;
    int32_t height = whole_height;
    if (scrolls) {
        trial = building;
        trial.list = {};
        trial.detail.clear();
        trial.order.clear();
        height = add_details_content(trial, *entry, pane, width, 0);
    }
    const int32_t limit = kit::ScrollArea::limit_for(height, pane.height);
    const int32_t offset = std::clamp(building.state->details_scroll, int32_t{0}, limit);
    add_details_content(building, *entry, pane, width, offset);
    if (limit > 0) {
        kit::ScrollArea scroll;
        scroll.view = pane;
        scroll.well = {pane.x + pane.width - well_width, pane.y, well_width, pane.height};
        scroll.content_height = height;
        scroll.limit = limit;
        scroll.offset = offset;
        add_item(building.list, kit::Role::scroll_bar, scroll.well, {}).look = kit::ScrollBarLook{
            scroll.thumb(metrics.least_thumb_height), building.state->bar_held == Pane::details
        };
    }
    if (actions_height > 0)
        add_detail_actions(
            building,
            *entry,
            {inner.x, inner.y + inner.height - actions_height, inner.width, actions_height}
        );
}

/// The narrowest a tile's words may be with its button beside them, as
/// Settings' key layout card keeps its words; narrower, the button stands
/// under the words.
constexpr int32_t least_beside_words = 120;

/// The side of an Active tile's badge. A list row's badge plus the card's
/// inset on either side, so the badge is larger than a row's and the size
/// is still taken from the kit's metrics.
///
/// @param metrics the class's metrics
/// @return the side, in points
int32_t active_badge_side(const kit::Metrics& metrics) {
    return metrics.list_row_badge + 2 * metrics.card_inset;
}

/// Returns the points between an Active tile's edges and its badge, its
/// words and its button: a space, as a list row keeps above and below its
/// words at Compact, and Settings' row padding at Large.
///
/// @param metrics the class's metrics
/// @return the points
int32_t tile_inset(const kit::Metrics& metrics) {
    return metrics.space;
}

/// Appends a tile's face: the kit card's fill and its outline. Nothing
/// here takes a press.
///
/// @param[in,out] building the layout
/// @param rect the tile
/// @param clip what drawing may touch
void add_tile_face(Building& building, const kit::Rect& rect, const kit::Rect& clip) {
    add_item(building.list, kit::Role::fill, rect, clip).colour = kit::colour::list_selected;
    add_item(building.list, kit::Role::outline, rect, clip).colour = kit::colour::control_border;
}

/// One line of text on a tile.
struct TileLine {
    std::string text{};
    kit::FontRole font{kit::FontRole::small};
    kit::Colour ink{kit::colour::text};
};

/// Returns the tops of a tile's lines from the first one's, as Settings
/// spaces a row's label and its hints: the first line first_height high,
/// a line in the small font hint_gap under a line in the regular font and
/// a hint's step under one in the small font (hint_step).
///
/// @param building the layout
/// @param lines the lines
/// @param first_height the first line's height: its font's line, or the
///        pill's height when a pill stands on it
/// @return each line's top, from 0, and the rows the lines take last
std::vector<int32_t>
tile_line_tops(const Building& building, const std::vector<TileLine>& lines, int32_t first_height) {
    const kit::Metrics& metrics = *building.metrics;
    std::vector<int32_t> tops;
    tops.reserve(lines.size() + 1);
    int32_t row = 0;
    int32_t above = 0; // the height of the line above
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const TileLine& line = lines[index];
        if (index > 0) {
            const bool hints =
                line.font == kit::FontRole::small && lines[index - 1].font == kit::FontRole::small;
            row += above + (hints ? hint_step(building) - metrics.small_line : metrics.hint_gap);
        }
        tops.push_back(row);
        above = index == 0                          ? first_height
                : line.font == kit::FontRole::small ? metrics.small_line
                                                    : metrics.regular_line;
    }
    tops.push_back(lines.empty() ? 0 : row + above);
    return tops;
}

/// Returns the height of a tile's first line: its font's line, and the
/// pill's height when a pill stands on it, as Settings' label line takes
/// the height of the control beside it.
///
/// @param building the layout
/// @param lines the lines
/// @param pill a pill stands on the first line
/// @return the height, in points
int32_t
first_tile_line_height(const Building& building, const std::vector<TileLine>& lines, bool pill) {
    const kit::Metrics& metrics = *building.metrics;
    const int32_t line = lines.empty() || lines.front().font == kit::FontRole::regular
                             ? metrics.regular_line
                             : metrics.small_line;
    return pill ? std::max(line, chip_height(metrics)) : line;
}

/// Tells whether an identity tile's button stands beside its words, as
/// Settings' key layout card's buttons do: while the words keep
/// least_beside_words beside it.
///
/// @param building the layout
/// @param width the tile's width
/// @param caption the button's words; empty for no button
/// @param may_stand_beside false keeps the button under the words
/// @return true beside, false under the words or with no button
bool button_beside(
    const Building& building, int32_t width, std::string_view caption, bool may_stand_beside
) {
    if (caption.empty() || !may_stand_beside)
        return false;
    const kit::Metrics& metrics = *building.metrics;
    const int32_t inset = tile_inset(metrics);
    const int32_t words = width - 2 * inset - active_badge_side(metrics) - metrics.list_row_gap -
                          button_width(building, caption) - button_spacing(metrics);
    return words >= least_beside_words;
}

/// Tells whether the lines after a tile's first fit beside its button
/// when the button stands at the tile's bottom right, under the first line.
///
/// @param building the layout
/// @param lines the lines
/// @param words_width the columns the lines take beside the badge
/// @param caption the button's words
/// @return true when every line after the first fits left of the button
bool lines_fit_beside_button(
    const Building& building,
    const std::vector<TileLine>& lines,
    int32_t words_width,
    std::string_view caption
) {
    const int32_t room =
        words_width - button_width(building, caption) - button_spacing(*building.metrics);
    for (std::size_t index = 1; index < lines.size(); ++index)
        if (width_of(building, lines[index].font, lines[index].text) > room)
            return false;
    return room > 0;
}

/// Returns an identity tile's height. The badge and the lines start at the
/// top inset, the inset under the taller of them. A button beside the
/// words is centred on the badge. Otherwise it stands at the tile's bottom
/// right: hint_gap under the first line when the lines after it fit beside
/// it, else two spaces under every line.
///
/// @param building the layout
/// @param lines the lines beside the badge
/// @param pill a pill stands on the first line
/// @param width the tile's width
/// @param caption the button's words; empty for no button
/// @param may_stand_beside false keeps the button at the bottom right
/// @return the height, in points
int32_t identity_tile_height(
    const Building& building,
    const std::vector<TileLine>& lines,
    bool pill,
    int32_t width,
    std::string_view caption,
    bool may_stand_beside
) {
    const kit::Metrics& metrics = *building.metrics;
    const int32_t inset = tile_inset(metrics);
    const int32_t first_height = first_tile_line_height(building, lines, pill);
    const std::vector<int32_t> tops = tile_line_tops(building, lines, first_height);
    const int32_t side = active_badge_side(metrics);
    int32_t stack = std::max(side, tops.back());
    if (!caption.empty() && !button_beside(building, width, caption, may_stand_beside)) {
        const int32_t words = width - 2 * inset - side - metrics.list_row_gap;
        if (lines_fit_beside_button(building, lines, words, caption))
            stack = std::max(stack, first_height + metrics.hint_gap + metrics.control_height);
        else
            stack += part_gap(metrics) + metrics.control_height;
    }
    return stack + 2 * inset;
}

/// Appends a tile's lines. Each line that stands beside the pill or the
/// button keeps room at its right for it; a line that cannot fit is left
/// out, and its row is kept.
///
/// @param[in,out] building the layout
/// @param lines the lines
/// @param left their left column
/// @param top the first line's top row
/// @param width their width
/// @param first_height the first line's height
/// @param first_room the first line's width, short of the pill
/// @param button the button's rectangle, or an empty one
/// @param clip what drawing may touch
void place_tile_lines(
    Building& building,
    const std::vector<TileLine>& lines,
    int32_t left,
    int32_t top,
    int32_t width,
    int32_t first_height,
    int32_t first_room,
    const kit::Rect& button,
    const kit::Rect& clip
) {
    const kit::Metrics& metrics = *building.metrics;
    const std::vector<int32_t> tops = tile_line_tops(building, lines, first_height);
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const TileLine& line = lines[index];
        const int32_t height = index == 0                            ? first_height
                               : line.font == kit::FontRole::regular ? metrics.regular_line
                                                                     : metrics.small_line;
        const int32_t row = top + tops[index];
        int32_t room = index == 0 ? first_room : width;
        if (button.width > 0 && row < button.y + button.height && row + height > button.y)
            room = std::min(room, button.x - button_spacing(metrics) - left);
        if (room > 0 && !line.text.empty()) {
            const std::string drawn = cut(building, line.font, line.text, room);
            if (width_of(building, line.font, drawn) <= room)
                add_text(
                    building,
                    line.font,
                    drawn,
                    {left, row, room, height},
                    kit::Align::left,
                    line.ink,
                    clip
                );
        }
    }
}

/// Appends a pill that takes no press, at the right of a line and centred
/// down it, when it fits the line's width.
///
/// @param[in,out] building the layout
/// @param line the line it sits on
/// @param words_shown the pill's words
/// @param state the pill's colours
/// @param clip what drawing may touch
/// @return the pill's width, or 0 when it does not fit
int32_t add_idle_pill(
    Building& building,
    const kit::Rect& line,
    const std::string& words_shown,
    kit::PillState state,
    const kit::Rect& clip
) {
    if (words_shown.empty() || line.width <= 0 || line.height <= 0)
        return 0;
    kit::PillLook look;
    look.text = words_shown;
    look.state = state;
    const int32_t width = pill_width_of(building, look);
    const int32_t height = chip_height(*building.metrics);
    if (width > line.width)
        return 0;
    const kit::Rect pill{
        line.x + line.width - width,
        line.y + (line.height - height) / 2,
        width,
        height,
    };
    kit::add_pill(building.list, pill, look, kit::no_control, {});
    building.list.items.back().clip = clip;
    return width;
}

/// Appends an identity tile: a badge and lines from the top inset, a pill
/// at the right of the title's line, and a button. As on Settings' key
/// layout card, the button stands beside the words, centred on the badge,
/// while the words keep their room (button_beside), and under them at the
/// tile's bottom right otherwise. The label is the tile left of or above
/// that button, so the two do not meet, and the label takes no press.
///
/// @param[in,out] building the layout
/// @param tile the tile
/// @param badge the badge; null for the letters
/// @param letters the name the letters are taken from
/// @param lines the lines beside the badge
/// @param pill the pill's words; empty for none
/// @param pill_state the pill's colours
/// @param button the tile holds a button
/// @param may_stand_beside false keeps the button under the words
/// @param button_caption the button's words, looked up
/// @param button_enabled the button takes a press
/// @param button_id the button's number
/// @param button_name the button's name
/// @param id the label's number
/// @param name the label's name
/// @param read the label's text
/// @param clip what drawing may touch
void add_identity_tile(
    Building& building,
    const kit::Rect& tile,
    const Badge* badge,
    std::string_view letters,
    const std::vector<TileLine>& lines,
    const std::string& pill,
    kit::PillState pill_state,
    bool button,
    bool may_stand_beside,
    const std::string& button_caption,
    bool button_enabled,
    kit::ControlId button_id,
    const std::string& button_name,
    kit::ControlId id,
    std::string name,
    std::string read,
    const kit::Rect& clip
) {
    const kit::Metrics& metrics = *building.metrics;
    const int32_t inset = tile_inset(metrics);
    const int32_t side =
        std::min(active_badge_side(metrics), std::max(tile.width - 2 * inset, int32_t{0}));
    add_tile_face(building, tile, clip);
    const int32_t inner_left = tile.x + inset;
    const int32_t inner_top = tile.y + inset;
    if (side > 0)
        add_badge_of(
            building, picture_of(badge), letters, {inner_left, inner_top, side, side}, clip
        );
    const bool has_button = button && !button_caption.empty();
    const bool beside =
        has_button && button_beside(building, tile.width, button_caption, may_stand_beside);
    const int32_t across = has_button ? std::min(
                                            button_width(building, button_caption),
                                            std::max(tile.width - 2 * inset, int32_t{0})
                                        )
                                      : 0;
    const int32_t text_left = inner_left + side + metrics.list_row_gap;
    const int32_t words_right = beside
                                    ? tile.x + tile.width - inset - across - button_spacing(metrics)
                                    : tile.x + tile.width - inset;
    const int32_t text_width = std::max(words_right - text_left, int32_t{0});
    const int32_t text_top = inner_top;
    const int32_t first_height = first_tile_line_height(building, lines, !pill.empty());
    const int32_t pill_width = add_idle_pill(
        building, {text_left, text_top, text_width, first_height}, pill, pill_state, clip
    );
    const int32_t first_room =
        pill_width > 0 ? std::max(text_width - pill_width - metrics.list_row_gap, int32_t{0})
                       : text_width;
    kit::Rect button_rect{};
    if (has_button && across > 0) {
        button_rect = beside ? kit::Rect{
                                   tile.x + tile.width - inset - across,
                                   inner_top + (side - metrics.control_height) / 2,
                                   across,
                                   metrics.control_height,
                               }
                             : kit::Rect{
                                   tile.x + tile.width - inset - across,
                                   tile.y + tile.height - inset - metrics.control_height,
                                   across,
                                   metrics.control_height,
                               };
        if (button_rect.y < tile.y)
            button_rect = {};
    }
    place_tile_lines(
        building,
        lines,
        text_left,
        text_top,
        text_width,
        first_height,
        first_room,
        beside ? kit::Rect{} : button_rect,
        clip
    );
    kit::Rect label = tile;
    if (button_rect.width > 0) {
        add_button(
            building,
            building.detail,
            button_rect,
            button_caption,
            false,
            button_enabled,
            button_id,
            button_name,
            clip,
            -1
        );
        if (beside)
            label.width =
                std::max(button_rect.x - button_spacing(metrics) / 2 - tile.x, int32_t{0});
        else
            label.height = std::max(button_rect.y - tile.y, int32_t{0});
    }
    add_label(
        building.detail, id, kit::intersect(label, clip), std::move(name), std::move(read), clip, -1
    );
}

/// Returns the rows from one small line of a map tile to the next: a
/// hint's step (hint_step), and at least hint_gap clear between them, so
/// the players line, the description and the From line stand apart in
/// either look.
///
/// @param building the layout
/// @return the rows, in points
int32_t map_line_step(const Building& building) {
    const kit::Metrics& metrics = *building.metrics;
    return std::max(hint_step(building), metrics.small_line + metrics.hint_gap);
}

/// Returns one map tile's height: the minimap or the lines beside it,
/// whichever is taller, with the tile's inset above and below. The lines
/// are spaced as Settings spaces a row's label and its hints: the players
/// and size hint_gap under the title, and the description and the From
/// line each a step under the line above (map_line_step). A pack of
/// several maps keeps a From line; a pack of one does not.
///
/// @param building the layout
/// @param side the minimap's side
/// @param described the map has a description to show
/// @param from_line the tile draws "From <pack>"
/// @return the height, in points
int32_t map_tile_height(const Building& building, int32_t side, bool described, bool from_line) {
    const kit::Metrics& metrics = *building.metrics;
    int32_t small_lines = 1;
    if (described)
        ++small_lines;
    if (from_line)
        ++small_lines;
    const int32_t lines = metrics.regular_line + metrics.hint_gap +
                          (small_lines - 1) * map_line_step(building) + metrics.small_line;
    const int32_t inset = tile_inset(metrics);
    return inset + std::max(side, lines) + inset;
}

/// What an identity tile shows. The layout paints it; the model chose the words.
struct IdentityTile {
    const Badge* badge{};
    std::string letters{};
    std::vector<TileLine> lines{};
    std::string pill{};
    kit::PillState pill_state{kit::PillState::neutral};
    bool button{}; ///< a button: beside the words, or at the bottom right
    /// The button may stand beside the words. The mod's stands under them,
    /// at the bottom right of a tile as tall as the expansions, so Tab
    /// reaches it after every expansion.
    bool beside{true};
    std::string button_caption{};
    bool button_enabled{true};
    kit::ControlId button_id{};
    std::string button_name{};
    kit::ControlId id{};
    std::string name{};
    std::string read{};
};

/// Returns the mod tile: the mod being played, or the base game.
///
/// @param[in,out] building the layout, which records the label's name
/// @param setup what the tab shows
/// @return the tile
IdentityTile mod_tile(Building& building, const ActiveSetup& setup) {
    IdentityTile tile;
    tile.badge = setup.mod_badge;
    tile.id = controls::active_mod;
    tile.name = unique_name(building, named("active.mod"));
    tile.read = setup.mod_line;
    if (setup.mod_name.empty()) {
        tile.letters = std::string(base_badge_name);
        if (!setup.base_title.empty())
            tile.lines.push_back(
                TileLine{setup.base_title, kit::FontRole::regular, kit::colour::text}
            );
        if (!setup.base_look.empty())
            tile.lines.push_back(
                TileLine{setup.base_look, kit::FontRole::small, kit::colour::hint}
            );
        // The label reads "No Mod", which is already the title. A different
        // line, when one is set, is drawn under the look.
        if (!setup.mod_line.empty() && setup.mod_line != setup.base_title)
            tile.lines.push_back(TileLine{setup.mod_line, kit::FontRole::small, kit::colour::hint});
    } else {
        tile.letters = setup.mod_name;
        tile.lines.push_back(TileLine{setup.mod_name, kit::FontRole::regular, kit::colour::text});
        const std::string& version =
            setup.mod_release.empty() ? setup.mod_version : setup.mod_release;
        if (!version.empty())
            tile.lines.push_back(TileLine{version, kit::FontRole::small, kit::colour::hint});
        if (!setup.mod_author.empty())
            tile.lines.push_back(
                TileLine{setup.mod_author, kit::FontRole::small, kit::colour::list_text}
            );
        tile.pill = setup.mod_state;
        tile.pill_state = kit::PillState::playing;
        tile.button = true;
        tile.beside = false;
        tile.button_caption = std::string(shown(building.library->text, words::action_unload_mod));
        tile.button_enabled = can_switch(*building.library);
        tile.button_id = controls::active_unload;
        tile.button_name = named("active.unload");
    }
    return tile;
}

/// Returns the language tile.
///
/// @param[in,out] building the layout, which records the label's name
/// @param setup what the tab shows
/// @return the tile
IdentityTile language_tile(Building& building, const ActiveSetup& setup) {
    IdentityTile tile;
    tile.badge = setup.language_badge;
    tile.letters = setup.language_name;
    tile.id = controls::active_language;
    tile.name = unique_name(building, named("active.language"));
    tile.read = setup.language_line;
    tile.pill = setup.language_note;
    tile.pill_state = kit::PillState::neutral;
    if (!setup.language_name.empty())
        tile.lines.push_back(
            TileLine{setup.language_name, kit::FontRole::regular, kit::colour::text}
        );
    if (!setup.language_english.empty())
        tile.lines.push_back(
            TileLine{setup.language_english, kit::FontRole::small, kit::colour::hint}
        );
    if (!setup.language_pack.empty())
        tile.lines.push_back(
            TileLine{setup.language_pack, kit::FontRole::small, kit::colour::list_text}
        );
    if (!setup.language_version.empty())
        tile.lines.push_back(
            TileLine{setup.language_version, kit::FontRole::small, kit::colour::hint}
        );
    if (!setup.language_coverage.empty())
        tile.lines.push_back(
            TileLine{setup.language_coverage, kit::FontRole::small, kit::colour::list_text}
        );
    return tile;
}

/// Returns the KEYS tile: the layout in use, its own-changes line, and
/// "With {mod}" when a per-mod layout is set. CHANGE… opens Keys on it.
///
/// @param[in,out] building the layout, which records the label's name
/// @param setup what the tab shows
/// @return the tile
IdentityTile keys_tile(Building& building, const ActiveSetup& setup) {
    IdentityTile tile;
    tile.letters = setup.keys_name.empty()
                       ? std::string(shown(building.library->text, words::built_in_layout_name))
                       : setup.keys_name;
    tile.id = controls::active_keys;
    tile.name = unique_name(building, named("active.keys"));
    tile.read = setup.keys_name;
    if (!setup.keys_line.empty())
        tile.read += " " + setup.keys_line;
    if (!setup.keys_with.empty())
        tile.read += " " + setup.keys_with;
    if (!setup.keys_name.empty())
        tile.lines.push_back(TileLine{setup.keys_name, kit::FontRole::regular, kit::colour::text});
    if (!setup.keys_line.empty())
        tile.lines.push_back(TileLine{setup.keys_line, kit::FontRole::small, kit::colour::hint});
    if (!setup.keys_with.empty())
        tile.lines.push_back(
            TileLine{setup.keys_with, kit::FontRole::small, kit::colour::list_text}
        );
    tile.pill = setup.keys_badge;
    tile.pill_state = setup.keys_badge == shown(building.library->text, words::chip_using)
                          ? kit::PillState::playing
                          : kit::PillState::neutral;
    tile.button = true;
    tile.button_caption = std::string(shown(building.library->text, words::active_change));
    tile.button_enabled = true;
    tile.button_id = controls::active_keys_change;
    tile.button_name = named("active.keys.change");
    return tile;
}

/// Returns an identity tile's height at this class and a width.
///
/// @param building the layout
/// @param tile the tile
/// @param width the tile's width
/// @return the height, in points
int32_t identity_height(const Building& building, const IdentityTile& tile, int32_t width) {
    return identity_tile_height(
        building,
        tile.lines,
        !tile.pill.empty(),
        width,
        tile.button ? std::string_view(tile.button_caption) : std::string_view{},
        tile.beside
    );
}

/// Appends an identity tile.
///
/// @param[in,out] building the layout
/// @param tile the tile
/// @param rect where it sits
/// @param clip what drawing may touch
void place_identity(
    Building& building, const IdentityTile& tile, const kit::Rect& rect, const kit::Rect& clip
) {
    add_identity_tile(
        building,
        rect,
        tile.badge,
        tile.letters,
        tile.lines,
        tile.pill,
        tile.pill_state,
        tile.button,
        tile.beside,
        tile.button_caption,
        tile.button_enabled,
        tile.button_id,
        tile.button_name,
        tile.id,
        tile.name,
        tile.read,
        clip
    );
}

/// Appends one installed map's tile: its minimap, title, players and size,
/// its description cut to one line, and the link to its pack. A pack of
/// several maps keeps that link on a From line. A pack of one map leaves
/// the line out and makes the title the link, in the same accent.
///
/// @param[in,out] building the layout
/// @param map the map
/// @param tile the tile
/// @param number the map's place among the tiles, from 1
/// @param clip what drawing may touch
void add_map_tile(
    Building& building,
    const ActiveMap& map,
    const kit::Rect& tile,
    std::size_t number,
    const kit::Rect& clip
) {
    const kit::Metrics& metrics = *building.metrics;
    const int32_t inset = tile_inset(metrics);
    const int32_t side =
        std::min(active_badge_side(metrics), std::max(tile.width - 2 * inset, int32_t{0}));
    add_tile_face(building, tile, clip);
    const int32_t inner_left = tile.x + inset;
    const int32_t inner_top = tile.y + inset;
    if (side > 0)
        add_fitted_preview(building, map.preview, {inner_left, inner_top, side, side}, clip);
    const int32_t text_left = inner_left + side + metrics.list_row_gap;
    const int32_t text_width = std::max(tile.x + tile.width - inset - text_left, int32_t{0});
    if (text_width <= 0)
        return;
    int32_t row = inner_top;
    const bool title_links = map.from.empty();
    const std::string title = cut(building, kit::FontRole::regular, map.name, text_width);
    const bool title_fits =
        !title.empty() && width_of(building, kit::FontRole::regular, title) <= text_width;
    const kit::ControlId link_id =
        controls::first_active_from + static_cast<kit::ControlId>(number - 1);
    // The title of a one-map pack is the pack's link: the accent, lighter
    // while the pointer is over it, with the focus ring when it is focused.
    const bool title_is_link = title_links && number <= controls::most_active_maps && title_fits;
    kit::Colour title_ink = kit::colour::text;
    if (title_is_link) {
        const bool over =
            hovered(building, link_id) || building.state->interaction.pressed == link_id;
        title_ink = over ? kit::colour::accent_light : kit::colour::accent;
    }
    if (title_fits)
        add_text(
            building,
            kit::FontRole::regular,
            title,
            {text_left, row, text_width, metrics.regular_line},
            kit::Align::left,
            title_ink,
            clip
        );
    if (title_is_link) {
        const int32_t link_across =
            std::min(width_of(building, kit::FontRole::regular, title), text_width);
        const kit::Rect link_rect =
            kit::intersect({text_left, row, link_across, metrics.regular_line}, clip);
        if (link_rect.width > 0 && link_rect.height > 0) {
            kit::LinkLook link;
            link.caption = title;
            link.hovered =
                hovered(building, link_id) || building.state->interaction.pressed == link_id;
            const std::size_t items_before = building.list.items.size();
            const std::size_t controls_before = building.list.controls.size();
            kit::add_link(
                building.list,
                link_rect,
                link,
                link_id,
                unique_name(building, named("active.from." + std::to_string(number)))
            );
            // The kit draws a link in the small font. The title stays in the
            // regular font, already drawn above, so the link item is dropped
            // and only its control remains.
            building.list.items.resize(items_before);
            move_controls(building.list, controls_before, building.detail);
            kit::Control& control = building.detail.back();
            control.clip = clip;
            control.group = controls::maps_group;
            control.focusable = true;
            control.enabled = true;
            tab_stop(building, control);
        }
    }
    const int32_t step = map_line_step(building);
    if (number <= controls::most_active_maps) {
        // The composed line stays a label. When the title is the link, the
        // label sits on the players-and-size line so the two do not meet.
        const int32_t label_top =
            title_is_link ? row + metrics.regular_line + metrics.hint_gap : row;
        const int32_t label_height = title_is_link ? metrics.small_line : metrics.regular_line;
        add_label(
            building.detail,
            controls::first_active_map + static_cast<kit::ControlId>(number - 1),
            kit::intersect({text_left, label_top, text_width, label_height}, clip),
            unique_name(building, named("active.map." + std::to_string(number))),
            map.line,
            clip,
            controls::maps_group
        );
    }
    row += metrics.regular_line + metrics.hint_gap;
    const std::string meta = filled(
        building.library->text,
        words::map_players_size,
        {{"players", map.players}, {"size", map.size}}
    );
    const std::string meta_drawn = cut(building, kit::FontRole::small, meta, text_width);
    if (!meta_drawn.empty() && width_of(building, kit::FontRole::small, meta_drawn) <= text_width)
        add_text(
            building,
            kit::FontRole::small,
            meta_drawn,
            {text_left, row, text_width, metrics.small_line},
            kit::Align::left,
            kit::colour::hint,
            clip
        );
    row += step;
    if (!map.description.empty()) {
        const std::string described =
            cut(building, kit::FontRole::small, map.description, text_width);
        if (!described.empty() && width_of(building, kit::FontRole::small, described) <= text_width)
            add_text(
                building,
                kit::FontRole::small,
                described,
                {text_left, row, text_width, metrics.small_line},
                kit::Align::left,
                kit::colour::list_text,
                clip
            );
        row += step;
    }
    if (number > controls::most_active_maps || map.from.empty())
        return;
    kit::LinkLook link;
    link.caption = cut(building, kit::FontRole::small, map.from, text_width);
    if (link.caption.empty() || width_of(building, kit::FontRole::small, link.caption) > text_width)
        return;
    const kit::ControlId id = controls::first_active_from + static_cast<kit::ControlId>(number - 1);
    link.hovered = hovered(building, id) || building.state->interaction.pressed == id;
    const int32_t link_width = std::min(kit::link_width(*building.fonts, link), text_width);
    const kit::Rect link_rect =
        kit::intersect({text_left, row, link_width, metrics.small_line}, clip);
    // A link scrolled out of the area is left out: a zero-wide caption would
    // not fit its box, and an off-screen stop would fail the arrow check.
    if (link_rect.width <= 0 || link_rect.height <= 0)
        return;
    const std::size_t before = building.list.controls.size();
    kit::add_link(
        building.list,
        link_rect,
        link,
        id,
        unique_name(building, named("active.from." + std::to_string(number)))
    );
    building.list.items.back().clip = clip;
    move_controls(building.list, before, building.detail);
    kit::Control& control = building.detail.back();
    control.clip = clip;
    control.group = controls::maps_group;
    control.focusable = true;
    control.enabled = true;
    tab_stop(building, control);
}

/// Returns a heading cut so its tracked width fits.
///
/// @param building the layout
/// @param words_shown the heading, looked up
/// @param width the room, in points
/// @return the heading, with "..." when it does not fit
std::string cut_heading(const Building& building, std::string_view words_shown, int32_t width) {
    const kit::Metrics& metrics = *building.metrics;
    const auto fits = [&](std::string_view text) {
        return kit::tracked_width(
                   *building.fonts, kit::FontRole::small, text, metrics.heading_tracking
               ) <= width;
    };
    if (width <= 0)
        return {};
    if (fits(words_shown))
        return std::string(words_shown);
    std::string kept;
    std::string next;
    for (std::size_t index = 0; index < words_shown.size();) {
        const std::size_t bytes =
            std::max(kit::character_bytes(words_shown.substr(index)), std::size_t{1});
        next.append(words_shown.substr(index, bytes));
        const std::string trial = next + "...";
        if (!fits(trial))
            break;
        kept = trial;
        index += bytes;
    }
    if (kept.empty() || !fits(kept))
        return {};
    return kept;
}

/// Appends Compact's KEYS line and returns the row under it.
///
/// A full tile here pushes an expansion pill off the 640 by 480 page once
/// the maps scroll into view. The line keeps the heading above it and puts
/// the layout's name, its own-changes line and CHANGE… on one row, with no
/// badge. The label reads the whole of that. The words drawn are cut to the
/// room left of the button.
///
/// @param[in,out] building the layout
/// @param keys the tile's words and its button
/// @param left the line's left column
/// @param top the line's top row, under the KEYS heading
/// @param width the line's width
/// @param clip what drawing may touch
/// @return the row under the line and the gap after it
int32_t place_compact_keys_line(
    Building& building,
    const IdentityTile& keys,
    int32_t left,
    int32_t top,
    int32_t width,
    const kit::Rect& clip
) {
    const kit::Metrics& metrics = *building.metrics;
    const int32_t row_height = std::max(metrics.regular_line, metrics.control_height);
    const int32_t across =
        std::min(button_width(building, keys.button_caption), std::max(width, int32_t{0}));
    const int32_t button_x = left + std::max(width - across, int32_t{0});
    const int32_t text_right = std::max(button_x - metrics.list_row_gap, left);
    const int32_t text_width = std::max(text_right - left, int32_t{0});
    const kit::Rect text_box{left, top, text_width, row_height};
    if (text_width > 0 && !keys.read.empty()) {
        add_text(
            building,
            kit::FontRole::regular,
            cut(building, kit::FontRole::regular, keys.read, text_width),
            text_box,
            kit::Align::left,
            kit::colour::text,
            clip
        );
    }
    if (across > 0 && !keys.button_caption.empty()) {
        const int32_t button_y =
            top + std::max((row_height - metrics.control_height) / 2, int32_t{0});
        add_button(
            building,
            building.detail,
            {button_x, button_y, across, metrics.control_height},
            keys.button_caption,
            false,
            keys.button_enabled,
            keys.button_id,
            keys.button_name,
            clip,
            -1
        );
    }
    add_label(
        building.detail, keys.id, kit::intersect(text_box, clip), keys.name, keys.read, clip, -1
    );
    return top + row_height + metrics.paragraph_gap;
}

/// Appends a heading of the Active tab, and returns the row under it.
///
/// @param[in,out] building the layout
/// @param words_shown the heading, looked up
/// @param left its left column
/// @param top its top row
/// @param width its width
/// @param clip what drawing may touch
/// @return the row under it and the gap after it
int32_t add_active_heading(
    Building& building,
    std::string_view words_shown,
    int32_t left,
    int32_t top,
    int32_t width,
    const kit::Rect& clip
) {
    const kit::Metrics& metrics = *building.metrics;
    const std::string drawn = cut_heading(building, words_shown, width);
    kit::Item& heading =
        add_item(building.list, kit::Role::heading, {left, top, width, metrics.small_line}, clip);
    heading.text = drawn;
    heading.look = kit::HeadingLook{drawn};
    return top + metrics.small_line + metrics.hint_gap;
}

/// Folds the Active tab's maps search, with spaces at either end left out.
/// An empty result is no filter.
///
/// @param state the screen's state
/// @return the folded search
std::string maps_query_folded(const ScreenState& state) {
    std::string_view text = state.maps_search.text;
    while (!text.empty() && text.front() == ' ')
        text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ')
        text.remove_suffix(1);
    return folded(capped_query(text));
}

/// Tells whether a map's name or its pack's name holds the folded search.
///
/// @param map the map
/// @param query the folded search; empty matches every map
/// @return true when the tile stays
bool map_matches_query(const ActiveMap& map, std::string_view query) {
    if (query.empty())
        return true;
    if (folded(map.name).find(query) != std::string::npos)
        return true;
    return folded(map.pack_name).find(query) != std::string::npos;
}

/// One row of map tiles, measured before it is placed.
struct MapRowGeom {
    int32_t top{};       ///< the row's top, from the pane's content top
    int32_t height{};    ///< the row's height
    std::size_t begin{}; ///< the first map of the row, among the maps that match
    std::size_t count{}; ///< how many tiles the row holds
};

/// The maps pane's rows at one width.
struct MapGrid {
    int32_t columns{1};      ///< one, or two at Large when more than one map matches
    int32_t left_width{};    ///< the left column's width; the only width when there is one
    int32_t gap{};           ///< the columns' gap, and the rows' gap
    int32_t content_width{}; ///< the width the tiles take
    int32_t height{};        ///< the rows' height, gaps included
    std::vector<MapRowGeom> rows{};
};

/// Returns a tile's width in a measured grid.
///
/// @param grid the grid
/// @param place the map's place among the maps that match, from 0
/// @return the width, in points
int32_t tile_across(const MapGrid& grid, std::size_t place) {
    if (grid.columns == 1 || place % 2 == 0)
        return grid.left_width;
    return std::max(grid.content_width - grid.left_width - grid.gap, int32_t{0});
}

/// Returns a tile's left column in a measured grid.
///
/// @param grid the grid
/// @param pane_left the pane's left column
/// @param place the map's place among the maps that match, from 0
/// @return the column, in points
int32_t tile_column(const MapGrid& grid, int32_t pane_left, std::size_t place) {
    if (grid.columns == 1 || place % 2 == 0)
        return pane_left;
    return pane_left + grid.left_width + grid.gap;
}

/// Measures the maps that match, without appending them. At Compact each
/// map is one line, the small font high, with no gap, so several fit in
/// the pane. At Regular and Large a tile's height follows its width, so a
/// narrower pane is measured again.
///
/// @param building the layout
/// @param setup what the tab shows
/// @param shown each matching map's place in the setup, from 0
/// @param width the width the tiles take
/// @return the rows
MapGrid measure_map_grid(
    const Building& building,
    const ActiveSetup& setup,
    const std::vector<std::size_t>& shown,
    int32_t width
) {
    const kit::Metrics& metrics = *building.metrics;
    const bool one_line = building.size_class == kit::SizeClass::compact;
    MapGrid grid;
    grid.content_width = std::max(width, int32_t{0});
    grid.gap = one_line ? 0 : metrics.card_gap;
    grid.columns =
        !one_line && building.size_class == kit::SizeClass::large && shown.size() > 1 ? 2 : 1;
    grid.left_width = grid.columns == 1 ? grid.content_width
                                        : std::max((grid.content_width - grid.gap) / 2, int32_t{0});
    std::size_t index = 0;
    int32_t top = 0;
    while (index < shown.size()) {
        const std::size_t count =
            std::min(static_cast<std::size_t>(grid.columns), shown.size() - index);
        int32_t row_height = one_line ? metrics.small_line : 0;
        if (!one_line) {
            for (std::size_t place = 0; place < count; ++place) {
                const ActiveMap& map = setup.maps[shown[index + place]];
                const int32_t across = tile_across(grid, index + place);
                const int32_t side = std::min(
                    active_badge_side(metrics),
                    std::max(across - 2 * tile_inset(metrics), int32_t{0})
                );
                row_height = std::max(
                    row_height,
                    map_tile_height(building, side, !map.description.empty(), !map.from.empty())
                );
            }
        }
        grid.rows.push_back(MapRowGeom{top, row_height, index, count});
        top += row_height;
        index += count;
        if (index < shown.size())
            top += grid.gap;
    }
    grid.height = top;
    return grid;
}

/// Appends a one-point mark of the maps' content, clipped to the pane, so
/// the pane's extent is the whole content when that edge is scrolled away.
/// A mark inside the pane is left out: it would paint.
///
/// @param[in,out] list the display list
/// @param pane the pane
/// @param row the mark's row
void add_extent_edge(kit::DisplayList& list, const kit::Rect& pane, int32_t row) {
    if (row >= pane.y && row < pane.y + pane.height)
        return;
    add_item(list, kit::Role::fill, {pane.x, row, 1, 1}, pane).colour = kit::colour::well;
}

/// Appends the maps search at the right of the MAPS heading.
///
/// @param[in,out] building the layout
/// @param rect the field
/// @param clip what drawing may touch
void add_maps_search(Building& building, const kit::Rect& rect, const kit::Rect& clip) {
    const Library& library = *building.library;
    kit::SearchLook look;
    look.field = building.state->maps_search;
    look.focused = focused(building, controls::active_search);
    look.hovered = hovered(building, controls::active_search);
    look.placeholder =
        cut(building,
            kit::FontRole::regular,
            shown(library.text, words::search_active_maps),
            kit::field_text_rect(rect, look).width);
    const std::size_t before = building.list.controls.size();
    kit::add_search(
        building.list,
        rect,
        look,
        controls::active_search,
        unique_name(building, named("active.search"))
    );
    building.list.items.back().clip = clip;
    move_controls(building.list, before, building.detail);
    kit::Control& added = building.detail.back();
    added.clip = clip;
    added.group = -1;
    added.label = std::string(shown(library.text, words::search_active_maps));
    tab_stop(building, added);
}

/// Appends the MAPS heading and, at its right, the maps search, a control
/// high as the other tabs' search is. While the search holds text, the count
/// of tiles shown sits after the heading.
///
/// @param[in,out] building the layout
/// @param left the row's left column
/// @param top the row's top
/// @param width the row's width
/// @param clip what drawing may touch
/// @param query the folded search; empty shows no count
/// @param shown_count how many tiles the search leaves
/// @param installed_count how many maps are installed
void add_maps_heading_row(
    Building& building,
    int32_t left,
    int32_t top,
    int32_t width,
    const kit::Rect& clip,
    std::string_view query,
    std::size_t shown_count,
    std::size_t installed_count
) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    const int32_t gap = metrics.list_row_gap;
    const std::string heading_words(shown(library.text, words::active_heading_maps));
    const int32_t heading_width = kit::tracked_width(
        *building.fonts, kit::FontRole::small, heading_words, metrics.heading_tracking
    );
    std::string count_words;
    int32_t count_width = 0;
    if (!query.empty()) {
        count_words = filled(
            library.text,
            words::active_maps_count,
            {{"shown", std::to_string(shown_count)}, {"all", std::to_string(installed_count)}}
        );
        count_width = width_of(building, kit::FontRole::small, count_words);
    }
    const int32_t beside = heading_width + gap + (count_width > 0 ? count_width + gap : 0) + gap;
    const int32_t field_width =
        std::max(std::min(maps_search_width, std::max(width - beside, int32_t{0})), int32_t{0});
    const int32_t field_x = left + std::max(width - field_width, int32_t{0});
    const int32_t text_top = top + std::max((metrics.control_height - metrics.small_line) / 2, 0);
    const int32_t heading_room =
        std::max(std::min(heading_width, std::max(field_x - gap - left, int32_t{0})), int32_t{0});
    if (heading_room > 0) {
        kit::Item& heading = add_item(
            building.list,
            kit::Role::heading,
            {left, text_top, heading_room, metrics.small_line},
            clip
        );
        heading.text = heading_words;
        heading.look = kit::HeadingLook{heading_words};
    }
    if (count_width > 0) {
        const int32_t count_left = left + heading_width + gap;
        const int32_t count_room = std::max(field_x - gap - count_left, int32_t{0});
        const std::string drawn = cut(building, kit::FontRole::small, count_words, count_room);
        if (count_room > 0 && width_of(building, kit::FontRole::small, drawn) <= count_room)
            add_text(
                building,
                kit::FontRole::small,
                drawn,
                {count_left, text_top, count_room, metrics.small_line},
                kit::Align::left,
                kit::colour::hint,
                clip
            );
    }
    if (field_width > 0)
        add_maps_search(building, {field_x, top, field_width, metrics.control_height}, clip);
}

/// Appends the line the maps pane shows when it has no tile.
///
/// @param[in,out] building the layout
/// @param words_shown the line, looked up
/// @param pane where the line sits
/// @param width the tile's width
/// @param clip what drawing may touch
void add_maps_note(
    Building& building,
    std::string_view words_shown,
    const kit::Rect& pane,
    int32_t width,
    const kit::Rect& clip
) {
    const kit::Metrics& metrics = *building.metrics;
    const int32_t inset = metrics.card_inset;
    const int32_t inner = std::max(width - 2 * inset, int32_t{0});
    const int32_t height = metrics.small_line + 2 * inset;
    const kit::Rect tile{pane.x, pane.y, width, height};
    add_tile_face(building, tile, clip);
    if (inner <= 0)
        return;
    const std::string drawn = cut(building, kit::FontRole::small, words_shown, inner);
    if (width_of(building, kit::FontRole::small, drawn) <= inner)
        add_text(
            building,
            kit::FontRole::small,
            drawn,
            {pane.x + inset, pane.y + inset, inner, metrics.small_line},
            kit::Align::left,
            kit::colour::hint,
            clip
        );
}

/// Appends one installed map on Compact: one line of its name, its players
/// and its size. A pack of several maps keeps the pack's link at the right
/// of that line. A pack of one map leaves the link out and makes the name
/// the link, in the same accent. The composed line stays a label, on the
/// part of the line that is not the link, so the two do not meet.
///
/// @param[in,out] building the layout
/// @param map the map
/// @param row the line
/// @param number the map's place among the maps, from 1
/// @param clip what drawing may touch
void add_compact_map_row(
    Building& building,
    const ActiveMap& map,
    const kit::Rect& row,
    std::size_t number,
    const kit::Rect& clip
) {
    const kit::Metrics& metrics = *building.metrics;
    if (row.width <= 0 || row.height <= 0)
        return;
    const int32_t gap = metrics.list_row_gap;
    const bool named_map = number <= controls::most_active_maps;
    const bool title_links = map.from.empty();
    const kit::ControlId link_id =
        controls::first_active_from + static_cast<kit::ControlId>(number - 1);

    int32_t from_width = 0;
    std::string from_caption;
    if (!title_links && named_map && !map.from.empty()) {
        const int32_t name_full = width_of(building, kit::FontRole::small, map.name);
        const int32_t from_full = width_of(building, kit::FontRole::small, map.from);
        // The name keeps a share of the line. The pack link takes its own
        // width beside it, cut when the two cannot both be whole.
        int32_t from_room = 0;
        if (from_full > 0 && name_full + gap < row.width)
            from_room = std::min(from_full, row.width - name_full - gap);
        else if (from_full > 0 && row.width > gap)
            from_room = std::max((row.width - gap) / 2, int32_t{0});
        if (from_room > 0) {
            from_caption = cut(building, kit::FontRole::small, map.from, from_room);
            if (!from_caption.empty() &&
                width_of(building, kit::FontRole::small, from_caption) <= from_room)
                from_width = width_of(building, kit::FontRole::small, from_caption);
            else
                from_caption.clear();
        }
    }
    const int32_t reading =
        from_width > 0 ? std::max(row.width - from_width - gap, int32_t{0}) : row.width;
    if (reading <= 0)
        return;

    const std::string title = cut(building, kit::FontRole::small, map.name, reading);
    const int32_t title_width = width_of(building, kit::FontRole::small, title);
    const bool title_fits = !title.empty() && title_width <= reading;
    const bool title_is_link = title_links && named_map && title_fits;
    kit::Colour title_ink = kit::colour::text;
    if (title_is_link) {
        const bool over =
            hovered(building, link_id) || building.state->interaction.pressed == link_id;
        title_ink = over ? kit::colour::accent_light : kit::colour::accent;
    }
    if (title_fits)
        add_text(
            building,
            kit::FontRole::small,
            title,
            {row.x, row.y, title_width, metrics.small_line},
            kit::Align::left,
            title_ink,
            clip
        );
    if (title_is_link) {
        const kit::Rect full_link{row.x, row.y, title_width, metrics.small_line};
        const kit::Rect link_rect = kit::intersect(full_link, clip);
        if (link_rect.width > 0 && link_rect.height > 0) {
            kit::LinkLook link;
            link.caption = title;
            link.hovered =
                hovered(building, link_id) || building.state->interaction.pressed == link_id;
            const std::size_t items_before = building.list.items.size();
            const std::size_t controls_before = building.list.controls.size();
            kit::add_link(
                building.list,
                full_link,
                link,
                link_id,
                unique_name(building, named("active.from." + std::to_string(number)))
            );
            // The name is already drawn above, in the small font the link
            // would use, so the link item is dropped and only its control
            // remains. The control keeps the whole line, so moving the focus
            // onto a line that only partly shows can scroll the rest in.
            building.list.items.resize(items_before);
            move_controls(building.list, controls_before, building.detail);
            kit::Control& control = building.detail.back();
            control.clip = clip;
            control.group = controls::maps_group;
            control.focusable = true;
            control.enabled = true;
            tab_stop(building, control);
        }
    }

    std::string suffix;
    if (map.line.size() >= map.name.size() && map.line.compare(0, map.name.size(), map.name) == 0)
        suffix = map.line.substr(map.name.size());
    const int32_t suffix_left = row.x + (title_fits ? title_width : 0);
    const int32_t suffix_room = std::max(row.x + reading - suffix_left, int32_t{0});
    if (suffix_room > 0 && !suffix.empty()) {
        const std::string drawn = cut(building, kit::FontRole::small, suffix, suffix_room);
        if (!drawn.empty() && width_of(building, kit::FontRole::small, drawn) <= suffix_room)
            add_text(
                building,
                kit::FontRole::small,
                drawn,
                {suffix_left, row.y, suffix_room, metrics.small_line},
                kit::Align::left,
                kit::colour::hint,
                clip
            );
    }
    if (named_map) {
        // When the name is the link, the label is the players and size, so
        // it does not meet the link. Otherwise it covers the reading line.
        const kit::Rect label_rect =
            title_is_link ? kit::Rect{suffix_left, row.y, suffix_room, metrics.small_line}
                          : kit::Rect{row.x, row.y, reading, metrics.small_line};
        const kit::Rect shown_label = kit::intersect(label_rect, clip);
        if (shown_label.width > 0 && shown_label.height > 0)
            add_label(
                building.detail,
                controls::first_active_map + static_cast<kit::ControlId>(number - 1),
                label_rect,
                unique_name(building, named("active.map." + std::to_string(number))),
                map.line,
                clip,
                controls::maps_group
            );
    }
    if (from_width <= 0 || from_caption.empty())
        return;
    kit::LinkLook link;
    link.caption = from_caption;
    link.hovered = hovered(building, link_id) || building.state->interaction.pressed == link_id;
    const kit::Rect full_from{
        row.x + row.width - from_width, row.y, from_width, metrics.small_line
    };
    const kit::Rect link_rect = kit::intersect(full_from, clip);
    if (link_rect.width <= 0 || link_rect.height <= 0)
        return;
    const std::size_t before = building.list.controls.size();
    kit::add_link(
        building.list,
        full_from,
        link,
        link_id,
        unique_name(building, named("active.from." + std::to_string(number)))
    );
    building.list.items.back().clip = clip;
    move_controls(building.list, before, building.detail);
    kit::Control& control = building.detail.back();
    control.clip = clip;
    control.group = controls::maps_group;
    control.focusable = true;
    control.enabled = true;
    tab_stop(building, control);
}

/// Appends the map rows that meet the clip. The rest are left out. When the
/// clip is the pane, marks at the content's edges, outside that pane, keep
/// the scroll extent equal to the measured height. Each row keeps its place
/// in the full list, so a pack link still opens that map's pack.
///
/// @param[in,out] building the layout
/// @param setup what the tab shows
/// @param shown each matching map's place in the setup, from 0
/// @param grid the rows, measured at the tiles' width
/// @param pane where the rows sit when the offset is 0
/// @param offset how far the tiles are scrolled up, in points
/// @param clip what drawing may touch
void add_visible_map_rows(
    Building& building,
    const ActiveSetup& setup,
    const std::vector<std::size_t>& shown,
    const MapGrid& grid,
    const kit::Rect& pane,
    int32_t offset,
    const kit::Rect& clip
) {
    const int32_t content_top = pane.y - offset;
    if (clip.y == pane.y && clip.height == pane.height && grid.height > pane.height) {
        add_extent_edge(building.list, pane, content_top);
        add_extent_edge(building.list, pane, content_top + grid.height - 1);
    }
    const bool pane_is_clip = clip.x == pane.x && clip.y == pane.y && clip.width == pane.width &&
                              clip.height == pane.height;
    for (const MapRowGeom& row : grid.rows) {
        const int32_t placed = content_top + row.top;
        if (placed >= clip.y + clip.height || placed + row.height <= clip.y)
            continue;
        // On the scrolling page a row is drawn whole or not at all, so a line
        // is never cut by the footer and two captions never share a pixel.
        // The maps pane still draws the slice of a tile that meets its edge.
        if (!pane_is_clip && (placed < clip.y || placed + row.height > clip.y + clip.height))
            continue;
        for (std::size_t place = 0; place < row.count; ++place) {
            const std::size_t shown_index = row.begin + place;
            const std::size_t map_index = shown[shown_index];
            const kit::Rect tile{
                tile_column(grid, pane.x, shown_index),
                placed,
                tile_across(grid, shown_index),
                row.height,
            };
            if (building.size_class == kit::SizeClass::compact)
                add_compact_map_row(building, setup.maps[map_index], tile, map_index + 1, clip);
            else
                add_map_tile(building, setup.maps[map_index], tile, map_index + 1, clip);
        }
    }
}

/// Returns the base game tile: its icon or "TA", its name, version and check,
/// and CHANGE….
///
/// @param[in,out] building the layout, which records the label's name
/// @param content what the tile shows
/// @return the tile
IdentityTile base_tile(Building& building, const ActiveContent& content) {
    IdentityTile tile;
    tile.badge = content.badge;
    tile.letters = content.letters;
    tile.id = controls::active_base;
    tile.name = unique_name(building, named("active.base"));
    tile.read = content.read;
    if (!content.name.empty())
        tile.lines.push_back(TileLine{content.name, kit::FontRole::regular, kit::colour::text});
    if (!content.version_line.empty())
        tile.lines.push_back(
            TileLine{content.version_line, kit::FontRole::small, kit::colour::hint}
        );
    if (!content.check_line.empty()) {
        const kit::Colour ink = content.check_good ? kit::colour::accent : kit::colour::lock;
        tile.lines.push_back(TileLine{content.check_line, kit::FontRole::small, ink});
    }
    tile.button = true;
    tile.button_caption = std::string(shown(building.library->text, words::active_change));
    tile.button_enabled = content.change_enabled;
    tile.button_id = controls::active_change;
    tile.button_name = named("active.change");
    return tile;
}

/// Returns a pill's words cut so the pill fits a width. An icon is counted
/// in the width, as the pill draws it.
///
/// @param building the layout
/// @param text the pill's words
/// @param room the width, in points
/// @param icon the icon inside the pill; none for the text alone
/// @return the words, with "..." when they do not fit
std::string fit_pill_text(
    const Building& building, std::string text, int32_t room, std::optional<kit::StateIcon> icon
) {
    kit::PillLook look;
    look.icon = icon;
    look.text = text;
    if (room <= 0)
        return {};
    if (pill_width_of(building, look) <= room)
        return text;
    const int32_t padding = chip_side(*building.metrics);
    std::string fitted =
        cut(building, kit::FontRole::small, text, std::max(room - 2 * padding, int32_t{0}));
    look.text = fitted;
    while (!fitted.empty() && pill_width_of(building, look) > room) {
        std::size_t drop = 1;
        if (fitted.size() >= 3 && fitted.compare(fitted.size() - 3, 3, "...") == 0 &&
            fitted.size() > 3) {
            const std::string_view body(fitted.data(), fitted.size() - 3);
            std::size_t index = 0;
            std::size_t previous = 0;
            while (index < body.size()) {
                previous = index;
                index += std::max(kit::character_bytes(body.substr(index)), std::size_t{1});
            }
            drop = body.size() - previous;
            if (drop == 0)
                drop = 1;
            fitted.erase(fitted.size() - 3 - drop, drop);
        } else {
            fitted.pop_back();
        }
        look.text = fitted;
    }
    return fitted;
}

/// One row of expansion pills, measured before it is placed.
struct ExpansionRow {
    std::vector<std::size_t> indexes{};
    std::vector<int32_t> widths{}; ///< each pill, without the download arrow
    int32_t band{};                ///< the row's height: the pill, or the arrow when it is taller
};

/// The expansion pills wrapped into the tile's inner width.
struct ExpansionWrap {
    std::vector<ExpansionRow> rows{};
    int32_t rows_height{}; ///< the rows, gaps included
    bool hint{};
};

/// Returns the points between two expansion pills, side by side or one
/// line under another: the kit's gap between chips, as a row's tags keep.
/// Compact puts one pill on each line. With every built-in expansion those
/// gaps set UNLOAD MOD over the footer, where no arrow can pass it, so the
/// lines sit together.
///
/// @param building the layout
/// @param pills how many pills are wrapping
/// @return the points
int32_t expansion_line_gap(const Building& building, std::size_t pills) {
    if (building.size_class == kit::SizeClass::compact && pills > 6)
        return 0;
    return building.metrics->chip_gap;
}

/// Wraps the expansion pills into a width.
///
/// @param building the layout
/// @param content the pills and the hint
/// @param inner the width inside the card
/// @return the rows
ExpansionWrap
wrap_expansions(const Building& building, const ActiveContent& content, int32_t inner) {
    const kit::Metrics& metrics = *building.metrics;
    ExpansionWrap wrap;
    const int32_t room = std::max(inner, int32_t{0});
    const int32_t gap = expansion_line_gap(building, content.pills.size());
    const int32_t pill_height = chip_height(metrics);
    ExpansionRow row;
    int32_t across = 0;
    for (std::size_t index = 0; index < content.pills.size(); ++index) {
        const ExpansionPill& pill = content.pills[index];
        const std::optional<kit::StateIcon> icon =
            pill.arrow ? std::optional<kit::StateIcon>{kit::StateIcon::download_arrow}
                       : std::nullopt;
        kit::PillLook look;
        look.icon = icon;
        look.text = fit_pill_text(building, pill.text, room, icon);
        int32_t width = room <= 0 ? 0 : pill_width_of(building, look);
        if (room > 0)
            width = std::min(width, room);
        if (!row.indexes.empty() && across + gap + width > room) {
            wrap.rows.push_back(std::move(row));
            row = {};
            across = 0;
        }
        row.indexes.push_back(index);
        row.widths.push_back(width);
        row.band = std::max(row.band, pill_height);
        across += width;
        if (row.indexes.size() > 1)
            across += gap;
    }
    if (!row.indexes.empty())
        wrap.rows.push_back(std::move(row));
    for (std::size_t row_index = 0; row_index < wrap.rows.size(); ++row_index) {
        if (row_index != 0)
            wrap.rows_height += expansion_line_gap(building, content.pills.size());
        wrap.rows_height += wrap.rows[row_index].band;
    }
    wrap.hint = !content.hint.empty();
    return wrap;
}

/// Returns the expansions hint wrapped to the tile's inner width. Every
/// line is kept: the hint wraps instead of being cut.
///
/// @param building the layout
/// @param hint the hint; empty returns nothing
/// @param inner the width inside the card
/// @return the lines
std::vector<std::string>
expansion_hint_lines(const Building& building, std::string_view hint, int32_t inner) {
    if (hint.empty() || inner <= 0)
        return {};
    return wrapped(building, kit::FontRole::small, hint, inner, 0);
}

/// Returns the room the padlock takes before the lock's words: the padlock
/// and the gap after it.
///
/// @param metrics the class's metrics
/// @return the room, in points
int32_t padlock_room(const kit::Metrics& metrics) {
    return metrics.padlock_width + metrics.padlock_gap;
}

/// Returns the lock under the expansion pills wrapped beside its padlock.
/// Every line is kept, as the hint's are.
///
/// @param building the layout
/// @param lock why the locked pills stay on; empty returns nothing
/// @param inner the width inside the card
/// @return the lines
std::vector<std::string>
expansion_lock_lines(const Building& building, std::string_view lock, int32_t inner) {
    const int32_t room = inner - padlock_room(*building.metrics);
    if (lock.empty() || room <= 0)
        return {};
    return wrapped(building, kit::FontRole::small, lock, room, 0);
}

/// Returns the expansions tile's height, including the room a button needs
/// when the mod tile has UNLOAD MOD: that button stands at the bottom of a
/// tile as tall, so the tile keeps the control height less a pill's under
/// the pills, and the button's top is level with the last line of pills or
/// under it, where Tab reaches it after them. The lock and the hint take
/// one line for each line they wrap to.
///
/// @param building the layout
/// @param content the pills, the lock and the hint
/// @param width the tile's width
/// @param reserve_button leave room for a button at the bottom
/// @return the height, in points
int32_t expansions_height(
    const Building& building, const ActiveContent& content, int32_t width, bool reserve_button
) {
    const kit::Metrics& metrics = *building.metrics;
    const int32_t inset = tile_inset(metrics);
    const int32_t inner = std::max(width - 2 * inset, int32_t{0});
    const ExpansionWrap wrap = wrap_expansions(building, content, inner);
    int32_t stack = wrap.rows_height;
    bool placed = !wrap.rows.empty();
    const std::vector<std::string> lock_lines = expansion_lock_lines(building, content.lock, inner);
    if (!lock_lines.empty()) {
        stack += (placed ? metrics.slider_gap : 0) + hint_lines_height(building, lock_lines.size());
        placed = true;
    }
    const std::vector<std::string> hint_lines = expansion_hint_lines(building, content.hint, inner);
    if (!hint_lines.empty())
        stack += (placed ? metrics.slider_gap : 0) + hint_lines_height(building, hint_lines.size());
    if (reserve_button)
        stack += std::max(metrics.control_height - chip_height(metrics), int32_t{0});
    return stack + 2 * inset;
}

/// Appends the expansions tile: wrapped pills, the lock and the hint. A
/// locked pill keeps its face under the kit's locked fade and takes no
/// press, and the lock under the pills is the padlock and why, in the lock
/// colour. The heading's label is the caller's.
///
/// @param[in,out] building the layout
/// @param content the pills, the lock and the hint
/// @param tile the tile
/// @param clip what drawing may touch
void place_expansions(
    Building& building, const ActiveContent& content, const kit::Rect& tile, const kit::Rect& clip
) {
    const kit::Metrics& metrics = *building.metrics;
    const int32_t inset = tile_inset(metrics);
    add_tile_face(building, tile, clip);
    const int32_t inner_left = tile.x + inset;
    const int32_t inner_width = std::max(tile.width - 2 * inset, int32_t{0});
    const ExpansionWrap wrap = wrap_expansions(building, content, inner_width);
    int32_t row_y = tile.y + inset;
    const int32_t pill_height = chip_height(metrics);
    for (const ExpansionRow& row : wrap.rows) {
        int32_t column = inner_left;
        const int32_t band = std::max(row.band, pill_height);
        for (std::size_t place = 0; place < row.indexes.size(); ++place) {
            const std::size_t index = row.indexes[place];
            const ExpansionPill& pill = content.pills[index];
            const int32_t width = row.widths[place];
            if (width <= 0 || index >= controls::most_expansions) {
                column += width + expansion_line_gap(building, content.pills.size());
                continue;
            }
            const kit::ControlId id =
                controls::first_expansion + static_cast<kit::ControlId>(index);
            // The arrow is part of the pill. GET and RETRY take a press only
            // while the arrow is on. CANCEL stays on. A copied pill has no
            // arrow, and its own enabled flag decides.
            const bool pressable = pill.arrow ? pill.arrow_enabled : pill.enabled;
            kit::PillLook look;
            look.icon = pill.arrow ? std::optional<kit::StateIcon>{kit::StateIcon::download_arrow}
                                   : std::nullopt;
            look.text = fit_pill_text(building, pill.text, width, look.icon);
            // An expansion that is off is outlined as a tag, so it reads as a
            // chip on the tile's face; one that is on is the accent.
            look.state =
                pill.face == ExpansionFace::on ? kit::PillState::active : kit::PillState::tag;
            look.hovered = pressable && hovered(building, id);
            look.focused = pressable && focused(building, id);
            // A locked pill keeps its face, faded below, so it still reads as on.
            look.disabled = !pressable && !pill.locked;
            const kit::Rect rect{column, row_y, width, pill_height};
            const std::size_t before = building.list.controls.size();
            kit::add_pill(
                building.list,
                rect,
                look,
                id,
                unique_name(building, named("active.expansion." + pill.id))
            );
            building.list.items.back().clip = clip;
            building.list.items.back().state.locked = pill.locked;
            if (pill.locked)
                add_item(building.list, kit::Role::locked_fade, rect, clip);
            move_controls(building.list, before, building.detail);
            kit::Control& control = building.detail.back();
            // Drawing may cut the words. Automation keeps the whole line, as a
            // button's caption does, so "Add from…" still reads when the tile is narrow.
            control.text = pill.text;
            control.clip = clip;
            control.group = -1;
            control.focusable = pressable;
            control.enabled = pressable;
            tab_stop(building, control);
            column += width + expansion_line_gap(building, content.pills.size());
        }
        row_y += band + expansion_line_gap(building, content.pills.size());
    }
    // After the last row of pills, row_y is a line gap below it. The lock
    // and the hint each stand slider_gap below what is above them, as
    // Settings keeps between a row's hints and its control, their lines a
    // hint's step apart (hint_step).
    bool placed = !wrap.rows.empty();
    if (placed)
        row_y -= expansion_line_gap(building, content.pills.size());
    const int32_t step = hint_step(building);
    const std::vector<std::string> lock_lines =
        expansion_lock_lines(building, content.lock, inner_width);
    if (!lock_lines.empty()) {
        if (placed)
            row_y += metrics.slider_gap;
        const int32_t lock_top = row_y;
        const int32_t indent = padlock_room(metrics);
        kit::add_mark(
            building.list,
            {inner_left, row_y + (metrics.small_line - metrics.padlock_height) / 2},
            kit::Mark::padlock,
            kit::colour::lock
        );
        building.list.items.back().clip = clip;
        for (const std::string& lock_line : lock_lines) {
            add_text(
                building,
                kit::FontRole::small,
                lock_line,
                {inner_left + indent, row_y, inner_width - indent, metrics.small_line},
                kit::Align::left,
                kit::colour::lock,
                clip
            );
            row_y += step;
        }
        row_y = lock_top + hint_lines_height(building, lock_lines.size());
        add_label(
            building.detail,
            controls::active_lock,
            kit::intersect({inner_left, lock_top, inner_width, row_y - lock_top}, clip),
            unique_name(building, named("active.lock")),
            content.lock,
            clip,
            -1
        );
        placed = true;
    }
    const std::vector<std::string> hint_lines =
        expansion_hint_lines(building, content.hint, inner_width);
    if (!hint_lines.empty()) {
        if (placed)
            row_y += metrics.slider_gap;
        for (const std::string& hint_line : hint_lines) {
            add_text(
                building,
                kit::FontRole::small,
                hint_line,
                {inner_left, row_y, inner_width, metrics.small_line},
                kit::Align::left,
                kit::colour::hint,
                clip
            );
            row_y += step;
        }
    }
}

/// Appends the base game, the language, the expansions, the mod and the
/// layout in use, and returns the row under them. The four tiles stand two
/// by two at every size. On a small window the layout is one line under its
/// heading, so the expansion rows stay in view when the maps scroll up. On
/// a larger window it is a full-width tile. `area` places them; `clip` is
/// what drawing may touch, the page when that page scrolls.
///
/// @param[in,out] building the layout
/// @param setup what the tab shows of the mod and the language
/// @param area where the tiles sit
/// @param clip what drawing may touch
/// @return the row under the tiles and the gap after them
int32_t place_fixed_identity(
    Building& building, const ActiveSetup& setup, const kit::Rect& area, const kit::Rect& clip
) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    const ActiveContent content = active_content(library);
    const IdentityTile base = base_tile(building, content);
    const IdentityTile language = language_tile(building, setup);
    const IdentityTile mod = mod_tile(building, setup);
    const int32_t gap = metrics.card_gap;
    const int32_t left_width = std::max((area.width - gap) / 2, int32_t{0});
    const int32_t right = area.x + left_width + gap;
    const int32_t right_width = std::max(area.x + area.width - right, int32_t{0});
    const int32_t row1 = std::max(
        identity_height(building, base, left_width),
        identity_height(building, language, right_width)
    );
    const int32_t row2 = std::max(
        expansions_height(building, content, left_width, mod.button),
        identity_height(building, mod, right_width)
    );

    int32_t top = area.y;
    const int32_t first = add_active_heading(
        building, shown(library.text, words::active_heading_base), area.x, top, left_width, clip
    );
    static_cast<void>(add_active_heading(
        building, shown(library.text, words::active_heading_language), right, top, right_width, clip
    ));
    place_identity(building, base, {area.x, first, left_width, row1}, clip);
    place_identity(building, language, {right, first, right_width, row1}, clip);

    top = first + row1 + metrics.paragraph_gap;
    const std::string expansions_heading = content.expansions_heading;
    const int32_t second =
        add_active_heading(building, expansions_heading, area.x, top, left_width, clip);
    add_label(
        building.detail,
        controls::active_expansions,
        kit::intersect({area.x, top, left_width, metrics.small_line}, clip),
        unique_name(building, named("active.expansions")),
        expansions_heading,
        clip,
        -1
    );
    static_cast<void>(add_active_heading(
        building, shown(library.text, words::active_heading_mod), right, top, right_width, clip
    ));
    place_expansions(building, content, {area.x, second, left_width, row2}, clip);
    place_identity(building, mod, {right, second, right_width, row2}, clip);

    const IdentityTile keys = keys_tile(building, setup);
    const int32_t keys_top = second + row2 + metrics.paragraph_gap;
    const int32_t keys_heading = add_active_heading(
        building,
        shown(library.text, words::active_heading_keys),
        area.x,
        keys_top,
        area.width,
        clip
    );
    if (building.size_class == kit::SizeClass::compact) {
        return place_compact_keys_line(building, keys, area.x, keys_heading, area.width, clip);
    }
    const int32_t keys_height = identity_height(building, keys, area.width);
    place_identity(building, keys, {area.x, keys_heading, area.width, keys_height}, clip);
    return keys_heading + keys_height + metrics.paragraph_gap;
}

/// Returns the row under the four tiles, measured without appending them.
///
/// @param building the layout
/// @param setup what the tab shows
/// @param area where the tiles would sit
/// @return that row
int32_t measured_identity_bottom(
    const Building& building, const ActiveSetup& setup, const kit::Rect& area
) {
    Building trial = building;
    trial.list = {};
    trial.detail.clear();
    trial.order.clear();
    return place_fixed_identity(trial, setup, area, area);
}

/// Appends the Active tab's scroll bar, clipped to the view it scrolls, so a
/// scroll can be read from the bar when no row of that view is on screen.
///
/// @param[in,out] building the layout
/// @param view what the bar scrolls
/// @param well the bar's column
/// @param content_height the scrolled content's height
/// @param limit the greatest offset
/// @param offset how far it is scrolled
void add_active_bar(
    Building& building,
    const kit::Rect& view,
    const kit::Rect& well,
    int32_t content_height,
    int32_t limit,
    int32_t offset
) {
    const kit::Metrics& metrics = *building.metrics;
    kit::ScrollArea scroll;
    scroll.view = view;
    scroll.well = well;
    scroll.content_height = content_height;
    scroll.limit = limit;
    scroll.offset = offset;
    add_item(building.list, kit::Role::scroll_bar, scroll.well, view).look = kit::ScrollBarLook{
        scroll.thumb(metrics.least_thumb_height), building.state->bar_held == Pane::maps
    };
}

/// Appends the Active tab. The four tiles stand under the tabs, then KEYS,
/// then the MAPS heading and its search, then the maps. When the tiles
/// leave the maps fewer than four rows, at any size, the page scrolls as
/// one: the tiles, then the maps, and once the maps are scrolled to the top at
/// least four rows fit. Otherwise the maps pane scrolls on its own and the
/// tiles stay. Either scroll bar is the one the other pages use. A search on
/// the heading leaves the tiles whose name or pack name holds the text.
///
/// @param[in,out] building the layout
/// @param area the area
void add_active(Building& building, const kit::Rect& area) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    const ActiveSetup setup = active_setup(library);
    const std::string query = maps_query_folded(*building.state);
    std::vector<std::size_t> matching;
    matching.reserve(setup.maps.size());
    for (std::size_t index = 0; index < setup.maps.size(); ++index)
        if (map_matches_query(setup.maps[index], query))
            matching.push_back(index);

    const int32_t heading_span = metrics.control_height + metrics.hint_gap;
    const int32_t full_bottom = measured_identity_bottom(building, setup, area);
    const int32_t pane_height = area.y + area.height - (full_bottom + heading_span);
    const bool page = pane_height < maps_rows_kept * metrics.small_line;
    const int32_t well_width = metrics.scroll_thumb_width + 2;

    if (page) {
        const int32_t content_width =
            std::max(area.width - well_width - metrics.button_gap, int32_t{0});
        const kit::Rect narrow{area.x, area.y, content_width, area.height};
        const int32_t identity_bottom = measured_identity_bottom(building, setup, narrow);
        const MapGrid grid = measure_map_grid(building, setup, matching, content_width);
        const int32_t note_height = metrics.small_line + 2 * metrics.card_inset;
        const int32_t maps_block = matching.empty() ? note_height : grid.height;
        const int32_t maps_height = heading_span + maps_block;
        const int32_t identity_span = identity_bottom - area.y;
        // The tiles left fewer than four rows. The page can scroll until the
        // heading sits high enough that four rows fit under it, even when
        // fewer maps than that are installed.
        const int32_t kept_room = heading_span + maps_rows_kept * metrics.small_line;
        const int32_t content_height =
            std::max(identity_span + maps_height, identity_span + kept_room);
        const int32_t limit = kit::ScrollArea::limit_for(content_height, area.height);
        const int32_t offset = std::clamp(building.state->maps_scroll, int32_t{0}, limit);
        const kit::Rect placed{area.x, area.y - offset, content_width, area.height};
        static_cast<void>(place_fixed_identity(building, setup, placed, area));
        const int32_t heading_top = identity_bottom - offset;
        add_maps_heading_row(
            building,
            area.x,
            heading_top,
            content_width,
            area,
            query,
            matching.size(),
            setup.maps.size()
        );
        const int32_t rows_top = heading_top + heading_span;
        if (matching.empty()) {
            const std::string_view note =
                setup.maps.empty() ? words::active_no_maps : words::active_no_map_match;
            add_maps_note(
                building,
                shown(library.text, note),
                {area.x, rows_top, content_width, note_height},
                content_width,
                area
            );
        } else {
            const kit::Rect rows{
                area.x, rows_top, content_width, std::max(grid.height, int32_t{0})
            };
            add_visible_map_rows(building, setup, matching, grid, rows, 0, area);
        }
        if (limit > 0) {
            add_extent_edge(building.list, area, area.y - offset);
            add_extent_edge(building.list, area, area.y - offset + content_height - 1);
            add_active_bar(
                building,
                area,
                {area.x + area.width - well_width, area.y, well_width, area.height},
                content_height,
                limit,
                offset
            );
        }
        return;
    }

    const int32_t heading_top = place_fixed_identity(building, setup, area, area);
    add_maps_heading_row(
        building, area.x, heading_top, area.width, area, query, matching.size(), setup.maps.size()
    );
    const int32_t pane_top = heading_top + heading_span;
    const int32_t pane_bottom = area.y + area.height;
    const kit::Rect pane{
        area.x, pane_top, area.width, std::max(pane_bottom - pane_top, int32_t{0})
    };
    if (pane.height <= 0)
        return;

    MapGrid grid = measure_map_grid(building, setup, matching, pane.width);
    const bool scrolls = !matching.empty() && grid.height > pane.height;
    int32_t tiles_width = pane.width;
    if (scrolls) {
        tiles_width = std::max(pane.width - well_width - metrics.button_gap, int32_t{0});
        grid = measure_map_grid(building, setup, matching, tiles_width);
        if (grid.height <= pane.height) {
            tiles_width = pane.width;
            grid = measure_map_grid(building, setup, matching, tiles_width);
        }
    }
    if (matching.empty()) {
        const std::string_view note =
            setup.maps.empty() ? words::active_no_maps : words::active_no_map_match;
        add_maps_note(building, shown(library.text, note), pane, tiles_width, pane);
        return;
    }
    const int32_t limit = kit::ScrollArea::limit_for(grid.height, pane.height);
    const int32_t offset = std::clamp(building.state->maps_scroll, int32_t{0}, limit);
    add_visible_map_rows(building, setup, matching, grid, pane, offset, pane);
    if (limit <= 0)
        return;
    add_active_bar(
        building,
        pane,
        {pane.x + pane.width - well_width, pane.y, well_width, pane.height},
        grid.height,
        limit,
        offset
    );
}

/// Appends a registry's Off/On switch, in the details' scroll.
///
/// @param[in,out] building the layout
/// @param rect the switch
/// @param on the switch is On
/// @param id its control's number
/// @param name its control's name
/// @param clip the part of it that shows
void add_registry_switch(
    Building& building,
    const kit::Rect& rect,
    bool on,
    kit::ControlId id,
    std::string name,
    const kit::Rect& clip
) {
    const Library& library = *building.library;
    kit::SwitchLook look;
    look.on = on;
    look.hovered = hovered(building, id);
    look.off_caption = std::string(shown(library.text, words::registry_off));
    look.on_caption = std::string(shown(library.text, words::registry_on));
    const std::size_t before = building.list.controls.size();
    kit::add_switch(building.list, rect, look, id, unique_name(building, std::move(name)));
    building.list.items.back().clip = clip;
    move_controls(building.list, before, building.detail);
    building.detail.back().clip = clip;
    building.detail.back().group = controls::details_group;
    tab_stop(building, building.detail.back());
}

/// Appends the key icon: the kit's key glyph in the icon square, so the
/// pointer and the keys can rest on it. Its text is the fingerprints, one a
/// line. Pressing it asks for nothing.
///
/// @param[in,out] building the layout
/// @param rect the square, state_icon_side on each side
/// @param id its control's number
/// @param name its control's name
/// @param fingerprints the fingerprints, one a line
/// @param clip the part of it that shows
void add_key_button(
    Building& building,
    const kit::Rect& rect,
    kit::ControlId id,
    std::string name,
    std::string fingerprints,
    const kit::Rect& clip
) {
    kit::ChipLook look;
    look.text = std::move(fingerprints);
    look.state = kit::ChipState::get;
    look.hovered = hovered(building, id);
    look.icon = kit::StateIcon::key;
    const std::size_t before = building.list.controls.size();
    kit::add_chip(building.list, rect, look, id, unique_name(building, std::move(name)));
    building.list.items.back().clip = clip;
    move_controls(building.list, before, building.detail);
    building.detail.back().clip = clip;
    building.detail.back().group = controls::details_group;
    tab_stop(building, building.detail.back());
}

/// Appends the Registries tab's parts, scrolled up by an offset: one card
/// for each registry (its avatar, heading, address, refresh time, description,
/// subtitle, key icon, REFRESH and switch; REMOVE and ADD MIRROR… for one the
/// player added, and a Built in pill for one that ships with the game), then
/// the refusal in amber and the note under the last card. Each control is in
/// the details' scroll area. The heading is the registry's title, or its
/// short name when it has no title.
///
/// @param[in,out] building the layout
/// @param view the area, which clips them
/// @param width the columns they take from the area's left
/// @param offset how far they are scrolled up, in points
/// @return the parts' height, in points
int32_t
add_registries_content(Building& building, const kit::Rect& view, int32_t width, int32_t offset) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    const int32_t first = view.y - offset;
    const int32_t left = view.x;
    const std::size_t first_item = building.list.items.size();
    int32_t cursor = first;
    const std::vector<Registry>& registries = library.inputs.registries;
    for (std::size_t index = 0; index < registries.size(); ++index) {
        const Registry& registry = registries[index];
        const std::string word = registry_word(registry.id);
        if (index > 0)
            cursor += metrics.card_gap;
        const int32_t card_top = cursor;
        const int32_t inset = metrics.card_inset;
        const int32_t inner_left = left + inset;
        const int32_t inner_right = left + width - inset;
        const int32_t band_top = card_top + inset;
        const int32_t side = metrics.list_row_badge;
        const int32_t title_band = std::max(metrics.regular_line, metrics.button_height);
        // The right cluster, from the switch inward: REFRESH, the refresh
        // mark while this registry is being asked or its check is showing,
        // and the key icon. They share the title's band when the title keeps
        // a letter's width beside them, so Tab reads them left to right and
        // then the buttons below. A translation of Refresh such as
        // Auffrischen can leave the title narrower than its first letter;
        // the controls then take the row under the title, and the title
        // keeps the card's width. A wrapped heading grows under its band.
        const uint16_t check_ms =
            registry.refreshing ? uint16_t{0} : registry_check_ms(*building.state, registry.id);
        const bool show_mark = registry.refreshing || check_ms > 0;
        int32_t cluster = inner_right;
        // The same growth as a settings switch: both halves hold the wider
        // caption, and the class's switch stays when the words already fit.
        const int32_t off_width =
            width_of(building, kit::FontRole::small, shown(library.text, words::registry_off));
        const int32_t on_width =
            width_of(building, kit::FontRole::small, shown(library.text, words::registry_on));
        constexpr int32_t switch_pad = 4;
        const int32_t switch_columns =
            std::max(metrics.switch_width, (std::max(off_width, on_width) + switch_pad) * 2 + 2);
        cluster -= switch_columns;
        const int32_t toggle_x = cluster;
        cluster -= metrics.button_gap;
        // REFRESH is a row's small secondary button with its arrow.
        const std::string_view refresh_caption = shown(library.text, words::registry_refresh);
        const int32_t refresh_width = small_button_width(building, refresh_caption) +
                                      kit::refresh_icon_room(metrics.button_height);
        cluster -= refresh_width;
        const int32_t refresh_x = cluster;
        cluster -= metrics.button_gap;
        int32_t turning_x = 0;
        if (show_mark) {
            cluster -= metrics.button_height;
            turning_x = cluster;
            cluster -= metrics.button_gap;
        }
        cluster -= metrics.state_icon_side;
        const int32_t key_x = cluster;
        const int32_t text_left = inner_left + side + metrics.list_row_gap;
        int32_t title_width = key_x - metrics.label_gap - text_left;
        const std::string_view heading = registry.title.empty() ? std::string_view{registry.name}
                                                                : std::string_view{registry.title};
        const std::size_t glyph_bytes = std::min(kit::character_bytes(heading), heading.size());
        const int32_t glyph =
            glyph_bytes == 0
                ? 0
                : width_of(building, kit::FontRole::regular, heading.substr(0, glyph_bytes));
        const bool stack_controls = glyph > 0 && title_width < glyph;
        if (stack_controls)
            title_width = std::max(inner_right - text_left, int32_t{0});
        // A built-in registry's pill sits on the title's line, at its right,
        // as the Active tab's Language tile places Built in. The heading
        // keeps clear of it.
        kit::PillLook built_in_pill;
        int32_t built_in_width = 0;
        bool show_built_in = false;
        int32_t heading_limit = std::max(title_width, int32_t{1});
        int32_t first_box = std::max(title_width, int32_t{0});
        if (registry.built_in && title_width > 0) {
            built_in_pill.text = std::string(shown(library.text, words::registry_built_in));
            built_in_pill.state = kit::PillState::neutral;
            built_in_width = kit::pill_width(*building.fonts, built_in_pill);
            const int32_t reserved = built_in_width + metrics.list_row_gap;
            if (built_in_width > 0 && built_in_width <= title_width) {
                show_built_in = true;
                const int32_t kept =
                    reserved <= title_width ? title_width - reserved : title_width - built_in_width;
                heading_limit = std::max(kept, int32_t{1});
                first_box = std::max(kept, int32_t{0});
            }
        }
        std::vector<std::string> heading_lines;
        if (title_width > 0)
            heading_lines = wrapped(building, kit::FontRole::regular, heading, heading_limit, 0);
        const int32_t heading_height =
            static_cast<int32_t>(std::max(heading_lines.size(), std::size_t{1})) *
            metrics.regular_line;
        const int32_t cluster_row = std::max(metrics.button_height, metrics.state_icon_side * 2);
        int32_t cluster_top = band_top;
        int32_t header_height = std::max(title_band, std::max(heading_height, side));
        if (stack_controls) {
            const int32_t title_block = std::max(title_band, std::max(heading_height, side));
            cluster_top = band_top + title_block + metrics.hint_gap;
            header_height = cluster_top - band_top + cluster_row;
        }
        const kit::Rect toggle{toggle_x, cluster_top, switch_columns, metrics.regular_line};
        const kit::Rect refresh_rect{refresh_x, cluster_top, refresh_width, metrics.button_height};
        kit::Rect turning_rect{};
        if (show_mark)
            turning_rect = {turning_x, cluster_top, metrics.button_height, metrics.button_height};
        const kit::Rect key_rect{
            key_x, cluster_top, metrics.state_icon_side, metrics.state_icon_side
        };
        const int32_t lines_width = std::max(inner_right - text_left, int32_t{0});
        std::vector<std::string> description_lines;
        if (!registry.description.empty() && lines_width > 0)
            description_lines = wrapped(
                building,
                kit::FontRole::small,
                registry.description,
                std::max(lines_width, int32_t{1}),
                0
            );

        const std::size_t card_at = building.list.items.size();
        const kit::Rect avatar{inner_left, band_top, side, side};
        // The icon takes the lettermark's square. The lettermark stays while
        // the picture is missing or still on its way.
        const oa::ui::frontend_renderer::RgbaPicture avatar_picture = registry_picture(registry);
        if (oa::ui::frontend_renderer::picture_drawable(avatar_picture)) {
            add_item(building.list, kit::Role::picture, avatar, view).look =
                kit::PictureLook{avatar_picture};
        } else {
            add_item(building.list, kit::Role::fill, avatar, view).colour =
                registry.built_in ? kit::colour::accent : kit::colour::switch_idle;
            add_text(
                building,
                kit::FontRole::small,
                cut(building, kit::FontRole::small, registry_initials(registry.name), side),
                avatar,
                kit::Align::centre,
                kit::colour::on_accent,
                view
            );
        }
        int32_t heading_top = band_top;
        for (std::size_t line = 0; line < heading_lines.size(); ++line) {
            const int32_t box_width =
                line == 0 && show_built_in ? first_box : std::max(title_width, int32_t{0});
            // A word wider than the box is not drawn. The wrap keeps a first
            // letter even when that letter is wider, and that letter would
            // fail the text-fit check.
            if (width_of(building, kit::FontRole::regular, heading_lines[line]) <= box_width)
                add_text(
                    building,
                    kit::FontRole::regular,
                    heading_lines[line],
                    {text_left, heading_top, box_width, metrics.regular_line},
                    kit::Align::left,
                    kit::colour::text,
                    view
                );
            heading_top += metrics.regular_line;
        }
        if (show_built_in) {
            const int32_t pill_height = kit::pill_height();
            const kit::Rect pill_rect{
                text_left + title_width - built_in_width,
                band_top + (metrics.regular_line - pill_height) / 2,
                built_in_width,
                pill_height,
            };
            kit::add_pill(building.list, pill_rect, built_in_pill, kit::no_control, {});
            building.list.items.back().clip = view;
        }
        std::string fingerprints;
        for (const SigningKey& signing_key : registry.keys) {
            if (!fingerprints.empty())
                fingerprints.push_back('\n');
            fingerprints += signing_key.fingerprint;
        }
        add_key_button(
            building,
            key_rect,
            controls::first_registry_key + static_cast<kit::ControlId>(index),
            named("registry." + word + ".key"),
            std::move(fingerprints),
            view
        );
        // A built-in registry's mark sits under the key, inside the header
        // band (the avatar is two icons tall). It is not a control.
        if (const std::optional<kit::MakerMark> mark = shown_maker(
                registry.built_in ? package_maker(registry.author) : PackageMaker::none
            )) {
            kit::ChipLook mark_look;
            mark_look.icon = *mark == kit::MakerMark::open_annihilation ? kit::StateIcon::oa
                                                                        : kit::StateIcon::bone;
            const kit::Rect mark_rect{
                key_rect.x,
                key_rect.y + metrics.state_icon_side,
                metrics.state_icon_side,
                metrics.state_icon_side,
            };
            kit::add_chip(building.list, mark_rect, mark_look, kit::no_control, {});
            building.list.items.back().clip = view;
        }
        if (show_mark) {
            kit::ButtonLook spinning;
            spinning.style = kit::ButtonStyle::quiet;
            spinning.enabled = false;
            spinning.mark = kit::ButtonMark::refresh;
            spinning.turn = building.state->refresh_turn;
            spinning.turning = registry.refreshing;
            spinning.check_ms = check_ms;
            kit::Item& spun = add_item(building.list, kit::Role::button, turning_rect, view);
            spun.look = spinning;
        }
        add_small_button(
            building,
            building.detail,
            refresh_rect,
            refresh_caption,
            registry.on && !registry.refreshing,
            controls::first_registry_refresh + static_cast<kit::ControlId>(index),
            named("registry." + word + ".refresh"),
            view,
            controls::details_group
        );
        if (auto* refresh_look = std::get_if<kit::ButtonLook>(&building.list.items.back().look))
            refresh_look->refresh_icon = true;
        add_registry_switch(
            building,
            toggle,
            registry.on,
            controls::first_registry + static_cast<kit::ControlId>(index),
            named("registry." + word),
            view
        );

        int32_t facts_top = band_top + header_height + metrics.hint_gap;
        int32_t bottom = band_top + header_height;
        const auto place_line = [&](std::string full, kit::ControlId id, std::string control_name) {
            if (full.empty() || lines_width <= 0)
                return;
            const kit::Rect box{text_left, facts_top, lines_width, metrics.small_line};
            add_text(
                building,
                kit::FontRole::small,
                cut(building, kit::FontRole::small, full, lines_width),
                box,
                kit::Align::left,
                kit::colour::hint,
                view
            );
            add_label(
                building.detail,
                id,
                box,
                unique_name(building, std::move(control_name)),
                full,
                view,
                controls::details_group
            );
            bottom = facts_top + metrics.small_line;
            facts_top = bottom + metrics.hint_gap;
        };
        place_line(
            registry.address,
            controls::first_registry_address + static_cast<kit::ControlId>(index),
            named("registry." + word + ".address")
        );
        place_line(
            registry_refresh_line(registry, library.text),
            controls::first_registry_refreshed + static_cast<kit::ControlId>(index),
            named("registry." + word + ".refreshed")
        );
        if (!description_lines.empty()) {
            for (const std::string& description_line : description_lines) {
                add_text(
                    building,
                    kit::FontRole::small,
                    description_line,
                    {text_left, facts_top, lines_width, metrics.small_line},
                    kit::Align::left,
                    kit::colour::text,
                    view
                );
                facts_top += metrics.small_line;
            }
            bottom = facts_top;
            facts_top += metrics.hint_gap;
        }
        const std::string subtitle = registry_subtitle(registry, library.text);
        if (!subtitle.empty() && lines_width > 0) {
            add_text(
                building,
                kit::FontRole::small,
                cut(building, kit::FontRole::small, subtitle, lines_width),
                {text_left, facts_top, lines_width, metrics.small_line},
                kit::Align::left,
                kit::colour::hint,
                view
            );
            bottom = facts_top + metrics.small_line;
        }
        if (!registry.built_in) {
            const int32_t buttons_top = bottom + metrics.folder_button_gap;
            const std::string_view remove_caption = shown(library.text, words::registry_remove);
            const int32_t remove_width = small_button_width(building, remove_caption);
            add_small_button(
                building,
                building.detail,
                {text_left, buttons_top, remove_width, metrics.button_height},
                remove_caption,
                true,
                controls::first_registry_remove + static_cast<kit::ControlId>(index),
                named("registry." + word + ".remove"),
                view,
                controls::details_group
            );
            const int32_t mirror_left = text_left + remove_width + metrics.button_gap;
            if (library.inputs.in_match) {
                const int32_t lock_width = std::max(inner_right - mirror_left, int32_t{0});
                if (lock_width > 0) {
                    kit::LockLook look;
                    look.text = std::string(shown(library.text, words::registry_game_lock));
                    kit::add_lock(
                        building.list,
                        {mirror_left, buttons_top, lock_width, metrics.button_height},
                        look
                    );
                    building.list.items.back().clip = view;
                }
            } else {
                const std::string_view mirror_caption =
                    shown(library.text, words::registry_add_mirror);
                add_small_button(
                    building,
                    building.detail,
                    {mirror_left,
                     buttons_top,
                     small_button_width(building, mirror_caption),
                     metrics.button_height},
                    mirror_caption,
                    true,
                    controls::first_registry_mirror + static_cast<kit::ControlId>(index),
                    named("registry." + word + ".add-mirror"),
                    view,
                    controls::details_group
                );
            }
            bottom = buttons_top + metrics.button_height;
        }
        const kit::Rect card{left, card_top, width, bottom + inset - card_top};
        const std::size_t drawn = building.list.items.size();
        add_tile_face(building, card, view);
        std::rotate(
            building.list.items.begin() + static_cast<std::ptrdiff_t>(card_at),
            building.list.items.begin() + static_cast<std::ptrdiff_t>(drawn),
            building.list.items.end()
        );
        cursor = card.y + card.height;
    }

    cursor += metrics.label_gap;
    if (!library.inputs.registries_notice.empty()) {
        const std::vector<std::string> notice_lines =
            wrapped(building, kit::FontRole::small, library.inputs.registries_notice, width, 0);
        const int32_t notice_top = cursor;
        for (const std::string& notice_line : notice_lines) {
            add_text(
                building,
                kit::FontRole::small,
                notice_line,
                {left, cursor, width, metrics.small_line},
                kit::Align::left,
                kit::colour::lock,
                view
            );
            cursor += metrics.small_line;
        }
        add_label(
            building.detail,
            controls::registries_notice,
            {left, notice_top, width, std::max(cursor - notice_top, metrics.small_line)},
            unique_name(building, named("registries.notice")),
            joined(notice_lines),
            view,
            controls::details_group
        );
        cursor += metrics.hint_gap;
    }

    const std::string note = registry_note(registries, library.text);
    const std::vector<std::string> note_lines =
        wrapped(building, kit::FontRole::small, note, width, 0);
    const int32_t note_top = cursor;
    for (const std::string& note_line : note_lines) {
        add_text(
            building,
            kit::FontRole::small,
            note_line,
            {left, cursor, width, metrics.small_line},
            kit::Align::left,
            kit::colour::hint,
            view
        );
        cursor += metrics.small_line;
    }
    if (note_lines.empty())
        cursor += metrics.small_line;
    add_label(
        building.detail,
        controls::registries_note,
        {left, note_top, width, std::max(cursor - note_top, metrics.small_line)},
        unique_name(building, named("registries.note")),
        note,
        view,
        controls::details_group
    );

    const std::optional<controls::Extent> extent =
        controls::clipped_extent(building.list, view, first_item);
    return extent ? extent->bottom - first : 0;
}

/// Appends the Registries tab: its cards in an area that scrolls when they
/// are taller than it, with a column for the scroll bar at its right.
///
/// @param[in,out] building the layout
/// @param area the area
void add_registries(Building& building, const kit::Rect& area) {
    const kit::Metrics& metrics = *building.metrics;
    Building trial = building;
    trial.list = {};
    trial.detail.clear();
    trial.order.clear();
    const int32_t whole_height = add_registries_content(trial, area, area.width, 0);
    const int32_t well_width = metrics.scroll_thumb_width + 2;
    const bool scrolls = whole_height > area.height;
    const int32_t width = scrolls ? area.width - well_width - metrics.button_gap : area.width;
    int32_t height = whole_height;
    if (scrolls) {
        trial = building;
        trial.list = {};
        trial.detail.clear();
        trial.order.clear();
        height = add_registries_content(trial, area, width, 0);
    }
    const int32_t limit = kit::ScrollArea::limit_for(height, area.height);
    const int32_t scroll_offset = std::clamp(building.state->details_scroll, int32_t{0}, limit);
    add_registries_content(building, area, width, scroll_offset);
    if (limit > 0) {
        kit::ScrollArea scroll;
        scroll.view = area;
        scroll.well = {area.x + area.width - well_width, area.y, well_width, area.height};
        scroll.content_height = height;
        scroll.limit = limit;
        scroll.offset = scroll_offset;
        add_item(building.list, kit::Role::scroll_bar, scroll.well, {}).look = kit::ScrollBarLook{
            scroll.thumb(metrics.least_thumb_height), building.state->bar_held == Pane::details
        };
    }
}

/// The sides of the square the icons are drawn in, in their own units.
constexpr int32_t icon_units = 1024;
/// The samples taken across a pixel, each way, to find how much of it an icon covers.
constexpr int32_t icon_samples = 4;

/// The star's corners, in icon units, clockwise from its top point.
constexpr std::array<std::array<int32_t, 2>, 10> star_corners{{
    {512, 115},
    {634, 365},
    {909, 403},
    {710, 595},
    {755, 870},
    {512, 742},
    {269, 870},
    {314, 595},
    {115, 403},
    {390, 365},
}};

/// The players' colours, as plain swatches of the game's ten.
constexpr std::array<kit::Colour, 10> player_swatches{{
    {0x1f, 0x4f, 0xd6},
    {0xd6, 0x2c, 0x1f},
    {0xe8, 0xe8, 0xe8},
    {0x23, 0xa0, 0x3a},
    {0x1b, 0x2d, 0x8c},
    {0x8e, 0x3f, 0xb8},
    {0xe8, 0xd2, 0x1f},
    {0x2b, 0x2b, 0x2b},
    {0x9f, 0xd4, 0xe8},
    {0xb0, 0x7a, 0x3a},
}};

/// Tells whether a point lies in a range, its ends included.
///
/// @param value the point
/// @param low the range's start
/// @param high the range's end
/// @return true inside it
constexpr bool within(int32_t value, int32_t low, int32_t high) noexcept {
    return value >= low && value <= high;
}

/// Tells whether an icon covers a point of its square.
///
/// @param icon the icon
/// @param x the point's column, in icon units
/// @param y the point's row, in icon units
/// @return true when the point is drawn
bool icon_covers(ReplayIcon icon, int32_t x, int32_t y) {
    switch (icon) {
    case ReplayIcon::film: {
        // A frame 12 by 11 units of 16, its edge a unit and a half, two
        // strips inside it, and three sprocket holes along each strip.
        const bool outer = within(x, 80, 944) && within(y, 112, 912);
        const bool inner = within(x, 176, 848) && within(y, 208, 816);
        if (outer && !inner)
            return true;
        if (!inner)
            return false;
        if (within(x, 272, 368) || within(x, 656, 752))
            return true;
        if (x < 272 || x > 752)
            return within(y, 291, 387) || within(y, 464, 560) || within(y, 637, 733);
        return false;
    }
    case ReplayIcon::star: {
        bool inside = false;
        for (std::size_t corner = 0, before = star_corners.size() - 1; corner < star_corners.size();
             before = corner++) {
            const int64_t xi = star_corners[corner][0];
            const int64_t yi = star_corners[corner][1];
            const int64_t xj = star_corners[before][0];
            const int64_t yj = star_corners[before][1];
            if ((yi > y) != (yj > y) &&
                (((x - xi) * (yj - yi) < (xj - xi) * (y - yi)) == (yj > yi)))
                inside = !inside;
        }
        return inside;
    }
    case ReplayIcon::dot: {
        const int64_t dx = x - icon_units / 2;
        const int64_t dy = y - icon_units / 2;
        const int64_t radius = icon_units * 7 / 16;
        return dx * dx + dy * dy <= radius * radius;
    }
    case ReplayIcon::edge: {
        const int64_t dx = x - icon_units / 2;
        const int64_t dy = y - icon_units / 2;
        const int64_t outer = icon_units * 7 / 16;
        const int64_t inner = icon_units * 5 / 16;
        const int64_t across = dx * dx + dy * dy;
        return across <= outer * outer && across >= inner * inner;
    }
    }
    return false;
}

/// Appends one of the Replays section's icons, drawn at the canvas's own
/// pixels so that it stays crisp, in one colour on a clear ground. Its
/// pixels live on the Library until the next layout.
///
/// @param[in,out] building the layout
/// @param icon the icon
/// @param rect its square, in points
/// @param ink its colour
/// @param clip what drawing may touch
void add_replay_icon(
    Building& building,
    ReplayIcon icon,
    const kit::Rect& rect,
    kit::Colour ink,
    const kit::Rect& clip
) {
    const kit::Point pixels = badge_pixel_size(building, rect);
    if (pixels.x <= 0 || pixels.y <= 0)
        return;
    const auto columns = static_cast<std::size_t>(pixels.x);
    const auto rows = static_cast<std::size_t>(pixels.y);
    std::vector<uint8_t> drawn(columns * rows * 4U, 0);
    const int64_t across = static_cast<int64_t>(pixels.x) * icon_samples;
    const int64_t down = static_cast<int64_t>(pixels.y) * icon_samples;
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t column = 0; column < columns; ++column) {
            int32_t covered = 0;
            for (int32_t sample_row = 0; sample_row < icon_samples; ++sample_row) {
                for (int32_t sample_column = 0; sample_column < icon_samples; ++sample_column) {
                    const int64_t sx = static_cast<int64_t>(column) * icon_samples + sample_column;
                    const int64_t sy = static_cast<int64_t>(row) * icon_samples + sample_row;
                    const auto x = static_cast<int32_t>((2 * sx + 1) * icon_units / (2 * across));
                    const auto y = static_cast<int32_t>((2 * sy + 1) * icon_units / (2 * down));
                    if (icon_covers(icon, x, y))
                        ++covered;
                }
            }
            const std::size_t at = (row * columns + column) * 4U;
            drawn[at] = ink.r;
            drawn[at + 1] = ink.g;
            drawn[at + 2] = ink.b;
            drawn[at + 3] = static_cast<uint8_t>(covered * 255 / (icon_samples * icon_samples));
        }
    }
    std::vector<uint8_t>& kept =
        building.library->scaled_badge_pixels.emplace_back(std::move(drawn));
    oa::ui::frontend_renderer::RgbaPicture picture;
    picture.width = static_cast<uint32_t>(columns);
    picture.height = static_cast<uint32_t>(rows);
    picture.pixels = kept;
    add_item(building.list, kit::Role::picture, rect, clip).look = kit::PictureLook{picture};
}

/// Returns a player's colour swatch.
///
/// @param colour the game's player colour
/// @return the swatch; the first for a colour out of range
kit::Colour player_swatch(int32_t colour) {
    if (colour < 0 || static_cast<std::size_t>(colour) >= player_swatches.size())
        return player_swatches.front();
    return player_swatches[static_cast<std::size_t>(colour)];
}

/// Returns a replay's map's picture, fitted to a square, or none.
///
/// @param building the layout
/// @param row the replay
/// @param square the square
/// @return the picture; empty without one
oa::ui::frontend_renderer::RgbaPicture
replay_picture(Building& building, const ReplayRow& row, const kit::Rect& square) {
    if (!row.picture || row.picture->rgba.empty() || row.picture->width == 0 ||
        row.picture->height == 0)
        return {};
    return fitted_well_picture(building, picture_of_map(*row.picture), square);
}

/// Appends a replay's map picture in a square: the installed map's minimap,
/// or a plain tile while there is none.
///
/// @param[in,out] building the layout
/// @param row the replay
/// @param square the square
/// @param clip what drawing may touch
void add_replay_thumb(
    Building& building, const ReplayRow& row, const kit::Rect& square, const kit::Rect& clip
) {
    const oa::ui::frontend_renderer::RgbaPicture picture = replay_picture(building, row, square);
    if (oa::ui::frontend_renderer::picture_drawable(picture)) {
        add_item(building.list, kit::Role::picture, square, clip).look = kit::PictureLook{picture};
        return;
    }
    add_item(building.list, kit::Role::fill, square, clip).colour = badge_face;
}

/// The most lines the card's note takes before it is cut: a refusal names the
/// unit definitions that differ.
constexpr std::size_t most_note_lines = 6;

/// The most lines a fact's value takes on the card before it is cut.
constexpr std::size_t most_fact_lines = 3;

/// The colour of the light ring round every player's dot.
constexpr kit::Colour player_dot_edge{0xc8, 0xc8, 0xc8};

/// The side of a player's dot, in points: two thirds of a small line.
///
/// @param metrics the class's metrics
/// @return the side
int32_t player_dot_side(const kit::Metrics& metrics) {
    return std::max(metrics.small_line * 2 / 3, int32_t{5});
}

/// Appends a player's colour dot with a light edge, so that a dark colour (a
/// black or a deep blue player) shows on the dark rows and card.
///
/// @param[in,out] building the layout
/// @param rect the dot's square
/// @param colour the game's player colour
/// @param clip what drawing may touch
void add_player_dot(
    Building& building, const kit::Rect& rect, int32_t colour, const kit::Rect& clip
) {
    add_replay_icon(building, ReplayIcon::dot, rect, player_swatch(colour), clip);
    add_replay_icon(building, ReplayIcon::edge, rect, player_dot_edge, clip);
}

/// The rows a day's heading takes in the list.
///
/// @param building the layout
/// @return the heading's height, in points
int32_t replay_day_height(const Building& building) {
    const kit::Metrics& metrics = *building.metrics;
    return metrics.small_line + 2 * metrics.hint_gap;
}

/// Returns a replay row's height: its map's line and the players' line, as
/// a catalogue row holds its title and summary.
///
/// @param building the layout
/// @return the height, in points
int32_t replay_row_height(const Building& building) {
    return row_height(
        building,
        building.size_class == kit::SizeClass::compact ? RowForm::compact : RowForm::regular
    );
}

/// The first eight Unicode scalars of a map's name, padded with `n` when shorter.
///
/// The width of this sample is the shortest readable name at Regular.
///
/// @param title the map's name
/// @return eight scalars
std::string eight_character_sample(std::string_view title) {
    std::string sample;
    std::size_t scalars = 0;
    std::size_t index = 0;
    while (index < title.size() && scalars < 8) {
        const auto lead = static_cast<unsigned char>(title[index]);
        std::size_t bytes = 1;
        if ((lead & 0xE0U) == 0xC0U)
            bytes = 2;
        else if ((lead & 0xF0U) == 0xE0U)
            bytes = 3;
        else if ((lead & 0xF8U) == 0xF0U)
            bytes = 4;
        if (index + bytes > title.size())
            break;
        sample.append(title.substr(index, bytes));
        index += bytes;
        ++scalars;
    }
    while (scalars < 8) {
        sample.push_back('n');
        ++scalars;
    }
    return sample;
}

/// Appends one replay's row.
///
/// @param[in,out] building the layout
/// @param row the replay
/// @param place its place among the visible replays, from 0
/// @param rect the row
/// @param view the list's view, which clips it
void add_replay_row(
    Building& building,
    const ReplayRow& row,
    std::size_t place,
    const kit::Rect& rect,
    const kit::Rect& view
) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    const kit::ControlId id = controls::first_replay_row + static_cast<kit::ControlId>(place);
    const std::vector<std::string>& marked = library.replays.marked;
    const bool selected = library.replays.selected == row.replay.path;
    const bool is_marked = std::find(marked.begin(), marked.end(), row.replay.path) != marked.end();
    if (selected) {
        add_item(building.list, kit::Role::fill, rect, view).colour = kit::colour::list_selected;
        const int32_t bar_height = std::max(rect.height / 3, int32_t{4});
        add_item(
            building.list,
            kit::Role::fill,
            {rect.x, rect.y + (rect.height - bar_height) / 2, 2, bar_height},
            view
        )
            .colour = kit::colour::accent;
    } else if (is_marked || hovered(building, id)) {
        add_item(building.list, kit::Role::fill, rect, view).colour = kit::colour::hover;
    }
    const int32_t inset = metrics.list_row_inset;
    const int32_t side = std::max(rect.height - 2 * inset, int32_t{0});
    const int32_t thumb_x = rect.x + inset + 2;
    int32_t text_left = thumb_x + side + metrics.list_row_gap;
    const std::string clock_words = replay_time_text(row.minute);
    const std::string size_words = size_text(row.replay.size, library.text);
    const int32_t aside_width = std::max(
        width_of(building, kit::FontRole::small, clock_words),
        width_of(building, kit::FontRole::small, size_words)
    );
    const int32_t right = rect.x + rect.width - inset;
    const int32_t aside_left = right - aside_width;
    int32_t text_right = aside_left - metrics.space;
    const int32_t block = metrics.regular_line + metrics.hint_gap + metrics.small_line;
    const int32_t first = rect.y + std::max(rect.height - block, int32_t{0}) / 2;
    const int32_t second = first + metrics.regular_line + metrics.hint_gap;

    // The map, its kind, and a chip for a replay finished after a crash or
    // one whose file does not read.
    const bool damaged = oa::data::replays::damaged(row.replay);
    std::optional<kit::ChipLook> chip;
    if (damaged) {
        chip = kit::ChipLook{};
        chip->text = std::string(shown(library.text, words::replay_damaged));
        chip->state = kit::ChipState::problem;
    } else if (const std::string needed = replay_need_chip(library, row); !needed.empty()) {
        chip = state_chip(needed);
    } else if (row.replay.after_crash) {
        chip = state_chip(std::string(shown(library.text, words::replay_after_crash)));
    }
    int32_t chip_width = chip ? tag_width(building, *chip) : 0;
    // At Regular, a chip that would leave the map's name shorter than about
    // eight characters moves to the second line, in place of the players.
    // The name keeps the first line. The picture and the time stay unless
    // the chip's words need their columns.
    bool chip_on_second = false;
    if (chip && building.size_class == kit::SizeClass::regular) {
        const std::string title = replay_title(row);
        const int32_t least =
            width_of(building, kit::FontRole::regular, eight_character_sample(title));
        const int32_t ellipsis = width_of(building, kit::FontRole::regular, "\xE2\x80\xA6");
        const int32_t beside = text_right - text_left - chip_width - metrics.space;
        const int32_t full = width_of(building, kit::FontRole::regular, title);
        // A name that fits stays. One that would be cut shorter than about
        // eight characters, once the ellipsis takes its room, moves the chip.
        if (full > beside && beside < least + ellipsis)
            chip_on_second = true;
    }
    // A need is never dropped. The map's name and its kind shorten first.
    // A chip wider than that line takes the time and size, then the picture's
    // columns. Only a need longer than the row itself is shortened.
    bool chip_takes_aside = false;
    bool chip_takes_thumb = false;
    if (!chip_on_second && chip && chip_width > text_right - text_left) {
        chip_takes_aside = true;
        text_right = right;
    }
    if (!chip_on_second && chip && chip_width > text_right - text_left) {
        chip_takes_thumb = true;
        text_left = thumb_x;
    }
    if (!chip_on_second && chip && chip_width > text_right - text_left) {
        const int32_t span = std::max(text_right - text_left, int32_t{0});
        const int32_t words = width_of(building, kit::FontRole::small, chip->text);
        // Less padding still draws the words inside the row.
        if (words <= span) {
            chip_width = span;
        } else {
            const int32_t room = std::max(span - 2 * chip_side(metrics), int32_t{0});
            chip->text = cut(building, kit::FontRole::small, chip->text, room);
            chip_width = std::min(tag_width(building, *chip), span);
        }
    }
    // The size shares the second line. A chip whose words need that column
    // takes it, then the picture's. The map's name keeps the first line.
    // Only a need longer than the row itself is shortened.
    int32_t second_left = text_left;
    int32_t second_right = text_right;
    bool chip_hides_size = false;
    if (chip_on_second && chip) {
        const int32_t words = width_of(building, kit::FontRole::small, chip->text);
        if (words > second_right - second_left) {
            chip_hides_size = true;
            second_right = right;
        }
        if (words > second_right - second_left) {
            chip_takes_thumb = true;
            second_left = thumb_x;
            text_left = thumb_x;
        }
    }
    if (!chip_takes_thumb)
        add_replay_thumb(building, row, {thumb_x, rect.y + inset, side, side}, view);
    const int32_t name_room =
        chip_on_second
            ? std::max(text_right - text_left, int32_t{0})
            : std::max(
                  text_right - text_left - (chip ? chip_width + metrics.space : 0), int32_t{0}
              );
    const std::string name = cut(building, kit::FontRole::regular, replay_title(row), name_room);
    const int32_t name_width = width_of(building, kit::FontRole::regular, name);
    // A cut that is still wider than its room would cover the chip.
    int32_t after = text_left;
    if (!name.empty() && name_width <= name_room) {
        add_text(
            building,
            kit::FontRole::regular,
            name,
            {text_left, first, name_width, metrics.regular_line},
            kit::Align::left,
            kit::colour::text,
            view
        );
        after += name_width + metrics.space;
    }
    const int32_t kind_room = std::max(text_left + name_room - after, int32_t{0});
    const std::string kind =
        cut(building, kit::FontRole::small, replay_kind_line(library, row), kind_room);
    const int32_t kind_width = width_of(building, kit::FontRole::small, kind);
    if (!kind.empty() && kind_width <= kind_room) {
        add_text(
            building,
            kit::FontRole::small,
            kind,
            {after,
             first + metrics.regular_line - metrics.small_line,
             kind_width,
             metrics.small_line},
            kit::Align::left,
            kit::colour::hint,
            view
        );
        after += kind_width + metrics.space;
    }
    if (chip && !chip_on_second) {
        const int32_t chip_height_points = chip_height(metrics);
        kit::Item& item = add_item(
            building.list,
            kit::Role::chip,
            {std::min(after, text_right - chip_width),
             first + (metrics.regular_line - chip_height_points) / 2,
             chip_width,
             chip_height_points},
            view
        );
        item.look = std::move(*chip);
    }

    // The players, each with a dot of their colour, and the game's length.
    // At Regular a chip that would swallow the map's name takes this line.
    if (chip_on_second && chip) {
        const int32_t span = std::max(second_right - second_left, int32_t{0});
        const int32_t words = width_of(building, kit::FontRole::small, chip->text);
        int32_t drawn_width = std::min(chip_width, span);
        if (words <= span) {
            drawn_width = std::min(std::max(chip_width, words), span);
        } else {
            const int32_t room = std::max(span - 2 * chip_side(metrics), int32_t{0});
            chip->text = cut(building, kit::FontRole::small, chip->text, room);
            drawn_width = std::min(tag_width(building, *chip), span);
        }
        const int32_t chip_height_points = chip_height(metrics);
        kit::Item& item = add_item(
            building.list,
            kit::Role::chip,
            {second_left,
             second + (metrics.small_line - chip_height_points) / 2,
             drawn_width,
             chip_height_points},
            view
        );
        item.look = std::move(*chip);
    } else {
        const int32_t side_of_dot = player_dot_side(metrics);
        const int32_t dot_top = second + (metrics.small_line - side_of_dot) / 2;
        const int32_t dot_gap = std::max(side_of_dot / 3, int32_t{2});
        int32_t x = text_left;
        for (const ReplayLinePiece& piece : replay_line_pieces(library, row)) {
            if (piece.dot) {
                if (x + side_of_dot > text_right)
                    break;
                add_player_dot(building, {x, dot_top, side_of_dot, side_of_dot}, *piece.dot, view);
                x += side_of_dot + dot_gap;
                continue;
            }
            const std::string drawn = cut(
                building, kit::FontRole::small, piece.text, std::max(text_right - x, int32_t{0})
            );
            const int32_t drawn_width = width_of(building, kit::FontRole::small, drawn);
            if (!drawn.empty())
                add_text(
                    building,
                    kit::FontRole::small,
                    drawn,
                    {x, second, drawn_width, metrics.small_line},
                    kit::Align::left,
                    kit::colour::list_text,
                    view
                );
            x += drawn_width;
            if (drawn != piece.text)
                break;
        }
    }

    // The time it was played, with a kept replay's star, and the file's size.
    // A chip that needed this column takes their place.
    if (!chip_takes_aside) {
        add_text(
            building,
            kit::FontRole::small,
            clock_words,
            {aside_left,
             first + metrics.regular_line - metrics.small_line,
             aside_width,
             metrics.small_line},
            kit::Align::right,
            kit::colour::list_text,
            view
        );
        if (row.replay.kept) {
            const int32_t star = metrics.small_line - 2;
            const int32_t time_width = width_of(building, kit::FontRole::small, clock_words);
            add_replay_icon(
                building,
                ReplayIcon::star,
                {right - time_width - metrics.space / 2 - star,
                 first + metrics.regular_line - metrics.small_line + 1,
                 star,
                 star},
                kit::colour::brand,
                view
            );
        }
        if (!chip_hides_size)
            add_text(
                building,
                kit::FontRole::small,
                size_words,
                {aside_left, second, aside_width, metrics.small_line},
                kit::Align::right,
                kit::colour::hint,
                view
            );
    }

    // The row is a list item, read as its whole line.
    kit::Control control;
    control.id = id;
    control.rect = rect;
    control.clip = view;
    control.name = unique_name(building, named("row.replay." + std::to_string(place + 1)));
    control.kind = kit::ControlKind::list_item;
    control.focusable = false;
    control.group = controls::list_group;
    control.checked = is_marked;
    control.text = replay_reading(library, row);
    building.rows.push_back(std::move(control));
}

/// One part of the replays list, placed from the top of its content: a
/// day's heading or a replay's row.
struct ReplayPart {
    int32_t top{};         ///< its top, from the content's top
    int32_t height{};      ///< its height
    std::string heading{}; ///< a heading's words; empty for a row
    std::size_t place{};   ///< a row's place among the visible replays
};

/// Places the replays list's headings and rows.
///
/// @param building the layout
/// @return the parts, top to bottom
std::vector<ReplayPart> replay_parts(const Building& building) {
    const Library& library = *building.library;
    std::vector<ReplayPart> parts;
    const int32_t row = replay_row_height(building);
    const int32_t day = replay_day_height(building);
    int32_t cursor = 0;
    for (const ReplayDay& group : replay_days(library)) {
        if (!group.heading.empty()) {
            parts.push_back({cursor, day, group.heading, 0});
            cursor += day;
        }
        for (std::size_t place = group.first; place < group.first + group.count; ++place) {
            parts.push_back({cursor, row, {}, place});
            cursor += row + 1;
        }
    }
    return parts;
}

/// Appends what the Replays section shows while no row shows: the film, the
/// words, Turn on recording while the setting does not record, and Open a
/// replay file where the platform has a file dialog.
///
/// @param[in,out] building the layout
/// @param area where it is centred
void add_replays_empty(Building& building, const kit::Rect& area) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    const ReplayEmpty empty = replay_empty(library);
    int32_t icon = std::min(3 * metrics.regular_line, std::max(area.height / 4, int32_t{0}));
    const std::vector<std::string> title =
        wrapped(building, kit::FontRole::regular, empty.title, area.width, 2);
    const std::vector<std::string> line =
        empty.line.empty() ? std::vector<std::string>{}
                           : wrapped(building, kit::FontRole::small, empty.line, area.width, 3);
    const int button_count = (empty.turn_on ? 1 : 0) + (empty.open_file ? 1 : 0);
    // A second button takes the room the film would have used, so the empty
    // section stays inside the window it already fitted with one button.
    if (button_count > 1)
        icon = std::max(icon - (metrics.row_gap + metrics.control_height), int32_t{0});
    int32_t height =
        icon + metrics.row_gap + static_cast<int32_t>(title.size()) * metrics.regular_line;
    if (!line.empty())
        height += metrics.hint_gap + static_cast<int32_t>(line.size()) * metrics.small_line;
    height += static_cast<int32_t>(button_count) * (metrics.row_gap + metrics.control_height);
    int32_t top = area.y + std::max(area.height - height, int32_t{0}) / 2;
    const int32_t middle = area.x + area.width / 2;
    if (icon > 0) {
        add_replay_icon(
            building,
            ReplayIcon::film,
            {middle - icon / 2, top, icon, icon},
            kit::colour::hint,
            area
        );
        top += icon + metrics.row_gap;
    }
    for (const std::string& text : title) {
        add_text(
            building,
            kit::FontRole::regular,
            text,
            {area.x, top, area.width, metrics.regular_line},
            kit::Align::centre,
            kit::colour::text,
            area
        );
        top += metrics.regular_line;
    }
    add_label(
        building.panes,
        controls::status,
        {area.x,
         top - static_cast<int32_t>(title.size()) * metrics.regular_line,
         area.width,
         static_cast<int32_t>(title.size()) * metrics.regular_line},
        unique_name(building, named("status")),
        empty.line.empty()
            ? empty.title
            : filled(
                  library.text, words::replay_joined, {{"left", empty.title}, {"right", empty.line}}
              ),
        {},
        -1
    );
    if (!line.empty()) {
        top += metrics.hint_gap;
        for (const std::string& text : line) {
            add_text(
                building,
                kit::FontRole::small,
                text,
                {area.x, top, area.width, metrics.small_line},
                kit::Align::centre,
                kit::colour::list_text,
                area
            );
            top += metrics.small_line;
        }
    }
    const auto place_button = [&](Action action) {
        const std::string_view caption = action_text(action, library.text);
        const int32_t across = std::min(button_width(building, caption), area.width);
        top += metrics.row_gap;
        add_button(
            building,
            building.panes,
            {middle - across / 2, top, across, metrics.control_height},
            caption,
            false,
            true,
            controls::action_control(action),
            named("action." + std::string(action_word(action))),
            {},
            -1
        );
        top += metrics.control_height;
    };
    if (empty.turn_on)
        place_button(Action::turn_on_recording);
    if (empty.open_file)
        place_button(Action::open_replay_file);
}

/// Appends the replays list: each day's heading and its rows, scrolled by
/// the list's scroll bar, the selected row brought into view when asked.
///
/// @param[in,out] building the layout
/// @param pane the list's pane
void add_replay_list(Building& building, const kit::Rect& pane) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    const int32_t well_width = metrics.scroll_thumb_width + 2;
    const int32_t rows_width = std::max(pane.width - well_width - metrics.button_gap, int32_t{0});
    const kit::Rect view = pane;
    const std::vector<ReplayPart> parts = replay_parts(building);
    const int32_t content = parts.empty() ? 0 : parts.back().top + parts.back().height;
    const int32_t limit = kit::ScrollArea::limit_for(content, view.height);
    if (building.state->reveal_row >= 0) {
        // The selected row comes into view, and the scroll stays where it
        // is while it shows.
        for (std::size_t index = 0; index < parts.size(); ++index) {
            const ReplayPart& part = parts[index];
            if (!part.heading.empty() ||
                part.place != static_cast<std::size_t>(building.state->reveal_row))
                continue;
            // The day's heading over a day's first row comes into view with it.
            int32_t top = part.top;
            if (index > 0 && !parts[index - 1].heading.empty())
                top = parts[index - 1].top;
            building.state->list_scroll = shown_offset(
                building.state->list_scroll, top, part.top + part.height - top, view.height, limit
            );
            break;
        }
        building.state->reveal_row = -1;
    }
    const int32_t offset = std::clamp(building.state->list_scroll, int32_t{0}, limit);
    building.state->list_scroll = offset;
    const std::vector<std::size_t>& visible = library.replays.visible;
    for (const ReplayPart& part : parts) {
        const int32_t top = view.y - offset + part.top;
        if (top + part.height <= view.y || top >= view.y + view.height)
            continue;
        if (!part.heading.empty()) {
            const std::string heading = heading_of(part.heading);
            kit::Item& item = add_item(
                building.list,
                kit::Role::heading,
                {view.x + 4, top + metrics.hint_gap, rows_width - 4, metrics.small_line},
                view
            );
            item.text = heading;
            item.look = kit::HeadingLook{heading};
            add_item(
                building.list, kit::Role::rule, {view.x, top + part.height - 1, rows_width, 1}, view
            )
                .colour = kit::colour::rule;
            continue;
        }
        const ReplayRow& row = library.inputs.replays.rows[visible[part.place]];
        add_replay_row(building, row, part.place, {view.x, top, rows_width, part.height}, view);
        add_item(building.list, kit::Role::rule, {view.x, top + part.height, rows_width, 1}, view)
            .colour = kit::colour::rule;
    }
    // The content's first and last rows, so its height reads from the list.
    add_extent_edge(building.list, view, view.y - offset);
    add_extent_edge(building.list, view, view.y - offset + content - 1);
    if (limit > 0) {
        kit::ScrollArea scroll;
        scroll.view = view;
        scroll.well = {view.x + view.width - well_width, view.y, well_width, view.height};
        scroll.page_step = view.height;
        scroll.content_height = content;
        scroll.limit = limit;
        scroll.offset = offset;
        add_item(building.list, kit::Role::scroll_bar, scroll.well, {}).look = kit::ScrollBarLook{
            scroll.thumb(metrics.least_thumb_height), building.state->bar_held == Pane::list
        };
    }
    kit::Control pane_control;
    pane_control.id = controls::list;
    pane_control.rect = view;
    pane_control.name = unique_name(building, named("list"));
    pane_control.kind = kit::ControlKind::list;
    building.panes.push_back(std::move(pane_control));
    tab_stop(building, building.panes.back());
}

/// Converts the card's buttons into the details' actions, to lay them out as those are.
///
/// @param library the model
/// @return the buttons as actions
std::vector<ActionButton> replay_actions(const Library& library) {
    std::vector<ActionButton> actions;
    for (const ReplayButton& button : replay_buttons(library))
        actions.push_back({button.action, button.enabled, button.text, button.danger});
    return actions;
}

/// The card's buttons. A primary caption wider than the bar uses the shorter
/// Watch words, so the caption is not cut and the other buttons stay.
///
/// @param building the layout
/// @param width the bar's width, in points
/// @return the buttons
std::vector<ActionButton> fitted_replay_actions(const Building& building, int32_t width) {
    std::vector<ActionButton> actions = replay_actions(*building.library);
    if (actions.empty() || actions.front().action != Action::watch_replay)
        return actions;
    if (button_width(building, actions.front().text) <= width)
        return actions;
    const std::string shorter = replay_watch_short(*building.library);
    if (!shorter.empty())
        actions.front().text = shorter;
    return actions;
}

/// Appends the selected replay's details: its picture, title, kind and
/// time, the marks, why it cannot be watched, its players and its facts,
/// from the top of a column. A fact's value wraps to a second or a third
/// line rather than being cut.
///
/// @param[in,out] building the layout
/// @param row the replay
/// @param left the column's left
/// @param top its top
/// @param width its width
/// @param clip what drawing may touch
/// @return the row under the last part
int32_t add_replay_details(
    Building& building,
    const ReplayRow& row,
    int32_t left,
    int32_t top,
    int32_t width,
    const kit::Rect& clip
) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    const bool large = building.size_class == kit::SizeClass::large;
    const int32_t thumb = large ? details_badge_side + details_badge_side / 2 : details_badge_side;
    add_replay_thumb(building, row, {left, top, thumb, thumb}, clip);
    const int32_t beside = left + thumb + metrics.list_row_gap;
    const int32_t beside_width = std::max(left + width - beside, int32_t{0});
    int32_t y = top;
    add_text(
        building,
        kit::FontRole::regular,
        cut(building, kit::FontRole::regular, replay_title(row), beside_width),
        {beside, y, beside_width, metrics.regular_line},
        kit::Align::left,
        kit::colour::text,
        clip
    );
    y += metrics.regular_line + metrics.hint_gap;
    for (const std::string& line :
         {replay_card_kind(library, row), replay_when_line(library, row)}) {
        add_text(
            building,
            kit::FontRole::small,
            cut(building, kit::FontRole::small, line, beside_width),
            {beside, y, beside_width, metrics.small_line},
            kit::Align::left,
            kit::colour::hint,
            clip
        );
        y += metrics.small_line + metrics.hint_gap;
    }
    // The replay's whole line is the card's label: automation reads it, and it
    // sits in the details' group, which gives the card its scroll.
    add_label(
        building.detail,
        controls::status,
        {beside, top, beside_width, std::max(y - top, metrics.regular_line)},
        unique_name(building, named("status")),
        replay_reading(library, row),
        clip,
        controls::details_group
    );
    y = std::max(y, top + thumb) + part_gap(metrics);

    const std::size_t marked = library.replays.marked.size();
    if (marked > 1) {
        add_text(
            building,
            kit::FontRole::small,
            cut(building,
                kit::FontRole::small,
                filled(library.text, words::replays_marked, {{"count", std::to_string(marked)}}),
                width),
            {left, y, width, metrics.small_line},
            kit::Align::left,
            kit::colour::accent,
            clip
        );
        y += metrics.small_line + part_gap(metrics);
    }

    // Content the replay still needs, in the kit's caution box. A damaged
    // file and a refusal stay in the note below, as they were.
    const std::string caution = replay_caution(library, row);
    if (!caution.empty()) {
        const int32_t pad = metrics.space;
        const int32_t inner_width = std::max(width - 2 * pad, int32_t{0});
        const std::vector<std::string> lines =
            wrapped(building, kit::FontRole::small, caution, inner_width, most_note_lines);
        const int32_t box_height =
            pad +
            static_cast<int32_t>(std::max(lines.size(), std::size_t{1})) * metrics.small_line + pad;
        add_item(building.list, kit::Role::fill, {left, y, width, box_height}, clip).colour =
            kit::colour::caution_tint;
        int32_t line_y = y + pad;
        for (const std::string& line : lines) {
            add_text(
                building,
                kit::FontRole::small,
                line,
                {left + pad, line_y, inner_width, metrics.small_line},
                kit::Align::left,
                kit::colour::lock,
                clip
            );
            line_y += metrics.small_line;
        }
        add_label(
            building.detail,
            controls::note,
            {left, y, width, box_height},
            unique_name(building, named("note")),
            caution,
            clip,
            -1
        );
        y += box_height + part_gap(metrics);
    }

    // Why the replay cannot be watched: its file does not read, or the game
    // refused it. A Delete the trash refused is said here too, first.
    const bool problem =
        oa::data::replays::damaged(row.replay) ||
        (!library.replays.refusal.empty() && library.replays.refused_path == row.replay.path);
    const bool delete_failed = !library.replays.delete_failure.empty() &&
                               library.replays.selected &&
                               *library.replays.selected == row.replay.path;
    const std::string note = problem         ? replay_note(library, row)
                             : delete_failed ? library.replays.delete_failure
                                             : std::string{};
    if (!note.empty()) {
        const std::vector<std::string> lines =
            wrapped(building, kit::FontRole::small, note, width, most_note_lines);
        const int32_t note_top = y;
        for (const std::string& line : lines) {
            add_text(
                building,
                kit::FontRole::small,
                line,
                {left, y, width, metrics.small_line},
                kit::Align::left,
                kit::colour::lock,
                clip
            );
            y += metrics.small_line;
        }
        add_label(
            building.detail,
            controls::note,
            {left, note_top, width, y - note_top},
            unique_name(building, named("note")),
            note,
            clip,
            -1
        );
        y += part_gap(metrics);
    }

    // The players: each one's colour, name and side. Every player is laid
    // out: the card scrolls, so none is left out for want of room.
    for (const oa::data::replays::Player& player : row.replay.players) {
        const int32_t side = player_dot_side(metrics);
        add_player_dot(
            building, {left, y + (metrics.small_line - side) / 2, side, side}, player.colour, clip
        );
        std::string_view side_word = player.side;
        if (player.side == "arm")
            side_word = shown(library.text, words::replay_side_arm);
        else if (player.side == "core")
            side_word = shown(library.text, words::replay_side_core);
        else if (player.side == "watcher" || player.kind == "watcher")
            side_word = shown(library.text, words::replay_side_watcher);
        const std::string side_text(side_word);
        const int32_t side_width =
            std::min(width_of(building, kit::FontRole::small, side_text), width / 3);
        const int32_t name_left = left + side + metrics.space;
        const int32_t name_room =
            std::max(left + width - side_width - metrics.space - name_left, int32_t{0});
        add_text(
            building,
            kit::FontRole::small,
            cut(building, kit::FontRole::small, player.name, name_room),
            {name_left, y, name_room, metrics.small_line},
            kit::Align::left,
            kit::colour::text,
            clip
        );
        add_text(
            building,
            kit::FontRole::small,
            cut(building, kit::FontRole::small, side_text, side_width),
            {left + width - side_width, y, side_width, metrics.small_line},
            kit::Align::right,
            kit::colour::hint,
            clip
        );
        y += metrics.small_line + metrics.hint_gap;
    }
    y += part_gap(metrics);

    // The facts, their labels in a column of their own, and each value wrapped
    // to the room beside it.
    const std::vector<ReplayFact> facts = replay_facts(library, row);
    int32_t label_width = 0;
    for (const ReplayFact& fact : facts)
        label_width = std::max(label_width, width_of(building, kit::FontRole::small, fact.label));
    label_width = std::min(label_width, width / 2);
    const int32_t value_left = left + label_width + metrics.space;
    const int32_t value_width = std::max(left + width - value_left, int32_t{0});
    for (std::size_t fact_place = 0; fact_place < facts.size(); ++fact_place) {
        const ReplayFact& fact = facts[fact_place];
        add_text(
            building,
            kit::FontRole::small,
            cut(building, kit::FontRole::small, fact.label, label_width),
            {left, y, label_width, metrics.small_line},
            kit::Align::left,
            kit::colour::hint,
            clip
        );
        const std::vector<std::string> value_lines =
            wrapped(building, kit::FontRole::small, fact.value, value_width, most_fact_lines);
        const int32_t fact_top = y;
        for (const std::string& line : value_lines) {
            add_text(
                building,
                kit::FontRole::small,
                line,
                {value_left, y, value_width, metrics.small_line},
                kit::Align::left,
                kit::colour::text,
                clip
            );
            y += metrics.small_line;
        }
        if (value_lines.empty())
            y += metrics.small_line;
        // Each fact is a label, its label and value read as one, so that a
        // screen reader and automation find it where it is scrolled to.
        add_label(
            building.detail,
            controls::first_replay_fact + static_cast<kit::ControlId>(fact_place),
            {left, fact_top, width, y - fact_top},
            unique_name(building, named("fact." + fact.key)),
            filled(
                library.text, words::replay_joined, {{"left", fact.label}, {"right", fact.value}}
            ),
            clip,
            -1
        );
        y += metrics.hint_gap;
    }
    return y;
}

/// Appends the card's buttons, as the details' actions are laid out.
///
/// @param[in,out] building the layout
/// @param bar where they lie
/// @param group the group their controls go to
void add_replay_buttons(
    Building& building, const kit::Rect& bar, std::vector<kit::Control>& group
) {
    const std::vector<ActionButton> actions = fitted_replay_actions(building, bar.width);
    const ActionPlan plan = plan_actions(building, actions, bar.width);
    for (std::size_t index = 0; index < actions.size() && index < plan.buttons.size(); ++index) {
        const ActionButton& action = actions[index];
        const kit::Rect& place = plan.buttons[index];
        add_button(
            building,
            group,
            {bar.x + place.x, bar.y + place.y, place.width, place.height},
            action.text,
            action.action == Action::watch_replay,
            action.enabled,
            controls::action_control(action.action),
            named("action." + std::string(action_word(action.action))),
            {},
            -1,
            true,
            action.danger
        );
    }
}

/// Appends the selected replay's card in a bordered card, as the catalogue's
/// details are. Its parts scroll when they are taller than the room above the
/// buttons, and the buttons sit at the card's foot, which does not scroll.
///
/// @param[in,out] building the layout
/// @param whole the card
void add_replay_card(Building& building, const kit::Rect& whole) {
    const ReplayRow* row = selected_replay(*building.library);
    if (row == nullptr)
        return;
    const kit::Metrics& metrics = *building.metrics;
    const int32_t pad = 2 * metrics.space;
    add_card_face(building, whole);
    const kit::Rect inner{
        whole.x + pad,
        whole.y + pad,
        std::max(whole.width - 2 * pad, int32_t{0}),
        std::max(whole.height - 2 * pad, int32_t{0}),
    };
    const ActionPlan plan =
        plan_actions(building, fitted_replay_actions(building, inner.width), inner.width);
    const kit::Rect pane{
        inner.x,
        inner.y,
        inner.width,
        std::max(inner.height - plan.height - metrics.row_gap, int32_t{0})
    };
    const auto lay_out_body = [&](Building& into, int32_t width, int32_t offset) {
        const std::size_t first_item = into.list.items.size();
        const int32_t first = pane.y - offset;
        static_cast<void>(add_replay_details(into, *row, pane.x, first, width, pane));
        const std::optional<controls::Extent> extent =
            controls::clipped_extent(into.list, pane, first_item);
        return extent ? extent->bottom - first : 0;
    };
    // Laid out once at the pane's width to learn its height, then again
    // narrower when that is taller than the pane.
    Building trial = building;
    trial.list = {};
    trial.detail.clear();
    trial.order.clear();
    const int32_t whole_height = lay_out_body(trial, pane.width, 0);
    const int32_t well_width = metrics.scroll_thumb_width + 2;
    const bool scrolls = whole_height > pane.height;
    const int32_t width = scrolls ? pane.width - well_width - metrics.button_gap : pane.width;
    int32_t height = whole_height;
    if (scrolls) {
        trial = building;
        trial.list = {};
        trial.detail.clear();
        trial.order.clear();
        height = lay_out_body(trial, width, 0);
    }
    const int32_t limit = kit::ScrollArea::limit_for(height, pane.height);
    const int32_t offset = std::clamp(building.state->details_scroll, int32_t{0}, limit);
    // Its parts scroll with the card: they are in the details' group.
    const std::size_t first_detail = building.detail.size();
    lay_out_body(building, width, offset);
    for (std::size_t index = first_detail; index < building.detail.size(); ++index)
        building.detail[index].group = controls::details_group;
    if (limit > 0) {
        kit::ScrollArea scroll;
        scroll.view = pane;
        scroll.well = {pane.x + pane.width - well_width, pane.y, well_width, pane.height};
        scroll.content_height = height;
        scroll.limit = limit;
        scroll.offset = offset;
        add_item(building.list, kit::Role::scroll_bar, scroll.well, {}).look = kit::ScrollBarLook{
            scroll.thumb(metrics.least_thumb_height), building.state->bar_held == Pane::details
        };
    }
    add_replay_buttons(
        building,
        {inner.x, inner.y + inner.height - plan.height, inner.width, plan.height},
        building.detail
    );
}

/// Returns the words the kind menu's field shows.
///
/// @param library the model
/// @param items the menu's items
/// @return the chosen item's words
std::string
replay_kind_caption(const Library& library, const std::vector<controls::MenuItem>& items) {
    for (const controls::MenuItem& item : items)
        if (item.replay_kind && *item.replay_kind == library.replays.kind)
            return item.caption;
    return replay_kind_title(library, library.replays.kind);
}

/// Appends the bar over the replays list: the kind menu and the sort menu.
///
/// @param[in,out] building the layout
/// @param row the bar
void add_replay_bar(Building& building, const kit::Rect& row) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    std::vector<controls::MenuItem> kinds =
        controls::menu_items(library, MenuKind::replay_kind, {});
    std::vector<controls::MenuItem> sorts =
        controls::menu_items(library, MenuKind::replay_sort, {});
    const std::string kind_caption = replay_kind_caption(library, kinds);
    const std::string sort_caption = replay_sort_title(library, library.replays.sort);
    const int32_t gap = metrics.space;
    // Each field is as wide as its widest item when both fit the row. When they do
    // not, each is as wide as the words it shows, and the open menu is wider
    // than its field (add_drop_down), so that its items are whole.
    int32_t kind_width = drop_down_width(building, kind_caption, kinds);
    int32_t sort_width = drop_down_width(building, sort_caption, sorts);
    if (kind_width + gap + sort_width > row.width) {
        const auto caption_width = [&](const std::string& caption) {
            return width_of(building, kit::FontRole::regular, caption) + metrics.choice_text_inset +
                   metrics.choice_arrow_room;
        };
        kind_width = caption_width(kind_caption);
        sort_width = caption_width(sort_caption);
        const int32_t room = std::max(row.width - gap, int32_t{0});
        if (kind_width + sort_width > room) {
            // Neither caption is cut while the other can give: the wider yields first.
            const int32_t half = room / 2;
            if (kind_width > half && sort_width > half)
                kind_width = sort_width = half;
            else if (kind_width > half)
                kind_width = room - sort_width;
            else
                sort_width = room - kind_width;
        }
    }
    add_drop_down(
        building,
        building.top,
        {row.x, row.y, kind_width, row.height},
        MenuKind::replay_kind,
        kind_caption,
        std::move(kinds),
        controls::replay_kind_menu,
        named("replays.kind"),
        true
    );
    add_drop_down(
        building,
        building.top,
        {row.x + kind_width + gap, row.y, sort_width, row.height},
        MenuKind::replay_sort,
        sort_caption,
        std::move(sorts),
        controls::replay_sort_menu,
        named("replays.sort"),
        true
    );
}

/// Appends the Replays section under its heading: the bar, then the list
/// and, at Regular and Large, the selected replay's card beside it. With no
/// row to show, the words of the empty section take the list's and the
/// card's place.
///
/// @param[in,out] building the layout
/// @param area the section's body, under the heading
/// @param window the window
void add_replays(Building& building, const kit::Rect& area, const Window& window) {
    const Library& library = *building.library;
    const bool compact = building.size_class == kit::SizeClass::compact;
    const bool large = building.size_class == kit::SizeClass::large;
    add_replay_bar(building, {area.x, area.y, area.width, window.field_height});
    const int32_t top = area.y + window.field_height + window.gap;
    const kit::Rect body{area.x, top, area.width, std::max(area.y + area.height - top, int32_t{0})};
    if (library.replays.visible.empty()) {
        add_replays_empty(building, body);
        return;
    }
    int32_t list_width = body.width;
    if (!compact) {
        const int32_t beside = std::max(body.width - window.gutter - details_floor, int32_t{1});
        list_width = large ? std::min(large_list_width, beside) : beside;
        if (list_width + window.gutter >= body.width)
            list_width = std::max(body.width / 2, int32_t{1});
    }
    add_replay_list(building, {body.x, body.y, list_width, body.height});
    if (!compact) {
        const int32_t card_left = body.x + list_width + window.gutter;
        add_replay_card(
            building,
            {card_left, body.y, std::max(body.x + body.width - card_left, int32_t{0}), body.height}
        );
    }
}

/// Appends the Replays section's line at the footer's left: how far the
/// folder has been read, with a bar, or what the list shows.
///
/// @param[in,out] building the layout
/// @param window the window
/// @param width the room the buttons leave, in points
void add_replays_footer_line(Building& building, const Window& window, int32_t width) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    const std::string line = replay_status(library);
    if (line.empty())
        return;
    const bool bar = library.inputs.replays.reading && library.inputs.replays.total > 0;
    const int32_t block =
        metrics.small_line + (bar ? metrics.hint_gap + metrics.progress_bar_height : 0);
    const int32_t line_top =
        window.footer_top + std::max(window.footer_height - block, int32_t{0}) / 2;
    const std::string drawn = cut(building, kit::FontRole::small, line, width);
    const kit::Rect box{window.left, line_top, width, metrics.small_line};
    add_text(building, kit::FontRole::small, drawn, box, kit::Align::left, kit::colour::text, {});
    add_label(
        building.footer,
        controls::replays_status,
        box,
        unique_name(building, named("replays.status")),
        drawn,
        {},
        -1
    );
    if (bar) {
        const ReplayInputs& inputs = library.inputs.replays;
        const auto done = static_cast<int32_t>(
            std::min(inputs.read, inputs.total) * static_cast<std::size_t>(progress_whole) /
            inputs.total
        );
        kit::add_progress(
            building.list,
            {window.left,
             box.y + box.height + metrics.hint_gap,
             width,
             metrics.progress_bar_height},
            done,
            progress_whole
        );
    }
}

/// Appends ADD REGISTRY… at the top right of the Registries tab, above the
/// list, and moves the body down past it. During a match the padlock and
/// "Choose from the main menu." stand there, and the button is absent. The
/// button joins Tab's order later, after the rows, so it follows them.
///
/// @param[in,out] building the layout
/// @param window the window
/// @param[in,out] top the body's first row; moved down past the button
void add_registry_bar(Building& building, const Window& window, int32_t& top) {
    const Library& library = *building.library;
    if (library.tab != Tab::registries)
        return;
    const kit::Metrics& metrics = *building.metrics;
    const int32_t height = window.button_height;
    const int32_t room = std::max(window.right - window.left, int32_t{0});
    if (library.inputs.in_match) {
        const int32_t lock_width = std::min(
            width_of(
                building, kit::FontRole::small, shown(library.text, words::registry_game_lock)
            ) + metrics.padlock_width +
                metrics.padlock_gap,
            room
        );
        if (lock_width > 0) {
            kit::LockLook look;
            look.text = std::string(shown(library.text, words::registry_game_lock));
            kit::add_lock(
                building.list, {window.right - lock_width, top, lock_width, height}, look
            );
        }
    } else {
        const std::string_view caption = shown(library.text, words::registry_add);
        const int32_t button = std::min(button_width(building, caption), room);
        if (button > 0) {
            add_button(
                building,
                building.top,
                {window.right - button, top, button, height},
                caption,
                false,
                true,
                controls::add_registry,
                named("add-registry"),
                {},
                -1,
                false
            );
        }
    }
    top += height + window.gap;
}

/// Appends the footer's band: the line over it and the band, Kit 2's footer
/// height (Window::footer_height) under the line.
///
/// @param[in,out] building the layout
/// @param window the window
void add_footer_band(Building& building, const Window& window) {
    const int32_t edge = building.metrics->edge;
    const int32_t inner = window.width - 2 * edge;
    add_item(building.list, kit::Role::rule, {edge, window.footer_rule, inner, 1}, {}).colour =
        kit::colour::rule;
    add_item(
        building.list, kit::Role::fill, {edge, window.footer_top, inner, window.footer_height}, {}
    )
        .colour = kit::colour::band;
}

/// Appends the footer's band, the queue's words at its left and its buttons
/// at its right: DETAILS at Compact but not on the Active or Registries
/// tabs, and CLOSE, the main action. On the Registries tab, ADD REGISTRY…
/// was drawn above the list; it joins Tab's order here, before CLOSE.
///
/// @param[in,out] building the layout
/// @param window the window
/// @param compact the class is Compact: the queue's short line, and DETAILS
void add_list_footer(Building& building, const Window& window, bool compact) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    add_footer_band(building, window);

    struct FooterButton {
        Action action{};
        kit::ControlId id{};
        bool enabled{};
    };

    std::vector<FooterButton> buttons;
    const bool replays = library.tab == Tab::replays;
    if (compact && replays)
        buttons.push_back(
            {Action::details, controls::details, selected_replay(library) != nullptr}
        );
    else if (compact && library.tab != Tab::active && library.tab != Tab::registries)
        buttons.push_back({Action::details, controls::details, library.selected.has_value()});
    buttons.push_back({Action::close, controls::close, true});
    int32_t total = 0;
    for (const FooterButton& button : buttons)
        total += button_width(building, action_text(button.action, library.text)) +
                 button_spacing(metrics);
    total -= button_spacing(metrics);
    int32_t left = window.right - total;
    const int32_t queue_right = left - metrics.label_gap;
    if (library.tab == Tab::registries) {
        for (kit::Control& control : building.top)
            if (control.id == controls::add_registry)
                tab_stop(building, control);
    }
    for (const FooterButton& button : buttons) {
        const std::string_view caption = action_text(button.action, library.text);
        const int32_t width = button_width(building, caption);
        add_button(
            building,
            building.footer,
            {left, window.button_top, width, window.button_height},
            caption,
            button.action == Action::close,
            button.enabled,
            button.id,
            named(std::string(action_word(button.action))),
            {},
            -1
        );
        left += width + button_spacing(metrics);
    }

    const int32_t queue_width = std::max(queue_right - window.left, int32_t{0});
    // Replays says what its list shows, or how far its folder has been read,
    // in the queue's place.
    if (replays) {
        add_replays_footer_line(building, window, queue_width);
        return;
    }
    const QueueLines lines = queue_lines(library);
    // Compact keeps the one short line. Regular and Large keep the two lines.
    // While a download runs, the same 8-point bar sits under the first line.
    const std::string primary = compact ? lines.compact : lines.first;
    if (primary.empty())
        return;
    const bool second_line = !compact && !lines.second.empty();
    const QueueItem* downloading = nullptr;
    for (const QueueItem& item : library.inputs.queue)
        if (item.phase == QueueItem::Phase::downloading) {
            downloading = &item;
            break;
        }
    const bool bar = downloading != nullptr;
    const int32_t line_count = second_line ? 2 : 1;
    int32_t gap = bar ? metrics.hint_gap : 0;
    int32_t block = line_count * metrics.small_line + (bar ? gap + metrics.progress_bar_height : 0);
    if (bar && block > window.footer_height) {
        gap = std::max(
            window.footer_height - line_count * metrics.small_line - metrics.progress_bar_height,
            int32_t{0}
        );
        block = line_count * metrics.small_line + gap + metrics.progress_bar_height;
    }
    const int32_t line_top = window.footer_top + (window.footer_height - block) / 2;
    const std::string first = cut(building, kit::FontRole::small, primary, queue_width);
    const kit::Rect box{window.left, line_top, queue_width, metrics.small_line};
    add_text(building, kit::FontRole::small, first, box, kit::Align::left, kit::colour::text, {});
    add_label(
        building.footer, controls::queue, box, unique_name(building, named("queue")), first, {}, -1
    );
    if (bar) {
        int32_t done = 0;
        if (downloading->total_bytes != 0)
            done = static_cast<int32_t>(
                std::min(downloading->done_bytes, downloading->total_bytes) *
                static_cast<uint64_t>(progress_whole) / downloading->total_bytes
            );
        kit::add_progress(
            building.list,
            {window.left, box.y + box.height + gap, queue_width, metrics.progress_bar_height},
            done,
            progress_whole
        );
    }
    if (second_line)
        add_text(
            building,
            kit::FontRole::small,
            cut(building, kit::FontRole::small, lines.second, queue_width),
            {window.left,
             line_top + metrics.small_line + (bar ? gap + metrics.progress_bar_height : 0),
             queue_width,
             metrics.small_line},
            kit::Align::left,
            kit::colour::hint,
            {}
        );
}

/// Appends the focus ring round the control that has the focus, unless the
/// control draws its own: a nav entry, a pill, or the search field's border.
///
/// @param[in,out] building the layout
void add_focus_ring(Building& building) {
    const kit::Interaction& interaction = building.state->interaction;
    if (!interaction.focus_shown || interaction.focused == kit::no_control ||
        interaction.focused == controls::search || interaction.focused == controls::active_search)
        return;
    for (const kit::Item& item : building.list.items)
        if (item.control == interaction.focused &&
            (item.role == kit::Role::nav || item.role == kit::Role::pill))
            return;
    for (const auto* group : {&building.top, &building.panes, &building.detail, &building.footer}) {
        for (const kit::Control& control : *group) {
            if (control.id != interaction.focused || !control.focusable)
                continue;
            if (control.clip.width > 0 && control.clip.height > 0) {
                const kit::Rect shown = kit::intersect(control.rect, control.clip);
                if (shown.width <= 0 || shown.height <= 0)
                    return;
            }
            const bool on_edge =
                control.kind == kit::ControlKind::tab || control.kind == kit::ControlKind::list;
            add_item(building.list, kit::Role::focus_ring, control.rect, control.clip).look =
                kit::FocusRingLook{!on_edge};
            return;
        }
    }
}

/// Appends the open drop-down's menu over everything, and a control for
/// each item it shows.
///
/// @param[in,out] building the layout
/// @param footer_line the row the menu keeps above unless it opens upward
void add_open_menu(Building& building, int32_t footer_line) {
    if (!building.open)
        return;
    const OpenMenu& menu_open = *building.open;
    const kit::Metrics& metrics = *building.metrics;
    const auto count = static_cast<int32_t>(menu_open.items.size());
    const kit::Rect menu = kit::choice_menu(menu_open.field, menu_open.items.size(), footer_line);
    const int32_t shown_count = kit::shown_choices(menu_open.items.size());
    const int32_t first = std::clamp(
        building.state->menu_first, int32_t{0}, std::max(count - shown_count, int32_t{0})
    );
    kit::ChoiceMenuLook look;
    look.first = first;
    look.total = count;
    look.chosen = menu_open.chosen;
    look.marked = building.state->menu_marked;
    look.focus_shown = building.state->interaction.focus_shown;
    // A menu with a heading names each row: a heading is a divider, which
    // takes no press and no name.
    const bool headed = std::any_of(
        menu_open.items.begin(), menu_open.items.end(), [](const controls::MenuItem& item) {
            return item.heading;
        }
    );
    if (headed)
        for (const controls::MenuItem& item : menu_open.items) {
            kit::ChoiceMenuRow row;
            row.id = item.heading ? std::string{} : item.word;
            row.divider = item.heading;
            row.choosable = !item.heading;
            look.items.push_back(std::move(row));
        }
    for (int32_t place = 0; place < shown_count && first + place < count; ++place) {
        const controls::MenuItem& item = menu_open.items[static_cast<std::size_t>(first + place)];
        const kit::Rect box = kit::choice_item(menu, place);
        look.shown.push_back(
            cut(building,
                kit::FontRole::regular,
                item.caption,
                box.width - metrics.choice_item_text_inset - metrics.list_row_inset)
        );
        if (item.heading)
            continue;
        kit::Control control;
        control.id = controls::first_menu_item + first + place;
        control.rect = box;
        control.name = unique_name(building, menu_open.name + "." + item.word);
        control.kind = kit::ControlKind::list_item;
        control.focusable = false;
        control.checked = first + place == menu_open.chosen;
        control.text = look.shown.back();
        building.menu.push_back(std::move(control));
    }
    add_item(building.list, kit::Role::choice_menu, menu, {}).look = std::move(look);
}

/// Starts a layout: the window's face.
///
/// @param library the model
/// @param state the screen's state
/// @param frame the frame
/// @param fonts the fonts
/// @return the layout, with the face drawn
Building start(
    const Library& library,
    const ScreenState& state,
    const kit::Frame& frame,
    const kit::Fonts& fonts
) {
    Building building;
    building.library = &library;
    building.state = &state;
    building.fonts = &fonts;
    building.metrics = &kit::metrics_of(frame.size_class);
    building.size_class = frame.size_class;
    building.scale_percent = frame.scale_percent > 0 ? frame.scale_percent : 100;
    library.scaled_badge_pixels.clear();
    const kit::Point size = window_size(frame);
    add_item(building.list, kit::Role::fill, {0, 0, size.x, size.y}, {}).colour =
        kit::colour::panel;
    return building;
}

/// Appends one line of a key tooltip, splitting it so each piece fits the
/// card's text width. A public key that is wider than the window is split
/// across consecutive lines, so the card shows every character; a sentence
/// breaks where the line breaker lets it (oa::base::text::first_row), at a
/// space or between Chinese or Japanese characters, and never inside a
/// character.
///
/// @param building the layout
/// @param[in,out] lines the tooltip's block
/// @param text the line
/// @param inner the text width, in points
void append_fitted(
    const Building& building, std::vector<std::string>& lines, std::string_view text, int32_t inner
) {
    const auto fits = [&](std::string_view start) {
        return width_of(building, kit::FontRole::small, start) <= inner;
    };
    while (!text.empty()) {
        const auto row = oa::base::text::first_row(text, fits);
        lines.emplace_back(text.substr(0, row.bytes));
        text.remove_prefix(row.next);
    }
}

/// Returns the key icon whose tooltip shows: the one the keys or the pad
/// focus, or the one under the pointer.
///
/// @param state the screen's state
/// @return the icon's control; none when neither rests on a key icon
kit::ControlId key_tip_id(const ScreenState& state) noexcept {
    const kit::Interaction& interaction = state.interaction;
    if (interaction.focus_shown && controls::registry_key_control(interaction.focused))
        return interaction.focused;
    if (controls::registry_key_control(interaction.hovered))
        return interaction.hovered;
    return kit::no_control;
}

/// Appends the key tooltip over the window when a key icon is hovered or
/// focused and still in view. The card names each signing key's fingerprint
/// and its full public key, and whether the registry is built in or was
/// added by the player. It is drawn last and is not clipped to the list.
///
/// @param[in,out] building the layout
/// @param window the window
void add_key_tooltip(Building& building, const Window& window) {
    const kit::ControlId tip = key_tip_id(*building.state);
    if (tip == kit::no_control)
        return;
    const kit::Control* icon = nullptr;
    for (const kit::Control& control : building.detail)
        if (control.id == tip)
            icon = &control;
    if (icon == nullptr)
        return;
    const kit::Rect seen = icon->clip.width > 0 && icon->clip.height > 0
                               ? kit::intersect(icon->rect, icon->clip)
                               : icon->rect;
    if (seen.width <= 0 || seen.height <= 0)
        return;
    const std::size_t index = controls::registry_index(tip);
    const std::vector<Registry>& registries = building.library->inputs.registries;
    if (index >= registries.size())
        return;
    const Registry& registry = registries[index];
    const TextHooks& text = building.library->text;
    const int32_t padding = kit::compact_metrics.hover_card_padding;
    const int32_t inner = std::max(window.width - 2 * padding, int32_t{1});
    kit::HoverCardLook look;
    look.title = std::string(
        shown(text, registry.keys.size() > 1 ? words::registry_key_many : words::registry_key_one)
    );
    look.aside = std::string(
        shown(text, registry.built_in ? words::registry_key_built_in : words::registry_key_added)
    );
    if (registry.keys.empty()) {
        append_fitted(building, look.block, shown(text, words::registry_no_key), inner);
    } else {
        for (const SigningKey& signing_key : registry.keys) {
            append_fitted(building, look.block, signing_key.fingerprint, inner);
            append_fitted(building, look.block, signing_key.public_key, inner);
        }
    }
    kit::Point size = kit::hover_card_size(*building.fonts, look);
    // The card's header is sized with the wider of the font and the estimate,
    // so "Signing keys" beside "added by you" is not cut at Compact.
    const int32_t card_padding = kit::compact_metrics.hover_card_padding;
    int32_t header_width = 0;
    const auto add_header = [&](std::string_view text) {
        if (text.empty())
            return;
        const int32_t part = std::max(
            kit::text_width(*building.fonts, kit::FontRole::small, text), kit::estimated_width(text)
        );
        header_width += (header_width > 0 ? card_padding : 0) + part;
    };
    add_header(look.title);
    add_header(look.aside);
    const int32_t header_card = header_width + 2 * card_padding;
    if (header_card > size.x)
        size.x = std::min(header_card, window.width);
    const kit::Rect frame{0, 0, window.width, window.height};
    const kit::Rect card = kit::hover_card_rect(seen, frame, size);
    const int32_t anchor_mid = seen.x + seen.width / 2;
    look.arrow_at = std::clamp(anchor_mid - card.x, int32_t{0}, std::max(card.width, int32_t{0}));
    look.arrow = card.y >= seen.y + seen.height ? kit::Side::top : kit::Side::bottom;
    kit::add_hover_card(building.list, card, look);
}

/// Appends UNLOAD MOD's hint when the pointer or the keys rest on it.
/// The card is playing_note for the mod being played: the same words
/// SWITCH OFF shows under that mod. The Active tab selects nothing, so
/// the selected entry's note is empty here.
///
/// @param[in,out] building the layout
/// @param window the window
void add_unload_note(Building& building, const Window& window) {
    const kit::ControlId tip = controls::active_unload;
    if (!hovered(building, tip) && !focused(building, tip))
        return;
    const Entry* played = nullptr;
    for (const Entry& candidate : building.library->entries) {
        if (candidate.id.kind == Kind::mod && candidate.installed != nullptr &&
            candidate.installed->playing) {
            played = &candidate;
            break;
        }
    }
    if (played == nullptr)
        return;
    const std::string note = playing_note(*building.library, *played);
    if (note.empty())
        return;
    const kit::Control* button = nullptr;
    for (const kit::Control& control : building.detail)
        if (control.id == tip)
            button = &control;
    if (button == nullptr)
        return;
    const kit::Rect seen = button->clip.width > 0 && button->clip.height > 0
                               ? kit::intersect(button->rect, button->clip)
                               : button->rect;
    if (seen.width <= 0 || seen.height <= 0)
        return;
    const int32_t padding = kit::compact_metrics.hover_card_padding;
    const int32_t inner = std::max(window.width - 2 * padding, int32_t{1});
    kit::HoverCardLook look;
    append_fitted(building, look.block, note, inner);
    const kit::Point size = kit::hover_card_size(*building.fonts, look);
    const kit::Rect frame{0, 0, window.width, window.height};
    const kit::Rect card = kit::hover_card_rect(seen, frame, size);
    const int32_t anchor_mid = seen.x + seen.width / 2;
    look.arrow_at = std::clamp(anchor_mid - card.x, int32_t{0}, std::max(card.width, int32_t{0}));
    look.arrow = card.y >= seen.y + seen.height ? kit::Side::top : kit::Side::bottom;
    kit::add_hover_card(building.list, card, look);
}

/// Appends the base game chooser over the Library: a shade, the dialog, one
/// row per base game, and CANCEL and USE THIS GAME. The rows and the buttons
/// are tried before the shade, and the shade before the Library.
///
/// @param[in,out] building the layout
/// @param window the window
void add_base_chooser(Building& building, const Window& window) {
    const Library& library = *building.library;
    const BaseChooser& chooser = library.chooser;
    if (!chooser.open)
        return;
    const kit::Metrics& metrics = *building.metrics;
    const int32_t margin = window.padding;
    const kit::Rect shade{0, 0, window.width, window.height};
    kit::Item& dim = add_item(building.list, kit::Role::blend, shade, {});
    dim.colour = kit::colour::backdrop;
    dim.opacity = 180;

    const int32_t dialog_width = std::max(window.width - 2 * margin, int32_t{1});
    const int32_t inner_width = std::max(dialog_width - 2 * margin, int32_t{1});
    const std::vector<std::string> lead = wrapped(
        building, kit::FontRole::small, shown(library.text, words::base_lead), inner_width, 0
    );
    const std::vector<std::string> note =
        chooser.note.empty()
            ? std::vector<std::string>{}
            : wrapped(building, kit::FontRole::small, chooser.note, inner_width, 0);
    const std::vector<std::string> problem =
        chooser.problem.empty()
            ? std::vector<std::string>{}
            : wrapped(building, kit::FontRole::small, chooser.problem, inner_width, 0);
    const int32_t title_band = metrics.button_height;
    const int32_t lead_height = static_cast<int32_t>(lead.size()) * metrics.small_line;
    const int32_t note_height =
        note.empty() ? 0 : static_cast<int32_t>(note.size()) * metrics.small_line + margin;
    const int32_t problem_height =
        problem.empty() ? 0
                        : static_cast<int32_t>(problem.size()) * metrics.small_line + window.gap;
    const int32_t row_gap = window.gap;
    int32_t rows_height = 0;
    std::vector<int32_t> row_heights;
    row_heights.reserve(chooser.rows.size());
    for (const BaseGameRow& row : chooser.rows) {
        int32_t height = metrics.regular_line + metrics.small_line + window.gap;
        if (!row.detail.empty())
            height += metrics.small_line;
        if (!row.expansions.empty())
            height += kit::pill_height() + window.gap;
        height = std::max(height, int32_t{52});
        row_heights.push_back(height);
        rows_height += height;
    }
    if (chooser.rows.size() > 1)
        rows_height += static_cast<int32_t>(chooser.rows.size() - 1) * row_gap;
    const int32_t footer_band = metrics.button_height + margin;
    int32_t dialog_height = title_band + margin + lead_height + row_gap + rows_height +
                            note_height + problem_height + footer_band + margin;
    const int32_t most = std::max(window.height - 2 * margin, int32_t{1});
    if (dialog_height > most)
        dialog_height = most;
    const int32_t dialog_x = margin;
    const int32_t dialog_y = std::max((window.height - dialog_height) / 2, int32_t{0});
    const kit::Rect dialog{dialog_x, dialog_y, dialog_width, dialog_height};
    add_item(building.list, kit::Role::fill, dialog, {}).colour = kit::colour::panel;
    add_item(building.list, kit::Role::outline, dialog, {}).colour = kit::colour::control_border;
    add_text(
        building,
        kit::FontRole::regular,
        std::string(shown(library.text, words::base_title)),
        {dialog_x + margin, dialog_y, inner_width, title_band},
        kit::Align::left,
        kit::colour::text,
        {}
    );
    int32_t cursor = dialog_y + title_band;
    add_item(building.list, kit::Role::rule, {dialog_x, cursor, dialog_width, 1}, {}).colour =
        kit::colour::control_border;
    cursor += margin / 2;
    for (const std::string& line : lead) {
        add_text(
            building,
            kit::FontRole::small,
            line,
            {dialog_x + margin, cursor, inner_width, metrics.small_line},
            kit::Align::left,
            kit::colour::hint,
            {}
        );
        cursor += metrics.small_line;
    }
    cursor += row_gap;
    const int32_t badge = 36;
    for (std::size_t index = 0; index < chooser.rows.size() && index < row_heights.size();
         ++index) {
        const BaseGameRow& row = chooser.rows[index];
        const int32_t height = row_heights[index];
        const kit::Rect rect{dialog_x + margin, cursor, inner_width, height};
        if (index < controls::most_base_rows) {
            const kit::ControlId id = controls::first_base_row + static_cast<kit::ControlId>(index);
            kit::ButtonLook look;
            look.style = kit::ButtonStyle::quiet;
            look.hovered = hovered(building, id);
            look.held = held(building, id);
            const std::size_t before = building.list.controls.size();
            kit::add_button(
                building.list, rect, look, id, unique_name(building, named("base." + row.id))
            );
            move_controls(building.list, before, building.overlay);
            kit::Control& control = building.overlay.back();
            control.focusable = true;
            control.enabled = true;
            building.chooser_order.push_back(control.id);
        }
        if (row.marked)
            add_item(building.list, kit::Role::outline, rect, {}).colour = kit::colour::accent;
        else
            add_item(building.list, kit::Role::outline, rect, {}).colour =
                kit::colour::control_border;
        const kit::Rect radio{rect.x + 8, rect.y + 10, 12, 12};
        add_item(building.list, kit::Role::outline, radio, {}).colour =
            row.marked ? kit::colour::accent : kit::colour::switch_idle;
        if (row.marked) {
            const kit::Rect dot{radio.x + 3, radio.y + 3, 6, 6};
            add_item(building.list, kit::Role::fill, dot, {}).colour = kit::colour::accent;
        }
        const kit::Rect badge_rect{radio.x + radio.width + 8, rect.y + 8, badge, badge};
        oa::ui::frontend_renderer::RgbaPicture picture = picture_of(row.badge);
        if (base_picture(*building.library, picture))
            picture = fitted_base_picture(building, picture, badge_rect);
        if (oa::ui::frontend_renderer::picture_drawable(picture)) {
            add_item(building.list, kit::Role::picture, badge_rect, {}).look =
                kit::PictureLook{picture};
            add_item(building.list, kit::Role::outline, badge_rect, {}).colour =
                kit::colour::control_border;
        } else {
            add_item(building.list, kit::Role::fill, badge_rect, {}).colour =
                kit::colour::off_selected;
            add_text(
                building,
                kit::FontRole::small,
                row.letters,
                badge_rect,
                kit::Align::centre,
                kit::colour::text,
                {}
            );
        }
        const int32_t text_x = badge_rect.x + badge + 8;
        const int32_t text_w = std::max(rect.x + rect.width - text_x - 8, int32_t{1});
        add_text(
            building,
            kit::FontRole::regular,
            row.name,
            {text_x, rect.y + 6, text_w, metrics.regular_line},
            kit::Align::left,
            kit::colour::text,
            {}
        );
        if (!row.version.empty()) {
            const int32_t version_w =
                std::min(width_of(building, kit::FontRole::small, row.version) + 8, text_w / 3);
            add_text(
                building,
                kit::FontRole::small,
                row.version,
                {text_x + width_of(building, kit::FontRole::regular, row.name) + 8,
                 rect.y + 8,
                 std::max(version_w, int32_t{1}),
                 metrics.small_line},
                kit::Align::left,
                kit::colour::hint,
                {}
            );
        }
        kit::PillLook state_pill;
        state_pill.text = row.state_text;
        state_pill.state = row.state == BaseGameState::not_added ? kit::PillState::update
                                                                 : kit::PillState::neutral;
        const int32_t state_w = std::min(kit::pill_width(*building.fonts, state_pill), text_w);
        kit::add_pill(
            building.list,
            {rect.x + rect.width - state_w - 8, rect.y + 6, state_w, kit::pill_height()},
            state_pill,
            kit::no_control,
            {}
        );
        int32_t line_y = rect.y + 6 + metrics.regular_line;
        if (!row.detail.empty()) {
            add_text(
                building,
                kit::FontRole::small,
                row.detail,
                {text_x, line_y, text_w, metrics.small_line},
                kit::Align::left,
                kit::colour::hint,
                {}
            );
            line_y += metrics.small_line;
        }
        int32_t pill_x = text_x;
        for (const std::string& expansion : row.expansions) {
            kit::PillLook expansion_pill;
            expansion_pill.text = expansion;
            expansion_pill.state = kit::PillState::active;
            const int32_t width =
                std::min(kit::pill_width(*building.fonts, expansion_pill), text_w);
            if (pill_x + width > text_x + text_w)
                break;
            kit::add_pill(
                building.list,
                {pill_x, line_y, width, kit::pill_height()},
                expansion_pill,
                kit::no_control,
                {}
            );
            pill_x += width + metrics.button_gap;
        }
        cursor += height + row_gap;
    }
    if (!note.empty()) {
        for (const std::string& line : note) {
            add_text(
                building,
                kit::FontRole::small,
                line,
                {dialog_x + margin, cursor, inner_width, metrics.small_line},
                kit::Align::left,
                kit::colour::button_text,
                {}
            );
            cursor += metrics.small_line;
        }
        cursor += window.gap;
    }
    for (const std::string& line : problem) {
        add_text(
            building,
            kit::FontRole::small,
            line,
            {dialog_x + margin, cursor, inner_width, metrics.small_line},
            kit::Align::left,
            kit::colour::lock,
            {}
        );
        cursor += metrics.small_line;
    }
    const int32_t button_y = dialog_y + dialog_height - margin / 2 - metrics.button_height;
    kit::ButtonLook use_look;
    use_look.caption = std::string(shown(library.text, words::base_use));
    use_look.style = kit::ButtonStyle::accent;
    use_look.hovered = hovered(building, controls::base_use);
    use_look.held = held(building, controls::base_use);
    const int32_t use_width = std::max(
        width_of(building, kit::FontRole::regular, use_look.caption) + 2 * margin, int32_t{120}
    );
    kit::ButtonLook cancel_look;
    cancel_look.caption = std::string(shown(library.text, words::action_cancel));
    cancel_look.style = kit::ButtonStyle::plain;
    cancel_look.hovered = hovered(building, controls::base_cancel);
    cancel_look.held = held(building, controls::base_cancel);
    const int32_t cancel_width = std::max(
        width_of(building, kit::FontRole::regular, cancel_look.caption) + 2 * margin, int32_t{80}
    );
    const int32_t use_x = dialog_x + dialog_width - margin - use_width;
    const int32_t cancel_x = use_x - window.gap - cancel_width;
    const auto take_button = [&](const kit::Rect& rect,
                                 const kit::ButtonLook& look,
                                 kit::ControlId id,
                                 std::string name) {
        const std::size_t before = building.list.controls.size();
        kit::add_button(building.list, rect, look, id, unique_name(building, named(name)));
        move_controls(building.list, before, building.overlay);
        kit::Control& control = building.overlay.back();
        control.focusable = true;
        control.enabled = true;
        building.chooser_order.push_back(control.id);
    };
    take_button(
        {cancel_x, button_y, cancel_width, metrics.button_height},
        cancel_look,
        controls::base_cancel,
        "base.cancel"
    );
    take_button(
        {use_x, button_y, use_width, metrics.button_height},
        use_look,
        controls::base_use,
        "base.use"
    );
    const std::size_t before = building.list.controls.size();
    kit::ButtonLook shade_look;
    shade_look.style = kit::ButtonStyle::quiet;
    kit::add_button(building.list, shade, shade_look, controls::base_blocker, named("base.block"));
    move_controls(building.list, before, building.overlay);
    building.overlay.back().enabled = true;
    building.overlay.back().focusable = false;
}

/// Returns a key as USE's question draws it: its keycaps and its gesture, or
/// the words for no key.
///
/// @param library the model
/// @param chord the key; no keycaps is no key
/// @return the chip
kit::ChordChipLook move_chip(const Library& library, const KeyChord& chord) {
    kit::ChordChipLook look;
    look.keys = chord.keys;
    look.gesture = chord.gesture;
    if (chord.keys.empty())
        look.none_text = std::string(shown(library.text, words::use_no_key));
    return look;
}

/// Returns a key as automation reads it: its keycaps joined with "+", then
/// its gesture, or the words for no key.
///
/// @param library the model
/// @param chord the key
/// @return the words
std::string chord_reading(const Library& library, const KeyChord& chord) {
    if (chord.keys.empty())
        return std::string(shown(library.text, words::use_no_key));
    std::string read;
    for (const std::string& cap : chord.keys) {
        if (!read.empty())
            read.push_back('+');
        read += cap;
    }
    if (!chord.gesture.empty()) {
        read.push_back(' ');
        read += chord.gesture;
    }
    return read;
}

/// What USE's question shows, and the rows each part takes, once it has
/// been fitted to the window.
struct KeyUsePlan {
    std::vector<std::string> saved{};   ///< where the own changes stay, wrapped
    std::vector<std::string> helpers{}; ///< the helpers' note, wrapped
    std::vector<std::string> pointer{}; ///< the interface note, wrapped
    bool show_moves{};                  ///< What moves is shown
    int32_t move_lines{};               ///< the lines it takes, "and {count} more" among them
    int32_t move_columns{1};            ///< its columns: two at Compact, else one
};

/// The least width of a column of What moves at Compact: a command's name,
/// a key on either side of the arrow, and the gaps between them.
constexpr int32_t least_move_column = 200;

/// Appends USE's question over the Library, scope first: a shade, the
/// title, Where (every game, or only the mod being played), the player's own
/// changes (keep them on top, or start clean, and where they stay either
/// way), What moves (each command's key now and afterwards), the helpers the
/// layout asks for, and Cancel and USE, whose caption names the answer to
/// Where. It is a dialog the width of a notice, the whole window at Compact,
/// as tall as it needs. Where the window is too short, What moves keeps
/// fewer lines and then the notes keep one line each. The choices and the
/// buttons are tried before the shade, and the shade before the Library.
///
/// @param[in,out] building the layout
/// @param window the window
void add_key_use(Building& building, const Window& window) {
    const Library& library = *building.library;
    const KeyUse& use = library.key_use;
    if (!use.open)
        return;
    const kit::Metrics& metrics = *building.metrics;
    const TextHooks& text = library.text;
    const bool compact = building.size_class == kit::SizeClass::compact;
    const kit::Rect shade{0, 0, window.width, window.height};
    kit::Item& dim = add_item(building.list, kit::Role::blend, shade, {});
    dim.colour = kit::colour::backdrop;
    dim.opacity = 180;

    // Kit 2's sizes: the controls' height, the window padding inside the
    // dialog, a space between a heading and what it heads, and a row's gap
    // between sections. Compact keeps half the padding round the dialog.
    const int32_t outer = compact ? metrics.space : window.padding;
    const int32_t pad = compact ? 2 * metrics.space : metrics.window_padding;
    const int32_t gap = metrics.space;
    const int32_t section_gap = metrics.row_gap;
    const int32_t control = metrics.control_height;
    const int32_t switch_row = std::min(metrics.switch_height, control);
    const int32_t move_line = kit::keycap_height + 2;
    const int32_t room_width = std::max(window.width - 2 * outer, int32_t{1});
    const int32_t dialog_width = compact ? room_width : std::min(metrics.notice_width, room_width);
    const int32_t inner = std::max(dialog_width - 2 * pad, int32_t{1});
    const int32_t most_height = std::max(window.height - 2 * outer, int32_t{1});

    const Entry* entry = find_entry(library, use.layout);
    const std::string name = entry != nullptr ? std::string(entry_name(*entry)) : std::string{};
    const bool show_own = use.show_own || use.already;
    const bool choose_own = use.show_own && !use.already;
    const std::size_t helper_count = std::min(use.helpers.size(), controls::most_use_helpers);
    const std::vector<KeyMove>& moves = key_use_moves(use);
    const uint32_t moves_total =
        std::max(key_use_moves_total(use), static_cast<uint32_t>(moves.size()));

    // The notes, at the widths they are drawn.
    std::string saved_words;
    if (use.already) {
        saved_words =
            use.already_count == 1
                ? filled(text, words::use_already_one, {{"name", use.already_name}})
                : filled(
                      text,
                      words::use_already_many,
                      {{"name", use.already_name}, {"count", std::to_string(use.already_count)}}
                  );
        if (use.show_own && !use.saved_with.empty())
            saved_words +=
                " " + filled(
                          text,
                          use.own_count == 1 ? words::use_saved_one : words::use_saved_many,
                          {{"name", use.saved_with}}
                      );
    } else if (use.show_own && !use.saved_with.empty()) {
        saved_words = filled(
            text,
            use.own_count == 1 ? words::use_saved_one : words::use_saved_many,
            {{"name", use.saved_with}}
        );
    }
    const std::string helpers_words =
        use.show_scope && use.for_mod
            ? filled(text, words::use_helpers_not_only, {{"mod", use.mod_name}})
            : std::string(shown(text, words::use_helpers_every_game));
    const int32_t callout_inner = std::max(inner - 2 * gap, int32_t{1});

    KeyUsePlan plan;
    plan.saved = wrapped(building, kit::FontRole::small, saved_words, inner, 0);
    if (helper_count > 0)
        plan.helpers = wrapped(building, kit::FontRole::small, helpers_words, callout_inner, 0);
    plan.pointer = wrapped(building, kit::FontRole::small, use.pointer_note, inner, 0);
    plan.show_moves = use.moves.known;
    const auto lines_height = [&](const std::vector<std::string>& lines) {
        return hint_lines_height(building, lines.size());
    };
    const auto fixed_height = [&] {
        int32_t height = pad + metrics.regular_line + section_gap;
        if (use.show_scope)
            height += metrics.small_line + gap + control + section_gap;
        if (show_own) {
            height += metrics.small_line + gap;
            if (choose_own)
                height += control + gap;
            height += lines_height(plan.saved) + section_gap;
        }
        if (plan.show_moves)
            height += metrics.small_line + gap + section_gap;
        if (helper_count > 0)
            height += gap + lines_height(plan.helpers) + gap +
                      static_cast<int32_t>(helper_count) * switch_row + gap + section_gap;
        if (!plan.pointer.empty())
            height += lines_height(plan.pointer) + section_gap;
        return height + control + pad;
    };
    // What moves takes the rows left over: every move when they fit, else
    // as many as fit and "and {count} more", and at least one line. At
    // Compact it fills two columns, row by row, where the dialog is wide
    // enough.
    if (compact && inner - gap >= 2 * least_move_column)
        plan.move_columns = 2;
    const int32_t wanted_cells =
        moves.empty() ? 1
                      : static_cast<int32_t>(moves.size()) +
                            (moves_total > static_cast<uint32_t>(moves.size()) ? 1 : 0);
    const int32_t wanted_lines = (wanted_cells + plan.move_columns - 1) / plan.move_columns;
    const auto fit_moves = [&] {
        const int32_t room = most_height - fixed_height();
        plan.move_lines = std::clamp(room / move_line, int32_t{1}, wanted_lines);
        return room >= plan.move_lines * move_line;
    };
    if (plan.show_moves && !fit_moves()) {
        // Too short: the notes keep one line each, then What moves goes.
        if (plan.saved.size() > 1)
            plan.saved = {cut(building, kit::FontRole::small, saved_words, inner)};
        if (plan.helpers.size() > 1)
            plan.helpers = {cut(building, kit::FontRole::small, helpers_words, callout_inner)};
        if (!fit_moves())
            plan.show_moves = false;
    }
    if (!plan.show_moves)
        plan.move_lines = 0;
    const int32_t dialog_height =
        std::min(fixed_height() + plan.move_lines * move_line, most_height);
    const int32_t dialog_x = (window.width - dialog_width) / 2;
    const int32_t dialog_y = std::max((window.height - dialog_height) / 2, int32_t{0});
    const kit::Rect dialog{dialog_x, dialog_y, dialog_width, dialog_height};
    add_item(building.list, kit::Role::fill, dialog, {}).colour = kit::colour::panel;
    add_item(building.list, kit::Role::outline, dialog, {}).colour = kit::colour::control_border;

    const int32_t left = dialog_x + pad;
    int32_t cursor = dialog_y + pad;
    int32_t number = 0;
    const auto heading = [&](std::string_view words_shown) {
        ++number;
        add_text(
            building,
            kit::FontRole::small,
            cut(building,
                kit::FontRole::small,
                filled(
                    text,
                    words::use_section,
                    {{"number", std::to_string(number)}, {"heading", std::string(words_shown)}}
                ),
                inner),
            {left, cursor, inner, metrics.small_line},
            kit::Align::left,
            kit::colour::hint,
            {}
        );
        cursor += metrics.small_line + gap;
    };
    const auto note = [&](const std::vector<std::string>& lines,
                          kit::ControlId id,
                          std::string control_name,
                          std::string_view read,
                          int32_t at,
                          int32_t width,
                          kit::Colour ink) {
        const int32_t top = cursor;
        cursor = add_lines(building, lines, at, cursor, width, ink, {});
        if (id != kit::no_control && !lines.empty())
            add_label(
                building.overlay,
                id,
                {at, top, width, cursor - top},
                unique_name(building, named(std::move(control_name))),
                std::string(read),
                {},
                -1
            );
    };
    // One of a pair of choices: a radio row, the marked one on the list's
    // selected face with the accent's edge and its dot.
    const auto choice = [&](kit::ControlId id,
                            std::string control_name,
                            const kit::Rect& rect,
                            std::string caption,
                            bool marked) {
        kit::RadioLook look;
        look.caption = cut(
            building, kit::FontRole::regular, caption, kit::radio_parts(rect, true).caption.width
        );
        look.on = marked;
        look.hovered = hovered(building, id);
        look.held = held(building, id);
        const std::size_t before = building.list.controls.size();
        kit::add_radio(
            building.list, rect, look, id, unique_name(building, named(std::move(control_name)))
        );
        move_controls(building.list, before, building.overlay);
        kit::Control& added = building.overlay.back();
        added.focusable = true;
        added.enabled = true;
        added.text = caption;
        building.question_order.push_back(added.id);
    };
    const int32_t half = std::max((inner - gap) / 2, int32_t{1});

    add_text(
        building,
        kit::FontRole::regular,
        cut(building,
            kit::FontRole::regular,
            filled(text, words::use_title, {{"name", name}}),
            inner),
        {left, cursor, inner, metrics.regular_line},
        kit::Align::left,
        kit::colour::text,
        {}
    );
    cursor += metrics.regular_line + section_gap;

    if (use.show_scope) {
        heading(shown(text, words::use_where));
        choice(
            controls::use_for_game,
            "use.for.game",
            {left, cursor, half, control},
            std::string(shown(text, words::use_every_game)),
            !use.for_mod
        );
        choice(
            controls::use_for_mod,
            "use.for.mod",
            {left + half + gap, cursor, inner - half - gap, control},
            filled(text, words::use_only_with, {{"mod", use.mod_name}}),
            use.for_mod
        );
        cursor += control + section_gap;
    }

    if (show_own) {
        if (use.show_own)
            heading(
                use.own_count == 1
                    ? filled(text, words::use_yours_one, {{"name", use.saved_with}})
                    : filled(
                          text,
                          words::use_yours_many,
                          {{"count", std::to_string(use.own_count)}, {"name", use.saved_with}}
                      )
            );
        else
            heading(shown(text, words::fact_your_changes));
        if (choose_own) {
            choice(
                controls::use_keep,
                "use.keep",
                {left, cursor, half, control},
                std::string(
                    shown(text, use.own_count == 1 ? words::use_keep_it : words::use_keep_them)
                ),
                use.keep
            );
            choice(
                controls::use_aside,
                "use.aside",
                {left + half + gap, cursor, inner - half - gap, control},
                std::string(shown(text, words::use_start_clean)),
                !use.keep
            );
            cursor += control + gap;
        }
        note(
            plan.saved,
            controls::use_saved,
            "use.saved",
            saved_words,
            left,
            inner,
            kit::colour::hint
        );
        cursor += section_gap;
    }

    if (plan.show_moves) {
        heading(shown(text, words::use_what_moves));
        const int32_t top = cursor;
        std::string read;
        const int32_t cells = plan.move_lines * plan.move_columns;
        const bool more = moves_total > static_cast<uint32_t>(moves.size()) ||
                          cells < static_cast<int32_t>(moves.size());
        const int32_t shown_moves =
            moves.empty() ? 0
                          : std::min(more ? cells - 1 : cells, static_cast<int32_t>(moves.size()));
        const int32_t column_width =
            plan.move_columns > 1 ? (inner - gap) / plan.move_columns : inner;
        const std::string_view arrow = "→";
        const int32_t arrow_width = width_of(building, kit::FontRole::small, arrow);
        if (moves.empty()) {
            const std::string none(shown(text, words::use_nothing_moves));
            add_text(
                building,
                kit::FontRole::small,
                cut(building, kit::FontRole::small, none, inner),
                {left, cursor, inner, move_line},
                kit::Align::left,
                kit::colour::hint,
                {}
            );
            read = none;
            cursor += move_line;
        }
        // A cell's place: row by row, the columns from the left.
        const auto cell_left = [&](int32_t index) {
            return left + (index % plan.move_columns) * (column_width + gap);
        };
        const auto cell_top = [&](int32_t index) {
            return top + (index / plan.move_columns) * move_line;
        };
        for (int32_t index = 0; index < shown_moves; ++index) {
            const KeyMove& move = moves[static_cast<std::size_t>(index)];
            const kit::ChordChipLook before = move_chip(library, move.before);
            const kit::ChordChipLook after = move_chip(library, move.after);
            const int32_t after_width = kit::chord_chip_width(*building.fonts, after);
            const int32_t before_width = kit::chord_chip_width(*building.fonts, before);
            const int32_t cell_x = cell_left(index);
            cursor = cell_top(index);
            const int32_t chip_top = cursor + (move_line - kit::keycap_height) / 2;
            const int32_t after_x = cell_x + column_width - after_width;
            const int32_t arrow_x = after_x - gap - arrow_width;
            const int32_t before_x = arrow_x - gap - before_width;
            kit::add_chord_chip(
                building.list,
                {after_x, chip_top, after_width, kit::keycap_height},
                after,
                kit::no_control,
                {}
            );
            add_text(
                building,
                kit::FontRole::small,
                std::string(arrow),
                {arrow_x, cursor, arrow_width, move_line},
                kit::Align::left,
                kit::colour::hint,
                {}
            );
            kit::add_chord_chip(
                building.list,
                {before_x, chip_top, before_width, kit::keycap_height},
                before,
                kit::no_control,
                {}
            );
            int32_t name_room = std::max(before_x - gap - cell_x, int32_t{0});
            const std::string command(shown(text, move.command));
            if (move.own) {
                kit::PillLook yours;
                yours.text = std::string(shown(text, words::use_yours));
                yours.state = kit::PillState::update;
                const int32_t yours_width = kit::pill_width(*building.fonts, yours);
                name_room = std::max(name_room - yours_width - gap, int32_t{0});
                const std::string drawn = cut(building, kit::FontRole::small, command, name_room);
                const int32_t drawn_width = width_of(building, kit::FontRole::small, drawn);
                add_text(
                    building,
                    kit::FontRole::small,
                    drawn,
                    {cell_x, cursor, drawn_width, move_line},
                    kit::Align::left,
                    kit::colour::text,
                    {}
                );
                kit::add_pill(
                    building.list,
                    {cell_x + drawn_width + gap,
                     cursor + (move_line - kit::pill_height()) / 2,
                     yours_width,
                     kit::pill_height()},
                    yours,
                    kit::no_control,
                    {}
                );
            } else {
                add_text(
                    building,
                    kit::FontRole::small,
                    cut(building, kit::FontRole::small, command, name_room),
                    {cell_x, cursor, name_room, move_line},
                    kit::Align::left,
                    kit::colour::text,
                    {}
                );
            }
            if (!read.empty())
                read += "\n";
            read += command;
            if (move.own)
                read += " (" + std::string(shown(text, words::use_yours)) + ")";
            read += ": " + chord_reading(library, move.before) + " " + std::string(arrow) + " " +
                    chord_reading(library, move.after);
        }
        if (more && !moves.empty()) {
            const uint32_t left_out = moves_total - static_cast<uint32_t>(shown_moves);
            const std::string rest =
                filled(text, words::names_more, {{"count", std::to_string(left_out)}});
            add_text(
                building,
                kit::FontRole::small,
                cut(building, kit::FontRole::small, rest, column_width),
                {cell_left(shown_moves), cell_top(shown_moves), column_width, move_line},
                kit::Align::left,
                kit::colour::hint,
                {}
            );
            read += "\n" + rest;
        }
        if (!moves.empty())
            cursor = top + plan.move_lines * move_line;
        add_label(
            building.overlay,
            controls::use_moves,
            {left, top, inner, cursor - top},
            unique_name(building, named("use.moves")),
            read,
            {},
            -1
        );
        cursor += section_gap;
    }

    if (helper_count > 0) {
        // The helpers are settings of the player's own, so the note says they
        // act in every game, inside a box in the lock colour's tint.
        const int32_t box_top = cursor;
        const int32_t box_height = gap + lines_height(plan.helpers) + gap +
                                   static_cast<int32_t>(helper_count) * switch_row + gap;
        add_item(building.list, kit::Role::fill, {left, box_top, inner, box_height}, {}).colour =
            kit::colour::caution_tint;
        cursor += gap;
        note(
            plan.helpers,
            controls::use_helpers_note,
            "use.helpers",
            helpers_words,
            left + gap,
            callout_inner,
            kit::colour::lock
        );
        cursor += gap;
        for (std::size_t index = 0; index < helper_count; ++index) {
            const KeyHelperSwitch& one = use.helpers[index];
            const kit::ControlId id =
                controls::first_use_helper + static_cast<kit::ControlId>(index);
            const kit::Rect rect{left + gap, cursor, metrics.switch_width, switch_row};
            kit::SwitchLook look;
            look.on = one.on;
            look.hovered = hovered(building, id);
            look.off_caption = std::string(shown(text, words::registry_off));
            look.on_caption = std::string(shown(text, words::registry_on));
            const std::size_t before = building.list.controls.size();
            kit::add_switch(
                building.list,
                rect,
                look,
                id,
                unique_name(building, named("use.helper." + one.name))
            );
            move_controls(building.list, before, building.overlay);
            kit::Control& added = building.overlay.back();
            added.focusable = true;
            added.enabled = true;
            added.text = one.label;
            building.question_order.push_back(added.id);
            const int32_t label_left = rect.x + rect.width + metrics.list_row_gap;
            add_text(
                building,
                kit::FontRole::small,
                cut(building, kit::FontRole::small, one.label, left + inner - gap - label_left),
                {label_left,
                 rect.y,
                 std::max(left + inner - gap - label_left, int32_t{0}),
                 rect.height},
                kit::Align::left,
                kit::colour::text,
                {}
            );
            cursor += switch_row;
        }
        cursor = box_top + box_height + section_gap;
    }
    if (!plan.pointer.empty()) {
        cursor = add_lines(building, plan.pointer, left, cursor, inner, kit::colour::lock, {});
        cursor += section_gap;
    }

    // Cancel, and USE named by the answer to Where, at the dialog's foot.
    const int32_t button_y = dialog_y + dialog_height - pad - control;
    const std::string use_caption = key_use_button(library);
    const std::string cancel_caption(shown(text, words::use_cancel));
    const int32_t use_width = std::min(
        std::max(small_button_width(building, use_caption), int32_t{80}), inner / 2 + inner / 4
    );
    const int32_t cancel_width =
        std::max(small_button_width(building, cancel_caption), int32_t{80});
    const int32_t use_x = left + inner - use_width;
    const int32_t cancel_x = use_x - gap - cancel_width;
    const auto take = [&](kit::ControlId id,
                          std::string control_name,
                          const kit::Rect& rect,
                          std::string caption,
                          bool accent) {
        kit::ButtonLook look;
        look.caption = cut(building, kit::FontRole::small, caption, rect.width - 2);
        look.style = accent ? kit::ButtonStyle::accent : kit::ButtonStyle::plain;
        look.hovered = hovered(building, id);
        look.held = held(building, id);
        const std::size_t before = building.list.controls.size();
        kit::add_button(
            building.list, rect, look, id, unique_name(building, std::move(control_name))
        );
        move_controls(building.list, before, building.overlay);
        kit::Control& added = building.overlay.back();
        added.focusable = true;
        added.enabled = true;
        added.text = caption;
        building.question_order.push_back(added.id);
    };
    take(
        controls::use_cancel,
        "prompt.cancel",
        {cancel_x, button_y, cancel_width, control},
        cancel_caption,
        false
    );
    take(
        controls::use_confirm,
        "prompt.use",
        {use_x, button_y, use_width, control},
        use_caption,
        true
    );
    const std::size_t before = building.list.controls.size();
    kit::ButtonLook shade_look;
    shade_look.style = kit::ButtonStyle::quiet;
    kit::add_button(building.list, shade, shade_look, controls::use_blocker, named("use.block"));
    move_controls(building.list, before, building.overlay);
    building.overlay.back().enabled = true;
    building.overlay.back().focusable = false;
}

/// Appends one of Export's buttons and puts it in the dialog's tab order.
///
/// @param[in,out] building the layout
/// @param id the control
/// @param control_name the name after `library.`
/// @param rect where it lies
/// @param caption the words, already looked up
/// @param accent the accent face
/// @param takes a press
void take_export_button(
    Building& building,
    kit::ControlId id,
    std::string control_name,
    const kit::Rect& rect,
    std::string caption,
    bool accent,
    bool takes
) {
    kit::ButtonLook look;
    look.caption =
        cut(building, kit::FontRole::small, caption, std::max(rect.width - 2, int32_t{0}));
    // A button that takes no press is drawn plain, as the Library's other
    // main buttons are, so a disabled Export does not look pressable.
    look.style = accent && takes ? kit::ButtonStyle::accent : kit::ButtonStyle::plain;
    look.hovered = hovered(building, id);
    look.held = held(building, id);
    look.enabled = takes;
    const std::size_t before = building.list.controls.size();
    kit::add_button(
        building.list, rect, look, id, unique_name(building, named(std::move(control_name)))
    );
    move_controls(building.list, before, building.overlay);
    kit::Control& added = building.overlay.back();
    added.focusable = takes;
    added.enabled = takes;
    added.text = std::move(caption);
    if (takes)
        building.export_order.push_back(added.id);
}

/// Appends Export over the Library. A `.oademo` is offered first, with the
/// size each file will be. A `.tad` adds what to include and the warnings
/// that apply. The chooser replaces the form. Where the window is short the
/// descriptions go first, then headings, and warnings that still do not fit
/// are paged as whole sets. A warning taller than the page is shown as the
/// lines that fit, and its label keeps the whole sentence. Every row is
/// counted, the pager included, before the dialog is placed, and nothing in
/// the body meets the buttons.
///
/// @param[in,out] building the layout
/// @param window the window
void add_export(Building& building, const Window& window) {
    const Library& library = *building.library;
    const ExportDialog& dialog = library.export_dialog;
    if (!dialog.open)
        return;
    const kit::Metrics& metrics = *building.metrics;
    const TextHooks& text = library.text;
    const bool compact = building.size_class == kit::SizeClass::compact;
    const kit::Rect shade{0, 0, window.width, window.height};
    kit::Item& dim = add_item(building.list, kit::Role::blend, shade, {});
    dim.colour = kit::colour::backdrop;
    dim.opacity = 180;

    const int32_t outer = compact ? metrics.space : window.padding;
    const int32_t pad = compact ? 2 * metrics.space : metrics.window_padding;
    const int32_t gap = metrics.space;
    const int32_t section_gap = metrics.section_gap;
    const int32_t control = metrics.control_height;
    const int32_t room_width = std::max(window.width - 2 * outer, int32_t{1});
    const int32_t dialog_width = compact ? room_width : std::min(metrics.notice_width, room_width);
    const int32_t inner = std::max(dialog_width - 2 * pad, int32_t{1});
    const int32_t most_height = std::max(window.height - 2 * outer, int32_t{1});
    const int32_t line = metrics.small_line;

    const auto folder_shown = [&]() {
        if (dialog.folder_is_downloads)
            return std::string(shown(text, words::export_downloads));
        if (!dialog.folder_label.empty())
            return dialog.folder_label;
        return dialog.folder;
    };
    const bool several = dialog.files.size() > 1;
    const std::string title =
        several
            ? filled(text, words::export_count, {{"count", std::to_string(dialog.files.size())}})
        : dialog.files.size() == 1 && !dialog.files.front().title.empty()
            ? filled(text, words::export_title, {{"name", dialog.files.front().title}})
            : std::string(shown(text, words::export_confirm));

    // The buttons are pinned to the foot. Everything above them gives way.
    const auto place_dialog = [&](int32_t wanted) {
        const int32_t height = std::min(std::max(wanted, pad + control + pad), most_height);
        const int32_t x = (window.width - dialog_width) / 2;
        const int32_t y = std::max((window.height - height) / 2, int32_t{0});
        const kit::Rect dialog{x, y, dialog_width, height};
        add_item(building.list, kit::Role::fill, dialog, {}).colour = kit::colour::panel;
        add_item(building.list, kit::Role::outline, dialog, {}).colour =
            kit::colour::control_border;
        return dialog;
    };
    const auto note_of = [&](kit::ControlId id, const kit::Rect& rect, std::string read) {
        add_label(
            building.overlay,
            id,
            rect,
            unique_name(building, named("export.note")),
            std::move(read),
            {},
            -1
        );
    };

    if (dialog.browsing) {
        const int32_t rows_wanted = static_cast<int32_t>(std::min(
            dialog.entries.size() - std::min(dialog.browse_first, dialog.entries.size()),
            static_cast<std::size_t>(controls::export_browse_page)
        ));
        const bool more =
            dialog.browse_first + static_cast<std::size_t>(controls::export_browse_page) <
            dialog.entries.size();
        const bool previous = dialog.browse_first > 0;
        const auto browse_height = [&](int32_t rows) {
            int32_t height = pad + metrics.regular_line + gap + control + gap;
            if (rows > 0)
                height += rows * control + (rows - 1) * gap;
            if (more || previous)
                height += gap + control;
            return height + section_gap + control + pad;
        };
        int32_t rows = std::max(rows_wanted, int32_t{0});
        while (rows > 1 && browse_height(rows) > most_height)
            --rows;
        const kit::Rect dialog_rect = place_dialog(browse_height(rows));
        const int32_t left = dialog_rect.x + pad;
        int32_t cursor = dialog_rect.y + pad;
        add_text(
            building,
            kit::FontRole::regular,
            cut(building, kit::FontRole::regular, dialog.browse_folder, inner),
            {left, cursor, inner, metrics.regular_line},
            kit::Align::left,
            kit::colour::text,
            {}
        );
        cursor += metrics.regular_line + gap;
        const std::string up_caption(shown(text, words::export_up));
        const int32_t up_width = std::min(small_button_width(building, up_caption), inner);
        take_export_button(
            building,
            controls::export_browse_up,
            "export.up",
            {left, cursor, up_width, control},
            up_caption,
            false,
            true
        );
        cursor += control + gap;
        const std::size_t first = dialog.browse_first;
        for (int32_t row = 0; row < rows; ++row) {
            const std::size_t place = first + static_cast<std::size_t>(row);
            if (place >= dialog.entries.size() || place >= controls::most_export_rows)
                break;
            const ExportFolder& entry = dialog.entries[place];
            const kit::ControlId id =
                controls::first_export_row + static_cast<kit::ControlId>(place);
            const kit::Rect rect{left, cursor, inner, control};
            kit::ButtonLook look;
            look.style = kit::ButtonStyle::quiet;
            look.caption = cut(building, kit::FontRole::small, entry.name, inner - 2 * gap);
            look.hovered = hovered(building, id);
            look.held = held(building, id);
            look.enabled = true;
            const std::size_t before = building.list.controls.size();
            kit::add_button(
                building.list,
                rect,
                look,
                id,
                unique_name(building, named("export.folder." + name_word(entry.name)))
            );
            move_controls(building.list, before, building.overlay);
            kit::Control& added = building.overlay.back();
            added.focusable = true;
            added.enabled = true;
            added.text = entry.name;
            building.export_order.push_back(added.id);
            cursor += control + gap;
        }
        if (more || previous) {
            const std::string more_caption(shown(text, words::export_more_folders));
            const std::string previous_caption(shown(text, words::export_previous_folders));
            const int32_t more_width =
                std::min(small_button_width(building, more_caption), inner / 2);
            const int32_t previous_width =
                std::min(small_button_width(building, previous_caption), inner / 2);
            if (previous)
                take_export_button(
                    building,
                    controls::export_browse_previous,
                    "export.previous",
                    {left, cursor, previous_width, control},
                    previous_caption,
                    false,
                    true
                );
            if (more)
                take_export_button(
                    building,
                    controls::export_browse_more,
                    "export.more",
                    {left + inner - more_width, cursor, more_width, control},
                    more_caption,
                    false,
                    true
                );
            cursor += control;
        }
        const int32_t button_y = dialog_rect.y + dialog_rect.height - pad - control;
        const std::string use_caption(shown(text, words::export_use_folder));
        const std::string cancel_caption(shown(text, words::action_cancel));
        const int32_t use_width = std::min(small_button_width(building, use_caption), inner / 2);
        const int32_t cancel_width =
            std::min(small_button_width(building, cancel_caption), inner / 2);
        take_export_button(
            building,
            controls::export_cancel,
            "export.cancel",
            {left + inner - use_width - gap - cancel_width, button_y, cancel_width, control},
            cancel_caption,
            false,
            true
        );
        take_export_button(
            building,
            controls::export_browse_use,
            "export.use",
            {left + inner - use_width, button_y, use_width, control},
            use_caption,
            true,
            true
        );
        static_cast<void>(cursor);
    } else if (dialog.exporting || dialog.done || dialog.files.empty()) {
        // One file names a compression only while one runs, and a write while
        // the bytes are copied. Several files show the count.
        const bool moving_one = dialog.exporting && !several && dialog.progress_bytes_total > 0;
        const auto writing_name = [&]() {
            std::string name = dialog.files.front().title;
            const std::string extension =
                dialog.format == ExportFormat::tad ? std::string(".tad") : std::string(".oademo");
            const bool already =
                name.size() >= extension.size() &&
                name.compare(name.size() - extension.size(), extension.size(), extension) == 0;
            if (!already)
                name += extension;
            return name;
        };
        const std::string progress =
            progress_text(dialog.progress_bytes_done, dialog.progress_bytes_total, text);
        const std::string sentence =
            moving_one && dialog.compressing
                ? std::string(shown(text, words::export_compressing)) + " · " + progress
            : moving_one
                ? filled(text, words::export_writing, {{"name", writing_name()}}) + " · " + progress
            : dialog.exporting ? filled(
                                     text,
                                     words::export_progress,
                                     {{"done", std::to_string(dialog.progress_done)},
                                      {"total", std::to_string(dialog.progress_total)}}
                                 )
            : dialog.done      ? filled(text, words::export_saved, {{"folder", folder_shown()}})
            : !dialog.problem.empty() ? dialog.problem
                                      : std::string(shown(text, words::export_none));
        const std::vector<std::string> heading =
            wrapped(building, kit::FontRole::regular, title, inner, 0);
        const std::vector<std::string> lines =
            wrapped(building, kit::FontRole::small, sentence, inner, 0);
        const int32_t heading_height =
            static_cast<int32_t>(std::max(heading.size(), std::size_t{1})) * metrics.regular_line;
        const int32_t bar = moving_one ? gap + metrics.progress_bar_height : 0;
        const int32_t wanted = pad + heading_height + gap +
                               static_cast<int32_t>(std::max(lines.size(), std::size_t{1})) * line +
                               bar + section_gap + control + pad;
        const kit::Rect dialog_rect = place_dialog(wanted);
        const int32_t left = dialog_rect.x + pad;
        int32_t cursor = dialog_rect.y + pad;
        for (const std::string& heading_line : heading) {
            add_text(
                building,
                kit::FontRole::regular,
                heading_line,
                {left, cursor, inner, metrics.regular_line},
                kit::Align::left,
                kit::colour::text,
                {}
            );
            cursor += metrics.regular_line;
        }
        if (heading.empty())
            cursor += metrics.regular_line;
        cursor += gap;
        const int32_t top = cursor;
        for (const std::string& sentence_line : lines) {
            add_text(
                building,
                kit::FontRole::small,
                sentence_line,
                {left, cursor, inner, line},
                kit::Align::left,
                kit::colour::text,
                {}
            );
            cursor += line;
        }
        note_of(controls::export_note, {left, top, inner, std::max(cursor - top, line)}, sentence);
        if (moving_one) {
            cursor += gap;
            const uint64_t whole = std::max(dialog.progress_bytes_total, uint64_t{1});
            const uint64_t taken = std::min(dialog.progress_bytes_done, whole);
            const int32_t filled_bar = static_cast<int32_t>(std::min<uint64_t>(
                taken * static_cast<uint64_t>(progress_whole) / whole,
                static_cast<uint64_t>(progress_whole)
            ));
            kit::add_progress(
                building.list,
                {left, cursor, inner, metrics.progress_bar_height},
                filled_bar,
                progress_whole
            );
        }
        const int32_t button_y = dialog_rect.y + dialog_rect.height - pad - control;
        if (dialog.done) {
            const std::string show_caption(shown(text, words::export_show));
            const std::string close_caption(shown(text, words::action_close));
            const int32_t show_width =
                std::min(small_button_width(building, show_caption), inner / 2);
            const int32_t close_width =
                std::min(small_button_width(building, close_caption), inner / 3);
            take_export_button(
                building,
                controls::export_close,
                "export.close",
                {left + inner - show_width - gap - close_width, button_y, close_width, control},
                close_caption,
                false,
                true
            );
            take_export_button(
                building,
                controls::export_show,
                "export.show-folder",
                {left + inner - show_width, button_y, show_width, control},
                show_caption,
                true,
                true
            );
        } else if (dialog.exporting) {
            const std::string cancel_caption(shown(text, words::action_cancel));
            const int32_t cancel_width =
                std::min(small_button_width(building, cancel_caption), inner / 2);
            take_export_button(
                building,
                controls::export_cancel,
                "export.cancel",
                {left + inner - cancel_width, button_y, cancel_width, control},
                cancel_caption,
                false,
                true
            );
        } else if (!dialog.problem.empty()) {
            const std::string close_caption(shown(text, words::action_close));
            const int32_t close_width =
                std::min(small_button_width(building, close_caption), inner / 2);
            take_export_button(
                building,
                controls::export_close,
                "export.close",
                {left + inner - close_width, button_y, close_width, control},
                close_caption,
                false,
                true
            );
        } else {
            const std::string export_caption(shown(text, words::export_confirm));
            const std::string cancel_caption(shown(text, words::action_cancel));
            const int32_t export_width =
                std::min(small_button_width(building, export_caption), inner / 2);
            const int32_t cancel_width =
                std::min(small_button_width(building, cancel_caption), inner / 3);
            take_export_button(
                building,
                controls::export_cancel,
                "export.cancel",
                {left + inner - export_width - gap - cancel_width, button_y, cancel_width, control},
                cancel_caption,
                false,
                true
            );
            take_export_button(
                building,
                controls::export_confirm,
                "export.export",
                {left + inner - export_width, button_y, export_width, control},
                export_caption,
                true,
                false
            );
        }
    } else {
        struct WrappedNote {
            std::string sentence{};
            std::vector<std::string> lines{};
        };

        std::vector<WrappedNote> warnings;
        if (dialog.format == ExportFormat::tad) {
            std::vector<std::string> ids;
            std::vector<std::string> labels;
            bool unnamed = false;
            bool wide = false;
            bool names_cut = false;
            bool status_cut = false;
            const auto mutator_label = [&](const ExportFile& file, std::size_t at) {
                const std::string& id = file.mutator_ids[at];
                for (const Entry& entry : library.entries) {
                    if (entry.id.kind != Kind::mutator || entry.id.key != id)
                        continue;
                    if (entry.installed != nullptr && !entry.installed->name.empty())
                        return entry.installed->name;
                    if (entry.listing != nullptr && !entry.listing->name.empty())
                        return entry.listing->name;
                }
                if (at < file.mutator_names.size() && !file.mutator_names[at].empty())
                    return file.mutator_names[at];
                return id;
            };
            for (const ExportFile& file : dialog.files) {
                wide = wide || file.wide_records;
                names_cut = names_cut || file.names_cut;
                status_cut = status_cut || file.status_cut;
                if (!file.mutators)
                    continue;
                if (file.mutator_ids.empty())
                    unnamed = true;
                for (std::size_t at = 0; at < file.mutator_ids.size(); ++at) {
                    bool seen = false;
                    for (const std::string& have : ids)
                        seen = seen || have == file.mutator_ids[at];
                    if (seen)
                        continue;
                    ids.push_back(file.mutator_ids[at]);
                    labels.push_back(mutator_label(file, at));
                }
            }
            const auto push_warning = [&](std::string sentence) {
                WrappedNote note;
                note.lines = wrapped(building, kit::FontRole::small, sentence, inner, 0);
                if (note.lines.empty())
                    note.lines.emplace_back();
                note.sentence = std::move(sentence);
                warnings.push_back(std::move(note));
            };
            for (const std::string& label : labels)
                push_warning(filled(text, words::export_mutator, {{"name", label}}));
            if (unnamed)
                push_warning(std::string(shown(text, words::export_mutators)));
            if (wide)
                push_warning(std::string(shown(text, words::export_wide)));
            if (names_cut)
                push_warning(std::string(shown(text, words::export_names)));
            if (status_cut)
                push_warning(std::string(shown(text, words::export_status)));
        }
        const auto measuring_size = [&](bool oademo_side) {
            for (const ExportFile& file : dialog.files) {
                if (file.unreadable)
                    return std::string{};
                if (!file.ready)
                    return std::string(shown(text, words::export_measuring));
            }
            uint64_t total = 0;
            for (const ExportFile& file : dialog.files)
                total += oademo_side
                             ? file.oademo_bytes
                             : (dialog.keep_oa_sectors ? file.tad_bytes : file.tad_without_bytes);
            return size_text(total, text);
        };

        struct FormatChoice {
            std::string full{};
            std::string name{};
            std::vector<std::string> detail{};
        };

        const auto choice_of = [&](std::string name, std::string detail, bool recommended) {
            FormatChoice choice;
            choice.name = std::move(name);
            std::string extra = std::move(detail);
            if (recommended) {
                if (!extra.empty())
                    extra += " · ";
                extra += shown(text, words::export_recommended);
            }
            choice.full = choice.name;
            if (!extra.empty()) {
                choice.full += " · ";
                choice.full += extra;
            }
            const kit::Rect probe{0, 0, inner, control};
            const int32_t room = std::max(kit::radio_parts(probe, true).caption.width, int32_t{0});
            // The fit is the regular face, which is what the layout test measures.
            if (width_of(building, kit::FontRole::regular, choice.full) > room && !extra.empty())
                choice.detail = wrapped(building, kit::FontRole::small, extra, inner, 0);
            return choice;
        };
        const FormatChoice oademo_choice =
            choice_of(std::string(shown(text, words::export_oademo)), measuring_size(true), true);
        const FormatChoice tad_choice =
            choice_of(std::string(shown(text, words::export_tad)), measuring_size(false), false);
        const std::vector<std::string> oademo_about = wrapped(
            building, kit::FontRole::small, shown(text, words::export_oademo_about), inner, 0
        );
        const std::vector<std::string> tad_about =
            wrapped(building, kit::FontRole::small, shown(text, words::export_tad_about), inner, 0);
        const std::vector<std::string> heading =
            wrapped(building, kit::FontRole::regular, title, inner, 0);
        const int32_t heading_height =
            static_cast<int32_t>(std::max(heading.size(), std::size_t{1})) * metrics.regular_line;
        const bool show_bar = !dialog.files.empty() && dialog.progress_bytes_total > 0;
        bool any_unreadable = false;
        for (const ExportFile& file : dialog.files)
            any_unreadable = any_unreadable || file.unreadable;
        const std::string form_note = any_unreadable
                                          ? std::string(shown(text, words::export_unreadable))
                                      : !dialog.problem.empty() ? dialog.problem
                                                                : std::string{};
        const std::vector<std::string> note_lines =
            form_note.empty() ? std::vector<std::string>{}
                              : wrapped(building, kit::FontRole::small, form_note, inner, 0);
        const auto lines_height = [&](const std::vector<std::string>& rows) {
            return static_cast<int32_t>(rows.size()) * line;
        };
        const std::string sectors(shown(text, words::export_oa_sectors));
        const int32_t sectors_room = std::max(inner - metrics.switch_width - gap, int32_t{1});
        const std::vector<std::string> sectors_lines =
            wrapped(building, kit::FontRole::small, sectors, sectors_room, 0);
        const int32_t sectors_height = std::max(control, lines_height(sectors_lines));
        const std::vector<std::string> include_lines =
            wrapped(building, kit::FontRole::small, shown(text, words::export_include), inner, 0);
        const std::vector<std::string> compatibility_lines = wrapped(
            building, kit::FontRole::small, shown(text, words::export_compatibility), inner, 0
        );
        const auto block_height = [&](const std::vector<std::string>& rows) {
            return std::max(lines_height(rows), line);
        };

        // Headings and the gap above the buttons give way, fullest first. Every
        // warning is kept when a stage can hold them. Only a stage that cannot
        // is paged, by whole warnings. When no stage holds even one warning
        // with the pager, the sparsest stage shows the lines of that warning
        // that fit, and the label keeps the whole sentence.
        struct ChromeStage {
            bool about{};
            bool include_heading{};
            bool compatibility_heading{};
            int32_t button_gap{};
        };

        const ChromeStage stages[] = {
            {true, true, true, section_gap},
            {false, true, true, section_gap},
            {false, true, false, section_gap},
            {false, false, false, section_gap},
            {false, false, false, gap},
        };
        static_assert(sizeof(stages) / sizeof(stages[0]) == 5);
        const auto warning_span = [&](std::size_t start, std::size_t count) {
            int32_t height = 0;
            for (std::size_t at = start; at < start + count && at < warnings.size(); ++at)
                height += lines_height(warnings[at].lines);
            return height;
        };
        // `cut_lines` replaces the warnings' own height, for one warning shown
        // as the lines that fit. `keep_where` leaves the folder row out when
        // even those lines need its room.
        const auto chrome = [&](const ChromeStage& stage,
                                std::size_t start,
                                std::size_t count,
                                bool with_page,
                                int32_t cut_lines = 0,
                                bool keep_where = true) {
            int32_t height =
                pad + heading_height + gap + control + lines_height(oademo_choice.detail);
            if (stage.about)
                height += gap + lines_height(oademo_about);
            height += gap + control + lines_height(tad_choice.detail);
            if (stage.about)
                height += gap + lines_height(tad_about);
            if (show_bar)
                height += gap + metrics.progress_bar_height;
            if (dialog.format == ExportFormat::tad) {
                height += section_gap;
                if (stage.include_heading)
                    height += block_height(include_lines) + gap;
                height += sectors_height;
                if (count > 0 || with_page) {
                    height += section_gap;
                    if (stage.compatibility_heading)
                        height += block_height(compatibility_lines) + gap;
                    if (cut_lines > 0)
                        height += cut_lines * line;
                    else
                        height += warning_span(start, count);
                    if (with_page)
                        height += gap + control;
                }
            }
            if (keep_where) {
                height += section_gap + control;
                if (!note_lines.empty())
                    height += gap + lines_height(note_lines);
            }
            return height + stage.button_gap + control + pad;
        };
        bool about = false;
        bool include_heading = false;
        bool compatibility_heading = false;
        int32_t button_gap = gap;
        bool page = false;
        std::size_t warning_start = std::min(dialog.warning_first, warnings.size());
        if (!warnings.empty() && warning_start >= warnings.size())
            warning_start = warnings.size() - 1;
        std::size_t warning_count = 0;
        int32_t partial_lines = 0;
        bool show_where = true;
        bool fitted = false;

        struct WarningFit {
            bool about{};
            bool include_heading{};
            bool compatibility_heading{};
            int32_t button_gap{};
            std::size_t count{};
            int32_t partial_lines{};
            bool page{};
            bool where{true};
        };

        // The fullest stage that holds the most whole warnings. A later,
        // sparser stage replaces it only when that stage holds more. The same
        // choice at every page start, so the pages do not share a warning.
        const auto fit_at = [&](std::size_t page_start) {
            WarningFit fit;
            fit.button_gap = gap;
            if (page_start >= warnings.size())
                return fit;
            bool held = false;
            bool spare = false;
            ChromeStage spare_stage{false, false, false, gap};
            std::size_t spare_count = 0;
            for (const ChromeStage& candidate : stages) {
                const std::size_t rest = warnings.size() - page_start;
                // A page already turned still needs its pager. The first page
                // does not, when every warning fits.
                const bool turned = page_start > 0;
                if (chrome(candidate, page_start, rest, turned) <= most_height) {
                    fit.about = candidate.about;
                    fit.include_heading = candidate.include_heading;
                    fit.compatibility_heading = candidate.compatibility_heading;
                    fit.button_gap = candidate.button_gap;
                    fit.count = rest;
                    fit.page = turned;
                    held = true;
                    break;
                }
                std::size_t held_count = 0;
                for (std::size_t step = 1; step <= rest; ++step) {
                    if (chrome(candidate, page_start, step, true) > most_height)
                        break;
                    held_count = step;
                }
                if (held_count > spare_count) {
                    spare = true;
                    spare_stage = candidate;
                    spare_count = held_count;
                }
            }
            if (!held && spare) {
                fit.about = spare_stage.about;
                fit.include_heading = spare_stage.include_heading;
                fit.compatibility_heading = spare_stage.compatibility_heading;
                fit.button_gap = spare_stage.button_gap;
                fit.count = spare_count;
                fit.page = true;
                return fit;
            }
            if (held)
                return fit;

            // The last stage is the sparsest. The assert above keeps this the last.
            const ChromeStage& sparse = stages[4];
            const bool paging = page_start > 0 || page_start + 1 < warnings.size();
            bool keep_where = true;
            int32_t base = chrome(sparse, page_start, 0, paging, 0, true);
            // A count of zero leaves the warning block out when the page has
            // no pager. The lines are drawn under that block's gap.
            if (!paging)
                base += section_gap;
            int32_t room = most_height - base;
            if (room < line) {
                keep_where = false;
                base = chrome(sparse, page_start, 0, paging, 0, false);
                if (!paging)
                    base += section_gap;
                room = most_height - base;
            }
            int32_t fitted_lines = room > 0 ? room / line : 0;
            if (fitted_lines < 1)
                fitted_lines = 1;
            const int32_t have_lines = static_cast<int32_t>(warnings[page_start].lines.size());
            fit.about = sparse.about;
            fit.include_heading = sparse.include_heading;
            fit.compatibility_heading = sparse.compatibility_heading;
            fit.button_gap = sparse.button_gap;
            fit.count = 1;
            fit.partial_lines = fitted_lines >= have_lines ? 0 : fitted_lines;
            fit.where = keep_where;
            fit.page = paging;
            return fit;
        };

        if (dialog.format != ExportFormat::tad || warnings.empty()) {
            for (const ChromeStage& candidate : stages) {
                if (chrome(candidate, 0, 0, false) > most_height)
                    continue;
                about = candidate.about;
                include_heading = candidate.include_heading;
                compatibility_heading = candidate.compatibility_heading;
                button_gap = candidate.button_gap;
                fitted = true;
                break;
            }
            if (!fitted) {
                about = false;
                include_heading = false;
                compatibility_heading = false;
                button_gap = gap;
            }
            dialog.warning_span = 0;
            dialog.warning_back = 0;
            dialog.warning_more = false;
        } else {
            std::size_t boundary = 0;
            std::size_t earlier = 0;
            const std::size_t requested = warning_start;
            while (boundary < requested && boundary < warnings.size()) {
                const WarningFit step_fit = fit_at(boundary);
                if (step_fit.count == 0)
                    break;
                const std::size_t next_start = boundary + step_fit.count;
                if (next_start > requested)
                    break;
                earlier = boundary;
                boundary = next_start;
            }
            const WarningFit chosen = fit_at(boundary);
            if (boundary != dialog.warning_first) {
                // The layout takes a const Library. A start inside a page is
                // put back on that page's first warning, so the next page
                // does not show a warning again.
                const_cast<ExportDialog&>(dialog).warning_first = boundary;
            }
            warning_start = boundary;
            about = chosen.about;
            include_heading = chosen.include_heading;
            compatibility_heading = chosen.compatibility_heading;
            button_gap = chosen.button_gap;
            warning_count = chosen.count;
            partial_lines = chosen.partial_lines;
            page = chosen.page;
            show_where = chosen.where;
            dialog.warning_span = chosen.count;
            dialog.warning_back = boundary == 0 ? std::size_t{0} : earlier;
            dialog.warning_more = chosen.count > 0 && boundary + chosen.count < warnings.size();
        }
        const kit::Rect dialog_rect = place_dialog(chrome(
            ChromeStage{about, include_heading, compatibility_heading, button_gap},
            warning_start,
            warning_count,
            page,
            partial_lines,
            show_where
        ));
        const int32_t left = dialog_rect.x + pad;
        int32_t cursor = dialog_rect.y + pad;
        // The buttons own the foot. A body row that would meet them is not drawn.
        const int32_t foot_top = dialog_rect.y + dialog_rect.height - pad - control;
        const auto above_foot = [&](int32_t height) { return cursor + height < foot_top; };
        if (heading.empty())
            cursor += metrics.regular_line;
        for (const std::string& heading_line : heading) {
            add_text(
                building,
                kit::FontRole::regular,
                heading_line,
                {left, cursor, inner, metrics.regular_line},
                kit::Align::left,
                kit::colour::text,
                {}
            );
            cursor += metrics.regular_line;
        }
        cursor += gap;

        const auto radio_row = [&](kit::ControlId id,
                                   std::string control_name,
                                   const FormatChoice& choice,
                                   bool marked) {
            const kit::Rect rect{left, cursor, inner, control};
            kit::RadioLook look;
            look.caption = choice.detail.empty() ? choice.full : choice.name;
            look.on = marked;
            look.hovered = hovered(building, id);
            look.held = held(building, id);
            const std::size_t before = building.list.controls.size();
            kit::add_radio(
                building.list, rect, look, id, unique_name(building, named(std::move(control_name)))
            );
            move_controls(building.list, before, building.overlay);
            kit::Control& added = building.overlay.back();
            added.focusable = true;
            added.enabled = true;
            added.text = choice.full;
            building.export_order.push_back(added.id);
            cursor += control;
            for (const std::string& detail_line : choice.detail) {
                add_text(
                    building,
                    kit::FontRole::small,
                    detail_line,
                    {left, cursor, inner, line},
                    kit::Align::left,
                    kit::colour::hint,
                    {}
                );
                cursor += line;
            }
        };
        const auto about_lines = [&](const std::vector<std::string>& rows) {
            if (!about || rows.empty())
                return;
            cursor += gap;
            for (const std::string& row : rows) {
                add_text(
                    building,
                    kit::FontRole::small,
                    row,
                    {left, cursor, inner, line},
                    kit::Align::left,
                    kit::colour::hint,
                    {}
                );
                cursor += line;
            }
        };
        radio_row(
            controls::export_format_oademo,
            "export.format.oademo",
            oademo_choice,
            dialog.format == ExportFormat::oademo
        );
        about_lines(oademo_about);
        cursor += gap;
        radio_row(
            controls::export_format_tad,
            "export.format.tad",
            tad_choice,
            dialog.format == ExportFormat::tad
        );
        about_lines(tad_about);
        if (show_bar) {
            cursor += gap;
            const uint64_t whole = std::max(dialog.progress_bytes_total, uint64_t{1});
            const uint64_t taken = std::min(dialog.progress_bytes_done, whole);
            const int32_t filled_bar = static_cast<int32_t>(std::min<uint64_t>(
                taken * static_cast<uint64_t>(progress_whole) / whole,
                static_cast<uint64_t>(progress_whole)
            ));
            kit::add_progress(
                building.list,
                {left, cursor, inner, metrics.progress_bar_height},
                filled_bar,
                progress_whole
            );
            cursor += metrics.progress_bar_height;
        }

        if (dialog.format == ExportFormat::tad) {
            cursor += section_gap;
            if (include_heading) {
                for (const std::string& include_line : include_lines) {
                    add_text(
                        building,
                        kit::FontRole::small,
                        include_line,
                        {left, cursor, inner, line},
                        kit::Align::left,
                        kit::colour::hint,
                        {}
                    );
                    cursor += line;
                }
                if (include_lines.empty())
                    cursor += line;
                cursor += gap;
            }
            const kit::Rect swatch{left, cursor, metrics.switch_width, control};
            kit::SwitchLook swatch_look;
            swatch_look.on = dialog.keep_oa_sectors;
            swatch_look.hovered = hovered(building, controls::export_oa_sectors);
            swatch_look.off_caption = std::string(shown(text, words::registry_off));
            swatch_look.on_caption = std::string(shown(text, words::registry_on));
            const std::size_t before = building.list.controls.size();
            kit::add_switch(
                building.list,
                swatch,
                swatch_look,
                controls::export_oa_sectors,
                unique_name(building, named("export.oa-sectors"))
            );
            move_controls(building.list, before, building.overlay);
            kit::Control& added = building.overlay.back();
            added.focusable = true;
            added.enabled = true;
            added.text = sectors;
            building.export_order.push_back(added.id);
            const int32_t label_left = swatch.x + swatch.width + gap;
            const int32_t label_width = std::max(left + inner - label_left, int32_t{0});
            int32_t label_y = cursor;
            for (const std::string& sectors_line : sectors_lines) {
                add_text(
                    building,
                    kit::FontRole::small,
                    sectors_line,
                    {label_left, label_y, label_width, line},
                    kit::Align::left,
                    kit::colour::text,
                    {}
                );
                label_y += line;
            }
            cursor += sectors_height;
            if (warning_count > 0 || page) {
                cursor += section_gap;
                if (compatibility_heading) {
                    for (const std::string& compatibility_line : compatibility_lines) {
                        add_text(
                            building,
                            kit::FontRole::small,
                            compatibility_line,
                            {left, cursor, inner, line},
                            kit::Align::left,
                            kit::colour::hint,
                            {}
                        );
                        cursor += line;
                    }
                    if (compatibility_lines.empty())
                        cursor += line;
                    cursor += gap;
                }
                for (std::size_t drawn = 0; drawn < warning_count; ++drawn) {
                    const std::size_t place = warning_start + drawn;
                    if (place >= warnings.size() || place >= controls::most_export_warnings)
                        break;
                    const WrappedNote& note = warnings[place];
                    // A warning cut to the lines that fit ends with the kit's
                    // ellipsis, as its other cuts do. The label keeps the
                    // whole sentence.
                    const bool cut = drawn == 0 && partial_lines > 0 &&
                                     static_cast<std::size_t>(partial_lines) < note.lines.size();
                    const std::vector<std::string> ended_lines =
                        cut ? wrapped(
                                  building,
                                  kit::FontRole::small,
                                  note.sentence,
                                  inner,
                                  static_cast<std::size_t>(partial_lines)
                              )
                            : std::vector<std::string>{};
                    const std::vector<std::string>& shown_lines = cut ? ended_lines : note.lines;
                    const int32_t drawn_lines = static_cast<int32_t>(shown_lines.size());
                    const int32_t note_height = drawn_lines * line;
                    if (!above_foot(note_height))
                        break;
                    const int32_t top = cursor;
                    for (int32_t line_at = 0; line_at < drawn_lines; ++line_at) {
                        add_text(
                            building,
                            kit::FontRole::small,
                            shown_lines[static_cast<std::size_t>(line_at)],
                            {left, cursor, inner, line},
                            kit::Align::left,
                            kit::colour::text,
                            {}
                        );
                        cursor += line;
                    }
                    add_label(
                        building.overlay,
                        controls::first_export_warning + static_cast<kit::ControlId>(place),
                        {left, top, inner, std::max(cursor - top, line)},
                        unique_name(building, named("export.warning." + std::to_string(place))),
                        note.sentence,
                        {},
                        -1
                    );
                }
                if (page && above_foot(gap + control)) {
                    cursor += gap;
                    const bool previous = warning_start > 0;
                    const bool more = warning_start + warning_count < warnings.size();
                    const std::string more_caption(shown(text, words::export_more_warnings));
                    const std::string previous_caption(
                        shown(text, words::export_previous_warnings)
                    );
                    const int32_t more_width =
                        std::min(small_button_width(building, more_caption), inner / 2);
                    const int32_t previous_width =
                        std::min(small_button_width(building, previous_caption), inner / 2);
                    if (previous)
                        take_export_button(
                            building,
                            controls::export_warnings_previous,
                            "export.warnings.previous",
                            {left, cursor, previous_width, control},
                            previous_caption,
                            false,
                            true
                        );
                    if (more)
                        take_export_button(
                            building,
                            controls::export_warnings_more,
                            "export.warnings.more",
                            {left + inner - more_width, cursor, more_width, control},
                            more_caption,
                            false,
                            true
                        );
                    cursor += control;
                }
            }
        }

        if (show_where && above_foot(section_gap + control)) {
            cursor += section_gap;
            const std::string where_caption(shown(text, words::use_where));
            const std::string where_value = folder_shown();
            const std::string where_line = where_caption + "  " + where_value;
            const int32_t choose_width = std::min(
                small_button_width(building, shown(text, words::export_choose)), inner / 3
            );
            const int32_t where_width = std::max(inner - choose_width - gap, int32_t{1});
            add_text(
                building,
                kit::FontRole::small,
                cut(building, kit::FontRole::small, where_line, where_width),
                {left, cursor, where_width, control},
                kit::Align::left,
                kit::colour::text,
                {}
            );
            add_label(
                building.overlay,
                controls::export_where,
                {left, cursor, where_width, control},
                unique_name(building, named("export.where")),
                where_value,
                {},
                -1
            );
            take_export_button(
                building,
                controls::export_choose,
                "export.choose",
                {left + inner - choose_width, cursor, choose_width, control},
                std::string(shown(text, words::export_choose)),
                false,
                !dialog.folder.empty()
            );
            cursor += control;
            if (!note_lines.empty() && above_foot(gap + lines_height(note_lines))) {
                cursor += gap;
                const int32_t top = cursor;
                for (const std::string& note_line : note_lines) {
                    add_text(
                        building,
                        kit::FontRole::small,
                        note_line,
                        {left, cursor, inner, line},
                        kit::Align::left,
                        kit::colour::text,
                        {}
                    );
                    cursor += line;
                }
                note_of(
                    controls::export_note,
                    {left, top, inner, std::max(cursor - top, line)},
                    form_note
                );
            }
        }

        const int32_t button_y = dialog_rect.y + dialog_rect.height - pad - control;
        const bool can_run = export_can_run(dialog);
        const std::string export_caption(shown(text, words::export_confirm));
        const std::string cancel_caption(shown(text, words::action_cancel));
        const int32_t export_width =
            std::min(small_button_width(building, export_caption), inner / 2);
        const int32_t cancel_width =
            std::min(small_button_width(building, cancel_caption), inner / 3);
        take_export_button(
            building,
            controls::export_cancel,
            "export.cancel",
            {left + inner - export_width - gap - cancel_width, button_y, cancel_width, control},
            cancel_caption,
            false,
            true
        );
        take_export_button(
            building,
            controls::export_confirm,
            "export.export",
            {left + inner - export_width, button_y, export_width, control},
            export_caption,
            true,
            can_run
        );
        static_cast<void>(cursor);
    }

    const std::size_t before = building.list.controls.size();
    kit::ButtonLook shade_look;
    shade_look.style = kit::ButtonStyle::quiet;
    kit::add_button(
        building.list, shade, shade_look, controls::export_blocker, named("export.block")
    );
    move_controls(building.list, before, building.overlay);
    building.overlay.back().enabled = true;
    building.overlay.back().focusable = false;
}

/// Finishes a layout: the window's edge, the focus ring, the open menu over
/// everything, the key tooltip, and the controls in the order a press tries them.
///
/// @param[in,out] building the layout
/// @param window the window
/// @return the display list
kit::DisplayList finish(Building& building, const Window& window) {
    add_item(building.list, kit::Role::bevel, {0, 0, window.width, window.height}, {}).colour =
        kit::colour::edge_light;
    add_focus_ring(building);
    add_open_menu(building, window.footer_rule);
    add_unload_note(building, window);
    add_key_tooltip(building, window);
    add_base_chooser(building, window);
    add_key_use(building, window);
    add_export(building, window);
    for (kit::Control& control : building.top)
        if (control.id == controls::close_x)
            tab_stop(building, control);
    kit::DisplayList list = std::move(building.list);
    list.controls.clear();
    for (kit::Control& control : building.overlay)
        list.controls.push_back(std::move(control));
    for (auto* group :
         {&building.menu,
          &building.top,
          &building.rows,
          &building.panes,
          &building.detail,
          &building.footer})
        for (kit::Control& control : *group)
            list.controls.push_back(std::move(control));
    if (building.library->export_dialog.open) {
        list.tab_order = std::move(building.export_order);
        // The dialog is modal. Tab and the arrows stay inside it, so a
        // control of the Library behind the shade does not take the focus.
        const std::unordered_set<kit::ControlId> in_dialog(
            list.tab_order.begin(), list.tab_order.end()
        );
        for (kit::Control& control : list.controls)
            if (in_dialog.count(control.id) == 0)
                control.focusable = false;
    } else if (building.library->chooser.open)
        list.tab_order = std::move(building.chooser_order);
    else if (building.library->key_use.open)
        list.tab_order = std::move(building.question_order);
    else
        list.tab_order = std::move(building.order);
    return list;
}

/// Lays out Compact's page of the selected replay: its map in the header;
/// its details in a card that scrolls; WATCH, EXPORT, KEEP, DELETE (and its
/// confirm) and OPEN FOLDER, and BACK to the list, in the footer. When the
/// buttons wrap, the footer grows so a second row stays inside the window.
///
/// @param library the model, with a replay selected
/// @param state the hover, press, focus and scroll
/// @param frame the room; the page takes its class's window size
/// @param fonts the fonts its texts are measured in
/// @return what the page draws and where its controls are
kit::DisplayList replay_page_layout(
    const Library& library,
    const ScreenState& state,
    const kit::Frame& frame,
    const kit::Fonts& fonts
) {
    Building building = start(library, state, frame, fonts);
    Window window = window_of(frame);
    const kit::Metrics& metrics = *building.metrics;
    const ReplayRow& row = *selected_replay(library);
    // Compact's footer is one row. When the card's buttons wrap, that row
    // grows so the second line stays inside the dialog. Back stays on the
    // first row, at the right.
    const int32_t back_width = button_width(building, action_text(Action::back, library.text));
    const int32_t back_left = window.right - back_width;
    const int32_t buttons_width = std::max(back_left - metrics.label_gap - window.left, int32_t{0});
    const ActionPlan actions =
        plan_actions(building, fitted_replay_actions(building, buttons_width), buttons_width);
    if (frame.size_class == kit::SizeClass::compact && actions.height > window.button_height) {
        const int32_t extra = actions.height - window.button_height;
        window.footer_height += extra;
        window.footer_rule -= extra;
        window.footer_top = window.footer_rule + 1;
        window.button_top = window.footer_top + metrics.space;
    }
    const int32_t buttons_height = std::max(actions.height, window.button_height);
    add_library_header(building, window, replay_title(row), {}, replay_time_text(row.minute));
    const int32_t body_top = window.body_top + window.gap;
    const int32_t pad = 2 * metrics.space;
    // The card stops above the footer, which has grown when the buttons wrap.
    const kit::Rect card{
        window.left,
        body_top,
        window.right - window.left,
        std::max(window.footer_rule - window.bottom_gap - body_top, int32_t{0}),
    };
    add_card_face(building, card);
    const kit::Rect view{
        card.x + pad,
        card.y + pad,
        std::max(card.width - 2 * pad, int32_t{0}),
        std::max(card.height - 2 * pad, int32_t{0}),
    };
    const auto lay_out_body = [&](Building& into, int32_t width, int32_t offset) {
        const std::size_t first_item = into.list.items.size();
        const int32_t first = view.y - offset;
        static_cast<void>(add_replay_details(into, row, view.x, first, width, view));
        const std::optional<controls::Extent> extent =
            controls::clipped_extent(into.list, view, first_item);
        return extent ? extent->bottom - first : 0;
    };
    Building trial = building;
    const int32_t whole_height = lay_out_body(trial, view.width, 0);
    const int32_t well_width = metrics.scroll_thumb_width + 2;
    const bool scrolls = whole_height > view.height;
    const int32_t width = scrolls ? view.width - well_width - metrics.button_gap : view.width;
    int32_t height = whole_height;
    if (scrolls) {
        trial = building;
        height = lay_out_body(trial, width, 0);
    }
    const int32_t limit = kit::ScrollArea::limit_for(height, view.height);
    const int32_t offset = std::clamp(state.details_scroll, int32_t{0}, limit);
    // Its parts scroll with the details: the note is in their group.
    const std::size_t first_detail = building.detail.size();
    lay_out_body(building, width, offset);
    for (std::size_t index = first_detail; index < building.detail.size(); ++index)
        building.detail[index].group = controls::details_group;
    if (limit > 0) {
        kit::ScrollArea scroll;
        scroll.view = view;
        scroll.well = {view.x + view.width - well_width, view.y, well_width, view.height};
        scroll.content_height = height;
        scroll.limit = limit;
        scroll.offset = offset;
        add_item(building.list, kit::Role::scroll_bar, scroll.well, {}).look = kit::ScrollBarLook{
            scroll.thumb(metrics.least_thumb_height), state.bar_held == Pane::details
        };
    }

    add_footer_band(building, window);
    const std::string_view back_caption = action_text(Action::back, library.text);
    add_replay_buttons(
        building, {window.left, window.button_top, buttons_width, buttons_height}, building.footer
    );
    add_button(
        building,
        building.footer,
        {back_left, window.button_top, back_width, window.button_height},
        back_caption,
        false,
        true,
        controls::back,
        named("back"),
        {},
        -1
    );
    return finish(building, window);
}

} // namespace

namespace controls {

std::optional<Action> row_icon_action(const Library& library, const Entry& entry) {
    if (entry.state != State::get && entry.state != State::update)
        return std::nullopt;
    const std::vector<ActionButton> actions = entry_actions(library, entry);
    if (actions.empty() || !actions.front().enabled)
        return std::nullopt;
    const Action first = actions.front().action;
    if (first != Action::get && first != Action::update && first != Action::get_with)
        return std::nullopt;
    return first;
}

std::vector<MenuItem>
menu_items(const Library& library, MenuKind menu, const std::vector<bool>& tag_shown) {
    std::vector<MenuItem> items;
    switch (menu) {
    case MenuKind::none:
        break;
    case MenuKind::filter: {
        for (std::size_t index = 0; index < filter_order.size(); ++index) {
            MenuItem item;
            item.caption = filter_caption(library, filter_order[index]);
            item.word = std::string(filter_words[index]);
            item.filter = filter_order[index];
            items.push_back(std::move(item));
        }
        const std::vector<std::pair<std::string, std::size_t>> tags = tags_in_tab(library);
        const bool labelled =
            library.tab == Tab::mutators || library.tab == Tab::brains || library.tab == Tab::keys;
        for (std::size_t index = 0; index < tags.size(); ++index) {
            MenuItem item;
            item.caption = filled(
                library.text,
                world_tag(tags[index].first) ? words::filter_world : words::filter_tag,
                {{"tag", tag_caption(library, tags[index].first, labelled)},
                 {"count", std::to_string(tags[index].second)}}
            );
            item.word = "tag-" + name_word(tags[index].first);
            item.tag = index;
            items.push_back(std::move(item));
        }
        break;
    }
    case MenuKind::tags: {
        const std::vector<std::pair<std::string, std::size_t>> tags = tags_in_tab(library);
        bool worlds_headed = false;
        for (std::size_t index = 0; index < tags.size(); ++index) {
            if (index < tag_shown.size() && tag_shown[index])
                continue;
            // The world tags come last, under their own heading.
            if (world_tag(tags[index].first) && !worlds_headed) {
                MenuItem heading;
                heading.caption = std::string(shown(library.text, words::world_heading));
                heading.heading = true;
                items.push_back(std::move(heading));
                worlds_headed = true;
            }
            MenuItem item;
            item.caption = filled(
                library.text,
                words::tag_with_count,
                {{"tag",
                  tag_caption(
                      library,
                      tags[index].first,
                      library.tab == Tab::mutators || library.tab == Tab::brains ||
                          library.tab == Tab::keys
                  )},
                 {"count", std::to_string(tags[index].second)}}
            );
            item.word = name_word(tags[index].first);
            item.tag = index;
            items.push_back(std::move(item));
        }
        break;
    }
    case MenuKind::replay_kind:
        for (const ReplayKindItem& kind : replay_kind_items(library)) {
            MenuItem item;
            item.caption = kind.title;
            item.word = kind.id.empty() ? std::string("all") : name_word(kind.id);
            item.replay_kind = kind.id;
            items.push_back(std::move(item));
        }
        break;
    case MenuKind::replay_sort:
        for (std::size_t index = 0; index < replay_sort_order.size(); ++index) {
            MenuItem item;
            item.caption = replay_sort_title(library, replay_sort_order[index]);
            item.word = std::string(replay_sort_words[index]);
            item.replay_sort = replay_sort_order[index];
            items.push_back(std::move(item));
        }
        break;
    case MenuKind::more: {
        const Entry* entry = selected_entry(library);
        if (entry == nullptr)
            break;
        const std::vector<ActionButton> actions = entry_actions(library, *entry);
        for (std::size_t index = compact_action_buttons; index < actions.size(); ++index) {
            MenuItem item;
            item.caption = std::string(actions[index].text);
            item.word = std::string(action_word(actions[index].action));
            item.action = actions[index].action;
            items.push_back(std::move(item));
        }
        break;
    }
    }
    return items;
}

int32_t chosen_item(const Library& library, MenuKind menu, const std::vector<MenuItem>& items) {
    if (menu == MenuKind::more)
        return -1;
    if (menu == MenuKind::replay_kind || menu == MenuKind::replay_sort) {
        for (std::size_t place = 0; place < items.size(); ++place) {
            const MenuItem& item = items[place];
            if ((item.replay_kind && *item.replay_kind == library.replays.kind) ||
                (item.replay_sort && *item.replay_sort == library.replays.sort))
                return static_cast<int32_t>(place);
        }
        return -1;
    }
    const std::vector<std::pair<std::string, std::size_t>> tags = tags_in_tab(library);
    for (std::size_t place = 0; place < items.size(); ++place) {
        const MenuItem& item = items[place];
        const bool tag_chosen = item.tag && *item.tag < tags.size() && !library.tag.empty() &&
                                tags[*item.tag].first == library.tag;
        const bool filter_chosen =
            item.filter && library.tag.empty() && *item.filter == shown_filter(library);
        if (tag_chosen || filter_chosen)
            return static_cast<int32_t>(place);
    }
    return -1;
}

std::optional<Extent>
clipped_extent(const kit::DisplayList& display, const kit::Rect& view, std::size_t first_item) {
    std::optional<Extent> extent;
    for (std::size_t index = first_item; index < display.items.size(); ++index) {
        const kit::Item& item = display.items[index];
        const bool same_clip = item.clip.x == view.x && item.clip.y == view.y &&
                               item.clip.width == view.width && item.clip.height == view.height;
        if (!same_clip || item.rect.height <= 0)
            continue;
        const int32_t bottom = item.rect.y + item.rect.height;
        if (!extent)
            extent = Extent{item.rect.y, bottom};
        extent->top = std::min(extent->top, item.rect.y);
        extent->bottom = std::max(extent->bottom, bottom);
    }
    return extent;
}

std::vector<bool> tags_shown(const kit::DisplayList& display, std::size_t tags) {
    std::vector<bool> shown_tags(tags, false);
    for (std::size_t index = 0; index < tags; ++index)
        shown_tags[index] =
            kit::control_of(display, first_tag + static_cast<kit::ControlId>(index)) != nullptr;
    return shown_tags;
}

} // namespace controls

kit::Point window_size(const kit::Frame& frame) noexcept {
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    kit::Point size{metrics.dialog_width, metrics.dialog_height};
    if (frame.area.width > 0)
        size.x = std::min(size.x, frame.area.width);
    if (frame.area.height > 0)
        size.y = std::min(size.y, frame.area.height);
    return size;
}

std::string badge_letters(std::string_view name) {
    std::vector<std::string_view> parts;
    std::size_t at = 0;
    while (at < name.size()) {
        while (at < name.size() && name[at] == ' ')
            ++at;
        const std::size_t word_start = at;
        while (at < name.size() && name[at] != ' ')
            ++at;
        if (at > word_start)
            parts.push_back(name.substr(word_start, at - word_start));
    }
    std::string letters;
    const auto take = [&letters](std::string_view part, std::size_t characters) {
        std::size_t used = 0;
        for (std::size_t count = 0; count < characters && used < part.size(); ++count)
            used += std::max(kit::character_bytes(part.substr(used)), std::size_t{1});
        letters += part.substr(0, used);
    };
    if (parts.size() == 1)
        take(parts.front(), badge_letter_count);
    else
        for (std::size_t index = 0; index < parts.size() && index < badge_letter_count; ++index)
            take(parts[index], 1);
    for (char& letter : letters)
        if (letter >= 'a' && letter <= 'z')
            letter = static_cast<char>(letter - 'a' + 'A');
    return letters;
}

kit::DisplayList library_layout(
    const Library& library,
    const ScreenState& state,
    const kit::Frame& frame,
    const kit::Fonts& fonts
) {
    if (frame.size_class == kit::SizeClass::compact && state.details_page && library.details_open &&
        selected_entry(library) != nullptr)
        return details_page_layout(library, state, frame, fonts);
    if (frame.size_class == kit::SizeClass::compact && state.replay_page &&
        library.tab == Tab::replays && selected_replay(library) != nullptr)
        return replay_page_layout(library, state, frame, fonts);
    Building building = start(library, state, frame, fonts);
    const Window window = window_of(frame);
    const bool compact = frame.size_class == kit::SizeClass::compact;
    const bool large = frame.size_class == kit::SizeClass::large;
    if (compact) {
        add_library_header(
            building,
            window,
            shown(library.text, words::title_library_header),
            {},
            library.inputs.version_text
        );
    } else {
        add_library_header(
            building,
            window,
            shown(library.text, words::title_open_annihilation),
            shown(library.text, words::title_library_header),
            library.inputs.version_text
        );
    }
    const auto [nav, content] = section_columns(window, *building.metrics);
    add_section_nav(building, nav);
    const bool active = library.tab == Tab::active;
    const bool registries = library.tab == Tab::registries;
    const bool replays = library.tab == Tab::replays;
    const bool catalogue = !active && !registries && !replays;
    // The Updates tab's line, count and UPDATE ALL, takes a row over its list.
    const bool updates_line = library.tab == Tab::updates;
    const int32_t updates_row = window.button_height + window.gap;
    int32_t top = content.y;
    const bool searched = catalogue || replays;
    const int32_t heading_height = searched ? window.field_height : building.metrics->small_line;
    add_section_heading(building, {content.x, top, content.width, heading_height}, searched);
    top += heading_height + window.gap;
    // The filters join Tab's order after the search, which the heading added.
    stop_on_filters(building);
    const auto body_of = [&](int32_t body_top) {
        return kit::Rect{
            content.x,
            body_top,
            content.width,
            window.footer_rule - window.bottom_gap - body_top,
        };
    };
    if (active)
        add_active(building, body_of(top));
    else if (replays)
        add_replays(building, body_of(top), window);
    else if (registries) {
        add_registry_bar(building, window, top);
        add_registries(building, body_of(top));
    } else {
        int32_t list_width = content.width;
        if (!compact) {
            const int32_t beside =
                std::max(content.width - window.gutter - details_floor, int32_t{1});
            list_width = large ? std::min(large_list_width, beside) : beside;
            if (list_width + window.gutter >= content.width)
                list_width = std::max(content.width / 2, int32_t{1});
        }
        const int32_t panes_height = window.footer_rule - window.bottom_gap - top;
        int32_t list_top = top;
        if (updates_line) {
            add_updates_line(
                building,
                {content.x, top, compact ? content.width : list_width, window.button_height}
            );
            list_top += updates_row;
        }
        const RowForm form = compact ? RowForm::compact
                             : (large || library.tab == Tab::brains || library.tab == Tab::keys)
                                 ? RowForm::large
                                 : RowForm::regular;
        add_list(
            building,
            {content.x,
             list_top,
             compact ? content.width : list_width,
             window.footer_rule - window.bottom_gap - list_top},
            form
        );
        if (!compact) {
            const int32_t details_left = content.x + list_width + window.gutter;
            add_details_pane(
                building,
                {details_left,
                 top,
                 std::max(content.x + content.width - details_left, int32_t{0}),
                 panes_height}
            );
        }
    }
    add_list_footer(building, window, compact);
    return finish(building, window);
}

kit::DisplayList details_page_layout(
    const Library& library,
    const ScreenState& state,
    const kit::Frame& frame,
    const kit::Fonts& fonts
) {
    Building building = start(library, state, frame, fonts);
    const Window window = window_of(frame);
    const kit::Metrics& metrics = *building.metrics;
    const Entry* entry = selected_entry(library);
    if (entry == nullptr) {
        add_library_header(
            building, window, shown(library.text, words::title_library_header), {}, {}
        );
        return finish(building, window);
    }
    // A mutator's name sits beside its 40-point badge in the body, so the
    // header keeps the Library title and the Mutators tab. Every other kind
    // keeps its name in the header and no badge on this page.
    const bool mutator_page = entry->id.kind == Kind::mutator;
    if (mutator_page) {
        add_library_header(
            building,
            window,
            shown(library.text, words::title_library_header),
            shown(library.text, words::tab_mutators),
            library.inputs.version_text
        );
    } else {
        add_library_header(
            building, window, entry_name(*entry), {}, entry_version(library, *entry)
        );
    }

    // The body scrolls in a card between the header and the footer. The
    // footer's actions stay outside the card.
    const int32_t content_width = window.right - window.left;
    const int32_t body_top = window.body_top + window.gap;
    const int32_t pad = 2 * metrics.space;
    const kit::Rect card{
        window.left, body_top, content_width, window.footer_rule - window.bottom_gap - body_top
    };
    add_card_face(building, card);
    const kit::Rect view{
        card.x + pad,
        card.y + pad,
        std::max(card.width - 2 * pad, int32_t{0}),
        std::max(card.height - 2 * pad, int32_t{0}),
    };
    const auto lay_out_body = [&](Building& into, int32_t width, int32_t offset) {
        const int32_t first = view.y - offset;
        const std::size_t first_item = into.list.items.size();
        int32_t top = first;
        int32_t status_width = width;
        int32_t status_left = view.x;
        if (mutator_page) {
            add_badge(into, *entry, {view.x, top, details_badge_side, details_badge_side}, view);
            status_left = view.x + details_badge_side + metrics.list_row_gap;
            status_width = std::max(view.x + width - status_left, int32_t{0});
            const std::string name =
                cut(into, kit::FontRole::regular, entry_name(*entry), status_width);
            add_text(
                into,
                kit::FontRole::regular,
                name,
                {status_left, top, status_width, metrics.regular_line},
                kit::Align::left,
                kit::colour::text,
                view
            );
            top += metrics.regular_line + metrics.hint_gap;
        }
        const std::string status =
            cut(into, kit::FontRole::small, status_line(library, *entry), status_width);
        const kit::Rect status_box{status_left, top, status_width, metrics.small_line};
        add_text(
            into,
            kit::FontRole::small,
            status,
            status_box,
            kit::Align::left,
            status_colour(*entry),
            view
        );
        add_label(
            into.detail,
            controls::status,
            status_box,
            unique_name(into, named("status")),
            status,
            view,
            controls::details_group
        );
        top += metrics.small_line;
        if (mutator_page)
            top = std::max(top, first + details_badge_side);
        const std::string by = byline(*entry, library.text);
        if (!by.empty()) {
            top += hint_step(into) - metrics.small_line;
            add_text(
                into,
                kit::FontRole::small,
                cut(into, kit::FontRole::small, by, width),
                {view.x, top, width, metrics.small_line},
                kit::Align::left,
                kit::colour::hint,
                view
            );
            top += metrics.small_line;
        }
        const int32_t gap = part_gap(metrics);
        top += gap;
        if (const std::string_view about = entry_summary(*entry); !about.empty()) {
            const std::vector<std::string> summary =
                wrapped(into, kit::FontRole::small, about, width, compact_summary_lines);
            top = add_lines(into, summary, view.x, top, width, kit::colour::list_text, view) + gap;
        }
        const int32_t tags_bottom =
            add_detail_tags(into, *entry, view.x, top, width, view, controls::details_group);
        if (tags_bottom != top)
            top = tags_bottom + gap;
        top = add_map_previews(into, *entry, view.x, top, width, view);
        top = add_key_picture(into, *entry, view.x, top, width, view);
        top =
            add_facts(into, *entry, {view.x, top, width, 0}, false, compact_fact_lines, view) + gap;
        static_cast<void>(add_note(into, view.x, top, width, view, controls::details_group));
        const std::optional<controls::Extent> extent =
            controls::clipped_extent(into.list, view, first_item);
        return extent ? extent->bottom - first : 0;
    };
    Building trial = building;
    const int32_t whole_height = lay_out_body(trial, view.width, 0);
    const int32_t well_width = metrics.scroll_thumb_width + 2;
    const bool scrolls = whole_height > view.height;
    const int32_t width = scrolls ? view.width - well_width - metrics.button_gap : view.width;
    int32_t height = whole_height;
    if (scrolls) {
        trial = building;
        height = lay_out_body(trial, width, 0);
    }
    const int32_t limit = kit::ScrollArea::limit_for(height, view.height);
    const int32_t offset = std::clamp(state.details_scroll, int32_t{0}, limit);
    lay_out_body(building, width, offset);
    if (limit > 0) {
        kit::ScrollArea scroll;
        scroll.view = view;
        scroll.well = {view.x + view.width - well_width, view.y, well_width, view.height};
        scroll.content_height = height;
        scroll.limit = limit;
        scroll.offset = offset;
        add_item(building.list, kit::Role::scroll_bar, scroll.well, {}).look = kit::ScrollBarLook{
            scroll.thumb(metrics.least_thumb_height), state.bar_held == Pane::details
        };
    }

    // The footer: the first three actions, MORE ▾ with the rest, and BACK.
    // BACK does what the old breadcrumb did: the list, with its place kept.
    add_footer_band(building, window);
    const std::vector<ActionButton> actions = entry_actions(library, *entry);
    int32_t left = window.left;
    for (std::size_t index = 0; index < actions.size() && index < compact_action_buttons; ++index) {
        const ActionButton& action = actions[index];
        const int32_t width_points = button_width(building, action.text);
        add_button(
            building,
            building.footer,
            {left, window.button_top, width_points, window.button_height},
            action.text,
            index == 0 && action.action != Action::uninstall,
            action.enabled,
            controls::action_control(action.action),
            named("action." + std::string(action_word(action.action))),
            {},
            -1
        );
        left += width_points + button_spacing(metrics);
    }
    if (actions.size() > compact_action_buttons) {
        std::vector<controls::MenuItem> items = controls::menu_items(library, MenuKind::more, {});
        const std::string_view caption = shown(library.text, words::more_menu);
        const int32_t width_points = drop_down_width(building, caption, items);
        add_drop_down(
            building,
            building.footer,
            {left, window.button_top, width_points, window.button_height},
            MenuKind::more,
            caption,
            std::move(items),
            controls::more_menu,
            named("action")
        );
    }
    const std::string_view back_caption = action_text(Action::back, library.text);
    const int32_t back_width = button_width(building, back_caption);
    add_button(
        building,
        building.footer,
        {window.right - back_width, window.button_top, back_width, window.button_height},
        back_caption,
        true,
        true,
        controls::back,
        named("back"),
        {},
        -1
    );
    return finish(building, window);
}

namespace {

/// What one refresh mark draws, enough to tell a later step from this one.
struct MarkShot {
    bool turning{};
    uint8_t turn{};
    uint8_t ease{};
    bool operator==(const MarkShot&) const = default;
};

/// Returns the picture a mark draws: the turning arrow, or the check's ease.
///
/// @param turning the arrow is turning
/// @param turn the shared eighth, used while it turns
/// @param check_ms how long the check has shown; 0 while it has not
/// @return the picture
MarkShot mark_shot(bool turning, uint8_t turn, uint16_t check_ms) noexcept {
    MarkShot shot;
    shot.turning = turning;
    shot.turn = turning ? turn : uint8_t{0};
    shot.ease =
        turning ? uint8_t{0} : (check_ms == 0 ? uint8_t{0} : kit::refresh_check_ease(check_ms));
    return shot;
}

/// Returns a registry's mark, or none when this step has not kept one.
///
/// @param state the marks
/// @param id the registry
/// @return the mark; none when the registry has no row
const ScreenState::RegistryMark* mark_of(const ScreenState& state, std::string_view id) noexcept {
    for (const ScreenState::RegistryMark& mark : state.registry_marks)
        if (mark.id == id)
            return &mark;
    return nullptr;
}

/// Moves a check forward, or starts one.
///
/// A start with no time yet still counts as begun. A check that has already
/// lasted kit::refresh_check_ms is over, and the arrow shows.
///
/// @param shown how long it has shown; 0 while it has not
/// @param begin true starts it from nothing
/// @param elapsed_ms the milliseconds to add
/// @return the milliseconds still to show; 0 when the arrow is back
uint16_t advance_check(uint16_t shown, bool begin, uint32_t elapsed_ms) noexcept {
    if (!begin && shown == 0)
        return 0;
    const uint32_t next =
        begin ? (elapsed_ms == 0 ? 1u : elapsed_ms) : static_cast<uint32_t>(shown) + elapsed_ms;
    if (next >= kit::refresh_check_ms)
        return 0;
    return static_cast<uint16_t>(next);
}

} // namespace

bool step_refresh_marks(
    ScreenState& state,
    bool checking,
    std::span<const RegistryPulse> registries,
    uint32_t elapsed_ms
) {
    const MarkShot header_before =
        mark_shot(state.refresh_was_checking, state.refresh_turn, state.refresh_check_ms);
    std::vector<MarkShot> rows_before;
    rows_before.reserve(registries.size());
    for (const RegistryPulse& pulse : registries) {
        const ScreenState::RegistryMark* held = mark_of(state, pulse.id);
        const bool turning = held != nullptr && held->was_refreshing;
        const uint16_t shown = held != nullptr ? held->check_ms : uint16_t{0};
        rows_before.push_back(mark_shot(turning, state.refresh_turn, shown));
    }

    bool any_refreshing = checking;
    for (const RegistryPulse& pulse : registries)
        any_refreshing = any_refreshing || pulse.refreshing;
    if (any_refreshing) {
        if (!state.refresh_spinning)
            state.refresh_spin_ms = 0;
        state.refresh_spin_ms += elapsed_ms;
        state.refresh_turn = kit::refresh_eighth(state.refresh_spin_ms);
    } else {
        state.refresh_spin_ms = 0;
        state.refresh_turn = 0;
    }
    state.refresh_spinning = any_refreshing;

    if (checking && !state.refresh_was_checking)
        state.refresh_read_list = false;
    for (const RegistryPulse& pulse : registries) {
        const ScreenState::RegistryMark* held = mark_of(state, pulse.id);
        const bool was_refreshing = held != nullptr && held->was_refreshing;
        if (was_refreshing && !pulse.refreshing && !pulse.failed &&
            (checking || state.refresh_was_checking))
            state.refresh_read_list = true;
    }

    if (checking)
        state.refresh_check_ms = 0;
    else if (state.refresh_was_checking) {
        state.refresh_check_ms =
            state.refresh_read_list ? advance_check(0, true, elapsed_ms) : uint16_t{0};
        state.refresh_read_list = false;
    } else {
        state.refresh_check_ms = advance_check(state.refresh_check_ms, false, elapsed_ms);
    }
    state.refresh_was_checking = checking;

    std::vector<ScreenState::RegistryMark> next_marks;
    next_marks.reserve(registries.size());
    for (const RegistryPulse& pulse : registries) {
        const ScreenState::RegistryMark* held = mark_of(state, pulse.id);
        const bool was_refreshing = held != nullptr && held->was_refreshing;
        const uint16_t previous = held != nullptr ? held->check_ms : uint16_t{0};
        ScreenState::RegistryMark row;
        row.id = std::string(pulse.id);
        row.was_refreshing = pulse.refreshing;
        if (pulse.refreshing)
            row.check_ms = 0;
        else if (was_refreshing)
            row.check_ms = pulse.failed ? uint16_t{0} : advance_check(0, true, elapsed_ms);
        else
            row.check_ms = advance_check(previous, false, elapsed_ms);
        if (row.was_refreshing || row.check_ms > 0)
            next_marks.push_back(std::move(row));
    }
    state.registry_marks = std::move(next_marks);

    if (mark_shot(checking, state.refresh_turn, state.refresh_check_ms) != header_before)
        return true;
    for (std::size_t row = 0; row < registries.size(); ++row) {
        const ScreenState::RegistryMark* held = mark_of(state, registries[row].id);
        const bool turning = held != nullptr && held->was_refreshing;
        const uint16_t shown = held != nullptr ? held->check_ms : uint16_t{0};
        if (mark_shot(turning, state.refresh_turn, shown) != rows_before[row])
            return true;
    }
    return false;
}

namespace {

/// Returns the first and last visible places whose rows the list shows.
///
/// @param library the model
/// @param list the display list
/// @return the places, inclusive; nothing when no row shows
std::optional<std::pair<std::size_t, std::size_t>>
shown_row_span(const Library& library, const kit::DisplayList& list) {
    std::optional<std::size_t> first;
    std::optional<std::size_t> last;
    for (const kit::Control& control : list.controls) {
        if (!controls::row_control(control.id))
            continue;
        const auto place = static_cast<std::size_t>(control.id - controls::first_row);
        if (place >= library.visible.size())
            continue;
        if (!first || place < *first)
            first = place;
        if (!last || place > *last)
            last = place;
    }
    if (!first || !last)
        return std::nullopt;
    return std::pair<std::size_t, std::size_t>{*first, *last};
}

} // namespace

std::vector<std::size_t> picture_places(const Library& library, const kit::DisplayList& list) {
    std::vector<std::size_t> places;
    const auto add = [&](std::size_t place) {
        if (place >= library.visible.size())
            return;
        if (std::find(places.begin(), places.end(), place) != places.end())
            return;
        places.push_back(place);
    };
    // The selected row is asked before the rows in view.
    if (library.selected) {
        for (std::size_t place = 0; place < library.visible.size(); ++place) {
            if (library.entries[library.visible[place]].id == *library.selected) {
                add(place);
                break;
            }
        }
    }
    const std::optional<std::pair<std::size_t, std::size_t>> shown = shown_row_span(library, list);
    if (!shown)
        return places;
    const std::size_t begin = shown->first > picture_margin ? shown->first - picture_margin : 0;
    std::size_t end = shown->second + 1;
    if (end < library.visible.size()) {
        const std::size_t room = library.visible.size() - end;
        end += room < picture_margin ? room : picture_margin;
    }
    for (std::size_t place = begin; place < end; ++place)
        add(place);
    return places;
}

void request_registry_icons(const Library& library, const RegistryPictureSource& source) {
    if (source.request == nullptr)
        return;
    std::vector<std::string> offered;
    const auto consider = [&](const Registry& registry) {
        if (registry.shipped_logo || registry.icon_url.empty() || registry.icon)
            return;
        if (std::find(offered.begin(), offered.end(), registry.id) != offered.end())
            return;
        if (source.known != nullptr && source.known(source.context, registry))
            return;
        offered.push_back(registry.id);
        source.request(source.context, registry);
    };
    if (library.tab == Tab::registries) {
        for (const Registry& registry : library.inputs.registries)
            consider(registry);
    }
    if (library.selected) {
        for (const Registry& registry : library.inputs.registries) {
            if (registry.id == library.selected->registry)
                consider(registry);
        }
    }
}

void request_visible_pictures(
    const Library& library, const kit::DisplayList& list, const PictureSource& source
) {
    if (source.request == nullptr)
        return;
    const std::vector<std::size_t> places = picture_places(library, list);
    for (std::size_t index = 0; index < places.size(); ++index) {
        const std::size_t place = places[index];
        if (place >= library.visible.size())
            continue;
        const Entry& entry = library.entries[library.visible[place]];
        const bool lead = index == 0 && library.selected && entry.id == *library.selected;
        if (!lead && source.known != nullptr && source.known(source.context, entry))
            continue;
        source.request(source.context, entry, lead);
    }
}

std::vector<std::size_t> replay_places_shown(const Library& library, const kit::DisplayList& list) {
    std::vector<std::size_t> places;
    for (const kit::Control& control : list.controls) {
        if (!controls::replay_row_control(control.id))
            continue;
        const auto place = static_cast<std::size_t>(control.id - controls::first_replay_row);
        if (place < library.replays.visible.size())
            places.push_back(place);
    }
    return places;
}

bool entry_row_shown(const Library& library, const kit::DisplayList& list, const EntryId& id) {
    if (library.selected && *library.selected == id)
        return true;
    for (const kit::Control& control : list.controls) {
        if (!controls::row_control(control.id))
            continue;
        const auto place = static_cast<std::size_t>(control.id - controls::first_row);
        if (place >= library.visible.size())
            continue;
        if (library.entries[library.visible[place]].id == id)
            return true;
    }
    return false;
}

} // namespace oa::ui::library
