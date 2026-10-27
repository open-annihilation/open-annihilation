// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library's layout at Compact, Regular and Large, made of the UI kit's
// components: the window's face, header and footer, the tabs, the search,
// the filters with their counts, the list and the details, the Active tab's
// setup, and the Registries tab's cards, every control named for automation.
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

/// The columns Compact's search row keeps right of the search field, for the
/// filter drop-down and its count, such as "Installed (12)".
constexpr int32_t compact_filter_room = 128;
/// The search field's width at Regular and Large.
constexpr int32_t wide_search_width = 240;
/// The Active tab's maps search, at the right of the MAPS heading, when the row has room.
constexpr int32_t maps_search_width = 148;
/// How many map rows the Active page keeps in view, once scrolled to them,
/// before it scrolls the tiles and the maps as one page.
constexpr int32_t maps_rows_kept = 4;
/// The list pane's width at Regular. It leaves the details room for a key
/// layout's main block at four points a quarter, where its legends show.
constexpr int32_t regular_list_width = 400;
/// The list pane's width at Large.
constexpr int32_t large_list_width = 400;
/// The filter pane's width at Large.
constexpr int32_t filter_pane_width = 180;
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
/// How many action buttons a line of the details pane holds.
constexpr int32_t actions_per_line = 3;
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
    "active", "mods", "maps", "mutators", "brains", "keys", "languages", "updates", "registries"
};
/// The filters, in the order they are shown.
constexpr std::array<Filter, filter_total> filter_order{
    Filter::all, Filter::newest, Filter::installed, Filter::updates
};
/// The words the filters are named by, in that order.
constexpr std::array<std::string_view, filter_total> filter_words{
    "all", "new", "installed", "updates"
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
/// pane and menu, use the eight ids' labels. Every other tag is the id as
/// written, so a mod's or a map pack's own tag does not change.
///
/// @param library the Library
/// @param tag the catalogue id
/// @param labelled true for a mutator's tags, or for the Mutators tab's filters
/// @return the words; a view of `tag` when this place does not label it
std::string_view tag_caption(const Library& library, std::string_view tag, bool labelled) {
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
    int32_t width{};       ///< the window's width, in points
    int32_t height{};      ///< the window's height, in points
    int32_t padding{};     ///< the class's padding
    int32_t left{};        ///< the content's left column
    int32_t right{};       ///< the column after the content's last
    int32_t body_top{};    ///< the first row under the header's rule
    int32_t footer_rule{}; ///< the row of the line over the footer
    int32_t footer_top{};  ///< the footer's first row
    int32_t button_top{};  ///< the footer's buttons' top row
    int32_t gap{};         ///< the rows between stacked parts
    /// The footer's height: Kit 2's window footer, or at Compact its controls
    /// with a space above and below, so the list keeps its room.
    int32_t footer_height{};
    int32_t button_height{}; ///< a footer's or a bar's button: Kit 2's control height
    int32_t tab_height{};    ///< the tab strip: Kit 2's tab strip height
    int32_t field_height{};  ///< the search field and a drop-down field: Kit 2's text field
    int32_t pill_height{};   ///< a filter pill: Kit 2's filter chip height
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
    // Kit 2's window padding, and its sizes for the shell's controls. The
    // header is the kit's, Compact's height at every class. Compact's
    // stacked parts sit a space apart and its footer holds its controls
    // with a space above and below, so the list keeps its room.
    window.padding = metrics.window_padding;
    window.left = window.padding;
    window.right = size.x - metrics.edge - window.padding;
    window.button_height = metrics.control_height;
    window.tab_height = metrics.tab_strip_height;
    window.field_height = metrics.text_field_height;
    window.pill_height = metrics.filter_chip_height;
    window.footer_height =
        compact ? metrics.control_height + 2 * metrics.space : metrics.window_footer_height;
    window.body_top = metrics.edge + metrics.header_height + 1;
    window.footer_rule = size.y - metrics.edge - window.footer_height - 1;
    window.footer_top = window.footer_rule + 1;
    window.button_top = window.footer_top + (window.footer_height - window.button_height) / 2;
    window.gap = compact ? metrics.space : metrics.row_gap;
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

/// Returns a button's width: its caption in the small font and the room a
/// question's button keeps either side, and at least OK's width.
///
/// @param building the layout
/// @param caption the caption
/// @return the width, in points
int32_t button_width(const Building& building, std::string_view caption) {
    const kit::Metrics& metrics = *building.metrics;
    return std::max(
        metrics.button_width,
        width_of(building, kit::FontRole::small, caption) + metrics.prompt_button_padding
    );
}

/// Appends a button.
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
    bool in_tab_order = true
) {
    kit::ButtonLook look;
    look.caption = cut(building, kit::FontRole::small, caption, rect.width - 2);
    look.style = accent && enabled ? kit::ButtonStyle::accent : kit::ButtonStyle::plain;
    look.hovered = hovered(building, id);
    look.held = held(building, id);
    look.enabled = enabled;
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

/// Returns a row's height in a form: Kit 2's row height (Metrics::row_height),
/// and at least the lines the form holds.
///
/// @param metrics the class's metrics
/// @param form the form
/// @return the height, in points
int32_t row_height(const kit::Metrics& metrics, RowForm form) {
    switch (form) {
    case RowForm::compact:
        return std::max(metrics.short_list_row_height, metrics.row_height);
    case RowForm::regular:
        // The title's line, a line under it and a chip line, as Kit 2's row holds them.
        return std::max(
            metrics.list_row_line_top + 2 * metrics.small_line + metrics.list_row_title_top,
            metrics.row_height
        );
    case RowForm::large:
        // The title's line, two lines under it and the rows above the title.
        return std::max(
            metrics.list_row_line_top + 2 * metrics.small_line + metrics.list_row_title_top,
            metrics.row_height
        );
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
                                 : kit::chip_width(*building.fonts, *look.chip);
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
    const int32_t gap = kit::compact_metrics.chip_gap;
    int32_t left = 0;
    for (const kit::ChipLook& chip : chips) {
        const int32_t width = kit::chip_width(*building.fonts, chip);
        if (left + width > room)
            return false;
        left += width + gap;
    }
    return true;
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

/// Returns a chip in the filter colour.
///
/// @param text the chip's words, looked up
/// @return the chip
kit::ChipLook filter_chip(std::string text) {
    kit::ChipLook chip;
    chip.text = std::move(text);
    chip.state = kit::ChipState::filter;
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
    if (named)
        look.lines.push_back(lines.row);
    else if (entry.listing != nullptr && !entry.listing->summary.empty())
        look.lines.push_back(entry.listing->summary);
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
            for (const std::string& tag : facts->tags) {
                kit::ChipLook chip;
                chip.text = std::string(brain_tag_label(tag, library.text));
                chip.state = kit::ChipState::filter;
                labels.push_back(std::move(chip));
            }
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
        // The summary is the line under the name. The own-changes count, in
        // the lock colour, and Made for share the chip line under that, at
        // Regular and Large, Made for as this chip and not as a line as
        // well. The tags take the room that is left. When the line cannot
        // hold them, the tags are the ones dropped, and the own-changes count
        // stays ahead of a Made for that would leave it no room.
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
            kept.push_back(filter_chip(made));
        if (!own.empty())
            kept.push_back(state_chip(std::move(own)));
        std::vector<kit::ChipLook> labels;
        if (const std::vector<std::string>* tags = shown_tags(entry)) {
            labels.reserve(tags->size());
            for (const std::string& tag : *tags)
                labels.push_back(filter_chip(std::string(tag_caption(library, tag, true))));
        }
        const int32_t room = chip_line_room(building, look, row_width, row_height);
        if (kept.size() == 2 && !chips_fit(building, kept, room))
            std::swap(kept[0], kept[1]);
        kept.insert(kept.end(), labels.begin(), labels.end());
        look.tags = std::move(kept);
    } else if (form == RowForm::large) {
        std::vector<kit::ChipLook> labels;
        if (const std::vector<std::string>* tags = shown_tags(entry)) {
            for (const std::string& tag : *tags) {
                kit::ChipLook chip;
                chip.text = std::string(tag_caption(library, tag, entry.id.kind == Kind::mutator));
                chip.state = kit::ChipState::filter;
                labels.push_back(std::move(chip));
            }
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
    const int32_t header_rule = compact.edge + compact.header_height;
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
        compact.edge + (compact.header_height - side) / 2,
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
        compact.header_height,
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

/// Returns the strip's look: each tab's caption and reading, the open tab,
/// the hovered one and the scroll asked for. The counts the kit would draw
/// beside a caption stay unset, so the brackets are the only digits. The
/// open tab is kept wholly in view until the player scrolls the strip, and
/// again once another tab opens.
///
/// @param building the layout
/// @return the look
kit::TabsLook tabs_look(const Building& building) {
    const Library& library = *building.library;
    const ScreenState& state = *building.state;
    kit::TabsLook look;
    look.count_style = kit::TabCountStyle::words;
    look.scroll = state.tab_scroll;
    look.keep_selected_in_view = !state.tab_scrolled || state.tab_laid_out != library.tab;
    look.left_arrow_hovered = hovered(building, controls::tab_scroll_left);
    look.right_arrow_hovered = hovered(building, controls::tab_scroll_right);
    for (std::size_t index = 0; index < tab_order.size(); ++index) {
        const Tab tab = tab_order[index];
        const std::string caption = tab_caption(library, tab, false);
        look.captions.push_back(caption);
        look.counts.push_back(-1);
        look.count_amber.push_back(0);
        if (tab == Tab::active)
            look.readings.emplace_back();
        else
            look.readings.push_back(caption);
        if (tab == library.tab)
            look.selected = index;
        if (hovered(building, controls::tab_control(tab)))
            look.hovered = index;
    }
    return look;
}

/// Tells whether every tab fits a strip of a width, with no scroll.
///
/// @param building the layout
/// @param width the strip's width, in points
/// @return true when the tabs fit
bool tabs_fit(const Building& building, int32_t width) {
    const kit::Rect strip{0, 0, width, building.metrics->tab_height};
    return !kit::place_tabs(*building.fonts, strip, tabs_look(building)).overflows;
}

/// Returns the place of the Updates tab in the strip.
///
/// @return its index
std::size_t updates_tab_index() {
    for (std::size_t index = 0; index < tab_order.size(); ++index)
        if (tab_order[index] == Tab::updates)
            return index;
    return tab_order.size();
}

/// Tells whether Updates lies wholly in the strip when the scroll is at the
/// start, which is where the Library opens on Active.
///
/// @param building the layout
/// @param strip the strip
/// @param look the captions and the gaps
/// @return true when Updates is wholly shown
bool updates_wholly_shown(
    const Building& building, const kit::Rect& strip, const kit::TabsLook& look
) {
    kit::TabsLook opening = look;
    opening.scroll = 0;
    opening.selected = 0;
    const kit::TabStrip placed = kit::place_tabs(*building.fonts, strip, opening);
    const std::size_t updates = updates_tab_index();
    return updates < placed.tabs.size() && kit::wholly_in(placed.tabs[updates], placed.view);
}

/// Tells whether the opening view shows Updates whole and still meets the
/// last tab. A last tab wholly past that view cannot be reached by the
/// arrows. Meeting it, even by a sliver, keeps the walk intact.
///
/// @param building the layout
/// @param strip the strip
/// @param look the captions and the gaps
/// @return true when both hold at the start of the strip
bool opening_tabs_reached(
    const Building& building, const kit::Rect& strip, const kit::TabsLook& look
) {
    if (!updates_wholly_shown(building, strip, look))
        return false;
    kit::TabsLook opening = look;
    opening.scroll = 0;
    opening.selected = 0;
    const kit::TabStrip placed = kit::place_tabs(*building.fonts, strip, opening);
    if (placed.tabs.empty())
        return false;
    const kit::Rect shown = kit::intersect(placed.tabs.back(), placed.view);
    return shown.width > 0 && shown.height > 0;
}

/// Writes Compact's short names into a strip that already holds the full ones.
///
/// @param library the model
/// @param[in,out] look the captions and their readings
void use_compact_tab_names(const Library& library, kit::TabsLook& look) {
    for (std::size_t index = 0; index < tab_order.size() && index < look.captions.size(); ++index) {
        const std::string caption = tab_caption(library, tab_order[index], true);
        look.captions[index] = caption;
        if (index >= look.readings.size())
            continue;
        if (tab_order[index] == Tab::active)
            look.readings[index].clear();
        else
            look.readings[index] = caption;
    }
}

/// Closes the gaps around the names until Updates lies wholly in the opening
/// view and the last tab still meets it. The largest padding that does it is
/// kept, and the theme's tracking while any padding can keep it. Compact's
/// short names stand in only when the full names cannot, however far the
/// gaps close.
///
/// @param building the layout
/// @param strip the strip
/// @param[in,out] look the captions, the open tab and the gaps
void keep_updates_in_view(Building& building, const kit::Rect& strip, kit::TabsLook& look) {
    if (opening_tabs_reached(building, strip, look))
        return;
    const int32_t theme_pad = building.metrics->tab_padding;
    const int32_t theme_track = building.metrics->tab_tracking;
    const auto tighten = [&](int32_t letter_gap) {
        for (int32_t side = theme_pad; side >= 0; --side) {
            look.side_padding = side;
            look.tracking = letter_gap;
            if (opening_tabs_reached(building, strip, look))
                return true;
        }
        return false;
    };
    if (tighten(theme_track))
        return;
    if (theme_track != 0 && tighten(0))
        return;
    use_compact_tab_names(*building.library, look);
    if (tighten(theme_track))
        return;
    if (theme_track != 0 && tighten(0))
        return;
    look.side_padding = 0;
    look.tracking = 0;
}

/// Appends the strip of tabs. Each tab after Active shows its count in
/// brackets after its title, in that title's font and colour. Active has
/// no count. The names are the full ones while those keep Updates wholly in
/// the view the strip opens on. When they do not fit, the strip scrolls
/// sideways: an arrow at an end moves it by one tab, and a drag moves it
/// with the pointer. The scroll is written back, clamped to the strip, and
/// so the open tab stays wholly in view until the player scrolls it.
///
/// @param[in,out] building the layout
/// @param strip the strip
void add_tabs(Building& building, const kit::Rect& strip) {
    kit::TabsLook look = tabs_look(building);
    keep_updates_in_view(building, strip, look);
    std::array<kit::ControlId, tab_order.size()> ids{};
    std::array<std::string, tab_order.size()> names{};
    for (std::size_t index = 0; index < tab_order.size(); ++index) {
        ids[index] = controls::tab_control(tab_order[index]);
        names[index] = unique_name(building, named("tab." + std::string(tab_words[index])));
    }
    const std::string left_name = unique_name(building, named("tab.scroll-left"));
    const std::string right_name = unique_name(building, named("tab.scroll-right"));
    const std::size_t before = building.list.controls.size();
    const std::size_t top_before = building.top.size();
    kit::add_tabs(
        building.list,
        *building.fonts,
        strip,
        look,
        ids,
        names,
        controls::tab_scroll_left,
        left_name,
        controls::tab_scroll_right,
        right_name
    );
    for (auto cursor = building.list.items.rbegin(); cursor != building.list.items.rend();
         ++cursor) {
        if (cursor->role != kit::Role::tabs)
            continue;
        if (const auto* stored = std::get_if<kit::TabsLook>(&cursor->look))
            building.state->tab_scroll = stored->scroll;
        break;
    }
    if (building.state->tab_laid_out != building.library->tab) {
        building.state->tab_laid_out = building.library->tab;
        building.state->tab_scrolled = false;
    }
    move_controls(building.list, before, building.top);
    for (std::size_t index = top_before; index < building.top.size(); ++index)
        tab_stop(building, building.top[index]);
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
void add_drop_down(
    Building& building,
    std::vector<kit::Control>& group,
    const kit::Rect& rect,
    MenuKind menu,
    std::string_view caption,
    std::vector<controls::MenuItem> items,
    kit::ControlId id,
    std::string name
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
        true,
        update_count(library) > 0,
        controls::update_all,
        named("action.update-all"),
        {},
        -1
    );
}

/// Appends Compact's search row: the search field and the filter drop-down.
///
/// @param[in,out] building the layout
/// @param row the row
void add_compact_search_row(Building& building, const kit::Rect& row) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    const int32_t field_width = std::max(row.width - compact_filter_room, int32_t{0});
    add_search(building, {row.x, row.y, field_width, row.height});
    std::vector<controls::MenuItem> items = controls::menu_items(library, MenuKind::filter, {});
    const int32_t chosen = controls::chosen_item(library, MenuKind::filter, items);
    const std::string caption =
        chosen >= 0 ? items[static_cast<std::size_t>(chosen)].caption : std::string();
    add_drop_down(
        building,
        building.top,
        {row.x + field_width + metrics.button_gap,
         row.y,
         compact_filter_room - metrics.button_gap,
         row.height},
        MenuKind::filter,
        caption,
        std::move(items),
        controls::filter_menu,
        named("filter")
    );
}

/// Appends Regular's pills: All, New, Installed and Updates with their
/// counts, the tab's tags while they fit, and TAGS ▾ with the rest. The
/// chosen one is active, the others inactive. Large's column is unchanged.
///
/// @param[in,out] building the layout
/// @param row the row
void add_chip_row(Building& building, const kit::Rect& row) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    // Kit 2's filter chips: the row's height, each pill wider by what it
    // gains in height, so its words stay clear of the rounder ends.
    const int32_t height = row.height;
    const int32_t taller = std::max(height - kit::pill_height(), int32_t{0});
    const int32_t pill_top = row.y;
    int32_t left = row.x;
    const auto add_one = [&](kit::PillLook look, kit::ControlId id, std::string name) {
        look.hovered = hovered(building, id);
        look.focused = focused(building, id);
        const int32_t width = kit::pill_width(*building.fonts, look) + taller;
        const kit::Rect rect{left, pill_top, width, height};
        const std::size_t before = building.list.controls.size();
        kit::add_pill(building.list, rect, look, id, unique_name(building, std::move(name)));
        move_controls(building.list, before, building.top);
        tab_stop(building, building.top.back());
        left += width + metrics.chip_gap;
    };
    for (std::size_t index = 0; index < filter_order.size(); ++index) {
        kit::PillLook look;
        look.text = filter_caption(library, filter_order[index]);
        look.state = shown_filter(library) == filter_order[index] ? kit::PillState::active
                                                                  : kit::PillState::inactive;
        add_one(
            look,
            controls::filter_control(filter_order[index]),
            named("filter." + std::string(filter_words[index]))
        );
    }
    const std::vector<std::pair<std::string, std::size_t>> tags = tags_in_tab(library);
    const bool mutator_tab = library.tab == Tab::mutators;
    const bool labelled = mutator_tab || library.tab == Tab::brains || library.tab == Tab::keys;
    std::vector<int32_t> widths;
    int32_t total = 0;
    for (const auto& [tag, count] : tags) {
        kit::PillLook look;
        look.text = std::string(tag_caption(library, tag, labelled));
        if (mutator_tab)
            look.tag = kit::tag_icon_of(tag);
        widths.push_back(kit::pill_width(*building.fonts, look) + taller);
        total += widths.back() + metrics.chip_gap;
    }
    const int32_t row_right = row.x + row.width;
    std::vector<bool> tag_shown(tags.size(), true);
    int32_t limit = row_right;
    if (left + total - metrics.chip_gap > row_right) {
        // Not every tag fits: TAGS ▾ takes the row's end and holds the rest.
        std::vector<bool> none(tags.size(), false);
        const std::vector<controls::MenuItem> every =
            controls::menu_items(library, MenuKind::tags, none);
        limit = row_right -
                drop_down_width(building, shown(library.text, words::tags_menu), every) -
                metrics.chip_gap;
    }
    for (std::size_t index = 0; index < tags.size(); ++index) {
        if (left + widths[index] > limit) {
            // The tags keep their order: once one does not fit, the rest go to TAGS ▾.
            std::fill(
                tag_shown.begin() + static_cast<std::ptrdiff_t>(index), tag_shown.end(), false
            );
            break;
        }
        kit::PillLook look;
        look.text = std::string(tag_caption(library, tags[index].first, labelled));
        if (mutator_tab)
            look.tag = kit::tag_icon_of(tags[index].first);
        look.state =
            library.tag == tags[index].first ? kit::PillState::active : kit::PillState::inactive;
        add_one(
            look,
            controls::first_tag + static_cast<kit::ControlId>(index),
            named("tag." + name_word(tags[index].first))
        );
    }
    if (std::find(tag_shown.begin(), tag_shown.end(), false) == tag_shown.end())
        return;
    std::vector<controls::MenuItem> items =
        controls::menu_items(library, MenuKind::tags, tag_shown);
    const std::string_view caption = shown(library.text, words::tags_menu);
    const int32_t width = std::min(drop_down_width(building, caption, items), row_right - left);
    add_drop_down(
        building,
        building.top,
        {row_right - width, row.y, width, row.height},
        MenuKind::tags,
        caption,
        std::move(items),
        controls::tags_menu,
        named("tags")
    );
}

/// Appends Large's filter pane: a nav list of All, New, Installed and
/// Updates with their counts, the heading TAGS and a nav list of the tab's
/// tags with their counts, TAGS ▾ holding those that do not fit.
///
/// @param[in,out] building the layout
/// @param pane the pane
void add_filter_pane(Building& building, const kit::Rect& pane) {
    const Library& library = *building.library;
    const kit::Metrics& metrics = *building.metrics;
    kit::NavLook nav;
    nav.area = pane;
    nav.rule_column = pane.x + pane.width - 1;
    const int32_t inset = metrics.paragraph_gap;
    const int32_t entry_left = pane.x + inset;
    const int32_t entry_width = pane.width - 2 * inset;
    // The kit's nav draws an entry's caption from 12 columns in, and keeps 4 clear at its right.
    const int32_t caption_room =
        entry_width - kit::compact_metrics.padding - kit::compact_metrics.list_row_inset;
    const int32_t entry_height = metrics.regular_line + metrics.list_row_inset;
    const int32_t step = entry_height + metrics.hairline;
    int32_t top = pane.y + inset;
    std::vector<kit::ControlId> ids;
    std::vector<std::string> names;
    const auto add_entry = [&](std::string caption,
                               bool selected,
                               kit::ControlId id,
                               std::string name,
                               std::optional<kit::TagIcon> tag = std::nullopt) {
        kit::NavEntry entry;
        entry.rect = {entry_left, top, entry_width, entry_height};
        // The kit draws a tag's icon in the caption's box and shifts the
        // words by the icon and a gap. The fit check still uses the whole
        // box, so the words are cut to what remains beside the icon.
        int32_t room = caption_room;
        if (tag.has_value()) {
            const int32_t icon_side = std::max(kit::compact_metrics.regular_line - 4, int32_t{0});
            const int32_t icon_shift = icon_side + kit::compact_metrics.list_row_gap;
            room = std::max(room - icon_shift, int32_t{0});
        }
        entry.caption = cut(building, kit::FontRole::regular, caption, room);
        entry.tag = tag;
        entry.selected = selected;
        entry.hovered = hovered(building, id) || building.state->interaction.pressed == id;
        entry.focused = focused(building, id);
        nav.entries.push_back(std::move(entry));
        ids.push_back(id);
        names.push_back(unique_name(building, std::move(name)));
        top += step;
    };
    for (std::size_t index = 0; index < filter_order.size(); ++index)
        add_entry(
            filter_caption(library, filter_order[index]),
            shown_filter(library) == filter_order[index],
            controls::filter_control(filter_order[index]),
            named("filter." + std::string(filter_words[index]))
        );
    const std::vector<std::pair<std::string, std::size_t>> tags = tags_in_tab(library);
    std::optional<kit::Rect> heading;
    std::vector<bool> tag_shown(tags.size(), false);
    if (!tags.empty()) {
        top += metrics.paragraph_gap;
        heading = kit::Rect{entry_left, top, entry_width, metrics.small_line};
        top += metrics.small_line + metrics.hint_gap;
        const int32_t bottom = pane.y + pane.height - inset;
        for (std::size_t index = 0; index < tags.size(); ++index) {
            const bool last = index + 1 == tags.size();
            // The last place left holds TAGS ▾ when tags remain after it.
            const int32_t needed = last ? entry_height : step + metrics.regular_line;
            if (top + needed > bottom)
                break;
            tag_shown[index] = true;
            add_entry(
                filled(
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
                ),
                library.tag == tags[index].first,
                controls::first_tag + static_cast<kit::ControlId>(index),
                named("tag." + name_word(tags[index].first)),
                library.tab == Tab::mutators ? kit::tag_icon_of(tags[index].first) : std::nullopt
            );
        }
    }
    const std::size_t before = building.list.controls.size();
    if (nav.entries.empty())
        add_item(building.list, kit::Role::nav, pane, {}).look = nav;
    else
        kit::add_nav(building.list, nav, ids, names);
    move_controls(building.list, before, building.top);
    for (std::size_t index = building.top.size() - ids.size(); index < building.top.size(); ++index)
        tab_stop(building, building.top[index]);
    if (heading)
        add_item(building.list, kit::Role::heading, *heading, {}).look =
            kit::HeadingLook{std::string(shown(library.text, words::tags_menu))};
    if (std::find(tag_shown.begin(), tag_shown.end(), false) == tag_shown.end())
        return;
    std::vector<controls::MenuItem> items =
        controls::menu_items(library, MenuKind::tags, tag_shown);
    const std::string_view caption = shown(library.text, words::tags_menu);
    add_drop_down(
        building,
        building.top,
        {entry_left, top, entry_width, metrics.regular_line},
        MenuKind::tags,
        caption,
        std::move(items),
        controls::tags_menu,
        named("tags")
    );
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
    const int32_t height = row_height(metrics, form);
    const int32_t view_height = std::max(area.height, int32_t{0}) / height * height;
    const kit::Rect view{area.x, area.y, area.width, view_height};
    const int32_t well_width = metrics.scroll_thumb_width + 2;
    const kit::Rect well{view.x + view.width - well_width, view.y, well_width, view.height};
    const int32_t rows_width = view.width - well_width - metrics.button_gap;
    const auto content = static_cast<int32_t>(library.visible.size()) * height;
    const int32_t limit = kit::ScrollArea::limit_for(content, view.height);
    // A From link names the row to put at the top of the list. The write is
    // through the state's mutable scroll: the layout is handed a const state,
    // and a temporary state discards it.
    if (building.state->reveal_row >= 0 && !library.visible.empty()) {
        const int32_t last = static_cast<int32_t>(library.visible.size()) - 1;
        const int32_t place = std::clamp(building.state->reveal_row, int32_t{0}, last);
        const int64_t raw = static_cast<int64_t>(place) * static_cast<int64_t>(height);
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
    for (std::size_t place = 0; place < library.visible.size(); ++place) {
        const int32_t top = view.y + static_cast<int32_t>(place) * height - offset;
        if (top + height <= view.y || top >= view.y + view.height)
            continue;
        const Entry& entry = library.entries[library.visible[place]];
        const kit::ControlId id = controls::first_row + static_cast<kit::ControlId>(place);
        kit::ListRowLook look = row_look(building, entry, form, rows_width, height);
        look.selected = library.selected.has_value() && *library.selected == entry.id;
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

/// Appends lines of a text in the small font, one under another.
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
            int32_t lines = one ? int32_t{0} : metrics.small_line;
            if (!map.size.empty() || map.players > 0)
                lines += metrics.small_line;
            if (!map.description.empty()) {
                descriptions[place] = wrapped(
                    building,
                    kit::FontRole::small,
                    map.description,
                    across,
                    one ? wide_summary_lines : map_preview_lines
                );
                lines += static_cast<int32_t>(descriptions[place].size()) * metrics.small_line;
            }
            block_height = std::max(block_height, across + metrics.hint_gap + lines);
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
                text_top += metrics.small_line;
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
                text_top += metrics.small_line;
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
/// text names the keys it marks. Compact and the wide details both call this
/// before the facts.
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
    // Beside the list, the main block alone, as wide as the details, so its
    // keys' legends can be read; the whole keyboard when a key the layout
    // changes lies outside that block. Compact's page is wide enough for the
    // whole keyboard.
    if (building.size_class != kit::SizeClass::compact) {
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
/// of lines, the last cut with "...". A value that is met ends with the
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
            line_top += metrics.small_line;
        }
        if (good) {
            const int32_t last_width =
                lines.empty() ? 0 : width_of(building, kit::FontRole::small, lines.back());
            const int32_t met_top = lines.empty() ? top : line_top - metrics.small_line;
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
            std::max(static_cast<int32_t>(lines.size()), int32_t{1}) * metrics.small_line;
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
        top += lines_height;
    }
    return top;
}

/// Appends lines of a text in the small font, one under another.
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
    for (const std::string& line : lines) {
        add_text(
            building,
            kit::FontRole::small,
            line,
            {left, top, width, building.metrics->small_line},
            kit::Align::left,
            ink,
            clip
        );
        top += building.metrics->small_line;
    }
    return top;
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

/// Appends a package's tags as inactive pills on a wrapping row. Pressing
/// one chooses that tag. Nothing is added when the package has no tags.
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
    const int32_t height = kit::pill_height();
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
        look.state = kit::PillState::inactive;
        const kit::ControlId id = controls::first_detail_tag + static_cast<kit::ControlId>(index);
        look.hovered = hovered(building, id);
        look.focused = focused(building, id);
        const int32_t across = kit::pill_width(*building.fonts, look);
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
    const int32_t chip_width = kit::chip_width(*building.fonts, chip);
    const int32_t chip_height = chip.icon ? metrics.state_icon_side : metrics.chip_height;
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
        first + (metrics.regular_line - chip_height) / 2,
        chip_width,
        chip_height,
    };
    kit::add_chip(building.list, chip_rect, chip, kit::no_control, {});
    building.list.items.back().clip = view;
    const std::vector<std::string> status =
        status_on_chip
            ? std::vector<std::string>{}
            : wrapped(building, kit::FontRole::small, status_words, text_width, wide_status_lines);
    const int32_t status_top = first + metrics.regular_line;
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
    int32_t top = first + std::max(details_badge_side, status_bottom - first) +
                  building.metrics->paragraph_gap;

    // The listing's summary, or a mod of the player's own's description.
    if (const std::string_view about = entry_summary(entry); !about.empty()) {
        const std::vector<std::string> summary =
            wrapped(building, kit::FontRole::small, about, width, wide_summary_lines);
        top = add_lines(building, summary, left, top, width, kit::colour::list_text, view) +
              metrics.paragraph_gap;
    }
    const int32_t tags_bottom =
        add_detail_tags(building, entry, left, top, width, view, controls::details_group);
    if (tags_bottom != top)
        top = tags_bottom + metrics.paragraph_gap;
    top = add_map_previews(building, entry, left, top, width, view);
    top = add_key_picture(building, entry, left, top, width, view);
    top = add_facts(building, entry, {left, top, width, 0}, true, wide_fact_lines, view) +
          metrics.paragraph_gap;

    static_cast<void>(add_note(building, left, top, width, view, controls::details_group));
    const std::optional<controls::Extent> extent =
        controls::clipped_extent(building.list, view, first_item);
    return extent ? extent->bottom - first : 0;
}

/// Returns the rows the details' actions take at the pane's foot: three to a
/// line at Kit 2's control height.
///
/// @param building the layout
/// @param entry the entry
/// @return the rows; 0 when it has no action
int32_t detail_actions_height(const Building& building, const Entry& entry) {
    const kit::Metrics& metrics = *building.metrics;
    const auto count = static_cast<int32_t>(entry_actions(*building.library, entry).size());
    if (count == 0)
        return 0;
    const int32_t lines = (count + actions_per_line - 1) / actions_per_line;
    return lines * metrics.control_height + (lines - 1) * metrics.button_gap;
}

/// Appends the details' actions, three to a line at Kit 2's control height,
/// in a bar at the pane's foot that does not scroll.
///
/// @param[in,out] building the layout
/// @param entry the entry
/// @param bar the bar
void add_detail_actions(Building& building, const Entry& entry, const kit::Rect& bar) {
    const kit::Metrics& metrics = *building.metrics;
    const std::vector<ActionButton> actions = entry_actions(*building.library, entry);
    const int32_t button_gap = metrics.button_gap;
    const int32_t button_height = metrics.control_height;
    const int32_t each =
        std::max((bar.width - (actions_per_line - 1) * button_gap) / actions_per_line, int32_t{0});
    for (std::size_t index = 0; index < actions.size(); ++index) {
        const auto column = static_cast<int32_t>(index) % actions_per_line;
        const auto line = static_cast<int32_t>(index) / actions_per_line;
        const ActionButton& action = actions[index];
        add_button(
            building,
            building.detail,
            {bar.x + column * (each + button_gap),
             bar.y + line * (button_height + button_gap),
             each,
             button_height},
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

/// Appends the details pane of the selected entry: its parts, which scroll
/// when they are taller than the room above the actions, the pane then
/// keeping a column for the scroll bar at its right, and the actions in a
/// bar at the pane's foot, which do not scroll.
///
/// @param[in,out] building the layout
/// @param whole the pane
void add_details_pane(Building& building, const kit::Rect& whole) {
    const Entry* entry = selected_entry(*building.library);
    if (entry == nullptr)
        return;
    const kit::Metrics& metrics = *building.metrics;
    const int32_t actions_height = detail_actions_height(building, *entry);
    kit::Rect pane = whole;
    if (actions_height > 0)
        pane.height = std::max(whole.height - actions_height - metrics.row_gap, int32_t{0});
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
            {whole.x, whole.y + whole.height - actions_height, whole.width, actions_height}
        );
}

/// The side of an Active tile's badge. A list row's badge plus the card's
/// inset on either side, so the badge is larger than a row's and the size
/// is still taken from the kit's metrics.
///
/// @param metrics the class's metrics
/// @return the side, in points
int32_t active_badge_side(const kit::Metrics& metrics) {
    return metrics.list_row_badge + 2 * metrics.card_inset;
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

/// Returns a tile line's height.
///
/// @param metrics the class's metrics
/// @param font the line's font
/// @return the height, in points
int32_t tile_line_height(const kit::Metrics& metrics, kit::FontRole font) {
    return font == kit::FontRole::regular ? metrics.regular_line : metrics.small_line;
}

/// Returns the height of a tile's lines, stacked.
///
/// @param metrics the class's metrics
/// @param lines the lines
/// @return the height, in points
int32_t tile_lines_height(const kit::Metrics& metrics, const std::vector<TileLine>& lines) {
    int32_t height = 0;
    for (const TileLine& line : lines)
        height += tile_line_height(metrics, line.font);
    return height;
}

/// Returns an identity tile's height. The badge and the lines start at the
/// top inset. A tile with a button keeps it under both, with the button gap
/// between and the card's inset below the button. One without a button keeps
/// that inset below the badge or the last line.
///
/// @param metrics the class's metrics
/// @param badge the badge's side
/// @param lines the lines beside the badge
/// @param button the tile holds a button
/// @return the height, in points
int32_t identity_tile_height(
    const kit::Metrics& metrics, int32_t badge, const std::vector<TileLine>& lines, bool button
) {
    int32_t stack = std::max(badge, tile_lines_height(metrics, lines));
    if (button)
        stack += metrics.button_gap + metrics.button_height;
    return stack + 2 * metrics.card_inset;
}

/// Appends a tile's lines. The first line keeps room at its right for a
/// chip; a line that cannot fit is left out, and its row is kept.
///
/// @param[in,out] building the layout
/// @param lines the lines
/// @param left their left column
/// @param top the first line's top row
/// @param width their width
/// @param first_room the first line's width
/// @param clip what drawing may touch
void place_tile_lines(
    Building& building,
    const std::vector<TileLine>& lines,
    int32_t left,
    int32_t top,
    int32_t width,
    int32_t first_room,
    const kit::Rect& clip
) {
    const kit::Metrics& metrics = *building.metrics;
    int32_t row = top;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const TileLine& line = lines[index];
        const int32_t room = index == 0 ? first_room : width;
        const int32_t height = tile_line_height(metrics, line.font);
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
        row += height;
    }
}

/// Appends a pill that takes no press, at the right of a line, when it fits.
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
    const int32_t width = kit::pill_width(*building.fonts, look);
    const int32_t height = kit::pill_height();
    if (width > line.width || height > line.height)
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
/// at the right of the title's line, and a button at the bottom right when
/// the tile has one. The label is the tile above that button, so the two do
/// not meet, and the label takes no press.
///
/// @param[in,out] building the layout
/// @param tile the tile
/// @param badge the badge; null for the letters
/// @param letters the name the letters are taken from
/// @param lines the lines beside the badge
/// @param pill the pill's words; empty for none
/// @param pill_state the pill's colours
/// @param button the tile holds a button
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
    const int32_t inset = metrics.card_inset;
    const int32_t side =
        std::min(active_badge_side(metrics), std::max(tile.width - 2 * inset, int32_t{0}));
    add_tile_face(building, tile, clip);
    const int32_t inner_left = tile.x + inset;
    const int32_t inner_top = tile.y + inset;
    if (side > 0)
        add_badge_of(
            building, picture_of(badge), letters, {inner_left, inner_top, side, side}, clip
        );
    const int32_t text_left = inner_left + side + metrics.list_row_gap;
    const int32_t text_width = std::max(tile.x + tile.width - inset - text_left, int32_t{0});
    const int32_t text_top = inner_top;
    const int32_t title_height =
        lines.empty() ? metrics.regular_line : tile_line_height(metrics, lines.front().font);
    const int32_t pill_width = add_idle_pill(
        building, {text_left, text_top, text_width, title_height}, pill, pill_state, clip
    );
    const int32_t first_room =
        pill_width > 0 ? std::max(text_width - pill_width - metrics.list_row_gap, int32_t{0})
                       : text_width;
    place_tile_lines(building, lines, text_left, text_top, text_width, first_room, clip);
    kit::Rect label = tile;
    if (button && !button_caption.empty()) {
        const int32_t across = std::min(
            button_width(building, button_caption), std::max(tile.width - 2 * inset, int32_t{0})
        );
        const kit::Rect button_rect{
            tile.x + tile.width - inset - across,
            tile.y + tile.height - inset - metrics.button_height,
            across,
            metrics.button_height,
        };
        if (across > 0 && button_rect.y >= tile.y) {
            add_button(
                building,
                building.detail,
                button_rect,
                button_caption,
                true,
                button_enabled,
                button_id,
                button_name,
                clip,
                -1
            );
            label.height = std::max(button_rect.y - tile.y, int32_t{0});
        }
    }
    add_label(
        building.detail, id, kit::intersect(label, clip), std::move(name), std::move(read), clip, -1
    );
}

/// Returns one map tile's height: the minimap or the lines beside it,
/// whichever is taller, with the card's padding above and below. A pack
/// of several maps keeps a From line; a pack of one does not.
///
/// @param metrics the class's metrics
/// @param side the minimap's side
/// @param described the map has a description to show
/// @param from_line the tile draws "From <pack>"
/// @return the height, in points
int32_t map_tile_height(const kit::Metrics& metrics, int32_t side, bool described, bool from_line) {
    int32_t lines = metrics.regular_line + metrics.small_line;
    if (described)
        lines += metrics.small_line;
    if (from_line)
        lines += metrics.small_line;
    return metrics.card_inset + std::max(side, lines) + metrics.card_inset;
}

/// What an identity tile shows. The layout paints it; the model chose the words.
struct IdentityTile {
    const Badge* badge{};
    std::string letters{};
    std::vector<TileLine> lines{};
    std::string pill{};
    kit::PillState pill_state{kit::PillState::neutral};
    bool button{}; ///< a button at the bottom right
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

/// Returns an identity tile's height at this class.
///
/// @param metrics the class's metrics
/// @param tile the tile
/// @return the height, in points
int32_t identity_height(const kit::Metrics& metrics, const IdentityTile& tile) {
    return identity_tile_height(metrics, active_badge_side(metrics), tile.lines, tile.button);
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
    const int32_t inset = metrics.card_inset;
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
    if (number <= controls::most_active_maps) {
        // The composed line stays a label. When the title is the link, the
        // label sits on the players-and-size line so the two do not meet.
        const int32_t label_top = title_is_link ? row + metrics.regular_line : row;
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
    row += metrics.regular_line;
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
    row += metrics.small_line;
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
        row += metrics.small_line;
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
    const int32_t row_height = std::max(metrics.regular_line, metrics.button_height);
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
            top + std::max((row_height - metrics.button_height) / 2, int32_t{0});
        add_button(
            building,
            building.detail,
            {button_x, button_y, across, metrics.button_height},
            keys.button_caption,
            true,
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
                    std::max(across - 2 * metrics.card_inset, int32_t{0})
                );
                row_height = std::max(
                    row_height,
                    map_tile_height(metrics, side, !map.description.empty(), !map.from.empty())
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
    tab_stop(building, added);
}

/// Appends the MAPS heading and, at its right, the maps search. While the
/// search holds text, the count of tiles shown sits after the heading.
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
    const int32_t text_top = top + std::max((metrics.field_height - metrics.small_line) / 2, 0);
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
        add_maps_search(building, {field_x, top, field_width, metrics.field_height}, clip);
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
    if (kit::pill_width(*building.fonts, look) <= room)
        return text;
    const int32_t padding = std::max(building.metrics->chip_padding, kit::pill_height() / 2);
    std::string fitted =
        cut(building, kit::FontRole::small, text, std::max(room - 2 * padding, int32_t{0}));
    look.text = fitted;
    while (!fitted.empty() && kit::pill_width(*building.fonts, look) > room) {
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
    const int32_t gap = metrics.button_gap;
    const int32_t pill_height = kit::pill_height();
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
        int32_t width = room <= 0 ? 0 : kit::pill_width(*building.fonts, look);
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
            wrap.rows_height += gap;
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

/// Returns the expansions tile's height, including a button's room when the
/// mod tile has UNLOAD MOD, so the pills stay above that button. The lock
/// and the hint take one line for each line they wrap to.
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
    const int32_t inset = metrics.card_inset;
    const int32_t inner = std::max(width - 2 * inset, int32_t{0});
    const ExpansionWrap wrap = wrap_expansions(building, content, inner);
    int32_t stack = wrap.rows_height;
    bool placed = !wrap.rows.empty();
    const std::vector<std::string> lock_lines = expansion_lock_lines(building, content.lock, inner);
    if (!lock_lines.empty()) {
        stack += (placed ? metrics.hint_gap : 0) +
                 static_cast<int32_t>(lock_lines.size()) * metrics.small_line;
        placed = true;
    }
    const std::vector<std::string> hint_lines = expansion_hint_lines(building, content.hint, inner);
    if (!hint_lines.empty())
        stack += (placed ? metrics.hint_gap : 0) +
                 static_cast<int32_t>(hint_lines.size()) * metrics.small_line;
    if (reserve_button)
        stack += metrics.button_gap + metrics.button_height;
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
    const int32_t inset = metrics.card_inset;
    add_tile_face(building, tile, clip);
    const int32_t inner_left = tile.x + inset;
    const int32_t inner_width = std::max(tile.width - 2 * inset, int32_t{0});
    const ExpansionWrap wrap = wrap_expansions(building, content, inner_width);
    int32_t row_y = tile.y + inset;
    const int32_t pill_height = kit::pill_height();
    for (const ExpansionRow& row : wrap.rows) {
        int32_t column = inner_left;
        const int32_t band = std::max(row.band, pill_height);
        for (std::size_t place = 0; place < row.indexes.size(); ++place) {
            const std::size_t index = row.indexes[place];
            const ExpansionPill& pill = content.pills[index];
            const int32_t width = row.widths[place];
            if (width <= 0 || index >= controls::most_expansions) {
                column += width + metrics.button_gap;
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
            look.state =
                pill.face == ExpansionFace::on ? kit::PillState::active : kit::PillState::inactive;
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
            control.clip = clip;
            control.group = -1;
            control.focusable = pressable;
            control.enabled = pressable;
            tab_stop(building, control);
            column += width + metrics.button_gap;
        }
        row_y += band + metrics.button_gap;
    }
    // After the last row of pills, row_y is a button gap below it. The lock
    // and the hint each stand a hint gap below what is above them.
    bool placed = !wrap.rows.empty();
    if (placed)
        row_y -= metrics.button_gap;
    const std::vector<std::string> lock_lines =
        expansion_lock_lines(building, content.lock, inner_width);
    if (!lock_lines.empty()) {
        if (placed)
            row_y += metrics.hint_gap;
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
            row_y += metrics.small_line;
        }
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
            row_y += metrics.hint_gap;
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
            row_y += metrics.small_line;
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
    const int32_t row1 =
        std::max(identity_height(metrics, base), identity_height(metrics, language));
    const int32_t row2 = std::max(
        expansions_height(building, content, left_width, mod.button), identity_height(metrics, mod)
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
    const int32_t keys_height = identity_height(metrics, keys);
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
/// then the MAPS heading and its search, then the maps. On a small window,
/// when the tiles leave the maps fewer than four rows, the page scrolls as
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

    const int32_t heading_span = metrics.field_height + metrics.hint_gap;
    const int32_t full_bottom = measured_identity_bottom(building, setup, area);
    const int32_t pane_height = area.y + area.height - (full_bottom + heading_span);
    const bool page = building.size_class == kit::SizeClass::compact &&
                      pane_height < maps_rows_kept * metrics.small_line;
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
        // and the key icon. The key, REFRESH and the switch share the band's
        // top, so Tab reads them left to right and then the buttons below.
        // A wrapped heading grows under that band.
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
        const kit::Rect toggle{
            cluster,
            band_top,
            switch_columns,
            metrics.regular_line,
        };
        cluster -= metrics.button_gap;
        const std::string_view refresh_caption = shown(library.text, words::registry_refresh);
        const int32_t refresh_width = button_width(building, refresh_caption);
        cluster -= refresh_width;
        const kit::Rect refresh_rect{cluster, band_top, refresh_width, metrics.button_height};
        cluster -= metrics.button_gap;
        kit::Rect turning_rect{};
        if (show_mark) {
            cluster -= metrics.button_height;
            turning_rect = {cluster, band_top, metrics.button_height, metrics.button_height};
            cluster -= metrics.button_gap;
        }
        cluster -= metrics.state_icon_side;
        const kit::Rect key_rect{
            cluster, band_top, metrics.state_icon_side, metrics.state_icon_side
        };
        const int32_t text_left = inner_left + side + metrics.list_row_gap;
        const int32_t title_right = cluster - metrics.label_gap;
        const int32_t title_width = title_right - text_left;
        const std::string_view heading = registry.title.empty() ? std::string_view{registry.name}
                                                                : std::string_view{registry.title};
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
        const int32_t header_height = std::max(title_band, std::max(heading_height, side));
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
        add_button(
            building,
            building.detail,
            refresh_rect,
            refresh_caption,
            false,
            registry.on && !registry.refreshing,
            controls::first_registry_refresh + static_cast<kit::ControlId>(index),
            named("registry." + word + ".refresh"),
            view,
            controls::details_group
        );
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
            const int32_t remove_width = button_width(building, remove_caption);
            add_button(
                building,
                building.detail,
                {text_left, buttons_top, remove_width, metrics.button_height},
                remove_caption,
                false,
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
                add_button(
                    building,
                    building.detail,
                    {mirror_left,
                     buttons_top,
                     button_width(building, mirror_caption),
                     metrics.button_height},
                    mirror_caption,
                    false,
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
    if (compact && library.tab != Tab::active && library.tab != Tab::registries)
        buttons.push_back({Action::details, controls::details, library.selected.has_value()});
    buttons.push_back({Action::close, controls::close, true});
    int32_t total = 0;
    for (const FooterButton& button : buttons)
        total +=
            button_width(building, action_text(button.action, library.text)) + metrics.button_gap;
    total -= metrics.button_gap;
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
        left += width + metrics.button_gap;
    }

    const QueueLines lines = queue_lines(library);
    const int32_t queue_width = std::max(queue_right - window.left, int32_t{0});
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
    for (int32_t place = 0; place < shown_count && first + place < count; ++place) {
        const controls::MenuItem& item = menu_open.items[static_cast<std::size_t>(first + place)];
        const kit::Rect box = kit::choice_item(menu, place);
        look.shown.push_back(
            cut(building,
                kit::FontRole::regular,
                item.caption,
                box.width - metrics.choice_item_text_inset - metrics.list_row_inset)
        );
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
        return static_cast<int32_t>(lines.size()) * metrics.small_line;
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
    // One of a pair of choices: a filter chip, the marked one on the
    // list's selected face with the accent border.
    const auto choice = [&](kit::ControlId id,
                            std::string control_name,
                            const kit::Rect& rect,
                            std::string caption,
                            bool marked) {
        kit::ChipLook look;
        look.text =
            cut(building, kit::FontRole::small, caption, rect.width - 2 * metrics.chip_padding);
        look.state = kit::ChipState::filter;
        look.selected = marked;
        look.hovered = hovered(building, id);
        const std::size_t before = building.list.controls.size();
        kit::add_chip(
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
    const int32_t use_width =
        std::min(std::max(button_width(building, use_caption), int32_t{80}), inner / 2 + inner / 4);
    const int32_t cancel_width = std::max(button_width(building, cancel_caption), int32_t{80});
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
    if (building.library->chooser.open)
        list.tab_order = std::move(building.chooser_order);
    else if (building.library->key_use.open)
        list.tab_order = std::move(building.question_order);
    else
        list.tab_order = std::move(building.order);
    return list;
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
                words::filter_tag,
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
        for (std::size_t index = 0; index < tags.size(); ++index) {
            if (index < tag_shown.size() && tag_shown[index])
                continue;
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
    Building building = start(library, state, frame, fonts);
    const Window window = window_of(frame);
    const bool compact = frame.size_class == kit::SizeClass::compact;
    const bool large = frame.size_class == kit::SizeClass::large;
    if (compact) {
        add_library_header(
            building,
            window,
            shown(library.text, words::title_library),
            {},
            library.inputs.version_text
        );
    } else {
        add_library_header(
            building,
            window,
            shown(library.text, words::title_open_annihilation),
            shown(library.text, words::title_library),
            library.inputs.version_text
        );
    }
    const int32_t content_width = window.right - window.left;
    int32_t top = window.body_top + window.gap;
    const bool active = library.tab == Tab::active;
    const bool registries = library.tab == Tab::registries;
    const bool catalogue = !active && !registries;
    // The Updates tab's line, count and UPDATE ALL, takes a row over its list.
    const bool updates_line = library.tab == Tab::updates;
    const int32_t updates_row = window.button_height + window.gap;
    const auto body_of = [&](int32_t body_top) {
        return kit::Rect{
            window.left, body_top, content_width, window.footer_rule - window.gap - body_top
        };
    };
    if (compact) {
        add_tabs(building, {window.left, top, content_width, window.tab_height});
        top += window.tab_height + window.gap;
        if (active)
            add_active(building, body_of(top));
        else if (registries) {
            add_registry_bar(building, window, top);
            add_registries(building, body_of(top));
        } else {
            add_compact_search_row(
                building, {window.left, top, content_width, window.field_height}
            );
            top += window.field_height + window.gap;
            if (updates_line) {
                add_updates_line(building, {window.left, top, content_width, window.button_height});
                top += updates_row;
            }
            add_list(
                building,
                {window.left, top, content_width, window.footer_rule - window.gap - top},
                RowForm::compact
            );
        }
    } else {
        // Active and Registries draw no search, so the tabs take the whole
        // row. A catalogue tab keeps room for the search beside them while
        // every tab fits there; when they do not, as a wordier language's
        // names do, the tabs take the whole row and the search a row of its
        // own under them, at the same place on the right, so that no tab is
        // hidden beside it. The names stay whole; a strip they do not fit
        // even then scrolls.
        const int32_t beside_width = content_width - wide_search_width - window.padding;
        const bool search_beside = catalogue && tabs_fit(building, beside_width);
        const int32_t tab_width = search_beside ? beside_width : content_width;
        add_tabs(building, {window.left, top, tab_width, window.tab_height});
        if (search_beside)
            add_search(
                building,
                {window.right - wide_search_width,
                 top + (window.tab_height - window.field_height) / 2,
                 wide_search_width,
                 window.field_height}
            );
        top += window.tab_height + window.gap;
        if (catalogue && !search_beside) {
            add_search(
                building,
                {window.right - wide_search_width, top, wide_search_width, window.field_height}
            );
            top += window.field_height + window.gap;
        }
        if (active)
            add_active(building, body_of(top));
        else if (registries) {
            add_registry_bar(building, window, top);
            add_registries(building, body_of(top));
        } else {
            if (!large) {
                add_chip_row(building, {window.left, top, content_width, window.pill_height});
                top += window.pill_height + window.gap;
            }
            const int32_t panes_height = window.footer_rule - window.gap - top;
            int32_t left = window.left;
            if (large) {
                add_filter_pane(building, {left, top, filter_pane_width, panes_height});
                left += filter_pane_width + window.padding;
            }
            const int32_t list_width = large ? large_list_width : regular_list_width;
            int32_t list_top = top;
            if (updates_line) {
                add_updates_line(building, {left, top, list_width, window.button_height});
                list_top += updates_row;
            }
            add_list(
                building,
                {left, list_top, list_width, window.footer_rule - window.gap - list_top},
                (large || library.tab == Tab::brains || library.tab == Tab::keys) ? RowForm::large
                                                                                  : RowForm::regular
            );
            left += list_width + window.padding;
            add_details_pane(building, {left, top, window.right - left, panes_height});
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
        add_library_header(building, window, shown(library.text, words::title_library), {}, {});
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
            shown(library.text, words::title_library),
            shown(library.text, words::tab_mutators),
            library.inputs.version_text
        );
    } else {
        add_library_header(
            building, window, entry_name(*entry), {}, entry_version(library, *entry)
        );
    }

    // The body scrolls between the header and the footer.
    const int32_t content_width = window.right - window.left;
    const int32_t body_top = window.body_top + window.gap;
    const kit::Rect view{
        window.left, body_top, content_width, window.footer_rule - window.gap - body_top
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
            top += metrics.regular_line;
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
        top += metrics.paragraph_gap;
        if (const std::string_view about = entry_summary(*entry); !about.empty()) {
            const std::vector<std::string> summary =
                wrapped(into, kit::FontRole::small, about, width, compact_summary_lines);
            top = add_lines(into, summary, view.x, top, width, kit::colour::list_text, view) +
                  metrics.paragraph_gap;
        }
        const int32_t tags_bottom =
            add_detail_tags(into, *entry, view.x, top, width, view, controls::details_group);
        if (tags_bottom != top)
            top = tags_bottom + metrics.paragraph_gap;
        top = add_map_previews(into, *entry, view.x, top, width, view);
        top = add_key_picture(into, *entry, view.x, top, width, view);
        top = add_facts(into, *entry, {view.x, top, width, 0}, false, compact_fact_lines, view) +
              metrics.paragraph_gap;
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
        left += width_points + metrics.button_gap;
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
