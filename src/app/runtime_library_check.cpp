// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-library: the Library over a feed of made-up packages, with no
// network. With no update waiting the main menu's OA button draws its count
// of the fixture's added packages (two mods, one map pack and two mutators)
// and opens the Library on Active, which reads the mod played, the language
// in use and the installed maps, and whose CLOSE goes back; with three it
// wears its dot, says "Library · 5 packages · 4 updates" to automation and
// opens the Library on Active again, whose Mutators tab reads "Mutators (5)"
// and whose Brains tab reads "Brains (4)" with the built-in row first.
// Its Updates tab opens on its Updates filter with UPDATE ALL over the list.
// GET and CANCEL reach the feed, OPEN FOLDER and HOMEPAGE the recorded
// openers, ROLL BACK asks the Roll Back Mod question over the Library and
// PLAY NOW asks for a soft restart. The settings shortcut opens Settings over the Library, Settings ›
// Language's MORE IN THE LIBRARY… the Library over Settings, and never two
// of either; the shortcut and the in-match OA button open Settings. At
// 640x480 and 1920x1080. Then, at 640x480, a stand-in controller: Y focuses
// the search, RB and LB change the tab and come back, X queues the selected
// entry's UPDATE, the right stick scrolls the list, and B leaves Compact's
// details and then closes the Library. The same buttons over Settings do
// what they did before.

#include "engine_settings_menu_host.hpp"
#include "engine_settings_state.hpp"
#include "library_host.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "pad_state.hpp"
#include "user_folder_state.hpp"
#include "web_link_state.hpp"

#include "oa/app/automation_host.hpp"
#include "oa/app/content/downloads.hpp"
#include "oa/app/content/updates.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/package_install/oamod.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/engine_settings/prompt.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/library/library.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

