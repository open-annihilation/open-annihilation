// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Where the settings dialog puts each of its parts, in source pixels from
// its top left corner, and the texts it shows. The events (dialog.cpp) and
// the drawing (dialog_draw.cpp) both place things through these functions,
// so a control is pressed where it is drawn.
#pragma once

#include "oa/data/mod_profile/value.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/rows.hpp"
#include "oa/ui/kit/theme.hpp"

#include "settings_rows.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::engine_settings::geometry {

using oa::data::mod_profile::Value;
using oa::ui::frontend_renderer::SourceRect;

// The parts that keep their places and sizes in every size class come first;
// then the parts that move with the class (Sizes, sizes_of), and their
// Compact values under the names 0.7.3's code and tests know them by.

/// The width of the dialog's raised edge.
inline constexpr int32_t edge = oa::ui::kit::compact_metrics.edge;
/// The header's height, under the top edge.
inline constexpr int32_t header_height = oa::ui::kit::compact_metrics.header_height;
/// The footer's height, over the bottom edge.
inline constexpr int32_t footer_height = oa::ui::kit::compact_metrics.footer_height;
/// The header's first row.
inline constexpr int32_t header_top = edge;
/// The row of the line between the header and the body.
inline constexpr int32_t header_rule_row = header_top + header_height;
/// The body's first row, under the header's line.
inline constexpr int32_t body_top = header_rule_row + 1;

/// The header's Open Annihilation icon, in the header's middle rows; or,
/// without the icon, the OA mark's outlined square in its middle. It keeps
/// Compact's place from the dialog's left edge in every size class.
inline constexpr SourceRect header_mark{
    oa::ui::kit::compact_metrics.padding,
    oa::ui::kit::compact_metrics.mark_top,
    oa::ui::kit::compact_metrics.mark_side,
    oa::ui::kit::compact_metrics.mark_side
};
/// The side of the OA mark's outlined square, which stands in for the icon.
inline constexpr int32_t header_mark_square = oa::ui::kit::compact_metrics.mark_square;
/// The space between the header's mark and the title, and between the
/// title's two words.
inline constexpr int32_t header_gap = oa::ui::kit::compact_metrics.header_gap;
/// Extra columns after each glyph of the title and the section heading.
inline constexpr int32_t heading_tracking = oa::ui::kit::compact_metrics.heading_tracking;

/// A section's entry in the list: its left column. Its height, the gaps and
/// its icon's and caption's columns are its size class's (Sizes).
inline constexpr int32_t list_item_left = edge + 6;
/// The selected entry's marker: its column within the entry, width and height.
inline constexpr int32_t list_marker_offset = 5;
/// The selected entry's marker's width.
inline constexpr int32_t list_marker_width = 2;
/// The selected entry's marker's height.
inline constexpr int32_t list_marker_height = 8;
/// The columns an entry keeps clear right of its text.
inline constexpr int32_t list_text_margin = 4;
/// The columns between the list's sides and a rule between its groups.
inline constexpr int32_t list_divider_inset = 4;

/// The section list's sizes at one size class: its entries, the gaps between
/// them and between their groups, where an entry's icon and caption stand,
/// and the columns the list takes over the class's nav width.
struct NavMetrics {
    int32_t entry_height{}; ///< an entry's height
    int32_t entry_gap{};    ///< the rows between two entries of a group
    int32_t rule_margin{};  ///< the rows above and below a rule between two groups
    int32_t margin{};       ///< the rows above the first entry and below the last
    int32_t icon_left{};    ///< the icon's column within its entry
    int32_t icon_side{};    ///< the icon's side
    int32_t text_left{};    ///< the caption's column within its entry
    /// The columns the list is wider than the class's (kit::Metrics::nav_width),
    /// so that each section's name stands whole right of its icon.
    int32_t extra_width{};
};

/// The section list's sizes at Compact, Regular and Large, in the order of
/// kit::SizeClass. Compact's list of eleven sections fits its room; Regular's
/// and Large's fit every section the engine lists.
inline constexpr std::array<NavMetrics, 3> nav_metrics{{
    {19, 0, 3, 6, 7, 9, 19, 0},
    {25, 1, 4, 8, 12, 14, 33, 0},
    {32, 1, 5, 10, 14, 16, 38, 0},
}};

/// The open section's heading's top row.
inline constexpr int32_t heading_top = body_top + 10;
/// The open section's heading's height.
inline constexpr int32_t heading_height = 12;
/// The first row's top, where its line is drawn.
inline constexpr int32_t first_row_top = heading_top + heading_height + 4;
/// The rows between a row's line and its label, and under its last part.
inline constexpr int32_t row_padding = 8;
/// A row's label line: the label, and a switch, a level strip or a lock.
inline constexpr int32_t label_line_height = oa::ui::kit::compact_metrics.regular_line;
/// The rows between the label line and the first hint line.
inline constexpr int32_t hint_gap = 2;
/// A hint line's height.
inline constexpr int32_t hint_line_height = oa::ui::kit::compact_metrics.small_line;
/// The rows added between two lines of a hint, or of a notice, while the
/// dialog's words are drawn in the modern fonts, whose ideographs stand as
/// tall as a hint line: one line's letters, outline and shadow then keep
/// clear of the next's.
inline constexpr int32_t tall_hint_line_gap = 3;
/// The most lines a hint takes before it is wrapped to the box.
inline constexpr std::size_t most_hint_lines = 2;
/// The most lines a notice row's text takes under its label: Controller's
/// Steam Input notice, before a wider script wraps it further.
inline constexpr std::size_t most_notice_lines = 4;
/// The most lines a wrapped hint or notice keeps under its label. A whole
/// text in another language can take more lines than the two a Compact
/// English hint was broken into.
inline constexpr std::size_t most_wrapped_lines = 12;
/// The most lines stored under any row's label.
inline constexpr std::size_t most_row_lines = most_wrapped_lines;
/// The rows between the last hint line and a slider.
inline constexpr int32_t slider_gap = 4;
/// A slider line's height: the track with its knob and stops, and the value.
inline constexpr int32_t slider_line_height = 14;
/// The width of a slider's value, right of its track.
inline constexpr int32_t slider_value_width = 110;
/// The columns between a slider's track and its value.
inline constexpr int32_t slider_value_gap = 10;
/// The columns kept clear between a label and the control or lock beside it,
/// and between a lock and the switch it stands beside.
inline constexpr int32_t label_gap = 8;
/// An Off/On switch's width; each half is half of it, inside a 1-pixel border.
inline constexpr int32_t switch_width = oa::ui::kit::compact_metrics.switch_width;
/// Enhanced anti-aliasing's level strip's segment width, inside the strip's
/// 1-pixel border.
inline constexpr int32_t level_width = 23;
/// Hardware acceleration's level strip's segment width, inside the strip's
/// 1-pixel border: room for Basic, its widest caption, with three clear
/// columns each side.
inline constexpr int32_t acceleration_level_width = 34;
/// One-finger drag's level strip's segment width, inside the strip's
/// 1-pixel border: room for Automatic, its widest caption, with three clear
/// columns each side.
inline constexpr int32_t touch_drag_level_width = 59;
/// QUEUE and ADD's level strip's segment width, inside the strip's 1-pixel
/// border: room for One action, its widest caption, with three clear
/// columns each side.
inline constexpr int32_t touch_latches_level_width = 64;
/// Order wheel's level strip's segment width, inside the strip's 1-pixel
/// border: room for Hold middle, its widest caption, with three clear
/// columns each side.
inline constexpr int32_t order_wheel_level_width = 76;
/// Control size's level strip's segment width, inside the strip's 1-pixel
/// border: room for Standard, its widest caption, with three clear columns
/// each side.
inline constexpr int32_t control_size_level_width = 53;
/// Scheme's level strip's segment width: room for Trackpads, its widest
/// caption, with three clear columns each side.
inline constexpr int32_t scheme_level_width = 60;
/// Right trackpad's level strip's segment width: room for Absolute, its
/// widest caption, with three clear columns each side.
inline constexpr int32_t right_trackpad_level_width = 51;
/// Pointer acceleration's level strip's segment width: Hardware
/// acceleration's, so that the two strips of three levels from Off look
/// alike; High, its widest caption, fits with room to spare.
inline constexpr int32_t pad_acceleration_level_width = acceleration_level_width;
/// View past the map's edge's level strip's segment width: Hardware
/// acceleration's, so that the strips of three levels from Off look alike;
/// 25% and 50%, its widest captions, fit with room to spare.
inline constexpr int32_t view_past_map_edge_level_width = acceleration_level_width;
/// Right stick's level strip's segment width: room for Nothing, its widest
/// caption, with three clear columns each side.
inline constexpr int32_t right_stick_level_width = 46;
/// The Controller section's Haptics' level strip's segment width: room for
/// Strong, its widest caption, with three clear columns each side.
inline constexpr int32_t pad_haptics_level_width = 41;
/// Menu scaling's level strip's segment width, inside the strip's 1-pixel
/// border: room for Whole steps, its widest caption, with three clear
/// columns each side.
inline constexpr int32_t menu_scaling_level_width = 71;
/// Explosion flash's level strip's segment width, inside the strip's
/// 1-pixel border: room for Reduced, its widest caption, with three clear
/// columns each side.
inline constexpr int32_t explosion_flash_level_width = 50;
/// Fog of War's level strip's segment width, inside the strip's 1-pixel
/// border: room for Classique and Klassisch, its widest captions, with
/// three clear columns each side.
inline constexpr int32_t fog_of_war_level_width = 56;
/// Zoomed out units' level strip's segment width, inside the strip's
/// 1-pixel border: room for Rendered, its widest caption, with three clear
/// columns each side.
inline constexpr int32_t zoomed_out_units_level_width = 56;
/// Window edge's level strip's segment width, inside the strip's 1-pixel
/// border: room for Capture, its widest caption, with three clear columns
/// each side.
inline constexpr int32_t window_edge_level_width = 50;
/// Window frame's level strip's segment width, inside the strip's 1-pixel
/// border: room for Hidden in play, its widest caption, with three clear
/// columns each side.
inline constexpr int32_t window_frame_level_width = 85;
/// MANAGE…'s width, Game files' button on its summary row's label line:
/// room for its caption in the small font with clear columns each side.
inline constexpr int32_t manage_button_width = 76;
/// A lock's width: the padlock and its text, right-aligned on the label line.
inline constexpr int32_t lock_width = 148;
/// The padlock's width.
inline constexpr int32_t padlock_width = oa::ui::kit::compact_metrics.padlock_width;
/// The padlock's height.
inline constexpr int32_t padlock_height = oa::ui::kit::compact_metrics.padlock_height;
/// The columns between the padlock and its text.
inline constexpr int32_t padlock_gap = oa::ui::kit::compact_metrics.padlock_gap;
/// The columns between a control and its keyboard focus outline.
inline constexpr int32_t focus_inset = oa::ui::kit::compact_metrics.focus_inset;

