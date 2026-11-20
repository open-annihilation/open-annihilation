// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "gadget_internal.hpp"
#include "oa/base/text/line_break.hpp"
#include "oa/ui/gui_input/scroll_bar.hpp"
#include "oa/ui/gui_input/tab_order.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::gui_input {
using namespace detail;
using ui::gui_layout::find_gadget;
using ui::gui_layout::kNoGadget;

namespace {

// Key-queue codes 0xE2..0xEB are not consumed when the panel only peeks.
constexpr int32_t kPeekIgnoredFirst = 0xE2;
constexpr int32_t kPeekIgnoredLast = 0xEB;

[[nodiscard]] bool activatable(const GadgetRecord& record) {
    if (record.bytes[field::active] == 0)
        return false;
    return !(
        ui::gui_layout::gadget_type_of(record) == gadget_type::button &&
        (record.bytes[field::button_flags] & 1) != 0
    );
}

void refresh_help(GadgetPanel& panel) {
    update_help_text(panel);
}

[[nodiscard]] ui::gui_layout::Gadget gadget_from_record(const GadgetRecord& record) {
    ui::gui_layout::Gadget gadget;
    const auto type = ui::gui_layout::gadget_type_of(record);
    gadget.common.type = static_cast<ui::gui_layout::GadgetType>(type);
    gadget.common.name = std::string(ui::gui_layout::record_string(record, field::name));
    gadget.common.x = i16(record, field::x);
    gadget.common.y = i16(record, field::y);
    gadget.common.width = i16(record, field::width);
    gadget.common.height = i16(record, field::height);
    gadget.common.attributes = static_cast<int32_t>(attributes(record));
    gadget.common.active = static_cast<int8_t>(record.bytes[field::active]);
    if (type == gadget_type::button) {
        ui::gui_layout::ButtonFields fields;
        fields.grayed_out = (record.bytes[field::button_flags] & 1) != 0;
        gadget.fields = fields;
    } else if (type == gadget_type::scroll_bar) {
        ui::gui_layout::ScrollBarFields fields;
        fields.locked = i32(record, field::scroll_locked) != 0;
        fields.range = i16(record, field::scroll_range);
        fields.knob_position = i16(record, field::scroll_knob);
        gadget.fields = fields;
    } else if (type == gadget_type::list_box) {
        gadget.fields = ui::gui_layout::ListBoxFields{};
    } else if (type == gadget_type::text_box) {
        gadget.fields = ui::gui_layout::TextBoxFields{};
    } else if (type == gadget_type::hot_surface) {
        gadget.fields = ui::gui_layout::HotSurfaceFields{};
    }
    return gadget;
}

void move_keyboard_focus(GadgetPanel& panel, int32_t index) {
    if (!valid_index(panel, index))
        return;
    panel.captured = kNoGadget;
    panel.owner->focus = index;
    if (ui::gui_layout::gadget_type_of(record_at(panel, index)) == gadget_type::text_box)
        focus_text_box(panel, index);
}

[[nodiscard]] int16_t scrolled_knob(FocusKeyEffect effect, int16_t range, int16_t knob) {
    const int16_t span = scroll_page_span(range);
    switch (effect) {
    case FocusKeyEffect::scroll_back:
        return static_cast<int16_t>(static_cast<int32_t>(knob) - 1);
    case FocusKeyEffect::scroll_forward:
        return static_cast<int16_t>(static_cast<int32_t>(knob) + 1);
    case FocusKeyEffect::scroll_page_back: {
        const int32_t stepped = static_cast<int32_t>(knob) - static_cast<int32_t>(span);
        return static_cast<int16_t>(stepped < -32768 ? -32768 : stepped);
    }
    case FocusKeyEffect::scroll_page_forward: {
        const int32_t stepped = static_cast<int32_t>(knob) + static_cast<int32_t>(span);
        return static_cast<int16_t>(stepped > 32767 ? 32767 : stepped);
    }
    case FocusKeyEffect::scroll_home:
        return 0;
    case FocusKeyEffect::scroll_end:
        return range > 0 ? static_cast<int16_t>(static_cast<int32_t>(range) - 1) : int16_t{0};
    default:
        return knob;
    }
}

} // namespace

