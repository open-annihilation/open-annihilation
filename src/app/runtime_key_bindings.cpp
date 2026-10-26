// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The key bindings' dispatcher. Each key, button and wheel is turned into
// action events after the input method has taken the keys it is composing.
// The events are kept. Nothing acts on them yet. Touch and the pad keep
// their own paths.

#include "oa/app/runtime.hpp"

#include "device_state.hpp"
#include "engine_settings_state.hpp"
#include "touch_state.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/match_console.hpp"
#include "oa_layer.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/data/key_library/library.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/key_bindings/dispatcher.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"
#include "oa/ui/key_bindings/layout_file.hpp"
#include "oa/ui/key_bindings/legacy.hpp"
#include "oa/ui/touch_hud.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {

namespace keys = oa::ui::key_bindings;
namespace actions = oa::data::input_actions;
namespace console = oa::ui::console;

namespace {

/// The bit of a context, or 0 when the table has no such name.
///
/// @param name the context id
/// @return the bit
uint32_t context_bit(std::string_view name) {
    const std::span<const actions::Context> table = actions::contexts();
    for (std::size_t index = 0; index < table.size() && index < 32; ++index) {
        if (table[index].name == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// The bit of a condition, or 0 when the table has no such name.
///
/// @param name the condition id
/// @return the bit
uint32_t condition_bit(std::string_view name) {
    const std::span<const std::string_view> table = actions::conditions();
    for (std::size_t index = 0; index < table.size() && index < 32; ++index) {
        if (table[index] == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// The physical modifier bits an event's modifiers carry. Never `primary`.
///
/// @param mods the SDL modifier bits
/// @return the binding bits
uint8_t binding_modifiers(SDL_Keymod mods) {
    const auto value = static_cast<uint32_t>(mods);
    uint8_t bits = 0;
    if ((value & SDL_KMOD_CTRL) != 0)
        bits = static_cast<uint8_t>(bits | keys::ctrl);
    if ((value & SDL_KMOD_SHIFT) != 0)
        bits = static_cast<uint8_t>(bits | keys::shift);
    if ((value & SDL_KMOD_ALT) != 0)
        bits = static_cast<uint8_t>(bits | keys::alt);
    if ((value & SDL_KMOD_GUI) != 0)
        bits = static_cast<uint8_t>(bits | keys::meta);
    return bits;
}

/// The sign of one wheel axis, as one notch.
///
/// @param value the axis
/// @return 1, -1 or 0
int8_t notch_sign(float value) {
    if (value > 0.F)
        return 1;
    if (value < 0.F)
        return -1;
    return 0;
}

/// The keyboard map SDL is using: what events carry, and what keys print.
///
/// @return the map
keys::KeyboardMap keyboard_from_sdl() {
    keys::KeyboardMap map;
    const int limit =
        std::min(static_cast<int>(SDL_SCANCODE_COUNT), static_cast<int>(keys::scancode_limit));
    for (int index = 1; index < limit; ++index) {
        const auto scancode = static_cast<SDL_Scancode>(index);
        const SDL_Keycode event_code = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, true);
        const SDL_Keycode printed_code = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
        char32_t printed = 0;
        if (printed_code > 0) {
            const auto printed_bits = static_cast<uint32_t>(printed_code);
            if ((printed_bits & keys::position_key_code_bit) == 0)
                printed = static_cast<char32_t>(printed_bits);
        }
        map.set(static_cast<uint16_t>(index), static_cast<uint32_t>(event_code), printed);
    }
    return map;
}

/// Which key Primary means on this build.
///
/// @return the platform
keys::Platform platform_here() {
    keys::Platform where;
#ifdef __APPLE__
    where.primary_is_meta = true;
#else
    where.primary_is_meta = false;
#endif
    return where;
}

/// A key, button or wheel as the dispatcher reads it, or nothing for another event.
///
/// @param event the SDL event
/// @param time_ms the time, from the same clock as `advance`
/// @return the input event
std::optional<keys::InputEvent> input_from_sdl(const SDL_Event& event, uint64_t time_ms) {
    keys::InputEvent input;
    input.time_ms = time_ms;
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
        input.kind = keys::InputKind::key;
        input.down = event.key.down;
        input.repeat = event.key.repeat;
        const auto scan = static_cast<uint32_t>(event.key.scancode);
        input.scancode = scan < keys::scancode_limit ? static_cast<uint16_t>(scan) : uint16_t{0};
        input.key_code = static_cast<uint32_t>(event.key.key);
        input.modifiers = binding_modifiers(event.key.mod);
        return input;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        input.kind = keys::InputKind::button;
        input.down = event.button.down;
        input.button = event.button.button;
        input.clicks = event.button.clicks;
        input.modifiers = binding_modifiers(device_state::modifiers_held());
        return input;
    }
    if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        float across = event.wheel.x;
        float away = event.wheel.y;
        if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            across = -across;
            away = -away;
        }
        input.kind = keys::InputKind::wheel;
        input.down = true;
        input.wheel_x = notch_sign(across);
        input.wheel_y = notch_sign(away);
        input.modifiers = binding_modifiers(device_state::modifiers_held());
        return input;
    }
    return std::nullopt;
}

/// Reports a key the devices or the automation endpoint hold.
///
/// @param context unused
/// @param scancode the position
/// @return true while that key is held
bool key_is_down(const void* context, uint16_t scancode) {
    (void)context;
    return device_state::key_held(static_cast<SDL_Scancode>(scancode));
}

namespace fs = std::filesystem;
namespace oakeys = oa::formats::oakeys;
namespace library = oa::data::key_library;

/// The preference that names the layout, and the per-mod key beside it.
constexpr std::string_view layout_preference = "open-annihilation.keys.layout";
/// Mods whose stored keys have already been imported.
constexpr std::string_view imported_preference = "open-annihilation.keys.imported";

/// Returns a path's generic UTF-8 spelling.
///
/// @param path the path
/// @return its UTF-8
std::string utf8_of(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

/// Returns a text with its ASCII letters lowered.
///
/// @param text the text
/// @return the text
std::string folded(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char byte : text) {
        const unsigned char lower =
            byte >= 'A' && byte <= 'Z' ? static_cast<unsigned char>(byte - 'A' + 'a') : byte;
        out.push_back(static_cast<char>(lower));
    }
    return out;
}

/// Reads a preference value. An absent key is empty, and the map is not changed.
///
/// @param values the preferences
/// @param key the key
/// @return the value, or empty
std::string
preference_text(const std::map<std::string, std::string>& values, std::string_view key) {
    const auto found = values.find(std::string(key));
    if (found == values.end())
        return {};
    return found->second;
}

/// Tells whether `--keys` names a path rather than an installed id.
///
/// A separator, an absolute path, a name that is already there, or a name
/// that ends in oakeys.yaml is a path.
///
/// @param value the option's value
/// @return true when it is a path
bool names_a_path(const std::string& value) {
    if (value.find('/') != std::string::npos || value.find('\\') != std::string::npos)
        return true;
    const fs::path path = path_from_utf8(value);
    if (path.is_absolute())
        return true;
    const std::string name = folded(value);
    constexpr std::string_view manifest = "oakeys.yaml";
    if (name.size() >= manifest.size() &&
        name.compare(name.size() - manifest.size(), manifest.size(), manifest) == 0)
        return true;
    std::error_code exists_error;
    return fs::exists(path, exists_error) && !exists_error;
}

/// Reads a file up to one byte past a bound.
///
/// @param file the file
/// @param most the most bytes that still count as read
/// @param[out] bytes the file, when it fits
/// @return true when the file was read and fits
bool read_bounded(const fs::path& file, std::size_t most, std::vector<uint8_t>& bytes) {
    std::error_code size_error;
    const uintmax_t size = fs::file_size(file, size_error);
    if (size_error || size > most)
        return false;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return false;
    bytes.clear();
    for (std::istreambuf_iterator<char> at{in}, end; at != end && bytes.size() <= most; ++at)
        bytes.push_back(static_cast<uint8_t>(*at));
    if (!in && !in.eof())
        return false;
    return bytes.size() <= most;
}

/// One manifest read, and the lines that describe why it failed.
struct ReadLayout {
    bool ok{};
    oakeys::Manifest manifest{};
    std::vector<std::string> lines{};
};

/// Reads oakeys.yaml, or the oakeys.yaml inside a folder.
///
/// @param path a file or a folder
/// @return the manifest, or the lines that say why it cannot be read
ReadLayout read_layout_file(const fs::path& path) {
    ReadLayout read;
    std::error_code status_error;
    const fs::file_status status = fs::symlink_status(path, status_error);
    fs::path file = path;
    if (!status_error && fs::is_directory(status))
        file = path / std::string(oakeys::manifest_file);
    std::vector<uint8_t> bytes;
    if (!read_bounded(file, oa::formats::oamod::max_input_bytes, bytes)) {
        read.lines.push_back(utf8_of(file) + ": it cannot be read");
        return read;
    }
    std::vector<oakeys::Problem> problems;
    if (!oakeys::read_manifest(bytes, read.manifest, problems)) {
        if (problems.empty())
            read.lines.push_back(utf8_of(file) + ": it cannot be read");
        for (const oakeys::Problem& problem : problems)
            read.lines.push_back(oakeys::describe(problem));
        return read;
    }
    read.ok = true;
    return read;
}

/// Prints one keys line on standard error.
///
/// @param text the line, without the prefix
void log_keys(std::string_view text) {
    std::cerr << "open-annihilation: keys: " << text << '\n';
}

/// Prints each problem of a layout.
///
/// @param problems the problems
void log_layout_problems(const std::vector<keys::LayoutProblem>& problems) {
    for (const keys::LayoutProblem& problem : problems) {
        log_keys(
            "oakeys.yaml:" + std::to_string(problem.line) + ":" + std::to_string(problem.column) +
            ": " + problem.key + ": " + problem.message
        );
    }
}

/// Tells whether a layout problem stops that layout being used.
///
/// @param problems the problems
/// @return true when one is an error
bool layout_blocked(const std::vector<keys::LayoutProblem>& problems) {
    for (const keys::LayoutProblem& problem : problems)
        if (!problem.warning)
            return true;
    return false;
}

/// Turns a manifest into a layout. Problems are logged. An error leaves the
/// layout unused.
///
/// @param manifest the manifest
/// @param[out] layout the layout, when every chord parsed
/// @return true when the layout can be used
bool use_layout(const oakeys::Manifest& manifest, keys::Layout& layout) {
    std::vector<keys::LayoutProblem> problems;
    const bool parsed = keys::layout_from_manifest(manifest, nullptr, layout, problems);
    log_layout_problems(problems);
    return parsed && !layout_blocked(problems);
}

/// The comma-separated ids of a preference.
///
/// @param text the value
/// @return the ids, without empty pieces
std::vector<std::string> split_ids(std::string_view text) {
    std::vector<std::string> ids;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t comma = text.find(',', at);
        std::string_view part =
            text.substr(at, comma == std::string_view::npos ? text.size() - at : comma - at);
        while (!part.empty() && part.front() == ' ')
            part.remove_prefix(1);
        while (!part.empty() && part.back() == ' ')
            part.remove_suffix(1);
        if (!part.empty())
            ids.emplace_back(part);
        if (comma == std::string_view::npos)
            break;
        at = comma + 1;
    }
    return ids;
}

/// Tells whether an id is one of a list.
///
/// @param ids the ids
/// @param id the id
/// @return true when the list holds it
bool lists_id(const std::vector<std::string>& ids, std::string_view id) {
    for (const std::string& one : ids)
        if (one == id)
            return true;
    return false;
}

/// Joins ids with commas.
///
/// @param ids the ids
/// @return the value
std::string join_ids(const std::vector<std::string>& ids) {
    std::string joined;
    for (const std::string& id : ids) {
        if (!joined.empty())
            joined.push_back(',');
        joined += id;
    }
    return joined;
}

/// The position of a stored key code, or 0 when it has none.
///
/// @param code the SDL key code
/// @return the position
uint16_t position_of(uint32_t code) {
    const auto key = static_cast<SDL_Keycode>(code);
    const SDL_Scancode scan = SDL_GetScancodeFromKey(key, nullptr);
    const auto raw = static_cast<uint32_t>(scan);
    if (raw == 0 || raw >= keys::scancode_limit)
        return 0;
    return static_cast<uint16_t>(raw);
}

/// Tells whether an own manifest already names an action.
///
/// @param manifest the own changes
/// @param action the action id
/// @return true when a binding names it
bool names_action(const oakeys::Manifest& manifest, std::string_view action) {
    for (const oakeys::BindingEntry& entry : manifest.bindings)
        if (entry.action == action)
            return true;
    return false;
}

/// Replaces or appends one action's chords.
///
/// @param[in,out] manifest the own changes
/// @param action the action id
/// @param chords the chords
void set_binding(
    oakeys::Manifest& manifest, std::string action, const std::vector<keys::Chord>& chords
) {
    std::vector<oakeys::ChordText> texts;
    texts.reserve(chords.size());
    for (const keys::Chord& chord : chords) {
        oakeys::ChordText text;
        text.text = keys::chord_text(chord);
        texts.push_back(std::move(text));
    }
    for (oakeys::BindingEntry& entry : manifest.bindings) {
        if (entry.action == action) {
            entry.chords = std::move(texts);
            return;
        }
    }
    oakeys::BindingEntry entry;
    entry.action = std::move(action);
    entry.chords = std::move(texts);
    manifest.bindings.push_back(std::move(entry));
}

/// A new own manifest for one layout.
///
/// @param base_id the layout the changes sit on
/// @return the manifest, with no bindings yet
oakeys::Manifest fresh_own(std::string_view base_id) {
    oakeys::Manifest manifest;
    manifest.format = oakeys::format_version;
    manifest.id = std::string("own-") + std::string(base_id);
    manifest.name = "Your changes";
    manifest.version.major = 1;
    manifest.version.minor = 0;
    manifest.version.parts = 2;
    manifest.author.name = "You";
    manifest.tags = {"own"};
    manifest.base = oakeys::Base::oa_default;
    return manifest;
}

/// The word `--trace-input` prints for an action's phase.
///
/// @param phase the phase
/// @return the word
const char* phase_word(keys::Phase phase) noexcept {
    switch (phase) {
    case keys::Phase::pressed:
        return "pressed";
    case keys::Phase::released:
        return "released";
    case keys::Phase::repeated:
        return "repeated";
    case keys::Phase::double_press:
        return "double";
    case keys::Phase::held:
        return "held";
    case keys::Phase::active:
        return "active";
    case keys::Phase::inactive:
        return "inactive";
    }
    return "pressed";
}

/// The modifier bit a key's own position holds, or 0.
///
/// @param scancode the position
/// @return the bit
uint8_t modifier_bit_of(uint16_t scancode) {
    struct Key {
        const char* code;
        uint8_t bit;
    };

    static constexpr Key keys[] = {
        {"ShiftLeft", keys::shift},
        {"ShiftRight", keys::shift},
        {"ControlLeft", keys::ctrl},
        {"ControlRight", keys::ctrl},
        {"AltLeft", keys::alt},
        {"AltRight", keys::alt},
        {"MetaLeft", keys::meta},
        {"MetaRight", keys::meta},
    };
    for (const Key& key : keys) {
        const std::optional<uint16_t> place = keys::scancode_of(key.code);
        if (place && *place == scancode)
            return key.bit;
    }
    return 0;
}

/// QUEUE, ADD or x5, when this id is one of those shared roles.
struct SharedRole {
    const char* id;
    oa::ui::touch_hud::Latch latch;
    oa::ui::touch_hud::ActionClass action;
};

/// The shared role of an action id, or null.
///
/// @param id the action id
/// @return the role
const SharedRole* shared_role_of(std::string_view id) {
    static constexpr SharedRole roles[] = {
        {"order.queue-modifier",
         oa::ui::touch_hud::Latch::queue,
         oa::ui::touch_hud::ActionClass::order},
        {"select.add-modifier",
         oa::ui::touch_hud::Latch::add,
         oa::ui::touch_hud::ActionClass::selection},
        {"build.queue-modifier",
         oa::ui::touch_hud::Latch::times_five,
         oa::ui::touch_hud::ActionClass::build_button},
    };
    for (const SharedRole& role : roles) {
        if (id == role.id)
            return &role;
    }
    return nullptr;
}

/// ASCII letters in capitals. Anything else is copied.
///
/// @param text the action's name
/// @return the chip's name
std::string capitals(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char byte : text) {
        const unsigned char upper =
            byte >= 'a' && byte <= 'z' ? static_cast<unsigned char>(byte - 'a' + 'A') : byte;
        out.push_back(static_cast<char>(upper));
    }
    return out;
}

/// An interface sentence with one token replaced. The lookup is copied first.
///
/// @param english the catalogue sentence
/// @param token the placeholder, including braces
/// @param value the replacement
/// @return the sentence
std::string
chip_sentence(std::string_view english, std::string_view token, std::string_view value) {
    std::string text{oa::data::languages::interface_text(english)};
    const auto at = text.find(token);
    if (at != std::string::npos)
        text.replace(at, token.size(), value);
    return text;
}

/// Whether the binding's conditions hold and one of its contexts is open.
///
/// @param bound the binding
/// @param contexts the open contexts
/// @return true when this event may latch it
bool binding_open(const keys::Bound& bound, const keys::Contexts& contexts) {
    return (bound.when & ~contexts.conditions) == 0 && (bound.contexts & contexts.active) != 0;
}

/// Whether this key event is one of the binding's chords.
///
/// A modifier alone matches the key's own bit. Any other chord matches the event.
///
/// @param bound the binding
/// @param input the event
/// @param bit the modifier bit of the key, or 0
/// @param map the keyboard
/// @param platform which key Primary means
/// @return true when the key is this binding
bool binding_key(
    const keys::Bound& bound,
    const keys::InputEvent& input,
    uint8_t bit,
    const keys::KeyboardMap& map,
    keys::Platform platform
) {
    for (const keys::BoundChord& item : bound.chords) {
        if (item.chord.kind == keys::TriggerKind::modifier_alone) {
            if (bit != 0 && keys::held(item.chord, bit, platform))
                return true;
            continue;
        }
        if (keys::match(item.chord, input, map, platform) != keys::Match::none)
            return true;
    }
    return false;
}

/// The English name of a modifier bit, or empty.
///
/// @param bit one modifier bit
/// @return SHIFT, CTRL, ALT or META
const char* modifier_label(uint8_t bit) {
    if (bit == keys::shift)
        return "SHIFT";
    if (bit == keys::ctrl)
        return "CTRL";
    if (bit == keys::alt)
        return "ALT";
    if (bit == keys::meta)
        return "META";
    return nullptr;
}

} // namespace

