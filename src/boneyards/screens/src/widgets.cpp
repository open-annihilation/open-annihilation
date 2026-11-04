// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/widgets.hpp"

#include "oa/boneyards/toolkit.hpp"
#include "text.hpp"

#include <cstdio>
#include <cstring>

namespace oa::boneyards::screens {

bool radio_add(RadioGroup* g, const char* name, std::int32_t value) {
    if (g->count < 0 || g->count >= static_cast<std::int32_t>(kRadioGroupCapacity))
        return false;
    RadioMember* m = &g->members[g->count++];
    copy_bounded(m->name, sizeof m->name, name);
    m->value = value;
    return true;
}

void radio_select_member(RadioGroup* g, std::int32_t member) {
    g->selected = -1;
    for (std::int32_t i = 0; i < g->count; ++i) {
        if (i == member) {
            g->selected = i;
            g->members[i].value = widget_value::selected;
        } else if (g->members[i].value != widget_value::disabled) {
            g->members[i].value = widget_value::enabled;
        }
    }
}

void radio_set_index(RadioGroup* g, std::int32_t index) {
    g->selected = index;
    radio_select_member(g, index);
}

void radio_select_by_name(RadioGroup* g, const char* name) {
    for (std::int32_t i = 0; i < g->count; ++i) {
        if (std::strcmp(g->members[i].name, name) == 0) {
            radio_set_index(g, i);
            return;
        }
    }
}

void radio_refresh(RadioGroup* g) {
    for (std::int32_t i = 0; i < g->count; ++i)
        g->members[i].value = i == g->selected ? widget_value::selected : widget_value::enabled;
}

void radio_disable_by_name(RadioGroup* g, const char* name) {
    for (std::int32_t i = 0; i < g->count; ++i) {
        if (std::strcmp(g->members[i].name, name) == 0) {
            g->members[i].value = widget_value::disabled;
            return;
        }
    }
}

void radio_disable_others(RadioGroup* g) {
    for (std::int32_t i = 0; i < g->count; ++i)
        if (i != g->selected)
            g->members[i].value = widget_value::disabled;
}

void radio_clear(RadioGroup* g) {
    g->selected = -1;
    for (std::int32_t i = 0; i < g->count; ++i)
        g->members[i].value = widget_value::disabled;
}

void format_signed_label(char* out, std::size_t capacity, std::uint32_t value) {
    std::snprintf(out, capacity, "%d", static_cast<std::int32_t>(value));
}

void count_label_set(CountLabel* label, std::uint32_t value) {
    label->count = static_cast<std::int32_t>(value);
    format_signed_label(label->text, sizeof label->text, value);
}

void count_label_add(CountLabel* label, std::int32_t delta) {
    label->count = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(label->count) + static_cast<std::uint32_t>(delta)
    );
    format_signed_label(label->text, sizeof label->text, static_cast<std::uint32_t>(label->count));
}

bool is_list_item_class(ListItemClass item, const char* class_name) {
    static constexpr const char* kNames[] = {
        "BYPlayerListItem",
        "BYCircleLstItem",
        "BYChatRoomListItem",
        "BYTOCListItem",
        "BYTOCCategoryListItem",
    };
    return names_equal(class_name, kNames[static_cast<std::size_t>(item)]);
}

bool expand_template_tags(
    char* out, std::size_t* size, const char* text, TemplateLookup lookup, void* context
) {
    if (text == nullptr)
        return false;
    for (int pass = 1; pass < 3; ++pass) {
        // The capacity is checked before the second pass copies, so a short
        // buffer is left untouched.
        char* dest = pass == 2 ? out : nullptr;
        std::size_t n = 0;
        const char* p = text;
        while (*p != '\0') {
            if (p[0] != '<' || p[1] != '$') {
                if (dest != nullptr)
                    dest[n] = *p;
                ++n;
                ++p;
                continue;
            }
            char tag[0x100];
            std::size_t tag_len = 0;
            const char* q = p + 2;
            for (; *q != '\0' && *q != '|' && *q != '>'; ++q)
                if (tag_len + 1 < sizeof tag)
                    tag[tag_len++] = *q;
            tag[tag_len] = '\0';
            const char* value = nullptr;
            if (lookup != nullptr && lookup(context, tag, &value)) {
                const std::size_t len = value != nullptr ? std::strlen(value) : 0;
                if (dest != nullptr && len != 0)
                    std::memcpy(dest + n, value, len);
                n += len;
                while (*q != '\0' && *q != '$')
                    ++q;
            } else if (*q != '\0') {
                while (q[1] != '\0' && q[1] != '$') {
                    ++q;
                    if (dest != nullptr)
                        dest[n] = *q;
                    ++n;
                }
                ++q;
            }
            p = q;
            if (*q != '\0') {
                p = q + 1;
                if (*p != '\0')
                    p = q + 2;
            }
        }
        if (pass == 1 && out == nullptr) {
            *size = n + 1;
            return true;
        }
        if (pass == 1 && *size < n + 1)
            return false;
        if (dest != nullptr)
            dest[n] = '\0';
    }
    return true;
}

} // namespace oa::boneyards::screens