int32_t dispatch_key(GadgetPanel& panel, int32_t key) {
    auto records = panel.owner->records();
    int32_t chosen = kNoGadget;
    const auto focus = panel.owner->focus;
    // With nothing focused an empty record stands for the focused one.
    GadgetRecord stand_in{};
    stand_in.bytes[field::type] = 0xFF;
    auto& focused = valid_index(panel, focus) ? record_at(panel, focus) : stand_in;
    const auto type = ui::gui_layout::gadget_type_of(focused);
    const auto& root = records[0];

    const auto finish_consumed = [&](int32_t result) {
        panel.dirty = 1;
        chosen = result;
    };

    bool consumed = false;
    int32_t result = key;
    // A text box keeps the caret keys. They are not a scroll or a page of the screen.
    if (type == gadget_type::text_box &&
        (key == key_code::left || key == key_code::right || key == key_code::home ||
         key == key_code::end || key == key_code::page_up || key == key_code::page_down))
        return key;

    ui::gui_layout::Gadget focused_gadget;
    const ui::gui_layout::Gadget* focused_query = nullptr;
    if (valid_index(panel, focus)) {
        focused_gadget = gadget_from_record(focused);
        focused_query = &focused_gadget;
    }
    FocusKeyQuery query;
    query.gadget = focused_query;
    query.key = key;
    query.shift = key_down(panel, key_code::shift);
    query.ring_shown = false;
    const FocusKeyEffect effect = focus_key_action(query);
    if (effect == FocusKeyEffect::tab_next || effect == FocusKeyEffect::tab_previous) {
        const bool forward = effect == FocusKeyEffect::tab_next;
        if (valid_index(panel, focus)) {
            std::vector<int32_t> order;
            {
                const int32_t end = scan_end(panel);
                std::vector<ui::gui_layout::Gadget> copied(static_cast<std::size_t>(end));
                for (int32_t index = 0; index < end; ++index)
                    copied[static_cast<std::size_t>(index)] =
                        gadget_from_record(record_at(panel, index));
                order = tab_order(copied);
            }
            bool found = false;
            int32_t landed = focus;
            for (std::size_t at = 0; at < order.size(); ++at) {
                if (order[at] != focus)
                    continue;
                found = true;
                if (forward)
                    landed = order[(at + 1U) % order.size()];
                else
                    landed = order[at == 0 ? order.size() - 1U : at - 1U];
                break;
            }
            if (found)
                move_keyboard_focus(panel, landed);
            else
                focus_nearest(panel, forward ? FocusDirection::next : FocusDirection::previous);
        } else {
            // Nothing is focused. Tab still moves, as it does today.
            focus_nearest(panel, forward ? FocusDirection::next : FocusDirection::previous);
        }
        finish_consumed(chosen);
        consumed = true;
    } else if (
        effect == FocusKeyEffect::scroll_back || effect == FocusKeyEffect::scroll_forward ||
        effect == FocusKeyEffect::scroll_page_back ||
        effect == FocusKeyEffect::scroll_page_forward || effect == FocusKeyEffect::scroll_home ||
        effect == FocusKeyEffect::scroll_end
    ) {
        if (valid_index(panel, focus)) {
            const auto& bar = record_at(panel, focus);
            place_scroll_knob(
                panel,
                focus,
                scrolled_knob(effect, i16(bar, field::scroll_range), i16(bar, field::scroll_knob))
            );
        }
        finish_consumed(chosen);
        consumed = true;
    } else if (effect == FocusKeyEffect::list_previous || effect == FocusKeyEffect::list_next) {
        if (effect == FocusKeyEffect::list_previous)
            list_select_previous(panel, focus);
        else
            list_select_next(panel, focus);
        if (focused.refs.list_changed != nullptr)
            focused.refs.list_changed(panel, focus);
        panel.dirty = 1;
        consumed = true;
    }

    auto activate_focused = [&]() -> bool {
        if (type == gadget_type::text_box ||
            (type != gadget_type::button && type != gadget_type::list_box &&
             type != gadget_type::hot_surface) ||
            focused.bytes[field::active] == 0 ||
            (type == gadget_type::button && (focused.bytes[field::button_flags] & 1) != 0)) {
            return false;
        }
        chosen = focus;
        if (type == gadget_type::button) {
            if ((focused.bytes[field::attributes] & attribute::text_list) != 0) {
                set_i16(focused, field::button_status, 1);
                clear_group_status(panel, focus);
                draw(panel, GadgetDraw::button, focus);
            }
            if (focused.bytes[field::button_stages] != 0) {
                auto& stage = focused.bytes[field::button_stage];
                ++stage;
                if (focused.bytes[field::button_stages] <= stage)
                    stage = 0;
            }
        }
        return true;
    };

    switch (consumed ? 0 : key) {
    case key_code::left:
        if (type == gadget_type::text_box)
            return key_code::left;
        if (type == gadget_type::scroll_bar &&
            i16(focused, field::height) < i16(focused, field::width))
            step_scroll_back(panel, focus);
        else
            focus_nearest(panel, FocusDirection::previous);
        finish_consumed(chosen);
        consumed = true;
        break;
    case key_code::tab:
        focus_nearest(
            panel,
            key_down(panel, key_code::shift) ? FocusDirection::previous : FocusDirection::next
        );
        finish_consumed(chosen);
        consumed = true;
        break;
    case key_code::enter: {
        if (panel.captured != kNoGadget &&
            ui::gui_layout::gadget_type_of(record_at(panel, panel.captured)) ==
                gadget_type::text_box) {
            return key_code::enter;
        }
        const auto target =
            find_gadget(records, ui::gui_layout::record_string(root, field::enter_default));
        if (target != kNoGadget && activatable(record_at(panel, target))) {
            chosen = target;
            consumed = true;
        } else if (activate_focused()) {
            consumed = true;
        }
        break;
    }
    case key_code::escape: {
        const auto target =
            find_gadget(records, ui::gui_layout::record_string(root, field::escape_default));
        if (target == kNoGadget || record_at(panel, target).bytes[field::active] == 0)
            return key_code::escape;
        chosen = target;
        consumed = true;
        break;
    }
    case key_code::space:
        consumed = activate_focused();
        break;
    case key_code::up:
        if (type == gadget_type::list_box) {
            list_select_previous(panel, focus);
            if (focused.refs.list_changed != nullptr)
                focused.refs.list_changed(panel, focus);
        } else {
            focus_nearest(panel, FocusDirection::up);
        }
        panel.dirty = 1;
        consumed = true;
        break;
    case key_code::right:
        if (type == gadget_type::text_box)
            return key_code::right;
        if (type == gadget_type::scroll_bar &&
            i16(focused, field::height) < i16(focused, field::width))
            step_scroll_forward(panel, focus);
        else
            focus_nearest(panel, FocusDirection::next);
        finish_consumed(chosen);
        consumed = true;
        break;
    case key_code::down:
        if (type == gadget_type::list_box) {
            list_select_next(panel, focus);
            if (focused.refs.list_changed != nullptr)
                focused.refs.list_changed(panel, focus);
        } else {
            focus_nearest(panel, FocusDirection::down);
        }
        finish_consumed(chosen);
        consumed = true;
        break;
    default:
        break;
    }
    if (consumed) {
        result = 0;
        if (panel.owner->keys_from_queue == 0)
            static_cast<void>(pop_key(panel));
    }
    if (chosen != kNoGadget) {
        panel.activated = chosen;
        panel.dirty = 1;
    }
    return result;
}

