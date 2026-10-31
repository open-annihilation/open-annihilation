// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutator picker's layout: a modal kit window at the size class's dialog
// size, the filters and the search, the On rows numbered above the Off rows,
// the selected mutator's details, and the footer. Which rows show, their
// order, their lines and what is locked come from the model. Nothing here
// takes a pointer, a key or a wheel.
#include "oa/ui/mutator_picker/screen.hpp"

#include "internal.hpp"
#include "screen_controls.hpp"

#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/rows.hpp"
#include "oa/ui/kit/tag_icon.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/library/screen.hpp"
#include "oa/ui/library/text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::mutator_picker {
namespace {

/// The accent line drawn across the list at a drag's target, in points.
constexpr int32_t drag_line = 2;

/// How much of a row is added. A drag keeps the controls on the well and
/// draws the moving row as items only.
enum class RowPaint : uint8_t {
    both,     ///< the items and the controls
    controls, ///< the controls, on the well
    items,    ///< the items, with no control
};

/// The window's rows and columns at a size class.
struct Window {
    int32_t width{};          ///< the window's width, in points
    int32_t height{};         ///< the window's height, in points
    int32_t padding{};        ///< Kit 2's window padding, as Settings keeps it
    int32_t left{};           ///< the content's left column
    int32_t right{};          ///< the column after the content's last
    int32_t header_height{};  ///< the header's band: Kit 2's window header, Compact's at Compact
    int32_t body_top{};       ///< the first row under the header's rule
    int32_t footer_rule{};    ///< the row of the line over the footer
    int32_t footer_top{};     ///< the footer's first row
    int32_t footer_height{};  ///< the footer's rows under its line: Kit 2's window footer
    int32_t button_top{};     ///< the footer's buttons' top row
    int32_t control_height{}; ///< Kit 2's control height: the buttons, the fields and the strips
    int32_t gap{};            ///< the rows between stacked parts
    int32_t bottom_gap{};     ///< the rows between the panes and the footer's rule: a space
};

/// One item of an open drop-down.
struct MenuEntry {
    std::string word{};    ///< the automation word
    std::string caption{}; ///< the words shown
};

/// A drop-down whose menu is open, drawn over everything else.
struct OpenMenu {
    kit::Rect field{};              ///< its field
    std::vector<MenuEntry> items{}; ///< its items, in order
    int32_t chosen{-1};             ///< the item shown as chosen
    std::string name{};             ///< the field's control's name
};

/// What a layout is built into.
struct Building {
    const Picker* picker{};                   ///< the model
    const ScreenState* state{};               ///< the screen's state
    const kit::Fonts* fonts{};                ///< the fonts texts are measured in
    const kit::Metrics* metrics{};            ///< the class's metrics
    kit::SizeClass size_class{};              ///< the class
    kit::DisplayList list{};                  ///< the items; controls move to the groups
    std::vector<kit::Control> menu{};         ///< an open drop-down's items, tried first
    std::vector<kit::Control> top{};          ///< the filters, the search and the close mark
    std::vector<kit::Control> rows{};         ///< the list's rows, grips and switches
    std::vector<kit::Control> panes{};        ///< the list and its scroll bar
    std::vector<kit::Control> detail{};       ///< the details' controls
    std::vector<kit::Control> footer{};       ///< the footer's buttons and labels
    std::vector<kit::ControlId> order{};      ///< Tab's order
    std::vector<std::string> names{};         ///< the names given so far
    std::vector<kit::ControlId> tag_pills{};  ///< the tag pills, in the order shown
    std::vector<kit::ControlId> option_ids{}; ///< the selected mutator's option controls
    std::optional<OpenMenu> menu_open{};      ///< the open drop-down
    int32_t number_width{0};                  ///< the On number column for this layout
    int32_t row_switch_width{0};              ///< every row switch, one column
};

/// Returns the window's rows and columns for a frame, in the sizes Settings
/// gives its window: Kit 2's window header at Regular and Large and
/// Compact's band at Compact, Kit 2's window padding, and Kit 2's window
/// footer with its controls in the middle. Stacked parts sit a space apart
/// at Compact and Kit 2's row gap apart at Regular and Large, as the
/// Library's do.
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
    window.padding = metrics.window_padding;
    window.left = metrics.edge + window.padding;
    window.right = size.x - metrics.edge - window.padding;
    window.header_height = compact ? metrics.header_height : metrics.window_header_height;
    window.body_top = metrics.edge + window.header_height + 1;
    window.control_height = metrics.control_height;
    window.footer_height = metrics.window_footer_height;
    window.footer_rule = size.y - metrics.edge - window.footer_height - 1;
    window.footer_top = window.footer_rule + 1;
    window.button_top = window.footer_top + (window.footer_height - window.control_height) / 2;
    window.gap = compact ? metrics.space : metrics.row_gap;
    window.bottom_gap = metrics.space;
    return window;
}

/// Returns a name no control has yet, the name itself or with "-2", "-3" and
/// so on after it, and records it.
///
/// @param[in,out] building the layout
/// @param name the name wanted
/// @return the name given
std::string unique_name(Building& building, std::string name) {
    std::string given = name;
    for (int32_t count = 2;
         std::find(building.names.begin(), building.names.end(), given) != building.names.end();
         ++count)
        given = name + "-" + std::to_string(count);
    building.names.push_back(given);
    return given;
}

/// Records a name that is already the one a control will use.
///
/// @param[in,out] building the layout
/// @param name the name
void keep_name(Building& building, const std::string& name) {
    if (std::find(building.names.begin(), building.names.end(), name) == building.names.end())
        building.names.push_back(name);
}

/// Returns a word no earlier word matches, appending "-2", "-3" and so on.
///
/// @param[in,out] used the words already given
/// @param word the word wanted
/// @return the word given
std::string unique_word(std::vector<std::string>& used, std::string word) {
    std::string given = word;
    for (int32_t count = 2; std::find(used.begin(), used.end(), given) != used.end(); ++count)
        given = word + "-" + std::to_string(count);
    used.push_back(given);
    return given;
}

/// Appends an item.
///
/// @param[in,out] list the display list
/// @param role what it draws
/// @param rect where
/// @param clip what drawing may touch; empty for the window
/// @return the item
kit::Item&
add_item(kit::DisplayList& list, kit::Role role, const kit::Rect& rect, const kit::Rect& clip) {
    kit::Item& added = list.items.emplace_back();
    added.role = role;
    added.rect = rect;
    added.clip = clip;
    return added;
}

/// Moves the controls a kit function appended into a group.
///
/// @param[in,out] list the display list
/// @param from how many controls the list held before
/// @param[in,out] group the group they go to
void move_controls(kit::DisplayList& list, std::size_t from, std::vector<kit::Control>& group) {
    for (std::size_t at = from; at < list.controls.size(); ++at)
        group.push_back(std::move(list.controls[at]));
    list.controls.resize(from);
}

/// Returns a text cut with the kit's ellipsis, or empty when it cannot fit.
///
/// @param building the layout
/// @param role the font
/// @param piece the text, in UTF-8
/// @param width the room, in points
/// @return the text, whole when it fits
std::string
cut(const Building& building, kit::FontRole role, std::string_view piece, int32_t width) {
    if (width <= 0)
        return {};
    const std::string shortened = kit::cut_to_width(*building.fonts, role, piece, width);
    if (kit::text_width(*building.fonts, role, shortened) > width)
        return {};
    return shortened;
}

/// Returns a tracked text cut until its tracked width fits.
///
/// @param building the layout
/// @param role the font
/// @param piece the text, in UTF-8
/// @param width the room, in points
/// @param tracking the extra columns after each character but the last
/// @return the text, whole when it fits
std::string cut_tracked(
    const Building& building,
    kit::FontRole role,
    std::string_view piece,
    int32_t width,
    int32_t tracking
) {
    if (width <= 0)
        return {};
    if (kit::tracked_width(*building.fonts, role, piece, tracking) <= width)
        return std::string(piece);
    std::string left(piece);
    while (!left.empty()) {
        std::size_t end = left.size();
        do {
            --end;
        } while (end > 0 && kit::continuation_byte(left[end]));
        left.resize(end);
        const std::string candidate = kit::cut_to_width(*building.fonts, role, left, width);
        if (candidate.empty())
            return {};
        if (kit::tracked_width(*building.fonts, role, candidate, tracking) <= width)
            return candidate;
    }
    return {};
}

/// Breaks a text into lines no wider than a width. The last kept line ends
/// with the kit's ellipsis when a limit drops lines.
///
/// @param building the layout
/// @param role the font
/// @param piece the text, in UTF-8
/// @param width the room, in points
/// @param most_lines the most lines kept; 0 keeps every line
/// @return the lines
std::vector<std::string> wrap_lines(
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
        rules.shorten_last = [fonts, role, room](std::string_view last) {
            return kit::cut_to_width(*fonts, role, last, room);
        };
    }
    std::vector<std::string> lines = kit::wrap(piece, std::max(width, int32_t{1}), measure, rules);
    for (std::string& line : lines) {
        if (kit::text_width(*fonts, role, line) > std::max(width, int32_t{0}))
            line = cut(building, role, line, width);
    }
    return lines;
}

/// Returns a text with its line breaks turned into spaces.
///
/// @param text the text
/// @return the one line
std::string one_line(std::string_view text) {
    std::string flat;
    flat.reserve(text.size());
    for (const char byte : text) {
        if (byte == '\n' || byte == '\r') {
            if (!flat.empty() && flat.back() != ' ')
                flat.push_back(' ');
        } else {
            flat.push_back(byte);
        }
    }
    return flat;
}

/// Joins lines with single spaces.
///
/// @param lines the lines
/// @return the text
std::string joined(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) {
        if (!text.empty())
            text.push_back(' ');
        text += line;
    }
    return text;
}

/// Returns the colour a row's line is drawn in.
///
/// @param tone the line's tone
/// @return the colour
kit::Colour tone_colour(Tone tone) noexcept {
    if (tone == Tone::warn)
        return kit::colour::lock;
    if (tone == Tone::bad)
        return kit::colour::danger;
    return kit::colour::hint;
}

/// Returns a badge's picture. The picture points into the badge.
///
/// @param badge the badge; may be null
/// @return the picture; empty without one
oa::ui::frontend_renderer::RgbaPicture picture_of(const library::Badge* badge) {
    oa::ui::frontend_renderer::RgbaPicture picture;
    if (badge == nullptr)
        return picture;
    picture.width = badge->width;
    picture.height = badge->height;
    picture.pixels = std::span<const uint8_t>(badge->rgba);
    return picture;
}

/// Appends a package badge: the picture, else the first tag's icon, else the letters.
///
/// @param[in,out] building the layout
/// @param mutator the mutator
/// @param rect the badge's square
/// @param clip what drawing may touch
void add_mutator_badge(
    Building& building, const Mutator& mutator, const kit::Rect& rect, const kit::Rect& clip
) {
    kit::BadgeLook look;
    const oa::ui::frontend_renderer::RgbaPicture picture = picture_of(mutator.badge);
    if (oa::ui::frontend_renderer::picture_drawable(picture)) {
        look.picture = picture;
    } else {
        for (const std::string& tag : mutator.tags) {
            const std::optional<kit::TagIcon> icon = kit::tag_icon_of(tag);
            if (icon) {
                look.tag = icon;
                break;
            }
        }
        if (!look.tag)
            look.letters = library::badge_letters(detail::display_name(mutator));
    }
    kit::add_badge(building.list, rect, look);
    if (!building.list.items.empty())
        building.list.items.back().clip = clip;
}

/// Returns a button's width as Settings sizes its buttons (Cancel, OK):
/// its caption in the regular font with two spaces either side, and at
/// least twice the control height.
///
/// @param building the layout
/// @param caption the words, already looked up
/// @return the width, in points
int32_t button_span(const Building& building, std::string_view caption) {
    const kit::Metrics& metrics = *building.metrics;
    const int32_t words = kit::text_width(*building.fonts, kit::FontRole::regular, caption);
    return std::max(words + 4 * metrics.space, 2 * metrics.control_height);
}

/// The columns Settings keeps between two buttons side by side: two spaces.
/// The filter row keeps them between its controls too.
///
/// @param metrics the class's metrics
/// @return the columns, in points
int32_t button_spacing(const kit::Metrics& metrics) {
    return 2 * metrics.space;
}

/// Returns a strip's segment width: its widest caption in the small font
/// and the room a Settings button keeps round its words, two spaces either
/// side.
///
/// @param building the layout
/// @param widest the widest caption, in points
/// @return the width, in points
int32_t segment_width(const Building& building, int32_t widest) {
    return widest + 4 * building.metrics->space;
}

