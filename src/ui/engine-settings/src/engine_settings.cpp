// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/engine_settings.hpp"

#include "stored_words.hpp"

#include "oa/sim/session.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace oa::ui::engine_settings {

namespace mod_profile = oa::data::mod_profile;
namespace pad_controls = oa::ui::pad_controls;

static_assert(default_unit_limit == oa::sim::session::kDefaultUnitLimit);
static_assert(lowest_stored_unit_limit == oa::sim::session::kMinUnitLimit);
static_assert(lowest_stored_unit_limit <= lowest_unit_limit);
static_assert(oa::sim::session::kMaxUnitLimit <= highest_unit_limit);
static_assert(highest_offered_unit_limit({}) == highest_unit_limit);
static_assert(raspberry_pi_frame_rate >= lowest_frame_rate);
static_assert(raspberry_pi_frame_rate <= highest_frame_rate);
static_assert((raspberry_pi_frame_rate - lowest_frame_rate) % frame_rate_step == 0);
static_assert(light_machine_frame_rate >= lowest_frame_rate);
static_assert(light_machine_frame_rate <= highest_frame_rate);
static_assert((light_machine_frame_rate - lowest_frame_rate) % frame_rate_step == 0);
static_assert((highest_text_size - lowest_text_size) % text_size_step == 0);
static_assert((default_text_size - lowest_text_size) % text_size_step == 0);
static_assert(
    lowest_text_size <= oa::present::game_font_text_size &&
    oa::present::game_font_text_size <= highest_text_size
);

namespace {

using oa::platform::preferences::Values;

/// Numbers past this size, either side of 0, read as it: every setting's
/// range lies far inside it.
constexpr int64_t number_bound = 1'000'000'000;

/// The section of totala.ini that holds the unit limit.
constexpr std::string_view installation_section = "Preferences";
/// The key of totala.ini's unit limit.
constexpr std::string_view installation_unit_limit_key = "UnitLimit";

/// A number read from the start of a text, and how much of the text it took.
struct LeadingNumber {
    int64_t value{};      ///< the number, within -number_bound..number_bound
    std::size_t length{}; ///< characters taken: the sign and the digits
};

/// Reads the whole decimal number at the start of a text: an optional sign,
/// then digits.
///
/// @param text the text
/// @return the number, its size held to number_bound; nothing without a digit
std::optional<LeadingNumber> leading_number(std::string_view text) noexcept {
    std::size_t at = 0;
    const bool negative = !text.empty() && text.front() == '-';
    if (!text.empty() && (text.front() == '-' || text.front() == '+'))
        at = 1;
    const std::size_t first_digit = at;
    int64_t magnitude = 0;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
        magnitude = std::min(magnitude * 10 + (text[at] - '0'), number_bound);
        ++at;
    }
    if (at == first_digit)
        return std::nullopt;
    return LeadingNumber{negative ? -magnitude : magnitude, at};
}

/// Returns the number a preferences key holds.
///
/// @param values the preferences
/// @param key the key
/// @return the number; nothing when the key is absent or its whole value is
///     not a decimal number
std::optional<int64_t> stored_number(const Values& values, std::string_view key) {
    const auto found = values.find(std::string{key});
    if (found == values.end())
        return std::nullopt;
    const auto number = leading_number(found->second);
    if (!number || number->length != found->second.size())
        return std::nullopt;
    return number->value;
}

/// Returns a stored number clamped into a setting's range.
///
/// @param number the stored number
/// @param lowest the setting's lowest value
/// @param highest the setting's highest value
/// @return the value
template <typename Value>
Value clamped(int64_t number, Value lowest, Value highest) noexcept {
    return static_cast<Value>(
        std::clamp(number, static_cast<int64_t>(lowest), static_cast<int64_t>(highest))
    );
}

/// Returns the anti-aliasing level a stored number reads as.
///
/// @param number the stored number
/// @return the highest level whose factor is not above `number`; off below 2
AntiAliasing anti_aliasing_from_number(int64_t number) noexcept {
    AntiAliasing level = AntiAliasing::off;
    for (const AntiAliasing candidate : anti_aliasing_levels)
        if (static_cast<int64_t>(candidate) <= number)
            level = candidate;
    return level;
}

/// Returns a text with the spaces, tabs and carriage returns at its ends removed.
///
/// @param text the text
/// @return the trimmed text, a view into `text`
std::string_view trimmed(std::string_view text) noexcept {
    constexpr std::string_view blanks = " \t\r";
    const auto first = text.find_first_not_of(blanks);
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

/// Tells whether two names are the same, ignoring the case of ASCII letters.
///
/// @param left a name
/// @param right a name
/// @return true when they match
bool same_name(std::string_view left, std::string_view right) noexcept {
    const auto lower = [](char letter) {
        return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
    };
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [&](char a, char b) {
               return lower(a) == lower(b);
           });
}

/// Writes or erases one setting's key, as write_settings describes.
///
/// @param[in,out] values the preferences
/// @param key the setting's key
/// @param text the chosen value as stored
/// @param changed the chosen value differs from the one the dialog opened with
/// @param at_default the chosen value is the default
/// @param restored Restore defaults was pressed
void store(
    Values& values,
    std::string_view key,
    const std::string& text,
    bool changed,
    bool at_default,
    bool restored
) {
    if (restored && at_default)
        values.erase(std::string{key});
    else if (changed)
        values[std::string{key}] = text;
}

/// The stored text of the desktop's screen size.
constexpr std::string_view desktop_text = "desktop";

/// Returns a switch's stored text.
///
/// @param on the switch's state
/// @return "1" or "0"
std::string switch_text(bool on) {
    return on ? "1" : "0";
}

/// Tells whether a file-type switch is on. Only the word off turns it off;
/// a missing key, "on", and any other text leave it on.
///
/// @param values the preferences
/// @param key the switch's key
/// @return false when the key's text is off
bool file_type_on(const oa::platform::preferences::Values& values, std::string_view key) {
    const auto found = values.find(std::string{key});
    return found == values.end() || found->second != "off";
}

