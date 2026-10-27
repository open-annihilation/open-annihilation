// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The key editor's layout: one kit window, the list of actions and the
// chosen action's details. The named field widths are the fields' widths.
// Buttons grow to the words they show.
#include "oa/ui/key_editor/screen.hpp"

#include "list_geom.hpp"
#include "menu_geom.hpp"
#include "screen_controls.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_editor/text.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/keys.hpp"
#include "oa/ui/kit/text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::key_editor {
namespace {

namespace keys = oa::ui::key_bindings;

/// The window the editor is laid out in.
struct Window {
    int32_t width{};         ///< the window's width, in points
    int32_t height{};        ///< the window's height, in points
    int32_t left{};          ///< the content's left column
    int32_t right{};         ///< the column after the content's last
    int32_t header_height{}; ///< the header's band: Kit 2's window header, Compact's at Compact
    int32_t body_top{};      ///< the first row under the header's rule
    int32_t footer_rule{};   ///< the row of the line over the footer
    int32_t footer_top{};    ///< the footer's first row
    int32_t button_top{};    ///< the footer's buttons' top row
    int32_t button_height{}; ///< the footer's buttons' height: Kit 2's control height
    int32_t gap{};           ///< the rows between the toolbar's stacked rows: Kit 2's spacing
};

/// Controls kept aside until a press can try the menu first.
struct Built {
    kit::DisplayList list{};               ///< the items, and controls not yet ordered
    std::vector<kit::Control> menu{};      ///< the open menu's items, tried first
    std::vector<kit::Control> rest{};      ///< every other control, in the order added
    std::vector<kit::ControlId> tabs{};    ///< Tab's order, without the close mark
    kit::ControlId close{kit::no_control}; ///< the close mark, last in Tab's order
    bool number_keys{false};               ///< the Groups switch was placed
    std::vector<kit::ControlId> details{}; ///< the details' controls, top to bottom
};

/// True when a rectangle holds any point.
///
/// @param rect the rectangle
/// @return true when it is wide and high
bool roomy(const kit::Rect& rect) noexcept {
    return rect.width > 0 && rect.height > 0;
}

/// Moves controls added since `from` into `into`.
///
/// @param[in,out] list the display list
/// @param[out] into where the controls go
/// @param from the first control added for this part
void steal(kit::DisplayList& list, std::vector<kit::Control>& into, std::size_t from) {
    for (std::size_t index = from; index < list.controls.size(); ++index)
        into.push_back(std::move(list.controls[index]));
    list.controls.resize(from);
}

/// Appends an item and returns it.
///
/// @param[in,out] list the display list
/// @param role what it is
/// @param rect where it is
/// @return the item
kit::Item& add_item(kit::DisplayList& list, kit::Role role, kit::Rect rect) {
    kit::Item item;
    item.role = role;
    item.rect = rect;
    list.items.push_back(std::move(item));
    return list.items.back();
}

/// Appends a text cut to its rectangle.
///
/// @param[in,out] built the layout
/// @param fonts the fonts
/// @param rect the rectangle
/// @param text the words
/// @param font which font
/// @param ink the colour
void add_line(
    Built& built,
    const kit::Fonts& fonts,
    kit::Rect rect,
    std::string text,
    kit::FontRole font,
    kit::Colour ink,
    kit::Align align = kit::Align::left
) {
    if (!roomy(rect))
        return;
    text = kit::cut_to_width(fonts, font, text, rect.width);
    kit::Item& item = add_item(built.list, kit::Role::text, rect);
    item.text = std::move(text);
    item.font = font;
    item.colour = ink;
    item.align = align;
}

/// True when the pointer is over a control or holds it.
///
/// @param state the pointer
/// @param id the control
/// @return true when it is hot
bool hot(const ScreenState& state, kit::ControlId id) noexcept {
    return state.interaction.hovered == id || state.interaction.pressed == id;
}

/// True when a press is held on a control and the pointer is still over it.
///
/// @param state the pointer
/// @param id the control
/// @return true when it is held
bool held(const ScreenState& state, kit::ControlId id) noexcept {
    return state.interaction.pressed == id && state.interaction.hovered == id;
}

/// True when the keyboard focus is shown on a control.
///
/// @param state the focus
/// @param id the control
/// @return true when it is shown there
bool focused_on(const ScreenState& state, kit::ControlId id) noexcept {
    return state.interaction.focus_shown && state.interaction.focused == id;
}

/// A button's width: the kit's button, or the caption when that is wider.
///
/// @param fonts the fonts
/// @param metrics the size class
/// @param caption the words
/// @return the width, in points
int32_t
button_width(const kit::Fonts& fonts, const kit::Metrics& metrics, std::string_view caption) {
    const int32_t text = kit::text_width(fonts, kit::FontRole::small, caption);
    return std::max(metrics.button_width, text + metrics.space * 4);
}

/// A switch's width, wide enough for both captions.
///
/// @param fonts the fonts
/// @param metrics the size class
/// @param off_caption the Off half
/// @param on_caption the On half
/// @return the width, in points
int32_t switch_width(
    const kit::Fonts& fonts,
    const kit::Metrics& metrics,
    std::string_view off_caption,
    std::string_view on_caption
) {
    const int32_t off = kit::text_width(fonts, kit::FontRole::small, off_caption);
    const int32_t on = kit::text_width(fonts, kit::FontRole::small, on_caption);
    const int32_t need = std::max(off, on) * 2 + 2;
    return std::max(metrics.switch_width, need);
}

/// Drops the last character of a UTF-8 string.
///
/// @param[in,out] text the text
void pop_char(std::string& text) {
    if (text.empty())
        return;
    text.pop_back();
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0U) == 0x80U)
        text.pop_back();
}

/// Cuts a tracked heading so its tracked width fits a room.
///
/// @param fonts the fonts
/// @param text the words
/// @param room the room, in points
/// @param tracking the extra columns after each glyph
/// @return the words that fit
std::string
fit_tracked(const kit::Fonts& fonts, std::string_view text, int32_t room, int32_t tracking) {
    if (room <= 0)
        return {};
    if (kit::tracked_width(fonts, kit::FontRole::regular, text, tracking) <= room)
        return std::string(text);
    std::string cut(text);
    while (!cut.empty() &&
           kit::tracked_width(fonts, kit::FontRole::regular, cut + "...", tracking) > room)
        pop_char(cut);
    if (cut.empty())
        return {};
    return cut + "...";
}

/// The window for a frame.
///
/// @param frame the frame
/// @return the window
Window window_of(const kit::Frame& frame) {
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    const kit::Point size = window_size(frame);
    Window window;
    window.width = size.x;
    window.height = size.y;
    // Kit 2's window padding, as Settings and the Library keep.
    window.left = metrics.window_padding;
    window.right = std::max(size.x - metrics.edge - metrics.window_padding, window.left);
    // Regular and Large take Kit 2's window header, as Settings and the
    // Library do. Compact keeps the kit's Compact band, as they do there.
    window.header_height = frame.size_class == kit::SizeClass::compact
                               ? metrics.header_height
                               : metrics.window_header_height;
    window.body_top = metrics.edge + window.header_height + 1;
    // Kit 2's footer: the control height and a space above and below at
    // Compact, the window footer's height above it.
    const int32_t footer_h = frame.size_class == kit::SizeClass::compact
                                 ? metrics.control_height + metrics.space * 2
                                 : metrics.window_footer_height;
    window.footer_rule = size.y - metrics.edge - footer_h - 1;
    window.footer_top = window.footer_rule + 1;
    window.button_height = metrics.control_height;
    window.button_top = window.footer_top + (footer_h - metrics.control_height) / 2;
    // The toolbar's rows sit a space apart at Compact and Kit 2's row gap
    // apart above it, as the Library's stacked parts do.
    window.gap = frame.size_class == kit::SizeClass::compact ? metrics.space : metrics.row_gap;
    return window;
}

/// Appends the header: the window's header band, its words in the band's
/// middle at Compact's columns, and the close mark centred in the band. The
/// fit of the title is judged on those columns.
///
/// @param[in,out] built the layout
/// @param editor the words and the layout
/// @param state the match, for the paused note
/// @param fonts the fonts
/// @param window the window
/// @param compact Compact's title is Keys
void add_header(
    Built& built,
    const Editor& editor,
    const ScreenState& state,
    const kit::Fonts& fonts,
    const Window& window,
    bool compact
) {
    const kit::Metrics& compact_metrics = kit::compact_metrics;
    const int32_t tracking = compact_metrics.heading_tracking;
    const int32_t right_inset = compact_metrics.button_height + compact_metrics.header_gap;
    const int32_t title_left =
        compact_metrics.padding + compact_metrics.mark_side + compact_metrics.header_gap;
    const int32_t right_edge =
        window.width - compact_metrics.edge - compact_metrics.padding - right_inset;
    const std::string title_word =
        compact ? show(editor.text(), words::keys) : show(editor.text(), words::source_oa);
    const std::string second_word = compact ? std::string{} : show(editor.text(), words::keys);
    std::string version;
    if (compact) {
        const std::string_view pattern =
            editor.changes() == 1 ? words::compact_context_one : words::compact_context;
        version = fill(
            show(editor.text(), pattern),
            {{"layout", editor.layout_name()}, {"n", std::to_string(editor.changes())}}
        );
    }
    if (state.in_match) {
        const std::string paused = show(editor.text(), words::game_paused);
        if (version.empty())
            version = paused;
        else
            version += " · " + paused;
    }
    const int32_t second_natural =
        second_word.empty()
            ? 0
            : kit::tracked_width(fonts, kit::FontRole::regular, second_word, tracking);
    const int32_t second_gap = second_word.empty() ? 0 : compact_metrics.header_gap;
    int32_t version_room = 0;
    if (!version.empty())
        version_room = std::min(
            kit::text_width(fonts, kit::FontRole::small, version),
            std::max((right_edge - title_left) / 3, int32_t{0})
        );
    int32_t title_room = right_edge - title_left - second_gap - second_natural;
    if (version_room > 0)
        title_room -= compact_metrics.header_gap + version_room;
    std::string second = second_word;
    int32_t second_width = second_natural;
    if (title_room < 8) {
        version_room = 0;
        title_room = right_edge - title_left - second_gap - second_natural;
    }
    if (title_room < 8) {
        second.clear();
        second_width = 0;
        title_room = std::max(right_edge - title_left, int32_t{0});
    }
    const std::string title =
        fit_tracked(fonts, title_word, std::max(title_room, int32_t{0}), tracking);
    const int32_t title_width = kit::tracked_width(fonts, kit::FontRole::regular, title, tracking);
    const int32_t words_right =
        title_left + title_width + (second.empty() ? 0 : compact_metrics.header_gap + second_width);
    const int32_t version_left_limit =
        window.width - compact_metrics.edge - compact_metrics.padding;
    int32_t version_fit =
        std::max(version_left_limit - words_right - compact_metrics.header_gap, int32_t{0});
    version = kit::cut_to_width(fonts, kit::FontRole::small, version, version_fit);
    const int32_t version_width = kit::text_width(fonts, kit::FontRole::small, version);

    kit::HeaderLook look;
    look.width = window.width;
    look.tracking = tracking;
    look.right_inset = right_inset;
    look.mark = true;
    look.title = title;
    look.title_width = title_width;
    look.second = second;
    look.second_width = second.empty() ? 0 : second_width;
    look.version = version;
    look.version_width = version_width;
    look.height = window.header_height;
    const int32_t header_rule = compact_metrics.edge + window.header_height;
    add_item(built.list, kit::Role::header, {0, 0, window.width, header_rule + 1}).look =
        std::move(look);

    const int32_t side = compact_metrics.button_height;
    const kit::Rect rect{
        window.width - compact_metrics.edge - compact_metrics.padding - side,
        compact_metrics.edge + (window.header_height - side) / 2,
        side,
        side,
    };
    kit::ButtonLook button;
    button.style = kit::ButtonStyle::quiet;
    button.mark = kit::ButtonMark::close;
    button.hovered = hot(state, controls::close_mark);
    button.held = held(state, controls::close_mark);
    const std::size_t before = built.list.controls.size();
    kit::add_button(built.list, rect, button, controls::close_mark, std::string(name_close));
    steal(built.list, built.rest, before);
    built.close = controls::close_mark;
}

/// Appends one button and steals its control.
///
/// @param[in,out] built the layout
/// @param rect the button
/// @param look its look
/// @param id its number
/// @param name its name
/// @param tab true when Tab stops on it
void place_button(
    Built& built,
    const kit::Rect& rect,
    const kit::ButtonLook& look,
    kit::ControlId id,
    std::string name,
    bool tab
) {
    const std::size_t before = built.list.controls.size();
    kit::add_button(built.list, rect, look, id, std::move(name));
    steal(built.list, built.rest, before);
    if (tab)
        built.tabs.push_back(id);
}

/// A drop-down's text cut to the field.
///
/// @param fonts the fonts
/// @param metrics the size class
/// @param field the field's width
/// @param text the words
/// @return the words that fit
std::string
choice_text(const kit::Fonts& fonts, const kit::Metrics& metrics, int32_t field, std::string text) {
    const int32_t room =
        std::max(field - metrics.choice_text_inset - metrics.choice_arrow_room, int32_t{0});
    return kit::cut_to_width(fonts, kit::FontRole::regular, text, room);
}

/// Appends a drop-down.
///
/// @param[in,out] built the layout
/// @param state the pointer
/// @param rect the field
/// @param text the words it shows
/// @param id its number
/// @param name its name
/// @param open true when its menu is open
void place_choice(
    Built& built,
    const ScreenState& state,
    const kit::Rect& rect,
    std::string text,
    kit::ControlId id,
    std::string name,
    bool open
) {
    kit::ChoiceLook look;
    look.text = std::move(text);
    look.hovered = hot(state, id);
    look.open = open;
    const std::size_t before = built.list.controls.size();
    kit::add_choice(built.list, rect, look, id, std::move(name));
    steal(built.list, built.rest, before);
    built.tabs.push_back(id);
}

/// The layout's summary under the drop-down: who wrote it, and the player's changes.
///
/// @param[in,out] built the layout
/// @param editor the editor
/// @param fonts the fonts
/// @param rect the row
void add_summary(
    Built& built, const Editor& editor, const kit::Fonts& fonts, const kit::Rect& rect
) {
    const std::string by = fill(
        show(editor.text(), words::by_author),
        {{"author", editor.layout_author()}, {"version", editor.layout_version()}}
    );
    // With no changes of the player's own the summary names the layout alone.
    std::string shown = by;
    if (editor.changes() == 1)
        shown += " · " + std::string(show(editor.text(), words::own_change_one));
    else if (editor.changes() > 1)
        shown += " · " + fill(
                             show(editor.text(), words::own_changes),
                             {{"n", std::to_string(editor.changes())}}
                         );
    add_line(built, fonts, rect, shown, kit::FontRole::small, kit::colour::hint);
    if (!roomy(rect))
        return;
    kit::Control control;
    control.id = controls::summary;
    control.rect = rect;
    control.enabled = true;
    control.focusable = false;
    control.name = std::string(name_summary);
    control.kind = kit::ControlKind::area;
    control.text = shown;
    built.rest.push_back(std::move(control));
}

/// One filter's caption.
///
/// @param editor the editor
/// @param state the category
/// @param filter which filter
/// @param word its name
/// @return the caption
std::string
pill_caption(const Editor& editor, const ScreenState& state, Filter filter, std::string_view word) {
    const std::string label = show(editor.text(), word);
    const std::string count = std::to_string(editor.rows(state.category, filter, "").size());
    return fill(show(editor.text(), words::filter_count), {{"filter", label}, {"count", count}});
}

/// Keeps one of the controls just added as the only Tab stop of that group.
///
/// @param[in,out] built the layout
/// @param from the first control of the group, in `built.rest`
/// @param chosen the control Tab stops on
void one_stop(Built& built, std::size_t from, kit::ControlId chosen) {
    for (std::size_t index = from; index < built.rest.size(); ++index) {
        if (built.rest[index].id != chosen)
            built.rest[index].focusable = false;
    }
    if (chosen != kit::no_control)
        built.tabs.push_back(chosen);
}

/// The filter pills, measured: each one's look and width on Kit 2's filter
/// chip, each wider by the height it gains over the kit's pill so its words
/// stay clear of the rounder ends.
struct PillRow {
    kit::PillLook looks[5]{}; ///< All, Changed, Clashes, Unbound and Latchable
    int32_t widths[5]{};      ///< each pill's width, in points
    int32_t packed{};         ///< the five side by side, chip_gap apart
    int32_t height{};         ///< a pill's height: Kit 2's filter chip height
};

/// The filters, in the order the pills show them.
constexpr Filter pill_filters[] = {
    Filter::all, Filter::changed, Filter::clashes, Filter::unbound, Filter::latchable
};

/// Measures the filter pills.
///
/// @param editor the editor
/// @param state the filter
/// @param fonts the fonts
/// @param metrics the size class
/// @param room the most a pill may be wide
/// @return the pills
PillRow plan_pills(
    const Editor& editor,
    const ScreenState& state,
    const kit::Fonts& fonts,
    const kit::Metrics& metrics,
    int32_t room
) {
    const kit::ControlId ids[] = {
        controls::filter_all,
        controls::filter_changed,
        controls::filter_clashes,
        controls::filter_unbound,
        controls::filter_latchable
    };
    const std::string_view words_of[] = {
        words::filter_all,
        words::filter_changed,
        words::filter_clashes,
        words::filter_unbound,
        words::filter_latchable
    };
    PillRow row;
    row.height = metrics.filter_chip_height;
    const int32_t taller = std::max(row.height - kit::pill_height(), int32_t{0});
    for (int index = 0; index < 5; ++index) {
        kit::PillLook look;
        look.text = pill_caption(editor, state, pill_filters[index], words_of[index]);
        look.state =
            state.filter == pill_filters[index] ? kit::PillState::active : kit::PillState::inactive;
        look.hovered = hot(state, ids[index]);
        look.focused = focused_on(state, ids[index]);
        int32_t width = kit::pill_width(fonts, look) + taller;
        if (width > room && room > 0) {
            look.text = kit::cut_to_width(
                fonts, kit::FontRole::small, look.text, std::max(room - taller - 8, int32_t{0})
            );
            width = std::min(kit::pill_width(fonts, look) + taller, room);
        }
        row.looks[index] = std::move(look);
        row.widths[index] = width;
        row.packed += width;
    }
    row.packed += metrics.chip_gap * 4;
    return row;
}

/// Appends the filter pills, wrapping onto another row when one does not fit.
///
/// @param[in,out] built the layout
/// @param editor the editor
/// @param state the filter
/// @param fonts the fonts
/// @param metrics the size class
/// @param left the row's left
/// @param right the column after the row
/// @param top the row's top
/// @param align_right true when the pills pack against `right`
/// @return the row under the pills
int32_t add_pills(
    Built& built,
    const Editor& editor,
    const ScreenState& state,
    const kit::Fonts& fonts,
    const kit::Metrics& metrics,
    int32_t left,
    int32_t right,
    int32_t top,
    bool align_right
) {
    const kit::ControlId ids[] = {
        controls::filter_all,
        controls::filter_changed,
        controls::filter_clashes,
        controls::filter_unbound,
        controls::filter_latchable
    };
    const std::string_view names[] = {
        name_filter_all,
        name_filter_changed,
        name_filter_clashes,
        name_filter_unbound,
        name_filter_latchable
    };
    const int32_t room = std::max(right - left, int32_t{0});
    const PillRow pills = plan_pills(editor, state, fonts, metrics, room);
    const int32_t height = pills.height;
    int32_t x = left;
    if (align_right && pills.packed <= room)
        x = right - pills.packed;
    int32_t y = top;
    // The pills are one Tab stop, the filter in use; Left and Right move
    // between them.
    const std::size_t rest = built.rest.size();
    kit::ControlId chosen = kit::no_control;
    for (int index = 0; index < 5; ++index) {
        const kit::PillLook& look = pills.looks[index];
        const int32_t width = pills.widths[index];
        if (x > left && x + width > right) {
            x = left;
            y += height + metrics.button_gap;
        }
        const kit::Rect rect{x, y, std::min(width, std::max(right - x, int32_t{0})), height};
        if (roomy(rect)) {
            const std::size_t before = built.list.controls.size();
            kit::add_pill(built.list, rect, look, ids[index], std::string(names[index]));
            steal(built.list, built.rest, before);
            if (state.filter == pill_filters[index] || chosen == kit::no_control)
                chosen = ids[index];
        }
        x += width + metrics.chip_gap;
    }
    one_stop(built, rest, chosen);
    return y + height;
}

/// One nav entry's words and the room they take. The kit draws no caption:
/// a long title wraps onto another line beside the count, and the kit's one
/// line would cut it.
struct NavPlan {
    int32_t height{};                 ///< the entry's height
    std::vector<std::string> lines{}; ///< the title, one line a string
    std::string count{};              ///< the count at the right, or empty
    int32_t count_width{};            ///< the count's width
    std::string caption{};            ///< the control's text
    bool heading{};                   ///< a group heading, not a category
};

/// Breaks a nav title so the first line sits beside the count and a later
/// line may run under the count. A word wider than its line is broken.
///
/// @param title the title
/// @param narrow the first line's room, beside the count
/// @param wide a later line's room
/// @param measure a text's width
/// @return the lines
std::vector<std::string>
wrap_nav_title(std::string_view title, int32_t narrow, int32_t wide, const kit::Measure& measure) {
    const int32_t first_room = std::max(narrow, int32_t{1});
    const int32_t next_room = std::max(wide, int32_t{1});
    std::vector<std::string> words;
    std::string pending;
    auto take_word = [&] {
        if (!pending.empty()) {
            words.push_back(pending);
            pending.clear();
        }
    };
    for (const char byte : title) {
        if (byte == ' ')
            take_word();
        else
            pending.push_back(byte);
    }
    take_word();

    std::vector<std::string> lines;
    std::string line;
    auto room_now = [&]() { return lines.empty() ? first_room : next_room; };
    auto break_onto = [&](const std::string& piece) {
        const std::vector<std::string> broken = kit::wrap(piece, room_now(), measure);
        if (broken.empty()) {
            line = piece;
            return;
        }
        for (std::size_t part = 0; part + 1 < broken.size(); ++part)
            lines.push_back(broken[part]);
        line = broken.back();
    };
    for (const std::string& word : words) {
        if (line.empty()) {
            if (measure(word) <= room_now())
                line = word;
            else
                break_onto(word);
            continue;
        }
        const std::string joined = line + " " + word;
        if (measure(joined) <= room_now()) {
            line = joined;
            continue;
        }
        lines.push_back(line);
        line.clear();
        if (measure(word) <= room_now())
            line = word;
        else
            break_onto(word);
    }
    if (!line.empty())
        lines.push_back(std::move(line));
    if (lines.empty())
        lines.emplace_back(title);
    return lines;
}

/// Plans one nav entry. The count sits at the right of the first line, and a
/// title that does not fit beside it wraps.
///
/// @param fonts the fonts
/// @param category the category
/// @param inner the columns the title and the count share
/// @return the plan
NavPlan plan_nav_entry(const kit::Fonts& fonts, const CategoryEntry& category, int32_t inner) {
    NavPlan plan;
    plan.heading = category.id.starts_with("heading.");
    const std::string title = category.title;
    if (!plan.heading) {
        plan.count = "(" + std::to_string(category.count) + ")";
        plan.count_width = kit::text_width(fonts, kit::FontRole::small, plan.count);
    }
    // The tint square takes the left of a category. A later line of a long
    // title runs under the count, so one long word is not cut in half to
    // leave the count its column.
    constexpr int32_t square = 16;
    constexpr int32_t square_gap = 4;
    const bool squared = !plan.heading && !category.letter.empty();
    const int32_t square_room = squared ? square + square_gap : 0;
    const int32_t wide = std::max(inner - square_room, int32_t{8});
    const int32_t beside = plan.count_width > 0 ? 4 : 0;
    const int32_t narrow = std::max(wide - plan.count_width - beside, int32_t{8});
    const kit::Measure measure = [&](std::string_view words) {
        return kit::text_width(fonts, kit::FontRole::regular, words);
    };
    plan.lines = wrap_nav_title(title, narrow, wide, measure);
    if (plan.lines.empty())
        plan.lines.push_back(std::move(title));
    const int32_t line_h = kit::metrics_of(kit::SizeClass::large).regular_line;
    const int32_t lines_h = static_cast<int32_t>(plan.lines.size()) * line_h;
    plan.height = std::max(line_h + 4, lines_h);
    plan.caption = plan.lines.empty() ? std::string{} : plan.lines.front();
    for (std::size_t index = 1; index < plan.lines.size(); ++index) {
        plan.caption.push_back(' ');
        plan.caption += plan.lines[index];
    }
    if (!plan.count.empty()) {
        plan.caption.push_back(' ');
        plan.caption += plan.count;
    }
    return plan;
}

