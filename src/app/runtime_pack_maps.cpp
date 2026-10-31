// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Pack maps in the skirmish picker and the match: one map's files mounted at
// a time, checked against the game and the mod played when they are mounted,
// and unmounted when another map is chosen, when the match ends and whenever
// the main menu shows.

#include "map_packs.hpp"

#include "oa/app/content/downloads.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/package_install/origin.hpp"
#include "oa/app/runtime.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/map_fit/map_fit.hpp"
#include "oa/data/map_pack/manifest.hpp"
#include "oa/data/map_pack/map_name.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/ota.hpp"
#include "oa/ui/frontend/savegame_dialogs.hpp"
#include "oa/ui/frontend_state/game_entry.hpp"
#include "oa/ui/frontend_multiplayer/map_list.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

namespace fit = oa::data::map_fit;
namespace pack = oa::data::map_pack;

// A saved game keeps the name of the map it was played on in its summary,
// with the terminating NUL. A pack map's name always fits there whole, so a
// saved game on a pack map loads that map by its name.
// clang-format off
static_assert(oa::data::map_pack::most_map_name_bytes < std::tuple_size_v<decltype(oa::ui::frontend::LoadSummary::mission)>, "a pack map's name cut short in a saved game names no map");
// clang-format on

struct Runtime::PackMapState {
    /// The game's and the mod's feature, weapon and unit names, collected at
    /// the first check: the run's archives never change.
    std::optional<fit::GameNames> game_names{};
    /// Each pack map's fit, by the pack's SHA-256, the map's stem and the
    /// compatibility key of the game that was checked.
    std::map<std::tuple<std::string, std::string, std::string>, fit::Fit> fits{};
    /// Why each refused map was refused: a ready sentence, or a fit detail.
    std::map<std::string, std::string, std::less<>> refusals{};

    /// Why each listed map is greyed. The strings stay until the next refresh.
    struct Block {
        std::string row{};
        std::string refusal{};
        bool sentence = false;
    };

    std::map<std::string, Block, std::less<>> blocks{};
    /// The name of the map whose files are mounted; empty when none is.
    std::string mounted_name{};
    /// The pack id of that map.
    std::string mounted_id{};
    /// The pack whose maps are not mounted while GET MAP installs it; empty
    /// when none is held (hold_pack_maps).
    std::string held_id{};
    /// The queue item that downloads and installs the held pack.
    uint64_t held_item{};
    /// Every installed pack map, in the index's order.
    std::vector<std::string> listed{};
    /// Packs already named in the log for a mod, as the pack id and the key.
    std::vector<std::pair<std::string, std::string>> omission_logged{};
    /// Those log lines, for a check to read.
    std::vector<std::string> omission_lines{};
    /// The catalogue generation the lists were last built from.
    uint64_t catalogue_generation{};
    /// True once the lists have been built from a catalogue snapshot.
    bool catalogue_noted{};
};