struct Runtime::KeyBindingsState {
    keys::Dispatcher dispatcher{};
    keys::Resolved resolved{};
    keys::KeyboardMap keyboard{};
    keys::Platform platform{};
    std::vector<keys::ActionEvent> events{};
    std::vector<std::string> hacks{};
    keys::PointerScheme scheme{keys::PointerScheme::right_click};
    bool number_keys{};
    uint64_t map_generation{};
    bool ready{};
    keys::Base base{keys::Base::oa_default};
    keys::Layout chosen{};
    bool has_chosen{};
    keys::Layout own{};
    bool has_own{};
    /// What the last load read. A later call with the same text does no work.
    std::string signature{};
    /// The resolved layout's helpers.toggle, chosen then the player's own.
    std::vector<std::string> toggle{};

    /// A shared latch this key is holding, so its release toggles that latch.
    struct SharedPress {
        uint16_t scancode{};
        oa::ui::touch_hud::Latch latch{};
    };

    std::vector<SharedPress> shared_presses{};

    /// Whether the resolved bindings name this action.
    ///
    /// @param action the action id
    /// @return true when the id is in the bindings
    [[nodiscard]] bool has_action(std::string_view action) const {
        for (const keys::Bound& bound : resolved.actions) {
            if (bound.id == action)
                return true;
        }
        return false;
    }
};