/// A slider's knob: its width and height.
inline constexpr int32_t knob_width = oa::ui::kit::compact_metrics.knob_width;
/// A slider knob's height.
inline constexpr int32_t knob_height = oa::ui::kit::compact_metrics.knob_height;
/// A slider track's height and its top within the slider line.
inline constexpr int32_t track_height = oa::ui::kit::compact_metrics.track_height;
/// A slider track's top row within the slider line.
inline constexpr int32_t track_offset = oa::ui::kit::compact_metrics.track_offset;
/// A stop mark's height and its top within the slider line.
inline constexpr int32_t stop_height = oa::ui::kit::compact_metrics.stop_height;
/// A stop mark's top row within the slider line.
inline constexpr int32_t stop_offset = oa::ui::kit::compact_metrics.stop_offset;
/// Stop marks are drawn only this many columns or more apart.
inline constexpr int32_t least_stop_spacing = oa::ui::kit::compact_metrics.least_stop_spacing;

/// The footer's buttons' height and top row.
inline constexpr int32_t button_height = oa::ui::kit::compact_metrics.button_height;
/// Restore defaults' width.
inline constexpr int32_t restore_button_width = 110;

using oa::ui::engine_settings::page_count;

/// The clear rows under the last row's line at the end of a section, so
/// that at its end the line never meets the footer's.
inline constexpr int32_t end_gap = row_padding;
/// The scroll bar's thumb's width.
inline constexpr int32_t scroll_thumb_width = oa::ui::kit::compact_metrics.scroll_thumb_width;
/// The scroll bar's thumb's least height.
inline constexpr int32_t least_thumb_height = oa::ui::kit::compact_metrics.least_thumb_height;
/// The rows a notch of the mouse wheel scrolls: two hint lines.
inline constexpr int32_t wheel_step = 2 * hint_line_height;

/// Where a section's rows scroll: the view, the scroll bar's well, the
/// margin a press holds the bar in, and the rows Page Up and Page Down move.
struct ScrollArea {
    SourceRect view{};   ///< what the rows are seen through
    SourceRect well{};   ///< the scroll bar's well, as high as the view
    SourceRect hit{};    ///< where a press holds the scroll bar
    int32_t page_step{}; ///< the rows Page Up and Page Down scroll
};

// Developer: its rows under the heading, Enable Developer Mode and Show
// performance statistics, placed closer than a section's so that the list
// under them has room; Developer Mode's list of the standard hacks in a view
// of its own; and a footer of its own with Show Active Only and Restore
// profile values. Only the list scrolls.

/// The rows between the line of each of Developer's rows and its label, and
/// under its last part: half a section's.
inline constexpr int32_t developer_row_padding = row_padding / 2;
/// The height of each of Developer's rows, from its line to the next row's:
/// its label line and one hint line.
inline constexpr int32_t developer_row_height = 1 + developer_row_padding + label_line_height +
                                                hint_gap + hint_line_height + developer_row_padding;
/// UI kit gallery's hint, in English, which the dialog shows through the
/// interface catalogue: one line at Regular and Large, two at Compact.
inline constexpr std::string_view kit_gallery_hint_text =
    "Every part of OA's own screens, in each of its states, at this size.";
/// The most lines UI kit gallery's hint takes.
inline constexpr std::size_t kit_gallery_hint_most_lines = 2;

/// Counts the lines a text of single-byte characters takes broken between
/// words into lines of at most a number of characters, as break_lines
/// breaks such a text: at its last space where a word would pass the line's
/// end, a word longer than a line cut there.
///
/// @param text the text, of single-byte characters and no location mark
/// @param characters the most characters a line holds, 1 or more
/// @return the lines; 0 for a text of spaces alone
[[nodiscard]] constexpr std::size_t
lines_between_words(std::string_view text, std::size_t characters) noexcept {
    characters = characters > 0 ? characters : 1;
    std::size_t lines = 0;
    std::size_t start = 0;
    while (start < text.size()) {
        while (start < text.size() && text[start] == ' ')
            ++start;
        if (start >= text.size())
            break;
        std::size_t end = start + characters < text.size() ? start + characters : text.size();
        if (end < text.size() && text[end] != ' ') {
            const std::size_t space = text.substr(start, end - start).rfind(' ');
            if (space != std::string_view::npos && space > 0)
                end = start + space;
        }
        ++lines;
        start = end;
    }
    return lines;
}

/// Returns the row of the line over Developer's list at a size class: under
/// its rows, each a label line and one hint line, but UI kit gallery's,
/// whose hint in English takes as many lines as the class's lines hold.
/// Where the rows end lower still, as in another language or in the modern
/// fonts, the list starts under them (developer_list_scroll).
///
/// @param hint_line_characters the most characters a hint line holds at the class
/// @return the row
[[nodiscard]] constexpr int32_t developer_rule_for(std::size_t hint_line_characters) noexcept {
    std::size_t gallery_lines = lines_between_words(kit_gallery_hint_text, hint_line_characters);
    gallery_lines = gallery_lines < 1 ? 1 : gallery_lines;
    gallery_lines =
        gallery_lines > kit_gallery_hint_most_lines ? kit_gallery_hint_most_lines : gallery_lines;
    return first_row_top + developer_row_count * developer_row_height +
           static_cast<int32_t>(gallery_lines - 1) * hint_line_height;
}

/// The rows between the footer's line, its two rows and the dialog's footer.
inline constexpr int32_t developer_footer_gap = 4;
/// Restore profile values' width.
inline constexpr int32_t restore_profile_width = 142;

/// The parts of the dialog that move with its size class, in points from its
/// top left corner: its size, its padding and its section list's width
/// (kit::metrics_of), and the content column, the body, the footer, the
/// views and the boxes that follow from them. The content column starts
/// right of the section list and ends the padding short of the right edge;
/// the body is the height between the header's line and the footer's. Rows,
/// texts and controls keep their sizes, so a larger class shows more rows.
struct Sizes {
    oa::ui::kit::SizeClass size_class{}; ///< the class these are the sizes of
    int32_t dialog_width{};              ///< the dialog's width
    int32_t dialog_height{};             ///< the dialog's height
    /// the space between a panel's edge and what it holds: Kit 2's window
    /// padding (kit::Metrics::window_padding)
    int32_t padding{};
    int32_t list_width{};               ///< the section list's width, from the left edge
    NavMetrics nav{};                   ///< the section list's entries, gaps and icons
    std::size_t hint_line_characters{}; ///< the most characters a host's line under a row holds
    int32_t footer_rule_row{};          ///< the row of the line between the body and the footer
    int32_t footer_top{};               ///< the footer's first row
    /// the footer's height, under its line: Kit 2's window footer
    /// (kit::Metrics::window_footer_height)
    int32_t footer_height{};
    /// the footer's buttons' height: Kit 2's control height (kit::Metrics::control_height)
    int32_t footer_button_height{};
    int32_t list_rule_column{}; ///< the column of the line right of the section list
    int32_t content_left{};     ///< the open section's first column
    int32_t content_right{};    ///< the column just right of the open section
    int32_t content_width{};    ///< the open section's width
    int32_t list_item_width{};  ///< a section's entry's width
    /// The column just right of the header's version: Compact's distance from
    /// the dialog's right edge in every class.
    int32_t version_right{};
    SourceRect heading{};        ///< the open section's heading
    int32_t button_top{};        ///< the footer's buttons' top row
    SourceRect restore_button{}; ///< Restore defaults, at the footer's left
    SourceRect ok_button{};      ///< OK, at the footer's right
    SourceRect cancel_button{};  ///< Cancel, left of OK
    /// The count of changed settings: the footer's room between Restore
    /// defaults and Cancel, the text right-aligned against Cancel.
    SourceRect changed_count{};
    /// The view the open section's rows scroll in, under its heading: from
    /// the first row's line to the row above the footer's line.
    SourceRect view{};
    /// What the rows are drawn clipped to: the view, wider on each side by a
    /// focus outline.
    SourceRect view_clip{};
    /// The scroll bar's well: in the margin right of the rows, one clear
    /// column right of a focus outline, as high as the view.
    SourceRect scroll_well{};
    SourceRect scroll_hit{}; ///< where a press holds the scroll bar: the margin right of the rows
    /// The rows Page Up and Page Down scroll: the view less three hint
    /// lines, so that what showed at one edge still shows at the other.
    int32_t page_step{};
    ScrollArea section_scroll{}; ///< where a section's rows scroll
    /// The row of the line over Developer's list, under its rows (developer_rule_for).
    int32_t developer_list_rule{};
    int32_t developer_footer_rule{}; ///< the row of the line over the footer under Developer's list
    SourceRect developer_view{}; ///< the view Developer's list scrolls in, between its two lines
    SourceRect developer_view_clip{};    ///< what Developer's list is drawn clipped to
    SourceRect active_only_label{};      ///< Show Active Only's label, in Developer's footer
    SourceRect active_only_switch{};     ///< Show Active Only's switch, at the footer's right
    SourceRect restore_profile_button{}; ///< Restore profile values, under Show Active Only
    ScrollArea developer_scroll{};       ///< where Developer's list scrolls
};

