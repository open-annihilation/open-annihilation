// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One brain button on each computer row this machine runs in the battle
// room, over that row's empty screen-size column. The button reads the
// seat's label, cut with … to the column, and opens that seat's picker.
// Another machine's brain is a label in the same place. Tab, the arrows and
// the controller reach the buttons. Resting the pointer, or the information
// key, shows the seat's card. With nothing to draw the screen is not there.

#include "oa_button_focus.hpp"
#include "oa_layer.hpp"
#include "library_host.hpp"
#include "engine_settings_state.hpp"

#include "oa/app/content/updates.hpp"
#include "oa/app/mod_summary.hpp"
#include "oa/app/runtime.hpp"
#include "oa/core/game_state.h"
#include "oa/data/brain_library/library.hpp"
#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/match_rules/difficulty_names.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/ui/brain_picker/model.hpp"
#include "oa/ui/brain_picker/text.hpp"
#include "oa/ui/frontend_multiplayer/get_map.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/panel.hpp"
#include "oa/ui/frontend_multiplayer/presence_card.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/pad_controls.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace brain = oa::data::brain_library;
namespace picker = oa::ui::brain_picker;
namespace library = oa::ui::library;
namespace oabrain = oa::formats::oabrain;
namespace oamut = oa::formats::oamut;
namespace place = oa::ui::frontend_state;
namespace mp = oa::ui::frontend_multiplayer;
namespace pad = oa::ui::pad_controls;
namespace kit = oa::ui::kit;

/// Writes a choice onto the computers this machine runs. The screen is not
/// a member, and a lambda handed to the picker reaches the choice through this.
struct BrainRoomAccess {
    /// Writes the choice onto one computer, or onto every computer this machine runs.
    ///
    /// @param runtime the runtime
    /// @param lobby the battle room
    /// @param player_id the computer the picker was opened on
    /// @param choice the choice USE THIS handed
    /// @param every true when the choice is for every computer this machine runs
    static void write(
        Runtime& runtime,
        mp::Lobby& lobby,
        uint32_t player_id,
        const picker::Choice& choice,
        bool every
    );
};

namespace {

constexpr std::string_view screen_name = "brain-room";
/// Control ids start here, clear of a hover card's line ids.
constexpr int32_t id_base = 100;
constexpr std::array<std::string_view, 3> level_labels{"Easy", "Medium", "Hard"};

/// Returns one of the interface's words in the language shown.
///
/// @param context unused
/// @param english the word, in English
/// @return its translation, or the English
std::string_view shown_word(void* /*context*/, std::string_view english) {
    return oa::data::languages::interface_text(english);
}

/// The hooks a seat's label and card look words up through.
///
/// @return the hooks
library::TextHooks text_hooks() {
    return library::TextHooks{nullptr, shown_word};
}

/// Tells whether two language tags name one language.
///
/// ASCII case is ignored, and '_' is read as '-'.
///
/// @param left a tag
/// @param right a tag
/// @return true when they name one language
bool same_tag(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t at = 0; at < left.size(); ++at) {
        unsigned char first = static_cast<unsigned char>(left[at]);
        unsigned char second = static_cast<unsigned char>(right[at]);
        if (first == '_')
            first = '-';
        if (second == '_')
            second = '-';
        if (std::tolower(first) != std::tolower(second))
            return false;
    }
    return true;
}

/// Copies a fixed field up to its first NUL.
///
/// @param text the field
/// @param bytes the field's size
/// @return the text
std::string copied_field(const char* text, std::size_t bytes) {
    std::string out;
    if (text == nullptr)
        return out;
    out.reserve(bytes);
    for (std::size_t at = 0; at < bytes && text[at] != '\0'; ++at)
        out.push_back(text[at]);
    return out;
}

/// The screen-size control's name for one slot: RES0 through RES9.
///
/// @param slot the slot, 0 to 9
/// @return the name
std::string res_name(int32_t slot) {
    std::string name("RES");
    name.push_back(static_cast<char>('0' + slot));
    return name;
}

/// The level id a built-in seat stores for a difficulty.
///
/// @param difficulty the difficulty
/// @return easy, medium or hard
const char* level_id_of(brain::Difficulty difficulty) noexcept {
    switch (difficulty) {
    case brain::Difficulty::easy:
        return "easy";
    case brain::Difficulty::hard:
        return "hard";
    case brain::Difficulty::medium:
        break;
    }
    return "medium";
}

/// The built-in difficulty a level id names. Anything else is hard.
///
/// @param level the level id
/// @return 0, 1 or 2
int32_t built_in_difficulty(std::string_view level) noexcept {
    if (level == "easy")
        return OA_DIFFICULTY_EASY;
    if (level == "medium")
        return OA_DIFFICULTY_MEDIUM;
    return OA_DIFFICULTY_HARD;
}

/// Maps a manifest difficulty onto the seat record's.
///
/// @param difficulty the manifest's difficulty
/// @return the seat difficulty
brain::Difficulty library_difficulty(oabrain::Difficulty difficulty) noexcept {
    switch (difficulty) {
    case oabrain::Difficulty::easy:
        return brain::Difficulty::easy;
    case oabrain::Difficulty::medium:
        return brain::Difficulty::medium;
    case oabrain::Difficulty::hard:
        return brain::Difficulty::hard;
    }
    return brain::Difficulty::medium;
}

/// The income a seat's level plays at.
///
/// @param difficulty the level's difficulty
/// @return OA_DIFFICULTY_EASY, MEDIUM or HARD
uint8_t income_of(brain::Difficulty difficulty) noexcept {
    switch (difficulty) {
    case brain::Difficulty::easy:
        return static_cast<uint8_t>(OA_DIFFICULTY_EASY);
    case brain::Difficulty::medium:
        return static_cast<uint8_t>(OA_DIFFICULTY_MEDIUM);
    case brain::Difficulty::hard:
        return static_cast<uint8_t>(OA_DIFFICULTY_HARD);
    }
    return static_cast<uint8_t>(OA_DIFFICULTY_MEDIUM);
}

/// The level id `default_level` chooses.
///
/// @param manifest the brain
/// @param wanted the setup's difficulty
/// @return the level id; empty when the brain has no level
std::string level_for(const oabrain::Manifest& manifest, brain::Difficulty wanted) {
    if (manifest.levels.empty())
        return {};
    std::vector<brain::Difficulty> levels;
    levels.reserve(manifest.levels.size());
    for (const oabrain::Level& level : manifest.levels)
        levels.push_back(library_difficulty(level.difficulty));
    const std::size_t chosen = brain::default_level(levels, wanted);
    if (chosen >= manifest.levels.size())
        return {};
    return manifest.levels[chosen].id;
}

/// The mod being played, for a brain's fit. Empty when the game plays no mod.
///
/// @param mods the mods the game lists
/// @return the mod's id, name and version; an empty id is the base game
template <typename Mods>
library::PlayedMod played_mod(const Mods& mods) {
    library::PlayedMod played;
    if (mods.playing_mod_folder.empty())
        return played;
    for (std::size_t at = 0;
         at < mods.mod_folders.size() && at < mods.mod_names.size() && at < mods.mod_details.size();
         ++at) {
        if (mods.mod_folders[at] != mods.playing_mod_folder)
            continue;
        const auto& details = mods.mod_details[at];
        played.id = details.profile_id;
        played.name = mods.mod_names[at];
        if (details.version != mod_summary_no_version)
            played.version = details.version;
        break;
    }
    return played;
}

