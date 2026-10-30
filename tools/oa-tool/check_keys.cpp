// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Checks a key layout's files and gathers the facts a catalogue copies. The
// picture marks the same triggers. Chords are parsed and not bound. Not
// knowing the player's platform, the check names the platforms a clash
// between Primary and Ctrl or Meta holds on.

#include "check_keys.hpp"

#include "oa/app/package_install/oakeys.hpp"
#include "oa/data/input_actions/actions.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/png.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/input.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"
#include "oa/ui/key_bindings/layout.hpp"
#include "oa/ui/key_bindings/layout_file.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::tool {
namespace {

namespace fs = std::filesystem;
namespace actions = oa::data::input_actions;
namespace install = oa::app::package_install;
namespace json = oa::formats::json;
namespace keys = oa::data::key_library;
namespace layouts = oa::formats::oakeys;
namespace mut = oa::formats::oamut;
namespace oamod = oa::formats::oamod;
namespace png = oa::formats::png;
namespace bindings = oa::ui::key_bindings;

/// The words for an icon the limits refuse.
constexpr std::string_view icon_refusal =
    "it is not a square PNG of at most 256 pixels and 256 KiB";

/// The warning for an action this build does not know.
constexpr std::string_view unknown_action = "Not in this version of Open Annihilation";

/// The words a clash warning holds.
constexpr std::string_view clash_words = "both bind";

/// The files `inspect_key_folder` is reading, for `PackageEntries`.
struct FileList {
    const std::vector<keys::PackageFile>* held{}; ///< the files; null lists nothing
};

/// Tells whether two texts are equal with ASCII letters folded.
///
/// @param left one text
/// @param right the other
/// @return true when they are equal
bool same_ascii(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](unsigned char byte) {
            return byte >= 'A' && byte <= 'Z' ? static_cast<unsigned char>(byte - 'A' + 'a') : byte;
        };
        if (fold(static_cast<unsigned char>(left[index])) !=
            fold(static_cast<unsigned char>(right[index])))
            return false;
    }
    return true;
}

/// Appends a line that is not already there.
///
/// @param[in,out] lines the lines
/// @param line the line
void add_unique(std::vector<std::string>& lines, std::string line) {
    for (const std::string& have : lines) {
        if (have == line)
            return;
    }
    lines.push_back(std::move(line));
}

/// Appends every file the list holds.
///
/// @param context a FileList
/// @param[out] paths the file paths
void list_files(void* context, std::vector<std::string>& paths) {
    const auto* list = static_cast<const FileList*>(context);
    if (list == nullptr || list->held == nullptr)
        return;
    for (const keys::PackageFile& file : *list->held)
        paths.push_back(file.name);
}

/// Copies one file out of the list.
///
/// @param context a FileList
/// @param file_path the file's path
/// @param limit the most bytes it may hold
/// @param[out] bytes the file; empty when it was not read
/// @param[out] too_large true when the file is larger than limit
/// @return true when the file was read
bool read_files(
    void* context,
    std::string_view file_path,
    std::size_t limit,
    std::vector<uint8_t>& bytes,
    bool& too_large
) {
    bytes.clear();
    too_large = false;
    const auto* list = static_cast<const FileList*>(context);
    if (list == nullptr || list->held == nullptr)
        return false;
    const keys::PackageFile* found = nullptr;
    for (const keys::PackageFile& file : *list->held) {
        if (file.name == file_path) {
            found = &file;
            break;
        }
    }
    if (found == nullptr)
        return false;
    if (found->bytes.size() > limit) {
        too_large = true;
        return false;
    }
    bytes = found->bytes;
    return true;
}

/// Reads one entry, or reports that it is missing or too large.
///
/// @param entries the reader
/// @param file_path the path
/// @param limit the most bytes
/// @param[out] bytes the file
/// @param[out] too_large true when the file is larger than limit
/// @return true when the file was read
bool read_limited(
    const PackageEntries& entries,
    std::string_view file_path,
    std::size_t limit,
    std::vector<uint8_t>& bytes,
    bool& too_large
) {
    bytes.clear();
    too_large = false;
    if (entries.read == nullptr)
        return false;
    return entries.read(entries.context, file_path, limit, bytes, too_large);
}

