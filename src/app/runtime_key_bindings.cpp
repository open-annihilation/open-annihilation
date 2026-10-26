// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The key bindings' dispatcher. Each key, button and wheel is turned into
// action events after the input method has taken the keys it is composing.
// The events are kept. A running match answers them through
// handle_match_action. Touch and the pad keep their own paths.

#include "oa/app/runtime.hpp"

#include "oa/app/frame_coordinates.hpp"
#include "gui_actions.hpp"
#include "touch_dispatch.hpp"
#include "device_state.hpp"
#include "oa/app/input_hints.hpp"
#include "engine_settings_state.hpp"
#include "touch_state.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/match_console.hpp"
#include "oa/app/package_install.hpp"
#include "oa/app/package_install/oakeys.hpp"
#include "oa_layer.hpp"

#include "oa/formats/zip/stream.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/data/key_library/library.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/key_bindings/dispatcher.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"
#include "oa/ui/key_bindings/layout_file.hpp"
#include "oa/ui/key_editor/model.hpp"
#include "oa/ui/key_bindings/legacy.hpp"
#include "oa/ui/touch_hud.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
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
namespace install = oa::app::package_install;
namespace zip = oa::formats::zip;

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

/// The stem of a `gui.` action id, between the two dots.
///
/// @param id the action id
/// @return the stem, or empty when the id is not a gui action
std::string_view gui_id_stem(std::string_view id) {
    constexpr std::string_view prefix = "gui.";
    if (!id.starts_with(prefix))
        return {};
    const std::string_view rest = id.substr(prefix.size());
    const auto dot = rest.find('.');
    if (dot == std::string_view::npos)
        return rest;
    return rest.substr(0, dot);
}

/// Leaves the menus' letters to the GUI files on screen.
///
/// Every gathered file's letters start in the menus. A file that is not the one
/// on screen loses that context, so a letter answers the button the player sees.
///
/// @param[in,out] resolved the bindings just resolved
/// @param stems the file stems on screen, folded as a gui id's
void keep_current_menu_letters(keys::Resolved& resolved, const std::vector<std::string>& stems) {
    const uint32_t menus = context_bit("menus");
    if (menus == 0)
        return;
    for (keys::Bound& bound : resolved.actions) {
        if (!bound.id.starts_with("gui.") || (bound.contexts & menus) == 0)
            continue;
        const std::string_view stem = gui_id_stem(bound.id);
        bool shown = false;
        for (const std::string& current : stems) {
            if (stem == current) {
                shown = true;
                break;
            }
        }
        if (!shown)
            bound.contexts &= ~menus;
    }
}

/// The file stem of a GUI path, folded as a gui id's piece.
///
/// @param path a GUI file path
/// @return the stem
std::string menu_stem_of(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    const std::string_view file = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto dot = file.find('.');
    const std::string_view stem = dot == std::string_view::npos ? file : file.substr(0, dot);
    return gui_key_piece(stem);
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
/// The options dialog's three key sliders, in slider order.
constexpr std::array<std::string_view, 3> slider_action_ids{
    "build.snap-override", "build.line", "build.rotate"
};

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

/// Tells whether a name ends in `.oakeys`, in any case.
///
/// @param value the option's value
/// @return true when the folded name ends in `.oakeys`
bool ends_oakeys(const std::string& value) {
    const std::string name = folded(value);
    constexpr std::string_view extension = ".oakeys";
    return name.size() >= extension.size() &&
           name.compare(name.size() - extension.size(), extension.size(), extension) == 0;
}

/// Tells whether `--keys` names a path rather than an installed id.
///
/// A separator, an absolute path, a name that is already there, or a name
/// that ends in oakeys.yaml or `.oakeys` is a path. A missing `.oakeys`
/// file is still a path, so it fails as unreadable rather than as an id.
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
    if (ends_oakeys(value))
        return true;
    std::error_code exists_error;
    return fs::exists(path, exists_error) && !exists_error;
}

/// Reads bytes of a package for the streamed zip reader.
///
/// @param context the open file (`std::ifstream`)
/// @param offset where the read starts
/// @param bytes where the bytes go
/// @return true when every byte was read
bool read_package_at(void* context, uint64_t offset, std::span<uint8_t> bytes) {
    auto* input = static_cast<std::ifstream*>(context);
    if (input == nullptr)
        return false;
    if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
        return false;
    input->clear();
    input->seekg(static_cast<std::streamoff>(offset));
    if (!*input)
        return false;
    input->read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return input->gcount() == static_cast<std::streamsize>(bytes.size());
}