namespace {

/// The interface's words for a refused map; {game} and {reason} are filled.
constexpr std::string_view kRefusedText = "Doesn't fit {game}: {reason}";

/// The interface's name for the base game, when no mod is played.
constexpr std::string_view kBaseGameText = "No Mod";

/// The words a log line uses when the mod being played has no catalogue key.
constexpr std::string_view kUnkeyedMod = "the mod being played";

/// Why a held pack's map is not mounted: its download is being installed.
constexpr std::string_view kHeldReason = "its pack is being downloaded";

/// The number of hex digits of a pack's SHA-256 in its layer's label.
constexpr std::size_t kLabelDigestDigits = 8;

/// Returns the manifest entry of an installed pack map.
///
/// @param map the map, as the installed packs list it
/// @return the entry its fit check and its layer read
pack::MapEntry entry_of(const PackMap& map) {
    pack::MapEntry entry{};
    entry.stem = map.stem;
    entry.title = map.title;
    entry.description = map.description;
    entry.size = map.size;
    entry.players = map.players;
    entry.files = map.files;
    return entry;
}

/// A fit with one failure: the map could not be checked or mounted.
///
/// @param subject the map's name
/// @param detail what went wrong
/// @return the fit
fit::Fit unreadable_fit(const std::string& subject, std::string detail) {
    fit::Fit unfit;
    unfit.failures.push_back({fit::Rule::complete, subject, std::move(detail)});
    return unfit;
}

/// Tells whether two names are the same, ignoring ASCII case.
///
/// @param left one name
/// @param right the other
/// @return true when they differ only in ASCII case
bool same_without_case(std::string_view left, std::string_view right) {
    return std::equal(left.begin(), left.end(), right.begin(), right.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
}

/// Tells whether a file lies inside a folder, comparing their parts.
///
/// @param file the file
/// @param folder the folder, absolute and normal
/// @return true when `file` is below `folder`
bool inside_folder(const std::filesystem::path& file, const std::filesystem::path& folder) {
    std::error_code error;
    const std::filesystem::path whole = std::filesystem::absolute(file, error).lexically_normal();
    if (error || folder.empty())
        return false;
    const std::filesystem::path relative = whole.lexically_relative(folder);
    return !relative.empty() && *relative.begin() != ".." && *relative.begin() != ".";
}

/// Returns the path the store shows a pack map's OTA at.
///
/// @param map the map
/// @return `maps/<stem>@<id>.ota`
std::string shown_ota_path(const PackMap& map) {
    return "maps/" + pack::pack_map_name(map.stem, map.id) + ".ota";
}

/// A pack map's files, with the played mod's settings for the map read in
/// place of the pack's own OTA.
class SettledMapFiles final : public fit::MapFiles {
  public:

    /// Lays the mod's settings over a pack map's files.
    ///
    /// @param files the pack map's files
    /// @param ota_path the path the map's OTA is shown at
    /// @param settings the mod's OTA
    SettledMapFiles(
        std::unique_ptr<fit::MapFiles> files,
        std::string ota_path,
        std::shared_ptr<const std::vector<uint8_t>> settings
    )
        : files_(std::move(files)), ota_path_(std::move(ota_path)), settings_(std::move(settings)) {
    }

    /// Returns the pack map's paths.
    ///
    /// @return the paths, in the map's order
    [[nodiscard]] std::vector<std::string> paths() const override { return files_->paths(); }

    /// Reads one of the map's files: the mod's settings for its OTA.
    ///
    /// @param path a resource path, matched ignoring ASCII case
    /// @return the bytes, or nothing when the map has no such file
    [[nodiscard]] std::optional<std::vector<uint8_t>> read(std::string_view path) const override {
        if (same_without_case(path, ota_path_))
            return *settings_;
        return files_->read(path);
    }

    /// Tells whether the files are the store's mounted layer.
    ///
    /// @return what the pack map's files say
    [[nodiscard]] bool from_layer() const noexcept override { return files_->from_layer(); }

  private:

    std::unique_ptr<fit::MapFiles> files_;
    std::string ota_path_;
    std::shared_ptr<const std::vector<uint8_t>> settings_;
};

/// Returns a fit's key part for the mod's settings a check read.
///
/// @param settings the mod's OTA; null when the pack's own was read
/// @return empty for none; otherwise a line naming the settings' SHA-256
std::string settings_stamp(const std::shared_ptr<const std::vector<uint8_t>>& settings) {
    if (!settings)
        return {};
    const auto hex = oa::base::sha256::to_hex(oa::base::sha256::digest_of(*settings));
    return "\nsettings " + std::string(hex.data(), hex.size());
}

/// Fills each "{name}" of a text with its value.
///
/// @param text the text
/// @param fields each field's name and value
/// @return the text with its fields filled
std::string filled(
    std::string text, std::initializer_list<std::pair<std::string_view, std::string_view>> fields
) {
    for (const auto& [name, value] : fields) {
        const std::string field = "{" + std::string(name) + "}";
        for (std::size_t at = text.find(field); at != std::string::npos;
             at = text.find(field, at + value.size()))
            text.replace(at, field.size(), value);
    }
    return text;
}

/// The game a kept fit belongs to, without the runtime's private target type.
struct FitStamp {
    std::string_view base_game{};
    std::span<const std::string> expansions{};
    std::string_view mod_key{};
    bool unkeyed = false;
};

/// The compatibility stamp a kept fit belongs to.
///
/// An unkeyed mod does not share the base game's stamp: its files are checked
/// on their own, and a later archive is still unread until drop_pack_map_fits.
///
/// @param target the game being played
/// @return the stamp
std::string fit_stamp(const FitStamp& target) {
    const std::string_view mod = target.unkeyed ? std::string_view{} : target.mod_key;
    std::string stamp = map_compatibility_key(target.base_game, target.expansions, mod);
    if (target.unkeyed)
        stamp.append("\nlocal");
    return stamp;
}

/// The name the description uses for the game being played.
///
/// @param profile_name the mod's name; empty when no mod is played
/// @return that name, or the interface's "No Mod"
std::string game_label(std::string_view profile_name) {
    if (!profile_name.empty())
        return std::string(profile_name);
    return std::string(oa::data::languages::interface_text(kBaseGameText));
}

/// A ready sentence translated, or empty when `reason` is a fit detail. The
/// name it holds is looked up too: a game's or an expansion's name shows in
/// the language, and a mod's name, which the catalogue lacks, as it is.
///
/// @param reason the stored reason, in English
/// @return the sentence the description shows; empty when it should be wrapped
std::string ready_sentence(std::string_view reason) {
    constexpr std::string_view demo = "This map is for the TA demo.";
    if (reason == demo)
        return std::string(oa::data::languages::interface_text(demo));
    constexpr std::string_view for_head = "This map is for ";
    if (reason.starts_with(for_head) && reason.ends_with('.') &&
        reason.size() > for_head.size() + 1) {
        const std::string_view name =
            reason.substr(for_head.size(), reason.size() - for_head.size() - 1);
        return filled(
            std::string(oa::data::languages::interface_text("This map is for {name}.")),
            {{"name", oa::data::languages::interface_text(name)}}
        );
    }
    constexpr std::string_view needs_head = "Needs ";
    constexpr std::string_view needs_mid = ", which is off.";
    if (reason.starts_with(needs_head)) {
        const std::size_t mid = reason.find(needs_mid);
        if (mid != std::string_view::npos && mid >= needs_head.size()) {
            const std::string_view name = reason.substr(needs_head.size(), mid - needs_head.size());
            std::string first = filled(
                std::string(oa::data::languages::interface_text("Needs {name}, which is off.")),
                {{"name", oa::data::languages::interface_text(name)}}
            );
            first.push_back(' ');
            first.append(oa::data::languages::interface_text("Turn it on in the Library."));
            return first;
        }
    }
    return {};
}

/// What choosing a refused map says: a ready sentence, translated, or a fit
/// detail in "Doesn't fit {game}: {reason}".
///
/// @param reason the stored reason
/// @param game the game being played, as the player sees it
/// @return the words
std::string refused_words(std::string_view reason, std::string_view game) {
    std::string ready = ready_sentence(reason);
    if (!ready.empty())
        return ready;
    return filled(
        std::string(oa::data::languages::interface_text(kRefusedText)),
        {{"game", game}, {"reason", reason}}
    );
}

/// A decision stored without naming the runtime's private type.
struct StoredDecision {
    std::string row{};
    std::string refusal{};
    bool sentence = false;
    bool final_note = false;
};

/// What choosing a map says, from the verdict.
///
/// A sentence that names the game or the expansion is kept in English, so the
/// description can translate it. A fit detail is stored on its own, with the
/// catalogue's "{game}" already filled.
///
/// @param verdict the verdict
/// @param game_name the game being played, as the player sees it
/// @return the decision
StoredDecision store_decision(const MapListVerdict& verdict, std::string_view game_name) {
    StoredDecision decision;
    decision.row = map_list_row_reason(verdict);
    decision.final_note = verdict.final_note;
    if (verdict.block == MapListBlock::playable || verdict.block == MapListBlock::ask_files)
        return decision;
    const std::string explanation = map_list_explanation(verdict, game_name);
    if (explanation.starts_with("Needs ") || explanation.starts_with("This map is ")) {
        decision.refusal = explanation;
        decision.sentence = true;
        return decision;
    }
    constexpr std::string_view catalogue = "Its catalogue says it does not fit {game}.";
    std::string detail = verdict.detail.empty() ? std::string("it does not fit") : verdict.detail;
    if (detail == catalogue) {
        detail = std::string(oa::data::languages::interface_text(catalogue));
        detail = filled(std::move(detail), {{"game", game_name}});
    }
    decision.refusal = std::move(detail);
    return decision;
}

} // namespace

void Runtime::destroy_pack_map_state(PackMapState* state) noexcept {
    delete state;
}

Runtime::PackMapState& Runtime::pack_map_state() {
    if (!pack_map_state_)
        pack_map_state_.reset(new PackMapState());
    return *pack_map_state_;
}

const PackMap* Runtime::pack_map(std::string_view name) {
    if (!pack::split_pack_map_name(name))
        return nullptr;
    return map_packs().find(name);
}

std::shared_ptr<const std::vector<uint8_t>> Runtime::mod_map_settings(std::string_view stem) const {
    if (stem.empty() || !plays_mod())
        return nullptr;
    const std::string base = "maps/" + std::string(stem);
    try {
        const oa::Provider settings = assets_.provider(base + ".ota");
        if ((settings.kind != oa::ProviderKind::loose &&
             settings.kind != oa::ProviderKind::archive) ||
            !inside_folder(settings.path, played_mod_folder()) ||
            assets_.provider(base + ".tnt").kind != oa::ProviderKind::none)
            return nullptr;
        std::vector<uint8_t> bytes = assets_.read(base + ".ota").bytes;
        const auto parsed = oa::formats::ota::parse(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())
        );
        if (!parsed.ok() ||
            oa::formats::ota::select_multiplayer_schema(*parsed.metadata, 2) == nullptr)
            return nullptr;
        return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    } catch (const std::exception&) {
        // A file the store cannot read is no settings.
        return nullptr;
    }
}

