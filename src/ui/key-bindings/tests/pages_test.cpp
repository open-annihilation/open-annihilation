// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The GUI files on screen move the gui actions' contexts, and a page change
// that starts again from a kept resolution gets what resolving anew gives,
// over a mod's eight thousand GUI keys. The run prints how long each takes;
// nothing is checked against a time.

#include "oa/test/check.hpp"
#include "oa/ui/key_bindings/layout.hpp"

#include "oa/data/input_actions/actions.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace keys = oa::ui::key_bindings;
namespace acts = oa::data::input_actions;

using keys::Bound;
using keys::Resolved;

/// GUI files in the mod-sized set, about as many as a large mod has.
constexpr int mod_files = 400;
/// Buttons with a quick key in each of those files.
constexpr int buttons_per_file = 20;
/// Kept-resolution page changes timed for each page, for a steady average.
constexpr int kept_repeats = 20;

/// The bit of a context.
///
/// @param name the context id
/// @return the bit, or 0
uint32_t context_bit(std::string_view name) {
    const std::span<const acts::Context> table = acts::contexts();
    for (std::size_t index = 0; index < table.size(); ++index) {
        if (table[index].name == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// Parses a chord the test expects to accept.
///
/// @param text the chord text
/// @return the chord, or a default when it was refused
keys::Chord must_parse(std::string_view text) {
    keys::ChordError error;
    const std::optional<keys::Chord> chord = keys::parse_chord(text, error);
    OA_CHECK(chord.has_value());
    return chord.value_or(keys::Chord{});
}

/// One layout line.
///
/// @param action the action id
/// @param texts the chord texts
/// @return the binding
keys::LayoutBinding binding(std::string action, std::initializer_list<std::string_view> texts) {
    keys::LayoutBinding out;
    out.action = std::move(action);
    for (const std::string_view text : texts)
        out.chords.push_back(must_parse(text));
    return out;
}

/// One GUI quick key.
///
/// @param stem the file stem
/// @param gadget the gadget name
/// @param letter the quick key
/// @return the key, with its gui action id
keys::GuiKey gui_key(std::string_view stem, std::string_view gadget, char32_t letter) {
    keys::GuiKey key;
    key.action = "gui." + std::string(stem) + "." + std::string(gadget);
    key.letter = letter;
    return key;
}

/// Finds an action in a result.
///
/// @param resolved the result
/// @param id the action id
/// @return the action, or null
const Bound* find_bound(const Resolved& resolved, std::string_view id) {
    for (const Bound& bound : resolved.actions) {
        if (bound.id == id)
            return &bound;
    }
    return nullptr;
}

/// The contexts of one action, or a value no action has when it is missing.
///
/// @param resolved the result
/// @param id the action id
/// @return the context bits
uint32_t contexts_of(const Resolved& resolved, std::string_view id) {
    const Bound* bound = find_bound(resolved, id);
    return bound != nullptr ? bound->contexts : ~uint32_t{0};
}

/// Reports whether the action's chords are exactly these texts, in order.
///
/// @param bound the action, or null
/// @param texts the texts
/// @return true when they match
bool chords_are(const Bound* bound, std::initializer_list<std::string_view> texts) {
    if (bound == nullptr || bound->chords.size() != texts.size())
        return false;
    std::size_t index = 0;
    for (const std::string_view text : texts) {
        if (keys::chord_text(bound->chords[index].chord) != text)
            return false;
        ++index;
    }
    return true;
}

/// Reports whether two resolutions hold the same actions, chords, unknown
/// names and lost chords, in the same order.
///
/// @param left one result
/// @param right the other
/// @return true when every field matches
bool same_resolved(const Resolved& left, const Resolved& right) {
    if (left.actions.size() != right.actions.size() ||
        left.unknown.size() != right.unknown.size() || left.lost.size() != right.lost.size() ||
        left.hold_to_confirm != right.hold_to_confirm)
        return false;
    for (std::size_t index = 0; index < left.actions.size(); ++index) {
        const Bound& one = left.actions[index];
        const Bound& other = right.actions[index];
        if (one.id != other.id || one.entry != other.entry || one.contexts != other.contexts ||
            one.when != other.when || one.hack != other.hack ||
            one.chords.size() != other.chords.size())
            return false;
        for (std::size_t chord = 0; chord < one.chords.size(); ++chord) {
            if (!(one.chords[chord].chord == other.chords[chord].chord) ||
                one.chords[chord].step != other.chords[chord].step)
                return false;
        }
    }
    for (std::size_t index = 0; index < left.unknown.size(); ++index) {
        const keys::LayoutBinding& one = left.unknown[index];
        const keys::LayoutBinding& other = right.unknown[index];
        if (one.action != other.action || one.chords != other.chords || one.line != other.line ||
            one.column != other.column)
            return false;
    }
    for (std::size_t index = 0; index < left.lost.size(); ++index) {
        const keys::Lost& one = left.lost[index];
        const keys::Lost& other = right.lost[index];
        if (one.action != other.action || !(one.chord == other.chord) || one.step != other.step ||
            one.reason != other.reason || one.winner != other.winner || one.line != other.line ||
            one.column != other.column)
            return false;
    }
    return true;
}

/// The side panel takes play and panels, a unit info file the unit info panel,
/// and a menu file keeps the menus only while it is on screen.
void screen_moves_gui_contexts() {
    const keys::KeyboardMap us = keys::KeyboardMap::us();
    keys::ModData mod;
    mod.keys.push_back(gui_key("armbuild1", "one", U'A'));
    mod.keys.push_back(gui_key("armbuild2", "two", U'B'));
    mod.keys.push_back(gui_key("unitinfo", "three", U'C'));
    mod.keys.push_back(gui_key("armopt", "four", U'D'));
    mod.keys.push_back(gui_key("armsound", "five", U'E'));
    keys::GuiKey attack = gui_key("armgen", "armattack", U'T');
    attack.gadget = "ARMATTACK";
    mod.keys.push_back(attack);
    keys::ResolveInput input;
    input.keyboard = &us;
    input.mod = &mod;
    const Resolved resolved = keys::resolve(input);

    const uint32_t play = context_bit("play");
    const uint32_t panel = context_bit("panel");
    const uint32_t unit_info = context_bit("unit-info");
    const uint32_t menus = context_bit("menus");
    OA_CHECK(play != 0 && panel != 0 && unit_info != 0 && menus != 0);
    OA_CHECK(contexts_of(resolved, "gui.armbuild1.one") == menus);
    OA_CHECK(contexts_of(resolved, "gui.armgen.armattack") == play);

    const std::vector<std::string> armopt = {"armopt"};
    keys::ShownGuis shown;
    shown.panel = "armbuild1";
    shown.menus = armopt;
    const Resolved screen = keys::bindings_for_screen(resolved, shown, false);
    OA_CHECK(contexts_of(screen, "gui.armbuild1.one") == (play | panel));
    OA_CHECK(contexts_of(screen, "gui.armbuild2.two") == 0);
    OA_CHECK(contexts_of(screen, "gui.unitinfo.three") == unit_info);
    OA_CHECK(contexts_of(screen, "gui.armopt.four") == menus);
    OA_CHECK(contexts_of(screen, "gui.armsound.five") == 0);
    OA_CHECK(contexts_of(screen, "gui.armgen.armattack") == play);
    OA_CHECK(chords_are(find_bound(screen, "build.page-5"), {"@Digit5", "Numpad5"}));
    // The resolution itself is left for the next page.
    OA_CHECK(contexts_of(resolved, "gui.armbuild1.one") == menus);
    OA_CHECK(contexts_of(resolved, "gui.unitinfo.three") == menus);

    // Nothing on screen: the unit info file keeps its panel, and no menu
    // file keeps the menus.
    const Resolved bare = keys::bindings_for_screen(resolved, keys::ShownGuis{}, false);
    OA_CHECK(contexts_of(bare, "gui.armbuild1.one") == 0);
    OA_CHECK(contexts_of(bare, "gui.unitinfo.three") == unit_info);
    OA_CHECK(contexts_of(bare, "gui.armopt.four") == 0);

    // Pointer keys set their chords aside, as pointer_keys_bindings does.
    const Resolved pointing = keys::bindings_for_screen(resolved, shown, true);
    OA_CHECK(chords_are(find_bound(pointing, "build.page-5"), {"@Digit5"}));
    OA_CHECK(contexts_of(pointing, "gui.armbuild1.one") == (play | panel));
}

/// A mod-sized set: the order file with the order gadgets, build pages of two
/// sides, two unit info files and menus, with letters spread over A to Z.
///
/// @return the mod data, the order file's gadget words set as the runtime sets them
keys::ModData mod_sized() {
    constexpr std::string_view orders[] = {
        "ARMMOVE",
        "ARMSTOP",
        "ARMPATROL",
        "ARMATTACK",
        "ARMDEFEND",
        "ARMBLAST",
        "ARMRECLAIM",
        "ARMREPAIR",
        "ARMCAPTURE",
        "ARMLOAD",
        "ARMUNLOAD",
        "ARMCLOAK",
        "ARMONOFF",
        "ARMFIREORD",
        "ARMMOVEORD",
        "ARMORDERS",
        "ARMBUILD",
    };
    keys::ModData mod;
    mod.keys.reserve(static_cast<std::size_t>(mod_files * buttons_per_file));
    for (int file = 0; file < mod_files; ++file) {
        std::string stem;
        if (file == 0)
            stem = "armgen";
        else if (file == 1)
            stem = "unitinfo";
        else if (file == 2)
            stem = "unitinfoarm";
        else if (file == 3)
            stem = "armopt";
        else if (file == 4)
            stem = "armsound";
        else if (file < 200)
            stem = "armbuild" + std::to_string(file);
        else if (file < 380)
            stem = "corbuild" + std::to_string(file);
        else
            stem = "menu" + std::to_string(file);
        for (int button = 0; button < buttons_per_file; ++button) {
            const auto letter = static_cast<char32_t>('A' + (file * 7 + button) % 26);
            const bool order = file == 0 && button < static_cast<int>(std::size(orders));
            std::string gadget = order ? std::string(orders[static_cast<std::size_t>(button)])
                                       : "button" + std::to_string(button);
            keys::GuiKey key = gui_key(stem, gadget, letter);
            if (order) {
                for (char& byte : key.action) {
                    if (byte >= 'A' && byte <= 'Z')
                        byte = static_cast<char>(byte - 'A' + 'a');
                }
                key.gadget = std::move(gadget);
            }
            key.menu = stem;
            mod.keys.push_back(std::move(key));
        }
    }
    mod.categories = "aw";
    for (int word = 0; word < 60; ++word)
        mod.tags.push_back("RIDGE_TAG" + std::to_string(word));
    return mod;
}

/// Milliseconds since a time.
///
/// @param since the start
/// @return the milliseconds
double elapsed_ms(std::chrono::steady_clock::time_point since) {
    const auto spent = std::chrono::steady_clock::now() - since;
    return std::chrono::duration<double, std::milli>(spent).count();
}

/// Starting again from a kept resolution gives what resolving anew gives, page
/// after page, with a chosen layout, the player's own changes, hacks and the
/// number-key swap, and the kept resolution stays as it was.
void kept_resolution_matches_resolving_anew() {
    const keys::KeyboardMap us = keys::KeyboardMap::us();
    const keys::ModData mod = mod_sized();
    keys::Layout chosen;
    chosen.id = "ridge";
    chosen.bindings.push_back(binding("gui.armbuild7.button3", {"Ctrl+J"}));
    chosen.bindings.push_back(binding("order.attack", {"F"}));
    keys::Layout own;
    own.bindings.push_back(binding("gui.armopt.button2", {"Shift+K"}));
    own.bindings.push_back(binding("game.speed-up", {"NumpadAdd"}));
    keys::ResolveInput input;
    input.keyboard = &us;
    input.mod = &mod;
    input.chosen = &chosen;
    input.own = &own;
    input.hacks_on = {"ui.megamap", "ui.selection-shortcuts"};
    input.number_keys_select_groups = true;

    auto began = std::chrono::steady_clock::now();
    const Resolved kept = keys::resolve(input);
    double anew_ms = elapsed_ms(began);
    int anew_runs = 1;

    struct Page {
        std::string_view panel{};
        std::vector<std::string> menus{};
        bool pointer_keys{};
    };

    const std::vector<Page> pages = {
        {"armbuild12", {}, false},
        {"armgen", {}, false},
        {"", {"armopt"}, false},
        {"armbuild12", {"armopt", "armsound"}, true},
        {"corbuild379", {"menu390"}, false},
    };
    const uint32_t play = context_bit("play");
    const uint32_t panel = context_bit("panel");
    const uint32_t menus = context_bit("menus");
    double kept_ms = 0.0;
    int kept_runs = 0;
    for (const Page& page : pages) {
        keys::ShownGuis shown;
        shown.panel = page.panel;
        shown.menus = page.menus;
        began = std::chrono::steady_clock::now();
        const Resolved fresh = keys::resolve(input);
        const Resolved full = keys::bindings_for_screen(fresh, shown, page.pointer_keys);
        anew_ms += elapsed_ms(began);
        ++anew_runs;
        // The pages before this one left the kept resolution as it was.
        OA_CHECK(same_resolved(fresh, kept));

        Resolved from_kept;
        for (int repeat = 0; repeat < kept_repeats; ++repeat) {
            began = std::chrono::steady_clock::now();
            from_kept = keys::bindings_for_screen(kept, shown, page.pointer_keys);
            kept_ms += elapsed_ms(began);
            ++kept_runs;
        }
        OA_CHECK(same_resolved(full, from_kept));
        if (!page.panel.empty()) {
            const std::string first = "gui." + std::string(page.panel) + ".button19";
            OA_CHECK(contexts_of(from_kept, first) == (play | panel));
        }
        for (const std::string& menu : page.menus)
            OA_CHECK((contexts_of(from_kept, "gui." + menu + ".button19") & menus) != 0);
    }
    // The pages differ, so the comparison above is not of one table.
    keys::ShownGuis first_page;
    first_page.panel = "armbuild12";
    keys::ShownGuis last_page;
    last_page.panel = "corbuild379";
    OA_CHECK(!same_resolved(
        keys::bindings_for_screen(kept, first_page, false),
        keys::bindings_for_screen(kept, last_page, false)
    ));

    std::printf(
        "%zu GUI keys, %zu actions: a page change took %.1f ms resolving anew and %.2f ms from "
        "the kept resolution, on average\n",
        mod.keys.size(),
        kept.actions.size(),
        anew_ms / static_cast<double>(anew_runs),
        kept_ms / static_cast<double>(kept_runs)
    );
}

} // namespace

int main() {
    screen_moves_gui_contexts();
    kept_resolution_matches_resolving_anew();
    return oa::test::check_exit_status();
}
