// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The settings dialog's sections, rows and controls, where they lie, and
// what pointer and key events do to them.

#include "oa/ui/engine_settings/dialog.hpp"

#include "cards.hpp"
#include "content_page.hpp"
#include "developer.hpp"
#include "downloads_page.hpp"
#include "geometry.hpp"
#include "updates_page.hpp"
#include "language_page.hpp"
#include "registered.hpp"
#include "settings_rows.hpp"

#include "oa/base/text/line_break.hpp"
#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/ui/library/text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::engine_settings {

std::string update_size_text(uint64_t bytes) {
    return oa::ui::library::size_text(bytes);
}

namespace geometry {

namespace {

/// The sections a dialog of the engine's settings lists, in the list's order.
struct EnginePageList {
    std::array<Page, most_listed_pages> pages{}; ///< the sections; the first `count` count
    std::size_t count{};                         ///< how many it lists
};

/// Returns the engine's sections a dialog lists, in the list's order, by
/// group: Controls, Accessibility, then Touch and Controller where each is
/// listed; Graphics, User Interface, Language & Text; Game files where it is
/// listed, then Downloads, Updates and Game Content; Engine Limits, System
/// and Developer.
///
/// @param touch the dialog lists Touch
/// @param controller the dialog lists Controller
/// @param game_files the dialog lists Game files
/// @return the sections
constexpr EnginePageList engine_page_list(bool touch, bool controller, bool game_files) noexcept {
    EnginePageList list{};
    const auto add = [&list](Page page) { list.pages[list.count++] = page; };
    add(Page::controls);
    add(Page::accessibility);
    if (touch)
        add(Page::touch);
    if (controller)
        add(Page::controller);
    for (const Page page : {Page::graphics, Page::user_interface, Page::language})
        add(page);
    if (game_files)
        add(Page::game_files);
    for (const Page page :
         {Page::downloads,
          Page::updates,
          Page::game_content,
          Page::common_tweaks,
          Page::system,
          Page::developer})
        add(page);
    return list;
}

/// Returns a list's place among kEnginePageLists.
///
/// @param touch the dialog lists Touch
/// @param controller the dialog lists Controller
/// @param game_files the dialog lists Game files
/// @return 0 to 7: Touch adds 1, Controller 2 and Game files 4
constexpr std::size_t
engine_page_list_index(bool touch, bool controller, bool game_files) noexcept {
    return (touch ? 1U : 0U) + (controller ? 2U : 0U) + (game_files ? 4U : 0U);
}

/// Every list of the engine's sections, by engine_page_list_index.
constexpr std::array<EnginePageList, 8> kEnginePageLists{
    engine_page_list(false, false, false),
    engine_page_list(true, false, false),
    engine_page_list(false, true, false),
    engine_page_list(true, true, false),
    engine_page_list(false, false, true),
    engine_page_list(true, false, true),
    engine_page_list(false, true, true),
    engine_page_list(true, true, true),
};
static_assert(kEnginePageLists.front().count == 11, "eleven sections without the optional three");
static_assert(
    kEnginePageLists.back().count == most_listed_pages,
    "the longest list holds the most sections a dialog lists"
);
/// The one section a Language dialog lists.
constexpr std::array<Page, 1> kLanguageTextPages{Page::language};

/// Tells whether every engine list starts with Controls and Accessibility,
/// ends with Engine Limits, System and Developer, and holds the sections'
/// entry numbers as they were before the groups: Touch 5, Controller 6,
/// Developer 7, Game files 8, Downloads 9, System 10, Game Content 11,
/// Updates 12 and Accessibility 13.
///
/// @return true when those hold
constexpr bool entries_follow_the_list() noexcept {
    if (page_control(Page::touch) != 5 || page_control(Page::controller) != 6 ||
        page_control(Page::developer) != 7 || page_control(Page::game_files) != 8 ||
        page_control(Page::downloads) != 9 || page_control(Page::system) != 10 ||
        page_control(Page::game_content) != 11 || page_control(Page::updates) != 12 ||
        page_control(Page::accessibility) != 13)
        return false;
    for (const EnginePageList& listed : kEnginePageLists) {
        if (listed.count < 11 || listed.pages[0] != Page::controls ||
            listed.pages[1] != Page::accessibility ||
            listed.pages[listed.count - 3] != Page::common_tweaks ||
            listed.pages[listed.count - 2] != Page::system ||
            listed.pages[listed.count - 1] != Page::developer)
            return false;
    }
    return true;
}

static_assert(
    entries_follow_the_list(),
    "every list starts with Controls and Accessibility and ends with Engine Limits, System and "
    "Developer, and the entries keep their numbers"
);
static_assert(
    page_control(Page::game_files) == page_control(Page::developer) + 1,
    "Game files' entry number follows Developer's, so Touch, Controller and Developer keep theirs"
);
/// The mod options' sections, in the list's order.
constexpr std::array<Page, 5> kModPages{
    Page::mod_keys, Page::mod_patrol, Page::mod_guard, Page::mod_tools, Page::mod_chat
};

/// Returns a section's place among Dialog::scroll.
///
/// @param page the section
/// @return its index, below page_count
std::size_t scroll_index(Page page) noexcept {
    return std::min(static_cast<std::size_t>(page), page_count - 1);
}

} // namespace

/// Returns a text's width at estimated_character_width a character, as the
/// rows are placed where no font measures their words.
///
/// @param text the text, in UTF-8
/// @return the width, in source pixels
int32_t estimated_width(std::string_view text) {
    return static_cast<int32_t>(oa::ui::kit::character_count(text)) * estimated_character_width;
}

SourceRect folder_button(const SourceRect& area, std::size_t index) noexcept {
    int32_t left = area.x;
    for (std::size_t before = 0; before < index && before < folder_button_count; ++before)
        left += folder_button_widths[before] + folder_button_gap;
    const int32_t width = index < folder_button_count ? folder_button_widths[index] : 0;
    return {left, area.y, width, area.height};
}

std::size_t folder_button_at(const SourceRect& area, int32_t x, int32_t y) noexcept {
    for (std::size_t index = 0; index < folder_button_count; ++index) {
        const SourceRect button = folder_button(area, index);
        if (x >= button.x && y >= button.y && x < button.x + button.width &&
            y < button.y + button.height)
            return index;
    }
    return folder_button_count;
}

std::string_view shown_text(std::string_view english) {
    return oa::data::languages::interface_text(english);
}

std::string filled(
    std::string_view english,
    std::initializer_list<std::pair<std::string_view, std::string_view>> places
) {
    const std::string pattern(shown_text(english));
    std::string text;
    std::size_t at = 0;
    while (at < pattern.size()) {
        const std::size_t open = pattern.find('{', at);
        const std::size_t close = open == std::string::npos ? open : pattern.find('}', open);
        if (close == std::string::npos) {
            text.append(pattern, at, std::string::npos);
            break;
        }
        text.append(pattern, at, open - at);
        const std::string_view name(pattern.data() + open + 1, close - open - 1);
        bool found = false;
        for (const auto& [place, value] : places)
            if (place == name) {
                text += value;
                found = true;
                break;
            }
        if (!found)
            text.append(pattern, open, close - open + 1);
        at = close + 1;
    }
    return text;
}

std::string cut_text(
    std::string_view text, int32_t width, const std::function<int32_t(std::string_view)>& text_width
) {
    if (text_width(text) <= width)
        return std::string(text);
    const std::string ellipsis(path_ellipsis);
    std::size_t end = text.size();
    while (end > 0) {
        --end;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U)
            --end;
        std::string shown = std::string(text.substr(0, end)) + ellipsis;
        if (text_width(shown) <= width)
            return shown;
    }
    return ellipsis;
}

RowContext row_context(const Dialog& dialog) noexcept {
    return RowContext{
        dialog.steam_input,
        dialog.steam_deck_panel_hz,
        dialog.library_link && dialog.kind == DialogKind::engine,
        dialog.file_type_switches,
        !dialog.played_mod.empty()
    };
}

bool estimated_measure(const TextWidth& measure) {
    return measure("n") == estimated_character_width && measure("W") == estimated_character_width;
}

/// Breaks each hint into lines that fit the section, dropping an empty line.
/// A path stays on its one shortened line. A notice stays a notice on every
/// line it wraps to.
///
/// @param[in,out] shown the row
/// @param width the room, in points
/// @param measure a text's width in the small font
/// @param path the first line is a folder's path
void wrap_hints(kit::RowView& shown, int32_t width, const TextWidth& measure, bool path) {
    std::vector<std::string> lines;
    std::vector<bool> notices;
    const int32_t room = std::max(width, int32_t{1});
    for (std::size_t index = 0; index < shown.hints.size(); ++index) {
        if (shown.hints[index].empty())
            continue;
        const bool notice = index < shown.notices.size() && shown.notices[index];
        if (path && index == 0) {
            lines.push_back(std::move(shown.hints[index]));
            notices.push_back(notice);
            continue;
        }
        std::vector<std::string> wrapped = kit::wrap(shown.hints[index], room, measure);
        if (wrapped.empty())
            wrapped.push_back(shown.hints[index]);
        for (std::string& line : wrapped) {
            if (line.empty())
                continue;
            lines.push_back(std::move(line));
            notices.push_back(notice);
        }
    }
    if (lines.size() > most_row_lines) {
        lines.resize(most_row_lines);
        notices.resize(most_row_lines);
    }
    shown.hints = std::move(lines);
    shown.notices = std::move(notices);
}

/// Widens a level strip or a drop-down to its widest caption when a real font
/// measures it wider than the class's box, and keeps it inside the section.
///
/// @param[in,out] shown the row
/// @param available_width the section's width
/// @param measure a text's width in the small font
void fit_control(kit::RowView& shown, int32_t available_width, const TextWidth& measure) {
    if (estimated_measure(measure) || shown.captions.empty() || available_width <= 0)
        return;
    if (shown.kind != kit::RowKind::levels && shown.kind != kit::RowKind::choice)
        return;
    int32_t widest = 0;
    for (const std::string& caption : shown.captions)
        widest = std::max(widest, measure(caption));
    if (shown.kind == kit::RowKind::levels) {
        const int32_t count = std::max(shown.count, int32_t{1});
        const int32_t segment = widest + 8;
        const int32_t cap = std::max(shown.control_width, (available_width - 48) / count);
        shown.control_width = std::min(std::max(shown.control_width, segment), cap);
        return;
    }
    shown.control_width =
        std::min(std::max(shown.control_width, widest + 16), std::max(available_width, int32_t{1}));
}

/// Widens a switch, a row of buttons or SHOW AGAIN to the caption in the
/// small font. A label that does not fit beside its control takes the whole
/// line and the control drops under it. When the label and the lock do not
/// fit on the label line, on any row, the lock drops under the label and the
/// label keeps its width: the whole line, or the room beside a control that
/// stays. Key layout keeps its count of changes and shortens only the name.
/// An estimated measure leaves the class's boxes, which the layout tests pin;
/// place_rows makes the same drop from the estimate.
///
/// @param[in,out] shown the row
/// @param available_width the section's width
/// @param small a text's width in the small font
/// @param regular a text's width in the regular font; null keeps the label's line
void fit_measured(
    kit::RowView& shown, int32_t available_width, const TextWidth& small, const TextWidth* regular
) {
    if (estimated_measure(small) || available_width <= 0)
        return;
    if (shown.kind == kit::RowKind::toggle && !shown.captions.empty()) {
        int32_t half = 0;
        for (const std::string& caption : shown.captions)
            half = std::max(half, small(caption));
        shown.control_width = std::max(shown.control_width, kit::switch_width_for(half));
    }
    if (shown.kind == kit::RowKind::buttons && !shown.buttons.empty()) {
        shown.button_widths.clear();
        for (std::size_t at = 0; at < shown.buttons.size(); ++at) {
            const int32_t metric = at < folder_button_widths.size()
                                       ? folder_button_widths[at]
                                       : oa::ui::kit::compact_metrics.button_width;
            shown.button_widths.push_back(std::max(metric, small(shown.buttons[at]) + 8));
        }
    }
    if (shown.kind == kit::RowKind::value_and_button && !shown.buttons.empty()) {
        const int32_t wanted = small(shown.buttons.front()) + 8;
        if (wanted > shown.control_width)
            shown.control_width = wanted;
    }
    if (!shown.lock.empty()) {
        const int32_t need = small(shown.lock) + padlock_width + padlock_gap;
        shown.lock_columns = std::max(need, int32_t{1});
    }
    if (regular == nullptr || estimated_measure(*regular))
        return;
    if (shown.kind == kit::RowKind::choice && !shown.captions.empty()) {
        int32_t widest = 0;
        for (const std::string& caption : shown.captions)
            widest = std::max(widest, (*regular)(caption));
        // The closed field keeps its inset and the arrow. An open row keeps
        // the menu's inset and the margin after the words. The field takes
        // whichever is wider, and stays inside the section.
        constexpr int32_t menu_margin = 4;
        const int32_t field = widest + oa::ui::kit::compact_metrics.choice_text_inset +
                              oa::ui::kit::compact_metrics.choice_arrow_room;
        const int32_t menu =
            widest + oa::ui::kit::compact_metrics.choice_item_text_inset + menu_margin;
        shown.control_width = std::min(
            std::max(shown.control_width, std::max(field, menu)),
            std::max(available_width, int32_t{1})
        );
    }
    if (shown.kind == kit::RowKind::slider && !shown.value.empty()) {
        // The value is drawn in the regular font. The class's box stays when
        // the value fits it.
        const int32_t wanted = (*regular)(shown.value) + 8;
        if (wanted > oa::ui::kit::compact_metrics.slider_value_width)
            shown.value_columns = wanted;
    }
    if (shown.kind == kit::RowKind::value_and_button && !shown.value.empty()) {
        int32_t value_width = (*regular)(shown.value);
        const int32_t label_w = shown.label.empty() ? 0 : (*regular)(shown.label);
        const int32_t button = shown.control_width > 0
                                   ? shown.control_width
                                   : oa::ui::kit::compact_metrics.button_row_width;
        const int32_t lock = shown.lock.empty() ? 0
                                                : (shown.lock_columns > 0
                                                       ? shown.lock_columns
                                                       : oa::ui::kit::compact_metrics.lock_width) +
                                                      label_gap;
        // The value drops onto the button's line only when it does not fit
        // beside the label and the button. The lock drops under the label
        // instead of taking that room, and never takes the button's place.
        const int32_t without =
            (label_w > 0 ? label_w + label_gap : 0) + value_width + label_gap + button;
        if (!shown.control_below && without > available_width)
            shown.control_below = true;
        else if (!shown.control_below && lock > 0 && without + lock > available_width)
            shown.lock_below = true;
    }
    if (shown.label.empty())
        return;
    const int32_t label_width = (*regular)(shown.label);
    if (label_width <= 0)
        return;
    const auto lock_span = [&]() {
        if (shown.lock.empty())
            return int32_t{0};
        return (shown.lock_columns > 0 ? shown.lock_columns
                                       : oa::ui::kit::compact_metrics.lock_width) +
               label_gap;
    };
    if (shown.kind == kit::RowKind::levels && shown.count > 0 && !shown.control_below) {
        int32_t least = 1;
        for (const std::string& caption : shown.captions)
            least = std::max(least, small(caption) + 8);
        // Beside the label only. The lock drops under the label when it
        // does not fit, and never narrows the strip. A caption that does not
        // fit beside the label takes the line under it.
        const int32_t room = available_width - label_width - label_gap;
        const int32_t segment = (room - 2) / shown.count;
        const int32_t full = std::max((available_width - 2) / shown.count, int32_t{1});
        if (segment < least) {
            shown.control_below = true;
            shown.control_width = std::min(std::max(least, shown.control_width), full);
        } else if (segment < shown.control_width) {
            shown.control_width = segment;
        }
    }
    // A label that does not fit beside its control takes the whole line and
    // the control drops. When the three do not fit but the label and the
    // control do, the lock drops and the control stays. A slider, a drop-down,
    // a text field and a text row keep their control off this line, so the
    // lock drops when the label and the lock alone do not fit, and the label
    // keeps the line.
    const int32_t on_line = [&]() {
        if (shown.kind == kit::RowKind::toggle)
            return std::max(oa::ui::kit::compact_metrics.switch_width, shown.control_width);
        if (shown.kind == kit::RowKind::levels)
            return shown.count * shown.control_width + 2;
        if (shown.kind == kit::RowKind::buttons && !shown.button_widths.empty()) {
            int32_t summed = 0;
            for (std::size_t at = 0; at < shown.button_widths.size(); ++at)
                summed += shown.button_widths[at] +
                          (at > 0 ? oa::ui::kit::compact_metrics.folder_button_gap : 0);
            return summed;
        }
        if (shown.kind == kit::RowKind::buttons || shown.kind == kit::RowKind::value_and_button)
            return shown.control_width > 0 ? shown.control_width
                                           : oa::ui::kit::compact_metrics.button_row_width;
        return int32_t{0};
    }();
    if (on_line > 0) {
        const int32_t with_control = label_width + label_gap + on_line;
        if (shown.control_below) {
            if (!shown.lock.empty() && label_width + lock_span() > available_width)
                shown.lock_below = true;
        } else if (with_control > available_width) {
            shown.control_below = true;
            if (!shown.lock.empty() && label_width + lock_span() > available_width)
                shown.lock_below = true;
        } else if (!shown.lock.empty() && with_control + lock_span() > available_width) {
            shown.lock_below = true;
        }
    } else if (!shown.lock.empty() && label_width + lock_span() > available_width) {
        shown.lock_below = true;
    }
}

