// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/gui_input/tab_order.hpp"

#include "oa/ui/gui_input/gadget_panel.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oa::ui::gui_input {
namespace {

using ui::gui_layout::Gadget;
using ui::gui_layout::GadgetType;

constexpr int32_t kColumnSlack = 16;
constexpr int32_t kRowCentreSlack = 8;
// A row's next control starts within this of the one before it. The music
// transport sits 14 apart; a screen's next column is further, so it stays a
// column.
constexpr int32_t kRowGap = 40;

[[nodiscard]] std::string lower_ascii(std::string_view text) {
    std::string lowered(text);
    for (char& character : lowered)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return lowered;
}

[[nodiscard]] bool name_is(const Gadget& gadget, std::string_view expected) {
    return lower_ascii(gadget.common.name) == lower_ascii(expected);
}

[[nodiscard]] bool name_has(const Gadget& gadget, std::string_view part) {
    return lower_ascii(gadget.common.name).find(lower_ascii(part)) != std::string::npos;
}

[[nodiscard]] bool has_name(std::span<const Gadget> gadgets, std::string_view expected) {
    return std::any_of(gadgets.begin(), gadgets.end(), [&](const Gadget& gadget) {
        return name_is(gadget, expected);
    });
}

[[nodiscard]] bool takes_tab(const Gadget& gadget) {
    if (gadget.common.width <= 0 || gadget.common.height <= 0)
        return false;
    return ui::gui_layout::keyboard_focusable(gadget);
}

[[nodiscard]] std::vector<int32_t> focusable_of(std::span<const Gadget> gadgets) {
    std::vector<int32_t> indices;
    for (std::size_t index = 1; index < gadgets.size(); ++index) {
        if (takes_tab(gadgets[index]))
            indices.push_back(static_cast<int32_t>(index));
    }
    return indices;
}

struct Placed {
    int32_t index = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t centre_y = 0;
};

[[nodiscard]] Placed place_of(const Gadget& gadget, int32_t index) {
    Placed placed;
    placed.index = index;
    placed.x = gadget.common.x;
    placed.y = gadget.common.y;
    placed.width = gadget.common.width;
    placed.centre_y =
        static_cast<int32_t>(gadget.common.y) + static_cast<int32_t>(gadget.common.height) / 2;
    return placed;
}

[[nodiscard]] std::vector<int32_t>
column_order(std::span<const Gadget> gadgets, std::span<const int32_t> indices) {
    std::vector<Placed> places;
    places.reserve(indices.size());
    for (const int32_t index : indices) {
        if (index <= 0 || static_cast<std::size_t>(index) >= gadgets.size())
            continue;
        places.push_back(place_of(gadgets[static_cast<std::size_t>(index)], index));
    }
    std::sort(places.begin(), places.end(), [](const Placed& left, const Placed& right) {
        if (left.x != right.x)
            return left.x < right.x;
        if (left.y != right.y)
            return left.y < right.y;
        return left.index < right.index;
    });
    std::vector<std::vector<Placed>> columns;
    int32_t column_x = 0;
    for (const Placed& placed : places) {
        if (columns.empty() || placed.x - column_x >= kColumnSlack) {
            columns.emplace_back();
            column_x = placed.x;
        }
        columns.back().push_back(placed);
    }
    for (std::vector<Placed>& column : columns) {
        std::sort(column.begin(), column.end(), [](const Placed& left, const Placed& right) {
            if (left.y != right.y)
                return left.y < right.y;
            if (left.x != right.x)
                return left.x < right.x;
            return left.index < right.index;
        });
    }
    std::vector<Placed> reading;
    for (const std::vector<Placed>& column : columns)
        reading.insert(reading.end(), column.begin(), column.end());

    std::vector<int32_t> order;
    std::vector<char> emitted(reading.size(), 0);
    for (std::size_t index = 0; index < reading.size(); ++index) {
        if (emitted[index] != 0)
            continue;
        std::vector<std::size_t> run;
        run.push_back(index);
        const int32_t centre = reading[index].centre_y;
        for (std::size_t later = index + 1; later < reading.size(); ++later) {
            if (emitted[later] != 0)
                continue;
            const int32_t delta = reading[later].centre_y - centre;
            if (delta <= kRowCentreSlack && delta >= -kRowCentreSlack)
                run.push_back(later);
        }
        std::sort(run.begin(), run.end(), [&](std::size_t left, std::size_t right) {
            if (reading[left].x != reading[right].x)
                return reading[left].x < reading[right].x;
            return reading[left].index < reading[right].index;
        });
        // Only the controls that sit against each other are one row. A
        // control further along the same line is the next column.
        std::vector<std::size_t> chain;
        chain.push_back(run.front());
        for (std::size_t at = 1; at < run.size(); ++at) {
            const Placed& previous = reading[chain.back()];
            const Placed& candidate = reading[run[at]];
            const int32_t gap = candidate.x - (previous.x + previous.width);
            if (gap > kRowGap)
                break;
            chain.push_back(run[at]);
        }
        for (const std::size_t at : chain) {
            emitted[at] = 1;
            order.push_back(reading[at].index);
        }
    }
    return order;
}

[[nodiscard]] int32_t find_stop(
    std::span<const Gadget> gadgets, std::span<const int32_t> stops, std::string_view expected
) {
    for (const int32_t index : stops) {
        if (name_is(gadgets[static_cast<std::size_t>(index)], expected))
            return index;
    }
    return -1;
}

class Order {
  public:

    explicit Order(std::span<const Gadget> gadgets, std::span<const int32_t> stops)
        : gadgets_(gadgets), stops_(stops), taken_(gadgets.size(), 0) {}

    void take(int32_t index) {
        if (index <= 0 || static_cast<std::size_t>(index) >= taken_.size() ||
            taken_[static_cast<std::size_t>(index)] != 0)
            return;
        taken_[static_cast<std::size_t>(index)] = 1;
        order_.push_back(index);
    }

    void take_name(std::string_view expected) { take(find_stop(gadgets_, stops_, expected)); }

    void take_containing(std::string_view part) {
        for (const int32_t index : stops_) {
            if (taken_[static_cast<std::size_t>(index)] != 0)
                continue;
            if (name_has(gadgets_[static_cast<std::size_t>(index)], part))
                take(index);
        }
    }

    void take_sorted(std::vector<int32_t> indices) {
        std::sort(indices.begin(), indices.end(), [&](int32_t left, int32_t right) {
            const Gadget& a = gadgets_[static_cast<std::size_t>(left)];
            const Gadget& b = gadgets_[static_cast<std::size_t>(right)];
            if (a.common.y != b.common.y)
                return a.common.y < b.common.y;
            if (a.common.x != b.common.x)
                return a.common.x < b.common.x;
            return left < right;
        });
        for (const int32_t index : indices)
            take(index);
    }

    void take_rest_by_column() {
        std::vector<int32_t> rest;
        for (const int32_t index : stops_) {
            if (taken_[static_cast<std::size_t>(index)] == 0)
                rest.push_back(index);
        }
        for (const int32_t index : column_order(gadgets_, rest))
            take(index);
    }

    void take_rest_by_y() {
        std::vector<int32_t> rest;
        for (const int32_t index : stops_) {
            if (taken_[static_cast<std::size_t>(index)] == 0)
                rest.push_back(index);
        }
        take_sorted(std::move(rest));
    }

    [[nodiscard]] std::vector<int32_t> finish() { return std::move(order_); }

  private:

    std::span<const Gadget> gadgets_;
    std::span<const int32_t> stops_;
    std::vector<char> taken_;
    std::vector<int32_t> order_;
};

[[nodiscard]] std::optional<int32_t> digits_of(std::string_view text) {
    if (text.empty())
        return std::nullopt;
    int32_t value = 0;
    for (const char character : text) {
        if (!std::isdigit(static_cast<unsigned char>(character)))
            return std::nullopt;
        value = value * 10 + (character - '0');
    }
    return value;
}

// A player-row part is Player, Side, Color, Allies, Metal, Energy or Brain
// plus a slot number, or brain-seat.<slot>. "StartLocation" and a campaign
// brain named brain-seat.campaign are not row parts.
[[nodiscard]] std::optional<std::pair<int32_t, int32_t>> row_part(std::string_view name) {
    const std::string lowered = lower_ascii(name);
    constexpr std::string_view seat = "brain-seat.";
    if (lowered.size() > seat.size() && lowered.compare(0, seat.size(), seat) == 0) {
        if (const std::optional<int32_t> slot =
                digits_of(std::string_view(lowered).substr(seat.size())))
            return std::pair<int32_t, int32_t>{*slot, 6};
    }
    constexpr std::array<std::string_view, 7> prefixes{
        "player", "side", "color", "allies", "metal", "energy", "brain"
    };
    for (int32_t role = 0; role < static_cast<int32_t>(prefixes.size()); ++role) {
        const std::string_view prefix = prefixes[static_cast<std::size_t>(role)];
        if (lowered.size() <= prefix.size() || lowered.compare(0, prefix.size(), prefix) != 0)
            continue;
        if (const std::optional<int32_t> slot =
                digits_of(std::string_view(lowered).substr(prefix.size())))
            return std::pair<int32_t, int32_t>{*slot, role};
    }
    return std::nullopt;
}

[[nodiscard]] bool colour_cell(const Gadget& gadget) {
    if (gadget.common.type != GadgetType::button && gadget.common.type != GadgetType::hot_surface)
        return false;
    std::string lowered = lower_ascii(gadget.common.name);
    while (!lowered.empty() && std::isdigit(static_cast<unsigned char>(lowered.back())))
        lowered.pop_back();
    return lowered == "color";
}

[[nodiscard]] std::optional<std::vector<int32_t>>
named_order(std::span<const Gadget> gadgets, std::span<const int32_t> stops) {
    const bool load_game = has_name(gadgets, "GAMES") &&
                           (has_name(gadgets, "LOAD") || has_name(gadgets, "GAMENAME")) &&
                           !has_name(gadgets, "MAPNAMES");
    if (load_game) {
        Order order(gadgets, stops);
        order.take_name("GAMES");
        order.take_name("GAMENAME");
        order.take_name("LOAD");
        order.take_name("DELETE");
        order.take_name("CANCEL");
        order.take_rest_by_column();
        return order.finish();
    }
    if (has_name(gadgets, "MAPNAMES")) {
        Order order(gadgets, stops);
        order.take_name("MAPNAMES");
        order.take_name("LOAD");
        order.take_name("PREVMENU");
        order.take_rest_by_column();
        return order.finish();
    }
    const bool skirmish = has_name(gadgets, "CommanderDeath") ||
                          has_name(gadgets, "StartLocation") ||
                          (has_name(gadgets, "SelectMap") && has_name(gadgets, "Player0"));
    if (skirmish) {
        struct RowItem {
            int32_t index = 0;
            int32_t slot = 0;
            int32_t role = 0;
            int32_t x = 0;
        };

        std::vector<RowItem> rows;
        std::vector<int32_t> rules;
        for (const int32_t index : stops) {
            const Gadget& gadget = gadgets[static_cast<std::size_t>(index)];
            if (const std::optional<std::pair<int32_t, int32_t>> part =
                    row_part(gadget.common.name)) {
                RowItem item;
                item.index = index;
                item.slot = part->first;
                item.role = part->second;
                item.x = gadget.common.x;
                rows.push_back(item);
                continue;
            }
            if (name_is(gadget, "SelectMap") || name_is(gadget, "Start") ||
                name_is(gadget, "PrevMenu") || name_has(gadget, "mutat"))
                continue;
            rules.push_back(index);
        }
        std::sort(rules.begin(), rules.end(), [&](int32_t left, int32_t right) {
            const Gadget& a = gadgets[static_cast<std::size_t>(left)];
            const Gadget& b = gadgets[static_cast<std::size_t>(right)];
            if (a.common.y != b.common.y)
                return a.common.y < b.common.y;
            if (a.common.x != b.common.x)
                return a.common.x < b.common.x;
            return left < right;
        });
        std::sort(rows.begin(), rows.end(), [](const RowItem& left, const RowItem& right) {
            if (left.slot != right.slot)
                return left.slot < right.slot;
            if (left.role != right.role)
                return left.role < right.role;
            if (left.x != right.x)
                return left.x < right.x;
            return left.index < right.index;
        });
        Order order(gadgets, stops);
        for (const int32_t index : rules)
            order.take(index);
        for (const RowItem& item : rows)
            order.take(item.index);
        order.take_name("SelectMap");
        order.take_containing("mutat");
        order.take_name("Start");
        order.take_name("PrevMenu");
        order.take_rest_by_column();
        return order.finish();
    }
    const bool campaign_end = has_name(gadgets, "Start") && has_name(gadgets, "MainMenu");
    if (campaign_end) {
        Order order(gadgets, stops);
        order.take_name("Start");
        order.take_name("MainMenu");
        order.take_rest_by_y();
        return order.finish();
    }
    const bool new_campaign = has_name(gadgets, "Difficulty") && has_name(gadgets, "Start") &&
                              (has_name(gadgets, "Arm") || has_name(gadgets, "Core") ||
                               has_name(gadgets, "Campaign") || has_name(gadgets, "Missions"));
    if (new_campaign) {
        std::vector<int32_t> sides;
        for (const char* side : {"Arm", "Core", "Side0", "Side1"}) {
            const int32_t index = find_stop(gadgets, stops, side);
            if (index > 0)
                sides.push_back(index);
        }
        std::sort(sides.begin(), sides.end(), [&](int32_t left, int32_t right) {
            const int16_t left_x = gadgets[static_cast<std::size_t>(left)].common.x;
            const int16_t right_x = gadgets[static_cast<std::size_t>(right)].common.x;
            if (left_x != right_x)
                return left_x < right_x;
            return left < right;
        });
        Order order(gadgets, stops);
        for (const int32_t index : sides)
            order.take(index);
        order.take_name("Campaign");
        order.take_name("Missions");
        order.take_name("Difficulty");
        order.take_containing("mutat");
        order.take_containing("brain");
        order.take_name("Start");
        order.take_name("PrevMenu");
        order.take_rest_by_column();
        return order.finish();
    }
    int32_t section_names = 0;
    for (const char* section : {"SOUND", "MUSIC", "SPEEDS", "VISUALS"}) {
        if (has_name(gadgets, section))
            ++section_names;
    }
    if (section_names >= 2) {
        std::vector<int32_t> sections;
        for (const char* section : {"SOUND", "MUSIC", "SPEEDS", "VISUALS"}) {
            const int32_t index = find_stop(gadgets, stops, section);
            if (index > 0)
                sections.push_back(index);
        }
        Order order(gadgets, stops);
        order.take_sorted(std::move(sections));
        order.take_rest_by_column();
        return order.finish();
    }
    if ((has_name(gadgets, "TextRegion") || has_name(gadgets, "MOREBAR")) &&
        (has_name(gadgets, "Start") || has_name(gadgets, "OK"))) {
        Order order(gadgets, stops);
        if (find_stop(gadgets, stops, "Start") > 0)
            order.take_name("Start");
        else
            order.take_name("OK");
        order.take_name("PrevMenu");
        order.take_rest_by_y();
        return order.finish();
    }
    return std::nullopt;
}

} // namespace

