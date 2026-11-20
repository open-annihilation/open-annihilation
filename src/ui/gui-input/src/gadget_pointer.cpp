// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "gadget_internal.hpp"
#include "oa/base/game_math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace oa::ui::gui_input {
using base::game_math::truncate_to_int64;
using namespace detail;
using ui::gui_layout::find_gadget;
using ui::gui_layout::GadgetRect;
using ui::gui_layout::kNoGadget;

namespace {

constexpr std::string_view kHeaderPrefix = "&G";

[[nodiscard]] bool is_header_line(const GadgetRecord& list, int32_t line) {
    const char* text = nth_line(list.refs.lines, list.refs.lines_size, line);
    return std::strncmp(kHeaderPrefix.data(), text, kHeaderPrefix.size()) == 0;
}

[[nodiscard]] int16_t low16(int64_t value) {
    return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint64_t>(value)));
}

// A comparison with NaN (an unordered result) counts as equal.
[[nodiscard]] bool equal_or_unordered(double a, double b) {
    return std::isnan(a) || std::isnan(b) || a == b;
}

void clamp_knob(GadgetRecord& record) {
    if (i16(record, field::scroll_range) - 1 < i16(record, field::scroll_knob))
        set_i16(record, field::scroll_knob, i16(record, field::scroll_range) - 1);
    if (i16(record, field::scroll_knob) < 0)
        set_i16(record, field::scroll_knob, 0);
}

void notify_scroll(GadgetPanel& panel, GadgetRecord& record) {
    if (record.refs.scroll_changed != nullptr)
        record.refs.scroll_changed(panel, i32(record, field::scroll_callback_argument));
}

void notify_list(GadgetPanel& panel, int32_t index) {
    auto& record = record_at(panel, index);
    if (record.refs.list_changed != nullptr)
        record.refs.list_changed(panel, index);
}

[[nodiscard]] int32_t list_row_height(GadgetPanel& panel, const GadgetRecord& list) {
    const auto height = i16(list, field::list_item_height);
    return height == 0 ? line_height(panel) + 1 : height;
}

[[nodiscard]] int32_t image_height(GadgetPanel& panel, const GadgetRecord& list, int32_t item) {
    int32_t width = 0;
    int32_t height = 0;
    if (panel.host.list_image_size != nullptr)
        panel.host.list_image_size(panel.host.context, list, item, width, height);
    return height;
}

void step_linked_scroll_bar(GadgetPanel& panel, int32_t index) {
    auto& button = record_at(panel, index);
    if ((attributes(button) & attribute::scroll_step_mask) == 0)
        return;
    const auto bar_index =
        ui::gui_layout::find_group_member(panel.owner->records(), index, gadget_type::scroll_bar);
    auto& bar = record_at(panel, bar_index);
    const auto knob = i16(bar, field::scroll_knob);
    if ((attributes(button) & attribute::scroll_step_back) == 0) {
        if (knob < i16(bar, field::scroll_range) - 1)
            set_i16(bar, field::scroll_knob, knob + 1);
    } else if (knob > 0) {
        set_i16(bar, field::scroll_knob, knob - 1);
    }
    owner_redraw(panel);
    draw(panel, GadgetDraw::scroll_bar, bar_index);
    sync_group_members(panel, bar_index);
    notify_scroll(panel, bar);
}

} // namespace

// The same walk as the line locator in oa-ui-services, kept local so
// this layer has no service dependency.
const char* nth_line(const char* lines, std::size_t lines_size, int32_t line) {
    if (lines == nullptr)
        return "";
    std::size_t offset = 0;
    int32_t seen = 0;
    while (seen != line) {
        if (offset >= lines_size)
            return "";
        if (lines[offset] == '\0' || lines[offset] == '\n')
            ++seen;
        ++offset;
    }
    return offset < lines_size ? lines + offset : "";
}

void set_list_selection(GadgetPanel& panel, std::string_view name, int16_t selected) {
    auto records = panel.owner->records();
    const auto index = find_gadget(records, name);
    if (index == kNoGadget)
        return;
    auto& list = record_at(panel, index);
    set_i16(list, field::list_selected, selected);
    apply_gadget_font(panel, index);
    const auto rows = (i16(list, field::height) - 2) / (line_height(panel) + 1);
    const auto current = i16(list, field::list_selected);
    if (rows - 1 + i16(list, field::list_top) < current || current < i16(list, field::list_top)) {
        const auto last_top = i16(list, field::list_last_top);
        if (last_top != 0)
            set_i16(list, field::list_top, current);
        if (last_top < i16(list, field::list_top))
            set_i16(list, field::list_top, last_top);
        const auto bar_index = ui::gui_layout::find_group_member(
            records, find_gadget(records, name), gadget_type::scroll_bar
        );
        auto& bar = record_at(panel, bar_index);
        const double position = static_cast<double>(i16(bar, field::scroll_range)) *
                                static_cast<double>(i16(list, field::list_top)) /
                                static_cast<double>(i16(list, field::list_last_top));
        if (!equal_or_unordered(static_cast<double>(i16(bar, field::scroll_knob)), position))
            set_i16(bar, field::scroll_knob, low16(truncate_to_int64(position)));
    }
    panel.dirty = 1;
}

