// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The brain seat picker's model: which brain a computer seat plays, at which
// level, with which options, and the words each row, the details, the footer,
// the seat's label and its hover card show. It draws nothing and reads no
// file, clock, preference or network. The host fills the inputs. Every word
// comes from text.hpp through the Library's TextHooks. Option values use the
// mutator picker's OptionSpec and number_values.
#pragma once

#include "oa/data/brain_library/seats.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/mutator_picker/model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::brain_picker {

/// One level a brain offers, in the brain's order.
struct Level {
    std::string id{};   ///< the level's id
    std::string name{}; ///< the name a player sees
    /// The income this level plays at.
    data::brain_library::Difficulty difficulty{};
};

/// One brain the picker can show. The host fills every field.
///
/// An empty `id` is the built-in computer player. The model always shows
/// that player itself, so a brain with an empty id in `brains` is not a
/// second row. `unfit` is why this brain cannot play this seat, already in
/// words; empty when it can.
struct Brain {
    std::string id{};                ///< the brain's id; empty is the built-in player
    std::string name{};              ///< the name a player sees
    std::string version{};           ///< the version as written
    std::string author{};            ///< who wrote it; empty when unnamed
    std::string summary{};           ///< the short line under the name
    std::string description{};       ///< the longer text
    std::vector<std::string> tags{}; ///< catalogue tag ids, in the order written
    std::vector<Level> levels{};     ///< 1 to 6 levels, in the brain's order
    /// The settings a player picks, in the order written. The mutator
    /// picker's option, so a value is checked by the same rules.
    std::vector<mutator_picker::OptionSpec> options{};
    bool full_awareness{};    ///< true when the brain sees every unit on the map
    uint64_t network_bytes{}; ///< the learned networks' size; 0 when it has none
    /// The mods it works with, already in words. The host writes it.
    std::string works_with_text{};
    /// The badge picture. The host keeps the pixels until the next open.
    const library::Badge* badge{};
    /// Why it cannot play this seat. Empty when it can.
    std::string unfit{};
};

/// The brain, level and options a seat plays.
///
/// An empty `brain` is the built-in computer player. `level` is a brain's
/// level id, or `easy`, `medium` or `hard` for the built-in player.
/// `options` are in the brain's option order. A number is the canonical
/// text `number_values` lists.
struct Choice {
    std::string brain{}; ///< the brain's id; empty is built-in
    std::string level{}; ///< the level id
    /// Each option's id and its value, in the brain's option order.
    std::vector<std::pair<std::string, std::string>> options{};
};

/// What the host gives the picker.
struct Inputs {
    data::brain_library::Setup setup{}; ///< which setup opened it
    std::string seat_name{};            ///< the seat, as a player reads it
    std::string mod_name{};             ///< the mod being played; empty is No Mod
    std::vector<Brain> brains{};        ///< every brain the picker can show
    Choice current{};                   ///< the seat's choice as the setup remembered it
    /// The setup's difficulty. A new brain starts at the level of this
    /// difficulty, and the built-in player uses it while `built_in_level_fixed`.
    data::brain_library::Difficulty setup_difficulty{};
    /// True in a skirmish or a campaign, where the Difficulty button sets
    /// every built-in seat's level. False in the battle room, where the
    /// seat has its own.
    bool built_in_level_fixed{};
    /// The built-in player's Easy, Medium and Hard, as the game names them.
    std::array<std::string, 3> built_in_level_names{};
    /// True when the picker offers to put the choice on every computer seat.
    bool offer_every_seat{};
    /// How texts are looked up. Empty shows the English words.
    library::TextHooks text{};
};

/// The picker's state. `open` fills it. The fields change through the
/// functions below.
struct Picker {
    Inputs inputs{};        ///< the host's inputs
    Choice choice{};        ///< the brain, level and options the seat would play
    std::string selected{}; ///< the selected row's id; empty is the built-in player
    /// True when the player asked to use this choice for every computer seat.
    bool every_seat{};
    /// The id of the brain `current` named that was not listed or cannot
    /// play. Empty when the picker opened on that choice.
    std::string left_out{};
};