/// Appends Large's nav.
///
/// The first entry reads "All", with its count at the right. A long title
/// takes another line rather than an ellipsis. The kit's nav draws the
/// selection and leaves the words to the lines placed under it, so a caption
/// is not drawn wider than the column.
///
/// @param[in,out] built the layout
/// @param editor the categories
/// @param state the chosen category
/// @param fonts the fonts
/// @param area the nav's column
void add_nav(
    Built& built,
    const Editor& editor,
    const ScreenState& state,
    const kit::Fonts& fonts,
    const kit::Rect& area
) {
    const std::vector<CategoryEntry> categories = editor.categories();
    if (!roomy(area) || categories.empty())
        return;
    // The same insets the kit's nav keeps for a caption.
    constexpr int32_t text_offset = 12;
    constexpr int32_t text_margin = 4;
    const int32_t text_x = area.x + text_offset;
    const int32_t text_right = area.x + area.width - text_margin;
    const int32_t inner = std::max(text_right - text_x, int32_t{0});
    // A category's tint square sits at the left of its title. All and the
    // headings have no letter, so they keep the whole column.
    constexpr int32_t square = 16;
    constexpr int32_t square_gap = 4;
    const int32_t line_h = kit::metrics_of(kit::SizeClass::large).regular_line;
    // All actions sits under a rule, apart from the categories.
    constexpr int32_t kRuleRoom = 9;
    std::vector<NavPlan> plans;
    plans.reserve(categories.size());
    int32_t total_h = 0;
    for (const CategoryEntry& category : categories) {
        plans.push_back(plan_nav_entry(fonts, category, inner));
        total_h += plans.back().height;
        if (category.id == "all")
            total_h += kRuleRoom;
    }
    if (total_h > area.height) {
        for (NavPlan& plan : plans) {
            if (plan.lines.size() != 1 || plan.height <= line_h)
                continue;
            const int32_t give = std::min(plan.height - line_h, total_h - area.height);
            plan.height -= give;
            total_h -= give;
            if (total_h <= area.height)
                break;
        }
    }
    kit::NavLook look;
    look.area = area;
    look.rule_column = area.x + area.width;
    std::vector<kit::ControlId> ids;
    std::vector<std::string> names;
    kit::ControlId chosen = kit::no_control;
    const auto entry_chosen = [&](const CategoryEntry& category) {
        return category.id == state.category;
    };
    int32_t y = area.y;
    int32_t index = 0;
    for (const CategoryEntry& category : categories) {
        const NavPlan& plan = plans[static_cast<std::size_t>(index)];
        const int32_t rule_room = category.id == "all" ? kRuleRoom : 0;
        if (y + rule_room + plan.height > area.y + area.height)
            break;
        if (rule_room > 0) {
            add_item(built.list, kit::Role::fill, {text_x, y + rule_room / 2, inner, 1}).colour =
                kit::colour::rule;
            y += rule_room;
        }
        kit::NavEntry entry;
        entry.rect = {area.x, y, area.width, plan.height};
        entry.selected = !plan.heading && category.id == state.category;
        const kit::ControlId id = controls::nav_first + static_cast<kit::ControlId>(index);
        entry.hovered = hot(state, id);
        entry.focused = focused_on(state, id);
        look.entries.push_back(std::move(entry));
        ids.push_back(id);
        names.push_back(std::string(name_nav_prefix) + category.id);
        // The nav is one Tab stop, the category in use, or the first one
        // when that one is not shown; Up and Down move between them.
        if (!plan.heading && (entry_chosen(category) || chosen == kit::no_control))
            chosen = id;
        y += plan.height;
        ++index;
    }
    if (look.entries.empty())
        return;
    const std::size_t drawn = look.entries.size();
    const std::size_t before = built.list.controls.size();
    kit::add_nav(built.list, look, ids, names);
    steal(built.list, built.rest, before);
    const std::size_t tail = built.rest.size() - drawn;
    one_stop(built, tail, chosen);
    for (std::size_t place = 0; place < drawn; ++place) {
        kit::Control& control = built.rest[tail + place];
        const NavPlan& plan = plans[place];
        control.text = plan.caption;
        const kit::Rect entry = look.entries[place].rect;
        const CategoryEntry& category = categories[place];
        const bool squared = !plan.heading && !category.letter.empty();
        int32_t line_x = text_x;
        if (squared) {
            kit::KeycapLook cap;
            cap.label = category.letter;
            cap.tint = category.tint;
            const int32_t cap_y = entry.y + std::max((entry.height - square) / 2, int32_t{0});
            const kit::Rect cap_rect{text_x, cap_y, square, square};
            if (roomy(cap_rect))
                kit::add_keycap(built.list, cap_rect, cap, kit::no_control, {});
            line_x = text_x + square + square_gap;
        }
        const int32_t block = static_cast<int32_t>(plan.lines.size()) * line_h;
        int32_t line_y = entry.y + std::max((entry.height - block) / 2, int32_t{0});
        const int32_t first_line_y = line_y;
        const int32_t title_right =
            plan.count_width > 0 ? text_right - plan.count_width - 4 : text_right;
        for (std::size_t line_at = 0; line_at < plan.lines.size(); ++line_at) {
            const std::string& line = plan.lines[line_at];
            const bool beside_count = line_at == 0 && plan.count_width > 0;
            const int32_t right_edge = beside_count ? title_right : text_right;
            const int32_t width = kit::text_width(fonts, kit::FontRole::regular, line);
            const int32_t room = std::max(right_edge - line_x, int32_t{0});
            const kit::Rect rect{
                line_x, line_y, std::min(std::max(width, int32_t{1}), room), line_h
            };
            kit::Item& item = add_item(built.list, kit::Role::text, rect);
            item.text = line;
            item.font = kit::FontRole::regular;
            if (plan.heading)
                item.colour = kit::colour::quiet;
            else if (look.entries[place].selected)
                item.colour = kit::colour::text;
            else
                item.colour = kit::colour::list_text;
            item.align = kit::Align::left;
            line_y += line_h;
        }
        if (plan.count_width > 0) {
            const int32_t count_h = kit::metrics_of(kit::SizeClass::large).small_line;
            // One line keeps the count centred. A wrapped title puts the count
            // on the first line, and the lines under it use the full column.
            const int32_t count_y =
                plan.lines.size() > 1
                    ? first_line_y + std::max((line_h - count_h) / 2, int32_t{0})
                    : entry.y + std::max((entry.height - count_h) / 2, int32_t{0});
            const kit::Rect rect{text_right - plan.count_width, count_y, plan.count_width, count_h};
            kit::Item& item = add_item(built.list, kit::Role::text, rect);
            item.text = plan.count;
            item.font = kit::FontRole::small;
            item.colour = kit::colour::hint;
            item.align = kit::Align::left;
        }
    }
}

/// A chord chip's look.
///
/// Every cap is a keycap, including one printed as a word (Ctrl, Shift, Esc).
/// The gesture word is the last cap, and only when the chord is not a plain
/// press, so Ctrl+D is "Ctrl" then "D" and a hold is "Ctrl + D hold".
///
/// @param editor the keyboard
/// @param chord the chord
/// @param mark the mark on its last key
/// @return the look
kit::ChordChipLook chip_of(const Editor& editor, const keys::Chord& chord, kit::KeyMark mark) {
    kit::ChordChipLook look;
    look.mark = mark;
    const std::vector<keys::KeyCap> caps =
        keys::keycaps(chord, editor.keyboard(), editor.platform());
    const bool gestured = chord.gesture != keys::Gesture::press;
    for (std::size_t index = 0; index < caps.size(); ++index) {
        const bool last_gesture = gestured && index + 1 == caps.size();
        if (last_gesture)
            look.gesture = caps[index].text;
        else
            look.keys.push_back(caps[index].text);
    }
    if (look.keys.empty() && look.gesture.empty())
        look.none_text = keys::display_text(chord, editor.keyboard(), editor.platform());
    return look;
}

/// Appends the list, its rows and, when Groups is in view, Number keys select groups.
///
/// @param[in,out] built the layout
/// @param editor the editor
/// @param state the list's scroll and the chosen row
/// @param fonts the fonts
/// @param metrics the size class
/// @param size_class the class, for the row height and how many chips show
/// @param pane the list's column, including its scroll bar
/// @param window the window, so the scroll bar stays inside it
void add_list(
    Built& built,
    const Editor& editor,
    const ScreenState& state,
    const kit::Fonts& fonts,
    const kit::Metrics& metrics,
    kit::SizeClass size_class,
    const kit::Rect& pane,
    const Window& window
) {
    if (!roomy(pane))
        return;
    const std::vector<Row> shown_rows =
        editor.rows(state.category, state.filter, state.search.text);
    const std::vector<Slot> slots = list_slots(editor, state);
    const int32_t row_h = list_row_height(size_class);
    const int32_t content_h = static_cast<int32_t>(slots.size()) * row_h;
    const bool scrolling = content_h > pane.height;
    const int32_t well_width = metrics.scroll_thumb_width + 2;
    const int32_t bar_gap = metrics.button_gap;
    const int32_t port_w =
        scrolling ? std::max(pane.width - well_width - bar_gap, int32_t{0}) : pane.width;
    const kit::Rect port{pane.x, pane.y, port_w, pane.height};
    int32_t offset = state.list_scroll;
    const int32_t limit = std::max(content_h - port.height, int32_t{0});
    if (offset < 0)
        offset = 0;
    if (offset > limit)
        offset = limit;

    const bool compact = size_class == kit::SizeClass::compact;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        const Slot& slot = slots[index];
        const kit::Rect full{
            port.x,
            port.y + static_cast<int32_t>(index) * row_h - offset,
            port.width,
            row_h,
        };
        const kit::Rect shown = kit::intersect(full, port);
        if (!roomy(shown))
            continue;
        const bool whole = shown.y == full.y && shown.height == full.height;
        if (slot.kind == Slot::Kind::heading) {
            kit::HeadingLook heading;
            heading.text = kit::cut_to_width(fonts, kit::FontRole::small, slot.text, shown.width);
            add_item(built.list, kit::Role::heading, shown).look = std::move(heading);
            continue;
        }
        if (slot.kind == Slot::Kind::number_keys) {
            const std::string off_caption = show(editor.text(), words::state_off);
            const std::string on_caption = show(editor.text(), words::state_on);
            const int32_t width = std::min(
                switch_width(fonts, metrics, off_caption, on_caption),
                std::max(shown.width - metrics.list_row_inset, int32_t{0})
            );
            const std::string label = show(editor.text(), words::number_keys);
            const int32_t label_w =
                std::max(shown.width - width - metrics.list_row_gap, int32_t{0});
            add_line(
                built,
                fonts,
                {shown.x + metrics.list_row_inset, shown.y, label_w, shown.height},
                label,
                kit::FontRole::small,
                kit::colour::text
            );
            if (whole && width > 0) {
                kit::SwitchLook toggle;
                toggle.on = editor.number_keys();
                toggle.hovered = hot(state, controls::number_keys);
                toggle.off_caption = off_caption;
                toggle.on_caption = on_caption;
                const kit::Rect rect{
                    shown.x + shown.width - width - metrics.list_row_inset,
                    shown.y + (shown.height - metrics.button_height) / 2,
                    width,
                    metrics.button_height,
                };
                if (roomy(rect) && kit::wholly_in(rect, port)) {
                    const std::size_t before = built.list.controls.size();
                    kit::add_switch(
                        built.list,
                        rect,
                        toggle,
                        controls::number_keys,
                        std::string(name_number_keys)
                    );
                    steal(built.list, built.rest, before);
                    built.number_keys = true;
                }
            }
            continue;
        }
        const Row* row = nullptr;
        for (const Row& candidate : shown_rows) {
            if (candidate.id == slot.id) {
                row = &candidate;
                break;
            }
        }
        if (row == nullptr)
            continue;
        // The mark column is 12 points. A row with no mark leaves it blank,
        // and the names still start on the same column. The category's tint
        // square belongs to the nav and the details, not the row.
        constexpr int32_t mark_side = 12;
        kit::ListRowLook look;
        look.show_badge = false;
        look.lead = mark_side + metrics.list_row_gap;
        look.title = row->name;
        look.by = row->aside;
        look.selected = state.selected == row->id;
        const kit::ControlId id =
            controls::row_first + static_cast<kit::ControlId>(static_cast<int32_t>(index));
        look.hovered = hot(state, id);
        if (whole) {
            const std::size_t before = built.list.controls.size();
            const std::string name = row_control_name(row->id, index);
            kit::add_list_row(built.list, shown, look, id, name);
            steal(built.list, built.rest, before);
            if (!built.rest.empty()) {
                std::string value = row->name;
                for (const keys::Chord& chord : row->chords) {
                    value.push_back(' ');
                    value += keys::display_text(chord, editor.keyboard(), editor.platform());
                }
                built.rest.back().text = std::move(value);
                built.rest.back().checked = state.selected == row->id;
            }
        } else {
            add_item(built.list, kit::Role::list_row, shown).look = look;
        }
        if (row->mark != kit::KeyMark::none) {
            kit::KeycapLook cap;
            cap.mark = row->mark;
            cap.mark_only = true;
            const int32_t mark_top = shown.y + std::max((shown.height - mark_side) / 2, int32_t{0});
            const kit::Rect mark_rect{
                shown.x + metrics.list_row_inset, mark_top, mark_side, mark_side
            };
            if (roomy(mark_rect) && kit::wholly_in(mark_rect, shown))
                kit::add_keycap(built.list, mark_rect, cap, kit::no_control, {});
        }

        constexpr int32_t kChipMost = 24;
        const int32_t chip_h = std::clamp(shown.height - 4, int32_t{1}, kChipMost);
        const int32_t chip_top = shown.y + (shown.height - chip_h) / 2;
        const int32_t chord_room = std::min(
            size_class == kit::SizeClass::large ? large_chord_width : shown.width / 2,
            std::max(shown.width / 2, int32_t{0})
        );
        int32_t chip_right = shown.x + shown.width - metrics.list_row_inset;
        const int32_t chip_left = std::max(chip_right - chord_room, shown.x);
        std::size_t drawn = 0;
        const std::size_t most = compact ? 1 : row->chords.size();
        for (std::size_t chord_at = 0; chord_at < row->chords.size() && drawn < most; ++chord_at) {
            const kit::ChordChipLook chip =
                chip_of(editor, row->chords[chord_at], kit::KeyMark::none);
            const int32_t natural = kit::chord_chip_width(fonts, chip);
            if (natural <= 0 || chip_right - natural < chip_left)
                break;
            chip_right -= natural;
            const kit::Rect chip_rect{chip_right, chip_top, natural, chip_h};
            if (roomy(chip_rect) && kit::wholly_in(chip_rect, shown))
                kit::add_chord_chip(built.list, chip_rect, chip, kit::no_control, {});
            chip_right -= metrics.chip_gap;
            ++drawn;
        }
        if (drawn < row->chords.size()) {
            const std::string more = fill(
                show(editor.text(), words::more_chords),
                {{"n", std::to_string(row->chords.size() - drawn)}}
            );
            const int32_t more_w = kit::text_width(fonts, kit::FontRole::small, more);
            if (chip_right - more_w >= chip_left) {
                chip_right -= more_w;
                add_line(
                    built,
                    fonts,
                    {chip_right, shown.y, more_w, shown.height},
                    more,
                    kit::FontRole::small,
                    kit::colour::hint
                );
                chip_right -= metrics.chip_gap;
            }
        }
        // Beside Regular and Large rows: Changed for the player's own change,
        // else the area of a row listed outside its own category. The words
        // keep clear of the name.
        if (!compact) {
            int32_t name_end = shown.x + look.lead + metrics.list_row_inset +
                               kit::text_width(fonts, kit::FontRole::regular, row->name) +
                               metrics.list_row_gap * 2;
            if (!row->aside.empty())
                name_end +=
                    kit::text_width(fonts, kit::FontRole::small, row->aside) + metrics.list_row_gap;
            if (row->changed) {
                kit::PillLook pill;
                pill.text = show(editor.text(), words::filter_changed);
                pill.state = kit::PillState::update;
                const int32_t pill_w = kit::pill_width(fonts, pill);
                const int32_t pill_h = kit::pill_height();
                if (chip_right - pill_w >= name_end) {
                    const kit::Rect pill_rect{
                        chip_right - pill_w,
                        shown.y + (shown.height - pill_h) / 2,
                        pill_w,
                        pill_h,
                    };
                    if (kit::wholly_in(pill_rect, shown))
                        kit::add_pill(built.list, pill_rect, pill, kit::no_control, {});
                }
            } else if (!row->area.empty()) {
                const int32_t area_w = kit::text_width(fonts, kit::FontRole::small, row->area);
                if (chip_right - area_w >= name_end)
                    add_line(
                        built,
                        fonts,
                        {chip_right - area_w, shown.y, area_w, shown.height},
                        row->area,
                        kit::FontRole::small,
                        kit::colour::quiet
                    );
            }
        }
        if (row->read_only)
            add_item(built.list, kit::Role::locked_fade, shown);
    }

    kit::Control list_control;
    list_control.id = controls::list;
    list_control.rect = port;
    list_control.enabled = true;
    list_control.focusable = true;
    list_control.name = std::string(name_list);
    list_control.kind = kit::ControlKind::list;
    built.rest.push_back(std::move(list_control));
    built.tabs.push_back(controls::list);

    if (!scrolling)
        return;
    const kit::Rect well{port.x + port.width + bar_gap, pane.y, well_width, pane.height};
    const kit::Rect whole{0, 0, window.width, window.height};
    if (!roomy(well) || !kit::wholly_in(well, whole))
        return;
    kit::ScrollArea area;
    area.view = port;
    area.well = well;
    area.content_height = content_h;
    area.limit = limit;
    area.offset = offset;
    kit::ScrollBarLook bar;
    bar.thumb = area.thumb(metrics.least_thumb_height);
    bar.hot = state.bar == ScreenState::Bar::list || hot(state, controls::list_bar);
    add_item(built.list, kit::Role::scroll_bar, well).look = bar;
    kit::Control control;
    control.id = controls::list_bar;
    control.rect = well;
    control.enabled = true;
    control.focusable = false;
    control.name = std::string(name_list_bar);
    control.kind = kit::ControlKind::scroll_bar;
    built.rest.push_back(std::move(control));
}

/// One thing the details stack, in content points.
struct Piece {
    enum class Kind : uint8_t { text, button, toggle, chip, square, well };
    Kind kind{Kind::text};                           ///< what it is
    int32_t y{};                                     ///< its top, in the content
    int32_t height{};                                ///< its height
    int32_t x{};                                     ///< its left, in the content
    int32_t width{};                                 ///< its width
    std::string text{};                              ///< a line, a caption or a letter
    kit::FontRole font{kit::FontRole::small};        ///< a line's font
    kit::Colour ink{kit::colour::text};              ///< a line's or a well's colour
    kit::ControlId id{kit::no_control};              ///< a control's number
    std::string name{};                              ///< a control's name
    bool enabled{true};                              ///< a control can be pressed
    bool on{};                                       ///< a switch is on
    kit::ButtonStyle style{kit::ButtonStyle::plain}; ///< a button's style
    kit::ButtonMark mark{kit::ButtonMark::none};     ///< a button's mark
    kit::ChordChipLook chip{};                       ///< a chord chip
    kit::KeyTint tint{kit::KeyTint::none};           ///< a title square's tint
    std::string said{}; ///< a control's text for automation, when not its caption
};

/// The chosen action's pieces, top to bottom: its name, what it does and
/// where; a draft clash with Settle…; each chord on a well with Change… and
/// its remove mark; + Add a key; More options and, while they are open, the
/// facts, the helpers and Find on keyboard; then where the keys come from
/// and Reset.
///
/// @param editor the editor
/// @param state the chosen action and whether More options are open
/// @param fonts the fonts
/// @param metrics the size class
/// @param width the content's width
/// @return the pieces
std::vector<Piece> detail_pieces(
    const Editor& editor,
    const ScreenState& state,
    const kit::Fonts& fonts,
    const kit::Metrics& metrics,
    int32_t width
) {
    std::vector<Piece> pieces;
    if (width <= 0)
        return pieces;
    int32_t y = 0;
    const int32_t control_h = metrics.control_height;
    // A line wraps onto the next rather than ending in an ellipsis. The
    // explanation of a letter key is wider than Regular's details pane.
    auto text_in =
        [&](std::string words, kit::FontRole font, kit::Colour ink, int32_t x, int32_t room) {
            const int32_t line_h =
                font == kit::FontRole::regular ? metrics.regular_line : metrics.small_line;
            const int32_t wrap_room = std::max(room, int32_t{1});
            const kit::Measure measure = [&](std::string_view words_measured) {
                return kit::text_width(fonts, font, words_measured);
            };
            std::vector<std::string> lines = kit::wrap(words, wrap_room, measure);
            if (lines.empty())
                lines.emplace_back();
            for (std::string& line : lines) {
                Piece piece;
                piece.kind = Piece::Kind::text;
                piece.y = y;
                piece.x = x;
                piece.height = line_h;
                piece.width = wrap_room;
                piece.text = std::move(line);
                piece.font = font;
                piece.ink = ink;
                y += piece.height + 2;
                pieces.push_back(std::move(piece));
            }
        };
    auto text = [&](std::string full, kit::FontRole font, kit::Colour ink) {
        text_in(std::move(full), font, ink, 0, width);
    };
    auto gap = [&]() { y += metrics.space * 2; };
    auto button_at = [&](std::string caption,
                         kit::ControlId id,
                         std::string_view control_name,
                         kit::ButtonStyle style,
                         int32_t x,
                         int32_t row,
                         int32_t room,
                         bool enabled) {
        Piece button;
        button.kind = Piece::Kind::button;
        button.x = x;
        button.y = row;
        button.height = control_h;
        button.width = std::min(button_width(fonts, metrics, caption), std::max(room, int32_t{1}));
        button.text =
            kit::cut_to_width(fonts, kit::FontRole::small, std::move(caption), button.width);
        button.id = id;
        button.name = std::string(control_name);
        button.style = style;
        button.enabled = enabled;
        const int32_t used = button.width;
        pieces.push_back(std::move(button));
        return used;
    };
    if (state.selected.empty()) {
        text(show(editor.text(), words::choose_action), kit::FontRole::regular, kit::colour::hint);
        return pieces;
    }
    const Details action = editor.details(state.selected);
    if (action.id.empty()) {
        text(show(editor.text(), words::choose_action), kit::FontRole::regular, kit::colour::hint);
        return pieces;
    }
    constexpr int32_t square = 16;
    constexpr int32_t square_gap = 4;
    int32_t name_x = 0;
    int32_t name_room = width;
    if (!action.letter.empty()) {
        Piece cap;
        cap.kind = Piece::Kind::square;
        cap.y = y;
        cap.width = square;
        cap.height = square;
        cap.text = action.letter;
        cap.tint = action.tint;
        pieces.push_back(std::move(cap));
        name_x = square + square_gap;
        name_room = std::max(width - name_x, int32_t{1});
    }
    text_in(action.name, kit::FontRole::regular, kit::colour::text, name_x, name_room);
    if (!action.description.empty())
        text(action.description, kit::FontRole::small, kit::colour::text_secondary);
    gap();
    if (!action.where.empty())
        text(action.where, kit::FontRole::small, kit::colour::quiet);

    // A draft clash: what it is, and Settle… opens the one clash question.
    if (const std::optional<keys::Clash> clash = editor.draft_clash(state.selected)) {
        const std::string other_id = clash->a == state.selected ? clash->b : clash->a;
        const Details other = editor.details(other_id);
        const std::string other_name = other.name.empty() ? other_id : other.name;
        const std::string chord_text =
            keys::display_text(clash->chord, editor.keyboard(), editor.platform());
        const int32_t top = y;
        Piece well;
        well.kind = Piece::Kind::well;
        well.y = top;
        well.width = width;
        well.ink = kit::colour::danger_tint;
        const std::size_t well_at = pieces.size();
        pieces.push_back(well);
        y += metrics.space;
        text_in(
            fill(show(editor.text(), words::does_two), {{"chord", chord_text}}),
            kit::FontRole::regular,
            kit::colour::danger,
            metrics.space,
            width - metrics.space * 2
        );
        text_in(
            fill(
                show(editor.text(), words::here_and_there),
                {{"name", other_name}, {"context", context_phrase(editor, state.selected)}}
            ),
            kit::FontRole::small,
            kit::colour::text_secondary,
            metrics.space,
            width - metrics.space * 2
        );
        button_at(
            show(editor.text(), words::settle_button),
            controls::card_settle,
            name_card_settle,
            kit::ButtonStyle::accent,
            metrics.space,
            y,
            width - metrics.space * 2,
            true
        );
        y += control_h + metrics.space;
        pieces[well_at].height = y - top;
        gap();
    }

    // Each chord on a well: the chip, Change… and the remove mark.
    kit::KeyMark row_mark = kit::KeyMark::none;
    for (const Row& row : editor.rows(std::string_view{"all"}, Filter::all, "")) {
        if (row.id == action.id) {
            row_mark = row.mark;
            break;
        }
    }
    const int32_t chord_h = control_h + metrics.space * 2;
    for (std::size_t index = 0; index < action.chords.size(); ++index) {
        const ChordDetail& chord = action.chords[index];
        const kit::KeyMark mark = chord.fixed ? kit::KeyMark::fixed : row_mark;
        Piece well;
        well.kind = Piece::Kind::well;
        well.y = y;
        well.height = chord_h;
        well.width = width;
        well.ink = kit::colour::list;
        pieces.push_back(well);
        const int32_t row = y + metrics.space;
        int32_t right = width - metrics.space;
        const int32_t remove_side = std::min(control_h, metrics.button_height + 4);
        // The remove mark sits at the right, but Tab reaches it after
        // Change…, as the two are read.
        std::optional<Piece> remove;
        if (chord.removable && right - remove_side > metrics.space) {
            Piece button;
            button.kind = Piece::Kind::button;
            button.y = row + (control_h - remove_side) / 2;
            button.height = remove_side;
            button.width = remove_side;
            button.x = right - remove_side;
            button.mark = kit::ButtonMark::close;
            button.style = kit::ButtonStyle::quiet;
            button.id =
                controls::chord_first + static_cast<kit::ControlId>(static_cast<int32_t>(index));
            button.name = std::string(name_chord_prefix) + std::to_string(index + 1) + ".remove";
            remove = std::move(button);
            right -= remove_side + metrics.space;
        }
        // Change… opens the capture for this chord. Escape on a fixed action
        // stays as it is.
        if (!chord.fixed && !action.read_only) {
            const std::string caption = show(editor.text(), words::change_button);
            const int32_t change_w = std::min(button_width(fonts, metrics, caption), right / 2);
            if (change_w > 8) {
                button_at(
                    caption,
                    controls::chord_row_first +
                        static_cast<kit::ControlId>(static_cast<int32_t>(index)),
                    std::string(name_chord_prefix) + std::to_string(index + 1),
                    kit::ButtonStyle::plain,
                    right - change_w,
                    row,
                    change_w,
                    true
                );
                // Its text for automation is the chord it changes.
                pieces.back().said =
                    keys::display_text(chord.chord, editor.keyboard(), editor.platform());
                right -= change_w + metrics.space;
            }
        }
        if (remove)
            pieces.push_back(std::move(*remove));
        Piece chip;
        chip.kind = Piece::Kind::chip;
        chip.x = metrics.space;
        chip.y = row + (control_h - std::min(control_h, int32_t{24})) / 2;
        chip.height = std::min(control_h, int32_t{24});
        chip.chip = chip_of(editor, chord.chord, mark);
        chip.width = std::min(
            kit::chord_chip_width(fonts, chip.chip), std::max(right - metrics.space, int32_t{0})
        );
        const int32_t kind_x = chip.x + chip.width + metrics.space * 2;
        pieces.push_back(std::move(chip));
        if (!action.kind.empty() && right - kind_x > 24) {
            Piece kind;
            kind.kind = Piece::Kind::text;
            kind.x = kind_x;
            kind.y = row + (control_h - metrics.small_line) / 2;
            kind.height = metrics.small_line;
            kind.width = right - kind_x;
            kind.text = kit::cut_to_width(fonts, kit::FontRole::small, action.kind, kind.width);
            kind.ink = kit::colour::text_secondary;
            pieces.push_back(std::move(kind));
        }
        y += chord_h + metrics.space;
    }
    if (!action.read_only && action.chords.size() < 4) {
        Piece add;
        add.kind = Piece::Kind::button;
        add.y = y;
        add.height = control_h;
        add.width = width;
        add.text = kit::cut_to_width(
            fonts, kit::FontRole::small, show(editor.text(), words::add_a_key), width
        );
        add.id = controls::detail_add;
        add.name = std::string(name_detail_add);
        add.style = kit::ButtonStyle::plain;
        y += add.height + metrics.space;
        pieces.push_back(std::move(add));
    }

    // More options: the facts, the helpers and Find on keyboard.
    const std::string more_caption =
        std::string(state.details_more ? "▾ " : "› ") + show(editor.text(), words::more_options);
    button_at(
        more_caption,
        controls::detail_more,
        name_detail_more,
        kit::ButtonStyle::quiet,
        0,
        y,
        width,
        true
    );
    y += control_h + metrics.space;
    if (state.details_more) {
        auto fact = [&](std::string_view label, const std::string& value) {
            if (value.empty())
                return;
            text(
                show(editor.text(), label) + "  " + value, kit::FontRole::small, kit::colour::hint
            );
        };
        fact(words::known_as, action.technical);
        fact(words::fact_kind, action.kind);
        fact(words::fact_base, action.base);
        fact(words::fact_works, action.works_with);
        for (const ChordDetail& chord : action.chords) {
            if (!chord.explanation.empty())
                text(
                    keys::display_text(chord.chord, editor.keyboard(), editor.platform()) + "  " +
                        chord.explanation,
                    kit::FontRole::small,
                    kit::colour::hint
                );
        }
        const std::string off_caption = show(editor.text(), words::state_off);
        const std::string on_caption = show(editor.text(), words::state_on);
        auto toggle = [&](bool offered,
                          bool on,
                          kit::ControlId id,
                          std::string_view name,
                          std::string_view word,
                          std::string_view hint) {
            if (!offered)
                return;
            text(show(editor.text(), word), kit::FontRole::small, kit::colour::text);
            Piece piece;
            piece.kind = Piece::Kind::toggle;
            piece.y = y;
            piece.height = metrics.button_height;
            piece.width = std::min(switch_width(fonts, metrics, off_caption, on_caption), width);
            piece.on = on;
            piece.id = id;
            piece.name = std::string(name);
            piece.text = off_caption;
            y += piece.height + 2;
            pieces.push_back(std::move(piece));
            if (!hint.empty())
                text(show(editor.text(), hint), kit::FontRole::small, kit::colour::hint);
        };
        toggle(
            action.latch_offered,
            action.latch_on,
            controls::detail_latch,
            name_latch,
            words::latch,
            words::latch_hint
        );
        toggle(
            action.confirm_offered,
            action.confirm_on,
            controls::detail_confirm,
            name_confirm,
            words::hold_to_confirm,
            {}
        );
        if (action.confirm_setting_off)
            text(show(editor.text(), words::confirm_off), kit::FontRole::small, kit::colour::quiet);
        button_at(
            show(editor.text(), words::find_on_keyboard_title),
            controls::detail_find,
            name_detail_find,
            kit::ButtonStyle::plain,
            0,
            y,
            width,
            true
        );
        y += control_h + metrics.space;
    }

    // Where the keys come from, and Reset while the player changed them.
    if (!action.read_only) {
        gap();
        Piece rule;
        rule.kind = Piece::Kind::well;
        rule.y = y;
        rule.height = 1;
        rule.width = width;
        rule.ink = kit::colour::rule;
        pieces.push_back(rule);
        y += metrics.space;
        bool changed = false;
        for (const Row& one : editor.rows("all", Filter::changed, "")) {
            if (one.id == action.id)
                changed = true;
        }
        const std::string from = fill(
            show(editor.text(), changed ? words::your_change_on : words::from_layout),
            {{"layout", editor.layout_name()}}
        );
        const std::string reset_caption = show(editor.text(), words::reset_title);
        const int32_t reset_w = std::min(button_width(fonts, metrics, reset_caption), width / 2);
        Piece line;
        line.kind = Piece::Kind::text;
        line.y = y + (control_h - metrics.small_line) / 2;
        line.height = metrics.small_line;
        line.width = std::max(width - reset_w - metrics.space, int32_t{1});
        line.text = kit::cut_to_width(fonts, kit::FontRole::small, from, line.width);
        line.ink = kit::colour::text_secondary;
        pieces.push_back(std::move(line));
        button_at(
            reset_caption,
            controls::detail_reset,
            name_detail_reset,
            kit::ButtonStyle::quiet,
            width - reset_w,
            y,
            reset_w,
            changed
        );
        y += control_h;
    }
    return pieces;
}