/// Writes a list of strings.
///
/// @param[in,out] writer the JSON being written
/// @param lines the strings
void write_strings(json::JsonWriter& writer, std::span<const std::string> lines) {
    writer.begin_array();
    for (const std::string& line : lines)
        writer.string(line);
    writer.end_array();
}

/// Writes one string member when the text is not empty.
///
/// @param[in,out] writer the object
/// @param key the member's name
/// @param text the text
void write_text(json::JsonWriter& writer, std::string_view key, std::string_view text) {
    if (text.empty())
        return;
    writer.key(key);
    writer.string(text);
}

/// Returns a condition as the manifest wrote it: the id, then the range.
///
/// @param condition the condition
/// @return the text
std::string condition_text(const mut::Condition& condition) {
    if (condition.range_text.empty())
        return condition.id;
    return condition.id + " " + condition.range_text;
}

/// The first line of a description, cut to 200 bytes on a character boundary.
///
/// @param description the description
/// @return the summary
std::string cut_summary(std::string_view description) {
    const std::size_t break_at = description.find('\n');
    if (break_at != std::string_view::npos)
        description = description.substr(0, break_at);
    if (description.size() <= mut::most_summary_bytes)
        return std::string{description};
    std::size_t cut = mut::most_summary_bytes;
    while (cut > 0 && (static_cast<unsigned char>(description[cut]) & 0xC0) == 0x80)
        --cut;
    return std::string{description.substr(0, cut)};
}

/// Returns a keyboard's catalogue name.
///
/// @param keyboard the keyboard
/// @return any, ansi, iso or jis
const char* keyboard_name(layouts::Keyboard keyboard) noexcept {
    switch (keyboard) {
    case layouts::Keyboard::any:
        return "any";
    case layouts::Keyboard::ansi:
        return "ansi";
    case layouts::Keyboard::iso:
        return "iso";
    case layouts::Keyboard::jis:
        return "jis";
    }
    return "any";
}

/// Returns a pointer scheme's catalogue name.
///
/// @param pointer the scheme
/// @return right-click or left-click
const char* pointer_name(layouts::Pointer pointer) noexcept {
    switch (pointer) {
    case layouts::Pointer::right_click:
        return "right-click";
    case layouts::Pointer::left_click:
        return "left-click";
    }
    return "right-click";
}

/// Returns a base's catalogue name.
///
/// @param base the base
/// @return oa-default or none
const char* base_name(layouts::Base base) noexcept {
    return base == layouts::Base::none ? "none" : "oa-default";
}

/// One layout error or warning, in the manifest's diagnostic form.
///
/// @param problem the problem
/// @return oakeys.yaml:line:column: key: message
std::string layout_line(const bindings::LayoutProblem& problem) {
    return std::string(layouts::manifest_file) + ":" + std::to_string(problem.line) + ":" +
           std::to_string(problem.column) + ": " + problem.key + ": " + problem.message;
}

/// Appends the icon problem.
///
/// @param[in,out] problems the problems
/// @param file_path the icon's path
void refuse_icon(std::vector<std::string>& problems, std::string_view file_path) {
    problems.push_back(std::string{file_path} + ": " + std::string{icon_refusal});
}

/// The trigger text of a chord, without its modifiers or its gesture.
///
/// A modifier alone has none.
///
/// @param chord the chord
/// @return the trigger, or empty
std::string trigger_text(const bindings::Chord& chord) {
    if (chord.kind == bindings::TriggerKind::modifier_alone)
        return {};
    bindings::Chord bare = chord;
    bare.modifiers = 0;
    bare.gesture = bindings::Gesture::press;
    return bindings::chord_text(bare);
}

/// Folds an ASCII letter to lower case. Any other character is unchanged.
///
/// @param character the character
/// @return the folded character
char32_t fold_letter(char32_t character) noexcept {
    if (character >= U'A' && character <= U'Z')
        return static_cast<char32_t>(character - U'A' + U'a');
    return character;
}