int32_t update_panel(GadgetPanel& panel) {
    if (!panel.owner)
        return 0;
    const auto tick = current_tick(panel);
    const auto previous_tick = panel.last_tick;
    panel.last_tick = tick;
    panel.tick_delta = tick - previous_tick;
    tick_cursor_and_read_pointer(panel);

    int32_t key = 0;
    if (panel.owner->keys_from_queue == 0) {
        key = peek_key(panel);
        if (key >= kPeekIgnoredFirst && key <= kPeekIgnoredLast)
            key = 0;
    } else {
        key = pop_key(panel);
    }
    if (panel.owner->keys_from_queue != 0 && key != 0 && panel.keyboard_enabled != 0) {
        key = dispatch_key(panel, key);
        if (key != 0) {
            auto& history = panel.owner->key_history;
            std::copy(history.begin() + 1, history.end(), history.begin());
            history.back() = static_cast<uint8_t>(upper(key));
            if (panel.owner->on_key != nullptr)
                panel.owner->on_key(panel);
            panel.activated = kNoGadget;
        }
    }

    int32_t chosen = panel.activated;
    if (panel.dirty == 1) {
        panel.dirty = 0;
        draw_panel(panel, panel.owner->flags | panel_flag::redraw);
    }
    auto records = panel.owner->records();
    const auto root_rect = ui::gui_layout::screen_rect(records, 0);
    const bool inside_root =
        ui::gui_layout::rect_contains(root_rect, panel.pointer.x, panel.pointer.y);
    follow_hover_cursor(panel, inside_root);
    const auto previous_hover = panel.hovered;
    panel.hovered = kNoGadget;
    const auto pointer = local_pointer(panel);
    bool repeat = false;
    if (static_cast<int32_t>(current_tick(panel) - panel.hover_repeat_timer) >= 1) {
        repeat = true;
        panel.hover_repeat_timer = current_tick(panel);
    }

    const auto end = scan_end(panel);
    for (int32_t index = 1; index < end; ++index) {
        auto& record = record_at(panel, index);
        if (record.bytes[field::active] == 0) {
            if (chosen != kNoGadget)
                break;
            continue;
        }
        if (ui::gui_layout::rect_contains(
                ui::gui_layout::panel_relative_rect(record), pointer.x, pointer.y
            ))
            panel.hovered = index;
        const bool alt = key_down(panel, key_code::alt);
        const auto type = ui::gui_layout::gadget_type_of(record);
        bool check_break = true;
        switch (type) {
        case gadget_type::hot_surface:
            if (hot_surface_pointer(panel, index))
                chosen = index;
            break;
        case gadget_type::image: {
            const auto flash = i32(record, field::flash_level);
            if (flash != 0 && repeat) {
                set_i32(record, field::flash_level, flash - 1);
                draw(panel, GadgetDraw::image, index);
                owner_redraw(panel);
            }
            break;
        }
        case gadget_type::progress:
            progress_tick(panel, index);
            break;
        case gadget_type::label: {
            if (!label_pointer(panel, index, key))
                break;
            const auto target =
                find_gadget(records, ui::gui_layout::record_string(record, field::label_link));
            if (target == kNoGadget) {
                chosen = index;
                break;
            }
            auto& linked = record_at(panel, target);
            if (ui::gui_layout::gadget_type_of(linked) == gadget_type::button) {
                if (linked.bytes[field::active] != 0 &&
                    (linked.bytes[field::button_flags] & 1) == 0) {
                    auto& stage = linked.bytes[field::button_stage];
                    ++stage;
                    if (linked.bytes[field::button_stages] <= stage)
                        stage = 0;
                    draw(panel, GadgetDraw::button, target);
                    chosen = target;
                } else {
                    chosen = kNoGadget;
                }
                break;
            }
            if (linked.bytes[field::active] == 0) {
                chosen = kNoGadget;
                break;
            }
            if (ui::gui_layout::gadget_type_of(linked) != gadget_type::scroll_bar ||
                i32(linked, field::scroll_locked) == 0) {
                set_focus(panel, target);
                chosen = target;
                break;
            }
            chosen = kNoGadget;
            check_break = false;
            break;
        }
        case gadget_type::button: {
            if (button_pointer(panel, index, key))
                chosen = index;
            const auto flash = i32(record, field::flash_level);
            if (flash != 0 && repeat) {
                set_i32(record, field::flash_level, std::max(flash - 2, 0));
                owner_redraw(panel);
            }
            break;
        }
        case gadget_type::list_box:
            if (list_pointer(panel, index))
                chosen = index;
            break;
        case gadget_type::text_box:
            if (text_box_pointer(panel, index, alt ? 0 : key))
                chosen = index;
            break;
        case gadget_type::scroll_bar:
            scroll_bar_pointer(panel, index);
            break;
        default:
            break;
        }
        if (check_break && chosen != kNoGadget)
            break;
    }

    if (panel.hovered != previous_hover)
        refresh_help(panel);
    if (panel.owner->on_tick != nullptr)
        panel.owner->on_tick();
    if (chosen != kNoGadget) {
        panel.activated = chosen;
        set_focus(panel, chosen);
        if (panel.owner->on_command != nullptr)
            panel.owner->on_command(panel);
        if (panel.activated != kNoGadget)
            close_top_panel(panel);
    }
    return 1;
}