const Runtime::BaseMapSummary* Runtime::pack_map_mod_summary(const PackMap& map) const {
    for (const BaseMapSummary& summary : mod_map_settings_)
        if (same_without_case(summary.name, map.stem))
            return &summary;
    return nullptr;
}

std::string_view Runtime::pack_map_title(const PackMap& map) const {
    if (const BaseMapSummary* settings = pack_map_mod_summary(map);
        settings != nullptr && !settings->title.empty())
        return settings->title;
    return map.title;
}

const fit::Fit& Runtime::pack_map_fit(const PackMap& map) {
    auto& state = pack_map_state();
    const PlayedMapTarget played = played_map_target();
    // The mod's settings for the map are checked in place of its own OTA, as
    // they are mounted.
    const std::shared_ptr<const std::vector<uint8_t>> settings = mod_map_settings(map.stem);
    auto key = std::make_tuple(
        map.sha256,
        map.stem,
        fit_stamp({played.base_game, played.expansions, played.mod_key, played.unkeyed}) +
            settings_stamp(settings)
    );
    if (const auto found = state.fits.find(key); found != state.fits.end())
        return found->second;
    fit::Fit checked;
    try {
        const oa::data::defs::DataLayout& layout = oa::data::defs::data_layout();
        if (!state.game_names)
            state.game_names = fit::collect_game_names(assets_, layout);
        const pack::MapEntry entry = entry_of(map);
        std::unique_ptr<fit::MapFiles> files = fit::folder_map_files(map.folder, entry, map.id);
        if (files && settings)
            files =
                std::make_unique<SettledMapFiles>(std::move(files), shown_ota_path(map), settings);
        checked = files ? fit::check_map(assets_, *state.game_names, *files, entry, map.id, layout)
                        : unreadable_fit(map.name, "could not be read from its pack's folder");
    } catch (const std::exception& error) {
        checked = unreadable_fit(map.name, std::string("could not be checked: ") + error.what());
    }
    return state.fits.emplace(std::move(key), std::move(checked)).first->second;
}