void Runtime::destroy_key_bindings_state(KeyBindingsState* state) noexcept {
    delete state;
}

/// Whether one hack is on: the display rules, the match's console when it
/// already exists, or the profile the match plays by.
///
/// @param id the hack id
/// @return true while that hack is on
bool Runtime::key_binding_hack(std::string_view id) const {
    const oa::data::mod_profile::UiRules& ui = ui_rules();
    const oa::data::mod_profile::ModProfile* profile = mod_profile();
    if (id == "ui.megamap")
        return ui.megamap.enabled;
    if (id == "ui.whiteboard")
        return ui.whiteboard.enabled;
    if (id == "ui.resource-panel")
        return ui.resource_panel.enabled;
    if (id == "ui.selection-shortcuts")
        return ui.selection_shortcuts.enabled;
    if (id == "ui.build-tools")
        return ui.build_tools.enabled;
    if (id == "ui.click-snap")
        return ui.click_snap.enabled;
    if (id == "ui.build-preview")
        return ui.build_preview.enabled;
    if (id == "ui.options-dialog")
        return ui.options_dialog.enabled;
    if (id == "ui.display-modes")
        return ui.display_modes.enabled;
    if (id == "units.build-rotation")
        return profile != nullptr && profile->rules.units.build_rotation.enabled;
    if (id == "console.key-remaps") {
        if (console_ != nullptr)
            return console_->state.key_remaps;
        return profile != nullptr && profile->rules.console.key_remaps.enabled;
    }
    if (id == "teams.alliance-menu-all-game-types") {
        if (console_ != nullptr)
            return console_->state.team_menu_every_game;
        return profile != nullptr && profile->rules.teams.alliance_menu_all_game_types.enabled;
    }
    return false;
}