kit::RowView row_view(
    const Dialog& dialog,
    Setting setting,
    Lock lock,
    bool status,
    const TextWidth& small_width,
    int32_t available_width,
    const TextWidth* regular_width
) {
    kit::RowView shown =
        kit::view_of(row_spec(setting), reading(dialog.chosen, dialog), &shown_text);
    // The lock the dialog puts on the row, looked up as the dialog has
    // always drawn it, and a switch a language sets showing On.
    shown.lock =
        lock == Lock::none ? std::string() : std::string(shown_text(shown_text(lock_text(lock))));
    shown.hint_is_status = status;
    if (lock == Lock::set_by_language)
        shown.on = true;
    for (std::size_t line = 0; line < shown.hints.size(); ++line) {
        if (line == 0 && hint_is_path(setting)) {
            const std::string path = row_hint(dialog, setting, 0).text;
            shown.hints[line] =
                path.empty()
                    ? std::string()
                    : std::string(shown_text(path_tail(path, available_width, small_width)));
            continue;
        }
        shown.hints[line] = std::string(shown_text(shown.hints[line]));
    }
    if (setting == Setting::language)
        shown.value = language_closed_field(dialog, std::move(shown.value));
    const int32_t hint_room = estimated_measure(small_width)
                                  ? static_cast<int32_t>(sizes_of(dialog).hint_line_characters) *
                                        kit::estimated_character_width
                                  : available_width;
    wrap_hints(shown, hint_room, small_width, hint_is_path(setting));
    fit_control(shown, available_width, small_width);
    fit_measured(shown, available_width, small_width, regular_width);
    return shown;
}

Rows place_section(
    const Dialog& dialog,
    Page page,
    const Locks& locks,
    const SectionHooks* section,
    const TextWidth& small_width,
    kit::PlacedRows* placed,
    const Sizes* sizes,
    const TextWidth* regular_width
) {
    const Sizes& sized_here = sizes != nullptr ? *sizes : sizes_of(dialog);
    if (page == Page::downloads)
        return downloads::place(dialog, locks, small_width, placed, &sized_here, regular_width);
    if (page == Page::updates)
        return updates::place(dialog, locks, small_width, placed, &sized_here, regular_width);
    if (page == Page::game_content)
        return content::place(dialog, locks, small_width, placed, &sized_here);
    const auto settings = section_settings(page, section, row_context(dialog));
    const bool own_section = section != nullptr && section->settings != nullptr;
    const Sizes& sized = sized_here;
    // Accessibility's practice pad stands in a column right of its rows
    // where the section is wide enough, and follows them where it is not.
    const bool with_pad =
        page == Page::accessibility && !own_section && dialog.kind == DialogKind::engine;
    const int32_t pad_column = with_pad ? cards::pad_column(sized) : 0;
    const int32_t column_gap = kit::metrics_of(sized.size_class).space * 3;
    const int32_t rows_right =
        pad_column > 0 ? sized.content_right - pad_column - column_gap : sized.content_right;
    const int32_t rows_width = rows_right - sized.content_left;
    std::vector<kit::RowView> views;
    std::vector<Lock> row_locks;
    std::vector<bool> statuses;
    views.reserve(settings.size());
    for (const Setting setting : settings) {
        row_locks.push_back(section_lock(locks, setting, section));
        statuses.push_back(section_hint_is_status(setting, section));
        views.push_back(row_view(
            dialog,
            setting,
            row_locks.back(),
            statuses.back(),
            small_width,
            rows_width,
            regular_width
        ));
    }
    // Developer's own rows lie closer, over its list; while the dialog's
    // words are drawn in the modern fonts, a hint's lines lie further apart.
    kit::RowPlacement placement;
    placement.left = sized.content_left;
    placement.right = rows_right;
    placement.first_top = sized.first_row_top;
    placement.row_padding =
        page == Page::developer && !own_section ? developer_row_padding : row_padding;
    placement.tall = oa::data::languages::interface_language().needs !=
                     oa::data::languages::TextNeeds::game_fonts;
    placement.first_control = first_row_control;
    modern_control_heights(placement, dialog, sized.size_class);
    kit::PlacedRows kit_rows = kit::place_rows(views, placement);
    const TextWidth regular = regular_width != nullptr && !estimated_measure(*regular_width)
                                  ? *regular_width
                                  : TextWidth{&estimated_width};
    // Key layout is a card: its row takes the card's height, the rows under
    // it moving down by as much, and its control is Edit keys….
    for (std::size_t index = 0; index < kit_rows.rows.size(); ++index) {
        kit::PlacedRow& key_row = kit_rows.rows[index];
        if (settings[index] != Setting::key_layout)
            continue;
        const int32_t margin = kit::metrics_of(sized.size_class).space * 2;
        const cards::KeyCard card =
            cards::key_card(dialog, sized, key_row.top + margin, regular, small_width);
        const int32_t height = card.height + 2 * margin;
        const int32_t moved = height - key_row.height;
        key_row.height = height;
        // The card's label is its eyebrow, Key layout.
        key_row.label = card.lines.empty() ? kit::Rect{} : card.lines.front().rect;
        key_row.lock_area = {};
        key_row.hints.clear();
        key_row.value = {};
        key_row.control_area = card.edit;
        for (std::size_t later = index + 1; later < kit_rows.rows.size(); ++later) {
            kit::PlacedRow& following = kit_rows.rows[later];
            following.top += moved;
            for (kit::Rect* rect :
                 {&following.label,
                  &following.lock_area,
                  &following.control_area,
                  &following.value})
                if (rect->width > 0 && rect->height > 0)
                    rect->y += moved;
            for (kit::Rect& hint : following.hints)
                hint.y += moved;
        }
        kit_rows.bottom += moved;
    }
    Rows rows{};
    rows.bottom = kit_rows.bottom;
    rows.rows.reserve(kit_rows.rows.size());
    for (std::size_t index = 0; index < kit_rows.rows.size(); ++index) {
        const kit::PlacedRow& placed_row = kit_rows.rows[index];
        Row& row = rows.rows.emplace_back();
        row.setting = settings[index];
        row.control = placed_row.control;
        row.lock = row_locks[index];
        row.hint_is_status = statuses[index];
        row.top = placed_row.top;
        row.height = placed_row.height;
        row.label = placed_row.label;
        row.lock_area = placed_row.lock_area;
        row.hint_lines = std::min(placed_row.hints.size(), most_row_lines);
        for (std::size_t line = 0; line < row.hint_lines; ++line)
            row.hints[line] = placed_row.hints[line];
        row.control_area = placed_row.control_area;
        // Only a slider shows a value of its own beside its control. A text
        // row's value shares the label line; the layout gives it the right
        // of that line, so the label and the value do not overlap.
        if (placed_row.view.kind == kit::RowKind::slider)
            row.value = placed_row.value;
        else if (placed_row.view.kind == kit::RowKind::text && !placed_row.view.value.empty()) {
            const SourceRect line = placed_row.label;
            const int32_t columns = std::max(
                static_cast<int32_t>(oa::ui::kit::character_count(placed_row.view.value)) *
                    estimated_character_width,
                int32_t{1}
            );
            int32_t value_span = std::min(columns, std::max(line.width / 2, int32_t{1}));
            if (value_span + label_gap >= line.width)
                value_span = std::max((line.width - label_gap) / 2, int32_t{1});
            row.value = {line.x + line.width - value_span, line.y, value_span, line.height};
            row.label.width = std::max(row.value.x - label_gap - row.label.x, int32_t{1});
        }
    }
    if (with_pad) {
        // Right of the rows from the section's top, or under the last row;
        // either way the section's content reaches the pad's foot.
        const int32_t margin = kit::metrics_of(sized.size_class).space * 2;
        const int32_t left = pad_column > 0 ? sized.content_right - pad_column : sized.content_left;
        const int32_t top =
            pad_column > 0 ? sized.first_row_top + margin : kit_rows.bottom + margin;
        const int32_t width = pad_column > 0 ? pad_column : sized.content_width;
        const cards::Pad pad =
            cards::practice_pad(dialog, sized, left, top, width, regular, small_width);
        rows.pad = pad.card;
        rows.bottom = std::max(rows.bottom, pad.card.y + pad.card.height + margin);
    }
    if (placed != nullptr)
        *placed = std::move(kit_rows);
    return rows;
}

Rows place_rows(
    Page page,
    const Locks& locks,
    int32_t scroll,
    const SectionHooks* section,
    const RowContext& context
) {
    // The rows' places hang on what the dialog shows beyond each setting's
    // rows: the Steam Input notice and a Steam Deck's rate.
    Dialog shown;
    shown.steam_input = context.steam_input;
    shown.steam_deck_panel_hz = context.steam_deck_panel_hz;
    shown.library_link = context.library_link;
    shown.file_type_switches = context.file_type_switches;
    if (context.played_mod)
        shown.played_mod = "mod";
    Rows placed = place_section(shown, page, locks, section, &estimated_width);
    scroll_rows(placed, scroll);
    return placed;
}

void scroll_rows(Rows& rows, int32_t by) noexcept {
    const auto lift = [by](SourceRect& rect) {
        if (rect.width > 0 && rect.height > 0)
            rect.y -= by;
    };
    lift(rows.pad);
    for (Row& row : rows.rows) {
        row.top -= by;
        lift(row.label);
        lift(row.lock_area);
        for (SourceRect& hint : row.hints)
            lift(hint);
        lift(row.control_area);
        lift(row.value);
    }
    rows.bottom -= by;
}

int32_t content_height(const Rows& rows, int32_t scroll, int32_t first_top) noexcept {
    return rows.bottom + scroll + 1 + end_gap - first_top;
}

int32_t scroll_limit(int32_t content_height, kit::SizeClass size_class) noexcept {
    return std::max(content_height - sizes_of(size_class).view.height, int32_t{0});
}

Locks shown_locks(const Dialog& dialog) noexcept {
    Locks locks = dialog.locks;
    // The language chosen, System default's included, may need the modern
    // fonts and chat in UTF-8.
    namespace languages = oa::data::languages;
    const languages::Language& system =
        dialog.system_language != nullptr ? *dialog.system_language : languages::english();
    const languages::Language& chosen = languages::chosen_language(dialog.chosen.language, system);
    if (locks.modern_fonts == Lock::none && chosen.needs == languages::TextNeeds::modern_fonts)
        locks.modern_fonts = Lock::set_by_language;
    if (locks.unicode_chat == Lock::none &&
        std::find(
            dialog.unicode_chat_languages.begin(), dialog.unicode_chat_languages.end(), chosen.tag
        ) != dialog.unicode_chat_languages.end())
        locks.unicode_chat = Lock::set_by_language;
    const bool modern_fonts =
        dialog.chosen.modern_fonts || locks.modern_fonts == Lock::set_by_language;
    if (locks.text_size == Lock::none && !modern_fonts)
        locks.text_size = Lock::needs_modern_fonts;
    // After zoom says where units turn to dots; drawn whole, they never do.
    if (locks.zoomed_out_after == Lock::none &&
        dialog.chosen.zoomed_out_units != ZoomedOutUnits::dots)
        locks.zoomed_out_after = Lock::needs_dots;
    if (locks.water_effects == Lock::none &&
        dialog.chosen.hardware_acceleration != HardwareAcceleration::full)
        locks.water_effects = Lock::needs_full_acceleration;
    if (locks.unlocked_camera == Lock::none && !dialog.chosen.allow_full_3d)
        locks.unlocked_camera = Lock::needs_full_3d;
    // Only a sticky edge has a firmness: Capture never lets the pointer go,
    // and Free never holds it.
    if (locks.edge_stickiness == Lock::none && dialog.chosen.window_edge != WindowEdge::sticky)
        locks.edge_stickiness = Lock::needs_sticky;
    return locks;
}

/// Grows the section list by a few columns and moves the open section right
/// by the same, leaving the dialog's right edge where it is.
///
/// @param[in,out] sized the sizes
/// @param extra the columns the list gains
void grow_list(Sizes& sized, int32_t extra) {
    if (extra <= 0)
        return;
    sized.list_width += extra;
    sized.list_item_width += extra;
    sized.list_rule_column += extra;
    sized.content_left += extra;
    sized.content_width -= extra;
    sized.heading.x += extra;
    sized.heading.width -= extra;
    sized.view.x += extra;
    sized.view.width -= extra;
    sized.view_clip.x += extra;
    sized.view_clip.width -= extra;
    sized.section_scroll.view = sized.view;
    sized.developer_view.x += extra;
    sized.developer_view.width -= extra;
    sized.developer_view_clip.x += extra;
    sized.developer_view_clip.width -= extra;
    sized.active_only_label.x += extra;
    sized.active_only_label.width -= extra;
    sized.restore_profile_button.x += extra;
    sized.developer_scroll.view = sized.developer_view;
}

/// Grows a button toward a column it must stay short of.
///
/// @param[in,out] button the button
/// @param text_width the caption's width
/// @param stop the column the button's right must stay short of
void grow_button(SourceRect& button, int32_t text_width, int32_t stop) {
    const int32_t wanted = text_width + 8;
    if (wanted <= button.width)
        return;
    const int32_t room = stop - oa::ui::kit::compact_metrics.button_gap - button.x;
    button.width = std::min(wanted, std::max(room, button.width));
}

Sizes fitted_sizes(const Dialog& dialog, const TextWidth* regular, const TextWidth* small) {
    Sizes sized = sizes_of(dialog);
    if (regular == nullptr || small == nullptr || estimated_measure(*regular) ||
        estimated_measure(*small))
        return sized;
    int32_t widest = 0;
    for (const Page page :
         dialog_pages(dialog.kind, dialog.touch, dialog.game_files, dialog.controller)) {
        widest = std::max(widest, (*regular)(std::string(shown_text(page_name(page)))));
    }
    int32_t text_room = sized.list_item_width - sized.nav.text_left - list_text_margin;
    if (widest > text_room) {
        // A section name that does not fit beside the icons takes their room:
        // the list then draws its names without icons, from the icons' column.
        sized.nav.text_left = sized.nav.icon_left;
        sized.nav.icon_side = 0;
        text_room = sized.list_item_width - sized.nav.text_left - list_text_margin;
    }
    if (widest > text_room) {
        // The open section keeps room for a description. A section name that
        // still does not fit the list takes a shorter word.
        const int32_t least_content = 270;
        const int32_t spare = std::max(sized.content_width - least_content, int32_t{0});
        grow_list(sized, std::min(widest - text_room, spare));
    }
    // The footer's captions are drawn in the regular font, with room each
    // side. OK and Cancel grow to the left, Restore defaults to the right,
    // so that a longer word is whole; the count of changes keeps the room
    // between them.
    const int32_t space = kit::metrics_of(dialog.size_class).space;
    const auto footer_width = [&](std::string_view caption) {
        return (*regular)(std::string(shown_text(caption))) + 4 * space;
    };
    const auto restore_width = [&](std::string_view caption) {
        return (*small)(std::string(shown_text(caption))) + 4 * space;
    };
    const auto grow_left = [](SourceRect& button, int32_t wanted, int32_t stop) {
        const int32_t room = std::max(button.x - stop, int32_t{0});
        const int32_t take = std::min(std::max(wanted - button.width, int32_t{0}), room);
        button.x -= take;
        button.width += take;
        return take;
    };
    const int32_t ok_grew = grow_left(
        sized.ok_button,
        footer_width(ok_text),
        sized.restore_button.x + sized.restore_button.width + sized.cancel_button.width
    );
    sized.cancel_button.x -= ok_grew;
    grow_left(
        sized.cancel_button,
        footer_width(cancel_text),
        sized.restore_button.x + sized.restore_button.width + 2 * space
    );
    const int32_t restore_wanted = restore_width(restore_text);
    if (restore_wanted > sized.restore_button.width)
        sized.restore_button.width = std::min(
            restore_wanted,
            std::max(
                sized.cancel_button.x - 2 * space - sized.restore_button.x,
                sized.restore_button.width
            )
        );
    const int32_t count_left = sized.restore_button.x + sized.restore_button.width + 3 * space;
    sized.changed_count.x = count_left;
    sized.changed_count.width =
        std::max(sized.cancel_button.x - 3 * space - count_left, int32_t{0});
    grow_button(
        sized.restore_profile_button,
        (*small)(std::string(shown_text(restore_profile_text))),
        sized.content_right
    );
    const int32_t half = std::max(
        (*small)(std::string(shown_text(off_text))), (*small)(std::string(shown_text(on_text)))
    );
    const int32_t switch_columns = oa::ui::kit::switch_width_for(half);
    if (switch_columns > sized.active_only_switch.width) {
        const int32_t grow = switch_columns - sized.active_only_switch.width;
        sized.active_only_switch.width = switch_columns;
        sized.active_only_switch.x -= grow;
        sized.active_only_label.width = std::max(sized.active_only_label.width - grow, int32_t{0});
    }
    return sized;
}