/// Appends the details, scrolled, and the controls that lie wholly in view.
///
/// @param[in,out] built the layout
/// @param editor the editor
/// @param state the details' scroll
/// @param fonts the fonts
/// @param metrics the size class
/// @param pane the details' column, including its scroll bar
/// @param window the window, so a scroll bar is not placed outside it
void add_details(
    Built& built,
    const Editor& editor,
    const ScreenState& state,
    const kit::Fonts& fonts,
    const kit::Metrics& metrics,
    const kit::Rect& pane,
    const Window& window
) {
    if (!roomy(pane))
        return;
    const int32_t well_width = metrics.scroll_thumb_width + 2;
    const int32_t bar_gap = metrics.button_gap;
    const std::vector<Piece> measured = detail_pieces(
        editor, state, fonts, metrics, std::max(pane.width - well_width - bar_gap, int32_t{0})
    );
    int32_t content_h = 0;
    for (const Piece& piece : measured)
        content_h = std::max(content_h, piece.y + piece.height);
    const bool scrolling = content_h > pane.height;
    const int32_t port_w =
        scrolling ? std::max(pane.width - well_width - bar_gap, int32_t{0}) : pane.width;
    const std::vector<Piece> pieces =
        scrolling ? measured : detail_pieces(editor, state, fonts, metrics, port_w);
    if (!scrolling) {
        content_h = 0;
        for (const Piece& piece : pieces)
            content_h = std::max(content_h, piece.y + piece.height);
    }
    const kit::Rect port{pane.x, pane.y, port_w, pane.height};
    int32_t offset = state.details_scroll;
    const int32_t limit = std::max(content_h - port.height, int32_t{0});
    if (offset < 0)
        offset = 0;
    if (offset > limit)
        offset = limit;
    const std::string off_caption = show(editor.text(), words::state_off);
    const std::string on_caption = show(editor.text(), words::state_on);
    for (const Piece& piece : pieces) {
        const kit::Rect full{
            port.x + piece.x, port.y + piece.y - offset, piece.width, piece.height
        };
        if (!roomy(full) || !kit::wholly_in(full, port))
            continue;
        if (piece.kind == Piece::Kind::text) {
            add_line(built, fonts, full, piece.text, piece.font, piece.ink);
            continue;
        }
        if (piece.kind == Piece::Kind::well) {
            add_item(built.list, kit::Role::fill, full).colour = piece.ink;
            continue;
        }
        if (piece.kind == Piece::Kind::square) {
            kit::KeycapLook cap;
            cap.label = piece.text;
            cap.tint = piece.tint;
            kit::add_keycap(built.list, full, cap, kit::no_control, {});
            continue;
        }
        if (piece.kind == Piece::Kind::chip) {
            kit::ChordChipLook look = piece.chip;
            if (piece.id != kit::no_control)
                look.hovered = hot(state, piece.id);
            if (piece.id == kit::no_control) {
                kit::add_chord_chip(built.list, full, look, kit::no_control, {});
                continue;
            }
            const std::size_t before = built.list.controls.size();
            kit::add_chord_chip(built.list, full, look, piece.id, piece.name);
            steal(built.list, built.rest, before);
            built.details.push_back(piece.id);
            continue;
        }
        if (piece.kind == Piece::Kind::toggle) {
            kit::SwitchLook toggle;
            toggle.on = piece.on;
            toggle.hovered = hot(state, piece.id);
            toggle.off_caption = off_caption;
            toggle.on_caption = on_caption;
            const std::size_t before = built.list.controls.size();
            kit::add_switch(built.list, full, toggle, piece.id, piece.name);
            steal(built.list, built.rest, before);
            built.details.push_back(piece.id);
            continue;
        }
        kit::ButtonLook button;
        button.caption = piece.text;
        button.style = piece.style;
        button.mark = piece.mark;
        button.enabled = piece.enabled;
        button.hovered = hot(state, piece.id);
        button.held = held(state, piece.id);
        const std::size_t before = built.list.controls.size();
        kit::add_button(built.list, full, button, piece.id, piece.name);
        steal(built.list, built.rest, before);
        if (!piece.said.empty() && !built.rest.empty() && built.rest.back().id == piece.id)
            built.rest.back().text = piece.said;
        built.details.push_back(piece.id);
    }
    if (!scrolling)
        return;
    const kit::Rect well{port.x + port.width + bar_gap, pane.y, well_width, pane.height};
    const kit::Rect whole{0, 0, window.width, window.height};
    if (!roomy(well) || !kit::wholly_in(well, whole))
        return;
    kit::ScrollArea area;
    area.view = port;
    area.well = well;
    area.content_height = content_h;
    area.limit = limit;
    area.offset = offset;
    kit::ScrollBarLook bar;
    bar.thumb = area.thumb(metrics.least_thumb_height);
    bar.hot = state.bar == ScreenState::Bar::details || hot(state, controls::details_bar);
    add_item(built.list, kit::Role::scroll_bar, well).look = bar;
    kit::Control control;
    control.id = controls::details_bar;
    control.rect = well;
    control.enabled = true;
    control.focusable = false;
    control.name = std::string(name_details_bar);
    control.kind = kit::ControlKind::scroll_bar;
    built.rest.push_back(std::move(control));
}

/// Appends the footer band: the draft line and Undo on the left, then
/// the note that asks for clashes to be settled, then More, Cancel and Save
/// changes from the right. Compact's details page keeps the draft line, Undo
/// and Save changes.
///
/// @param[in,out] built the layout
/// @param editor the editor
/// @param state the pointer
/// @param fonts the fonts
/// @param metrics the size class
/// @param window the window
/// @param page Compact's details page
/// @param compact Compact's shorter draft line
void add_footer(
    Built& built,
    const Editor& editor,
    const ScreenState& state,
    const kit::Fonts& fonts,
    const kit::Metrics& metrics,
    const Window& window,
    bool page,
    bool compact
) {
    kit::FooterBandLook band;
    band.width = window.width;
    band.rule_row = window.footer_rule;
    // The band reaches the window's foot whatever the kit's band paints.
    add_item(
        built.list,
        kit::Role::fill,
        {0, window.footer_rule, window.width, window.height - window.footer_rule}
    )
        .colour = kit::colour::band;
    add_item(
        built.list,
        kit::Role::footer_band,
        {0, window.footer_rule, window.width, window.height - window.footer_rule}
    )
        .look = band;

    struct FootButton {
        kit::ControlId id{};
        std::string name{};
        std::string caption{};
        kit::ButtonStyle style{kit::ButtonStyle::plain};
        bool enabled{true};
    };

    auto make = [&](kit::ControlId id,
                    std::string_view control_name,
                    std::string_view word,
                    kit::ButtonStyle style,
                    bool enabled) {
        FootButton button;
        button.id = id;
        button.name = std::string(control_name);
        button.caption = show(editor.text(), word);
        button.style = style;
        button.enabled = enabled;
        return button;
    };
    auto place = [&](const FootButton& button, int32_t at, int32_t width) {
        if (at + width > window.right)
            width = std::max(window.right - at, int32_t{0});
        const kit::Rect rect{at, window.button_top, width, window.button_height};
        if (!roomy(rect))
            return;
        kit::ButtonLook look;
        look.caption = button.caption;
        if (kit::text_width(fonts, kit::FontRole::small, look.caption) > rect.width)
            look.caption = kit::cut_to_width(fonts, kit::FontRole::small, look.caption, rect.width);
        look.style = button.style;
        look.enabled = button.enabled;
        look.hovered = hot(state, button.id);
        look.held = held(state, button.id);
        place_button(built, rect, look, button.id, button.name, true);
    };
    const int32_t gap = metrics.button_gap;

    // The draft line: a dot and the count while edits are unsaved.
    const std::size_t unsaved = editor.unsaved();
    std::string draft;
    if (unsaved == 0)
        draft = show(editor.text(), words::no_unsaved);
    else if (compact)
        draft = fill(show(editor.text(), words::unsaved_short), {{"n", std::to_string(unsaved)}});
    else if (unsaved == 1)
        draft = show(editor.text(), words::unsaved_edit_one);
    else
        draft = fill(show(editor.text(), words::unsaved_edits), {{"n", std::to_string(unsaved)}});
    constexpr int32_t kDot = 8;
    int32_t x = window.left;
    const int32_t draft_w = (unsaved == 0 ? 0 : kDot + metrics.space) +
                            kit::text_width(fonts, kit::FontRole::small, draft);
    const kit::Rect draft_rect{x, window.button_top, draft_w, window.button_height};
    if (unsaved != 0) {
        kit::KeycapLook dot;
        dot.mark = kit::KeyMark::changed;
        dot.mark_only = true;
        kit::add_keycap(
            built.list,
            {x, window.button_top + (window.button_height - kDot) / 2, kDot, kDot},
            dot,
            kit::no_control,
            {}
        );
    }
    add_line(
        built,
        fonts,
        {x + (unsaved == 0 ? 0 : kDot + metrics.space),
         window.button_top,
         kit::text_width(fonts, kit::FontRole::small, draft),
         window.button_height},
        draft,
        kit::FontRole::small,
        unsaved == 0 ? kit::colour::quiet : kit::colour::text
    );
    {
        kit::Control control;
        control.id = controls::draft;
        control.rect = draft_rect;
        control.enabled = true;
        control.focusable = false;
        control.name = std::string(name_draft);
        control.kind = kit::ControlKind::area;
        control.text = draft;
        built.rest.push_back(std::move(control));
    }
    x += draft_w + gap;
    const FootButton undo =
        make(controls::undo, name_undo, words::undo, kit::ButtonStyle::quiet, editor.can_undo());
    const int32_t undo_w = button_width(fonts, metrics, undo.caption);
    place(undo, x, undo_w);
    x += undo_w + gap;

    std::vector<FootButton> right;
    if (!page)
        right.push_back(
            make(controls::more, name_more, words::more_title, kit::ButtonStyle::plain, true)
        );
    if (!page)
        right.push_back(
            make(controls::cancel, name_cancel, words::cancel_title, kit::ButtonStyle::plain, true)
        );
    // While a clash holds Save changes back it keeps no accent, so it does not
    // read as the one to press.
    right.push_back(make(
        controls::save,
        name_save,
        words::save_changes,
        editor.can_save() ? kit::ButtonStyle::accent : kit::ButtonStyle::plain,
        editor.can_save()
    ));
    std::vector<int32_t> widths;
    int32_t right_total = 0;
    for (const FootButton& button : right) {
        widths.push_back(button_width(fonts, metrics, button.caption));
        right_total += widths.back();
    }
    right_total += gap * (static_cast<int32_t>(right.size()) - 1);
    int32_t right_x = std::max(window.right - right_total, x);

    const std::size_t clashes = editor.draft_clashes();
    std::string note;
    if (clashes == 1)
        note = std::string(show(editor.text(), words::settle_one));
    else if (clashes > 1)
        note = fill(show(editor.text(), words::settle), {{"n", std::to_string(clashes)}});
    const int32_t note_room = right_x - gap - x;
    if (!note.empty() && note_room > 8) {
        note = kit::cut_to_width(fonts, kit::FontRole::small, note, note_room);
        const int32_t text_w =
            std::min(kit::text_width(fonts, kit::FontRole::small, note), note_room);
        const kit::Rect rect{
            right_x - gap - text_w, window.button_top, text_w, window.button_height
        };
        add_line(built, fonts, rect, note, kit::FontRole::small, kit::colour::danger);
        kit::Control control;
        control.id = controls::footer_note;
        control.rect = rect;
        control.enabled = true;
        control.focusable = false;
        control.name = std::string(name_footer_note);
        control.kind = kit::ControlKind::area;
        control.text = note;
        built.rest.push_back(std::move(control));
    }
    for (std::size_t index = 0; index < right.size(); ++index) {
        place(right[index], right_x, widths[index]);
        right_x += widths[index] + gap;
    }
}

/// True when a category-menu item is the filter the editor is using.
///
/// Compact keeps the filters in that menu. The menu's chosen row stays the
/// category, and the filter's own row still reports that it is on, as its
/// pill does at Regular and Large.
///
/// @param entry the item
/// @param filter the filter in use
/// @return true when the item is that filter
bool filter_in_use(const MenuEntry& entry, Filter filter) {
    if (entry.kind != MenuKind::filter)
        return false;
    switch (filter) {
    case Filter::changed:
        return entry.id == "changed";
    case Filter::clashes:
        return entry.id == "clashes";
    case Filter::unbound:
        return entry.id == "unbound";
    case Filter::latchable:
        return entry.id == "latchable";
    case Filter::all:
        break;
    }
    return entry.id == "all";
}

/// Appends the open menu over the window.
///
/// @param[in,out] built the layout
/// @param editor the editor
/// @param state the menu
/// @param fonts the fonts
/// @param metrics the size class
/// @param window the window
/// @param field the drop-down's field
/// @param compact Compact's filters live in the category menu
void add_menu(
    Built& built,
    const Editor& editor,
    const ScreenState& state,
    const kit::Fonts& fonts,
    const kit::Metrics& metrics,
    const Window& window,
    const kit::Rect& field,
    bool compact
) {
    if (state.menu == Menu::none || !roomy(field))
        return;
    const std::vector<MenuEntry> entries = menu_entries(editor, state.menu, state, compact);
    if (entries.empty())
        return;
    const kit::Rect menu = kit::choice_menu(field, entries.size(), window.footer_rule);
    const kit::Rect whole{0, 0, window.width, window.height};
    if (!kit::wholly_in(menu, whole))
        return;
    const int32_t shown_count = kit::shown_choices(entries.size());
    const int32_t count = static_cast<int32_t>(entries.size());
    int32_t first = state.menu_first;
    if (first < 0)
        first = 0;
    if (first > std::max(count - shown_count, int32_t{0}))
        first = std::max(count - shown_count, int32_t{0});
    int32_t marked = state.menu_marked;
    if (marked < 0 || marked >= count)
        marked = -1;
    kit::ChoiceMenuLook look;
    look.first = first;
    look.total = count;
    look.chosen = menu_chosen(editor, state.menu, state, entries);
    look.marked = marked;
    look.focus_shown = state.interaction.focus_shown;
    look.items.resize(entries.size());
    for (int32_t index = 0; index < count; ++index) {
        const MenuEntry& entry = entries[static_cast<std::size_t>(index)];
        kit::ChoiceMenuRow row;
        row.divider = entry.kind == MenuKind::divider;
        row.choosable = !row.divider;
        row.id = entry.name;
        look.items[static_cast<std::size_t>(index)] = row;
        const bool visible = index >= first && index < first + shown_count;
        if (row.divider) {
            if (visible) {
                const kit::Rect box = kit::choice_item(menu, index - first);
                look.shown.push_back(
                    kit::cut_to_width(
                        fonts,
                        kit::FontRole::regular,
                        entry.caption,
                        std::max(box.width - metrics.choice_item_text_inset, int32_t{0})
                    )
                );
            }
            continue;
        }
        kit::Control control;
        control.id = controls::menu_first + index;
        control.name = entry.name;
        control.kind = kit::ControlKind::list_item;
        control.focusable = false;
        control.checked = index == look.chosen || filter_in_use(entry, state.filter);
        if (visible) {
            const kit::Rect box = kit::choice_item(menu, index - first);
            const std::string caption = kit::cut_to_width(
                fonts,
                kit::FontRole::regular,
                entry.caption,
                std::max(box.width - metrics.choice_item_text_inset, int32_t{0})
            );
            look.shown.push_back(caption);
            control.rect = box;
            control.text = caption;
            control.enabled = true;
        } else {
            control.enabled = false;
            control.text = entry.caption;
        }
        built.menu.push_back(std::move(control));
    }
    add_item(built.list, kit::Role::choice_menu, menu).look = std::move(look);
}

/// Appends the focus ring for the focused control, when it sits inside the window.
///
/// @param[in,out] built the layout
/// @param state the focus
/// @param metrics the size class
/// @param window the window
void add_focus(
    Built& built, const ScreenState& state, const kit::Metrics& metrics, const Window& window
) {
    if (!state.interaction.focus_shown || state.interaction.focused == kit::no_control)
        return;
    if (state.interaction.focused == controls::search)
        return;
    const kit::Rect whole{0, 0, window.width, window.height};
    for (const kit::Control& control : built.rest) {
        if (control.id != state.interaction.focused || !control.focusable)
            continue;
        if (!kit::wholly_in(kit::grown(control.rect, metrics.focus_inset), whole))
            return;
        const bool on_edge = control.kind == kit::ControlKind::list;
        kit::add_focus_ring(built.list, control.rect, !on_edge);
        return;
    }
}

/// The first letter in capitals, for the clash question's place.
///
/// @param text the phrase
/// @return the phrase
std::string capital_first(std::string text) {
    if (!text.empty()) {
        const auto letter = static_cast<unsigned char>(text[0]);
        if (letter >= 'a' && letter <= 'z')
            text[0] = static_cast<char>(letter - 'a' + 'A');
    }
    return text;
}

/// A mouse button's keycap. The stored numbers are not the kit's keycap words.
///
/// @param button the chord's button, 1 to 8
/// @return the word in the well
std::string mouse_cap(uint8_t button) {
    if (button == 1)
        return "Left";
    if (button == 2)
        return "Right";
    if (button == 3)
        return "Middle";
    if (button == 4)
        return "Back";
    if (button == 5)
        return "Forward";
    return "Mouse " + std::to_string(button);
}

/// True when Means can offer the letter for this chord.
///
/// @param editor the keyboard
/// @param chord the chord
/// @return true when a key on this keyboard prints it
bool letter_choice(const Editor& editor, const keys::Chord& chord) {
    if (chord.kind == keys::TriggerKind::named || chord.kind == keys::TriggerKind::mouse ||
        chord.kind == keys::TriggerKind::wheel || chord.kind == keys::TriggerKind::modifier_alone ||
        chord.kind == keys::TriggerKind::pad)
        return false;
    keys::Chord trial;
    trial.kind = keys::TriggerKind::character;
    trial.character = chord.character;
    if (trial.character == 0 && chord.scancode != 0)
        trial.character = editor.keyboard().printed(chord.scancode);
    return keys::on_keyboard(trial, editor.keyboard());
}

/// True when an action holds a chord, as a player would call it the same key.
///
/// @param editor the bindings and the keyboard
/// @param id the action
/// @param chord the chord
/// @return true when one of its chords matches
bool holds_chord(const Editor& editor, std::string_view id, const keys::Chord& chord) {
    for (const ChordDetail& one : editor.details(id).chords) {
        if (chords_match(one.chord, chord, editor.keyboard(), editor.platform()))
            return true;
    }
    return false;
}

/// One button of a prompt.
struct PromptButton {
    kit::ControlId id{kit::no_control}; ///< its number
    std::string name{};                 ///< its automation name
    std::string caption{};              ///< its words
    bool accent{false};                 ///< the filled style
    bool quiet{false};                  ///< the quiet style, words with no face
    bool enabled{true};                 ///< a press reaches it
};

/// The capture prompt or the clash question, the size of its parts.
///
/// The box is centred over the editor. The caller lays the editor under it
/// and dims that editor; this list is only the prompt.
///
/// @param editor the editor
/// @param state the prompt
/// @param frame the frame
/// @param fonts the fonts
/// @return the display list
/// One button on SHARE…, PASTE… or IMPORT….
struct ExchangeButton {
    kit::ControlId id{kit::no_control}; ///< its number
    std::string name{};                 ///< its automation name
    std::string caption{};              ///< its words
    bool accent{false};                 ///< the filled style
    bool enabled{true};                 ///< a press does something
};

/// An action's name as the list names it: its details' name, else the
/// registry's, else its id, which only an action this version does not know
/// keeps.
///
/// @param editor the actions
/// @param id the action
/// @return the name
std::string listed_name(const Editor& editor, std::string_view id) {
    const Details details = editor.details(id);
    if (!details.name.empty())
        return details.name;
    if (const oa::data::input_actions::Action* action = oa::data::input_actions::find(id))
        return show(editor.text(), action->words.name);
    return std::string(id);
}

/// Chords as the player reads them, ", " between them.
///
/// @param editor the keyboard and the platform
/// @param chords the chords
/// @return the words
std::string chords_text(const Editor& editor, const std::vector<keys::Chord>& chords) {
    std::string text;
    for (const keys::Chord& chord : chords) {
        if (!text.empty())
            text += ", ";
        text += keys::display_text(chord, editor.keyboard(), editor.platform());
    }
    return text;
}

/// One of Import's sources: its button and the line that says what it reads.
struct ImportSource {
    ExchangeButton button{}; ///< the button
    std::string aside{};     ///< the line beside it; empty for none
};

/// The left-and-right sentence, in the player's language.
///
/// The reader stores the English sentence. Shift, Ctrl and Alt stay as written.
///
/// @param editor the words
/// @param note the reader's sentence; empty when the key is not a sided modifier
/// @return the sentence, or empty
std::string hands_sentence(const Editor& editor, std::string_view note) {
    std::string_view modifier;
    if (note.find("Shift") != std::string_view::npos)
        modifier = "Shift";
    else if (note.find("Ctrl") != std::string_view::npos)
        modifier = "Ctrl";
    else if (note.find("Alt") != std::string_view::npos)
        modifier = "Alt";
    if (modifier.empty())
        return std::string(note);
    return fill(show(editor.text(), words::hands_are_one), {{"modifier", std::string(modifier)}});
}