/// Works out the dialog's sizes at a size class from the class's metrics.
///
/// @param size_class the class
/// @return its sizes; Compact's are 0.7.3's
[[nodiscard]] constexpr Sizes make_sizes(oa::ui::kit::SizeClass size_class) noexcept {
    const oa::ui::kit::Metrics& metrics = oa::ui::kit::metrics_of(size_class);
    Sizes sized{};
    sized.size_class = size_class;
    sized.dialog_width = metrics.dialog_width;
    sized.dialog_height = metrics.dialog_height;
    sized.padding = metrics.window_padding;
    sized.nav = nav_metrics[std::min(static_cast<std::size_t>(size_class), nav_metrics.size() - 1)];
    sized.list_width = metrics.nav_width + sized.nav.extra_width;
    sized.hint_line_characters = static_cast<std::size_t>(metrics.hint_line_characters);
    sized.footer_height = metrics.window_footer_height;
    sized.footer_button_height = metrics.control_height;
    sized.footer_rule_row = sized.dialog_height - edge - sized.footer_height - 1;
    sized.footer_top = sized.footer_rule_row + 1;
    sized.list_rule_column = edge + sized.list_width;
    sized.content_left = sized.list_rule_column + 1 + sized.padding;
    sized.content_right = sized.dialog_width - edge - sized.padding;
    sized.content_width = sized.content_right - sized.content_left;
    sized.list_item_width = sized.list_width - 12;
    sized.version_right = sized.dialog_width - edge - oa::ui::kit::compact_metrics.padding -
                          oa::ui::kit::compact_metrics.button_height -
                          oa::ui::kit::compact_metrics.header_gap;
    sized.heading = {sized.content_left, heading_top, sized.content_width, heading_height};
    sized.button_top = sized.footer_top + (sized.footer_height - sized.footer_button_height) / 2;
    // Kit 2's buttons: as tall as the class's controls, OK and Cancel twice
    // as wide as they are tall, Restore defaults wider by as much as its
    // height grew over Compact's.
    const int32_t grown = metrics.control_height - oa::ui::kit::compact_metrics.control_height;
    const int32_t answer_width =
        std::max(oa::ui::kit::compact_metrics.button_width, 2 * metrics.control_height);
    sized.restore_button = {
        sized.padding,
        sized.button_top,
        restore_button_width + 2 * grown,
        sized.footer_button_height
    };
    sized.ok_button = {
        sized.dialog_width - edge - sized.padding - answer_width,
        sized.button_top,
        answer_width,
        sized.footer_button_height
    };
    sized.cancel_button = {
        sized.ok_button.x - metrics.space * 2 - answer_width,
        sized.button_top,
        answer_width,
        sized.footer_button_height
    };
    sized.changed_count = {
        sized.restore_button.x + sized.restore_button.width + metrics.space * 3,
        sized.button_top,
        sized.cancel_button.x - metrics.space * 3 -
            (sized.restore_button.x + sized.restore_button.width + metrics.space * 3),
        sized.footer_button_height
    };
    sized.view = {
        sized.content_left,
        first_row_top,
        sized.content_width,
        sized.footer_rule_row - first_row_top
    };
    sized.view_clip = {
        sized.view.x - focus_inset,
        sized.view.y,
        sized.view.width + 2 * focus_inset,
        sized.view.height
    };
    sized.scroll_well = {
        sized.content_right + focus_inset + 1,
        sized.view.y,
        scroll_thumb_width + 2,
        sized.view.height
    };
    sized.scroll_hit = {sized.content_right, sized.view.y, sized.padding, sized.view.height};
    sized.page_step = sized.view.height - 3 * hint_line_height;
    sized.section_scroll = {sized.view, sized.scroll_well, sized.scroll_hit, sized.page_step};
    sized.developer_list_rule = developer_rule_for(sized.hint_line_characters);
    sized.developer_footer_rule = sized.footer_rule_row - 2 - button_height - developer_footer_gap -
                                  label_line_height - developer_footer_gap;
    sized.developer_view = {
        sized.content_left,
        sized.developer_list_rule + 1,
        sized.content_width,
        sized.developer_footer_rule - sized.developer_list_rule - 1
    };
    sized.developer_view_clip = {
        sized.developer_view.x - focus_inset,
        sized.developer_view.y,
        sized.developer_view.width + 2 * focus_inset,
        sized.developer_view.height
    };
    sized.active_only_label = {
        sized.content_left,
        sized.developer_footer_rule + developer_footer_gap,
        sized.content_width - switch_width - label_gap,
        label_line_height
    };
    sized.active_only_switch = {
        sized.content_right - switch_width,
        sized.developer_footer_rule + developer_footer_gap,
        switch_width,
        label_line_height
    };
    sized.restore_profile_button = {
        sized.content_left,
        sized.active_only_label.y + label_line_height + developer_footer_gap,
        restore_profile_width,
        button_height
    };
    sized.developer_scroll = {
        sized.developer_view,
        {sized.scroll_well.x,
         sized.developer_view.y,
         sized.scroll_well.width,
         sized.developer_view.height},
        {sized.scroll_hit.x,
         sized.developer_view.y,
         sized.scroll_hit.width,
         sized.developer_view.height},
        sized.developer_view.height - 3 * hint_line_height,
    };
    return sized;
}

/// The dialog's sizes at each size class, Compact, Regular and Large, in
/// the order of kit::SizeClass.
inline constexpr std::array<Sizes, 3> class_sizes{
    make_sizes(oa::ui::kit::SizeClass::compact),
    make_sizes(oa::ui::kit::SizeClass::regular),
    make_sizes(oa::ui::kit::SizeClass::large),
};

/// Returns the dialog's sizes at a size class.
///
/// @param size_class the class
/// @return its sizes
[[nodiscard]] constexpr const Sizes& sizes_of(oa::ui::kit::SizeClass size_class) noexcept {
    const auto index = static_cast<std::size_t>(size_class);
    return class_sizes[index < class_sizes.size() ? index : 0];
}

/// Returns the dialog's sizes at the size class it is laid out at
/// (Dialog::size_class).
///
/// @param dialog the dialog
/// @return its sizes
[[nodiscard]] inline const Sizes& sizes_of(const Dialog& dialog) noexcept {
    return sizes_of(dialog.size_class);
}

/// The dialog's sizes at Compact: 0.7.3's. The names below are its parts at
/// Compact, as 0.7.3's code and the tests know them.
inline constexpr const Sizes& compact_sizes = class_sizes[0];

/// The row of the line between the body and the footer, at Compact.
inline constexpr int32_t footer_rule_row = compact_sizes.footer_rule_row;
/// The footer's first row, at Compact.
inline constexpr int32_t footer_top = compact_sizes.footer_top;
/// The section list's width, from the left edge, at Compact.
inline constexpr int32_t list_width = compact_sizes.list_width;
/// The column of the line between the section list and the open section, at Compact.
inline constexpr int32_t list_rule_column = compact_sizes.list_rule_column;
/// The space between a panel's edge and what it holds, at Compact.
inline constexpr int32_t padding = compact_sizes.padding;
/// The open section's first column, at Compact.
inline constexpr int32_t content_left = compact_sizes.content_left;
/// The column just right of the open section and the header's version, at Compact.
inline constexpr int32_t content_right = compact_sizes.content_right;
/// The open section's width, at Compact.
inline constexpr int32_t content_width = compact_sizes.content_width;
/// A section's entry's width, at Compact.
inline constexpr int32_t list_item_width = compact_sizes.list_item_width;
/// The open section's heading, at Compact.
inline constexpr SourceRect heading = compact_sizes.heading;
/// The most characters a line of a host's text under a row holds, broken
/// between words, at Compact: what fits the section's width in the small font.
inline constexpr std::size_t hint_line_characters = compact_sizes.hint_line_characters;
/// The footer's buttons' top row, at Compact.
inline constexpr int32_t button_top = compact_sizes.button_top;
/// Restore defaults, at the footer's left, at Compact.
inline constexpr SourceRect restore_button = compact_sizes.restore_button;
/// OK, at the footer's right, at Compact.
inline constexpr SourceRect ok_button = compact_sizes.ok_button;
/// Cancel, left of OK, at Compact.
inline constexpr SourceRect cancel_button = compact_sizes.cancel_button;
/// The view the open section's rows scroll in, under its heading, at Compact.
inline constexpr SourceRect view = compact_sizes.view;
/// What the rows are drawn clipped to, at Compact.
inline constexpr SourceRect view_clip = compact_sizes.view_clip;
/// The scroll bar's well, at Compact; the thumb runs inside its one-pixel border.
inline constexpr SourceRect scroll_well = compact_sizes.scroll_well;
/// Where a press holds the scroll bar, at Compact: the whole margin right of the rows.
inline constexpr SourceRect scroll_hit = compact_sizes.scroll_hit;
/// The rows Page Up and Page Down scroll, at Compact.
inline constexpr int32_t page_step = compact_sizes.page_step;
/// Where a section's rows scroll at Compact; Developer's list scrolls in
/// developer_scroll.
inline constexpr ScrollArea section_scroll = compact_sizes.section_scroll;
/// The row of the line over Developer's list, under its rows, at Compact.
inline constexpr int32_t developer_list_rule = compact_sizes.developer_list_rule;
/// The row of the line over the footer under Developer's list, at Compact.
inline constexpr int32_t developer_footer_rule = compact_sizes.developer_footer_rule;
/// The view Developer's list scrolls in, between the two lines, at Compact.
inline constexpr SourceRect developer_view = compact_sizes.developer_view;
/// What Developer's list is drawn clipped to, at Compact.
inline constexpr SourceRect developer_view_clip = compact_sizes.developer_view_clip;
/// Show Active Only's label, in the footer, at Compact.
inline constexpr SourceRect active_only_label = compact_sizes.active_only_label;
/// Show Active Only's switch, at the footer's right, at Compact.
inline constexpr SourceRect active_only_switch = compact_sizes.active_only_switch;
/// Restore profile values, under Show Active Only, at Compact.
inline constexpr SourceRect restore_profile_button = compact_sizes.restore_profile_button;
/// Where Developer's list scrolls, at Compact.
inline constexpr ScrollArea developer_scroll = compact_sizes.developer_scroll;

