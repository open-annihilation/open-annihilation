// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What each kind of package asks of the running game. A kind is named in
// one branch of each function: a mod package (oamod), a map pack (oamap),
// a language pack (oalang), an expansion, a mutator (oamut), a brain
// (oabrain) and a key layout (oakeys). An
// expansion is copied by install_oaexp; these hooks are not its install.

#include "map_packs.hpp"
#include "package_paths.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/package_install.hpp"
#include "oa/app/package_install/oamap.hpp"
#include "oa/app/runtime.hpp"

#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

/// Where the fit check of a map pack's install finds the game's content.
struct FitContent {
    fs::path game_dir{}; ///< the base game's folder
    /// The folders the game plays, highest precedence first; empty for the
    /// game folder alone.
    std::vector<fs::path> played{};
    fs::path mod_folder{};  ///< the mod folder layered over the game; empty when none
    fs::path user_folder{}; ///< the player's folder, which holds the copied expansions
};

} // namespace

struct Runtime::MapPackState {
    MapPacks packs;
    FitContent content{};
    package_install::oamap::FitHooks hooks{};

    /// Remembers the Maps folder and the game the fit check reads.
    ///
    /// @param maps_folder the Maps folder
    /// @param game the base game's folder
    MapPackState(fs::path maps_folder, fs::path game) : packs(std::move(maps_folder)) {
        content.game_dir = std::move(game);
    }
};

namespace {

/// Checks one staged map against the base game and the expansions the game
/// finds features in: those that are on and those the pack needs.
///
/// @param context the game's content (FitContent)
/// @param staged the unpacked folder
/// @param map the map
/// @param id the pack's id
/// @param required the expansions the pack needs
/// @return every rule the map broke
oa::data::map_fit::Fit check_runtime_base_fit(
    void* context,
    const fs::path& staged,
    const oa::data::map_pack::MapEntry& map,
    std::string_view id,
    std::span<const std::string> required
) {
    const auto* where = static_cast<const FitContent*>(context);
    if (where == nullptr)
        return {};
    const std::vector<fs::path> folders = map_pack_fit_folders(
        where->played, where->mod_folder, where->game_dir, where->user_folder, required
    );
    return check_content_fit(folders, staged, map, id);
}

} // namespace

void Runtime::destroy_map_pack_state(MapPackState* state) noexcept {
    delete state;
}

MapPacks& Runtime::map_packs() const {
    if (!map_pack_state_) {
        map_pack_state_.reset(new MapPackState(
            user_folder_ / std::string(package_install::oamap::maps_folder_name), options_.game_dir
        ));
    }
    return map_pack_state_->packs;
}

bool Runtime::package_target_in_use(
    const package_install::PackageKind& kind, const fs::path& folder
) const {
    if (kind.name == "oamod")
        return !options_.layered_mod_folder.empty() &&
               same_folder(kept_path(folder), kept_path(options_.layered_mod_folder));
    if (kind.name == "oamap")
        return false;
    // A pack is read whole when packs are read. Nothing holds its folder
    // open, and nothing waits for the run to end.
    if (kind.name == "oalang")
        return false;
    if (kind.name == "expansion")
        return false;
    // A mutator's files are mounted only while a match runs, when nothing
    // installs. Nothing holds its folder open.
    if (kind.name == "oamut")
        return false;
    // Nothing installs while a match runs, and only a match reads a brain's
    // files. Nothing holds its folder open.
    if (kind.name == "oabrain")
        return false;
    // A layout is read whole when it is chosen. Nothing holds its folder open.
    if (kind.name == "oakeys")
        return false;
    return false;
}

bool Runtime::package_offers_play(const package_install::PackageKind& kind) const {
    if (kind.name == "oamod")
        return options_.mod_dir.empty() && !options_.base_game;
    if (kind.name == "oamap")
        return false;
    if (kind.name == "oalang")
        return false;
    if (kind.name == "expansion")
        return false;
    // Installing a mutator does not turn it on, and PLAY NOW is not offered.
    if (kind.name == "oamut")
        return false;
    // Installing a brain does not seat it, and PLAY NOW is not offered.
    if (kind.name == "oabrain")
        return false;
    // Installing a key layout does not make it the one in use, and PLAY NOW
    // is not offered.
    if (kind.name == "oakeys")
        return false;
    return false;
}

void Runtime::package_changed(const package_install::PackageKind& kind, const fs::path& folder) {
    if (kind.name == "oamod")
        list_offered_mods();
    if (kind.name == "oamap") {
        map_packs().refresh();
        refresh_listed_pack_maps();
    }
    // A pack is read whole. Nothing holds its folder, so the lists, the
    // catalogue and the font faces are replaced in place.
    if (kind.name == "oalang")
        reload_language_packs();
    // An expansion's files are read with the content copy, not as a package
    // the Library lists. The copied set can have changed with this folder.
    if (kind.name == "expansion") {
        note_content_bundles_changed();
        return;
    }
    // The listing is read again. A change before the first read leaves it
    // unmade: nothing has asked for it yet.
    if (kind.name == "oamut")
        reload_installed_mutators();
    // The listing is read again. A change before the first read leaves it
    // unmade: nothing has asked for it yet.
    if (kind.name == "oabrain")
        reload_installed_brains();
    // The runtime keeps no separate listing of layouts. When this folder is
    // the layout in use, it is read again so an update plays at once.
    if (kind.name == "oakeys")
        reload_key_layout_if_in_use(folder);
}

package_install::PackageOptions
Runtime::package_options(const package_install::PackageKind& kind) const {
    package_install::PackageOptions options{};
    if (kind.name == "oamod") {
        options.accept_unimplemented_hacks = options_.accept_unimplemented_hacks;
        options.preferences = &preference_values_;
        options.game_folder =
            options_.game_folders.empty() ? options_.game_dir : options_.game_folders.back();
    }
    if (kind.name == "oamap") {
        std::ignore = map_packs();
        FitContent& where = map_pack_state_->content;
        where.game_dir = options_.game_dir;
        where.played = options_.game_folders;
        where.mod_folder = options_.layered_mod_folder;
        where.user_folder = user_folder_;
        map_pack_state_->hooks.context = &where;
        map_pack_state_->hooks.check_base_fit = &check_runtime_base_fit;
        options.context = &map_pack_state_->hooks;
    }
    // A language pack's hooks read the package themselves and need no context.
    if (kind.name == "oalang")
        return options;
    // An expansion's manifest reader uses the built-in table and no context.
    if (kind.name == "expansion")
        return options;
    // A mutator's hooks read the package themselves and need no context.
    if (kind.name == "oamut")
        return options;
    // A brain's hooks read the package themselves and need no context.
    if (kind.name == "oabrain")
        return options;
    // A key layout's hooks read the package themselves and need no context.
    // Installing never writes the layout in use.
    if (kind.name == "oakeys")
        return options;
    return options;
}

} // namespace oa::app