/// What a brain is fitted against: this build, the mod in play, the expansions on.
///
/// @param settings the settings in effect
/// @param content the base game and expansions
/// @return the fit inputs
template <typename Settings, typename Content>
library::Inputs fit_inputs(const Settings& settings, const Content& content) {
    library::Inputs probe;
    probe.engine = content::this_engine_rules().engine;
    probe.played = played_mod(settings);
    probe.content.enabled = content.expansions;
    return probe;
}

/// Why a brain cannot play this room's game. Empty when it can.
///
/// @param probe this room's fit
/// @param installed the brain
/// @return the reason, or empty
std::string unfit_reason(const library::Inputs& probe, const brain::InstalledBrain& installed) {
    return library::brain_unfit_reason(
        probe, brain_facts_of(installed.manifest, installed.network_bytes)
    );
}

/// A brain's name and one level's name, in the language shown.
struct NamedBrain {
    std::string name{};  ///< the brain's name
    std::string level{}; ///< the level's name
};

/// Resolves a brain's name and a level's name the way the picker does.
///
/// The manifest's own strings fill a name no translation gave. A brain with
/// no name uses its id, so a seat label does not read as the built-in player.
///
/// @param manifest the brain
/// @param level_id the level
/// @return the names
NamedBrain names_of(const oabrain::Manifest& manifest, std::string_view level_id) {
    NamedBrain named;
    const oa::data::languages::FallbackChain chain =
        oa::data::languages::fallback_chain(oa::data::languages::interface_language());
    for (const oa::data::languages::Language* language : chain.view()) {
        if (language == nullptr)
            continue;
        for (const oabrain::Translation& translation : manifest.strings) {
            if (!same_tag(translation.words.language, language->tag))
                continue;
            if (named.name.empty() && !translation.words.name.empty())
                named.name = translation.words.name;
            if (named.level.empty()) {
                for (const oabrain::LevelName& level : translation.levels) {
                    if (level.id == level_id && !level.name.empty())
                        named.level = level.name;
                }
            }
        }
    }
    if (named.name.empty())
        named.name = manifest.name;
    if (named.name.empty())
        named.name = manifest.id;
    if (named.level.empty()) {
        if (const oabrain::Level* level = oabrain::find_level(manifest, level_id))
            named.level = level->name;
    }
    if (named.level.empty())
        named.level = std::string(level_id);
    return named;
}

/// The built-in player's level, as the battle room names that difficulty.
///
/// @param runtime the runtime
/// @param level easy, medium or hard; anything else is hard
/// @return the word, in the language shown
std::string built_in_word(Runtime& runtime, std::string_view level) {
    const int32_t difficulty = built_in_difficulty(level);
    const int32_t named =
        oa::data::match_rules::difficulty_name_index(runtime.difficulty_names(), difficulty);
    const std::size_t label_at = named >= 0 && named < 3 ? static_cast<std::size_t>(named)
                                                         : static_cast<std::size_t>(difficulty);
    return runtime.translate_ui(level_labels[label_at]);
}

/// What a choice publishes, and what its row shows.
struct Published {
    picker::SeatShown shown{};                ///< the label and the card
    uint8_t difficulty{OA_DIFFICULTY_HARD};   ///< the level's income
    std::optional<mp::ComputerBrain> brain{}; ///< empty for the built-in player
};

/// Describes a stored choice from the brains installed here.
///
/// A brain that is missing or unfit is shown as the built-in player at hard,
/// and is not published. The stored choice itself is left as it is.
///
/// @param runtime the runtime
/// @param listing the brains installed here
/// @param probe this room's fit
/// @param choice the stored choice
/// @return the face, the income and the brain to publish
Published describe_choice(
    Runtime& runtime,
    const brain::Listing& listing,
    const library::Inputs& probe,
    const picker::Choice& choice
) {
    Published published;
    if (choice.brain.empty()) {
        published.shown.level = built_in_word(runtime, choice.level);
        published.difficulty = static_cast<uint8_t>(built_in_difficulty(choice.level));
        return published;
    }
    const brain::InstalledBrain* installed = brain::find(listing, choice.brain);
    if (installed == nullptr || !unfit_reason(probe, *installed).empty()) {
        published.shown.level = built_in_word(runtime, "hard");
        return published;
    }
    std::string level_id = choice.level;
    const oabrain::Level* level = oabrain::find_level(installed->manifest, level_id);
    if (level == nullptr) {
        level_id = level_for(installed->manifest, brain::Difficulty::hard);
        level = oabrain::find_level(installed->manifest, level_id);
    }
    if (level == nullptr) {
        published.shown.level = built_in_word(runtime, "hard");
        return published;
    }
    const NamedBrain names = names_of(installed->manifest, level->id);
    published.shown.name = names.name;
    published.shown.version = oamut::version_text(installed->manifest.version);
    published.shown.level = names.level;
    published.shown.full_awareness = installed->manifest.full_awareness;
    published.difficulty = income_of(library_difficulty(level->difficulty));
    mp::ComputerBrain brain_row;
    brain_row.id = installed->manifest.id;
    brain_row.name = names.name;
    brain_row.version = published.shown.version;
    brain_row.level_id = level->id;
    brain_row.level = names.level;
    brain_row.full_awareness = installed->manifest.full_awareness;
    published.brain = std::move(brain_row);
    return published;
}

/// The default brain at hard's level, or the built-in player at hard.
///
/// Nothing is logged. A default that is missing, unfit or without a level
/// is the built-in player.
///
/// @param listing the brains installed here
/// @param probe this room's fit
/// @param default_id the player's default brain; empty when none is set
/// @param seated_default set when the default brain was the one returned
/// @return the choice
picker::Choice default_choice(
    const brain::Listing& listing,
    const library::Inputs& probe,
    std::string_view default_id,
    bool* seated_default
) {
    if (seated_default != nullptr)
        *seated_default = false;
    const brain::InstalledBrain* installed =
        default_id.empty() ? nullptr : brain::find(listing, default_id);
    if (installed != nullptr && unfit_reason(probe, *installed).empty()) {
        const std::string level = level_for(installed->manifest, brain::Difficulty::hard);
        if (!level.empty()) {
            picker::Choice choice;
            choice.brain = installed->manifest.id;
            choice.level = level;
            if (seated_default != nullptr)
                *seated_default = true;
            return choice;
        }
    }
    picker::Choice built_in;
    built_in.level = level_id_of(brain::Difficulty::hard);
    return built_in;
}

/// Says why a remembered brain was not seated.
///
/// @param player the slot
/// @param id the brain that was not seated
/// @param reason why
/// @param default_plays true when the default brain plays instead
void log_refused(uint8_t player, std::string_view id, std::string_view reason, bool default_plays) {
    std::cerr << "open-annihilation: brains: player " << static_cast<int>(player) << ": " << id
              << ": " << reason << "; "
              << (default_plays ? "the default brain plays" : "the built-in player plays") << '\n';
}