/// The W3C code a chord's trigger marks, or empty when the picture has no key.
///
/// @param chord the chord
/// @param us the US keyboard
/// @return the code
std::string code_of_chord(const bindings::Chord& chord, const bindings::KeyboardMap& us) {
    if (chord.kind == bindings::TriggerKind::character) {
        const char32_t want = fold_letter(chord.character);
        for (uint16_t scancode = 0; scancode < bindings::scancode_limit; ++scancode) {
            const char32_t printed = fold_letter(us.printed(scancode));
            if (printed == 0 || printed != want)
                continue;
            const std::string_view code = bindings::code_of(scancode);
            if (!code.empty() && !bindings::position_refused(code))
                return std::string{code};
            return {};
        }
        return {};
    }
    if (chord.kind != bindings::TriggerKind::place && chord.kind != bindings::TriggerKind::named)
        return {};
    uint16_t scancode = chord.scancode;
    if (chord.kind == bindings::TriggerKind::named && chord.named < bindings::named_keys().size())
        scancode = bindings::named_keys()[chord.named].scancode;
    const std::string_view code = bindings::code_of(scancode);
    if (code.empty() || bindings::position_refused(code))
        return {};
    return std::string{code};
}

/// The triggers a layout's own bindings name, and the keys they mark.
struct Marks {
    std::vector<std::string> triggers{}; ///< each trigger once, in first-use order
    std::vector<std::string> codes{};    ///< each W3C code once, in first-use order
};

/// Tells whether a list already holds a text.
///
/// @param lines the list
/// @param text the text
/// @return true when it does
bool holds(const std::vector<std::string>& lines, std::string_view text) {
    for (const std::string& line : lines) {
        if (line == text)
            return true;
    }
    return false;
}

/// Walks a layout's own chords for their triggers and their key codes.
///
/// @param manifest the layout
/// @return the marks
Marks marks_of(const layouts::Manifest& manifest) {
    Marks marks;
    const bindings::KeyboardMap us = bindings::KeyboardMap::us();
    for (const layouts::BindingEntry& binding : manifest.bindings) {
        for (const layouts::ChordText& written : binding.chords) {
            bindings::ChordError chord_error{};
            const std::optional<bindings::Chord> parsed =
                bindings::parse_chord(written.text, chord_error);
            if (!parsed)
                continue;
            const std::string trigger = trigger_text(*parsed);
            if (!trigger.empty() && !holds(marks.triggers, trigger))
                marks.triggers.push_back(trigger);
            const std::string code = code_of_chord(*parsed, us);
            if (!code.empty() && !holds(marks.codes, code))
                marks.codes.push_back(code);
        }
    }
    return marks;
}

/// Checks that the icon is a square PNG within the limits.
///
/// @param entries the reader
/// @param paths the package's files
/// @param manifest the manifest
/// @param[in,out] problems the problems
void check_icon(
    const PackageEntries& entries,
    const std::vector<std::string>& paths,
    const layouts::Manifest& manifest,
    std::vector<std::string>& problems
) {
    if (manifest.icon.empty())
        return;
    bool held = false;
    for (const std::string& file_path : paths) {
        if (file_path == manifest.icon) {
            held = true;
            break;
        }
    }
    if (!held) {
        refuse_icon(problems, manifest.icon);
        return;
    }
    std::vector<uint8_t> bytes;
    bool too_large = false;
    if (!read_limited(
            entries,
            manifest.icon,
            static_cast<std::size_t>(install::oakeys::most_icon_bytes),
            bytes,
            too_large
        )) {
        refuse_icon(problems, manifest.icon);
        return;
    }
    png::Info info{};
    const bool png_ok = png::has_signature(bytes) && png::read_info(bytes, png::Messages{}, &info);
    const bool square = png_ok && info.header.width == info.header.height &&
                        info.header.width >= 1 &&
                        info.header.width <= install::oakeys::most_icon_side;
    if (!square)
        refuse_icon(problems, manifest.icon);
}

