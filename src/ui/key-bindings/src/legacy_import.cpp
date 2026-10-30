// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/key_bindings/legacy_import.hpp"

#include "oa/ui/key_bindings/key_codes.hpp"

#include <iterator>
#include <string>

namespace oa::ui::key_bindings {
namespace {

/// How many bytes of a legacy file are read.
constexpr std::size_t kByteLimit = 64u * 1024u;

/// One Windows virtual-key number and the UI Events code it is.
struct VirtualKey {
    uint32_t number{};
    std::string_view code{};
};

/// The table. Modifier numbers are included so each one is a code K04 knows.
constexpr VirtualKey kVirtualKeys[] = {
    {0x08, "Backspace"},
    {0x09, "Tab"},
    {0x0D, "Enter"},
    {0x10, "ShiftLeft"},
    {0x11, "ControlLeft"},
    {0x12, "AltLeft"},
    {0x13, "Pause"},
    {0x1B, "Escape"},
    {0x20, "Space"},
    {0x21, "PageUp"},
    {0x22, "PageDown"},
    {0x23, "End"},
    {0x24, "Home"},
    {0x25, "ArrowLeft"},
    {0x26, "ArrowUp"},
    {0x27, "ArrowRight"},
    {0x28, "ArrowDown"},
    {0x2C, "PrintScreen"},
    {0x2D, "Insert"},
    {0x2E, "Delete"},
    {0x30, "Digit0"},
    {0x31, "Digit1"},
    {0x32, "Digit2"},
    {0x33, "Digit3"},
    {0x34, "Digit4"},
    {0x35, "Digit5"},
    {0x36, "Digit6"},
    {0x37, "Digit7"},
    {0x38, "Digit8"},
    {0x39, "Digit9"},
    {0x41, "KeyA"},
    {0x42, "KeyB"},
    {0x43, "KeyC"},
    {0x44, "KeyD"},
    {0x45, "KeyE"},
    {0x46, "KeyF"},
    {0x47, "KeyG"},
    {0x48, "KeyH"},
    {0x49, "KeyI"},
    {0x4A, "KeyJ"},
    {0x4B, "KeyK"},
    {0x4C, "KeyL"},
    {0x4D, "KeyM"},
    {0x4E, "KeyN"},
    {0x4F, "KeyO"},
    {0x50, "KeyP"},
    {0x51, "KeyQ"},
    {0x52, "KeyR"},
    {0x53, "KeyS"},
    {0x54, "KeyT"},
    {0x55, "KeyU"},
    {0x56, "KeyV"},
    {0x57, "KeyW"},
    {0x58, "KeyX"},
    {0x59, "KeyY"},
    {0x5A, "KeyZ"},
    {0x5D, "ContextMenu"},
    {0x60, "Numpad0"},
    {0x61, "Numpad1"},
    {0x62, "Numpad2"},
    {0x63, "Numpad3"},
    {0x64, "Numpad4"},
    {0x65, "Numpad5"},
    {0x66, "Numpad6"},
    {0x67, "Numpad7"},
    {0x68, "Numpad8"},
    {0x69, "Numpad9"},
    {0x6A, "NumpadMultiply"},
    {0x6B, "NumpadAdd"},
    {0x6D, "NumpadSubtract"},
    {0x6E, "NumpadDecimal"},
    {0x6F, "NumpadDivide"},
    {0x70, "F1"},
    {0x71, "F2"},
    {0x72, "F3"},
    {0x73, "F4"},
    {0x74, "F5"},
    {0x75, "F6"},
    {0x76, "F7"},
    {0x77, "F8"},
    {0x78, "F9"},
    {0x79, "F10"},
    {0x7A, "F11"},
    {0x7B, "F12"},
    {0x7C, "F13"},
    {0x7D, "F14"},
    {0x7E, "F15"},
    {0x7F, "F16"},
    {0x80, "F17"},
    {0x81, "F18"},
    {0x82, "F19"},
    {0x83, "F20"},
    {0x84, "F21"},
    {0x85, "F22"},
    {0x86, "F23"},
    {0x87, "F24"},
    {0xA0, "ShiftLeft"},
    {0xA1, "ShiftRight"},
    {0xA2, "ControlLeft"},
    {0xA3, "ControlRight"},
    {0xA4, "AltLeft"},
    {0xA5, "AltRight"},
    {0xBA, "Semicolon"},
    {0xBB, "Equal"},
    {0xBC, "Comma"},
    {0xBD, "Minus"},
    {0xBE, "Period"},
    {0xBF, "Slash"},
    {0xC0, "Backquote"},
    {0xDB, "BracketLeft"},
    {0xDC, "Backslash"},
    {0xDD, "BracketRight"},
    {0xDE, "Quote"},
    {0xE2, "IntlBackslash"},
};

/// One named tdraw key.
struct TdrawName {
    std::string_view name{};
    std::string_view action{};
};

constexpr TdrawName kTdrawNames[] = {
    {"whiteboardkey", "whiteboard.draw"},
    {"megamapkey", "megamap.toggle"},
    {"buildlinesringskey", "build.line"},
    {"keycode", "build.line"},
    {"clicksnapoverridekey", "build.snap-override"},
    {"rotatebuildkey", "build.rotate"},
};

/// One cnc-ddraw hotkey.
struct DdrawName {
    std::string_view name{};
    std::string_view action{}; ///< empty when Open Annihilation has no action
    uint8_t extra{};           ///< the modifier the hotkey adds
    bool second{};             ///< the action's second chord
};

constexpr DdrawName kDdrawNames[] = {
    {"keytogglefullscreen", "ui.full-screen", alt, false},
    {"keytogglemaximize", "", alt, false},
    {"keyunlockcursor1", "ui.release-pointer", ctrl, false},
    {"keyunlockcursor2", "ui.release-pointer", alt, true},
    {"keyscreenshot", "game.screenshot", 0, false},
};

/// Folds one ASCII letter.
///
/// @param byte the byte
/// @return the lower-case letter, or the byte
char folded(unsigned char byte) noexcept {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return static_cast<char>(byte);
}

/// Compares two names ignoring ASCII case.
///
/// @param left one name
/// @param right the other
/// @return true when they match
bool same_name(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (folded(static_cast<unsigned char>(left[index])) !=
            folded(static_cast<unsigned char>(right[index])))
            return false;
    }
    return true;
}

/// Drops spaces and tabs at both ends.
///
/// @param text the text
/// @return the middle
std::string_view trimmed(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

/// Cuts a `;` comment off a line.
///
/// @param text the line
/// @return the line before the comment
std::string_view without_comment(std::string_view text) noexcept {
    const std::size_t mark = text.find(';');
    if (mark == std::string_view::npos)
        return text;
    return text.substr(0, mark);
}

/// Reads a decimal or `0x` hex number.
///
/// @param text the value
/// @return the number, or nothing when it is not one
std::optional<uint32_t> parse_number(std::string_view text) noexcept {
    text = trimmed(text);
    if (text.empty())
        return std::nullopt;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
        if (text.empty())
            return std::nullopt;
    }
    uint32_t value = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto byte = static_cast<unsigned char>(text[index]);
        int digit = -1;
        if (byte >= '0' && byte <= '9')
            digit = static_cast<int>(byte - '0');
        else if (base == 16 && byte >= 'a' && byte <= 'f')
            digit = static_cast<int>(byte - 'a' + 10);
        else if (base == 16 && byte >= 'A' && byte <= 'F')
            digit = static_cast<int>(byte - 'A' + 10);
        if (digit < 0)
            return std::nullopt;
        if (value > (0xffffffffu - static_cast<uint32_t>(digit)) / static_cast<uint32_t>(base))
            return std::nullopt;
        value = value * static_cast<uint32_t>(base) + static_cast<uint32_t>(digit);
    }
    return value;
}

