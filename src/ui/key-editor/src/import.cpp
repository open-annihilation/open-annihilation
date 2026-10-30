// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/key_editor/model.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/legacy_import.hpp"

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
/// @return the chords
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
bool same_list(
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

/// True when a chord can be written back onto this action.
///
/// A modifier on its own is only a chord of a modifier action. A chord whose
/// text does not parse back to the same chord is shown and not applied, so a
/// later save can still read the layout.
///
/// @param chord the chord
/// @param action the registry row, or null
/// @return true when IMPORT may write it
bool can_apply(const keys::Chord& chord, const acts::Action* action) {
    if (chord.kind == keys::TriggerKind::modifier_alone) {
        if (action == nullptr || action->kind != acts::Kind::modifier)
            return false;
    }
    const std::string text = keys::chord_text(chord);
    keys::ChordError error{};
    const std::optional<keys::Chord> parsed = keys::parse_chord(text, error);
    return parsed.has_value() && *parsed == chord;
}

/// The actions that hold one of these chords, other than the action being bound.
///
/// @param editor the editor
/// @param action the action that would gain the chords
/// @param chords the chords
/// @return the other actions, joined by a comma
std::string
losers_of(const Editor& editor, std::string_view action, const std::vector<keys::Chord>& chords) {
    const std::vector<Row> listed = editor.rows({}, Filter::all, "");
    std::vector<std::string> names;
    for (const keys::Chord& chord : chords) {
        for (const Row& row : listed) {
            if (row.id == action)
                continue;
            bool holds = false;
            for (const keys::Chord& bound : row.chords) {
                if (keys::same_key(bound, chord, &editor.keyboard()))
                    holds = true;
            }
            if (!holds)
                continue;
            bool seen = false;
            for (const std::string& name : names) {
                if (name == row.id)
                    seen = true;
            }
            if (!seen)
                names.push_back(row.id);
        }
    }
    std::string joined;
    for (const std::string& name : names) {
        if (!joined.empty())
            joined += ", ";
        joined += name;
    }
    return joined;
}

/// Takes a chord off an action, leaving whatever else it holds.
///
/// @param[in,out] editor the draft
/// @param id the action that loses the chord
/// @param chord the chord
void take_from(Editor& editor, std::string_view id, const keys::Chord& chord) {
    const Details shown = editor.details(id);
    if (shown.id.empty() || shown.read_only)
        return;
    std::vector<keys::Chord> remain;
    bool held = false;
    for (const ChordDetail& one : shown.chords) {
        if (keys::same_key(one.chord, chord, &editor.keyboard()))
            held = true;
        else
            remain.push_back(one.chord);
    }
    if (held)
        editor.set_chords(id, std::move(remain));
}

} // namespace

ImportPreview import_preview(
    const Editor& editor, const keys::LegacyRead& read, std::string_view tool, std::string_view file
) {
    ImportPreview preview;
    preview.problems = read.problems;
    preview.found = read.keys.size();
    preview.tool = std::string{tool};
    preview.file = std::string{file};
    for (const keys::LegacyKey& key : read.keys) {
        const acts::Action* action = key.action.empty() ? nullptr : acts::find(key.action);
        std::vector<keys::Chord> applicable;
        applicable.reserve(key.chords.size());
        for (const keys::Chord& chord : key.chords) {
            if (can_apply(chord, action))
                applicable.push_back(chord);
        }
        ImportRow row;
        row.action = key.action;
        row.source_line = key.source_line;
        row.line = key.line;
        row.note = key.note;
        row.unknown = !key.action.empty() && action == nullptr;
        row.unused = key.action.empty();
        row.before =
            key.action.empty() ? std::vector<keys::Chord>{} : chords_in_use(editor, key.action);
        if (row.unused || applicable.empty()) {
            if (row.unused || !row.note.empty())
                preview.rows.push_back(std::move(row));
            continue;
        }
        if (same_list(editor, row.before, applicable))
            continue;
        row.after = std::move(applicable);
        row.differs = true;
        row.loses = losers_of(editor, row.action, row.after);
        ++preview.differ;
        preview.rows.push_back(std::move(row));
    }
    return preview;
}

void apply_import(Editor& editor, const ImportPreview& preview) {
    const EditStep step(editor);
    for (const ImportRow& row : preview.rows) {
        if (!row.differs || row.action.empty())
            continue;
        const std::vector<Row> listed = editor.rows({}, Filter::all, "");
        for (const keys::Chord& chord : row.after) {
            for (const Row& have : listed) {
                if (have.id == row.action)
                    continue;
                bool holds = false;
                for (const keys::Chord& bound : have.chords) {
                    if (keys::same_key(bound, chord, &editor.keyboard()))
                        holds = true;
                }
                if (holds)
                    take_from(editor, have.id, chord);
            }
        }
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
        } else {
            editor.set_chords(row.action, row.after);
        }
    }
    if (!preview.tool.empty() && !preview.file.empty() && !preview.date.empty()) {
        oakeys::ImportedFrom from;
        from.tool = preview.tool;
        from.file = preview.file;
        from.date = preview.date;
        editor.imported_from_ = std::move(from);
    }
}

} // namespace oa::ui::key_editor