/// Lays SHARE…, PASTE… or IMPORT… as a Kit 2 question over the editor, as the
/// capture and the clash question are: as wide as they are, inset from the
/// window by Kit 2's window padding, as tall as its content, with a rule over
/// the footer and control-high buttons sized to their words, the main one at
/// the right.
///
/// The text block scrolls when its lines do not fit above the buttons.
///
/// @param editor the model
/// @param state the prompt and its fields
/// @param frame the frame
/// @param fonts the fonts
/// @return the prompt
kit::DisplayList exchange_prompt(
    const Editor& editor, const ScreenState& state, const kit::Frame& frame, const kit::Fonts& fonts
) {
    kit::DisplayList list;
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    const Window window = window_of(frame);
    constexpr int32_t kPromptWide = 560;
    const int32_t kSectionGap = metrics.space * 2;
    const int32_t pad = metrics.window_padding;
    const int32_t box_w = std::min(kPromptWide, std::max(window.width - pad * 2, int32_t{1}));
    const int32_t column_x = pad;
    const int32_t column_w = std::max(box_w - pad * 2, int32_t{1});
    const int32_t button_h = metrics.control_height;
    // The footer: a space under its rule, then the buttons.
    const int32_t footer_h = metrics.space + button_h;
    const int32_t button_row = std::max(window.height - pad - button_h, pad);
    const int32_t content_stop = std::max(button_row - metrics.space - kSectionGap, pad);
    int32_t y = pad;

    auto text_at = [&](kit::Rect rect,
                       std::string words,
                       kit::FontRole font,
                       kit::Colour ink,
                       kit::Rect clip = {}) {
        if (rect.width <= 0 || rect.height <= 0 || words.empty())
            return;
        words = kit::cut_to_width(fonts, font, std::move(words), rect.width);
        if (words.empty())
            return;
        kit::Item item;
        item.role = kit::Role::text;
        item.rect = rect;
        item.clip = clip;
        item.text = std::move(words);
        item.font = font;
        item.colour = ink;
        item.align = kit::Align::left;
        list.items.push_back(std::move(item));
    };
    auto wrapped = [&](int32_t at,
                       std::string words,
                       kit::FontRole font,
                       kit::Colour ink,
                       int32_t most_lines) -> int32_t {
        const int32_t line_h =
            font == kit::FontRole::regular ? metrics.regular_line : metrics.small_line;
        if (column_w <= 0 || words.empty() || at >= content_stop)
            return 0;
        const auto measure = [&](std::string_view measured) {
            return kit::text_width(fonts, font, measured);
        };
        std::vector<std::string> lines =
            kit::wrap(std::move(words), column_w, measure, kit::WrapRules{.newlines = true});
        int32_t used = 0;
        int32_t drawn = 0;
        for (std::string& line : lines) {
            if (at + used + line_h > content_stop || drawn >= most_lines)
                break;
            text_at({column_x, at + used, column_w, line_h}, std::move(line), font, ink);
            used += line_h;
            ++drawn;
        }
        return used;
    };
    auto area = [&](kit::Rect rect, kit::ControlId id, std::string_view name, std::string words) {
        if (rect.width <= 0 || rect.height <= 0)
            return;
        kit::Control control;
        control.id = id;
        control.rect = rect;
        control.enabled = true;
        control.focusable = false;
        control.name = std::string(name);
        control.kind = kit::ControlKind::area;
        control.text = std::move(words);
        list.controls.push_back(std::move(control));
    };
    // Places a row of buttons in a room, from its left or against its right,
    // and returns the column the first one took.
    auto place_buttons = [&](int32_t x,
                             int32_t row,
                             int32_t room,
                             const std::vector<ExchangeButton>& buttons,
                             bool align_right) -> int32_t {
        if (buttons.empty() || room <= 0)
            return x + std::max(room, int32_t{0});
        const int32_t gap = metrics.button_gap;
        std::vector<int32_t> widths;
        widths.reserve(buttons.size());
        int32_t total = 0;
        for (const ExchangeButton& button : buttons) {
            const int32_t width = button_width(fonts, metrics, button.caption);
            widths.push_back(width);
            total += width;
        }
        if (buttons.size() > 1)
            total += gap * (static_cast<int32_t>(buttons.size()) - 1);
        int32_t shrinks = 0;
        while (total > room && shrinks < 10000) {
            ++shrinks;
            std::size_t widest = 0;
            for (std::size_t index = 1; index < widths.size(); ++index) {
                if (widths[index] > widths[widest])
                    widest = index;
            }
            if (widths[widest] <= 8)
                break;
            const int32_t cut = std::min(widths[widest] - 8, total - room);
            widths[widest] -= cut;
            total -= cut;
        }
        int32_t cursor = x;
        if (align_right)
            cursor = x + std::max(room - total, int32_t{0});
        const int32_t first = cursor;
        for (std::size_t button_at = 0; button_at < buttons.size(); ++button_at) {
            const ExchangeButton& button = buttons[button_at];
            const int32_t room_left = x + room - cursor;
            if (room_left <= 0)
                break;
            int32_t width = std::min(widths[button_at], room_left);
            const kit::Rect rect{cursor, row, width, button_h};
            if (rect.width <= 0)
                continue;
            kit::ButtonLook look;
            look.caption =
                kit::cut_to_width(fonts, kit::FontRole::small, button.caption, rect.width);
            // A button that cannot be pressed keeps no accent, as the
            // capture's does, so it does not read as the one to press.
            look.style = button.accent && button.enabled ? kit::ButtonStyle::accent
                                                         : kit::ButtonStyle::plain;
            look.enabled = button.enabled;
            look.hovered = hot(state, button.id);
            look.held = held(state, button.id);
            kit::add_button(list, rect, look, button.id, button.name);
            if (!list.controls.empty() && !button.enabled)
                list.controls.back().focusable = false;
            else if (button.enabled)
                list.tab_order.push_back(button.id);
            cursor += width + gap;
        }
        return first;
    };
    auto make = [&](kit::ControlId id,
                    std::string_view name,
                    std::string_view word,
                    bool accent,
                    bool enabled) {
        ExchangeButton button;
        button.id = id;
        button.name = std::string(name);
        button.caption = show(editor.text(), word);
        button.accent = accent;
        button.enabled = enabled;
        return button;
    };
    auto title = [&](std::string_view word) {
        const int32_t used =
            wrapped(y, show(editor.text(), word), kit::FontRole::regular, kit::colour::text, 2);
        y += std::max(used, metrics.regular_line);
        y += kSectionGap;
    };
    auto line_height = [&](kit::FontRole font) {
        return font == kit::FontRole::regular ? metrics.regular_line : metrics.small_line;
    };
    auto block_height = [&](const std::string& words, kit::FontRole font, int32_t most_lines) {
        const int32_t line_h = line_height(font);
        if (column_w <= 0 || line_h <= 0 || words.empty() || most_lines <= 0)
            return int32_t{0};
        const auto measure = [&](std::string_view measured) {
            return kit::text_width(fonts, font, measured);
        };
        const std::vector<std::string> lines =
            kit::wrap(words, column_w, measure, kit::WrapRules{.newlines = true});
        const int32_t count = std::min(static_cast<int32_t>(lines.size()), most_lines);
        return count * line_h;
    };
    /// Draws a block of lines in a well. A new line starts a line, and a line
    /// wider than the column wraps. A block taller than `room` scrolls, and
    /// the bar sits in the panel's padding so the lines keep their width.
    ///
    /// @param words the lines, as the control names them
    /// @param font which font
    /// @param ink the colour
    /// @param room how tall the block may be, in points
    /// @param id the control
    /// @param name the control's name
    /// @return the height the block took
    auto place_block = [&](const std::string& words,
                           kit::FontRole font,
                           kit::Colour ink,
                           int32_t room,
                           kit::ControlId id,
                           std::string_view name) -> int32_t {
        const int32_t line_h = line_height(font);
        if (column_w <= 0 || line_h <= 0 || room <= 0 || words.empty())
            return 0;
        const auto measure = [&](std::string_view measured) {
            return kit::text_width(fonts, font, measured);
        };
        // Keep every line. The room is the view, so a text taller than the
        // room scrolls instead of being cut off after a fixed count.
        std::vector<std::string> lines =
            kit::wrap(words, column_w, measure, kit::WrapRules{.newlines = true});
        if (lines.empty())
            return 0;
        const int32_t content_h = static_cast<int32_t>(lines.size()) * line_h;
        int32_t view_h = std::min(content_h, room);
        if (view_h >= line_h)
            view_h = (view_h / line_h) * line_h;
        if (view_h < 1)
            return 0;
        const bool scrolling = content_h > view_h;
        const kit::Rect port{column_x, y, column_w, view_h};
        add_item(list, kit::Role::fill, port).colour = kit::colour::well;
        int32_t offset = 0;
        if (scrolling) {
            const int32_t limit = std::max(content_h - view_h, int32_t{0});
            offset = std::clamp(state.exchange_scroll, int32_t{0}, limit);
            if (line_h > 0)
                offset = (offset / line_h) * line_h;
        }
        const kit::Rect clip = scrolling ? port : kit::Rect{};
        for (std::size_t line_at = 0; line_at < lines.size(); ++line_at) {
            const int32_t line_top = y + static_cast<int32_t>(line_at) * line_h - offset;
            if (line_top + line_h <= y || line_top >= y + view_h)
                continue;
            text_at({column_x, line_top, column_w, line_h}, lines[line_at], font, ink, clip);
        }
        area(port, id, name, words);
        if (!scrolling)
            return view_h;
        const int32_t bar_w = metrics.scroll_thumb_width + 2;
        const int32_t bar_x = column_x + column_w + std::max((pad - bar_w) / 2, int32_t{1});
        const kit::Rect bar_rect{bar_x, y, bar_w, view_h};
        if (bar_rect.x + bar_rect.width > box_w)
            return view_h;
        kit::ScrollArea scrolled;
        scrolled.view = port;
        scrolled.well = bar_rect;
        scrolled.content_height = content_h;
        scrolled.limit = std::max(content_h - view_h, int32_t{0});
        scrolled.offset = offset;
        kit::ScrollBarLook bar;
        bar.thumb = scrolled.thumb(metrics.least_thumb_height);
        bar.hot = hot(state, controls::exchange_bar);
        add_item(list, kit::Role::scroll_bar, bar_rect).look = bar;
        kit::Control control;
        control.id = controls::exchange_bar;
        control.rect = bar_rect;
        control.enabled = true;
        control.focusable = false;
        control.name = std::string(name_exchange_bar);
        control.kind = kit::ControlKind::scroll_bar;
        // The wheel reads the block's height back from the bar.
        control.group = content_h;
        list.controls.push_back(std::move(control));
        return view_h;
    };
    /// Pins the footer under the content, as the clash question's: a rule,
    /// the `left` buttons from the left and the `right` ones against the
    /// right, the main one last; then covers it all with a centred panel.
    auto place_footer = [&](const std::vector<ExchangeButton>& left,
                            const std::vector<ExchangeButton>& right) {
        if (y > pad)
            y += kSectionGap;
        int32_t footer_y = y;
        if (footer_y + footer_h + pad > window.height)
            footer_y = std::max(window.height - pad - footer_h, int32_t{0});
        add_item(list, kit::Role::fill, {0, footer_y, box_w, 1}).colour = kit::colour::rule;
        footer_y += metrics.space;
        int32_t left_x = column_x;
        if (!left.empty()) {
            const int32_t right_w = [&] {
                int32_t width = 0;
                for (const ExchangeButton& button : right)
                    width += button_width(fonts, metrics, button.caption) + metrics.button_gap;
                return width;
            }();
            const int32_t left_room = std::max(column_w - right_w, column_w / 2);
            left_x = place_buttons(column_x, footer_y, left_room, left, false);
            for (const ExchangeButton& button : left)
                left_x += button_width(fonts, metrics, button.caption) + metrics.button_gap;
            left_x = std::min(left_x, column_x + column_w);
        }
        place_buttons(left_x, footer_y, column_x + column_w - left_x, right, true);
        const int32_t bottom = footer_y + button_h;
        int32_t box_h = bottom + pad;
        if (box_h > window.height)
            box_h = window.height;
        if (box_h < 1)
            box_h = 1;
        kit::Item edge;
        edge.role = kit::Role::bevel;
        edge.rect = {0, 0, box_w, box_h};
        edge.colour = kit::colour::edge_light;
        kit::Item face;
        face.role = kit::Role::fill;
        face.rect = {0, 0, box_w, box_h};
        face.colour = kit::colour::panel;
        list.items.insert(list.items.begin(), std::move(edge));
        list.items.insert(list.items.begin(), std::move(face));
        const int32_t origin_x = std::max((window.width - box_w) / 2, int32_t{0});
        const int32_t origin_y = std::max((window.height - box_h) / 2, int32_t{0});
        for (kit::Item& item : list.items) {
            item.rect.x += origin_x;
            item.rect.y += origin_y;
            if (item.clip.width > 0 && item.clip.height > 0) {
                item.clip.x += origin_x;
                item.clip.y += origin_y;
            }
        }
        for (kit::Control& control : list.controls) {
            control.rect.x += origin_x;
            control.rect.y += origin_y;
            if (control.clip.width > 0 && control.clip.height > 0) {
                control.clip.x += origin_x;
                control.clip.y += origin_y;
            }
        }
    };

    if (state.prompt == Prompt::share) {
        const ShareSummary summary = share_summary(editor);
        const std::optional<std::string> id = id_from_name(editor.share_name());
        title(words::share_title);
        const int32_t label_h = metrics.small_line;
        if (y + label_h <= content_stop) {
            text_at(
                {column_x, y, column_w, label_h},
                show(editor.text(), words::share_name_label),
                kit::FontRole::small,
                kit::colour::quiet
            );
            y += label_h;
        }
        if (y + metrics.text_field_height <= content_stop) {
            const kit::Rect field{column_x, y, column_w, metrics.text_field_height};
            kit::SearchLook look;
            look.field = state.share_field;
            look.focused = focused_on(state, controls::share_name);
            look.hovered = hot(state, controls::share_name);
            look.magnifier = false;
            look.placeholder = show(editor.text(), words::share_name_label);
            kit::add_search(list, field, look, controls::share_name, std::string(name_share_name));
            list.tab_order.push_back(controls::share_name);
            y += metrics.text_field_height + metrics.space;
        }
        const int32_t id_h = metrics.small_line;
        if (y + id_h <= content_stop) {
            const std::string shown_id = id ? *id : std::string{};
            text_at(
                {column_x, y, column_w, id_h}, shown_id, kit::FontRole::small, kit::colour::hint
            );
            area({column_x, y, column_w, id_h}, controls::share_id, name_share_id, shown_id);
            y += id_h;
        }
        const std::string from = show(editor.text(), words::starts_from) + " " + summary.base;
        y += wrapped(y, from, kit::FontRole::small, kit::colour::text, 2);
        std::string_view counted = words::keys_and_helpers;
        if (summary.keys == 1 && summary.helpers == 1)
            counted = words::keys_one_helper_one;
        else if (summary.keys == 1)
            counted = words::keys_one_helpers;
        else if (summary.helpers == 1)
            counted = words::keys_helpers_one;
        const std::string changes = show(editor.text(), words::changes_label) + " " +
                                    fill(
                                        show(editor.text(), counted),
                                        {{"keys", std::to_string(summary.keys)},
                                         {"helpers", std::to_string(summary.helpers)}}
                                    );
        y += wrapped(y, changes, kit::FontRole::small, kit::colour::text, 2);
        if (!state.share_place.empty()) {
            y += wrapped(
                y,
                fill(show(editor.text(), words::saved_to), {{"place", state.share_place}}),
                kit::FontRole::small,
                kit::colour::text,
                2
            );
        }
        const std::string note = show(editor.text(), words::share_file_note);
        const int32_t note_h = block_height(note, kit::FontRole::small, 4);
        const int32_t room =
            std::max(window.height - y - note_h - kSectionGap - footer_h - pad, int32_t{0});
        y += place_block(
            summary.text,
            kit::FontRole::small,
            kit::colour::quiet,
            room,
            controls::share_text,
            name_share_text
        );
        y += wrapped(y, note, kit::FontRole::small, kit::colour::hint, 4);
        // Copy as text and Open folder at the left; Close and Save file…, the
        // main one, at the right.
        std::vector<ExchangeButton> aside;
        aside.push_back(
            make(controls::share_copy, name_share_copy, words::copy_as_text, false, true)
        );
        if (!state.share_place.empty()) {
            aside.push_back(
                make(controls::share_open, name_share_open, words::open_folder, false, true)
            );
        }
        std::vector<ExchangeButton> buttons;
        buttons.push_back(
            make(controls::share_close, name_share_close, words::close_word, false, true)
        );
        buttons.push_back(
            make(controls::share_save, name_share_save, words::save_file, true, id.has_value())
        );
        place_footer(aside, buttons);
        return list;
    }

    if (state.prompt == Prompt::paste) {
        title(words::paste_title);
        const std::string_view change_word =
            state.paste.changes == 1 ? words::text_changes_one : words::text_changes;
        const std::string summary = fill(
            show(editor.text(), change_word),
            {{"n", std::to_string(state.paste.changes)}, {"layout", editor.layout_name()}}
        );
        std::string spoken = summary;
        if (state.paste.clashes > 0) {
            spoken += ' ';
            spoken += std::to_string(state.paste.clashes);
        }
        if (state.paste.unknown > 0) {
            spoken += ' ';
            spoken += std::to_string(state.paste.unknown);
        }
        const int32_t summary_top = y;
        y += wrapped(y, summary, kit::FontRole::small, kit::colour::text, 3);
        if (y > summary_top)
            area(
                {column_x, summary_top, column_w, y - summary_top},
                controls::paste_summary,
                name_paste_summary,
                spoken
            );
        // Each row names its action as the list does, then its keys: "{chord}
        // does two things" for a clash, "place" for a key's place, and "Not in
        // this version" for an action only a later version knows.
        std::string rows;
        for (const PasteRow& row : state.paste.rows) {
            if (!rows.empty())
                rows += '\n';
            rows += listed_name(editor, row.action);
            const std::string chords = chords_text(editor, row.after);
            if (row.clash && !chords.empty())
                rows += " · " + fill(show(editor.text(), words::does_two), {{"chord", chords}});
            else if (!chords.empty())
                rows += " · " + chords;
            for (const keys::Chord& chord : row.after) {
                if (chord.kind != keys::TriggerKind::place)
                    continue;
                rows += " · ";
                rows += show(editor.text(), words::place_word);
                break;
            }
            if (row.unknown)
                rows += " · " + show(editor.text(), words::not_in_version);
        }
        for (const auto& problem : state.paste.problems) {
            if (!rows.empty())
                rows += '\n';
            rows += std::to_string(problem.line);
            rows += ':';
            rows += std::to_string(problem.column);
            rows += ": ";
            rows += problem.message;
        }
        std::string tail;
        for (const PasteRow& row : state.paste.rows) {
            if (!row.clash)
                continue;
            const std::string chord =
                row.after.empty() ? std::string{} : keys::chord_text(row.after.front());
            const std::string holder =
                listed_name(editor, row.holder.empty() ? row.action : row.holder);
            tail = fill(
                show(editor.text(), words::paste_clash), {{"chord", chord}, {"action", holder}}
            );
            break;
        }
        std::string kept;
        for (const PasteRow& row : state.paste.rows) {
            if (!row.unknown)
                continue;
            kept = fill(
                show(editor.text(), words::kept_for_later),
                {{"action", listed_name(editor, row.action)}}
            );
            break;
        }
        const std::string until = show(editor.text(), words::nothing_until_apply);
        const int32_t tail_h = block_height(tail, kit::FontRole::small, 3) +
                               block_height(kept, kit::FontRole::small, 2) +
                               block_height(until, kit::FontRole::small, 2);
        const int32_t room =
            std::max(window.height - y - tail_h - kSectionGap - footer_h - pad, int32_t{0});
        y += place_block(
            rows,
            kit::FontRole::small,
            kit::colour::text,
            room,
            controls::paste_list,
            name_paste_list
        );
        if (!tail.empty())
            y += wrapped(y, tail, kit::FontRole::small, kit::colour::hint, 3);
        if (!kept.empty())
            y += wrapped(y, kept, kit::FontRole::small, kit::colour::hint, 2);
        y += wrapped(y, until, kit::FontRole::small, kit::colour::hint, 2);
        std::vector<ExchangeButton> buttons;
        buttons.push_back(
            make(controls::paste_cancel, name_paste_cancel, words::cancel, false, true)
        );
        buttons.push_back(make(
            controls::paste_apply,
            name_paste_apply,
            state.paste.changes == 1 ? words::apply_change_one : words::apply_changes,
            true,
            state.paste.changes > 0
        ));
        // The caption keeps {n}. Fill it after make, which looks the word up whole.
        buttons.back().caption =
            fill(buttons.back().caption, {{"n", std::to_string(state.paste.changes)}});
        place_footer({}, buttons);
        return list;
    }

    title(words::import_title);
    const bool showing =
        state.import_chosen >= 0 || state.importing.found > 0 || !state.importing.rows.empty();
    if (!showing) {
        y += wrapped(
            y,
            show(editor.text(), words::nothing_until_apply),
            kit::FontRole::small,
            kit::colour::hint,
            2
        );
        std::vector<ImportSource> sources;
        bool eye = state.import_eye;
        bool tdraw = false;
        bool ddraw = false;
        for (const ImportOffer& offer : state.import_offers) {
            if (offer.id == "eye" || offer.tool == "eye")
                eye = true;
            else if (offer.tool == "tdraw")
                tdraw = true;
            else if (offer.tool == "cnc-ddraw")
                ddraw = true;
        }
        if (eye)
            sources.push_back(
                {make(controls::import_eye, name_import_eye, words::from_eye, false, true),
                 show(editor.text(), words::from_eye_aside)}
            );
        if (tdraw || state.import_offers.empty())
            sources.push_back(
                {make(controls::import_tdraw, name_import_tdraw, words::from_tdraw, false, true),
                 show(editor.text(), words::from_tdraw_aside)}
            );
        if (ddraw || state.import_offers.empty())
            sources.push_back(
                {make(controls::import_ddraw, name_import_ddraw, words::from_ddraw, false, true),
                 show(editor.text(), words::from_ddraw_aside)}
            );
        sources.push_back(
            {make(
                 controls::import_choose,
                 name_import_choose,
                 words::choose_another_file,
                 false,
                 true
             ),
             {}}
        );
        // Each source is a row: its button, and beside it the line that says
        // what it reads. The buttons share one width, so the lines align.
        int32_t button_w = 0;
        for (const ImportSource& source : sources)
            button_w = std::max(button_w, button_width(fonts, metrics, source.button.caption));
        button_w = std::min(button_w, std::max(column_w * 2 / 5, int32_t{1}));
        const int32_t aside_x = column_x + button_w + kSectionGap;
        const int32_t aside_w = std::max(column_x + column_w - aside_x, int32_t{1});
        const auto measure = [&](std::string_view measured) {
            return kit::text_width(fonts, kit::FontRole::small, measured);
        };
        constexpr std::size_t kAsideLines = 3;
        if (!sources.empty())
            y += metrics.space;
        for (const ImportSource& source : sources) {
            std::vector<std::string> lines;
            if (!source.aside.empty()) {
                kit::WrapRules rules;
                rules.most_lines = kAsideLines;
                rules.shorten_last = [&](std::string_view rest) {
                    return kit::cut_to_width(fonts, kit::FontRole::small, rest, aside_w);
                };
                lines = kit::wrap(source.aside, aside_w, measure, rules);
            }
            const int32_t lines_h = static_cast<int32_t>(lines.size()) * metrics.small_line;
            const int32_t row_h = std::max(button_h, lines_h);
            if (y + row_h > content_stop)
                break;
            place_buttons(column_x, y + (row_h - button_h) / 2, button_w, {source.button}, false);
            int32_t line_y = y + (row_h - lines_h) / 2;
            for (std::string& line : lines) {
                text_at(
                    {aside_x, line_y, aside_w, metrics.small_line},
                    std::move(line),
                    kit::FontRole::small,
                    kit::colour::hint
                );
                line_y += metrics.small_line;
            }
            y += row_h + metrics.space;
        }
        std::vector<ExchangeButton> buttons;
        buttons.push_back(
            make(controls::import_cancel, name_import_cancel, words::cancel, false, true)
        );
        place_footer({}, buttons);
        return list;
    }

    std::string file = state.importing.file;
    if (state.import_chosen >= 0 &&
        static_cast<std::size_t>(state.import_chosen) < state.import_offers.size())
        file = state.import_offers[static_cast<std::size_t>(state.import_chosen)].file;
    if (!file.empty()) {
        y += wrapped(
            y,
            fill(show(editor.text(), words::chosen_file), {{"file", file}}),
            kit::FontRole::small,
            kit::colour::text,
            2
        );
    }
    std::string_view found_word = words::found_differ;
    if (state.importing.found == 1 && state.importing.differ == 1)
        found_word = words::found_one_differs_one;
    else if (state.importing.found == 1)
        found_word = words::found_one_key;
    else if (state.importing.differ == 1)
        found_word = words::found_differs_one;
    const std::string found = fill(
        show(editor.text(), found_word),
        {{"n", std::to_string(state.importing.found)},
         {"m", std::to_string(state.importing.differ)},
         {"layout", editor.layout_name()}}
    );
    const int32_t found_top = y;
    y += wrapped(y, found, kit::FontRole::small, kit::colour::text, 3);
    if (y > found_top)
        area(
            {column_x, found_top, column_w, y - found_top},
            controls::import_summary,
            name_import_summary,
            found
        );
    // Each row names its action as the list does and the keys Import would
    // give it. A key Open Annihilation has no action for shows the file's own
    // line, the only name it has.
    std::string rows;
    for (const ImportRow& row : state.importing.rows) {
        if (!rows.empty())
            rows += '\n';
        const bool unused = row.unused || row.action.empty();
        rows += unused ? show(editor.text(), words::not_used) : listed_name(editor, row.action);
        const std::string chords = chords_text(editor, row.after);
        if (!chords.empty())
            rows += " · " + chords;
        else if (unused && !row.source_line.empty())
            rows += " · " + row.source_line;
        if (!row.note.empty())
            rows += " · " + hands_sentence(editor, row.note);
    }
    for (const auto& problem : state.importing.problems) {
        if (!rows.empty())
            rows += '\n';
        rows += std::to_string(problem.line);
        rows += ':';
        rows += std::to_string(problem.column);
        rows += ": ";
        rows += problem.message;
    }
    std::string moves;
    for (const ImportRow& moved : state.importing.rows) {
        if (moved.loses.empty())
            continue;
        moves = fill(
            show(editor.text(), words::import_moves), {{"action", listed_name(editor, moved.loses)}}
        );
        break;
    }
    const std::string numbers =
        fill(show(editor.text(), words::numbers_are_places), {{"file", file}});
    const int32_t tail_h = block_height(moves, kit::FontRole::small, 3) +
                           block_height(numbers, kit::FontRole::small, 4);
    const int32_t room =
        std::max(window.height - y - tail_h - kSectionGap - footer_h - pad, int32_t{0});
    y += place_block(
        rows, kit::FontRole::small, kit::colour::text, room, controls::import_list, name_import_list
    );
    if (!moves.empty())
        y += wrapped(y, moves, kit::FontRole::small, kit::colour::hint, 3);
    y += wrapped(y, numbers, kit::FontRole::small, kit::colour::hint, 4);
    std::vector<ExchangeButton> buttons;
    buttons.push_back(
        make(controls::import_cancel, name_import_cancel, words::cancel, false, true)
    );
    buttons.push_back(make(
        controls::import_apply,
        name_import_apply,
        state.importing.differ == 1 ? words::import_one_key : words::import_n_keys,
        true,
        state.importing.differ > 0
    ));
    // The caption keeps {n}. Fill it after make, which looks the word up whole.
    buttons.back().caption =
        fill(buttons.back().caption, {{"n", std::to_string(state.importing.differ)}});
    place_footer({}, buttons);
    return list;
}