ScrolledRows open_rows(
    const Dialog& dialog,
    const TextWidth* small_width,
    const Sizes* sizes,
    const TextWidth* regular_width
) {
    const Sizes& sized = sizes != nullptr ? *sizes : sizes_of(dialog);
    const TextWidth& measure = small_width != nullptr ? *small_width : estimated_width;
    ScrolledRows open{};
    open.area = sized.section_scroll;
    open.rows = place_section(
        dialog,
        dialog.page,
        shown_locks(dialog),
        dialog.section_hooks,
        measure,
        nullptr,
        &sized,
        regular_width
    );
    if (developer_page(dialog)) {
        // Developer's rows stay at its top; its list scrolls under them in a
        // view of its own, with the end gap under its last row.
        open.area = developer_list_scroll(dialog, small_width, &sized, regular_width);
        int32_t switch_columns = 0;
        if (small_width != nullptr && !estimated_measure(*small_width)) {
            const int32_t half = std::max(
                (*small_width)(std::string(shown_text(off_text))),
                (*small_width)(std::string(shown_text(on_text)))
            );
            switch_columns = oa::ui::kit::switch_width_for(half);
        }
        const TextWidth* measured_small =
            small_width != nullptr && !estimated_measure(*small_width) ? small_width : nullptr;
        const TextWidth* measured_regular =
            regular_width != nullptr && !estimated_measure(*regular_width) ? regular_width
                                                                           : nullptr;
        open.list = place_list(dialog, 0, switch_columns, measured_small, measured_regular, &sized);
        if (!developer_scrolls_as_one(dialog)) {
            open.content_height = open.list.bottom + end_gap - open.area.view.y;
            open.limit = std::max(open.content_height - open.area.view.height, int32_t{0});
            open.scroll =
                std::clamp(dialog.scroll[scroll_index(dialog.page)], int32_t{0}, open.limit);
            scroll_list(open.list, open.scroll);
            open.footer_rule = sized.developer_footer_rule;
            open.active_only_label = sized.active_only_label;
            open.active_only_switch = sized.active_only_switch;
            open.restore_profile_button = sized.restore_profile_button;
            return open;
        }
        // Scrolling as one: the footer follows the list, its line where the
        // list ends, and the rows, the list and the footer move together.
        open.footer_rule = open.list.bottom;
        const int32_t label_top = open.footer_rule + developer_footer_gap;
        open.active_only_label = sized.active_only_label;
        open.active_only_label.y = label_top;
        open.active_only_switch = sized.active_only_switch;
        open.active_only_switch.y = label_top;
        open.restore_profile_button = sized.restore_profile_button;
        open.restore_profile_button.y = label_top + label_line_height + developer_footer_gap;
        open.content_height = open.restore_profile_button.y + open.restore_profile_button.height +
                              end_gap - open.area.view.y;
        open.limit = std::max(open.content_height - open.area.view.height, int32_t{0});
        open.scroll = std::clamp(dialog.scroll[scroll_index(dialog.page)], int32_t{0}, open.limit);
        scroll_developer(open, open.scroll);
        return open;
    }
    open.content_height = content_height(open.rows, 0, sized.first_row_top);
    open.limit = scroll_limit(open.content_height, dialog.size_class);
    // While the dialog's words are drawn in the modern fonts, whose
    // ideographs fill a hint line from its top row, the view's top edge
    // cuts no hint line at the end of the scroll, of which a sliver would
    // show under it: the section scrolls on until the line has passed the
    // edge, its end gap that much taller.
    const bool tall = oa::data::languages::interface_language().needs !=
                      oa::data::languages::TextNeeds::game_fonts;
    if (open.limit > 0 && tall) {
        const int32_t view_top = open.area.view.y;
        for (bool moved = true; moved;) {
            moved = false;
            for (const Row& row : open.rows.rows)
                for (std::size_t line = 0; line < row.hint_lines; ++line) {
                    const SourceRect& box = row.hints[line];
                    const int32_t top = box.y - open.limit;
                    if (top <= view_top && top + box.height > view_top) {
                        open.limit += top + box.height - view_top;
                        moved = true;
                    }
                }
        }
        open.content_height = open.limit + sized.view.height;
    }
    open.scroll = std::clamp(dialog.scroll[scroll_index(dialog.page)], int32_t{0}, open.limit);
    scroll_rows(open.rows, open.scroll);
    return open;
}

NavBar nav_bar(const NavLayout& layout, const Sizes& sized, int32_t scroll) noexcept {
    NavBar bar;
    if (layout.limit <= 0 || layout.viewport.height <= 0)
        return bar;
    constexpr int32_t width = 2;
    constexpr int32_t least = 12;
    const int32_t entries_right = list_item_left + sized.list_item_width;
    const int32_t x = entries_right + (sized.list_rule_column - entries_right - width) / 2;
    bar.track = {x, layout.viewport.y, width, layout.viewport.height};
    const int32_t whole = layout.viewport.height + layout.limit;
    const int32_t height = std::max(least, layout.viewport.height * layout.viewport.height / whole);
    const int32_t travel = layout.viewport.height - height;
    const int32_t at = std::clamp(scroll, int32_t{0}, layout.limit);
    bar.thumb = {
        x, layout.viewport.y + (travel * at + layout.limit / 2) / layout.limit, width, height
    };
    return bar;
}

ScrollArea developer_list_scroll(
    const Dialog& dialog,
    const TextWidth* small_width,
    const Sizes* sizes,
    const TextWidth* regular_width
) {
    const Sizes& sized = sizes != nullptr ? *sizes : sizes_of(dialog);
    if (developer_scrolls_as_one(dialog))
        return sized.section_scroll;
    const TextWidth& measure = small_width != nullptr ? *small_width : estimated_width;
    ScrollArea area = sized.developer_scroll;
    const Rows rows = place_section(
        dialog,
        Page::developer,
        shown_locks(dialog),
        dialog.section_hooks,
        measure,
        nullptr,
        &sized,
        regular_width
    );
    const int32_t lower = std::clamp(
        rows.bottom - sized.developer_list_rule, int32_t{0}, sized.developer_view.height
    );
    for (SourceRect* rect : {&area.view, &area.well, &area.hit}) {
        rect->y += lower;
        rect->height -= lower;
    }
    area.page_step = std::max(area.page_step - lower, hint_line_height);
    return area;
}

int32_t developer_list_top(
    const Dialog& dialog,
    const TextWidth* small_width,
    const Sizes* sizes,
    const TextWidth* regular_width
) {
    const Sizes& sized = sizes != nullptr ? *sizes : sizes_of(dialog);
    if (!developer_scrolls_as_one(dialog))
        return developer_list_scroll(dialog, small_width, &sized, regular_width).view.y;
    const Rows rows = place_section(
        dialog,
        Page::developer,
        shown_locks(dialog),
        dialog.section_hooks,
        small_width != nullptr ? *small_width : estimated_width,
        nullptr,
        &sized,
        regular_width
    );
    return rows.bottom + 1;
}

void scroll_developer(ScrolledRows& open, int32_t by) noexcept {
    scroll_rows(open.rows, by);
    scroll_list(open.list, by);
    open.footer_rule -= by;
    for (SourceRect* rect :
         {&open.active_only_label, &open.active_only_switch, &open.restore_profile_button})
        rect->y -= by;
}

int32_t scroll_showing(const ScrolledRows& open, std::size_t index) noexcept {
    if (index >= open.rows.rows.size())
        return open.scroll;
    const Row& row = open.rows.rows[index];
    // The row's line, at the section's top, may come up to the view's first
    // row; the line under it, or the end gap under the last row, down to its
    // last. A row taller than the view would show its top.
    const SourceRect& seen = open.area.view;
    const int32_t line = row.top + open.scroll;
    const int32_t highest = line - seen.y;
    const int32_t lowest = index + 1 == open.rows.rows.size()
                               ? open.limit
                               : line + row.height - (seen.y + seen.height - 1);
    const int32_t scroll = std::min(std::max(open.scroll, lowest), highest);
    return std::clamp(scroll, int32_t{0}, open.limit);
}

SourceRect scroll_thumb(int32_t scroll, int32_t limit, int32_t content_height) noexcept {
    return scroll_thumb(section_scroll, scroll, limit, content_height);
}

SourceRect scroll_thumb(
    const ScrollArea& area, int32_t scroll, int32_t limit, int32_t content_height
) noexcept {
    const SourceRect inside{
        area.well.x + 1, area.well.y + 1, area.well.width - 2, area.well.height - 2
    };
    int32_t height = inside.height;
    if (content_height > area.view.height)
        height = std::max(least_thumb_height, inside.height * area.view.height / content_height);
    const int32_t travel = inside.height - height;
    int32_t top = inside.y;
    if (limit > 0)
        top += (travel * std::clamp(scroll, int32_t{0}, limit) + limit / 2) / limit;
    return {inside.x, top, inside.width, height};
}

int32_t scroll_at(int32_t thumb_top, int32_t limit, int32_t content_height) noexcept {
    return scroll_at(section_scroll, thumb_top, limit, content_height);
}

int32_t scroll_at(
    const ScrollArea& area, int32_t thumb_top, int32_t limit, int32_t content_height
) noexcept {
    const SourceRect thumb = scroll_thumb(area, 0, limit, content_height);
    const int32_t travel = area.well.height - 2 - thumb.height;
    if (travel <= 0 || limit <= 0)
        return 0;
    const int32_t along = std::clamp(thumb_top - thumb.y, int32_t{0}, travel);
    return (limit * along + travel / 2) / travel;
}

NavGroup nav_group(Page page) noexcept {
    switch (page) {
    case Page::controls:
    case Page::accessibility:
    case Page::touch:
    case Page::controller:
        return NavGroup::play;
    case Page::graphics:
    case Page::user_interface:
    case Page::language:
        return NavGroup::look;
    case Page::game_files:
    case Page::downloads:
    case Page::updates:
    case Page::game_content:
        return NavGroup::content;
    case Page::common_tweaks:
    case Page::system:
    case Page::developer:
        return NavGroup::system;
    case Page::mod_keys:
    case Page::mod_patrol:
    case Page::mod_guard:
    case Page::mod_tools:
    case Page::mod_chat:
    case Page::registered:
        break;
    }
    return NavGroup::play;
}

NavLayout nav_layout(const Dialog& dialog, const Sizes* sizes) {
    const Sizes& sized = sizes != nullptr ? *sizes : sizes_of(dialog);
    const NavMetrics& nav = sized.nav;
    NavLayout layout;
    layout.area = {edge, sized.body_top, sized.list_width, sized.footer_rule_row - sized.body_top};
    const int32_t first_top = sized.body_top + nav.margin;
    const int32_t view_bottom = sized.footer_rule_row - nav.margin;
    layout.viewport = {list_item_left, first_top, sized.list_item_width, view_bottom - first_top};
    const SourceRect rule_box{
        list_item_left + list_divider_inset, 0, sized.list_item_width - 2 * list_divider_inset, 1
    };
    int32_t cursor = first_top;
    const auto add_rule = [&] {
        cursor += nav.rule_margin;
        NavPiece rule;
        rule.kind = NavPiece::Kind::rule;
        rule.rect = rule_box;
        rule.rect.y = cursor;
        layout.pieces.push_back(std::move(rule));
        cursor += 1 + nav.rule_margin;
    };
    const auto add_entry = [&](NavPiece entry) {
        entry.kind = NavPiece::Kind::entry;
        entry.rect = {list_item_left, cursor, sized.list_item_width, nav.entry_height};
        layout.pieces.push_back(std::move(entry));
        cursor += nav.entry_height;
    };
    bool any = false;
    NavGroup group = NavGroup::play;
    for (const Page page :
         dialog_pages(dialog.kind, dialog.touch, dialog.game_files, dialog.controller)) {
        if (any && nav_group(page) != group)
            add_rule();
        else if (any)
            cursor += nav.entry_gap;
        any = true;
        group = nav_group(page);
        NavPiece entry;
        entry.control = page_control(page);
        entry.text = std::string(shown_text(page_name(page)));
        entry.page = page;
        add_entry(std::move(entry));
    }
    // Registered pages, only in the engine's settings: each owner on a rule
    // of its own, named, over its pages.
    const int32_t line = kit::metrics_of(dialog.size_class).small_line;
    std::size_t index = 0;
    while (dialog.kind == DialogKind::engine && index < dialog.registered_pages.size()) {
        const std::string owner = dialog.registered_pages[index].owner;
        add_rule();
        NavPiece label;
        label.kind = NavPiece::Kind::owner;
        label.rect = {
            list_item_left + nav.text_left,
            cursor,
            sized.list_item_width - nav.text_left - list_text_margin,
            line,
        };
        label.text = owner;
        layout.pieces.push_back(std::move(label));
        cursor += line;
        while (index < dialog.registered_pages.size() &&
               dialog.registered_pages[index].owner == owner) {
            cursor += nav.entry_gap;
            NavPiece entry;
            entry.control = registered_nav_control(index);
            entry.text = dialog.registered_pages[index].title;
            entry.registered = true;
            entry.index = index;
            add_entry(std::move(entry));
            ++index;
        }
    }
    layout.limit = std::max(int32_t{0}, cursor - view_bottom);
    return layout;
}

SourceRect
list_item(Page page, bool touch, bool game_files, bool controller, kit::SizeClass size_class) {
    Dialog shown;
    shown.kind = static_cast<int32_t>(page) >= static_cast<int32_t>(Page::mod_keys)
                     ? DialogKind::mod_options
                     : DialogKind::engine;
    shown.touch = touch;
    shown.game_files = game_files;
    shown.controller = controller;
    shown.size_class = size_class;
    const NavLayout layout = nav_layout(shown);
    for (const NavPiece& piece : layout.pieces)
        if (piece.kind == NavPiece::Kind::entry && !piece.registered && piece.page == page)
            return piece.rect;
    return {list_item_left, layout.viewport.y, 0, 0};
}

SourceRect footer_button(int32_t control, kit::SizeClass size_class) noexcept {
    const Sizes& sized = sizes_of(size_class);
    if (control == restore_control)
        return sized.restore_button;
    if (control == cancel_control)
        return sized.cancel_button;
    return sized.ok_button;
}

EngineSettings slider_settings(const Dialog& dialog) {
    EngineSettings shown = dialog.chosen;
    if (dialog.window_screen_size)
        shown.screen_size = *dialog.window_screen_size;
    return shown;
}

std::vector<std::string>
break_lines(std::string_view text, std::size_t characters, std::size_t most_lines) {
    // The mark between a location's folders: "Open Annihilation › Total
    // Annihilation".
    constexpr std::string_view location_mark = "›";
    std::vector<std::string> lines;
    characters = std::max<std::size_t>(characters, 1);
    // A line holds at most `characters` whole characters, so that no line
    // cuts one.
    const auto leading_bytes = [](std::string_view part, std::size_t most) {
        std::size_t bytes = 0;
        for (std::size_t count = 0; count < most && bytes < part.size(); ++count)
            bytes += oa::base::text::break_character(part.substr(bytes)).bytes;
        return bytes;
    };
    const auto fits = [&](std::string_view start) {
        return leading_bytes(start, characters) == start.size();
    };
    while (!text.empty() && lines.size() < most_lines) {
        while (!text.empty() && text.front() == ' ')
            text.remove_prefix(1);
        if (text.empty())
            break;
        // The line breaks where the line breaker lets it, as every wrap
        // does (oa::base::text::first_row): at a space, or between Chinese
        // or Japanese characters; a word longer than a line is cut at the
        // line's end. A line that does not hold the rest breaks after a
        // location's last separator instead when that stands in its second
        // half, which keeps the folders' names whole.
        auto row = oa::base::text::first_row(text, fits);
        if (row.next < text.size()) {
            const std::size_t span = leading_bytes(text, characters);
            std::size_t separator = std::string_view::npos;
            for (std::size_t at = location_mark.size(); at < span; ++at)
                if (text[at] == ' ' &&
                    text.substr(at - location_mark.size(), location_mark.size()) == location_mark)
                    separator = at;
            if (separator != std::string_view::npos && separator >= span / 2)
                row = {separator, separator};
        }
        std::string_view line = text.substr(0, row.bytes);
        while (!line.empty() && line.back() == ' ')
            line.remove_suffix(1);
        lines.emplace_back(line);
        text.remove_prefix(row.next);
    }
    return lines;
}

std::vector<std::string>
changed_count_lines(const Dialog& dialog, const TextWidth& measure, int32_t room) {
    const std::size_t count = changed_settings_count(dialog);
    if (count == 0)
        return {};
    const std::string words = count == 1
                                  ? std::string(shown_text(one_changed_text))
                                  : filled(many_changed_text, {{"count", std::to_string(count)}});
    if (measure(words) <= room || room <= 0)
        return {words};
    kit::WrapRules rules;
    rules.most_lines = 2;
    return kit::wrap(words, room, measure, rules);
}

SourceRect dialog_list_item(const Dialog& dialog, Page page) {
    const NavLayout layout = nav_layout(dialog);
    const int32_t scroll = std::clamp(dialog.nav_scroll, int32_t{0}, layout.limit);
    for (const NavPiece& piece : layout.pieces) {
        if (piece.kind != NavPiece::Kind::entry || piece.registered || piece.page != page)
            continue;
        SourceRect rect = piece.rect;
        rect.y -= scroll;
        return rect;
    }
    return {};
}

void reveal_engine_nav(Dialog& dialog) {
    const NavLayout layout = nav_layout(dialog);
    if (layout.limit == 0) {
        dialog.nav_scroll = 0;
        return;
    }
    if (dialog.pressed != no_control)
        return;
    // The focused entry, or with no focus the open section's; a row's focus
    // leaves the list where it is.
    const NavPiece* shown = nullptr;
    for (const NavPiece& piece : layout.pieces) {
        if (piece.kind != NavPiece::Kind::entry)
            continue;
        const bool open = piece.registered ? dialog.page == Page::registered &&
                                                 dialog.registered_page == piece.index
                                           : dialog.page == piece.page;
        if (dialog.focused == no_control ? open : piece.control == dialog.focused)
            shown = &piece;
    }
    if (shown == nullptr)
        return;
    int32_t scroll = std::clamp(dialog.nav_scroll, int32_t{0}, layout.limit);
    const int32_t top = shown->rect.y - scroll;
    const int32_t bottom = top + shown->rect.height;
    const int32_t view_top = layout.viewport.y;
    const int32_t view_bottom = layout.viewport.y + layout.viewport.height;
    if (top < view_top)
        scroll -= view_top - top;
    else if (bottom > view_bottom)
        scroll += bottom - view_bottom;
    dialog.nav_scroll = std::clamp(scroll, int32_t{0}, layout.limit);
}

std::string_view lock_text(Lock lock) noexcept {
    switch (lock) {
    case Lock::none:
        return {};
    case Lock::in_game:
        return "Locked during a game";
    case Lock::set_by_host:
        return "Set by the host";
    case Lock::command_line:
        return "Set on the command line";
    case Lock::unavailable:
        return "Not available here";
    case Lock::set_by_mod:
        return "Set by the mod";
    case Lock::needs_modern_fonts:
        return "Needs modern fonts";
    case Lock::always_on:
        return "Always on here";
    case Lock::set_by_language:
        return "Set by the language";
    case Lock::needs_dots:
        return "Needs Dots";
    case Lock::needs_sticky:
        return "Needs Sticky";
    case Lock::needs_full_acceleration:
        return "Needs Full acceleration";
    case Lock::needs_full_3d:
        return "Needs Allow full 3D mode";
    case Lock::main_menu:
        return "Choose from the main menu.";
    }
    return {};
}

} // namespace geometry