/// Returns the setup's word, already looked up.
///
/// @param hooks how texts are looked up
/// @param setup which setup opened the picker
/// @return the word
std::string setup_word(const library::TextHooks& hooks, Setup setup) {
    std::string_view english = words::setup_skirmish;
    if (setup == Setup::multiplayer)
        english = words::setup_battle;
    else if (setup == Setup::campaign)
        english = words::setup_campaign;
    return std::string(library::shown(hooks, english));
}

/// Returns a filter's word, already looked up.
///
/// @param hooks how texts are looked up
/// @param which the filter
/// @return the word
std::string filter_word(const library::TextHooks& hooks, Filter which) {
    std::string_view english = words::filter_all;
    if (which == Filter::installed)
        english = words::filter_installed;
    else if (which == Filter::downloads)
        english = words::filter_downloads;
    return std::string(library::shown(hooks, english));
}

/// Returns a filter's pill, the word and how many it keeps.
///
/// @param building the layout
/// @param which the filter
/// @return the text
std::string filter_caption(const Building& building, Filter which) {
    const library::TextHooks& hooks = building.picker->inputs.text;
    return library::filled(
        hooks,
        words::filter_with_count,
        {{"filter", filter_word(hooks, which)},
         {"count", std::to_string(filter_count(*building.picker, which))}}
    );
}

/// Returns a tag's compact line, its label and how many the filter keeps.
///
/// @param building the layout
/// @param tag the catalogue id
/// @return the text
std::string tag_caption(const Building& building, std::string_view tag) {
    const library::TextHooks& hooks = building.picker->inputs.text;
    const std::string label(library::tag_label(tag, hooks));
    return library::filled(
        hooks,
        words::tag_with_count,
        {{"tag", label}, {"count", std::to_string(tag_count(*building.picker, tag))}}
    );
}

/// The On number column: the class's least width, or the widest On number shown.
///
/// @param building the layout
/// @param shown the rows
/// @return the width, in points
int32_t measured_number_column(const Building& building, const std::vector<Row>& shown) {
    const bool compact = building.size_class == kit::SizeClass::compact;
    int32_t width = compact ? compact_number_column : number_column;
    for (const Row& row : shown) {
        if (!row.on)
            continue;
        width = std::max(
            width,
            kit::text_width(*building.fonts, kit::FontRole::small, std::to_string(row.number))
        );
    }
    return width;
}

/// Every row switch's width, from the shared OFF and ON, so they share one column.
///
/// @param building the layout
/// @return the width, in points
int32_t measured_switch_width(const Building& building) {
    const library::TextHooks& hooks = building.picker->inputs.text;
    const int32_t off_width = kit::text_width(
        *building.fonts, kit::FontRole::small, library::shown(hooks, words::switch_off)
    );
    const int32_t on_width = kit::text_width(
        *building.fonts, kit::FontRole::small, library::shown(hooks, words::switch_on)
    );
    return kit::switch_width_for(std::max(off_width, on_width));
}

/// Appends a text in a box.
///
/// @param[in,out] building the layout
/// @param role the font
/// @param drawn the text, as drawn
/// @param box the box
/// @param align where it sits along the box
/// @param ink its colour
/// @param clip what drawing may touch
void add_text(
    Building& building,
    kit::FontRole role,
    std::string drawn,
    const kit::Rect& box,
    kit::Align align,
    kit::Colour ink,
    const kit::Rect& clip
) {
    if (drawn.empty() || box.width <= 0 || box.height <= 0)
        return;
    kit::Item& added = add_item(building.list, kit::Role::text, box, clip);
    added.text = std::move(drawn);
    added.font = role;
    added.align = align;
    added.colour = ink;
}

/// Appends a label that takes no press and no Tab.
///
/// @param[in,out] group the group it goes to
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

/// Adds a control to Tab's order when it takes the focus. One clipped wholly
/// out of its scroll area does not, except a details control: the keys keep
/// it and scroll the details until it shows.
///
/// @param[in,out] building the layout
/// @param[in,out] control the control
void tab_stop(Building& building, kit::Control& control) {
    if (control.clip.width > 0 && control.clip.height > 0) {
        const kit::Rect shown = kit::intersect(control.rect, control.clip);
        if ((shown.width <= 0 || shown.height <= 0) && control.group != controls::details_group)
            control.focusable = false;
    }
    if (control.enabled && control.focusable)
        building.order.push_back(control.id);
}

/// Tells whether the pointer is over an enabled control.
///
/// @param building the layout
/// @param id the control
/// @param enabled the control can act
/// @return true while it is hovered
bool hovered(const Building& building, kit::ControlId id, bool enabled) noexcept {
    return enabled && building.state->interaction.hovered == id;
}

/// Tells whether a press is held on an enabled control with the pointer over it.
///
/// @param building the layout
/// @param id the control
/// @param enabled the control can act
/// @return true while it is held
bool held(const Building& building, kit::ControlId id, bool enabled) noexcept {
    return enabled && building.state->interaction.hovered == id &&
           building.state->interaction.pressed == id;
}

/// Tells whether the keyboard focus is shown on a control.
///
/// @param building the layout
/// @param id the control
/// @return true while the focus is shown there
bool focused_on(const Building& building, kit::ControlId id) noexcept {
    return building.state->interaction.focus_shown && building.state->interaction.focused == id;
}

/// Returns the control of a number, searching every group.
///
/// @param building the layout
/// @param id the control
/// @return the control, or null
kit::Control* find_control(Building& building, kit::ControlId id) {
    for (std::vector<kit::Control>* group :
         {&building.top, &building.rows, &building.panes, &building.detail, &building.footer}) {
        for (kit::Control& control : *group) {
            if (control.id == id)
                return &control;
        }
    }
    return nullptr;
}

/// Appends the header as Settings and the Library draw theirs: "OPEN
/// ANNIHILATION" then "MUTATORS" in the header's second colour, the setup
/// and the mod at the right, and room for the close mark. The title is cut
/// first when the window is too narrow for both words.
///
/// The kit's header keeps Compact's places from the window's edges at every
/// class; its band is the window's header height.
///
/// @param[in,out] building the layout
/// @param window the window
void add_header(Building& building, const Window& window) {
    const kit::Metrics& compact = kit::compact_metrics;
    const library::TextHooks& hooks = building.picker->inputs.text;
    const int32_t tracking = compact.heading_tracking;
    const int32_t right_inset = compact.button_height + compact.header_gap;
    const int32_t title_left = compact.padding + compact.mark_side + compact.header_gap;
    const int32_t right_edge = window.width - compact.edge - compact.padding - right_inset;

    std::string second(library::shown(hooks, words::title_mutators_header));
    second = cut_tracked(
        building,
        kit::FontRole::regular,
        second,
        std::max(right_edge - title_left, int32_t{0}),
        tracking
    );
    const int32_t second_width =
        kit::tracked_width(*building.fonts, kit::FontRole::regular, second, tracking);
    const int32_t most_for_title = std::max(
        right_edge - title_left - compact.header_gap - second_width - compact.header_gap, int32_t{0}
    );
    std::string title(library::shown(hooks, words::title_open_annihilation));
    title = cut_tracked(building, kit::FontRole::regular, title, most_for_title, tracking);
    const int32_t title_width =
        kit::tracked_width(*building.fonts, kit::FontRole::regular, title, tracking);
    const int32_t version_room = std::max(
        right_edge -
            (title_left + title_width + compact.header_gap + second_width + compact.header_gap),
        int32_t{0}
    );
    const std::string setup = setup_word(hooks, building.picker->inputs.setup);
    const std::string mod = building.picker->inputs.mod_name.empty()
                                ? std::string(library::shown(hooks, words::no_mod))
                                : building.picker->inputs.mod_name;
    std::string context =
        library::filled(hooks, words::context_setup, {{"setup", setup}, {"mod", mod}});
    context = cut(building, kit::FontRole::small, context, version_room);

    kit::HeaderLook look;
    look.width = window.width;
    look.height = window.header_height;
    look.tracking = tracking;
    look.right_inset = right_inset;
    look.mark = true;
    look.title = std::move(title);
    look.title_width = title_width;
    look.second = std::move(second);
    look.second_width = second_width;
    look.version = std::move(context);
    look.version_width = kit::text_width(*building.fonts, kit::FontRole::small, look.version);
    const int32_t header_rule = compact.edge + window.header_height;
    add_item(building.list, kit::Role::header, {0, 0, window.width, header_rule + 1}, {}).look =
        std::move(look);
}

/// Appends the header's close mark, in the middle of the header's band, where
/// Settings and the Library keep theirs. Tab's order gains it last.
///
/// @param[in,out] building the layout
/// @param window the window
void add_close_mark(Building& building, const Window& window) {
    const kit::Metrics& compact = kit::compact_metrics;
    const int32_t side = compact.button_height;
    const kit::Rect rect{
        window.width - compact.edge - compact.padding - side,
        compact.edge + (window.header_height - side) / 2,
        side,
        side,
    };
    const bool enabled = true;
    kit::ButtonLook look;
    look.style = kit::ButtonStyle::quiet;
    look.mark = kit::ButtonMark::close;
    look.hovered = hovered(building, controls::close_mark, enabled);
    look.held = held(building, controls::close_mark, enabled);
    look.enabled = enabled;
    const std::size_t before = building.list.controls.size();
    const std::string name(name_close);
    keep_name(building, name);
    kit::add_button(building.list, rect, look, controls::close_mark, name);
    move_controls(building.list, before, building.top);
    building.top.back().text =
        std::string(library::shown(building.picker->inputs.text, words::close_button));
}

/// Returns a filter pill's width at Kit 2's filter chip height: the kit's
/// pill widened by the height it gains, as the key editor's are, so its
/// words stay clear of the rounder ends. A pill with a tag icon is widened
/// only by how much further in the taller pill sets its icon, so the icon
/// stands as far in from the round end as from the top and the words keep
/// the kit's room beside it.
///
/// @param building the layout
/// @param look the pill
/// @return the width, in points
int32_t filter_pill_width(const Building& building, const kit::PillLook& look) {
    const int32_t height = building.metrics->filter_chip_height;
    const int32_t taller = std::max(height - kit::pill_height(), int32_t{0});
    if (!look.tag.has_value())
        return kit::pill_width(*building.fonts, look) + taller;
    const auto icon_inset = [](int32_t pill) {
        return std::max((pill - (kit::pill_height() - 2)) / 2, int32_t{1});
    };
    return kit::pill_width(*building.fonts, look) + icon_inset(height) -
           icon_inset(kit::pill_height());
}

/// Appends one filter pill.
///
/// @param[in,out] building the layout
/// @param id its number
/// @param name its name
/// @param rect the pill
/// @param caption its words
/// @param active it is the chosen filter
void add_filter_pill(
    Building& building,
    kit::ControlId id,
    std::string name,
    const kit::Rect& rect,
    std::string caption,
    bool active
) {
    const bool enabled = true;
    kit::PillLook look;
    look.text = std::move(caption);
    look.state = active ? kit::PillState::active : kit::PillState::inactive;
    look.hovered = hovered(building, id, enabled);
    look.focused = focused_on(building, id);
    const std::size_t before = building.list.controls.size();
    keep_name(building, name);
    kit::add_pill(building.list, rect, look, id, std::move(name));
    move_controls(building.list, before, building.top);
}