/// The stored text of a file-type switch.
///
/// @param on the switch's state
/// @return "on" or "off"
std::string file_type_text(bool on) {
    return on ? "on" : "off";
}

/// Returns the level of hardware acceleration a stored value reads as.
///
/// @param text the stored value
/// @return the level its word names; for a whole number, as the switch the
///     setting was before, Full above 0 and Off otherwise; nothing for any
///     other text
std::optional<HardwareAcceleration> stored_acceleration(std::string_view text) {
    if (const auto level = hardware_acceleration_from_text(text))
        return level;
    const auto number = leading_number(text);
    if (!number || number->length != text.size())
        return std::nullopt;
    return number->value > 0 ? HardwareAcceleration::full : HardwareAcceleration::off;
}

/// A switch of the Language, the Touch, the Controller or the Game files
/// section: its key and the member of EngineSettings it is.
struct TextSwitch {
    std::string_view key;            ///< its preferences key
    bool EngineSettings::* member{}; ///< its value
};

/// The Language switches, read and written alike.
constexpr std::array<TextSwitch, 5> text_switches{{
    {key::modern_fonts, &EngineSettings::modern_fonts},
    {key::text_outline, &EngineSettings::text_outline},
    {key::text_shadow, &EngineSettings::text_shadow},
    {key::text_background, &EngineSettings::text_background},
    {key::unicode_chat, &EngineSettings::unicode_chat},
}};

/// The Touch and Game files sections' switches, read and written as the
/// Language ones.
constexpr std::array<TextSwitch, 3> touch_switches{{
    {key::touch_haptics, &EngineSettings::touch_haptics},
    {key::touch_left_handed, &EngineSettings::touch_left_handed},
    {key::game_files_backed_up, &EngineSettings::game_files_backed_up},
}};

/// Returns the key a profile's overrides are kept under.
///
/// @param profile_id the profile's id
/// @return key::hack_overrides followed by the id
std::string overrides_key(std::string_view profile_id) {
    return std::string{key::hack_overrides} + std::string{profile_id};
}

/// The Controller section's switches, read and written as the Touch ones.
constexpr std::array<TextSwitch, 3> pad_switches{{
    {key::pad_glide, &EngineSettings::pad_glide},
    {key::pad_magnetism, &EngineSettings::pad_magnetism},
    {key::pad_left_handed, &EngineSettings::pad_left_handed},
}};

/// Returns the choice a stored word names.
///
/// @param words the setting's choices and words
/// @param text the stored word
/// @return the choice; nothing for any other text
template <typename Choice, std::size_t Count>
std::optional<Choice>
choice_of_word(const std::array<ChoiceWord<Choice>, Count>& words, std::string_view text) noexcept {
    for (const ChoiceWord<Choice>& entry : words)
        if (entry.word == text)
            return entry.choice;
    return std::nullopt;
}

/// Reads a stored word into a setting, leaving it as it is when the key is
/// absent or holds any other text.
///
/// @param values the preferences
/// @param key the setting's key
/// @param words the setting's choices and words
/// @param[in,out] setting the setting
template <typename Choice, std::size_t Count>
void read_word(
    const Values& values,
    std::string_view key,
    const std::array<ChoiceWord<Choice>, Count>& words,
    Choice& setting
) {
    if (const auto found = values.find(std::string{key}); found != values.end())
        setting = choice_of_word(words, found->second).value_or(setting);
}

/// Writes or erases a setting kept as a word, as write_settings describes.
///
/// @param[in,out] values the preferences
/// @param key the setting's key
/// @param words the setting's choices and words
/// @param opened the setting when the dialog opened
/// @param chosen the setting the player keeps
/// @param at_default the default
/// @param restored Restore defaults was pressed
template <typename Choice, std::size_t Count>
void store_word(
    Values& values,
    std::string_view key,
    const std::array<ChoiceWord<Choice>, Count>& words,
    Choice opened,
    Choice chosen,
    Choice at_default,
    bool restored
) {
    store(
        values,
        key,
        std::string{word_of(words, chosen)},
        chosen != opened,
        chosen == at_default,
        restored
    );
}

/// Returns a stored number held to a setting's range and put on its nearest stop.
///
/// @param number the stored number
/// @param lowest the setting's lowest value, its first stop
/// @param highest the setting's highest value, a stop
/// @param step the stops' spacing
/// @return lowest to highest, a whole number of steps above lowest; half a step rounds up
constexpr uint32_t
on_stop(int64_t number, uint32_t lowest, uint32_t highest, uint32_t step) noexcept {
    const int64_t held = std::clamp(number, int64_t{lowest}, int64_t{highest});
    const int64_t steps = (held - lowest + step / 2) / step;
    return static_cast<uint32_t>(std::min(int64_t{lowest} + steps * step, int64_t{highest}));
}

static_assert(
    (pad_controls::highest_pointer_speed - pad_controls::lowest_pointer_speed) %
                pad_controls::pointer_speed_step ==
            0 &&
        (pad_controls::default_pointer_speed - pad_controls::lowest_pointer_speed) %
                pad_controls::pointer_speed_step ==
            0,
    "Pointer speed's stops run from its lowest to its highest, its default among them"
);
static_assert(
    (pad_controls::highest_gyro_speed - pad_controls::lowest_gyro_speed) %
                pad_controls::gyro_speed_step ==
            0 &&
        (pad_controls::default_gyro_speed - pad_controls::lowest_gyro_speed) %
                pad_controls::gyro_speed_step ==
            0,
    "Gyro speed's stops run from its lowest to its highest, its default among them"
);
static_assert(on_stop(62, lowest_frame_rate, highest_frame_rate, frame_rate_step) == 60);
static_assert(on_stop(63, lowest_frame_rate, highest_frame_rate, frame_rate_step) == 65);
static_assert(on_stop(240, lowest_frame_rate, highest_frame_rate, frame_rate_step) == 120);