/// True when the number is a left-hand or right-hand modifier, not the generic one.
///
/// @param code the virtual-key number
/// @return true for the six specific hands
bool sided_modifier(uint32_t code) noexcept {
    return code == 0xA0 || code == 0xA1 || code == 0xA2 || code == 0xA3 || code == 0xA4 ||
           code == 0xA5;
}

/// The English name of a modifier bit, for the row's sentence.
///
/// @param bit Shift, Ctrl or Alt
/// @return the word
std::string_view modifier_word(uint8_t bit) noexcept {
    if (bit == shift)
        return "Shift";
    if (bit == ctrl)
        return "Ctrl";
    if (bit == alt)
        return "Alt";
    return {};
}

/// Builds the chord a virtual-key number names, with extra modifier bits.
///
/// A place is written with an at sign, so a named key stays a named key and
/// a letter stays a place. A modifier number is that modifier, joined with
/// the extra bits. The note is set when the number names one hand.
///
/// @param number the virtual-key number
/// @param extra modifier bits the hotkey adds
/// @param[out] note the left-and-right sentence, or empty
/// @return the chord, or nothing when the number is not in the table
std::optional<Chord> chord_of_number(uint32_t number, uint8_t extra, std::string& note) {
    note.clear();
    const std::optional<std::string_view> code = code_of_virtual_key(number);
    if (!code)
        return std::nullopt;
    const std::optional<uint8_t> modifier = modifier_of_virtual_key(number);
    if (modifier && position_refused(*code)) {
        Chord chord;
        chord.kind = TriggerKind::modifier_alone;
        chord.modifiers = static_cast<uint8_t>(extra | *modifier);
        if (sided_modifier(number)) {
            const std::string_view word = modifier_word(*modifier);
            note = "Left and right ";
            note += word;
            note += " are one key in Open Annihilation";
        }
        return chord;
    }
    std::string text;
    if ((extra & ctrl) != 0)
        text += "Ctrl+";
    if ((extra & alt) != 0)
        text += "Alt+";
    if ((extra & shift) != 0)
        text += "Shift+";
    text.push_back('@');
    text += *code;
    ChordError error{};
    return parse_chord(text, error);
}

