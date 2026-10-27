// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/key_editor/model.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/ui/key_bindings/chord.hpp"

#include <string>
#include <utility>
#include <vector>

namespace oa::ui::key_editor {
namespace {

namespace keys = oa::ui::key_bindings;
namespace oakeys = oa::formats::oakeys;
namespace acts = oa::data::input_actions;

/// The chords an action is using.
///
/// @param editor the editor
/// @param action the action id
/// @return the chords, or none when the list has no such action
std::vector<keys::Chord> chords_in_use(const Editor& editor, std::string_view action) {
    std::vector<keys::Chord> chords;
    const Details shown = editor.details(action);
    chords.reserve(shown.chords.size());
    for (const ChordDetail& one : shown.chords)
        chords.push_back(one.chord);
    return chords;
}

/// True when two chord lists name the same keys, in order.
///
/// @param editor the keyboard
/// @param left one list
/// @param right the other
/// @return true when they match
bool same_chords(
    const Editor& editor,
    const std::vector<keys::Chord>& left,
    const std::vector<keys::Chord>& right
) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!keys::same_key(left[index], right[index], &editor.keyboard()))
            return false;
    }
    return true;
}

/// Appends one problem.
///
/// @param[in,out] preview the preview
/// @param line the line
/// @param column the column
/// @param key the action
/// @param message the sentence
void add_problem(
    PastePreview& preview, uint32_t line, uint32_t column, std::string key, std::string message
) {
    oakeys::Problem problem;
    problem.line = line == 0 ? 1 : line;
    problem.column = column == 0 ? 1 : column;
    problem.key = std::move(key);
    problem.message = std::move(message);
    preview.problems.push_back(std::move(problem));
}

/// One chord text found on a line, with where it sits.
struct ScannedChord {
    std::string text{};
    uint32_t line{1};
    uint32_t column{1};
};

/// Splits the inside of `[...]` into chord texts.
///
/// Quotes are dropped. A comma separates chords.
///
/// @param inside the text between the brackets
/// @param start_column the column of `inside`'s first byte
/// @return the chords
std::vector<ScannedChord> split_chords(std::string_view inside, uint32_t start_column) {
    std::vector<ScannedChord> chords;
    std::size_t at = 0;
    while (at < inside.size()) {
        while (at < inside.size() && (inside[at] == ' ' || inside[at] == '\t' || inside[at] == ','))
            ++at;
        if (at >= inside.size())
            break;
        const std::size_t began = at;
        if (inside[at] == '"' || inside[at] == '\'') {
            const char quote = inside[at];
            ++at;
            const std::size_t text_at = at;
            while (at < inside.size() && inside[at] != quote)
                ++at;
            ScannedChord chord;
            chord.text = std::string{inside.substr(text_at, at - text_at)};
            chord.column = start_column + static_cast<uint32_t>(text_at);
            chords.push_back(std::move(chord));
            if (at < inside.size())
                ++at;
            continue;
        }
        while (at < inside.size() && inside[at] != ',')
            ++at;
        std::string_view raw = inside.substr(began, at - began);
        while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t'))
            raw.remove_suffix(1);
        if (!raw.empty()) {
            ScannedChord chord;
            chord.text = std::string{raw};
            chord.column = start_column + static_cast<uint32_t>(began);
            chords.push_back(std::move(chord));
        }
    }
    return chords;
}

/// One binding scanned from a line when the document itself was refused.
struct ScannedBinding {
    std::string action{};
    uint32_t line{1};
    uint32_t column{1};
    std::vector<ScannedChord> chords{};
};

/// Reads `action: [chords]` lines from a text the strict reader refused.
///
/// @param bytes the text
/// @return the bindings those lines name
std::vector<ScannedBinding> scan_bindings(std::span<const uint8_t> bytes) {
    std::vector<ScannedBinding> found;
    const auto* data = reinterpret_cast<const char*>(bytes.data());
    std::size_t start = 0;
    uint32_t line = 1;
    while (start <= bytes.size()) {
        std::size_t end = start;
        while (end < bytes.size() && data[end] != '\n')
            ++end;
        std::string_view row{data + start, end - start};
        if (!row.empty() && row.back() == '\r')
            row.remove_suffix(1);
        const std::size_t colon = row.find(':');
        const std::size_t open = row.find('[');
        const std::size_t close = row.rfind(']');
        if (colon != std::string_view::npos && open != std::string_view::npos &&
            close != std::string_view::npos && colon < open && open < close) {
            std::string_view action = row.substr(0, colon);
            while (!action.empty() && (action.front() == ' ' || action.front() == '\t'))
                action.remove_prefix(1);
            while (!action.empty() && (action.back() == ' ' || action.back() == '\t'))
                action.remove_suffix(1);
            if (!action.empty() && action.find(' ') == std::string_view::npos &&
                action.front() != '#') {
                ScannedBinding binding;
                binding.action = std::string{action};
                binding.line = line;
                binding.column = static_cast<uint32_t>(row.find(action) + 1);
                const std::string_view inside = row.substr(open + 1, close - open - 1);
                binding.chords = split_chords(inside, static_cast<uint32_t>(open + 2));
                for (ScannedChord& chord : binding.chords)
                    chord.line = line;
                found.push_back(std::move(binding));
            }
        }
        if (end >= bytes.size())
            break;
        start = end + 1;
        ++line;
    }
    return found;
}

