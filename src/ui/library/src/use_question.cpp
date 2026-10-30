// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/library/use_question.hpp"

#include "oa/ui/library/text.hpp"

#include <string>
#include <utility>

namespace oa::ui::library {

namespace {

/// The player's interface scheme. Empty means right-click.
///
/// @param inputs the Library's inputs
/// @return "right-click" or "left-click"
std::string_view player_pointer(const Inputs& inputs) {
    if (inputs.player_pointer.empty())
        return "right-click";
    return inputs.player_pointer;
}

} // namespace

void open_key_use(Library& library, const Entry& entry) {
    KeyUse use;
    use.open = true;
    use.layout = entry.id;
    const TextHooks& text = library.text;
    const Inputs& inputs = library.inputs;

    const Entry* playing = nullptr;
    for (const Entry& other : library.entries) {
        if (layout_in_use(library, other)) {
            playing = &other;
            break;
        }
    }
    uint32_t own = 0;
    if (playing != nullptr) {
        use.saved_with = std::string(entry_name(*playing));
        if (const KeyLayoutFacts* facts = key_facts(*playing))
            own = facts->own_changes;
    }
    if (own == 0)
        own = !inputs.keys_for_mod.empty() ? inputs.keys_for_mod_own : inputs.keys_own_changes;
    if (own > 0) {
        use.show_own = true;
        use.keep = true;
        use.own_count = own;
    }

    if (const KeyLayoutFacts* facts = key_facts(entry)) {
        if (facts->own_changes > 0) {
            use.already = true;
            use.already_count = facts->own_changes;
            use.already_name = std::string(entry_name(entry));
        }
        for (const std::string& token : facts->helpers) {
            KeyHelperSwitch one;
            one.token = token;
            one.name = key_helper_name(token);
            one.label = key_helper_label(token, text);
            one.on = true;
            use.helpers.push_back(std::move(one));
        }
        const std::string_view yours = player_pointer(inputs);
        if (!facts->pointer.empty() && facts->pointer != "any" && facts->pointer != yours) {
            use.pointer_note = filled(
                text,
                words::use_pointer_mismatch,
                {{"layout", std::string(facts->pointer)}, {"yours", std::string(yours)}}
            );
        }
        if (!inputs.played.id.empty()) {
            use.show_scope = true;
            use.mod_name = inputs.played.name.empty() ? inputs.played.id : inputs.played.name;
            use.for_mod = layout_made_for_played(inputs, facts->works_with);
        }
    } else if (!inputs.played.id.empty()) {
        use.show_scope = true;
        use.mod_name = inputs.played.name.empty() ? inputs.played.id : inputs.played.name;
    }
    if (library.key_hooks.moves) {
        std::string from;
        if (playing != nullptr)
            from = is_built_in_layout(playing->id) ? std::string("oa-default") : playing->id.key;
        else
            from = layout_in_effect(inputs);
        const std::string to =
            is_built_in_layout(entry.id) ? std::string("oa-default") : entry.id.key;
        use.moves = library.key_hooks.moves(from, to);
    }
    library.key_use = std::move(use);
}

void close_key_use(Library& library) noexcept {
    library.key_use = {};
}

void set_key_use_keep(Library& library, bool keep) noexcept {
    if (!library.key_use.open)
        return;
    library.key_use.keep = keep;
}

void set_key_use_helper(Library& library, std::size_t index, bool on) noexcept {
    if (!library.key_use.open || index >= library.key_use.helpers.size())
        return;
    library.key_use.helpers[index].on = on;
}

void set_key_use_scope(Library& library, bool for_mod) noexcept {
    if (!library.key_use.open)
        return;
    library.key_use.for_mod = for_mod;
}

bool key_use_keeps(const KeyUse& use) noexcept {
    return use.show_own && use.keep && !use.already;
}

const std::vector<KeyMove>& key_use_moves(const KeyUse& use) noexcept {
    return key_use_keeps(use) ? use.moves.kept : use.moves.clean;
}

uint32_t key_use_moves_total(const KeyUse& use) noexcept {
    return key_use_keeps(use) ? use.moves.kept_total : use.moves.clean_total;
}

std::string key_use_button(const Library& library) {
    const KeyUse& use = library.key_use;
    if (use.show_scope && use.for_mod)
        return filled(library.text, words::use_for_mod, {{"mod", use.mod_name}});
    return std::string(shown(library.text, words::use_for_every_game));
}

std::optional<KeyUseResult> key_use_result(const Library& library) {
    if (!library.key_use.open)
        return std::nullopt;
    const KeyUse& use = library.key_use;
    KeyUseResult result;
    result.layout = use.layout;
    result.keep = key_use_keeps(use);
    result.for_mod = use.show_scope && use.for_mod;
    for (const KeyHelperSwitch& one : use.helpers) {
        if (one.on)
            result.helpers_on.push_back(one.token);
    }
    return result;
}

} // namespace oa::ui::library