/// The choice a remembered seat becomes.
///
/// An empty brain is the built-in player on purpose, at the seat's level
/// when that is easy, medium or hard, and at hard otherwise. A brain that
/// is missing or unfit falls back to the default brain at hard, else the
/// built-in player at hard, and that is logged.
///
/// @param listing the brains installed here
/// @param probe this room's fit
/// @param default_id the player's default brain; empty when none is set
/// @param slot the slot the computer sits in
/// @param seat the remembered seat
/// @return the choice to store
picker::Choice choice_from_seat(
    const brain::Listing& listing,
    const library::Inputs& probe,
    std::string_view default_id,
    uint8_t slot,
    const brain::Seat& seat
) {
    if (seat.brain.empty()) {
        picker::Choice built_in;
        if (seat.level == "easy" || seat.level == "medium" || seat.level == "hard")
            built_in.level = seat.level;
        else
            built_in.level = level_id_of(brain::Difficulty::hard);
        return built_in;
    }
    const brain::InstalledBrain* installed = brain::find(listing, seat.brain);
    std::string reason;
    if (installed == nullptr) {
        reason = "it is not installed";
    } else {
        reason = unfit_reason(probe, *installed);
        if (reason.empty()) {
            std::string level_id = seat.level;
            if (oabrain::find_level(installed->manifest, level_id) == nullptr)
                level_id = level_for(installed->manifest, brain::Difficulty::hard);
            if (!level_id.empty()) {
                picker::Choice choice;
                choice.brain = installed->manifest.id;
                choice.level = level_id;
                choice.options = seat.options;
                return choice;
            }
            reason = "the brain has no level";
        }
    }
    bool seated_default = false;
    picker::Choice fallen = default_choice(listing, probe, default_id, &seated_default);
    log_refused(slot, seat.brain, reason, seated_default);
    return fallen;
}

/// The difficulty a launch named for one slot. Unset is hard.
///
/// @param lobby the battle room
/// @param slot the computer's slot
/// @return easy, medium or hard
brain::Difficulty difficulty_of_launch(const mp::Lobby& lobby, uint8_t slot) noexcept {
    if (slot >= mp::kSlotCount || !lobby.computer_difficulty_set[slot])
        return brain::Difficulty::hard;
    if (lobby.computer_difficulty[slot] == 0)
        return brain::Difficulty::easy;
    if (lobby.computer_difficulty[slot] == 1)
        return brain::Difficulty::medium;
    return brain::Difficulty::hard;
}

/// The choice a launch's brain becomes.
///
/// The level is the one `default_level` gives for the launch's difficulty,
/// hard when the launch named none. The brain's default options are left
/// for the seat to fill. A brain that is missing, unfit or without a level
/// plays the built-in player at that difficulty, and both the launch and
/// the fallback are logged.
///
/// @param listing the brains installed here
/// @param probe this room's fit
/// @param id the launch's brain
/// @param slot the slot the computer sits in
/// @param wanted the launch's difficulty
/// @return the choice to store
picker::Choice choice_from_launch(
    const brain::Listing& listing,
    const library::Inputs& probe,
    std::string_view id,
    uint8_t slot,
    brain::Difficulty wanted
) {
    std::cerr << "open-annihilation: brains: the launch puts " << id << " on player "
              << static_cast<int>(slot) << '\n';
    picker::Choice built_in;
    built_in.level = level_id_of(wanted);
    const brain::InstalledBrain* installed = brain::find(listing, id);
    std::string reason;
    if (installed == nullptr) {
        reason = "it is not installed";
    } else {
        reason = unfit_reason(probe, *installed);
        if (reason.empty()) {
            const std::string level = level_for(installed->manifest, wanted);
            if (!level.empty()) {
                picker::Choice choice;
                choice.brain = installed->manifest.id;
                choice.level = level;
                return choice;
            }
            reason = "the brain has no level";
        }
    }
    std::cerr << "open-annihilation: brains: player " << static_cast<int>(slot) << ": " << id
              << ": " << reason << "; the built-in player plays\n";
    return built_in;
}

/// Grows a rectangle by the focus ring's margin.
///
/// @param rect the rectangle
/// @param margin the margin, in points
/// @return the grown rectangle
kit::Rect outset(const kit::Rect& rect, int32_t margin) noexcept {
    return {rect.x - margin, rect.y - margin, rect.width + margin * 2, rect.height + margin * 2};
}

/// Returns the union of two rectangles.
///
/// @param into the rectangle so far; empty when its width is not positive
/// @param extra the rectangle to include
/// @return the union
kit::Rect unite(const kit::Rect& into, const kit::Rect& extra) noexcept {
    if (into.width <= 0 || into.height <= 0)
        return extra;
    const int32_t left = std::min(into.x, extra.x);
    const int32_t top = std::min(into.y, extra.y);
    const int32_t right = std::max(into.x + into.width, extra.x + extra.width);
    const int32_t bottom = std::max(into.y + into.height, extra.y + extra.height);
    return {left, top, right - left, bottom - top};
}

/// A rectangle in the screen's own points.
///
/// @param rect the rectangle, in the layer's points
/// @param origin the screen's origin, in the layer's points
/// @return the rectangle
kit::Rect local_of(const kit::Rect& rect, const kit::Rect& origin) noexcept {
    return {rect.x - origin.x, rect.y - origin.y, rect.width, rect.height};
}

/// Returns where a hover card's arrow is drawn.
///
/// @param placed the card
/// @param look the card's look, its arrow pointed
/// @param arrow_depth how far the arrow reaches out of the card
/// @return the arrow's rectangle; empty without an arrow
kit::Rect arrow_rect(const kit::Rect& placed, const kit::HoverCardLook& look, int32_t arrow_depth) {
    const int32_t across = 2 * arrow_depth - 1;
    const int32_t left = placed.x + look.arrow_at - (arrow_depth - 1);
    switch (look.arrow) {
    case kit::Side::top:
        return {left, placed.y - arrow_depth, across, arrow_depth};
    case kit::Side::bottom:
        return {left, placed.y + placed.height, across, arrow_depth};
    case kit::Side::none:
    case kit::Side::left:
    case kit::Side::right:
        break;
    }
    return {};
}

/// Tells whether this machine runs the computer in a slot.
///
/// The session may not have marked the player in use yet. The id is enough.
///
/// @param player the slot's player
/// @return true when this machine runs that computer
bool own_computer(const oa::Player& player) noexcept {
    return player.status == mp::kSlotComputer && player.player_id != 0;
}

/// Publishes one stored choice onto its slot.
///
/// A computer the session has not seated yet is left as it is. The next note
/// publishes it. The other machines learn the brain from this machine's
/// presence record, which goes out now: a launch that does not pass the
/// battle room again still leaves them the brain a recording of the game names.
///
/// @param runtime the runtime
/// @param lobby the battle room
/// @param slot the slot
/// @param choice the choice
/// @param listing the brains installed here
/// @param probe this room's fit
void publish_choice(
    Runtime& runtime,
    mp::Lobby& lobby,
    int32_t slot,
    const picker::Choice& choice,
    const brain::Listing& listing,
    const library::Inputs& probe
) {
    const Published published = describe_choice(runtime, listing, probe, choice);
    mp::lobby_set_computer_brain(lobby, slot, published.brain, published.difficulty);
    mp::presence_refresh(lobby);
    mp::presence_send_due(lobby);
}

} // namespace

void BrainRoomAccess::write(
    Runtime& runtime, mp::Lobby& lobby, uint32_t player_id, const picker::Choice& choice, bool every
) {
    runtime.apply_room_brain_choice(lobby, player_id, choice, every);
}

/// The brain seats over the battle room. Not modal, no backdrop, not opaque,
/// so the picture shows round them. A button takes its own press and the
/// focus handed to it. A label takes neither.
struct Runtime::BrainRoomScreen final : LayerScreen {
    /// Makes the screen.
    ///
    /// @param runtime the runtime
    explicit BrainRoomScreen(Runtime& runtime) : runtime_(runtime) {}

    /// Returns "brain-room".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return screen_name; }