namespace oa::app {

namespace {

namespace settings = oa::ui::engine_settings;
namespace kit = oa::ui::kit;
namespace layout = oa::ui::display_layout;
namespace library = oa::ui::library;

/// The modifier the settings' shortcut takes on this platform.
#ifdef SDL_PLATFORM_MACOS
constexpr SDL_Keymod kShortcutModifier = SDL_KMOD_GUI;
#else
constexpr SDL_Keymod kShortcutModifier = SDL_KMOD_CTRL;
#endif

/// The made-up built-in registry.
constexpr std::string_view kBuiltIn = "fixture-core";
/// The made-up registry the player added.
constexpr std::string_view kAdded = "fixture-extra";
/// Ridge's homepage, which HOMEPAGE records.
constexpr std::string_view kRidgeHomepage = "http://ridge.example.net/";
/// Where the pointer rests: over no control of the main menu or the button.
constexpr layout::Point kRestingPointer{8, 8};

/// One made-up mod the check writes into the player's Mods folder: Ridge,
/// installed at revision 3 and keeping revision 2 to roll back to, and
/// Lattice. Neither changes the game's rules.
struct FixtureMod {
    std::string_view folder;  ///< the folder's name
    std::string_view profile; ///< its oamod.yaml
    std::string_view backup;  ///< its .backup's oamod.yaml; empty for none
};

/// The made-up mods.
constexpr std::array<FixtureMod, 2> kFixtureMods{{
    {"library-check-ridge",
     "oamod: 1\n"
     "id: ridge\n"
     "name: Ridge\n"
     "version: \"4.8\"\n"
     "description: A made-up mod the Library check plays.\n"
     "requires: {base: ta-3.1c, catalogue: 1}\n"
     "author: {name: unknown}\n"
     "packaging: {revision: 3, date: 2026-10-11, packager: Open Annihilation}\n",
     "oamod: 1\n"
     "id: ridge\n"
     "name: Ridge\n"
     "version: \"4.8\"\n"
     "description: A made-up mod the Library check plays.\n"
     "requires: {base: ta-3.1c, catalogue: 1}\n"
     "author: {name: unknown}\n"
     "packaging: {revision: 2, date: 2026-10-10, packager: Open Annihilation}\n"},
    {"library-check-lattice",
     "oamod: 1\n"
     "id: lattice\n"
     "name: Lattice\n"
     "version: \"3.0\"\n"
     "description: A second made-up mod of the Library check.\n"
     "requires: {base: ta-3.1c, catalogue: 1}\n"
     "author: {name: unknown}\n"
     "packaging: {revision: 3, date: 2026-10-11, packager: Open Annihilation}\n",
     {}},
}};

/// One call the Library made of the fixture feed.
struct FeedCall {
    std::string registry{};                                           ///< the registry's id
    std::string key{};                                                ///< the package's key
    content::DownloadReason reason{content::DownloadReason::install}; ///< enqueue's reason
};

/// The fixture feed the check gives the Library: its inputs, which the check
/// changes, and what the Library asked of it.
struct FixtureFeed {
    library::Inputs inputs{};          ///< what the Library is built from
    uint64_t generation{1};            ///< moves whenever the check changes inputs
    std::vector<FeedCall> enqueued{};  ///< GET's and UPDATE's, in order
    std::vector<FeedCall> cancelled{}; ///< CANCEL's, in order
    std::vector<FeedCall> retried{};   ///< RETRY's, in order
    uint64_t next_item{1};             ///< the next queue item enqueue gives
};

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("library check: " + std::string(what));
}

/// The fixture's inputs.
///
/// @param context the fixture
/// @return its inputs
library::Inputs fixture_inputs(void* context) {
    return static_cast<FixtureFeed*>(context)->inputs;
}

/// The fixture's generation.
///
/// @param context the fixture
/// @return the generation
uint64_t fixture_generation(void* context) {
    return static_cast<FixtureFeed*>(context)->generation;
}

/// Records a download asked for, and gives it a queue item.
///
/// @param context the fixture
/// @param registry the registry's id
/// @param key the package's key
/// @param reason why
/// @return the item
std::optional<uint64_t> fixture_enqueue(
    void* context,
    std::string_view registry,
    std::string_view key,
    content::DownloadReason reason,
    std::string* /*why*/
) {
    auto& fixture = *static_cast<FixtureFeed*>(context);
    fixture.enqueued.push_back({std::string(registry), std::string(key), reason});
    return fixture.next_item++;
}

/// Records a cancel.
///
/// @param context the fixture
/// @param registry the registry's id
/// @param key the package's key
void fixture_cancel(void* context, std::string_view registry, std::string_view key) {
    static_cast<FixtureFeed*>(context)->cancelled.push_back(
        {std::string(registry), std::string(key), content::DownloadReason::install}
    );
}

/// Records a retry.
///
/// @param context the fixture
/// @param registry the registry's id
/// @param key the package's key
void fixture_retry(void* context, std::string_view registry, std::string_view key) {
    static_cast<FixtureFeed*>(context)->retried.push_back(
        {std::string(registry), std::string(key), content::DownloadReason::install}
    );
}

/// Tells whether a registry of the fixture's inputs is built in: the
/// Registry::built_in its inputs give.
///
/// @param context the fixture
/// @param registry the registry's id
/// @return true for the built-in one
bool fixture_built_in(void* context, std::string_view registry) {
    for (const library::Registry& listed : static_cast<FixtureFeed*>(context)->inputs.registries)
        if (listed.id == registry)
            return listed.built_in;
    return false;
}

/// Returns a made-up mod's listing.
///
/// @param registry the registry's id
/// @param key its key
/// @param name its name
/// @param version its version
/// @param revision its revision
/// @param release its release
/// @return the listing
library::Listing mod_listing(
    std::string_view registry,
    std::string_view key,
    std::string_view name,
    std::string_view version,
    int64_t revision,
    int64_t release
) {
    library::Listing listing;
    listing.registry = std::string(registry);
    listing.kind = library::Kind::mod;
    listing.key = std::string(key);
    listing.name = std::string(name);
    listing.version = std::string(version);
    listing.revision = revision;
    listing.release = release;
    listing.author = "the " + std::string(name) + " team";
    listing.summary = "A made-up mod of the Library check.";
    listing.base = std::string(library::supported_base);
    listing.size = 3U * 1024U * 1024U;
    return listing;
}

/// Returns a made-up mutator's listing, with no badge.
///
/// @param key its key
/// @param name its name
/// @param version its version
/// @param facts its tags, works-with list and prerequisites
/// @return the listing
library::Listing mutator_listing(
    std::string_view key,
    std::string_view name,
    std::string_view version,
    library::MutatorFacts facts
) {
    library::Listing listing;
    listing.registry = std::string(kBuiltIn);
    listing.kind = library::Kind::mutator;
    listing.key = std::string(key);
    listing.name = std::string(name);
    listing.version = std::string(version);
    listing.revision = 1;
    listing.release = 1;
    listing.author = "the " + std::string(name) + " team";
    listing.summary = "A made-up mutator of the Library check.";
    listing.size = 1024U * 1024U;
    listing.mutator = std::move(facts);
    return listing;
}

/// Returns a made-up brain's listing, with no badge.
///
/// @param key its key
/// @param name its name
/// @param version its version
/// @param facts its tags, works-with list and what it requires
/// @return the listing
library::Listing brain_listing(
    std::string_view key, std::string_view name, std::string_view version, library::BrainFacts facts
) {
    library::Listing listing;
    listing.registry = std::string(kBuiltIn);
    listing.kind = library::Kind::brain;
    listing.key = std::string(key);
    listing.name = std::string(name);
    listing.version = std::string(version);
    listing.revision = 1;
    listing.release = 1;
    listing.author = "the " + std::string(name) + " team";
    listing.summary = "A made-up brain of the Library check.";
    listing.size = 1024U * 1024U;
    listing.brain = std::move(facts);
    return listing;
}

/// Returns the fixture's inputs: two registries, Ridge and Lattice installed
/// from the built-in one, Cinder to get from it and Dunes from the added one,
/// queued; with or without an update of each installed mod. Five made-up
/// mutators from the built-in registry: Fast Builders and Gatehouse installed,
/// and Fast Builders' update when updates wait; Gatehouse Keep, Ridge Walls
/// and Grounded to get. Four made-up brains: Steady Commander and Lantern
/// Scout installed, Tide Keeper installed with an update when updates wait,
/// and Ridge Raider to get. For the Active tab, Basalt is played from a folder
/// outside the Mods folder, a German pack shows the game's words and one map
/// pack is installed. The played mod's id stays empty, so Ridge Walls does
/// not work with the base game and Ridge Raider is made for Ridge, and both
/// are still listed. An installed brain is not one of the added packages.
///
/// @param mods the player's Mods folder
/// @param updates the two updates wait
/// @return the inputs
library::Inputs fixture_inputs_of(const fs::path& mods, bool updates) {
    library::Inputs inputs;
    // The two empty strings are the title and the description. The card heading
    // is the short name, and the check does not show a description.
    inputs.registries.push_back({std::string(kBuiltIn), "Fixture Core", {}, {}, true, true, {}});
    inputs.registries.push_back({std::string(kAdded), "Fixture Extra", {}, {}, false, true, {}});
    library::Listing ridge = mod_listing(kBuiltIn, "ridge", "Ridge", "4.8", 4, 4);
    ridge.homepage = std::string(kRidgeHomepage);
    inputs.listings.push_back(std::move(ridge));
    inputs.listings.push_back(mod_listing(kBuiltIn, "lattice", "Lattice", "3.0", 4, 4));
    inputs.listings.push_back(mod_listing(kBuiltIn, "cinder", "Cinder", "1.2", 1, 1));
    inputs.listings.push_back(mod_listing(kAdded, "dunes", "Dunes", "0.9", 1, 1));

    library::Installed ridge_installed;
    ridge_installed.kind = library::Kind::mod;
    ridge_installed.key = "ridge";
    ridge_installed.folder = mods / std::string(kFixtureMods[0].folder);
    ridge_installed.name = "Ridge";
    ridge_installed.version = "4.8";
    ridge_installed.revision = 3;
    ridge_installed.registry = std::string(kBuiltIn);
    ridge_installed.release = 3;
    ridge_installed.kept = library::Kept{"4.8", 2};
    inputs.installed.push_back(std::move(ridge_installed));
    library::Installed lattice_installed;
    lattice_installed.kind = library::Kind::mod;
    lattice_installed.key = "lattice";
    lattice_installed.folder = mods / std::string(kFixtureMods[1].folder);
    lattice_installed.name = "Lattice";
    lattice_installed.version = "3.0";
    lattice_installed.revision = 3;
    lattice_installed.registry = std::string(kBuiltIn);
    lattice_installed.release = 3;
    inputs.installed.push_back(std::move(lattice_installed));

    library::MutatorFacts fast_facts;
    fast_facts.tags = {"economy", "statistics"};
    fast_facts.works_with.push_back({"*", {}, {}});
    inputs.listings.push_back(mutator_listing("fast-builders", "Fast Builders", "1.0", fast_facts));
    library::Installed fast_installed;
    fast_installed.kind = library::Kind::mutator;
    fast_installed.key = "fast-builders";
    fast_installed.folder = "fast-builders";
    fast_installed.name = "Fast Builders";
    fast_installed.version = "1.0";
    fast_installed.revision = 1;
    fast_installed.registry = std::string(kBuiltIn);
    fast_installed.release = 1;
    fast_installed.mutator = fast_facts;
    inputs.installed.push_back(std::move(fast_installed));

    library::MutatorFacts gatehouse_facts;
    gatehouse_facts.tags = {"gameplay", "new-units"};
    inputs.listings.push_back(mutator_listing("gatehouse", "Gatehouse", "4.1", gatehouse_facts));
    library::Installed gatehouse_installed;
    gatehouse_installed.kind = library::Kind::mutator;
    gatehouse_installed.key = "gatehouse";
    gatehouse_installed.folder = "gatehouse";
    gatehouse_installed.name = "Gatehouse";
    gatehouse_installed.version = "4.1";
    gatehouse_installed.revision = 1;
    gatehouse_installed.registry = std::string(kBuiltIn);
    gatehouse_installed.release = 1;
    gatehouse_installed.mutator = gatehouse_facts;
    inputs.installed.push_back(std::move(gatehouse_installed));

    library::MutatorFacts keep_facts;
    keep_facts.tags = {"economy"};
    keep_facts.needs.push_back({"gatehouse", ">= 4.0", "Gatehouse"});
    inputs.listings.push_back(
        mutator_listing("gatehouse-keep", "Gatehouse Keep", "2.0", std::move(keep_facts))
    );
    library::MutatorFacts wall_facts;
    wall_facts.works_with.push_back({"ridge", {}, "Ridge"});
    wall_facts.tags = {"commanders"};
    inputs.listings.push_back(mutator_listing("ridge-walls", "Ridge Walls", "1.0", wall_facts));
    library::MutatorFacts grounded_facts;
    grounded_facts.tags = {"disable"};
    inputs.listings.push_back(
        mutator_listing("grounded", "Grounded", "1.0", std::move(grounded_facts))
    );

    const auto install_brain =
        [&](std::string_view key, std::string_view name, const library::BrainFacts& facts) {
            library::Installed installed;
            installed.kind = library::Kind::brain;
            installed.key = std::string(key);
            installed.folder = std::string(key);
            installed.name = std::string(name);
            installed.version = "1.0";
            installed.revision = 1;
            installed.registry = std::string(kBuiltIn);
            installed.release = 1;
            installed.brain = facts;
            inputs.installed.push_back(std::move(installed));
        };
    library::BrainFacts steady_facts;
    steady_facts.tags = {"classic"};
    steady_facts.works_with.push_back({"*", {}, {}});
    inputs.listings.push_back(
        brain_listing("steady-commander", "Steady Commander", "1.0", steady_facts)
    );
    install_brain("steady-commander", "Steady Commander", steady_facts);
    library::BrainFacts lantern_facts;
    lantern_facts.tags = {"economy"};
    lantern_facts.full_awareness = true;
    install_brain("lantern-scout", "Lantern Scout", lantern_facts);
    library::BrainFacts tide_facts;
    tide_facts.tags = {"turtle"};
    tide_facts.works_with.push_back({"*", {}, {}});
    inputs.listings.push_back(brain_listing("tide-keeper", "Tide Keeper", "1.0", tide_facts));
    install_brain("tide-keeper", "Tide Keeper", tide_facts);
    library::BrainFacts raider_facts;
    raider_facts.tags = {"rush"};
    raider_facts.works_with.push_back({"ridge", {}, "Ridge"});
    inputs.listings.push_back(brain_listing("ridge-raider", "Ridge Raider", "1.0", raider_facts));

    if (updates) {
        for (const std::string_view key :
             {std::string_view("ridge"), std::string_view("lattice")}) {
            library::UpdateNote note;
            note.registry = std::string(kBuiltIn);
            note.kind = library::Kind::mod;
            note.key = std::string(key);
            note.to_release = 4;
            note.to_version = key == "ridge" ? "4.8" : "3.0";
            note.to_revision = 4;
            note.size = 1024U * 1024U;
            inputs.updates.push_back(std::move(note));
        }
        library::UpdateNote fast_update;
        fast_update.registry = std::string(kBuiltIn);
        fast_update.kind = library::Kind::mutator;
        fast_update.key = "fast-builders";
        fast_update.to_release = 2;
        fast_update.to_version = "1.1";
        fast_update.to_revision = 2;
        fast_update.size = 1024U * 1024U;
        inputs.updates.push_back(std::move(fast_update));
        library::UpdateNote tide_update;
        tide_update.registry = std::string(kBuiltIn);
        tide_update.kind = library::Kind::brain;
        tide_update.key = "tide-keeper";
        tide_update.to_release = 2;
        tide_update.to_version = "1.1";
        tide_update.to_revision = 2;
        tide_update.size = 1024U * 1024U;
        inputs.updates.push_back(std::move(tide_update));
    }

    library::QueueItem dunes;
    dunes.registry = std::string(kAdded);
    dunes.kind = library::Kind::mod;
    dunes.key = "dunes";
    dunes.name = "Dunes";
    dunes.version = "0.9";
    dunes.phase = library::QueueItem::Phase::waiting;
    dunes.total_bytes = 3U * 1024U * 1024U;
    inputs.queue.push_back(std::move(dunes));

    inputs.engine = content::this_engine_rules().engine;
    inputs.version_text = "v0.8.0";
    inputs.list_age_seconds = 4 * 60;
    inputs.playing_mod = library::PlayingMod{"Basalt", "2.1", std::nullopt};
    inputs.language = {"Deutsch", "Deutsch", "1.2"};
    inputs.map_packs = {
        {"Fixture Isles", {{"Isle Hop", 4, "16x16"}, {"Long Strait", 2, "20x12"}}},
    };
    return inputs;
}

/// How many installed mods with an update the controller step adds, so the
/// list at 640×480 is long enough for the right stick to scroll.
constexpr int kSpurUpdates = 24;

/// Appends installed mods that each have an update, named Spur 0 onward, on
/// the built-in registry. Their folders are not written: the step queues an
/// update and scrolls, and does not open a folder.
///
/// @param[in,out] inputs the fixture's inputs
/// @param count how many to append
void append_spur_updates(library::Inputs& inputs, int count) {
    for (int index = 0; index < count; ++index) {
        const std::string key = "spur-" + std::to_string(index);
        const std::string name = "Spur " + std::to_string(index);
        inputs.listings.push_back(mod_listing(kBuiltIn, key, name, "1.0", 2, 2));

        library::Installed installed;
        installed.kind = library::Kind::mod;
        installed.key = key;
        installed.folder = "spur-unused";
        installed.name = name;
        installed.version = "1.0";
        installed.revision = 1;
        installed.registry = std::string(kBuiltIn);
        installed.release = 1;
        inputs.installed.push_back(std::move(installed));

        library::UpdateNote note;
        note.registry = std::string(kBuiltIn);
        note.kind = library::Kind::mod;
        note.key = key;
        note.to_release = 2;
        note.to_version = "1.0";
        note.to_revision = 2;
        note.size = 1024U * 1024U;
        inputs.updates.push_back(std::move(note));
    }
}

/// A joystick input a gamepad button or axis is bound to.
struct PadBinding {
    SDL_GamepadBindingType type{SDL_GAMEPAD_BINDTYPE_NONE}; ///< button, axis or hat
    int index{-1};                                          ///< the joystick button, axis or hat
    int hat_mask{};                                         ///< the hat's direction, for a hat
    int input_least{};  ///< the joystick axis's value at the bound range's start
    int input_most{};   ///< the joystick axis's value at its end
    int output_least{}; ///< the gamepad axis's value at the start
    int output_most{};  ///< the gamepad axis's value at the end
};

/// The largest and smallest joystick axis values.
constexpr int kAxisMost = SDL_JOYSTICK_AXIS_MAX;
/// The smallest joystick axis value.
constexpr int kAxisLeast = SDL_JOYSTICK_AXIS_MIN;

/// The buttons the controller step presses: the face and the shoulders.
constexpr std::array<SDL_GamepadButton, 6> kLibraryPadButtons{
    SDL_GAMEPAD_BUTTON_SOUTH,
    SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
};

/// Two touchpads, so the right stick is the wheel in the menus rather than a pointer.
constexpr std::array<SDL_VirtualJoystickTouchpadDesc, 2> kLibraryTouchpads{{{1, {}}, {1, {}}}};

/// The USB numbers of an ordinary gamepad the stand-in imitates.
constexpr uint16_t kLibraryPadVendor = 0x045e;
constexpr uint16_t kLibraryPadProduct = 0x02ea;

/// A virtual gamepad the controller step attaches, and detaches however the step ends.
struct LibraryPad {
    SDL_JoystickID id{};                                        ///< 0 before it is attached
    SDL_Joystick* joystick{};                                   ///< the check's own handle
    std::array<PadBinding, SDL_GAMEPAD_BUTTON_COUNT> buttons{}; ///< by gamepad button
    std::array<PadBinding, SDL_GAMEPAD_AXIS_COUNT> axes{};      ///< by gamepad axis