/// Reads a package's manifest entry into a manifest.
///
/// The kind's check already ran in open_package. This reads the same entry
/// again, in memory, so `--keys` can use the layout without installing it.
///
/// @param package the opened package
/// @param[out] manifest the manifest
/// @param[out] lines why it could not be read
/// @return true when the manifest was read
bool read_opened_manifest(
    const install::Package& package, oakeys::Manifest& manifest, std::vector<std::string>& lines
) {
    if (package.manifest_entry >= package.directory.entries.size()) {
        lines.push_back(utf8_of(package.file) + ": it cannot be read");
        return false;
    }
    std::ifstream input(package.file, std::ios::binary);
    if (!input) {
        lines.push_back(utf8_of(package.file) + ": it cannot be read");
        return false;
    }
    zip::SourceHooks source;
    source.context = &input;
    source.read_at = &read_package_at;
    std::vector<uint8_t> bytes;
    zip::ZipError error{};
    const bool read = zip::read_stream_entry(
        source,
        package.archive_bytes,
        package.directory.entries[package.manifest_entry],
        oa::formats::oamod::max_input_bytes,
        bytes,
        error
    );
    if (!read) {
        lines.push_back(utf8_of(package.file) + ": it cannot be read");
        return false;
    }
    std::vector<oakeys::Problem> problems;
    if (!oakeys::read_manifest(bytes, manifest, problems)) {
        if (problems.empty())
            lines.push_back(utf8_of(package.file) + ": it cannot be read");
        for (const oakeys::Problem& problem : problems)
            lines.push_back(oakeys::describe(problem));
        return false;
    }
    return true;
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

/// The chord texts one action lists, or none when the manifest does not name it.
///
/// @param manifest the own changes
/// @param action the action id
/// @return the texts, in file order
std::vector<std::string> binding_texts(const oakeys::Manifest& manifest, std::string_view action) {
    for (const oakeys::BindingEntry& entry : manifest.bindings) {
        if (entry.action != action)
            continue;
        std::vector<std::string> texts;
        texts.reserve(entry.chords.size());
        for (const oakeys::ChordText& chord : entry.chords)
            texts.push_back(chord.text);
        return texts;
    }
    return {};
}

/// Drops one action from an own manifest.
///
/// @param[in,out] manifest the own changes
/// @param action the action id
void remove_binding(oakeys::Manifest& manifest, std::string_view action) {
    std::vector<oakeys::BindingEntry> kept;
    kept.reserve(manifest.bindings.size());
    for (oakeys::BindingEntry& entry : manifest.bindings) {
        if (entry.action != action)
            kept.push_back(std::move(entry));
    }
    manifest.bindings = std::move(kept);
}

/// Parses chord texts a slider wrote.
///
/// @param texts the chord texts
/// @return the chords that parse
std::vector<keys::Chord> chords_of(const std::vector<std::string>& texts) {
    std::vector<keys::Chord> chords;
    chords.reserve(texts.size());
    for (const std::string& text : texts) {
        keys::ChordError error;
        if (const std::optional<keys::Chord> parsed = keys::parse_chord(text, error))
            chords.push_back(*parsed);
    }
    return chords;
}

/// Tells whether two manifests give the three key sliders the same chords.
///
/// @param left one manifest
/// @param right the other
/// @return true when each slider action lists the same texts
bool same_slider_bindings(const oakeys::Manifest& left, const oakeys::Manifest& right) {
    for (const std::string_view id : slider_action_ids) {
        if (binding_texts(left, id) != binding_texts(right, id))
            return false;
    }
    return true;
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

/// Whether two resolutions list the same action ids in the same order.
///
/// @param before the bindings in use
/// @param after the bindings just resolved
/// @return true when every id matches
bool same_action_ids(const keys::Resolved& before, const keys::Resolved& after) {
    if (before.actions.size() != after.actions.size())
        return false;
    for (std::size_t action_index = 0; action_index < before.actions.size(); ++action_index) {
        if (before.actions[action_index].id != after.actions[action_index].id)
            return false;
    }
    return true;
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
    const char* id{};
    oa::ui::touch_hud::Latch latch{};
    oa::ui::touch_hud::ActionClass action{};
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
    keys::Platform platform{};
    std::vector<keys::ActionEvent> events{};

    /// A key down slow keys have not admitted yet, kept so its press can reach the game once.
    struct HeldBack {
        bool held{};       ///< a down is waiting
        SDL_Event event{}; ///< the down, replayed when it is admitted
    };

    std::array<HeldBack, keys::scancode_limit> held_back{};
    std::vector<std::string> hacks{};
    keys::PointerScheme scheme{keys::PointerScheme::right_click};
    bool number_keys{};
    /// The order file stem the last resolution used.
    std::string order_stem{};
    /// The category letters the last resolution used. A match that loads units
    /// after the menu's resolution has to build the category actions.
    std::string categories{};
    /// The shown panel stem the last resolution used.
    std::string shown_stem{};
    /// The menu GUI stems the last resolution used.
    std::vector<std::string> menu_stems{};
    uint64_t map_generation{};
    bool ready{};
    /// Pointer keys were on when these bindings were resolved.
    bool pointer_keys_on{};
    /// The numpad pointer's directions and buttons.
    keys::PointerKeys pointer_keys{};
    /// Where the numpad pointer last was, in canvas pixels.
    float pointer_keys_x{};
    /// Where the numpad pointer last was, in canvas pixels.
    float pointer_keys_y{};
    /// pointer_keys_x and pointer_keys_y have been placed.
    bool pointer_keys_known{};
    /// The buttons the numpad pointer already has down.
    uint32_t pointer_keys_buttons{};
    keys::Base base{keys::Base::oa_default};
    keys::Layout chosen{};
    bool has_chosen{};
    keys::Layout own{};
    bool has_own{};
    /// What the last load read. A later call with the same text does no work.
    std::string signature{};
    /// The resolved layout's helpers.toggle, chosen then the player's own.
    std::vector<std::string> toggle{};
    /// helpers.hold-to-confirm, chosen then the player's own. Unset means every
    /// confirmable action. Set, even empty, means only those ids.
    std::optional<std::vector<std::string>> hold_to_confirm{};

    /// One press that must be held before self-destruct or Yes runs.
    struct HoldSession {
        keys::HoldConfirm hold{}; ///< the press
        std::string title{};      ///< the card's title, in English
        std::string chord{};      ///< the chord as display_text writes it
        bool click{false};        ///< the press was a click, so the card says the button
        bool button{false};       ///< the ring sits on a button
        bool swallow{false};      ///< this button's release must not activate it
        uint16_t scancode{0};     ///< the key that started a keyboard hold
        uint8_t mouse_button{0};  ///< the button that started a click hold
        std::size_t gadget{0};    ///< the button's gadget, when the ring sits on one
        bool clock_pinned{false}; ///< progress reads pinned_now, for a replayed fill
        uint64_t pinned_now{0};   ///< the clock replay_hold_confirm froze
        Runtime::ConfirmHoldKind kind{Runtime::ConfirmHoldKind::none}; ///< which action fires
    };

    HoldSession hold_session{};

    /// A mouse button held back until it is a click, a drag or the order wheel.
    struct OrderWheel {
        keys::WheelButton button{};       ///< the press
        bool tracking{};                  ///< the press is waiting on a decision
        uint8_t button_id{};              ///< the button that is waiting, or that opened the wheel
        SDL_Event down{};                 ///< the press, in the coordinates SDL delivered
        bool replay{};                    ///< a click or a drag is waiting to reach the game
        bool replay_drag{};               ///< the replay is a drag; otherwise a click
        bool replay_up{};                 ///< the replay includes the button coming up
        SDL_Event motion{};               ///< the motion that crossed the drag threshold
        bool mouse_open{};                ///< the wheel on screen was opened by this button
        std::optional<std::size_t> aim{}; ///< the wedge under the pointer, as a slot
        bool escape_up{};                 ///< Escape's key-up still belongs to the wheel
    };

    OrderWheel order_wheel{};

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
        for (const keys::Bound& bound : dispatcher.bindings().actions) {
            if (bound.id == action)
                return true;
        }
        return false;
    }

    /// The options dialog's three key sliders while that dialog is open.
    struct KeySliderEdit {
        bool open{};    ///< the dialog is open
        bool blocked{}; ///< the own file is there and cannot be read
        std::string base_id{};
        oakeys::Manifest original{};
        oakeys::Manifest last_written{};
        std::array<uint32_t, 3> shown{};
        std::array<bool, 3> noted{};
        std::array<uint32_t, 3> logged{};
    };

    KeySliderEdit key_sliders{};
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
        text += utf8_of(user_folder_);
        return text;
    };
    if (key_bindings_state_ != nullptr && key_bindings_state_->signature == signature_now())
        return;
    gui_quick_keys_gathered_ = false;

    const bool one_run = options_.keys_reset || !options_.keys.empty();
    const fs::path keys_folder = user_folder_ / std::string(library::keys_folder_name);
    const library::Listing listing = library::list_installed(keys_folder);

    keys::Base base = keys::Base::oa_default;
    keys::Layout chosen;
    bool has_chosen = false;
    std::string base_id = "oa-default";
    std::optional<std::vector<std::string>> chosen_toggle;
    std::optional<std::vector<std::string>> chosen_hold;

    const auto use_builtin = [&] {
        base = keys::Base::oa_default;
        has_chosen = false;
        base_id = "oa-default";
        chosen_toggle.reset();
        chosen_hold.reset();
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
        chosen_hold = manifest.helpers.hold_to_confirm;
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
        } else if (ends_oakeys(options_.keys)) {
            const install::PackageResult opened =
                install::open_package(path_from_utf8(options_.keys));
            if (!opened.package) {
                for (const std::string& line : opened.problem.lines)
                    log_keys(line);
                key_bindings_refusal_ = "--keys: " + options_.keys + ": " +
                                        install::oakeys::refusal_text(opened.problem);
                log_keys(key_bindings_refusal_);
            } else {
                oakeys::Manifest manifest;
                std::vector<std::string> lines;
                if (!read_opened_manifest(*opened.package, manifest, lines)) {
                    for (const std::string& line : lines)
                        log_keys(line);
                    key_bindings_refusal_ =
                        "--keys: " + options_.keys + ": the layout cannot be read";
                    log_keys(key_bindings_refusal_);
                } else {
                    use_manifest(manifest, true);
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
            if (!have_own_manifest && !own_broken)
                own_manifest = fresh_own(base_id);
            for (const std::string_view stored_action : slider_action_ids) {
                const auto imported =
                    view_rules::imported_key(stored_action, [&](std::string_view name) {
                        return read_number(view_rules::view_settings_section, name);
                    });
                if (!imported)
                    continue;
                const actions::Action* action = actions::find(stored_action);
                if (action == nullptr)
                    continue;
                if (have_own_manifest && names_action(own_manifest, stored_action)) {
                    log_keys(
                        std::string("skipped ") + std::string(stored_action) +
                        "; your changes already name it"
                    );
                    continue;
                }
                const std::optional<keys::Chord> chord =
                    keys::legacy_key_chord(imported->code, position_of(imported->code), action->by);
                if (!chord) {
                    log_keys(
                        std::string("skipped ") + std::string(imported->setting) +
                        "; it maps to no chord"
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
                        std::string("skipped ") + std::string(imported->setting) +
                        "; it is already the default"
                    );
                    continue;
                }
                if (own_broken) {
                    needed_write = true;
                    log_keys(
                        std::string("skipped ") + std::string(stored_action) +
                        "; your changes cannot be read"
                    );
                    continue;
                }
                const std::vector<keys::Chord> chords = keys::imported_list(*action, *chord);
                if (chords.empty()) {
                    log_keys(
                        std::string("skipped ") + std::string(imported->setting) +
                        "; it maps to no chord"
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
                set_binding(own_manifest, std::string(stored_action), chords);
                have_own_manifest = true;
                log_keys(
                    std::string("imported ") + std::string(stored_action) + " from " +
                    std::string(imported->setting) + " as " + shown
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
    std::optional<std::vector<std::string>> holds;
    if (chosen_hold)
        holds = *chosen_hold;
    if (apply_own && have_own_manifest && !own_broken && own_manifest.helpers.hold_to_confirm)
        holds = *own_manifest.helpers.hold_to_confirm;
    state.hold_to_confirm = std::move(holds);
    state.signature = signature_now();
    state.ready = false;
}

void Runtime::reload_key_layout_if_in_use(const fs::path& folder) {
    // The runtime keeps no separate listing of layouts. The layout in use is
    // the one this load chose. The signature does not include the folder's
    // files, so a match would skip the reread. Clearing it makes the next
    // load read the folder again. An update of the layout in use then plays
    // at once. Installing never writes the preference.
    if (key_bindings_state_ == nullptr || !key_bindings_state_->has_chosen)
        return;
    if (utf8_of(folder.filename()) != key_bindings_state_->chosen.id)
        return;
    key_bindings_state_->signature.clear();
    load_key_bindings();
}

std::string Runtime::bound_chord_text(std::string_view action_id) {
    ensure_key_bindings(false);
    if (key_bindings_state_ == nullptr)
        return {};
    keys::Chord held;
    const keys::Chord* chord = nullptr;
    if (key_bindings_state_->ready) {
        for (const keys::Bound& bound : key_bindings_state_->dispatcher.bindings().actions) {
            if (bound.id == action_id && !bound.chords.empty()) {
                chord = &bound.chords.front().chord;
                break;
            }
        }
    }
    if (chord == nullptr) {
        const actions::Action* action = actions::find(action_id);
        if (action == nullptr || action->defaults.empty())
            return {};
        keys::ChordError error;
        const std::optional<keys::Chord> parsed =
            keys::parse_chord(action->defaults.front(), error);
        if (!parsed)
            return {};
        held = *parsed;
        chord = &held;
    }
    return keys::display_text(
        *chord, key_bindings_state_->dispatcher.keyboard(), key_bindings_state_->platform
    );
}

void Runtime::fill_mod_option_keys(
    oa::ui::engine_settings::ModOptions& current, oa::ui::engine_settings::ModOptions& defaults
) {
    ensure_key_bindings(false);
    if (key_bindings_state_ == nullptr)
        return;
    KeyBindingsState& state = *key_bindings_state_;
    KeyBindingsState::KeySliderEdit edit{};
    edit.base_id = state.has_chosen && !state.chosen.id.empty() ? state.chosen.id : "oa-default";
    const fs::path keys_folder = user_folder_ / std::string(library::keys_folder_name);
    const fs::path own_file =
        keys_folder / library::own_folder_name(edit.base_id) / std::string(oakeys::manifest_file);
    std::error_code exists_error;
    if (fs::exists(own_file, exists_error) && !exists_error) {
        const ReadLayout read = read_layout_file(own_file);
        if (read.ok) {
            edit.original = read.manifest;
        } else {
            edit.blocked = true;
            edit.original = fresh_own(edit.base_id);
            log_keys(edit.base_id + ": your changes cannot be read");
        }
    } else {
        edit.original = fresh_own(edit.base_id);
    }
    edit.last_written = edit.original;
    std::array<uint32_t, 3> shown{};
    std::array<uint32_t, 3> factory{};
    for (std::size_t slot = 0; slot < slider_action_ids.size(); ++slot) {
        const std::string_view id = slider_action_ids[slot];
        std::vector<std::string> texts;
        if (state.ready) {
            for (const keys::Bound& bound : state.dispatcher.bindings().actions) {
                if (bound.id != id)
                    continue;
                for (const keys::BoundChord& item : bound.chords)
                    texts.push_back(keys::chord_text(item.chord));
                break;
            }
        }
        std::vector<std::string_view> chord_views;
        chord_views.reserve(texts.size());
        for (const std::string& text : texts)
            chord_views.emplace_back(text);
        shown[slot] = view_rules::slider_value(id, chord_views);
        const actions::Action* action = actions::find(id);
        factory[slot] = action != nullptr ? view_rules::slider_value(id, action->defaults)
                                          : oa::ui::engine_settings::option_keys[0].code;
    }
    edit.shown = shown;
    edit.open = true;
    state.key_sliders = std::move(edit);
    oa::ui::engine_settings::set_mod_option_keys(current, shown[0], shown[1], shown[2]);
    oa::ui::engine_settings::set_mod_option_keys(defaults, factory[0], factory[1], factory[2]);
}

void Runtime::take_mod_option_keys(const oa::ui::engine_settings::ModOptions& chosen) {
    if (key_bindings_state_ == nullptr || !key_bindings_state_->key_sliders.open)
        return;
    KeyBindingsState::KeySliderEdit& edit = key_bindings_state_->key_sliders;
    if (edit.blocked)
        return;
    const std::array<uint32_t, 3> chosen_codes = oa::ui::engine_settings::mod_option_keys(chosen);
    oakeys::Manifest editing = edit.original;
    for (std::size_t slot = 0; slot < slider_action_ids.size(); ++slot) {
        const std::string_view id = slider_action_ids[slot];
        std::vector<std::string> desired = binding_texts(edit.original, id);
        if (chosen_codes[slot] != edit.shown[slot]) {
            const std::vector<std::string> mapped =
                view_rules::slider_chords(id, chosen_codes[slot]);
            if (mapped.empty()) {
                desired = binding_texts(edit.last_written, id);
                if (!edit.noted[slot] || edit.logged[slot] != chosen_codes[slot]) {
                    const auto named = view_rules::imported_key(id, {});
                    if (named) {
                        log_keys(
                            std::string("skipped ") + std::string(named->setting) +
                            "; it maps to no chord"
                        );
                    }
                    edit.noted[slot] = true;
                    edit.logged[slot] = chosen_codes[slot];
                }
            } else {
                desired = mapped;
                edit.noted[slot] = false;
            }
        }
        if (desired.empty())
            remove_binding(editing, id);
        else if (binding_texts(editing, id) != desired)
            set_binding(editing, std::string(id), chords_of(desired));
    }
    if (same_slider_bindings(editing, edit.last_written))
        return;
    const fs::path keys_folder = user_folder_ / std::string(library::keys_folder_name);
    std::string error;
    if (!library::write_own(keys_folder, edit.base_id, editing, error)) {
        log_keys(edit.base_id + ": " + error);
        return;
    }
    edit.last_written = std::move(editing);
    key_bindings_state_->signature.clear();
    ensure_key_bindings(false);
}

void Runtime::restore_mod_option_keys() {
    if (key_bindings_state_ == nullptr)
        return;
    KeyBindingsState::KeySliderEdit& edit = key_bindings_state_->key_sliders;
    if (!edit.open)
        return;
    if (!edit.blocked && !same_slider_bindings(edit.last_written, edit.original)) {
        const fs::path keys_folder = user_folder_ / std::string(library::keys_folder_name);
        std::string error;
        if (library::write_own(keys_folder, edit.base_id, edit.original, error)) {
            edit.last_written = edit.original;
            key_bindings_state_->signature.clear();
            ensure_key_bindings(false);
        } else {
            log_keys(edit.base_id + ": " + error);
        }
    }
    if (key_bindings_state_ != nullptr)
        key_bindings_state_->key_sliders.open = false;
}

void Runtime::close_mod_option_keys() {
    if (key_bindings_state_ != nullptr)
        key_bindings_state_->key_sliders.open = false;
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
    keys::KeyboardMap loaded_map;
    const keys::KeyboardMap* map = &state.dispatcher.keyboard();
    bool map_changed = false;
    if (keymap_changed || state.map_generation == 0) {
        loaded_map = keyboard_from_sdl();
        map = &loaded_map;
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
    const bool pointer_keys = engine_settings().pointer_keys;
    const std::string order_stem = order_side_stem();
    const std::string shown_stem = shown_hud_stem();
    const std::string categories = loaded_category_letters();
    const bool bindings_same = state.ready && !map_changed && state.hacks == hacks &&
                               state.scheme == scheme && state.number_keys == number_keys &&
                               state.pointer_keys_on == pointer_keys &&
                               state.order_stem == order_stem && state.categories == categories;
    if (bindings_same && state.shown_stem == shown_stem && state.menu_stems == menu_gui_stems_)
        return;

    // The mod list is a local of the lambda. resolve copies what it keeps, and
    // the returned bindings do not point into it.
    const auto resolved_for = [&](std::string_view side_stem, std::string_view panel_stem) {
        if (!gui_quick_keys_gathered_)
            gather_gui_quick_keys();
        const keys::ModData mod = gui_mod_data(side_stem);
        keys::ResolveInput input;
        input.base = state.base;
        input.chosen = state.has_chosen ? &state.chosen : nullptr;
        input.own = state.has_own ? &state.own : nullptr;
        input.keyboard = map;
        input.platform = state.platform;
        input.hacks_on = hacks;
        input.scheme = scheme;
        input.number_keys_select_groups = number_keys;
        input.mod = &mod;
        keys::Resolved made = keys::resolve(input);
        apply_shown_gui_file(made, panel_stem);
        keep_current_menu_letters(made, menu_gui_stems_);
        return keys::pointer_keys_bindings(made, pointer_keys);
    };
    keys::Resolved resolved = resolved_for(order_stem, shown_stem);
    resolved.hold_to_confirm = state.hold_to_confirm;
    // A shown-file change keeps the same action ids, so a held page key, a
    // latch and a key held back stay. Anything else replaces the bindings.
    const bool keep_held = bindings_same && same_action_ids(state.dispatcher.bindings(), resolved);
    if (keep_held) {
        state.events.clear();
        state.dispatcher.retarget(std::move(resolved));
    } else {
        // The release events would point into the bindings set_bindings replaces.
        (void)state.dispatcher.release_all(SDL_GetTicks());
        state.events.clear();
        if (touch_state_if_made() != nullptr) {
            auto& hud_latches = touch_state().hud.latches;
            for (const KeyBindingsState::SharedPress& press : state.shared_presses)
                hud_latches.cancel(press.latch);
        }
        state.shared_presses.clear();
        if (map_changed)
            state.dispatcher.set_keyboard(loaded_map);
        state.dispatcher.set_platform(state.platform);
        state.dispatcher.set_bindings(std::move(resolved));
        std::vector<std::string> dispatcher_toggle;
        for (const std::string& id : state.toggle) {
            if (shared_role_of(id) == nullptr)
                dispatcher_toggle.push_back(id);
        }
        state.dispatcher.set_toggle(std::move(dispatcher_toggle));
        state.dispatcher.set_sticky_modifiers(engine_settings().sticky_modifiers);
        forget_held_back_keys();
        apply_key_timings();
        // The keys the dispatcher was holding are gone, and so is a direction
        // or a button the numpad pointer still had down.
        state.pointer_keys.clear();
    }
    state.hacks = std::move(hacks);
    state.scheme = scheme;
    state.number_keys = number_keys;
    state.pointer_keys_on = pointer_keys;
    state.order_stem = order_stem;
    state.categories = categories;
    state.shown_stem = shown_stem;
    state.menu_stems = menu_gui_stems_;
    state.ready = true;
    if (!keep_held)
        rebuild_pointer_roles(state.dispatcher.bindings());
}

SDL_Scancode Runtime::order_attack_press_scancode() {
    ensure_key_bindings(false);
    if (key_bindings_state_ == nullptr)
        return SDL_SCANCODE_UNKNOWN;
    for (const keys::Bound& bound : key_bindings_state_->dispatcher.bindings().actions) {
        if (bound.id != "order.attack")
            continue;
        for (const keys::BoundChord& binding : bound.chords) {
            const keys::Chord& chord = binding.chord;
            if (chord.gesture != keys::Gesture::press || chord.modifiers != 0)
                continue;
            if (chord.kind == keys::TriggerKind::place)
                return static_cast<SDL_Scancode>(chord.scancode);
            if (chord.kind == keys::TriggerKind::character && chord.character >= U'A' &&
                chord.character <= U'Z') {
                const auto offset = static_cast<int>(chord.character - U'A');
                return static_cast<SDL_Scancode>(static_cast<int>(SDL_SCANCODE_A) + offset);
            }
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}

keys::Contexts Runtime::input_contexts() {
    keys::Contexts contexts;
    contexts.active = context_bit("global");
    // A modal Open Annihilation screen takes the input, so the match and the
    // menus are not searched under it.
    const bool on_match = screen_ == Screen::match;
    const bool modal_layer = oa_layer().modal_shown(on_match);
    if (modal_layer) {
        contexts.active |= context_bit("oa-screens");
    } else if (on_match && match_ != nullptr) {
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
        if (whiteboard_on() && action_held("whiteboard.draw"))
            contexts.active |= context_bit("whiteboard");
        if (match_command_ == MatchCommand::build || pending_build_type_ != 0)
            contexts.active |= context_bit("placing");
        if (match_drag_.has_value())
            contexts.active |= context_bit("box");
    } else if (!on_match) {
        contexts.active |= context_bit("menus");
    }
    // A screen that shows without taking every key is searched too, so its own
    // keys still answer while a menu or the match has the rest.
    if (!modal_layer && oa_layer().shows(on_match))
        contexts.active |= context_bit("oa-screens");

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
    if (engine_settings().pointer_keys)
        contexts.conditions |= condition_bit("pointer-keys");
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

/// The slot a key's held-back down is kept in.
///
/// @param scancode the position
/// @return the slot; a position past the map shares the unused slot
uint16_t held_back_slot(uint16_t scancode) noexcept {
    return scancode < keys::scancode_limit ? scancode : uint16_t{0};
}

void Runtime::forget_held_back_keys() {
    if (key_bindings_state_ == nullptr)
        return;
    for (KeyBindingsState::HeldBack& slot : key_bindings_state_->held_back)
        slot = {};
}

int Runtime::recent_action_phase_count(std::string_view action, keys::Phase phase) const {
    if (key_bindings_state_ == nullptr)
        return 0;
    int count = 0;
    for (const keys::ActionEvent& action_event : key_bindings_state_->events)
        if (action_event.action == action && action_event.phase == phase)
            ++count;
    return count;
}

void Runtime::apply_key_timings() {
    namespace settings = oa::ui::engine_settings;
    const settings::EngineSettings current =
        engine_settings_ != nullptr ? engine_settings_->current : settings::EngineSettings{};
    set_mouse_double_click_time(current.double_press_ms);
    if (key_bindings_state_ == nullptr)
        return;
    keys::Timings timings;
    timings.slow_keys_ms = current.slow_keys_ms;
    timings.bounce_keys_ms = current.bounce_keys_ms;
    timings.double_press_ms = current.double_press_ms == 0 ? 500U : current.double_press_ms;
    timings.long_press_ms = current.long_press_ms;
    timings.repeat_delay_ms = current.repeat_delay_ms;
    timings.repeat_rate = current.repeat_rate;
    key_bindings_state_->dispatcher.set_timings(timings);
    const uint32_t double_press = current.double_press_ms == 0 ? 500U : current.double_press_ms;
    key_bindings_state_->pointer_keys.set_double_press_ms(double_press);
}

bool Runtime::selection_counting_down() const {
    if (match_ == nullptr)
        return false;
    const std::vector<uint16_t> ids = selected_local_ids();
    for (const uint16_t id : ids) {
        if (match_->self_destruct_remaining(id) > 0)
            return true;
    }
    return false;
}

bool Runtime::confirm_action_needs_hold(std::string_view id) {
    ensure_key_bindings(false);
    if (key_bindings_state_ == nullptr || !engine_settings().hold_to_confirm)
        return false;
    return keys::needs_hold(key_bindings_state_->dispatcher.bindings(), id, true);
}

bool Runtime::begin_confirm_hold(
    std::string_view id,
    ConfirmHoldKind kind,
    uint16_t scancode,
    bool from_button,
    uint8_t mouse_button,
    std::size_t gadget
) {
    if (kind == ConfirmHoldKind::self_destruct && selection_counting_down())
        return false;
    if (kind == ConfirmHoldKind::yes && !match_question_open())
        return false;
    if (!confirm_action_needs_hold(id))
        return false;
    KeyBindingsState& state = *key_bindings_state_;
    if (state.hold_session.hold.active())
        return true;
    keys::HoldConfirm::Target target;
    if (from_button && match_hud_ && gadget < match_hud_->layout.gadgets.size()) {
        const auto centre = hud_gadget_centre(gadget);
        const auto& common = match_hud_->layout.gadgets[gadget].common;
        const int width = std::max(
            1,
            static_cast<int>(std::lround(static_cast<double>(common.width) * match_layout_.scale))
        );
        const int height = std::max(
            1,
            static_cast<int>(std::lround(static_cast<double>(common.height) * match_layout_.scale))
        );
        target.button = true;
        target.width = width;
        target.height = height;
        target.x = centre.x - width / 2;
        target.y = centre.y - height / 2;
    } else {
        target.x = static_cast<int32_t>(std::lround(static_cast<double>(match_pointer_x_)));
        target.y = static_cast<int32_t>(std::lround(static_cast<double>(match_pointer_y_)));
    }
    KeyBindingsState::HoldSession& session = state.hold_session;
    session.hold.start(id, SDL_GetTicks(), target);
    session.clock_pinned = false;
    session.kind = kind;
    session.click = from_button;
    session.button = target.button;
    session.swallow = from_button;
    session.scancode = from_button ? uint16_t{0} : scancode;
    session.mouse_button = from_button ? mouse_button : uint8_t{0};
    session.gadget = gadget;
    session.title.clear();
    if (const oa::data::input_actions::Action* entry = actions::find(id))
        session.title.assign(entry->words.name.begin(), entry->words.name.end());
    else if (id == "gui.yesorno.choice1")
        session.title = "Yes";
    session.chord.clear();
    for (const keys::Bound& bound : state.dispatcher.bindings().actions) {
        if (bound.id != id || bound.chords.empty())
            continue;
        session.chord = keys::display_text(
            bound.chords.front().chord, state.dispatcher.keyboard(), state.platform
        );
        break;
    }
    return true;
}

bool Runtime::start_self_destruct_hold(
    uint16_t scancode, bool from_button, uint8_t mouse_button, std::size_t gadget
) {
    return begin_confirm_hold(
        "order.self-destruct",
        ConfirmHoldKind::self_destruct,
        scancode,
        from_button,
        mouse_button,
        gadget
    );
}

bool Runtime::start_yes_hold(
    uint16_t scancode, bool from_button, uint8_t mouse_button, std::size_t gadget
) {
    return begin_confirm_hold(
        "gui.yesorno.choice1", ConfirmHoldKind::yes, scancode, from_button, mouse_button, gadget
    );
}

void Runtime::cancel_hold_confirm() {
    hold_confirm_ate_release_ = false;
    if (key_bindings_state_ == nullptr)
        return;
    KeyBindingsState::HoldSession& session = key_bindings_state_->hold_session;
    session.clock_pinned = false;
    session.hold.cancel();
    session.kind = ConfirmHoldKind::none;
    session.swallow = false;
    session.click = false;
    session.button = false;
}

void Runtime::release_hold_confirm_key(uint16_t scancode) {
    if (key_bindings_state_ == nullptr)
        return;
    KeyBindingsState::HoldSession& session = key_bindings_state_->hold_session;
    if (session.kind == ConfirmHoldKind::none || session.click)
        return;
    if (session.scancode != scancode)
        return;
    session.hold.release();
}

void Runtime::release_hold_confirm_button(uint8_t button) {
    if (key_bindings_state_ == nullptr)
        return;
    KeyBindingsState::HoldSession& session = key_bindings_state_->hold_session;
    if (!session.click || session.mouse_button != button)
        return;
    if (session.swallow)
        hold_confirm_ate_release_ = true;
    session.swallow = false;
    session.hold.release();
}

bool Runtime::take_hold_confirm_release() {
    const bool ate = hold_confirm_ate_release_;
    hold_confirm_ate_release_ = false;
    return ate;
}

void Runtime::replay_hold_confirm(uint32_t elapsed_ms) {
    if (key_bindings_state_ == nullptr)
        return;
    KeyBindingsState::HoldSession& session = key_bindings_state_->hold_session;
    if (session.kind == ConfirmHoldKind::none)
        return;
    const std::string id = session.hold.id();
    const keys::HoldConfirm::Target target = session.hold.target();
    const uint64_t now = SDL_GetTicks();
    const uint64_t began = now >= elapsed_ms ? now - elapsed_ms : 0;
    session.hold.start(id, began, target);
    // The draw reads this clock, so a later millisecond does not move the fill.
    session.clock_pinned = true;
    session.pinned_now = now;
}

void Runtime::tick_hold_confirm() {
    if (key_bindings_state_ == nullptr)
        return;
    KeyBindingsState::HoldSession& session = key_bindings_state_->hold_session;
    if (session.kind == ConfirmHoldKind::none)
        return;
    session.clock_pinned = false;
    if (session.kind == ConfirmHoldKind::yes && !match_question_open()) {
        cancel_hold_confirm();
        return;
    }
    if (!engine_settings().hold_to_confirm) {
        cancel_hold_confirm();
        return;
    }
    const uint32_t long_press = key_bindings_state_->dispatcher.timings().long_press_ms;
    const keys::HoldState state = session.hold.tick(SDL_GetTicks(), long_press);
    if (state == keys::HoldState::none) {
        session.kind = ConfirmHoldKind::none;
        return;
    }
    if (state != keys::HoldState::done)
        return;
    const auto kind = session.kind;
    const std::size_t gadget = session.gadget;
    const bool swallow = session.swallow;
    session.hold.cancel();
    session.kind = ConfirmHoldKind::none;
    session.click = false;
    session.button = false;
    session.swallow = false;
    // The button is still down. Its release must not activate.
    if (swallow)
        hold_confirm_ate_release_ = true;
    if (kind == ConfirmHoldKind::self_destruct && match_ != nullptr) {
        const std::vector<uint16_t> ids = selected_local_ids();
        match_->toggle_self_destruct(ids);
        status_ = "Self-destruct";
    } else if (kind == ConfirmHoldKind::yes)
        activate_match_hud(gadget);
}

Runtime::HoldConfirmDraw Runtime::hold_confirm_draw() const {
    HoldConfirmDraw draw;
    if (key_bindings_state_ == nullptr)
        return draw;
    const KeyBindingsState::HoldSession& session = key_bindings_state_->hold_session;
    if (!session.hold.active())
        return draw;
    const keys::HoldConfirm::Target& target = session.hold.target();
    draw.show = true;
    draw.button = session.button;
    draw.click = session.click;
    draw.x = target.x;
    draw.y = target.y;
    draw.width = target.width;
    draw.height = target.height;
    const uint64_t now = session.clock_pinned ? session.pinned_now : SDL_GetTicks();
    draw.progress =
        session.hold.progress(now, key_bindings_state_->dispatcher.timings().long_press_ms);
    draw.long_press_ms = key_bindings_state_->dispatcher.timings().long_press_ms;
    draw.gadget = session.gadget;
    draw.title = session.title;
    draw.chord = session.chord;
    return draw;
}

namespace {

/// A motion at the same window point as a button event.
///
/// @param event a motion, or the button event whose point crossed the threshold
/// @return the motion
SDL_Event motion_at(const SDL_Event& event) {
    if (event.type == SDL_EVENT_MOUSE_MOTION)
        return event;
    const float x = event.button.x;
    const float y = event.button.y;
    const SDL_WindowID window = event.button.windowID;
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.windowID = window;
    motion.motion.x = x;
    motion.motion.y = y;
    return motion;
}

} // namespace

uint8_t Runtime::order_wheel_button() const {
    if (engine_settings_ == nullptr)
        return 0;
    switch (engine_settings_->current.order_wheel) {
    case oa::ui::engine_settings::OrderWheel::hold_right:
        return SDL_BUTTON_RIGHT;
    case oa::ui::engine_settings::OrderWheel::hold_middle:
        return SDL_BUTTON_MIDDLE;
    case oa::ui::engine_settings::OrderWheel::off:
        break;
    }
    return 0;
}

bool Runtime::order_wheel_canvas(const SDL_Event& event, float& x, float& y) const {
    SDL_Event frame = event;
    if (sdl_.renderer != nullptr && !convert_event_to_frame(sdl_.renderer, frame))
        return false;
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        x = frame.motion.x;
        y = frame.motion.y;
    } else {
        x = frame.button.x;
        y = frame.button.y;
    }
    return true;
}

bool Runtime::order_wheel_mouse_open() const {
    return key_bindings_state_ != nullptr && key_bindings_state_->order_wheel.mouse_open;
}

std::optional<std::size_t> Runtime::order_wheel_mouse_aim() const {
    if (key_bindings_state_ == nullptr)
        return std::nullopt;
    return key_bindings_state_->order_wheel.aim;
}

bool Runtime::order_wheel_press_eligible(float x, float y) {
    if (screen_ != Screen::match || match_ == nullptr || match_finished_ || match_paused_)
        return false;
    if ((current_extension_state() & extension_state::local_watcher) != 0)
        return false;
    if (match_command_ == MatchCommand::build || pending_build_type_ != 0)
        return false;
    if (megamap_shown() || megamap_open_)
        return false;
    if (whiteboard_input_.drawing || whiteboard_input_.erasing || whiteboard_input_.moving >= 0 ||
        (whiteboard_on() && action_held("whiteboard.draw")))
        return false;
    if (match_->commander_placement() != oa::sim::match_runtime::CommanderPlacement::none)
        return false;
    if (resource_panel_.dragging || order_drag_unit_ != 0 || build_queue_drag_.has_value())
        return false;
    if (!battlefield_contains(x, y) || radar_contains(x, y) || placed_hud_covers(x, y))
        return false;
    if (unit_info_panel_.has_value() && unit_info_point(x, y))
        return false;
    update_pointer(x, y);
    return !hovered_.has_value();
}

void Runtime::ensure_order_wheel_viewport() {
    auto& state = touch_state();
    const bool filling = touch_controls_active() || state.hud.pad.hud;
    if (filling && state.viewport.width > 0 && state.viewport.height > 0)
        return;
    const auto& layout = match_layout_;
    oa::ui::touch_hud::Viewport viewport{};
    viewport.width = layout.width;
    viewport.height = layout.height;
    viewport.px_per_point =
        (layout.px_per_point > 0.0 ? static_cast<float>(layout.px_per_point) : 1.0F) *
        touch_control_scale();
    viewport.safe = layout.safe;
    viewport.device = oa::ui::touch_hud::DeviceClass::tablet;
    viewport.left_handed = false;
    viewport.chrome = layout;
    state.viewport = viewport;
}

void Runtime::open_mouse_order_wheel() {
    if (key_bindings_state_ == nullptr)
        return;
    auto& wheel = key_bindings_state_->order_wheel;
    float x = 0.0F;
    float y = 0.0F;
    if (!order_wheel_canvas(wheel.down, x, y))
        return;
    ensure_order_wheel_viewport();
    TouchDispatchAccess::open_radial(*this, x, y);
    if (touch_ == nullptr || !touch_->hud.radial)
        return;
    wheel.mouse_open = true;
    wheel.aim.reset();
    ++touch_->hud.revision;
    // The hold took the middle button, so a pan that was running stops and
    // this press does not start another.
    if (wheel.button_id == SDL_BUTTON_MIDDLE)
        middle_pan_ = {};
}

void Runtime::close_mouse_order_wheel() {
    if (key_bindings_state_ == nullptr || !key_bindings_state_->order_wheel.mouse_open)
        return;
    if (touch_ != nullptr && touch_->hud.radial)
        std::ignore = TouchDispatchAccess::close_overlay(*this);
    key_bindings_state_->order_wheel.mouse_open = false;
    key_bindings_state_->order_wheel.aim.reset();
    if (touch_ != nullptr)
        ++touch_->hud.revision;
}

void Runtime::cancel_order_wheel() {
    if (key_bindings_state_ == nullptr)
        return;
    auto& wheel = key_bindings_state_->order_wheel;
    wheel.button.cancel();
    wheel.tracking = false;
    wheel.replay = false;
    wheel.replay_drag = false;
    wheel.replay_up = false;
    wheel.escape_up = false;
    close_mouse_order_wheel();
}

void Runtime::aim_mouse_order_wheel(const SDL_Event& event) {
    if (touch_ == nullptr || !touch_->hud.radial || key_bindings_state_ == nullptr)
        return;
    float x = 0.0F;
    float y = 0.0F;
    if (!order_wheel_canvas(event, x, y))
        return;
    const oa::ui::touch_hud::Point at{
        static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))
    };
    const auto hit = oa::ui::touch_hud::radial_hit(*touch_->hud.radial, at, touch_->viewport);
    std::optional<std::size_t> aimed;
    if (hit) {
        const auto& wedges = touch_->hud.radial->wedges;
        for (std::size_t index = 0; index < wedges.size(); ++index) {
            if (wedges[index].item == *hit) {
                aimed = index;
                break;
            }
        }
    }
    if (aimed == key_bindings_state_->order_wheel.aim)
        return;
    key_bindings_state_->order_wheel.aim = aimed;
    ++touch_->hud.revision;
}

void Runtime::release_mouse_order_wheel(const SDL_Event& event) {
    if (touch_ == nullptr || !touch_->hud.radial || key_bindings_state_ == nullptr) {
        close_mouse_order_wheel();
        return;
    }
    float x = 0.0F;
    float y = 0.0F;
    if (!order_wheel_canvas(event, x, y)) {
        close_mouse_order_wheel();
        return;
    }
    const oa::ui::touch_hud::Point at{
        static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))
    };
    const auto hit = oa::ui::touch_hud::radial_hit(*touch_->hud.radial, at, touch_->viewport);
    if (hit) {
        TouchDispatchAccess::radial_pick(*this, *hit);
        // A greyed wedge leaves the wheel open.
        if (touch_->hud.radial)
            return;
        key_bindings_state_->order_wheel.mouse_open = false;
        key_bindings_state_->order_wheel.aim.reset();
        ++touch_->hud.revision;
        return;
    }
    close_mouse_order_wheel();
}

bool Runtime::order_wheel_decide(const SDL_Event& event) {
    auto& wheel = key_bindings_state_->order_wheel;
    const uint32_t long_press =
        engine_settings_ != nullptr ? engine_settings_->current.long_press_ms : 500U;
    const auto decision =
        wheel.button.tick(SDL_GetTicks(), long_press, static_cast<float>(drag_click_pixels()));
    if (decision == keys::WheelDecision::waiting)
        return true;
    wheel.tracking = false;
    if (decision == keys::WheelDecision::open_wheel) {
        open_mouse_order_wheel();
        return true;
    }
    wheel.replay = true;
    wheel.replay_drag = decision == keys::WheelDecision::pass_drag;
    wheel.replay_up =
        decision == keys::WheelDecision::pass_click || event.type == SDL_EVENT_MOUSE_BUTTON_UP;
    if (wheel.replay_drag)
        wheel.motion = motion_at(event);
    return true;
}

bool Runtime::take_order_wheel_aside(const SDL_Event& event) {
    if (key_bindings_state_ == nullptr)
        return false;
    auto& wheel = key_bindings_state_->order_wheel;
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE &&
        (wheel.mouse_open || wheel.escape_up)) {
        if (wheel.mouse_open)
            close_mouse_order_wheel();
        wheel.escape_up = true;
        return true;
    }
    if (event.type == SDL_EVENT_KEY_UP && event.key.key == SDLK_ESCAPE && wheel.escape_up) {
        wheel.escape_up = false;
        return true;
    }
    if (event.type != SDL_EVENT_MOUSE_MOTION)
        return false;
    if (wheel.mouse_open) {
        aim_mouse_order_wheel(event);
        return true;
    }
    if (!wheel.tracking)
        return false;
    float x = 0.0F;
    float y = 0.0F;
    if (!order_wheel_canvas(event, x, y))
        return true;
    wheel.button.motion({x, y});
    return order_wheel_decide(event);
}

bool Runtime::take_order_wheel_button(const SDL_Event& event) {
    if (key_bindings_state_ == nullptr)
        return false;
    if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN && event.type != SDL_EVENT_MOUSE_BUTTON_UP)
        return false;
    auto& wheel = key_bindings_state_->order_wheel;
    if (wheel.mouse_open) {
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == wheel.button_id)
            release_mouse_order_wheel(event);
        return true;
    }
    const uint8_t watched = order_wheel_button();
    if (watched == 0 || event.button.button != watched)
        return false;
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        float x = 0.0F;
        float y = 0.0F;
        if (!order_wheel_canvas(event, x, y) || !order_wheel_press_eligible(x, y))
            return false;
        wheel.button.press({x, y}, SDL_GetTicks());
        wheel.tracking = true;
        wheel.button_id = watched;
        wheel.down = event;
        wheel.replay = false;
        return true;
    }
    if (!wheel.tracking)
        return false;
    float x = 0.0F;
    float y = 0.0F;
    if (!order_wheel_canvas(event, x, y)) {
        wheel.button.cancel();
        wheel.tracking = false;
        return false;
    }
    wheel.button.release({x, y});
    return order_wheel_decide(event);
}

void Runtime::advance_order_wheel() {
    if (key_bindings_state_ == nullptr || !key_bindings_state_->order_wheel.tracking)
        return;
    auto& wheel = key_bindings_state_->order_wheel;
    const uint32_t long_press =
        engine_settings_ != nullptr ? engine_settings_->current.long_press_ms : 500U;
    const auto decision =
        wheel.button.tick(SDL_GetTicks(), long_press, static_cast<float>(drag_click_pixels()));
    if (decision == keys::WheelDecision::waiting)
        return;
    wheel.tracking = false;
    if (decision == keys::WheelDecision::open_wheel)
        open_mouse_order_wheel();
}

void Runtime::replay_order_wheel(bool& running) {
    if (key_bindings_state_ == nullptr || !key_bindings_state_->order_wheel.replay)
        return;
    auto& wheel = key_bindings_state_->order_wheel;
    wheel.replay = false;
    const bool drag = wheel.replay_drag;
    const bool release = wheel.replay_up;
    // Kept in window coordinates. dispatch_after_actions converts the copy it is given.
    const SDL_Event down_at = wheel.down;
    const SDL_Event motion_at_event = wheel.motion;
    wheel.replay_drag = false;
    wheel.replay_up = false;
    SDL_Event down = down_at;
    dispatch_after_actions(down, running);
    if (!running)
        return;
    if (drag) {
        SDL_Event motion = motion_at_event;
        dispatch_after_actions(motion, running);
        if (!running || !release)
            return;
        SDL_Event up = down_at;
        up.type = SDL_EVENT_MOUSE_BUTTON_UP;
        up.button.down = false;
        up.button.x = motion_at_event.motion.x;
        up.button.y = motion_at_event.motion.y;
        dispatch_after_actions(up, running);
        return;
    }
    if (release) {
        SDL_Event up = down_at;
        up.type = SDL_EVENT_MOUSE_BUTTON_UP;
        up.button.down = false;
        dispatch_after_actions(up, running);
    }
}

bool Runtime::feed_input_actions(const SDL_Event& event) {
    // A capture or a clash prompt takes the key itself. Nothing is fed, so no
    // action fires, and a key-up still reaches the prompt (Escape cancels).
    if (key_editor_capturing()) {
        if (event.type == SDL_EVENT_KEY_UP)
            key_editor_capture_event(event);
        return false;
    }
    const bool keymap_changed = event.type == SDL_EVENT_KEYMAP_CHANGED;
    ensure_key_bindings(keymap_changed);
    KeyBindingsState& state = *key_bindings_state_;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        state.events = state.dispatcher.release_all(SDL_GetTicks()).events;
        note_pointer_key_events(state.events);
        drop_pointer_keys();
        if (touch_state_if_made() != nullptr)
            touch_state().hud.latches.clear();
        state.shared_presses.clear();
        sticky_event_ = false;
        sticky_event_mods_ = 0;
        key_latch_seen_.clear();
        key_latch_chips_drawn_.clear();
        cancel_hold_confirm();
        cancel_order_wheel();
        refresh_pointer_modifiers();
        trace_fed_actions(state.events);
        forget_held_back_keys();
        return false;
    }
    // Motion and Escape never become an input action. A wheel the mouse opened, or a press it is
    // holding, takes them here.
    if (take_order_wheel_aside(event))
        return true;
    const std::optional<keys::InputEvent> input = input_from_sdl(event, SDL_GetTicks());
    if (!input)
        return false;
    const keys::Contexts contexts = input_contexts();
    state.dispatcher.set_contexts(contexts);
    const keys::Fed fed = state.dispatcher.feed(*input);
    state.events = fed.events;
    note_pointer_key_events(state.events);
    trace_fed_actions(state.events);
    if (fed.legacy == keys::LegacyGate::hold && event.type == SDL_EVENT_KEY_DOWN) {
        KeyBindingsState::HeldBack& slot = state.held_back[held_back_slot(input->scancode)];
        slot.held = true;
        slot.event = event;
        return true;
    }
    if (fed.legacy == keys::LegacyGate::drop) {
        if (!input->down)
            state.held_back[held_back_slot(input->scancode)] = {};
        return true;
    }
    // The bindings accepted this event. A key held back or dropped returned above.
    apply_shared_key_latches(*input, contexts);
    // The dispatcher has seen this button. The order wheel may hold it back.
    if (take_order_wheel_button(event))
        return true;
    return false;
}

void Runtime::advance_input_actions(bool& running) {
    // A hold that started before the prompt must not fire while it is open.
    if (key_editor_capturing())
        return;
    ensure_key_bindings(false);
    KeyBindingsState& state = *key_bindings_state_;
    const keys::Contexts contexts = input_contexts();
    state.dispatcher.set_contexts(contexts);
    const keys::Fed fed = state.dispatcher.advance(SDL_GetTicks());
    // An empty frame leaves the last event's action events in place, and is
    // not printed again.
    if (!fed.events.empty()) {
        state.events = fed.events;
        note_pointer_key_events(fed.events);
        trace_fed_actions(state.events);
    }
    step_pointer_keys();
    for (const keys::InputEvent& admitted : fed.admitted) {
        // The down was held back, so the shared latches did not see it then.
        apply_shared_key_latches(admitted, contexts);
        KeyBindingsState::HeldBack& slot = state.held_back[held_back_slot(admitted.scancode)];
        if (!slot.held)
            continue;
        SDL_Event replay = slot.event;
        slot = {};
        // The dispatcher admitted this down just now and its actions are the
        // frame's events. The hotkey handler must not feed it again: a second
        // down would be held back once more.
        match_key_already_fed_ = true;
        try {
            dispatch_after_actions(replay, running);
        } catch (...) {
            match_key_already_fed_ = false;
            throw;
        }
        match_key_already_fed_ = false;
    }
    advance_order_wheel();
}

std::span<const keys::ActionEvent> Runtime::match_action_events() const {
    if (key_bindings_state_ == nullptr)
        return {};
    return key_bindings_state_->events;
}

const keys::ActionEvent* Runtime::fired_action(std::string_view id) const {
    if (key_bindings_state_ == nullptr)
        return nullptr;
    for (const keys::ActionEvent& fed : key_bindings_state_->events) {
        if (fed.action != id)
            continue;
        if (fed.phase == keys::Phase::pressed || fed.phase == keys::Phase::repeated)
            return &fed;
    }
    return nullptr;
}

std::string Runtime::screen_action_for_key(uint32_t key, uint16_t sdl_mods) const {
    if (key_bindings_state_ != nullptr) {
        for (const keys::ActionEvent& fed : key_bindings_state_->events) {
            if (!fed.action.starts_with("ui.screen-"))
                continue;
            if (fed.phase != keys::Phase::pressed && fed.phase != keys::Phase::repeated)
                continue;
            return std::string(fed.action);
        }
    }
    if (key_bindings_state_ == nullptr || !key_bindings_state_->ready)
        return {};
    keys::InputEvent input;
    input.kind = keys::InputKind::key;
    input.down = true;
    input.key_code = key;
    input.modifiers = binding_modifiers(static_cast<SDL_Keymod>(sdl_mods));
    const keys::Resolved& resolved = key_bindings_state_->dispatcher.bindings();
    const keys::KeyboardMap& keyboard = key_bindings_state_->dispatcher.keyboard();
    const keys::Platform where = key_bindings_state_->platform;
    std::string best;
    int best_rank = -1;
    for (const keys::Bound& bound : resolved.actions) {
        if (!bound.id.starts_with("ui.screen-"))
            continue;
        for (const keys::BoundChord& binding : bound.chords) {
            if (keys::match(binding.chord, input, keyboard, where) == keys::Match::none)
                continue;
            const int ranked = keys::rank(binding.chord);
            if (ranked > best_rank) {
                best_rank = ranked;
                best = bound.id;
            }
        }
    }
    return best;
}

void Runtime::set_menu_gui(std::string_view path) {
    menu_gui_stems_.clear();
    add_menu_gui(path);
}

void Runtime::add_menu_gui(std::string_view path) {
    if (path.empty())
        return;
    std::string stem = menu_stem_of(path);
    if (stem.empty())
        return;
    for (const std::string& have : menu_gui_stems_) {
        if (have == stem)
            return;
    }
    menu_gui_stems_.push_back(std::move(stem));
}

bool Runtime::action_key_held(std::string_view action) {
    ensure_key_bindings(false);
    keys::HeldKeys keys_held;
    keys_held.down = key_is_down;
    return key_bindings_state_->dispatcher.chord_key_held(action, keys_held);
}

bool Runtime::action_held(std::string_view action) const {
    // Filling the binding cache does not change a key the player set.
    const_cast<Runtime*>(this)->ensure_key_bindings(false);
    if (key_bindings_state_ == nullptr || !key_bindings_state_->ready)
        return false;
    keys::HeldKeys keys_held;
    keys_held.down = key_is_down;
    if (key_bindings_state_->dispatcher.chord_key_held(action, keys_held))
        return true;
    return modifier_role_active(action);
}

bool Runtime::action_button_matches(
    std::string_view action, uint8_t button, keys::Gesture gesture
) {
    ensure_key_bindings(false);
    if (key_bindings_state_ == nullptr || !key_bindings_state_->ready || button == 0)
        return false;
    const keys::Bound* found = nullptr;
    for (const keys::Bound& bound : key_bindings_state_->dispatcher.bindings().actions) {
        if (bound.id == action) {
            found = &bound;
            break;
        }
    }
    if (found == nullptr)
        return false;
    // A chord numbers MouseLeft, MouseRight and MouseMiddle as 1, 2 and 3.
    // SDL numbers the right and middle buttons 3 and 2. The caller passes
    // SDL's number.
    uint8_t chord_button = button;
    if (button == SDL_BUTTON_RIGHT)
        chord_button = 2;
    else if (button == SDL_BUTTON_MIDDLE)
        chord_button = 3;
    keys::InputEvent input;
    input.kind = keys::InputKind::button;
    input.down = true;
    input.button = chord_button;
    input.clicks = gesture == keys::Gesture::double_press ? 2 : 1;
    input.modifiers = binding_modifiers(device_state::modifiers_held());
    const keys::KeyboardMap& map = key_bindings_state_->dispatcher.keyboard();
    const keys::Platform where = key_bindings_state_->platform;
    for (const keys::BoundChord& item : found->chords) {
        if (item.chord.gesture != gesture)
            continue;
        if (keys::match(item.chord, input, map, where) != keys::Match::none)
            return true;
    }
    return false;
}

bool Runtime::action_key_matches(std::string_view action, const SDL_KeyboardEvent& key) {
    ensure_key_bindings(false);
    if (key_bindings_state_ == nullptr || !key_bindings_state_->ready)
        return false;
    const keys::Bound* found = nullptr;
    for (const keys::Bound& bound : key_bindings_state_->dispatcher.bindings().actions) {
        if (bound.id == action) {
            found = &bound;
            break;
        }
    }
    if (found == nullptr)
        return false;
    keys::InputEvent input;
    input.kind = keys::InputKind::key;
    input.down = key.down;
    input.repeat = key.repeat;
    const auto scan = static_cast<uint32_t>(key.scancode);
    input.scancode = scan < keys::scancode_limit ? static_cast<uint16_t>(scan) : uint16_t{0};
    input.key_code = static_cast<uint32_t>(key.key);
    input.modifiers = binding_modifiers(key.mod);
    return binding_key(
        *found,
        input,
        modifier_bit_of(input.scancode),
        key_bindings_state_->dispatcher.keyboard(),
        key_bindings_state_->platform
    );
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
        for (const keys::Bound& item : state.dispatcher.bindings().actions) {
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
        if (!binding_key(*bound, input, bit, state.dispatcher.keyboard(), state.platform))
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

void Runtime::note_pointer_key_events(const std::vector<keys::ActionEvent>& events) {
    if (key_bindings_state_ == nullptr || !engine_settings().pointer_keys)
        return;
    KeyBindingsState& state = *key_bindings_state_;
    const uint64_t now = SDL_GetTicks();
    for (const keys::ActionEvent& action_event : events) {
        const std::string_view id = action_event.action;
        const bool pointer_action = id.starts_with("pointer.move-") ||
                                    id.starts_with("pointer.click-") || id == "pointer.hold-left" ||
                                    id == "pointer.let-go";
        if (!pointer_action)
            continue;
        if (action_event.phase == keys::Phase::pressed)
            state.pointer_keys.press(id, now);
        else if (action_event.phase == keys::Phase::released)
            state.pointer_keys.release(id, now);
    }
}

void Runtime::step_pointer_keys() {
    if (key_bindings_state_ == nullptr || !engine_settings().pointer_keys)
        return;
    KeyBindingsState& state = *key_bindings_state_;
    const uint32_t window =
        engine_settings().double_press_ms == 0 ? 500U : engine_settings().double_press_ms;
    state.pointer_keys.set_double_press_ms(window);
    if (input_contexts().text_entry) {
        const uint64_t now = SDL_GetTicks();
        for (const char* id : {
                 "pointer.move-up",
                 "pointer.move-down",
                 "pointer.move-left",
                 "pointer.move-right",
                 "pointer.move-up-left",
                 "pointer.move-up-right",
                 "pointer.move-down-left",
                 "pointer.move-down-right",
             })
            state.pointer_keys.release(id, now);
    }
    const keys::PointerStep moved = state.pointer_keys.step(SDL_GetTicks());
    emit_pointer_keys_step(
        moved,
        state.pointer_keys_x,
        state.pointer_keys_y,
        state.pointer_keys_known,
        state.pointer_keys_buttons
    );
}

void Runtime::drop_pointer_keys() {
    if (key_bindings_state_ == nullptr)
        return;
    KeyBindingsState& state = *key_bindings_state_;
    state.pointer_keys.clear();
    for (int attempt = 0; attempt < 4; ++attempt) {
        const keys::PointerStep moved = state.pointer_keys.step(SDL_GetTicks());
        const bool quiet = moved.dx == 0.F && moved.dy == 0.F && !moved.down && !moved.up;
        emit_pointer_keys_step(
            moved,
            state.pointer_keys_x,
            state.pointer_keys_y,
            state.pointer_keys_known,
            state.pointer_keys_buttons
        );
        if (quiet)
            break;
    }
}

void Runtime::apply_pointer_keys(bool on) {
    ensure_key_bindings(false);
    if (!on)
        drop_pointer_keys();
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

namespace {

namespace editor = oa::ui::key_editor;

/// True when an id can be one path segment of the Keys folder.
///
/// @param id the layout id
/// @return false for an empty id, a dot segment, an own folder, or a separator
bool safe_layout_id(std::string_view id) {
    if (id.empty() || id == "." || id == ".." || id.starts_with("own-"))
        return false;
    return id.find_first_of("/\\:") == std::string_view::npos;
}

/// Writes one installed layout's manifest into Keys/<id>/oakeys.yaml.
///
/// @param keys_folder the Keys folder
/// @param manifest the layout
/// @param[out] problem why it was not written
/// @return true when the file was written
bool write_installed_layout(
    const fs::path& keys_folder, const oakeys::Manifest& manifest, std::string& problem
) {
    if (!safe_layout_id(manifest.id)) {
        problem = "the layout's id is refused";
        return false;
    }
    const fs::path folder = keys_folder / manifest.id;
    std::error_code mkdir_error;
    fs::create_directories(folder, mkdir_error);
    if (mkdir_error) {
        problem = "the layout's folder was not made";
        return false;
    }
    const std::string text = oakeys::write_manifest(manifest);
    const fs::path file = folder / std::string(oakeys::manifest_file);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        problem = "the layout was not written";
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        problem = "the layout was not written";
        return false;
    }
    return true;
}

/// One installed layout the editor can show, from its manifest.
///
/// @param manifest the manifest
/// @param[out] summary the summary, when the layout can be used
/// @return false when the layout's chords do not parse
bool summary_from_manifest(const oakeys::Manifest& manifest, editor::LayoutSummary& summary) {
    keys::Layout layout;
    if (!use_layout(manifest, layout))
        return false;
    summary.id = manifest.id;
    summary.name = manifest.name;
    summary.author = manifest.author.name;
    summary.version = oa::formats::oamut::version_text(manifest.version);
    summary.helpers = manifest.helpers;
    summary.layout = std::move(layout);
    return true;
}

} // namespace

void Runtime::fill_key_editor(editor::EditorInputs& inputs) {
    ensure_key_bindings(false);
    if (key_bindings_state_ == nullptr)
        return;
    const KeyBindingsState& state = *key_bindings_state_;
    inputs.base = state.base;
    inputs.keyboard = state.dispatcher.keyboard();
    inputs.platform = state.platform;
    inputs.hacks_on = state.hacks;
    inputs.scheme = state.scheme;
    inputs.number_keys_select_groups = engine_settings().switch_alt;
    inputs.developer_mode = developer_mode();
    inputs.hold_to_confirm_setting = engine_settings().hold_to_confirm;
    inputs.mod = gui_mod_data(state.order_stem);
    inputs.mod.categories = state.categories;
    if (const oa::data::mod_profile::ModProfile* profile = mod_profile()) {
        if (!profile->name.empty())
            inputs.mod_name = profile->name;
    }
    inputs.text.shown = [](void*, std::string_view english) {
        return std::string(oa::data::languages::interface_text(english));
    };

    const bool one_run = options_.keys_reset || !options_.keys.empty();
    const std::string base_id =
        state.has_chosen && !state.chosen.id.empty() ? state.chosen.id : std::string("oa-default");
    const fs::path keys_folder = user_folder_ / std::string(library::keys_folder_name);
    if (!one_run) {
        const fs::path own_file =
            keys_folder / library::own_folder_name(base_id) / std::string(oakeys::manifest_file);
        std::error_code own_error;
        if (fs::exists(own_file, own_error) && !own_error) {
            const ReadLayout read = read_layout_file(own_file);
            keys::Layout own_layout;
            if (read.ok && use_layout(read.manifest, own_layout)) {
                inputs.own = std::move(own_layout);
                inputs.helpers = read.manifest.helpers;
            }
        }
    }

    const library::Listing listing = library::list_installed(keys_folder);
    bool chosen_listed = false;
    for (const library::Installed& installed : listing.layouts) {
        if (installed.origin == library::Origin::own || installed.manifest.id.starts_with("own-"))
            continue;
        editor::LayoutSummary summary;
        if (!summary_from_manifest(installed.manifest, summary))
            continue;
        if (state.has_chosen && summary.id == state.chosen.id) {
            inputs.chosen = summary;
            inputs.has_chosen = true;
            chosen_listed = true;
        }
        inputs.installed.push_back(std::move(summary));
    }
    if (state.has_chosen && !chosen_listed) {
        editor::LayoutSummary summary;
        summary.id = state.chosen.id;
        summary.name = state.chosen.id.empty() ? std::string("oa-default") : state.chosen.id;
        summary.layout = state.chosen;
        inputs.chosen = summary;
        inputs.has_chosen = true;
        inputs.installed.push_back(std::move(summary));
    }
}

bool Runtime::save_key_editor(const editor::SaveResult& saved, std::string& error) {
    const std::string base_id =
        saved.layout_id.empty() ? std::string("oa-default") : saved.layout_id;
    if (!safe_layout_id(base_id)) {
        error = "the layout's id is refused";
        return false;
    }
    for (const oakeys::Manifest& made : saved.layouts) {
        if (!safe_layout_id(made.id)) {
            error = "the layout's id is refused";
            return false;
        }
    }

    const fs::path keys_folder = user_folder_ / std::string(library::keys_folder_name);
    std::error_code mkdir_error;
    fs::create_directories(keys_folder, mkdir_error);
    if (mkdir_error) {
        error = "the Keys folder was not made";
        return false;
    }
    for (const oakeys::Manifest& made : saved.layouts) {
        if (!write_installed_layout(keys_folder, made, error))
            return false;
    }
    if (!library::write_own(keys_folder, base_id, saved.manifest, error))
        return false;

    const std::string preference_key{layout_preference};
    if (preference_values_[preference_key] != base_id) {
        preference_values_[preference_key] = base_id;
        preferences_dirty_ = true;
    }
    save_preferences();

    const auto reload_bindings = [this] {
        if (key_bindings_state_ != nullptr)
            key_bindings_state_->signature.clear();
        load_key_bindings();
        ensure_key_bindings(false);
    };
    if (saved.number_keys != engine_settings().switch_alt) {
        const oa::ui::engine_settings::EngineSettings opened = engine_settings();
        oa::ui::engine_settings::EngineSettings chosen = opened;
        chosen.switch_alt = saved.number_keys;
        apply_engine_settings(chosen);
        const std::optional<std::string> problem = save_engine_settings(opened, chosen, false);
        if (problem) {
            reload_bindings();
            error = *problem;
            return false;
        }
    }
    reload_bindings();
    if (oa::ui::engine_settings::Dialog* open = engine_settings_dialog())
        refresh_settings_key_layout_summary(*open);
    error.clear();
    return true;
}

} // namespace oa::app