void reset_list_scroll(GadgetPanel& panel, int32_t index) {
    auto& list = record_at(panel, index);
    set_i16(list, field::list_top, 0);
    set_i16(list, field::list_selected, 0);
    apply_gadget_font(panel, index);
    const auto pitch = line_height(panel) + 2;
    const auto height = static_cast<int32_t>(i16(list, field::height));
    set_i16(list, field::height, height - height % pitch);
    ui::gui_layout::set_record_u32(list, field::list_repeat_tick, current_tick(panel));
}

void init_text_list(
    GadgetPanel& panel,
    std::string_view name,
    const char* lines,
    std::size_t lines_size,
    int32_t count,
    const uint8_t* item_flags
) {
    auto records = panel.owner->records();
    auto* list = ui::gui_layout::find_gadget_record(records, name);
    if (list == nullptr)
        return;
    ui::gui_layout::set_record_u32(
        *list, field::attributes, attributes(*list) | attribute::text_list
    );
    set_i16(*list, field::list_count, count);
    list->refs.lines = lines;
    list->refs.lines_size = lines_size;
    const auto height = line_height(panel);
    if (!(height + 1 < i16(*list, field::list_item_height)))
        set_i16(*list, field::list_item_height, line_height(panel) + 1);
    if (item_flags != nullptr) {
        list->bytes[field::attributes_high] |= 0x08;
        list->refs.item_flags = item_flags;
    }
    int32_t remaining = i16(*list, field::height);
    set_i16(*list, field::list_top, 0);
    set_i16(*list, field::list_selected, 0);
    set_i16(*list, field::list_last_top, count - 1);
    const auto step = i16(*list, field::list_item_height) == 0
                          ? line_height(panel) + 1
                          : i16(*list, field::list_item_height);
    while (--count > -1) {
        remaining -= step;
        if (remaining < 0)
            break;
        set_i16(*list, field::list_last_top, count);
    }
    if (list->bytes[field::active] != 0) {
        const auto bar = ui::gui_layout::find_group_member(
            records, find_gadget(records, name), gadget_type::scroll_bar
        );
        const auto bar_name =
            std::string(ui::gui_layout::record_string(record_at(panel, bar), field::name));
        set_gadget_active_by_name(panel, bar_name, remaining < 0 ? 1 : 0);
        if (remaining < 0)
            update_scroll_knob_size(panel, bar);
    }
}

void init_image_list(
    GadgetPanel& panel, GadgetOwner& owner, std::string_view name, const void* images, int32_t count
) {
    auto* list = ui::gui_layout::find_gadget_record(owner.records(), name);
    if (list == nullptr)
        return;
    list->bytes[field::attributes] |= static_cast<uint8_t>(attribute::image_records);
    set_i16(*list, field::list_top, 0);
    set_i16(*list, field::list_selected, 0);
    set_i16(*list, field::list_count, count);
    list->refs.images = images;
    int32_t remaining = i16(*list, field::height);
    set_i16(*list, field::list_last_top, count - 1);
    for (auto item = count - 1; item >= 0; --item) {
        int32_t step = i16(*list, field::list_item_height);
        if (step == 0)
            step = image_height(panel, *list, item);
        remaining -= step;
        if (remaining < 0)
            return;
        set_i16(*list, field::list_last_top, item);
    }
}