void Runtime::drop_pack_map_fits() {
    if (pack_map_state_ == nullptr)
        return;
    auto& state = pack_map_state();
    state.fits.clear();
    state.game_names.reset();
}

Runtime::PackMapDecision Runtime::decide_pack_map(const PackMap& map) {
    const PlayedMapTarget target = played_map_target();
    const std::string game_name = game_label(
        options_.mod_profile && !options_.mod_profile->name.empty() ? options_.mod_profile->name
                                                                    : std::string_view{}
    );
    const bool from_registry = !map.registry.empty() && map.release > 0;
    ListedFit listed = ListedFit::unchecked;
    // The flags are views of these strings. Both verdicts read them, including
    // the reason that names another mod, so the strings stay until then.
    std::vector<std::pair<std::string, bool>> owned_rows;
    std::vector<CompatibleFlag> flags;
    if (from_registry && !target.unkeyed) {
        const CatalogueMapTable table = catalogue_map_table(map);
        if (table.unlisted) {
            listed = ListedFit::unlisted;
        } else if (!table.named) {
            listed = ListedFit::unfit;
        } else {
            owned_rows = table.rows;
            flags.reserve(owned_rows.size());
            for (const auto& row : owned_rows)
                flags.push_back(CompatibleFlag{row.first, row.second});
            const std::string_view mod = target.mod_key;
            if (const std::optional<bool> answer =
                    compatible_answer(flags, target.base_game, target.expansions, mod))
                listed = *answer ? ListedFit::fits : ListedFit::unfit;
            else
                listed = ListedFit::unchecked;
        }
    }
    const std::string_view mod_key =
        target.unkeyed ? std::string_view{} : std::string_view{target.mod_key};
    MapListVerdict verdict = map_list_verdict(
        map.supported_games,
        map.required_expansions,
        target.base_game,
        target.expansions,
        mod_key,
        listed,
        flags,
        {}
    );
    if (verdict.block == MapListBlock::ask_files) {
        const fit::Fit& checked = pack_map_fit(map);
        std::string detail;
        if (!checked.fits())
            detail = checked.failures.empty() ? std::string("it does not fit")
                                              : fit::describe(checked.failures.front());
        const FileFit files{true, checked.fits(), detail};
        verdict = map_list_verdict(
            map.supported_games,
            map.required_expansions,
            target.base_game,
            target.expansions,
            mod_key,
            listed,
            flags,
            files
        );
    }
    const StoredDecision stored = store_decision(verdict, game_name);
    PackMapDecision decision;
    decision.row = stored.row;
    decision.refusal = stored.refusal;
    decision.sentence = stored.sentence;
    decision.final_note = stored.final_note;
    return decision;
}