/// Reads the folder's oakeys.yaml, when a regular file of that name is there.
///
/// @param folder the folder
/// @param[out] bytes the file; empty when it was not read
/// @param[in,out] problems a size problem, when the file is over 256 KiB
/// @return true when `bytes` holds the file
bool load_manifest_file(
    const fs::path& folder, std::vector<uint8_t>& bytes, std::vector<std::string>& problems
) {
    bytes.clear();
    std::error_code listed;
    if (!fs::is_directory(fs::symlink_status(folder, listed)) || listed) {
        problems.push_back("it cannot be listed");
        return false;
    }
    fs::path manifest_path;
    try {
        const fs::directory_iterator end;
        for (fs::directory_iterator cursor(folder); cursor != end; ++cursor) {
            std::error_code status_error;
            const fs::file_status status = cursor->symlink_status(status_error);
            if (status_error || fs::is_symlink(status) || !fs::is_regular_file(status))
                continue;
            const auto name_text = cursor->path().filename().generic_u8string();
            const std::string name(name_text.begin(), name_text.end());
            if (!same_ascii(name, layouts::manifest_file))
                continue;
            manifest_path = cursor->path();
            break;
        }
    } catch (const std::exception&) {
        problems.push_back("it cannot be listed");
        return false;
    }
    if (manifest_path.empty())
        return false;
    std::ifstream input(manifest_path, std::ios::binary);
    if (!input)
        return false;
    input.seekg(0, std::ios::end);
    const std::streamoff end_at = input.tellg();
    if (end_at < 0)
        return false;
    const auto size = static_cast<uint64_t>(end_at);
    if (size > static_cast<uint64_t>(oamod::max_input_bytes)) {
        problems.push_back(std::string(layouts::manifest_file) + " is larger than 256 KiB");
        return false;
    }
    input.seekg(0);
    bytes.resize(static_cast<std::size_t>(size));
    if (size > 0)
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!input) {
        bytes.clear();
        return false;
    }
    return true;
}

/// Platforms that read Primary alike, named as a warning names them, since
/// a check does not know which one the player uses.
struct NamedPlatforms {
    std::string_view names{};      ///< the platforms, as a warning names them
    bindings::Platform platform{}; ///< which key Primary means on them
};

/// Where Primary is Ctrl, and where it is the command key.
constexpr NamedPlatforms named_platforms[] = {
    {"Windows, Linux and the Steam Deck", bindings::Platform{false, false}},
    {"macOS", bindings::Platform{true, false}},
};

/// The platforms a warning names for a clash that holds on all of them.
constexpr std::string_view every_platform = "Windows, Linux, the Steam Deck and macOS";

/// One chord a layout writes, and the action it binds.
struct WrittenChord {
    bindings::Chord chord{}; ///< the chord
    std::string action{};    ///< the action, as the layout writes it
};

/// Tells whether two actions can be live at once, so that one key on both is
/// a clash.
///
/// Their contexts must meet and their conditions hold together, and two
/// modifier roles share a modifier without a clash. An action the registry
/// does not know, a mod's own made action among them, is taken to meet every
/// other. The registry's contexts are read with no hack on.
///
/// @param first one action id
/// @param second the other action id
/// @return true when the two can be live together
bool live_together(std::string_view first, std::string_view second) {
    const actions::Action* one = actions::find(first);
    const actions::Action* other = actions::find(second);
    if (one == nullptr || other == nullptr)
        return true;
    if (one->kind == actions::Kind::modifier && other->kind == actions::Kind::modifier)
        return false;
    return bindings::layers_meet(one->context, other->context) &&
           bindings::conditions_meet(one->when, other->when);
}