std::string key_layout_summary_text(std::string_view name, int32_t changes) {
    if (changes <= 0)
        return std::string(name);
    if (changes == 1)
        return geometry::filled("{name} · 1 change of your own", {{"name", name}});
    return geometry::filled(
        "{name} · {n} changes of your own", {{"name", name}, {"n", std::to_string(changes)}}
    );
}

namespace layout = geometry;

bool layout::list_row_takes_input(const layout::ListRow& row) noexcept {
    if (row.control == no_control)
        return false;
    return row.kind == layout::ListRowKind::area || row.kind == layout::ListRowKind::hack ||
           !row.locked;
}

namespace {

/// Tells whether a rectangle lies wholly in another.
///
/// @param rect the rectangle
/// @param outer the other
/// @return true when no part of it lies outside
bool wholly_in(const layout::SourceRect& rect, const layout::SourceRect& outer) noexcept {
    return rect.x >= outer.x && rect.y >= outer.y && rect.x + rect.width <= outer.x + outer.width &&
           rect.y + rect.height <= outer.y + outer.height;
}

/// Returns the row a control is, when it is one of the open section's.
///
/// @param rows the open section's rows
/// @param control the control
/// @return the row; nullptr for a control that is not a row's
const layout::Row* row_of(const layout::Rows& rows, int32_t control) noexcept {
    const int32_t index = control - first_row_control;
    if (index < 0 || static_cast<std::size_t>(index) >= rows.rows.size())
        return nullptr;
    return &rows.rows[static_cast<std::size_t>(index)];
}

/// Returns the open section's rows where the dialog draws them: in the fonts
/// it is drawn in (Dialog::drawn_fonts), at the sizes they widen.
///
/// @param dialog the dialog
/// @return its rows, offset, limit and content height (layout::shown_section)
layout::ScrolledRows drawn_rows(const Dialog& dialog) {
    return layout::shown_section(dialog, dialog.drawn_fonts).open;
}

/// Returns the control a press at a point acts on: the first of the
/// dialog's display list's controls that a press reaches there (kit::hit),
/// the list the dialog is drawn from. While a drop-down list is open its
/// items come first.
///
/// @param dialog the dialog
/// @param x the point's column
/// @param y the point's row
/// @return the control; no_control when none is there
int32_t control_under(const Dialog& dialog, int32_t x, int32_t y) {
    return kit::hit(
        layout::dialog_list(dialog, dialog.drawn_fonts), {x, y}, kit::DisabledControl::passes
    );
}

/// Notes where the pointer is, so that the hover can follow the rows a
/// scroll moves under it.
///
/// @param[in,out] dialog the dialog
/// @param x the pointer's column
/// @param y the pointer's row
void note_pointer(Dialog& dialog, int32_t x, int32_t y) noexcept {
    dialog.pointer_known = true;
    dialog.pointer_x = x;
    dialog.pointer_y = y;
}

/// Finds the control under the last pointer point again, after a scroll
/// moved the rows under it; a held press keeps its hover.
///
/// @param[in,out] dialog the dialog, at the offset the scroll left
void hover_again(Dialog& dialog) {
    if (!dialog.pointer_known || dialog.pressed != no_control)
        return;
    dialog.hovered = control_under(dialog, dialog.pointer_x, dialog.pointer_y);
}

/// Scrolls the open section to an offset, moving its placed rows with it;
/// on Developer, its list alone. No scroll changes a setting or moves the
/// focus.
///
/// @param[in,out] dialog the dialog
/// @param[in,out] open the open section's rows (layout::open_rows), left at
///     the new offset
/// @param offset the offset, clamped to the section's limit
/// @return DialogAction::redraw when the section moved, else DialogAction::none
DialogAction scroll_to(Dialog& dialog, layout::ScrolledRows& open, int32_t offset) {
    const int32_t next = std::clamp(offset, int32_t{0}, open.limit);
    dialog.scroll[layout::scroll_index(dialog.page)] = next;
    if (next == open.scroll)
        return DialogAction::none;
    if (layout::developer_scrolls_as_one(dialog))
        layout::scroll_developer(open, next - open.scroll);
    else {
        if (!layout::developer_page(dialog))
            layout::scroll_rows(open.rows, next - open.scroll);
        layout::scroll_list(open.list, next - open.scroll);
    }
    open.scroll = next;
    hover_again(dialog);
    return DialogAction::redraw;
}

/// Scrolls the least that shows a row whole, when a control is one of the
/// open section's rows; nothing moves while a press is held.
///
/// @param[in,out] dialog the dialog
/// @param[in,out] open the open section's rows, left at the offset shown
/// @param control the control
/// @return DialogAction::redraw when the section moved, else DialogAction::none
DialogAction show_row(Dialog& dialog, layout::ScrolledRows& open, int32_t control) {
    if (dialog.pressed != no_control)
        return DialogAction::none;
    // Change layout… is the key layout card's, the section's first row; the
    // update card's other buttons are Updates' first row's.
    if ((control == change_layout_control || updates::card_button(control).has_value()) &&
        !open.rows.rows.empty())
        return scroll_to(dialog, open, layout::scroll_showing(open, 0));
    // The practice pad's button shows the pad's foot.
    if (control == helpers_off_control && open.rows.pad.width > 0) {
        const layout::SourceRect& seen = open.area.view;
        const int32_t foot = open.rows.pad.y + open.rows.pad.height + open.scroll;
        const int32_t lowest = foot - (seen.y + seen.height - 1);
        return scroll_to(dialog, open, std::clamp(std::max(open.scroll, lowest), 0, open.limit));
    }
    if (control < first_row_control)
        return DialogAction::none;
    // Where Developer scrolls as one, a row, a row of its list, Show Active
    // Only and Restore profile values each scroll the least that shows them
    // whole, the line over a row included.
    if (layout::developer_scrolls_as_one(dialog)) {
        layout::SourceRect shown{};
        if (control == active_only_control)
            shown = open.active_only_switch;
        else if (control == restore_profile_control)
            shown = open.restore_profile_button;
        else if (const layout::ListRow* item = layout::list_row(open.list, control))
            shown = {0, item->top, 1, item->height};
        else if (const layout::Row* row = row_of(open.rows, control))
            shown = {0, row->top, 1, row->height + 1};
        if (shown.height <= 0)
            return DialogAction::none;
        const layout::SourceRect& seen = open.area.view;
        int32_t next = open.scroll;
        if (shown.y + shown.height > seen.y + seen.height)
            next += shown.y + shown.height - (seen.y + seen.height);
        if (shown.y < seen.y)
            next -= seen.y - shown.y;
        return scroll_to(dialog, open, next);
    }
    // Developer's rows and its footer's switch and button stay where they
    // are; its list's rows scroll.
    if (layout::developer_page(dialog))
        return control < first_hack_list_control
                   ? DialogAction::none
                   : scroll_to(dialog, open, layout::list_scroll_showing(open, control));
    return scroll_to(
        dialog,
        open,
        layout::scroll_showing(open, static_cast<std::size_t>(control - first_row_control))
    );
}

/// Moves the focus to the next or previous control in the display list's
/// declared order (kit::next_in_tab_order), round from one end to the
/// other, and scrolls its row into view when it is a row. From a control
/// not in the order, forward goes to the first and back to the last.
///
/// @param[in,out] dialog the dialog
/// @param[in,out] open the open section's rows, left at the offset shown
/// @param forward true for the next control, false for the previous
/// @return DialogAction::redraw
DialogAction move_in_tab_order(Dialog& dialog, layout::ScrolledRows& open, bool forward) {
    const int32_t next = kit::next_in_tab_order(
        layout::dialog_list(dialog, dialog.drawn_fonts), dialog.focused, forward
    );
    if (next == no_control)
        return DialogAction::none;
    dialog.focused = next;
    static_cast<void>(show_row(dialog, open, dialog.focused));
    layout::reveal_engine_nav(dialog);
    return DialogAction::redraw;
}

/// Moves the focus to the control an arrow points at, by where the
/// display list's controls lie (kit::focus_toward): within the focused
/// control's scroll area first, the open section's rows or Developer's
/// list, rows it does not show included; then among the controls
/// that show. A row it moves onto is scrolled into view (show_row). From no
/// focus, or a focus on no control the dialog shows, Up shows the focus on
/// the last control in the declared order and the other arrows on the
/// first.
///
/// @param[in,out] dialog the dialog
/// @param[in,out] open the open section's rows, left at the offset shown
/// @param direction the arrow's direction
/// @return DialogAction::redraw when the focus moved, else DialogAction::none
DialogAction move_toward(Dialog& dialog, layout::ScrolledRows& open, kit::Direction direction) {
    const kit::DisplayList list = layout::dialog_list(dialog, dialog.drawn_fonts);
    const int32_t next =
        kit::control_of(list, dialog.focused) == nullptr
            ? kit::next_in_tab_order(list, no_control, direction != kit::Direction::up)
            : kit::focus_toward(list, dialog.focused, direction);
    if (next == no_control)
        return DialogAction::none;
    dialog.focused = next;
    static_cast<void>(show_row(dialog, open, dialog.focused));
    layout::reveal_engine_nav(dialog);
    return DialogAction::redraw;
}

/// Scrolls the open section for Page Up, Page Down, Home or End, whatever
/// has the focus; nothing moves while a press is held.
///
/// @param[in,out] dialog the dialog
/// @param[in,out] open the open section's rows, left at the new offset
/// @param key the key
/// @return DialogAction::redraw when the section moved, else DialogAction::none
DialogAction scroll_key(Dialog& dialog, layout::ScrolledRows& open, DialogKey key) {
    if (dialog.pressed != no_control)
        return DialogAction::none;
    int32_t next = open.scroll;
    if (key == DialogKey::page_up)
        next -= open.area.page_step;
    else if (key == DialogKey::page_down)
        next += open.area.page_step;
    else if (key == DialogKey::home)
        next = 0;
    else
        next = open.limit;
    return scroll_to(dialog, open, next);
}

/// Reports a change of the chosen settings, or only a look's. Hardware
/// acceleration passing to a higher level, from Off to Basic or Full or
/// from Basic to Full, asks for the graphics card to be tried afresh, and
/// leaving Full turns the four effects off and reaching it sets each to its
/// own default there (follow_acceleration_with_effects).
///
/// @param[in,out] dialog the dialog, its chosen settings after the event
/// @param before the chosen settings before the event
/// @return DialogAction::changed when they differ, else DialogAction::redraw
DialogAction changed_or_redraw(Dialog& dialog, const EngineSettings& before) noexcept {
    if (dialog.chosen.hardware_acceleration > before.hardware_acceleration)
        ++dialog.forget_renderer_failures;
    follow_acceleration_with_effects(dialog.chosen, before.hardware_acceleration);
    return before == dialog.chosen ? DialogAction::redraw : DialogAction::changed;
}

/// Returns the model of the settings the dialog shows, which its rows change.
///
/// @param[in,out] dialog the dialog
/// @return the model of its chosen settings
layout::SettingsModel chosen_model(Dialog& dialog) noexcept {
    return {&dialog.chosen, &dialog};
}

/// Returns a row as the kit's events read it: its control and the width of
/// its strip's levels.
///
/// @param row the row, placed
/// @return the kit's placed row
kit::PlacedRow placed_row(const layout::Row& row) {
    kit::PlacedRow placed;
    placed.control = row.control;
    placed.control_area = row.control_area;
    placed.view.kind = layout::kind_of(row.setting);
    placed.view.control_width = layout::row_spec(row.setting).control_width;
    return placed;
}

/// Notes that a slider's knob moved: Screen size shows the size chosen from
/// then on, no longer the window's own.
///
/// @param[in,out] dialog the dialog
/// @param setting the slider's setting
void knob_moved(Dialog& dialog, Setting setting) noexcept {
    if (setting == Setting::screen_size)
        dialog.window_screen_size.reset();
}

/// Moves a row's control one step down or up through its row spec: a
/// switch to Off or On, a level strip one level, a drop-down one choice, as
/// the kit steps them; a slider to the stop next to the one its value is
/// nearest, held to its ends, so that a value between two stops at an end
/// moves onto the end stop.
///
/// @param[in,out] dialog the dialog, whose chosen settings change
/// @param setting the row's setting
/// @param up true for a step up
void step(Dialog& dialog, Setting setting, bool up) {
    const auto& spec = layout::row_spec(setting);
    layout::SettingsModel model = chosen_model(dialog);
    // Left and Right stay among System default and the installed languages.
    // An available pack is chosen only from the open list.
    if (setting == Setting::language) {
        const int32_t installed = layout::language_installed_count();
        const int32_t current = kit::row_index(spec, model);
        if (current >= installed) {
            if (up || installed <= 0)
                return;
            layout::set_language(model, installed - 1);
            return;
        }
        const int32_t next = current + (up ? 1 : -1);
        if (next < 0 || next >= installed)
            return;
        layout::set_language(model, next);
        return;
    }
    if (spec.kind != kit::RowKind::slider) {
        static_cast<void>(kit::step(spec, model, up));
        return;
    }
    kit::set_row_index(spec, model, kit::row_index(spec, model) + (up ? 1 : -1));
    knob_moved(dialog, setting);
}

/// An open drop-down list, as it lies now.
struct OpenList {
    const layout::Row* row{};  ///< the drop-down's row
    layout::SourceRect rect{}; ///< the list, its border included
    std::size_t choices{};     ///< the items it offers
    int32_t shown{};           ///< the items it shows at once
};

/// Returns the open list, placed by its row.
///
/// @param dialog the dialog
/// @param open the open section's rows
/// @return the list; nothing while none is open or its row is not shown
std::optional<OpenList> open_list(const Dialog& dialog, const layout::ScrolledRows& open) {
    if (dialog.open_list == no_control)
        return std::nullopt;
    const layout::Row* row = row_of(open.rows, dialog.open_list);
    const bool choice =
        row != nullptr && row->lock == Lock::none &&
        (row->downloads >= 0 ? downloads::is_choice(*row)
         : row->updates >= 0 ? updates::is_choice(*row)
                             : layout::kind_of(row->setting) == kit::RowKind::choice);
    if (!choice)
        return std::nullopt;
    const auto choices =
        row->downloads >= 0 ? static_cast<std::size_t>(downloads::choice_count())
        : row->updates >= 0
            ? static_cast<std::size_t>(updates::choice_count(*row))
            : static_cast<std::size_t>(kit::row_count(
                  layout::row_spec(row->setting), layout::reading(dialog.chosen, dialog)
              ));
    return OpenList{
        row,
        layout::choice_list(row->control_area, choices, dialog.size_class),
        choices,
        layout::shown_choices(choices)
    };
}

/// Returns the item of an open list under a point: the item whose control
/// the display list puts there (control_under).
///
/// @param dialog the dialog, its list open
/// @param x the point's column
/// @param y the point's row
/// @return the item, from 0; -1 for a point on no item
int32_t list_item_at(const Dialog& dialog, int32_t x, int32_t y) {
    const int32_t control = control_under(dialog, x, y);
    return control <= layout::first_menu_item_control ? layout::first_menu_item_control - control
                                                      : -1;
}

/// Closes the open list, choosing nothing.
///
/// @param[in,out] dialog the dialog
void close_list(Dialog& dialog) noexcept {
    dialog.open_list = no_control;
    dialog.list_pressed = -1;
}

/// Scrolls an open list the least that shows an item.
///
/// @param[in,out] dialog the dialog
/// @param list the list
/// @param item the item, from 0
void show_list_item(Dialog& dialog, const OpenList& list, int32_t item) noexcept {
    if (item < dialog.list_first)
        dialog.list_first = item;
    else if (item >= dialog.list_first + list.shown)
        dialog.list_first = item - list.shown + 1;
    dialog.list_first = std::clamp(
        dialog.list_first, int32_t{0}, std::max(static_cast<int32_t>(list.choices) - list.shown, 0)
    );
}

/// Opens a drop-down's list, marking its chosen item and showing it.
///
/// @param[in,out] dialog the dialog
/// @param row the drop-down's row
/// @return DialogAction::redraw
DialogAction open_choices(Dialog& dialog, const layout::Row& row) {
    const bool downloads_choice = row.downloads >= 0;
    const bool updates_choice = row.updates >= 0;
    const auto& spec = downloads_choice || updates_choice ? layout::row_spec(Setting::language)
                                                          : layout::row_spec(row.setting);
    const layout::SettingsModel model = layout::reading(dialog.chosen, dialog);
    const auto choices = downloads_choice ? static_cast<std::size_t>(downloads::choice_count())
                         : updates_choice ? static_cast<std::size_t>(updates::choice_count(row))
                                          : static_cast<std::size_t>(kit::row_count(spec, model));
    const OpenList list{
        &row,
        layout::choice_list(row.control_area, choices, dialog.size_class),
        choices,
        layout::shown_choices(choices)
    };
    dialog.open_list = row.control;
    dialog.list_pressed = -1;
    dialog.list_first = 0;
    dialog.list_marked = downloads_choice ? downloads::choice_index(dialog)
                         : updates_choice ? updates::choice_index(dialog, row)
                                          : kit::row_index(spec, model);
    show_list_item(dialog, list, dialog.list_marked);
    return DialogAction::redraw;
}

/// Chooses an open list's item and closes the list.
///
/// @param[in,out] dialog the dialog
/// @param list the list
/// @param item the item, from 0
/// @return DialogAction::changed when the choice moved, else DialogAction::redraw
DialogAction choose(Dialog& dialog, const OpenList& list, int32_t item) {
    if (list.row->downloads >= 0) {
        close_list(dialog);
        const EngineSettings before = dialog.chosen;
        downloads::apply_choice(dialog, item);
        return changed_or_redraw(dialog, before);
    }
    if (list.row->updates >= 0) {
        close_list(dialog);
        const EngineSettings before = dialog.chosen;
        updates::apply_choice(dialog, *list.row, item);
        return changed_or_redraw(dialog, before);
    }
    const Setting setting = list.row->setting;
    // A divider and a pack whose downloads are off leave the list open.
    // Choosing an available pack asks for its download even when its tag
    // was already the choice, so a failed download can be tried again.
    if (setting == Setting::language) {
        const layout::LanguagePick pick = layout::language_pick(dialog, item);
        if (pick.kind == layout::LanguagePickKind::refused)
            return DialogAction::redraw;
        if (pick.kind == layout::LanguagePickKind::download) {
            close_list(dialog);
            dialog.chosen.language = pick.tag;
            dialog.download_request = pick.tag;
            return DialogAction::download_language;
        }
    }
    close_list(dialog);
    const EngineSettings before = dialog.chosen;
    layout::SettingsModel model = chosen_model(dialog);
    static_cast<void>(kit::choose(layout::row_spec(setting), model, item));
    return changed_or_redraw(dialog, before);
}

/// Takes a key while a drop-down list is open: Up and Down mark the item
/// above or below, Page Up and Page Down a list's height of items away, Home
/// and End the first and the last; Enter and Space choose the marked item;
/// Escape closes the list; Tab and Shift+Tab close it and move the focus.
///
/// @param[in,out] dialog the dialog
/// @param[in,out] open the open section's rows
/// @param list the open list
/// @param key the key
/// @return what the key asks of the host
DialogAction
list_key(Dialog& dialog, layout::ScrolledRows& open, const OpenList& list, DialogKey key) {
    const int32_t last = static_cast<int32_t>(list.choices) - 1;
    int32_t marked = dialog.list_marked;
    switch (key) {
    case DialogKey::up:
    case DialogKey::down:
    case DialogKey::page_up:
    case DialogKey::page_down:
    case DialogKey::home:
    case DialogKey::end:
        // Language's divider is not an item the keys can mark. A step that
        // lands on it continues one further in the same direction.
        if (list.row->setting == Setting::language) {
            int32_t moved = 0;
            if (key == DialogKey::up)
                moved = -1;
            else if (key == DialogKey::down)
                moved = 1;
            else if (key == DialogKey::page_up)
                moved = -list.shown;
            else if (key == DialogKey::page_down)
                moved = list.shown;
            else if (key == DialogKey::home)
                moved = -(last + 1);
            else
                moved = last + 1;
            marked = layout::language_mark_toward(dialog, marked, moved);
        } else if (key == DialogKey::up) {
            --marked;
        } else if (key == DialogKey::down) {
            ++marked;
        } else if (key == DialogKey::page_up) {
            marked -= list.shown;
        } else if (key == DialogKey::page_down) {
            marked += list.shown;
        } else if (key == DialogKey::home) {
            marked = 0;
        } else {
            marked = last;
        }
        break;
    case DialogKey::enter:
    case DialogKey::space:
        return choose(dialog, list, std::clamp(marked, int32_t{0}, last));
    case DialogKey::escape:
        close_list(dialog);
        return DialogAction::redraw;
    case DialogKey::tab:
    case DialogKey::back_tab:
        close_list(dialog);
        return move_in_tab_order(dialog, open, key == DialogKey::tab);
    default:
        return DialogAction::none;
    }
    marked = std::clamp(marked, int32_t{0}, last);
    if (marked == dialog.list_marked)
        return DialogAction::none;
    dialog.list_marked = marked;
    show_list_item(dialog, list, marked);
    return DialogAction::redraw;
}

/// Resets every setting the dialog can change to its default. Each locked
/// setting, found through its row's lock on every section the dialog lists,
/// keeps its value. A dialog of the engine's settings keeps the mod
/// options, and each press there also asks for the graphics card to be
/// tried afresh; a dialog of a mod's options resets the mod options alone,
/// up to the mod's most snap radii. Either reports a change even when no
/// setting moved.
///
/// @param[in,out] dialog the dialog
/// @return DialogAction::changed
DialogAction restore_defaults(Dialog& dialog) {
    const EngineSettings before = dialog.chosen;
    if (dialog.kind == DialogKind::mod_options) {
        ModOptions options = dialog.defaults.mod_options;
        options.mex_snap_most = before.mod_options.mex_snap_most;
        options.wreck_snap_most = before.mod_options.wreck_snap_most;
        options.mex_snap_radius = std::min(options.mex_snap_radius, options.mex_snap_most);
        options.wreck_snap_radius = std::min(options.wreck_snap_radius, options.wreck_snap_most);
        if (dialog.locks.mex_snap != Lock::none)
            options.mex_snap_radius = before.mod_options.mex_snap_radius;
        if (dialog.locks.wreck_snap != Lock::none)
            options.wreck_snap_radius = before.mod_options.wreck_snap_radius;
        dialog.chosen.mod_options = options;
        dialog.restored = true;
        return DialogAction::changed;
    }
    if (dialog.kind == DialogKind::language_text) {
        // Language alone: only its settings go back to their defaults, each
        // locked one kept.
        const auto language = layout::section_settings(Page::language, dialog.section_hooks);
        for (const Setting setting : language)
            if (layout::section_lock(dialog.locks, setting, dialog.section_hooks) == Lock::none)
                layout::copy_row(
                    setting, chosen_model(dialog), layout::reading(dialog.defaults, dialog)
                );
        dialog.restored = true;
        return DialogAction::changed;
    }
    EngineSettings restored = dialog.defaults;
    restored.mod_options = before.mod_options;
    // The overrides stay: Restore profile values clears them. The mod
    // changes only through the Library.
    restored.hack_overrides = before.hack_overrides;
    restored.picked_mod_folder = before.picked_mod_folder;
    restored.mod_folder = before.mod_folder;
    // The backups switch changes only where the dialog lists it.
    if (!dialog.game_files)
        restored.game_files_backed_up = before.game_files_backed_up;
    for (const Page page :
         dialog_pages(dialog.kind, dialog.touch, dialog.game_files, dialog.controller)) {
        const auto settings =
            layout::section_settings(page, dialog.section_hooks, geometry::row_context(dialog));
        for (const Setting setting : settings) {
            if (layout::section_lock(dialog.locks, setting, dialog.section_hooks) != Lock::none)
                layout::copy_row(setting, {&restored, &dialog}, layout::reading(before, dialog));
        }
    }
    dialog.chosen = restored;
    // Every install ID goes back on. The checks follow the defaults.
    for (InstallIdChoice& choice : dialog.install_id_chosen)
        choice.on = true;
    updates::follow_mode(dialog);
    dialog.restored = true;
    ++dialog.forget_renderer_failures;
    restore_registered_page(dialog);
    return DialogAction::changed;
}

/// Closes the dialog keeping what it shows.
///
/// @param[in,out] dialog the dialog
/// @return DialogAction::accepted
DialogAction accept(Dialog& dialog) noexcept {
    dialog.open_list = no_control;
    dialog.pressed = no_control;
    dialog.dragging = false;
    return DialogAction::accepted;
}

/// Closes the dialog putting back what it opened with.
///
/// @param[in,out] dialog the dialog
/// @return DialogAction::cancelled
DialogAction cancel(Dialog& dialog) noexcept {
    dialog.open_list = no_control;
    dialog.chosen = dialog.opened;
    dialog.install_id_chosen = dialog.install_id_opened;
    dialog.registered_values = dialog.registered_opened;
    updates::follow_mode(dialog);
    dialog.pressed = no_control;
    dialog.dragging = false;
    return DialogAction::cancelled;
}

/// Shows a section.
///
/// @param[in,out] dialog the dialog
/// @param page the section
/// @return DialogAction::redraw
DialogAction show_page(Dialog& dialog, Page page) noexcept {
    dialog.open_list = no_control;
    dialog.list_pressed = -1;
    dialog.page = page;
    dialog.wheel_rows = 0.0F;
    if (dialog.focused >= first_row_control || registered_nav_index(dialog, dialog.focused) >= 0)
        dialog.focused = page == Page::registered ? registered_nav_control(dialog.registered_page)
                                                  : page_control(page);
    layout::reveal_engine_nav(dialog);
    return DialogAction::redraw;
}

/// Asks the host to open one of Your files' folders.
///
/// @param[in,out] dialog the dialog
/// @param button the button pressed, which the keys then mark
/// @return DialogAction::open_folder
DialogAction open_folder(Dialog& dialog, FolderButton button) noexcept {
    dialog.folder_marked = button;
    dialog.folder_to_open = button;
    return DialogAction::open_folder;
}

/// Takes what a row's button asks for: MANAGE… the Game files screen, a
/// button of Your files its folder, MORE IN THE LIBRARY… the Library's
/// Languages, REGISTRIES IN THE LIBRARY… the Library's Registries, SHOW
/// the UI kit gallery, SHOW AGAIN the welcome setup at the next main menu.
///
/// @param[in,out] dialog the dialog
/// @param action the button's action (manage_game_files_action,
///     first_folder_action and its place, open_library_action,
///     show_gallery_action or show_welcome_action)
/// @return what it asks of the host
DialogAction row_action(Dialog& dialog, kit::ActionId action) noexcept {
    static_assert(
        layout::open_library_action != downloads::reset_install_id_action &&
            layout::open_library_action != downloads::empty_downloads_action &&
            layout::open_library_action != layout::manage_game_files_action &&
            (layout::open_library_action < layout::first_folder_action ||
             layout::open_library_action >=
                 layout::first_folder_action + static_cast<kit::ActionId>(folder_button_count)),
        "MORE IN THE LIBRARY… asks for an action no other row's button asks for"
    );
    static_assert(
        layout::show_gallery_action != downloads::reset_install_id_action &&
            layout::show_gallery_action != downloads::empty_downloads_action &&
            layout::show_gallery_action != downloads::manage_registries_action &&
            layout::show_gallery_action != layout::manage_game_files_action &&
            layout::show_gallery_action != layout::open_library_action &&
            (layout::show_gallery_action < layout::first_folder_action ||
             layout::show_gallery_action >=
                 layout::first_folder_action + static_cast<kit::ActionId>(folder_button_count)),
        "SHOW asks for an action no other row's button asks for"
    );
    static_assert(
        layout::show_welcome_action != downloads::reset_install_id_action &&
            layout::show_welcome_action != downloads::empty_downloads_action &&
            layout::show_welcome_action != downloads::manage_registries_action &&
            layout::show_welcome_action != layout::manage_game_files_action &&
            layout::show_welcome_action != layout::open_library_action &&
            layout::show_welcome_action != layout::show_gallery_action &&
            (layout::show_welcome_action < layout::first_folder_action ||
             layout::show_welcome_action >=
                 layout::first_folder_action + static_cast<kit::ActionId>(folder_button_count)),
        "SHOW AGAIN asks for an action no other row's button asks for"
    );
    static_assert(
        layout::edit_keys_action != downloads::reset_install_id_action &&
            layout::edit_keys_action != downloads::empty_downloads_action &&
            layout::edit_keys_action != downloads::manage_registries_action &&
            layout::edit_keys_action != layout::manage_game_files_action &&
            layout::edit_keys_action != layout::open_library_action &&
            layout::edit_keys_action != layout::show_gallery_action &&
            layout::edit_keys_action != layout::show_welcome_action &&
            layout::edit_keys_action != updates::check_updates_action &&
            layout::edit_keys_action != updates::open_release_action &&
            layout::edit_keys_action != updates::restart_update_action &&
            layout::edit_keys_action != updates::retry_update_action &&
            layout::edit_keys_action != updates::roll_back_action &&
            (layout::edit_keys_action < layout::first_folder_action ||
             layout::edit_keys_action >=
                 layout::first_folder_action + static_cast<kit::ActionId>(folder_button_count)),
        "EDIT KEYS… asks for an action no other row's button asks for"
    );
    static_assert(
        layout::choose_latch_action != layout::edit_keys_action &&
            layout::choose_latch_action != downloads::reset_install_id_action &&
            layout::choose_latch_action != downloads::empty_downloads_action &&
            layout::choose_latch_action != downloads::manage_registries_action &&
            layout::choose_latch_action != layout::manage_game_files_action &&
            layout::choose_latch_action != layout::open_library_action &&
            layout::choose_latch_action != layout::show_gallery_action &&
            layout::choose_latch_action != layout::show_welcome_action &&
            layout::choose_latch_action != updates::check_updates_action &&
            layout::choose_latch_action != updates::open_release_action &&
            layout::choose_latch_action != updates::restart_update_action &&
            layout::choose_latch_action != updates::retry_update_action &&
            layout::choose_latch_action != updates::roll_back_action &&
            (layout::choose_latch_action < layout::first_folder_action ||
             layout::choose_latch_action >=
                 layout::first_folder_action + static_cast<kit::ActionId>(folder_button_count)),
        "CHOOSE… asks for an action no other row's button asks for"
    );
    static_assert(
        updates::first_card_action > layout::choose_latch_action &&
            updates::first_card_action + static_cast<kit::ActionId>(most_update_card_actions) <=
                layout::reset_windows_action,
        "the update card's buttons ask for actions no other row's button asks for"
    );
    if (action == layout::edit_keys_action) {
        dialog.edit_keys_filter = KeyEditorFilter::all;
        return DialogAction::edit_keys;
    }
    if (action == layout::reset_windows_action) {
        // Every moved window goes back at once; Cancel does not undo it.
        dialog.window_reset_asked = true;
        return DialogAction::changed;
    }
    if (action == layout::choose_latch_action) {
        dialog.edit_keys_filter = KeyEditorFilter::latchable;
        return DialogAction::edit_keys;
    }
    static_assert(
        layout::arrange_overlays_action != layout::choose_latch_action &&
            layout::arrange_overlays_action != layout::edit_keys_action &&
            layout::arrange_overlays_action != layout::show_welcome_action &&
            layout::arrange_overlays_action != updates::check_updates_action &&
            layout::arrange_overlays_action != updates::open_release_action &&
            layout::arrange_overlays_action != updates::restart_update_action &&
            layout::arrange_overlays_action != updates::retry_update_action &&
            layout::arrange_overlays_action != updates::roll_back_action &&
            updates::first_card_action > layout::arrange_overlays_action &&
            (layout::arrange_overlays_action < layout::first_folder_action ||
             layout::arrange_overlays_action >=
                 layout::first_folder_action + static_cast<kit::ActionId>(folder_button_count)),
        "ARRANGE… asks for an action no other row's button asks for"
    );
    if (action == layout::arrange_overlays_action)
        return DialogAction::arrange_overlays;
    if (action == layout::show_welcome_action) {
        // The preference stays until OK. Cancel drops the dialog, and the
        // flag with it.
        dialog.welcome_again = true;
        return DialogAction::changed;
    }
    if (action == layout::show_gallery_action)
        return DialogAction::show_gallery;
    if (action == downloads::reset_install_id_action)
        return DialogAction::reset_install_id;
    if (action == downloads::empty_downloads_action)
        return DialogAction::empty_downloads;
    if (action == layout::manage_game_files_action)
        return DialogAction::manage_game_files;
    if (action == layout::open_library_action) {
        dialog.library_tab = LibraryTab::languages;
        return DialogAction::open_library;
    }
    if (action == downloads::manage_registries_action) {
        dialog.library_tab = LibraryTab::registries;
        return DialogAction::open_library;
    }
    if (const std::optional<DialogAction> card = updates::card_action(dialog, action))
        return *card;
    if (action == updates::check_updates_action)
        return DialogAction::check_updates;
    if (action == updates::open_release_action)
        return DialogAction::open_release;
    if (action == updates::restart_update_action)
        return DialogAction::restart_update;
    if (action == updates::retry_update_action)
        return DialogAction::retry_update;
    if (action == updates::roll_back_action)
        return DialogAction::roll_back;
    const kit::ActionId button = action - layout::first_folder_action;
    if (button < 0 || button >= static_cast<kit::ActionId>(folder_button_count))
        return DialogAction::none;
    return open_folder(dialog, static_cast<FolderButton>(button));
}

/// Presses a button, flips a switch or opens a drop-down's list, as Space
/// or a click does; in Developer's list, opens or closes an area or a hack.
///
/// @param[in,out] dialog the dialog
/// @param open the open section's rows
/// @param control the control
/// @return what it asks of the host
DialogAction activate(Dialog& dialog, const layout::ScrolledRows& open, int32_t control) {
    if (control == restore_control)
        return restore_defaults(dialog);
    if (control == cancel_control || control == close_x_control)
        return cancel(dialog);
    if (control == ok_control)
        return accept(dialog);
    // A section's entry, by its number: a dialog without Touch has no
    // entry numbered as Touch.
    for (const Page page :
         dialog_pages(dialog.kind, dialog.touch, dialog.game_files, dialog.controller))
        if (control == page_control(page))
            return show_page(dialog, page);
    if (const std::optional<DialogAction> registered = activate_registered(dialog, control))
        return *registered;
    if (layout::developer_page(dialog)) {
        if (control == active_only_control)
            return developer::set_active_only(dialog, !dialog.developer.active_only);
        if (control == restore_profile_control)
            return developer::restore_profile_values(dialog);
        if (const layout::ListRow* row = layout::list_row(open.list, control))
            return developer::activate(dialog, *row);
    }
    // The key layout card's Change layout… opens the Library on Keys; the
    // practice pad's button turns every helper off.
    if (control == change_layout_control && dialog.library_link) {
        dialog.library_tab = LibraryTab::keys;
        return DialogAction::open_library;
    }
    // Updates' card's second and third buttons ask the host for their actions.
    if (const std::optional<std::size_t> button = updates::card_button(control);
        button.has_value() && dialog.page == Page::updates && cards::update_card_shown(dialog))
        return updates::take_card(dialog, *button);
    if (control == helpers_off_control && open.rows.pad.width > 0 &&
        cards::any_helper_on(dialog.chosen)) {
        const EngineSettings before = dialog.chosen;
        cards::turn_helpers_off(dialog);
        return changed_or_redraw(dialog, before);
    }
    // A row takes Space as its row spec says: a switch flips, a drop-down
    // opens its list, MANAGE… and the marked button of Your files ask the
    // host to act; a slider, a strip and a row of text take nothing.
    const layout::Row* row = row_of(open.rows, control);
    if (row == nullptr || row->lock != Lock::none)
        return DialogAction::none;
    if (row->content >= 0)
        return content::take(dialog, *row);
    if (row->downloads >= 0) {
        const EngineSettings before = dialog.chosen;
        const kit::RowResult result = downloads::activate(dialog, *row);
        if (result.event == kit::RowEvent::open_menu)
            return open_choices(dialog, *row);
        if (result.event == kit::RowEvent::action)
            return row_action(dialog, result.action);
        return changed_or_redraw(dialog, before);
    }
    if (row->updates >= 0) {
        const EngineSettings before = dialog.chosen;
        const kit::RowResult result = updates::activate(dialog, *row);
        if (result.event == kit::RowEvent::open_menu)
            return open_choices(dialog, *row);
        if (result.event == kit::RowEvent::action)
            return row_action(dialog, result.action);
        return changed_or_redraw(dialog, before);
    }
    const auto& spec = layout::row_spec(row->setting);
    const EngineSettings before = dialog.chosen;
    layout::SettingsModel model = chosen_model(dialog);
    const kit::RowResult result =
        kit::activate(spec, model, static_cast<std::size_t>(dialog.folder_marked));
    if (result.event == kit::RowEvent::open_menu)
        return open_choices(dialog, *row);
    if (result.event == kit::RowEvent::action)
        return row_action(dialog, result.action);
    if (spec.kind != kit::RowKind::toggle)
        return DialogAction::none;
    return changed_or_redraw(dialog, before);
}

/// Sets a slider to the stop under a column.
///
/// @param[in,out] dialog the dialog
/// @param row the slider's row
/// @param column the column
/// @return what it asks of the host
DialogAction drag_to(Dialog& dialog, const layout::Row& row, int32_t column) {
    const EngineSettings before = dialog.chosen;
    layout::SettingsModel model = chosen_model(dialog);
    static_cast<void>(kit::drag(layout::row_spec(row.setting), model, placed_row(row), column));
    knob_moved(dialog, row.setting);
    return changed_or_redraw(dialog, before);
}

/// Returns where a finger's press lands, by the kit's reach (kit::reach)
/// over the dialog's display list: the finger's own point over a control,
/// else the nearest point of the nearest control within reach. While a
/// drop-down list is open only its items are reached, since it takes every
/// press.
///
/// @param dialog the dialog
/// @param open the open section's rows
/// @param x the finger's column
/// @param y the finger's row
/// @param reach how far a control may lie from the finger, in source pixels
/// @return the point the press takes; the finger's own with nothing within reach
kit::Point finger_landing(
    const Dialog& dialog, const layout::ScrolledRows& open, int32_t x, int32_t y, int32_t reach
) {
    kit::DisplayList list = layout::dialog_list(dialog, dialog.drawn_fonts);
    if (open_list(dialog, open).has_value())
        std::erase_if(list.controls, [](const kit::Control& control) {
            return control.id > layout::first_menu_item_control;
        });
    return kit::reach(list, {x, y}, reach).at;
}

/// Tells whether a press is held: on a control, or on an open list's item.
///
/// @param dialog the dialog
/// @return true while a press is held
bool press_held(const Dialog& dialog) noexcept {
    return dialog.pressed != no_control || dialog.list_pressed >= 0;
}

} // namespace

