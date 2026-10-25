// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/key_bindings/layout_file.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/ui/key_bindings/chord.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::key_bindings {
namespace {

namespace actions = oa::data::input_actions;
namespace oakeys = oa::formats::oakeys;

/// The sentence for a binding that leaves Escape off a fixed action, or
/// gives Escape to another action in a context that meets it.
constexpr std::string_view escape_warning =
    "Escape always closes the chat line and Open Annihilation's screens";

/// What the registry or a family says about one action id.
struct ActionFacts {
    const actions::Action* registry{nullptr}; ///< the action, when the registry has it
    bool known{false};                        ///< the registry or a family makes this id
    actions::Kind kind{actions::Kind::press}; ///< how it fires
    uint32_t context{0};                      ///< bits over the contexts
    bool fixed{false};                        ///< a fixed action
};

/// The family that makes an id, when the id is one a family makes.
///
/// @param id the id a layout wrote
/// @return the family, or null
const actions::Family* family_of(std::string_view id) {
    std::string_view wanted;
    constexpr std::string_view gui = "gui.";
    if (id.size() > gui.size() && id.starts_with(gui)) {
        wanted = "gui";
    } else {
        constexpr std::string_view category = "select.category-";
        if (id.size() != category.size() + 1 || !id.starts_with(category))
            return nullptr;
        const char letter = id.back();
        const bool lower = letter >= 'a' && letter <= 'z';
        const bool upper = letter >= 'A' && letter <= 'Z';
        if (!lower && !upper)
            return nullptr;
        wanted = "select.category";
    }
    for (const actions::Family& family : actions::families()) {
        if (family.id == wanted)
            return &family;
    }
    return nullptr;
}

/// Looks up an action id. An alias is the action it names, and the written
/// id is left as it was.
///
/// @param id the id a layout wrote
/// @return what this build knows about it
ActionFacts facts_of(std::string_view id) {
    ActionFacts facts;
    facts.registry = actions::find(id);
    if (facts.registry != nullptr) {
        facts.known = true;
        facts.kind = facts.registry->kind;
        facts.context = facts.registry->context;
        facts.fixed = facts.registry->fixed;
        return facts;
    }
    const actions::Family* family = family_of(id);
    if (family != nullptr) {
        facts.known = true;
        facts.kind = family->kind;
        facts.context = family->context;
    }
    return facts;
}

/// One chord already seen in the file, and the action that bound it first.
struct SeenChord {
    Chord chord{};        ///< the chord
    std::string action{}; ///< the action that bound it first
};

/// The path of one chord.
///
/// @param action the action id
/// @param chord_index the 0-based index in that action's list
/// @return bindings.<action>[index]
std::string chord_path(std::string_view action, std::size_t chord_index) {
    return "bindings." + std::string{action} + "[" + std::to_string(chord_index) + "]";
}

/// Appends one problem.
///
/// @param[in,out] found the problems
/// @param line the line
/// @param column the column
/// @param key the path
/// @param message the sentence
/// @param warning whether the binding is kept
void add_problem(
    std::vector<LayoutProblem>& found,
    uint32_t line,
    uint32_t column,
    std::string key,
    std::string message,
    bool warning
) {
    found.push_back(LayoutProblem{line, column, std::move(key), std::move(message), warning});
}

} // namespace

bool layout_from_manifest(
    const oakeys::Manifest& manifest,
    const KeyboardMap* keyboard,
    Layout& layout,
    std::vector<LayoutProblem>& problems
) {
    ChordError escape_error{};
    const std::optional<Chord> escape = parse_chord("Escape", escape_error);

    std::vector<LayoutBinding> built;
    std::vector<LayoutProblem> found;
    std::vector<SeenChord> seen;
    bool failed = false;

    for (const oakeys::BindingEntry& binding : manifest.bindings) {
        const ActionFacts facts = facts_of(binding.action);
        const std::string binding_path = "bindings." + binding.action;
        if (!facts.known) {
            add_problem(
                found,
                binding.line,
                binding.column,
                binding_path,
                "Not in this version of Open Annihilation",
                true
            );
        }

        LayoutBinding row;
        row.action = binding.action;
        row.line = binding.line;
        row.column = binding.column;
        bool has_escape = false;

        for (std::size_t chord_index = 0; chord_index < binding.chords.size(); ++chord_index) {
            const oakeys::ChordText& written = binding.chords[chord_index];
            const std::string path = chord_path(binding.action, chord_index);
            ChordError chord_error{};
            const std::optional<Chord> parsed = parse_chord(written.text, chord_error);
            if (!parsed) {
                add_problem(
                    found,
                    written.line,
                    written.column,
                    path,
                    std::string{chord_error_text(chord_error.reason)},
                    false
                );
                failed = true;
                continue;
            }
            if (parsed->kind == TriggerKind::modifier_alone && facts.known &&
                facts.kind != actions::Kind::modifier) {
                add_problem(
                    found,
                    written.line,
                    written.column,
                    path,
                    "a modifier on its own is not a chord of this action",
                    false
                );
                failed = true;
                continue;
            }
            if (keyboard != nullptr && !on_keyboard(*parsed, *keyboard)) {
                add_problem(
                    found, written.line, written.column, path, "Not on this keyboard", true
                );
            }
            if (escape.has_value() && same_key(*parsed, *escape, keyboard)) {
                has_escape = true;
                for (const actions::Action& action : actions::actions()) {
                    if (!action.fixed)
                        continue;
                    if (facts.registry != nullptr && facts.registry->id == action.id)
                        continue;
                    if (!facts.known || !layers_meet(facts.context, action.context))
                        continue;
                    add_problem(
                        found, written.line, written.column, path, std::string{escape_warning}, true
                    );
                }
            }
            for (const SeenChord& earlier : seen) {
                if (earlier.chord == *parsed) {
                    add_problem(
                        found,
                        written.line,
                        written.column,
                        path,
                        binding.action + " and " + earlier.action + " both bind " +
                            chord_text(*parsed),
                        true
                    );
                    break;
                }
            }
            seen.push_back(SeenChord{*parsed, binding.action});
            row.chords.push_back(*parsed);
        }

        if (facts.fixed && !has_escape) {
            add_problem(
                found, binding.line, binding.column, binding_path, std::string{escape_warning}, true
            );
        }
        built.push_back(std::move(row));
    }

    problems.insert(problems.end(), found.begin(), found.end());
    if (failed)
        return false;

    layout.id = manifest.id;
    layout.base = manifest.base == oakeys::Base::none ? Base::none : Base::oa_default;
    layout.bindings = std::move(built);
    return true;
}

oakeys::Manifest manifest_from_layout(const Layout& layout, const oakeys::Manifest& rest) {
    oakeys::Manifest manifest = rest;
    manifest.id = layout.id;
    manifest.base = layout.base == Base::none ? oakeys::Base::none : oakeys::Base::oa_default;
    manifest.bindings.clear();
    for (const LayoutBinding& binding : layout.bindings) {
        oakeys::BindingEntry entry;
        entry.action = binding.action;
        entry.line = binding.line;
        entry.column = binding.column;
        for (const Chord& chord : binding.chords) {
            oakeys::ChordText text;
            text.text = chord_text(chord);
            text.line = binding.line;
            text.column = binding.column;
            entry.chords.push_back(std::move(text));
        }
        manifest.bindings.push_back(std::move(entry));
    }
    return manifest;
}

} // namespace oa::ui::key_bindings