bool list_pointer(GadgetPanel& panel, int32_t index) {
    if (panel.activated != kNoGadget)
        return false;
    auto& list = record_at(panel, index);
    const auto previous = static_cast<int32_t>(i16(list, field::list_selected));
    if (i16(list, field::list_count) == 0)
        return false;
    auto rect = ui::gui_layout::panel_relative_rect(list);
    rect.top += 2;
    rect.bottom -= 3;
    const auto pointer = local_pointer(panel);
    apply_gadget_font(panel, index);
    const auto step = list_row_height(panel, list);
    const auto rows = (i16(list, field::height) - 2) / step;
    const bool inside = ui::gui_layout::rect_contains(rect, pointer.x, pointer.y);

    if (is_double_click(panel, kLeftButton)) {
        if (inside && i16(list, field::list_count) != 0) {
            if ((attributes(list) & attribute::skip_headers) == 0)
                return true;
            const auto top = i16(list, field::list_top);
            const auto row = static_cast<int16_t>((pointer.y - rect.top) / step + top);
            set_i16(list, field::list_selected, row);
            if (row < 0)
                return true;
            if (rows - 1 < row - top)
                set_i16(list, field::list_selected, top - 1 + rows);
            if (i16(list, field::list_count) - 1 <= i16(list, field::list_selected))
                set_i16(list, field::list_selected, i16(list, field::list_count) - 1);
            if (!is_header_line(list, i16(list, field::list_selected)))
                return true;
            set_i16(list, field::list_selected, previous);
            return false;
        }
    } else if (is_button_message(panel, kLeftButton)) {
        if (inside) {
            capture_gadget(panel, index);
            set_last_message(panel, 1);
        }
    } else if (is_button_message(panel, kRightButton)) {
        if (inside) {
            capture_gadget(panel, index);
            set_last_message(panel, 2);
        }
    }

    if (panel.captured != index)
        return panel.activated != kNoGadget;
    if (!buttons_held(panel, kAnyButtonHeld))
        panel.captured = kNoGadget;

    const bool x_inside = rect.left <= pointer.x && pointer.x <= rect.right;
    bool redraw = false;
    if (x_inside && rect.top <= pointer.y && pointer.y <= rect.bottom) {
        panel.owner->focus = index;
        const auto attrs = attributes(list);
        if ((attrs & attribute::text_list) == 0) {
            // Every image list takes this path, whatever its attributes.
            int32_t item = i16(list, field::list_top);
            int32_t remaining = (pointer.y - rect.top) - 2;
            int16_t offset = 0;
            do {
                const auto height = i16(list, field::list_item_height) == 0
                                        ? image_height(panel, list, item)
                                        : step;
                remaining -= height;
                if (remaining < 1) {
                    set_i16(list, field::list_selected, i16(list, field::list_top) + offset);
                    break;
                }
                ++offset;
                ++item;
            } while (item <= i16(list, field::list_count) - 1);
        } else {
            const auto top = i16(list, field::list_top);
            const auto row = static_cast<int16_t>((pointer.y - rect.top) / step + top);
            set_i16(list, field::list_selected, row);
            if (row < 0) {
                set_i16(list, field::list_selected, previous);
            } else {
                if (rows - 1 < row - top)
                    set_i16(list, field::list_selected, top - 1 + rows);
                if (i16(list, field::list_count) - 1 <= i16(list, field::list_selected))
                    set_i16(list, field::list_selected, i16(list, field::list_count) - 1);
                if (i16(list, field::list_selected) < 0)
                    set_i16(list, field::list_selected, 0);
                if ((attrs & attribute::skip_headers) != 0 &&
                    is_header_line(list, i16(list, field::list_selected))) {
                    set_i16(list, field::list_selected, previous);
                }
                const auto last = std::min<int32_t>(
                    ui::gui_layout::gadget_last_index(panel.owner->records()),
                    static_cast<int32_t>(panel.owner->table.records.size()) - 1
                );
                for (int32_t other = 1; other <= last; ++other) {
                    auto& member = record_at(panel, other);
                    if (ui::gui_layout::gadget_type_of(member) == gadget_type::list_box &&
                        member.bytes[field::group] == list.bytes[field::group]) {
                        const auto limit = i16(member, field::list_count) - 1;
                        set_i16(
                            member,
                            field::list_selected,
                            std::min<int32_t>(i16(list, field::list_selected), limit)
                        );
                    }
                }
            }
        }
        if (previous != i16(list, field::list_selected)) {
            draw(panel, GadgetDraw::list, index);
            notify_list(panel, index);
        }
        if ((attributes(list) & attribute::toggle) != 0)
            return true;
        panel.dirty = 1;
        return panel.activated != kNoGadget;
    }

    if (pointer.y >= rect.top) {
        if (pointer.y <= rect.bottom ||
            i16(list, field::list_last_top) <= i16(list, field::list_top))
            return panel.activated != kNoGadget;
        if (static_cast<int32_t>(current_tick(panel)) <= i32(list, field::list_repeat_tick))
            return panel.activated != kNoGadget;
        const auto tick = current_tick(panel);
        set_i16(list, field::list_top, i16(list, field::list_top) + 1);
        ui::gui_layout::set_record_u32(list, field::list_repeat_tick, tick + 2);
        set_i16(list, field::list_selected, i16(list, field::list_top) - 1 + rows);
        if (list.refs.lines != nullptr && is_header_line(list, i16(list, field::list_selected)))
            set_i16(list, field::list_selected, previous);
        redraw = true;
    } else {
        if (i16(list, field::list_top) > 0 &&
            i32(list, field::list_repeat_tick) < static_cast<int32_t>(current_tick(panel))) {
            ui::gui_layout::set_record_u32(list, field::list_repeat_tick, current_tick(panel) + 2);
            const auto top = i16(list, field::list_top);
            if (top < i16(list, field::list_selected))
                set_i16(list, field::list_selected, top);
            set_i16(list, field::list_selected, i16(list, field::list_selected) - 1);
            set_i16(list, field::list_top, top - 1);
            if (list.refs.lines != nullptr) {
                const auto row = std::max<int32_t>(i16(list, field::list_selected), 0);
                if (is_header_line(list, row))
                    set_i16(list, field::list_selected, previous);
            }
            redraw = true;
        } else {
            if (i16(list, field::list_selected) < 1)
                return panel.activated != kNoGadget;
            set_i16(list, field::list_selected, 0);
            redraw = true;
        }
    }
    if (redraw) {
        draw(panel, GadgetDraw::list, index);
        sync_group_members(panel, index);
    }
    return panel.activated != kNoGadget;
}