/// Check for content updates' words, in the order the Downloads page lists them.
constexpr std::array<ChoiceWord<ContentCheck>, 3> content_check_words{{
    {ContentCheck::automatically, "automatically"},
    {ContentCheck::when_library_opens, "library"},
    {ContentCheck::never, "never"},
}};

} // namespace

EngineSettings default_settings(const Inputs& inputs) {
    EngineSettings settings{};
    settings.escape_opens_menu = inputs.macos && inputs.players_own_profile;
    // Full for the player's own file on every machine: whether the graphics
    // card is used is decided apart, so the default never moves with it.
    settings.hardware_acceleration =
        inputs.players_own_profile ? HardwareAcceleration::full : HardwareAcceleration::off;
    // A named file draws game text in the game's own fonts.
    settings.modern_fonts = inputs.players_own_profile;
    // A named file plays in English, as the game does without a language on
    // its command line; the player's own follows the operating system.
    settings.language = inputs.players_own_profile
                            ? std::string(oa::data::languages::system_choice)
                            : std::string(oa::data::languages::english().tag);
    if (inputs.players_own_profile)
        settings.unit_limit =
            installation_unit_limit(inputs.installation_ini, inputs.units_per_player)
                .value_or(inputs.units_per_player.default_limit);
    else
        settings.unit_limit = inputs.units_per_player.default_limit;
    // Where the platform opens every window at native density, the setting
    // says so.
    settings.native_density = inputs.native_density_windows;
    if (inputs.players_own_profile && inputs.raspberry_pi) {
        settings.max_frame_rate = raspberry_pi_frame_rate;
        settings.anti_aliasing = AntiAliasing::off;
    }
    if (inputs.players_own_profile && inputs.light_machine) {
        settings.max_frame_rate = light_machine_frame_rate;
        settings.anti_aliasing = AntiAliasing::off;
        const bool small_desktop = inputs.desktop != desktop_screen_size &&
                                   (inputs.desktop.width < light_machine_screen_size.width ||
                                    inputs.desktop.height < light_machine_screen_size.height);
        settings.screen_size =
            small_desktop ? small_desktop_screen_size : light_machine_screen_size;
    }
    // A Steam Deck starts at its screen's own rate, with the touch controls
    // at the size that suits its small, dense screen.
    if (inputs.players_own_profile && inputs.steam_deck_panel_hz != 0) {
        settings.max_frame_rate = on_stop(
            inputs.steam_deck_panel_hz, lowest_frame_rate, highest_frame_rate, frame_rate_step
        );
        settings.touch_control_size = steam_deck_control_size;
    }
    return settings;
}

std::string screen_size_text(ScreenSize size) {
    if (size == desktop_screen_size)
        return std::string{desktop_text};
    return std::to_string(size.width) + "x" + std::to_string(size.height);
}

std::optional<ScreenSize> screen_size_from_text(std::string_view text) {
    if (text == desktop_text)
        return desktop_screen_size;
    const auto cross = text.find('x');
    if (cross == std::string_view::npos)
        return std::nullopt;
    // A side: digits alone, no leading zero, at least `least` and at most
    // longest_screen_side.
    const auto side = [](std::string_view digits, uint16_t least) -> std::optional<uint16_t> {
        if (digits.empty() || digits.size() > 5 || digits.front() == '0')
            return std::nullopt;
        uint32_t value = 0;
        for (const char digit : digits) {
            if (digit < '0' || digit > '9')
                return std::nullopt;
            value = value * 10 + static_cast<uint32_t>(digit - '0');
        }
        if (value < least || value > longest_screen_side)
            return std::nullopt;
        return static_cast<uint16_t>(value);
    };
    const auto width = side(text.substr(0, cross), smallest_screen_size.width);
    const auto height = side(text.substr(cross + 1), smallest_screen_size.height);
    if (!width || !height)
        return std::nullopt;
    return ScreenSize{*width, *height};
}

std::string edge_stickiness_text(uint32_t tenths) {
    return std::to_string(tenths / edge_stickiness_per_level) + "." +
           std::to_string(tenths % edge_stickiness_per_level);
}

std::optional<uint32_t> edge_stickiness_from_text(std::string_view text) noexcept {
    std::size_t at = 0;
    const bool negative = !text.empty() && text.front() == '-';
    if (!text.empty() && (text.front() == '-' || text.front() == '+'))
        at = 1;
    // The whole part, then at most one point and the fraction's digits.
    // Hundredths and beyond only round the tenths.
    int64_t whole = 0;
    std::size_t digits = 0;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
        whole = std::min(whole * 10 + (text[at] - '0'), number_bound);
        ++at;
        ++digits;
    }
    int64_t tenths = 0;
    int64_t hundredths = 0;
    if (at < text.size() && text[at] == '.') {
        ++at;
        std::size_t place = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            if (place == 0)
                tenths = text[at] - '0';
            else if (place == 1)
                hundredths = text[at] - '0';
            ++place;
            ++at;
            ++digits;
        }
    }
    if (digits == 0 || at != text.size())
        return std::nullopt;
    int64_t level = whole * edge_stickiness_per_level + tenths + (hundredths >= 5 ? 1 : 0);
    if (negative)
        level = -level;
    return static_cast<uint32_t>(
        std::clamp(level, int64_t{lowest_edge_stickiness}, int64_t{highest_edge_stickiness})
    );
}

std::string_view hardware_acceleration_text(HardwareAcceleration level) noexcept {
    switch (level) {
    case HardwareAcceleration::off:
        return "off";
    case HardwareAcceleration::basic:
        return "basic";
    case HardwareAcceleration::full:
        return "full";
    }
    return {};
}

std::optional<HardwareAcceleration>
hardware_acceleration_from_text(std::string_view text) noexcept {
    for (const HardwareAcceleration level : hardware_acceleration_levels)
        if (text == hardware_acceleration_text(level))
            return level;
    return std::nullopt;
}

std::string_view menu_scaling_text(MenuScaling scaling) noexcept {
    switch (scaling) {
    case MenuScaling::sharp:
        return "sharp";
    case MenuScaling::whole_steps:
        return "whole-steps";
    case MenuScaling::unfiltered:
        return "unfiltered";
    }
    return {};
}