namespace {

/// Wraps text with Chinese, Japanese or Korean characters: a line breaks
/// where the line breaker allows (oa::base::text::first_row), at a space
/// or between two Chinese or Japanese characters, which are written without
/// spaces, before it reaches the width. A break at spaces drops them; each
/// break is CR LF, and the text's own line breaks stay.
///
/// @param text the text
/// @param width the line width, in pixels
/// @param measure a NUL-ended line's width, in pixels
/// @return the wrapped text
template <class Measure>
std::string wrap_wide_text(std::string_view text, int32_t width, const Measure& measure) {
    std::string out;
    std::string line;
    const auto fits = [&](std::string_view start) {
        line.assign(start);
        return measure(line.c_str()) < width;
    };
    for (;;) {
        const auto newline = text.find('\n');
        std::string_view rest = text.substr(0, newline);
        // A CR before the line break stays with it.
        const bool carriage = !rest.empty() && rest.back() == '\r';
        if (carriage)
            rest.remove_suffix(1);
        while (!rest.empty()) {
            const auto row = oa::base::text::first_row(rest, fits);
            out.append(rest.substr(0, row.bytes));
            rest.remove_prefix(row.next);
            if (!rest.empty())
                out += "\r\n";
        }
        if (carriage)
            out += '\r';
        if (newline == std::string_view::npos)
            return out;
        out += '\n';
        text.remove_prefix(newline + 1);
    }
}

} // namespace