kit::DisplayList prompt_layout(
    const Editor& editor, const ScreenState& state, const kit::Frame& frame, const kit::Fonts& fonts
) {
    if (state.prompt == Prompt::share || state.prompt == Prompt::paste ||
        state.prompt == Prompt::import)
        return exchange_prompt(editor, state, frame, fonts);
    kit::DisplayList list;
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    const Window window = window_of(frame);
    // Wide enough for the question at Large, and inset from the window at
    // every class so the dimmed editor stays visible around it.
    constexpr int32_t kPromptWide = 560;
    const int32_t kSectionGap = metrics.space * 2;
    constexpr int32_t kWellHigh = 76;
    constexpr int32_t kCapHigh = 44;
    const int32_t pad = metrics.window_padding;
    const int32_t box_w = std::min(kPromptWide, std::max(window.width - pad * 2, int32_t{1}));
    const int32_t column_x = pad;
    const int32_t column_w = std::max(box_w - pad * 2, int32_t{1});
    const int32_t content_stop =
        std::max(window.height - pad - metrics.control_height - pad - kSectionGap, pad);
    int32_t y = pad;

    auto text_at = [&](kit::Rect rect,
                       std::string words,
                       kit::FontRole font,
                       kit::Colour ink,
                       kit::Align align) {
        if (rect.width <= 0 || rect.height <= 0 || words.empty())
            return;
        words = kit::cut_to_width(fonts, font, std::move(words), rect.width);
        if (words.empty())
            return;
        kit::Item item;
        item.role = kit::Role::text;
        item.rect = rect;
        item.text = std::move(words);
        item.font = font;
        item.colour = ink;
        item.align = align;
        list.items.push_back(std::move(item));
    };
    auto wrapped = [&](int32_t x,
                       int32_t at,
                       int32_t width,
                       std::string words,
                       kit::FontRole font,
                       kit::Colour ink) -> int32_t {
        const int32_t line_h =
            font == kit::FontRole::regular ? metrics.regular_line : metrics.small_line;
        if (width <= 0 || words.empty() || at >= content_stop)
            return 0;
        const auto measure = [&](std::string_view words_measured) {
            return kit::text_width(fonts, font, words_measured);
        };
        std::vector<std::string> lines =
            kit::wrap(std::move(words), std::max(width, int32_t{1}), measure);
        int32_t used = 0;
        for (std::string& line : lines) {
            if (at + used + line_h > content_stop)
                break;
            text_at({x, at + used, width, line_h}, std::move(line), font, ink, kit::Align::left);
            used += line_h;
        }
        return used;
    };
    auto area = [&](kit::Rect rect, kit::ControlId id, std::string_view name, std::string words) {
        if (rect.width <= 0 || rect.height <= 0)
            return;
        kit::Control control;
        control.id = id;
        control.rect = rect;
        control.enabled = true;
        control.focusable = false;
        control.name = std::string(name);
        control.kind = kit::ControlKind::area;
        control.text = std::move(words);
        list.controls.push_back(std::move(control));
    };
    // Returns the column where the first button was placed.
    auto place_buttons = [&](int32_t x,
                             int32_t row,
                             int32_t room,
                             const std::vector<PromptButton>& buttons,
                             bool align_right) -> int32_t {
        if (buttons.empty() || room <= 0)
            return x + std::max(room, int32_t{0});
        const int32_t gap = metrics.button_gap;
        std::vector<int32_t> widths;
        widths.reserve(buttons.size());
        int32_t total = 0;
        for (const PromptButton& button : buttons) {
            const int32_t width = button_width(fonts, metrics, button.caption);
            widths.push_back(width);
            total += width;
        }
        if (buttons.size() > 1)
            total += gap * (static_cast<int32_t>(buttons.size()) - 1);
        int32_t shrinks = 0;
        while (total > room && shrinks < 10000) {
            ++shrinks;
            std::size_t widest = 0;
            for (std::size_t index = 1; index < widths.size(); ++index) {
                if (widths[index] > widths[widest])
                    widest = index;
            }
            if (widths[widest] <= 8)
                break;
            const int32_t cut = std::min(widths[widest] - 8, total - room);
            widths[widest] -= cut;
            total -= cut;
        }
        int32_t cursor = x;
        if (align_right)
            cursor = x + std::max(room - total, int32_t{0});
        const int32_t first = cursor;
        for (std::size_t button_at = 0; button_at < buttons.size(); ++button_at) {
            const PromptButton& button = buttons[button_at];
            int32_t width = widths[button_at];
            const int32_t room_left = x + room - cursor;
            if (room_left <= 0)
                break;
            if (width > room_left)
                width = room_left;
            const kit::Rect rect{cursor, row, width, metrics.control_height};
            if (rect.width <= 0)
                continue;
            kit::ButtonLook look;
            look.caption =
                kit::cut_to_width(fonts, kit::FontRole::small, button.caption, rect.width);
            look.style = button.accent  ? kit::ButtonStyle::accent
                         : button.quiet ? kit::ButtonStyle::quiet
                                        : kit::ButtonStyle::plain;
            look.enabled = button.enabled;
            look.hovered = hot(state, button.id);
            look.held = held(state, button.id);
            kit::add_button(list, rect, look, button.id, button.name);
            if (!list.controls.empty() && !button.enabled)
                list.controls.back().focusable = false;
            else if (button.enabled)
                list.tab_order.push_back(button.id);
            cursor += width + gap;
        }
        return first;
    };
    auto gap_after = [&]() {
        if (y + kSectionGap <= content_stop)
            y += kSectionGap;
    };

    // The keys of a chord, centred in a well: keycaps joined by "+", and a
    // gesture word after them. Held modifiers show while a chord waits.
    auto draw_keys = [&](const kit::Rect& well,
                         const std::optional<keys::Chord>& chord,
                         uint8_t held_modifiers,
                         kit::KeyMark mark,
                         const std::string& waiting_words) {
        keys::Chord shown;
        bool have = false;
        if (chord) {
            shown = *chord;
            have = true;
        } else if (held_modifiers != 0) {
            shown.kind = keys::TriggerKind::modifier_alone;
            shown.modifiers = held_modifiers;
            have = true;
        }
        if (!have) {
            int32_t text_w = kit::text_width(fonts, kit::FontRole::small, waiting_words);
            const int32_t room = std::max(well.width - 8, int32_t{1});
            if (text_w > room)
                text_w = room;
            const int32_t text_x = well.x + std::max((well.width - text_w) / 2, int32_t{0});
            const int32_t text_y =
                well.y + std::max((well.height - metrics.small_line) / 2, int32_t{0});
            text_at(
                {text_x, text_y, std::max(text_w, int32_t{1}), metrics.small_line},
                waiting_words,
                kit::FontRole::small,
                kit::colour::hint,
                kit::Align::centre
            );
            return;
        }
        std::vector<std::string> caps;
        std::string gesture;
        const std::string words_of_chord =
            keys::display_text(shown, editor.keyboard(), editor.platform());
        if (shown.kind == keys::TriggerKind::mouse) {
            caps.push_back(mouse_cap(shown.button));
        } else {
            const kit::ChordChipLook chip = chip_of(editor, shown, mark);
            caps = chip.keys;
            gesture = chip.gesture;
        }
        if (caps.empty() && gesture.empty() && !words_of_chord.empty())
            caps.push_back(words_of_chord);
        int32_t cap_h = kCapHigh;
        if (cap_h > well.height - 8)
            cap_h = well.height - 8;
        if (cap_h < kit::keycap_height)
            cap_h = std::min(kit::keycap_height, well.height);
        if (cap_h <= 0 || caps.empty())
            return;
        struct WellCap {
            std::string label;
            int32_t width{0};
            int32_t least{0};
            bool word{false};
            kit::KeyMark stamp{kit::KeyMark::none};
        };
        std::vector<WellCap> drawn;
        drawn.reserve(caps.size());
        for (std::size_t cap_at = 0; cap_at < caps.size(); ++cap_at) {
            WellCap one;
            one.label = caps[cap_at];
            one.word = one.label.size() > 1;
            one.stamp = cap_at + 1 == caps.size() ? mark : kit::KeyMark::none;
            kit::KeycapLook measure;
            measure.label = one.label;
            measure.modifier = one.word;
            one.least = std::max(kit::keycap_width(fonts, measure), int32_t{1});
            one.width = one.least;
            if (!one.word)
                one.width = std::max(one.width, cap_h);
            else
                one.width = std::max(one.width, cap_h * 66 / 42);
            drawn.push_back(std::move(one));
        }
        const int32_t plus_w =
            std::max(kit::text_width(fonts, kit::FontRole::regular, "+"), int32_t{1});
        const int32_t between = plus_w + 8;
        const int32_t gesture_w =
            gesture.empty() ? 0 : kit::text_width(fonts, kit::FontRole::small, gesture);
        auto group_width = [&]() {
            int32_t total = 0;
            for (const WellCap& one : drawn)
                total += one.width;
            if (drawn.size() > 1)
                total += between * (static_cast<int32_t>(drawn.size()) - 1);
            if (gesture_w > 0)
                total += 8 + gesture_w;
            return total;
        };
        const int32_t room = std::max(well.width - 8, int32_t{1});
        int32_t shrinks = 0;
        while (group_width() > room && shrinks < 10000) {
            ++shrinks;
            std::size_t widest = 0;
            for (std::size_t index = 1; index < drawn.size(); ++index) {
                if (drawn[index].width - drawn[index].least >
                    drawn[widest].width - drawn[widest].least)
                    widest = index;
            }
            if (drawn[widest].width <= drawn[widest].least)
                break;
            --drawn[widest].width;
        }
        int32_t total = group_width();
        if (total > room)
            total = room;
        int32_t cursor = well.x + std::max((well.width - total) / 2, int32_t{0});
        const int32_t cap_y = well.y + std::max((well.height - cap_h) / 2, int32_t{0});
        const int32_t limit = well.x + well.width - 4;
        for (std::size_t cap_at = 0; cap_at < drawn.size(); ++cap_at) {
            const WellCap& one = drawn[cap_at];
            if (cap_at > 0) {
                if (cursor + between > limit)
                    break;
                const int32_t plus_x = cursor + (between - plus_w) / 2;
                text_at(
                    {plus_x, cap_y, plus_w, cap_h},
                    "+",
                    kit::FontRole::regular,
                    kit::colour::quiet,
                    kit::Align::centre
                );
                cursor += between;
            }
            if (cursor >= limit)
                break;
            int32_t width = one.width;
            if (cursor + width > limit)
                width = limit - cursor;
            if (width < one.least)
                break;
            kit::KeycapLook cap;
            cap.label = one.label;
            cap.modifier = one.word;
            cap.mark = one.stamp;
            kit::add_keycap(list, {cursor, cap_y, width, cap_h}, cap, kit::no_control, {});
            cursor += width;
        }
        if (gesture_w > 0 && cursor + 8 < limit) {
            cursor += 8;
            const int32_t room_left = limit - cursor;
            if (room_left > 0)
                text_at(
                    {cursor, cap_y, room_left, cap_h},
                    gesture,
                    kit::FontRole::small,
                    kit::colour::hint,
                    kit::Align::left
                );
        }
    };
    // A well that takes a press: the capture's, or Find by key's.
    auto add_well = [&](kit::ControlId id, std::string_view name, std::string words, int32_t high) {
        int32_t well_h = high;
        if (y + well_h > content_stop)
            well_h = std::max(content_stop - y, int32_t{0});
        const kit::Rect well{column_x, y, column_w, well_h};
        if (!roomy(well))
            return well;
        add_item(list, kit::Role::fill, well).colour = kit::colour::band;
        kit::Control control;
        control.id = id;
        control.rect = well;
        control.enabled = true;
        control.focusable = false;
        control.name = std::string(name);
        control.kind = kit::ControlKind::area;
        control.text = std::move(words);
        list.controls.push_back(std::move(control));
        y += well.height;
        return well;
    };
    // A status badge, centred: free, in use, refused.
    auto badge = [&](std::string words, kit::PillState tone) {
        if (words.empty() || y + kit::pill_height() > content_stop)
            return;
        kit::PillLook pill;
        pill.state = tone;
        pill.text = std::move(words);
        int32_t width = kit::pill_width(fonts, pill);
        if (width > column_w) {
            pill.text = kit::cut_to_width(fonts, kit::FontRole::small, pill.text, column_w - 16);
            width = std::min(kit::pill_width(fonts, pill), column_w);
        }
        const kit::Rect rect{column_x, y, width, kit::pill_height()};
        kit::add_pill(list, rect, pill, kit::no_control, {});
        y += kit::pill_height();
    };
    auto eyebrow = [&](std::string words) {
        y += wrapped(
            column_x, y, column_w, std::move(words), kit::FontRole::small, kit::colour::quiet
        );
    };

    const Details bound = editor.details(state.capture.action);
    const std::string action_name = bound.name.empty() ? state.capture.action : bound.name;
    const CaptureStatus status = capture_status(editor, state.capture);
    std::string chord_words;
    if (state.capture.chord)
        chord_words =
            keys::display_text(*state.capture.chord, editor.keyboard(), editor.platform());
    const std::string context = context_phrase(editor, state.capture.action);
    std::vector<PromptButton> footer;
    std::vector<PromptButton> footer_left;
    std::string footer_hint;

    if (state.prompt == Prompt::find) {
        // Find by key: the chord heard, every action on it, and Open › on each.
        const std::string title = show(editor.text(), words::find_by_key);
        std::string finding_words;
        if (state.finding.chord)
            finding_words =
                keys::display_text(*state.finding.chord, editor.keyboard(), editor.platform());
        std::vector<ChordUse> uses;
        if (state.finding.chord)
            uses = find_by_chord(editor, *state.finding.chord);
        std::string line;
        if (!state.finding.chord)
            line = show(editor.text(), words::find_press);
        else if (uses.empty())
            line = fill(show(editor.text(), words::find_free), {{"chord", finding_words}});
        else if (uses.size() == 1)
            line = fill(show(editor.text(), words::find_uses_one), {{"chord", finding_words}});
        else
            line = fill(
                show(editor.text(), words::find_uses),
                {{"chord", finding_words}, {"n", std::to_string(uses.size())}}
            );
        y += wrapped(column_x, y, column_w, title, kit::FontRole::regular, kit::colour::text);
        gap_after();
        const kit::Rect well = add_well(
            controls::find_well,
            name_find_well,
            finding_words.empty() ? show(editor.text(), words::press_a_key) : finding_words,
            kWellHigh
        );
        if (roomy(well))
            draw_keys(
                well,
                state.finding.chord,
                state.finding.held_modifiers,
                kit::KeyMark::none,
                show(editor.text(), words::press_a_key)
            );
        gap_after();
        const int32_t line_top = y;
        y +=
            wrapped(column_x, y, column_w, line, kit::FontRole::small, kit::colour::text_secondary);
        std::string spoken = line;
        for (const ChordUse& use : uses)
            spoken += "\n" + use.name + " · " + use.context;
        area(
            {column_x, line_top, column_w, std::max(y - line_top, int32_t{1})},
            controls::find_title,
            name_find_prompt,
            std::move(spoken)
        );
        gap_after();
        const int32_t row_h = metrics.row_height;
        const std::string open_words = show(editor.text(), words::open_action);
        const int32_t open_w = kit::text_width(fonts, kit::FontRole::small, open_words);
        for (std::size_t index = 0; index < uses.size(); ++index) {
            if (y + row_h > content_stop)
                break;
            const ChordUse& use = uses[index];
            const kit::ControlId id =
                controls::find_row_first + static_cast<kit::ControlId>(static_cast<int32_t>(index));
            const kit::Rect row{column_x, y, column_w, row_h};
            add_item(list, kit::Role::fill, row).colour =
                hot(state, id) ? kit::colour::hover : kit::colour::list;
            const int32_t inset = metrics.space * 2;
            const int32_t text_w = std::max(column_w - inset * 3 - open_w, int32_t{1});
            const int32_t name_top =
                y + std::max((row_h - metrics.regular_line - metrics.small_line) / 2, int32_t{0});
            text_at(
                {column_x + inset, name_top, text_w, metrics.regular_line},
                use.name,
                kit::FontRole::regular,
                kit::colour::text,
                kit::Align::left
            );
            text_at(
                {column_x + inset, name_top + metrics.regular_line, text_w, metrics.small_line},
                capital_first(use.context),
                kit::FontRole::small,
                kit::colour::quiet,
                kit::Align::left
            );
            text_at(
                {column_x + column_w - inset - open_w, y, open_w, row_h},
                open_words,
                kit::FontRole::small,
                kit::colour::accent,
                kit::Align::left
            );
            kit::Control control;
            control.id = id;
            control.rect = row;
            control.enabled = true;
            control.focusable = true;
            control.name = std::string(name_find_row_prefix) + std::to_string(index + 1);
            control.kind = kit::ControlKind::button;
            control.text = use.name + " · " + use.context;
            list.controls.push_back(std::move(control));
            list.tab_order.push_back(id);
            y += row_h + 2;
        }
        if (state.finding.chord && y < content_stop)
            y += wrapped(
                column_x,
                y,
                column_w,
                show(editor.text(), words::find_again),
                kit::FontRole::small,
                kit::colour::hint
            );
        gap_after();
        PromptButton close;
        close.id = controls::find_close;
        close.name = std::string(name_find_close);
        close.caption = show(editor.text(), words::close_title);
        footer.push_back(std::move(close));
        footer_hint = show(editor.text(), words::find_escape);
    } else if (state.prompt == Prompt::clash) {
        // The one clash question: what each outcome leaves on both actions,
        // and nothing changes until the confirm button.
        const std::string here = state.pending_clash.a;
        const std::string there = state.pending_clash.b;
        const Details here_details = editor.details(here);
        const Details there_details = editor.details(there);
        const std::string here_name = here_details.name.empty() ? here : here_details.name;
        const std::string there_name = there_details.name.empty() ? there : there_details.name;
        const keys::Chord clash_chord =
            state.settling
                ? state.pending_clash.chord
                : (state.capture.chord ? *state.capture.chord : state.pending_clash.chord);
        const std::string clash_words =
            keys::display_text(clash_chord, editor.keyboard(), editor.platform());
        const std::string here_context = context_phrase(editor, here);

        struct Outcome {
            kit::ControlId id{};
            std::string name{};
            std::string title{};
            std::string confirm{};
            std::vector<keys::Chord> here_after{};
            std::vector<keys::Chord> there_after{};
        };

        std::vector<Outcome> outcomes;
        // Each outcome is worked out on a copy of the draft, so what it lists
        // is what its confirm button would leave on both actions.
        const auto chords_after = [&](const Editor& changed, const std::string& id) {
            std::vector<keys::Chord> chords;
            for (const ChordDetail& one : changed.details(id).chords)
                chords.push_back(one.chord);
            return chords;
        };
        const auto outcome_of = [&](Outcome& outcome, const Editor& changed) {
            outcome.here_after = chords_after(changed, here);
            outcome.there_after = chords_after(changed, there);
        };
        if (state.settling) {
            Outcome keep_here;
            keep_here.id = controls::card_here;
            keep_here.name = std::string(name_card_here);
            keep_here.title =
                fill(show(editor.text(), words::keep_it_for), {{"action", here_name}});
            keep_here.confirm = keep_here.title;
            Editor kept_here = editor;
            settle(kept_here, state.pending_clash, Settle::keep_here);
            outcome_of(keep_here, kept_here);
            outcomes.push_back(std::move(keep_here));
            Outcome keep_there;
            keep_there.id = controls::card_there;
            keep_there.name = std::string(name_card_there);
            keep_there.title =
                fill(show(editor.text(), words::keep_it_for), {{"action", there_name}});
            keep_there.confirm = keep_there.title;
            Editor kept_there = editor;
            settle(kept_there, state.pending_clash, Settle::keep_there);
            outcome_of(keep_there, kept_there);
            outcomes.push_back(std::move(keep_there));
        } else {
            Capture heard = state.capture;
            heard.chord = clash_chord;
            Outcome move;
            move.id = controls::clash_replace;
            move.name = std::string(name_clash_replace);
            move.title = fill(show(editor.text(), words::move_it_to), {{"action", here_name}});
            move.confirm = fill(show(editor.text(), words::move_to), {{"action", here_name}});
            Editor moved = editor;
            static_cast<void>(apply_capture(moved, heard, ClashChoice::replace));
            outcome_of(move, moved);
            outcomes.push_back(std::move(move));
            if (state.capture.replacing) {
                Outcome swap;
                swap.id = controls::clash_swap;
                swap.name = std::string(name_clash_swap);
                swap.title = show(editor.text(), words::swap_them);
                swap.confirm = swap.title;
                Editor swapped = editor;
                static_cast<void>(apply_capture(swapped, heard, ClashChoice::swap));
                outcome_of(swap, swapped);
                outcomes.push_back(std::move(swap));
            }
        }
        const std::size_t chosen = std::min<std::size_t>(state.outcome, outcomes.size() - 1);

        const std::string eyebrow_words =
            state.settling
                ? capital_first(here_context)
                : fill(show(editor.text(), words::new_key_for), {{"action", action_name}}) + " · " +
                      here_context;
        const std::string title =
            state.settling
                ? fill(show(editor.text(), words::does_two), {{"chord", clash_words}})
                : fill(show(editor.text(), words::already_in_use), {{"chord", clash_words}});
        const std::string body =
            state.settling ? fill(
                                 show(editor.text(), words::both_use),
                                 {{"name", there_name}, {"context", here_context}}
                             )
                           : fill(
                                 show(editor.text(), words::uses_it),
                                 {{"name", there_name}, {"context", context_phrase(editor, there)}}
                             );
        const int32_t title_top = y;
        eyebrow(eyebrow_words);
        y += wrapped(column_x, y, column_w, title, kit::FontRole::regular, kit::colour::text);
        gap_after();
        y +=
            wrapped(column_x, y, column_w, body, kit::FontRole::small, kit::colour::text_secondary);
        gap_after();
        std::string spoken = title + " " + body;

        auto chips_at = [&](int32_t x,
                            int32_t row,
                            int32_t room,
                            const std::vector<keys::Chord>& chords) {
            constexpr int32_t kChip = 20;
            if (chords.empty()) {
                text_at(
                    {x, row, room, kChip},
                    show(editor.text(), words::no_key),
                    kit::FontRole::small,
                    kit::colour::lock,
                    kit::Align::left
                );
                return;
            }
            int32_t chip_x = x;
            for (const keys::Chord& chord : chords) {
                const kit::ChordChipLook chip = chip_of(editor, chord, kit::KeyMark::none);
                const int32_t natural = std::max(kit::chord_chip_width(fonts, chip), int32_t{1});
                if (chip_x + natural > x + room)
                    break;
                kit::add_chord_chip(list, {chip_x, row, natural, kChip}, chip, kit::no_control, {});
                chip_x += natural + 4;
            }
        };
        const int32_t inset = metrics.space * 2;
        const int32_t radio = 12;
        const int32_t line_h = 22;
        const int32_t card_h = inset * 2 + metrics.regular_line + 4 + line_h * 2;
        for (std::size_t index = 0; index < outcomes.size(); ++index) {
            if (y + card_h > content_stop)
                break;
            const Outcome& outcome = outcomes[index];
            const bool on = index == chosen;
            const kit::Rect card{column_x, y, column_w, card_h};
            add_item(list, kit::Role::fill, card).colour =
                on ? kit::colour::list_selected
                   : (hot(state, outcome.id) ? kit::colour::hover : kit::colour::list);
            add_item(list, kit::Role::outline, card).colour =
                on ? kit::colour::accent : kit::colour::control_border;
            const kit::Rect ring{
                column_x + inset, y + inset + (metrics.regular_line - radio) / 2, radio, radio
            };
            add_item(list, kit::Role::outline, ring).colour =
                on ? kit::colour::accent : kit::colour::hint;
            if (on)
                add_item(list, kit::Role::fill, {ring.x + 3, ring.y + 3, radio - 6, radio - 6})
                    .colour = kit::colour::accent;
            const int32_t text_x = column_x + inset + radio + inset;
            const int32_t text_w = std::max(column_x + column_w - inset - text_x, int32_t{1});
            text_at(
                {text_x, y + inset, text_w, metrics.regular_line},
                outcome.title,
                kit::FontRole::regular,
                kit::colour::text,
                kit::Align::left
            );
            const int32_t label_w = std::min(text_w * 2 / 5, int32_t{170});
            int32_t row = y + inset + metrics.regular_line + 4;
            const std::pair<const std::string*, const std::vector<keys::Chord>*> lines[] = {
                {&here_name, &outcome.here_after}, {&there_name, &outcome.there_after}
            };
            std::string said = outcome.title;
            for (const auto& [who, after] : lines) {
                text_at(
                    {text_x, row, label_w, line_h},
                    *who,
                    kit::FontRole::small,
                    kit::colour::text_tertiary,
                    kit::Align::left
                );
                chips_at(
                    text_x + label_w + metrics.space,
                    row + 1,
                    text_w - label_w - metrics.space,
                    *after
                );
                said += "; " + *who + ": ";
                if (after->empty())
                    said += show(editor.text(), words::no_key);
                for (std::size_t chord_at = 0; chord_at < after->size(); ++chord_at) {
                    if (chord_at > 0)
                        said += ", ";
                    said += keys::display_text(
                        (*after)[chord_at], editor.keyboard(), editor.platform()
                    );
                }
                row += line_h;
            }
            spoken += " " + said + ".";
            kit::Control control;
            control.id = outcome.id;
            control.rect = card;
            control.enabled = true;
            control.focusable = true;
            control.checked = on;
            control.name = outcome.name;
            control.kind = kit::ControlKind::list_item;
            control.text = said;
            list.controls.push_back(std::move(control));
            list.tab_order.push_back(outcome.id);
            y += card_h + metrics.space;
        }
        // Uses of the chord that never meet this action's stay as they are.
        if (!state.settling) {
            for (const ChordUse& use : find_by_chord(editor, clash_chord)) {
                if (use.id == here || use.id == there)
                    continue;
                bool meets = false;
                for (const keys::Clash& clash : editor.clashes_for(here, clash_chord)) {
                    if (clash.a == use.id || clash.b == use.id)
                        meets = true;
                }
                if (meets)
                    continue;
                const std::string note = fill(
                    show(editor.text(), words::also_used),
                    {{"chord", clash_words},
                     {"name", use.name},
                     {"context", use.context},
                     {"where", here_context}}
                );
                // The note shows whole or not at all: half a sentence under
                // the outcomes would read as the whole of it.
                const auto note_measure = [&](std::string_view words_measured) {
                    return kit::text_width(fonts, kit::FontRole::small, words_measured);
                };
                const auto note_lines = static_cast<int32_t>(
                    kit::wrap(note, std::max(column_w - inset * 2, int32_t{1}), note_measure).size()
                );
                if (y + metrics.space * 2 + note_lines * metrics.small_line > content_stop)
                    break;
                const int32_t top = y;
                const std::size_t ground_at = list.items.size();
                const int32_t note_h = wrapped(
                    column_x + inset,
                    y + metrics.space,
                    column_w - inset * 2,
                    note,
                    kit::FontRole::small,
                    kit::colour::information
                );
                if (note_h > 0) {
                    kit::Item ground;
                    ground.role = kit::Role::fill;
                    ground.rect = {column_x, top, column_w, note_h + metrics.space * 2};
                    ground.colour = kit::colour::information_tint;
                    list.items.insert(
                        list.items.begin() + static_cast<std::ptrdiff_t>(ground_at), ground
                    );
                    y = top + ground.rect.height + metrics.space;
                    spoken += " " + note;
                }
                break;
            }
        }
        area(
            {column_x, title_top, column_w, std::max(y - title_top, metrics.small_line)},
            controls::clash_title,
            name_clash,
            std::move(spoken)
        );
        gap_after();
        const bool can_choose_another = !state.settling || holds_chord(editor, here, clash_chord);
        if (can_choose_another) {
            PromptButton another;
            another.id = controls::clash_another;
            another.name = std::string(name_clash_another);
            another.caption = show(editor.text(), words::choose_another_key);
            another.quiet = true;
            footer_left.push_back(std::move(another));
        }
        PromptButton cancel;
        cancel.id = controls::clash_cancel;
        cancel.name = std::string(name_clash_cancel);
        cancel.caption = show(editor.text(), words::cancel_title);
        footer.push_back(std::move(cancel));
        PromptButton confirm;
        confirm.id = controls::clash_confirm;
        confirm.name = std::string(name_clash_confirm);
        confirm.caption = outcomes[chosen].confirm;
        confirm.accent = true;
        footer.push_back(std::move(confirm));
    } else {
        // The capture: the key heard, what it would do, and More options.
        const std::string title =
            fill(show(editor.text(), words::new_key_for), {{"action", action_name}});
        std::string spoken = title;
        if (!chord_words.empty()) {
            spoken += ' ';
            spoken += chord_words;
        }
        if (!status.line.empty()) {
            spoken += ' ';
            spoken += status.line;
        }
        const int32_t title_top = y;
        eyebrow(fill(
            show(
                editor.text(), state.capture.replacing ? words::change_eyebrow : words::add_eyebrow
            ),
            {{"context", context}}
        ));
        y += wrapped(column_x, y, column_w, title, kit::FontRole::regular, kit::colour::text);
        if (y < title_top + metrics.regular_line)
            y = title_top + metrics.regular_line;
        area(
            {column_x, title_top, column_w, y - title_top},
            controls::capture_title,
            name_capture,
            std::move(spoken)
        );
        gap_after();
        kit::KeyMark mark = kit::KeyMark::none;
        if (state.capture.state == CaptureState::clash)
            mark = kit::KeyMark::clash;
        else if (state.capture.state == CaptureState::refused)
            mark = kit::KeyMark::fixed;
        const kit::Rect well = add_well(
            controls::capture_well,
            name_capture_well,
            chord_words.empty() ? show(editor.text(), words::press_a_key) : chord_words,
            kWellHigh
        );
        if (roomy(well)) {
            draw_keys(
                well,
                state.capture.chord,
                state.capture.held_modifiers,
                mark,
                show(editor.text(), words::press_a_key)
            );
            kit::Colour edge = kit::colour::control_border;
            if (state.capture.state == CaptureState::complete)
                edge = kit::colour::accent;
            else if (state.capture.state == CaptureState::clash)
                edge = kit::colour::lock;
            else if (state.capture.state == CaptureState::refused)
                edge = kit::colour::danger;
            add_item(list, kit::Role::outline, well).colour = edge;
        }
        gap_after();
        // The outcome, as a badge, and a line under it.
        const int32_t status_top = y;
        std::string under;
        if (state.capture.state == CaptureState::waiting) {
            under = show(editor.text(), words::hold_modifiers);
        } else if (state.capture.state == CaptureState::refused) {
            badge(status.line, kit::PillState::problem);
        } else if (state.capture.state == CaptureState::clash && status.clash) {
            badge(
                fill(
                    show(editor.text(), words::in_use),
                    {{"chord", chord_words}, {"context", context}}
                ),
                kit::PillState::update
            );
            std::vector<std::string> names;
            for (const keys::Clash& clash :
                 editor.clashes_for(state.capture.action, *state.capture.chord)) {
                const std::string other = clash.a == state.capture.action ? clash.b : clash.a;
                const Details row = editor.details(other);
                names.push_back(row.name.empty() ? other : row.name);
            }
            std::string joined;
            for (std::size_t index = 0; index < names.size(); ++index) {
                if (index > 0)
                    joined += ", ";
                joined += names[index];
            }
            under = fill(show(editor.text(), words::in_use_by), {{"name", joined}});
        } else {
            badge(
                fill(
                    show(editor.text(), words::free_badge),
                    {{"chord", chord_words}, {"context", context}}
                ),
                kit::PillState::installed
            );
            under = show(editor.text(), words::try_another);
        }
        if (!under.empty()) {
            if (y > status_top)
                y += metrics.space;
            y += wrapped(
                column_x, y, column_w, under, kit::FontRole::small, kit::colour::text_secondary
            );
        }
        if (!status.line.empty())
            area(
                {column_x, status_top, column_w, std::max(y - status_top, metrics.small_line)},
                controls::capture_status,
                name_capture_status,
                status.line
            );
        gap_after();

        // More options: the key's place, and how it acts.
        const bool device =
            state.capture.chord && (state.capture.chord->kind == keys::TriggerKind::mouse ||
                                    state.capture.chord->kind == keys::TriggerKind::wheel ||
                                    state.capture.chord->kind == keys::TriggerKind::modifier_alone);
        const bool show_letter =
            !state.capture.chord || letter_choice(editor, *state.capture.chord);
        const bool show_starts = !state.capture.chord ||
                                 (state.capture.chord->kind != keys::TriggerKind::wheel &&
                                  state.capture.chord->kind != keys::TriggerKind::modifier_alone);
        if (y + metrics.control_height <= content_stop) {
            PromptButton more;
            more.id = controls::capture_more;
            more.name = std::string(name_capture_more);
            more.caption = std::string(state.capture_more ? "▾ " : "› ") +
                           show(editor.text(), words::more_options);
            more.quiet = true;
            place_buttons(column_x, y, column_w, {more}, false);
            y += metrics.control_height;
        }
        if (state.capture_more) {
            const std::string place_label = show(editor.text(), words::same_place);
            const std::string acts_label = show(editor.text(), words::how_it_acts);
            int32_t label_column = std::max(
                kit::text_width(fonts, kit::FontRole::small, place_label),
                kit::text_width(fonts, kit::FontRole::small, acts_label)
            );
            label_column = std::min(label_column, column_w / 2);
            if (!device && show_letter && y + metrics.control_height <= content_stop) {
                text_at(
                    {column_x, y, label_column, metrics.control_height},
                    place_label,
                    kit::FontRole::small,
                    kit::colour::text,
                    kit::Align::left
                );
                const std::string off_caption = show(editor.text(), words::state_off);
                const std::string on_caption = show(editor.text(), words::state_on);
                const int32_t switch_w = std::min(
                    switch_width(fonts, metrics, off_caption, on_caption), column_w - label_column
                );
                kit::SwitchLook toggle;
                toggle.on = state.capture.means == oa::data::input_actions::By::place;
                toggle.hovered = hot(state, controls::capture_means);
                toggle.off_caption = off_caption;
                toggle.on_caption = on_caption;
                const kit::Rect rect{
                    column_x + column_w - switch_w,
                    y + (metrics.control_height - metrics.button_height) / 2,
                    switch_w,
                    metrics.button_height
                };
                if (roomy(rect)) {
                    kit::add_switch(
                        list, rect, toggle, controls::capture_means, std::string(name_capture_means)
                    );
                    list.tab_order.push_back(controls::capture_means);
                }
                y += metrics.control_height;
            } else if (
                !show_letter && state.capture.chord && !device &&
                y + metrics.small_line <= content_stop
            ) {
                text_at(
                    {column_x, y, column_w, metrics.small_line},
                    show(editor.text(), words::not_on_keyboard),
                    kit::FontRole::small,
                    kit::colour::lock,
                    kit::Align::left
                );
                y += metrics.small_line + metrics.space;
            }
            if (show_starts && y + metrics.control_height <= content_stop) {
                auto gesture_button = [&](kit::ControlId id,
                                          std::string_view name,
                                          std::string_view word,
                                          keys::Gesture which) {
                    PromptButton button;
                    button.id = id;
                    button.name = std::string(name);
                    button.caption = show(editor.text(), word);
                    button.accent = state.capture.starts == which;
                    return button;
                };
                text_at(
                    {column_x, y, label_column, metrics.control_height},
                    acts_label,
                    kit::FontRole::small,
                    kit::colour::text,
                    kit::Align::left
                );
                const int32_t strip_x = column_x + label_column + metrics.space * 2;
                const int32_t strip_w = std::max(column_x + column_w - strip_x, int32_t{1});
                place_buttons(
                    strip_x,
                    y,
                    strip_w,
                    {gesture_button(
                         controls::capture_press,
                         name_capture_press,
                         words::a_press,
                         keys::Gesture::press
                     ),
                     gesture_button(
                         controls::capture_double,
                         name_capture_double,
                         words::a_double_press,
                         keys::Gesture::double_press
                     ),
                     gesture_button(
                         controls::capture_hold,
                         name_capture_hold,
                         words::a_hold,
                         keys::Gesture::hold
                     )},
                    true
                );
                y += metrics.control_height;
            }
        }
        gap_after();

        PromptButton cancel;
        cancel.id = controls::capture_cancel;
        cancel.name = std::string(name_capture_cancel);
        cancel.caption = show(editor.text(), words::cancel_title);
        footer.push_back(std::move(cancel));
        PromptButton ok;
        ok.id = controls::capture_ok;
        ok.name = std::string(name_capture_ok);
        if (state.capture.state == CaptureState::clash)
            ok.caption = show(editor.text(), words::choose_outcome);
        else
            ok.caption = fill(
                show(editor.text(), words::use_for),
                {{"chord", chord_words.empty() ? std::string("…") : chord_words},
                 {"action", action_name}}
            );
        ok.accent = status.ok_enabled;
        ok.enabled = status.ok_enabled;
        footer.push_back(std::move(ok));
        footer_hint = show(editor.text(), words::escape_hint);
    }

    int32_t footer_y = y;
    if (footer_y + metrics.control_height + pad > window.height)
        footer_y = std::max(window.height - pad - metrics.control_height, int32_t{0});
    // A rule over the footer, then the quiet button at the left, the hint
    // after it, and the buttons from the right.
    add_item(list, kit::Role::fill, {0, footer_y - metrics.space, box_w, 1}).colour =
        kit::colour::rule;
    footer_y += metrics.space;
    int32_t left_x = column_x;
    if (!footer_left.empty()) {
        place_buttons(left_x, footer_y, column_w / 2, footer_left, false);
        left_x += button_width(fonts, metrics, footer_left.front().caption) + metrics.button_gap;
    }
    const int32_t buttons_x =
        place_buttons(left_x, footer_y, column_x + column_w - left_x, footer, true);
    const int32_t hint_w = buttons_x - metrics.button_gap - left_x;
    if (!footer_hint.empty() && hint_w > 24) {
        // A hint wider than its room takes as many lines as the buttons'
        // height holds, centred beside them, and only its last line is cut.
        const auto measure = [&](std::string_view words_measured) {
            return kit::text_width(fonts, kit::FontRole::small, words_measured);
        };
        std::vector<std::string> lines = kit::wrap(footer_hint, hint_w, measure);
        const auto most = static_cast<std::size_t>(
            std::max(metrics.control_height / metrics.small_line, int32_t{1})
        );
        if (lines.size() <= 1 || most < 2) {
            text_at(
                {left_x, footer_y, hint_w, metrics.control_height},
                footer_hint,
                kit::FontRole::small,
                kit::colour::hint,
                kit::Align::left
            );
        } else {
            for (std::size_t rest = most; rest < lines.size(); ++rest)
                lines[most - 1] += " " + lines[rest];
            if (lines.size() > most)
                lines.resize(most);
            const int32_t block = static_cast<int32_t>(lines.size()) * metrics.small_line;
            int32_t line_y = footer_y + (metrics.control_height - block) / 2;
            for (std::string& line : lines) {
                text_at(
                    {left_x, line_y, hint_w, metrics.small_line},
                    std::move(line),
                    kit::FontRole::small,
                    kit::colour::hint,
                    kit::Align::left
                );
                line_y += metrics.small_line;
            }
        }
    }
    y = footer_y + metrics.control_height;

    int32_t box_h = y + pad;
    if (box_h > window.height)
        box_h = window.height;
    if (box_h < 1)
        box_h = 1;
    kit::Item edge;
    edge.role = kit::Role::bevel;
    edge.rect = {0, 0, box_w, box_h};
    edge.colour = kit::colour::edge_light;
    kit::Item face;
    face.role = kit::Role::fill;
    face.rect = {0, 0, box_w, box_h};
    face.colour = kit::colour::panel;
    list.items.insert(list.items.begin(), std::move(edge));
    list.items.insert(list.items.begin(), std::move(face));
    const int32_t origin_x = std::max((window.width - box_w) / 2, int32_t{0});
    const int32_t origin_y = std::max((window.height - box_h) / 2, int32_t{0});
    for (kit::Item& item : list.items) {
        item.rect.x += origin_x;
        item.rect.y += origin_y;
    }
    for (kit::Control& control : list.controls) {
        control.rect.x += origin_x;
        control.rect.y += origin_y;
    }
    return list;
}