std::optional<MenuScaling> menu_scaling_from_text(std::string_view text) noexcept {
    for (const MenuScaling scaling : menu_scaling_choices)
        if (text == menu_scaling_text(scaling))
            return scaling;
    return std::nullopt;
}

std::string_view interface_size_text(InterfaceSize size) noexcept {
    switch (size) {
    case InterfaceSize::automatic:
        return "automatic";
    case InterfaceSize::size_100:
        return "100";
    case InterfaceSize::size_150:
        return "150";
    case InterfaceSize::size_200:
        return "200";
    case InterfaceSize::size_300:
        return "300";
    case InterfaceSize::size_400:
        return "400";
    }
    return {};
}

std::optional<InterfaceSize> interface_size_from_text(std::string_view text) noexcept {
    for (const InterfaceSize size : interface_size_choices)
        if (text == interface_size_text(size))
            return size;
    return std::nullopt;
}

std::string_view touch_drag_text(TouchDrag drag) noexcept {
    switch (drag) {
    case TouchDrag::automatic:
        return "automatic";
    case TouchDrag::box:
        return "box";
    case TouchDrag::scroll:
        return "scroll";
    }
    return {};
}

std::optional<TouchDrag> touch_drag_from_text(std::string_view text) noexcept {
    for (const TouchDrag drag : touch_drag_choices)
        if (text == touch_drag_text(drag))
            return drag;
    return std::nullopt;
}

std::string_view touch_latches_text(TouchLatches latches) noexcept {
    switch (latches) {
    case TouchLatches::stay_on:
        return "stay-on";
    case TouchLatches::one_action:
        return "one-action";
    }
    return {};
}

std::optional<TouchLatches> touch_latches_from_text(std::string_view text) noexcept {
    for (const TouchLatches latches : touch_latches_choices)
        if (text == touch_latches_text(latches))
            return latches;
    return std::nullopt;
}