void Runtime::load_key_bindings() {
    key_bindings_armed_ = true;
    const auto* profile = mod_profile();
    const std::string mod_id = profile != nullptr ? profile->id : std::string{};
    const oa::data::mod_profile::UiRules& ui = ui_rules();
    const bool import_hacks = ui.build_tools.enabled || ui.click_snap.enabled ||
                              ui.build_preview.enabled || ui.options_dialog.enabled;
    const auto signature_now = [&] {
        std::string text;
        text += options_.keys_reset ? "reset\n" : "run\n";
        text += options_.keys;
        text += '\n';
        text += mod_id;
        text += '\n';
        text += preference_text(preference_values_, layout_preference);
        text += '\n';
        if (!mod_id.empty())
            text +=
                preference_text(preference_values_, std::string(layout_preference) + "." + mod_id);
        text += '\n';
        text += preference_text(preference_values_, imported_preference);
        text += '\n';
        text += import_hacks ? "hacks\n" : "plain\n";
        text += std::to_string(view_settings_.snap_override_key);
        text += '\n';
        text += std::to_string(view_settings_.autoclick_key);
        text += '\n';
        text += std::to_string(view_settings_.rotate_build_key);
        text += '\n';
        text += utf8_of(user_folder_);
        return text;
    };
    if (key_bindings_state_ != nullptr && key_bindings_state_->signature == signature_now())
        return;

    const bool one_run = options_.keys_reset || !options_.keys.empty();
    const fs::path keys_folder = user_folder_ / std::string(library::keys_folder_name);
    const library::Listing listing = library::list_installed(keys_folder);

    keys::Base base = keys::Base::oa_default;
    keys::Layout chosen;
    bool has_chosen = false;
    std::string base_id = "oa-default";
    std::optional<std::vector<std::string>> chosen_toggle;

    const auto use_builtin = [&] {
        base = keys::Base::oa_default;
        has_chosen = false;
        base_id = "oa-default";
        chosen_toggle.reset();
    };
    const auto use_manifest = [&](const oakeys::Manifest& manifest, bool refuse_errors) {
        keys::Layout layout;
        std::vector<keys::LayoutProblem> problems;
        const bool parsed = keys::layout_from_manifest(manifest, nullptr, layout, problems);
        log_layout_problems(problems);
        if (!parsed || layout_blocked(problems)) {
            if (refuse_errors) {
                key_bindings_refusal_ = "--keys: " + options_.keys + ": the layout cannot be read";
                log_keys(key_bindings_refusal_);
            } else {
                log_keys("layout " + manifest.id + " cannot be read; using oa-default");
                use_builtin();
            }
            return;
        }
        base = layout.base;
        base_id = layout.id.empty() ? std::string("oa-default") : layout.id;
        chosen = std::move(layout);
        has_chosen = true;
        chosen_toggle = manifest.helpers.toggle;
    };

    if (options_.keys_reset) {
        use_builtin();
    } else if (!options_.keys.empty()) {
        if (!names_a_path(options_.keys)) {
            if (options_.keys == "oa-default") {
                use_builtin();
            } else {
                const library::Installed* installed = library::find(listing, options_.keys);
                if (installed == nullptr) {
                    key_bindings_refusal_ = "--keys: " + options_.keys + ": it is not installed";
                    log_keys(key_bindings_refusal_);
                } else {
                    use_manifest(installed->manifest, true);
                }
            }
        } else {
            const ReadLayout read = read_layout_file(path_from_utf8(options_.keys));
            if (!read.ok) {
                for (const std::string& line : read.lines)
                    log_keys(line);
                key_bindings_refusal_ = "--keys: " + options_.keys + ": the layout cannot be read";
                log_keys(key_bindings_refusal_);
            } else {
                use_manifest(read.manifest, true);
            }
        }
    } else {
        std::string asked;
        if (!mod_id.empty()) {
            const std::string per_mod =
                preference_text(preference_values_, std::string(layout_preference) + "." + mod_id);
            if (!per_mod.empty() && per_mod != "same")
                asked = per_mod;
        }
        if (asked.empty())
            asked = preference_text(preference_values_, layout_preference);
        if (asked.empty() || asked == "oa-default" || asked == "same") {
            use_builtin();
        } else {
            const library::Installed* installed = library::find(listing, asked);
            if (installed == nullptr) {
                log_keys("layout " + asked + " is not installed; using oa-default");
                use_builtin();
            } else {
                use_manifest(installed->manifest, false);
            }
        }
    }

    const bool apply_own = !one_run && key_bindings_refusal_.empty();
    keys::Layout own_layout;
    bool has_own = false;
    oakeys::Manifest own_manifest;
    bool have_own_manifest = false;
    bool own_broken = false;
    if (apply_own) {
        const fs::path own_file =
            keys_folder / library::own_folder_name(base_id) / std::string(oakeys::manifest_file);
        std::error_code own_error;
        if (fs::exists(own_file, own_error) && !own_error) {
            const ReadLayout read = read_layout_file(own_file);
            if (!read.ok) {
                own_broken = true;
                for (const std::string& line : read.lines)
                    log_keys(line);
            } else {
                own_manifest = read.manifest;
                have_own_manifest = true;
                if (use_layout(own_manifest, own_layout))
                    has_own = true;
                else
                    own_broken = true;
            }
        }
    }

    bool needed_write = false;
    if (apply_own && import_hacks && !mod_id.empty()) {
        const std::vector<std::string> already =
            split_ids(preference_text(preference_values_, imported_preference));
        if (!lists_id(already, mod_id)) {
            struct StoredKey {
                const char* setting;
                const char* action;
                uint32_t code;
            };

            const StoredKey stored[] = {
                {"ClickSnapOverrideKey", "build.snap-override", view_settings_.snap_override_key},
                {"KeyCode", "build.line", view_settings_.autoclick_key},
                {"RotateBuildKey", "build.rotate", view_settings_.rotate_build_key},
            };
            if (!have_own_manifest && !own_broken)
                own_manifest = fresh_own(base_id);
            for (const StoredKey& stored_key : stored) {
                const actions::Action* action = actions::find(stored_key.action);
                if (action == nullptr)
                    continue;
                if (have_own_manifest && names_action(own_manifest, stored_key.action)) {
                    log_keys(
                        std::string("skipped ") + stored_key.action +
                        "; your changes already name it"
                    );
                    continue;
                }
                const std::optional<keys::Chord> chord = keys::legacy_key_chord(
                    stored_key.code, position_of(stored_key.code), action->by
                );
                if (!chord) {
                    log_keys(
                        std::string("skipped ") + stored_key.setting + "; it maps to no chord"
                    );
                    continue;
                }
                bool same_as_default = false;
                if (!action->defaults.empty()) {
                    keys::ChordError chord_error;
                    const std::optional<keys::Chord> first =
                        keys::parse_chord(action->defaults.front(), chord_error);
                    same_as_default = first.has_value() && *first == *chord;
                }
                if (same_as_default) {
                    log_keys(
                        std::string("skipped ") + stored_key.setting + "; it is already the default"
                    );
                    continue;
                }
                if (own_broken) {
                    needed_write = true;
                    log_keys(
                        std::string("skipped ") + stored_key.action +
                        "; your changes cannot be read"
                    );
                    continue;
                }
                const std::vector<keys::Chord> chords = keys::imported_list(*action, *chord);
                if (chords.empty()) {
                    log_keys(
                        std::string("skipped ") + stored_key.setting + "; it maps to no chord"
                    );
                    continue;
                }
                needed_write = true;
                std::string shown;
                for (const keys::Chord& one : chords) {
                    if (!shown.empty())
                        shown += ", ";
                    shown += keys::chord_text(one);
                }
                set_binding(own_manifest, std::string(stored_key.action), chords);
                have_own_manifest = true;
                log_keys(
                    std::string("imported ") + stored_key.action + " from " + stored_key.setting +
                    " as " + shown
                );
            }
            bool wrote = !needed_write;
            if (needed_write && !own_broken) {
                std::string write_error;
                wrote = library::write_own(keys_folder, base_id, own_manifest, write_error);
                if (!wrote)
                    log_keys(base_id + ": " + write_error);
            }
            if (wrote && !(needed_write && own_broken)) {
                std::vector<std::string> ids = already;
                ids.push_back(mod_id);
                const std::string joined = join_ids(ids);
                const std::string key{imported_preference};
                const auto found = preference_values_.find(key);
                if (found == preference_values_.end() || found->second != joined) {
                    preference_values_[key] = joined;
                    preferences_dirty_ = true;
                }
            }
            if (have_own_manifest && !own_broken) {
                keys::Layout imported;
                if (use_layout(own_manifest, imported)) {
                    own_layout = std::move(imported);
                    has_own = true;
                }
            }
        }
    }

    if (!key_bindings_refusal_.empty())
        return;
    if (key_bindings_state_ == nullptr) {
        std::unique_ptr<KeyBindingsState, void (*)(KeyBindingsState*) noexcept> made{
            new KeyBindingsState, destroy_key_bindings_state
        };
        key_bindings_state_ = std::move(made);
    }
    KeyBindingsState& state = *key_bindings_state_;
    state.base = base;
    state.has_chosen = has_chosen;
    if (has_chosen)
        state.chosen = std::move(chosen);
    state.has_own = has_own;
    if (has_own)
        state.own = std::move(own_layout);
    std::vector<std::string> merged;
    if (chosen_toggle)
        merged = *chosen_toggle;
    if (apply_own && have_own_manifest && !own_broken && own_manifest.helpers.toggle)
        merged = *own_manifest.helpers.toggle;
    state.toggle = std::move(merged);
    state.signature = signature_now();
    state.ready = false;
}