/// How wide the tabs are at one side padding.
///
/// @param fonts the fonts
/// @param look the captions and the padding
/// @return the width, in points
int32_t tabs_width(const kit::Fonts& fonts, const kit::TabsLook& look) {
    const std::vector<kit::Rect> rects = kit::tab_rects(fonts, {0, 0, 100000, 18}, look);
    if (rects.empty())
        return 0;
    return rects.back().x + rects.back().width;
}

/// Appends List, Keyboard and Mouse on Kit 2's tab strip. Only the open tab
/// takes Tab.
///
/// @param[in,out] built the layout
/// @param editor the words
/// @param state which tab is open
/// @param fonts the fonts
/// @param metrics the size class
/// @param x the strip's left
/// @param y the strip's top
/// @param room the most the strip may be wide
/// @return the width the strip took
int32_t add_view_tabs(
    Built& built,
    const Editor& editor,
    const ScreenState& state,
    const kit::Fonts& fonts,
    const kit::Metrics& metrics,
    int32_t x,
    int32_t y,
    int32_t room
) {
    if (room <= 0)
        return 0;
    const std::string captions[] = {
        show(editor.text(), words::view_list),
        show(editor.text(), words::view_keyboard),
        show(editor.text(), words::view_mouse),
    };
    const kit::ControlId ids[] = {
        controls::view_list, controls::view_keyboard, controls::view_mouse
    };
    const std::string names[] = {
        std::string(name_view_list),
        std::string(name_view_keyboard),
        std::string(name_view_mouse),
    };
    const std::size_t selected = state.view == View::keyboard ? 1
                                 : state.view == View::mouse  ? 2
                                                              : 0;
    kit::TabsLook look;
    look.captions.assign(std::begin(captions), std::end(captions));
    look.selected = selected;
    look.side_padding = metrics.tab_padding;
    int32_t width = tabs_width(fonts, look);
    while (width > room && look.side_padding > 2) {
        look.side_padding -= 2;
        width = tabs_width(fonts, look);
    }
    if (width > room)
        width = room;
    if (width <= 0)
        return 0;
    for (std::size_t index = 0; index < 3; ++index) {
        if (hot(state, ids[index]))
            look.hovered = index;
    }
    const kit::Rect strip{x, y, width, metrics.tab_strip_height};
    const std::size_t before = built.list.controls.size();
    const std::size_t rest = built.rest.size();
    kit::add_tabs(
        built.list,
        fonts,
        strip,
        look,
        std::span<const kit::ControlId>(ids, 3),
        std::span<const std::string>(names, 3)
    );
    steal(built.list, built.rest, before);
    one_stop(built, rest, ids[selected]);
    return width;
}

/// The word of one modifier layer.
///
/// @param editor the words
/// @param layer the layer's bit; 0 is Nothing
/// @return the word
std::string layer_name(const Editor& editor, uint8_t layer) {
    std::string_view word = words::holding_nothing;
    if (layer == keys::Modifier::shift)
        word = words::layer_shift;
    else if (layer == keys::Modifier::ctrl)
        word = words::layer_ctrl;
    else if (layer == keys::Modifier::alt)
        word = words::layer_alt;
    else if (layer == keys::Modifier::meta)
        word = words::layer_cmd;
    return show(editor.text(), word);
}

/// The keyboard shape's name: ANSI, ISO or JIS.
///
/// @param editor the words
/// @param state the shape
/// @return the name
std::string shape_name(const Editor& editor, const ScreenState& state) {
    std::string_view word = words::shape_ansi;
    if (state.shape == kit::KeyboardShape::iso)
        word = words::shape_iso;
    else if (state.shape == kit::KeyboardShape::jis)
        word = words::shape_jis;
    return show(editor.text(), word);
}

/// What the shape drop-down shows closed: the shape, and that the system
/// chose it.
///
/// @param editor the words
/// @param state the shape and whether the system chose it
/// @return the words
std::string shape_caption(const Editor& editor, const ScreenState& state) {
    const std::string name = shape_name(editor, state);
    if (!state.shape_from_system)
        return name;
    return fill(show(editor.text(), words::shape_from_system), {{"shape", name}});
}

/// Appends Holding, the layer pills and the shape drop-down: the pills on
/// Kit 2's filter chip, each wider by the height it gains, and the drop-down
/// a Kit 2 text field.
///
/// @param[in,out] built the layout
/// @param editor the words and the platform
/// @param state the layer
/// @param fonts the fonts
/// @param metrics the size class
/// @param left the row's left
/// @param right the column after the row
/// @param top the row's top
/// @param[out] shape_field the drop-down, for its menu
/// @return the row under this bar
int32_t add_layer_bar(
    Built& built,
    const Editor& editor,
    const ScreenState& state,
    const kit::Fonts& fonts,
    const kit::Metrics& metrics,
    int32_t left,
    int32_t right,
    int32_t top,
    kit::Rect& shape_field
) {
    const int32_t room = std::max(right - left, int32_t{0});
    const int32_t field_h = metrics.text_field_height;
    const int32_t pill_h = metrics.filter_chip_height;
    const int32_t taller = std::max(pill_h - kit::pill_height(), int32_t{0});
    const int32_t row_h = std::max(field_h, pill_h);
    if (room <= 0)
        return top + row_h;
    const bool command = editor.platform().primary_is_meta;
    const uint8_t bits[] = {
        0,
        keys::Modifier::shift,
        keys::Modifier::ctrl,
        keys::Modifier::alt,
        keys::Modifier::meta,
    };
    const kit::ControlId ids[] = {
        controls::layer_none,
        controls::layer_shift,
        controls::layer_ctrl,
        controls::layer_alt,
        controls::layer_cmd,
    };
    const std::string_view names[] = {
        name_layer_none, name_layer_shift, name_layer_ctrl, name_layer_alt, name_layer_cmd
    };
    const int count = command ? 5 : 4;
    const kit::ControlId chosen = [&] {
        for (int index = 0; index < count; ++index) {
            if (bits[index] == state.layer)
                return ids[index];
        }
        return controls::layer_none;
    }();
    kit::PillLook looks[5];
    int32_t widths[5] = {};
    int32_t pills = 0;
    for (int index = 0; index < count; ++index) {
        looks[index].text = layer_name(editor, bits[index]);
        looks[index].state =
            ids[index] == chosen ? kit::PillState::active : kit::PillState::inactive;
        looks[index].hovered = hot(state, ids[index]);
        looks[index].focused = focused_on(state, ids[index]);
        widths[index] = kit::pill_width(fonts, looks[index]) + taller;
        pills += widths[index];
    }
    pills += metrics.chip_gap * (count - 1);
    const std::string holding = show(editor.text(), words::holding);
    int32_t holding_w = kit::text_width(fonts, kit::FontRole::small, holding);
    // The drop-down is as wide as its words where a third of the row holds
    // them, and at least 168 points where the row allows.
    std::string shape_words = shape_caption(editor, state);
    const int32_t shape_natural = kit::text_width(fonts, kit::FontRole::regular, shape_words) +
                                  metrics.choice_text_inset + metrics.choice_arrow_room;
    int32_t shape_w =
        std::min(std::max(int32_t{168}, shape_natural), std::max(room / 3, int32_t{0}));
    const int32_t gap = metrics.button_gap;
    if (holding_w + gap + pills + gap + shape_w > room)
        holding_w = 0;
    if (pills + gap + shape_w > room)
        shape_w = std::max(room - gap - pills, int32_t{72});
    if (shape_w > room)
        shape_w = room;
    int32_t pills_room = room - shape_w - (shape_w > 0 ? gap : 0);
    if (holding_w > 0)
        pills_room -= holding_w + gap;
    if (pills > pills_room && pills_room > 0) {
        const int32_t each =
            std::max((pills_room - metrics.chip_gap * (count - 1)) / count, int32_t{8});
        pills = 0;
        for (int index = 0; index < count; ++index) {
            if (widths[index] > each) {
                looks[index].text = kit::cut_to_width(
                    fonts,
                    kit::FontRole::small,
                    looks[index].text,
                    std::max(each - taller - 8, int32_t{1})
                );
                widths[index] = std::min(kit::pill_width(fonts, looks[index]) + taller, each);
            }
            pills += widths[index];
        }
        pills += metrics.chip_gap * (count - 1);
    }
    int32_t x = left;
    const int32_t text_y = top + std::max((row_h - field_h) / 2, int32_t{0});
    if (holding_w > 0) {
        add_line(
            built,
            fonts,
            {x, text_y, holding_w, field_h},
            holding,
            kit::FontRole::small,
            kit::colour::quiet
        );
        x += holding_w + gap;
    }
    const int32_t pill_y = top + std::max((row_h - pill_h) / 2, int32_t{0});
    const int32_t pills_right = left + room - shape_w - (shape_w > 0 ? gap : 0);
    const std::size_t rest = built.rest.size();
    for (int index = 0; index < count; ++index) {
        if (x + widths[index] > pills_right)
            break;
        const kit::Rect rect{x, pill_y, widths[index], pill_h};
        if (!roomy(rect))
            break;
        const std::size_t before = built.list.controls.size();
        kit::add_pill(built.list, rect, looks[index], ids[index], std::string(names[index]));
        steal(built.list, built.rest, before);
        x += widths[index] + metrics.chip_gap;
    }
    one_stop(built, rest, chosen);
    if (shape_w > 0) {
        shape_field = {left + room - shape_w, text_y, shape_w, field_h};
        // Where "· from your system" does not fit, the shape's name alone;
        // the open menu still says the system chose it.
        if (shape_natural > shape_w && state.shape_from_system)
            shape_words = shape_name(editor, state);
        place_choice(
            built,
            state,
            shape_field,
            choice_text(fonts, metrics, shape_w, std::move(shape_words)),
            controls::keyboard_shape,
            std::string(name_keyboard_shape),
            state.menu == Menu::shape
        );
    }
    return top + row_h;
}

/// Appends "Clicks give orders with" and the two scheme pills, on Kit 2's
/// filter chip.
///
/// @param[in,out] built the layout
/// @param editor the scheme
/// @param state the pointer
/// @param fonts the fonts
/// @param metrics the size class
/// @param left the row's left
/// @param right the column after the row
/// @param top the row's top
/// @return the row under this bar
int32_t add_scheme_bar(
    Built& built,
    const Editor& editor,
    const ScreenState& state,
    const kit::Fonts& fonts,
    const kit::Metrics& metrics,
    int32_t left,
    int32_t right,
    int32_t top
) {
    const int32_t room = std::max(right - left, int32_t{0});
    const int32_t field_h = metrics.text_field_height;
    const int32_t pill_h = metrics.filter_chip_height;
    const int32_t taller = std::max(pill_h - kit::pill_height(), int32_t{0});
    const int32_t row_h = std::max(field_h, pill_h);
    if (room <= 0)
        return top + row_h;
    const bool left_click = editor.scheme() == keys::PointerScheme::left_click;
    const kit::ControlId ids[] = {controls::scheme_right, controls::scheme_left};
    const std::string_view names[] = {name_scheme_right, name_scheme_left};
    const std::string_view words_of[] = {words::scheme_right_click, words::scheme_left_click};
    const kit::ControlId chosen = left_click ? controls::scheme_left : controls::scheme_right;
    kit::PillLook looks[2];
    int32_t widths[2] = {};
    int32_t pills = 0;
    for (int index = 0; index < 2; ++index) {
        looks[index].text = show(editor.text(), words_of[index]);
        looks[index].state =
            ids[index] == chosen ? kit::PillState::active : kit::PillState::inactive;
        looks[index].hovered = hot(state, ids[index]);
        looks[index].focused = focused_on(state, ids[index]);
        widths[index] = kit::pill_width(fonts, looks[index]) + taller;
        pills += widths[index];
    }
    pills += metrics.chip_gap;
    const std::string label = show(editor.text(), words::clicks_give_orders);
    int32_t label_w = kit::text_width(fonts, kit::FontRole::small, label);
    const int32_t gap = metrics.button_gap;
    if (label_w + gap + pills > room)
        label_w = std::max(room - gap - pills, int32_t{0});
    if (label_w > 0 && label_w + gap + pills > room)
        label_w = 0;
    int32_t x = left;
    const int32_t text_y = top + std::max((row_h - field_h) / 2, int32_t{0});
    if (label_w > 0) {
        const std::string shown =
            kit::text_width(fonts, kit::FontRole::small, label) > label_w
                ? kit::cut_to_width(fonts, kit::FontRole::small, label, label_w)
                : label;
        add_line(
            built,
            fonts,
            {x, text_y, label_w, field_h},
            shown,
            kit::FontRole::small,
            kit::colour::quiet
        );
        x += label_w + gap;
    }
    const int32_t pill_y = top + std::max((row_h - pill_h) / 2, int32_t{0});
    const std::size_t rest = built.rest.size();
    for (int index = 0; index < 2; ++index) {
        if (x + widths[index] > right)
            break;
        const std::size_t before = built.list.controls.size();
        kit::add_pill(
            built.list,
            {x, pill_y, widths[index], pill_h},
            looks[index],
            ids[index],
            std::string(names[index])
        );
        steal(built.list, built.rest, before);
        x += widths[index] + metrics.chip_gap;
    }
    one_stop(built, rest, chosen);
    return top + row_h;
}

/// Drops a caption the key is too small to show, and cuts one that is too wide.
///
/// The lower half holds the caption and the category letter. Below two small
/// lines that half meets the printed label, so the key shows its tint and
/// letter alone. The card still names the action.
///
/// @param[in,out] look the picture
/// @param fonts the fonts
/// @param scale points per quarter unit, as add_keyboard will scale the picture
void fit_key_captions(kit::KeyboardLook& look, const kit::Fonts& fonts, int32_t scale) {
    if (scale < 1)
        scale = 1;
    int32_t inset = 0;
    if (scale >= 8)
        inset = 2;
    else if (scale >= 4)
        inset = 1;
    const std::span<const kit::KeyPlace> places = kit::keyboard_places(look.shape);
    for (kit::KeyboardKey& key : look.keys) {
        if (key.cap.sub.empty())
            continue;
        const kit::KeyPlace* place = nullptr;
        for (const kit::KeyPlace& candidate : places) {
            if (candidate.code == key.code) {
                place = &candidate;
                break;
            }
        }
        if (place == nullptr)
            continue;
        const int32_t raw_w = static_cast<int32_t>(place->w) * scale;
        const int32_t raw_h = static_cast<int32_t>(place->h) * scale;
        const int32_t inset_x = inset * 2 < raw_w ? inset : 0;
        const int32_t inset_y = inset * 2 < raw_h ? inset : 0;
        const int32_t width = raw_w - 2 * inset_x;
        const int32_t height = raw_h - 2 * inset_y;
        const bool letter = !key.cap.letter.empty() && width >= 12 && height >= 12;
        const int32_t letter_side = letter ? std::min(int32_t{8}, std::min(width, height) / 2) : 0;
        const int32_t tail = letter_side > 0 ? letter_side + 2 : 2;
        const int32_t room = std::max(width - tail - 2, int32_t{0});
        if (height < 24 || room < 8) {
            key.cap.sub.clear();
            continue;
        }
        if (kit::text_width(fonts, kit::FontRole::small, key.cap.sub) <= room)
            continue;
        std::string cut = kit::cut_to_width(fonts, kit::FontRole::small, key.cap.sub, room);
        if (kit::text_width(fonts, kit::FontRole::small, cut) > room)
            cut.clear();
        key.cap.sub = std::move(cut);
    }
}

/// Marks one key focused or hovered, adding it when the picture left it free.
///
/// @param[in,out] look the picture
/// @param code the W3C code
/// @param focused the focus ring
/// @param hovered the pointer is over it
void mark_key(kit::KeyboardLook& look, const std::string& code, bool focused, bool hovered) {
    if (code.empty() || (!focused && !hovered))
        return;
    for (kit::KeyboardKey& key : look.keys) {
        if (key.code != code)
            continue;
        key.cap.focused = key.cap.focused || focused;
        key.cap.hovered = key.cap.hovered || hovered;
        return;
    }
    kit::KeyboardKey key;
    key.code = code;
    key.cap.focused = focused;
    key.cap.hovered = hovered;
    look.keys.push_back(std::move(key));
}