void update_scroll_knob_size(GadgetPanel& panel, int32_t index) {
    auto records = panel.owner->records();
    auto& bar = record_at(panel, index);
    const auto list_index =
        ui::gui_layout::find_group_member(records, index, gadget_type::list_box);
    if (list_index != 0 &&
        ui::gui_layout::gadget_type_of(record_at(panel, list_index)) == gadget_type::list_box) {
        auto& list = record_at(panel, list_index);
        const auto attrs = attributes(list);
        if ((attrs & attribute::text_list) != 0) {
            apply_gadget_font(panel, list_index);
            const auto minimum = line_height(panel) + 1;
            const auto pitch = std::max<int32_t>(i16(list, field::list_item_height), minimum);
            const auto rows = (i16(list, field::height) - 2) / pitch;
            const auto count = i16(list, field::list_count);
            const auto bar_height = i16(bar, field::height);
            const double size = static_cast<double>(rows) / static_cast<double>(count) *
                                static_cast<double>(bar_height - 3);
            set_i16(bar, field::scroll_knob_size, low16(truncate_to_int64(size)));
            if (i16(bar, field::scroll_knob_size) < 10)
                set_i16(bar, field::scroll_knob_size, 10);
            if (count > rows)
                set_i16(
                    bar, field::scroll_range, bar_height - i16(bar, field::scroll_knob_size) - 3
                );
            else
                set_i16(bar, field::scroll_range, 0);
        } else {
            int32_t knob = 0;
            bool sized = false;
            if ((attrs & attribute::image_list) != 0) {
                int32_t total = 0;
                if (i16(list, field::list_count) > 0)
                    total = image_height(panel, list, 0) * i16(list, field::list_count);
                if (total != 0) {
                    knob = i16(list, field::height) * i16(bar, field::height) / total;
                    sized = true;
                }
            } else if (
                (attrs & attribute::image_records) != 0 &&
                i16(list, field::list_item_height) != 0 && i16(list, field::list_count) != 0
            ) {
                knob = i16(list, field::height) / i16(list, field::list_item_height) *
                       i16(bar, field::height) / i16(list, field::list_count);
                sized = true;
            }
            if (sized) {
                set_i16(bar, field::scroll_knob_size, knob);
                const auto extent = (bar.bytes[field::attributes] & attribute::horizontal) != 0
                                        ? i16(bar, field::width)
                                        : i16(bar, field::height);
                set_i16(bar, field::scroll_range, extent - static_cast<int16_t>(knob));
            }
        }
    }
    draw(panel, GadgetDraw::scroll_bar, index);
}

void scroll_bar_pointer(GadgetPanel& panel, int32_t index) {
    auto& bar = record_at(panel, index);
    if ((bar.bytes[field::attributes] & attribute::text_list) != 0)
        return;
    if (i32(bar, field::scroll_locked) != 0)
        return;
    const auto pointer = local_pointer(panel);
    GadgetRect outer;
    GadgetRect knob;
    ui::gui_layout::scroll_bar_rects(bar, outer, knob);
    if (panel.captured != index) {
        if (panel.dragging != 0)
            return;
        int32_t message = 0;
        if (is_button_message(panel, kLeftButton))
            message = 1;
        else if (is_button_message(panel, kRightButton))
            message = 2;
        else
            return;
        panel.dragging = 0;
        if (!ui::gui_layout::rect_contains(outer, pointer.x, pointer.y))
            return;
        capture_gadget(panel, index);
        set_last_message(panel, message);
        if (!ui::gui_layout::rect_contains(knob, pointer.x, pointer.y))
            return;
        panel.dragging = 1;
        panel.drag_origin = panel.pointer;
        panel.drag_origin.x = pointer.x;
        panel.drag_origin.y = pointer.y;
        panel.drag_knob = i16(bar, field::scroll_knob);
        return;
    }
    if (!buttons_held(panel, kAnyButtonHeld)) {
        panel.captured = kNoGadget;
        panel.dragging = 0;
    }
    const auto old = i16(bar, field::scroll_knob);
    const bool horizontal = (bar.bytes[field::attributes] & attribute::horizontal) != 0;
    if (panel.dragging != 0) {
        const auto moved = horizontal ? (panel.drag_knob - panel.drag_origin.x) + pointer.x
                                      : (panel.drag_knob - panel.drag_origin.y) + pointer.y;
        set_i16(bar, field::scroll_knob, moved);
    } else if (!horizontal) {
        if (pointer.y < knob.top)
            set_i16(bar, field::scroll_knob, old - 1);
        else if (pointer.y > knob.bottom)
            set_i16(bar, field::scroll_knob, old + 1);
    } else {
        if (pointer.x < knob.left)
            set_i16(bar, field::scroll_knob, old - 1);
        else if (pointer.x > knob.right)
            set_i16(bar, field::scroll_knob, old + 1);
    }
    clamp_knob(bar);
    if (i16(bar, field::scroll_knob) == old)
        return;
    owner_redraw(panel);
    draw(panel, GadgetDraw::scroll_bar, index);
    sync_group_members(panel, index);
    notify_scroll(panel, bar);
}