void Runtime::ensure_key_bindings(bool keymap_changed) {
    if (key_bindings_armed_)
        load_key_bindings();
    if (!key_bindings_state_) {
        std::unique_ptr<KeyBindingsState, void (*)(KeyBindingsState*) noexcept> made{
            new KeyBindingsState, destroy_key_bindings_state
        };
        key_bindings_state_ = std::move(made);
    }
    KeyBindingsState& state = *key_bindings_state_;
    bool map_changed = false;
    if (keymap_changed || state.map_generation == 0) {
        state.keyboard = keyboard_from_sdl();
        state.platform = platform_here();
        ++state.map_generation;
        if (state.map_generation == 0)
            state.map_generation = 1;
        map_changed = true;
    }

    std::vector<std::string> hacks;
    for (const std::string_view id : actions::hack_ids()) {
        if (key_binding_hack(id))
            hacks.emplace_back(id);
    }
    const int32_t face =
        match_ != nullptr ? match_->state().game.interface_type : preferences_.interface_type;
    const keys::PointerScheme scheme = face == oa::sim::gameplay_input::interface_left_click
                                           ? keys::PointerScheme::left_click
                                           : keys::PointerScheme::right_click;
    const bool number_keys = EngineSettingsState::live_switch_alt(*this);
    if (state.ready && !map_changed && state.hacks == hacks && state.scheme == scheme &&
        state.number_keys == number_keys)
        return;

    // The release events would point into the bindings set_bindings replaces.
    (void)state.dispatcher.release_all(SDL_GetTicks());
    state.events.clear();
    if (touch_state_if_made() != nullptr) {
        auto& hud_latches = touch_state().hud.latches;
        for (const KeyBindingsState::SharedPress& press : state.shared_presses)
            hud_latches.cancel(press.latch);
    }
    state.shared_presses.clear();
    keys::ResolveInput input;
    input.base = state.base;
    input.chosen = state.has_chosen ? &state.chosen : nullptr;
    input.own = state.has_own ? &state.own : nullptr;
    input.keyboard = &state.keyboard;
    input.platform = state.platform;
    input.hacks_on = hacks;
    input.scheme = scheme;
    input.number_keys_select_groups = number_keys;
    keys::Resolved resolved = keys::resolve(input);
    state.dispatcher.set_keyboard(state.keyboard);
    state.dispatcher.set_platform(state.platform);
    state.dispatcher.set_bindings(resolved);
    std::vector<std::string> dispatcher_toggle;
    for (const std::string& id : state.toggle) {
        if (shared_role_of(id) == nullptr)
            dispatcher_toggle.push_back(id);
    }
    state.dispatcher.set_toggle(std::move(dispatcher_toggle));
    state.dispatcher.set_sticky_modifiers(engine_settings().sticky_modifiers);
    state.resolved = std::move(resolved);
    state.hacks = std::move(hacks);
    state.scheme = scheme;
    state.number_keys = number_keys;
    state.ready = true;
    rebuild_pointer_roles(state.resolved);
}

