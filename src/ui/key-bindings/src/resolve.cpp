// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Resolution order: which actions exist, the base, the chosen layout, the
// mod's letters, the player's own changes, the number-key swap, then fixed
// keys and the four-chord limit. Reads nothing but ResolveInput and the
// registry.

#include "oa/ui/key_bindings/layout.hpp"

#include "overlap.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::key_bindings {
namespace {

namespace actions = oa::data::input_actions;

using actions::Action;
using actions::Family;
using actions::Kind;
using actions::Target;

/// At most this many chords stay on one action.
constexpr std::size_t kChordLimit = 4;

/// A chord while it is being resolved, including where a layout named it.
struct Held {
    Chord chord{};
    Step step{};
    uint32_t line{};
    uint32_t column{};
};

/// One action through the steps.
struct Row {
    std::string id{};
    const Action* entry{};
    uint32_t contexts{};
    uint32_t when{};
    bool hack{};
    int precedence{};
    Kind kind{Kind::press};
    Target target{Target::none};
    bool fixed{};
    std::string replaces{};
    std::vector<Held> chords{};
};

/// Reports whether a hack id is in the list. The list's order is ignored.
///
/// @param hacks_on the hacks that are on
/// @param hack the id
/// @return true when the list holds it
bool hack_listed(const std::vector<std::string>& hacks_on, std::string_view hack) {
    for (const std::string& listed : hacks_on) {
        if (listed == hack)
            return true;
    }
    return false;
}

/// A source that is part of the game even with every hack off.
///
/// @param source one source name
/// @return true for 3.1c, OA or data
bool permanent_source(std::string_view source) {
    return source == "3.1c" || source == "OA" || source == "data";
}

/// Reports whether the action has a 3.1c, OA or data source.
///
/// @param action the action
/// @return true when it exists with every hack off
bool any_permanent(const Action& action) {
    for (const std::string_view source : action.source) {
        if (permanent_source(source))
            return true;
    }
    return false;
}

/// Reports whether a hack source of the action is on.
///
/// @param action the action
/// @param hacks_on the hacks that are on
/// @return true when a source hack is on
bool any_hack_source(const Action& action, const std::vector<std::string>& hacks_on) {
    for (const std::string_view source : action.source) {
        if (!permanent_source(source) && hack_listed(hacks_on, source))
            return true;
    }
    return false;
}

/// Reports whether a hack that is on has an entry on the action.
///
/// @param action the action
/// @param hacks_on the hacks that are on
/// @return true when an entry's hack is on
bool hack_changes(const Action& action, const std::vector<std::string>& hacks_on) {
    for (const actions::HackChange& change : action.hacks) {
        if (hack_listed(hacks_on, change.hack))
            return true;
    }
    return false;
}

/// The condition bits after every hack entry that is on, in registry order.
///
/// @param action the action
/// @param hacks_on the hacks that are on
/// @return the bits
uint32_t effective_when(const Action& action, const std::vector<std::string>& hacks_on) {
    uint32_t when = action.when;
    for (const actions::HackChange& change : action.hacks) {
        if (!hack_listed(hacks_on, change.hack))
            continue;
        if (change.sets_when)
            when = change.when;
    }
    return when;
}

/// True when the action exists only because a hack is on, or a hack changes it.
///
/// @param action the action
/// @param hacks_on the hacks that are on
/// @return the flag stored on the bound action
bool hack_flag(const Action& action, const std::vector<std::string>& hacks_on) {
    if (!any_permanent(action) && any_hack_source(action, hacks_on))
        return true;
    return hack_changes(action, hacks_on);
}

/// Parses one chord text. A refusal becomes no chord.
///
/// @param text the chord text
/// @return the chord, or nothing
std::optional<Chord> parsed(std::string_view text) {
    ChordError error;
    return parse_chord(text, error);
}

/// The base chords of one registry action, before a replacer copies another's.
///
/// @param action the action
/// @param input the resolution input
/// @return the chords, each marked as the base
std::vector<Held> own_base(const Action& action, const ResolveInput& input) {
    std::vector<Held> chords;
    if (input.base == Base::none)
        return chords;
    const auto add = [&chords](std::string_view text) {
        const std::optional<Chord> chord = parsed(text);
        if (!chord)
            return;
        Held held;
        held.chord = *chord;
        held.step = Step::base;
        chords.push_back(held);
    };
    if (input.scheme == PointerScheme::left_click && !action.left_click_default.empty()) {
        add(action.left_click_default);
        return chords;
    }
    bool hack_default = false;
    std::span<const std::string_view> texts;
    for (const actions::HackChange& change : action.hacks) {
        if (!hack_listed(input.hacks_on, change.hack) || !change.sets_default)
            continue;
        texts = change.defaults;
        hack_default = true;
    }
    const std::span<const std::string_view> chosen = hack_default ? texts : action.defaults;
    for (const std::string_view text : chosen)
        add(text);
    return chords;
}

/// The bit of a context, or 0 when the name is not one.
///
/// @param name the context id
/// @return the bit
uint32_t context_bit(std::string_view name) {
    const std::span<const actions::Context> table = actions::contexts();
    for (std::size_t index = 0; index < table.size(); ++index) {
        if (table[index].name == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// Finds a row by id.
///
/// @param rows the rows
/// @param id the action id
/// @return the row, or null
Row* find_row(std::vector<Row>& rows, std::string_view id) {
    for (Row& row : rows) {
        if (row.id == id)
            return &row;
    }
    return nullptr;
}

/// The family with this id, or null.
///
/// @param id `gui` or `select.category`
/// @return the family
const Family* family_named(std::string_view id) {
    for (const Family& family : actions::families()) {
        if (family.id == id)
            return &family;
    }
    return nullptr;
}

/// Folds an ASCII letter. Any other byte is unchanged.
///
/// @param byte the byte
/// @return the folded byte
unsigned char fold_byte(unsigned char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<unsigned char>(byte - 'A' + 'a');
    return byte;
}

/// Reports whether the gadget name holds the word, ignoring ASCII case.
///
/// @param gadget the gadget name
/// @param word the registry word
/// @return true when the word is a substring
bool holds_word(std::string_view gadget, std::string_view word) {
    if (word.empty() || word.size() > gadget.size())
        return false;
    for (std::size_t start = 0; start + word.size() <= gadget.size(); ++start) {
        bool match = true;
        for (std::size_t index = 0; index < word.size(); ++index) {
            const unsigned char hay = static_cast<unsigned char>(gadget[start + index]);
            const unsigned char needle = static_cast<unsigned char>(word[index]);
            if (fold_byte(hay) != fold_byte(needle)) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

/// The registry action whose gadget word is the longest one the name holds.
///
/// @param gadget the gadget name
/// @return that action, or null
const Action* gadget_action(std::string_view gadget) {
    if (gadget.empty())
        return nullptr;
    const Action* best = nullptr;
    std::size_t best_length = 0;
    for (const Action& action : actions::actions()) {
        if (action.gadget.empty() || action.gadget.size() <= best_length)
            continue;
        if (!holds_word(gadget, action.gadget))
            continue;
        best = &action;
        best_length = action.gadget.size();
    }
    return best;
}

/// A one-character chord, with a letter in capitals.
///
/// @param letter the quick key
/// @return the chord, or nothing when it is not a chord
std::optional<Chord> letter_chord(char32_t letter) {
    if (letter >= U'a' && letter <= U'z')
        letter = static_cast<char32_t>(letter - U'a' + U'A');
    if (letter < U' ' || letter > U'~')
        return std::nullopt;
    const std::string text(1, static_cast<char>(letter));
    return parsed(text);
}

/// True when a family makes this id, whether or not this mod did.
///
/// @param id the id a layout wrote
/// @return true for `gui.` and `select.category-` plus one letter
bool family_makes(std::string_view id) {
    constexpr std::string_view gui = "gui.";
    if (id.size() > gui.size() && id.starts_with(gui))
        return true;
    constexpr std::string_view category = "select.category-";
    if (id.size() != category.size() + 1 || !id.starts_with(category))
        return false;
    const char letter = id.back();
    const bool lower = letter >= 'a' && letter <= 'z';
    const bool upper = letter >= 'A' && letter <= 'Z';
    return lower || upper;
}

/// The canonical id, when the registry knows the name or an old one.
///
/// @param written the id a layout wrote
/// @return the registry id, or the written text
std::string canonical_id(std::string_view written) {
    if (const Action* found = actions::find(written))
        return std::string{found->id};
    return std::string{written};
}

/// Reports whether the id is in the list.
///
/// @param names the ids
/// @param id the id
/// @return true when the list holds it
bool listed(const std::vector<std::string>& names, std::string_view id) {
    for (const std::string& name : names)
        if (name == id)
            return true;
    return false;
}

/// A side for the clash rule, from a row.
///
/// @param row the row
/// @return the side
Side side_of(const Row& row) {
    Side side;
    side.id = row.id;
    side.contexts = row.contexts;
    side.when = row.when;
    side.precedence = row.precedence;
    side.kind = row.kind;
    side.target = row.target;
    return side;
}

/// True when the two chords are a clash, and not a shadow.
///
/// @param row the action taking the chord
/// @param chord the chord
/// @param other the action that already has one
/// @param held that chord
/// @param input the scheme and the keyboard
/// @return true when they clash
bool is_clash(
    const Row& row,
    const Chord& chord,
    const Row& other,
    const Held& held,
    const ResolveInput& input
) {
    const Relation relation = relate(side_of(row), chord, side_of(other), held.chord, input);
    return relation.overlap == Overlap::clash;
}

/// Appends a lost chord.
///
/// @param lost the list
/// @param action the action that lost it
/// @param chord the chord
/// @param step the step it came from
/// @param reason why
/// @param winner the action that kept it, or empty
/// @param line the binding's line
/// @param column the binding's column
void lose(
    std::vector<Lost>& lost,
    std::string action,
    const Chord& chord,
    Step step,
    LostReason reason,
    std::string winner,
    uint32_t line,
    uint32_t column
) {
    Lost entry;
    entry.action = std::move(action);
    entry.chord = chord;
    entry.step = step;
    entry.reason = reason;
    entry.winner = std::move(winner);
    entry.line = line;
    entry.column = column;
    lost.push_back(std::move(entry));
}

/// Applies one layout file: the chosen layout, or the player's own changes.
///
/// Within the file an earlier binding keeps the chord. Own changes also take
/// a chord from an earlier step. A name a family does not make, and that the
/// registry does not know, is kept in `unknown`.
///
/// @param layout the file
/// @param step layout or own
/// @param take_earlier true for the player's own changes
/// @param rows the actions
/// @param lost chords that lose
/// @param unknown names kept whole
/// @param input the scheme and the keyboard
void apply_file(
    const Layout& layout,
    Step step,
    bool take_earlier,
    std::vector<Row>& rows,
    std::vector<Lost>& lost,
    std::vector<LayoutBinding>& unknown,
    const ResolveInput& input
) {
    std::vector<std::string> named;
    named.reserve(layout.bindings.size());
    for (const LayoutBinding& binding : layout.bindings)
        named.push_back(canonical_id(binding.action));

    for (const LayoutBinding& binding : layout.bindings) {
        const Action* found = actions::find(binding.action);
        const std::string id = found != nullptr ? std::string{found->id} : binding.action;
        Row* row = find_row(rows, id);
        if (row == nullptr && found != nullptr) {
            for (Row& candidate : rows) {
                if (candidate.replaces == found->id && !listed(named, candidate.id)) {
                    row = &candidate;
                    break;
                }
            }
        }
        if (row == nullptr) {
            if (found != nullptr || family_makes(binding.action))
                continue;
            unknown.push_back(binding);
            continue;
        }

        row->chords.clear();
        for (const Chord& chord : binding.chords) {
            std::string winner;
            bool earlier = false;
            for (const Row& other : rows) {
                for (const Held& held : other.chords) {
                    const bool same_row = &other == row;
                    const bool blocks =
                        same_row ? same_key(held.chord, chord, input.keyboard)
                                 : held.step == step && is_clash(*row, chord, other, held, input);
                    if (!blocks)
                        continue;
                    earlier = true;
                    winner = other.id;
                    break;
                }
                if (earlier)
                    break;
            }
            if (earlier) {
                lose(
                    lost,
                    row->id,
                    chord,
                    step,
                    LostReason::earlier_in_file,
                    std::move(winner),
                    binding.line,
                    binding.column
                );
                continue;
            }
            if (take_earlier) {
                for (Row& other : rows) {
                    if (&other == row)
                        continue;
                    std::vector<Held> kept;
                    kept.reserve(other.chords.size());
                    for (const Held& held : other.chords) {
                        if (!is_clash(*row, chord, other, held, input)) {
                            kept.push_back(held);
                            continue;
                        }
                        lose(
                            lost,
                            other.id,
                            held.chord,
                            held.step,
                            LostReason::later_step,
                            row->id,
                            held.line,
                            held.column
                        );
                    }
                    other.chords = std::move(kept);
                }
            }
            Held held;
            held.chord = chord;
            held.step = step;
            held.line = binding.line;
            held.column = binding.column;
            row->chords.push_back(held);
        }
    }
}

/// The mod's letters, for actions the chosen layout does not name.
///
/// A registry gadget letter replaces that action's chords and is kept when
/// it meets another action's base chord: the clash stays visible. It loses
/// when a chosen-layout chord already has the key. A `gui.` letter loses
/// against either. The comparison uses the chords from before this step, so
/// a letter the mod itself just wrote is not what drops another.
///
/// @param mod the mod data
/// @param chosen the chosen layout, or null
/// @param rows the actions
/// @param lost chords that lose
/// @param input the scheme and the keyboard
void apply_mod(
    const ModData& mod,
    const Layout* chosen,
    std::vector<Row>& rows,
    std::vector<Lost>& lost,
    const ResolveInput& input
) {
    std::vector<std::string> named;
    if (chosen != nullptr) {
        for (const LayoutBinding& binding : chosen->bindings)
            named.push_back(canonical_id(binding.action));
    }
    std::vector<std::vector<Held>> earlier(rows.size());
    for (std::size_t index = 0; index < rows.size(); ++index)
        earlier[index] = rows[index].chords;

    for (const GuiKey& key : mod.keys) {
        const std::optional<Chord> letter = letter_chord(key.letter);
        if (!letter)
            continue;
        if (const Action* moved = gadget_action(key.gadget)) {
            if (!listed(named, moved->id)) {
                if (Row* row = find_row(rows, moved->id)) {
                    Held held;
                    held.chord = *letter;
                    held.step = Step::mod;
                    row->chords.clear();
                    row->chords.push_back(held);
                }
            }
        }
        if (!key.action.starts_with("gui.") || listed(named, key.action))
            continue;
        if (Row* row = find_row(rows, key.action)) {
            Held held;
            held.chord = *letter;
            held.step = Step::mod;
            row->chords.clear();
            row->chords.push_back(held);
        }
    }

    for (std::size_t index = 0; index < rows.size(); ++index) {
        Row& row = rows[index];
        const bool gui = row.entry == nullptr && row.id.starts_with("gui.");
        std::vector<Held> kept;
        kept.reserve(row.chords.size());
        for (const Held& held : row.chords) {
            if (held.step != Step::mod) {
                kept.push_back(held);
                continue;
            }
            std::string winner;
            bool dropped = false;
            for (std::size_t other_index = 0; other_index < rows.size(); ++other_index) {
                if (other_index == index)
                    continue;
                for (const Held& previous : earlier[other_index]) {
                    const bool against_layout = previous.step == Step::layout;
                    if (!gui && !against_layout)
                        continue;
                    if (!is_clash(row, held.chord, rows[other_index], previous, input))
                        continue;
                    dropped = true;
                    winner = rows[other_index].id;
                    break;
                }
                if (dropped)
                    break;
            }
            if (dropped) {
                lose(
                    lost,
                    row.id,
                    held.chord,
                    Step::mod,
                    LostReason::moved_by_layout,
                    std::move(winner),
                    held.line,
                    held.column
                );
                continue;
            }
            kept.push_back(held);
        }
        row.chords = std::move(kept);
    }
}

/// Flips the Alt bit of one chord. Select groups without Alt turns
/// Alt+Shift+1 into Shift+1, and a chord that named no Alt gains it.
///
/// @param chord the chord
void flip_alt(Chord& chord) {
    chord.modifiers = static_cast<uint8_t>(chord.modifiers ^ static_cast<uint8_t>(Modifier::alt));
}

/// Swaps group.select-n with build.page-n, then flips Alt on group.add-n.
///
/// A chord the flipped key lands on, in a layer the add meets, is dropped.
/// Where the page kept an Alt chord, it also keeps that chord with Shift, so
/// Alt+Shift+1 is an exact page and not a tie with the add. The layout and
/// the player's own changes are left as they were given.
///
/// @param rows the actions
/// @param lost chords that lose
/// @param input the keyboard the same-key test reads
void swap_number_keys(std::vector<Row>& rows, std::vector<Lost>& lost, const ResolveInput& input) {
    for (int number = 1; number <= 9; ++number) {
        const std::string suffix = std::to_string(number);
        Row* select_row = find_row(rows, "group.select-" + suffix);
        Row* page_row = find_row(rows, "build.page-" + suffix);
        if (select_row != nullptr && page_row != nullptr)
            std::swap(select_row->chords, page_row->chords);

        Row* add_row = find_row(rows, "group.add-" + suffix);
        if (add_row == nullptr)
            continue;
        for (Held& held : add_row->chords)
            flip_alt(held.chord);
        std::vector<Chord> landed;
        landed.reserve(add_row->chords.size());
        for (const Held& held : add_row->chords)
            landed.push_back(held.chord);
        const std::string winner = add_row->id;
        const uint32_t add_contexts = add_row->contexts;
        for (Row& other : rows) {
            if (&other == add_row || !layers_meet(other.contexts, add_contexts))
                continue;
            std::vector<Held> kept;
            kept.reserve(other.chords.size());
            for (const Held& held : other.chords) {
                bool landed_on = false;
                for (const Chord& chord : landed) {
                    if (same_key(held.chord, chord, input.keyboard))
                        landed_on = true;
                }
                if (!landed_on) {
                    kept.push_back(held);
                    continue;
                }
                lose(
                    lost,
                    other.id,
                    held.chord,
                    held.step,
                    LostReason::number_swap,
                    winner,
                    held.line,
                    held.column
                );
            }
            other.chords = std::move(kept);
        }
        // Alt+Shift+1 must be the page. The page's Alt+1 matches it only with
        // an extra Shift, and the add's Shift+1 matches it with an extra Alt,
        // so the two tie and the earlier action would win. An exact chord on
        // the page is searched first.
        if (page_row == nullptr)
            continue;
        const std::vector<Held> alt_chords = page_row->chords;
        for (const Held& held : alt_chords) {
            if (held.chord.modifiers != static_cast<uint8_t>(Modifier::alt))
                continue;
            Held extra = held;
            extra.chord.modifiers =
                static_cast<uint8_t>(extra.chord.modifiers | static_cast<uint8_t>(Modifier::shift));
            bool already = false;
            for (const Held& kept : page_row->chords) {
                if (same_key(kept.chord, extra.chord, input.keyboard))
                    already = true;
            }
            if (!already)
                page_row->chords.push_back(extra);
        }
    }
}

/// Puts each fixed action's registry chords first, and drops the same key
/// from any other action in a layer the fixed action meets.
///
/// @param rows the actions
/// @param lost chords that lose
/// @param input the keyboard
void apply_fixed(std::vector<Row>& rows, std::vector<Lost>& lost, const ResolveInput& input) {
    for (Row& row : rows) {
        if (!row.fixed || row.entry == nullptr)
            continue;
        std::vector<Chord> forced;
        for (const std::string_view text : row.entry->defaults) {
            const std::optional<Chord> chord = parsed(text);
            if (chord)
                forced.push_back(*chord);
        }
        std::vector<Held> rest;
        for (const Held& held : row.chords) {
            bool is_forced = false;
            for (const Chord& chord : forced) {
                if (same_key(held.chord, chord, input.keyboard))
                    is_forced = true;
            }
            if (!is_forced)
                rest.push_back(held);
        }
        std::vector<Held> front;
        for (const Chord& chord : forced) {
            bool found = false;
            for (const Held& held : row.chords) {
                if (!same_key(held.chord, chord, input.keyboard))
                    continue;
                front.push_back(held);
                found = true;
                break;
            }
            if (found)
                continue;
            Held held;
            held.chord = chord;
            held.step = Step::base;
            front.push_back(held);
        }
        front.insert(front.end(), rest.begin(), rest.end());
        row.chords = std::move(front);
    }

    for (std::size_t index = 0; index < rows.size(); ++index) {
        const Row& fixed_row = rows[index];
        if (!fixed_row.fixed || fixed_row.entry == nullptr)
            continue;
        std::vector<Chord> forced;
        for (const std::string_view text : fixed_row.entry->defaults) {
            const std::optional<Chord> chord = parsed(text);
            if (chord)
                forced.push_back(*chord);
        }
        for (std::size_t other_index = 0; other_index < rows.size(); ++other_index) {
            if (other_index == index)
                continue;
            Row& other = rows[other_index];
            if (!layers_meet(other.contexts, fixed_row.contexts))
                continue;
            std::vector<Held> kept;
            for (const Held& held : other.chords) {
                bool blocked = false;
                for (const Chord& chord : forced) {
                    if (same_key(held.chord, chord, input.keyboard))
                        blocked = true;
                }
                if (!blocked) {
                    kept.push_back(held);
                    continue;
                }
                lose(
                    lost,
                    other.id,
                    held.chord,
                    held.step,
                    LostReason::fixed,
                    fixed_row.id,
                    held.line,
                    held.column
                );
            }
            other.chords = std::move(kept);
        }
    }
}

/// Drops a fifth chord and later.
///
/// @param rows the actions
/// @param lost chords that lose
void trim_chords(std::vector<Row>& rows, std::vector<Lost>& lost) {
    for (Row& row : rows) {
        if (row.chords.size() <= kChordLimit)
            continue;
        for (std::size_t index = kChordLimit; index < row.chords.size(); ++index) {
            const Held& held = row.chords[index];
            lose(
                lost,
                row.id,
                held.chord,
                held.step,
                LostReason::too_many,
                std::string{},
                held.line,
                held.column
            );
        }
        row.chords.resize(kChordLimit);
    }
}

/// True when some registry row already binds the chord in a layer play meets.
///
/// @param rows the actions so far
/// @param chord the category chord
/// @param keyboard the map
/// @return true when the letter is taken
bool play_binds(const std::vector<Row>& rows, const Chord& chord, const KeyboardMap* keyboard) {
    const uint32_t play = context_bit("play");
    for (const Row& row : rows) {
        if (row.entry == nullptr || !layers_meet(row.contexts, play))
            continue;
        for (const Held& held : row.chords) {
            if (same_key(held.chord, chord, keyboard))
                return true;
        }
    }
    return false;
}

/// Adds the mod's made actions: GUI keys, then category letters in string order.
///
/// @param mod the mod data
/// @param rows the actions
/// @param input the base and the keyboard
void add_made(const ModData& mod, std::vector<Row>& rows, const ResolveInput& input) {
    const Family* gui = family_named("gui");
    for (const GuiKey& key : mod.keys) {
        if (!key.action.starts_with("gui.") || find_row(rows, key.action) != nullptr)
            continue;
        Row row;
        row.id = key.action;
        row.kind = gui != nullptr ? gui->kind : Kind::press;
        const bool order_panel = gadget_action(key.gadget) != nullptr;
        if (gui == nullptr)
            row.contexts = 0;
        else if (order_panel && gui->order_panel_context != 0)
            row.contexts = gui->order_panel_context;
        else
            row.contexts = gui->context;
        rows.push_back(std::move(row));
    }

    const Family* category = family_named("select.category");
    for (const char byte : mod.categories) {
        char lower = 0;
        char upper = 0;
        if (byte >= 'a' && byte <= 'z') {
            lower = byte;
            upper = static_cast<char>(byte - 'a' + 'A');
        } else if (byte >= 'A' && byte <= 'Z') {
            upper = byte;
            lower = static_cast<char>(byte - 'A' + 'a');
        } else {
            continue;
        }
        std::string id = "select.category-";
        id.push_back(lower);
        if (find_row(rows, id) != nullptr)
            continue;
        const std::string text = std::string{"Ctrl+"} + upper;
        const std::optional<Chord> chord = parsed(text);
        if (!chord)
            continue;
        if (play_binds(rows, *chord, input.keyboard))
            continue;
        Row row;
        row.id = std::move(id);
        row.kind = category != nullptr ? category->kind : Kind::press;
        row.contexts = category != nullptr ? category->context : 0;
        if (input.base != Base::none) {
            Held held;
            held.chord = *chord;
            held.step = Step::base;
            row.chords.push_back(held);
        }
        rows.push_back(std::move(row));
    }
}

/// The registry rows that exist, with base chords, replacers already copied.
///
/// @param input the resolution input
/// @return the rows, in registry order
std::vector<Row> registry_rows(const ResolveInput& input) {
    struct Draft {
        const Action* entry{};
        bool live{};
        bool hidden{};
        uint32_t when{};
        int precedence{};
        bool hack{};
        std::vector<Held> chords{};
    };

    std::vector<Draft> drafts;
    const std::span<const Action> table = actions::actions();
    drafts.reserve(table.size());
    for (const Action& action : table) {
        Draft draft;
        draft.entry = &action;
        draft.live = any_permanent(action) || any_hack_source(action, input.hacks_on);
        draft.when = effective_when(action, input.hacks_on);
        draft.precedence = action_precedence(action, input.hacks_on);
        draft.hack = hack_flag(action, input.hacks_on);
        if (draft.live)
            draft.chords = own_base(action, input);
        drafts.push_back(std::move(draft));
    }
    for (const Draft& draft : drafts) {
        if (!draft.live || any_permanent(*draft.entry) || draft.entry->replaces.empty())
            continue;
        for (Draft& replaced : drafts) {
            if (replaced.entry->id == draft.entry->replaces)
                replaced.hidden = true;
        }
    }
    for (Draft& draft : drafts) {
        if (!draft.live || draft.hidden || draft.entry->replaces.empty())
            continue;
        for (const Draft& replaced : drafts) {
            if (replaced.entry->id != draft.entry->replaces)
                continue;
            draft.chords = replaced.chords;
            break;
        }
    }
    std::vector<Row> rows;
    for (Draft& draft : drafts) {
        if (!draft.live || draft.hidden)
            continue;
        Row row;
        row.id = std::string{draft.entry->id};
        row.entry = draft.entry;
        row.contexts = draft.entry->context;
        row.when = draft.when;
        row.hack = draft.hack;
        row.precedence = draft.precedence;
        row.kind = draft.entry->kind;
        row.target = draft.entry->target;
        row.fixed = draft.entry->fixed;
        row.replaces = std::string{draft.entry->replaces};
        row.chords = std::move(draft.chords);
        rows.push_back(std::move(row));
    }
    return rows;
}

/// Copies a row out as a bound action.
///
/// @param row the row
/// @return the bound action
Bound bound_of(Row& row) {
    Bound bound;
    bound.id = std::move(row.id);
    bound.entry = row.entry;
    bound.contexts = row.contexts;
    bound.when = row.when;
    bound.hack = row.hack;
    bound.chords.reserve(row.chords.size());
    for (const Held& held : row.chords) {
        BoundChord chord;
        chord.chord = held.chord;
        chord.step = held.step;
        bound.chords.push_back(chord);
    }
    return bound;
}

} // namespace

Resolved resolve(const ResolveInput& input) {
    std::vector<Row> rows = registry_rows(input);
    if (input.mod != nullptr)
        add_made(*input.mod, rows, input);

    Resolved resolved;
    if (input.chosen != nullptr)
        apply_file(
            *input.chosen, Step::layout, false, rows, resolved.lost, resolved.unknown, input
        );
    if (input.mod != nullptr)
        apply_mod(*input.mod, input.chosen, rows, resolved.lost, input);
    if (input.own != nullptr)
        apply_file(*input.own, Step::own, true, rows, resolved.lost, resolved.unknown, input);
    if (input.number_keys_select_groups)
        swap_number_keys(rows, resolved.lost, input);
    apply_fixed(rows, resolved.lost, input);
    trim_chords(rows, resolved.lost);

    resolved.actions.reserve(rows.size());
    for (Row& row : rows)
        resolved.actions.push_back(bound_of(row));
    return resolved;
}

} // namespace oa::ui::key_bindings