/// An area's or a hack's header in the list: its height.
inline constexpr int32_t list_header_height = 20;
/// A line of text under a hack (its summary, its scope, a note): its height.
inline constexpr int32_t list_text_height = hint_line_height;
/// A parameter's switch row: its height.
inline constexpr int32_t list_toggle_height = 20;
/// A parameter's slider row, its name and value over the slider: its height.
inline constexpr int32_t list_slider_height = 2 + hint_line_height + 2 + slider_line_height + 2;
/// The line naming a set or a list over its items: its height.
inline constexpr int32_t list_heading_height = hint_line_height + 2;
/// The rows over a hack's first line of text, and under its last row.
inline constexpr int32_t list_body_gap = 2;
/// The column of an area's arrow, from content_left.
inline constexpr int32_t area_arrow_offset = 1;
/// The column of an area's name, from content_left.
inline constexpr int32_t area_text_offset = 10;
/// The column of a hack's arrow, from content_left.
inline constexpr int32_t hack_arrow_offset = 9;
/// The column of a hack's id and of what is under it, from content_left.
inline constexpr int32_t hack_text_offset = 18;
/// The column of a set's values and a list's items, from content_left.
inline constexpr int32_t item_text_offset = 28;
/// An arrow's side: closed it points right, open it points down.
inline constexpr int32_t arrow_side = 5;
/// The columns an area's count of hacks that are on keeps, at the row's right.
inline constexpr int32_t area_count_width = 80;
/// The most characters a line of a hack's summary holds, broken between
/// words: what fits the columns under a hack in the small font.
inline constexpr std::size_t summary_line_characters = 45;

/// A drop-down's field: its width. It stands on its own line under the
/// hint, as a slider's track does.
inline constexpr int32_t choice_width = 200;
/// A wide drop-down's field: room for Gyro pointer's longest choice, While
/// the right stick is touched, in the regular font with the field's inset
/// and arrow.
inline constexpr int32_t wide_choice_width = 248;
/// A drop-down field's height.
inline constexpr int32_t choice_line_height = label_line_height;
/// The columns between a drop-down field's left edge and its text.
inline constexpr int32_t choice_text_inset = oa::ui::kit::compact_metrics.choice_text_inset;
/// The columns a drop-down field keeps at its right for its arrow.
inline constexpr int32_t choice_arrow_room = oa::ui::kit::compact_metrics.choice_arrow_room;
/// The drop-down's arrow's width.
inline constexpr int32_t choice_arrow_width = oa::ui::kit::compact_metrics.choice_arrow_width;
/// The drop-down's arrow's height.
inline constexpr int32_t choice_arrow_height = oa::ui::kit::compact_metrics.choice_arrow_height;
/// An open drop-down list's item's height.
inline constexpr int32_t choice_item_height = oa::ui::kit::compact_metrics.choice_item_height;
/// The most items an open drop-down list shows at once; a longer list
/// scrolls.
inline constexpr int32_t most_shown_choices = oa::ui::kit::compact_metrics.most_shown_choices;
/// The columns between an open list item's left edge and its text, right of
/// the marker the chosen item shows.
inline constexpr int32_t choice_item_text_inset =
    oa::ui::kit::compact_metrics.choice_item_text_inset;

/// What a slider offers: its stops' count.
struct Slider {
    int32_t stops{}; ///< 2 or more
};

/// What a level strip offers: its levels' count and their width.
struct Strip {
    std::size_t levels{};  ///< 2 or more, left to right
    int32_t level_width{}; ///< each level's columns, inside the strip's 1-pixel border
    /// The levels a player may choose, counted from the left; the others are
    /// shown faded and choosing them changes nothing. 0 offers every level.
    std::size_t offered{};
};

/// Returns the levels of a strip a player may choose, from its left.
///
/// @param strip the strip
/// @return Strip::offered, or every level where it is 0
[[nodiscard]] constexpr std::size_t offered_levels(const Strip& strip) noexcept {
    return strip.offered == 0 ? strip.levels : std::min(strip.offered, strip.levels);
}

/// One row of the open section, placed.
struct Row {
    Setting setting{}; ///< what it changes
    /// Its place among Downloads' rows, from 0; -1 for every other section's row.
    int16_t downloads{-1};
    /// Its place among Updates' rows, from 0; -1 for every other section's row.
    int16_t updates{-1};
    /// Its place among Game Content's rows, from 0; -1 for every other section's row.
    int16_t content{-1};
    int32_t control{no_control}; ///< its control's number
    Lock lock{};                 ///< why it cannot be changed now
    bool hint_is_status{};       ///< its hint lines are its status, which a lock never fades
    int32_t top{};               ///< the row of its line
    int32_t height{};            ///< rows from its line to the next row's
    SourceRect label{};          ///< its label
    SourceRect lock_area{};      ///< its padlock and lock text; empty when unlocked
    std::array<SourceRect, most_row_lines> hints{}; ///< its hint's or notice's lines
    std::size_t hint_lines{};                       ///< the lines its hint takes
    /// Its switch, level strip, button or slider track. A lock never
    /// clears it: the control stays and is drawn faded.
    SourceRect control_area{};
    SourceRect value{}; ///< a slider's value; empty for the others
};

/// The text the dialog shows for one of its own words: the interface
/// catalogue's translation into the language shown
/// (oa/data/languages/interface_text.hpp), or the word itself.
///
/// @param english the word, in English as the source writes it
/// @return the text to show
[[nodiscard]] std::string_view shown_text(std::string_view english);

/// Returns a text the dialog shows, looked up whole in the language shown
/// (shown_text), with its places ({name}) filled; the values are not looked
/// up, and a place no value names stays as it is written.
///
/// @param english the text, in English
/// @param places each place's name and value
/// @return the text
[[nodiscard]] std::string filled(
    std::string_view english,
    std::initializer_list<std::pair<std::string_view, std::string_view>> places
);

/// The open section's rows, placed.
struct Rows {
    std::vector<Row> rows; ///< one for each setting the section shows
    /// The row of the line under the last row, or on Accessibility under the
    /// practice pad where that reaches lower: where the section's content ends.
    int32_t bottom{};
    /// Accessibility's practice pad's place (cards::practice_pad); empty on
    /// every other section.
    SourceRect pad{};
};

/// What a row of Developer's list is.
enum class ListRowKind : uint8_t {
    area,    ///< an area's header: its title and how many of its hacks are on
    hack,    ///< a hack's header: its title and its switch
    id,      ///< the first line under an open hack: its id, for reference
    text,    ///< a line under a hack: its summary or a note
    scope,   ///< "Applies at next match", under a rule (sim-scope) hack
    toggle,  ///< a parameter's switch, or one value of a set's
    slider,  ///< a parameter's slider: a number, a choice, a list's length or one of its items
    heading, ///< the name of a set or a list, over its items
};

/// ListRow::item for a row that changes its whole parameter.
inline constexpr int32_t whole_parameter = -1;
/// ListRow::item for a list's length.
inline constexpr int32_t list_length = -2;

/// One row of Developer's list, placed.
struct ListRow {
    ListRowKind kind{};
    int32_t control{no_control};   ///< its control; no_control for a line or a heading
    std::size_t area{};            ///< its area's place among developer_areas
    std::size_t hack{};            ///< its hack's place among the standard hacks; an area's first
    int32_t parameter{-1};         ///< its parameter's index within its hack; -1 for none
    int32_t item{whole_parameter}; ///< a list's item or a set's value, or list_length
    int32_t top{};                 ///< its first row
    int32_t height{};              ///< its rows
    std::string text;              ///< its title, id, name or line
    SourceRect label{};            ///< where its text is drawn
    /// Its switch or slider track; a header's whole row, which a press opens
    /// or closes, its switch apart.
    SourceRect control_area{};
    SourceRect toggle{}; ///< a hack's switch, inside its header
    SourceRect value{};  ///< a slider's value, or an area's count of hacks that are on
    SourceRect arrow{};  ///< a header's open or closed mark
    std::string shown;   ///< a slider's value, or an area's count, as the row shows it
    bool open{};         ///< a header's part is open
    bool on{};           ///< a hack's or a switch's state
    int32_t stops{};     ///< a slider's stops, 1 or more
    int32_t stop{};      ///< the stop a slider's knob is on
    /// It shows but takes no change: Developer Mode is off, or the row's hack
    /// is off or not implemented. A header still opens and closes.
    bool locked{};
};