keys::Contexts Runtime::input_contexts() {
    keys::Contexts contexts;
    contexts.active = context_bit("global");
    // A modal Open Annihilation screen takes the input, so the match and the
    // menus are not searched under it.
    if (oa_layer().modal_shown(screen_ == Screen::match)) {
        contexts.active |= context_bit("oa-screens");
    } else if (screen_ == Screen::match && match_ != nullptr) {
        contexts.active |= context_bit("match");
        const bool watcher = (current_extension_state() & extension_state::local_watcher) != 0;
        contexts.active |= watcher ? context_bit("watch") : context_bit("play");
        if (chat_composing_ || whiteboard_input_.editing)
            contexts.active |= context_bit("chat");
        if (match_paused_ && (pause_menu_shown() || team_panel_open()))
            contexts.active |= context_bit("panel");
        if (unit_info_panel_.has_value())
            contexts.active |= context_bit("unit-info");
        if (megamap_shown())
            contexts.active |= context_bit("megamap");
        if (whiteboard_on() && device_state::key_held(SDL_SCANCODE_BACKSLASH))
            contexts.active |= context_bit("whiteboard");
        if (match_command_ == MatchCommand::build || pending_build_type_ != 0)
            contexts.active |= context_bit("placing");
        if (match_drag_.has_value())
            contexts.active |= context_bit("box");
    } else if (screen_ != Screen::match) {
        contexts.active |= context_bit("menus");
    }

    contexts.text_entry = chat_composing_ || whiteboard_input_.editing || save_dialog_open();
    if (multiplayer_session())
        contexts.conditions |= condition_bit("multiplayer");
    else if (match_ != nullptr)
        contexts.conditions |= condition_bit("single-player");
    if (match_ != nullptr && (current_extension_state() & extension_state::local_watcher) == 0)
        contexts.conditions |= condition_bit("seat");
    if (has_local_selection())
        contexts.conditions |= condition_bit("selection");
    if (match_ != nullptr) {
        const oa::Game& game = match_->state().game;
        if ((console::console_flags(game) & console::console_flag::developer) != 0)
            contexts.conditions |= condition_bit("developer");
        if ((game.outcome_flags & console::outcome_flag::debug_keys) != 0)
            contexts.conditions |= condition_bit("debug-keys");
    }
    if (engine_settings().wheel_zoom)
        contexts.conditions |= condition_bit("wheel-zoom");
    else
        contexts.conditions |= condition_bit("no-wheel-zoom");
    return contexts;
}

void Runtime::trace_fed_actions(const std::vector<keys::ActionEvent>& events) const {
    if (!options_.trace_input || events.empty())
        return;
    const std::span<const actions::Context> table = actions::contexts();
    for (const keys::ActionEvent& action_event : events) {
        std::string_view context_name;
        if (action_event.context < table.size())
            context_name = table[action_event.context].name;
        std::cerr << "input action " << phase_word(action_event.phase) << ' ' << action_event.action
                  << " chord=" << keys::chord_text(action_event.chord)
                  << " context=" << context_name << '\n';
    }
}