void sync_group_members(GadgetPanel& panel, int32_t index) {
    auto& source = record_at(panel, index);
    const auto source_type = ui::gui_layout::gadget_type_of(source);
    const auto source_attributes = attributes(source);
    const auto end = scan_end(panel);
    for (int32_t other = 1; other < end; ++other) {
        if (other == index)
            continue;
        auto& member = record_at(panel, other);
        if (member.bytes[field::group] != source.bytes[field::group])
            continue;
        const auto type = ui::gui_layout::gadget_type_of(member);
        if (type == gadget_type::list_box) {
            if (source_type == gadget_type::list_box) {
                set_i16(member, field::list_top, i16(source, field::list_top));
                set_i16(member, field::list_selected, i16(source, field::list_selected));
            } else if (source_type == gadget_type::scroll_bar) {
                int32_t offset = 0;
                if ((member.bytes[field::attributes] & attribute::image_list) != 0)
                    offset =
                        i16(source, field::scroll_range) / (i16(member, field::list_last_top) + 1);
                const auto item_height = i16(member, field::list_item_height);
                if (item_height != 0) {
                    const auto visible = i16(member, field::height) / item_height;
                    const double top =
                        static_cast<double>(i16(member, field::list_count) - visible) *
                        static_cast<double>(i16(source, field::scroll_knob) + offset) /
                        static_cast<double>(i16(source, field::scroll_range) - 1);
                    set_i16(member, field::list_top, low16(truncate_to_int64(top)));
                }
            } else {
                continue;
            }
            draw(panel, GadgetDraw::list, other);
        } else if (type == gadget_type::text_box) {
            if (source_type == gadget_type::list_box &&
                (source_attributes & attribute::checkbox) != 0) {
                const char* line = nth_line(
                    source.refs.lines, source.refs.lines_size, i16(source, field::list_selected)
                );
                ui::gui_layout::copy_record_cstring(member, field::text, line);
                draw(panel, GadgetDraw::text_box, other);
            }
        } else if (
            type == gadget_type::scroll_bar && source_type == gadget_type::list_box &&
            i16(source, field::list_count) > 1
        ) {
            int32_t knob = 0;
            if (i16(source, field::list_last_top) != 0) {
                const double position = static_cast<double>(i16(source, field::list_top)) *
                                        static_cast<double>(i16(member, field::scroll_range)) /
                                        static_cast<double>(i16(source, field::list_last_top));
                knob = static_cast<int32_t>(truncate_to_int64(position));
            }
            if (i16(member, field::scroll_knob) != knob) {
                set_i16(member, field::scroll_knob, knob);
                draw(panel, GadgetDraw::scroll_bar, other);
            }
        }
    }
}

void step_scroll_back(GadgetPanel& panel, int32_t index) {
    auto& bar = record_at(panel, index);
    const auto old = i16(bar, field::scroll_knob);
    set_i16(bar, field::scroll_knob, old - 1);
    clamp_knob(bar);
    if (i16(bar, field::scroll_knob) != old) {
        panel.dirty = 1;
        draw(panel, GadgetDraw::scroll_bar, index);
        sync_group_members(panel, index);
    }
    notify_scroll(panel, bar);
}

void step_scroll_forward(GadgetPanel& panel, int32_t index) {
    auto& bar = record_at(panel, index);
    const auto old = i16(bar, field::scroll_knob);
    set_i16(bar, field::scroll_knob, old + 1);
    clamp_knob(bar);
    if (i16(bar, field::scroll_knob) != old) {
        panel.dirty = 1;
        draw(panel, GadgetDraw::scroll_bar, index);
        sync_group_members(panel, index);
    }
    notify_scroll(panel, bar);
}

void place_scroll_knob(GadgetPanel& panel, int32_t index, int16_t knob) {
    auto& bar = record_at(panel, index);
    const auto old = i16(bar, field::scroll_knob);
    set_i16(bar, field::scroll_knob, knob);
    clamp_knob(bar);
    if (i16(bar, field::scroll_knob) != old) {
        panel.dirty = 1;
        draw(panel, GadgetDraw::scroll_bar, index);
        sync_group_members(panel, index);
    }
    notify_scroll(panel, bar);
}

void list_select_previous(GadgetPanel& panel, int32_t index) {
    auto& list = record_at(panel, index);
    apply_gadget_font(panel, index);
    const auto rows = (i16(list, field::height) - 2) / (line_height(panel) + 1);
    const auto top = i16(list, field::list_top);
    const auto selected = i16(list, field::list_selected);
    if (selected < rows + top && top <= selected) {
        if (i16(list, field::list_count) != 0 && selected != 0) {
            const auto row = static_cast<int16_t>(selected - 1);
            set_i16(list, field::list_selected, row);
            if (row < top)
                set_i16(list, field::list_top, top - 1);
            if (list.refs.lines != nullptr && is_header_line(list, row))
                set_i16(list, field::list_selected, selected);
            draw(panel, GadgetDraw::list, index);
            sync_group_members(panel, index);
        }
    } else if (i16(list, field::list_count) != 0) {
        set_list_selection(panel, ui::gui_layout::record_string(list, field::name), selected);
    }
}