bool Runtime::prepare_pack_map(std::string_view name, std::string* reason) {
    auto& state = pack_map_state();
    // GET MAP is replacing this pack's folder: none of its maps is mounted
    // until the install ends.
    if (const auto split = pack::split_pack_map_name(name); split && pack_maps_held(split->id)) {
        if (state.mounted_id == split->id)
            release_pack_map();
        if (reason != nullptr)
            *reason = std::string(kHeldReason);
        return false;
    }
    // A map already mounted stays as it is: a joiner asks every frame.
    if (!state.mounted_name.empty() && state.mounted_name == name && assets_.pack_layer_mounted())
        return true;
    release_pack_map();
    const PackMap* map = pack_map(name);
    if (map == nullptr) {
        if (reason != nullptr)
            *reason = "the map is not installed";
        return false;
    }
    // A map the list greys is refused before any of its files are mounted.
    // The decision was made when the list was built; one that was not listed
    // is decided here, still without mounting.
    PackMapState::Block blocked;
    if (const auto kept = state.blocks.find(name); kept != state.blocks.end()) {
        blocked = kept->second;
    } else {
        const PackMapDecision decision = decide_pack_map(*map);
        blocked.row = decision.row;
        blocked.refusal = decision.refusal;
        blocked.sentence = decision.sentence;
        state.blocks.emplace(std::string(name), blocked);
    }
    if (!blocked.refusal.empty()) {
        if (const auto kept = state.refusals.find(name); kept != state.refusals.end())
            kept->second = blocked.refusal;
        else
            state.refusals.emplace(std::string(name), blocked.refusal);
        if (reason != nullptr)
            *reason = blocked.refusal;
        return false;
    }
    const auto refuse = [&](const fit::Fit& unfit) {
        for (const fit::Failure& failure : unfit.failures)
            std::cerr << "open-annihilation: map " << name
                      << " does not fit: " << fit::describe(failure) << '\n';
        const std::string why = unfit.failures.empty() ? std::string("it does not fit")
                                                       : fit::describe(unfit.failures.front());
        if (const auto kept = state.refusals.find(name); kept != state.refusals.end())
            kept->second = why;
        else
            state.refusals.emplace(std::string(name), why);
        if (reason != nullptr)
            *reason = why;
        return false;
    };
    const fit::Fit& kept = pack_map_fit(*map);
    if (!kept.fits())
        return refuse(kept);

    const pack::MapEntry entry = entry_of(*map);
    oa::PackLayerSpec spec{};
    spec.kind = oa::PackLayerKind::folder;
    spec.location = map->folder;
    spec.files = fit::layer_files(entry, map->id);
    spec.label = map->id + " " + map->sha256.substr(0, kLabelDigestDigits);
    // The played mod's own settings for the map are shown in place of its
    // pack's OTA, as the mod's OTA took the place of the map's own when the
    // game had the map's files.
    bool settled = false;
    if (const std::shared_ptr<const std::vector<uint8_t>> settings = mod_map_settings(map->stem)) {
        const std::string ota = shown_ota_path(*map);
        for (oa::PackLayerFile& file : spec.files)
            if (same_without_case(file.path, ota)) {
                file.contents = settings;
                settled = true;
            }
    }
    const std::size_t file_count = spec.files.size();
    std::string error;
    if (!assets_.mount_pack_layer(std::move(spec), &error))
        return refuse(unreadable_fit(
            map->name, "could not be mounted: " + (error.empty() ? std::string("no reason") : error)
        ));
    // The pack's folder may have changed since the kept check, so the files
    // as the store now shows them are checked whatever that check found.
    fit::Fit mounted;
    try {
        if (!state.game_names)
            state.game_names = fit::collect_game_names(assets_, oa::data::defs::data_layout());
        const std::unique_ptr<fit::MapFiles> files = fit::mounted_map_files(assets_);
        mounted = files ? fit::check_map(
                              assets_,
                              *state.game_names,
                              *files,
                              entry,
                              map->id,
                              oa::data::defs::data_layout()
                          )
                        : unreadable_fit(map->name, "could not be read as mounted");
    } catch (const std::exception& caught) {
        mounted = unreadable_fit(map->name, std::string("could not be checked: ") + caught.what());
    }
    if (!mounted.fits()) {
        std::ignore = assets_.unmount_pack_layer();
        return refuse(mounted);
    }
    state.mounted_name = map->name;
    state.mounted_id = map->id;
    if (const auto refused = state.refusals.find(name); refused != state.refusals.end())
        state.refusals.erase(refused);
    std::cerr << "open-annihilation: map pack " << map->id << ": mounted " << map->name << " ("
              << file_count << " files)";
    if (settled)
        std::cerr << ", with the mod's settings from maps/" << map->stem << ".ota";
    std::cerr << '\n';
    return true;
}

void Runtime::release_pack_map() {
    if (!assets_.pack_layer_mounted()) {
        if (pack_map_state_)
            pack_map_state_->mounted_name.clear();
        return;
    }
    const std::optional<std::string> label = assets_.pack_layer_label();
    if (!assets_.unmount_pack_layer())
        return;
    if (pack_map_state_ && !pack_map_state_->mounted_name.empty()) {
        std::cerr << "open-annihilation: map pack " << pack_map_state_->mounted_id << ": unmounted "
                  << pack_map_state_->mounted_name << '\n';
        pack_map_state_->mounted_name.clear();
        pack_map_state_->mounted_id.clear();
    } else {
        std::cerr << "open-annihilation: map pack layer " << label.value_or(std::string())
                  << ": unmounted\n";
    }
}

void Runtime::hold_pack_maps(std::string_view id, uint64_t item) {
    auto& state = pack_map_state();
    state.held_id = std::string(id);
    state.held_item = id.empty() ? 0 : item;
}