EngineSettings read_settings(
    const oa::platform::preferences::Values& values, const Inputs& inputs, bool switch_alt
) {
    EngineSettings settings = default_settings(inputs);
    settings.switch_alt = switch_alt;
    if (const auto number = stored_number(values, key::path_search_nodes))
        settings.path_search_nodes =
            clamped(*number, base_path_search_nodes, highest_path_search_nodes);
    if (const auto number = stored_number(values, key::wheel_zoom))
        settings.wheel_zoom = *number > 0;
    read_word(values, key::max_zoom_out, zoom_out_words, settings.max_zoom_out);
    read_word(values, key::max_zoom_in, zoom_in_words, settings.max_zoom_in);
    read_word(
        values, key::view_past_map_edge, view_past_map_edge_words, settings.view_past_map_edge
    );
    read_word(values, key::window_edge, window_edge_words, settings.window_edge);
    if (const auto found = values.find(std::string{key::edge_stickiness}); found != values.end())
        settings.edge_stickiness =
            edge_stickiness_from_text(found->second).value_or(settings.edge_stickiness);
    if (const auto number = stored_number(values, key::middle_click_pan))
        settings.middle_click_pan = *number > 0;
    if (const auto number = stored_number(values, key::escape_opens_menu))
        settings.escape_opens_menu = *number > 0;
    if (const auto number = stored_number(values, key::unit_limit))
        settings.unit_limit = clamped(
            *number,
            inputs.units_per_player.minimum,
            highest_offered_unit_limit(inputs.units_per_player)
        );
    if (const auto number = stored_number(values, key::max_frame_rate))
        settings.max_frame_rate = clamped(*number, lowest_frame_rate, highest_frame_rate);
    if (const auto number = stored_number(values, key::anti_aliasing))
        settings.anti_aliasing = anti_aliasing_from_number(*number);
    if (const auto number = stored_number(values, key::frame_stats))
        settings.frame_stats = *number > 0;
    if (const auto found = values.find(std::string{key::screen_size}); found != values.end())
        settings.screen_size = screen_size_from_text(found->second).value_or(settings.screen_size);
    if (const auto found = values.find(std::string{key::hardware_acceleration});
        found != values.end())
        settings.hardware_acceleration =
            stored_acceleration(found->second).value_or(settings.hardware_acceleration);
    if (const auto number = stored_number(values, key::vertical_sync))
        settings.vertical_sync = *number > 0;
    if (const auto number = stored_number(values, key::allow_full_3d))
        settings.allow_full_3d = *number > 0;
    if (const auto number = stored_number(values, key::unlocked_camera))
        settings.unlocked_camera = *number > 0;
    if (const auto number = stored_number(values, key::smooth_motion))
        settings.smooth_motion = *number > 0;
    if (const auto number = stored_number(values, key::weapon_lighting_intensity))
        settings.weapon_lighting = static_cast<uint32_t>((clamped(*number, 0, 200) + 5) / 10 * 10);
    else if (const auto number = stored_number(values, key::weapon_lighting))
        settings.weapon_lighting = *number > 0 ? 100 : 0;
    if (const auto number = stored_number(values, key::nanolathe_lighting_intensity))
        settings.nanolathe_lighting =
            static_cast<uint32_t>((clamped(*number, 0, 200) + 5) / 10 * 10);
    else if (const auto number = stored_number(values, key::nanolathe_lighting))
        settings.nanolathe_lighting = *number > 0 ? 100 : 0;
    if (const auto number = stored_number(values, key::wreck_glow_intensity))
        settings.wreck_glow = static_cast<uint32_t>((clamped(*number, 0, 200) + 5) / 10 * 10);
    else if (const auto number = stored_number(values, key::wreck_glow))
        settings.wreck_glow = *number > 0 ? 100 : 0;
    if (const auto number = stored_number(values, key::heat_distortion_intensity))
        settings.heat_distortion = static_cast<uint32_t>((clamped(*number, 0, 200) + 5) / 10 * 10);
    else if (const auto number = stored_number(values, key::heat_distortion))
        settings.heat_distortion = *number > 0 ? 100 : 0;

    if (const auto found = values.find(std::string{key::menu_scaling}); found != values.end())
        settings.menu_scaling =
            menu_scaling_from_text(found->second).value_or(settings.menu_scaling);
    if (const auto number = stored_number(values, key::native_density))
        settings.native_density = inputs.native_density_windows || *number > 0;
    read_word(values, key::explosion_flash, explosion_flash_words, settings.explosion_flash);
    read_word(values, key::zoomed_out_units, zoomed_out_units_words, settings.zoomed_out_units);
    read_word(values, key::zoomed_out_after, zoomed_out_after_words, settings.zoomed_out_after);
    read_word(values, key::window_frame, window_frame_words, settings.window_frame);
    if (const auto number = stored_number(values, key::hud_scaling))
        settings.hud_scaling = *number > 0;
    if (const auto found = values.find(std::string{key::interface_size}); found != values.end())
        settings.interface_size =
            interface_size_from_text(found->second).value_or(settings.interface_size);
    for (const TextSwitch& entry : text_switches)
        if (const auto number = stored_number(values, entry.key))
            settings.*entry.member = *number > 0;
    // 150% is drawn in the modern fonts: read with them off, it is 100%.
    settings.interface_size =
        interface_size_with_fonts(settings.interface_size, settings.modern_fonts);
    if (const auto number = stored_number(values, key::text_size))
        settings.text_size = clamped(*number, lowest_text_size, highest_text_size);
    settings.language = stored_language(values, inputs.players_own_profile);
    settings.picked_mod_folder = remembered_picked_folder(values);
    if (const auto found = values.find(std::string{key::mod_directory}); found != values.end()) {
        settings.mod_folder = found->second;
        // A folder the game folder does not offer was picked: it is kept as
        // the picked one, which the Library lists.
        if (!settings.mod_folder.empty() &&
            std::find(inputs.mod_folders.begin(), inputs.mod_folders.end(), settings.mod_folder) ==
                inputs.mod_folders.end())
            settings.picked_mod_folder = settings.mod_folder;
    }
    if (const auto number = stored_number(values, key::developer_mode))
        settings.developer_mode = *number > 0;
    if (const auto found = values.find(std::string{key::touch_drag}); found != values.end())
        settings.touch_drag = touch_drag_from_text(found->second).value_or(settings.touch_drag);
    if (const auto number = stored_number(values, key::touch_hold_delay))
        settings.touch_hold_ms = snapped_touch_hold_ms(*number);
    if (const auto found = values.find(std::string{key::touch_latches}); found != values.end())
        settings.touch_latches =
            touch_latches_from_text(found->second).value_or(settings.touch_latches);
    for (const TextSwitch& entry : touch_switches)
        if (const auto number = stored_number(values, entry.key))
            settings.*entry.member = *number > 0;
    read_word(values, key::touch_control_size, control_size_words, settings.touch_control_size);
    read_word(values, key::pad_scheme, scheme_words, settings.pad_scheme);
    read_word(values, key::pad_right_trackpad, right_trackpad_words, settings.pad_right_trackpad);
    if (const auto number = stored_number(values, key::pad_pointer_speed))
        settings.pad_pointer_speed = on_stop(
            *number,
            pad_controls::lowest_pointer_speed,
            pad_controls::highest_pointer_speed,
            pad_controls::pointer_speed_step
        );
    read_word(values, key::pad_acceleration, acceleration_words, settings.pad_acceleration);
    read_word(values, key::pad_right_stick, right_stick_words, settings.pad_right_stick);
    read_word(values, key::pad_gyro, gyro_words, settings.pad_gyro);
    if (const auto number = stored_number(values, key::pad_gyro_speed))
        settings.pad_gyro_speed = on_stop(
            *number,
            pad_controls::lowest_gyro_speed,
            pad_controls::highest_gyro_speed,
            pad_controls::gyro_speed_step
        );
    read_word(values, key::pad_haptics, haptics_words, settings.pad_haptics);
    read_word(values, key::pad_prompts, prompts_words, settings.pad_prompts);
    for (const TextSwitch& entry : pad_switches)
        if (const auto number = stored_number(values, entry.key))
            settings.*entry.member = *number > 0;
    if (!inputs.profile_id.empty())
        if (const auto found = values.find(overrides_key(inputs.profile_id)); found != values.end())
            settings.hack_overrides = mod_profile::read_overrides(found->second);
    // Unknown text leaves the default read_settings started from.
    read_word(values, key::content_check, content_check_words, settings.content_check);
    settings.file_type_oamod = file_type_on(values, key::file_type_oamod);
    settings.file_type_oamap = file_type_on(values, key::file_type_oamap);
    settings.file_type_oalang = file_type_on(values, key::file_type_oalang);
    settings.file_type_oareg = file_type_on(values, key::file_type_oareg);
    settings.file_type_oamut = file_type_on(values, key::file_type_oamut);
    settings.file_type_oalaunch = file_type_on(values, key::file_type_oalaunch);
    return settings;
}