/// Appends one problem.
///
/// @param[in,out] read the result
/// @param line the line
/// @param key the name
/// @param message the sentence
void add_problem(LegacyRead& read, uint32_t line, std::string key, std::string message) {
    LayoutProblem problem;
    problem.line = line;
    problem.column = 1;
    problem.key = std::move(key);
    problem.message = std::move(message);
    problem.warning = true;
    read.problems.push_back(std::move(problem));
}

/// One assignment the ini reader kept.
struct Assignment {
    std::string section{};
    std::string name{};
    std::string value{};
    std::string source{};
    uint32_t line{};
};

/// Reads sections and keys, ignoring case, with `;` comments and spaces around `=`.
///
/// @param bytes the file, of which at most 64 KiB are read
/// @param[out] stopped true when the file was longer than the limit
/// @return the assignments
std::vector<Assignment> read_ini(std::span<const uint8_t> bytes, bool& stopped) {
    stopped = bytes.size() > kByteLimit;
    const std::size_t count = stopped ? kByteLimit : bytes.size();
    std::vector<Assignment> rows;
    std::string section;
    uint32_t line = 1;
    std::size_t at = 0;
    while (at < count) {
        std::size_t end = at;
        while (end < count && bytes[end] != '\n')
            ++end;
        std::string_view row{reinterpret_cast<const char*>(bytes.data() + at), end - at};
        if (!row.empty() && row.back() == '\r')
            row.remove_suffix(1);
        const std::string source{row};
        row = trimmed(without_comment(row));
        if (!row.empty() && row.front() == '[' && row.back() == ']') {
            section = std::string{trimmed(row.substr(1, row.size() - 2))};
        } else {
            const std::size_t equal = row.find('=');
            if (equal != std::string_view::npos && !section.empty()) {
                const std::string_view name = trimmed(row.substr(0, equal));
                const std::string_view value = trimmed(row.substr(equal + 1));
                if (!name.empty()) {
                    Assignment made;
                    made.section = section;
                    made.name = std::string{name};
                    made.value = std::string{value};
                    made.source = source;
                    made.line = line;
                    rows.push_back(std::move(made));
                }
            }
        }
        if (end >= count)
            break;
        at = end + 1;
        ++line;
    }
    return rows;
}

/// The tdraw action of a key name, or empty when the name is not one of them.
///
/// @param name the key, any case
/// @return the action id
std::string_view tdraw_action(std::string_view name) noexcept {
    for (const TdrawName& one : kTdrawNames) {
        if (same_name(name, one.name))
            return one.action;
    }
    return {};
}

/// The ddraw hotkey, or null.
///
/// @param name the key, any case
/// @return the hotkey
const DdrawName* ddraw_name(std::string_view name) noexcept {
    for (const DdrawName& one : kDdrawNames) {
        if (same_name(name, one.name))
            return &one;
    }
    return nullptr;
}

/// Finds a key already made for an action, so a second chord joins it.
///
/// @param keys the keys
/// @param action the action id
/// @return the key, or null
LegacyKey* key_for(std::vector<LegacyKey>& keys, std::string_view action) {
    if (action.empty())
        return nullptr;
    for (LegacyKey& key : keys) {
        if (key.action == action)
            return &key;
    }
    return nullptr;
}

} // namespace

