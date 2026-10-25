// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// How a layout, a mod's letters and a player's own changes become the
// bindings in use, and which of those bindings clash.
#pragma once

#include "oa/data/input_actions/actions.hpp"
#include "oa/ui/key_bindings/chord.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::key_bindings {

/// Which step of resolution bound a chord.
enum class Step {
    base,   ///< the base layout, or a fixed chord put back
    layout, ///< the chosen layout
    mod,    ///< the loaded mod's letters
    own,    ///< the player's own changes
};

/// One action as a layout file names it, in file order.
struct LayoutBinding {
    std::string action{};        ///< the action id, or a name this build does not know
    std::vector<Chord> chords{}; ///< the chords the line gives that action
    uint32_t line{};             ///< the line in the file, for a chord that loses
    uint32_t column{};           ///< the column in the file, for a chord that loses
};

/// Which base a resolution starts from.
enum class Base {
    oa_default, ///< the registry's defaults
    none,       ///< no base chords, apart from a fixed action's own chord
};

/// A layout: an id, the base it starts from, and its bindings in file order.
struct Layout {
    std::string id{};                      ///< the layout's id
    Base base{Base::oa_default};           ///< the base it names
    std::vector<LayoutBinding> bindings{}; ///< bindings in the file's order
};

/// One quick key gathered from a mod's GUI.
struct GuiKey {
    std::string action{}; ///< the made action id, `gui.` and the rest
    std::string gadget{}; ///< the gadget name, which may hold a registry word
    char32_t letter{};    ///< the quick key
    std::string help{};   ///< the help the mod gives, unused by resolution
};

/// The mod data resolution reads. Gathering it is another ticket's work.
struct ModData {
    std::vector<GuiKey> keys{}; ///< the GUI quick keys, in the mod's order
    std::string categories{};   ///< the category letters the loaded units use
};

/// Which pointer scheme the player is using.
enum class PointerScheme {
    right_click, ///< the right button commands
    left_click,  ///< the left button commands, where an action has that default
};

/// Everything resolution is allowed to read.
struct ResolveInput {
    Base base{Base::oa_default};         ///< which base to start from
    const Layout* chosen{};              ///< the chosen layout, or null
    const ModData* mod{};                ///< the mod's letters, or null
    const Layout* own{};                 ///< the player's own changes, or null
    std::vector<std::string> hacks_on{}; ///< the hacks that are on; order does not matter
    PointerScheme scheme{PointerScheme::right_click}; ///< the pointer scheme
    bool number_keys_select_groups{}; ///< 3.1c's SwitchAlt, applied last and not stored
    Platform platform{};              ///< which key Primary means
    const KeyboardMap* keyboard{};    ///< the map a letter and a place are compared on
};

/// One chord of a resolved action, and the step that bound it.
struct BoundChord {
    Chord chord{}; ///< the chord
    Step step{};   ///< the step that bound it
};

/// One action that exists for this resolution, with the chords it kept.
///
/// `entry` is null for a made `gui.` or category action. Its contexts and
/// its kind come from its family.
struct Bound {
    std::string id{};                               ///< the action id
    const oa::data::input_actions::Action* entry{}; ///< the registry row, or null
    uint32_t contexts{};                            ///< context bits, after a hack's change
    uint32_t when{};                                ///< condition bits, after a hack's change
    bool hack{};                      ///< exists, or was changed, because a hack is on
    std::vector<BoundChord> chords{}; ///< the chords it kept, at most four
};

/// Why a chord did not stay bound.
enum class LostReason {
    later_step,      ///< a later step took the chord
    earlier_in_file, ///< the same file bound the chord earlier
    moved_by_layout, ///< the layout, or the base it kept, already had the chord
    fixed,           ///< the chord is a fixed action's key, in a layer it meets
    too_many,        ///< a fifth chord or later
    number_swap,     ///< the number-key swap flipped Alt onto this chord
};

/// A chord that resolution dropped.
///
/// `step` is the step that had bound the chord, or the step that offered it
/// when it never stayed. `line` and `column` are the binding's, or 0.
struct Lost {
    std::string action{}; ///< the action that lost the chord
    Chord chord{};        ///< the chord
    Step step{};          ///< the step the chord came from
    LostReason reason{};  ///< why it lost
    std::string winner{}; ///< the action that kept the chord, or empty
    uint32_t line{};      ///< the line of the binding that lost, or 0
    uint32_t column{};    ///< the column of the binding that lost, or 0
};