void write_settings(
    oa::platform::preferences::Values& values,
    const EngineSettings& opened,
    const EngineSettings& chosen,
    const EngineSettings& defaults,
    bool restored,
    std::string_view profile_id
) {
    // No mod, and no picked folder, is stored as no key, never as an empty
    // path.
    const auto store_folder = [&values](std::string_view key, const std::string& folder) {
        if (folder.empty())
            values.erase(std::string{key});
        else
            values[std::string{key}] = folder;
    };
    if ((restored && chosen.mod_folder == defaults.mod_folder) ||
        chosen.mod_folder != opened.mod_folder)
        store_folder(key::mod_directory, chosen.mod_folder);
    if (chosen.picked_mod_folder != opened.picked_mod_folder)
        store_folder(key::picked_mod_directory, chosen.picked_mod_folder);
    // A picked folder is forgotten once it is no longer the mod stored.
    if (remembered_picked_folder(values).empty())
        values.erase(std::string{key::picked_mod_directory});
    store(
        values,
        key::path_search_nodes,
        std::to_string(chosen.path_search_nodes),
        chosen.path_search_nodes != opened.path_search_nodes,
        chosen.path_search_nodes == defaults.path_search_nodes,
        restored
    );
    store(
        values,
        key::wheel_zoom,
        switch_text(chosen.wheel_zoom),
        chosen.wheel_zoom != opened.wheel_zoom,
        chosen.wheel_zoom == defaults.wheel_zoom,
        restored
    );
    store_word(
        values,
        key::max_zoom_out,
        zoom_out_words,
        opened.max_zoom_out,
        chosen.max_zoom_out,
        defaults.max_zoom_out,
        restored
    );
    store_word(
        values,
        key::max_zoom_in,
        zoom_in_words,
        opened.max_zoom_in,
        chosen.max_zoom_in,
        defaults.max_zoom_in,
        restored
    );
    store_word(
        values,
        key::view_past_map_edge,
        view_past_map_edge_words,
        opened.view_past_map_edge,
        chosen.view_past_map_edge,
        defaults.view_past_map_edge,
        restored
    );
    store(
        values,
        key::middle_click_pan,
        switch_text(chosen.middle_click_pan),
        chosen.middle_click_pan != opened.middle_click_pan,
        chosen.middle_click_pan == defaults.middle_click_pan,
        restored
    );
    store(
        values,
        key::escape_opens_menu,
        switch_text(chosen.escape_opens_menu),
        chosen.escape_opens_menu != opened.escape_opens_menu,
        chosen.escape_opens_menu == defaults.escape_opens_menu,
        restored
    );
    store(
        values,
        key::unit_limit,
        std::to_string(chosen.unit_limit),
        chosen.unit_limit != opened.unit_limit,
        chosen.unit_limit == defaults.unit_limit,
        restored
    );
    store(
        values,
        key::max_frame_rate,
        std::to_string(chosen.max_frame_rate),
        chosen.max_frame_rate != opened.max_frame_rate,
        chosen.max_frame_rate == defaults.max_frame_rate,
        restored
    );
    store(
        values,
        key::anti_aliasing,
        std::to_string(static_cast<int>(chosen.anti_aliasing)),
        chosen.anti_aliasing != opened.anti_aliasing,
        chosen.anti_aliasing == defaults.anti_aliasing,
        restored
    );
    store(
        values,
        key::frame_stats,
        switch_text(chosen.frame_stats),
        chosen.frame_stats != opened.frame_stats,
        chosen.frame_stats == defaults.frame_stats,
        restored
    );
    store(
        values,
        key::screen_size,
        screen_size_text(chosen.screen_size),
        chosen.screen_size != opened.screen_size,
        chosen.screen_size == defaults.screen_size,
        restored
    );
    store(
        values,
        key::hardware_acceleration,
        std::string{hardware_acceleration_text(chosen.hardware_acceleration)},
        chosen.hardware_acceleration != opened.hardware_acceleration,
        chosen.hardware_acceleration == defaults.hardware_acceleration,
        restored
    );
    store(
        values,
        key::vertical_sync,
        switch_text(chosen.vertical_sync),
        chosen.vertical_sync != opened.vertical_sync,
        chosen.vertical_sync == defaults.vertical_sync,
        restored
    );
    store(
        values,
        key::allow_full_3d,
        switch_text(chosen.allow_full_3d),
        chosen.allow_full_3d != opened.allow_full_3d,
        chosen.allow_full_3d == defaults.allow_full_3d,
        restored
    );
    store(
        values,
        key::unlocked_camera,
        switch_text(chosen.unlocked_camera),
        chosen.unlocked_camera != opened.unlocked_camera,
        chosen.unlocked_camera == defaults.unlocked_camera,
        restored
    );
    store(
        values,
        key::smooth_motion,
        switch_text(chosen.smooth_motion),
        chosen.smooth_motion != opened.smooth_motion,
        chosen.smooth_motion == defaults.smooth_motion,
        restored
    );
    if (restored || chosen.weapon_lighting != opened.weapon_lighting)
        values.erase(std::string{key::weapon_lighting});
    store(
        values,
        key::weapon_lighting_intensity,
        std::to_string(chosen.weapon_lighting),
        chosen.weapon_lighting != opened.weapon_lighting,
        chosen.weapon_lighting == defaults.weapon_lighting,
        restored
    );
    if (restored || chosen.nanolathe_lighting != opened.nanolathe_lighting)
        values.erase(std::string{key::nanolathe_lighting});
    store(
        values,
        key::nanolathe_lighting_intensity,
        std::to_string(chosen.nanolathe_lighting),
        chosen.nanolathe_lighting != opened.nanolathe_lighting,
        chosen.nanolathe_lighting == defaults.nanolathe_lighting,
        restored
    );
    if (restored || chosen.wreck_glow != opened.wreck_glow)
        values.erase(std::string{key::wreck_glow});
    store(
        values,
        key::wreck_glow_intensity,
        std::to_string(chosen.wreck_glow),
        chosen.wreck_glow != opened.wreck_glow,
        chosen.wreck_glow == defaults.wreck_glow,
        restored
    );
    if (restored || chosen.heat_distortion != opened.heat_distortion)
        values.erase(std::string{key::heat_distortion});
    store(
        values,
        key::heat_distortion_intensity,
        std::to_string(chosen.heat_distortion),
        chosen.heat_distortion != opened.heat_distortion,
        chosen.heat_distortion == defaults.heat_distortion,
        restored
    );
    store(
        values,
        key::menu_scaling,
        std::string{menu_scaling_text(chosen.menu_scaling)},
        chosen.menu_scaling != opened.menu_scaling,
        chosen.menu_scaling == defaults.menu_scaling,
        restored
    );
    store(
        values,
        key::native_density,
        switch_text(chosen.native_density),
        chosen.native_density != opened.native_density,
        chosen.native_density == defaults.native_density,
        restored
    );
    store_word(
        values,
        key::explosion_flash,
        explosion_flash_words,
        opened.explosion_flash,
        chosen.explosion_flash,
        defaults.explosion_flash,
        restored
    );
    store_word(
        values,
        key::zoomed_out_units,
        zoomed_out_units_words,
        opened.zoomed_out_units,
        chosen.zoomed_out_units,
        defaults.zoomed_out_units,
        restored
    );
    store_word(
        values,
        key::zoomed_out_after,
        zoomed_out_after_words,
        opened.zoomed_out_after,
        chosen.zoomed_out_after,
        defaults.zoomed_out_after,
        restored
    );
    store_word(
        values,
        key::window_edge,
        window_edge_words,
        opened.window_edge,
        chosen.window_edge,
        defaults.window_edge,
        restored
    );
    store(
        values,
        key::edge_stickiness,
        edge_stickiness_text(chosen.edge_stickiness),
        chosen.edge_stickiness != opened.edge_stickiness,
        chosen.edge_stickiness == defaults.edge_stickiness,
        restored
    );
    store_word(
        values,
        key::window_frame,
        window_frame_words,
        opened.window_frame,
        chosen.window_frame,
        defaults.window_frame,
        restored
    );
    store(
        values,
        key::hud_scaling,
        switch_text(chosen.hud_scaling),
        chosen.hud_scaling != opened.hud_scaling,
        chosen.hud_scaling == defaults.hud_scaling,
        restored
    );
    store(
        values,
        key::interface_size,
        std::string{interface_size_text(chosen.interface_size)},
        chosen.interface_size != opened.interface_size,
        chosen.interface_size == defaults.interface_size,
        restored
    );
    for (const TextSwitch& entry : text_switches)
        store(
            values,
            entry.key,
            switch_text(chosen.*entry.member),
            chosen.*entry.member != opened.*entry.member,
            chosen.*entry.member == defaults.*entry.member,
            restored
        );
    store(
        values,
        key::text_size,
        std::to_string(chosen.text_size),
        chosen.text_size != opened.text_size,
        chosen.text_size == defaults.text_size,
        restored
    );
    store(
        values,
        key::language,
        chosen.language,
        chosen.language != opened.language,
        chosen.language == defaults.language,
        restored
    );
    store(
        values,
        key::developer_mode,
        switch_text(chosen.developer_mode),
        chosen.developer_mode != opened.developer_mode,
        chosen.developer_mode == defaults.developer_mode,
        restored
    );
    store(
        values,
        key::touch_drag,
        std::string{touch_drag_text(chosen.touch_drag)},
        chosen.touch_drag != opened.touch_drag,
        chosen.touch_drag == defaults.touch_drag,
        restored
    );
    store(
        values,
        key::touch_hold_delay,
        std::to_string(chosen.touch_hold_ms),
        chosen.touch_hold_ms != opened.touch_hold_ms,
        chosen.touch_hold_ms == defaults.touch_hold_ms,
        restored
    );
    store(
        values,
        key::touch_latches,
        std::string{touch_latches_text(chosen.touch_latches)},
        chosen.touch_latches != opened.touch_latches,
        chosen.touch_latches == defaults.touch_latches,
        restored
    );
    for (const TextSwitch& entry : touch_switches)
        store(
            values,
            entry.key,
            switch_text(chosen.*entry.member),
            chosen.*entry.member != opened.*entry.member,
            chosen.*entry.member == defaults.*entry.member,
            restored
        );
    store_word(
        values,
        key::touch_control_size,
        control_size_words,
        opened.touch_control_size,
        chosen.touch_control_size,
        defaults.touch_control_size,
        restored
    );
    store_word(
        values,
        key::pad_scheme,
        scheme_words,
        opened.pad_scheme,
        chosen.pad_scheme,
        defaults.pad_scheme,
        restored
    );
    store_word(
        values,
        key::pad_right_trackpad,
        right_trackpad_words,
        opened.pad_right_trackpad,
        chosen.pad_right_trackpad,
        defaults.pad_right_trackpad,
        restored
    );
    store(
        values,
        key::pad_pointer_speed,
        std::to_string(chosen.pad_pointer_speed),
        chosen.pad_pointer_speed != opened.pad_pointer_speed,
        chosen.pad_pointer_speed == defaults.pad_pointer_speed,
        restored
    );
    store_word(
        values,
        key::pad_acceleration,
        acceleration_words,
        opened.pad_acceleration,
        chosen.pad_acceleration,
        defaults.pad_acceleration,
        restored
    );
    store_word(
        values,
        key::pad_right_stick,
        right_stick_words,
        opened.pad_right_stick,
        chosen.pad_right_stick,
        defaults.pad_right_stick,
        restored
    );
    store_word(
        values,
        key::pad_gyro,
        gyro_words,
        opened.pad_gyro,
        chosen.pad_gyro,
        defaults.pad_gyro,
        restored
    );
    store(
        values,
        key::pad_gyro_speed,
        std::to_string(chosen.pad_gyro_speed),
        chosen.pad_gyro_speed != opened.pad_gyro_speed,
        chosen.pad_gyro_speed == defaults.pad_gyro_speed,
        restored
    );
    store_word(
        values,
        key::pad_haptics,
        haptics_words,
        opened.pad_haptics,
        chosen.pad_haptics,
        defaults.pad_haptics,
        restored
    );
    store_word(
        values,
        key::pad_prompts,
        prompts_words,
        opened.pad_prompts,
        chosen.pad_prompts,
        defaults.pad_prompts,
        restored
    );
    for (const TextSwitch& entry : pad_switches)
        store(
            values,
            entry.key,
            switch_text(chosen.*entry.member),
            chosen.*entry.member != opened.*entry.member,
            chosen.*entry.member == defaults.*entry.member,
            restored
        );
    store_word(
        values,
        key::content_check,
        content_check_words,
        opened.content_check,
        chosen.content_check,
        defaults.content_check,
        restored
    );
    const auto store_file_type = [&](std::string_view key, bool EngineSettings::* member) {
        store(
            values,
            key,
            file_type_text(chosen.*member),
            chosen.*member != opened.*member,
            chosen.*member == defaults.*member,
            restored
        );
    };
    store_file_type(key::file_type_oamod, &EngineSettings::file_type_oamod);
    store_file_type(key::file_type_oamap, &EngineSettings::file_type_oamap);
    store_file_type(key::file_type_oalang, &EngineSettings::file_type_oalang);
    store_file_type(key::file_type_oareg, &EngineSettings::file_type_oareg);
    store_file_type(key::file_type_oamut, &EngineSettings::file_type_oamut);
    store_file_type(key::file_type_oalaunch, &EngineSettings::file_type_oalaunch);
    // Restore defaults keeps the overrides: Restore profile values clears them.
    if (!profile_id.empty() && chosen.hack_overrides != opened.hack_overrides) {
        if (chosen.hack_overrides.empty())
            values.erase(overrides_key(profile_id));
        else
            values[overrides_key(profile_id)] = mod_profile::overrides_text(chosen.hack_overrides);
    }
}