    /// Detaches the stand-in and closes the check's handle.
    ~LibraryPad() { detach(); }

    /// Closes the handle and detaches the stand-in.
    void detach() {
        if (joystick != nullptr)
            SDL_CloseJoystick(joystick);
        joystick = nullptr;
        if (id != 0)
            SDL_DetachVirtualJoystick(id);
        id = 0;
    }
};

/// Attaches the stand-in and reads the joystick inputs SDL binds its buttons and axes to.
///
/// @param[in,out] stand_in the stand-in
void attach_library_pad(LibraryPad& stand_in) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = kLibraryPadVendor;
    desc.product_id = kLibraryPadProduct;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis)
        desc.axis_mask |= 1U << static_cast<unsigned>(axis);
    for (const SDL_GamepadButton button : kLibraryPadButtons)
        desc.button_mask |= 1U << static_cast<unsigned>(button);
    desc.nbuttons = static_cast<Uint16>(kLibraryPadButtons.size());
    desc.ntouchpads = static_cast<Uint16>(kLibraryTouchpads.size());
    desc.touchpads = kLibraryTouchpads.data();
    desc.name = "the Library check's gamepad";
    stand_in.id = SDL_AttachVirtualJoystick(&desc);
    require(stand_in.id != 0, std::string("SDL refused the stand-in gamepad: ") + SDL_GetError());
    stand_in.joystick = SDL_OpenJoystick(stand_in.id);
    require(
        stand_in.joystick != nullptr,
        std::string("the stand-in gamepad's joystick did not open: ") + SDL_GetError()
    );
    SDL_Gamepad* gamepad = SDL_OpenGamepad(stand_in.id);
    require(
        gamepad != nullptr,
        std::string("SDL does not take the stand-in as a gamepad: ") + SDL_GetError()
    );
    require(
        SDL_GetNumGamepadTouchpads(gamepad) >= 2,
        "the stand-in gamepad reports fewer than two touchpads, so its right stick is not the wheel"
    );
    int count = 0;
    SDL_GamepadBinding** bindings = SDL_GetGamepadBindings(gamepad, &count);
    for (int at = 0; bindings != nullptr && at < count; ++at) {
        const SDL_GamepadBinding& binding = *bindings[at];
        PadBinding found{};
        found.type = binding.input_type;
        if (binding.input_type == SDL_GAMEPAD_BINDTYPE_BUTTON) {
            found.index = binding.input.button;
        } else if (binding.input_type == SDL_GAMEPAD_BINDTYPE_HAT) {
            found.index = binding.input.hat.hat;
            found.hat_mask = binding.input.hat.hat_mask;
        } else if (binding.input_type == SDL_GAMEPAD_BINDTYPE_AXIS) {
            found.index = binding.input.axis.axis;
            found.input_least = binding.input.axis.axis_min;
            found.input_most = binding.input.axis.axis_max;
        }
        if (binding.output_type == SDL_GAMEPAD_BINDTYPE_BUTTON && binding.output.button >= 0 &&
            binding.output.button < SDL_GAMEPAD_BUTTON_COUNT) {
            stand_in.buttons[static_cast<std::size_t>(binding.output.button)] = found;
        } else if (
            binding.output_type == SDL_GAMEPAD_BINDTYPE_AXIS && binding.output.axis.axis >= 0 &&
            binding.output.axis.axis < SDL_GAMEPAD_AXIS_COUNT
        ) {
            found.output_least = binding.output.axis.axis_min;
            found.output_most = binding.output.axis.axis_max;
            stand_in.axes[static_cast<std::size_t>(binding.output.axis.axis)] = found;
        }
    }
    SDL_free(bindings);
    SDL_CloseGamepad(gamepad);
    for (const SDL_GamepadButton button : kLibraryPadButtons)
        require(
            stand_in.buttons[static_cast<std::size_t>(button)].type != SDL_GAMEPAD_BINDTYPE_NONE,
            "the stand-in gamepad has no binding for a button the step presses"
        );
    require(
        stand_in.axes[static_cast<std::size_t>(SDL_GAMEPAD_AXIS_RIGHTY)].type ==
            SDL_GAMEPAD_BINDTYPE_AXIS,
        "the stand-in gamepad has no binding for its right stick"
    );
}