/// Appends the filters and the search on a row of Kit 2's control height,
/// a gap under the header.
///
/// Regular and Large: the three pills and the tags that fit, Kit 2's filter
/// chips as the key editor has them, then Tags, Settings' drop-down, and the
/// search pinned at the right, both at the control height. Compact: the
/// drop-down and the search across the rest.
///
/// @param[in,out] building the layout
/// @param window the window
/// @param tags the tags the model shows, in its order
void add_filters(Building& building, const Window& window, const std::vector<std::string>& tags) {
    const kit::Metrics& metrics = *building.metrics;
    const library::TextHooks& hooks = building.picker->inputs.text;
    const bool compact = building.size_class == kit::SizeClass::compact;
    const int32_t row = window.body_top + window.gap;
    const int32_t row_height = window.control_height;
    const int32_t gap = button_spacing(metrics);
    const Picker& picker = *building.picker;

    if (compact) {
        const int32_t field_w = compact_filter_width;
        const kit::Rect field{window.left, row, field_w, row_height};
        const int32_t search_x = window.left + field_w + gap;
        const int32_t search_w = std::max(window.right - search_x, int32_t{0});
        int32_t chosen = 0;
        std::string shown_text;
        if (picker.filter == Filter::installed)
            chosen = 1;
        else if (picker.filter == Filter::downloads)
            chosen = 2;
        shown_text = filter_caption(building, picker.filter);
        std::vector<std::string> part_words;
        std::vector<MenuEntry> entries;
        const Filter filters[] = {Filter::all, Filter::installed, Filter::downloads};
        const char* words_of[] = {"all", "installed", "downloads"};
        for (int32_t place = 0; place < 3; ++place) {
            MenuEntry entry;
            entry.word = words_of[place];
            entry.caption = filter_caption(building, filters[place]);
            part_words.push_back(entry.word);
            entries.push_back(std::move(entry));
        }
        std::vector<std::string> used = part_words;
        for (std::size_t place = 0; place < tags.size(); ++place) {
            MenuEntry entry;
            entry.word = unique_word(used, "tag-" + controls::name_word(tags[place]));
            entry.caption = tag_caption(building, tags[place]);
            if (picker.tag == tags[place]) {
                chosen = 3 + static_cast<int32_t>(place);
                shown_text = entry.caption;
            }
            part_words.push_back(entry.word);
            entries.push_back(std::move(entry));
        }
        const int32_t text_room =
            field.width - metrics.choice_text_inset - metrics.choice_arrow_room;
        kit::ChoiceLook look;
        look.text = cut(building, kit::FontRole::regular, shown_text, text_room);
        look.hovered = hovered(building, controls::filter, true);
        look.open = building.state->menu == Menu::filter;
        const std::size_t before = building.list.controls.size();
        const std::string name(name_filter);
        keep_name(building, name);
        kit::add_choice(building.list, field, look, controls::filter, name);
        move_controls(building.list, before, building.top);
        building.top.back().parts = std::move(part_words);
        if (building.state->menu == Menu::filter) {
            OpenMenu menu_open;
            menu_open.field = field;
            menu_open.items = std::move(entries);
            menu_open.chosen = chosen;
            menu_open.name = name;
            building.menu_open = std::move(menu_open);
        }
        kit::SearchLook search;
        search.magnifier = true;
        search.field = building.state->search;
        search.focused = focused_on(building, controls::search);
        search.hovered = hovered(building, controls::search, true);
        const kit::Rect search_rect{search_x, row, search_w, row_height};
        const std::string placeholder(library::shown(hooks, words::search_mutators));
        const int32_t placeholder_room = kit::field_text_rect(search_rect, search).width;
        search.placeholder = cut(building, kit::FontRole::regular, placeholder, placeholder_room);
        const std::size_t search_before = building.list.controls.size();
        const std::string search_name(name_search);
        keep_name(building, search_name);
        kit::add_search(building.list, search_rect, search, controls::search, search_name);
        move_controls(building.list, search_before, building.top);
        return;
    }

    int32_t search_w =
        building.size_class == kit::SizeClass::large ? large_search_width : regular_search_width;
    const int32_t content = std::max(window.right - window.left, int32_t{0});
    if (search_w > content)
        search_w = content;
    const int32_t search_x = window.right - search_w;
    const std::string tags_word(library::shown(hooks, words::tags_menu));
    // Tags is at least as wide as Settings' narrowest button, twice its height.
    const int32_t tags_w = std::max(
        kit::text_width(*building.fonts, kit::FontRole::regular, tags_word) +
            metrics.choice_text_inset + metrics.choice_arrow_room,
        2 * row_height
    );
    const int32_t tags_x = search_x - gap - tags_w;
    const int32_t pill_limit = tags_x - gap;
    const int32_t pill_h = metrics.filter_chip_height;
    const int32_t pill_gap = metrics.chip_gap;
    const int32_t pill_y = row + (row_height - pill_h) / 2;

    int32_t cursor = window.left;
    const Filter filters[] = {Filter::all, Filter::installed, Filter::downloads};
    const kit::ControlId filter_ids[] = {
        controls::filter_all, controls::filter_installed, controls::filter_downloads
    };
    const std::string filter_names[] = {
        std::string(name_filter_all),
        std::string(name_filter_installed),
        std::string(name_filter_downloads),
    };
    for (int32_t place = 0; place < 3; ++place) {
        kit::PillLook measure;
        measure.text = filter_caption(building, filters[place]);
        const int32_t width = filter_pill_width(building, measure);
        add_filter_pill(
            building,
            filter_ids[place],
            filter_names[place],
            {cursor, pill_y, width, pill_h},
            measure.text,
            picker.filter == filters[place]
        );
        cursor += width + pill_gap;
    }

    std::vector<std::string> used_words;
    std::vector<std::string> overflow_words;
    std::vector<MenuEntry> overflow;
    int32_t overflow_chosen = -1;
    bool stopped = false;
    for (std::size_t place = 0; place < tags.size(); ++place) {
        const std::string word = unique_word(used_words, controls::name_word(tags[place]));
        kit::PillLook measure;
        measure.text = std::string(library::tag_label(tags[place], hooks));
        measure.tag = kit::tag_icon_of(tags[place]);
        const int32_t width = filter_pill_width(building, measure);
        const bool fits = !stopped && cursor + width <= pill_limit;
        if (fits) {
            const kit::ControlId id =
                controls::first_tag_pill + static_cast<kit::ControlId>(static_cast<int32_t>(place));
            const bool enabled = true;
            measure.state =
                picker.tag == tags[place] ? kit::PillState::active : kit::PillState::inactive;
            measure.hovered = hovered(building, id, enabled);
            measure.focused = focused_on(building, id);
            const std::string name = std::string(name_tags) + "." + word;
            const std::size_t before = building.list.controls.size();
            keep_name(building, name);
            kit::add_pill(building.list, {cursor, pill_y, width, pill_h}, measure, id, name);
            move_controls(building.list, before, building.top);
            building.tag_pills.push_back(id);
            cursor += width + pill_gap;
        } else {
            stopped = true;
            MenuEntry entry;
            entry.word = word;
            entry.caption = measure.text;
            if (picker.tag == tags[place])
                overflow_chosen = static_cast<int32_t>(overflow.size());
            overflow_words.push_back(word);
            overflow.push_back(std::move(entry));
        }
    }

    const bool tags_on = !overflow.empty();
    const kit::Rect tags_rect{tags_x, row, std::max(tags_w, int32_t{0}), row_height};
    kit::ChoiceLook tags_look;
    tags_look.text =
        cut(building,
            kit::FontRole::regular,
            tags_word,
            tags_rect.width - metrics.choice_text_inset - metrics.choice_arrow_room);
    tags_look.hovered = hovered(building, controls::tags, tags_on);
    tags_look.open = tags_on && building.state->menu == Menu::tags;
    const std::size_t tags_before = building.list.controls.size();
    const std::string tags_name(name_tags);
    keep_name(building, tags_name);
    kit::add_choice(building.list, tags_rect, tags_look, controls::tags, tags_name);
    move_controls(building.list, tags_before, building.top);
    building.top.back().parts = overflow_words;
    building.top.back().checked = overflow_chosen >= 0;
    if (!tags_on) {
        building.top.back().enabled = false;
        building.top.back().focusable = false;
    }
    if (tags_look.open) {
        OpenMenu menu_open;
        menu_open.field = tags_rect;
        menu_open.items = std::move(overflow);
        menu_open.chosen = overflow_chosen;
        menu_open.name = tags_name;
        building.menu_open = std::move(menu_open);
    }

    kit::SearchLook search;
    search.magnifier = true;
    search.field = building.state->search;
    search.focused = focused_on(building, controls::search);
    search.hovered = hovered(building, controls::search, true);
    const kit::Rect search_rect{search_x, row, search_w, row_height};
    const std::string placeholder(library::shown(hooks, words::search_mutators));
    search.placeholder =
        cut(building,
            kit::FontRole::regular,
            placeholder,
            kit::field_text_rect(search_rect, search).width);
    const std::size_t search_before = building.list.controls.size();
    const std::string search_name(name_search);
    keep_name(building, search_name);
    kit::add_search(building.list, search_rect, search, controls::search, search_name);
    move_controls(building.list, search_before, building.top);
}

/// One piece of the list's scrolling content.
struct ListPiece {
    /// What the piece is.
    enum class Kind : uint8_t {
        hint,    ///< the hint under the filters
        heading, ///< an On or Off heading
        row,     ///< one mutator
        empty,   ///< the sentence when a filter hides every Off row
    };
    Kind kind{Kind::row};             ///< what it is
    int32_t y{};                      ///< the top, in content points
    int32_t height{};                 ///< the height, in points
    bool on_heading{false};           ///< a heading: the On heading when true
    std::size_t row{0};               ///< a row's place in the shown rows
    std::vector<std::string> lines{}; ///< a hint's or an empty sentence's lines
    std::string text{};               ///< a heading's words
};

/// The list's content, measured and not yet drawn.
struct ListPlan {
    std::vector<ListPiece> pieces{}; ///< the hint, the headings and the rows
    int32_t height{0};               ///< the content's height, in points
    int32_t width{0};                ///< the content's width, in points
};

/// Measures the list's hint, headings and rows at a width. Adds nothing.
///
/// @param building the layout
/// @param shown_rows the rows the model gives, in order
/// @param hint_text the hint; empty draws none
/// @param width the content's width, in points
/// @return the plan
ListPlan plan_list(
    const Building& building,
    const std::vector<Row>& shown_rows,
    std::string_view hint_text,
    int32_t width
) {
    const kit::Metrics& metrics = *building.metrics;
    const bool compact = building.size_class == kit::SizeClass::compact;
    const int32_t row_height = compact                                        ? compact_row_height
                               : building.size_class == kit::SizeClass::large ? large_row_height
                                                                              : regular_row_height;
    ListPlan plan;
    plan.width = std::max(width, int32_t{0});
    int32_t y = 0;
    if (!hint_text.empty()) {
        ListPiece piece;
        piece.kind = ListPiece::Kind::hint;
        piece.y = y;
        piece.lines =
            wrap_lines(building, kit::FontRole::small, hint_text, plan.width, compact ? 1 : 2);
        piece.height =
            std::max(static_cast<int32_t>(piece.lines.size()), int32_t{1}) * metrics.small_line;
        y += piece.height;
        plan.pieces.push_back(std::move(piece));
    }
    bool any_on = false;
    bool any_off = false;
    for (const Row& row : shown_rows) {
        if (row.on)
            any_on = true;
        else
            any_off = true;
    }
    const auto heading = [&](bool on_heading) {
        if (y > 0)
            y += metrics.paragraph_gap;
        ListPiece piece;
        piece.kind = ListPiece::Kind::heading;
        piece.y = y;
        piece.height = metrics.small_line;
        piece.on_heading = on_heading;
        const std::string_view english = on_heading ? words::heading_on : words::heading_off;
        const std::string looked(library::shown(building.picker->inputs.text, english));
        piece.text = cut_tracked(
            building, kit::FontRole::small, looked, plan.width, metrics.heading_tracking
        );
        y += piece.height;
        plan.pieces.push_back(std::move(piece));
    };
    if (any_on)
        heading(true);
    for (std::size_t place = 0; place < shown_rows.size(); ++place) {
        if (!shown_rows[place].on)
            continue;
        ListPiece piece;
        piece.kind = ListPiece::Kind::row;
        piece.y = y;
        piece.height = row_height;
        piece.row = place;
        y += row_height;
        plan.pieces.push_back(std::move(piece));
    }
    const Picker& picker = *building.picker;
    const bool narrowed =
        picker.filter != Filter::all || !picker.tag.empty() || !picker.query.empty();
    if (any_off)
        heading(false);
    else if (any_on && narrowed) {
        heading(false);
        ListPiece piece;
        piece.kind = ListPiece::Kind::empty;
        piece.y = y;
        const std::string sentence(library::shown(picker.inputs.text, words::empty_filtered));
        piece.lines = wrap_lines(building, kit::FontRole::small, sentence, plan.width, 3);
        piece.height =
            std::max(static_cast<int32_t>(piece.lines.size()), int32_t{1}) * metrics.small_line;
        y += piece.height;
        plan.pieces.push_back(std::move(piece));
    }
    for (std::size_t place = 0; place < shown_rows.size(); ++place) {
        if (shown_rows[place].on)
            continue;
        ListPiece piece;
        piece.kind = ListPiece::Kind::row;
        piece.y = y;
        piece.height = row_height;
        piece.row = place;
        y += row_height;
        plan.pieces.push_back(std::move(piece));
    }
    plan.height = y;
    return plan;
}