std::string
wrap_text(GadgetPanel& panel, std::string_view text, int32_t width, int32_t font_record) {
    const auto measure = [&](const char* value) {
        if (font_record == -1)
            return text_width(panel, value);
        return panel.host.text_width ? panel.host.text_width(panel.host.context, nullptr, value)
                                     : 0;
    };
    if (font_record != -1)
        apply_gadget_font(panel, font_record);
    if (const auto ended = text.substr(0, std::min(text.find('\0'), text.find('\xFF')));
        oa::base::text::has_wide_script(ended))
        return wrap_wide_text(ended, width, measure);
    const auto glyph = std::max(measure("d"), 1);
    const auto length = static_cast<int32_t>(text.size());
    const auto per_line = std::max(width / glyph, 1);
    const auto size = static_cast<std::size_t>((length / per_line) * 3 + 2 + length);
    std::vector<char> out(size + 2, '\0');
    std::size_t written = 0;
    std::size_t line_start = 0;
    std::size_t position = 0;
    const auto at = [&](std::size_t index) -> unsigned char {
        return index < text.size() ? static_cast<unsigned char>(text[index]) : 0;
    };
    for (;;) {
        if (at(position) == 0 || at(position) == 0xFF) {
            out[written] = '\0';
            break;
        }
        if (written + 2 >= out.size())
            out.resize(out.size() * 2, '\0');
        out[written++] = static_cast<char>(at(position));
        const auto next = at(position + 1);
        ++position;
        if (next == ' ' || next == '\n' || next == '-') {
            if (width <= measure(out.data() + line_start)) {
                std::size_t cut = written;
                std::size_t source = position;
                for (;;) {
                    cut = written;
                    position = source;
                    out[cut] = '\0';
                    if (position == 0 || at(position - 1) == ' ')
                        break;
                    source = position - 1;
                    written = cut - 1;
                    if (at(position - 1) == '-')
                        break;
                }
                if (cut >= 1)
                    out[cut - 1] = '\r';
                out[cut] = '\n';
                written = cut + 1;
                line_start = written;
            }
        }
        if (at(position) == '\n')
            line_start = written + 1;
    }
    return std::string(out.data());
}

void alloc_blink_words(GadgetPanel& panel, int32_t count) {
    panel.blink_words.assign(static_cast<std::size_t>(std::max(count, 0)), BlinkWord{});
    panel.blink_words_active = 1;
    if (!panel.blink_words.empty())
        panel.blink_words[0].font_record = -1;
}

void free_blink_words(GadgetPanel& panel) {
    panel.blink_words.clear();
    panel.blink_words_active = 0;
}

void set_blink_font_record(GadgetPanel& panel, int32_t record) {
    if (!panel.blink_words.empty())
        panel.blink_words[0].font_record = record;
}

void clear_blink_words(GadgetPanel& panel) {
    for (auto& word : panel.blink_words)
        word.text[0] = '\0';
}

void update_blink_words(GadgetPanel& panel) {
    if (panel.blink_words_active == 0 || panel.blink_words.empty())
        return;
    const auto tick = current_tick(panel);
    const auto font_record = panel.blink_words[0].font_record;
    if (font_record != -1)
        apply_gadget_font(panel, font_record);
    const auto rate = [&] {
        return panel.host.ticks_per_second ? panel.host.ticks_per_second(panel.host.context) : 0;
    };
    for (auto& word : panel.blink_words) {
        if (word.text[0] == '\0')
            continue;
        const auto now = static_cast<float>(static_cast<int32_t>(tick));
        if (!(word.next_tick >= now)) {
            const auto duration = word.lit == 0 ? word.on_seconds : word.off_seconds;
            word.next_tick = static_cast<float>(
                static_cast<double>(rate()) * static_cast<double>(duration) +
                static_cast<double>(now)
            );
            word.lit = word.lit == 0 ? 1 : 0;
        }
        const auto color = word.lit == 0 ? word.color_off : word.color_on;
        if (panel.host.draw_label != nullptr)
            panel.host.draw_label(
                panel.host.context, panel, word.text.data(), word.x, word.y, color
            );
    }
}

} // namespace oa::ui::gui_input