/// Developer's list, placed.
struct List {
    std::vector<ListRow> rows; ///< the rows the open areas and hacks show, top to bottom
    int32_t bottom{};          ///< the row under the last row
};

/// The open section's rows placed at its scroll offset, and the offset's range.
struct ScrolledRows {
    /// Placed `scroll` rows higher than at the section's top; Developer's
    /// stay at its top, over its list.
    Rows rows;
    List list;                       ///< Developer's list, placed `scroll` rows higher
    ScrollArea area{section_scroll}; ///< where they scroll
    int32_t scroll{};                ///< the offset, 0 to `limit`
    int32_t limit{};                 ///< the most the section scrolls; 0 when its rows fit the view
    int32_t content_height{}; ///< rows from the first row's line to the end gap under the last
};

/// Cuts a text to a width, ending it with "..." when it is cut.
///
/// @param text the text, in UTF-8
/// @param width the room, in source pixels
/// @param text_width a text's width in the font it is drawn in
/// @return the text, or as much of it as fits before "..."
[[nodiscard]] std::string cut_text(
    std::string_view text, int32_t width, const std::function<int32_t(std::string_view)>& text_width
);

// The functions from here to hint_is_status read a setting's row in the
// table (row_spec): each is the kit's reading of the row's spec, over a
// dialog that offers what its arguments give.

/// Returns what a slider setting offers at most: its row's stops, each snap
/// radius up to the most any mod allows.
///
/// @param setting a slider setting
/// @param highest_offered_unit the unit limit slider's highest value, in
///     units per player
/// @param offered_sizes the Screen size slider's stops, in order
///     (Dialog::offered_screen_sizes); never empty
/// @return its stops
[[nodiscard]] Slider slider_of(
    Setting setting,
    uint16_t highest_offered_unit = highest_unit_limit,
    std::span<const ScreenSize> offered_sizes = screen_sizes
) noexcept;

/// Returns the stops a slider setting's row offers for the settings shown: a
/// snap radius runs from 0 to the mod's most (at least two stops), the
/// others as slider_of gives them.
///
/// @param settings the settings shown
/// @param setting a slider setting
/// @param highest_offered_unit the unit limit slider's highest value, in
///     units per player
/// @param offered_sizes the Screen size slider's stops, in order
///     (Dialog::offered_screen_sizes); never empty
/// @return the stops, 1 or more; 2 or more but for a Screen size slider of
///     one stop
[[nodiscard]] int32_t stops_of(
    const EngineSettings& settings,
    Setting setting,
    uint16_t highest_offered_unit = highest_unit_limit,
    std::span<const ScreenSize> offered_sizes = screen_sizes
) noexcept;

/// Returns the stop nearest a setting's value, as its row reads it.
///
/// @param settings the settings
/// @param setting a slider setting
/// @param highest_offered_unit the unit limit slider's highest value, in
///     units per player
/// @param offered_sizes the Screen size slider's stops, in order
///     (Dialog::offered_screen_sizes); a screen size not among them is on
///     the first
/// @return 0 for the lowest value to stops - 1 for the highest
[[nodiscard]] int32_t stop_of(
    const EngineSettings& settings,
    Setting setting,
    uint16_t highest_offered_unit = highest_unit_limit,
    std::span<const ScreenSize> offered_sizes = screen_sizes
) noexcept;

/// Sets a slider setting to a stop's value, through its row.
///
/// @param[in,out] settings the settings
/// @param setting a slider setting
/// @param stop the stop, clamped to the slider's
/// @param highest_offered_unit the unit limit slider's highest value, in
///     units per player
/// @param offered_sizes the Screen size slider's stops, in order
///     (Dialog::offered_screen_sizes); never empty
void set_stop(
    EngineSettings& settings,
    Setting setting,
    int32_t stop,
    uint16_t highest_offered_unit = highest_unit_limit,
    std::span<const ScreenSize> offered_sizes = screen_sizes
) noexcept;

/// Returns what a strip setting's row offers.
///
/// @param setting a strip setting
/// @return its levels and their width; no levels for any other setting
[[nodiscard]] Strip strip_of(Setting setting) noexcept;

/// Returns the level a strip setting's row shows.
///
/// @param settings the settings
/// @param setting a strip setting
/// @return its level's index, from 0 at the strip's left
[[nodiscard]] std::size_t strip_level(const EngineSettings& settings, Setting setting) noexcept;

/// Sets a strip setting to a level, through its row: a level the strip
/// shows but does not offer changes nothing.
///
/// @param[in,out] settings the settings
/// @param setting a strip setting; any other is left alone
/// @param level the level's index
void set_strip_level(EngineSettings& settings, Setting setting, std::size_t level) noexcept;

/// Returns a level's caption in a strip, in English: its row spec's.
///
/// @param setting a strip setting
/// @param level the level's index
/// @return "Off", "2x", "Basic", "Automatic", "Stay on" and so on; empty
///     past the strip's last, and for a setting that is no strip
[[nodiscard]] std::string strip_caption(Setting setting, std::size_t level);

/// Returns the languages the Language drop-down offers after System
/// default: the playable languages, a built-in or an installed pack, in
/// the registry's order. A language that is not installed is not offered.
///
/// @return the languages
[[nodiscard]] std::span<const oa::data::languages::Language* const> offered_languages();

/// Returns how many choices a drop-down's row offers.
///
/// @param dialog the dialog
/// @param setting a drop-down setting
/// @return the items; for Language, System default, the offered languages
///     and, when packs can be downloaded, a divider and those packs; 0 for
///     a setting that is no drop-down
[[nodiscard]] std::size_t choice_count(const Dialog& dialog, Setting setting);

/// Returns a drop-down's choice as the drop-down names it: Language's first
/// is System default with the operating system's language named in itself,
/// "System default (Deutsch)", and the others each language named in itself.
/// An available pack is named in itself, or in English when its endonym
/// cannot be drawn. The divider reads AVAILABLE TO DOWNLOAD.
///
/// @param dialog the dialog, whose mods and system language the choices name
/// @param setting a drop-down setting
/// @param index the choice, from 0
/// @return the text, in UTF-8; empty past the last choice
[[nodiscard]] std::string choice_text(const Dialog& dialog, Setting setting, std::size_t index);

/// Returns the choice a drop-down shows.
///
/// @param dialog the dialog, whose chosen settings count
/// @param setting a drop-down setting
/// @return the choice, from 0; 0, System default, for a value that is neither
///     offered nor available to download
[[nodiscard]] std::size_t choice_index(const Dialog& dialog, Setting setting);

/// Sets a drop-down setting to a choice, through its row.
///
/// @param[in,out] dialog the dialog, whose chosen settings change
/// @param setting a drop-down setting; any other is left alone
/// @param index the choice, clamped to the drop-down's
void set_choice(Dialog& dialog, Setting setting, std::size_t index);

/// Returns where a drop-down's open list lies: under its field, its left
/// edge with the field's, as wide as the field and as tall as the items it
/// shows, at most most_shown_choices, with a one-pixel border; over the
/// field when it would reach below the footer's line.
///
/// @param field the drop-down's field
/// @param choices the choices it offers
/// @param size_class the size class the dialog is laid out at, whose footer's line it keeps above
/// @return the list's rectangle
[[nodiscard]] inline SourceRect choice_list(
    const SourceRect& field,
    std::size_t choices,
    oa::ui::kit::SizeClass size_class = oa::ui::kit::SizeClass::compact
) noexcept {
    return oa::ui::kit::choice_menu(field, choices, sizes_of(size_class).footer_rule_row);
}

/// Returns how many items an open list shows at once.
///
/// @param choices the choices it offers
/// @return at most most_shown_choices
[[nodiscard]] inline int32_t shown_choices(std::size_t choices) noexcept {
    return oa::ui::kit::shown_choices(choices);
}

/// Returns one shown item's rectangle in an open list.
///
/// @param list the list (choice_list)
/// @param shown the item's place among those shown, from 0
/// @return the item's rectangle, inside the list's border
[[nodiscard]] inline SourceRect choice_item(const SourceRect& list, int32_t shown) noexcept {
    return oa::ui::kit::choice_item(list, shown);
}

/// The columns between two of Your files' buttons.
inline constexpr int32_t folder_button_gap = 4;
/// Each of Your files' buttons' width, left to right: room for its caption
/// with clear columns each side.
inline constexpr std::array<int32_t, folder_button_count> folder_button_widths{46, 82, 44};
/// The width of Your files' buttons together, with the gaps between them.
inline constexpr int32_t folder_buttons_width = folder_button_widths[0] + folder_button_widths[1] +
                                                folder_button_widths[2] + 2 * folder_button_gap;

/// Returns one of Your files' buttons within the row's control area.
///
/// @param area the row's control area: the buttons together
/// @param index the button, from 0 at the left
/// @return its rectangle
[[nodiscard]] SourceRect folder_button(const SourceRect& area, std::size_t index) noexcept;

/// Returns the button of Your files under a point.
///
/// @param area the row's control area
/// @param x the point's column
/// @param y the point's row
/// @return the button, from 0 at the left; folder_button_count for none,
///     as between two buttons
[[nodiscard]] std::size_t folder_button_at(const SourceRect& area, int32_t x, int32_t y) noexcept;

/// Tells whether a switch setting is On, as its row reads it: each switch's
/// row binds its own value, so a new switch is added in one place.
///
/// @param settings the settings
/// @param setting a switch setting
/// @return true for On; false for a setting that is not a switch
[[nodiscard]] bool switch_on(const EngineSettings& settings, Setting setting) noexcept;

/// Sets a switch setting, through its row as switch_on reads it.
///
/// @param[in,out] settings the settings
/// @param setting a switch setting; any other is left alone
/// @param on true for On
void set_switch(EngineSettings& settings, Setting setting, bool on) noexcept;

