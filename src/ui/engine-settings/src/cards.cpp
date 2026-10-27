// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Controls' key layout card and Accessibility's practice pad: where their
// parts lie, and what the pad does with the keys the player tries there.

#include "cards.hpp"

#include "oa/ui/kit/text.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::engine_settings {

namespace cards {

namespace {

namespace kit = oa::ui::kit;

/// The narrowest the key layout card's words may be with its buttons beside
/// them; narrower, the buttons stand under the words.
constexpr int32_t kLeastBesideWords = 120;
/// The practice pad's column beside Accessibility's rows at Regular and at Large.
constexpr std::array<int32_t, 3> kPadColumns{0, 200, 260};
/// The rows between a card's lines that belong together.
constexpr int32_t kLineGap = 2;

/// The modifiers, in the order the pad shows their keycaps, and their legends.
constexpr std::array<std::pair<PracticeModifier, std::string_view>, 3> kModifiers{{
    {PracticeModifier::shift, "Shift"},
    {PracticeModifier::ctrl, "Ctrl"},
    {PracticeModifier::alt, "Alt"},
}};

/// The key Hold to confirm's chord takes with Ctrl: self-destruct's.
constexpr std::string_view kHoldKey = "D";

/// What to try, with Sticky modifiers on.
constexpr std::string_view kTapShiftText = "Tap Shift, then press A.";
/// What to try, with Slow keys on.
constexpr std::string_view kHoldUntilText = "Hold a key until it counts.";
/// What to try, with Bounce keys on.
constexpr std::string_view kTwiceText = "Press a key twice, quickly.";
/// What to try, with none of those on.
constexpr std::string_view kAnyKeyText = "Press a key, alone or with Shift, Ctrl or Alt.";
/// Before a key is pressed.
constexpr std::string_view kStayHereText = "Keys pressed here never reach the game.";
/// A modifier tapped alone with Sticky modifiers on.
constexpr std::string_view kLatchedText = "{key} stays down for the next key.";
/// A modifier tapped twice.
constexpr std::string_view kLockedText = "{key} stays down until you tap it again.";
/// A locked modifier tapped again.
constexpr std::string_view kUnlockedText = "{key} let go.";
/// A key that counted.
constexpr std::string_view kCountedText = "{chord} counted.";
/// A key that counted with latched modifiers, which then let go.
constexpr std::string_view kCountedReleasedText = "{chord} counted, then {keys} let go.";
/// A key held, short of Slow keys' time.
constexpr std::string_view kWaitingText = "{key} held {held} of {wait} s.";
/// A key let go before Slow keys' time.
constexpr std::string_view kTooShortText = "{key} did not count: let go after {held} of {wait} s.";
/// A key pressed again within Bounce keys' time.
constexpr std::string_view kBouncedText = "{key} did not count: pressed again within {wait} s.";
/// Hold to confirm's chord held to its end.
constexpr std::string_view kHeldText = "Held to the end: the order goes.";
/// Hold to confirm's chord let go early.
constexpr std::string_view kCancelledText = "Let go before the end: nothing happens.";
/// How to try Hold to confirm.
constexpr std::string_view kHoldTryText = "Hold Ctrl + D to try Hold to confirm.";
/// Hold to confirm's bar while it fills.
constexpr std::string_view kHoldingText = "{done} of {whole} s · let go to cancel";

/// Returns a time in seconds with one decimal, as the pad shows it.
///
/// @param ms the time, in milliseconds
/// @return the text, such as "0.3"
std::string seconds_text(uint64_t ms) {
    const uint64_t tenths = (ms + 50) / 100;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}

/// Returns the modifiers of a set joined by " + ", in kModifiers' order.
///
/// @param set the modifiers, practice_bit's bits
/// @return the text; empty for none
std::string modifier_names(uint8_t set) {
    std::string names;
    for (const auto& [modifier, name] : kModifiers) {
        if ((set & practice_bit(modifier)) == 0)
            continue;
        if (!names.empty())
            names += " + ";
        names += geometry::shown_text(name);
    }
    return names;
}

/// Returns a chord's text: its modifiers, then its key.
///
/// @param set the modifiers
/// @param key the key's legend
/// @return the text, such as "Shift + A"
std::string chord_text(uint8_t set, std::string_view key) {
    std::string chord = modifier_names(set);
    if (!chord.empty())
        chord += " + ";
    chord += key;
    return chord;
}

/// Breaks a text into lines of a width and appends them.
///
/// @param[in,out] lines the lines
/// @param text the text, as shown
/// @param font its font
/// @param colour its colour
/// @param measure a text's width in that font
/// @param left the lines' left column
/// @param width the lines' width
/// @param[in,out] y the first line's top row; moved under the last line
void add_wrapped(
    std::vector<Line>& lines,
    std::string_view text,
    DialogFont font,
    kit::Colour colour,
    const TextWidth& measure,
    int32_t left,
    int32_t width,
    int32_t& y
) {
    const int32_t height = font == DialogFont::regular ? kit::compact_metrics.regular_line
                                                       : kit::compact_metrics.small_line;
    for (std::string& wrapped : kit::wrap(text, std::max(width, int32_t{1}), measure)) {
        lines.push_back({{left, y, width, height}, std::move(wrapped), font, colour});
        y += height;
    }
}

/// Returns what the pad says of the last key.
///
/// @param dialog the dialog
/// @return the words, as shown
std::string outcome_text(const Dialog& dialog) {
    const PracticePad& pad = dialog.practice;
    const EngineSettings& on = dialog.chosen;
    using geometry::filled;
    // A key held short of Slow keys' time says how far it has come.
    if (!pad.key_down.empty() && !pad.key_counted && !pad.key_bounced && !pad.holding &&
        on.slow_keys_ms > 0)
        return filled(
            kWaitingText,
            {{"key", pad.key_down},
             {"held", seconds_text(pad.now_ms - pad.key_down_ms)},
             {"wait", seconds_text(on.slow_keys_ms)}}
        );
    switch (pad.outcome) {
    case PracticeOutcome::none:
        break;
    case PracticeOutcome::latched:
        return filled(kLatchedText, {{"key", pad.outcome_key}});
    case PracticeOutcome::locked:
        return filled(kLockedText, {{"key", pad.outcome_key}});
    case PracticeOutcome::unlocked:
        return filled(kUnlockedText, {{"key", pad.outcome_key}});
    case PracticeOutcome::counted:
        if (pad.outcome_released != 0)
            return filled(
                kCountedReleasedText,
                {{"chord", chord_text(pad.outcome_modifiers, pad.outcome_key)},
                 {"keys", modifier_names(pad.outcome_released)}}
            );
        return filled(
            kCountedText, {{"chord", chord_text(pad.outcome_modifiers, pad.outcome_key)}}
        );
    case PracticeOutcome::too_short:
        return filled(
            kTooShortText,
            {{"key", pad.outcome_key},
             {"held", seconds_text(pad.outcome_ms)},
             {"wait", seconds_text(on.slow_keys_ms)}}
        );
    case PracticeOutcome::bounced:
        return filled(
            kBouncedText, {{"key", pad.outcome_key}, {"wait", seconds_text(on.bounce_keys_ms)}}
        );
    case PracticeOutcome::held:
        return std::string(geometry::shown_text(kHeldText));
    case PracticeOutcome::cancelled:
        return std::string(geometry::shown_text(kCancelledText));
    }
    return std::string(geometry::shown_text(kStayHereText));
}

/// Returns what the pad asks the player to try, by the first helper on.
///
/// @param settings the settings shown
/// @return the words, as shown
std::string_view prompt_text(const EngineSettings& settings) {
    if (settings.sticky_modifiers)
        return geometry::shown_text(kTapShiftText);
    if (settings.slow_keys_ms > 0)
        return geometry::shown_text(kHoldUntilText);
    if (settings.bounce_keys_ms > 0)
        return geometry::shown_text(kTwiceText);
    return geometry::shown_text(kAnyKeyText);
}

/// Counts the key held now: the outcome names its chord, and the latched
/// modifiers it used let go.
///
/// @param[in,out] pad the pad
void count_key(PracticePad& pad) {
    pad.key_counted = true;
    pad.outcome = PracticeOutcome::counted;
    pad.outcome_key = pad.key_down;
    pad.outcome_modifiers = pad.key_modifiers;
    pad.outcome_released = static_cast<uint8_t>(pad.latched & pad.key_modifiers);
    pad.latched = 0;
}

} // namespace

KeyCard key_card(
    const Dialog& dialog,
    const geometry::Sizes& sized,
    int32_t top,
    const TextWidth& regular,
    const TextWidth& small
) {
    const kit::Metrics& metrics = kit::metrics_of(sized.size_class);
    const int32_t inset = metrics.space * 2;
    const int32_t gap = metrics.space * 2;
    const int32_t tile_side = metrics.row_height;
    KeyCard card;
    card.card = {sized.content_left, top, sized.content_width, 0};
    const int32_t right = card.card.x + card.card.width - inset;

    // The buttons: Change layout… only where the Library can open.
    card.edit_caption = std::string(geometry::shown_text(edit_keys_text));
    const int32_t button_height = metrics.control_height;
    const auto button_width = [&](const std::string& caption) {
        return std::max(regular(caption) + 4 * metrics.space, 2 * metrics.control_height);
    };
    const int32_t edit_width = button_width(card.edit_caption);
    int32_t change_width = 0;
    if (dialog.library_link) {
        card.change_caption = std::string(geometry::shown_text(change_layout_text));
        change_width = button_width(card.change_caption);
    }
    const int32_t buttons = edit_width + (change_width > 0 ? change_width + gap : 0);

    const int32_t text_left = card.card.x + inset + tile_side + gap;
    const int32_t beside_width = right - buttons - gap - text_left;
    const bool beside = beside_width >= kLeastBesideWords;
    const int32_t text_width = beside ? beside_width : right - text_left;

    int32_t y = top + inset;
    card.lines.push_back(
        {{text_left, y, text_width, kit::compact_metrics.small_line},
         std::string(geometry::shown_text(key_layout_eyebrow_text)),
         DialogFont::small,
         kit::colour::text_tertiary}
    );
    y += kit::compact_metrics.small_line + kLineGap;

    std::string name =
        dialog.key_layout_name.empty() ? dialog.key_layout_summary : dialog.key_layout_name;
    if (name.empty())
        name = "OA default";
    const std::size_t first_name = card.lines.size();
    add_wrapped(
        card.lines, name, DialogFont::regular, kit::colour::text, regular, text_left, text_width, y
    );
    // The badge follows the name on its last line, or takes a line of its own.
    if (dialog.key_layout_changes > 0) {
        card.badge_text =
            dialog.key_layout_changes == 1
                ? std::string(geometry::shown_text(one_own_change_text))
                : geometry::filled(
                      own_changes_text, {{"count", std::to_string(dialog.key_layout_changes)}}
                  );
        const int32_t badge_width = small(card.badge_text) + 4 * metrics.space;
        const int32_t badge_height = kit::compact_metrics.regular_line;
        const Line& last = card.lines.back();
        const int32_t after = last.rect.x + regular(last.text) + gap;
        if (card.lines.size() > first_name && after + badge_width <= text_left + text_width) {
            card.badge = {after, last.rect.y, badge_width, badge_height};
        } else {
            y += kLineGap;
            card.badge = {text_left, y, std::min(badge_width, text_width), badge_height};
            y += badge_height;
        }
    }
    y += kLineGap;
    const std::string applies =
        dialog.key_layout_mod_only && !dialog.played_mod.empty()
            ? geometry::filled(applies_with_mod_text, {{"mod", dialog.played_mod}})
            : std::string(geometry::shown_text(applies_everywhere_text));
    const std::string shown_keys =
        geometry::filled(keys_in_match_text, {{"primary", dialog.primary_modifier}});
    add_wrapped(
        card.lines,
        applies + " " + shown_keys,
        DialogFont::small,
        kit::colour::text_secondary,
        small,
        text_left,
        text_width,
        y
    );

    int32_t bottom = std::max(y, top + inset + tile_side);
    if (!beside)
        bottom = y + gap + button_height;
    card.height = bottom + inset - top;
    card.card.height = card.height;
    card.tile = {
        card.card.x + inset,
        beside ? top + (card.height - tile_side) / 2 : top + inset,
        tile_side,
        tile_side,
    };
    const int32_t button_top = beside ? top + (card.height - button_height) / 2 : y + gap;
    card.edit = {right - edit_width, button_top, edit_width, button_height};
    if (change_width > 0)
        card.change = {card.edit.x - gap - change_width, button_top, change_width, button_height};
    return card;
}

int32_t pad_column(const geometry::Sizes& sized) noexcept {
    const auto index = static_cast<std::size_t>(sized.size_class);
    return index < kPadColumns.size() ? kPadColumns[index] : 0;
}

Pad practice_pad(
    const Dialog& dialog,
    const geometry::Sizes& sized,
    int32_t left,
    int32_t top,
    int32_t width,
    const TextWidth& regular,
    const TextWidth& small
) {
    const kit::Metrics& metrics = kit::metrics_of(sized.size_class);
    const PracticePad& state = dialog.practice;
    const EngineSettings& on = dialog.chosen;
    const int32_t inset = metrics.space * 2;
    const int32_t space = metrics.space;
    const int32_t inner_left = left + inset;
    const int32_t inner = std::max(width - 2 * inset, int32_t{1});
    Pad pad;
    pad.card = {left, top, width, 0};
    int32_t y = top + inset;
    pad.lines.push_back(
        {{inner_left, y, inner, kit::compact_metrics.small_line},
         std::string(geometry::shown_text(try_it_text)),
         DialogFont::small,
         kit::colour::accent}
    );
    y += kit::compact_metrics.small_line + space;
    add_wrapped(
        pad.lines,
        prompt_text(on),
        DialogFont::regular,
        kit::colour::text,
        regular,
        inner_left,
        inner,
        y
    );
    y += space;

    // The well: the modifiers held, latched or locked, then the key, joined
    // by "+". Before any key it shows what to try, not pressed.
    std::vector<std::pair<std::string, bool>> shown;
    const auto down_set = static_cast<uint8_t>(state.held | state.latched | state.locked);
    for (const auto& [modifier, name] : kModifiers)
        if ((down_set & practice_bit(modifier)) != 0)
            shown.emplace_back(std::string(geometry::shown_text(name)), true);
    if (!state.key_down.empty())
        shown.emplace_back(state.key_down, true);
    else if (
        !state.outcome_key.empty() &&
        (state.outcome == PracticeOutcome::counted || state.outcome == PracticeOutcome::too_short ||
         state.outcome == PracticeOutcome::bounced)
    )
        shown.emplace_back(state.outcome_key, false);
    if (shown.empty()) {
        if (on.sticky_modifiers)
            shown.emplace_back(std::string(geometry::shown_text("Shift")), false);
        shown.emplace_back("A", false);
    }
    const int32_t cap_height = kit::compact_metrics.regular_line + 2 * space;
    const int32_t plus_width = regular("+") + 2 * space;
    const int32_t well_top = y;
    int32_t x = inner_left + space;
    int32_t row = well_top + space;
    for (std::size_t index = 0; index < shown.size(); ++index) {
        const int32_t cap_width = std::max(cap_height, regular(shown[index].first) + 4 * space);
        const int32_t needed = cap_width + (index > 0 ? plus_width : 0);
        // A row of keycaps that is full goes on under it; no keycap is cut.
        if (index > 0 && x + needed > inner_left + inner - space) {
            x = inner_left + space;
            row += cap_height + space;
        } else if (index > 0) {
            pad.lines.push_back(
                {{x, row, plus_width, cap_height},
                 "+",
                 DialogFont::regular,
                 kit::colour::text_tertiary,
                 kit::Align::centre}
            );
            x += plus_width;
        }
        pad.caps.push_back(
            {{x, row, cap_width, cap_height}, shown[index].first, shown[index].second}
        );
        x += cap_width;
    }
    pad.well = {inner_left, well_top, inner, row + cap_height + space - well_top};
    y = pad.well.y + pad.well.height + space;

    add_wrapped(
        pad.lines,
        outcome_text(dialog),
        DialogFont::small,
        kit::colour::text_secondary,
        small,
        inner_left,
        inner,
        y
    );

    if (on.hold_to_confirm) {
        y += space;
        add_wrapped(
            pad.lines,
            geometry::shown_text(kHoldTryText),
            DialogFont::small,
            kit::colour::text_tertiary,
            small,
            inner_left,
            inner,
            y
        );
        y += kLineGap;
        pad.bar_whole = static_cast<int32_t>(std::max<uint32_t>(on.long_press_ms, 1));
        if (state.holding)
            pad.bar_done = static_cast<int32_t>(
                std::min<uint64_t>(state.now_ms - state.hold_started_ms, on.long_press_ms)
            );
        else if (state.outcome == PracticeOutcome::held)
            pad.bar_done = pad.bar_whole;
        pad.bar = {inner_left, y, inner, kit::compact_metrics.progress_bar_height};
        y += pad.bar.height + kLineGap;
        if (state.holding)
            add_wrapped(
                pad.lines,
                geometry::filled(
                    kHoldingText,
                    {{"done", seconds_text(static_cast<uint64_t>(pad.bar_done))},
                     {"whole", seconds_text(on.long_press_ms)}}
                ),
                DialogFont::small,
                kit::colour::text_secondary,
                small,
                inner_left,
                inner,
                y
            );
    }

    y += space * 2;
    pad.off_caption = std::string(geometry::shown_text(helpers_off_text));
    pad.off_enabled = any_helper_on(on);
    pad.off = {
        inner_left,
        y,
        std::min(regular(pad.off_caption) + 4 * space, inner),
        metrics.control_height,
    };
    y += pad.off.height + inset;
    pad.height = y - top;
    pad.card.height = pad.height;
    return pad;
}

bool any_helper_on(const EngineSettings& settings) noexcept {
    return settings.sticky_modifiers || settings.hold_to_confirm || settings.slow_keys_ms > 0 ||
           settings.bounce_keys_ms > 0 || settings.order_wheel != OrderWheel::off ||
           settings.pointer_keys;
}

void turn_helpers_off(Dialog& dialog) noexcept {
    EngineSettings& on = dialog.chosen;
    on.sticky_modifiers = false;
    on.hold_to_confirm = false;
    on.slow_keys_ms = 0;
    on.bounce_keys_ms = 0;
    on.order_wheel = OrderWheel::off;
    on.pointer_keys = false;
    const uint64_t now = dialog.practice.now_ms;
    dialog.practice = PracticePad{};
    dialog.practice.now_ms = now;
}

} // namespace cards

