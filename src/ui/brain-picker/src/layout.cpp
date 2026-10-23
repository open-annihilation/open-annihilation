// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The brain picker's layout: a modal kit window at the size class's dialog
// size, the list of brains, the selected brain's details, and the footer.
// Which rows show, their order, their lines and what is locked come from
// the model. Nothing here takes a pointer, a key or a wheel.
#include "oa/ui/brain_picker/screen.hpp"

#include "internal.hpp"
#include "screen_controls.hpp"

#include "oa/ui/brain_picker/text.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/rows.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/library/screen.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/mutator_picker/model.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::brain_picker {
namespace {

/// How one option is drawn.
enum class OptionForm : uint8_t { levels, choice, toggle, slider };

/// The window's rows and columns at a size class.
struct Window {
    int32_t width{};       ///< the window's width, in points
    int32_t height{};      ///< the window's height, in points
    int32_t padding{};     ///< the class's padding
    int32_t origin{};      ///< the content's first column
    int32_t limit{};       ///< the column after the content's last
    int32_t body_top{};    ///< the first row under the header's rule
    int32_t footer_rule{}; ///< the row of the line over the footer
    int32_t footer_top{};  ///< the footer's first row
    int32_t button_top{};  ///< the footer's buttons' top row
    int32_t gap{};         ///< the rows between stacked parts
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
    int32_t label_width{0};             ///< the label's columns; 0 when it stands alone
    int32_t value_x{0};                 ///< the value's first column
    int32_t value_w{0};                 ///< the value's columns
    std::string label{};                ///< the label, as drawn
    std::vector<std::string> lines{};   ///< the value, wrapped
    kit::Colour ink{kit::colour::text}; ///< the value's colour
};

/// The level row, measured in the details' content.
struct LevelPlan {
    bool note{false};                    ///< the Difficulty note stands in for the strip
    bool locked{false};                  ///< the strip cannot be changed
    int32_t label_y{0};                  ///< the "Level" label's top
    int32_t control_y{0};                ///< the strip's or the note's top
    int32_t control_h{0};                ///< the strip's height
    int32_t control_w{0};                ///< the strip's width
    int32_t level_width{0};              ///< one level's columns
    std::size_t chosen{0};               ///< the chosen level
    std::string label{};                 ///< "Level", as drawn
    std::string note_text{};             ///< the Difficulty note, as drawn
    std::vector<std::string> captions{}; ///< the level names, as drawn
    std::vector<std::string> parts{};    ///< the level ids
};

/// The every-seat switch, measured in the details' content.
struct SwitchPlan {
    bool shown{false};         ///< the picker offers it
    bool on{false};            ///< it is on
    int32_t label_y{0};        ///< the sentence's top
    int32_t control_y{0};      ///< the switch's top
    int32_t control_w{0};      ///< the switch's width
    std::string label{};       ///< the sentence, as drawn
    std::string off_caption{}; ///< Off
    std::string on_caption{};  ///< On
};

/// The details' content, measured and not yet drawn.
struct DetailsPlan {
    int32_t width{0};                       ///< the content's width
    int32_t height{0};                      ///< the content's height
    std::string name{};                     ///< the brain's name, as drawn
    std::string version{};                  ///< the version and author, as drawn
    int32_t name_y{0};                      ///< the name's top
    int32_t name_x{0};                      ///< the name's first column
    int32_t name_w{0};                      ///< the name's columns
    std::vector<std::string> description{}; ///< the description, wrapped
    int32_t description_y{0};               ///< the description's top
    bool has_level{false};                  ///< a strip or a note is drawn
    LevelPlan level{};                      ///< the level row
    std::vector<OptionPlan> options{};      ///< the option controls
    std::vector<FactPlan> facts{};          ///< the facts
    SwitchPlan every{};                     ///< the every-seat switch
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
    std::vector<kit::Control> top{};          ///< the close mark and the hint
    std::vector<kit::Control> rows{};         ///< the list's rows
    std::vector<kit::Control> panes{};        ///< the list and its scroll bar
    std::vector<kit::Control> detail{};       ///< the details' controls
    std::vector<kit::Control> footer{};       ///< the footer's buttons and the summary
    std::vector<kit::ControlId> order{};      ///< Tab's order
    std::vector<std::string> names{};         ///< the names given so far
    std::vector<kit::ControlId> option_ids{}; ///< the selected brain's option controls
    bool level_stop{false};                   ///< the level strip takes a Tab stop
    bool every_stop{false};                   ///< the every-seat switch takes a Tab stop
    std::optional<OpenMenu> menu_open{};      ///< the open drop-down
};

/// Returns the window's rows and columns for a frame.
///
/// @param frame the frame
/// @return the window
Window window_of(const kit::Frame& frame) {
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    const kit::Point size = window_size(frame);
    Window window;
    window.width = size.x;
    window.height = size.y;
    window.padding = metrics.padding;
    window.origin = metrics.padding;
    window.limit = size.x - metrics.edge - metrics.padding;
    window.body_top = metrics.edge + metrics.header_height + 1;
    window.footer_rule = size.y - metrics.edge - metrics.footer_height - 1;
    window.footer_top = window.footer_rule + 1;
    window.button_top = window.footer_top + (metrics.footer_height - metrics.button_height) / 2;
    window.gap = metrics.paragraph_gap;
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
    for (int32_t tally = 2;
         std::find(building.names.begin(), building.names.end(), given) != building.names.end();
         ++tally)
        given = name + "-" + std::to_string(tally);
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
    for (int32_t tally = 2; std::find(used.begin(), used.end(), given) != used.end(); ++tally)
        given = word + "-" + std::to_string(tally);
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
    std::string kept(piece);
    while (!kept.empty()) {
        std::size_t end = kept.size();
        do {
            --end;
        } while (end > 0 && kit::continuation_byte(kept[end]));
        kept.resize(end);
        const std::string candidate = kit::cut_to_width(*building.fonts, role, kept, width);
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

/// Returns the colour a row's line, or a fact's value, is drawn in.
///
/// @param tone the tone
/// @return the colour
kit::Colour tone_colour(Tone tone) noexcept {
    if (tone == Tone::lock)
        return kit::colour::lock;
    if (tone == Tone::hint)
        return kit::colour::hint;
    return kit::colour::hint;
}

/// Returns a fact value's colour. A plain fact is ordinary text.
///
/// @param tone the tone
/// @return the colour
kit::Colour fact_colour(Tone tone) noexcept {
    if (tone == Tone::lock)
        return kit::colour::lock;
    if (tone == Tone::hint)
        return kit::colour::hint;
    return kit::colour::text;
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

/// Appends a brain's badge: the picture when it can be drawn, else the letters.
///
/// @param[in,out] building the layout
/// @param brain the brain; null for the built-in player
/// @param letters_of the name the letters are taken from
/// @param rect the badge's square
/// @param clip what drawing may touch
void add_brain_badge(
    Building& building,
    const Brain* brain,
    std::string_view letters_of,
    const kit::Rect& rect,
    const kit::Rect& clip
) {
    kit::BadgeLook look;
    const oa::ui::frontend_renderer::RgbaPicture picture =
        picture_of(brain == nullptr ? nullptr : brain->badge);
    if (oa::ui::frontend_renderer::picture_drawable(picture))
        look.picture = picture;
    else
        look.letters = library::badge_letters(letters_of);
    kit::add_badge(building.list, rect, look);
    if (!building.list.items.empty())
        building.list.items.back().clip = clip;
}

/// Returns a button's width: the class's button, or its words plus the padding.
///
/// @param building the layout
/// @param caption the words, already looked up
/// @return the width, in points
int32_t button_span(const Building& building, std::string_view caption) {
    const int32_t words = kit::text_width(*building.fonts, kit::FontRole::small, caption);
    return std::max(
        building.metrics->button_width, words + building.metrics->prompt_button_padding
    );
}

/// Returns a row's height at this class, in points.
///
/// @param building the layout
/// @return the height
int32_t row_height_of(const Building& building) noexcept {
    if (building.size_class == kit::SizeClass::compact)
        return compact_row_height;
    if (building.size_class == kit::SizeClass::large)
        return large_row_height;
    return regular_row_height;
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

/// Appends the header: "Computer player", the seat and the mod, and room for
/// the close mark.
///
/// The kit's header keeps Compact's places from the window's edges at every class.
///
/// @param[in,out] building the layout
/// @param window the window
void add_header(Building& building, const Window& window) {
    const kit::Metrics& compact = kit::compact_metrics;
    const library::TextHooks& hooks = building.picker->inputs.text;
    const int32_t tracking = compact.heading_tracking;
    const int32_t right_inset = compact.button_height + compact.header_gap;
    const int32_t title_origin = compact.padding + compact.mark_side + compact.header_gap;
    const int32_t right_edge = window.width - compact.edge - compact.padding - right_inset;
    const int32_t most_for_title =
        std::max(right_edge - title_origin - compact.header_gap, int32_t{0});

    std::string title(library::shown(hooks, words::title_player));
    title = cut_tracked(building, kit::FontRole::regular, title, most_for_title, tracking);
    const int32_t title_width =
        kit::tracked_width(*building.fonts, kit::FontRole::regular, title, tracking);
    const int32_t version_room =
        std::max(right_edge - (title_origin + title_width + compact.header_gap), int32_t{0});
    const std::string seat = building.picker->inputs.seat_name;
    const std::string mod = building.picker->inputs.mod_name.empty()
                                ? std::string(library::shown(hooks, words::no_mod))
                                : building.picker->inputs.mod_name;
    std::string context =
        library::filled(hooks, words::context_seat, {{"seat", seat}, {"mod", mod}});
    context = cut(building, kit::FontRole::small, context, version_room);

    kit::HeaderLook look;
    look.width = window.width;
    look.tracking = tracking;
    look.right_inset = right_inset;
    look.mark = true;
    look.title = std::move(title);
    look.title_width = title_width;
    look.version = std::move(context);
    look.version_width = kit::text_width(*building.fonts, kit::FontRole::small, look.version);
    const int32_t header_rule = compact.edge + compact.header_height;
    add_item(building.list, kit::Role::header, {0, 0, window.width, header_rule + 1}, {}).look =
        std::move(look);
}

/// Appends the header's close mark. Tab's order gains it last.
///
/// @param[in,out] building the layout
/// @param window the window
void add_close_mark(Building& building, const Window& window) {
    const kit::Metrics& compact = kit::compact_metrics;
    const int32_t side = compact.button_height;
    const kit::Rect rect{
        window.width - compact.edge - compact.padding - side,
        compact.edge + (compact.header_height - side) / 2,
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

/// Appends the hint under the header, in the lock colour, and returns how
/// tall the band is, including the gap under it. Empty when the model has
/// nothing to say.
///
/// @param[in,out] building the layout
/// @param window the window
/// @param hint_text the hint
/// @return the band's height, in points
int32_t add_hint(Building& building, const Window& window, const std::string& hint_text) {
    if (hint_text.empty())
        return 0;
    const kit::Metrics& metrics = *building.metrics;
    const bool compact = building.size_class == kit::SizeClass::compact;
    const int32_t width = std::max(window.limit - window.origin, int32_t{0});
    const std::vector<std::string> lines = wrap_lines(
        building, kit::FontRole::small, hint_text, width, compact ? std::size_t{1} : std::size_t{2}
    );
    if (lines.empty())
        return 0;
    int32_t line_y = window.body_top;
    for (const std::string& line : lines) {
        add_text(
            building,
            kit::FontRole::small,
            line,
            {window.origin, line_y, width, metrics.small_line},
            kit::Align::left,
            kit::colour::lock,
            {}
        );
        line_y += metrics.small_line;
    }
    const int32_t hint_h = static_cast<int32_t>(lines.size()) * metrics.small_line;
    const std::string name(name_hint);
    keep_name(building, name);
    add_label(
        building.top,
        controls::hint_label,
        {window.origin, window.body_top, width, hint_h},
        name,
        joined(lines),
        {},
        -1
    );
    return hint_h + metrics.paragraph_gap;
}

/// Appends one row, clipped to the list.
///
/// @param[in,out] building the layout
/// @param shown_rows the rows
/// @param place the row's place
/// @param rect the row, in window points
/// @param clip the list's view
void add_row(
    Building& building,
    const std::vector<Row>& shown_rows,
    std::size_t place,
    const kit::Rect& rect,
    const kit::Rect& clip
) {
    const kit::Metrics& metrics = *building.metrics;
    const Picker& picker = *building.picker;
    const Row& row = shown_rows[place];
    const bool compact = building.size_class == kit::SizeClass::compact;
    const bool selected = picker.selected == row.id;
    const kit::ControlId row_id =
        controls::first_row + static_cast<kit::ControlId>(static_cast<int32_t>(place));

    add_item(building.list, kit::Role::fill, rect, clip).colour =
        selected ? kit::colour::list_selected : kit::colour::list;
    add_item(
        building.list,
        kit::Role::rule,
        {rect.x, rect.y + rect.height - metrics.hairline, rect.width, metrics.hairline},
        clip
    )
        .colour = kit::colour::rule;

    int32_t text_x = rect.x + metrics.list_row_inset;
    const Brain* brain = row.id.empty() ? nullptr : detail::find_brain(picker, row.id);
    const kit::Rect badge{
        text_x,
        rect.y + (rect.height - row_badge_side) / 2,
        row_badge_side,
        row_badge_side,
    };
    add_brain_badge(building, brain, row.name, badge, clip);
    text_x += row_badge_side + metrics.list_row_gap;

    int32_t text_right = rect.x + rect.width - metrics.list_row_inset;
    if (row.chosen) {
        const int32_t icon = metrics.state_icon_side;
        if (text_right - icon >= text_x) {
            text_right -= icon;
            kit::ChipLook mark;
            mark.icon = kit::StateIcon::circled_check;
            mark.state = kit::ChipState::installed;
            const kit::Rect mark_rect{
                text_right,
                rect.y + (rect.height - icon) / 2,
                icon,
                icon,
            };
            kit::add_chip(building.list, mark_rect, mark, kit::no_control, {});
            if (!building.list.items.empty())
                building.list.items.back().clip = clip;
            text_right -= metrics.list_row_gap;
        }
    }
    if (!compact && row.full_awareness) {
        kit::ChipLook chip;
        chip.text =
            std::string(library::shown(picker.inputs.text, library::words::chip_full_awareness));
        chip.state = kit::ChipState::update;
        const int32_t chip_w = kit::chip_width(*building.fonts, chip);
        if (chip_w > 0 && text_right - chip_w >= text_x) {
            text_right -= chip_w;
            const kit::Rect chip_rect{
                text_right,
                rect.y + (rect.height - metrics.chip_height) / 2,
                chip_w,
                metrics.chip_height,
            };
            const std::string drawn =
                cut(building,
                    kit::FontRole::small,
                    chip.text,
                    std::max(chip_w - 2 * metrics.chip_padding, int32_t{0}));
            if (!drawn.empty())
                chip.text = drawn;
            kit::add_chip(building.list, chip_rect, chip, kit::no_control, {});
            if (!building.list.items.empty())
                building.list.items.back().clip = clip;
            text_right -= metrics.list_row_gap;
        }
    }

    const int32_t text_w = std::max(text_right - text_x, int32_t{0});
    const int32_t block = metrics.regular_line + metrics.small_line;
    const int32_t text_y = rect.y + std::max((rect.height - block) / 2, int32_t{0});
    add_text(
        building,
        kit::FontRole::regular,
        cut(building, kit::FontRole::regular, row.name, text_w),
        {text_x, text_y, text_w, metrics.regular_line},
        kit::Align::left,
        kit::colour::text,
        clip
    );
    add_text(
        building,
        kit::FontRole::small,
        cut(building, kit::FontRole::small, one_line(row.line), text_w),
        {text_x, text_y + metrics.regular_line, text_w, metrics.small_line},
        kit::Align::left,
        tone_colour(row.tone),
        clip
    );

    if (selected)
        add_item(building.list, kit::Role::outline, rect, clip).colour = kit::colour::accent;
    if (row.locked)
        add_item(building.list, kit::Role::locked_fade, rect, clip);

    const std::string row_name = row.id.empty()
                                     ? std::string(name_row_prefix) + "built-in"
                                     : std::string(name_row_prefix) + controls::name_word(row.id);
    kit::Control control;
    control.id = row_id;
    control.rect = rect;
    control.clip = clip;
    control.enabled = true;
    control.focusable = false;
    control.name = unique_name(building, row_name);
    control.kind = kit::ControlKind::list_item;
    control.group = controls::list_group;
    control.checked = selected;
    control.text = row.name;
    building.rows.push_back(std::move(control));
}

/// The lines of the empty-list sentence at a width.
///
/// @param building the layout
/// @param width the room, in points
/// @return the lines
std::vector<std::string> empty_lines(const Building& building, int32_t width) {
    const std::string sentence(library::shown(building.picker->inputs.text, words::empty_brains));
    return wrap_lines(building, kit::FontRole::small, sentence, width, std::size_t{4});
}

/// Appends the sentence shown under the built-in row when it is the only one.
///
/// @param[in,out] building the layout
/// @param port the list's view
/// @param top the sentence's top, in window points
/// @param lines the lines, already wrapped to the port
void add_empty_sentence(
    Building& building, const kit::Rect& port, int32_t top, const std::vector<std::string>& lines
) {
    const kit::Metrics& metrics = *building.metrics;
    int32_t line_y = top;
    for (const std::string& line : lines) {
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
}

/// Appends the list pane and its scroll bar.
///
/// @param[in,out] building the layout
/// @param pane the pane
/// @param shown_rows the rows the model gives
void add_list(Building& building, const kit::Rect& pane, const std::vector<Row>& shown_rows) {
    const kit::Metrics& metrics = *building.metrics;
    const int32_t well_width = metrics.scroll_thumb_width + 2;
    const int32_t bar_gap = metrics.button_gap;
    const int32_t row_h = row_height_of(building);
    add_item(building.list, kit::Role::fill, pane, {}).colour = kit::colour::list;

    const bool alone = shown_rows.size() == 1;
    const auto sentence_block = [&](int32_t width) {
        if (!alone)
            return int32_t{0};
        const int32_t lines = static_cast<int32_t>(empty_lines(building, width).size());
        if (lines <= 0)
            return int32_t{0};
        return metrics.paragraph_gap + lines * metrics.small_line;
    };
    int32_t content_h =
        static_cast<int32_t>(shown_rows.size()) * row_h + sentence_block(pane.width);
    const bool scrolling = content_h > pane.height;
    const int32_t content_w =
        scrolling ? std::max(pane.width - well_width - bar_gap, int32_t{0}) : pane.width;
    if (scrolling)
        content_h = static_cast<int32_t>(shown_rows.size()) * row_h + sentence_block(content_w);
    const kit::Rect port{pane.x, pane.y, content_w, pane.height};
    const int32_t limit = kit::ScrollArea::limit_for(content_h, port.height);
    const int32_t offset = std::clamp(building.state->list_scroll, int32_t{0}, limit);

    for (std::size_t place = 0; place < shown_rows.size(); ++place) {
        const int32_t window_y = port.y - offset + static_cast<int32_t>(place) * row_h;
        add_row(building, shown_rows, place, {port.x, window_y, port.width, row_h}, port);
    }
    if (alone) {
        const std::vector<std::string> lines = empty_lines(building, port.width);
        const int32_t sentence_y = port.y - offset +
                                   static_cast<int32_t>(shown_rows.size()) * row_h +
                                   metrics.paragraph_gap;
        add_empty_sentence(building, port, sentence_y, lines);
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
    area.content_height = content_h;
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

/// Returns the caption a choice or a toggle shows for one value.
///
/// @param spec the option
/// @param place the value's place
/// @return the caption, or the value when it names none
std::string caption_at(const mutator_picker::OptionSpec& spec, std::size_t place) {
    if (place < spec.captions.size() && !spec.captions[place].empty())
        return spec.captions[place];
    if (place < spec.values.size())
        return spec.values[place];
    return {};
}

/// Measures one option into the plan and advances the pen.
///
/// @param building the layout
/// @param spec the option
/// @param place the option's place
/// @param value the value the details show
/// @param locked the row is not the choice, so the control cannot change it
/// @param width the content's width
/// @param[in,out] pen the next row
/// @return the measured option
OptionPlan measure_option(
    const Building& building,
    const mutator_picker::OptionSpec& spec,
    std::size_t place,
    std::string_view value,
    bool locked,
    int32_t width,
    int32_t& pen
) {
    const kit::Metrics& metrics = *building.metrics;
    OptionPlan option;
    option.spec = place;
    option.locked = locked;
    pen += metrics.paragraph_gap;
    option.label_y = pen;
    option.label = cut(building, kit::FontRole::regular, spec.label, width);
    pen += metrics.regular_line + metrics.hint_gap;
    option.control_y = pen;
    std::size_t chosen_place = 0;
    for (std::size_t slot = 0; slot < spec.values.size(); ++slot) {
        if (spec.values[slot] == value)
            chosen_place = slot;
    }
    if (spec.type == mutator_picker::OptionSpec::Type::toggle) {
        option.form = OptionForm::toggle;
        option.control_h = metrics.button_height;
        option.on = spec.values.size() > 1 && value == spec.values[1];
        option.off_caption = caption_at(spec, 0);
        option.on_caption = caption_at(spec, 1);
        const int32_t off_width =
            kit::text_width(*building.fonts, kit::FontRole::small, option.off_caption);
        const int32_t on_width =
            kit::text_width(*building.fonts, kit::FontRole::small, option.on_caption);
        option.control_w = kit::switch_width_for(std::max(off_width, on_width));
        if (width > 0 && option.control_w > width) {
            option.control_w = width;
            const int32_t room = std::max((option.control_w - 2) / 2, int32_t{0});
            option.off_caption = cut(building, kit::FontRole::small, option.off_caption, room);
            option.on_caption = cut(building, kit::FontRole::small, option.on_caption, room);
        }
    } else if (spec.type == mutator_picker::OptionSpec::Type::number) {
        option.form = OptionForm::slider;
        option.control_h = std::max(metrics.knob_height, metrics.small_line);
        const std::vector<std::string> stops = mutator_picker::number_values(spec);
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
        option.value_w = std::min(metrics.slider_value_width, std::max(width, int32_t{0}));
        const int32_t room = std::max(width - metrics.slider_value_gap, int32_t{0});
        if (option.value_w > room)
            option.value_w = room;
        option.control_w = std::max(width - option.value_w - metrics.slider_value_gap, int32_t{0});
        option.value_x = option.control_w + metrics.slider_value_gap;
        option.value_text = cut(building, kit::FontRole::small, value, option.value_w);
    } else if (spec.values.size() >= 1 && spec.values.size() <= 4) {
        option.form = OptionForm::levels;
        option.control_h = metrics.button_height;
        option.chosen = chosen_place;
        std::vector<std::string> used;
        int32_t widest = 0;
        for (std::size_t slot = 0; slot < spec.values.size(); ++slot) {
            std::string caption = caption_at(spec, slot);
            widest =
                std::max(widest, kit::text_width(*building.fonts, kit::FontRole::small, caption));
            option.parts.push_back(unique_word(used, controls::name_word(caption)));
            option.captions.push_back(std::move(caption));
        }
        option.level_width = widest + 2 * metrics.chip_padding;
        const int32_t tally = static_cast<int32_t>(option.captions.size());
        int32_t strip = 2 + tally * option.level_width;
        if (tally > 0 && strip > width) {
            option.level_width = std::max((width - 2) / tally, int32_t{1});
            strip = 2 + tally * option.level_width;
            const int32_t caption_room =
                std::max(option.level_width - 2 * metrics.chip_padding, int32_t{0});
            for (std::string& caption : option.captions)
                caption = cut(building, kit::FontRole::small, caption, caption_room);
        }
        option.control_w = std::min(strip, std::max(width, int32_t{0}));
    } else {
        option.form = OptionForm::choice;
        option.control_h = metrics.field_height;
        option.control_w = width;
        option.chosen = chosen_place;
        option.locked = locked || spec.values.empty();
        std::vector<std::string> used;
        if (spec.values.empty()) {
            option.choice_text =
                cut(building,
                    kit::FontRole::regular,
                    value,
                    option.control_w - metrics.choice_text_inset - metrics.choice_arrow_room);
        }
        for (std::size_t slot = 0; slot < spec.values.size(); ++slot) {
            std::string caption = caption_at(spec, slot);
            option.parts.push_back(unique_word(used, controls::name_word(caption)));
            option.captions.push_back(std::move(caption));
        }
        if (!option.captions.empty()) {
            const std::string& shown_choice = option.chosen < option.captions.size()
                                                  ? option.captions[option.chosen]
                                                  : option.captions.front();
            option.choice_text =
                cut(building,
                    kit::FontRole::regular,
                    shown_choice,
                    option.control_w - metrics.choice_text_inset - metrics.choice_arrow_room);
        }
    }
    pen += option.control_h;
    return option;
}

/// Measures the level row and advances the pen. A note replaces the strip.
///
/// @param building the layout
/// @param shown the details
/// @param width the content's width
/// @param[in,out] pen the next row
/// @return the row; has nothing when the brain offers no level and no note
LevelPlan
measure_level(const Building& building, const Details& shown, int32_t width, int32_t& pen) {
    LevelPlan level;
    if (shown.level_note.empty() && shown.levels.empty())
        return level;
    const kit::Metrics& metrics = *building.metrics;
    const library::TextHooks& hooks = building.picker->inputs.text;
    pen += metrics.paragraph_gap;
    level.label_y = pen;
    level.label = cut(building, kit::FontRole::regular, library::shown(hooks, words::level), width);
    pen += metrics.regular_line + metrics.hint_gap;
    level.control_y = pen;
    if (!shown.level_note.empty()) {
        level.note = true;
        level.note_text = cut(building, kit::FontRole::small, shown.level_note, width);
        pen += metrics.small_line;
        return level;
    }
    level.locked = shown.levels_locked;
    level.control_h = metrics.button_height;
    level.chosen = shown.level < shown.levels.size() ? shown.level : std::size_t{0};
    std::vector<std::string> used;
    int32_t widest = 0;
    for (const Level& one : shown.levels) {
        widest = std::max(widest, kit::text_width(*building.fonts, kit::FontRole::small, one.name));
        level.parts.push_back(unique_word(used, one.id.empty() ? std::string("x") : one.id));
        level.captions.push_back(one.name);
    }
    level.level_width = widest + 2 * metrics.chip_padding;
    const int32_t tally = static_cast<int32_t>(level.captions.size());
    int32_t strip = tally == 0 ? 0 : 2 + tally * level.level_width;
    if (tally > 0 && strip > width) {
        level.level_width = std::max((width - 2) / tally, int32_t{1});
        strip = 2 + tally * level.level_width;
        const int32_t caption_room =
            std::max(level.level_width - 2 * metrics.chip_padding, int32_t{0});
        for (std::string& caption : level.captions)
            caption = cut(building, kit::FontRole::small, caption, caption_room);
    }
    level.control_w = std::min(strip, std::max(width, int32_t{0}));
    pen += level.control_h;
    return level;
}

/// Measures the details at a width. Adds nothing.
///
/// @param building the layout
/// @param shown the model's details
/// @param width the content's width, in points
/// @return the plan
DetailsPlan plan_details(const Building& building, const Details& shown, int32_t width) {
    const kit::Metrics& metrics = *building.metrics;
    const Picker& picker = *building.picker;
    const library::TextHooks& hooks = picker.inputs.text;
    const bool compact = building.size_class == kit::SizeClass::compact;
    DetailsPlan plan;
    plan.width = std::max(width, int32_t{0});
    plan.name = shown.name;
    plan.version = shown.version_author;
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
        wrap_lines(building, kit::FontRole::small, shown.description, plan.width, most_lines);
    int32_t pen =
        plan.description_y + static_cast<int32_t>(plan.description.size()) * metrics.small_line;

    plan.level = measure_level(building, shown, plan.width, pen);
    plan.has_level = !shown.level_note.empty() || !shown.levels.empty();

    for (std::size_t place = 0; place < shown.options.size(); ++place) {
        const std::string value = place < shown.values.size() ? shown.values[place] : std::string{};
        plan.options.push_back(measure_option(
            building, shown.options[place], place, value, !shown.chosen, plan.width, pen
        ));
    }

    if (!shown.facts.empty()) {
        pen += metrics.paragraph_gap;
        int32_t label_width = 0;
        bool any_label = false;
        for (const Fact& fact : shown.facts) {
            if (fact.label.empty())
                continue;
            any_label = true;
            label_width = std::max(
                label_width, kit::text_width(*building.fonts, kit::FontRole::small, fact.label)
            );
        }
        const int32_t half = std::max(plan.width / 2, int32_t{0});
        if (label_width > half)
            label_width = half;
        const int32_t value_x = any_label ? label_width + metrics.label_gap : 0;
        const int32_t value_w = std::max(plan.width - value_x, int32_t{0});
        for (const Fact& fact : shown.facts) {
            FactPlan drawn;
            drawn.y = pen;
            const bool alone = fact.label.empty();
            drawn.label_width = alone ? 0 : label_width;
            drawn.value_x = alone ? 0 : value_x;
            drawn.value_w = alone ? plan.width : value_w;
            if (!alone)
                drawn.label = cut(building, kit::FontRole::small, fact.label, label_width);
            drawn.lines = wrap_lines(
                building, kit::FontRole::small, fact.value, drawn.value_w, std::size_t{6}
            );
            drawn.ink = fact_colour(fact.tone);
            drawn.height =
                std::max(static_cast<int32_t>(drawn.lines.size()), int32_t{1}) * metrics.small_line;
            pen += drawn.height;
            plan.facts.push_back(std::move(drawn));
        }
    }

    if (picker.inputs.offer_every_seat) {
        pen += metrics.paragraph_gap;
        plan.every.shown = true;
        plan.every.on = picker.every_seat;
        plan.every.label_y = pen;
        plan.every.label =
            cut(building,
                kit::FontRole::regular,
                library::shown(hooks, words::use_every_seat),
                plan.width);
        pen += metrics.regular_line + metrics.hint_gap;
        plan.every.control_y = pen;
        plan.every.off_caption = std::string(library::shown(hooks, words::switch_off));
        plan.every.on_caption = std::string(library::shown(hooks, words::switch_on));
        const int32_t off_width =
            kit::text_width(*building.fonts, kit::FontRole::small, plan.every.off_caption);
        const int32_t on_width =
            kit::text_width(*building.fonts, kit::FontRole::small, plan.every.on_caption);
        plan.every.control_w = kit::switch_width_for(std::max(off_width, on_width));
        if (plan.width > 0 && plan.every.control_w > plan.width) {
            plan.every.control_w = plan.width;
            const int32_t room = std::max((plan.every.control_w - 2) / 2, int32_t{0});
            plan.every.off_caption =
                cut(building, kit::FontRole::small, plan.every.off_caption, room);
            plan.every.on_caption =
                cut(building, kit::FontRole::small, plan.every.on_caption, room);
        }
        pen += metrics.button_height;
    }
    plan.height = pen;
    return plan;
}

/// Appends the measured details into a view, scrolled by an offset.
///
/// @param[in,out] building the layout
/// @param plan the measured details
/// @param shown the model's details
/// @param port the view
/// @param offset how far the content is scrolled
void emit_details(
    Building& building,
    const DetailsPlan& plan,
    const Details& shown,
    const kit::Rect& port,
    int32_t offset
) {
    const kit::Metrics& metrics = *building.metrics;
    const Picker& picker = *building.picker;
    const auto at = [&](int32_t x, int32_t y, int32_t w, int32_t h) {
        return kit::Rect{
            port.x + x, port.y + y - offset, std::max(w, int32_t{0}), std::max(h, int32_t{0})
        };
    };
    const Brain* brain =
        picker.selected.empty() ? nullptr : detail::find_brain(picker, picker.selected);
    add_brain_badge(
        building, brain, shown.name, at(0, 0, details_badge_side, details_badge_side), port
    );
    add_text(
        building,
        kit::FontRole::regular,
        plan.name,
        at(plan.name_x, plan.name_y, plan.name_w, metrics.regular_line),
        kit::Align::left,
        kit::colour::text,
        port
    );
    const std::string brain_name(name_brain);
    keep_name(building, brain_name);
    add_label(
        building.detail,
        controls::name_label,
        at(plan.name_x, plan.name_y, std::max(plan.name_w, int32_t{1}), metrics.regular_line),
        brain_name,
        plan.name.empty() ? shown.name : plan.name,
        port,
        controls::details_group
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

    if (plan.has_level) {
        add_text(
            building,
            kit::FontRole::regular,
            plan.level.label,
            at(0, plan.level.label_y, plan.width, metrics.regular_line),
            kit::Align::left,
            kit::colour::text,
            port
        );
        if (plan.level.note) {
            add_text(
                building,
                kit::FontRole::small,
                plan.level.note_text,
                at(0, plan.level.control_y, plan.width, metrics.small_line),
                kit::Align::left,
                kit::colour::hint,
                port
            );
        } else if (!plan.level.captions.empty()) {
            kit::LevelsLook look;
            look.captions = plan.level.captions;
            look.level_width = plan.level.level_width;
            look.chosen = plan.level.chosen;
            look.offered = plan.level.captions.size();
            look.locked = plan.level.locked;
            look.hovered = hovered(building, controls::level, !plan.level.locked);
            const std::string level_name(name_level);
            keep_name(building, level_name);
            const std::size_t before = building.list.controls.size();
            kit::add_levels(
                building.list,
                at(0, plan.level.control_y, plan.level.control_w, plan.level.control_h),
                look,
                controls::level,
                level_name
            );
            move_controls(building.list, before, building.detail);
            kit::Control& control = building.detail.back();
            control.clip = port;
            control.group = controls::details_group;
            control.parts = plan.level.parts;
            if (plan.level.locked) {
                control.enabled = false;
                control.focusable = false;
            }
            if (!building.list.items.empty())
                building.list.items.back().clip = port;
            building.level_stop = control.enabled && control.focusable;
        }
    }

    for (std::size_t place = 0; place < plan.options.size(); ++place) {
        const OptionPlan& option = plan.options[place];
        const mutator_picker::OptionSpec& spec = shown.options[option.spec];
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

    for (const FactPlan& fact : plan.facts) {
        if (!fact.label.empty()) {
            add_text(
                building,
                kit::FontRole::small,
                fact.label,
                at(0, fact.y, fact.label_width, metrics.small_line),
                kit::Align::left,
                kit::colour::hint,
                port
            );
        }
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

    if (plan.every.shown) {
        add_text(
            building,
            kit::FontRole::regular,
            plan.every.label,
            at(0, plan.every.label_y, plan.width, metrics.regular_line),
            kit::Align::left,
            kit::colour::text,
            port
        );
        kit::SwitchLook look;
        look.on = plan.every.on;
        look.locked = false;
        look.off_caption = plan.every.off_caption;
        look.on_caption = plan.every.on_caption;
        look.hovered = hovered(building, controls::every_seat, true);
        const std::string switch_name(name_every_seat);
        keep_name(building, switch_name);
        const std::size_t before = building.list.controls.size();
        kit::add_switch(
            building.list,
            at(0, plan.every.control_y, plan.every.control_w, metrics.button_height),
            look,
            controls::every_seat,
            switch_name
        );
        move_controls(building.list, before, building.detail);
        kit::Control& control = building.detail.back();
        control.clip = port;
        control.group = controls::details_group;
        control.parts = {"off", "on"};
        if (!building.list.items.empty())
            building.list.items.back().clip = port;
        building.every_stop = true;
    }
}

/// Appends the details pane, scrolled when it holds more than it shows.
///
/// @param[in,out] building the layout
/// @param pane the pane
/// @param shown the model's details
void add_details(Building& building, const kit::Rect& pane, const Details& shown) {
    const kit::Metrics& metrics = *building.metrics;
    add_item(building.list, kit::Role::fill, pane, {}).colour = kit::colour::list;
    const int32_t well_width = metrics.scroll_thumb_width + 2;
    const int32_t bar_gap = metrics.button_gap;
    const DetailsPlan wide = plan_details(building, shown, pane.width);
    const bool scrolling = wide.height > pane.height;
    const int32_t content_w =
        scrolling ? std::max(pane.width - well_width - bar_gap, int32_t{0}) : pane.width;
    const DetailsPlan plan = scrolling ? plan_details(building, shown, content_w) : wide;
    const kit::Rect port{pane.x, pane.y, content_w, pane.height};
    const int32_t limit = kit::ScrollArea::limit_for(plan.height, port.height);
    const int32_t offset = std::clamp(building.state->details_scroll, int32_t{0}, limit);
    emit_details(building, plan, shown, port, offset);
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
    building.panes.push_back(std::move(bar));
}

/// Appends one footer button, placed from a column that moves left.
///
/// @param[in,out] building the layout
/// @param[in,out] cursor the column the button's right edge ends at
/// @param window the window
/// @param id the control
/// @param name its name
/// @param caption its words, already looked up
/// @param accent the accent look when true
void add_footer_button(
    Building& building,
    int32_t& cursor,
    const Window& window,
    kit::ControlId id,
    std::string_view control_name,
    std::string caption,
    bool accent
) {
    const kit::Metrics& metrics = *building.metrics;
    const int32_t width = button_span(building, caption);
    cursor -= width;
    kit::ButtonLook look;
    look.caption = caption;
    look.style = accent ? kit::ButtonStyle::accent : kit::ButtonStyle::plain;
    look.enabled = true;
    look.hovered = hovered(building, id, true);
    look.held = held(building, id, true);
    const kit::Rect rect{cursor, window.button_top, width, metrics.button_height};
    cursor -= metrics.button_gap;
    const std::size_t before = building.list.controls.size();
    keep_name(building, std::string(control_name));
    kit::add_button(building.list, rect, look, id, std::string(control_name));
    move_controls(building.list, before, building.footer);
}

/// Appends the footer band, the summary and the buttons.
///
/// Compact's list shows DETAILS. The details page shows BACK. USE THIS is
/// always enabled: the model decides what the choice is.
///
/// @param[in,out] building the layout
/// @param window the window
/// @param page Compact's details page
void add_footer(Building& building, const Window& window, bool page) {
    const kit::Metrics& metrics = *building.metrics;
    const library::TextHooks& hooks = building.picker->inputs.text;
    const bool compact = building.size_class == kit::SizeClass::compact;
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

    int32_t cursor = window.limit;
    add_footer_button(
        building,
        cursor,
        window,
        controls::use,
        name_use,
        std::string(library::shown(hooks, words::use_this)),
        true
    );
    if (page) {
        add_footer_button(
            building,
            cursor,
            window,
            controls::back,
            name_back,
            std::string(library::shown(hooks, words::back)),
            false
        );
    } else {
        add_footer_button(
            building,
            cursor,
            window,
            controls::cancel,
            name_cancel,
            std::string(library::shown(hooks, words::cancel)),
            false
        );
        if (compact) {
            add_footer_button(
                building,
                cursor,
                window,
                controls::details,
                name_details,
                std::string(library::shown(hooks, words::details)),
                false
            );
        }
    }

    const int32_t summary_w = std::max(cursor - window.origin, int32_t{0});
    if (summary_w <= 0)
        return;
    const std::string source = summary(*building.picker);
    const std::string drawn = cut(building, kit::FontRole::small, source, summary_w);
    const int32_t summary_y = window.footer_top + (metrics.footer_height - metrics.small_line) / 2;
    const kit::Rect rect{window.origin, summary_y, summary_w, metrics.small_line};
    add_text(building, kit::FontRole::small, drawn, rect, kit::Align::left, kit::colour::hint, {});
    const std::string summary_name(name_summary);
    keep_name(building, summary_name);
    add_label(building.footer, controls::summary_label, rect, summary_name, drawn, {}, -1);
}

/// Declares Tab's order. A control that is missing or disabled is left out,
/// as is one clipped away, except a details control.
///
/// The close mark stays on the details page so a pointer press on it is a
/// key press too.
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
        if (building.level_stop)
            stop(controls::level);
        for (const kit::ControlId id : building.option_ids)
            stop(id);
        if (building.every_stop)
            stop(controls::every_seat);
        stop(controls::back);
        stop(controls::use);
        stop(controls::close_mark);
        return;
    }
    if (compact) {
        stop(controls::list);
        stop(controls::details);
        stop(controls::cancel);
        stop(controls::use);
        stop(controls::close_mark);
        return;
    }
    stop(controls::list);
    if (building.level_stop)
        stop(controls::level);
    for (const kit::ControlId id : building.option_ids)
        stop(id);
    if (building.every_stop)
        stop(controls::every_seat);
    stop(controls::cancel);
    stop(controls::use);
    stop(controls::close_mark);
}

/// Appends the focus ring around the focused control, unless the ring would
/// leave the window.
///
/// @param[in,out] building the layout
/// @param window the window
void add_focus_ring(Building& building, const Window& window) {
    const kit::Interaction& interaction = building.state->interaction;
    if (!interaction.focus_shown || interaction.focused == kit::no_control)
        return;
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
    const auto tally = static_cast<int32_t>(menu_open.items.size());
    const kit::Rect menu = kit::choice_menu(menu_open.field, menu_open.items.size(), footer_line);
    const int32_t shown_count = kit::shown_choices(menu_open.items.size());
    const int32_t first = std::clamp(
        building.state->menu_first, int32_t{0}, std::max(tally - shown_count, int32_t{0})
    );
    int32_t marked = building.state->menu_marked;
    if (marked < 0 || marked >= tally)
        marked = -1;
    kit::ChoiceMenuLook look;
    look.first = first;
    look.total = tally;
    look.chosen = menu_open.chosen;
    look.marked = marked;
    look.focus_shown = building.state->interaction.focus_shown;
    for (int32_t index = 0; index < tally; ++index) {
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

/// Lays the picker out. `page` is Compact's details page: no list.
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
    const Details shown = details(picker);
    const std::string hint_text = hint(picker);

    add_item(building.list, kit::Role::fill, {0, 0, window.width, window.height}, {}).colour =
        kit::colour::panel;
    add_header(building, window);
    add_close_mark(building, window);
    const int32_t hint_band = add_hint(building, window, hint_text);

    const int32_t content_w = std::max(window.limit - window.origin, int32_t{0});
    const int32_t body = window.body_top + hint_band;
    const int32_t body_h = std::max(window.footer_rule - body, int32_t{0});
    if (page) {
        add_details(building, {window.origin, body, content_w, body_h}, shown);
    } else if (compact) {
        add_list(building, {window.origin, body, content_w, body_h}, shown_rows);
    } else {
        int32_t list_w =
            frame.size_class == kit::SizeClass::large ? large_list_width : regular_list_width;
        if (list_w > content_w)
            list_w = content_w;
        const int32_t gap = building.metrics->padding;
        int32_t details_x = window.origin + list_w + gap;
        int32_t details_w = std::max(window.limit - details_x, int32_t{0});
        if (details_x > window.limit) {
            details_x = window.limit;
            details_w = 0;
        }
        add_list(building, {window.origin, body, list_w, body_h}, shown_rows);
        add_details(building, {details_x, body, details_w, body_h}, shown);
    }
    add_footer(building, window, page);
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

} // namespace oa::ui::brain_picker