/// What a dialog shows beyond each setting's own rows and lines.
struct RowContext {
    /// Controller's first row is the Steam Input notice (Dialog::steam_input).
    bool steam_input{};
    /// Maximum frame rate's hint has a second line naming a Steam Deck's
    /// screen rate (Dialog::steam_deck_panel_hz); 0 for none.
    uint32_t steam_deck_panel_hz{};
    /// Language lists MORE IN THE LIBRARY… under its row
    /// (Dialog::library_link, for the engine's settings).
    bool library_link{};
    /// System lists the file-type switches (Dialog::file_type_switches).
    /// Off, it shows the text row. The default is this system's own.
    bool file_type_switches{file_types_switchable};
    /// Controls lists Key layout with the mod being played
    /// (Dialog::played_mod). Off, that row is left out.
    bool played_mod{};
};

/// Returns what a dialog shows beyond each setting's own rows and lines.
///
/// @param dialog the dialog
/// @return its Steam Input notice, its Steam Deck's screen rate, whether
///     Language links to the Library, and whether System lists the switches
[[nodiscard]] RowContext row_context(const Dialog& dialog) noexcept;

/// Tells whether a setting's hint lines are its status, as its row spec
/// says: such a row, locked, keeps its control, faded, and fades only its
/// label line and that control, so the status keeps its strength.
///
/// @param setting the setting
/// @return true for Hardware acceleration, whose hint lines are its status
[[nodiscard]] bool hint_is_status(Setting setting) noexcept;

/// Returns the settings a section shows, a check's own section's when given.
///
/// @param page the section
/// @param section a check's own section; null for the dialog's
/// @param context what the dialog shows beyond each setting's rows:
///     Controller starts with the Steam Input notice while it applies, and
///     Language has MORE IN THE LIBRARY… under its row while it links to
///     the Library
/// @return its settings, top to bottom
[[nodiscard]] std::span<const Setting>
section_settings(Page page, const SectionHooks* section, const RowContext& context = {});

/// A text's width in a font of the dialog, in source pixels.
using TextWidth = std::function<int32_t(std::string_view)>;

/// Returns a text's width at estimated_character_width a character, as the
/// rows are placed where no font measures their words.
///
/// @param text the text, in UTF-8
/// @return the width, in source pixels
[[nodiscard]] int32_t estimated_width(std::string_view text);

/// Tells whether a measure is the stand-in that counts every character the same.
///
/// @param measure a text's width
/// @return true when n and W are both the estimated character width
[[nodiscard]] bool estimated_measure(const TextWidth& measure);

/// Returns a setting's row as the dialog shows it, read from its row spec:
/// the spec's view, with the lock the dialog puts on the row, whether its
/// hint lines are its status, a switch a language sets showing On, and each
/// hint line looked up in the language shown once more, as the dialog has
/// always drawn its hints, but a folder's path (hint_is_path), shown as its
/// tail that fits the section's width.
///
/// @param dialog the dialog the row is shown in, whose choice it shows
/// @param setting the row's setting
/// @param lock the row's lock (section_lock)
/// @param status its hint lines are its status (section_hint_is_status)
/// @param small_width a text's width in the small font
/// @param content_width the section's width; a real font wraps a hint to it
/// @return the row's view
[[nodiscard]] oa::ui::kit::RowView row_view(
    const Dialog& dialog,
    Setting setting,
    Lock lock,
    bool status,
    const TextWidth& small_width,
    int32_t content_width,
    const TextWidth* regular_width = nullptr
);

/// Places a section's rows at its top, through the kit's placement of their
/// views: from first_row_top, Developer's own rows with
/// developer_row_padding and every other section's with row_padding, a
/// hint's lines further apart while the dialog's words are drawn in the
/// modern fonts, and the first row's control first_row_control.
///
/// @param dialog the dialog the rows are shown in
/// @param page the section
/// @param locks the locks the rows show
/// @param section a check's own section; null for the dialog's
/// @param small_width a text's width in the small font, for a folder's path
/// @param[out] placed the kit's placement of the rows, their views with
///     them; may be null
/// @param sizes the sizes the rows are placed in; null for the class's own
/// @return the rows
[[nodiscard]] Rows place_section(
    const Dialog& dialog,
    Page page,
    const Locks& locks,
    const SectionHooks* section,
    const TextWidth& small_width,
    oa::ui::kit::PlacedRows* placed = nullptr,
    const Sizes* sizes = nullptr,
    const TextWidth* regular_width = nullptr
);

/// Places the rows of a section; Developer's own, over its list, with
/// developer_row_padding.
///
/// A locked row keeps its control, drawn faded. Its lock stands on the
/// label line, left of a control that sits there, otherwise at the line's
/// right. When the label and the lock do not fit on that line, on any row,
/// the lock moves to the start of the line under the label, before the hint,
/// and the label keeps the line, or the room beside a control that stays.
/// A row whose hint lines are its status fades that line and a lock or a
/// control that dropped under it, and stops before the status.
///
/// @param page the section
/// @param locks the dialog's locks
/// @param scroll the rows the section is scrolled by from its top; not clamped
/// @param section a check's own section; null for the dialog's
/// @param context what the dialog shows beyond each setting's rows and
///     lines (row_context)
/// @return its rows
[[nodiscard]] Rows place_rows(
    Page page,
    const Locks& locks,
    int32_t scroll = 0,
    const SectionHooks* section = nullptr,
    const RowContext& context = {}
);

/// Moves placed rows up: each row's line and every part it has, and the
/// line under the last row. An empty part stays empty.
///
/// @param[in,out] rows the rows
/// @param by the rows they move up; negative moves them down
void scroll_rows(Rows& rows, int32_t by) noexcept;

/// Returns a section's content height: from its first row's line to the
/// end gap under its last row's.
///
/// @param rows its rows, placed at any offset
/// @param scroll the offset they are placed at
/// @return the height, in rows
[[nodiscard]] int32_t content_height(const Rows& rows, int32_t scroll) noexcept;

/// Returns the most a section scrolls.
///
/// @param content_height its content height (content_height)
/// @param size_class the size class the dialog is laid out at, whose view it scrolls in
/// @return the rows its content is taller than the view; 0 when it fits
[[nodiscard]] int32_t scroll_limit(
    int32_t content_height, oa::ui::kit::SizeClass size_class = oa::ui::kit::SizeClass::compact
) noexcept;

/// Returns the locks the open section's rows show: the dialog's, and Text
/// size locked Lock::needs_modern_fonts while the dialog shows Use modern
/// fonts for game text Off.
///
/// @param dialog the dialog
/// @return the locks
[[nodiscard]] Locks shown_locks(const Dialog& dialog) noexcept;

/// The dialog's sizes, widened for the font in use where a section name or a
/// footer button is wider than the box the class gives it. An estimated
/// measure, or no measure, keeps the class's sizes.
///
/// @param dialog the dialog
/// @param regular a text's width in the regular font; null to estimate
/// @param small a text's width in the small font; null to estimate
/// @return the sizes
[[nodiscard]] Sizes
fitted_sizes(const Dialog& dialog, const TextWidth* regular, const TextWidth* small);

/// Places the open section's rows once, at its offset clamped to its limit,
/// with the locks shown_locks gives.
///
/// @param dialog the dialog
/// @param small_width a text's width in the small font; null to estimate
/// @param sizes the sizes the rows are placed in; null for the class's own
/// @return its rows, offset, limit and content height
[[nodiscard]] ScrolledRows open_rows(
    const Dialog& dialog,
    const TextWidth* small_width = nullptr,
    const Sizes* sizes = nullptr,
    const TextWidth* regular_width = nullptr
);

/// Tells whether a row of Developer's list takes a press and the focus:
/// every header, which opens and closes, and a parameter's control while it
/// can change.
///
/// @param row the row
/// @return true when it does
[[nodiscard]] bool list_row_takes_input(const ListRow& row) noexcept;

/// The scroll group of the open section's rows. The arrows move the focus
/// within a control's own group first (kit::focus_toward).
inline constexpr int32_t section_group = 0;
/// The scroll group of Developer's list.
inline constexpr int32_t developer_list_group = 1;

/// The control of an open drop-down list's first item: the items count down
/// from it, under no_control, by their places in the list.
inline constexpr int32_t first_menu_item_control = no_control - 1;

/// Returns the control of an open drop-down list's item.
///
/// @param item the item, from 0
/// @return its control
[[nodiscard]] constexpr int32_t menu_item_control(int32_t item) noexcept {
    return first_menu_item_control - item;
}

/// Returns the offset nearest the open one that shows a row whole: from its
/// line to the line under it, or for the last row the section's end.
///
/// @param open the open section's rows (open_rows)
/// @param index the row, from 0
/// @return the offset, 0 to the section's limit; the open one for no such row
[[nodiscard]] int32_t scroll_showing(const ScrolledRows& open, std::size_t index) noexcept;

/// Tells whether a dialog of the engine's settings shows Developer's list
/// and its footer under Developer's rows: Developer is open, and no check's
/// own section takes its place (SectionHooks::settings).
///
/// @param dialog the dialog
/// @return true while it shows the list
[[nodiscard]] bool developer_page(const Dialog& dialog) noexcept;