/// Returns where a drag's accent line sits in the list's content, in points.
///
/// @param plan the list's content
/// @param shown_rows the rows
/// @param target where the row would land
/// @return the line's top, in content points
int32_t line_at(const ListPlan& plan, const std::vector<Row>& shown_rows, const Drop& target) {
    std::size_t on_count = 0;
    for (const Row& row : shown_rows) {
        if (row.on)
            ++on_count;
    }
    int32_t off_top = -1;
    int32_t last_on_bottom = -1;
    std::vector<int32_t> on_tops;
    for (const ListPiece& piece : plan.pieces) {
        if (piece.kind == ListPiece::Kind::heading && !piece.on_heading)
            off_top = piece.y;
        if (piece.kind != ListPiece::Kind::row)
            continue;
        if (piece.row < shown_rows.size() && shown_rows[piece.row].on) {
            on_tops.push_back(piece.y);
            last_on_bottom = piece.y + piece.height;
        }
    }
    if (target.into_on) {
        const std::size_t place = std::min(target.place, on_count);
        if (place < on_tops.size())
            return on_tops[place];
        if (last_on_bottom >= 0)
            return last_on_bottom;
        if (off_top >= 0)
            return off_top;
        return 0;
    }
    if (off_top >= 0)
        return off_top;
    if (last_on_bottom >= 0)
        return last_on_bottom;
    return 0;
}

/// Appends one row at a window row, clipped to the list.
///
/// @param[in,out] building the layout
/// @param shown_rows the rows
/// @param place the row's place
/// @param rect the row, in window points
/// @param clip the list's view
/// @param selected_look the selected face, whatever the model says
/// @param paint the items, the controls, or both
void add_row(
    Building& building,
    const std::vector<Row>& shown_rows,
    std::size_t place,
    const kit::Rect& rect,
    const kit::Rect& clip,
    bool selected_look,
    RowPaint paint
) {
    const kit::Metrics& metrics = *building.metrics;
    const Picker& picker = *building.picker;
    const Row& row = shown_rows[place];
    const bool read_only = picker.inputs.read_only;
    const Mutator* mutator = detail::find_mutator(picker, row.id);
    const bool model_selected = picker.selected && *picker.selected == row.id;
    const kit::ControlId row_id =
        controls::first_row + static_cast<kit::ControlId>(static_cast<int32_t>(place));
    const kit::ControlId grip_id =
        controls::first_grip + static_cast<kit::ControlId>(static_cast<int32_t>(place));
    const kit::ControlId switch_id =
        controls::first_switch + static_cast<kit::ControlId>(static_cast<int32_t>(place));
    const bool visuals = paint != RowPaint::controls;
    const bool named = paint != RowPaint::items;

    if (visuals) {
        add_item(building.list, kit::Role::fill, rect, clip).colour =
            selected_look ? kit::colour::list_selected : kit::colour::list;
        add_item(
            building.list,
            kit::Role::rule,
            {rect.x, rect.y + rect.height - metrics.hairline, rect.width, metrics.hairline},
            clip
        )
            .colour = kit::colour::rule;
    }

    const int32_t number_w = building.number_width;
    int32_t x = rect.x + metrics.list_row_inset;
    if (visuals && row.on) {
        const std::string number = std::to_string(row.number);
        add_text(
            building,
            kit::FontRole::small,
            cut(building, kit::FontRole::small, number, number_w),
            {x, rect.y, number_w, rect.height},
            kit::Align::right,
            kit::colour::quiet,
            clip
        );
    }
    x += number_w + metrics.list_row_gap;

    const bool grip_enabled = !read_only;
    kit::GripLook grip_look;
    grip_look.disabled = !grip_enabled;
    grip_look.hovered = hovered(building, grip_id, grip_enabled);
    grip_look.held = held(building, grip_id, grip_enabled);
    const kit::Rect grip_rect{
        x,
        rect.y + (rect.height - kit::grip_height) / 2,
        kit::grip_width,
        kit::grip_height,
    };
    const std::string row_name =
        named ? unique_name(building, std::string(name_row_prefix) + controls::name_word(row.id))
              : std::string{};
    if (visuals || named) {
        const std::string grip_name =
            named ? row_name + std::string(name_grip_suffix) : std::string("grip");
        if (named)
            keep_name(building, grip_name);
        const std::size_t grip_controls = building.list.controls.size();
        const std::size_t grip_items = building.list.items.size();
        kit::add_grip(building.list, grip_rect, grip_look, grip_id, grip_name);
        if (named) {
            move_controls(building.list, grip_controls, building.rows);
            building.rows.back().focusable = false;
            building.rows.back().clip = clip;
            building.rows.back().group = controls::list_group;
            if (!visuals)
                building.list.items.resize(grip_items);
            else if (!building.list.items.empty())
                building.list.items.back().clip = clip;
        } else {
            building.list.controls.resize(grip_controls);
            if (!building.list.items.empty()) {
                building.list.items.back().control = kit::no_control;
                building.list.items.back().clip = clip;
            }
        }
    }
    x += kit::grip_width + metrics.list_row_gap;

    if (visuals && mutator != nullptr) {
        const kit::Rect badge{
            x,
            rect.y + (rect.height - row_badge_side) / 2,
            row_badge_side,
            row_badge_side,
        };
        add_mutator_badge(building, *mutator, badge, clip);
    }
    x += row_badge_side + metrics.list_row_gap;

    const bool switch_locked = row.locked || read_only;
    const int32_t switch_w = building.row_switch_width;
    const int32_t switch_x = rect.x + rect.width - metrics.list_row_inset - switch_w;
    int32_t text_right = switch_x - metrics.list_row_gap;
    if (visuals && row.to_download) {
        const int32_t icon = metrics.state_icon_side;
        text_right -= icon + metrics.list_row_gap;
        kit::ChipLook icon_look;
        icon_look.icon = kit::StateIcon::download_arrow;
        icon_look.state = kit::ChipState::get;
        const kit::Rect icon_rect{
            switch_x - metrics.list_row_gap - icon,
            rect.y + (rect.height - icon) / 2,
            icon,
            icon,
        };
        kit::add_chip(building.list, icon_rect, icon_look, kit::no_control, {});
        if (!building.list.items.empty())
            building.list.items.back().clip = clip;
    }
    const int32_t text_w = std::max(text_right - x, int32_t{0});
    const int32_t block = metrics.regular_line + metrics.small_line;
    const int32_t text_y = rect.y + std::max((rect.height - block) / 2, int32_t{0});
    const std::string name = mutator != nullptr ? detail::display_name(*mutator) : row.id;
    if (visuals) {
        add_text(
            building,
            kit::FontRole::regular,
            cut(building, kit::FontRole::regular, name, text_w),
            {x, text_y, text_w, metrics.regular_line},
            kit::Align::left,
            kit::colour::text,
            clip
        );
        add_text(
            building,
            kit::FontRole::small,
            cut(building, kit::FontRole::small, one_line(row.line), text_w),
            {x, text_y + metrics.regular_line, text_w, metrics.small_line},
            kit::Align::left,
            tone_colour(row.tone),
            clip
        );
    }

    if (visuals || named) {
        kit::SwitchLook switch_look;
        switch_look.on = row.on;
        switch_look.locked = switch_locked;
        switch_look.off_caption =
            std::string(library::shown(picker.inputs.text, words::switch_off));
        switch_look.on_caption = std::string(library::shown(picker.inputs.text, words::switch_on));
        switch_look.hovered = hovered(building, switch_id, !switch_locked);
        // Settings' switch: its track as tall as the kit's switch, where the
        // row has the room.
        const int32_t switch_h = std::min(metrics.switch_height, rect.height);
        const kit::Rect switch_rect{
            switch_x,
            rect.y + (rect.height - switch_h) / 2,
            switch_w,
            switch_h,
        };
        const std::string switch_name =
            named ? row_name + std::string(name_switch_suffix) : std::string("switch");
        if (named)
            keep_name(building, switch_name);
        const std::size_t switch_controls = building.list.controls.size();
        const std::size_t switch_items = building.list.items.size();
        kit::add_switch(building.list, switch_rect, switch_look, switch_id, switch_name);
        if (named) {
            move_controls(building.list, switch_controls, building.rows);
            building.rows.back().focusable = false;
            building.rows.back().clip = clip;
            building.rows.back().group = controls::list_group;
            if (!visuals)
                building.list.items.resize(switch_items);
            else if (!building.list.items.empty())
                building.list.items.back().clip = clip;
        } else {
            building.list.controls.resize(switch_controls);
            if (!building.list.items.empty()) {
                building.list.items.back().control = kit::no_control;
                building.list.items.back().clip = clip;
            }
        }
    }

    if (visuals && selected_look)
        add_item(building.list, kit::Role::outline, rect, clip).colour = kit::colour::accent;
    if (visuals && row.locked)
        add_item(building.list, kit::Role::locked_fade, rect, clip);

    if (named) {
        kit::Control control;
        control.id = row_id;
        control.rect = rect;
        control.clip = clip;
        control.enabled = true;
        control.focusable = false;
        control.name = row_name;
        control.kind = kit::ControlKind::list_item;
        control.group = controls::list_group;
        control.checked = model_selected;
        control.text = name;
        building.rows.push_back(std::move(control));
    }
}