/// Warns of two actions whose chords are written apart but pressed as one
/// key on some platforms, such as Primary+5 and Ctrl+5, naming those
/// platforms.
///
/// Two chords written alike are layout_from_manifest's own warning, two
/// chords of one action are not a clash, and neither are two actions that
/// are never live together. A chord that does not parse is left to the
/// errors. Each warning is at the later chord, as `<later> and <earlier> both
/// bind <chord> on <platforms>`, the chord as it is pressed.
///
/// @param manifest the layout
/// @param[in,out] found one warning appended for each such pair
void add_platform_clashes(
    const layouts::Manifest& manifest, std::vector<bindings::LayoutProblem>& found
) {
    std::vector<WrittenChord> seen;
    for (const layouts::BindingEntry& binding : manifest.bindings) {
        for (std::size_t chord_index = 0; chord_index < binding.chords.size(); ++chord_index) {
            const layouts::ChordText& written = binding.chords[chord_index];
            bindings::ChordError chord_error{};
            const std::optional<bindings::Chord> parsed =
                bindings::parse_chord(written.text, chord_error);
            if (!parsed)
                continue;
            for (const WrittenChord& earlier : seen) {
                if (earlier.action == binding.action || earlier.chord == *parsed)
                    continue;
                if (!live_together(earlier.action, binding.action))
                    continue;
                const NamedPlatforms* first = nullptr;
                std::size_t held_on = 0;
                for (const NamedPlatforms& group : named_platforms) {
                    if (!bindings::same_key_on(earlier.chord, *parsed, nullptr, group.platform))
                        continue;
                    if (first == nullptr)
                        first = &group;
                    ++held_on;
                }
                if (first == nullptr)
                    continue;
                const std::string_view where =
                    held_on == std::size(named_platforms) ? every_platform : first->names;
                const std::string pressed =
                    bindings::chord_text(bindings::pressed_chord(*parsed, first->platform));
                bindings::LayoutProblem problem;
                problem.line = written.line;
                problem.column = written.column;
                problem.key =
                    "bindings." + binding.action + "[" + std::to_string(chord_index) + "]";
                problem.message = binding.action + " and " + earlier.action + " both bind " +
                                  pressed + " on " + std::string{where};
                problem.warning = true;
                found.push_back(std::move(problem));
            }
            seen.push_back(WrittenChord{*parsed, binding.action});
        }
    }
}

} // namespace

KeyFolder inspect_key_folder(const fs::path& folder) {
    KeyFolder out;
    std::string folder_problem;
    std::optional<std::vector<keys::PackageFile>> packed =
        keys::read_folder(folder, &folder_problem);

    std::vector<uint8_t> manifest_bytes;
    if (load_manifest_file(folder, manifest_bytes, out.problems)) {
        std::vector<layouts::Problem> found;
        layouts::Manifest manifest;
        if (!layouts::read_manifest(manifest_bytes, manifest, found)) {
            for (const layouts::Problem& one : found)
                add_unique(out.problems, layouts::describe(one));
        } else {
            out.manifest = std::move(manifest);
        }
    }
    if (!packed) {
        if (!folder_problem.empty())
            add_unique(out.problems, std::move(folder_problem));
        if (out.problems.empty())
            out.problems.push_back(std::string(layouts::manifest_file) + ": it cannot be read");
        return out;
    }

    out.files = std::move(*packed);
    out.have_files = true;
    FileList list{&out.files};
    const PackageEntries entries{&list, &list_files, &read_files};
    check_keys_files(entries, out.manifest, out.problems, out.checked);
    return out;
}

void check_keys_files(
    const PackageEntries& entries,
    const layouts::Manifest& manifest,
    std::vector<std::string>& problems,
    KeysChecked& checked
) {
    checked = {};
    checked.changes = static_cast<uint32_t>(manifest.bindings.size());
    checked.triggers = marks_of(manifest).triggers;

    std::vector<std::string> paths;
    if (entries.list != nullptr)
        entries.list(entries.context, paths);
    for (const std::string& file_path : paths) {
        if (keys::file_role(file_path, manifest.icon) == keys::FileRole::outside)
            problems.push_back(file_path + ": outside the files a key layout may hold");
    }
    check_icon(entries, paths, manifest, problems);

    bindings::Layout layout;
    std::vector<bindings::LayoutProblem> found;
    static_cast<void>(bindings::layout_from_manifest(manifest, nullptr, layout, found));
    add_platform_clashes(manifest, found);
    for (const bindings::LayoutProblem& one : found) {
        if (!one.warning) {
            problems.push_back(layout_line(one));
            continue;
        }
        checked.warnings.push_back(layout_line(one));
        if (one.message == unknown_action)
            ++checked.unknown_actions;
        if (one.message.find(clash_words) != std::string::npos)
            ++checked.clashes;
    }
}