/// How a row's line, or a fact's value, is drawn.
enum class Tone : uint8_t {
    plain, ///< an ordinary line
    hint,  ///< the chosen row's level
    lock,  ///< a brain that cannot play, or full awareness
};

/// One row, in the order the list draws them.
struct Row {
    std::string id{};       ///< the brain's id; empty is the built-in player
    std::string name{};     ///< the name
    std::string line{};     ///< the line under the name
    Tone tone{Tone::plain}; ///< how `line` is drawn
    bool locked{};          ///< true when it cannot be chosen
    bool chosen{};          ///< true when it is the choice
    bool full_awareness{};  ///< true when the brain sees every unit
};

/// One fact under the details: a label and its value.
struct Fact {
    std::string label{};    ///< the label; empty when the value stands alone
    std::string value{};    ///< the value
    Tone tone{Tone::plain}; ///< how `value` is drawn
};

/// The selected row's details.
struct Details {
    std::string name{};           ///< the brain's name
    std::string version_author{}; ///< the version and the author; empty for the built-in player
    std::string description{};    ///< the longer text
    std::vector<Level> levels{};  ///< the levels, in order
    std::size_t level{};          ///< which level is shown, an index into `levels`
    bool levels_locked{};         ///< true when the level cannot be changed here
    std::string level_note{};     ///< why the level is locked; empty when it is not
    /// The brain's options, in its order. Empty for the built-in player.
    std::vector<mutator_picker::OptionSpec> options{};
    /// Each option's value, in the same order as `options`.
    std::vector<std::string> values{};
    std::vector<Fact> facts{}; ///< the facts, in the order they are drawn
    bool chosen{};             ///< true when this row is the choice
};

/// A seat as the screens that list seats show it.
///
/// An empty `name` is the built-in computer player. `level` is the level's
/// name, already in words, not its id.
struct SeatShown {
    std::string name{};    ///< the brain's name; empty is the built-in player
    std::string version{}; ///< the brain's version; empty for the built-in player
    std::string level{};   ///< the level's name
    bool full_awareness{}; ///< true when the brain sees every unit
};

/// Opens the picker on `inputs`.
///
/// The choice is `current` when its brain is listed and can play. Its level
/// is kept when the brain has it, and is otherwise `default_level` for
/// `setup_difficulty`. An option is kept when the brain has it and allows
/// the value, and is otherwise the default. Every option the choice does
/// not name is the default. A current brain that is not listed, or that
/// cannot play, gives the built-in player and sets `left_out` to its id.
/// The selection is the chosen row. The built-in player's level is
/// `setup_difficulty` while `built_in_level_fixed`. Otherwise it is the
/// current level when that is `easy`, `medium` or `hard`, and
/// `setup_difficulty` when it is not. `every_seat` starts clear.
///
/// @param[in,out] picker the picker
/// @param inputs the host's inputs
void open(Picker& picker, Inputs inputs);

/// Moves the selection to a listed row.
///
/// A row that can play becomes the choice. Its level and options stay when
/// it was already the choice; otherwise the level is `default_level` for
/// the setup's difficulty and the options are the defaults. The built-in
/// player's fresh level is the setup's difficulty. A locked row is
/// selected and shown, and the choice stays. A row that is not listed
/// changes nothing.
///
/// @param[in,out] picker the picker
/// @param id the row's id; empty is the built-in player
/// @return true when the selection or the choice changed
[[nodiscard]] bool select(Picker& picker, std::string_view id);

/// Sets the chosen brain's level.
///
/// Refuses a level the chosen brain does not have, and refuses every level
/// of the built-in player while `built_in_level_fixed`. The built-in
/// player's levels are `easy`, `medium` and `hard`.
///
/// @param[in,out] picker the picker
/// @param level the level id
/// @return true when the level changed
[[nodiscard]] bool set_level(Picker& picker, std::string_view level);