/// Appends the list pane: the hint, the headings, the rows, and the scroll bar.
///
/// An empty list draws its sentence in the pane and does not scroll. A drag
/// leaves the row's controls on the well and draws the row, as items only,
/// whole inside the list's view. The accent line is drawn when the target
/// is set, under the moving row.
///
/// @param[in,out] building the layout
/// @param pane the pane
/// @param shown_rows the rows the model gives
/// @param hint_text the hint
void add_list(
    Building& building,
    const kit::Rect& pane,
    const std::vector<Row>& shown_rows,
    const std::string& hint_text
) {
    const kit::Metrics& metrics = *building.metrics;
    const int32_t well_width = metrics.scroll_thumb_width + 2;
    const int32_t bar_gap = metrics.button_gap;
    add_item(building.list, kit::Role::fill, pane, {}).colour = kit::colour::list;

    if (shown_rows.empty()) {
        int32_t top = pane.y;
        int32_t room = pane.height;
        if (!hint_text.empty()) {
            const bool compact = building.size_class == kit::SizeClass::compact;
            const std::vector<std::string> lines =
                wrap_lines(building, kit::FontRole::small, hint_text, pane.width, compact ? 1 : 2);
            int32_t line_y = pane.y;
            for (const std::string& line : lines) {
                add_text(
                    building,
                    kit::FontRole::small,
                    line,
                    {pane.x, line_y, pane.width, metrics.small_line},
                    kit::Align::left,
                    kit::colour::hint,
                    pane
                );
                line_y += metrics.small_line;
            }
            const int32_t hint_h =
                std::max(static_cast<int32_t>(lines.size()), int32_t{1}) * metrics.small_line;
            const std::string name(name_hint);
            keep_name(building, name);
            add_label(
                building.panes,
                controls::hint_label,
                {pane.x, pane.y, pane.width, hint_h},
                name,
                joined(lines),
                pane,
                controls::list_group
            );
            top = pane.y + hint_h + metrics.paragraph_gap;
            room = std::max(pane.y + pane.height - top, int32_t{0});
        }
        const std::string_view english =
            building.picker->inputs.mutators.empty() ? words::empty_none : words::empty_filtered;
        const std::string sentence(library::shown(building.picker->inputs.text, english));
        const int32_t most = std::max(room / std::max(metrics.small_line, int32_t{1}), int32_t{1});
        const std::vector<std::string> lines = wrap_lines(
            building, kit::FontRole::small, sentence, pane.width, static_cast<std::size_t>(most)
        );
        const int32_t block = static_cast<int32_t>(lines.size()) * metrics.small_line;
        int32_t line_y = top + std::max((room - block) / 2, int32_t{0});
        for (const std::string& line : lines) {
            add_text(
                building,
                kit::FontRole::small,
                line,
                {pane.x, line_y, pane.width, metrics.small_line},
                kit::Align::centre,
                kit::colour::hint,
                {pane.x, top, pane.width, room}
            );
            line_y += metrics.small_line;
        }
        kit::Control list_control;
        list_control.id = controls::list;
        list_control.rect = pane;
        list_control.enabled = true;
        list_control.focusable = true;
        list_control.name = std::string(name_list);
        keep_name(building, list_control.name);
        list_control.kind = kit::ControlKind::list;
        building.panes.push_back(std::move(list_control));
        return;
    }

    ListPlan wide = plan_list(building, shown_rows, hint_text, pane.width);
    const bool scrolling = wide.height > pane.height;
    const int32_t content_w =
        scrolling ? std::max(pane.width - well_width - bar_gap, int32_t{0}) : pane.width;
    const ListPlan plan = scrolling ? plan_list(building, shown_rows, hint_text, content_w) : wide;
    const kit::Rect port{pane.x, pane.y, content_w, pane.height};
    const int32_t limit = kit::ScrollArea::limit_for(plan.height, port.height);
    const int32_t offset = std::clamp(building.state->list_scroll, int32_t{0}, limit);

    std::optional<std::size_t> dragged;
    if (building.state->drag) {
        for (std::size_t place = 0; place < shown_rows.size(); ++place) {
            if (shown_rows[place].id == building.state->drag->id) {
                dragged = place;
                break;
            }
        }
    }

    for (const ListPiece& piece : plan.pieces) {
        const int32_t window_y = piece.y + port.y - offset;
        if (piece.kind == ListPiece::Kind::hint) {
            int32_t line_y = window_y;
            for (const std::string& line : piece.lines) {
                add_text(
                    building,
                    kit::FontRole::small,
                    line,
                    {port.x, line_y, port.width, metrics.small_line},
                    kit::Align::left,
                    kit::colour::hint,
                    port
                );
                line_y += metrics.small_line;
            }
            const std::string name(name_hint);
            keep_name(building, name);
            add_label(
                building.panes,
                controls::hint_label,
                {port.x, window_y, port.width, piece.height},
                name,
                joined(piece.lines),
                port,
                controls::list_group
            );
            continue;
        }
        if (piece.kind == ListPiece::Kind::heading) {
            kit::HeadingLook look;
            look.text = piece.text;
            add_item(
                building.list,
                kit::Role::heading,
                {port.x, window_y, port.width, piece.height},
                port
            )
                .look = std::move(look);
            continue;
        }
        if (piece.kind == ListPiece::Kind::empty) {
            int32_t line_y = window_y;
            for (const std::string& line : piece.lines) {
                add_text(
                    building,
                    kit::FontRole::small,
                    line,
                    {port.x, line_y, port.width, metrics.small_line},
                    kit::Align::centre,
                    kit::colour::hint,
                    port
                );
                line_y += metrics.small_line;
            }
            continue;
        }
        const bool is_dragged = dragged && *dragged == piece.row;
        if (is_dragged) {
            add_item(
                building.list, kit::Role::fill, {port.x, window_y, port.width, piece.height}, port
            )
                .colour = kit::colour::well;
            add_row(
                building,
                shown_rows,
                piece.row,
                {port.x, window_y, port.width, piece.height},
                port,
                false,
                RowPaint::controls
            );
            continue;
        }
        const bool selected =
            building.picker->selected && *building.picker->selected == shown_rows[piece.row].id;
        add_row(
            building,
            shown_rows,
            piece.row,
            {port.x, window_y, port.width, piece.height},
            port,
            selected,
            RowPaint::both
        );
    }

    if (dragged && building.state->drag->target) {
        const int32_t content_y = line_at(plan, shown_rows, *building.state->drag->target);
        const int32_t window_y = content_y + port.y - offset;
        add_item(building.list, kit::Role::fill, {port.x, window_y, port.width, drag_line}, port)
            .colour = kit::colour::accent;
    }

    if (dragged) {
        int32_t row_height = 0;
        for (const ListPiece& piece : plan.pieces) {
            if (piece.kind == ListPiece::Kind::row && piece.row == *dragged)
                row_height = piece.height;
        }
        const int32_t least = port.y;
        const int32_t most = port.y + std::max(port.height - row_height, int32_t{0});
        int32_t top = building.state->drag->y;
        if (top < least)
            top = least;
        if (top > most)
            top = most;
        add_row(
            building,
            shown_rows,
            *dragged,
            {port.x, top, port.width, row_height},
            port,
            true,
            RowPaint::items
        );
    }

    kit::Control list_control;
    list_control.id = controls::list;
    list_control.rect = port;
    list_control.enabled = true;
    list_control.focusable = true;
    list_control.name = std::string(name_list);
    keep_name(building, list_control.name);
    list_control.kind = kit::ControlKind::list;
    building.panes.push_back(std::move(list_control));

    if (!scrolling)
        return;
    const kit::Rect well{pane.x + content_w + bar_gap, pane.y, well_width, pane.height};
    kit::ScrollArea area;
    area.view = port;
    area.well = well;
    area.content_height = plan.height;
    area.limit = limit;
    area.offset = offset;
    kit::ScrollBarLook bar_look;
    bar_look.thumb = area.thumb(metrics.least_thumb_height);
    bar_look.hot = building.state->bar == ScreenState::Bar::list ||
                   building.state->interaction.hovered == controls::list_bar ||
                   building.state->interaction.pressed == controls::list_bar;
    add_item(building.list, kit::Role::scroll_bar, well, {}).look = bar_look;
    kit::Control bar;
    bar.id = controls::list_bar;
    bar.rect = well;
    bar.enabled = true;
    bar.focusable = false;
    bar.name = std::string(name_list_bar);
    keep_name(building, bar.name);
    bar.kind = kit::ControlKind::scroll_bar;
    bar.group = controls::list_group;
    building.panes.push_back(std::move(bar));
}

/// How one option is drawn.
enum class OptionForm : uint8_t { levels, choice, toggle, slider };

/// One option, measured in the details' content.
struct OptionPlan {
    std::size_t spec{0}; ///< the option's place
    OptionForm form{OptionForm::choice};
    int32_t label_y{0};                  ///< the label's top
    int32_t control_y{0};                ///< the control's top
    int32_t control_h{0};                ///< the control's height
    int32_t control_w{0};                ///< the control's width
    int32_t value_x{0};                  ///< a slider's value
    int32_t value_w{0};                  ///< a slider's value width
    int32_t level_width{0};              ///< one level's columns
    std::size_t chosen{0};               ///< the chosen level or choice
    bool on{false};                      ///< a toggle's state
    int32_t stop{0};                     ///< a slider's stop
    int32_t stops{2};                    ///< a slider's stops
    bool locked{false};                  ///< it cannot be changed
    std::string label{};                 ///< the label, as drawn
    std::string choice_text{};           ///< a drop-down's text
    std::string value_text{};            ///< a slider's value, as drawn
    std::string off_caption{};           ///< a toggle's Off caption
    std::string on_caption{};            ///< a toggle's On caption
    std::vector<std::string> captions{}; ///< a strip's or a menu's captions
    std::vector<std::string> parts{};    ///< the automation words
};

/// One fact, measured in the details' content.
struct FactPlan {
    int32_t y{0};                       ///< the fact's top, in content points
    int32_t height{0};                  ///< the fact's height
    int32_t label_width{0};             ///< the label's columns
    int32_t value_x{0};                 ///< the value's left column
    int32_t value_w{0};                 ///< the value's columns
    std::string label{};                ///< the label, as drawn
    std::vector<std::string> lines{};   ///< the value, wrapped
    kit::Colour ink{kit::colour::text}; ///< the value's colour
};

/// The details' content, measured and not yet drawn.
struct DetailsPlan {
    int32_t width{0};                          ///< the content's width
    int32_t height{0};                         ///< the content's height
    std::string name{};                        ///< the mutator's name, as drawn
    std::string version{};                     ///< the version and author, as drawn
    int32_t name_y{0};                         ///< the name's top
    int32_t name_x{0};                         ///< the name's left column
    int32_t name_w{0};                         ///< the name's columns
    std::vector<std::string> description{};    ///< the description, wrapped
    int32_t description_y{0};                  ///< the description's top
    std::string summary{};                     ///< an Off mutator's option line, as drawn
    int32_t summary_y{0};                      ///< that line's top
    bool has_summary{false};                   ///< the line is drawn
    std::vector<OptionPlan> options{};         ///< the option controls, when it is on
    bool has_clash{false};                     ///< it brings units or weapons and is on
    int32_t clash_label_y{0};                  ///< the clash label's top
    int32_t clash_y{0};                        ///< the clash strip's top
    int32_t clash_w{0};                        ///< the clash strip's width
    int32_t clash_level_width{0};              ///< one clash caption's columns
    std::size_t clash_chosen{0};               ///< 0 is Add, 1 is Replace
    bool clash_locked{false};                  ///< it cannot be changed
    std::string clash_label{};                 ///< the clash label, as drawn
    std::vector<std::string> clash_captions{}; ///< Add and Replace, as drawn
    int32_t order_y{0};                        ///< the order row's top
    int32_t up_x{0};                           ///< ▲ UP's left column
    int32_t up_w{0};                           ///< ▲ UP's width
    int32_t down_x{0};                         ///< ▼ DOWN's left column
    int32_t down_w{0};                         ///< ▼ DOWN's width
    int32_t order_w{0};                        ///< the order label's columns
    bool up_enabled{false};                    ///< ▲ UP can be pressed
    bool down_enabled{false};                  ///< ▼ DOWN can be pressed
    std::string order{};                       ///< the order label, as drawn
    std::string up_caption{};                  ///< ▲ UP's words
    std::string down_caption{};                ///< ▼ DOWN's words
    std::vector<FactPlan> facts{};             ///< Checked, Works with and Changes
};

/// Returns the caption a choice or a toggle shows for one value.
///
/// @param spec the option
/// @param place the value's place
/// @return the caption, or the value when it names none
std::string caption_at(const OptionSpec& spec, std::size_t place) {
    if (place < spec.captions.size() && !spec.captions[place].empty())
        return spec.captions[place];
    if (place < spec.values.size())
        return spec.values[place];
    return {};
}