bool Runtime::pack_maps_held(std::string_view id) {
    if (!pack_map_state_ || pack_map_state_->held_id.empty() || pack_map_state_->held_id != id)
        return false;
    namespace content = oa::app::content;
    const uint64_t item = pack_map_state_->held_item;
    const std::vector<content::DownloadView> items = content_downloads().view();
    const auto found =
        std::find_if(items.begin(), items.end(), [item](const content::DownloadView& view) {
            return view.item == item;
        });
    const bool ended = found == items.end() || found->state == content::DownloadState::done ||
                       found->state == content::DownloadState::failed ||
                       found->state == content::DownloadState::cancelled;
    if (!ended)
        return true;
    std::cerr << "open-annihilation: map pack " << pack_map_state_->held_id
              << ": its maps may be mounted again\n";
    pack_map_state_->held_id.clear();
    pack_map_state_->held_item = 0;
    return false;
}

const PackMap* Runtime::mounted_pack_map() {
    if (!pack_map_state_ || pack_map_state_->mounted_name.empty() || !assets_.pack_layer_mounted())
        return nullptr;
    return map_packs().find(pack_map_state_->mounted_name);
}

std::optional<std::string> Runtime::pack_map_refusal(std::string_view name) const {
    if (!pack_map_state_)
        return std::nullopt;
    const auto found = pack_map_state_->refusals.find(name);
    if (found == pack_map_state_->refusals.end())
        return std::nullopt;
    return found->second;
}

std::optional<std::string> Runtime::map_refusal(std::string_view name) {
    if (std::optional<std::string> refused = pack_map_refusal(name))
        return refused;
    if (!unloadable_map_name_.empty() && unloadable_map_name_ == name)
        return std::string(entry::message_text(entry::Message::missing_terrain));
    return std::nullopt;
}

void Runtime::show_refused_map(std::string_view name, std::string_view reason) {
    const PackMap* map = pack_map(name);
    const BaseMapSummary* settings = map != nullptr ? pack_map_mod_summary(*map) : nullptr;
    set_modal_text("MAPNAME", map != nullptr ? pack_map_title(*map) : name, 0);
    std::string size;
    if (settings != nullptr)
        size = settings->size + "  " + translate_ui("Players") + ": " +
               std::to_string(settings->players);
    else if (map != nullptr)
        size = map->size + "  " + translate_ui("Players") + ": " + std::to_string(map->players);
    set_modal_text("SIZE", size, 0);
    const auto shown = picture();
    if (shown.value != 0) {
        release_picture(shown);
        set_picture({});
    }
    const std::string game = game_label(
        options_.mod_profile && !options_.mod_profile->name.empty() ? options_.mod_profile->name
                                                                    : std::string_view{}
    );
    // A map whose files could not be loaded says so in the game's own words.
    const std::string_view terrain = entry::message_text(entry::Message::missing_terrain);
    set_modal_text(
        "DESCRIPTION", reason == terrain ? translate_ui(terrain) : refused_words(reason, game), 0
    );
    invalidate_menu();
}

std::string Runtime::catalogue_map_refusal(
    std::span<const std::string> supported_games,
    std::span<const std::string> required_expansions,
    std::span<const std::pair<std::string, bool>> compatible
) const {
    const PlayedMapTarget target = played_map_target();
    const MapListVerdict verdict = catalogue_map_verdict(
        supported_games,
        required_expansions,
        compatible,
        target.base_game,
        target.expansions,
        target.mod_key,
        target.unkeyed
    );
    if (verdict.block == MapListBlock::playable || verdict.block == MapListBlock::ask_files)
        return {};
    const std::string game = game_label(
        options_.mod_profile && !options_.mod_profile->name.empty() ? options_.mod_profile->name
                                                                    : std::string_view{}
    );
    return refused_words(store_decision(verdict, game).refusal, game);
}

Runtime::PlayedMapTarget Runtime::played_map_target() const {
    namespace bundles = oa::data::content_bundles;
    PlayedMapTarget target;
    if (options_.game_dir_named && options_.content.empty()) {
        const std::vector<std::string> present = copied_content_bundles(options_.game_dir);
        const auto held = [&](std::string_view id) {
            return std::find(present.begin(), present.end(), id) != present.end();
        };
        if (held(bundles::default_base_game_id))
            target.base_game = std::string(bundles::default_base_game_id);
        else if (held("totala-demo"))
            target.base_game = "totala-demo";
        else
            target.base_game = std::string(bundles::default_base_game_id);
        for (const bundles::Bundle& bundle : bundles::bundles()) {
            if (bundle.kind != bundles::Kind::expansion)
                continue;
            bool extends = false;
            for (const std::string_view game : bundle.extends)
                if (game == target.base_game)
                    extends = true;
            if (extends && held(bundle.id))
                target.expansions.emplace_back(bundle.id);
        }
    } else {
        const bundles::ContentChoice choice = content_choice();
        target.base_game = choice.base_game.empty() ? std::string(bundles::default_base_game_id)
                                                    : choice.base_game;
        target.expansions = choice.expansions;
    }
    if (!plays_mod())
        return target;
    try {
        const std::optional<package_install::Origin> origin =
            package_install::read_origin(played_mod_folder());
        if (!origin || origin->kind != package_install::OriginKind::catalogue ||
            origin->catalogue_id.empty() || origin->release <= 0) {
            target.unkeyed = true;
            return target;
        }
        target.mod_key = origin->catalogue_id + "@" + std::to_string(origin->release);
    } catch (const std::exception&) {
        target.unkeyed = true;
    }
    return target;
}