int32_t dialog_page_step(const Dialog& dialog) noexcept {
    return geometry::sizes_of(dialog).page_step;
}

void set_dialog_size_class(Dialog& dialog, oa::ui::kit::SizeClass size_class) {
    dialog.size_class = size_class;
    geometry::reveal_engine_nav(dialog);
}

std::size_t changed_settings_count(const Dialog& dialog) {
    std::size_t count = 0;
    if (dialog.kind == DialogKind::mod_options) {
        // A mod's options are kept apart from the preferences: each row whose
        // value moved counts.
        const layout::SettingsModel opened = layout::reading(dialog.opened, dialog);
        const layout::SettingsModel chosen = layout::reading(dialog.chosen, dialog);
        for (const Page page : dialog_pages(DialogKind::mod_options))
            for (const Setting setting : page_settings(page))
                if (layout::row_value_text(setting, opened) !=
                    layout::row_value_text(setting, chosen))
                    ++count;
        return count;
    }
    // Every setting whose stored value would change writes its key.
    oa::platform::preferences::Values written;
    write_settings(
        written,
        dialog.opened,
        dialog.chosen,
        dialog.defaults,
        false,
        dialog.played_mod_id.empty() ? oa::data::mod_profile::base_game_id
                                     : std::string_view{dialog.played_mod_id}
    );
    count += written.size();
    if (dialog.chosen.skirmish_players != dialog.opened.skirmish_players)
        ++count;
    if (dialog.chosen.switch_alt != dialog.opened.switch_alt)
        ++count;
    if (dialog.welcome_again)
        ++count;
    for (const InstallIdChoice& chosen : dialog.install_id_chosen) {
        const auto opened = std::find_if(
            dialog.install_id_opened.begin(),
            dialog.install_id_opened.end(),
            [&chosen](const InstallIdChoice& choice) {
                return choice.registry_id == chosen.registry_id;
            }
        );
        if (opened == dialog.install_id_opened.end() || opened->on != chosen.on)
            ++count;
    }
    count += registered_writes(dialog).size();
    return count;
}

