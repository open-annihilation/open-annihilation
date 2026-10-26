// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Settling a chord that is taken: REPLACE, SWAP, and the draft card's
// KEEP IT HERE and KEEP IT FOR. Finding every action on one chord.
#include "oa/ui/key_editor/model.hpp"

#include "oa/ui/key_bindings/chord.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::key_editor {
namespace {

namespace keys = oa::ui::key_bindings;

/// How many chords one action may keep.
constexpr std::size_t kChordLimit = 4;

/// A character chord with no position, so it matches a parsed chord.
///
/// @param chord the chord
/// @return the chord a clash compares
keys::Chord comparable(keys::Chord chord) {
    if (chord.kind == keys::TriggerKind::character)
        chord.scancode = 0;
    return chord;
}

/// True when two chords are the key the player pressed, including Primary's pair.
///
/// @param editor the keyboard and the platform
/// @param bound a chord in use
/// @param wanted the chord being asked
/// @return true when they are one key
bool matches(const Editor& editor, const keys::Chord& bound, const keys::Chord& wanted) {
    return chords_match(bound, wanted, editor.keyboard(), editor.platform());
}

/// The chords an action would keep after one chord is dropped.
///
/// @param editor the keyboard
/// @param id the action
/// @param drop the chord to drop
/// @param[out] had_own the action had a chord of its own
/// @param[out] own_remains a chord of its own would remain
/// @return the chords that remain
std::vector<keys::Chord> without_chord(
    const Editor& editor,
    std::string_view id,
    const keys::Chord& drop,
    bool& had_own,
    bool& own_remains
) {
    had_own = false;
    own_remains = false;
    std::vector<keys::Chord> remain;
    const Details shown = editor.details(id);
    for (const ChordDetail& one : shown.chords) {
        const bool own = one.step == keys::Step::own;
        if (own)
            had_own = true;
        if (matches(editor, one.chord, drop))
            continue;
        remain.push_back(comparable(one.chord));
        if (own)
            own_remains = true;
    }
    return remain;
}

/// Drops a chord from an action by writing what remains, even when nothing remains.
///
/// @param[in,out] editor the draft
/// @param id the action
/// @param drop the chord
/// @return true when the draft changed
bool drop_for_replace(Editor& editor, std::string_view id, const keys::Chord& drop) {
    bool had_own = false;
    bool own_remains = false;
    const std::vector<keys::Chord> remain = without_chord(editor, id, drop, had_own, own_remains);
    static_cast<void>(had_own);
    static_cast<void>(own_remains);
    return editor.set_chords(id, remain);
}

/// Drops a chord the way the card does: an action left with no chord of its
/// own returns to the layout under it.
///
/// @param[in,out] editor the draft
/// @param id the action that loses the chord
/// @param drop the chord
/// @return true when the draft changed
bool drop_for_card(Editor& editor, std::string_view id, const keys::Chord& drop) {
    bool had_own = false;
    bool own_remains = false;
    const std::vector<keys::Chord> remain = without_chord(editor, id, drop, had_own, own_remains);
    if (had_own && !own_remains)
        return editor.clear_chords(id);
    return editor.set_chords(id, remain);
}

/// The other action of a clash whose `a` is the action being bound.
///
/// @param clash the clash
/// @param action the action being bound
/// @return the other action id
std::string other_of(const keys::Clash& clash, std::string_view action) {
    if (clash.a == action)
        return clash.b;
    return clash.a;
}

} // namespace

ApplyResult apply_capture(Editor& editor, const Capture& capture, ClashChoice choice) {
    ApplyResult result;
    if (!capture.chord)
        return result;
    if (capture.state == CaptureState::waiting || capture.state == CaptureState::refused)
        return result;
    if (choice == ClashChoice::swap && !capture.replacing)
        return result;
    const Details current = editor.details(capture.action);
    if (current.id.empty() || current.read_only)
        return result;
    const keys::Chord chord = comparable(*capture.chord);
    std::vector<keys::Chord> next;
    keys::Chord replaced;
    bool have_replaced = false;
    if (capture.replacing) {
        if (*capture.replacing >= current.chords.size())
            return result;
        replaced = comparable(current.chords[*capture.replacing].chord);
        have_replaced = true;
        next.reserve(current.chords.size());
        for (std::size_t index = 0; index < current.chords.size(); ++index) {
            if (index == *capture.replacing)
                next.push_back(chord);
            else
                next.push_back(comparable(current.chords[index].chord));
        }
    } else {
        if (current.chords.size() >= kChordLimit)
            return result;
        next.reserve(current.chords.size() + 1);
        for (const ChordDetail& one : current.chords)
            next.push_back(comparable(one.chord));
        next.push_back(chord);
    }
    const std::vector<keys::Clash> partners = editor.clashes_for(capture.action, chord);
    if (choice == ClashChoice::swap) {
        if (!have_replaced || partners.empty())
            return result;
        const std::string partner = other_of(partners.front(), capture.action);
        const Details other = editor.details(partner);
        if (other.id.empty() || other.read_only)
            return result;
        std::vector<keys::Chord> swapped;
        bool found = false;
        swapped.reserve(other.chords.size());
        for (const ChordDetail& one : other.chords) {
            if (!found && matches(editor, one.chord, chord)) {
                swapped.push_back(replaced);
                found = true;
            } else {
                swapped.push_back(comparable(one.chord));
            }
        }
        if (!found || swapped.size() > kChordLimit)
            return result;
        if (!editor.set_chords(partner, std::move(swapped)))
            return result;
        if (!editor.set_chords(capture.action, std::move(next)))
            return result;
        result.applied = true;
        result.select = capture.action;
        return result;
    }
    for (const keys::Clash& partner : partners) {
        const std::string other = other_of(partner, capture.action);
        if (other == capture.action)
            continue;
        drop_for_replace(editor, other, chord);
    }
    if (!editor.set_chords(capture.action, std::move(next)))
        return result;
    result.applied = true;
    if (partners.empty())
        result.select = capture.action;
    else
        result.select = other_of(partners.front(), capture.action);
    return result;
}

bool settle(Editor& editor, const keys::Clash& clash, Settle which) {
    const std::string keeper = which == Settle::keep_here ? clash.a : clash.b;
    const std::string loser = which == Settle::keep_here ? clash.b : clash.a;
    static_cast<void>(keeper);
    if (loser.empty())
        return false;
    return drop_for_card(editor, loser, clash.chord);
}

std::vector<ChordUse> find_by_chord(const Editor& editor, const keys::Chord& chord) {
    const keys::Chord asked = comparable(chord);
    const std::vector<Row> rows = editor.rows({}, Filter::all, "");
    std::vector<ChordUse> uses;
    auto add_id = [&](const std::string& id) {
        if (id.empty())
            return;
        for (const ChordUse& have : uses) {
            if (have.id == id)
                return;
        }
        for (const Row& listed : rows) {
            if (listed.id != id)
                continue;
            ChordUse made;
            made.id = listed.id;
            made.name = listed.name;
            made.context = context_phrase(editor, listed.id);
            uses.push_back(std::move(made));
            return;
        }
    };
    for (const Row& row : rows) {
        bool hit = false;
        for (const keys::Chord& bound : row.chords) {
            if (matches(editor, bound, asked))
                hit = true;
        }
        if (hit)
            add_id(row.id);
    }
    // The draft took the chord, so the loser no longer shows it. The search
    // still names that action.
    for (const DraftTake& take : editor.draft_takes()) {
        if (!matches(editor, take.chord, asked))
            continue;
        add_id(take.winner);
        add_id(take.loser);
    }
    return uses;
}

} // namespace oa::ui::key_editor