DialogAction
dialog_practice_key(Dialog& dialog, const PracticeKey& key, bool down, uint64_t now_ms) {
    if (dialog.page != Page::accessibility || dialog.kind != DialogKind::engine)
        return DialogAction::none;
    PracticePad& pad = dialog.practice;
    const EngineSettings& on = dialog.chosen;
    pad.now_ms = now_ms;
    const uint8_t ctrl = practice_bit(PracticeModifier::ctrl);
    if (key.modifier != PracticeModifier::none) {
        const uint8_t bit = practice_bit(key.modifier);
        if (down) {
            if ((pad.held & bit) != 0)
                return DialogAction::none;
            pad.held = static_cast<uint8_t>(pad.held | bit);
            pad.alone = static_cast<uint8_t>(pad.alone | bit);
            return DialogAction::redraw;
        }
        pad.held = static_cast<uint8_t>(pad.held & ~bit);
        // Letting go of Ctrl ends Hold to confirm's chord before its end.
        if (pad.holding && bit == ctrl) {
            pad.holding = false;
            if (pad.outcome != PracticeOutcome::held)
                pad.outcome = PracticeOutcome::cancelled;
        }
        // A modifier let go with no other key since it went down was tapped.
        if ((pad.alone & bit) != 0) {
            pad.alone = static_cast<uint8_t>(pad.alone & ~bit);
            if (on.sticky_modifiers) {
                pad.outcome_key = key.label;
                if ((pad.locked & bit) != 0) {
                    pad.locked = static_cast<uint8_t>(pad.locked & ~bit);
                    pad.outcome = PracticeOutcome::unlocked;
                } else if ((pad.latched & bit) != 0) {
                    pad.latched = static_cast<uint8_t>(pad.latched & ~bit);
                    pad.locked = static_cast<uint8_t>(pad.locked | bit);
                    pad.outcome = PracticeOutcome::locked;
                } else {
                    pad.latched = static_cast<uint8_t>(pad.latched | bit);
                    pad.outcome = PracticeOutcome::latched;
                }
            }
        }
        return DialogAction::redraw;
    }
    if (key.label.empty())
        return DialogAction::none;
    if (down) {
        // One key at a time; a repeat of the key held changes nothing.
        if (!pad.key_down.empty())
            return DialogAction::none;
        pad.alone = 0;
        pad.key_down = key.label;
        pad.key_modifiers = static_cast<uint8_t>(pad.held | pad.latched | pad.locked);
        pad.key_down_ms = now_ms;
        pad.key_counted = false;
        pad.key_bounced = false;
        if (on.bounce_keys_ms > 0 && key.label == pad.last_released &&
            now_ms - pad.last_release_ms < on.bounce_keys_ms) {
            pad.key_bounced = true;
            pad.outcome = PracticeOutcome::bounced;
            pad.outcome_key = key.label;
            pad.outcome_ms = static_cast<uint32_t>(now_ms - pad.last_release_ms);
            return DialogAction::redraw;
        }
        if (on.hold_to_confirm && (pad.key_modifiers & ctrl) != 0 && key.label == cards::kHoldKey) {
            pad.holding = true;
            pad.hold_started_ms = now_ms;
            pad.outcome = PracticeOutcome::none;
            return DialogAction::redraw;
        }
        if (on.slow_keys_ms == 0)
            cards::count_key(pad);
        return DialogAction::redraw;
    }
    if (key.label != pad.key_down)
        return DialogAction::none;
    if (pad.holding) {
        pad.holding = false;
        if (pad.outcome != PracticeOutcome::held)
            pad.outcome = PracticeOutcome::cancelled;
    } else if (!pad.key_counted && !pad.key_bounced) {
        pad.outcome = PracticeOutcome::too_short;
        pad.outcome_key = pad.key_down;
        pad.outcome_ms = static_cast<uint32_t>(now_ms - pad.key_down_ms);
    }
    if (pad.key_counted) {
        pad.last_released = pad.key_down;
        pad.last_release_ms = now_ms;
    }
    pad.key_down.clear();
    return DialogAction::redraw;
}

DialogAction dialog_practice_tick(Dialog& dialog, uint64_t now_ms) {
    if (dialog.page != Page::accessibility || dialog.kind != DialogKind::engine)
        return DialogAction::none;
    PracticePad& pad = dialog.practice;
    const EngineSettings& on = dialog.chosen;
    pad.now_ms = now_ms;
    if (pad.holding) {
        if (now_ms - pad.hold_started_ms >= on.long_press_ms &&
            pad.outcome != PracticeOutcome::held) {
            pad.outcome = PracticeOutcome::held;
            pad.key_counted = true;
        }
        return DialogAction::redraw;
    }
    if (!pad.key_down.empty() && !pad.key_counted && !pad.key_bounced) {
        if (on.slow_keys_ms > 0 && now_ms - pad.key_down_ms >= on.slow_keys_ms)
            cards::count_key(pad);
        return DialogAction::redraw;
    }
    return DialogAction::none;
}

} // namespace oa::ui::engine_settings