std::string Runtime::played_map_key() const {
    const PlayedMapTarget target = played_map_target();
    if (target.unkeyed)
        return {};
    return map_compatibility_key(target.base_game, target.expansions, target.mod_key);
}

const std::vector<std::string>& Runtime::listed_pack_maps() {
    if (pack_map_state_ == nullptr) {
        static const std::vector<std::string> none;
        return none;
    }
    return pack_map_state_->listed;
}

std::vector<std::string> Runtime::pack_map_omission_lines() const {
    if (pack_map_state_ == nullptr)
        return {};
    return pack_map_state_->omission_lines;
}

void Runtime::refresh_listed_pack_maps() {
    auto& state = pack_map_state();
    const std::string key = played_map_key();
    const std::string shown_key = key.empty() ? std::string(kUnkeyedMod) : key;

    // Copy the index out before the lists change. A later refresh of the
    // index would free the span maps() returns.
    std::vector<PackMap> installed;
    for (const PackMap& installed_map : map_packs().maps())
        installed.push_back(installed_map);

    std::vector<std::string> base;
    base.reserve(eligible_map_names_.size());
    for (const std::string& name : eligible_map_names_)
        if (!pack::split_pack_map_name(name))
            base.push_back(name);

    std::map<std::string, int, std::less<>> omitted;
    std::map<std::string, PackMapState::Block, std::less<>> blocks;
    std::vector<std::string> listed;
    listed.reserve(installed.size());
    for (const PackMap& installed_map : installed) {
        if (std::find(base.begin(), base.end(), installed_map.name) != base.end())
            continue;
        // Every installed map is listed. A miss greys it; the files are read
        // only when the catalogue has not answered, and nothing is mounted.
        const PackMapDecision decision = decide_pack_map(installed_map);
        PackMapState::Block block;
        block.row = decision.row;
        block.refusal = decision.refusal;
        block.sentence = decision.sentence;
        if (decision.final_note)
            ++omitted[installed_map.id];
        blocks.emplace(installed_map.name, std::move(block));
        listed.push_back(installed_map.name);
    }
    state.blocks = std::move(blocks);

    for (const auto& [id, count] : omitted) {
        if (count <= 0)
            continue;
        const bool said = std::any_of(
            state.omission_logged.begin(),
            state.omission_logged.end(),
            [&](const std::pair<std::string, std::string>& kept) {
                return kept.first == id && kept.second == key;
            }
        );
        if (said)
            continue;
        state.omission_logged.emplace_back(id, key);
        const char* verb =
            count == 1 ? " map cannot be played with " : " maps cannot be played with ";
        const std::string line =
            "open-annihilation: map pack " + id + ": " + std::to_string(count) + verb + shown_key;
        std::cerr << line << '\n';
        state.omission_lines.push_back(line);
    }

    state.listed = listed;
    eligible_map_names_ = std::move(base);
    for (const std::string& name : state.listed)
        eligible_map_names_.push_back(name);
    const auto choosable = [&](const std::string& name) {
        if (!pack::split_pack_map_name(name))
            return true;
        const auto found = state.blocks.find(name);
        return found == state.blocks.end() || found->second.refusal.empty();
    };
    const bool default_listed =
        !first_map_name_.empty() &&
        std::find(eligible_map_names_.begin(), eligible_map_names_.end(), first_map_name_) !=
            eligible_map_names_.end();
    if (!first_map_name_.empty() && (!default_listed || !choosable(first_map_name_))) {
        const auto next =
            std::find_if(eligible_map_names_.begin(), eligible_map_names_.end(), choosable);
        if (next != eligible_map_names_.end())
            first_map_name_ = *next;
        else
            first_map_name_.clear();
    }

    if (screen_ == Screen::map_selection && map_modal_.active) {
        std::string highlighted;
        if (modal_map_index_ >= 0 &&
            static_cast<std::size_t>(modal_map_index_) < bound_map_names_.size())
            highlighted = bound_map_names_[static_cast<std::size_t>(modal_map_index_)];
        std::vector<std::string> names = copy_map_names();
        try {
            order_map_names(names);
        } catch (const std::exception& error) {
            std::cerr << "open-annihilation: map list: " << error.what() << '\n';
            names = copy_map_names();
        }
        map_modal_.names = names;
        bind_map_names(names);
        if (!highlighted.empty()) {
            const auto row =
                std::find(bound_map_names_.begin(), bound_map_names_.end(), highlighted);
            if (row != bound_map_names_.end()) {
                const auto place = static_cast<std::ptrdiff_t>(row - bound_map_names_.begin());
                if (place >= 0 &&
                    place <= static_cast<std::ptrdiff_t>(std::numeric_limits<int16_t>::max()))
                    set_selected_map_index(static_cast<int16_t>(place));
            }
        }
    }

    oa::ui::frontend_multiplayer::multiplayer_reload_maps();
}