/// Places Developer's list: the areas, and under each open one its
/// hacks, and under each open hack its summary, its scope, its notes and its
/// parameters, as Show Active Only filters them.
///
/// @param dialog the dialog
/// @param scroll the rows the list is scrolled by from its top; not clamped
/// @param switch_columns a measured Off/On switch; 0 keeps the class's switch
/// @param small_width a text's width in the small font; null keeps the class's boxes
/// @param regular_width a text's width in the regular font; null keeps an area
///     name beside its count
/// @param sizes the dialog's sizes; null uses the class's own
/// @return the list
[[nodiscard]] List place_list(
    const Dialog& dialog,
    int32_t scroll = 0,
    int32_t switch_columns = 0,
    const TextWidth* small_width = nullptr,
    const TextWidth* regular_width = nullptr,
    const Sizes* sizes = nullptr
);

/// Returns where Developer's list scrolls: its size class's
/// (Sizes::developer_scroll), whose top is the row under the class's
/// developer_list_rule, moved down to the row under Developer's rows
/// wherever they end lower, as in a language whose hints take more lines or
/// while a hint's lines lie further apart in the modern fonts. Its height,
/// its well and the margin a press holds the bar in shrink by as much, and
/// its page step.
///
/// @param dialog the dialog, on Developer
/// @return the view, the well, the margin and the page step
[[nodiscard]] ScrollArea developer_list_scroll(
    const Dialog& dialog,
    const TextWidth* small_width = nullptr,
    const Sizes* sizes = nullptr,
    const TextWidth* regular_width = nullptr
);

/// Moves a placed list up, every part of every row with it.
///
/// @param[in,out] list the list
/// @param by the rows it moves up; negative moves it down
void scroll_list(List& list, int32_t by) noexcept;

/// Returns the offset nearest the open one that shows one of the list's
/// rows whole.
///
/// @param open Developer's list (open_rows)
/// @param control the row's control
/// @return the offset, 0 to the list's limit; the open one for no such row
[[nodiscard]] int32_t list_scroll_showing(const ScrolledRows& open, int32_t control) noexcept;

/// Returns the row of a list's control.
///
/// @param list the list
/// @param control the control
/// @return the row; null when no row of the list is the control
[[nodiscard]] const ListRow* list_row(const List& list, int32_t control) noexcept;

/// Returns the lines a summary is broken into: at most
/// summary_line_characters bytes each, broken between words, a word longer
/// than a line after one of its slashes or else at the line's end, with
/// each character the game's fonts lack written as ASCII. A summary with
/// Chinese, Japanese or Korean characters, which stay, breaks where the
/// line breaker lets it (oa::base::text::first_row), never inside a
/// character.
///
/// @param summary the summary, UTF-8
/// @return the lines, in order
[[nodiscard]] std::vector<std::string> summary_lines(std::string_view summary);

/// Returns a text in the characters the game's fonts hold: a degree sign,
/// a middle dot, a multiplication sign and a plus-minus sign as " degrees",
/// "*", "x" and "+/-", Chinese, Japanese and Korean characters as they are,
/// for the modern fonts to draw, and any other character outside printable
/// ASCII as "?".
///
/// @param text the text, UTF-8
/// @return the text in printable ASCII and those characters
[[nodiscard]] std::string ascii_text(std::string_view text);

/// Returns a value as a list's slider shows it: a number with its unit,
/// "none", a word of an enumeration or a string.
///
/// @param value the value
/// @param unit what a number counts, such as "ticks"; empty for none
/// @return the text
[[nodiscard]] std::string list_value_text(const Value& value, std::string_view unit);

/// Returns the scroll bar's thumb: inside the well's border, as tall as the
/// view's share of the content and never under least_thumb_height, and as
/// far down its travel as the offset is down the limit, to the nearest row.
///
/// @param scroll the offset, 0 to `limit`
/// @param limit the section's limit, above 0
/// @param content_height the section's content height
/// @return the thumb
[[nodiscard]] SourceRect
scroll_thumb(int32_t scroll, int32_t limit, int32_t content_height) noexcept;

/// Returns the scroll bar's thumb in a scroll area, as scroll_thumb places
/// it in a section's.
///
/// @param area where the section scrolls
/// @param scroll the offset, 0 to `limit`
/// @param limit the section's limit, above 0
/// @param content_height the section's content height
/// @return the thumb
[[nodiscard]] SourceRect scroll_thumb(
    const ScrollArea& area, int32_t scroll, int32_t limit, int32_t content_height
) noexcept;

/// Returns the offset that puts the scroll bar's thumb's top at a row, to
/// the nearest row.
///
/// @param thumb_top the thumb's top row, clamped to its travel
/// @param limit the section's limit, above 0
/// @param content_height the section's content height
/// @return the offset, 0 to `limit`
[[nodiscard]] int32_t scroll_at(int32_t thumb_top, int32_t limit, int32_t content_height) noexcept;

/// Returns the offset that puts the scroll bar's thumb's top at a row in a
/// scroll area, as scroll_at does in a section's.
///
/// @param area where the section scrolls
/// @param thumb_top the thumb's top row, clamped to its travel
/// @param limit the section's limit, above 0
/// @param content_height the section's content height
/// @return the offset, 0 to `limit`
[[nodiscard]] int32_t scroll_at(
    const ScrollArea& area, int32_t thumb_top, int32_t limit, int32_t content_height
) noexcept;

/// The groups the engine's sections are listed in, top to bottom, a rule
/// between each two: what the player plays with, how the game looks, what
/// it holds and fetches, and the machine and the engine.
enum class NavGroup : uint8_t {
    play,    ///< Controls, Accessibility, Touch and Controller
    look,    ///< Graphics, User Interface and Language & Text
    content, ///< Game files, Downloads, Updates and Game Content
    system,  ///< Engine Limits, System and Developer
};

/// Returns the group a section is listed in. A mod's options, a Language
/// dialog's one section and a registered page are in the first group.
///
/// @param page the section
/// @return its group
[[nodiscard]] NavGroup nav_group(Page page) noexcept;

/// One piece of the section list before it scrolls: an entry, a rule between
/// two groups or before an owner's pages, or an owner's name.
struct NavPiece {
    /// What a piece is.
    enum class Kind : uint8_t {
        entry, ///< a section's entry
        rule,  ///< a hairline
        owner, ///< an owner's name
    };

    Kind kind{Kind::entry};      ///< what it is
    SourceRect rect{};           ///< where it lies before the list scrolls
    int32_t control{no_control}; ///< the entry's control; no_control for a rule or a name
    std::string text;            ///< the entry's caption or the owner's name
    bool registered{};           ///< the entry is a registered page
    std::size_t index{};         ///< the registered page's index, when registered
    Page page{Page::controls};   ///< the engine section, when the entry is one
};

/// The section list, before it scrolls: its pieces, the room its entries
/// show in, its face and how far it scrolls.
struct NavLayout {
    std::vector<NavPiece> pieces; ///< top to bottom
    SourceRect viewport{};        ///< entries listed wholly in here
    SourceRect area{};            ///< the list's face, what drawing clips to
    int32_t limit{};              ///< the greatest scroll, in source pixels; 0 when it fits
};

/// Lays out a dialog's section list: its sections (dialog_pages) in their
/// groups, a rule between two groups, then each registered page's owner on
/// a rule of its own, named, over that owner's pages in registration order.
/// Every entry is the class's height; the list scrolls as one, by
/// Dialog::nav_scroll, once its pieces pass the room above the footer.
///
/// @param dialog the dialog
/// @param sizes the dialog's sizes; null uses the class's own
/// @return the pieces, the viewport, the face and the scroll limit
[[nodiscard]] NavLayout nav_layout(const Dialog& dialog, const Sizes* sizes = nullptr);

/// Scrolls the section list the least that shows the focused entry, or the
/// open section's entry when no entry has the focus. A row's focus leaves
/// the scroll. A list that does not scroll is put back at the top. Nothing
/// moves while a press is held.
///
/// @param[in,out] dialog the dialog
void reveal_engine_nav(Dialog& dialog);

/// Returns a section's entry in the list of the engine's settings or a mod's
/// options, before the list scrolls, as nav_layout places it in a dialog of
/// that kind listing the optional sections asked for.
///
/// @param page the section
/// @param touch the dialog lists Touch (Dialog::touch)
/// @param game_files the dialog lists Game files (Dialog::game_files)
/// @param controller the dialog lists Controller (Dialog::controller)
/// @param size_class the size class the dialog is laid out at
/// @return its rectangle; an empty one at the list's top for a section not listed
[[nodiscard]] SourceRect list_item(
    Page page,
    bool touch = false,
    bool game_files = false,
    bool controller = false,
    oa::ui::kit::SizeClass size_class = oa::ui::kit::SizeClass::compact
);

/// MANAGE…'s caption.
inline constexpr std::string_view manage_text = "MANAGE…";
/// The caption of Restore profile values.
inline constexpr std::string_view restore_profile_text = "RESTORE PROFILE VALUES";
/// What a rule (sim-scope) hack says under its summary.
inline constexpr std::string_view next_match_text = "Applies at next match";
/// What a hack that is off says under its summary.
inline constexpr std::string_view hack_off_text = "Off: it plays as 3.1c.";
/// What a hack this engine does not implement says under its summary.
inline constexpr std::string_view not_implemented_text = "Not implemented by this engine.";
/// The name of a list's length.
inline constexpr std::string_view items_text = "Items";

/// Returns Show Active Only's label.
///
/// @param active the hacks that are on (active_hack_count)
/// @param total every standard hack
/// @return "Show Active Only (X/Y)"
[[nodiscard]] std::string active_only_text(std::size_t active, std::size_t total);

/// Returns a footer button's rectangle.
///
/// @param control restore_control, cancel_control or ok_control
/// @param size_class the size class the dialog is laid out at
/// @return its rectangle
[[nodiscard]] SourceRect footer_button(
    int32_t control, oa::ui::kit::SizeClass size_class = oa::ui::kit::SizeClass::compact
) noexcept;