    /// Tells whether a row needs the screen.
    ///
    /// True while the battle room is in front, the fonts are ready, and some
    /// computer row would draw a button or a label.
    ///
    /// @param runtime the runtime
    /// @return true when the screen should be up
    [[nodiscard]] static bool wanted(Runtime& runtime) {
        mp::BattleRoomMap map;
        if (!mp::multiplayer_battle_room_map(&map))
            return false;
        if (runtime.oa_layer().screen_fonts() == nullptr)
            return false;
        mp::Lobby& lobby = mp::multiplayer_lobby();
        if (lobby.game == nullptr)
            return false;
        const std::vector<mp::BattleRoomControl> controls = mp::multiplayer_battle_room_controls();
        const bool installed = !runtime.installed_brains().brains.empty();
        for (int32_t slot = 0; slot < mp::kSlotCount; ++slot) {
            const oa::Player& player = mp::slot_player(lobby, slot);
            bool draw = false;
            if (own_computer(player)) {
                const picker::Choice* choice = nullptr;
                for (const Runtime::RoomBrainChoice& held : runtime.room_brain_choices_) {
                    if (held.player_id == player.player_id)
                        choice = &held.choice;
                }
                draw = choice != nullptr && (!choice->brain.empty() || installed);
            } else if (
                player.status == mp::kSlotRemote && mp::lobby_computer_brain(lobby, slot) != nullptr
            ) {
                draw = true;
            }
            if (!draw)
                continue;
            const std::string name = res_name(slot);
            for (const mp::BattleRoomControl& control : controls) {
                if (control.name == name && control.width > 0 && control.height > 0)
                    return true;
            }
        }
        return false;
    }