/// Sets one option of the chosen brain.
///
/// A choice's value must be one of its values, as written. A toggle's must
/// be one of its two. A number must be one of `mutator_picker::number_values`,
/// the canonical text, so a spelling that grammar would read as the same
/// number is refused. An option the brain does not have is refused. The
/// built-in player has none.
///
/// @param[in,out] picker the picker
/// @param option the option's id
/// @param value the value, as written
/// @return true when the value changed
[[nodiscard]] bool set_option(Picker& picker, std::string_view option, std::string_view value);

/// Sets whether the choice is used for every computer seat.
///
/// Refuses the change unless the picker offers it.
///
/// @param[in,out] picker the picker
/// @param every true to use the choice for every computer seat
/// @return true when the flag changed
[[nodiscard]] bool set_every_seat(Picker& picker, bool every);

/// Returns the rows: the built-in player, then each brain by its name folded
/// to lower case.
///
/// The built-in player's name is "Built-in computer player". Its line is
/// its chosen level, or "Built into Open Annihilation" when it is not the
/// choice. Another row's line is the first that holds: why it cannot play,
/// in the lock tone, the chosen row's level name, in the hint tone, or the
/// summary.
///
/// @param picker the picker
/// @return the rows, the built-in player first
[[nodiscard]] std::vector<Row> rows(const Picker& picker);

/// Returns the selected row's details.
///
/// The built-in player's level note is "Set by the Difficulty button:
/// {level}" while `built_in_level_fixed`, and its levels are then locked.
/// A locked row's levels are locked too, and its reason is the first fact,
/// in the lock tone, with no label. The other facts are Works with, Full
/// awareness in the lock tone, and Network. A fact with nothing to say is
/// left out. Full awareness uses the Library's sentence. The values are
/// the choice's when the row is the choice, and the defaults otherwise.
///
/// @param picker the picker
/// @return the details
[[nodiscard]] Details details(const Picker& picker);

/// Returns the footer's summary: the choice's seat label, then " · every
/// computer seat" while `every_seat`.
///
/// The label has no version. The level is the name, not the id.
///
/// @param picker the picker
/// @return the summary, such as "Steady Commander · Firm"
[[nodiscard]] std::string summary(const Picker& picker);

/// Returns the hint under the list.
///
/// The sentence names the brain `left_out` holds, by its name when the
/// inputs still have it and by its id otherwise. Empty when nothing was
/// left out.
///
/// @param picker the picker
/// @return the hint; empty when there is nothing to say
[[nodiscard]] std::string hint(const Picker& picker);

/// Returns the choice, with every option of the chosen brain and its value.
///
/// @param picker the picker
/// @return the choice a seat would store
[[nodiscard]] Choice result(const Picker& picker);

/// Writes a seat as its screen shows it.
///
/// "Built-in · Hard" for the built-in player, "Steady Commander · Firm"
/// for a brain, and "Steady Commander 1.2 · Firm" when `with_version` and
/// the brain names a version. The built-in player has no version in either
/// form. An empty version uses the form without one.
///
/// @param shown the seat
/// @param with_version true in the battle room, where the version is shown
/// @param text how texts are looked up; empty shows the English words
/// @return the label
[[nodiscard]] std::string
seat_label(const SeatShown& shown, bool with_version, const library::TextHooks& text = {});

/// Makes the hover card a seat shows.
///
/// The title is the brain's name, or "Built-in computer player". The rows
/// are Version, for a brain only, then Level, then, when the brain has full
/// awareness, "Full awareness" whose value "Sees every unit on the map" is
/// in the lock colour. The card has no arrow; the host sets that.
///
/// @param shown the seat
/// @param text how texts are looked up; empty shows the English words
/// @return the card
[[nodiscard]] kit::HoverCardLook
seat_card(const SeatShown& shown, const library::TextHooks& text = {});

} // namespace oa::ui::brain_picker