/// Sets a gamepad button of the stand-in through the joystick input it is bound to.
///
/// @param stand_in the stand-in
/// @param button the gamepad button
/// @param down whether it is down
void set_library_button(LibraryPad& stand_in, SDL_GamepadButton button, bool down) {
    const PadBinding& binding = stand_in.buttons[static_cast<std::size_t>(button)];
    bool ok = false;
    switch (binding.type) {
    case SDL_GAMEPAD_BINDTYPE_BUTTON:
        ok = SDL_SetJoystickVirtualButton(stand_in.joystick, binding.index, down);
        break;
    case SDL_GAMEPAD_BINDTYPE_HAT:
        ok = SDL_SetJoystickVirtualHat(
            stand_in.joystick,
            binding.index,
            static_cast<Uint8>(down ? binding.hat_mask : SDL_HAT_CENTERED)
        );
        break;
    case SDL_GAMEPAD_BINDTYPE_AXIS:
        ok = SDL_SetJoystickVirtualAxis(
            stand_in.joystick,
            binding.index,
            static_cast<Sint16>(down ? binding.input_most : binding.input_least)
        );
        break;
    case SDL_GAMEPAD_BINDTYPE_NONE:
        require(false, "the stand-in gamepad has no binding for a button the step presses");
        break;
    }
    require(ok, std::string("the stand-in gamepad's button could not be set: ") + SDL_GetError());
}

/// Sets a gamepad axis of the stand-in, in the binding's own range.
///
/// @param stand_in the stand-in
/// @param axis the gamepad axis
/// @param value -1..1, y down
void set_library_axis(LibraryPad& stand_in, SDL_GamepadAxis axis, float value) {
    const PadBinding& binding = stand_in.axes[static_cast<std::size_t>(axis)];
    require(
        binding.type == SDL_GAMEPAD_BINDTYPE_AXIS,
        "the stand-in gamepad has no binding for an axis the step moves"
    );
    const double wanted = value >= 0.0F ? static_cast<double>(value) * kAxisMost
                                        : static_cast<double>(-value) * kAxisLeast;
    double part = 0.0;
    if (binding.output_most != binding.output_least)
        part = (wanted - static_cast<double>(binding.output_least)) /
               static_cast<double>(binding.output_most - binding.output_least);
    const double raw =
        binding.input_least + part * static_cast<double>(binding.input_most - binding.input_least);
    const auto clamped =
        static_cast<Sint16>(std::clamp(static_cast<int>(std::lround(raw)), kAxisLeast, kAxisMost));
    require(
        SDL_SetJoystickVirtualAxis(stand_in.joystick, binding.index, clamped),
        std::string("the stand-in gamepad's axis could not be set: ") + SDL_GetError()
    );
}

/// Returns a folder as the settings keep it: absolute, normal, in UTF-8.
///
/// @param folder the folder
/// @return its path
std::string kept_path(const fs::path& folder) {
    std::error_code error;
    const fs::path absolute = fs::absolute(folder, error);
    return path_to_utf8((error ? folder : absolute).lexically_normal());
}

/// Returns the path of one step's snapshot: <stem>-<step>.ppm beside --snapshot.
///
/// @param snapshot the --snapshot path
/// @param step the step's name
/// @return the step's snapshot path
fs::path step_snapshot(const fs::path& snapshot, std::string_view step) {
    return snapshot.parent_path() / (snapshot.stem().string() + '-' + std::string(step) + ".ppm");
}

/// Writes a text file, making its folder.
///
/// @param file the file
/// @param text its text
void write_text(const fs::path& file, std::string_view text) {
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(out), "a made-up mod could not be written");
}

/// Counts the pixels of a rectangle in which two pictures of one size differ.
///
/// @param left one picture
/// @param right the other
/// @param area the rectangle, in their pixels
/// @param[out] outside how many of those lie outside `inside`
/// @param inside a rectangle within the area
/// @return the pixels that differ in the area
std::size_t differing_in(
    const renderer::Surface& left,
    const renderer::Surface& right,
    const kit::Rect& area,
    std::size_t& outside,
    const kit::Rect& inside
) {
    outside = 0;
    if (left.width != right.width || left.height != right.height)
        return static_cast<std::size_t>(area.width) * static_cast<std::size_t>(area.height);
    std::size_t differing = 0;
    for (int32_t y = area.y; y < area.y + area.height; ++y)
        for (int32_t x = area.x; x < area.x + area.width; ++x) {
            if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= left.width ||
                static_cast<uint32_t>(y) >= left.height)
                continue;
            const std::size_t at =
                (static_cast<std::size_t>(y) * left.width + static_cast<std::size_t>(x)) * 3;
            if (left.rgb[at] == right.rgb[at] && left.rgb[at + 1] == right.rgb[at + 1] &&
                left.rgb[at + 2] == right.rgb[at + 2])
                continue;
            ++differing;
            if (x < inside.x || y < inside.y || x >= inside.x + inside.width ||
                y >= inside.y + inside.height)
                ++outside;
        }
    return differing;
}

} // namespace