/// Appends the keyboard view: the picture, or every key as a list, with Show
/// as list, the legend and the note under it, and the inspector.
///
/// Keycaps show their legends and their category's tint; what a key does is
/// the inspector's. At Regular and Large the inspector is a column at the
/// right. At Compact the picture may be wider than the well: it scrolls, and
/// the inspector sits under it. A press on a key only inspects it; the
/// inspector's buttons open the capture.
///
/// @param[in,out] built the layout
/// @param editor the bindings
/// @param state the shape, the layer, the key under the pointer and the list
/// @param fonts the fonts
/// @param metrics the size class
/// @param body the room under the bars
/// @param compact Compact may scroll a picture wider than the well
void add_keyboard_body(
    Built& built,
    const Editor& editor,
    const ScreenState& state,
    const kit::Fonts& fonts,
    const kit::Metrics& metrics,
    const kit::Rect& body,
    bool compact
) {
    if (!roomy(body))
        return;
    const int32_t gap = std::max(metrics.space * 2, int32_t{2});
    const int32_t line_h = metrics.small_line;
    KeyboardViewInputs inputs;
    inputs.shape = state.shape;
    inputs.shape_from_system = state.shape_from_system;
    inputs.layer = state.layer;
    const Labels labels = key_labels(editor.text());
    kit::KeyboardLook look = keyboard_look(editor, inputs, labels);
    // A key shows its legend and its category's tint; the action's name is
    // the inspector's, and the legend under the picture names the tints.
    for (kit::KeyboardKey& key : look.keys) {
        key.cap.sub.clear();
        key.cap.letter.clear();
    }
    std::string focus = state.focus_code;
    if (focus.empty()) {
        const std::span<const kit::KeyPlace> places = kit::keyboard_places(state.shape);
        if (!places.empty())
            focus = std::string(places.front().code);
    }
    const std::string shown = state.card_code.empty() ? focus : state.card_code;
    mark_key(look, focus, true, shown == focus && !state.card_code.empty());
    if (shown != focus)
        mark_key(look, shown, false, true);
    if (!state.outline.empty()) {
        for (const std::string& code : keys_of(editor, state.outline))
            mark_key(look, code, true, false);
    }
    auto label_of = [&](std::string_view code) {
        for (const kit::KeyboardKey& key : look.keys) {
            if (key.code == code && !key.cap.label.empty())
                return key.cap.label;
        }
        for (const kit::KeyPlace& place : kit::keyboard_places(state.shape)) {
            if (place.code == code)
                return std::string(place.label);
        }
        return std::string(code);
    };

    // The inspector's column, and the picture's.
    const bool beside = !compact && body.width >= 480;
    const int32_t inspector_w =
        beside ? std::clamp(body.width * 3 / 10, int32_t{200}, int32_t{260}) : body.width;
    const int32_t column_w =
        beside ? std::max(body.width - inspector_w - gap, int32_t{1}) : body.width;
    const kit::Measure measure = [&](std::string_view words_measured) {
        return kit::text_width(fonts, kit::FontRole::small, words_measured);
    };

    // The legend: each tint's square and letter, then its area.
    struct Swatch {
        kit::KeyTint tint{kit::KeyTint::none};
        std::string letter{};
        std::string title{};
        int32_t x{};
        int32_t row{};
        int32_t width{};
    };

    std::vector<Swatch> swatches;
    {
        const kit::KeyTint tints[] = {
            kit::KeyTint::orders,
            kit::KeyTint::selection,
            kit::KeyTint::groups,
            kit::KeyTint::camera,
            kit::KeyTint::building,
            kit::KeyTint::game,
            kit::KeyTint::chat,
            kit::KeyTint::hacks,
            kit::KeyTint::mod,
        };
        const std::string_view letters[] = {
            words::letter_orders,
            words::letter_selection,
            words::letter_groups,
            words::letter_camera,
            words::letter_building,
            words::letter_game,
            words::letter_chat,
            words::letter_hacks,
            words::letter_mod,
        };
        const std::string_view titles[] = {
            words::area_orders,
            words::area_selection,
            words::area_groups,
            words::area_camera,
            words::area_building,
            words::legend_game_window,
            words::area_chat,
            words::legend_hacks,
            words::legend_data,
        };
        int32_t x = 0;
        int32_t row = 0;
        for (std::size_t index = 0; index < std::size(tints); ++index) {
            Swatch one;
            one.tint = tints[index];
            one.letter = show(editor.text(), letters[index]);
            one.title = show(editor.text(), titles[index]);
            one.width = 12 + 4 + kit::text_width(fonts, kit::FontRole::small, one.title);
            if (x > 0 && x + one.width > column_w) {
                x = 0;
                ++row;
            }
            one.x = x;
            one.row = row;
            x += one.width + metrics.space * 3;
            swatches.push_back(std::move(one));
        }
    }
    const int32_t legend_lines = swatches.empty() ? 0 : swatches.back().row + 1;
    const std::vector<std::string> legend_rows(static_cast<std::size_t>(legend_lines));
    const std::vector<std::string> note_rows = kit::wrap(
        show(editor.text(), words::grey_keys_free), std::max(column_w, int32_t{1}), measure
    );
    const int32_t button_row = metrics.control_height;
    int32_t under = button_row;
    if (beside)
        under += gap + static_cast<int32_t>(legend_rows.size() + note_rows.size()) * line_h;
    // At Compact the inspector shares the column under the picture.
    const int32_t inspector_least = compact ? line_h * 2 : 0;
    int32_t room_h = body.height - under - gap - (beside ? 0 : inspector_least + gap);
    if (room_h < 25)
        room_h = std::max(body.height - button_row - gap, int32_t{25});
    const int32_t scale_h = std::max(room_h / 25, int32_t{1});
    const int32_t scale_w = std::max(column_w / 91, int32_t{1});
    int32_t scale = std::min(scale_h, scale_w);
    if (compact && scale_h >= 4)
        scale = std::min(scale_h, int32_t{5});
    if (scale < 1)
        scale = 1;
    fit_key_captions(look, fonts, scale);
    const int32_t pic_w = 91 * scale;
    const int32_t pic_h = 25 * scale;
    int32_t y = body.y;

    if (state.keys_as_list) {
        // Every key that does something in this layer, one line each:
        // its legend, where, and what.
        std::vector<std::string> rows;
        for (const kit::KeyPlace& place : kit::keyboard_places(state.shape)) {
            for (const CardLine& line : key_card(editor, place.code, state.layer)) {
                std::string row = label_of(place.code) + "   " + line.context + " · " + line.action;
                if (!line.note.empty())
                    row += " · " + line.note;
                rows.push_back(std::move(row));
            }
        }
        const int32_t list_h = std::max(pic_h, room_h);
        const kit::Rect port{body.x, y, column_w, list_h};
        add_item(built.list, kit::Role::fill, port).colour = kit::colour::list;
        const int32_t content_h = static_cast<int32_t>(rows.size()) * line_h + metrics.space * 2;
        const int32_t offset =
            std::clamp(state.key_list_scroll, int32_t{0}, std::max(content_h - list_h, int32_t{0}));
        int32_t row_y = y + metrics.space - offset;
        std::string reading;
        for (const std::string& row : rows) {
            if (!reading.empty())
                reading += '\n';
            reading += row;
            if (row_y >= y && row_y + line_h <= y + list_h)
                add_line(
                    built,
                    fonts,
                    {body.x + metrics.space * 2, row_y, column_w - metrics.space * 4, line_h},
                    row,
                    kit::FontRole::small,
                    kit::colour::text
                );
            row_y += line_h;
        }
        kit::Control control;
        control.id = controls::key_list;
        control.rect = port;
        control.enabled = true;
        control.focusable = true;
        control.name = std::string(name_key_list);
        control.kind = kit::ControlKind::list;
        control.text = std::move(reading);
        built.rest.push_back(std::move(control));
        built.tabs.push_back(controls::key_list);
        y += list_h + gap;
    } else {
        int32_t scroll = state.keyboard_scroll;
        if (scroll < 0)
            scroll = 0;
        const int32_t max_scroll = std::max(pic_w - column_w, int32_t{0});
        if (scroll > max_scroll)
            scroll = max_scroll;
        const kit::Rect picture{body.x - scroll, y, pic_w, pic_h};
        const kit::Rect well{body.x, y, column_w, pic_h};
        kit::ControlId focus_id = kit::no_control;
        const std::span<const kit::KeyPlace> places = kit::keyboard_places(state.shape);
        for (std::size_t index = 0; index < places.size(); ++index) {
            if (places[index].code == focus)
                focus_id = controls::keyboard_first + static_cast<kit::ControlId>(index);
        }
        const std::size_t item_at = built.list.items.size();
        const std::size_t before = built.list.controls.size();
        const std::size_t rest = built.rest.size();
        kit::add_keyboard(
            built.list, picture, look, controls::keyboard_first, std::string(name_keyboard)
        );
        if (item_at < built.list.items.size())
            built.list.items[item_at].clip = well;
        steal(built.list, built.rest, before);
        for (std::size_t index = rest; index < built.rest.size(); ++index) {
            built.rest[index].clip = well;
            built.rest[index].focusable = built.rest[index].id == focus_id;
        }
        if (focus_id != kit::no_control)
            built.tabs.push_back(focus_id);
        y += pic_h + gap;
    }

    // What the inspector shows, and its buttons.
    const std::string key_label = label_of(shown);
    // The key's name, with the layer's modifier held when there is one.
    std::string title;
    if (state.layer != 0)
        title = layer_name(editor, state.layer) + " + " + key_label;
    const std::vector<CardLine> lines = key_card(editor, shown, state.layer);
    std::string reading = title.empty() ? key_label : title;
    // The buttons sit at the inspector's foot.
    const std::string face = [&]() -> std::string {
        const std::string playing = show(editor.text(), words::card_while_playing);
        for (const CardLine& line : lines) {
            if (!line.fixed && line.context == playing && !line.id.empty())
                return line.id;
        }
        for (const CardLine& line : lines) {
            if (!line.fixed && !line.id.empty())
                return line.id;
        }
        return {};
    }();
    std::vector<PromptButton> buttons;
    if (!face.empty()) {
        const Details details = editor.details(face);
        PromptButton change;
        change.id = controls::card_change;
        change.name = std::string(name_card_change);
        change.caption = fill(
            show(editor.text(), words::change_action_key),
            {{"action", details.name.empty() ? face : details.name}}
        );
        change.enabled = !details.read_only;
        buttons.push_back(std::move(change));
    }
    if (!state.selected.empty() && state.selected != face) {
        const Details chosen = editor.details(state.selected);
        if (!chosen.read_only && !chosen.name.empty()) {
            PromptButton use;
            use.id = controls::card_use;
            use.name = std::string(name_card_use);
            use.caption = fill(
                show(editor.text(), words::use_key_for),
                {{"key", key_label}, {"action", chosen.name}}
            );
            use.quiet = true;
            buttons.push_back(std::move(use));
        }
    }
    // Show as list, then the legend and the note.
    {
        const std::string caption =
            show(editor.text(), state.keys_as_list ? words::show_keyboard : words::show_as_list);
        const int32_t width = std::min(button_width(fonts, metrics, caption), column_w);
        kit::ButtonLook button;
        button.caption = kit::cut_to_width(fonts, kit::FontRole::small, caption, width);
        button.style = kit::ButtonStyle::plain;
        button.hovered = hot(state, controls::show_as_list);
        button.held = held(state, controls::show_as_list);
        if (y + button_row <= body.y + body.height)
            place_button(
                built,
                {body.x, y, width, button_row},
                button,
                controls::show_as_list,
                std::string(name_show_as_list),
                true
            );
        // Compact keeps the inspector's buttons beside it, or a note when the
        // key has none.
        if (!beside) {
            int32_t right = body.x + column_w;
            const int32_t least = body.x + width + gap;
            for (auto one = buttons.rbegin(); one != buttons.rend(); ++one) {
                const int32_t button_w = button_width(fonts, metrics, one->caption);
                if (right - button_w < least)
                    break;
                kit::ButtonLook look_of;
                look_of.caption = one->caption;
                look_of.style = one->quiet ? kit::ButtonStyle::quiet : kit::ButtonStyle::plain;
                look_of.enabled = one->enabled;
                look_of.hovered = hot(state, one->id);
                look_of.held = held(state, one->id);
                place_button(
                    built,
                    {right - button_w, y, button_w, button_row},
                    look_of,
                    one->id,
                    one->name,
                    true
                );
                right -= button_w + gap;
            }
            if (buttons.empty() && column_w - width - gap > 24)
                add_line(
                    built,
                    fonts,
                    {least, y, column_w - width - gap, button_row},
                    show(editor.text(), words::select_a_key),
                    kit::FontRole::small,
                    kit::colour::quiet
                );
            buttons.clear();
        }
        y += button_row + gap;
    }
    if (beside) {
        const int32_t legend_top = y;
        for (const Swatch& one : swatches) {
            const int32_t row_y = legend_top + one.row * line_h;
            if (row_y + line_h > body.y + body.height)
                break;
            kit::KeycapLook square;
            square.label = one.letter;
            square.tint = one.tint;
            square.small = true;
            kit::add_keycap(
                built.list,
                {body.x + one.x, row_y + std::max((line_h - 12) / 2, int32_t{0}), 12, 12},
                square,
                kit::no_control,
                {}
            );
            add_line(
                built,
                fonts,
                {body.x + one.x + 16, row_y, one.width - 16, line_h},
                one.title,
                kit::FontRole::small,
                kit::colour::hint
            );
        }
        y = legend_top + static_cast<int32_t>(legend_rows.size()) * line_h + metrics.space;
        for (const std::string& wrapped : note_rows) {
            if (y + line_h > body.y + body.height)
                break;
            add_line(
                built,
                fonts,
                {body.x, y, column_w, line_h},
                wrapped,
                kit::FontRole::small,
                kit::colour::quiet
            );
            y += line_h;
        }
    }

    // The inspector: the key, everything it does in each place, and the
    // buttons that open the capture.
    const kit::Rect pane =
        beside ? kit::Rect{body.x + column_w + gap, body.y, inspector_w, body.height}
               : kit::Rect{body.x, y, body.width, body.y + body.height - y};
    if (!roomy(pane))
        return;
    add_item(built.list, kit::Role::fill, pane).colour = kit::colour::list;
    const int32_t pad = metrics.space * 2;
    const int32_t inner_w = std::max(pane.width - pad * 2, int32_t{1});
    const int32_t bottom = pane.y + pane.height - pad;
    int32_t at = pane.y + pad;
    const int32_t buttons_h =
        static_cast<int32_t>(buttons.size()) * (metrics.control_height + metrics.space);
    const int32_t lines_stop = bottom - buttons_h;
    const int32_t cap_side = std::min(metrics.control_height + 6, int32_t{44});
    if (beside && at + cap_side <= lines_stop) {
        kit::KeycapLook cap;
        cap.label = key_label;
        cap.modifier = key_label.size() > 1;
        kit::KeycapLook measured = cap;
        const int32_t cap_w =
            std::min(std::max(kit::keycap_width(fonts, measured), cap_side), inner_w / 2);
        kit::add_keycap(built.list, {pane.x + pad, at, cap_w, cap_side}, cap, kit::no_control, {});
        if (!title.empty())
            add_line(
                built,
                fonts,
                {pane.x + pad + cap_w + pad, at, inner_w - cap_w - pad, cap_side},
                title,
                kit::FontRole::regular,
                kit::colour::text
            );
        at += cap_side + pad;
    }
    if (at + line_h <= lines_stop) {
        add_line(
            built,
            fonts,
            {pane.x + pad, at, inner_w, line_h},
            fill(show(editor.text(), words::what_key_does), {{"key", key_label}}),
            kit::FontRole::small,
            kit::colour::quiet
        );
        at += line_h + metrics.space;
    }
    if (lines.empty() && at + line_h <= lines_stop) {
        add_line(
            built,
            fonts,
            {pane.x + pad, at, inner_w, line_h},
            show(editor.text(), words::does_nothing),
            kit::FontRole::small,
            kit::colour::quiet
        );
        at += line_h;
    }
    for (const CardLine& line : lines) {
        std::string row = line.context;
        if (!line.action.empty())
            row += " · " + line.action;
        if (!line.note.empty())
            row += " · " + line.note;
        reading += '\n';
        reading += row;
        if (!beside) {
            // Compact's inspector is one line a place.
            if (at + line_h <= lines_stop)
                add_line(
                    built,
                    fonts,
                    {pane.x + pad, at, inner_w, line_h},
                    row,
                    kit::FontRole::small,
                    line.fixed ? kit::colour::quiet : kit::colour::text
                );
            at += line_h;
            continue;
        }
        // The place and what the key does there each wrap onto a second line
        // rather than being cut.
        const int32_t text_w = std::max(inner_w - pad * 2, int32_t{1});
        const kit::Measure small_measure = [&](std::string_view words_measured) {
            return kit::text_width(fonts, kit::FontRole::small, words_measured);
        };
        const auto two_lines = [&](const std::string& words) {
            if (small_measure(words) <= text_w)
                return std::vector<std::string>{words};
            std::vector<std::string> broken = kit::wrap(words, text_w, small_measure);
            if (broken.empty())
                broken.emplace_back();
            for (std::size_t rest = 2; rest < broken.size(); ++rest)
                broken[1] += " " + broken[rest];
            if (broken.size() > 2)
                broken.resize(2);
            return broken;
        };
        std::string what = line.action;
        if (!line.note.empty())
            what += "  " + line.note;
        const std::vector<std::string> where_lines = two_lines(line.context);
        const std::vector<std::string> what_lines = two_lines(what);
        const int32_t box_h =
            line_h * static_cast<int32_t>(where_lines.size() + what_lines.size()) +
            metrics.space * 2;
        if (at + box_h > lines_stop)
            continue;
        add_item(built.list, kit::Role::fill, {pane.x + pad, at, inner_w, box_h}).colour =
            kit::colour::panel;
        int32_t text_y = at + metrics.space;
        for (const std::string& piece : where_lines) {
            add_line(
                built,
                fonts,
                {pane.x + pad * 2, text_y, text_w, line_h},
                piece,
                kit::FontRole::small,
                kit::colour::text_tertiary
            );
            text_y += line_h;
        }
        for (const std::string& piece : what_lines) {
            add_line(
                built,
                fonts,
                {pane.x + pad * 2, text_y, text_w, line_h},
                piece,
                kit::FontRole::small,
                line.fixed ? kit::colour::quiet : kit::colour::text
            );
            text_y += line_h;
        }
        at += box_h + metrics.space;
    }
    kit::Control control;
    control.id = controls::key_card;
    control.rect = {pane.x, pane.y, pane.width, std::clamp(at - pane.y, int32_t{1}, pane.height)};
    control.enabled = true;
    control.focusable = false;
    control.name = std::string(name_key_card);
    control.kind = kit::ControlKind::area;
    control.text = std::move(reading);
    built.rest.push_back(std::move(control));
    int32_t button_y = bottom - buttons_h + metrics.space;
    for (const PromptButton& one : buttons) {
        if (button_y < at || button_y + metrics.control_height > bottom)
            break;
        kit::ButtonLook button;
        button.caption = kit::cut_to_width(fonts, kit::FontRole::small, one.caption, inner_w);
        button.style = one.quiet ? kit::ButtonStyle::quiet : kit::ButtonStyle::plain;
        button.enabled = one.enabled;
        button.hovered = hot(state, one.id);
        button.held = held(state, one.id);
        place_button(
            built,
            {pane.x + pad, button_y, inner_w, metrics.control_height},
            button,
            one.id,
            one.name,
            true
        );
        button_y += metrics.control_height + metrics.space;
    }
}

/// Which look a mouse control hovers. The wheel is one control and two looks.
///
/// @param index the control's place, from 0
/// @return the look's place, or -1
int mouse_look_index(int index) noexcept {
    if (index < 3)
        return index;
    if (index == 3)
        return 3;
    return index + 1;
}

/// The words for a mouse role's chord: its modifiers and its button, or the
/// button alone on a row with no chord.
///
/// @param editor the keyboard and the platform
/// @param row the role
/// @return the words
std::string mouse_row_chord(const Editor& editor, const RoleLine& row) {
    if (!row.has_chord)
        return row.button;
    return keys::display_text(row.chord, editor.keyboard(), editor.platform());
}

/// The roles a button's callout shows: every role, or its one name.
///
/// @param cap the button
/// @return the words
std::string callout_roles(const kit::KeycapLook& cap) {
    if (!cap.sub.empty())
        return cap.sub;
    return cap.label;
}

/// Lays a callout's roles, " · " between them, on at most `most` lines of a
/// width, each role whole: as many roles as fit, then "+n" for the rest. A
/// role wider than the width takes lines of its own, broken between words.
///
/// @param editor the words
/// @param roles the roles, " · " between them
/// @param width the callout's width, in points
/// @param measure a text's width
/// @param most the most lines
/// @return the lines; empty when not even the first role fits whole
std::vector<std::string> fit_roles(
    const Editor& editor,
    std::string_view roles,
    int32_t width,
    const kit::Measure& measure,
    int most
) {
    constexpr std::string_view separator = " · ";
    std::vector<std::string> each;
    for (std::size_t from = 0; from <= roles.size();) {
        const std::size_t at = roles.find(separator, from);
        const std::size_t to = at == std::string_view::npos ? roles.size() : at;
        if (to > from)
            each.emplace_back(roles.substr(from, to - from));
        if (at == std::string_view::npos)
            break;
        from = at + separator.size();
    }
    // Packs the first `shown` roles, and the count of the others, into lines.
    // Returns more than `most` lines when they do not fit whole.
    const auto pack = [&](std::size_t shown) {
        std::vector<std::string> items(
            each.begin(), each.begin() + static_cast<std::ptrdiff_t>(shown)
        );
        const std::size_t rest = each.size() - shown;
        std::vector<std::string> lines;
        std::string line;
        for (std::size_t index = 0; index <= items.size(); ++index) {
            const bool count = index == items.size();
            if (count && rest == 0)
                break;
            const std::string item =
                count ? fill(show(editor.text(), words::more_chords), {{"n", std::to_string(rest)}})
                      : items[index];
            const std::string joined =
                line.empty() ? item : line + (count ? " " : std::string(separator)) + item;
            if (measure(joined) <= width) {
                line = joined;
                continue;
            }
            if (!line.empty())
                lines.push_back(std::move(line));
            line.clear();
            if (measure(item) <= width) {
                line = item;
                continue;
            }
            // A role wider than a line breaks between its words, never in one.
            const std::vector<std::string> broken = kit::wrap(item, width, measure);
            std::string rejoined;
            for (const std::string& part : broken)
                rejoined += (rejoined.empty() ? "" : " ") + part;
            if (broken.empty() || rejoined != item)
                return std::vector<std::string>(static_cast<std::size_t>(most) + 1);
            for (std::size_t part = 0; part + 1 < broken.size(); ++part)
                lines.push_back(broken[part]);
            line = broken.back();
        }
        if (!line.empty())
            lines.push_back(std::move(line));
        return lines;
    };
    for (std::size_t shown = each.size(); shown >= 1; --shown) {
        std::vector<std::string> lines = pack(shown);
        if (static_cast<int>(lines.size()) <= most)
            return lines;
    }
    return {};
}

/// What a mouse control reads as to automation: the roles of its button, as
/// a key reads as its legend. The wheel is both its turns' roles.
///
/// @param look the mouse, with each button's roles
/// @param word the control's last name part: mouse-left, wheel, mouse-6, ...
/// @return the roles, " · " between them
std::string mouse_control_text(const kit::MouseLook& look, std::string_view word) {
    const auto roles = [&](kit::MouseButton which) {
        return callout_roles(look.buttons[static_cast<std::size_t>(which)]);
    };
    if (word == "wheel") {
        std::string turns = roles(kit::MouseButton::wheel_up);
        const std::string down = roles(kit::MouseButton::wheel_down);
        if (!down.empty() && down != turns)
            turns += turns.empty() ? down : " · " + down;
        return turns;
    }
    const std::pair<std::string_view, kit::MouseButton> buttons[] = {
        {"mouse-left", kit::MouseButton::left},
        {"mouse-right", kit::MouseButton::right},
        {"mouse-middle", kit::MouseButton::middle},
        {"mouse-back", kit::MouseButton::back},
        {"mouse-forward", kit::MouseButton::forward},
        {"mouse-6", kit::MouseButton::button6},
        {"mouse-7", kit::MouseButton::button7},
        {"mouse-8", kit::MouseButton::button8},
    };
    for (const auto& [name, which] : buttons) {
        if (word == name)
            return roles(which);
    }
    return {};
}

/// Gives the mouse's controls, from `from` in `built.rest`, their buttons'
/// roles as their text, and keeps them out of Tab's order.
///
/// @param[in,out] built the layout
/// @param from the first of the mouse's controls
/// @param look the mouse, with each button's roles
void name_mouse_roles(Built& built, std::size_t from, const kit::MouseLook& look) {
    for (std::size_t index = from; index < built.rest.size(); ++index) {
        kit::Control& control = built.rest[index];
        control.focusable = false;
        const std::size_t dot = control.name.rfind('.');
        const std::string_view word = dot == std::string::npos
                                          ? std::string_view(control.name)
                                          : std::string_view(control.name).substr(dot + 1);
        control.text = mouse_control_text(look, word);
    }
}

/// The colour of a callout whose binding was changed or clashes.
///
/// @param cap the button
/// @return accent, danger, or the hint colour
kit::Colour callout_ink(const kit::KeycapLook& cap) noexcept {
    if (cap.mark == kit::KeyMark::clash)
        return kit::colour::danger;
    if (cap.mark == kit::KeyMark::changed)
        return kit::colour::accent;
    return kit::colour::hint;
}