void list_select_next(GadgetPanel& panel, int32_t index) {
    auto& list = record_at(panel, index);
    apply_gadget_font(panel, index);
    const auto top = i16(list, field::list_top);
    const auto rows = (i16(list, field::height) - 2) / (line_height(panel) + 1);
    const auto selected = i16(list, field::list_selected);
    const auto bottom = top + rows;
    if (selected < bottom && top <= selected) {
        if (i16(list, field::list_count) != 0 &&
            selected != i16(list, field::list_last_top) - 1 + rows) {
            const auto row = static_cast<int16_t>(selected + 1);
            set_i16(list, field::list_selected, row);
            if (bottom - 1 < row)
                set_i16(list, field::list_top, top + 1);
            if (i16(list, field::list_count) - 1 <= row)
                set_i16(list, field::list_selected, i16(list, field::list_count) - 1);
            if (list.refs.lines != nullptr && is_header_line(list, i16(list, field::list_selected)))
                set_i16(list, field::list_selected, selected);
            draw(panel, GadgetDraw::list, index);
            sync_group_members(panel, index);
        }
    } else if (i16(list, field::list_count) != 0) {
        set_list_selection(panel, ui::gui_layout::record_string(list, field::name), selected);
    }
}

bool label_pointer(GadgetPanel& panel, int32_t index, int32_t key) {
    auto& label = record_at(panel, index);
    if (label.bytes[field::label_quick_key] == 0 && (attributes(label) & attribute::text_list) != 0)
        return false;
    const auto rect = ui::gui_layout::panel_relative_rect(label);
    const auto pointer = local_pointer(panel);
    const bool inside = ui::gui_layout::rect_contains(rect, pointer.x, pointer.y);
    if (is_button_message(panel, kLeftButton)) {
        if (inside) {
            capture_gadget(panel, index);
            set_last_message(panel, 1);
        }
    } else if (is_button_message(panel, kRightButton) && inside) {
        capture_gadget(panel, index);
        set_last_message(panel, 2);
    }
    if (panel.captured == index && !buttons_held(panel, kAnyButtonHeld)) {
        panel.captured = kNoGadget;
        if (inside)
            return true;
    }
    if (panel.captured != kNoGadget &&
        ui::gui_layout::gadget_type_of(record_at(panel, panel.captured)) == gadget_type::text_box &&
        !key_down(panel, key_code::alt)) {
        return false;
    }
    if (panel.quick_keys_enabled != 1 || key == 0)
        return false;
    const auto quick = signed_byte(label.bytes[field::label_quick_key]);
    if (static_cast<char>(lower(quick)) != static_cast<char>(key) &&
        static_cast<char>(upper(quick)) != static_cast<char>(key)) {
        return false;
    }
    static_cast<void>(pop_key(panel));
    return true;
}

void progress_tick(GadgetPanel& panel, int32_t index) {
    auto& record = record_at(panel, index);
    if (i32(record, field::progress_running) == 0 ||
        i32(record, field::progress_value) >= i32(record, field::progress_limit)) {
        return;
    }
    if (i32(record, field::progress_deadline) < static_cast<int32_t>(current_tick(panel))) {
        float step = 0;
        std::memcpy(&step, record.bytes.data() + field::progress_step, sizeof(step));
        const auto increment = static_cast<int32_t>(truncate_to_int64(step));
        set_i32(record, field::progress_value, i32(record, field::progress_value) + increment);
        if (i32(record, field::progress_limit) < i32(record, field::progress_value)) {
            set_i32(record, field::progress_running, 0);
            set_i32(record, field::progress_value, i32(record, field::progress_limit));
        }
        set_i32(
            record,
            field::progress_deadline,
            static_cast<int32_t>(current_tick(panel)) + i32(record, field::progress_interval)
        );
    }
    draw(panel, GadgetDraw::progress, index);
}

bool hot_surface_pointer(GadgetPanel& panel, int32_t index) {
    auto& record = record_at(panel, index);
    const auto rect = ui::gui_layout::panel_relative_rect(record);
    if (record.refs.hot_callback != nullptr)
        record.refs.hot_callback(panel, index);
    if ((record.bytes[field::hot_flags] & 1) == 0)
        return false;
    const bool inside = ui::gui_layout::rect_contains(rect, panel.pointer.x, panel.pointer.y);
    if (is_button_message(panel, kLeftButton)) {
        if (inside) {
            capture_gadget(panel, index);
            set_last_message(panel, 1);
        }
    } else if (is_button_message(panel, kRightButton) && inside) {
        capture_gadget(panel, index);
        set_last_message(panel, 2);
    }
    if (panel.captured == index && !buttons_held(panel, kAnyButtonHeld)) {
        panel.captured = kNoGadget;
        if (inside)
            return true;
    }
    return false;
}