/// Measures the details at a width. Adds nothing.
///
/// @param building the layout
/// @param mutator the selected mutator
/// @param chosen the On record; null when it is off
/// @param on_at where it sits in the On list; empty when it is off
/// @param band the places it may occupy
/// @param width the content's width, in points
/// @return the plan
DetailsPlan plan_details(
    const Building& building,
    const Mutator& mutator,
    const Chosen* chosen,
    std::optional<std::size_t> on_at,
    const Band& band,
    int32_t width
) {
    const kit::Metrics& metrics = *building.metrics;
    const Picker& picker = *building.picker;
    const library::TextHooks& hooks = picker.inputs.text;
    const bool compact = building.size_class == kit::SizeClass::compact;
    const bool read_only = picker.inputs.read_only;
    DetailsPlan plan;
    plan.width = std::max(width, int32_t{0});
    plan.name = detail::display_name(mutator);
    if (!mutator.version.empty() && !mutator.author.empty()) {
        plan.version = library::filled(
            hooks, words::version_author, {{"version", mutator.version}, {"author", mutator.author}}
        );
    } else if (!mutator.version.empty()) {
        plan.version = mutator.version;
    } else {
        plan.version = mutator.author;
    }
    const int32_t block =
        plan.version.empty() ? metrics.regular_line : metrics.regular_line + metrics.small_line;
    plan.name_y = std::max((details_badge_side - block) / 2, int32_t{0});
    plan.name_x = details_badge_side + metrics.list_row_gap;
    plan.name_w = std::max(plan.width - plan.name_x, int32_t{0});
    plan.name = cut(building, kit::FontRole::regular, plan.name, plan.name_w);
    plan.version = cut(building, kit::FontRole::small, plan.version, plan.name_w);

    plan.description_y = details_badge_side + metrics.paragraph_gap;
    const std::size_t most_lines = compact ? 3 : 6;
    plan.description =
        wrap_lines(building, kit::FontRole::small, mutator.description, plan.width, most_lines);
    int32_t y =
        plan.description_y + static_cast<int32_t>(plan.description.size()) * metrics.small_line;

    const bool on = on_at.has_value() && chosen != nullptr;
    if (!on) {
        const std::string summary = default_options_text(picker, mutator.id);
        if (!summary.empty()) {
            y += metrics.paragraph_gap;
            plan.summary_y = y;
            plan.summary = cut(building, kit::FontRole::regular, one_line(summary), plan.width);
            plan.has_summary = !plan.summary.empty();
            if (plan.has_summary)
                y += metrics.regular_line;
        }
    } else {
        for (std::size_t place = 0; place < mutator.options.size(); ++place) {
            const OptionSpec& spec = mutator.options[place];
            OptionPlan option;
            option.spec = place;
            option.locked = read_only;
            y += metrics.paragraph_gap;
            option.label_y = y;
            option.label = cut(building, kit::FontRole::regular, spec.label, plan.width);
            y += metrics.regular_line + metrics.hint_gap;
            option.control_y = y;
            std::string value = detail::chosen_value(*chosen, spec.id);
            if (value.empty())
                value = spec.fallback;
            std::size_t chosen_place = 0;
            for (std::size_t slot = 0; slot < spec.values.size(); ++slot) {
                if (spec.values[slot] == value)
                    chosen_place = slot;
            }
            if (spec.type == OptionSpec::Type::toggle) {
                option.form = OptionForm::toggle;
                option.control_h = metrics.control_height;
                option.on = spec.values.size() > 1 && value == spec.values[1];
                option.off_caption = caption_at(spec, 0);
                option.on_caption = caption_at(spec, 1);
                const int32_t off_width =
                    kit::text_width(*building.fonts, kit::FontRole::small, option.off_caption);
                const int32_t on_width =
                    kit::text_width(*building.fonts, kit::FontRole::small, option.on_caption);
                option.control_w = kit::switch_width_for(std::max(off_width, on_width));
                if (plan.width > 0 && option.control_w > plan.width) {
                    option.control_w = plan.width;
                    const int32_t room = std::max((option.control_w - 2) / 2, int32_t{0});
                    option.off_caption =
                        cut(building, kit::FontRole::small, option.off_caption, room);
                    option.on_caption =
                        cut(building, kit::FontRole::small, option.on_caption, room);
                }
            } else if (spec.type == OptionSpec::Type::number) {
                option.form = OptionForm::slider;
                option.control_h = std::max(metrics.knob_height, metrics.small_line);
                const std::vector<std::string> stops = number_values(spec);
                option.stop = 0;
                for (std::size_t slot = 0; slot < stops.size(); ++slot) {
                    if (stops[slot] == value)
                        option.stop = static_cast<int32_t>(slot);
                }
                option.stops = static_cast<int32_t>(stops.size());
                if (option.stops < 2) {
                    option.stops = 2;
                    option.stop = 0;
                    option.locked = true;
                }
                option.value_w =
                    std::min(metrics.slider_value_width, std::max(plan.width, int32_t{0}));
                const int32_t room = std::max(plan.width - metrics.slider_value_gap, int32_t{0});
                if (option.value_w > room)
                    option.value_w = room;
                option.control_w =
                    std::max(plan.width - option.value_w - metrics.slider_value_gap, int32_t{0});
                option.value_x = option.control_w + metrics.slider_value_gap;
                option.value_text = cut(building, kit::FontRole::small, value, option.value_w);
            } else if (spec.values.size() >= 1 && spec.values.size() <= 4) {
                option.form = OptionForm::levels;
                option.control_h = metrics.control_height;
                option.chosen = chosen_place;
                std::vector<std::string> used;
                int32_t widest = 0;
                for (std::size_t slot = 0; slot < spec.values.size(); ++slot) {
                    std::string caption = caption_at(spec, slot);
                    widest = std::max(
                        widest, kit::text_width(*building.fonts, kit::FontRole::small, caption)
                    );
                    option.parts.push_back(unique_word(used, controls::name_word(caption)));
                    option.captions.push_back(std::move(caption));
                }
                option.level_width = segment_width(building, widest);
                const int32_t count = static_cast<int32_t>(option.captions.size());
                int32_t strip = 2 + count * option.level_width;
                if (count > 0 && strip > plan.width) {
                    option.level_width = std::max((plan.width - 2) / count, int32_t{1});
                    strip = 2 + count * option.level_width;
                    const int32_t caption_room =
                        std::max(option.level_width - 2 * metrics.chip_padding, int32_t{0});
                    for (std::string& caption : option.captions)
                        caption = cut(building, kit::FontRole::small, caption, caption_room);
                }
                option.control_w = std::min(strip, std::max(plan.width, int32_t{0}));
            } else {
                // Settings' drop-down: as tall as a control, choice_width wide,
                // or as wide as its widest choice up to the details' width.
                option.form = OptionForm::choice;
                option.control_h = metrics.control_height;
                option.control_w = plan.width;
                option.chosen = chosen_place;
                option.locked = read_only || spec.values.empty();
                std::vector<std::string> used;
                if (spec.values.empty()) {
                    option.choice_text =
                        cut(building,
                            kit::FontRole::regular,
                            value,
                            option.control_w - metrics.choice_text_inset -
                                metrics.choice_arrow_room);
                }
                int32_t widest = 0;
                for (std::size_t slot = 0; slot < spec.values.size(); ++slot) {
                    std::string caption = caption_at(spec, slot);
                    widest = std::max(
                        widest, kit::text_width(*building.fonts, kit::FontRole::regular, caption)
                    );
                    option.parts.push_back(unique_word(used, controls::name_word(caption)));
                    option.captions.push_back(std::move(caption));
                }
                if (!option.captions.empty())
                    option.control_w = std::clamp(
                        widest + metrics.choice_text_inset + metrics.choice_arrow_room,
                        std::min(metrics.choice_width, std::max(plan.width, int32_t{0})),
                        std::max(plan.width, int32_t{0})
                    );
                if (!option.captions.empty()) {
                    const std::string& shown_choice = option.chosen < option.captions.size()
                                                          ? option.captions[option.chosen]
                                                          : option.captions.front();
                    option.choice_text =
                        cut(building,
                            kit::FontRole::regular,
                            shown_choice,
                            option.control_w - metrics.choice_text_inset -
                                metrics.choice_arrow_room);
                }
            }
            y += option.control_h;
            plan.options.push_back(std::move(option));
        }
        if (mutator.brings) {
            y += metrics.paragraph_gap;
            plan.has_clash = true;
            plan.clash_locked = read_only;
            plan.clash_label_y = y;
            const std::string label(library::shown(hooks, words::clash_label));
            plan.clash_label = cut(building, kit::FontRole::regular, label, plan.width);
            y += metrics.regular_line + metrics.hint_gap;
            plan.clash_y = y;
            const Clash mode = chosen->clash.value_or(Clash::replace);
            plan.clash_chosen = mode == Clash::add ? std::size_t{0} : std::size_t{1};
            plan.clash_captions = {
                std::string(library::shown(hooks, words::clash_mode_add)),
                std::string(library::shown(hooks, words::clash_mode_replace)),
            };
            int32_t widest = 0;
            for (const std::string& caption : plan.clash_captions) {
                widest = std::max(
                    widest, kit::text_width(*building.fonts, kit::FontRole::small, caption)
                );
            }
            plan.clash_level_width = segment_width(building, widest);
            constexpr int32_t count = 2;
            int32_t strip = 2 + count * plan.clash_level_width;
            if (strip > plan.width) {
                plan.clash_level_width = std::max((plan.width - 2) / count, int32_t{1});
                strip = 2 + count * plan.clash_level_width;
                const int32_t caption_room =
                    std::max(plan.clash_level_width - 2 * metrics.chip_padding, int32_t{0});
                for (std::string& caption : plan.clash_captions)
                    caption = cut(building, kit::FontRole::small, caption, caption_room);
            }
            plan.clash_w = std::min(strip, std::max(plan.width, int32_t{0}));
            y += metrics.control_height;
        }
    }

    y += metrics.paragraph_gap;
    plan.order_y = y;
    plan.up_caption = std::string(library::shown(hooks, words::order_up));
    plan.down_caption = std::string(library::shown(hooks, words::order_down));
    plan.up_w = button_span(building, plan.up_caption);
    plan.down_w = button_span(building, plan.down_caption);
    plan.down_x = std::max(plan.width - plan.down_w, int32_t{0});
    plan.up_x = std::max(plan.down_x - button_spacing(metrics) - plan.up_w, int32_t{0});
    plan.order_w = std::max(plan.up_x - button_spacing(metrics), int32_t{0});
    plan.order = cut(
        building, kit::FontRole::regular, library::shown(hooks, words::order_label), plan.order_w
    );
    const std::size_t on_index = on_at.has_value() ? *on_at : std::size_t{0};
    const bool may_move = on && !read_only && band.first <= band.last;
    plan.up_enabled = may_move && on_index > band.first;
    plan.down_enabled = may_move && on_index < band.last;
    y += metrics.control_height;

    y += metrics.paragraph_gap;
    const std::string checked_label(library::shown(hooks, library::words::fact_checked));
    const std::string works_label(library::shown(hooks, library::words::fact_works_with));
    const std::string changes_label(library::shown(hooks, library::words::fact_changes));
    int32_t label_width = 0;
    for (const std::string& label : {checked_label, works_label, changes_label}) {
        label_width =
            std::max(label_width, kit::text_width(*building.fonts, kit::FontRole::small, label));
    }
    const int32_t half = std::max(plan.width / 2, int32_t{0});
    if (label_width > half)
        label_width = half;
    const int32_t value_x = label_width + metrics.label_gap;
    const int32_t value_w = std::max(plan.width - value_x, int32_t{0});

    const std::string mod = picker.inputs.mod_name.empty()
                                ? std::string(library::shown(hooks, words::no_mod))
                                : picker.inputs.mod_name;
    const library::CheckedWords checked = library::checked_text(mutator.verdict, mod, hooks);
    const std::string checked_value = checked.value;
    kit::Colour checked_ink = kit::colour::text;
    if (checked.mark == library::Mark::good)
        checked_ink = kit::colour::accent;
    else if (checked.mark == library::Mark::warn)
        checked_ink = kit::colour::lock;
    else if (checked.mark == library::Mark::bad)
        checked_ink = kit::colour::danger;
    const std::string works_value =
        mutator.works_with_text.empty()
            ? std::string(library::shown(hooks, library::words::works_with_any))
            : mutator.works_with_text;
    std::vector<std::string> layer_names;
    for (const std::string& id : mutator.layers) {
        const std::string_view word = library::layer_word(id);
        if (!word.empty())
            layer_names.emplace_back(library::shown(hooks, word));
        else if (!id.empty())
            layer_names.push_back(id);
    }
    const std::string changes_value = detail::join_names(hooks, layer_names);
    const std::string labels[] = {checked_label, works_label, changes_label};
    const std::string values[] = {checked_value, works_value, changes_value};
    const kit::Colour inks[] = {checked_ink, kit::colour::text, kit::colour::text};
    for (int32_t place = 0; place < 3; ++place) {
        FactPlan fact;
        fact.y = y;
        fact.label_width = label_width;
        fact.value_x = value_x;
        fact.value_w = value_w;
        fact.label = cut(building, kit::FontRole::small, labels[place], label_width);
        fact.lines = wrap_lines(building, kit::FontRole::small, values[place], value_w, 2);
        fact.ink = inks[place];
        fact.height =
            std::max(static_cast<int32_t>(fact.lines.size()), int32_t{1}) * metrics.small_line;
        y += fact.height;
        plan.facts.push_back(std::move(fact));
    }
    plan.height = y;
    return plan;
}