/// Returns the column a slider's knob is centred on.
///
/// @param track the slider's track area
/// @param stop the stop
/// @param stops the slider's stops
/// @return the column
[[nodiscard]] inline int32_t
knob_column(const SourceRect& track, int32_t stop, int32_t stops) noexcept {
    return oa::ui::kit::knob_column(track, stop, stops);
}

/// Returns the stop nearest a column on a slider.
///
/// @param track the slider's track area
/// @param column the column
/// @param stops the slider's stops
/// @return the stop, 0 to stops - 1
[[nodiscard]] inline int32_t
stop_at(const SourceRect& track, int32_t column, int32_t stops) noexcept {
    return oa::ui::kit::stop_at(track, column, stops);
}

/// Returns the level of a strip under a column.
///
/// @param area the strip's control area
/// @param strip what the strip offers
/// @param column the column
/// @return the level's index, from 0 at the strip's left to its last; the
///     nearest end for a column outside it
[[nodiscard]] inline std::size_t
level_at(const SourceRect& area, const Strip& strip, int32_t column) noexcept {
    return oa::ui::kit::level_at(area, strip.levels, strip.level_width, column);
}

/// Returns a section's name, as its list entry shows it: the table's.
///
/// @param page the section
/// @return the name
[[nodiscard]] std::string_view page_name(Page page) noexcept;

/// Returns a section's heading, as the open section shows it: the table's.
///
/// @param page the section
/// @return the heading, in capitals
[[nodiscard]] std::string_view page_heading(Page page) noexcept;

/// Returns a setting's label: its row spec's.
///
/// @param setting the setting
/// @return the label, in English
[[nodiscard]] std::string_view label_of(Setting setting) noexcept;

/// A line of the text under a row, and whether it is drawn as a notice.
struct HintLine {
    std::string text; ///< the line; empty past the last
    /// Drawn in the notice colour, as locks are: what the player must know
    /// of the choice shown.
    bool notice{};
};

/// Returns a hint's line as its row spec gives it, with no dialog's own
/// texts: in English, but Include in device backups' line naming the
/// neutral device, Controller's Steam Input notice in the language shown,
/// and the Game files rows and Your files' path empty.
///
/// @param setting the setting
/// @param settings the settings shown; the anti-aliasing hint depends on its level
/// @param acceleration Hardware acceleration's status, which is its hint
/// @param line the line, from 0
/// @return the line; empty past the hint's last
[[nodiscard]] std::string hint_line(
    Setting setting,
    const EngineSettings& settings,
    const AccelerationStatus& acceleration,
    std::size_t line
);

/// Returns a line of Hardware acceleration's status: the first says what
/// runs, or why not; the second what draws the view, what the player can
/// do, or, while it is in use, what it does on this machine.
///
/// @param acceleration the status
/// @param line the line, 0 or 1
/// @return the line; empty past the second
[[nodiscard]] std::string_view
status_line(const AccelerationStatus& acceleration, std::size_t line) noexcept;

/// Returns the lines a setting's hint takes, as its row spec gives them.
///
/// @param setting the setting
/// @return 1 or 2; most_notice_lines for the Steam Input notice
[[nodiscard]] std::size_t hint_line_count(Setting setting) noexcept;

/// Returns the lines a row's hint takes in a dialog, as its row spec gives
/// them: hint_line_count, and on a Steam Deck one more for Maximum frame
/// rate, which names the rate it starts at.
///
/// @param setting the setting
/// @param context what the dialog shows beyond each setting's lines
/// @return 1 or 2; most_notice_lines for the Steam Input notice
[[nodiscard]] std::size_t hint_line_count(Setting setting, const RowContext& context) noexcept;

/// The text of Controller's Steam Input notice, which its row breaks into
/// lines between words.
inline constexpr std::string_view steam_input_notice_text =
    "Steam Input is on: the trackpads and back grips reach the game as Steam's mouse and keys. "
    "Turn Steam Input off for Open Annihilation in Steam's controller settings to use them here.";
/// Maximum frame rate's second hint line on a Steam Deck: the screen's
/// rate, which the setting starts at, in place of {rate}.
inline constexpr std::string_view steam_deck_rate_text =
    "Steam Deck: starts at the screen's {rate} fps.";

/// Returns a line of the text under a row as its row spec gives it, before
/// the dialog looks it up: the hint's (hint_line); for the Game files rows
/// the host's texts: the
/// summary and its sizes line, the backups hint with the device's name
/// (Dialog::game_files_device, "device" without one), and where the files
/// are, broken into lines between words (break_lines); for Your files
/// the player's own folder, then why the last folder asked for could not be
/// opened, as a notice; for the Steam Input notice its text, broken into
/// lines between words, as a notice; and on a Steam Deck Maximum frame
/// rate's second line, naming the rate it starts at.
///
/// @param dialog the dialog
/// @param setting the row's setting
/// @param line the line, from 0
/// @return the line; empty past the last
[[nodiscard]] HintLine row_hint(const Dialog& dialog, Setting setting, std::size_t line);

/// Breaks a text into lines of at most a number of characters where the
/// line breaker lets it (oa::base::text::first_row): between words, or
/// between Chinese or Japanese characters, and after a location's mark
/// between folders (›) where one stands in a line's second half; a word
/// longer than a line is cut at the line's end. Characters are counted as
/// UTF-8 characters, not bytes.
///
/// @param text the text, UTF-8
/// @param characters the most characters a line holds, 1 or more
/// @param most_lines the most lines kept; the rest of the text is dropped
/// @return the lines, in order; none for an empty text
[[nodiscard]] std::vector<std::string>
break_lines(std::string_view text, std::size_t characters, std::size_t most_lines);

/// The footer's count while one setting changed.
inline constexpr std::string_view one_changed_text = "1 setting changed";
/// The footer's count while more changed; {count} is how many.
inline constexpr std::string_view many_changed_text = "{count} settings changed";

/// Returns the footer's count of changed settings (changed_settings_count)
/// as lines: one, or two broken between words where one does not fit the
/// room; none while nothing changed.
///
/// @param dialog the dialog
/// @param measure a text's width in the small font
/// @param room the count's room, in points (Sizes::changed_count)
/// @return the lines, top to bottom
[[nodiscard]] std::vector<std::string>
changed_count_lines(const Dialog& dialog, const TextWidth& measure, int32_t room);

/// Returns a section's entry in the list of a dialog of any kind, where it
/// shows: as nav_layout places it, moved up by the list's scroll.
///
/// @param dialog the dialog
/// @param page the section
/// @return its rectangle; empty for a section the dialog does not list
[[nodiscard]] SourceRect dialog_list_item(const Dialog& dialog, Page page);

/// Returns a slider's value as its row shows it.
///
/// @param setting a slider setting
/// @param settings the settings shown
/// @return the text; empty for a setting that is no slider
[[nodiscard]] std::string value_text(Setting setting, const EngineSettings& settings);

/// Returns the settings a dialog's sliders show: the settings chosen, but
/// Screen size at the window's own size until its knob moves
/// (Dialog::window_screen_size).
///
/// @param dialog the dialog
/// @return the settings
[[nodiscard]] EngineSettings slider_settings(const Dialog& dialog);

/// Returns a slider's value as the dialog's row shows it: as value_text of
/// slider_settings, but "Custom" for Screen size at the window's own size
/// that the display does not offer (Dialog::custom_screen_size).
///
/// @param setting a slider setting
/// @param dialog the dialog
/// @return the text
[[nodiscard]] std::string value_text(Setting setting, const Dialog& dialog);

/// Returns a lock's text.
///
/// @param lock the lock
/// @return the text; empty for Lock::none
[[nodiscard]] std::string_view lock_text(Lock lock) noexcept;

/// The title's first words.
inline constexpr std::string_view title_text = "OPEN ANNIHILATION";
/// The title's last word, drawn muted.
inline constexpr std::string_view title_suffix_text = "SETTINGS";
/// What a shortened path or text ends or starts with.
inline constexpr std::string_view path_ellipsis = "...";
/// Your files' second hint line while no folder failed to open.
inline constexpr std::string_view user_folder_hint_text =
    "Saved games, screenshots, films and mods.";
/// What the header says while a shared game keeps running.
inline constexpr std::string_view shared_game_text = "Shared game - still running";
/// Restore defaults' caption.
inline constexpr std::string_view restore_text = "RESTORE DEFAULTS";
/// Cancel's caption.
inline constexpr std::string_view cancel_text = "Cancel";
/// OK's caption.
inline constexpr std::string_view ok_text = "OK";
/// A switch's Off caption.
inline constexpr std::string_view off_text = "OFF";
/// A switch's On caption.
inline constexpr std::string_view on_text = "ON";
/// The columns the header keeps for the title's first words.
inline constexpr int32_t title_width = 138;
/// The columns the header keeps for the title's last word.
inline constexpr int32_t title_suffix_width = 68;
/// The widest version text the header has room for, in the small font's columns.
inline constexpr int32_t version_width = 44;
/// The columns between the shared game's text and the version.
inline constexpr int32_t version_gap = 8;
/// The header's close mark. Its accessible caption is "Close".
inline constexpr std::string_view close_x_text = "Close";

/// Returns the header's close mark, the same square at every size class,
/// in the place a window keeps for its close button.
///
/// @param sized the dialog's sizes
/// @return the button
[[nodiscard]] constexpr SourceRect header_close_button(const Sizes& sized) noexcept {
    const int32_t side = oa::ui::kit::compact_metrics.button_height;
    return {
        sized.dialog_width - edge - oa::ui::kit::compact_metrics.padding - side,
        header_top + (header_height - side) / 2,
        side,
        side,
    };
}

} // namespace oa::ui::engine_settings::geometry