bool button_pointer(GadgetPanel& panel, int32_t index, int32_t key) {
    auto& button = record_at(panel, index);
    if ((button.bytes[field::button_flags] & 1) != 0)
        return false;
    const auto rect = ui::gui_layout::panel_relative_rect(button);
    const auto pointer = local_pointer(panel);
    const bool inside = ui::gui_layout::rect_contains(rect, pointer.x, pointer.y);
    if (inside) {
        panel.hovered = index;
        int32_t message = 0;
        if (is_button_message(panel, kLeftButton))
            message = 1;
        else if (is_button_message(panel, kRightButton))
            message = 2;
        if (message != 0) {
            panel.captured = kNoGadget;
            capture_gadget(panel, index);
            set_last_message(panel, message);
            panel.pressed_status = i16(button, field::button_status);
        }
    }

    const auto redraw = [&] { draw(panel, GadgetDraw::button, index); };
    if (panel.captured != index) {
        const bool typing = panel.captured != kNoGadget &&
                            ui::gui_layout::gadget_type_of(record_at(panel, panel.captured)) ==
                                gadget_type::text_box;
        if ((!typing || key_down(panel, key_code::alt)) && panel.quick_keys_enabled == 1 &&
            key != 0) {
            const auto quick = signed_byte(button.bytes[field::button_quick_key]);
            if (static_cast<char>(lower(quick)) == static_cast<char>(key) ||
                static_cast<char>(upper(quick)) == static_cast<char>(key)) {
                if ((attributes(button) & attribute::toggle) != 0) {
                    set_i16(
                        button, field::button_status, i16(button, field::button_status) == 0 ? 1 : 0
                    );
                    redraw();
                } else if (
                    (attributes(button) & attribute::text_list) != 0 &&
                    i16(button, field::button_status) == 0
                ) {
                    set_i16(button, field::button_status, 1);
                    redraw();
                }
                clear_group_status(panel, index);
                static_cast<void>(pop_key(panel));
                return true;
            }
        }
        return false;
    }

    const auto attrs = attributes(button);
    if ((attrs & attribute::text_list) != 0) {
        // Hold button: activates while the pointer is still held down.
        if (!buttons_held(panel, kAnyButtonHeld))
            return false;
        panel.captured = kNoGadget;
        if (!inside) {
            set_i16(button, field::button_status, panel.pressed_status);
            redraw();
            return false;
        }
        set_i16(button, field::button_status, 1);
        clear_group_status(panel, index);
        redraw();
        return true;
    }
    if ((attrs & attribute::toggle) != 0) {
        if (!buttons_held(panel, kAnyButtonHeld)) {
            panel.captured = kNoGadget;
            if (!inside) {
                set_i16(button, field::button_status, panel.pressed_status);
                redraw();
                return false;
            }
            set_i16(button, field::button_status, panel.pressed_status == 0 ? 1 : 0);
            clear_group_status(panel, index);
            redraw();
            return true;
        }
        if (!inside) {
            if (i16(button, field::button_status) != 0) {
                set_i16(button, field::button_status, 0);
                redraw();
            }
        } else if (i16(button, field::button_status) == 0) {
            set_i16(button, field::button_status, 1);
            redraw();
        }
        return false;
    }
    if ((attrs & attribute::checkbox) != 0) {
        if (!buttons_held(panel, kAnyButtonHeld) && inside) {
            const auto status = i16(button, field::button_status);
            if (status == 1)
                set_i16(button, field::button_status, 0);
            else if (status == 0)
                set_i16(button, field::button_status, 1);
            clear_group_status(panel, index);
            redraw();
            panel.captured = kNoGadget;
            return true;
        }
        return false;
    }
    if ((attrs & attribute::cycle_frames) != 0) {
        if (is_button_message(panel, kLeftButton) && inside) {
            if (button.refs.sprite != nullptr) {
                const auto frames =
                    panel.host.sprite_frames != nullptr
                        ? panel.host.sprite_frames(panel.host.context, button.refs.sprite)
                        : 0;
                if (i16(button, field::button_status) < frames - 1)
                    set_i16(button, field::button_status, i16(button, field::button_status) + 1);
                else
                    set_i16(button, field::button_status, 0);
            }
            clear_group_status(panel, index);
            redraw();
            panel.captured = kNoGadget;
            return true;
        }
        return false;
    }

    // Plain push button.
    if (!buttons_held(panel, kAnyButtonHeld)) {
        panel.captured = kNoGadget;
        set_i16(button, field::button_status, 0);
        clear_group_status(panel, index);
        if (!inside || (attrs & attribute::scroll_step_mask) != 0) {
            redraw();
            return false;
        }
        if (button.bytes[field::button_stages] != 0) {
            auto& stage = button.bytes[field::button_stage];
            ++stage;
            if (button.bytes[field::button_stages] <= stage)
                stage = 0;
        }
        redraw();
        return true;
    }
    const auto status = i16(button, field::button_status);
    if (status == 0) {
        if (inside) {
            set_i16(button, field::button_status, 1);
            panel.click_repeat_delay = 0xF;
            redraw();
            step_linked_scroll_bar(panel, index);
        }
        return false;
    }
    if ((attrs & attribute::repeat) == 0) {
        if (!inside) {
            set_i16(button, field::button_status, 0);
            redraw();
        }
        return false;
    }
    const auto tick = current_tick(panel);
    if (panel.click_repeat_last != tick) {
        panel.click_repeat_last = current_tick(panel);
        if (panel.click_repeat_delay < 1) {
            redraw();
            step_linked_scroll_bar(panel, index);
        } else {
            --panel.click_repeat_delay;
        }
    }
    return false;
}