    /// Returns the rows, the focus ring's margin and the open card, in the window.
    ///
    /// @param placed_on what the screen is placed on
    /// @return its place; empty while it does not show
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        const Geometry laid = geometry(placed_on);
        if (!laid.ok)
            return {};
        const int32_t percent = std::max(placed_on.scale_percent, int32_t{100});
        const kit::Rect shown_at = kit::to_canvas(laid.bounds, percent);
        return {
            {shown_at.x, shown_at.y, shown_at.width, shown_at.height},
            laid.bounds.width,
            laid.bounds.height,
            percent
        };
    }

    /// Tells that what the rows do not take goes on.
    ///
    /// @return false
    [[nodiscard]] bool modal() const override { return false; }

    /// Tells that the rows darken nothing.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Tells that the rows lie over the picture.
    ///
    /// @return false
    [[nodiscard]] bool opaque() const override { return false; }

    /// Follows the view the rows are placed on.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override { view_ = placed_on; }

    /// Draws the rows, the open card and, while a button holds the focus, its ring.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override { kit::paint(canvas, display_list()); }

    /// Takes a press on a button or the open card. A press on a disabled
    /// button is taken and opens nothing. A press anywhere else clears the
    /// focus and goes on.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the screen's points
    /// @param finger_reach a finger's reach; 0 for a mouse
    /// @return redraw or none when the screen takes it; pass otherwise
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        const Geometry laid = geometry(view_);
        if (!laid.ok)
            return LayerAnswer::pass;
        pointer_layer_ = {at.x + laid.bounds.x, at.y + laid.bounds.y};
        have_pointer_ = true;
        const int32_t over_mouse = button_at(laid, at);
        const int32_t over = kind != ScreenInputKind::pointer_move && finger_reach > 0
                                 ? finger_slot(laid, at, finger_reach)
                                 : over_mouse;
        if (over_mouse != hovered_slot_) {
            hovered_slot_ = over_mouse;
            ++revision_;
        }
        const int32_t card_before = card_.open_slot();
        follow_card(laid);
        if (card_.open_slot() != card_before)
            ++revision_;
        const bool on_card = card_contains(laid, at);
        if (kind == ScreenInputKind::pointer_down && button == SDL_BUTTON_LEFT) {
            if (over >= 0) {
                pressed_slot_ = over;
                card_pressed_ = false;
                ++revision_;
                return LayerAnswer::redraw;
            }
            if (on_card) {
                pressed_slot_ = -1;
                card_pressed_ = true;
                return LayerAnswer::none;
            }
            pressed_slot_ = -1;
            card_pressed_ = false;
            if (focused_slot_ >= 0) {
                focused_slot_ = -1;
                ++revision_;
            }
            return LayerAnswer::pass;
        }
        if (kind == ScreenInputKind::pointer_up && button == SDL_BUTTON_LEFT) {
            if (card_pressed_) {
                card_pressed_ = false;
                return LayerAnswer::none;
            }
            if (on_card && pressed_slot_ < 0)
                return LayerAnswer::none;
            if (pressed_slot_ >= 0) {
                const int32_t pressed = pressed_slot_;
                pressed_slot_ = -1;
                ++revision_;
                const SeatRow* row = button_of(laid, pressed);
                if (over == pressed && row != nullptr && row->enabled)
                    open_picker(pressed);
                return LayerAnswer::redraw;
            }
        }
        return LayerAnswer::pass;
    }

    /// Takes the information key, Escape when a card is open, and the keys
    /// a focused button holds.
    ///
    /// @param pressed the key
    /// @return none or redraw when the screen takes it; pass otherwise
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        if (pressed == kit::Key::info) {
            const int32_t focused = focused_slot_ >= 0 ? focused_slot_ : mp::presence_card_no_slot;
            if (card_.key_info(focused)) {
                ++revision_;
                return LayerAnswer::none;
            }
            return LayerAnswer::pass;
        }
        if (pressed == kit::Key::escape) {
            if (card_.key_close()) {
                ++revision_;
                return LayerAnswer::none;
            }
            return LayerAnswer::pass;
        }
        return answer_key(pressed);
    }

    /// Lets every shortcut go on.
    ///
    /// @return pass
    LayerAnswer shortcut(uint32_t /*sdl_key*/, uint16_t /*modifiers*/) override {
        return LayerAnswer::pass;
    }

    /// Takes the focus keys and a press on the same terms as the keyboard.
    /// Y, View and Back go on.
    ///
    /// @param action the gamepad action
    /// @return none or redraw when the screen takes it; pass otherwise
    LayerAnswer pad_action(pad::Action action) override {
        if (action == pad::Action::press)
            return answer_key(kit::Key::enter);
        if (action == pad::Action::focus_next)
            return answer_key(kit::Key::tab);
        if (action == pad::Action::focus_previous)
            return answer_key(kit::Key::back_tab);
        if (action == pad::Action::focus_up)
            return answer_key(kit::Key::up);
        if (action == pad::Action::focus_down)
            return answer_key(kit::Key::down);
        if (action == pad::Action::focus_left)
            return answer_key(kit::Key::left);
        if (action == pad::Action::focus_right)
            return answer_key(kit::Key::right);
        return LayerAnswer::pass;
    }

    /// Lets the wheel go on.
    ///
    /// @return pass
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::pass; }

    /// Returns the rows, the open card and the ring.
    ///
    /// A row's automation name is brain-seat.<slot>, which the endpoint
    /// lists as oa.brain-seat.<slot>. A button's text is the whole label.
    /// The drawn caption is that label cut to the button. A label's drawn
    /// text is cut the same way, in the small font. Tab's order holds the
    /// buttons. The card's lines are not focusable.
    ///
    /// @return the display list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        const Geometry laid = geometry(view_);
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (!laid.ok || fonts == nullptr)
            return list;
        for (const SeatRow& row : laid.rows) {
            if (!row.button) {
                const auto measure = [fonts](std::string_view text) {
                    return kit::text_width(*fonts, kit::FontRole::small, text);
                };
                const std::string fitted = measure(row.label) <= row.local.width
                                               ? row.label
                                               : kit::fit(row.label, row.local.width, measure);
                kit::Item item;
                item.role = kit::Role::text;
                item.rect = row.local;
                item.text = fitted;
                item.font = kit::FontRole::small;
                item.align = kit::Align::centre;
                item.colour = kit::colour::text;
                list.items.push_back(std::move(item));
                kit::Control control;
                control.id = row.id;
                control.rect = row.local;
                control.enabled = false;
                control.name = row.name;
                control.kind = kit::ControlKind::area;
                control.focusable = false;
                control.text = row.label;
                list.controls.push_back(std::move(control));
                continue;
            }
            kit::ButtonLook look;
            look.style = kit::ButtonStyle::filled;
            look.enabled = row.enabled;
            look.hovered = row.enabled && hovered_slot_ == row.slot;
            look.held = row.enabled && pressed_slot_ == row.slot && hovered_slot_ == row.slot;
            look.oa_mark = true;
            fit_caption(look, *fonts, row.local, row.label);
            kit::add_button(list, row.local, look, row.id, row.name);
            if (!list.controls.empty())
                list.controls.back().text = row.label;
            if (focused_slot_ == row.slot)
                kit::add_focus_ring(list, row.local, true);
        }
        for (const Stop& stop : stops_of(laid)) {
            if (stop.button)
                list.tab_order.push_back(id_base + stop.id);
        }
        if (laid.card) {
            kit::add_hover_card(list, *laid.card, laid.card_look);
            mp::presence_card_controls(list, *laid.card, laid.card_look);
        }
        return list;
    }

    /// Returns the buttons' hover, press and focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown_state;
        shown_state.hovered = control_id(hovered_slot_);
        shown_state.pressed = control_id(pressed_slot_);
        shown_state.focused = control_id(focused_slot_);
        shown_state.focus_shown = focused_slot_ >= 0;
        return shown_state;
    }

    /// Closes once the battle room is gone or no row needs the screen.
    /// Notes this machine's computers, so a choice sticks once the session
    /// seats the player. Gives up a button whose row is gone, and gives up
    /// the focus once the battle room's own focus is on a control again.
    ///
    /// @return close when it should leave; otherwise none
    LayerAnswer tick() override {
        runtime_.note_room_computers(mp::multiplayer_lobby());
        if (!wanted(runtime_))
            return LayerAnswer::close;
        Geometry laid = geometry(view_);
        if (focused_slot_ >= 0 && button_of(laid, focused_slot_) == nullptr) {
            focused_slot_ = -1;
            pressed_slot_ = -1;
            ++revision_;
        }
        if (focused_slot_ >= 0 && mp::multiplayer_battle_room_focus() != mp::kNoControl) {
            focused_slot_ = -1;
            pressed_slot_ = -1;
            ++revision_;
        }
        if (card_.open_slot() != mp::presence_card_no_slot &&
            row_of(laid, card_.open_slot()) == nullptr)
            card_.close();
        const int32_t card_before = card_.open_slot();
        follow_card(laid);
        if (card_.open_slot() != card_before)
            ++revision_;
        laid = geometry(view_);
        const std::string now = seal_of(laid);
        if (now != seal_) {
            seal_ = std::move(now);
            ++revision_;
        }
        return LayerAnswer::none;
    }

    /// Returns the screen's revision.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Has nothing of its own to close. The picker, when it is open, is its own screen.
    void close(bool /*by_key*/) override {}

  private:

    /// One computer row's button or label.
    struct SeatRow {
        int32_t slot{-1};          ///< the slot, from 0
        uint32_t player_id{};      ///< the computer's player id; 0 for another machine's
        bool button{};             ///< true for a button this machine runs
        bool enabled{true};        ///< false while this player is ready
        kit::ControlId id{};       ///< its control id
        std::string name{};        ///< its automation name, without oa.
        place::PlaceRect at{};     ///< the row, in the layer's points
        kit::Rect local{};         ///< the row, in the screen's points
        std::string label{};       ///< the whole label, uncut
        picker::SeatShown shown{}; ///< what the card reads
        picker::Choice choice{};   ///< the choice the picker opens on
    };

    /// Where the rows, the records and the open card lie.
    struct Geometry {
        bool ok{};                               ///< a row was placed
        kit::Rect bounds{};                      ///< the screen's place, in the layer's points
        std::vector<SeatRow> rows{};             ///< the computer rows that draw
        std::vector<FocusRecord> focusable{};    ///< the records Tab can stop on
        std::vector<place::PlaceRect> gadgets{}; ///< every control, in the screen's points
        std::optional<kit::Rect> card{};         ///< the open card, in the screen's points
        std::optional<kit::Rect> arrow{};        ///< its arrow, in the screen's points
        kit::HoverCardLook card_look{};          ///< the open card's look
    };

    /// One stop in the combined Tab order: a record, or a seat button.
    struct Stop {
        bool button{};     ///< true for a seat button
        int32_t id{-1};    ///< the control's index, or the slot
        int32_t place{-1}; ///< the record's place in the battle room's order
        int64_t key{};     ///< a button's Tab key; 0 for a record
    };

    /// The control id of a slot, or none.
    ///
    /// @param slot the slot; negative is none
    /// @return the id
    [[nodiscard]] static kit::ControlId control_id(int32_t slot) noexcept {
        if (slot < 0)
            return kit::no_control;
        return id_base + slot;
    }

    /// The choice stored for a player, or null.
    ///
    /// @param player_id the computer
    /// @return the choice
    [[nodiscard]] const picker::Choice* choice_of(uint32_t player_id) const noexcept {
        for (const RoomBrainChoice& held : runtime_.room_brain_choices_) {
            if (held.player_id == player_id)
                return &held.choice;
        }
        return nullptr;
    }

    /// Where the rows and the open card lie for a view.
    ///
    /// @param placed_on what the screen is placed on
    /// @return the geometry; not ok while no row is placed
    [[nodiscard]] Geometry geometry(const LayerView& placed_on) const {
        Geometry laid;
        if (!wanted(runtime_) || placed_on.picture_size.x <= 0)
            return laid;
        mp::Lobby& lobby = mp::multiplayer_lobby();
        const std::vector<mp::BattleRoomControl> controls = mp::multiplayer_battle_room_controls();
        const bool ready =
            lobby.game != nullptr && (mp::local_info(lobby).options & mp::option::ready) != 0;
        const library::TextHooks hooks = text_hooks();
        std::vector<place::PlaceRect> everything;
        everything.reserve(controls.size());
        for (const mp::BattleRoomControl& control : controls) {
            const kit::Rect frame{control.x, control.y, control.width, control.height};
            const kit::Rect points = frame_to_points(placed_on, frame);
            if (points.width <= 0 || points.height <= 0)
                continue;
            everything.push_back(place::PlaceRect{points.x, points.y, points.width, points.height});
            if (control.focus_place < 0)
                continue;
            FocusRecord record;
            record.index = control.index;
            record.at = everything.back();
            record.frame_x = control.x;
            record.frame_y = control.y;
            record.focus_place = control.focus_place;
            laid.focusable.push_back(record);
        }
        const brain::Listing& listing = runtime_.installed_brains();
        const library::Inputs probe =
            fit_inputs(runtime_.engine_settings_state(), runtime_.content_choice());
        const bool installed = !listing.brains.empty();
        for (int32_t slot = 0; slot < mp::kSlotCount; ++slot) {
            const oa::Player& player = mp::slot_player(lobby, slot);
            const bool own = own_computer(player);
            const mp::ComputerBrain* remote = nullptr;
            const picker::Choice* choice = nullptr;
            if (own) {
                choice = choice_of(player.player_id);
                if (choice == nullptr || (choice->brain.empty() && !installed))
                    continue;
            } else if (player.status == mp::kSlotRemote) {
                remote = mp::lobby_computer_brain(lobby, slot);
                if (remote == nullptr)
                    continue;
            } else {
                continue;
            }
            const std::string name = res_name(slot);
            const mp::BattleRoomControl* res = nullptr;
            for (const mp::BattleRoomControl& control : controls) {
                if (control.name == name && control.width > 0 && control.height > 0)
                    res = &control;
            }
            if (res == nullptr)
                continue;
            const kit::Rect res_frame{res->x, res->y, res->width, res->height};
            const kit::Rect res_points = frame_to_points(placed_on, res_frame);
            const int32_t band = std::min(kit::compact_metrics.button_height, res_points.height);
            if (band <= 0 || res_points.width <= 0)
                continue;
            SeatRow row;
            row.slot = slot;
            row.id = control_id(slot);
            row.name = "brain-seat." + std::to_string(slot);
            row.at = place::PlaceRect{
                res_points.x, res_points.y + (res_points.height - band) / 2, res_points.width, band
            };
            if (own) {
                row.button = true;
                row.enabled = !ready;
                row.player_id = player.player_id;
                row.choice = *choice;
                row.shown = describe_choice(runtime_, listing, probe, *choice).shown;
            } else {
                row.button = false;
                row.enabled = false;
                row.shown.name = remote->name;
                row.shown.version = remote->version;
                row.shown.level = remote->level;
                row.shown.full_awareness = remote->full_awareness;
            }
            row.label = picker::seat_label(row.shown, true, hooks);
            laid.rows.push_back(std::move(row));
        }
        if (laid.rows.empty())
            return laid;
        kit::Rect bounds{};
        for (const SeatRow& row : laid.rows) {
            const kit::Rect rect{row.at.x, row.at.y, row.at.width, row.at.height};
            bounds =
                unite(bounds, row.button ? outset(rect, kit::compact_metrics.focus_inset) : rect);
        }
        const int32_t open = card_.open_slot();
        const SeatRow* open_row = row_of(laid, open);
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (open_row != nullptr && fonts != nullptr) {
            const int32_t arrow_depth =
                kit::metrics_of(placed_on.frame.size_class).hover_card_arrow;
            laid.card_look = picker::seat_card(open_row->shown, hooks);
            const kit::Point size = kit::hover_card_size(*fonts, laid.card_look);
            const kit::Rect badge{
                open_row->at.x, open_row->at.y, open_row->at.width, open_row->at.height
            };
            const kit::Rect card =
                mp::presence_card_place(badge, size, placed_on.frame.area, arrow_depth);
            mp::presence_card_point(laid.card_look, card, badge);
            const kit::Rect arrow = arrow_rect(card, laid.card_look, arrow_depth);
            bounds = unite(bounds, card);
            if (arrow.width > 0 && arrow.height > 0)
                bounds = unite(bounds, arrow);
            laid.card = local_of(card, bounds);
            if (arrow.width > 0 && arrow.height > 0)
                laid.arrow = local_of(arrow, bounds);
        }
        for (SeatRow& row : laid.rows) {
            const kit::Rect rect{row.at.x, row.at.y, row.at.width, row.at.height};
            row.local = local_of(rect, bounds);
        }
        laid.gadgets.reserve(everything.size());
        for (const place::PlaceRect& rect : everything) {
            const kit::Rect shifted =
                local_of(kit::Rect{rect.x, rect.y, rect.width, rect.height}, bounds);
            laid.gadgets.push_back(
                place::PlaceRect{shifted.x, shifted.y, shifted.width, shifted.height}
            );
        }
        laid.bounds = bounds;
        laid.ok = bounds.width > 0 && bounds.height > 0;
        return laid;
    }

    /// A text that changes when the rows, the focus or the card change.
    ///
    /// @param laid the geometry
    /// @return the text
    [[nodiscard]] std::string seal_of(const Geometry& laid) const {
        std::string text = std::to_string(focused_slot_);
        text.push_back('/');
        text += std::to_string(card_.open_slot());
        for (const SeatRow& row : laid.rows) {
            text.push_back('|');
            text += std::to_string(row.slot);
            text += row.label;
            text += std::to_string(row.at.x);
            text += std::to_string(row.at.y);
        }
        return text;
    }

    /// The slot whose button holds a point, or -1. A label is not a button.
    ///
    /// @param laid the geometry
    /// @param at the point, in the screen's points
    /// @return the slot
    [[nodiscard]] static int32_t button_at(const Geometry& laid, kit::Point at) noexcept {
        for (const SeatRow& row : laid.rows) {
            if (row.button && kit::contains(row.local, at))
                return row.slot;
        }
        return -1;
    }

    /// The row of a slot, or null. A label counts.
    ///
    /// @param laid the geometry
    /// @param slot the slot
    /// @return the row
    [[nodiscard]] static const SeatRow* row_of(const Geometry& laid, int32_t slot) noexcept {
        for (const SeatRow& row : laid.rows) {
            if (row.slot == slot)
                return &row;
        }
        return nullptr;
    }

    /// The button of a slot, or null.
    ///
    /// @param laid the geometry
    /// @param slot the slot
    /// @return the button
    [[nodiscard]] static const SeatRow* button_of(const Geometry& laid, int32_t slot) noexcept {
        const SeatRow* row = row_of(laid, slot);
        if (row == nullptr || !row->button)
            return nullptr;
        return row;
    }

    /// Tells whether the open card or its arrow holds a point.
    ///
    /// @param laid the geometry
    /// @param at the point, in the screen's points
    /// @return true when it does
    [[nodiscard]] static bool card_contains(const Geometry& laid, kit::Point at) noexcept {
        if (laid.card && kit::contains(*laid.card, at))
            return true;
        return laid.arrow && kit::contains(*laid.arrow, at);
    }

    /// Puts the label in the look, in the regular font where it fits after
    /// the badge, else the small font, cut with … only when even that does
    /// not fit.
    ///
    /// @param look the look the words go in
    /// @param fonts the fonts
    /// @param button the button, in the screen's points
    /// @param label the whole label
    void fit_caption(
        kit::ButtonLook& look,
        const kit::Fonts& fonts,
        const kit::Rect& button,
        const std::string& label
    ) const {
        const kit::ButtonMarkParts parts = kit::button_mark_parts(button);
        const int32_t room = std::max(parts.caption.width, int32_t{0});
        look.font = kit::FontRole::regular;
        if (kit::text_width(fonts, kit::FontRole::regular, label) <= room) {
            look.caption = label;
            return;
        }
        look.font = kit::FontRole::small;
        const auto measure = [&fonts](std::string_view text) {
            return kit::text_width(fonts, kit::FontRole::small, text);
        };
        look.caption = measure(label) <= room ? label : kit::fit(label, room, measure);
    }

    /// Tells whether a finger's reach lands on a button.
    ///
    /// A control nearer the point, or one the point lies inside, keeps it.
    /// A disabled button is still the one the finger lands on.
    ///
    /// @param laid where the buttons and the controls lie
    /// @param at the finger, in the screen's points
    /// @param finger_reach how far the finger reaches, in points
    /// @return the slot, or -1
    [[nodiscard]] int32_t
    finger_slot(const Geometry& laid, kit::Point at, int32_t finger_reach) const {
        if (finger_reach <= 0)
            return -1;
        for (const SeatRow& row : laid.rows) {
            if (!row.button || row.local.width <= 0 || row.local.height <= 0)
                continue;
            const place::PlaceRect button_place{
                row.local.x, row.local.y, row.local.width, row.local.height
            };
            if (place::nearer_gadget(at.x, at.y, button_place, laid.gadgets))
                continue;
            kit::DisplayList list = display_list();
            for (kit::Control& control : list.controls) {
                if (control.id == row.id)
                    control.enabled = true;
            }
            kit::Interaction trial = interaction();
            if (kit::finger_down(trial, list, at, finger_reach).control == row.id)
                return row.slot;
        }
        return -1;
    }

    /// The slot whose row the card follows: a button or a label.
    ///
    /// @param laid the geometry
    /// @param at the point, in the screen's points
    /// @return the slot, or -1
    [[nodiscard]] static int32_t row_at(const Geometry& laid, kit::Point at) noexcept {
        for (const SeatRow& row : laid.rows) {
            if (kit::contains(row.local, at))
                return row.slot;
        }
        return -1;
    }

    /// Follows the pointer and the focus for the card.
    ///
    /// The pointer is kept in the layer's points, so a card opening, which
    /// moves the screen's origin, still finds the row.
    ///
    /// @param laid the geometry
    void follow_card(const Geometry& laid) {
        int32_t slot = mp::presence_card_no_slot;
        bool over = false;
        if (laid.ok && have_pointer_) {
            const kit::Point local{
                pointer_layer_.x - laid.bounds.x, pointer_layer_.y - laid.bounds.y
            };
            slot = row_at(laid, local);
            over = card_contains(laid, local);
        }
        card_.over_card(over);
        (void)card_.pointer(slot, static_cast<uint64_t>(SDL_GetTicks()));
        const int32_t focused = focused_slot_ >= 0 ? focused_slot_ : mp::presence_card_no_slot;
        (void)card_.focus(focused);
    }

    /// The place of one record in the battle room's focus order.
    ///
    /// @param laid the geometry
    /// @param index the control
    /// @return its place, or -1
    [[nodiscard]] static int32_t place_of(const Geometry& laid, int32_t index) noexcept {
        for (const FocusRecord& record : laid.focusable) {
            if (record.index == index)
                return record.focus_place;
        }
        return -1;
    }

    /// The combined Tab order. Records keep the battle room's own order. A
    /// button sits just after the record before it, and buttons that share
    /// that record follow their own Tab key.
    ///
    /// @param laid the geometry
    /// @return the stops
    [[nodiscard]] static std::vector<Stop> stops_of(const Geometry& laid) {
        std::vector<Stop> stops;
        stops.reserve(laid.focusable.size() + laid.rows.size());
        for (const FocusRecord& record : laid.focusable)
            stops.push_back(Stop{false, record.index, record.focus_place, 0});
        for (const SeatRow& row : laid.rows) {
            if (!row.button)
                continue;
            const int32_t before = record_before(laid.focusable, row.at);
            const int32_t place = before >= 0 ? place_of(laid, before) : -1;
            stops.push_back(Stop{true, row.slot, place, order_key(row.at)});
        }
        std::sort(stops.begin(), stops.end(), [](const Stop& left, const Stop& right) {
            if (left.place != right.place)
                return left.place < right.place;
            if (left.button != right.button)
                return !left.button;
            return left.key < right.key;
        });
        return stops;
    }

    /// Takes the focus onto a button and clears the battle room's ring.
    ///
    /// @param slot the slot
    /// @return redraw
    LayerAnswer take_focus(int32_t slot) {
        focused_slot_ = slot;
        pressed_slot_ = -1;
        mp::multiplayer_set_battle_room_focus(mp::kNoControl);
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Hands the focus to a record and clears the button's ring.
    ///
    /// @param index the record; 0 is a real control
    /// @return redraw
    LayerAnswer hand_focus(int32_t index) {
        focused_slot_ = -1;
        mp::multiplayer_set_battle_room_focus(index);
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Hands the focus to one stop.
    ///
    /// @param stop the stop
    /// @return redraw, or none when a record cannot take it
    LayerAnswer take_stop(const Stop& stop) {
        if (stop.button)
            return take_focus(stop.id);
        if (stop.id >= 0)
            return hand_focus(stop.id);
        return LayerAnswer::none;
    }

    /// The seat button above or below, when there is one.
    ///
    /// @param laid the geometry
    /// @param slot the focused slot
    /// @param down true for the row below
    /// @return its slot, or -1
    [[nodiscard]] static int32_t neighbour_slot(const Geometry& laid, int32_t slot, bool down) {
        const SeatRow* current = button_of(laid, slot);
        if (current == nullptr)
            return -1;
        int32_t found = -1;
        int32_t best_y = 0;
        int32_t best_x = 0;
        for (const SeatRow& row : laid.rows) {
            if (!row.button || row.slot == slot)
                continue;
            const bool that_way = down ? row.at.y > current->at.y : row.at.y < current->at.y;
            if (!that_way)
                continue;
            const bool closer = found < 0 || (down ? row.at.y < best_y : row.at.y > best_y) ||
                                (row.at.y == best_y && row.at.x < best_x);
            if (!closer)
                continue;
            found = row.slot;
            best_y = row.at.y;
            best_x = row.at.x;
        }
        return found;
    }

    /// Answers one key, for the keyboard and for the pad actions that become keys.
    ///
    /// @param pressed the key
    /// @return none or redraw when the screen takes it; pass otherwise
    LayerAnswer answer_key(kit::Key pressed) {
        const Geometry laid = geometry(view_);
        if (!laid.ok)
            return LayerAnswer::pass;
        const int32_t current = mp::multiplayer_battle_room_focus();
        if (focused_slot_ < 0) {
            if (pressed == kit::Key::tab || pressed == kit::Key::back_tab) {
                const SeatRow* best = nullptr;
                for (const SeatRow& row : laid.rows) {
                    if (!row.button)
                        continue;
                    if (pressed == kit::Key::tab) {
                        const int32_t before = record_before(laid.focusable, row.at);
                        if (before < 0 || current != before)
                            continue;
                        if (best == nullptr || order_key(row.at) < order_key(best->at))
                            best = &row;
                    } else {
                        const int32_t after = record_after(laid.focusable, row.at);
                        if (after < 0 || current != after)
                            continue;
                        if (best == nullptr || order_key(row.at) > order_key(best->at))
                            best = &row;
                    }
                }
                if (best != nullptr)
                    return take_focus(best->slot);
            }
            return LayerAnswer::pass;
        }
        if (pressed == kit::Key::enter || pressed == kit::Key::space) {
            const SeatRow* row = button_of(laid, focused_slot_);
            if (row != nullptr && row->enabled)
                open_picker(focused_slot_);
            return LayerAnswer::none;
        }
        const std::vector<Stop> stops = stops_of(laid);
        std::size_t at = stops.size();
        for (std::size_t found = 0; found < stops.size(); ++found) {
            if (stops[found].button && stops[found].id == focused_slot_)
                at = found;
        }
        if ((pressed == kit::Key::tab || pressed == kit::Key::back_tab) && at < stops.size() &&
            !stops.empty()) {
            const std::size_t next = pressed == kit::Key::tab
                                         ? (at + 1 < stops.size() ? at + 1 : 0)
                                         : (at > 0 ? at - 1 : stops.size() - 1);
            return take_stop(stops[next]);
        }
        const SeatRow* focused_row = button_of(laid, focused_slot_);
        if (focused_row == nullptr)
            return LayerAnswer::pass;
        if (pressed == kit::Key::up || pressed == kit::Key::down) {
            const int32_t neighbour =
                neighbour_slot(laid, focused_slot_, pressed == kit::Key::down);
            if (neighbour >= 0)
                return take_focus(neighbour);
            const Aim aim = pressed == kit::Key::down ? Aim::down : Aim::up;
            const int32_t next = nearest_record(laid.focusable, focused_row->at, aim);
            return next >= 0 ? hand_focus(next) : LayerAnswer::none;
        }
        if (pressed == kit::Key::left || pressed == kit::Key::right) {
            const Aim aim = pressed == kit::Key::left ? Aim::left : Aim::right;
            const int32_t next = nearest_record(laid.focusable, focused_row->at, aim);
            return next >= 0 ? hand_focus(next) : LayerAnswer::none;
        }
        return LayerAnswer::pass;
    }

    /// Opens the picker on one seat. USE THIS writes that computer, or every
    /// computer this machine runs. A disabled button opens nothing.
    ///
    /// @param slot the slot
    void open_picker(int32_t slot) {
        const Geometry laid = geometry(view_);
        const SeatRow* row = button_of(laid, slot);
        if (row == nullptr || !row->enabled)
            return;
        BrainPickerRequest request;
        request.setup = brain::Setup::multiplayer;
        mp::Lobby& lobby = mp::multiplayer_lobby();
        request.seat_name = copied_field(
            mp::slot_player(lobby, slot).name, sizeof(mp::slot_player(lobby, slot).name)
        );
        request.current = row->choice;
        request.setup_difficulty = brain::Difficulty::hard;
        request.built_in_level_fixed = false;
        request.offer_every_seat = true;
        Runtime* runtime = &runtime_;
        const uint32_t player_id = row->player_id;
        request.used = [runtime, player_id](picker::Choice choice, bool every) {
            BrainRoomAccess::write(*runtime, mp::multiplayer_lobby(), player_id, choice, every);
        };
        request.cancelled = [] {};
        runtime_.open_brain_picker(std::move(request));
    }

    Runtime& runtime_;               ///< the runtime whose room it shows over
    LayerView view_{};               ///< the view it was last laid out for
    mp::PresenceCardTrigger card_{}; ///< when a seat's card opens and closes
    int32_t hovered_slot_{-1};       ///< the button the pointer is over; -1 for none
    int32_t pressed_slot_{-1};       ///< the button a press is held on; -1 for none
    int32_t focused_slot_{-1};       ///< the button the keyboard focus is on; -1 for none
    bool have_pointer_{};            ///< the pointer has been seen
    kit::Point pointer_layer_{};     ///< the pointer, in the layer's points
    bool card_pressed_{};            ///< a press started on the open card
    std::string seal_{};             ///< the rows and the card last drawn
    uint64_t revision_{};            ///< counts the changes to how it looks
};

void Runtime::keep_brain_room_screen() {
    note_room_computers(mp::multiplayer_lobby());
    OaLayer& layer = oa_layer();
    if (layer.find(screen_name) != nullptr)
        return;
    if (!BrainRoomScreen::wanted(*this))
        return;
    layer.push(std::make_unique<BrainRoomScreen>(*this));
}

void Runtime::note_room_computers(mp::Lobby& lobby) {
    if (lobby.game == nullptr) {
        room_brain_choices_.clear();
        room_brain_cursor_ = 0;
        return;
    }

    struct LocalComputer {
        uint32_t player_id{};
        uint8_t slot{};
    };

    std::vector<LocalComputer> locals;
    for (int32_t slot = 0; slot < mp::kSlotCount; ++slot) {
        const oa::Player& player = mp::slot_player(lobby, slot);
        if (!own_computer(player))
            continue;
        locals.push_back(LocalComputer{player.player_id, static_cast<uint8_t>(slot)});
    }
    if (locals.empty()) {
        room_brain_choices_.clear();
        room_brain_cursor_ = 0;
        return;
    }
    std::vector<RoomBrainChoice> kept;
    kept.reserve(room_brain_choices_.size());
    for (RoomBrainChoice& held : room_brain_choices_) {
        for (const LocalComputer& local : locals) {
            if (local.player_id != held.player_id)
                continue;
            held.slot = local.slot;
            kept.push_back(std::move(held));
            break;
        }
    }
    room_brain_choices_ = std::move(kept);

    std::vector<brain::Seat> remembered;
    if (const std::optional<brain::Seats> stored =
            remembered_brain_seats(brain::Setup::multiplayer)) {
        remembered = stored->seats;
        std::stable_sort(
            remembered.begin(),
            remembered.end(),
            [](const brain::Seat& left, const brain::Seat& right) {
                return left.place < right.place;
            }
        );
    }
    const brain::Listing& listing = installed_brains();
    const library::Inputs probe = fit_inputs(engine_settings_state(), content_choice());
    const std::string default_id = default_brain();
    for (const LocalComputer& local : locals) {
        bool have = false;
        for (const RoomBrainChoice& held : room_brain_choices_) {
            if (held.player_id == local.player_id)
                have = true;
        }
        if (have)
            continue;
        RoomBrainChoice made;
        made.player_id = local.player_id;
        made.slot = local.slot;
        const std::string launch_id = lobby.computer_launch_brain[local.slot];
        if (!launch_id.empty()) {
            made.choice = choice_from_launch(
                listing, probe, launch_id, local.slot, difficulty_of_launch(lobby, local.slot)
            );
        } else if (room_brain_cursor_ < remembered.size()) {
            const brain::Seat seat = remembered[room_brain_cursor_];
            ++room_brain_cursor_;
            made.choice = choice_from_seat(listing, probe, default_id, local.slot, seat);
        } else {
            made.choice = default_choice(listing, probe, default_id, nullptr);
        }
        room_brain_choices_.push_back(std::move(made));
    }
    for (const RoomBrainChoice& held : room_brain_choices_)
        publish_choice(*this, lobby, static_cast<int32_t>(held.slot), held.choice, listing, probe);
}

void Runtime::keep_remote_played_brains(const mp::Lobby& lobby) {
    remote_played_brains_.clear();
    if (match_ == nullptr || lobby.game == nullptr)
        return;
    for (int32_t slot = 0; slot < mp::kSlotCount; ++slot) {
        const oa::Player& player = lobby.game->players[slot];
        if (own_computer(player))
            continue;
        const mp::ComputerBrain* shown = mp::lobby_computer_brain(lobby, slot);
        if (shown == nullptr || shown->id.empty())
            continue;
        RemotePlayedBrain label;
        label.player = slot;
        label.id = shown->id;
        label.name = shown->name;
        label.version = shown->version;
        label.level_id = shown->level_id;
        label.level_name = shown->level;
        remote_played_brains_.push_back(std::move(label));
    }
}

brain::Seats Runtime::room_brain_seats() const {
    brain::Seats seats;
    seats.seats.reserve(room_brain_choices_.size());
    for (const RoomBrainChoice& held : room_brain_choices_) {
        brain::Seat seat;
        seat.place = static_cast<int32_t>(held.slot);
        seat.brain = held.choice.brain;
        seat.level = held.choice.level;
        seat.options = held.choice.options;
        seats.seats.push_back(std::move(seat));
    }
    return seats;
}

void Runtime::apply_room_brain_choice(
    mp::Lobby& lobby, uint32_t player_id, const picker::Choice& choice, bool every
) {
    const brain::Listing& listing = installed_brains();
    const library::Inputs probe = fit_inputs(engine_settings_state(), content_choice());
    for (RoomBrainChoice& held : room_brain_choices_) {
        if (!every && held.player_id != player_id)
            continue;
        held.choice = choice;
        publish_choice(*this, lobby, static_cast<int32_t>(held.slot), held.choice, listing, probe);
    }
}

} // namespace oa::app