std::string_view page_word(Page page) noexcept {
    return geometry::page_word(page);
}

std::span<const Page>
dialog_pages(DialogKind kind, bool touch, bool game_files, bool controller) noexcept {
    switch (kind) {
    case DialogKind::engine:
        break;
    case DialogKind::mod_options:
        return layout::kModPages;
    case DialogKind::language_text:
        return layout::kLanguageTextPages;
    }
    const layout::EnginePageList& list =
        layout::kEnginePageLists[layout::engine_page_list_index(touch, controller, game_files)];
    return std::span<const Page>(list.pages.data(), list.count);
}

void open_dialog(
    Dialog& dialog,
    const EngineSettings& current,
    const EngineSettings& defaults,
    const Locks& locks,
    std::string_view version,
    Page page,
    const AccelerationStatus& acceleration,
    uint16_t highest_offered_unit,
    std::span<const oa::data::mod_profile::HackState> profile_hacks,
    const oa::data::languages::Language* system_language,
    bool touch,
    bool game_files,
    bool controller
) {
    dialog = Dialog{};
    dialog.touch = touch;
    dialog.game_files = game_files;
    dialog.controller = controller;
    dialog.system_language = system_language;
    dialog.highest_offered_unit = highest_offered_unit;
    dialog.opened = current;
    dialog.chosen = current;
    dialog.defaults = defaults;
    dialog.locks = locks;
    dialog.acceleration = acceleration;
    dialog.version = std::string(version);
    // Touch, Controller and Game files show only while they are listed.
    dialog.page = (page == Page::touch && !touch) || (page == Page::game_files && !game_files) ||
                          (page == Page::controller && !controller)
                      ? dialog_pages(DialogKind::engine).front()
                      : page;
    if (profile_hacks.empty())
        dialog.developer.profile = oa::data::mod_profile::base_hack_states();
    else
        dialog.developer.profile.assign(profile_hacks.begin(), profile_hacks.end());
    dialog.developer.areas_open.assign(developer_areas().size(), 0);
    dialog.developer.hacks_open.assign(oa::data::mod_profile::standard_hacks().size(), 0);
    updates::follow_mode(dialog);
    geometry::reveal_engine_nav(dialog);
}

void set_downloads_offer(Dialog& dialog, const DownloadsOffer& offer) {
    bool same = dialog.install_id_chosen.size() == offer.install_ids.size();
    for (std::size_t index = 0; same && index < offer.install_ids.size(); ++index)
        same = dialog.install_id_chosen[index].registry_id == offer.install_ids[index].registry_id;
    dialog.downloads = offer;
    if (same)
        return;
    dialog.install_id_chosen.clear();
    dialog.install_id_opened.clear();
    dialog.install_id_chosen.reserve(offer.install_ids.size());
    dialog.install_id_opened.reserve(offer.install_ids.size());
    for (const InstallIdRow& row : offer.install_ids) {
        const InstallIdChoice choice{row.registry_id, row.on};
        dialog.install_id_chosen.push_back(choice);
        dialog.install_id_opened.push_back(choice);
    }
}

void open_mod_options_dialog(
    Dialog& dialog,
    const EngineSettings& current,
    const EngineSettings& defaults,
    const Locks& locks,
    std::string_view version,
    Page page
) {
    open_dialog(dialog, current, defaults, locks, version, page);
    dialog.kind = DialogKind::mod_options;
    const auto pages = dialog_pages(DialogKind::mod_options);
    if (std::find(pages.begin(), pages.end(), page) == pages.end())
        dialog.page = pages.front();
}

void open_language_text_dialog(
    Dialog& dialog,
    const EngineSettings& current,
    const EngineSettings& defaults,
    const Locks& locks,
    std::string_view version,
    const oa::data::languages::Language* system_language
) {
    open_dialog(
        dialog,
        current,
        defaults,
        locks,
        version,
        Page::language,
        {},
        highest_unit_limit,
        {},
        {},
        system_language
    );
    dialog.kind = DialogKind::language_text;
}

DialogAction
set_acceleration_status(Dialog& dialog, const AccelerationStatus& acceleration) noexcept {
    if (dialog.acceleration == acceleration)
        return DialogAction::none;
    dialog.acceleration = acceleration;
    return DialogAction::redraw;
}

DialogAction set_touch_controls(Dialog& dialog, bool touch) noexcept {
    if (dialog.touch == touch)
        return DialogAction::none;
    dialog.touch = touch;
    if (dialog.kind != DialogKind::engine || touch) {
        geometry::reveal_engine_nav(dialog);
        return DialogAction::redraw;
    }
    // Touch's entry and rows leave the dialog: what pointed at them points
    // at nothing, and Touch's section gives way to the first.
    const int32_t entry = page_control(Page::touch);
    for (int32_t* control : {&dialog.hovered, &dialog.pressed, &dialog.focused})
        if (*control == entry || (dialog.page == Page::touch && *control >= first_row_control))
            *control = no_control;
    if (dialog.page == Page::touch) {
        static_cast<void>(show_page(dialog, dialog_pages(DialogKind::engine).front()));
        dialog.dragging = false;
    }
    geometry::reveal_engine_nav(dialog);
    return DialogAction::redraw;
}

DialogAction set_controller_section(Dialog& dialog, bool controller, bool steam_input) noexcept {
    if (dialog.controller == controller && dialog.steam_input == steam_input)
        return DialogAction::none;
    const bool showing = dialog.kind == DialogKind::engine && dialog.page == Page::controller;
    if (dialog.steam_input != steam_input && showing && dialog.controller && controller) {
        // The notice comes or goes above Controller's rows: each row's
        // control moves by one, the focus with its row; a hover, a press or
        // an open list on a row lets go.
        const int32_t moved = steam_input ? 1 : -1;
        if (dialog.focused >= first_row_control)
            dialog.focused = std::max(dialog.focused + moved, first_row_control);
        for (int32_t* control : {&dialog.hovered, &dialog.pressed})
            if (*control >= first_row_control)
                *control = no_control;
        if (dialog.open_list >= first_row_control) {
            dialog.open_list = no_control;
            dialog.list_pressed = -1;
        }
        dialog.dragging = false;
    }
    dialog.steam_input = steam_input;
    if (dialog.controller == controller) {
        geometry::reveal_engine_nav(dialog);
        return DialogAction::redraw;
    }
    dialog.controller = controller;
    if (dialog.kind != DialogKind::engine || controller) {
        geometry::reveal_engine_nav(dialog);
        return DialogAction::redraw;
    }
    // Controller's entry and rows leave the dialog: what pointed at them
    // points at nothing, and Controller's section gives way to the first.
    const int32_t entry = page_control(Page::controller);
    for (int32_t* control : {&dialog.hovered, &dialog.pressed, &dialog.focused})
        if (*control == entry || (showing && *control >= first_row_control))
            *control = no_control;
    if (showing) {
        static_cast<void>(show_page(dialog, dialog_pages(DialogKind::engine).front()));
        dialog.dragging = false;
    }
    geometry::reveal_engine_nav(dialog);
    return DialogAction::redraw;
}

DialogAction dialog_pointer_move(Dialog& dialog, int32_t x, int32_t y) {
    // A finger's held press moves as it was moved to the control it took.
    if (press_held(dialog)) {
        x += dialog.finger_shift_x;
        y += dialog.finger_shift_y;
    }
    note_pointer(dialog, x, y);
    layout::ScrolledRows open = drawn_rows(dialog);
    // An open list marks the item under the pointer.
    if (const auto list = open_list(dialog, open)) {
        const int32_t item = list_item_at(dialog, x, y);
        if (item < 0 || item == dialog.list_marked)
            return DialogAction::none;
        dialog.list_marked = item;
        return DialogAction::redraw;
    }
    if (dialog.dragging) {
        // The thumb follows the pointer's row only, and the offset the thumb.
        if (dialog.pressed == scroll_bar_control)
            return scroll_to(
                dialog,
                open,
                layout::scroll_at(
                    open.area, y - dialog.scroll_grab, open.limit, open.content_height
                )
            );
        if (const layout::ListRow* row = layout::list_row(open.list, dialog.pressed))
            return developer::drag_to(dialog, *row, x);
        const layout::Row* row = row_of(open.rows, dialog.pressed);
        if (row != nullptr)
            return drag_to(dialog, *row, x);
    }
    const int32_t hovered = control_under(dialog, x, y);
    // Your files lights the button under the pointer.
    const layout::Row* buttons = row_of(open.rows, hovered);
    const std::size_t button =
        buttons != nullptr && layout::kind_of(buttons->setting) == kit::RowKind::buttons
            ? layout::folder_button_at(buttons->control_area, x, y)
            : folder_button_count;
    if (hovered == dialog.hovered && button == dialog.folder_hovered)
        return DialogAction::none;
    dialog.hovered = hovered;
    dialog.folder_hovered = button;
    return DialogAction::redraw;
}

DialogAction dialog_pointer_down(Dialog& dialog, int32_t x, int32_t y) {
    dialog.finger_shift_x = 0;
    dialog.finger_shift_y = 0;
    note_pointer(dialog, x, y);
    layout::ScrolledRows open = drawn_rows(dialog);
    // An open list takes the press: on an item it holds the item; anywhere
    // else it closes the list, and the press does nothing more.
    if (const auto list = open_list(dialog, open)) {
        dialog.pressed = no_control;
        dialog.dragging = false;
        const int32_t item = list_item_at(dialog, x, y);
        if (item >= 0) {
            dialog.list_pressed = item;
            dialog.list_marked = item;
        } else if (kit::contains(list->rect, {x, y})) {
            // The divider and the menu's border are inside the menu and are
            // not items: the press keeps the list open and chooses nothing.
            dialog.list_pressed = -1;
        } else {
            close_list(dialog);
        }
        return DialogAction::redraw;
    }
    close_list(dialog);
    const int32_t control = control_under(dialog, x, y);
    dialog.hovered = control;
    dialog.pressed = control;
    dialog.dragging = false;
    if (control == no_control)
        return DialogAction::none;
    if (control == scroll_bar_control) {
        // On the thumb, the press grabs it at the row pressed; on the well,
        // the thumb's middle jumps to the pointer and the drag starts there.
        // The scroll bar takes no focus, so the focus stays where it is.
        dialog.dragging = true;
        const layout::SourceRect thumb =
            layout::scroll_thumb(open.area, open.scroll, open.limit, open.content_height);
        if (y >= thumb.y && y < thumb.y + thumb.height) {
            dialog.scroll_grab = y - thumb.y;
            return DialogAction::redraw;
        }
        dialog.scroll_grab = thumb.height / 2;
        static_cast<void>(scroll_to(
            dialog,
            open,
            layout::scroll_at(open.area, y - dialog.scroll_grab, open.limit, open.content_height)
        ));
        return DialogAction::redraw;
    }
    if (dialog.focused != no_control)
        dialog.focused = control;
    if (const layout::ListRow* row = layout::list_row(open.list, control)) {
        // A press on a slider of the list moves its knob and drags it.
        if (row->kind != layout::ListRowKind::slider)
            return DialogAction::redraw;
        dialog.dragging = true;
        return developer::drag_to(dialog, *row, x);
    }
    const layout::Row* row = row_of(open.rows, control);
    // Game Content's rows are not settings. An unset setting is the
    // pathfinding slider, so a press on Check again must not drag it.
    if (row != nullptr && row->content < 0 && row->downloads < 0 && row->updates < 0 &&
        layout::kind_of(row->setting) == kit::RowKind::slider) {
        dialog.dragging = true;
        const DialogAction action = drag_to(dialog, *row, x);
        return action;
    }
    // A press on Your files holds the button under it.
    if (row != nullptr && row->content < 0 && row->downloads < 0 && row->updates < 0 &&
        layout::kind_of(row->setting) == kit::RowKind::buttons)
        dialog.folder_hovered = layout::folder_button_at(row->control_area, x, y);
    return DialogAction::redraw;
}

DialogAction dialog_finger_down(Dialog& dialog, int32_t x, int32_t y, int32_t reach) {
    const kit::Point target = finger_landing(dialog, drawn_rows(dialog), x, y, reach);
    const DialogAction action = dialog_pointer_down(dialog, target.x, target.y);
    if (press_held(dialog)) {
        dialog.finger_shift_x = target.x - x;
        dialog.finger_shift_y = target.y - y;
    }
    return action;
}