/// Appends the measured details into a view, scrolled by an offset.
///
/// @param[in,out] building the layout
/// @param plan the measured details
/// @param mutator the selected mutator
/// @param port the view
/// @param offset how far the content is scrolled
void emit_details(
    Building& building,
    const DetailsPlan& plan,
    const Mutator& mutator,
    const kit::Rect& port,
    int32_t offset
) {
    const kit::Metrics& metrics = *building.metrics;
    const auto at = [&](int32_t x, int32_t y, int32_t w, int32_t h) {
        return kit::Rect{
            port.x + x, port.y + y - offset, std::max(w, int32_t{0}), std::max(h, int32_t{0})
        };
    };
    add_mutator_badge(building, mutator, at(0, 0, details_badge_side, details_badge_side), port);
    add_text(
        building,
        kit::FontRole::regular,
        plan.name,
        at(plan.name_x, plan.name_y, plan.name_w, metrics.regular_line),
        kit::Align::left,
        kit::colour::text,
        port
    );
    if (!plan.version.empty()) {
        add_text(
            building,
            kit::FontRole::small,
            plan.version,
            at(plan.name_x, plan.name_y + metrics.regular_line, plan.name_w, metrics.small_line),
            kit::Align::left,
            kit::colour::hint,
            port
        );
    }
    int32_t line_y = plan.description_y;
    for (const std::string& line : plan.description) {
        add_text(
            building,
            kit::FontRole::small,
            line,
            at(0, line_y, plan.width, metrics.small_line),
            kit::Align::left,
            kit::colour::text,
            port
        );
        line_y += metrics.small_line;
    }
    if (plan.has_summary) {
        add_text(
            building,
            kit::FontRole::regular,
            plan.summary,
            at(0, plan.summary_y, plan.width, metrics.regular_line),
            kit::Align::left,
            kit::colour::text,
            port
        );
    }
    for (std::size_t place = 0; place < plan.options.size(); ++place) {
        const OptionPlan& option = plan.options[place];
        const OptionSpec& spec = mutator.options[option.spec];
        add_text(
            building,
            kit::FontRole::regular,
            option.label,
            at(0, option.label_y, plan.width, metrics.regular_line),
            kit::Align::left,
            kit::colour::text,
            port
        );
        const kit::ControlId id =
            controls::first_option + static_cast<kit::ControlId>(static_cast<int32_t>(place));
        const std::string name =
            unique_name(building, std::string(name_option_prefix) + controls::name_word(spec.id));
        const kit::Rect control_rect = at(0, option.control_y, option.control_w, option.control_h);
        const std::size_t before = building.list.controls.size();
        if (option.form == OptionForm::toggle) {
            kit::SwitchLook look;
            look.on = option.on;
            look.locked = option.locked;
            look.off_caption = option.off_caption;
            look.on_caption = option.on_caption;
            look.hovered = hovered(building, id, !option.locked);
            kit::add_switch(building.list, control_rect, look, id, name);
        } else if (option.form == OptionForm::slider) {
            kit::SliderLook look;
            look.stop = option.stop;
            look.stops = option.stops;
            look.locked = option.locked;
            look.hovered = hovered(building, id, !option.locked);
            kit::add_slider(building.list, control_rect, look, id, name);
            add_text(
                building,
                kit::FontRole::small,
                option.value_text,
                at(option.value_x, option.control_y, option.value_w, option.control_h),
                kit::Align::left,
                kit::colour::text,
                port
            );
        } else if (option.form == OptionForm::levels) {
            kit::LevelsLook look;
            look.captions = option.captions;
            look.level_width = option.level_width;
            look.chosen = option.chosen;
            look.offered = option.captions.size();
            look.locked = option.locked;
            look.hovered = hovered(building, id, !option.locked);
            kit::add_levels(building.list, control_rect, look, id, name);
        } else {
            kit::ChoiceLook look;
            look.text = option.choice_text;
            look.hovered = hovered(building, id, !option.locked);
            look.open = !option.locked && building.state->menu == Menu::choice &&
                        building.state->menu_option == spec.id;
            kit::add_choice(building.list, control_rect, look, id, name);
            if (look.open) {
                OpenMenu menu_open;
                menu_open.field = control_rect;
                menu_open.chosen = static_cast<int32_t>(option.chosen);
                menu_open.name = name;
                for (std::size_t slot = 0; slot < option.captions.size(); ++slot) {
                    MenuEntry entry;
                    entry.word = option.parts[slot];
                    entry.caption = option.captions[slot];
                    menu_open.items.push_back(std::move(entry));
                }
                building.menu_open = std::move(menu_open);
            }
        }
        move_controls(building.list, before, building.detail);
        kit::Control& control = building.detail.back();
        control.clip = port;
        control.group = controls::details_group;
        control.parts = option.parts;
        if (option.locked) {
            control.enabled = false;
            control.focusable = false;
        }
        if (!building.list.items.empty())
            building.list.items.back().clip = port;
        building.option_ids.push_back(id);
    }

    if (plan.has_clash) {
        add_text(
            building,
            kit::FontRole::regular,
            plan.clash_label,
            at(0, plan.clash_label_y, plan.width, metrics.regular_line),
            kit::Align::left,
            kit::colour::text,
            port
        );
        kit::LevelsLook look;
        look.captions = plan.clash_captions;
        look.level_width = plan.clash_level_width;
        look.chosen = plan.clash_chosen;
        look.offered = plan.clash_captions.size();
        look.locked = plan.clash_locked;
        look.hovered = hovered(building, controls::clash, !plan.clash_locked);
        const std::string clash_name(name_clash);
        keep_name(building, clash_name);
        const std::size_t before = building.list.controls.size();
        kit::add_levels(
            building.list,
            at(0, plan.clash_y, plan.clash_w, metrics.control_height),
            look,
            controls::clash,
            clash_name
        );
        move_controls(building.list, before, building.detail);
        kit::Control& control = building.detail.back();
        control.clip = port;
        control.group = controls::details_group;
        control.parts = {"add", "replace"};
        if (plan.clash_locked) {
            control.enabled = false;
            control.focusable = false;
        }
        if (!building.list.items.empty())
            building.list.items.back().clip = port;
    }

    add_text(
        building,
        kit::FontRole::regular,
        plan.order,
        at(0, plan.order_y, plan.order_w, metrics.control_height),
        kit::Align::left,
        kit::colour::text,
        port
    );

    const auto add_order = [&](kit::ControlId id,
                               std::string_view control_name,
                               int32_t x,
                               int32_t width,
                               const std::string& caption,
                               bool enabled) {
        kit::ButtonLook look;
        look.caption = caption;
        look.style = kit::ButtonStyle::plain;
        look.enabled = enabled;
        look.hovered = hovered(building, id, enabled);
        look.held = held(building, id, enabled);
        look.font = kit::FontRole::regular;
        const std::size_t before = building.list.controls.size();
        keep_name(building, std::string(control_name));
        kit::add_button(
            building.list,
            at(x, plan.order_y, width, metrics.control_height),
            look,
            id,
            std::string(control_name)
        );
        move_controls(building.list, before, building.detail);
        building.detail.back().clip = port;
        building.detail.back().group = controls::details_group;
        if (!building.list.items.empty())
            building.list.items.back().clip = port;
    };
    add_order(controls::up, name_up, plan.up_x, plan.up_w, plan.up_caption, plan.up_enabled);
    add_order(
        controls::down, name_down, plan.down_x, plan.down_w, plan.down_caption, plan.down_enabled
    );

    for (const FactPlan& fact : plan.facts) {
        add_text(
            building,
            kit::FontRole::small,
            fact.label,
            at(0, fact.y, fact.label_width, metrics.small_line),
            kit::Align::left,
            kit::colour::hint,
            port
        );
        int32_t line_top = fact.y;
        for (const std::string& line : fact.lines) {
            add_text(
                building,
                kit::FontRole::small,
                line,
                at(fact.value_x, line_top, fact.value_w, metrics.small_line),
                kit::Align::left,
                fact.ink,
                port
            );
            line_top += metrics.small_line;
        }
    }
}

/// Appends the details pane, scrolled when it holds more than it shows.
///
/// Nothing is added when no mutator is selected. Read-only locks every option.
///
/// @param[in,out] building the layout
/// @param pane the pane
void add_details(Building& building, const kit::Rect& pane) {
    const kit::Metrics& metrics = *building.metrics;
    add_item(building.list, kit::Role::fill, pane, {}).colour = kit::colour::list;
    const Picker& picker = *building.picker;
    if (!picker.selected)
        return;
    const Mutator* mutator = detail::find_mutator(picker, *picker.selected);
    if (mutator == nullptr)
        return;
    const std::optional<std::size_t> on_at = detail::on_index(picker, mutator->id);
    const Chosen* chosen = on_at ? &picker.on[*on_at] : nullptr;
    const Band band = allowed_places(picker, mutator->id);

    const int32_t well_width = metrics.scroll_thumb_width + 2;
    const int32_t bar_gap = metrics.button_gap;
    const DetailsPlan wide = plan_details(building, *mutator, chosen, on_at, band, pane.width);
    const bool scrolling = wide.height > pane.height;
    const int32_t content_w =
        scrolling ? std::max(pane.width - well_width - bar_gap, int32_t{0}) : pane.width;
    const DetailsPlan plan =
        scrolling ? plan_details(building, *mutator, chosen, on_at, band, content_w) : wide;
    const kit::Rect port{pane.x, pane.y, content_w, pane.height};
    const int32_t limit = kit::ScrollArea::limit_for(plan.height, port.height);
    const int32_t offset = std::clamp(building.state->details_scroll, int32_t{0}, limit);
    emit_details(building, plan, *mutator, port, offset);
    if (!scrolling)
        return;
    const kit::Rect well{pane.x + content_w + bar_gap, pane.y, well_width, pane.height};
    kit::ScrollArea area;
    area.view = port;
    area.well = well;
    area.content_height = plan.height;
    area.limit = limit;
    area.offset = offset;
    kit::ScrollBarLook bar_look;
    bar_look.thumb = area.thumb(metrics.least_thumb_height);
    bar_look.hot = building.state->bar == ScreenState::Bar::details ||
                   building.state->interaction.hovered == controls::details_bar ||
                   building.state->interaction.pressed == controls::details_bar;
    add_item(building.list, kit::Role::scroll_bar, well, {}).look = bar_look;
    kit::Control bar;
    bar.id = controls::details_bar;
    bar.rect = well;
    bar.enabled = true;
    bar.focusable = false;
    bar.name = std::string(name_details_bar);
    keep_name(building, bar.name);
    bar.kind = kit::ControlKind::scroll_bar;
    bar.group = controls::details_group;
    building.detail.push_back(std::move(bar));
}

/// Appends one footer button, one of Settings' buttons: at the control
/// height, its words in the regular font, placed from a column that moves
/// left, two spaces apart.
///
/// @param[in,out] building the layout
/// @param[in,out] cursor the column the button's right edge ends at
/// @param window the window
/// @param id the control
/// @param name its name
/// @param caption its words, already looked up
/// @param accent the accent look when true
/// @param enabled it can be pressed
void add_footer_button(
    Building& building,
    int32_t& cursor,
    const Window& window,
    kit::ControlId id,
    std::string_view name,
    std::string caption,
    bool accent,
    bool enabled
) {
    const kit::Metrics& metrics = *building.metrics;
    const int32_t width = button_span(building, caption);
    cursor -= width;
    kit::ButtonLook look;
    look.caption = caption;
    look.style = accent ? kit::ButtonStyle::accent : kit::ButtonStyle::plain;
    look.enabled = enabled;
    look.hovered = hovered(building, id, enabled);
    look.held = held(building, id, enabled);
    look.font = kit::FontRole::regular;
    const kit::Rect rect{cursor, window.button_top, width, window.control_height};
    cursor -= button_spacing(metrics);
    const std::size_t before = building.list.controls.size();
    keep_name(building, std::string(name));
    kit::add_button(building.list, rect, look, id, std::string(name));
    move_controls(building.list, before, building.footer);
}