void Runtime::check_library() {
    require(sdl_.window != nullptr, "the check needs the SDL presenter");
    require(!user_folder_.empty(), "the run has no folder of the player's own");
    const fs::path mods = user_folder_ / std::string(user_mods_folder_name);
    for (const FixtureMod& mod : kFixtureMods) {
        write_text(mods / std::string(mod.folder) / "oamod.yaml", mod.profile);
        if (!mod.backup.empty())
            write_text(
                mods / std::string(mod.folder) / std::string(package_install::backup_folder_name) /
                    "oamod.yaml",
                mod.backup
            );
    }
    list_offered_mods();

    // The Library takes the fixture's feed from the first frame on, and the
    // runtime's own back as the check ends.
    FixtureFeed fixture;
    fixture.inputs = fixture_inputs_of(mods, false);
    LibraryHost& host = library_host();
    const LibraryFeed own = host.feed;

    struct OwnFeedBack {
        LibraryHost& host;
        LibraryFeed feed;

        ~OwnFeedBack() {
            host.feed = feed;
            host.own_feed = true;
            host.refreshed = false;
        }
    } own_back{host, own};

    host.feed = LibraryFeed{
        &fixture,
        fixture_inputs,
        fixture_generation,
        fixture_enqueue,
        fixture_cancel,
        fixture_retry,
        fixture_built_in
    };
    host.own_feed = false;
    host.refreshed = false;

    const auto* fonts = engine_settings_fonts();
    require(fonts != nullptr, "the dialog's fonts did not load");
    const auto icon = EngineSettingsMenuHost::mods_icon(*this);
    require(
        oa::ui::frontend_renderer::picture_drawable(icon), "the Library button has no icon to show"
    );
    bool running = true;
    const auto window_of = [this](int width, int height) {
        require(
            SDL_SetWindowSize(sdl_.window, width, height) && SDL_SyncWindow(sdl_.window),
            "the window did not take " + std::to_string(width) + 'x' + std::to_string(height)
        );
        apply_output_mode();
    };
    const auto tap = [this](SDL_Keycode code, SDL_Keymod modifiers = SDL_KMOD_NONE) {
        bool going = true;
        for (const bool down : {true, false}) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.windowID = SDL_GetWindowID(sdl_.window);
            event.key.key = code;
            event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
            event.key.mod = modifiers;
            event.key.down = down;
            dispatch_event(event, going);
        }
        require(going, "a key ended the run");
    };
    const auto named = [this](std::string_view name) -> std::optional<AutomationControl> {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls)
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        return std::nullopt;
    };
    const auto click_named = [&](std::string_view name) {
        const std::optional<AutomationControl> control = named(name);
        require(control.has_value(), std::string(name) + " is not shown");
        const layout::Point pixel{
            control->window_x + control->window_width / 2,
            control->window_y + control->window_height / 2
        };
        for (const SDL_EventType type :
             {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event = window_pointer_event(
                sdl_.window,
                sdl_.renderer,
                type,
                pixel,
                type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
            );
            bool going = true;
            dispatch_event(event, going);
            require(going, "a click ended the run");
        }
    };
    // With --snapshot, the whole window as presented, beside the snapshot.
    const auto snapshot = [this](const std::string& step) {
        if (options_.snapshot.empty())
            return;
        oa_layer().read_whole_window(true);
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        write_ppm(step_snapshot(options_.snapshot, step), presented);
    };
    const auto top_is = [this](std::string_view name) {
        const LayerScreen* top = oa_layer().top();
        return top != nullptr && top->name() == name;
    };
    // A few frames on the main menu; a notice nobody asked for is closed.
    const auto settle = [&] {
        for (int frame = 0; frame < 12; ++frame) {
            run_frame(running);
            require(running, "a frame ended the run");
            const LayerScreen* top = oa_layer().top();
            if (top != nullptr && (dynamic_cast<const NoticeScreen*>(top) != nullptr ||
                                   dynamic_cast<const QuestionScreen*>(top) != nullptr))
                tap(SDLK_RETURN);
            else if (top == nullptr && frame >= 2)
                return;
        }
        require(oa_layer().top() == nullptr, "a screen stayed over the main menu");
    };
    // The button's square, and that square of the main menu's frame with the
    // button drawn over it without the dot.
    const auto button_frames = [&] {
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);
        run_frame(running);
        const renderer::Surface shown = frame_without_cursor();
        renderer::Surface plain = shown;
        const auto square = EngineSettingsMenuHost::button_rect(*this);
        // The icon's clear edge is not opaque. Drawing it again over the
        // button blends that edge twice, so the menu under the button goes
        // back first. The same badge the frame drew, without the dot: a
        // frame with no update matches, and a frame with updates differs
        // only there.
        const EngineSettingsMenuHost::ButtonPixels& kept =
            engine_settings_menu_host().library_pixels;
        require(
            kept.kept && kept.at.x == square.x && kept.at.y == square.y &&
                kept.at.width == square.width && kept.at.height == square.height &&
                kept.under.size() == static_cast<std::size_t>(square.width) *
                                         static_cast<std::size_t>(square.height) * 3U,
            "the menu under the OA button was not kept"
        );
        for (int32_t row = 0; row < square.height; ++row) {
            for (int32_t column = 0; column < square.width; ++column) {
                const std::size_t from =
                    (static_cast<std::size_t>(row) * static_cast<std::size_t>(square.width) +
                     static_cast<std::size_t>(column)) *
                    3U;
                const std::size_t to = (static_cast<std::size_t>(square.y + row) * plain.width +
                                        static_cast<std::size_t>(square.x + column)) *
                                       3U;
                plain.rgb[to] = kept.under[from];
                plain.rgb[to + 1] = kept.under[from + 1];
                plain.rgb[to + 2] = kept.under[from + 2];
            }
        }
        kit::OaButtonMark mark = EngineSettingsMenuHost::library_mark(*this);
        mark.dot = false;
        oa::ui::frontend_renderer::RgbaPicture picture;
        if (const WindowIcon* square_icon =
                engine_settings_menu_host().icon_for(true, square.width))
            picture = {square_icon->width, square_icon->height, square_icon->pixels};
        kit::draw_menu_icon_button(
            {&plain, {square.x, square.y, 1}, fonts, picture},
            square.width,
            kit::OaButtonState::idle,
            false,
            mark
        );
        return std::array<renderer::Surface, 2>{shown, plain};
    };
    const auto press_button = [&] {
        const auto square = EngineSettingsMenuHost::button_rect(*this);
        const layout::Point centre{square.x + square.width / 2, square.y + square.height / 2};
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, centre, 0);
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, centre, SDL_BUTTON_LEFT);
        require(oa_layer().find(library_screen_name) == nullptr, "a press opened the Library");
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_UP, centre, SDL_BUTTON_LEFT);
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);
    };
    const auto feed_changes = [&](bool updates) {
        fixture.inputs = fixture_inputs_of(mods, updates);
        ++fixture.generation;
        run_frame(running);
        require(running, "a frame ended the run");
    };
    // Shows an entry's details: its row selected, and at Compact its page.
    const auto show_entry = [&](std::string_view row) {
        if (named("oa.library.back"))
            click_named("oa.library.back");
        click_named(row);
    };
    // Presses one of the shown entry's actions: a button, or an item of
    // MORE ▾ at Compact.
    const auto press_action = [&](std::string_view word) {
        const std::string action = "oa.library.action." + std::string(word);
        if (!named(action))
            click_named("oa.library.action");
        click_named(action);
    };

    load(Screen::main_menu);
    require(screen_ == Screen::main_menu, "the main menu did not open");
    const std::array<std::array<int, 2>, 2> windows{{{kCanvasWidth, kCanvasHeight}, {1920, 1080}}};
    for (const auto& [width, height] : windows) {
        const std::string size = std::to_string(width) + 'x' + std::to_string(height);
        const std::string on = " on the " + size + " window";
        window_of(width, height);
        settle();
        feed_changes(false);
        require(
            library_registry_built_in(kBuiltIn) && !library_registry_built_in(kAdded),
            "library_registry_built_in does not answer as the feed's registries say" + on
        );

        // 1. With no update waiting, the button wears the package count and no dot.
        // Two installed mods, one map pack and two mutators: five packages.
        // A map pack alone counts as one.
        require(library_update_count() == 0, "an update waits in the fixture without one" + on);
        {
            const auto [shown, plain] = button_frames();
            const auto square = EngineSettingsMenuHost::button_rect(*this);
            std::size_t outside = 0;
            require(
                differing_in(
                    shown, plain, {square.x, square.y, square.width, square.height}, outside, {}
                ) == 0,
                "with no update the OA button is not the button drawn without a dot" + on
            );
            const std::optional<AutomationControl> quiet = named("oa.button");
            require(
                quiet.has_value() && quiet->text == "Library · 5 packages",
                "automation does not list oa.button as \"Library · 5 packages\"" + on
            );
        }
        if (width == kCanvasWidth) {
            library::Inputs maps_only = fixture.inputs;
            maps_only.listings.clear();
            maps_only.installed.clear();
            maps_only.updates.clear();
            maps_only.queue.clear();
            maps_only.playing_mod.reset();
            maps_only.language = {};
            maps_only.content = {};
            maps_only.map_packs = {
                {"Fixture Isles", {{"Isle Hop", 4, "16x16"}, {"Long Strait", 2, "20x12"}}},
            };
            fixture.inputs = std::move(maps_only);
            ++fixture.generation;
            run_frame(running);
            require(running, "a frame ended the run");
            const std::optional<AutomationControl> maps = named("oa.button");
            require(
                maps.has_value() && maps->text == "Library · 1 package",
                "a map pack alone does not count as 1 package" + on
            );
            feed_changes(false);
        }

        // 2. Releasing the press opens the Library on Active, whose lines
        // automation reads and which takes no press.
        press_button();
        require(
            top_is(library_screen_name) && host.model.tab == library::Tab::active &&
                engine_settings_dialog() == nullptr,
            "the OA button did not open the Library on Active" + on
        );
        run_frame(running);
        {
            const std::optional<AutomationControl> mod = named("oa.library.active.mod");
            const std::optional<AutomationControl> language = named("oa.library.active.language");
            // The mod sits on the MOD tile and the language on the LANGUAGE
            // tile. On a small window the tiles can leave the maps below the
            // page, so the press is taken while those tiles still show.
            require(
                mod.has_value() && mod->text == "Basalt 2.1" && language.has_value() &&
                    language->text == "Deutsch 1.2",
                "the Active tab does not read Basalt and Deutsch 1.2" + on
            );
            require(
                !named("oa.library.search").has_value() &&
                    !named("oa.library.filter.all").has_value() &&
                    !named("oa.library.filter").has_value(),
                "the Active tab shows a search or a filter" + on
            );
            const std::size_t asked = fixture.enqueued.size();
            click_named("oa.library.active.mod");
            require(
                top_is(library_screen_name) && host.model.tab == library::Tab::active &&
                    fixture.enqueued.size() == asked,
                "a press on the Active tab's line did something" + on
            );
            // Page Down until Fixture Isles' first map is in view. On a large
            // window it already is. The second map is read only when it is.
            for (int step = 0; step < 8 && !named("oa.library.active.map.1"); ++step) {
                tap(SDLK_PAGEDOWN);
                run_frame(running);
                require(running, "a frame ended the run");
            }
            const std::optional<AutomationControl> first_map = named("oa.library.active.map.1");
            const std::optional<AutomationControl> second_map = named("oa.library.active.map.2");
            require(
                first_map.has_value() && first_map->text == "Isle Hop · 4 players · 16x16" &&
                    (!second_map.has_value() ||
                     second_map->text == "Long Strait · 2 players · 20x12"),
                "the Active tab does not read Fixture Isles' maps" + on
            );
        }
        snapshot("active-" + size);

        // An expansion the fixture registry offers shows the download arrow.
        // The copy turns the pill on. Closing must not mount that choice:
        // the check has not installed the expansion's files.
        if (width == kCanvasWidth) {
            library::DownloadableExpansion units;
            units.id = "cavedog-units";
            units.registry = std::string(kBuiltIn);
            units.name = "Cavedog units";
            units.extends = {"totala"};
            fixture.inputs.downloadable_expansions.push_back(std::move(units));
            ++fixture.generation;
            run_frame(running);
            require(running, "a frame ended the run");
            const std::optional<AutomationControl> offered =
                named("oa.library.active.expansion.cavedog-units");
            const std::optional<AutomationControl> tactics =
                named("oa.library.active.expansion.battle-tactics");
            require(
                offered.has_value() && offered->text == "Cavedog units" && offered->enabled &&
                    !named("oa.library.active.expansion.cavedog-units.get").has_value(),
                "the offered expansion's chip is not the download press" + on
            );
            require(
                tactics.has_value() && tactics->text.find("ADD FROM") != std::string::npos,
                "an expansion no registry offers does not read ADD FROM" + on
            );
            const std::size_t asked = fixture.enqueued.size();
            click_named("oa.library.active.expansion.cavedog-units");
            require(
                fixture.enqueued.size() == asked + 1 &&
                    fixture.enqueued.back().registry == kBuiltIn &&
                    fixture.enqueued.back().key == "cavedog-units" &&
                    fixture.enqueued.back().reason == content::DownloadReason::install &&
                    !soft_restart_requested(),
                "the expansion's download arrow did not ask the feed to install it" + on
            );
            library::QueueItem fetching;
            fetching.registry = std::string(kBuiltIn);
            fetching.kind = library::Kind::mod;
            fetching.expansion = true;
            fetching.key = "cavedog-units";
            fetching.name = "Cavedog units";
            fetching.phase = library::QueueItem::Phase::downloading;
            fetching.done_bytes = 1024;
            fetching.total_bytes = 4096;
            fixture.inputs.queue.push_back(std::move(fetching));
            ++fixture.generation;
            run_frame(running);
            require(running, "a frame ended the run");
            const std::optional<AutomationControl> cancel =
                named("oa.library.active.expansion.cavedog-units");
            const std::optional<AutomationControl> queue = named("oa.library.queue");
            require(
                cancel.has_value() && cancel->enabled &&
                    library::active_content(host.model).hint.find("Downloading") !=
                        std::string::npos &&
                    queue.has_value() && queue->text.find("Cavedog units") != std::string::npos,
                "the expansion's download does not show its progress" + on
            );
            const std::size_t cancelled = fixture.cancelled.size();
            click_named("oa.library.active.expansion.cavedog-units");
            require(
                fixture.cancelled.size() == cancelled + 1 &&
                    fixture.cancelled.back().registry == kBuiltIn &&
                    fixture.cancelled.back().key == "cavedog-units",
                "CANCEL on the expansion's arrow did not reach the feed" + on
            );
            std::vector<library::QueueItem>& queued_items = fixture.inputs.queue;
            queued_items.erase(
                std::remove_if(
                    queued_items.begin(),
                    queued_items.end(),
                    [](const library::QueueItem& queued_item) {
                        return queued_item.expansion && queued_item.key == "cavedog-units";
                    }
                ),
                queued_items.end()
            );
            ++fixture.generation;
            run_frame(running);
            require(running, "a frame ended the run");
            require(
                named("oa.library.active.expansion.cavedog-units").has_value() &&
                    !named("oa.library.active.expansion.cavedog-units.cancel").has_value(),
                "cancelling the expansion's download did not bring the chip back" + on
            );
            click_named("oa.library.active.expansion.cavedog-units");
            library::QueueItem installed;
            installed.registry = std::string(kBuiltIn);
            installed.kind = library::Kind::mod;
            installed.expansion = true;
            installed.key = "cavedog-units";
            installed.name = "Cavedog units";
            installed.phase = library::QueueItem::Phase::installed;
            fixture.inputs.queue.push_back(std::move(installed));
            fixture.inputs.content.copied = {"campaign", "core-contingency", "cavedog-units"};
            ++fixture.generation;
            run_frame(running);
            require(running, "a frame ended the run");
            const std::optional<AutomationControl> pill =
                named("oa.library.active.expansion.cavedog-units");
            require(
                pill.has_value() && pill->checked &&
                    pill->text.find("ADD FROM") == std::string::npos && host.model.content.edited &&
                    !host.model.content.applied && !soft_restart_requested(),
                "the expansion's pill did not turn on without mounting" + on
            );
            host.model.content.edited = false;
            host.model.content.downloads.clear();
        }

        // 3. CLOSE goes back to the main menu.
        click_named("oa.library.close");
        require(
            oa_layer().top() == nullptr && screen_ == Screen::main_menu,
            "CLOSE did not go back to the main menu" + on
        );

        // 4. With four updates the button wears its dot, says so, and opens
        // the Library on Active; its Updates tab opens on Updates, with
        // UPDATE ALL over the list and the filters of the other tabs.
        feed_changes(true);
        require(library_update_count() == 4, "the four updates are not counted" + on);
        {
            const auto [shown, plain] = button_frames();
            const auto square = EngineSettingsMenuHost::button_rect(*this);
            const kit::Rect dot = kit::oa_button_dot(square.width);
            std::size_t outside = 0;
            const std::size_t differing = differing_in(
                shown,
                plain,
                {square.x, square.y, square.width, square.height},
                outside,
                {square.x + dot.x, square.y + dot.y, dot.width, dot.height}
            );
            require(
                differing > 0 && outside == 0,
                "with four updates the OA button does not differ only in its dot" + on
            );
        }
        snapshot("menu-dot-" + size);
        {
            const library::Tab shown_tab = host.model.tab;
            host.model.tab = library::Tab::mods;
            require(
                library::filter_count(host.model, library::Filter::all) == 4,
                "the Mods tab's All is not the fixture's four mods" + on
            );
            host.model.tab = shown_tab;
        }
        const std::optional<AutomationControl> button = named("oa.button");
        require(
            button.has_value() && button->text == "Library · 5 packages · 4 updates",
            "automation does not list oa.button as \"Library · 5 packages · 4 updates\"" + on
        );
        press_button();
        require(
            top_is(library_screen_name) && host.model.tab == library::Tab::active,
            "the OA button did not open the Library on Active" + on
        );
        require(
            named("oa.library.tab.updates").has_value(),
            "automation does not list oa.library.tab.updates" + on
        );
        const auto show_tab = [&](std::string_view name) {
            for (int step = 0; step < 8 && !named(name); ++step) {
                if (!named("oa.library.tab.scroll-right"))
                    break;
                click_named("oa.library.tab.scroll-right");
                run_frame(running);
                require(running, "a frame ended the run");
            }
        };
        show_tab("oa.library.tab.mutators");
        click_named("oa.library.tab.mutators");
        run_frame(running);
        require(running, "a frame ended the run");
        {
            const std::optional<AutomationControl> mutators = named("oa.library.tab.mutators");
            require(
                host.model.tab == library::Tab::mutators && mutators.has_value() &&
                    mutators->text == "Mutators (5)",
                "the Mutators tab does not read Mutators (5)" + on
            );
            for (int step = 0; step < 8 && !named("oa.library.row.fixture-core.ridge-walls");
                 ++step) {
                tap(SDLK_PAGEDOWN);
                run_frame(running);
                require(running, "a frame ended the run");
            }
            const std::optional<AutomationControl> walls =
                named("oa.library.row.fixture-core.ridge-walls");
            const std::optional<AutomationControl> walls_get =
                named("oa.library.row.fixture-core.ridge-walls.get");
            require(
                walls.has_value() && walls->enabled && walls_get.has_value() && walls_get->enabled,
                "Ridge Walls is not listed with GET on" + on
            );
            std::cout << "library check: the Mutators tab reads Mutators (5)" << on << '\n';
        }
        show_tab("oa.library.tab.brains");
        click_named("oa.library.tab.brains");
        run_frame(running);
        require(running, "a frame ended the run");
        {
            const std::optional<AutomationControl> brains = named("oa.library.tab.brains");
            const library::Entry* first = host.model.visible.empty()
                                              ? nullptr
                                              : &host.model.entries[host.model.visible.front()];
            require(
                host.model.tab == library::Tab::brains && brains.has_value() &&
                    brains->text == "Brains (4)" && first != nullptr &&
                    library::is_built_in_brain(first->id),
                "the Brains tab does not read Brains (4) with the built-in row first" + on
            );
            const std::optional<AutomationControl> built_in_row = named("oa.library.row.built-in");
            require(
                built_in_row.has_value() &&
                    built_in_row->text.find("Built-in") != std::string::npos,
                "the built-in row is not first on the Brains tab" + on
            );
            for (int step = 0; step < 8 && !named("oa.library.row.fixture-core.ridge-raider");
                 ++step) {
                tap(SDLK_PAGEDOWN);
                run_frame(running);
                require(running, "a frame ended the run");
            }
            const std::optional<AutomationControl> raider =
                named("oa.library.row.fixture-core.ridge-raider");
            const std::optional<AutomationControl> raider_get =
                named("oa.library.row.fixture-core.ridge-raider.get");
            const library::Entry* raider_entry = library::find_entry(
                host.model, {std::string(kBuiltIn), library::Kind::brain, "ridge-raider"}
            );
            const library::BrainFacts* raider_facts =
                raider_entry != nullptr ? library::brain_facts(*raider_entry) : nullptr;
            const std::string reason =
                raider_facts != nullptr
                    ? library::brain_unfit_reason(host.model.inputs, *raider_facts, host.model.text)
                    : std::string{};
            require(
                raider.has_value() && raider->enabled && raider_get.has_value() &&
                    raider_get->enabled && reason == "Made for Ridge",
                "Ridge Raider is not listed with Made for Ridge and GET on" + on
            );
            std::cout << "library check: the Brains tab reads Brains (4) and the built-in row first"
                      << on << '\n';
        }
        show_tab("oa.library.tab.updates");
        click_named("oa.library.tab.updates");
        require(
            host.model.tab == library::Tab::updates &&
                library::shown_filter(host.model) == library::Filter::updates &&
                host.model.visible.size() == 4,
            "the Updates tab did not open on its four updates" + on
        );
        run_frame(running);
        require(
            named("oa.library.action.update-all").has_value() &&
                (named("oa.library.filter").has_value() ||
                 named("oa.library.filter.updates").has_value()),
            "the Updates tab shows no UPDATE ALL or no filters" + on
        );
        snapshot("updates-" + size);

        // 5. GET and CANCEL reach the feed.
        click_named("oa.library.tab.mods");
        show_entry("oa.library.row.fixture-core.cinder");
        const std::size_t enqueued = fixture.enqueued.size();
        press_action("get");
        require(
            fixture.enqueued.size() == enqueued + 1 &&
                fixture.enqueued.back().registry == kBuiltIn &&
                fixture.enqueued.back().key == "cinder" &&
                fixture.enqueued.back().reason == content::DownloadReason::install,
            "GET did not reach the feed for fixture-core's cinder" + on
        );
        show_entry("oa.library.row.fixture-extra.dunes");
        const std::size_t cancelled = fixture.cancelled.size();
        press_action("cancel");
        require(
            fixture.cancelled.size() == cancelled + 1 &&
                fixture.cancelled.back().registry == kAdded &&
                fixture.cancelled.back().key == "dunes",
            "CANCEL did not reach the feed for fixture-extra's dunes" + on
        );

        // 6. OPEN FOLDER and HOMEPAGE through the recorded openers, ROLL BACK's
        // question over the Library, and PLAY NOW's soft restart.
        const fs::path ridge = mods / std::string(kFixtureMods[0].folder);
        show_entry("oa.library.row.fixture-core.ridge");
        const std::size_t opened = user_folder_state().opened.size();
        press_action("open-folder");
        require(
            user_folder_state().opened.size() == opened + 1 &&
                kept_path(user_folder_state().opened.back()) == kept_path(ridge),
            "OPEN FOLDER did not show Ridge's folder through the recorded opener" + on
        );
        require(web_links_ != nullptr, "the run keeps no record of web links");
        const std::size_t links = web_links_->requests.size();
        press_action("homepage");
        require(
            web_links_->requests.size() == links + 1 &&
                web_links_->requests.back() == kRidgeHomepage,
            "HOMEPAGE did not record Ridge's homepage" + on
        );
        press_action("roll-back");
        {
            const auto* asked = dynamic_cast<const QuestionScreen*>(oa_layer().top());
            require(
                asked != nullptr && oa_layer().find(library_screen_name) != nullptr &&
                    engine_settings_dialog() == nullptr,
                "ROLL BACK did not ask its question over the Library" + on
            );
            const settings::Prompt& question = asked->question();
            require(
                question.title == "ROLL BACK MOD" && question.buttons.size() == 2 &&
                    question.buttons[0].id == "cancel" && question.buttons[1].id == "roll-back" &&
                    question.marked == 0 && question.paragraphs.size() == 1 &&
                    question.paragraphs[0].text.starts_with("Roll back Ridge to 4.8 revision 2?"),
                "ROLL BACK did not ask the Roll Back Mod question for Ridge" + on
            );
        }
        tap(SDLK_ESCAPE);
        require(
            top_is(library_screen_name) && engine_settings_dialog() == nullptr &&
                package_install::oamod::read_installed_mod(ridge).revision == 3,
            "CANCEL on the Roll Back question did not go back to the Library unchanged" + on
        );
        const settings::EngineSettings before = engine_settings_state().current;
        press_action("play-now");
        require(soft_restart_requested(), "PLAY NOW on Ridge did not ask for a soft restart" + on);
        {
            const std::shared_ptr<const LibraryReturn> kept = library_return();
            require(
                kept != nullptr && kept->tab == host.model.tab &&
                    kept->selected == host.model.selected &&
                    kept->selected_row == host.selected_row() &&
                    kept->list_scroll == host.state.list_scroll,
                "PLAY NOW did not keep the Library as it showed for the next run" + on
            );
            // The check goes on: the run is not ended, the Library is not
            // kept for a next run, and the Mod setting goes back.
            soft_restart_requested_ = false;
            exit_requested_ = false;
            library_kept_.reset();
            restart_line_.clear();
            const settings::EngineSettings played = engine_settings_state().current;
            require(
                played.mod_folder == kept_path(ridge), "PLAY NOW did not choose Ridge's folder" + on
            );
            apply_engine_settings(before);
            require(
                !save_engine_settings(played, before, false).has_value(),
                "the Mod setting could not be put back" + on
            );
        }

        // 7. The settings shortcut opens Settings over the Library; the same
        // shortcut on the main menu opens Settings, not the Library. The
        // footer has no Settings button. Compact's details page goes back
        // to the list first.
        if (named("oa.library.back"))
            click_named("oa.library.back");
        require(named("oa.library.refresh").has_value(), "the header has no refresh icon" + on);
        require(!named("oa.library.settings").has_value(), "the footer still offers Settings" + on);
        tap(SDLK_COMMA, kShortcutModifier);
        require(
            engine_settings_dialog() != nullptr && top_is("settings") &&
                oa_layer().find(library_screen_name) != nullptr,
            "the shortcut did not open Settings over the Library" + on
        );
        tap(SDLK_ESCAPE);
        require(top_is(library_screen_name), "Settings did not close back to the Library" + on);
        click_named("oa.library.close");
        require(oa_layer().top() == nullptr, "CLOSE did not go back to the main menu" + on);
        tap(SDLK_COMMA, kShortcutModifier);
        require(
            engine_settings_dialog() != nullptr && top_is("settings") &&
                oa_layer().find(library_screen_name) == nullptr,
            "the shortcut did not open Settings alone" + on
        );

        // 8. Settings › Language's MORE IN THE LIBRARY… opens the Library on
        // Languages over Settings, and closing it goes back to Settings.
        click_named("oa.settings.nav.language");
        snapshot("settings-language-" + size);
        click_named("oa.settings.more-in-library");
        require(
            top_is(library_screen_name) && oa_layer().find("settings") != nullptr &&
                engine_settings_dialog() != nullptr && host.model.tab == library::Tab::languages,
            "MORE IN THE LIBRARY… did not open the Library on Languages over Settings" + on
        );
        click_named("oa.library.close");
        require(
            top_is("settings") && engine_settings_dialog() != nullptr,
            "closing the Library did not go back to Settings" + on
        );
        tap(SDLK_ESCAPE);
        require(oa_layer().top() == nullptr, "Escape did not close Settings" + on);
        // From the Library, Settings, then MORE IN THE LIBRARY… goes back to
        // the Library: never two of either.
        press_button();
        tap(SDLK_COMMA, kShortcutModifier);
        click_named("oa.settings.nav.language");
        click_named("oa.settings.more-in-library");
        require(
            top_is(library_screen_name) && oa_layer().find("settings") == nullptr &&
                engine_settings_dialog() == nullptr && host.model.tab == library::Tab::languages,
            "MORE IN THE LIBRARY… over the Library stacked a second Library" + on
        );
        click_named("oa.library.close");
        require(
            oa_layer().find(library_screen_name) == nullptr && oa_layer().top() == nullptr,
            "a second Library was left on the layer" + on
        );
        std::cout << "library check: the OA button, its dot, the Library's actions and Settings"
                  << on << '\n';
    }

    // 9. In a skirmish the in-match OA button opens Settings, the Library
    // opens nowhere, and Language does not link to it.
    start_benchmark_skirmish();
    require(screen_ == Screen::match && match_ != nullptr, "the skirmish did not start");
    open_library(std::nullopt);
    require(oa_layer().find(library_screen_name) == nullptr, "the Library opened in a match");
    show_match_pause_menu();
    require(ingame_menu_column_shown(), "the in-game menu's column does not show");
    render();
    const layout::Rect in_match = OaLayer::match_button_rect(match_layout_);
    const layout::Point centre{in_match.x + in_match.width / 2, in_match.y + in_match.height / 2};
    send_check_pointer(SDL_EVENT_MOUSE_MOTION, centre, 0);
    send_check_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, centre, SDL_BUTTON_LEFT);
    send_check_pointer(SDL_EVENT_MOUSE_BUTTON_UP, centre, SDL_BUTTON_LEFT);
    require(
        engine_settings_dialog() != nullptr && oa_layer().find("settings") != nullptr &&
            oa_layer().find(library_screen_name) == nullptr,
        "the in-match OA button did not open Settings"
    );
    require(!engine_settings_dialog()->library_link, "Language links to the Library in a match");
    std::ignore = take_engine_settings_action(settings::DialogAction::cancelled);
    leave_match();
    load(Screen::main_menu);
    std::cout << "library check: the in-match OA button opens Settings\n";

    // 10. At 640×480 a stand-in controller reaches the Library's own commands
    // through the same events a gamepad sends. The same buttons over Settings
    // do what they did before this step.
    window_of(kCanvasWidth, kCanvasHeight);
    settle();
    fixture.inputs = fixture_inputs_of(mods, true);
    append_spur_updates(fixture.inputs, kSpurUpdates);
    ++fixture.generation;
    run_frame(running);
    require(running, "a frame ended the run");
    open_library(library::Tab::updates);
    run_frame(running);
    require(
        running && top_is(library_screen_name) && host.model.tab == library::Tab::updates &&
            host.model.visible.size() > static_cast<std::size_t>(kSpurUpdates),
        "the Library did not open on a long Updates list"
    );

    struct VirtualOnly {
        PadState& state;
        bool before;

        explicit VirtualOnly(PadState& state) : state(state), before(state.virtual_pads_only) {
            state.virtual_pads_only = true;
        }

        ~VirtualOnly() { state.virtual_pads_only = before; }
    } virtual_only{pad_state()};

    LibraryPad stand_in;
    attach_library_pad(stand_in);
    for (int frame = 0; frame < 4; ++frame)
        run_frame(running);
    require(running, "opening the stand-in gamepad ended the run");

    const auto tap_pad = [&](SDL_GamepadButton button) {
        set_library_button(stand_in, button, true);
        run_frame(running);
        set_library_button(stand_in, button, false);
        run_frame(running);
        require(running, "a controller button ended the run");
    };
    const auto stick_down = [&] {
        set_library_axis(stand_in, SDL_GAMEPAD_AXIS_RIGHTY, 1.0F);
        run_frame(running);
        set_library_axis(stand_in, SDL_GAMEPAD_AXIS_RIGHTY, 0.0F);
        run_frame(running);
        require(running, "the right stick ended the run");
    };
    const auto shown_row = [&]() -> std::optional<std::string> {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls) {
            // A row is a check box; its GET or UPDATE icon, listed under the
            // row's name, is a button.
            if (control.visible && control.window_width > 0 && control.window_height > 0 &&
                control.kind == AutomationControlKind::check_box &&
                control.name.compare(0, std::string("oa.library.row.").size(), "oa.library.row.") ==
                    0)
                return control.name;
        }
        return std::nullopt;
    };

    const std::optional<AutomationControl> search_before = named("oa.library.search");
    require(
        search_before.has_value() && !search_before->focused,
        "the search was missing or already focused"
    );
    tap_pad(SDL_GAMEPAD_BUTTON_NORTH);
    const std::optional<AutomationControl> search = named("oa.library.search");
    require(search.has_value() && search->focused, "Y did not focus the search");
    std::cout << "library check: pad Y focuses the search\n";

    require(host.model.tab == library::Tab::updates, "Y left the Updates tab");
    tap_pad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    require(host.model.tab != library::Tab::updates, "RB did not change the tab");
    tap_pad(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    require(host.model.tab == library::Tab::updates, "LB did not come back to Updates");
    std::cout << "library check: pad RB then LB change the tab and come back\n";

    const library::Entry* selected = library::selected_entry(host.model);
    require(
        selected != nullptr && selected->state == library::State::update,
        "the selected entry has no UPDATE"
    );
    const std::string selected_key = selected->id.key;
    const std::string selected_registry = selected->id.registry;
    const std::size_t enqueued = fixture.enqueued.size();
    const std::size_t cancelled = fixture.cancelled.size();
    const std::size_t retried = fixture.retried.size();
    tap_pad(SDL_GAMEPAD_BUTTON_WEST);
    require(
        fixture.enqueued.size() == enqueued + 1 && fixture.cancelled.size() == cancelled &&
            fixture.retried.size() == retried &&
            fixture.enqueued.back().registry == selected_registry &&
            fixture.enqueued.back().key == selected_key &&
            fixture.enqueued.back().reason == content::DownloadReason::update,
        "X did not queue the selected entry's UPDATE"
    );
    std::cout << "library check: pad X queues the selected UPDATE\n";

    const int32_t scrolled_from = host.state.list_scroll;
    stick_down();
    require(
        host.state.list_scroll > scrolled_from && top_is(library_screen_name),
        "the right stick did not scroll the list"
    );
    std::cout << "library check: pad right stick scrolls the list\n";

    const std::optional<std::string> row = shown_row();
    require(row.has_value(), "no row of the list is shown to open");
    click_named(*row);
    require(named("oa.library.back").has_value(), "the row did not open Compact's details");
    tap_pad(SDL_GAMEPAD_BUTTON_EAST);
    require(
        !named("oa.library.back").has_value() && top_is(library_screen_name),
        "B did not go back from the details to the list"
    );
    tap_pad(SDL_GAMEPAD_BUTTON_EAST);
    require(
        oa_layer().top() == nullptr && oa_layer().find(library_screen_name) == nullptr,
        "B did not close the Library"
    );
    std::cout << "library check: pad B leaves the details and then closes the Library\n";

    open_library(library::Tab::updates);
    run_frame(running);
    require(top_is(library_screen_name), "the Library did not open under Settings");
    if (named("oa.library.back"))
        click_named("oa.library.back");
    tap(SDLK_COMMA, kShortcutModifier);
    require(
        engine_settings_dialog() != nullptr && top_is("settings") &&
            oa_layer().find(library_screen_name) != nullptr,
        "the shortcut did not open Settings over the Library for the controller"
    );
    settings::Dialog* dialog = engine_settings_dialog();
    const int32_t focus_before = dialog->focused;
    const library::Tab tab_before = host.model.tab;
    const int32_t scroll_before = host.state.list_scroll;
    const std::size_t queued_before = fixture.enqueued.size();
    tap_pad(SDL_GAMEPAD_BUTTON_NORTH);
    tap_pad(SDL_GAMEPAD_BUTTON_WEST);
    require(
        engine_settings_dialog() != nullptr && top_is("settings") &&
            dialog->focused == focus_before && host.model.tab == tab_before &&
            host.state.list_scroll == scroll_before && fixture.enqueued.size() == queued_before,
        "Y or X changed something while Settings was open"
    );
    std::cout << "library check: pad Y and X do nothing over Settings\n";

    tap_pad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    require(
        engine_settings_dialog() != nullptr && dialog->focused != focus_before,
        "RB did not move the focus in Settings"
    );
    const int32_t focus_after_rb = dialog->focused;
    tap_pad(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    require(
        engine_settings_dialog() != nullptr && dialog->focused != focus_after_rb,
        "LB did not move the focus in Settings"
    );
    std::cout << "library check: pad RB and LB move the focus in Settings\n";

    stick_down();
    require(
        top_is("settings") && engine_settings_dialog() != nullptr &&
            host.state.list_scroll == scroll_before && host.model.tab == tab_before &&
            fixture.enqueued.size() == queued_before,
        "the right stick changed the Library while Settings was open"
    );
    std::cout
        << "library check: pad right stick leaves the Library's list while Settings is open\n";

    tap_pad(SDL_GAMEPAD_BUTTON_EAST);
    require(
        engine_settings_dialog() == nullptr && top_is(library_screen_name),
        "B did not close Settings back to the Library"
    );
    std::cout << "library check: pad B closes Settings back to the Library\n";

    stand_in.detach();
    run_frame(running);
    require(running, "detaching the stand-in gamepad ended the run");
}

} // namespace oa::app