/// Appends the mouse, the table of roles and the note under them.
///
/// At Regular and Large the mouse is large on the left, with a callout by
/// each button, and the table on the right is a chord and its role grouped
/// by place. At Compact the roles stay in a list. A button too small for a
/// name shows its tint and letter: the callout or the list names the role.
///
/// @param[in,out] built the layout
/// @param editor the scheme
/// @param state whether buttons 6 to 8 show
/// @param fonts the fonts
/// @param metrics the size class
/// @param body the room under the bars
/// @param compact Compact keeps the role list
void add_mouse_body(
    Built& built,
    const Editor& editor,
    const ScreenState& state,
    const kit::Fonts& fonts,
    const kit::Metrics& metrics,
    const kit::Rect& body,
    bool compact
) {
    if (!roomy(body))
        return;
    const int32_t gap = std::max(metrics.paragraph_gap / 2, int32_t{2});
    // The note under the mouse wraps onto a second line rather than being cut.
    const kit::Measure note_measure = [&](std::string_view words_measured) {
        return kit::text_width(fonts, kit::FontRole::small, words_measured);
    };
    std::vector<std::string> note_lines = kit::wrap(
        show(editor.text(), words::mouse_note), std::max(body.width, int32_t{1}), note_measure
    );
    for (std::size_t rest = 2; rest < note_lines.size(); ++rest)
        note_lines[1] += " " + note_lines[rest];
    if (note_lines.size() > 2)
        note_lines.resize(2);
    const int32_t note_h = metrics.small_line * static_cast<int32_t>(note_lines.size());
    const int32_t note_y = body.y + body.height - note_h;
    const int32_t bottom = note_y - gap;
    const int32_t avail_h = std::max(bottom - body.y, int32_t{0});
    const int32_t line_h = metrics.small_line;
    kit::MouseLook look = mouse_look(editor, editor.scheme());
    look.extra_buttons = state.extra_buttons;
    const int buttons = state.extra_buttons ? 9 : 6;
    for (int index = 0; index < buttons; ++index) {
        const kit::ControlId id = controls::mouse_first + static_cast<kit::ControlId>(index);
        if (!hot(state, id))
            continue;
        const int look_at = mouse_look_index(index);
        if (look_at >= 0 && look_at < 10)
            look.buttons[static_cast<std::size_t>(look_at)].hovered = true;
        if (index == 3)
            look.buttons[static_cast<std::size_t>(kit::MouseButton::wheel_down)].hovered = true;
    }
    const auto face = [&](kit::MouseButton which) -> const kit::KeycapLook& {
        return look.buttons[static_cast<std::size_t>(which)];
    };
    const std::string left_roles = callout_roles(face(kit::MouseButton::left));
    const std::string right_roles = callout_roles(face(kit::MouseButton::right));
    const std::string back_roles = callout_roles(face(kit::MouseButton::back));
    const std::string forward_roles = callout_roles(face(kit::MouseButton::forward));
    const kit::Colour left_ink = callout_ink(face(kit::MouseButton::left));
    const kit::Colour right_ink = callout_ink(face(kit::MouseButton::right));
    const kit::Colour back_ink = callout_ink(face(kit::MouseButton::back));
    const kit::Colour forward_ink = callout_ink(face(kit::MouseButton::forward));
    std::string wheel_roles = callout_roles(face(kit::MouseButton::wheel_up));
    const std::string wheel_down_roles = callout_roles(face(kit::MouseButton::wheel_down));
    const std::string middle_roles = callout_roles(face(kit::MouseButton::middle));
    if (!wheel_down_roles.empty() && wheel_down_roles != wheel_roles) {
        if (!wheel_roles.empty())
            wheel_roles += " · ";
        wheel_roles += wheel_down_roles;
    }
    if (!middle_roles.empty() && middle_roles != wheel_roles) {
        if (!wheel_roles.empty())
            wheel_roles += " · ";
        wheel_roles += middle_roles;
    }
    kit::Colour wheel_ink = kit::colour::hint;
    for (const kit::MouseButton which :
         {kit::MouseButton::wheel_up, kit::MouseButton::wheel_down, kit::MouseButton::middle}) {
        const kit::Colour ink = callout_ink(face(which));
        if (ink == kit::colour::danger)
            wheel_ink = ink;
        else if (wheel_ink != kit::colour::danger && ink == kit::colour::accent)
            wheel_ink = ink;
    }
    // The callouts and the list name the roles; the picture shows the tints.
    // The controls keep the roles for automation.
    const kit::MouseLook named = look;
    for (kit::KeycapLook& cap : look.buttons) {
        cap.label.clear();
        cap.sub.clear();
    }
    const bool wide = !compact && body.width >= 560 && avail_h >= 64;
    if (wide) {
        const int32_t half = body.width / 2;
        const int32_t least_callout = 72;
        // The callouts stop short of the table, so a role and a row do not meet.
        const int32_t gutter = 12;
        const int32_t column = std::max(half - gutter, least_callout);
        // The wheel callout is its name and up to two lines of roles.
        const int32_t wheel_lines = 3;
        const int32_t wheel_band = line_h * wheel_lines;
        int32_t room_h = avail_h;
        int32_t wheel_top = 0;
        if (avail_h >= 64 + wheel_band + gap) {
            room_h = avail_h - wheel_band - gap;
            wheel_top = wheel_band + gap;
        }
        // The mouse takes as much of the left half as a callout on each side
        // still leaves. A callout narrower than a short name is no use.
        const int32_t by_height = std::max(room_h / 64, int32_t{1});
        const int32_t by_width = std::max((column - 2 * least_callout - 2 * gap) / 40, int32_t{1});
        int32_t unit = std::min(by_height, by_width);
        if (unit < 1)
            unit = 1;
        int32_t pic_w = 40 * unit;
        int32_t callout_w = std::max((column - pic_w - 2 * gap) / 2, int32_t{1});
        while (unit > 1 && callout_w * 2 + pic_w + 2 * gap > column) {
            --unit;
            pic_w = 40 * unit;
            callout_w = std::max((column - pic_w - 2 * gap) / 2, int32_t{1});
        }
        const int32_t pic_h = 64 * unit;
        const int32_t pic_x = body.x + callout_w + gap;
        int32_t pic_y = body.y + wheel_top;
        if (room_h > pic_h)
            pic_y += (room_h - pic_h) / 2;
        const kit::Rect picture{pic_x, pic_y, pic_w, pic_h};
        const std::size_t before = built.list.controls.size();
        kit::add_mouse(built.list, picture, look, controls::mouse_first, std::string(name_mouse));
        const std::size_t rest = built.rest.size();
        steal(built.list, built.rest, before);
        name_mouse_roles(built, rest, named);
        const kit::Measure measure_roles = [&](std::string_view words) {
            return kit::text_width(fonts, kit::FontRole::small, words);
        };
        // A callout is the button's name and its roles, on two lines when
        // they do not fit one. `anchor` is the top, or the centre when
        // `centred` is set. The block starts at `floor` when centring would
        // land on the callout above it. Returns the block's bottom.
        auto add_callout = [&](int32_t x,
                               int32_t anchor,
                               int32_t width,
                               kit::Align align,
                               const std::string& title,
                               const std::string& roles,
                               kit::Colour ink,
                               bool centred,
                               int32_t floor) -> int32_t {
            if (width <= 0)
                return floor;
            constexpr int role_lines = 2;
            std::vector<std::string> lines;
            if (!roles.empty()) {
                // Whole roles, and a count of the rest where they do not all
                // fit; a role is cut only where not even one fits whole.
                lines = fit_roles(editor, roles, width, measure_roles, role_lines);
                if (lines.empty()) {
                    kit::WrapRules rules;
                    rules.most_lines = static_cast<std::size_t>(role_lines);
                    rules.shorten_last = [&](std::string_view rest) {
                        return kit::cut_to_width(fonts, kit::FontRole::small, rest, width);
                    };
                    lines = kit::wrap(roles, width, measure_roles, rules);
                }
            }
            const int32_t count = 1 + static_cast<int32_t>(lines.size());
            int32_t top = centred ? anchor - (count * line_h) / 2 : anchor;
            if (top < floor)
                top = floor;
            if (top < body.y)
                top = body.y;
            if (top + line_h > bottom)
                return floor;
            const kit::Colour title_ink = ink == kit::colour::hint ? kit::colour::text : ink;
            add_line(
                built, fonts, {x, top, width, line_h}, title, kit::FontRole::small, title_ink, align
            );
            for (std::size_t line_at = 0; line_at < lines.size(); ++line_at) {
                const int32_t line_y = top + line_h * static_cast<int32_t>(line_at + 1);
                if (line_y + line_h > bottom)
                    break;
                add_line(
                    built,
                    fonts,
                    {x, line_y, width, line_h},
                    lines[line_at],
                    kit::FontRole::small,
                    ink,
                    align
                );
            }
            return top + count * line_h;
        };
        int32_t left_floor = body.y + wheel_top;
        int32_t right_floor = body.y + wheel_top;
        auto beside_button = [&](int32_t design_y,
                                 int32_t design_h,
                                 int32_t x,
                                 int32_t width,
                                 kit::Align align,
                                 const std::string& title,
                                 const std::string& roles,
                                 kit::Colour ink,
                                 bool on_left) {
            int32_t& floor = on_left ? left_floor : right_floor;
            const int32_t centre = pic_y + (design_y + design_h / 2) * unit;
            floor = add_callout(x, centre, width, align, title, roles, ink, true, floor);
        };
        const std::string left_name = show(editor.text(), words::button_left);
        const std::string right_name = show(editor.text(), words::button_right);
        const std::string back_name = show(editor.text(), words::card_back);
        const std::string forward_name = show(editor.text(), words::button_forward);
        const std::string wheel_name = show(editor.text(), words::cap_wheel_middle);
        const int32_t right_x = pic_x + pic_w + gap;
        beside_button(
            4, 16, body.x, callout_w, kit::Align::right, left_name, left_roles, left_ink, true
        );
        beside_button(
            18, 10, body.x, callout_w, kit::Align::right, back_name, back_roles, back_ink, true
        );
        beside_button(
            30,
            10,
            body.x,
            callout_w,
            kit::Align::right,
            forward_name,
            forward_roles,
            forward_ink,
            true
        );
        beside_button(
            4, 16, right_x, callout_w, kit::Align::left, right_name, right_roles, right_ink, false
        );
        if (wheel_top > 0) {
            int32_t wheel_w = std::max(pic_w, int32_t{96});
            if (wheel_w > column - gap)
                wheel_w = std::max(column - gap, int32_t{1});
            int32_t wheel_x = pic_x + (pic_w - wheel_w) / 2;
            if (wheel_x < body.x)
                wheel_x = body.x;
            add_callout(
                wheel_x,
                body.y,
                wheel_w,
                kit::Align::centre,
                wheel_name,
                wheel_roles,
                wheel_ink,
                false,
                body.y
            );
        }
        const int32_t table_x = body.x + half;
        const int32_t table_w = std::max(body.x + body.width - table_x, int32_t{0});
        const int32_t row_h = line_h;
        int32_t y = body.y;
        const std::vector<RoleLine> rows = mouse_table(editor, editor.scheme());
        std::string place;
        int place_number = 0;
        int row_index = 0;
        for (const RoleLine& row : rows) {
            if (row.place_id != place) {
                place = row.place_id;
                place_number = 0;
                if (y + line_h > bottom)
                    break;
                add_line(
                    built,
                    fonts,
                    {table_x, y, table_w, line_h},
                    row.place,
                    kit::FontRole::small,
                    kit::colour::quiet
                );
                y += line_h;
            }
            if (y + row_h > bottom)
                break;
            ++place_number;
            const kit::ControlId id =
                controls::mouse_table_first + static_cast<kit::ControlId>(row_index);
            const std::string name = std::string(name_mouse_table_prefix) + row.place_id + "." +
                                     std::to_string(place_number);
            int32_t text_x = table_x;
            int32_t text_w = table_w;
            if (row.has_chord && table_w > kit::keycap_least_width) {
                const kit::ChordChipLook chip = chip_of(editor, row.chord, row.mark);
                int32_t chip_w = kit::chord_chip_width(fonts, chip);
                const int32_t most = std::max(table_w / 2, kit::keycap_least_width);
                if (chip_w > most)
                    chip_w = most;
                const int32_t chip_h = std::min(kit::keycap_height, row_h);
                const kit::Rect chip_rect{
                    table_x, y + std::max((row_h - chip_h) / 2, int32_t{0}), chip_w, chip_h
                };
                if (roomy(chip_rect))
                    kit::add_chord_chip(built.list, chip_rect, chip, kit::no_control, {});
                text_x = table_x + chip_w + gap;
                text_w = std::max(table_x + table_w - text_x, int32_t{0});
            }
            kit::Colour role_ink = kit::colour::hint;
            if (row.mark == kit::KeyMark::clash)
                role_ink = kit::colour::danger;
            else if (row.mark == kit::KeyMark::changed)
                role_ink = kit::colour::accent;
            else if (hot(state, id))
                role_ink = kit::colour::text;
            if (text_w > 0)
                add_line(
                    built,
                    fonts,
                    {text_x, y, text_w, row_h},
                    row.role,
                    kit::FontRole::small,
                    role_ink
                );
            kit::Control control;
            control.id = id;
            control.rect = {table_x, y, std::max(table_w, int32_t{1}), row_h};
            control.enabled = true;
            control.focusable = false;
            control.name = name;
            control.kind = kit::ControlKind::button;
            // The picture draws the role beside the chip. The control names the
            // chord as well, so a row reads as "Alt+Left button · Orbit camera".
            control.text = row.free ? row.role : mouse_row_chord(editor, row) + " · " + row.role;
            built.rest.push_back(std::move(control));
            y += row_h + 2;
            ++row_index;
        }
    } else {
        int32_t unit = 1;
        while (unit < 3 && (unit + 1) * 64 <= avail_h && (unit + 1) * 40 + 180 <= body.width)
            ++unit;
        const int32_t pic_w = 40 * unit;
        const int32_t pic_h = std::min(64 * unit, avail_h);
        const kit::Rect picture{body.x, body.y, pic_w, std::max(pic_h, int32_t{1})};
        const std::size_t before = built.list.controls.size();
        kit::add_mouse(built.list, picture, look, controls::mouse_first, std::string(name_mouse));
        const std::size_t rest = built.rest.size();
        steal(built.list, built.rest, before);
        name_mouse_roles(built, rest, named);
        const bool beside = body.width - pic_w - gap >= 160;
        const int32_t table_x = beside ? body.x + pic_w + gap : body.x;
        const int32_t table_w = std::max(body.x + body.width - table_x, int32_t{0});
        int32_t y = beside ? body.y : body.y + pic_h + gap;
        const std::vector<RoleLine> rows = mouse_table(editor, editor.scheme());
        std::string place;
        int place_number = 0;
        int row_index = 0;
        for (const RoleLine& row : rows) {
            if (row.place_id != place) {
                place = row.place_id;
                place_number = 0;
                if (y + line_h > bottom)
                    break;
                add_line(
                    built,
                    fonts,
                    {table_x, y, table_w, line_h},
                    row.place,
                    kit::FontRole::small,
                    kit::colour::quiet
                );
                y += line_h;
            }
            if (y + metrics.field_height > bottom)
                break;
            ++place_number;
            // The chord with its modifiers, so Alt+Left's role does not read
            // as Left's.
            std::string caption =
                row.free ? row.role : mouse_row_chord(editor, row) + " · " + row.role;
            kit::ButtonLook button;
            button.caption = kit::cut_to_width(
                fonts, kit::FontRole::small, caption, std::max(table_w, int32_t{1})
            );
            button.style = kit::ButtonStyle::quiet;
            const kit::ControlId id =
                controls::mouse_table_first + static_cast<kit::ControlId>(row_index);
            const std::string name = std::string(name_mouse_table_prefix) + row.place_id + "." +
                                     std::to_string(place_number);
            button.hovered = hot(state, id);
            const kit::Rect rect{table_x, y, table_w, metrics.field_height};
            if (roomy(rect)) {
                const std::size_t added = built.list.controls.size();
                kit::add_button(built.list, rect, button, id, name);
                steal(built.list, built.rest, added);
                if (!built.rest.empty())
                    built.rest.back().focusable = false;
            }
            y += metrics.field_height + 2;
            ++row_index;
        }
    }
    if (body.width > 0) {
        int32_t line_y = note_y;
        for (const std::string& line : note_lines) {
            add_line(
                built,
                fonts,
                {body.x, line_y, body.width, metrics.small_line},
                line,
                kit::FontRole::small,
                kit::colour::hint
            );
            line_y += metrics.small_line;
        }
    }
}

/// The category drop-down's words: the chosen category's title.
///
/// @param editor the words
/// @param state the chosen category
/// @return the caption
std::string category_caption(const Editor& editor, const ScreenState& state) {
    const std::string wanted = state.category.empty() ? std::string("all") : state.category;
    for (const CategoryEntry& category : editor.categories()) {
        if (category.id == wanted)
            return category.title;
    }
    return show(editor.text(), words::all_actions);
}

/// Lays the editor out. `page` is Compact's details page.
///
/// @param editor the editor
/// @param state the screen's state
/// @param frame the frame
/// @param fonts the fonts
/// @param page the details page
/// @return the display list
kit::DisplayList
lay(const Editor& editor,
    const ScreenState& state,
    const kit::Frame& frame,
    const kit::Fonts& fonts,
    bool page) {
    if (state.prompt != Prompt::none) {
        // The editor stays under the prompt, dimmed, as a question sits over
        // the screen that opened it. Its controls are not live: a press on
        // the dim reaches the prompt only.
        ScreenState behind = state;
        behind.prompt = Prompt::none;
        behind.interaction.focus_shown = false;
        kit::DisplayList shown = lay(editor, behind, frame, fonts, page);
        shown.controls.clear();
        shown.tab_order.clear();
        const Window window = window_of(frame);
        kit::Item& veil = add_item(shown, kit::Role::blend, {0, 0, window.width, window.height});
        veil.colour = kit::colour::backdrop;
        veil.opacity = kit::menu_backdrop_opacity;
        const kit::DisplayList prompt = prompt_layout(editor, state, frame, fonts);
        shown.items.insert(shown.items.end(), prompt.items.begin(), prompt.items.end());
        shown.controls = prompt.controls;
        shown.tab_order = prompt.tab_order;
        return shown;
    }
    Built built;
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    const bool compact = frame.size_class == kit::SizeClass::compact;
    const bool large = frame.size_class == kit::SizeClass::large;
    const Window window = window_of(frame);
    const int32_t content_w = std::max(window.right - window.left, int32_t{0});
    add_item(built.list, kit::Role::fill, {0, 0, window.width, window.height}).colour =
        kit::colour::panel;
    add_header(built, editor, state, fonts, window, compact);

    kit::Rect layout_field{};
    kit::Rect category_field{};
    kit::Rect more_field{};
    kit::Rect shape_field{};
    // The toolbar takes Kit 2's sizes, as the Library's does: the tab strip,
    // text fields and drop-downs, filter chips and control-high buttons, its
    // rows the window's gap apart.
    const int32_t tab_h = metrics.tab_strip_height;
    const int32_t field_h = metrics.text_field_height;
    const int32_t chip_h = metrics.filter_chip_height;
    int32_t y = window.body_top + window.gap;
    const bool show_keyboard = !page && state.view == View::keyboard;
    const bool show_mouse = !page && state.view == View::mouse;
    if (show_keyboard || show_mouse) {
        const int32_t gap = metrics.button_gap;
        const int32_t tabs_w =
            add_view_tabs(built, editor, state, fonts, metrics, window.left, y, content_w);
        const auto bars = [&](int32_t bar_left, int32_t bar_right, int32_t bar_top) {
            if (show_keyboard)
                return add_layer_bar(
                    built, editor, state, fonts, metrics, bar_left, bar_right, bar_top, shape_field
                );
            return add_scheme_bar(
                built, editor, state, fonts, metrics, bar_left, bar_right, bar_top
            );
        };
        if (compact || window.right - (window.left + tabs_w + gap) < 220) {
            y += tab_h + window.gap;
            y = bars(window.left, window.right, y) + window.gap;
        } else {
            // The bar sits in the middle of the tab strip's height.
            const int32_t bar_h = std::max(field_h, chip_h);
            const int32_t bar_top = y + std::max((tab_h - bar_h) / 2, int32_t{0});
            const int32_t tools_bottom = bars(window.left + tabs_w + gap, window.right, bar_top);
            y = std::max(y + tab_h, tools_bottom) + window.gap;
        }
    } else if (!page) {
        const int32_t gap = metrics.button_gap;
        // The search, then Find by key at its right: typing searches
        // commands, and Find by key listens for a key apart from it.
        auto place_search = [&](const kit::Rect& given) {
            kit::Rect rect = given;
            const std::string find_caption = show(editor.text(), words::find_by_key);
            const int32_t find_w = std::min(
                button_width(fonts, metrics, find_caption), std::max(rect.width / 2, int32_t{0})
            );
            if (find_w > 8 && rect.width - find_w - gap > 24) {
                // Find by key is a control-high button in the middle of the
                // field's row.
                const int32_t find_h = std::min(metrics.control_height, rect.height);
                const kit::Rect find_rect{
                    rect.x + rect.width - find_w,
                    rect.y + (rect.height - find_h) / 2,
                    find_w,
                    find_h,
                };
                kit::ButtonLook find;
                find.caption = kit::cut_to_width(fonts, kit::FontRole::small, find_caption, find_w);
                find.style = kit::ButtonStyle::plain;
                find.hovered = hot(state, controls::find);
                find.held = held(state, controls::find);
                rect.width = std::max(rect.width - find_w - gap, int32_t{0});
                kit::SearchLook search;
                search.field = state.search;
                search.focused = focused_on(state, controls::search);
                search.hovered = hot(state, controls::search);
                search.magnifier = true;
                std::string placeholder = show(editor.text(), words::search_commands);
                const int32_t text_room = kit::field_text_rect(rect, search).width;
                if (kit::text_width(fonts, kit::FontRole::regular, placeholder) > text_room)
                    placeholder = kit::cut_to_width(
                        fonts, kit::FontRole::regular, placeholder, std::max(text_room, int32_t{0})
                    );
                search.placeholder = std::move(placeholder);
                const std::size_t before = built.list.controls.size();
                kit::add_search(
                    built.list, rect, search, controls::search, std::string(name_search)
                );
                steal(built.list, built.rest, before);
                built.tabs.push_back(controls::search);
                place_button(built, find_rect, find, controls::find, std::string(name_find), true);
                return;
            }
            kit::SearchLook search;
            search.field = state.search;
            search.focused = focused_on(state, controls::search);
            search.hovered = hot(state, controls::search);
            search.magnifier = true;
            search.placeholder = kit::cut_to_width(
                fonts,
                kit::FontRole::regular,
                show(editor.text(), words::search_commands),
                std::max(kit::field_text_rect(rect, search).width, int32_t{0})
            );
            const std::size_t before = built.list.controls.size();
            kit::add_search(built.list, rect, search, controls::search, std::string(name_search));
            steal(built.list, built.rest, before);
            built.tabs.push_back(controls::search);
        };
        auto place_category = [&](const kit::Rect& rect) {
            category_field = rect;
            place_choice(
                built,
                state,
                rect,
                choice_text(fonts, metrics, rect.width, category_caption(editor, state)),
                controls::category,
                std::string(name_category),
                state.menu == Menu::category
            );
        };
        if (compact) {
            // The view tabs head the screen. Category and search share the next row.
            add_view_tabs(built, editor, state, fonts, metrics, window.left, y, content_w);
            y += tab_h + window.gap;
            const int32_t category_w = std::min(compact_category_width, content_w);
            if (category_w > 0)
                place_category({window.left, y, category_w, field_h});
            const int32_t search_x = window.left + category_w + (category_w > 0 ? gap : 0);
            const int32_t search_w = std::max(window.right - search_x, int32_t{0});
            if (search_w > 0)
                place_search({search_x, y, search_w, field_h});
            y += field_h + window.gap;
        } else {
            // LAYOUT, its drop-down, the aside, and a search held at the named width.
            const int32_t layout_w = large ? large_layout_width : regular_layout_width;
            const int32_t search_w = large ? large_search_width : regular_search_width;
            const std::string layout_label = show(editor.text(), words::layout);
            int32_t label_w = kit::text_width(fonts, kit::FontRole::small, layout_label);
            const int32_t least = label_w + gap + layout_w + gap + search_w;
            if (least > content_w)
                label_w = std::max(label_w - (least - content_w), int32_t{0});
            int32_t x = window.left;
            if (label_w > 0) {
                const std::string shown =
                    kit::text_width(fonts, kit::FontRole::small, layout_label) > label_w
                        ? kit::cut_to_width(fonts, kit::FontRole::small, layout_label, label_w)
                        : layout_label;
                add_line(
                    built,
                    fonts,
                    {x, y, label_w, field_h},
                    shown,
                    kit::FontRole::small,
                    kit::colour::quiet
                );
                x += label_w + gap;
            }
            int32_t used_layout = layout_w;
            int32_t used_search = search_w;
            const int32_t remain = std::max(window.right - x, int32_t{0});
            if (used_layout + gap + used_search > remain) {
                const int32_t over = used_layout + gap + used_search - remain;
                const int32_t search_give = std::min(used_search, over);
                used_search -= search_give;
                used_layout = std::max(used_layout - (over - search_give), int32_t{0});
            }
            if (used_layout > 0) {
                layout_field = {x, y, used_layout, field_h};
                place_choice(
                    built,
                    state,
                    layout_field,
                    choice_text(fonts, metrics, used_layout, editor.layout_name()),
                    controls::layout_choice,
                    std::string(name_layout),
                    state.menu == Menu::layout
                );
            }
            if (used_search > 0) {
                const int32_t search_x = window.right - used_search;
                const int32_t aside_x = x + used_layout + gap;
                const int32_t aside_w = search_x - gap - aside_x;
                if (aside_w > 8)
                    add_summary(built, editor, fonts, {aside_x, y, aside_w, field_h});
                place_search({search_x, y, used_search, field_h});
            }
            y += field_h + window.gap;
            // The view tabs lead the filter row. The pills stay to their
            // right while they fit there, and take a row of their own under
            // it when they do not.
            const int32_t tabs_w =
                add_view_tabs(built, editor, state, fonts, metrics, window.left, y, content_w);
            const int32_t after_tabs = window.left + tabs_w + (tabs_w > 0 ? gap : 0);
            int32_t pills_left = after_tabs;
            if (!large) {
                int32_t category_w = std::min(
                    regular_category_width, std::max(window.right - after_tabs, int32_t{0})
                );
                if (after_tabs + category_w + gap + 80 > window.right)
                    category_w = std::max(window.right - after_tabs - gap - 80, int32_t{0});
                const int32_t field_y = y + std::max((tab_h - field_h) / 2, int32_t{0});
                if (category_w > 0)
                    place_category({after_tabs, field_y, category_w, field_h});
                pills_left = after_tabs + category_w + (category_w > 0 ? gap : 0);
            }
            const int32_t packed = plan_pills(editor, state, fonts, metrics, content_w).packed;
            if (pills_left + packed <= window.right) {
                const int32_t pills_top = y + std::max((tab_h - chip_h) / 2, int32_t{0});
                const int32_t pills_bottom = add_pills(
                    built, editor, state, fonts, metrics, pills_left, window.right, pills_top, large
                );
                y = std::max(y + tab_h, pills_bottom) + window.gap;
            } else {
                y += tab_h + window.gap;
                y = add_pills(
                        built, editor, state, fonts, metrics, window.left, window.right, y, false
                    ) +
                    window.gap;
            }
        }
    }

    if (page) {
        // Compact's details page: Back names the list it returns to, which
        // keeps its place, its search and its filter.
        const std::string caption = "‹ " + category_caption(editor, state);
        const int32_t back_w = std::min(button_width(fonts, metrics, caption), content_w);
        kit::ButtonLook back;
        back.caption = kit::cut_to_width(fonts, kit::FontRole::small, caption, back_w);
        back.style = kit::ButtonStyle::quiet;
        back.hovered = hot(state, controls::back);
        back.held = held(state, controls::back);
        place_button(
            built,
            {window.left, y, back_w, metrics.control_height},
            back,
            controls::back,
            std::string(name_back),
            true
        );
        y += metrics.control_height + metrics.space;
    }
    const int32_t body_h = std::max(window.footer_rule - y, int32_t{0});
    const kit::Rect body{window.left, y, content_w, body_h};
    if (page) {
        add_details(built, editor, state, fonts, metrics, body, window);
    } else if (show_keyboard) {
        add_keyboard_body(built, editor, state, fonts, metrics, body, compact);
    } else if (show_mouse) {
        add_mouse_body(built, editor, state, fonts, metrics, body, compact);
    } else if (compact) {
        add_list(built, editor, state, fonts, metrics, frame.size_class, body, window);
    } else if (large) {
        const int32_t nav_w = std::min(large_nav_width, content_w);
        int32_t details_w = std::min(
            large_details_width, std::max(content_w - nav_w - metrics.padding, int32_t{0})
        );
        int32_t list_w = content_w - nav_w - details_w - metrics.padding * 2;
        if (list_w < 80) {
            details_w = std::max(details_w - (80 - list_w), int32_t{0});
            list_w = content_w - nav_w - details_w - metrics.padding * 2;
        }
        if (list_w < 0)
            list_w = 0;
        add_nav(built, editor, state, fonts, {window.left, y, nav_w, body_h});
        const int32_t list_x = window.left + nav_w + metrics.padding;
        add_list(
            built,
            editor,
            state,
            fonts,
            metrics,
            frame.size_class,
            {list_x, y, std::max(list_w, int32_t{0}), body_h},
            window
        );
        const int32_t details_x = list_x + std::max(list_w, int32_t{0}) + metrics.padding;
        add_details(
            built,
            editor,
            state,
            fonts,
            metrics,
            {details_x, y, std::max(window.right - details_x, int32_t{0}), body_h},
            window
        );
    } else {
        int32_t details_w = std::min(regular_details_width, std::max(content_w / 3, int32_t{0}));
        int32_t list_w = content_w - details_w - metrics.padding;
        if (list_w < 80) {
            details_w = std::max(content_w - 80 - metrics.padding, int32_t{0});
            list_w = content_w - details_w - metrics.padding;
        }
        add_list(
            built,
            editor,
            state,
            fonts,
            metrics,
            frame.size_class,
            {window.left, y, std::max(list_w, int32_t{0}), body_h},
            window
        );
        const int32_t details_x = window.left + std::max(list_w, int32_t{0}) + metrics.padding;
        add_details(
            built,
            editor,
            state,
            fonts,
            metrics,
            {details_x, y, std::max(window.right - details_x, int32_t{0}), body_h},
            window
        );
    }

    add_footer(built, editor, state, fonts, metrics, window, page, compact);
    if (page) {
        // Tab: Back, the page's controls, Undo, Save changes, then the close mark.
        std::vector<kit::ControlId> order;
        for (const kit::ControlId id : built.tabs) {
            if (id == controls::back)
                order.push_back(id);
        }
        for (const kit::ControlId id : built.details)
            order.push_back(id);
        for (const kit::ControlId id : built.tabs) {
            if (id == controls::undo || id == controls::save)
                order.push_back(id);
        }
        built.tabs = std::move(order);
    } else if (show_keyboard || show_mouse) {
        std::vector<kit::ControlId> order;
        auto take = [&](kit::ControlId id) {
            for (const kit::ControlId tab : built.tabs) {
                if (tab == id)
                    order.push_back(id);
            }
        };
        take(controls::view_list);
        take(controls::view_keyboard);
        take(controls::view_mouse);
        take(controls::layer_none);
        take(controls::layer_shift);
        take(controls::layer_ctrl);
        take(controls::layer_alt);
        take(controls::layer_cmd);
        take(controls::keyboard_shape);
        take(controls::scheme_right);
        take(controls::scheme_left);
        take(controls::show_as_list);
        for (const kit::ControlId tab : built.tabs) {
            if (tab >= controls::keyboard_first && tab < controls::mouse_first)
                order.push_back(tab);
        }
        take(controls::key_list);
        take(controls::card_change);
        take(controls::card_use);
        take(controls::undo);
        take(controls::more);
        take(controls::cancel);
        take(controls::save);
        built.tabs = std::move(order);
    } else {
        std::vector<kit::ControlId> order;
        auto take = [&](kit::ControlId id) {
            for (const kit::ControlId tab : built.tabs) {
                if (tab == id)
                    order.push_back(id);
            }
        };
        take(controls::view_list);
        take(controls::view_keyboard);
        take(controls::view_mouse);
        take(controls::layout_choice);
        take(controls::search);
        take(controls::find);
        take(controls::category);
        for (const kit::ControlId tab : built.tabs) {
            if (tab >= controls::nav_first && tab < controls::row_first)
                order.push_back(tab);
        }
        take(controls::filter_all);
        take(controls::filter_changed);
        take(controls::filter_clashes);
        take(controls::filter_unbound);
        take(controls::filter_latchable);
        take(controls::list);
        if (built.number_keys)
            order.push_back(controls::number_keys);
        for (const kit::ControlId id : built.details)
            order.push_back(id);
        take(controls::undo);
        take(controls::more);
        take(controls::cancel);
        take(controls::save);
        built.tabs = std::move(order);
    }
    if (built.close != kit::no_control)
        built.tabs.push_back(built.close);

    add_focus(built, state, metrics, window);
    kit::Rect menu_field{};
    if (state.menu == Menu::layout)
        menu_field = layout_field;
    else if (state.menu == Menu::category)
        menu_field = category_field;
    else if (state.menu == Menu::shape)
        menu_field = shape_field;
    else if (state.menu == Menu::more) {
        // More's menu is as wide as its widest item, its right edge on More's.
        for (const kit::Control& control : built.rest) {
            if (control.id == controls::more)
                more_field = control.rect;
        }
        int32_t widest = more_field.width;
        for (const MenuEntry& entry : menu_entries(editor, Menu::more, state, compact))
            widest = std::max(
                widest,
                kit::text_width(fonts, kit::FontRole::regular, entry.caption) +
                    metrics.choice_item_text_inset * 2
            );
        widest = std::min(widest, window.right - window.left);
        more_field.x = std::max(more_field.x + more_field.width - widest, window.left);
        more_field.width = widest;
        menu_field = more_field;
    }
    add_menu(built, editor, state, fonts, metrics, window, menu_field, compact);

    kit::DisplayList laid = std::move(built.list);
    laid.controls.clear();
    for (kit::Control& control : built.menu)
        laid.controls.push_back(std::move(control));
    for (kit::Control& control : built.rest)
        laid.controls.push_back(std::move(control));
    laid.tab_order = std::move(built.tabs);
    static_cast<void>(more_field);
    return laid;
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

kit::DisplayList editor_layout(
    const Editor& editor, const ScreenState& state, const kit::Frame& frame, const kit::Fonts& fonts
) {
    if (frame.size_class == kit::SizeClass::compact && state.details_page)
        return details_page_layout(editor, state, frame, fonts);
    return lay(editor, state, frame, fonts, false);
}

kit::DisplayList details_page_layout(
    const Editor& editor, const ScreenState& state, const kit::Frame& frame, const kit::Fonts& fonts
) {
    return lay(editor, state, frame, fonts, true);
}

} // namespace oa::ui::key_editor