bool text_box_pointer(GadgetPanel& panel, int32_t index, int32_t key) {
    auto& box = record_at(panel, index);
    const auto rect = ui::gui_layout::panel_relative_rect(box);
    apply_gadget_font(panel, index);
    const auto pointer = local_pointer(panel);
    if (ui::gui_layout::rect_contains(rect, pointer.x, pointer.y)) {
        panel.hovered = index;
        if (is_button_message(panel, kLeftButton)) {
            focus_text_box(panel, index);
            set_last_message(panel, 1);
        } else if (is_button_message(panel, kRightButton)) {
            focus_text_box(panel, index);
            set_last_message(panel, 2);
        }
    }
    if (panel.captured != index)
        return false;
    const auto result = edit_keystroke(panel, index, key);
    if (result == key_code::enter) {
        panel.captured = kNoGadget;
        return true;
    }
    if (result == key_code::escape) {
        panel.captured = kNoGadget;
        box.bytes[field::text] = 0;
        return true;
    }
    owner_redraw(panel);
    return false;
}

int32_t edit_keystroke(GadgetPanel& panel, int32_t index, int32_t key) {
    auto& box = record_at(panel, index);
    char* text = text_of(box);
    int32_t result = 0;
    if (panel.owner->keys_from_queue == 0)
        key = pop_key(panel);
    if (key == 0)
        return result;
    const auto max_chars = static_cast<int32_t>(i16(box, field::text_max_chars));
    do {
        result = key;
        const auto length = static_cast<int32_t>(text_length(box, field::text));
        if (key == key_code::escape)
            break;
        if (key == key_code::delete_forward || key == key_code::backspace) {
            bool remove = false;
            if (key == key_code::delete_forward) {
                remove = length != 0 && panel.caret < length;
            } else if (panel.caret != 0) {
                --panel.caret;
                remove = true;
            }
            if (remove) {
                const auto current = static_cast<int32_t>(text_length(box, field::text));
                for (auto position = panel.caret; position < current - 1; ++position)
                    text[position] = text[position + 1];
                if (current > 0)
                    text[current - 1] = '\0';
            }
        } else if (key == key_code::paste || key == key_code::paste_alternate) {
            if (panel.host.read_clipboard != nullptr) {
                std::array<char, field::text_bytes> clip{};
                const auto size = static_cast<int32_t>(
                    panel.host.read_clipboard(panel.host.context, clip.data(), clip.size())
                );
                if (size != 0) {
                    std::fill_n(text, field::text_bytes, '\0');
                    auto count = max_chars - 1;
                    if (size < count)
                        count = size;
                    count = std::clamp<int32_t>(count, 0, static_cast<int32_t>(clip.size()));
                    std::copy_n(clip.begin(), count, text);
                    while (text_width(panel, text) > i16(box, field::width)) {
                        const auto current = std::strlen(text);
                        if (current == 0)
                            break;
                        text[current - 1] = '\0';
                    }
                }
            }
        } else if (key == key_code::home) {
            panel.caret = 0;
        } else if (key == key_code::end) {
            panel.caret = length;
        } else if (key == key_code::left) {
            if (panel.caret != 0)
                panel.caret = panel.caret - 1;
        } else if (key == key_code::right) {
            if (panel.caret < length)
                ++panel.caret;
        } else if (
            length != max_chars && key > 0x1F && key < 0x80 &&
            ((attributes(box) & attribute::centered) == 0 || std::isalnum(key) != 0 || key == '_' ||
             key == ' ' || key == '\'')
        ) {
            const char typed[2] = {static_cast<char>(key), '\0'};
            if (text_width(panel, text) + text_width(panel, typed) <= i16(box, field::width) - 4) {
                for (auto position = max_chars; position - 1 > panel.caret; --position) {
                    if (position - 2 >= 0 && position - 1 < static_cast<int32_t>(field::text_bytes))
                        text[position - 1] = text[position - 2];
                }
                if (panel.caret >= 0 && panel.caret < static_cast<int32_t>(field::text_bytes)) {
                    text[panel.caret] = static_cast<char>(key);
                    ++panel.caret;
                }
            }
        }
        key = pop_key(panel);
    } while (key != 0);
    draw(panel, GadgetDraw::text_box, index);
    return result;
}

} // namespace oa::ui::gui_input