/// Appends the footer band, the verdict, the summary and the buttons.
///
/// Read-only shows the summary and CLOSE alone. Compact shows no chip.
/// While waiting is set, the summary reads it. USE THESE stays enabled unless
/// an On mutator is unavailable or conflicts with another that is on.
///
/// @param[in,out] building the layout
/// @param window the window
/// @param foot the model's footer
/// @param page Compact's details page
void add_footer(Building& building, const Window& window, const Footer& foot, bool page) {
    const kit::Metrics& metrics = *building.metrics;
    const library::TextHooks& hooks = building.picker->inputs.text;
    const bool compact = building.size_class == kit::SizeClass::compact;
    const bool read_only = building.picker->inputs.read_only;
    kit::FooterBandLook band;
    band.width = window.width;
    band.rule_row = window.footer_rule;
    const int32_t band_height = std::max(window.height - window.footer_rule, int32_t{0});
    add_item(
        building.list,
        kit::Role::footer_band,
        {0, window.footer_rule, window.width, band_height},
        {}
    )
        .look = band;
    // The kit's band is Compact's footer high; Kit 2's footer is taller.
    if (window.footer_height > metrics.footer_height)
        add_item(
            building.list,
            kit::Role::fill,
            {metrics.edge,
             window.footer_top + metrics.footer_height,
             window.width - 2 * metrics.edge,
             window.footer_height - metrics.footer_height},
            {}
        )
            .colour = kit::colour::band;

    int32_t cursor = window.right;
    if (read_only) {
        add_footer_button(
            building,
            cursor,
            window,
            controls::done,
            name_done,
            std::string(library::shown(hooks, words::close_button)),
            true,
            true
        );
    } else if (page) {
        add_footer_button(
            building,
            cursor,
            window,
            controls::use,
            name_use,
            std::string(library::shown(hooks, words::use_these)),
            true,
            use_these_enabled(*building.picker)
        );
        add_footer_button(
            building,
            cursor,
            window,
            controls::back,
            name_back,
            std::string(library::shown(hooks, words::back_button)),
            false,
            true
        );
    } else if (compact) {
        add_footer_button(
            building,
            cursor,
            window,
            controls::use,
            name_use,
            std::string(library::shown(hooks, words::use_these)),
            true,
            use_these_enabled(*building.picker)
        );
        add_footer_button(
            building,
            cursor,
            window,
            controls::cancel,
            name_cancel,
            std::string(library::shown(hooks, words::cancel)),
            false,
            true
        );
        const bool details_on =
            building.picker->selected &&
            detail::find_mutator(*building.picker, *building.picker->selected) != nullptr;
        add_footer_button(
            building,
            cursor,
            window,
            controls::details,
            name_details,
            std::string(library::shown(hooks, words::details_button)),
            false,
            details_on
        );
    } else {
        add_footer_button(
            building,
            cursor,
            window,
            controls::use,
            name_use,
            std::string(library::shown(hooks, words::use_these)),
            true,
            use_these_enabled(*building.picker)
        );
        add_footer_button(
            building,
            cursor,
            window,
            controls::cancel,
            name_cancel,
            std::string(library::shown(hooks, words::cancel)),
            false,
            true
        );
    }

    const int32_t limit = cursor;
    int32_t left = window.left;
    const bool chip_shown = !read_only && !compact && !page && !foot.chip.empty();
    if (chip_shown) {
        const int32_t room = std::max(limit - left, int32_t{0});
        kit::ChipLook look;
        look.text = foot.chip;
        if (foot.chip_mark == library::Mark::good)
            look.state = kit::ChipState::installed;
        else if (foot.chip_mark == library::Mark::warn)
            look.state = kit::ChipState::update;
        else if (foot.chip_mark == library::Mark::bad)
            look.state = kit::ChipState::problem;
        else
            look.state = kit::ChipState::get;
        const int32_t text_room = std::max(room - 2 * metrics.chip_padding, int32_t{0});
        look.text = cut(building, kit::FontRole::small, look.text, text_room);
        int32_t width = kit::chip_width(*building.fonts, look);
        if (width > room) {
            look.text.clear();
            width = std::min(kit::chip_width(*building.fonts, look), room);
        }
        const int32_t chip_y = window.footer_top + (window.footer_height - metrics.chip_height) / 2;
        const kit::Rect rect{left, chip_y, std::max(width, int32_t{0}), metrics.chip_height};
        if (rect.width > 0) {
            kit::add_chip(building.list, rect, look, kit::no_control, {});
            const std::string name(name_verdict);
            keep_name(building, name);
            add_label(building.footer, controls::verdict_label, rect, name, look.text, {}, -1);
            left += width + button_spacing(metrics);
        }
    }
    const int32_t summary_w = std::max(limit - left, int32_t{0});
    if (summary_w > 0) {
        const std::string source =
            building.state->waiting.empty() ? foot.summary : building.state->waiting;
        const std::string drawn = cut(building, kit::FontRole::small, source, summary_w);
        const int32_t summary_y =
            window.footer_top + (window.footer_height - metrics.small_line) / 2;
        const kit::Rect rect{left, summary_y, summary_w, metrics.small_line};
        add_text(
            building, kit::FontRole::small, drawn, rect, kit::Align::left, kit::colour::hint, {}
        );
        const std::string name(name_summary);
        keep_name(building, name);
        add_label(building.footer, controls::summary_label, rect, name, drawn, {}, -1);
    }
}

/// Declares Tab's order. A control that is missing or disabled is left out,
/// as is one clipped away, except a details control.
///
/// @param[in,out] building the layout
/// @param page Compact's details page
void declare_tabs(Building& building, bool page) {
    const bool compact = building.size_class == kit::SizeClass::compact;
    const auto stop = [&](kit::ControlId id) {
        if (kit::Control* control = find_control(building, id))
            tab_stop(building, *control);
    };
    if (page) {
        for (const kit::ControlId id : building.option_ids)
            stop(id);
        stop(controls::clash);
        stop(controls::up);
        stop(controls::down);
        stop(controls::back);
        stop(controls::use);
        stop(controls::done);
        stop(controls::close_mark);
        return;
    }
    if (compact) {
        stop(controls::filter);
        stop(controls::search);
        stop(controls::list);
        stop(controls::details);
        stop(controls::cancel);
        stop(controls::use);
        stop(controls::done);
        stop(controls::close_mark);
        return;
    }
    stop(controls::filter_all);
    stop(controls::filter_installed);
    stop(controls::filter_downloads);
    for (const kit::ControlId id : building.tag_pills)
        stop(id);
    stop(controls::tags);
    stop(controls::search);
    stop(controls::list);
    for (const kit::ControlId id : building.option_ids)
        stop(id);
    stop(controls::clash);
    stop(controls::up);
    stop(controls::down);
    stop(controls::cancel);
    stop(controls::use);
    stop(controls::done);
    stop(controls::close_mark);
}

/// Appends the focus ring around the focused control, unless that control
/// draws its own or the ring would leave the window.
///
/// @param[in,out] building the layout
/// @param window the window
void add_focus_ring(Building& building, const Window& window) {
    const kit::Interaction& interaction = building.state->interaction;
    if (!interaction.focus_shown || interaction.focused == kit::no_control)
        return;
    if (interaction.focused == controls::search)
        return;
    for (const kit::Item& item : building.list.items) {
        if (item.control == interaction.focused && item.role == kit::Role::pill)
            return;
    }
    const kit::Rect whole{0, 0, window.width, window.height};
    for (const std::vector<kit::Control>* group :
         {&building.top, &building.panes, &building.detail, &building.footer}) {
        for (const kit::Control& control : *group) {
            if (control.id != interaction.focused || !control.focusable)
                continue;
            if (control.clip.width > 0 && control.clip.height > 0) {
                const kit::Rect shown = kit::intersect(control.rect, control.clip);
                if (shown.width <= 0 || shown.height <= 0)
                    return;
            }
            const bool on_edge = control.kind == kit::ControlKind::list;
            if (!on_edge &&
                !kit::wholly_in(kit::grown(control.rect, building.metrics->focus_inset), whole))
                return;
            add_item(building.list, kit::Role::focus_ring, control.rect, control.clip).look =
                kit::FocusRingLook{!on_edge};
            return;
        }
    }
}

/// Appends the open drop-down's menu over everything, and a control for each
/// item it shows.
///
/// @param[in,out] building the layout
/// @param footer_line the row the menu keeps above unless it opens upward
void add_open_menu(Building& building, int32_t footer_line) {
    if (!building.menu_open)
        return;
    const OpenMenu& menu_open = *building.menu_open;
    const kit::Metrics& metrics = *building.metrics;
    const auto count = static_cast<int32_t>(menu_open.items.size());
    const kit::Rect menu = kit::choice_menu(menu_open.field, menu_open.items.size(), footer_line);
    const int32_t shown_count = kit::shown_choices(menu_open.items.size());
    const int32_t first = std::clamp(
        building.state->menu_first, int32_t{0}, std::max(count - shown_count, int32_t{0})
    );
    int32_t marked = building.state->menu_marked;
    if (marked < 0 || marked >= count)
        marked = -1;
    kit::ChoiceMenuLook look;
    look.first = first;
    look.total = count;
    look.chosen = menu_open.chosen;
    look.marked = marked;
    look.focus_shown = building.state->interaction.focus_shown;
    // Every item is a named control, including those the menu has scrolled
    // out of the eight it shows. One past that window has no rectangle, so
    // it is not drawn and a press does not reach it.
    for (int32_t index = 0; index < count; ++index) {
        const MenuEntry& entry = menu_open.items[static_cast<std::size_t>(index)];
        const bool visible = index >= first && index < first + shown_count;
        kit::Control control;
        control.id = controls::first_menu_item + static_cast<kit::ControlId>(index);
        control.name = menu_open.name + "." + entry.word;
        keep_name(building, control.name);
        control.kind = kit::ControlKind::list_item;
        control.focusable = false;
        control.checked = index == menu_open.chosen;
        if (visible) {
            const kit::Rect box = kit::choice_item(menu, index - first);
            const std::string caption =
                cut(building,
                    kit::FontRole::regular,
                    entry.caption,
                    box.width - metrics.choice_item_text_inset - metrics.list_row_inset);
            look.shown.push_back(caption);
            control.rect = box;
            control.text = caption;
        } else {
            control.enabled = false;
            control.text = entry.caption;
        }
        building.menu.push_back(std::move(control));
    }
    add_item(building.list, kit::Role::choice_menu, menu, {}).look = std::move(look);
}

/// Finishes a layout: the focus ring, the open menu, and the controls in the
/// order a press tries them.
///
/// @param[in,out] building the layout
/// @param window the window
/// @return the display list
kit::DisplayList finish(Building& building, const Window& window) {
    add_focus_ring(building, window);
    add_open_menu(building, window.footer_rule);
    kit::DisplayList laid = std::move(building.list);
    laid.controls.clear();
    for (std::vector<kit::Control>* group :
         {&building.menu,
          &building.top,
          &building.rows,
          &building.panes,
          &building.detail,
          &building.footer}) {
        for (kit::Control& control : *group)
            laid.controls.push_back(std::move(control));
    }
    laid.tab_order = std::move(building.order);
    return laid;
}

/// Lays the picker out. `page` is Compact's details page: no list and no filters.
///
/// @param picker the picker
/// @param state the screen's state
/// @param frame the frame
/// @param fonts the fonts
/// @param page the details page
/// @return the display list
kit::DisplayList
lay(const Picker& picker,
    const ScreenState& state,
    const kit::Frame& frame,
    const kit::Fonts& fonts,
    bool page) {
    Building building;
    building.picker = &picker;
    building.state = &state;
    building.fonts = &fonts;
    building.metrics = &kit::metrics_of(frame.size_class);
    building.size_class = frame.size_class;
    const Window window = window_of(frame);
    const bool compact = frame.size_class == kit::SizeClass::compact;
    const std::vector<Row> shown_rows = rows(picker);
    building.number_width = measured_number_column(building, shown_rows);
    building.row_switch_width = measured_switch_width(building);
    const Footer foot = footer(picker);
    const std::string hint_text = hint(picker);
    const std::vector<std::string> tags = tags_shown(picker);

    add_item(building.list, kit::Role::fill, {0, 0, window.width, window.height}, {}).colour =
        kit::colour::panel;
    add_header(building, window);
    add_close_mark(building, window);
    if (!page)
        add_filters(building, window, tags);

    const int32_t content_w = std::max(window.right - window.left, int32_t{0});
    // The panes start a gap under the filter row, or under the header on
    // Compact's details page, and stop a space short of the footer's rule.
    int32_t body = window.body_top + window.gap;
    if (!page)
        body += window.control_height + window.gap;
    const int32_t body_h = std::max(window.footer_rule - window.bottom_gap - body, int32_t{0});
    if (page) {
        add_details(building, {window.left, body, content_w, body_h});
    } else if (compact) {
        add_list(building, {window.left, body, content_w, body_h}, shown_rows, hint_text);
    } else {
        int32_t list_w =
            frame.size_class == kit::SizeClass::large ? large_list_width : regular_list_width;
        if (list_w > content_w)
            list_w = content_w;
        const int32_t gap = window.padding;
        int32_t details_x = window.left + list_w + gap;
        int32_t details_w = std::max(window.right - details_x, int32_t{0});
        if (details_x > window.right) {
            details_x = window.right;
            details_w = 0;
        }
        add_list(building, {window.left, body, list_w, body_h}, shown_rows, hint_text);
        add_details(building, {details_x, body, details_w, body_h});
    }
    add_footer(building, window, foot, page);
    declare_tabs(building, page);
    return finish(building, window);
}

} // namespace

kit::Point window_size(const kit::Frame& frame) noexcept {
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    kit::Point size{metrics.dialog_width, metrics.dialog_height};
    if (frame.area.width > 0)
        size.x = std::min(size.x, frame.area.width);
    if (frame.area.height > 0)
        size.y = std::min(size.y, frame.area.height);
    return size;
}

kit::DisplayList picker_layout(
    const Picker& picker, const ScreenState& state, const kit::Frame& frame, const kit::Fonts& fonts
) {
    if (frame.size_class == kit::SizeClass::compact && state.details_page)
        return details_page_layout(picker, state, frame, fonts);
    return lay(picker, state, frame, fonts, false);
}

kit::DisplayList details_page_layout(
    const Picker& picker, const ScreenState& state, const kit::Frame& frame, const kit::Fonts& fonts
) {
    return lay(picker, state, frame, fonts, true);
}

} // namespace oa::ui::mutator_picker
