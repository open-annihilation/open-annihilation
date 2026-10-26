// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The key editor's layout: one kit window, the list of actions and the
// chosen action's details. The named field widths are the fields' widths.
// Buttons grow to the words they show.
#include "oa/ui/key_editor/screen.hpp"

#include "list_geom.hpp"
#include "menu_geom.hpp"
#include "screen_controls.hpp"

#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_editor/text.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/keys.hpp"
#include "oa/ui/kit/text.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::key_editor {
namespace {

namespace keys = oa::ui::key_bindings;

/// The window the editor is laid out in.
struct Window {
    int32_t width{};       ///< the window's width, in points
    int32_t height{};      ///< the window's height, in points
    int32_t left{};        ///< the content's left column
    int32_t right{};       ///< the column after the content's last
    int32_t body_top{};    ///< the first row under the header's rule
    int32_t footer_rule{}; ///< the row of the line over the footer
    int32_t footer_top{};  ///< the footer's first row
    int32_t button_top{};  ///< the footer's buttons' top row
    int32_t gap{};         ///< the rows between stacked parts
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
    kit::Colour ink
) {
    if (!roomy(rect))
        return;
    text = kit::cut_to_width(fonts, font, text, rect.width);
    kit::Item& item = add_item(built.list, kit::Role::text, rect);
    item.text = std::move(text);
    item.font = font;
    item.colour = ink;
    item.align = kit::Align::left;
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
    return std::max(metrics.button_width, text);
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
    window.left = metrics.padding;
    window.right = std::max(size.x - metrics.edge - metrics.padding, window.left);
    window.body_top = metrics.edge + metrics.header_height + 1;
    window.footer_rule = size.y - metrics.edge - metrics.footer_height - 1;
    window.footer_top = window.footer_rule + 1;
    window.button_top = window.footer_top + (metrics.footer_height - metrics.button_height) / 2;
    window.gap = metrics.paragraph_gap;
    return window;
}

/// Appends the header. The kit keeps Compact's places at every class, and
/// the fit of the title is judged on those places.
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
    const int32_t header_rule = compact_metrics.edge + compact_metrics.header_height;
    add_item(built.list, kit::Role::header, {0, 0, window.width, header_rule + 1}).look =
        std::move(look);