DialogAction dialog_pointer_up(Dialog& dialog, int32_t x, int32_t y) {
    // A finger's release lands as its press was moved; the next press
    // starts afresh.
    if (press_held(dialog)) {
        x += dialog.finger_shift_x;
        y += dialog.finger_shift_y;
    }
    dialog.finger_shift_x = 0;
    dialog.finger_shift_y = 0;
    note_pointer(dialog, x, y);
    const layout::ScrolledRows open = drawn_rows(dialog);
    // A release over the list item the press held chooses it.
    if (const auto list = open_list(dialog, open)) {
        const int32_t held = dialog.list_pressed;
        dialog.list_pressed = -1;
        if (held >= 0 && list_item_at(dialog, x, y) == held)
            return choose(dialog, *list, held);
        return held >= 0 ? DialogAction::redraw : DialogAction::none;
    }
    const int32_t pressed = dialog.pressed;
    const bool dragged = dialog.dragging;
    dialog.pressed = no_control;
    dialog.dragging = false;
    dialog.scroll_grab = 0;
    if (pressed == no_control)
        return DialogAction::none;
    const int32_t control = control_under(dialog, x, y);
    dialog.hovered = control;
    if (dragged || control != pressed)
        return DialogAction::redraw;
    if (const std::optional<DialogAction> registered =
            press_registered_row(dialog, control, {x, y}))
        return *registered;
    if (layout::developer_page(dialog)) {
        if (control == active_only_control) {
            const layout::SourceRect& active = open.active_only_switch;
            return developer::set_active_only(dialog, x >= active.x + active.width / 2);
        }
        if (const layout::ListRow* row = layout::list_row(open.list, control))
            return developer::release_on(dialog, *row, x);
    }
    // A row takes the release as its row spec says: a switch's half sets
    // it, a strip's level is chosen, a drop-down opens its list, MANAGE…
    // and the button of Your files the press held ask the host to act.
    const layout::Row* row = row_of(open.rows, control);
    if (row == nullptr)
        return activate(dialog, open, control);
    if (row->content >= 0)
        return content::take(dialog, *row);
    if (row->downloads >= 0) {
        const EngineSettings before = dialog.chosen;
        const kit::RowResult result = downloads::release(dialog, *row, x, y);
        if (result.event == kit::RowEvent::open_menu)
            return open_choices(dialog, *row);
        if (result.event == kit::RowEvent::action)
            return row_action(dialog, result.action);
        return changed_or_redraw(dialog, before);
    }
    if (row->updates >= 0) {
        // A game locks ROLL BACK the way it locks the page's other rows.
        // The kit omits the control while the lock shows; this stops a press
        // that still reaches the row.
        if (row->lock != Lock::none)
            return DialogAction::none;
        const EngineSettings before = dialog.chosen;
        const kit::RowResult result = updates::release(dialog, *row, x, y);
        if (result.event == kit::RowEvent::open_menu)
            return open_choices(dialog, *row);
        if (result.event == kit::RowEvent::action)
            return row_action(dialog, result.action);
        return changed_or_redraw(dialog, before);
    }
    const auto& spec = layout::row_spec(row->setting);
    if (spec.kind == kit::RowKind::buttons) {
        // A release over the button the press held opens its folder.
        const std::size_t held = dialog.folder_hovered;
        const std::size_t button = layout::folder_button_at(row->control_area, x, y);
        dialog.folder_hovered = button;
        if (button == folder_button_count || button != held)
            return DialogAction::redraw;
    }
    const EngineSettings before = dialog.chosen;
    layout::SettingsModel model = chosen_model(dialog);
    const kit::RowResult result = kit::press(spec, model, placed_row(*row), {x, y});
    if (result.event == kit::RowEvent::open_menu)
        return open_choices(dialog, *row);
    if (result.event == kit::RowEvent::action)
        return row_action(dialog, result.action);
    return changed_or_redraw(dialog, before);
}

DialogAction dialog_key(Dialog& dialog, DialogKey key) {
    layout::ScrolledRows open = drawn_rows(dialog);
    const layout::Rows& rows = open.rows;
    if (const auto list = open_list(dialog, open))
        return list_key(dialog, open, *list, key);
    close_list(dialog);
    switch (key) {
    case DialogKey::enter:
        return accept(dialog);
    case DialogKey::escape:
        return cancel(dialog);
    case DialogKey::tab:
    case DialogKey::back_tab:
        return move_in_tab_order(dialog, open, key == DialogKey::tab);
    case DialogKey::up:
        return move_toward(dialog, open, kit::Direction::up);
    case DialogKey::down:
        return move_toward(dialog, open, kit::Direction::down);
    case DialogKey::page_up:
    case DialogKey::page_down:
    case DialogKey::home:
    case DialogKey::end:
        return scroll_key(dialog, open, key);
    // Y and N answer a question, and do nothing while none shows. The
    // dialog has no text to edit.
    case DialogKey::yes:
    case DialogKey::no:
    case DialogKey::backspace:
    case DialogKey::delete_forward:
        return DialogAction::none;
    default:
        break;
    }
    if (dialog.focused == no_control)
        return move_in_tab_order(dialog, open, true);
    if (key != DialogKey::space) {
        // Left and Right step a control that takes steps; from any other
        // they move the focus to the control on that side.
        const kit::DisplayList list = layout::dialog_list(dialog, dialog.drawn_fonts);
        const kit::Control* focused = kit::control_of(list, dialog.focused);
        if (focused == nullptr || !focused->steps)
            return move_toward(
                dialog, open, key == DialogKey::left ? kit::Direction::left : kit::Direction::right
            );
    }
    // A key that acts on a row brings it into view first, so that the
    // player sees what it changed.
    const DialogAction shown = show_row(dialog, open, dialog.focused);
    const auto or_shown = [shown](DialogAction action) {
        return action == DialogAction::none ? shown : action;
    };
    if (key == DialogKey::space)
        return or_shown(activate(dialog, open, dialog.focused));
    const bool up = key == DialogKey::right;
    if (layout::developer_page(dialog)) {
        if (dialog.focused == active_only_control)
            return or_shown(developer::set_active_only(dialog, up));
        if (const layout::ListRow* row = layout::list_row(open.list, dialog.focused))
            return or_shown(developer::step(dialog, *row, up));
    }
    if (key == DialogKey::left || key == DialogKey::right) {
        if (const std::optional<DialogAction> stepped =
                step_registered_row(dialog, dialog.focused, key == DialogKey::right))
            return or_shown(*stepped);
    }
    const layout::Row* row = row_of(rows, dialog.focused);
    if (row != nullptr && row->content >= 0)
        return shown;
    if (row != nullptr && row->downloads >= 0) {
        if (row->lock != Lock::none)
            return shown;
        return or_shown(downloads::step(dialog, *row, up));
    }
    if (row != nullptr && row->updates >= 0) {
        if (row->lock != Lock::none)
            return shown;
        return or_shown(updates::step(dialog, *row, up));
    }
    const kit::RowKind kind = row != nullptr ? layout::kind_of(row->setting) : kit::RowKind::text;
    if (row != nullptr && row->lock == Lock::none && kind == kit::RowKind::buttons) {
        // Left and Right move Your files' mark along its buttons.
        const auto marked = static_cast<std::size_t>(dialog.folder_marked);
        std::size_t next = marked;
        layout::SettingsModel model = chosen_model(dialog);
        static_cast<void>(kit::step(layout::row_spec(row->setting), model, up, next));
        if (next == marked)
            return shown;
        dialog.folder_marked = static_cast<FolderButton>(next);
        return DialogAction::redraw;
    }
    if (row == nullptr || row->lock != Lock::none)
        return shown;
    const EngineSettings before = dialog.chosen;
    step(dialog, row->setting, up);
    return changed_or_redraw(dialog, before);
}

/// Scrolls the section list when the pointer is over it and the list is
/// taller than its room. A list that fits returns nothing, so the open
/// section still scrolls.
///
/// @param[in,out] dialog the dialog
/// @param x the pointer's column
/// @param y the pointer's row
/// @param notches the wheel's notches, away from the player positive
/// @return what it asks; nothing when the list does not take the wheel
std::optional<DialogAction> wheel_engine_nav(Dialog& dialog, int32_t x, int32_t y, float notches) {
    // The list as wide as the fonts it is drawn in make it.
    const geometry::Sizes sized = geometry::shown_sizes(dialog, dialog.drawn_fonts);
    if (x < geometry::edge || y < sized.body_top || x >= geometry::edge + sized.list_width ||
        y >= sized.footer_rule_row)
        return std::nullopt;
    const geometry::NavLayout nav = geometry::nav_layout(dialog, &sized);
    if (nav.limit <= 0)
        return std::nullopt;
    dialog.nav_scroll = std::clamp(dialog.nav_scroll, int32_t{0}, nav.limit);
    kit::WheelCarry carry{dialog.nav_wheel_rows};
    const int32_t next =
        kit::wheel_offset(carry, notches, geometry::wheel_step, dialog.nav_scroll, nav.limit);
    dialog.nav_wheel_rows = carry.rows;
    if (next == dialog.nav_scroll)
        return DialogAction::none;
    dialog.nav_scroll = next;
    return DialogAction::redraw;
}

DialogAction dialog_wheel(Dialog& dialog, int32_t x, int32_t y, float notches) {
    if (!dialog_contains(dialog, x, y) || dialog.pressed != no_control || !std::isfinite(notches))
        return DialogAction::none;
    note_pointer(dialog, x, y);
    layout::ScrolledRows open = drawn_rows(dialog);
    // An open list keeps the section still and scrolls itself, an item a
    // notch, when it holds more items than it shows.
    if (const auto list = open_list(dialog, open)) {
        if (!kit::contains(list->rect, {x, y}) ||
            static_cast<int32_t>(list->choices) <= list->shown)
            return DialogAction::none;
        const int32_t first = std::clamp(
            dialog.list_first - static_cast<int32_t>(std::lround(notches)),
            int32_t{0},
            static_cast<int32_t>(list->choices) - list->shown
        );
        if (first == dialog.list_first)
            return DialogAction::none;
        dialog.list_first = first;
        return DialogAction::redraw;
    }
    if (const std::optional<DialogAction> engine = wheel_engine_nav(dialog, x, y, notches))
        return *engine;
    if (const std::optional<DialogAction> registered = wheel_registered_content(dialog, notches))
        return *registered;
    // Away from the player scrolls towards the top, wheel_step rows a notch,
    // the fraction carried to the next turn (kit::wheel_offset). A section
    // that does not scroll drops what was carried.
    kit::WheelCarry carry{dialog.wheel_rows};
    const int32_t next =
        kit::wheel_offset(carry, notches, layout::wheel_step, open.scroll, open.limit);
    dialog.wheel_rows = carry.rows;
    if (open.limit == 0)
        return DialogAction::none;
    return scroll_to(dialog, open, next);
}

DialogAction set_folder_notice(Dialog& dialog, std::string_view reason) {
    if (dialog.folder_notice == reason)
        return DialogAction::none;
    dialog.folder_notice = std::string(reason);
    return DialogAction::redraw;
}

namespace {

/// Tells whether a character separates a path's components.
///
/// @param character the character
/// @return true for '/' and '\\'
bool path_separator(char character) noexcept {
    return character == '/' || character == '\\';
}

} // namespace

std::string path_tail(
    std::string_view path, int32_t width, const std::function<int32_t(std::string_view)>& text_width
) {
    while (path.size() > 1 && path_separator(path.back()))
        path.remove_suffix(1);
    if (text_width(path) <= width)
        return std::string(path);
    const std::string ellipsis(layout::path_ellipsis);
    // The most whole components that fit, the separator before them kept.
    for (std::size_t at = 1; at < path.size(); ++at) {
        if (!path_separator(path[at]))
            continue;
        std::string tail = ellipsis + std::string(path.substr(at));
        if (text_width(tail) <= width)
            return tail;
    }
    // The last component alone is too wide: as much of its end as fits.
    std::size_t last = path.size();
    while (last > 0 && !path_separator(path[last - 1]))
        --last;
    for (std::size_t at = last; at < path.size(); ++at) {
        if ((static_cast<unsigned char>(path[at]) & 0xC0U) == 0x80U)
            continue;
        std::string tail = ellipsis + std::string(path.substr(at));
        if (text_width(tail) <= width)
            return tail;
    }
    return ellipsis;
}

bool dialog_contains(int32_t x, int32_t y) noexcept {
    return x >= 0 && y >= 0 && x < dialog_width && y < dialog_height;
}

int32_t scroll_limit(const Dialog& dialog) {
    return drawn_rows(dialog).limit;
}

bool dialog_contains(const Dialog& dialog, int32_t x, int32_t y) noexcept {
    const layout::Sizes& sized = layout::sizes_of(dialog);
    return x >= 0 && y >= 0 && x < sized.dialog_width && y < sized.dialog_height;
}

namespace {

/// Adds a switch's two halves to the parts, its control on each.
///
/// @param[in,out] parts the parts
/// @param area the switch
/// @param control its control; no_control for one that takes no press
void switch_parts(std::vector<LayoutPart>& parts, const layout::SourceRect& area, int32_t control) {
    const int32_t half = (area.width - 2) / 2;
    parts.push_back(
        LayoutPart{
            {area.x + 1, area.y + 1, half, area.height - 2},
            std::string(layout::off_text),
            DialogFont::small,
            0,
            control,
        }
    );
    parts.push_back(
        LayoutPart{
            {area.x + 1 + half, area.y + 1, half, area.height - 2},
            std::string(layout::on_text),
            DialogFont::small,
            0,
            control,
        }
    );
}

/// Adds the parts under Developer's rows: those of its list's rows that lie
/// wholly in the list's view, and its footer.
///
/// @param dialog the dialog
/// @param open Developer's rows and list (layout::open_rows)
/// @param[in,out] parts the parts
void developer_layout(
    const Dialog& dialog, const layout::ScrolledRows& open, std::vector<LayoutPart>& parts
) {
    // The list's rows, only the parts wholly in its view.
    std::vector<LayoutPart> rows;
    const auto row_text =
        [&rows](layout::SourceRect rect, std::string text, DialogFont font, int32_t control) {
            rows.push_back(LayoutPart{rect, std::move(text), font, 0, control});
        };
    for (const layout::ListRow& row : open.list.rows) {
        const bool takes = row.kind == layout::ListRowKind::area ||
                           row.kind == layout::ListRowKind::hack || !row.locked;
        const int32_t control = takes ? row.control : no_control;
        switch (row.kind) {
        case layout::ListRowKind::area:
            rows.push_back(LayoutPart{row.arrow, {}, DialogFont::regular, 0, no_control});
            row_text(row.label, row.text, DialogFont::regular, control);
            row_text(row.value, row.shown, DialogFont::small, no_control);
            break;
        case layout::ListRowKind::hack:
            rows.push_back(LayoutPart{row.arrow, {}, DialogFont::regular, 0, no_control});
            row_text(row.label, row.text, DialogFont::small, control);
            switch_parts(rows, row.toggle, row.locked ? no_control : row.control);
            break;
        case layout::ListRowKind::id:
        case layout::ListRowKind::text:
        case layout::ListRowKind::scope:
        case layout::ListRowKind::heading:
            row_text(row.label, row.text, DialogFont::small, no_control);
            break;
        case layout::ListRowKind::toggle:
            row_text(row.label, row.text, DialogFont::small, no_control);
            switch_parts(rows, row.control_area, control);
            break;
        case layout::ListRowKind::slider:
            row_text(row.label, row.text, DialogFont::small, no_control);
            row_text(row.value, row.shown, DialogFont::small, no_control);
            rows.push_back(LayoutPart{row.control_area, {}, DialogFont::regular, 0, control});
            break;
        }
    }
    // The list's footer: still at the section's foot, or, where Developer
    // scrolls as one, under the list and listed where the view shows it whole.
    const bool as_one = layout::developer_scrolls_as_one(dialog);
    std::vector<LayoutPart> footer;
    footer.push_back(
        LayoutPart{
            open.active_only_label,
            std::string(
                layout::shown_text(
                    layout::active_only_text(
                        active_hack_count(dialog), oa::data::mod_profile::standard_hacks().size()
                    )
                )
            ),
            DialogFont::regular,
            0,
            no_control
        }
    );
    switch_parts(footer, open.active_only_switch, active_only_control);
    footer.push_back(
        LayoutPart{
            open.restore_profile_button,
            std::string(layout::shown_text(layout::restore_profile_text)),
            DialogFont::small,
            0,
            developer::restore_profile_enabled(dialog) ? restore_profile_control : no_control
        }
    );
    for (LayoutPart& part : rows)
        if (wholly_in(part.rect, open.area.view))
            parts.push_back(std::move(part));
    for (LayoutPart& part : footer)
        if (!as_one || wholly_in(part.rect, open.area.view))
            parts.push_back(std::move(part));
}

} // namespace