std::string remembered_picked_folder(const oa::platform::preferences::Values& values) {
    const auto picked = values.find(std::string{key::picked_mod_directory});
    const auto mod = values.find(std::string{key::mod_directory});
    if (picked == values.end() || mod == values.end() || picked->second != mod->second)
        return {};
    return picked->second;
}

std::string
stored_language(const oa::platform::preferences::Values& values, bool players_own_profile) {
    namespace languages = oa::data::languages;
    if (const auto found = values.find(std::string{key::language}); found != values.end()) {
        const std::string_view stored = trimmed(found->second);
        if (same_name(stored, languages::system_choice))
            return std::string(languages::system_choice);
        if (const auto* language = languages::find_by_tag(stored);
            language != nullptr && languages::drawable(*language))
            return std::string(language->tag);
    }
    return players_own_profile ? std::string(languages::system_choice)
                               : std::string(languages::english().tag);
}

std::optional<uint16_t>
installation_unit_limit(std::string_view ini_text, const oa::data::limits::UnitsPerPlayer& units) {
    std::string_view rest = ini_text.substr(0, std::min(ini_text.size(), installation_ini_limit));
    bool in_section = false;
    bool section_seen = false;
    while (!rest.empty()) {
        const auto end = rest.find('\n');
        const std::string_view line =
            trimmed(end == std::string_view::npos ? rest : rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        if (line.empty() || line.front() == ';')
            continue;
        if (line.front() == '[') {
            if (section_seen)
                return std::nullopt;
            const auto close = line.find(']');
            const auto name =
                trimmed(line.substr(1, close == std::string_view::npos ? close : close - 1));
            in_section = same_name(name, installation_section);
            section_seen = in_section;
            continue;
        }
        if (!in_section)
            continue;
        const auto equals = line.find('=');
        if (equals == std::string_view::npos ||
            !same_name(trimmed(line.substr(0, equals)), installation_unit_limit_key))
            continue;
        const auto number = leading_number(trimmed(line.substr(equals + 1)));
        const int64_t limit = number ? std::max(number->value, int64_t{0}) : 0;
        return static_cast<uint16_t>(
            oa::sim::session::clamp_unit_limit(static_cast<int32_t>(limit), units)
        );
    }
    return std::nullopt;
}

int32_t path_search_multiplier(int32_t nodes) noexcept {
    const int64_t nearest = (int64_t{nodes} + base_path_search_nodes / 2) / base_path_search_nodes;
    return clamped(nearest, int32_t{1}, highest_path_search_multiplier);
}

int32_t match_path_search_nodes(
    const EngineSettings& settings, bool shared_or_replay, int32_t game_nodes
) noexcept {
    if (shared_or_replay)
        return game_nodes;
    const int64_t scaled =
        int64_t{settings.path_search_nodes} * game_nodes / base_path_search_nodes;
    return static_cast<int32_t>(
        std::clamp<int64_t>(scaled, 1, oa::data::limits::highest_path_search_nodes)
    );
}

Locks settings_locks(const GameState& state) noexcept {
    Locks locks{};
    const Lock game_lock = !state.in_game                      ? Lock::none
                           : state.shared_game || state.replay ? Lock::set_by_host
                                                               : Lock::in_game;
    locks.path_search = game_lock;
    locks.unit_limit = game_lock;
    locks.skirmish_players = game_lock;
    locks.max_frame_rate = state.frame_rate_from_command_line ? Lock::command_line : Lock::none;
    locks.shared_game = state.in_game && state.shared_game;
    // A game never locks hardware acceleration: Off must stay possible in a
    // shared game, where On waits for the match to end.
    locks.hardware_acceleration = state.renderer_from_command_line ? Lock::command_line
                                  : state.acceleration_unavailable ? Lock::unavailable
                                                                   : Lock::none;
    locks.vertical_sync = state.vertical_sync_unavailable                        ? Lock::unavailable
                          : state.in_game && (state.shared_game || state.replay) ? Lock::in_game
                                                                                 : Lock::none;
    // The window's density holds for the run and changes from the next
    // start, so no game locks it.
    locks.native_density = state.native_density_windows             ? Lock::always_on
                           : state.native_density_from_command_line ? Lock::command_line
                                                                    : Lock::none;
    // The language changes only what players read, so no game locks it.
    locks.language = state.language_from_command_line ? Lock::command_line : Lock::none;
    // SHOW AGAIN starts the welcome setup at the next main menu, so a game
    // locks the button until the player is back there.
    locks.welcome_setup = state.in_game ? Lock::main_menu : Lock::none;
    return locks;
}

static_assert(
    oa::ui::pad_controls::default_hold_ms == default_touch_hold_ms,
    "the gamepad's hold delay defaults to the touch controls' own"
);

float control_size_scale(ControlSize size) noexcept {
    // The points scale of each Control size.
    constexpr float standard_scale = 1.0F;
    constexpr float large_scale = 1.25F;
    constexpr float larger_scale = 1.5F;
    switch (size) {
    case ControlSize::standard:
        break;
    case ControlSize::large:
        return large_scale;
    case ControlSize::larger:
        return larger_scale;
    }
    return standard_scale;
}

} // namespace oa::ui::engine_settings