    const int32_t side = compact_metrics.button_height;
    const kit::Rect rect{
        window.width - compact_metrics.edge - compact_metrics.padding - side,
        compact_metrics.edge + (compact_metrics.header_height - side) / 2,
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
    const std::string own =
        editor.changes() == 1
            ? std::string(show(editor.text(), words::own_change_one))
            : fill(
                  show(editor.text(), words::own_changes), {{"n", std::to_string(editor.changes())}}
              );
    const std::string shown = by + " · " + own;
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
    control.text = kit::cut_to_width(fonts, kit::FontRole::small, shown, rect.width);
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
    const Filter filters[] = {
        Filter::all, Filter::changed, Filter::clashes, Filter::unbound, Filter::latchable
    };
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
    const std::string_view words_of[] = {
        words::filter_all,
        words::filter_changed,
        words::filter_clashes,
        words::filter_unbound,
        words::filter_latchable
    };
    const int32_t height = kit::pill_height();
    const int32_t room = std::max(right - left, int32_t{0});
    kit::PillLook looks[5];
    int32_t widths[5] = {};
    int32_t packed = 0;
    for (int index = 0; index < 5; ++index) {
        kit::PillLook look;
        look.text = pill_caption(editor, state, filters[index], words_of[index]);
        look.state =
            state.filter == filters[index] ? kit::PillState::active : kit::PillState::inactive;
        look.hovered = hot(state, ids[index]);
        look.focused = focused_on(state, ids[index]);
        int32_t width = kit::pill_width(fonts, look);
        if (width > room && room > 0) {
            look.text = kit::cut_to_width(
                fonts, kit::FontRole::small, look.text, std::max(room - 8, int32_t{0})
            );
            width = std::min(kit::pill_width(fonts, look), room);
        }
        looks[index] = std::move(look);
        widths[index] = width;
        packed += width;
    }
    packed += metrics.chip_gap * 4;
    int32_t x = left;
    if (align_right && packed <= room)
        x = right - packed;
    int32_t y = top;
    for (int index = 0; index < 5; ++index) {
        const kit::PillLook& look = looks[index];
        const int32_t width = widths[index];
        if (x > left && x + width > right) {
            x = left;
            y += height + metrics.button_gap;
        }
        const kit::Rect rect{x, y, std::min(width, std::max(right - x, int32_t{0})), height};
        if (roomy(rect)) {
            const std::size_t before = built.list.controls.size();
            kit::add_pill(built.list, rect, look, ids[index], std::string(names[index]));
            steal(built.list, built.rest, before);
            built.tabs.push_back(ids[index]);
        }
        x += width + metrics.chip_gap;
    }
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

/// Plans one nav entry. "All" is the first entry's word. The count sits at
/// the right of the first line, and a title that does not fit beside it wraps.
///
/// @param editor the words
/// @param fonts the fonts
/// @param category the category
/// @param inner the columns the title and the count share
/// @return the plan
NavPlan plan_nav_entry(
    const Editor& editor, const kit::Fonts& fonts, const CategoryEntry& category, int32_t inner
) {
    NavPlan plan;
    plan.heading = category.id.starts_with("heading.");
    const std::string title =
        category.id == "all" ? std::string(show(editor.text(), words::filter_all)) : category.title;
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
    std::vector<NavPlan> plans;
    plans.reserve(categories.size());
    int32_t total_h = 0;
    for (const CategoryEntry& category : categories) {
        plans.push_back(plan_nav_entry(editor, fonts, category, inner));
        total_h += plans.back().height;
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
    int32_t y = area.y;
    int32_t index = 0;
    for (const CategoryEntry& category : categories) {
        const NavPlan& plan = plans[static_cast<std::size_t>(index)];
        if (y + plan.height > area.y + area.height)
            break;
        kit::NavEntry entry;
        entry.rect = {area.x, y, area.width, plan.height};
        entry.selected = !plan.heading && category.id == state.category;
        const kit::ControlId id = controls::nav_first + static_cast<kit::ControlId>(index);
        entry.hovered = hot(state, id);
        entry.focused = focused_on(state, id);
        look.entries.push_back(std::move(entry));
        ids.push_back(id);
        names.push_back(std::string(name_nav_prefix) + category.id);
        if (!plan.heading)
            built.tabs.push_back(id);
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
    for (std::size_t place = 0; place < drawn; ++place) {
        kit::Control& control = built.rest[tail + place];
        const NavPlan& plan = plans[place];
        if (plan.heading)
            control.focusable = false;
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

        const int32_t chip_h = std::max(shown.height - 4, int32_t{1});
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
    enum class Kind : uint8_t { text, button, toggle, chip, square };
    Kind kind{Kind::text};                           ///< what it is
    int32_t y{};                                     ///< its top, in the content
    int32_t height{};                                ///< its height
    int32_t x{};                                     ///< its left, in the content
    int32_t width{};                                 ///< its width
    std::string text{};                              ///< a line, a caption or a letter
    kit::FontRole font{kit::FontRole::small};        ///< a line's font
    kit::Colour ink{kit::colour::text};              ///< a line's colour
    kit::ControlId id{kit::no_control};              ///< a control's number
    std::string name{};                              ///< a control's name
    bool enabled{true};                              ///< a control can be pressed
    bool on{};                                       ///< a switch is on
    kit::ButtonStyle style{kit::ButtonStyle::plain}; ///< a button's style
    kit::ButtonMark mark{kit::ButtonMark::none};     ///< a button's mark
    kit::ChordChipLook chip{};                       ///< a chord chip
    kit::KeyTint tint{kit::KeyTint::none};           ///< a title square's tint
};

/// The chosen action's pieces, top to bottom. `reset_in_body` is false on
/// Compact's page, where RESET is a footer button.
///
/// @param editor the editor
/// @param state the chosen action
/// @param fonts the fonts
/// @param metrics the size class
/// @param width the content's width
/// @param reset_in_body true when RESET sits in the details
/// @return the pieces
std::vector<Piece> detail_pieces(
    const Editor& editor,
    const ScreenState& state,
    const kit::Fonts& fonts,
    const kit::Metrics& metrics,
    int32_t width,
    bool reset_in_body
) {
    std::vector<Piece> pieces;
    if (width <= 0)
        return pieces;
    int32_t y = 0;
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
    if (!action.where.empty())
        text(action.where, kit::FontRole::small, kit::colour::hint);
    if (!action.description.empty())
        text(action.description, kit::FontRole::small, kit::colour::text);
    if (!action.kind.empty())
        text(
            show(editor.text(), words::fact_kind) + "  " + action.kind,
            kit::FontRole::small,
            kit::colour::hint
        );
    if (!action.base.empty())
        text(
            show(editor.text(), words::fact_base) + "  " + action.base,
            kit::FontRole::small,
            kit::colour::hint
        );
    if (!action.works_with.empty())
        text(
            show(editor.text(), words::fact_works) + "  " + action.works_with,
            kit::FontRole::small,
            kit::colour::hint
        );
    text(show(editor.text(), words::keys_heading), kit::FontRole::small, kit::colour::quiet);

    kit::KeyMark row_mark = kit::KeyMark::none;
    for (const Row& row : editor.rows(std::string_view{}, Filter::all, "")) {
        if (row.id == action.id) {
            row_mark = row.mark;
            break;
        }
    }
    const int32_t button_side = metrics.button_height;
    for (std::size_t index = 0; index < action.chords.size(); ++index) {
        const ChordDetail& chord = action.chords[index];
        const kit::KeyMark mark = chord.fixed ? kit::KeyMark::fixed : row_mark;
        Piece chip;
        chip.kind = Piece::Kind::chip;
        chip.y = y;
        chip.height = std::max(button_side, int32_t{16});
        chip.chip = chip_of(editor, chord.chord, mark);
        const int32_t remove_w = chord.removable ? button_side + metrics.button_gap : 0;
        chip.width = std::min(
            kit::chord_chip_width(fonts, chip.chip), std::max(width - remove_w, int32_t{0})
        );
        pieces.push_back(std::move(chip));
        if (chord.removable && width >= button_side) {
            Piece button;
            button.kind = Piece::Kind::button;
            button.y = y;
            button.height = button_side;
            button.width = button_side;
            button.x = width - button_side;
            button.mark = kit::ButtonMark::close;
            button.id =
                controls::chord_first + static_cast<kit::ControlId>(static_cast<int32_t>(index));
            button.name = std::string(name_chord_prefix) + std::to_string(index + 1) + ".remove";
            pieces.push_back(std::move(button));
        }
        y += std::max(button_side, int32_t{16}) + 2;
        if (!chord.explanation.empty())
            text(chord.explanation, kit::FontRole::small, kit::colour::hint);
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
    if (reset_in_body && !action.read_only && !action.reset.empty()) {
        Piece button;
        button.kind = Piece::Kind::button;
        button.y = y;
        button.height = metrics.button_height;
        button.text = action.reset;
        const int32_t natural = button_width(fonts, metrics, button.text);
        if (natural > width)
            button.text = kit::cut_to_width(fonts, kit::FontRole::small, button.text, width);
        button.width = std::min(
            std::max(button_width(fonts, metrics, button.text), metrics.button_width), width
        );
        button.id = controls::detail_reset;
        button.name = std::string(name_detail_reset);
        pieces.push_back(std::move(button));
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
/// @param reset_in_body true when RESET sits in the details
/// @param window the window, so a scroll bar is not placed outside it
void add_details(
    Built& built,
    const Editor& editor,
    const ScreenState& state,
    const kit::Fonts& fonts,
    const kit::Metrics& metrics,
    const kit::Rect& pane,
    bool reset_in_body,
    const Window& window
) {
    if (!roomy(pane))
        return;
    const bool scrolling_guess = true;
    const int32_t well_width = metrics.scroll_thumb_width + 2;
    const int32_t bar_gap = metrics.button_gap;
    const std::vector<Piece> measured = detail_pieces(
        editor,
        state,
        fonts,
        metrics,
        std::max(pane.width - well_width - bar_gap, int32_t{0}),
        reset_in_body
    );
    int32_t content_h = 0;
    for (const Piece& piece : measured)
        content_h = std::max(content_h, piece.y + piece.height);
    const bool scrolling = content_h > pane.height;
    const int32_t port_w =
        scrolling ? std::max(pane.width - well_width - bar_gap, int32_t{0}) : pane.width;
    const std::vector<Piece> pieces =
        scrolling ? measured : detail_pieces(editor, state, fonts, metrics, port_w, reset_in_body);
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
        if (piece.kind == Piece::Kind::square) {
            kit::KeycapLook cap;
            cap.label = piece.text;
            cap.tint = piece.tint;
            kit::add_keycap(built.list, full, cap, kit::no_control, {});
            continue;
        }
        if (piece.kind == Piece::Kind::chip) {
            kit::add_chord_chip(built.list, full, piece.chip, kit::no_control, {});
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
        built.details.push_back(piece.id);
    }
    static_cast<void>(scrolling_guess);
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

/// Appends the footer band, its note and its buttons, from the right.
///
/// @param[in,out] built the layout
/// @param editor the editor
/// @param state the pointer, and whether SAVE is the page's
/// @param fonts the fonts
/// @param metrics the size class
/// @param window the window
/// @param page Compact's details page
/// @param compact Compact's MORE
/// @param large Large's number-keys aside
void add_footer(
    Built& built,
    const Editor& editor,
    const ScreenState& state,
    const kit::Fonts& fonts,
    const kit::Metrics& metrics,
    const Window& window,
    bool page,
    bool compact,
    bool large
) {
    kit::FooterBandLook band;
    band.width = window.width;
    band.rule_row = window.footer_rule;
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
        bool accent{};
        bool enabled{true};
    };

    auto make = [&](kit::ControlId id,
                    std::string_view control_name,
                    std::string caption,
                    bool accent,
                    bool enabled) {
        FootButton button;
        button.id = id;
        button.name = std::string(control_name);
        button.caption = std::move(caption);
        button.accent = accent;
        button.enabled = enabled;
        return button;
    };
    // Large and Regular keep RESTORE DEFAULTS at the left. Everything else
    // packs from the right, and the note sits just left of that pack.
    FootButton left{};
    bool has_left = false;
    std::vector<FootButton> right;
    if (page) {
        right.push_back(
            make(controls::back, name_back, show(editor.text(), words::back), false, true)
        );
        const Details action = editor.details(state.selected);
        std::string reset = action.reset.empty() ? show(editor.text(), words::reset) : action.reset;
        right.push_back(make(
            controls::detail_reset,
            name_detail_reset,
            std::move(reset),
            false,
            !action.read_only && !action.id.empty()
        ));
    } else if (compact) {
        right.push_back(
            make(controls::more, name_more, show(editor.text(), words::more), false, true)
        );
    } else {
        left =
            make(controls::restore, name_restore, show(editor.text(), words::restore), false, true);
        has_left = true;
    }
    if (!page)
        right.push_back(
            make(controls::cancel, name_cancel, show(editor.text(), words::cancel), false, true)
        );
    right.push_back(
        make(controls::save, name_save, show(editor.text(), words::save), true, editor.can_save())
    );

    auto place = [&](const FootButton& button, int32_t at, int32_t width) {
        if (at + width > window.right)
            width = std::max(window.right - at, int32_t{0});
        const kit::Rect rect{at, window.button_top, width, metrics.button_height};
        if (!roomy(rect))
            return;
        kit::ButtonLook look;
        look.caption = button.caption;
        if (kit::text_width(fonts, kit::FontRole::small, look.caption) > rect.width)
            look.caption = kit::cut_to_width(fonts, kit::FontRole::small, look.caption, rect.width);
        look.style = button.accent ? kit::ButtonStyle::accent : kit::ButtonStyle::plain;
        look.enabled = button.enabled;
        look.hovered = hot(state, button.id);
        look.held = held(state, button.id);
        place_button(built, rect, look, button.id, button.name, true);
    };

    const int32_t gap = metrics.button_gap;
    std::vector<int32_t> right_widths;
    int32_t right_total = 0;
    for (const FootButton& button : right) {
        const int32_t width = button_width(fonts, metrics, button.caption);
        right_widths.push_back(width);
        right_total += width;
    }
    if (right.size() > 1)
        right_total += gap * (static_cast<int32_t>(right.size()) - 1);
    int32_t left_width = has_left ? button_width(fonts, metrics, left.caption) : 0;
    const int32_t content = std::max(window.right - window.left, int32_t{0});
    if (has_left && left_width + gap + right_total > content) {
        const int32_t room = std::max(content - right_total - gap, int32_t{0});
        left.caption = kit::cut_to_width(fonts, kit::FontRole::small, left.caption, room);
        left_width = std::min(button_width(fonts, metrics, left.caption), room);
    }
    int32_t right_x = window.right - right_total;
    if (right_x < window.left)
        right_x = window.left;
    if (has_left && left_width > 0) {
        const int32_t limit = std::max(right_x - gap, window.left);
        if (window.left + left_width > limit)
            left_width = std::max(limit - window.left, int32_t{0});
        place(left, window.left, left_width);
    }

    const int32_t note_left = has_left ? window.left + left_width + gap : window.left;
    const int32_t note_right = right_x - gap;
    std::string note;
    const std::size_t clashes = editor.draft_clashes();
    if (clashes == 1)
        note = std::string(show(editor.text(), words::settle_one));
    else if (clashes > 1)
        note = fill(show(editor.text(), words::settle), {{"n", std::to_string(clashes)}});
    else if (large && !page) {
        note = fill(
            show(editor.text(), words::number_keys_state),
            {{"state",
              show(editor.text(), editor.number_keys() ? words::state_on : words::state_off)}}
        );
    }
    if (!note.empty() && note_right - note_left > 8) {
        const int32_t note_room = note_right - note_left;
        note = kit::cut_to_width(fonts, kit::FontRole::small, note, note_room);
        const int32_t text_w =
            std::min(kit::text_width(fonts, kit::FontRole::small, note), note_room);
        const kit::Rect rect{note_right - text_w, window.button_top, text_w, metrics.button_height};
        add_line(built, fonts, rect, note, kit::FontRole::small, kit::colour::hint);
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
        place(right[index], right_x, right_widths[index]);
        right_x += right_widths[index] + gap;
    }
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
        control.checked = index == look.chosen;
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

/// The category drop-down's words. The whole list reads "All categories".
///
/// @param editor the words
/// @param state the chosen category
/// @return the caption
std::string category_caption(const Editor& editor, const ScreenState& state) {
    if (state.category.empty() || state.category == "all")
        return show(editor.text(), words::all_categories);
    for (const CategoryEntry& category : editor.categories()) {
        if (category.id == state.category)
            return category.title;
    }
    return show(editor.text(), words::all_categories);
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
    int32_t y = window.body_top + metrics.paragraph_gap / 2;
    if (!page) {
        const int32_t field_h = metrics.field_height;
        const int32_t gap = metrics.button_gap;
        auto place_search = [&](const kit::Rect& rect, bool long_word) {
            kit::SearchLook search;
            search.field = state.search;
            search.focused = focused_on(state, controls::search);
            search.hovered = hot(state, controls::search);
            search.magnifier = true;
            std::string placeholder =
                show(editor.text(), long_word ? words::search_actions : words::search_short);
            const int32_t text_room = kit::field_text_rect(rect, search).width;
            if (kit::text_width(fonts, kit::FontRole::regular, placeholder) > text_room)
                placeholder = kit::cut_to_width(
                    fonts, kit::FontRole::regular, placeholder, std::max(text_room, int32_t{0})
                );
            search.placeholder = std::move(placeholder);
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
            // Category and search share the row under the header. LAYOUT is in MORE.
            const int32_t category_w = std::min(compact_category_width, content_w);
            if (category_w > 0)
                place_category({window.left, y, category_w, field_h});
            const int32_t search_x = window.left + category_w + (category_w > 0 ? gap : 0);
            const int32_t search_w = std::max(window.right - search_x, int32_t{0});
            if (search_w > 0)
                place_search({search_x, y, search_w, field_h}, false);
            y += field_h + metrics.paragraph_gap / 2;
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
                place_search({search_x, y, used_search, field_h}, large);
            }
            y += field_h + metrics.paragraph_gap / 2;
            if (large) {
                y = add_pills(
                        built, editor, state, fonts, metrics, window.left, window.right, y, true
                    ) +
                    metrics.paragraph_gap / 2;
            } else {
                const int32_t category_w = std::min(regular_category_width, content_w);
                if (category_w > 0)
                    place_category({window.left, y, category_w, field_h});
                const int32_t pills_left = window.left + category_w + (category_w > 0 ? gap : 0);
                const int32_t pills_top =
                    y + std::max((field_h - kit::pill_height()) / 2, int32_t{0});
                add_pills(
                    built, editor, state, fonts, metrics, pills_left, window.right, pills_top, false
                );
                y += field_h + metrics.paragraph_gap / 2;
            }
        }
    }

    const int32_t body_h = std::max(window.footer_rule - y, int32_t{0});
    const kit::Rect body{window.left, y, content_w, body_h};
    if (page) {
        add_details(built, editor, state, fonts, metrics, body, false, window);
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
            true,
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
            true,
            window
        );
    }

    add_footer(built, editor, state, fonts, metrics, window, page, compact, large);
    if (page) {
        // Tab: the page's controls, BACK, RESET, SAVE, then the close mark.
        // place_button already appended BACK, RESET and SAVE. The details
        // were recorded first. Rebuild so the page's controls lead.
        std::vector<kit::ControlId> order = built.details;
        for (const kit::ControlId id : built.tabs) {
            if (id == controls::back || id == controls::detail_reset || id == controls::save)
                order.push_back(id);
        }
        built.tabs = std::move(order);
    } else {
        std::vector<kit::ControlId> order;
        auto take = [&](kit::ControlId id) {
            for (const kit::ControlId tab : built.tabs) {
                if (tab == id)
                    order.push_back(id);
            }
        };
        take(controls::layout_choice);
        take(controls::search);
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
        take(controls::restore);
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
    else if (state.menu == Menu::more) {
        for (const kit::Control& control : built.rest) {
            if (control.id == controls::more)
                more_field = control.rect;
        }
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