std::optional<std::string_view> code_of_virtual_key(uint32_t code) {
    for (const VirtualKey& key : kVirtualKeys) {
        if (key.number == code)
            return key.code;
    }
    return std::nullopt;
}

std::optional<uint8_t> modifier_of_virtual_key(uint32_t code) {
    if (code == 0x10 || code == 0xA0 || code == 0xA1)
        return shift;
    if (code == 0x11 || code == 0xA2 || code == 0xA3)
        return ctrl;
    if (code == 0x12 || code == 0xA4 || code == 0xA5)
        return alt;
    return std::nullopt;
}

LegacyRead read_tdraw_settings(std::span<const uint8_t> bytes) {
    LegacyRead read;
    bool stopped = false;
    const std::vector<Assignment> rows = read_ini(bytes, stopped);
    if (stopped)
        add_problem(read, 1, "file", "only the first 64 KiB were read");
    for (const Assignment& row : rows) {
        if (!same_name(row.section, "Settings"))
            continue;
        const std::string_view action = tdraw_action(row.name);
        if (action.empty()) {
            add_problem(
                read, row.line, row.name, "ignored; it is not a key Open Annihilation imports"
            );
            continue;
        }
        const std::optional<uint32_t> number = parse_number(row.value);
        if (!number) {
            add_problem(read, row.line, row.name, "the value is not a decimal or hex number");
            continue;
        }
        if (*number == 0)
            continue;
        std::string note;
        const std::optional<Chord> chord = chord_of_number(*number, 0, note);
        if (!chord) {
            add_problem(
                read,
                row.line,
                row.name,
                "Windows key " + row.value + " is not a key this version knows"
            );
            continue;
        }
        LegacyKey key;
        key.action = std::string{action};
        key.chords.push_back(*chord);
        key.source_line = row.source;
        key.line = row.line;
        key.note = std::move(note);
        read.keys.push_back(std::move(key));
    }
    return read;
}

LegacyRead read_cnc_ddraw(std::span<const uint8_t> bytes) {
    LegacyRead read;
    bool stopped = false;
    const std::vector<Assignment> rows = read_ini(bytes, stopped);
    if (stopped)
        add_problem(read, 1, "file", "only the first 64 KiB were read");

    /// The last value of each hotkey. TotalA is read after ddraw, so it wins.
    struct Kept {
        bool seen{false};
        Assignment row{};
        bool off{false};
    };

    Kept kept[std::size(kDdrawNames)]{};
    for (const Assignment& row : rows) {
        const bool ddraw = same_name(row.section, "ddraw");
        const bool totala = same_name(row.section, "TotalA");
        if (!ddraw && !totala)
            continue;
        const DdrawName* named = ddraw_name(row.name);
        if (named == nullptr)
            continue;
        const std::size_t index = static_cast<std::size_t>(named - kDdrawNames);
        const std::optional<uint32_t> number = parse_number(row.value);
        if (!number) {
            add_problem(read, row.line, row.name, "the value is not a decimal or hex number");
            continue;
        }
        kept[index].seen = true;
        kept[index].row = row;
        kept[index].off = *number == 0;
    }
    for (std::size_t index = 0; index < std::size(kDdrawNames); ++index) {
        if (!kept[index].seen || kept[index].off)
            continue;
        const DdrawName& named = kDdrawNames[index];
        const Assignment& row = kept[index].row;
        const std::optional<uint32_t> number = parse_number(row.value);
        if (!number || *number == 0)
            continue;
        std::string note;
        const std::optional<Chord> chord = chord_of_number(*number, named.extra, note);
        if (!chord) {
            add_problem(
                read,
                row.line,
                row.name,
                "Windows key " + row.value + " is not a key this version knows"
            );
            continue;
        }
        if (named.action.empty()) {
            LegacyKey key;
            key.source_line = row.source;
            key.line = row.line;
            key.chords.push_back(*chord);
            key.note = std::move(note);
            read.keys.push_back(std::move(key));
            continue;
        }
        LegacyKey* existing = key_for(read.keys, named.action);
        if (existing == nullptr) {
            LegacyKey key;
            key.action = std::string{named.action};
            key.chords.push_back(*chord);
            key.source_line = row.source;
            key.line = row.line;
            key.note = std::move(note);
            read.keys.push_back(std::move(key));
        } else {
            existing->chords.push_back(*chord);
            if (existing->note.empty())
                existing->note = std::move(note);
            if (!named.second) {
                existing->source_line = row.source;
                existing->line = row.line;
            }
        }
    }
    return read;
}

} // namespace oa::ui::key_bindings