std::vector<LayoutPart> dialog_layout(const Dialog& dialog, const DialogFonts* fonts) {
    // Without fonts given, the parts are where the fonts the dialog is drawn
    // in put them, which is where a press finds them.
    if (fonts == nullptr && dialog.drawn_fonts != nullptr)
        return dialog_layout(dialog, dialog.drawn_fonts);
    std::vector<LayoutPart> parts;
    // The player's own folder's path is shortened to its place in the fonts, or at
    // an estimated width a character without them.
    const layout::TextWidth text_width = [fonts](std::string_view text) {
        if (fonts != nullptr)
            return dialog_text_width(*fonts, DialogFont::regular, text);
        return static_cast<int32_t>(oa::ui::kit::character_count(text)) * estimated_character_width;
    };
    // Your files' path is shortened to its hint line in the small font.
    const layout::TextWidth small_text_width = [fonts](std::string_view text) {
        if (fonts != nullptr)
            return dialog_text_width(*fonts, DialogFont::small, text);
        return static_cast<int32_t>(oa::ui::kit::character_count(text)) * estimated_character_width;
    };
    // A real font widens the section list and the footer buttons, and wraps
    // the rows' texts; an estimate keeps the class's sizes, which the layout
    // tests pin. The drawing and a press take the same (shown_section).
    const layout::ShownSection measured = layout::shown_section(dialog, fonts);
    const layout::Sizes& sized = measured.sizes;
    // Each text as the dialog shows it, the interface's words in the
    // language shown (layout::shown_text).
    const auto text_part =
        [&parts](
            layout::SourceRect rect, std::string_view text, DialogFont font, int32_t tracking = 0
        ) {
            parts.push_back(
                LayoutPart{rect, std::string(layout::shown_text(text)), font, tracking, no_control}
            );
        };
    const auto control_part = [&parts](layout::SourceRect rect, int32_t control) {
        parts.push_back(LayoutPart{rect, {}, DialogFont::regular, 0, control});
    };

    // The header: the mark, the title, and at the right the version and the
    // shared game's note.
    control_part(sized.header_mark, no_control);
    const int32_t title_left = sized.header_mark.x + sized.header_mark.width + layout::header_gap;
    text_part(
        {title_left, layout::header_top, layout::title_width, sized.header_height},
        layout::title_text,
        DialogFont::regular,
        layout::heading_tracking
    );
    const int32_t suffix_left = title_left + layout::title_width + layout::header_gap;
    int32_t suffix_width = layout::title_suffix_width;
    if (fonts != nullptr) {
        const int32_t tracked = dialog_text_width(
            *fonts, DialogFont::regular, layout::shown_text(layout::title_suffix_text)
        );
        // Tracking adds a column after every character but the last.
        const int32_t gaps = std::max(
            static_cast<int32_t>(
                oa::ui::kit::character_count(layout::shown_text(layout::title_suffix_text))
            ) - 1,
            int32_t{0}
        );
        suffix_width = std::max(suffix_width, tracked + gaps * layout::heading_tracking);
    }
    text_part(
        {suffix_left, layout::header_top, suffix_width, sized.header_height},
        layout::title_suffix_text,
        DialogFont::regular,
        layout::heading_tracking
    );
    control_part(layout::header_close_button(sized), close_x_control);
    const int32_t shared_left = suffix_left + suffix_width + layout::header_gap;
    // The version keeps a fixed box beside the close mark. While the shared
    // game's note is up, the version gives way and the note takes that box,
    // so the note is not cut against the mark.
    if (dialog.locks.shared_game) {
        text_part(
            {shared_left,
             layout::header_top,
             sized.version_right - layout::version_gap - shared_left,
             sized.header_height},
            layout::shared_game_text,
            DialogFont::small
        );
    } else {
        text_part(
            {sized.version_right - layout::version_width,
             layout::header_top,
             layout::version_width,
             sized.header_height},
            dialog.version,
            DialogFont::small
        );
    }

    // The section list: each entry's caption right of its icon, the rules
    // between its groups, and registered pages' owners. Pieces scrolled out
    // of the list's room are not drawn; their controls stay in the display
    // list, so Tab still reaches them.
    {
        const layout::NavLayout nav = layout::nav_layout(dialog, &sized);
        const int32_t scroll = std::clamp(dialog.nav_scroll, int32_t{0}, nav.limit);
        for (const layout::NavPiece& piece : nav.pieces) {
            layout::SourceRect rect = piece.rect;
            rect.y -= scroll;
            if (piece.kind == layout::NavPiece::Kind::entry) {
                const layout::SourceRect text{
                    rect.x + sized.nav.text_left,
                    rect.y,
                    rect.width - sized.nav.text_left - layout::list_text_margin,
                    rect.height,
                };
                if (!wholly_in(rect, nav.viewport))
                    continue;
                parts.push_back(
                    LayoutPart{text, piece.text, DialogFont::regular, 0, piece.control}
                );
                continue;
            }
            if (!wholly_in(rect, nav.viewport))
                continue;
            if (piece.kind == layout::NavPiece::Kind::rule)
                control_part(rect, no_control);
            else
                parts.push_back(LayoutPart{rect, piece.text, DialogFont::small, 0, no_control});
        }
    }

    // The open section. A registered page's heading is its title as registered.
    if (dialog.page == Page::registered)
        parts.push_back(
            LayoutPart{
                sized.heading,
                registered_heading(dialog),
                DialogFont::small,
                layout::heading_tracking,
                no_control,
            }
        );
    else
        text_part(
            sized.heading,
            layout::page_heading(dialog.page),
            DialogFont::small,
            layout::heading_tracking
        );
    // The rows: only the parts wholly in the view are listed, so that each
    // listed control is pressed where it is drawn and each text is whole.
    const layout::ScrolledRows& open = measured.open;
    const auto row_part = [&parts, &sized](LayoutPart part) {
        if (wholly_in(part.rect, sized.view))
            parts.push_back(std::move(part));
    };
    const auto row_text =
        [&row_part](layout::SourceRect rect, std::string_view text, DialogFont font) {
            row_part(LayoutPart{rect, std::string(layout::shown_text(text)), font, 0, no_control});
        };
    if (dialog.page == Page::registered)
        append_registered_content(dialog, parts, &sized);
    // Every other section's rows as they are drawn, from their row specs:
    // Developer's stay at its top while its list scrolls.
    oa::ui::kit::PlacedRows placed;
    static_cast<void>(layout::place_section(
        dialog,
        dialog.page,
        layout::shown_locks(dialog),
        dialog.section_hooks,
        small_text_width,
        &placed,
        &sized,
        fonts != nullptr ? &text_width : nullptr
    ));
    oa::ui::kit::scroll(
        placed,
        layout::developer_page(dialog) && !layout::developer_scrolls_as_one(dialog) ? 0
                                                                                    : open.scroll
    );
    const layout::SettingsModel model = layout::reading(dialog.chosen, dialog);
    for (std::size_t index = 0; index < placed.rows.size() && index < open.rows.rows.size();
         ++index) {
        const layout::Row& row = open.rows.rows[index];
        const oa::ui::kit::RowView& view = placed.rows[index].view;
        // A locked row's control is drawn but takes no press.
        const int32_t control = row.lock == Lock::none ? row.control : no_control;
        // Updates' card: its lines, its bar and its buttons.
        if (row.updates == 0 && cards::update_card_shown(dialog)) {
            const int32_t margin = kit::metrics_of(sized.size_class).space * 2;
            const cards::UpdateCard card = cards::update_card(
                dialog, sized, placed.rows[index].top + margin, text_width, small_text_width
            );
            for (const cards::Line& line : card.lines)
                row_part(LayoutPart{line.rect, line.text, line.font, 0, no_control});
            if (card.bar.width > 0)
                row_part(LayoutPart{card.bar, {}, DialogFont::regular, 0, no_control});
            for (std::size_t button = 0; button < card.buttons.size(); ++button)
                row_part(
                    LayoutPart{
                        card.buttons[button],
                        card.captions[button],
                        DialogFont::regular,
                        0,
                        updates::card_control(button, control),
                    }
                );
            continue;
        }
        // Key layout is its card: its lines, its badge and its buttons.
        if (row.setting == Setting::key_layout) {
            const int32_t margin = kit::metrics_of(sized.size_class).space * 2;
            const cards::KeyCard card = cards::key_card(
                dialog, sized, placed.rows[index].top + margin, text_width, small_text_width
            );
            for (const cards::Line& line : card.lines)
                row_part(LayoutPart{line.rect, line.text, line.font, 0, no_control});
            if (card.badge.width > 0)
                row_part(LayoutPart{card.badge, card.badge_text, DialogFont::small, 0, no_control});
            if (card.change.width > 0)
                row_part(
                    LayoutPart{
                        card.change,
                        card.change_caption,
                        DialogFont::regular,
                        0,
                        change_layout_control
                    }
                );
            row_part(LayoutPart{card.edit, card.edit_caption, DialogFont::regular, 0, control});
            continue;
        }
        // Key layout and Previous version name a value beside the label.
        // The other buttons of this kind, including Downloads', show a
        // caption and no value part, as they did before Key layout.
        const bool names_value =
            !view.value.empty() && (row.setting == Setting::key_layout || row.updates == 3);
        // A value that shares the label's box keeps the right of that line.
        // The label keeps the left, so the two texts do not cover each other.
        layout::SourceRect label_box = row.label;
        layout::SourceRect value_box = placed.rows[index].value;
        if (names_value && value_box.x == label_box.x && value_box.y == label_box.y &&
            value_box.width == label_box.width && value_box.height == label_box.height) {
            const auto span_of = [&](std::string_view words) {
                return std::max(text_width(words), int32_t{1});
            };
            int32_t value_span = span_of(view.value);
            const int32_t label_span = view.label.empty() ? 0 : span_of(view.label);
            const int32_t room = std::max(label_box.width - layout::label_gap, int32_t{1});
            // Previous version keeps half the line when the two do not fit.
            // Key layout has already shortened its name to the room beside
            // the label, so the value keeps that room rather than half of it.
            if (label_span + layout::label_gap + value_span > label_box.width) {
                if (row.setting == Setting::key_layout)
                    value_span =
                        std::max(label_box.width - label_span - layout::label_gap, int32_t{1});
                else
                    value_span = std::min(value_span, std::max(room / 2, int32_t{1}));
            }
            value_span = std::min(value_span, label_box.width);
            value_box = {
                label_box.x + label_box.width - value_span,
                label_box.y,
                value_span,
                label_box.height
            };
            if (!view.label.empty())
                label_box.width =
                    std::max(value_box.x - layout::label_gap - label_box.x, int32_t{1});
        }
        row_part(LayoutPart{label_box, view.label, DialogFont::regular, 0, no_control});
        if (row.lock != Lock::none) {
            const layout::SourceRect text_area{
                row.lock_area.x + layout::padlock_width + layout::padlock_gap,
                row.lock_area.y,
                row.lock_area.width - layout::padlock_width - layout::padlock_gap,
                row.lock_area.height,
            };
            row_part(
                LayoutPart{
                    {row.lock_area.x, row.lock_area.y, layout::padlock_width, row.lock_area.height},
                    {},
                    DialogFont::regular,
                    0,
                    no_control,
                }
            );
            row_text(text_area, layout::lock_text(row.lock), DialogFont::small);
        }
        for (std::size_t line = 0; line < row.hint_lines && line < view.hints.size(); ++line)
            if (!view.hints[line].empty())
                row_part(
                    LayoutPart{row.hints[line], view.hints[line], DialogFont::small, 0, no_control}
                );
        switch (view.kind) {
        case oa::ui::kit::RowKind::value_and_button:
            // Previous version names the kept release, and Key layout names
            // the layout in use, beside the label. A button with no value
            // shows its caption alone.
            if (names_value)
                row_part(LayoutPart{value_box, view.value, DialogFont::regular, 0, no_control});
            if (!view.buttons.empty())
                row_part(
                    LayoutPart{
                        row.control_area, view.buttons.front(), DialogFont::small, 0, control
                    }
                );
            break;
        case oa::ui::kit::RowKind::buttons:
            if (row.control_area.width > 0)
                for (std::size_t button = 0; button < view.buttons.size(); ++button)
                    row_part(
                        LayoutPart{
                            layout::folder_button(row.control_area, button),
                            view.buttons[button],
                            DialogFont::small,
                            0,
                            control,
                        }
                    );
            break;
        case oa::ui::kit::RowKind::levels:
            if (row.control_area.width > 0)
                for (std::size_t level = 0; level < view.captions.size(); ++level)
                    row_part(
                        LayoutPart{
                            {row.control_area.x + 1 +
                                 static_cast<int32_t>(level) * view.control_width,
                             row.control_area.y + 1,
                             view.control_width,
                             row.control_area.height - 2},
                            view.captions[level],
                            DialogFont::small,
                            0,
                            control,
                        }
                    );
            break;
        case oa::ui::kit::RowKind::choice: {
            // The field shows the choice, in the regular font. Language's
            // field names a pack that is still downloading, and its state.
            std::string caption =
                oa::ui::kit::row_caption(layout::row_spec(row.setting), model, view.index);
            if (row.setting == Setting::language)
                caption = layout::language_closed_field(dialog, std::move(caption));
            if (row.downloads >= 0)
                caption = downloads::choice_caption(downloads::choice_index(dialog));
            if (row.updates >= 0)
                caption = updates::choice_caption(row, updates::choice_index(dialog, row));
            row_part(
                LayoutPart{row.control_area, std::move(caption), DialogFont::regular, 0, control}
            );
            break;
        }
        case oa::ui::kit::RowKind::slider:
            row_part(LayoutPart{row.control_area, {}, DialogFont::regular, 0, control});
            row_part(LayoutPart{row.value, view.value, DialogFont::regular, 0, no_control});
            break;
        case oa::ui::kit::RowKind::toggle:
            if (row.control_area.width > 0) {
                const int32_t half = (row.control_area.width - 2) / 2;
                for (std::size_t side = 0; side < view.captions.size(); ++side)
                    row_part(
                        LayoutPart{
                            {row.control_area.x + 1 + static_cast<int32_t>(side) * half,
                             row.control_area.y + 1,
                             half,
                             row.control_area.height - 2},
                            view.captions[side],
                            DialogFont::small,
                            0,
                            control,
                        }
                    );
            }
            break;
        case oa::ui::kit::RowKind::text_field:
        case oa::ui::kit::RowKind::link:
            // Where the files are and the Steam Input notice show text alone.
            break;
        case oa::ui::kit::RowKind::text:
            if (!view.value.empty())
                row_part(LayoutPart{row.value, view.value, DialogFont::regular, 0, no_control});
            break;
        }
    }
    // Accessibility's practice pad: its lines, its keycaps, the bar and its button.
    if (open.rows.pad.width > 0) {
        const cards::Pad pad = cards::practice_pad(
            dialog,
            sized,
            open.rows.pad.x,
            open.rows.pad.y,
            open.rows.pad.width,
            text_width,
            small_text_width
        );
        for (const cards::Line& line : pad.lines)
            row_part(LayoutPart{line.rect, line.text, line.font, 0, no_control});
        for (const cards::Pad::Cap& cap : pad.caps)
            row_part(LayoutPart{cap.rect, cap.label, DialogFont::regular, 0, no_control});
        if (pad.bar.width > 0)
            row_part(LayoutPart{pad.bar, {}, DialogFont::regular, 0, no_control});
        row_part(
            LayoutPart{
                pad.off,
                pad.off_caption,
                DialogFont::regular,
                0,
                pad.off_enabled ? helpers_off_control : no_control,
            }
        );
    }
    // On Developer, its list and the list's footer under its rows.
    if (layout::developer_page(dialog))
        developer_layout(dialog, open, parts);
    if (open.limit > 0)
        control_part(open.area.well, scroll_bar_control);

    // The footer.
    const std::array<std::pair<int32_t, std::string_view>, 3> buttons{{
        {restore_control, layout::restore_text},
        {cancel_control, layout::cancel_text},
        {ok_control, layout::ok_text},
    }};
    for (const auto& [control, caption] : buttons) {
        const layout::SourceRect rect = control == restore_control  ? sized.restore_button
                                        : control == cancel_control ? sized.cancel_button
                                                                    : sized.ok_button;
        parts.push_back(
            LayoutPart{
                rect,
                std::string(layout::shown_text(caption)),
                control == restore_control ? DialogFont::small : DialogFont::regular,
                0,
                control,
            }
        );
    }
    // How many settings changed, right-aligned against Cancel.
    {
        const std::vector<std::string> count =
            layout::changed_count_lines(dialog, small_text_width, sized.changed_count.width);
        const int32_t line = kit::compact_metrics.small_line;
        const auto lines = static_cast<int32_t>(count.size());
        for (int32_t at = 0; at < lines; ++at)
            parts.push_back(
                LayoutPart{
                    {sized.changed_count.x,
                     sized.changed_count.y + (sized.changed_count.height - lines * line) / 2 +
                         at * line,
                     sized.changed_count.width,
                     line},
                    count[static_cast<std::size_t>(at)],
                    DialogFont::small,
                    0,
                    no_control,
                }
            );
    }
    // An open drop-down list lies over what is under it, which is not listed:
    // its items are.
    if (const auto list = open_list(dialog, open)) {
        const auto overlaps = [&](const LayoutPart& part) {
            const auto& a = part.rect;
            const auto& b = list->rect;
            return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
                   b.y < a.y + a.height;
        };
        parts.erase(std::remove_if(parts.begin(), parts.end(), overlaps), parts.end());
        for (int32_t shown = 0; shown < list->shown; ++shown) {
            const int32_t item = dialog.list_first + shown;
            if (item >= static_cast<int32_t>(list->choices))
                break;
            const layout::SourceRect box = layout::choice_item(list->rect, shown);
            if (list->row->downloads >= 0) {
                parts.push_back(
                    LayoutPart{
                        box,
                        downloads::choice_caption(item),
                        DialogFont::regular,
                        0,
                        no_control,
                    }
                );
                continue;
            }
            if (list->row->updates >= 0) {
                parts.push_back(
                    LayoutPart{
                        box,
                        updates::choice_caption(*list->row, item),
                        DialogFont::regular,
                        0,
                        no_control,
                    }
                );
                continue;
            }
            const Setting setting = list->row->setting;
            const std::string caption =
                oa::ui::kit::row_caption(layout::row_spec(setting), model, item);
            // The divider is a section heading, not a choice.
            if (layout::language_item_divider(dialog, setting, item)) {
                parts.push_back(
                    LayoutPart{
                        box,
                        caption,
                        DialogFont::small,
                        layout::heading_tracking,
                        no_control,
                    }
                );
                continue;
            }
            const std::string aside = layout::language_item_aside(dialog, setting, item);
            if (aside.empty()) {
                parts.push_back(LayoutPart{box, caption, DialogFont::regular, 0, no_control});
                continue;
            }
            // The same gap the menu draws between a label and its aside, so
            // the two parts do not meet.
            constexpr int32_t kMargin = 4;
            constexpr int32_t kGap = 8;
            constexpr int32_t kMinLabel = 8;
            const int32_t inset = kit::compact_metrics.choice_item_text_inset;
            const int32_t inner_left = box.x + inset;
            const int32_t inner_right = box.x + box.width - kMargin;
            int32_t aside_width = text_width(aside);
            int32_t room = inner_right - inner_left - kMinLabel - kGap;
            if (room < 1)
                room = 1;
            if (aside_width > room)
                aside_width = room;
            const int32_t aside_left = inner_right - aside_width;
            const int32_t label_width = std::max(aside_left - kGap - inner_left, int32_t{0});
            parts.push_back(
                LayoutPart{
                    {inner_left, box.y, label_width, box.height},
                    caption,
                    DialogFont::regular,
                    0,
                    no_control,
                }
            );
            parts.push_back(
                LayoutPart{
                    {aside_left, box.y, aside_width, box.height},
                    aside,
                    DialogFont::regular,
                    0,
                    no_control,
                }
            );
        }
    }
    return parts;
}

} // namespace oa::ui::engine_settings