/// Builds one row from parsed chords.
///
/// @param[in,out] preview the preview
/// @param editor the chords in use
/// @param action the action id
/// @param line the action's line
/// @param column the action's column
/// @param chords the chords that parsed
void add_row(
    PastePreview& preview,
    const Editor& editor,
    std::string action,
    uint32_t line,
    uint32_t column,
    std::vector<keys::Chord> chords
) {
    if (chords.empty())
        return;
    PasteRow row;
    row.action = std::move(action);
    row.line = line;
    row.column = column;
    row.before = chords_in_use(editor, row.action);
    row.after = std::move(chords);
    row.unknown = acts::find(row.action) == nullptr;
    if (!row.unknown) {
        for (const keys::Chord& chord : row.after) {
            const std::vector<keys::Clash> clashes = editor.clashes_for(row.action, chord);
            if (clashes.empty())
                continue;
            row.clash = true;
            if (row.holder.empty())
                row.holder = clashes.front().b;
        }
    }
    const bool unchanged = !row.clash && !row.unknown && same_chords(editor, row.before, row.after);
    row.apply = !unchanged;
    if (row.clash)
        ++preview.clashes;
    if (row.unknown)
        ++preview.unknown;
    if (row.apply)
        ++preview.changes;
    preview.rows.push_back(std::move(row));
}

/// Parses one binding's chord texts into a row, recording each refusal.
///
/// @param[in,out] preview the preview
/// @param editor the chords in use
/// @param action the action id
/// @param line the action's line
/// @param column the action's column
/// @param texts the chord text, its line and its column
void take_binding(
    PastePreview& preview,
    const Editor& editor,
    std::string action,
    uint32_t line,
    uint32_t column,
    const std::vector<ScannedChord>& texts
) {
    std::vector<keys::Chord> chords;
    for (const ScannedChord& text : texts) {
        keys::ChordError error{};
        const std::optional<keys::Chord> parsed = keys::parse_chord(text.text, error);
        if (!parsed) {
            add_problem(
                preview,
                text.line,
                text.column,
                action,
                std::string{keys::chord_error_text(error.reason)}
            );
            continue;
        }
        chords.push_back(*parsed);
    }
    add_row(preview, editor, std::move(action), line, column, std::move(chords));
}

} // namespace

PastePreview paste_preview(const Editor& editor, std::span<const uint8_t> bytes) {
    PastePreview preview;
    oakeys::Manifest manifest;
    std::vector<oakeys::Problem> problems;
    const bool read = oakeys::read_bindings_text(bytes, manifest, problems);
    if (!read) {
        for (const oakeys::Problem& problem : problems)
            preview.problems.push_back(problem);
        for (const ScannedBinding& binding : scan_bindings(bytes))
            take_binding(
                preview, editor, binding.action, binding.line, binding.column, binding.chords
            );
        return preview;
    }
    for (const oakeys::BindingEntry& entry : manifest.bindings) {
        std::vector<ScannedChord> texts;
        texts.reserve(entry.chords.size());
        const uint32_t line = entry.line == 0 ? 1 : entry.line;
        for (const oakeys::ChordText& text : entry.chords) {
            ScannedChord chord;
            chord.text = text.text;
            chord.line = text.line == 0 ? line : text.line;
            chord.column = text.column == 0 ? 1 : text.column;
            texts.push_back(std::move(chord));
        }
        take_binding(
            preview, editor, entry.action, line, entry.column == 0 ? 1 : entry.column, texts
        );
    }
    return preview;
}

void apply_paste(Editor& editor, const PastePreview& preview) {
    for (const PasteRow& row : preview.rows) {
        if (!row.apply)
            continue;
        if (row.unknown) {
            bool replaced = false;
            for (keys::LayoutBinding& binding : editor.own_.bindings) {
                if (binding.action == row.action) {
                    binding.chords = row.after;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                keys::LayoutBinding binding;
                binding.action = row.action;
                binding.chords = row.after;
                editor.own_.bindings.push_back(std::move(binding));
            }
            continue;
        }
        std::vector<keys::Chord> kept;
        std::vector<keys::Chord> clashing;
        for (const keys::Chord& chord : row.after) {
            const std::vector<keys::Clash> clashes = editor.clashes_for(row.action, chord);
            if (!clashes.empty())
                clashing.push_back(chord);
            else
                kept.push_back(chord);
        }
        if (row.clash) {
            for (const keys::Chord& chord : clashing) {
                HeldClash held;
                held.pasted = row.action;
                held.holder = row.holder;
                if (held.holder.empty()) {
                    const std::vector<keys::Clash> clashes = editor.clashes_for(row.action, chord);
                    if (!clashes.empty())
                        held.holder = clashes.front().b;
                }
                held.chord = chord;
                if (!held.holder.empty())
                    editor.held_clashes_.push_back(std::move(held));
            }
            if (!kept.empty())
                editor.set_chords(row.action, std::move(kept));
            continue;
        }
        editor.set_chords(row.action, row.after);
    }
}

} // namespace oa::ui::key_editor