void Runtime::feed_input_actions(const SDL_Event& event) {
    const bool keymap_changed = event.type == SDL_EVENT_KEYMAP_CHANGED;
    ensure_key_bindings(keymap_changed);
    KeyBindingsState& state = *key_bindings_state_;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        state.events = state.dispatcher.release_all(SDL_GetTicks()).events;
        if (touch_state_if_made() != nullptr)
            touch_state().hud.latches.clear();
        state.shared_presses.clear();
        sticky_event_ = false;
        sticky_event_mods_ = 0;
        key_latch_seen_.clear();
        key_latch_chips_drawn_.clear();
        refresh_pointer_modifiers();
        trace_fed_actions(state.events);
        return;
    }
    const std::optional<keys::InputEvent> input = input_from_sdl(event, SDL_GetTicks());
    if (!input)
        return;
    const keys::Contexts contexts = input_contexts();
    state.dispatcher.set_contexts(contexts);
    apply_shared_key_latches(*input, contexts);
    state.events = state.dispatcher.feed(*input).events;
    trace_fed_actions(state.events);
}

void Runtime::advance_input_actions() {
    ensure_key_bindings(false);
    KeyBindingsState& state = *key_bindings_state_;
    const keys::Fed fed = state.dispatcher.advance(SDL_GetTicks());
    // An empty frame leaves the last event's action events in place, and is
    // not printed again.
    if (!fed.events.empty()) {
        state.events = fed.events;
        trace_fed_actions(state.events);
    }
}

void Runtime::apply_binding_roles(uint32_t& modifiers, ModifierUse use) const {
    if (use == ModifierUse::keyboard || key_bindings_state_ == nullptr ||
        !key_bindings_state_->ready)
        return;
    const auto adjust = [&](std::string_view action, SDL_Keymod family, SDL_Keymod added) {
        if (!key_bindings_state_->has_action(action))
            return;
        keys::HeldKeys keys_held;
        keys_held.down = key_is_down;
        const bool active = key_bindings_state_->dispatcher.role_active(
            action, binding_modifiers(static_cast<SDL_Keymod>(modifiers)), keys_held
        );
        const auto family_bits = static_cast<uint32_t>(family);
        if (active) {
            if ((modifiers & family_bits) == 0)
                modifiers |= static_cast<uint32_t>(added);
        } else {
            modifiers &= ~family_bits;
        }
    };
    if (use == ModifierUse::order)
        adjust("order.queue-modifier", SDL_KMOD_SHIFT, SDL_KMOD_LSHIFT);
    else if (use == ModifierUse::selection)
        adjust("select.add-modifier", SDL_KMOD_SHIFT, SDL_KMOD_LSHIFT);
    else if (use == ModifierUse::build_button)
        adjust("build.queue-modifier", SDL_KMOD_SHIFT, SDL_KMOD_LSHIFT);
    if (use == ModifierUse::order)
        adjust("order.force-modifier", SDL_KMOD_CTRL, SDL_KMOD_LCTRL);
}

bool Runtime::modifier_role_active(std::string_view action) const {
    const uint8_t mods = pointer_modifier_bits();
    if (key_bindings_state_ != nullptr && key_bindings_state_->ready) {
        keys::HeldKeys held_keys;
        held_keys.down = key_is_down;
        if (key_bindings_state_->dispatcher.role_active(action, mods, held_keys))
            return true;
        // A pointer chord's button is someone else's check. Its modifiers are
        // the role role_active cannot see.
        return pointer_role_modifiers_held(action, mods);
    }
    if (action == "order.force-modifier" || action == "build.hundred-modifier")
        return (mods & keys::ctrl) != 0;
    if (action == "order.queue-modifier" || action == "select.add-modifier" ||
        action == "build.queue-modifier" || action == "build.move-queued-site")
        return (mods & keys::shift) != 0;
    return false;
}

void Runtime::apply_shared_key_latches(
    const keys::InputEvent& input, const keys::Contexts& contexts
) {
    if (key_bindings_state_ == nullptr)
        return;
    KeyBindingsState& state = *key_bindings_state_;
    bool listed = false;
    for (const std::string& id : state.toggle) {
        if (shared_role_of(id) != nullptr)
            listed = true;
    }
    if (!listed)
        return;
    if (input.repeat)
        return;

    namespace hud = oa::ui::touch_hud;
    const auto release_scancode = [&](uint16_t scancode) {
        if (touch_state_if_made() == nullptr) {
            std::vector<KeyBindingsState::SharedPress> kept;
            for (const KeyBindingsState::SharedPress& press : state.shared_presses) {
                if (press.scancode != scancode)
                    kept.push_back(press);
            }
            state.shared_presses = std::move(kept);
            return;
        }
        auto& hud_latches = touch_state().hud.latches;
        const uint32_t hold_ms = state.dispatcher.timings().long_press_ms;
        std::vector<KeyBindingsState::SharedPress> kept;
        for (const KeyBindingsState::SharedPress& press : state.shared_presses) {
            if (press.scancode != scancode) {
                kept.push_back(press);
                continue;
            }
            hud_latches.release(press.latch, input.time_ms, hold_ms);
        }
        state.shared_presses = std::move(kept);
    };

    if (input.kind == keys::InputKind::button && input.down) {
        if (touch_state_if_made() == nullptr || state.shared_presses.empty())
            return;
        auto& hud_latches = touch_state().hud.latches;
        for (const KeyBindingsState::SharedPress& press : state.shared_presses) {
            if (!hud_latches.held(press.latch))
                continue;
            const hud::ActionClass action =
                press.latch == hud::Latch::add          ? hud::ActionClass::selection
                : press.latch == hud::Latch::times_five ? hud::ActionClass::build_button
                                                        : hud::ActionClass::order;
            hud_latches.used(action, hud::LatchMode::stay_on);
        }
        return;
    }
    if (input.kind != keys::InputKind::key)
        return;
    if (!input.down) {
        release_scancode(input.scancode);
        return;
    }

    const uint8_t bit = modifier_bit_of(input.scancode);
    for (const std::string& id : state.toggle) {
        const SharedRole* role = shared_role_of(id);
        if (role == nullptr)
            continue;
        const keys::Bound* bound = nullptr;
        for (const keys::Bound& item : state.resolved.actions) {
            if (item.id == id) {
                bound = &item;
                break;
            }
        }
        if (bound == nullptr || !binding_open(*bound, contexts))
            continue;
        bool already = false;
        for (const KeyBindingsState::SharedPress& press : state.shared_presses) {
            if (press.latch == role->latch)
                already = true;
        }
        if (already)
            continue;
        if (touch_state_if_made() != nullptr && touch_state().hud.latches.held(role->latch))
            continue;
        if (!binding_key(*bound, input, bit, state.keyboard, state.platform))
            continue;
        touch_state().hud.latches.press(role->latch, input.time_ms);
        state.shared_presses.push_back(KeyBindingsState::SharedPress{input.scancode, role->latch});
    }
}