/// The bindings in use, the names kept unknown, and the chords that lost.
struct Resolved {
    std::vector<Bound> actions{}; ///< every action that exists, registry order then made ones
    std::vector<LayoutBinding>
        unknown{};            ///< names neither the registry nor a family makes, chords kept
    std::vector<Lost> lost{}; ///< chords that did not stay
};

/// Resolves the bindings in use.
///
/// The order is the base, the chosen layout, the mod's letters, the player's
/// own changes, the number-key swap, then fixed keys and the four-chord limit.
/// The swap exchanges `group.select-n` with `build.page-n` and flips Alt on
/// `group.add-n`, dropping any chord that flip lands on. The result does not
/// depend on the order of `hacks_on`. Nothing is written back into a layout.
///
/// @param input the base, the layouts, the mod, the hacks and the scheme
/// @return the actions, the unknown names and the chords that lost
[[nodiscard]] Resolved resolve(const ResolveInput& input);

/// Reports whether two context masks can be active together.
///
/// `global` meets every context. An overlay meets only itself. `play` and
/// `watch` do not meet; each meets `match`. `menus` and `oa-screens` meet
/// only themselves.
///
/// @param contexts_a one action's context bits
/// @param contexts_b the other action's context bits
/// @return true when some context of each shares a layer
[[nodiscard]] bool layers_meet(uint32_t contexts_a, uint32_t contexts_b);

/// Reports whether two condition masks can hold at once.
///
/// @param when_a one action's condition bits
/// @param when_b the other action's condition bits
/// @return false when the masks split an exclusive pair
[[nodiscard]] bool conditions_meet(uint32_t when_a, uint32_t when_b);

/// Reports whether two chords are one key.
///
/// Equal chords are one key. A letter and a place are one key when the map
/// says that place prints the letter. With no map, only equal chords match.
///
/// @param first one chord
/// @param second the other chord
/// @param keyboard the map, or null
/// @return true when a player would call them the same key
[[nodiscard]] bool same_key(const Chord& first, const Chord& second, const KeyboardMap* keyboard);

/// Two actions whose chords clash.
struct Clash {
    std::string a{}; ///< one action
    std::string b{}; ///< the other action
    Chord chord{};   ///< the shared key, as the first action binds it
};

/// Why the editor lists a pair as a shadow instead of a clash.
///
/// A hack's precedence and an overlay are the two the editor names. A third,
/// `while_conditions`, is a binding whose conditions include another's and
/// more: it takes the key while they hold, and it is not a clash either.
enum class ShadowKind {
    taken_by_hack,    ///< a hack that is on takes the chord; the other chord stays
    while_overlay,    ///< an overlay is open over the match
    while_conditions, ///< one binding's conditions include the other's and more
};

/// A pair the editor lists as a shadow.
struct Shadow {
    std::string winner{};   ///< the action that takes the key
    std::string shadowed{}; ///< the action that waits
    Chord chord{};          ///< the shared key, as the winner binds it
    ShadowKind kind{};      ///< why it is a shadow
};

/// Lists the clashes in a resolved layout.
///
/// A hack's precedence and an overlay's shadow are not clashes, and neither
/// is a binding whose conditions include another's.
///
/// @param resolved the bindings
/// @param input the scheme, the hacks and the keyboard the resolution used
/// @return each clashing pair, in action order
[[nodiscard]] std::vector<Clash> find_clashes(const Resolved& resolved, const ResolveInput& input);

/// Lists what binding one more chord on an action would clash with.
///
/// @param resolved the bindings
/// @param input the scheme, the hacks and the keyboard
/// @param action the action the chord would be bound to
/// @param chord the chord
/// @return the clashes, and not the shadows
[[nodiscard]] std::vector<Clash> clashes_with(
    const Resolved& resolved, const ResolveInput& input, std::string_view action, const Chord& chord
);

/// Lists each shadowed pair, for the editor.
///
/// @param resolved the bindings
/// @param input the scheme, the hacks and the keyboard the resolution used
/// @return each pair a hack, an overlay or a stricter condition takes
[[nodiscard]] std::vector<Shadow> shadows(const Resolved& resolved, const ResolveInput& input);

} // namespace oa::ui::key_bindings