FocusKeyEffect focus_key_action(const FocusKeyQuery& query) noexcept {
    if (query.key == key_code::tab)
        return query.shift ? FocusKeyEffect::tab_previous : FocusKeyEffect::tab_next;
    const Gadget* gadget = query.gadget;
    if (gadget == nullptr)
        return FocusKeyEffect::unhandled;
    if (gadget->common.type == GadgetType::text_box &&
        (query.key == key_code::left || query.key == key_code::right ||
         query.key == key_code::home || query.key == key_code::end ||
         query.key == key_code::page_up || query.key == key_code::page_down))
        return FocusKeyEffect::unhandled;
    if (gadget->common.type == GadgetType::scroll_bar &&
        gadget->common.width > gadget->common.height) {
        bool locked = false;
        if (const auto* bar = std::get_if<ui::gui_layout::ScrollBarFields>(&gadget->fields))
            locked = bar->locked;
        if (!locked) {
            switch (query.key) {
            case key_code::left:
                return FocusKeyEffect::scroll_back;
            case key_code::right:
                return FocusKeyEffect::scroll_forward;
            case key_code::page_up:
                return FocusKeyEffect::scroll_page_back;
            case key_code::page_down:
                return FocusKeyEffect::scroll_page_forward;
            case key_code::home:
                return FocusKeyEffect::scroll_home;
            case key_code::end:
                return FocusKeyEffect::scroll_end;
            default:
                break;
            }
        }
    }
    if (gadget->common.type == GadgetType::list_box) {
        if (query.key == key_code::up)
            return FocusKeyEffect::list_previous;
        if (query.key == key_code::down)
            return FocusKeyEffect::list_next;
    }
    if (query.ring_shown && colour_cell(*gadget)) {
        if (query.key == key_code::right)
            return FocusKeyEffect::colour_forward;
        if (query.key == key_code::left)
            return FocusKeyEffect::colour_back;
    }
    return FocusKeyEffect::unhandled;
}

std::vector<int32_t> tab_order(std::span<const Gadget> gadgets) {
    const std::vector<int32_t> stops = focusable_of(gadgets);
    if (const std::optional<std::vector<int32_t>> named = named_order(gadgets, stops))
        return *named;
    return column_order(gadgets, stops);
}

} // namespace oa::ui::gui_input