void Runtime::apply_sticky_modifiers(bool on) {
    if (key_bindings_state_ == nullptr)
        return;
    key_bindings_state_->dispatcher.set_sticky_modifiers(on);
}

uint8_t Runtime::sticky_modifier_bits() const {
    if (sticky_event_)
        return sticky_event_mods_;
    if (key_bindings_state_ == nullptr || !key_bindings_state_->ready)
        return 0;
    return key_bindings_state_->dispatcher.sticky().mods();
}

void Runtime::begin_sticky_event() {
    sticky_event_mods_ = 0;
    sticky_event_ = false;
    if (key_bindings_state_ == nullptr || !key_bindings_state_->ready)
        return;
    sticky_event_mods_ = key_bindings_state_->dispatcher.sticky().mods();
    sticky_event_ = true;
}

void Runtime::end_sticky_event() {
    sticky_event_ = false;
    sticky_event_mods_ = 0;
    refresh_pointer_modifiers();
}

void Runtime::consume_sticky_modifiers() {
    if (key_bindings_state_ == nullptr || !key_bindings_state_->ready)
        return;
    key_bindings_state_->dispatcher.consume_sticky();
}

void Runtime::note_shared_latch_used(int which) {
    const char* id = nullptr;
    oa::ui::touch_hud::ActionClass action = oa::ui::touch_hud::ActionClass::order;
    if (which == 0) {
        id = "select.add-modifier";
        action = oa::ui::touch_hud::ActionClass::selection;
    } else if (which == 1) {
        id = "order.queue-modifier";
        action = oa::ui::touch_hud::ActionClass::order;
    } else if (which == 2) {
        id = "build.queue-modifier";
        action = oa::ui::touch_hud::ActionClass::build_button;
    } else {
        return;
    }
    if (key_bindings_state_ == nullptr)
        return;
    bool listed = false;
    for (const std::string& toggle_id : key_bindings_state_->toggle) {
        if (toggle_id == id)
            listed = true;
    }
    if (!listed || touch_state_if_made() == nullptr)
        return;
    auto& look = touch_state().hud;
    look.latches.used(action, look.latch_mode);
}

bool Runtime::action_latched(std::string_view id) const {
    if (key_bindings_state_ == nullptr || !key_bindings_state_->ready)
        return false;
    return key_bindings_state_->dispatcher.latches().latched(id);
}

void Runtime::gather_key_latch_chips(std::vector<KeyLatchChip>& chips) {
    chips.clear();
    if (key_bindings_state_ == nullptr || !key_bindings_state_->ready)
        return;
    const KeyBindingsState& state = *key_bindings_state_;

    struct Item {
        std::string key;
        std::string text;
        bool filled;
    };

    std::vector<Item> items;

    const auto latched_line = [&](std::string_view id) {
        const actions::Action* action = actions::find(id);
        const std::string_view name = action != nullptr ? action->words.name : id;
        return chip_sentence("{name} · latched", "{name}", capitals(name));
    };

    for (const std::string& id : state.dispatcher.latches().shown())
        items.push_back(Item{"d:" + id, latched_line(id), true});

    const uint8_t locked = state.dispatcher.sticky().locked();
    for (const uint8_t bit : state.dispatcher.sticky().shown()) {
        const char* label = modifier_label(bit);
        if (label == nullptr)
            continue;
        const bool stays = (locked & bit) != 0;
        const char* sentence = stays ? "{modifier} · locked" : "{modifier} · next key or click";
        items.push_back(
            Item{"s:" + std::to_string(bit), chip_sentence(sentence, "{modifier}", label), stays}
        );
    }

    bool touch_on = touch_controls_active();
    bool pad_on = false;
    bool build_page = false;
    bool over_build = false;
    const TouchState* touch = touch_state_if_made();
    if (touch != nullptr) {
        pad_on = touch->hud.pad.hud;
        build_page = touch->hud.build_page_loaded;
        over_build = touch->hud.pad.over_build_button;
    }
    const bool hide_queue_add = touch_on || pad_on;
    const bool hide_times_five = (touch_on && build_page) || (pad_on && over_build);
    if (touch != nullptr) {
        const auto consider = [&](const char* id, oa::ui::touch_hud::Latch latch, bool hidden) {
            if (hidden || !touch->hud.latches.latched(latch))
                return;
            bool wanted = false;
            for (const std::string& toggle_id : state.toggle) {
                if (toggle_id == id)
                    wanted = true;
            }
            if (!wanted)
                return;
            items.push_back(Item{std::string("h:") + id, latched_line(id), true});
        };
        consider("order.queue-modifier", oa::ui::touch_hud::Latch::queue, hide_queue_add);
        consider("select.add-modifier", oa::ui::touch_hud::Latch::add, hide_queue_add);
        consider("build.queue-modifier", oa::ui::touch_hud::Latch::times_five, hide_times_five);
    }

    std::vector<KeyLatchSeen> kept;
    for (const KeyLatchSeen& seen : key_latch_seen_) {
        for (const Item& item : items) {
            if (item.key == seen.key) {
                kept.push_back(seen);
                break;
            }
        }
    }
    for (const Item& item : items) {
        bool found = false;
        for (const KeyLatchSeen& seen : kept) {
            if (seen.key == item.key)
                found = true;
        }
        if (!found)
            kept.push_back(KeyLatchSeen{item.key, key_latch_seen_next_++});
    }
    key_latch_seen_ = std::move(kept);

    for (const KeyLatchSeen& seen : key_latch_seen_) {
        for (const Item& item : items) {
            if (item.key != seen.key)
                continue;
            KeyLatchChip chip;
            chip.text = item.text;
            chip.filled = item.filled;
            chips.push_back(std::move(chip));
            break;
        }
    }
}

} // namespace oa::app