void Runtime::order_map_names(std::vector<std::string>& names) {
    namespace mp = oa::ui::frontend_multiplayer;

    // Copy each title before the next find. The first find may refresh the
    // index, which frees the list a pointer into it would still name.
    struct Held {
        std::string key;
        std::string title;
        std::string pack_name;
        std::string reason;
        bool pack = false;
    };

    std::vector<Held> held;
    held.reserve(names.size());
    for (const std::string& name : names) {
        Held row;
        row.key = name;
        if (pack::split_pack_map_name(name)) {
            row.pack = true;
            if (const PackMap* map = map_packs().find(name)) {
                row.title = std::string(pack_map_title(*map));
                row.pack_name = map->pack_name;
            }
            if (pack_map_state_) {
                const auto blocked = pack_map_state_->blocks.find(name);
                if (blocked != pack_map_state_->blocks.end())
                    row.reason = blocked->second.row;
            }
        }
        held.push_back(std::move(row));
    }
    std::vector<mp::MapListFact> facts;
    facts.reserve(held.size());
    for (const Held& row : held)
        facts.push_back(
            mp::MapListFact{row.key, row.title, {}, row.pack_name, row.pack, row.reason}
        );
    const std::vector<std::size_t> order = mp::map_list_order(facts);
    if (order.size() != held.size())
        return;
    for (const std::size_t index : order) {
        if (index >= held.size())
            return;
    }
    std::vector<std::string> ordered;
    ordered.reserve(held.size());
    for (const std::size_t index : order)
        ordered.push_back(std::move(held[index].key));
    names = std::move(ordered);
}

void Runtime::label_bound_maps() {
    namespace mp = oa::ui::frontend_multiplayer;

    // Copy each title before the next find. The first find may refresh the
    // index, which frees the list a pointer into it would still name.
    struct Held {
        std::string key;
        std::string title;
        std::string pack_name;
        std::string reason;
        bool pack = false;
    };

    std::vector<Held> held;
    held.reserve(bound_map_names_.size());
    for (const std::string& name : bound_map_names_) {
        Held row;
        row.key = name;
        if (pack::split_pack_map_name(name)) {
            row.pack = true;
            if (const PackMap* map = map_packs().find(name)) {
                row.title = std::string(pack_map_title(*map));
                row.pack_name = map->pack_name;
            }
            if (pack_map_state_) {
                const auto blocked = pack_map_state_->blocks.find(name);
                if (blocked != pack_map_state_->blocks.end())
                    row.reason = blocked->second.row;
            }
        }
        held.push_back(std::move(row));
    }
    std::vector<mp::MapListFact> facts;
    facts.reserve(held.size());
    bound_map_grey_.assign(held.size(), 0);
    for (std::size_t index = 0; index < held.size(); ++index) {
        const Held& row = held[index];
        if (!row.reason.empty())
            bound_map_grey_[index] = 1;
        facts.push_back(
            mp::MapListFact{row.key, row.title, {}, row.pack_name, row.pack, row.reason}
        );
    }
    bound_map_labels_ = mp::map_list_texts(facts);
    if (bound_map_labels_.size() != bound_map_names_.size()) {
        bound_map_labels_ = bound_map_names_;
        bound_map_grey_.assign(bound_map_names_.size(), 0);
    }
}

std::string Runtime::pack_map_row_reason(std::string_view name) const {
    if (pack_map_state_ == nullptr)
        return {};
    const auto found = pack_map_state_->blocks.find(name);
    if (found == pack_map_state_->blocks.end())
        return {};
    return found->second.row;
}

const char* Runtime::pack_map_blocked_reason(std::string_view name) const {
    if (pack_map_state_ == nullptr)
        return nullptr;
    const auto found = pack_map_state_->blocks.find(name);
    if (found == pack_map_state_->blocks.end() || found->second.row.empty())
        return nullptr;
    return found->second.row.c_str();
}

void Runtime::note_pack_catalogue() {
    const std::optional<uint64_t> generation = content_catalogue_generation();
    if (!generation)
        return;
    auto& state = pack_map_state();
    if (state.catalogue_noted && state.catalogue_generation == *generation)
        return;
    state.catalogue_noted = true;
    state.catalogue_generation = *generation;
    refresh_listed_pack_maps();
}

} // namespace oa::app