bool read_oakeys_manifest(
    const PackageEntries& entries, layouts::Manifest& manifest, std::vector<std::string>& problems
) {
    std::vector<std::string> paths;
    if (entries.list != nullptr)
        entries.list(entries.context, paths);
    std::string manifest_path;
    for (const std::string& file_path : paths) {
        if (same_ascii(file_path, layouts::manifest_file)) {
            manifest_path = file_path;
            break;
        }
    }
    if (manifest_path.empty()) {
        problems.push_back(std::string(layouts::manifest_file) + ": it cannot be read");
        return false;
    }
    std::vector<uint8_t> bytes;
    bool too_large = false;
    if (!read_limited(entries, manifest_path, oamod::max_input_bytes, bytes, too_large) ||
        too_large) {
        problems.push_back(
            too_large ? std::string(layouts::manifest_file) + " is larger than 256 KiB"
                      : std::string(layouts::manifest_file) + ": it cannot be read"
        );
        return false;
    }
    std::vector<layouts::Problem> found;
    if (!layouts::read_manifest(bytes, manifest, found)) {
        for (const layouts::Problem& one : found)
            problems.push_back(layouts::describe(one));
        return false;
    }
    return true;
}

std::vector<std::string> changed_key_codes(const layouts::Manifest& manifest) {
    return marks_of(manifest).codes;
}

void describe_oakeys(
    const install::Package& package, json::JsonWriter& writer, const KeysChecked& checked
) {
    const PackageEntries entries = entries_of(package);
    layouts::Manifest manifest;
    std::vector<std::string> ignored;
    const bool read = read_oakeys_manifest(entries, manifest, ignored);
    if (read) {
        write_text(writer, "summary", cut_summary(manifest.description));
        write_text(writer, "description", manifest.description);
        write_text(writer, "homepage", manifest.homepage);
        if (!manifest.tags.empty()) {
            writer.key("tags");
            write_strings(writer, manifest.tags);
        }
        write_text(writer, "author", manifest.author.name);
        if (!manifest.requires_engine.empty()) {
            writer.key("requires");
            writer.begin_object();
            writer.key("engine");
            writer.string(manifest.requires_engine);
            writer.end_object();
        }
        if (!manifest.works_with.empty()) {
            writer.key("works_with");
            writer.begin_array();
            for (const mut::Condition& condition : manifest.works_with)
                writer.string(condition_text(condition));
            writer.end_array();
        }
        writer.key("base");
        writer.string(base_name(manifest.base));
        writer.key("keyboard");
        writer.string(keyboard_name(manifest.keyboard));
        if (manifest.pointer.has_value()) {
            writer.key("pointer");
            writer.string(pointer_name(*manifest.pointer));
        }
        std::vector<std::string> helpers;
        if (manifest.helpers.toggle.has_value())
            helpers.emplace_back("toggle");
        if (manifest.helpers.sticky_modifiers.has_value())
            helpers.emplace_back("sticky-modifiers");
        if (manifest.helpers.hold_to_confirm.has_value())
            helpers.emplace_back("hold-to-confirm");
        if (manifest.helpers.pointer_keys.has_value())
            helpers.emplace_back("pointer-keys");
        if (manifest.helpers.radial_orders.has_value())
            helpers.emplace_back("radial-orders");
        if (manifest.helpers.edge_scroll.has_value())
            helpers.emplace_back("edge-scroll");
        if (!helpers.empty()) {
            writer.key("helpers");
            write_strings(writer, helpers);
        }
    }
    writer.key("changes");
    writer.integer(static_cast<int64_t>(checked.changes));
    writer.key("triggers");
    write_strings(writer, checked.triggers);
    writer.key("unknown_actions");
    writer.integer(static_cast<int64_t>(checked.unknown_actions));
    writer.key("clashes");
    writer.integer(static_cast<int64_t>(checked.clashes));
    if (read)
        write_icon_facts(package, manifest.icon, install::oakeys::most_icon_bytes, writer);
}

} // namespace oa::tool
