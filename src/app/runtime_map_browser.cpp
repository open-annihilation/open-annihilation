// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The map browser on the OA layer. MORE MAPS…, a screen of the layer of its
// own (more-maps), shows a kit button under the map list of the skirmish
// setup's map picker (SELMAP) and of the host's map dialog in the battle
// room, takes a press or a tap on it, and lets every other input go on to
// the picker. Pressing it opens the map browser (map-browser), modal over a
// backdrop, its cards built from the runtime's data: the base maps the start
// scan found, the installed map packs, and the map packs the catalogues
// offer, each labelled with the bundle, the mod or the pack it comes from.
// Select moves the picker's selection to the chosen map; Download and
// select first queues its pack for download as the Library's GET does, shows
// the download's progress, and selects the map once it is installed; Cancel
// changes nothing. The player still chooses the map with the picker's own
// LOAD.

#include "library_host.hpp"
#include "map_packs.hpp"
#include "oa_layer.hpp"

#include "oa/app/content/downloads.hpp"
#include "oa/app/content/pictures.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/content_copy.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/package_install/origin.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/data/content_bundles/content_choice.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/base/threads.hpp"
#include "oa/data/catalogue/catalogue.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/map_fit/map_fit.hpp"
#include "oa/data/map_pack/map_name.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/formats/url.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_multiplayer/panel.hpp"
#include "oa/ui/frontend_state/oa_button_place.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/frontend_state/map_selection.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/map_browser/model.hpp"
#include "oa/ui/map_browser/screen.hpp"
#include "oa/ui/map_previews/map_previews.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <ios>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace kit = oa::ui::kit;
namespace mb = oa::ui::map_browser;
namespace place = oa::ui::frontend_state;
namespace mp = oa::ui::frontend_multiplayer;
namespace previews = oa::ui::map_previews;
namespace fit = oa::data::map_fit;

/// MORE MAPS…'s screen's name on the OA layer.
constexpr std::string_view more_maps_screen_name = "more-maps";
/// MORE MAPS…'s control's automation name, which the endpoint lists as oa.more-maps.
constexpr std::string_view more_maps_control_name = "more-maps";
/// MORE MAPS…'s control's number on its screen.
constexpr kit::ControlId more_maps_control = 1;
/// MORE MAPS…'s words.
constexpr std::string_view word_more_maps = "MORE MAPS…";
/// MORE MAPS…'s sizes: the kit's buttons at the regular size, Regular's
/// control height and the buttons' least width, the caption in the regular
/// font with a button's padding.
constexpr const kit::Metrics& more_maps_metrics = kit::regular_metrics;
/// MORE MAPS…'s caption's font: the regular buttons'.
constexpr kit::FontRole more_maps_font = kit::FontRole::regular;
/// The words for a download the queue would not start.
constexpr std::string_view word_not_queued = "The download could not start: {reason}";
/// The words for a download that failed.
constexpr std::string_view word_failed = "The pack was not installed: {reason}";
/// The words for an online map the catalogue says does not fit.
constexpr std::string_view word_catalogue_unfit = "Its catalogue says it does not fit {game}.";
/// The words for a map the picker could not show.
constexpr std::string_view word_not_offered = "The map list does not hold this map.";
/// The game's short name in the words above.
constexpr std::string_view word_game_short = "No Mod";
/// The sound MORE MAPS… plays as the browser opens: the OA button's.
constexpr std::string_view open_sound = "Options";
/// The sound Cancel plays: the settings' Cancel.
constexpr std::string_view cancel_sound = "Previous";
/// The sound Select plays.
constexpr std::string_view select_sound = "SmallButton";

/// The side of the square a card's picture is fitted into, in pixels.
constexpr int32_t preview_side = 160;
/// The pictures the browser's preview cache keeps.
constexpr std::size_t kept_previews = 256;
/// The pictures it decodes a frame, at most.
constexpr int32_t decodes_a_frame = 4;
/// The largest pack preview read, in bytes.
constexpr uintmax_t most_preview_bytes = uintmax_t{4} << 20U;
/// The list's frame is drawn this many frame pixels outside the list gadget.
constexpr int32_t list_frame_outset = 3;

/// Returns one of the browser's words in the language shown.
///
/// @param english the words, in English
/// @return their translation, or the English
std::string shown(std::string_view english) {
    return std::string(oa::data::languages::interface_text(english));
}

/// Returns words with {name} replaced by a value, the pattern looked up first.
///
/// @param english the pattern, in English
/// @param name the place's name
/// @param value its value
/// @return the words
std::string shown_with(std::string_view english, std::string_view name, std::string_view value) {
    std::string text = shown(english);
    const std::string place = "{" + std::string(name) + "}";
    if (const std::size_t at = text.find(place); at != std::string::npos)
        text.replace(at, place.size(), value);
    return text;
}

/// Tells whether two rectangles share a point.
///
/// @param first a rectangle
/// @param second another
/// @return true when they meet
bool meet(const kit::Rect& first, const kit::Rect& second) noexcept {
    return kit::intersect(first, second).width > 0 && kit::intersect(first, second).height > 0;
}

/// The window row where a frame rectangle's bottom edge ends, rounded outward.
///
/// @param seen what the layer's screens are placed on
/// @param frame_rect the rectangle, in the frame's pixels
/// @return that row, in window pixels
int32_t frame_window_bottom(const LayerView& seen, const kit::Rect& frame_rect) {
    if (seen.picture_size.y <= 0)
        return frame_rect.y + frame_rect.height;
    const int64_t down = std::max(seen.picture_size.y, int32_t{1});
    const int64_t row = static_cast<int64_t>(frame_rect.y) + frame_rect.height;
    const int64_t mapped = (row * static_cast<int64_t>(seen.picture.height) + down - 1) / down;
    return static_cast<int32_t>(static_cast<int64_t>(seen.picture.y) + mapped);
}

/// The first point that draws on or below a window row.
///
/// A point drawn with nearest rounding can land one window pixel above the
/// row the frame's own scale rounds outward, which leaves the button on the
/// frame when the picture is not a whole number of frame pixels.
///
/// @param window_row the row, in window pixels
/// @param percent the layer's scale, in percent
/// @return the point
int32_t point_on_or_below(int32_t window_row, int32_t percent) {
    const int64_t scale = std::max(percent, int32_t{1});
    int32_t point = 0;
    if (window_row > 0)
        point = static_cast<int32_t>(static_cast<int64_t>(window_row) * 100 / scale);
    else if (window_row < 0)
        point =
            static_cast<int32_t>(-((static_cast<int64_t>(-window_row) * 100 + scale - 1) / scale));
    for (int32_t step = 0; step < 4; ++step) {
        if (kit::to_canvas(kit::Rect{0, point, 1, 1}, percent).y >= window_row)
            return point;
        ++point;
    }
    return point;
}

/// Reads a whole file of at most a size.
///
/// @param file the file
/// @param most the most bytes read
/// @param[out] bytes its bytes
/// @return false when it is missing, larger or cannot be read
bool read_whole_file(const fs::path& file, uintmax_t most, std::vector<uint8_t>& bytes) {
    std::error_code error;
    const uintmax_t size = fs::file_size(file, error);
    if (error || size > most)
        return false;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return false;
    bytes.assign(static_cast<std::size_t>(size), 0);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<std::size_t>(in.gcount()) == bytes.size();
}

namespace bundles = oa::data::content_bundles;

/// The catalogue key the base game's overlay shows. Other bundles show the
/// table's name in the language shown.
constexpr std::string_view base_overlay = "TA 3.1c";

/// Returns the overlay a bundle's maps show.
///
/// @param bundle the bundle
/// @return "TA 3.1c" for the base game, otherwise its name in the language
///     shown
std::string bundle_overlay(const bundles::Bundle& bundle) {
    if (bundle.id == bundles::default_base_game_id)
        return std::string(base_overlay);
    return std::string(oa::data::languages::interface_text(bundle.english_name));
}

/// Returns the overlay of the base game being played.
///
/// @param base_game the base game's id; empty when none is known
/// @return "TA 3.1c" when that is the base game, or none is known; otherwise
///     the base game's English name
std::string playing_base_overlay(std::string_view base_game) {
    if (base_game.empty() || base_game == bundles::default_base_game_id)
        return std::string(base_overlay);
    if (const bundles::Bundle* bundle = bundles::find_bundle(base_game))
        return bundle_overlay(*bundle);
    return std::string(base_overlay);
}

/// Returns a path as lower-case UTF-8 with '/' between its parts.
///
/// @param path the path
/// @return the folded text
std::string folded_path(const fs::path& path) {
    std::string text = path_to_utf8(path);
    for (char& letter : text) {
        if (letter == '\\')
            letter = '/';
        else if (letter >= 'A' && letter <= 'Z')
            letter = static_cast<char>(letter - 'A' + 'a');
    }
    return text;
}

/// Tells whether a file sits in a folder or in a folder inside it.
///
/// The comparison ignores ASCII case and treats '\\' as '/'. A path that
/// cannot be made absolute matches nothing.
///
/// @param file the file
/// @param folder the folder
/// @return true when the file is in the folder
bool path_is_under(const fs::path& file, const fs::path& folder) {
    if (file.empty() || folder.empty())
        return false;
    std::error_code error;
    const fs::path canon_file = fs::weakly_canonical(file, error);
    if (error)
        return false;
    error.clear();
    const fs::path canon_folder = fs::weakly_canonical(folder, error);
    if (error)
        return false;
    std::string child = folded_path(canon_file);
    std::string parent = folded_path(canon_folder);
    while (parent.size() > 1 && parent.back() == '/')
        parent.pop_back();
    if (parent.empty() || child.size() < parent.size())
        return false;
    if (child.compare(0, parent.size(), parent) != 0)
        return false;
    return child.size() == parent.size() || child[parent.size()] == '/';
}

/// Returns the last part of a content path.
///
/// @param path a path with '/' or '\\'
/// @return the file name
std::string_view last_component(std::string_view path) noexcept {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string_view::npos)
        return path;
    return path.substr(slash + 1);
}

/// Returns the bundle whose archive has this file name, from the whole table.
///
/// @param archive_name the archive's file name
/// @return the bundle; null when no bundle lists that archive
const bundles::Bundle* bundle_for_archive(std::string_view archive_name) {
    if (archive_name.empty())
        return nullptr;
    for (const bundles::Bundle& bundle : bundles::bundles()) {
        for (const bundles::ContentFile& file : bundle.files) {
            if (same_archive_file_name(archive_name, last_component(file.copy_name)))
                return &bundle;
            for (const std::string_view source_path : file.source_names)
                if (same_archive_file_name(archive_name, last_component(source_path)))
                    return &bundle;
        }
    }
    return nullptr;
}

/// Returns the bundle whose folder holds a file. The longest folder wins,
/// so an expansion stored beside the base game is not labelled as the base.
///
/// @param file the file
/// @param user the player's folder
/// @return the bundle; null when the file is in no bundle folder
const bundles::Bundle* bundle_holding_path(const fs::path& file, const fs::path& user) {
    const bundles::Bundle* found = nullptr;
    std::size_t longest = 0;
    for (const bundles::Bundle& bundle : bundles::bundles()) {
        const fs::path folder = bundle_folder(user, bundle);
        if (!path_is_under(file, folder))
            continue;
        std::error_code error;
        const fs::path canon = fs::weakly_canonical(folder, error);
        const std::size_t length = error ? std::size_t{0} : folded_path(canon).size();
        if (length > longest) {
            found = &bundle;
            longest = length;
        }
    }
    return found;
}

/// Splits the scanned maps into the game's and the mod's. A map whose file
/// sits in the mod's folder is the mod's, even when its archive is a
/// bundle's. Otherwise the archive's file name names the bundle, and a file
/// in a bundle's folder names that bundle. Anything else, including a map
/// whose file cannot be found, stays in the list labelled with the base
/// game being played. No map is dropped.
///
/// @param assets the mounted files
/// @param mod_label the mod's name; empty when no mod plays
/// @param mod_plays whether a mod plays
/// @param mod_folder the mod's folder; unused when no mod plays
/// @param user the player's folder
/// @param base_game the base game's id
/// @param played the scanned maps
/// @param[out] game the game's maps
/// @param[out] from_mod the mod's maps
void classify_played_maps(
    const oa::AssetStore& assets,
    const std::string& mod_label,
    bool mod_plays,
    const fs::path& mod_folder,
    const fs::path& user,
    std::string_view base_game,
    std::vector<mb::MapCard> played,
    std::vector<mb::MapCard>& game,
    std::vector<mb::MapCard>& from_mod
) {
    const std::string base_label = playing_base_overlay(base_game);
    for (mb::MapCard& card : played) {
        oa::Provider found;
        try {
            found = assets.provider("maps/" + card.name + ".ota");
        } catch (const std::exception&) {
            found = {};
        }
        const bool placed = found.kind != oa::ProviderKind::none && !found.path.empty();
        if (mod_plays && placed && path_is_under(found.path, mod_folder)) {
            card.source_name = mod_label;
            from_mod.push_back(std::move(card));
            continue;
        }
        const bundles::Bundle* bundle = nullptr;
        if (placed && found.kind == oa::ProviderKind::archive)
            bundle = bundle_for_archive(path_to_utf8(found.path.filename()));
        if (bundle == nullptr && placed)
            bundle = bundle_holding_path(found.path, user);
        card.source_name = bundle != nullptr ? bundle_overlay(*bundle) : base_label;
        game.push_back(std::move(card));
    }
}

/// One online map's pack: where to download it from, and its picture.
struct OnlineSource {
    std::string registry{}; ///< the registry's id
    std::string key{};      ///< the pack's key in its catalogue
    std::string picture{};  ///< the map's picture's address; empty for none
};

} // namespace

// ---------------------------------------------------------------------------------------------
// The browser

/// The map browser as a screen of the OA layer: modal, over a backdrop,
/// centred in the window at its size class's dialog size, over the picker
/// that opened it, and never in a match. Its model, its state and its
/// pictures are its own; what it asks for it carries out through the
/// runtime.
struct Runtime::MapBrowserScreen final : LayerScreen {
    /// Makes the browser over a picker, its cards built from the runtime's
    /// data, and reads the game palette the base maps' pictures take.
    ///
    /// @param runtime the runtime
    /// @param opener the picker that opens it
    MapBrowserScreen(Runtime& runtime, mb::Browser::Opener opener)
        : runtime_(runtime), previews_(kept_previews, preview_side) {
        model_.opener = opener;
        model_.mod_name = mod_name();
        mod_key_ = mod_release_key();
        build_cards();
        try {
            palette_ = load_active_palette(runtime_.assets_);
        } catch (const std::exception& error) {
            std::cerr << "open-annihilation: map browser: base map pictures: " << error.what()
                      << '\n';
        }
        host_.context = this;
        host_.read_map = read_map;
        host_.read_file = read_file;
        host_.online_bytes = online_bytes;
        host_.palette = palette_ ? &*palette_ : nullptr;
    }

    /// Returns "map-browser".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return mb::screen_name; }

    /// Returns where the browser shows: centred at the view's scale, its
    /// size class's window size; nowhere in a match or once its picker is
    /// gone.
    ///
    /// @param placed_on what the screen is placed on
    /// @return its place
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        if (placed_on.match || !opener_shows())
            return {};
        const kit::Point size = mb::window_size(placed_on.frame);
        return centred_placement(placed_on, size.x, size.y);
    }

    /// Tells that the browser takes every input.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that what lies under the browser darkens.
    ///
    /// @return true
    [[nodiscard]] bool backdrop() const override { return true; }

    /// Lays the browser out again when the view's frame changed.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override {
        if (laid_out_ && placed_on.frame.area.width == frame_.area.width &&
            placed_on.frame.area.height == frame_.area.height &&
            placed_on.frame.size_class == frame_.size_class &&
            placed_on.frame.scale_percent == frame_.scale_percent)
            return;
        frame_ = placed_on.frame;
        laid_out_ = true;
        relayout();
    }

    /// Draws the browser at the canvas's scale.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override {
        mb::draw_browser(canvas, list_, mb::window_size(frame_));
    }

    /// Takes a pointer's move, press or release.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the browser's points
    /// @param finger_reach a finger's reach; 0 for a mouse
    /// @return what the browser did
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        mb::PointerKind moved = mb::PointerKind::move;
        if (kind == ScreenInputKind::pointer_down)
            moved = mb::PointerKind::down;
        else if (kind == ScreenInputKind::pointer_up)
            moved = mb::PointerKind::up;
        else if (kind != ScreenInputKind::pointer_move)
            return LayerAnswer::none;
        return take(mb::browser_pointer(model_, state_, list_, moved, button, at, finger_reach));
    }

    /// Takes a key, as the kit names it.
    ///
    /// @param pressed the key
    /// @return what the browser did
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        return take(mb::browser_key(model_, state_, list_, pressed));
    }

    /// Takes the controller's Y, which gives the search the focus as it does
    /// in the Library; every other action becomes a key, as on any screen.
    ///
    /// @param action the gamepad action
    /// @return what the browser did; pass for every other action
    LayerAnswer pad_action(oa::ui::pad_controls::Action action) override {
        if (action != oa::ui::pad_controls::Action::library_search)
            return LayerAnswer::pass;
        return take(mb::browser_find(state_, list_));
    }

    /// Takes the controller's right stick: it scrolls the grid.
    ///
    /// @param notches the stick's turn; positive towards the top
    /// @return what the browser did
    LayerAnswer pad_stick(float notches) override {
        return take(mb::browser_wheel(model_, state_, list_, notches));
    }

    /// Takes a turn of the wheel: it scrolls the grid.
    ///
    /// @param notches how far the wheel turned
    /// @return what the browser did
    LayerAnswer wheel(kit::Point /*at*/, float notches) override {
        return take(mb::browser_wheel(model_, state_, list_, notches));
    }

    /// Takes text typed into the search.
    ///
    /// @param utf8 the text
    /// @return what the browser did
    LayerAnswer text(std::string_view utf8) override {
        return take(mb::browser_text(model_, state_, list_, utf8));
    }

    /// Returns the search field while it has the focus.
    ///
    /// @return its rectangle, in the browser's points; none otherwise
    [[nodiscard]] std::optional<kit::Rect> text_field() const override {
        return mb::focused_field(state_, list_);
    }

    /// Returns what the browser draws and where its controls are.
    ///
    /// @return the display list
    [[nodiscard]] kit::DisplayList display_list() const override { return list_; }

    /// Returns the browser's hover, press and focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override { return state_.interaction; }

    /// Brings the browser up to date once a frame: closes it when its picker
    /// is gone or a match starts, follows a download, and decodes a few of
    /// the pictures the cards in view ask for.
    ///
    /// @return close when the browser closes; redraw when what it shows
    ///     changed; otherwise none
    LayerAnswer tick() override {
        if (!opener_shows() || runtime_.screen_ == Screen::match || runtime_.match_ != nullptr)
            return LayerAnswer::close;
        const uint64_t before = revision_;
        if (follow_download())
            return LayerAnswer::close;
        const std::size_t pictures_before = pictures_shown();
        previews_.new_frame();
        relayout();
        previews_.pump(host_, decodes_a_frame);
        // The pump may have let pictures go: the list is laid out again
        // before anything draws it.
        relayout();
        if (pictures_shown() == pictures_before && revision_ == before + 2)
            revision_ = before;
        else
            revision_ = before + 1;
        return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Returns the browser's revision, which counts the changes to what it shows.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Lets the browser go: a download under way goes on in the queue, and
    /// the Library shows it.
    void close(bool /*by_key*/) override {}

    /// Opens the browser over a picker, unless it shows already.
    ///
    /// @param runtime the runtime
    /// @param opener the picker
    static void open(Runtime& runtime, mb::Browser::Opener opener) {
        OaLayer& layer = runtime.oa_layer();
        if (layer.find(mb::screen_name) != nullptr || runtime.oa_layer().screen_fonts() == nullptr)
            return;
        runtime.play_ui_sound(open_sound, 0);
        layer.push(std::make_unique<MapBrowserScreen>(runtime, opener));
    }

    /// Tells whether a picker shows: SELMAP, or the host's map dialog in the
    /// battle room.
    ///
    /// @param runtime the runtime
    /// @param opener the picker
    /// @return true while it shows
    static bool picker_shows(const Runtime& runtime, mb::Browser::Opener opener) {
        if (opener == mb::Browser::Opener::skirmish)
            return runtime.screen_ == Screen::map_selection && runtime.map_modal_.active;
        return mp::multiplayer_map_list_rect(nullptr, nullptr, nullptr, nullptr);
    }

  private:

    /// Tells whether the picker that opened the browser shows.
    ///
    /// @return true while it does
    [[nodiscard]] bool opener_shows() const { return picker_shows(runtime_, model_.opener); }

    /// Returns the name of the mod being played: its profile's name, else
    /// its folder's; empty when no mod plays.
    ///
    /// @return the name
    [[nodiscard]] std::string mod_name() const {
        if (!runtime_.plays_mod())
            return {};
        if (const auto* profile = runtime_.mod_profile();
            profile != nullptr && !profile->name.empty())
            return profile->name;
        return path_to_utf8(runtime_.played_mod_folder().filename());
    }

    /// Returns the compatible table's key for the mod being played,
    /// `<id>@<release>`, from its folder's origin record when it came from
    /// a catalogue.
    ///
    /// @return the key; none when no mod plays or its release is not known
    [[nodiscard]] std::optional<std::string> mod_release_key() const {
        if (!runtime_.plays_mod())
            return std::nullopt;
        const std::optional<package_install::Origin> origin =
            package_install::read_origin(runtime_.played_mod_folder());
        if (!origin || origin->kind != package_install::OriginKind::catalogue ||
            origin->catalogue_id.empty() || origin->release <= 0)
            return std::nullopt;
        return origin->catalogue_id + "@" + std::to_string(origin->release);
    }

    /// Returns the cards of the base maps the start scan found.
    ///
    /// @return the cards
    [[nodiscard]] std::vector<mb::MapCard> base_cards() const {
        std::vector<mb::MapCard> cards;
        for (const BaseMapSummary& summary : runtime_.base_map_summaries_) {
            mb::MapCard card;
            card.name = summary.name;
            card.title = summary.title.empty() ? summary.name : summary.title;
            card.size = summary.size;
            card.players = summary.players;
            card.player_counts = summary.player_counts;
            card.preview_key = summary.name;
            cards.push_back(std::move(card));
        }
        return cards;
    }

    /// Returns the cards of the installed pack maps, from their packs'
    /// manifests. Their fit is checked as they come into view (check_fits).
    ///
    /// @param registry_names each registry's name, by id
    /// @return the cards
    std::vector<mb::MapCard>
    installed_cards(const std::map<std::string, std::string, std::less<>>& registry_names) {
        std::vector<mb::MapCard> cards;
        for (const PackMap& map : runtime_.map_packs().maps()) {
            mb::MapCard card;
            card.name = map.name;
            const std::string_view title = runtime_.pack_map_title(map);
            card.title = title.empty() ? map.name : std::string(title);
            card.pack_name = map.pack_name;
            card.pack_version = map.pack_version;
            if (const auto named = registry_names.find(map.registry); named != registry_names.end())
                card.registry_name = named->second;
            else
                card.registry_name = map.registry;
            // The played mod's settings for the map are what it plays with.
            if (const BaseMapSummary* settings = runtime_.pack_map_mod_summary(map)) {
                card.size = settings->size;
                card.players = settings->players;
                card.player_counts = settings->player_counts;
            } else {
                card.size = map.size;
                card.players = map.players;
                card.player_counts = {map.players};
            }
            card.base_fit = mb::MapCard::Fit::unknown;
            card.mod_fit = mb::MapCard::Fit::unknown;
            if (!map.preview.empty()) {
                card.preview_key = path_to_utf8(map.preview);
                preview_files_[card.name] = map.preview;
            }
            cards.push_back(std::move(card));
        }
        return cards;
    }

    /// Returns a fit the catalogue's compatible table gives for the game being played.
    ///
    /// The new names are read first, then `ta-3.1c` and `<id>@<release>`.
    ///
    /// @param table the map's compatible table
    /// @param target the base game, the expansions that are on, and the mod's key
    /// @param mod_key the mod's key; empty asks about the base game alone
    /// @return fits, does not fit, or unknown when the table does not say
    [[nodiscard]] static mb::MapCard::Fit catalogue_fit(
        const std::vector<std::pair<std::string, bool>>& table,
        const PlayedMapTarget& target,
        std::string_view mod_key
    ) {
        std::vector<CompatibleFlag> rows;
        rows.reserve(table.size());
        for (const auto& [game, fitting] : table)
            rows.push_back(CompatibleFlag{game, fitting});
        const std::optional<bool> answer =
            compatible_answer(rows, target.base_game, target.expansions, mod_key);
        if (!answer)
            return mb::MapCard::Fit::unknown;
        return *answer ? mb::MapCard::Fit::fits : mb::MapCard::Fit::does_not_fit;
    }

    /// Returns the cards of the map packs the catalogues offer, and records
    /// where each comes from. A pack whose picture's address names no
    /// SHA-256 shows none.
    ///
    /// @param snapshot the content service's snapshot; null offers none
    /// @return the cards
    std::vector<mb::MapCard> online_cards(const content::Snapshot* snapshot) {
        std::vector<mb::MapCard> cards;
        if (snapshot == nullptr)
            return cards;
        for (const content::Entry& listed : snapshot->entries) {
            if (listed.package == nullptr || listed.package->kind != data::catalogue::Kind::oamap)
                continue;
            const data::catalogue::Package& package = *listed.package;
            const content::RegistryView* registry = nullptr;
            for (const content::RegistryView& candidate : snapshot->registries)
                if (candidate.registry.descriptor.id == listed.registry)
                    registry = &candidate;
            for (const data::catalogue::MapEntry& map : package.maps) {
                mb::MapCard card;
                card.name = map.map;
                card.title = map.name.empty() ? map.map : map.name;
                card.pack_name = package.name;
                card.pack_version = package.version;
                card.registry_name =
                    registry != nullptr ? registry->registry.descriptor.name : listed.registry;
                card.size = std::to_string(map.width) + "x" + std::to_string(map.height);
                card.players = map.players;
                card.player_counts = {map.players};
                card.download_bytes = package.size;
                const PlayedMapTarget target = runtime_.played_map_target();
                card.base_fit = catalogue_fit(map.compatible, target, {});
                card.mod_fit = model_.mod_name.empty() ? mb::MapCard::Fit::fits
                               : !mod_key_ ? mb::MapCard::Fit::unknown
                                           : catalogue_fit(map.compatible, target, *mod_key_);
                if (card.base_fit == mb::MapCard::Fit::does_not_fit)
                    card.fit_reason =
                        shown_with(word_catalogue_unfit, "game", shown(word_game_short));
                else if (card.mod_fit == mb::MapCard::Fit::does_not_fit)
                    card.fit_reason = shown_with(word_catalogue_unfit, "game", model_.mod_name);
                OnlineSource source;
                source.registry = listed.registry;
                source.key = package.id;
                if (!map.preview.empty() && registry != nullptr && registry->served_from)
                    if (const std::optional<formats::url::Url> resolved =
                            formats::url::resolve_reference(*registry->served_from, map.preview))
                        if (const std::string address = formats::url::url_text(*resolved);
                            content::picture_digest(address)) {
                            source.picture = address;
                            card.preview_key = address;
                        }
                if (online_.count(card.name) == 0)
                    online_[card.name] = source;
                if (!card.preview_key.empty())
                    picture_registry_[card.preview_key] = source.registry;
                cards.push_back(std::move(card));
            }
        }
        return cards;
    }

    /// Builds every card again from the runtime's data, keeping the filters
    /// and the selected map, and filters them.
    void build_cards() {
        const std::optional<std::string> was_selected =
            model_.selected && *model_.selected < model_.cards.size()
                ? std::optional<std::string>(model_.cards[*model_.selected].name)
                : std::nullopt;
        online_.clear();
        picture_registry_.clear();
        preview_files_.clear();
        fit_checked_.clear();
        const std::shared_ptr<const content::Snapshot> snapshot =
            runtime_.content_service().snapshot();
        std::map<std::string, std::string, std::less<>> registry_names;
        if (snapshot)
            for (const content::RegistryView& registry : snapshot->registries)
                registry_names[registry.registry.descriptor.id] = registry.registry.descriptor.name;
        std::vector<mb::MapCard> game;
        std::vector<mb::MapCard> from_mod;
        const bool mod_plays = runtime_.plays_mod();
        const fs::path mod_folder = mod_plays ? runtime_.played_mod_folder() : fs::path{};
        classify_played_maps(
            runtime_.assets_,
            model_.mod_name,
            mod_plays,
            mod_folder,
            runtime_.user_folder(),
            runtime_.played_map_target().base_game,
            base_cards(),
            game,
            from_mod
        );
        model_.cards = mb::merge_cards(
            std::move(game),
            std::move(from_mod),
            installed_cards(registry_names),
            online_cards(snapshot.get())
        );
        model_.selected.reset();
        if (was_selected)
            for (std::size_t index = 0; index < model_.cards.size(); ++index)
                if (model_.cards[index].name == *was_selected)
                    model_.selected = index;
        mb::apply_filters(model_);
    }

    /// Checks the fit of the installed pack maps that are in view, in the
    /// row after it or selected, the first time each is: P08's check of the
    /// map's files against the game and the mod being played, kept for the
    /// pack. A map that fails it fails the mod's fit while a mod plays, and
    /// the game's otherwise: a pack that clashes with TA 3.1c itself is
    /// refused when it is installed.
    void check_fits() {
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (fonts == nullptr)
            return;
        std::vector<std::size_t> due = mb::cards_in_view(model_, state_, frame_, *fonts);
        if (model_.selected)
            due.push_back(*model_.selected);
        for (const std::size_t index : due) {
            if (index >= model_.cards.size())
                continue;
            mb::MapCard& card = model_.cards[index];
            if (card.origin != mb::MapCard::Origin::installed ||
                !fit_checked_.insert(card.name).second)
                continue;
            const PackMap* map = runtime_.pack_map(card.name);
            if (map == nullptr)
                continue;
            const fit::Fit& checked = runtime_.pack_map_fit(*map);
            card.base_fit = mb::MapCard::Fit::fits;
            card.mod_fit = mb::MapCard::Fit::fits;
            if (checked.fits())
                continue;
            card.fit_reason = fit::describe(checked.failures.front());
            if (model_.mod_name.empty())
                card.base_fit = mb::MapCard::Fit::does_not_fit;
            else
                card.mod_fit = mb::MapCard::Fit::does_not_fit;
        }
    }

    /// Lays the browser out from its model and state, checking the fit of
    /// the installed maps coming into view first, and counts the change.
    void relayout() {
        ++revision_;
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (fonts == nullptr) {
            list_ = {};
            return;
        }
        check_fits();
        mb::PreviewHooks hooks;
        hooks.context = this;
        hooks.ask = ask_picture;
        hooks.failed = picture_failed;
        list_ = mb::browser_layout(model_, state_, frame_, *fonts, hooks);
    }

    /// Counts the cards whose pictures show, to tell a frame that decoded one.
    ///
    /// @return the count
    [[nodiscard]] std::size_t pictures_shown() const {
        std::size_t count = 0;
        for (const kit::Item& item : list_.items)
            if (const auto* card = std::get_if<kit::CardLook>(&item.look);
                card != nullptr && oa::ui::frontend_renderer::picture_drawable(card->picture))
                ++count;
        return count;
    }

    /// Carries out what an input asked for, and lays the browser out again
    /// when what it shows changed.
    ///
    /// @param result what the input asked for
    /// @return close when the browser closes; redraw when it changed; otherwise none
    LayerAnswer take(const mb::ScreenResult& result) {
        switch (result.action) {
        case mb::BrowserAction::cancel:
            runtime_.play_ui_sound(cancel_sound, 0);
            return LayerAnswer::close;
        case mb::BrowserAction::choose:
            if (choose())
                return LayerAnswer::close;
            relayout();
            return LayerAnswer::redraw;
        case mb::BrowserAction::none:
            break;
        }
        if (!result.redraw)
            return LayerAnswer::none;
        relayout();
        return LayerAnswer::redraw;
    }

    /// Carries out the primary button on the selected card: an online map's
    /// pack is queued for download, and the browser stays open while it
    /// downloads; any other map is selected in the picker and the browser
    /// closes. A map that does not fit is never chosen.
    ///
    /// @return true when the browser closes
    bool choose() {
        check_fits();
        if (!model_.selected || *model_.selected >= model_.cards.size() ||
            !mb::primary_action(model_).enabled)
            return false;
        const mb::MapCard card = model_.cards[*model_.selected];
        if (card.origin == mb::MapCard::Origin::online) {
            download(card);
            return false;
        }
        if (!select_in_picker(card.name)) {
            model_.error = shown(word_not_offered);
            return false;
        }
        runtime_.play_ui_sound(select_sound, 0);
        return true;
    }

    /// Queues an online map's pack for download, as the Library's GET does,
    /// never while a match runs.
    ///
    /// @param card the map's card
    void download(const mb::MapCard& card) {
        const auto source = online_.find(card.name);
        if (source == online_.end() || runtime_.match_ != nullptr ||
            runtime_.screen_ == Screen::match)
            return;
        std::string why;
        const std::optional<uint64_t> item = runtime_.queue_download(
            source->second.registry, source->second.key, content::DownloadReason::install, &why
        );
        if (!item) {
            model_.error = shown_with(word_not_queued, "reason", why);
            return;
        }
        download_item_ = item;
        model_.downloading = card.name;
        model_.progress = 0.0F;
        model_.error.clear();
    }

    /// Follows the download under way: its progress, its failure, which
    /// shows in the footer and gives the button back, and its install, once
    /// the installed packs list the map, after which the map is selected in
    /// the picker as Select does.
    ///
    /// @return true when the browser closes, the map selected
    bool follow_download() {
        if (!model_.downloading || !download_item_)
            return false;
        std::optional<content::DownloadView> found;
        for (const content::DownloadView& queued : runtime_.content_downloads().view())
            if (queued.item == *download_item_)
                found = queued;
        if (!found)
            return false;
        const std::string downloaded = *model_.downloading;
        switch (found->state) {
        case content::DownloadState::done: {
            if (runtime_.pack_map(downloaded) == nullptr)
                runtime_.map_packs().refresh();
            if (runtime_.pack_map(downloaded) == nullptr)
                return false;
            model_.downloading.reset();
            download_item_.reset();
            build_cards();
            for (std::size_t index = 0; index < model_.cards.size(); ++index)
                if (model_.cards[index].name == downloaded)
                    model_.selected = index;
            mb::apply_filters(model_);
            if (!select_in_picker(downloaded)) {
                model_.error = shown(word_not_offered);
                ++revision_;
                return false;
            }
            runtime_.play_ui_sound(select_sound, 0);
            return true;
        }
        case content::DownloadState::failed:
        case content::DownloadState::cancelled: {
            std::string reason = content::failure_text(found->failure);
            if (!found->detail.empty())
                reason +=
                    (reason.empty() ? "" : " (") + found->detail + (reason.empty() ? "" : ")");
            model_.error = shown_with(word_failed, "reason", reason);
            model_.downloading.reset();
            download_item_.reset();
            ++revision_;
            return false;
        }
        default:
            break;
        }
        const float progress =
            found->target.size == 0
                ? 0.0F
                : static_cast<float>(
                      static_cast<double>(std::min(found->done, found->target.size)) /
                      static_cast<double>(found->target.size)
                  );
        if (progress != model_.progress) {
            model_.progress = progress;
            ++revision_;
        }
        return false;
    }

    /// Moves the picker's selection to a map, as a click on its row does,
    /// and previews it: SELMAP's list is built again first when the map is
    /// a pack installed since it opened. The player still chooses it with
    /// the picker's LOAD.
    ///
    /// @param map_name the map's game name
    /// @return true when the picker selected it
    bool select_in_picker(const std::string& map_name) {
        if (model_.opener == mb::Browser::Opener::battle_room)
            return mp::multiplayer_offer_map(map_name.c_str());
        Runtime& game = runtime_;
        if (std::find(game.bound_map_names_.begin(), game.bound_map_names_.end(), map_name) ==
            game.bound_map_names_.end()) {
            // A pack installed since the list was made joins the list in the
            // name order when the map fits the mod being played.
            game.refresh_listed_pack_maps();
        }
        const auto row =
            std::find(game.bound_map_names_.begin(), game.bound_map_names_.end(), map_name);
        if (row == game.bound_map_names_.end())
            return false;
        game.preview_map_index(static_cast<std::size_t>(row - game.bound_map_names_.begin()));
        return true;
    }

    /// The hooks' ask: a card's picture from the preview cache, asked for
    /// when it is not decoded.
    ///
    /// @param context the browser
    /// @param card the card
    /// @return the picture; null while it is not decoded
    static const previews::Thumbnail* ask_picture(void* context, const mb::MapCard& card) {
        auto& self = *static_cast<MapBrowserScreen*>(context);
        previews::PreviewRequest request;
        request.key = card.preview_key;
        request.map = card.name;
        switch (card.origin) {
        case mb::MapCard::Origin::game:
        case mb::MapCard::Origin::mod:
            request.source = previews::PreviewSource::base;
            break;
        case mb::MapCard::Origin::installed: {
            request.source = previews::PreviewSource::pack;
            if (const auto file = self.preview_files_.find(card.name);
                file != self.preview_files_.end())
                request.file = file->second;
            break;
        }
        case mb::MapCard::Origin::online:
            request.source = previews::PreviewSource::online;
            break;
        }
        return self.previews_.ask(request);
    }

    /// The hooks' failed: a card's picture could not be decoded, or the
    /// catalogue's picture was refused.
    ///
    /// @param context the browser
    /// @param card the card
    /// @return true when it has none
    static bool picture_failed(void* context, const mb::MapCard& card) {
        auto& self = *static_cast<MapBrowserScreen*>(context);
        if (self.previews_.failed(card.preview_key))
            return true;
        if (card.origin != mb::MapCard::Origin::online)
            return false;
        const auto registry = self.picture_registry_.find(card.preview_key);
        const LibraryHost& library = self.runtime_.library_host();
        return registry == self.picture_registry_.end() || !library.work.pictures ||
               library.work.pictures->refused(registry->second, card.preview_key);
    }

    /// The preview host's terrain reader: a range of a base map's terrain file.
    ///
    /// @param context the browser
    /// @param path the file
    /// @param offset where the range starts, in bytes
    /// @param out where it is copied
    /// @param size its bytes
    /// @return false when fewer are there
    static bool
    read_map(void* context, const char* path, uint32_t offset, void* out, uint32_t size) {
        auto& self = *static_cast<MapBrowserScreen*>(context);
        if (path == nullptr || out == nullptr)
            return false;
        try {
            return self.runtime_.assets_.read_chunk(
                path, offset, std::span<uint8_t>(static_cast<uint8_t*>(out), size)
            );
        } catch (const std::exception&) {
            return false;
        }
    }

    /// The preview host's file reader: an installed pack's preview picture.
    ///
    /// @param file the picture
    /// @param[out] out its bytes
    /// @return false when it cannot be read
    static bool read_file(void* /*context*/, const fs::path& file, std::vector<uint8_t>* out) {
        return out != nullptr && read_whole_file(file, most_preview_bytes, *out);
    }

    /// The preview host's online bytes: a catalogue picture's checked file
    /// once the Library's worker has read or fetched it; otherwise it is
    /// asked for, and the worker woken.
    ///
    /// @param context the browser
    /// @param address the picture's address
    /// @param[out] out its bytes
    /// @return false while they are not ready
    static bool online_bytes(void* context, const std::string& address, std::vector<uint8_t>* out) {
        auto& self = *static_cast<MapBrowserScreen*>(context);
        const auto registry = self.picture_registry_.find(address);
        if (registry == self.picture_registry_.end() || out == nullptr)
            return false;
        LibraryHost& library = self.runtime_.library_host();
        if (!library.work.pictures)
            return false;
        if (std::optional<std::vector<uint8_t>> bytes =
                library.work.pictures->take_bytes(registry->second, address)) {
            *out = std::move(*bytes);
            return true;
        }
        // Asked for once more: a picture taken before is read again from
        // the folder, and one refused is not asked for.
        library.work.pictures->request(registry->second, address);
        bool held = false;
        {
            base::threads::LockGuard guard(library.work.mutex);
            held = library.work.match_running;
            if (!held && library.work.pictures->waiting())
                library.work.pictures_due = true;
        }
        if (!held && library.ensure_worker())
            library.work.wake.notify_all();
        return false;
    }

    Runtime& runtime_;                          ///< the runtime whose maps it shows
    mb::Browser model_{};                       ///< the cards, the filters and the selection
    mb::ScreenState state_{};                   ///< the hover, focus, scroll and search
    kit::DisplayList list_{};                   ///< what it draws and where its controls are
    kit::Frame frame_{};                        ///< the frame it was laid out for
    bool laid_out_{};                           ///< frame_ was set by lay_out
    uint64_t revision_{};                       ///< counts the changes to what it shows
    previews::PreviewCache previews_;           ///< the cards' pictures
    previews::PreviewHost host_{};              ///< where the pictures' bytes come from
    std::optional<oa::PaletteBytes> palette_{}; ///< the game palette the base maps' pictures take
    std::optional<std::string> mod_key_{};      ///< the mod's compatible key, `<id>@<release>`
    std::map<std::string, OnlineSource> online_{}; ///< each online map's pack, by its game name
    std::map<std::string, std::string> picture_registry_{}; ///< each picture's registry, by address
    std::map<std::string, fs::path> preview_files_{}; ///< each installed map's picture, by name
    std::set<std::string> fit_checked_{};             ///< the installed maps whose fit was checked
    std::optional<uint64_t> download_item_{};         ///< the queue item of the download under way
};

// ---------------------------------------------------------------------------------------------
// MORE MAPS…

/// MORE MAPS… as a screen of the OA layer: not modal, no backdrop, the
/// kit's primary button, lime at the regular size, under the picker's map
/// list, its right edge on the list's, or
/// above the list where the place under it meets another gadget of the
/// picker. It takes a press or a tap on its button, and the controller's
/// Y and the find action, which the pickers leave unused; a plain slash
/// stays with the picker. Everything else, a pointer move that only changes
/// its hover included, goes on to the picker.
struct Runtime::MoreMapsScreen final : LayerScreen {
    /// Makes the button.
    ///
    /// @param runtime the runtime
    explicit MoreMapsScreen(Runtime& runtime) : runtime_(runtime) {}

    /// Returns "more-maps".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return more_maps_screen_name; }

    /// Returns where the button shows: its rectangle at the view's scale;
    /// nowhere while no picker shows, a message box is up, or in a match.
    ///
    /// @param placed_on what the screen is placed on
    /// @return its place
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        const std::optional<kit::Rect> place = place_of(placed_on);
        if (!place)
            return {};
        const int32_t percent = std::max(placed_on.scale_percent, int32_t{100});
        const kit::Rect shown_at = kit::to_canvas(*place, percent);
        return {
            {shown_at.x, shown_at.y, shown_at.width, shown_at.height},
            place->width,
            place->height,
            percent
        };
    }

    /// Tells that the button lets what it does not take go on.
    ///
    /// @return false
    [[nodiscard]] bool modal() const override { return false; }

    /// Tells that the button darkens nothing.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Follows the view the button is placed on.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override { view_ = placed_on; }

    /// Draws the button over its whole place.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override { kit::paint(canvas, display_list()); }

    /// Takes a press or a tap on the button, and its release; everything
    /// else goes on.
    ///
    /// A mouse presses the drawn rectangle. A finger presses by its reach
    /// when no gadget of the picker, the map list included, is nearer the
    /// point. A point inside a gadget stays that gadget's.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the button's points
    /// @param finger_reach a finger's reach; 0 for a mouse
    /// @return none or redraw for the button's own press; pass otherwise
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        const std::optional<kit::Rect> shown = place_of(view_);
        const bool inside = shown && kit::contains({0, 0, shown->width, shown->height}, at);
        if (inside != hovered_) {
            hovered_ = inside;
            ++revision_;
        }
        const bool over = kind != ScreenInputKind::pointer_move && finger_reach > 0
                              ? finger_reaches(at, finger_reach)
                              : inside;
        if (kind == ScreenInputKind::pointer_down && button == SDL_BUTTON_LEFT && over) {
            pressed_ = true;
            ++revision_;
            return LayerAnswer::redraw;
        }
        if (kind == ScreenInputKind::pointer_up && button == SDL_BUTTON_LEFT && pressed_) {
            pressed_ = false;
            ++revision_;
            if (over)
                open_browser();
            return LayerAnswer::redraw;
        }
        return LayerAnswer::pass;
    }

    /// Lets every key go on to the picker.
    ///
    /// @return pass
    LayerAnswer key(kit::Key /*pressed*/, uint32_t /*sdl_key*/) override {
        return LayerAnswer::pass;
    }

    /// Takes the find action, which the pickers leave unused, as a press on
    /// the button. A plain slash stays with the picker, and so does every
    /// other key.
    ///
    /// @return none for the find action; pass otherwise
    LayerAnswer shortcut(uint32_t /*sdl_key*/, uint16_t /*modifiers*/) override {
        const auto* find = runtime_.fired_action("ui.screen-find");
        if (find == nullptr || !place_of(view_))
            return LayerAnswer::pass;
        const bool typed_slash = find->chord.kind == oa::ui::key_bindings::TriggerKind::character &&
                                 find->chord.character == U'/' && find->chord.modifiers == 0;
        if (typed_slash)
            return LayerAnswer::pass;
        open_browser();
        return LayerAnswer::none;
    }

    /// Takes the controller's Y, which the pickers leave unused, as a press
    /// on the button; every other action goes on.
    ///
    /// @param action the gamepad action
    /// @return none for Y; pass otherwise
    LayerAnswer pad_action(oa::ui::pad_controls::Action action) override {
        if (action != oa::ui::pad_controls::Action::library_search || !place_of(view_))
            return LayerAnswer::pass;
        open_browser();
        return LayerAnswer::none;
    }

    /// Lets the wheel go on.
    ///
    /// @return pass
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::pass; }

    /// Returns the button: one kit button, named more-maps.
    ///
    /// @return the display list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        const std::optional<kit::Rect> place = place_of(view_);
        if (!place)
            return list;
        kit::ButtonLook look;
        look.caption = shown(word_more_maps);
        look.style = kit::ButtonStyle::accent;
        look.font = more_maps_font;
        look.hovered = hovered_;
        look.held = pressed_ && hovered_;
        kit::add_button(
            list,
            {0, 0, place->width, place->height},
            look,
            more_maps_control,
            std::string(more_maps_control_name)
        );
        list.tab_order.clear();
        return list;
    }

    /// Returns the button's hover and press.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown_state;
        shown_state.hovered = hovered_ ? more_maps_control : kit::no_control;
        shown_state.pressed = pressed_ ? more_maps_control : kit::no_control;
        return shown_state;
    }

    /// Closes the button once neither picker shows.
    ///
    /// @return close when no picker shows; otherwise none
    LayerAnswer tick() override {
        if (!opener())
            return LayerAnswer::close;
        return LayerAnswer::none;
    }

    /// Returns the button's revision.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Has nothing of its own to close.
    void close(bool /*by_key*/) override {}

    /// Returns the picker MORE MAPS… shows over now.
    ///
    /// @param runtime the runtime
    /// @return SELMAP or the battle room's map dialog; none while neither shows
    static std::optional<mb::Browser::Opener> picker(const Runtime& runtime) {
        if (runtime.screen_ == Screen::match || runtime.match_ != nullptr)
            return std::nullopt;
        if (MapBrowserScreen::picker_shows(runtime, mb::Browser::Opener::skirmish))
            return mb::Browser::Opener::skirmish;
        if (MapBrowserScreen::picker_shows(runtime, mb::Browser::Opener::battle_room))
            return mb::Browser::Opener::battle_room;
        return std::nullopt;
    }

  private:

    /// Returns the picker MORE MAPS… shows over now.
    ///
    /// @return the picker; none while none shows
    [[nodiscard]] std::optional<mb::Browser::Opener> opener() const { return picker(runtime_); }

    /// Tells whether a finger's reach lands on the button.
    ///
    /// A gadget nearer the point, or one the point lies inside, keeps it.
    /// The hover stays the drawn rectangle.
    ///
    /// @param at the finger, in the button's points
    /// @param finger_reach how far the finger reaches, in points
    /// @return true when the finger presses the button
    [[nodiscard]] bool finger_reaches(kit::Point at, int32_t finger_reach) const {
        const std::optional<kit::Rect> shown = place_of(view_);
        if (!shown || finger_reach <= 0 || shown->width <= 0 || shown->height <= 0)
            return false;
        const place::PlaceRect button{0, 0, shown->width, shown->height};
        std::vector<place::PlaceRect> gadgets;
        kit::Rect list_rect{};
        std::vector<kit::Rect> others;
        if (picker_rects(list_rect, others)) {
            const auto add = [&](const kit::Rect& frame) {
                const kit::Rect points = frame_to_points(view_, frame);
                if (points.width <= 0 || points.height <= 0)
                    return;
                gadgets.push_back(
                    {points.x - shown->x, points.y - shown->y, points.width, points.height}
                );
            };
            add(list_rect);
            for (const kit::Rect& other : others)
                add(other);
        }
        if (place::nearer_gadget(at.x, at.y, button, gadgets))
            return false;
        const kit::DisplayList list = display_list();
        kit::Interaction trial = interaction();
        return kit::finger_down(trial, list, at, finger_reach).control == more_maps_control;
    }

    /// Returns the picker's map list and the other gadgets of its panel, in
    /// the 640 by 480 frame.
    ///
    /// @param list_rect[out] the map list
    /// @param others[out] every other gadget of the panel
    /// @return false while no picker shows
    bool picker_rects(kit::Rect& list_rect, std::vector<kit::Rect>& others) const {
        const std::optional<mb::Browser::Opener> shown_over = opener();
        if (!shown_over)
            return false;
        if (*shown_over == mb::Browser::Opener::battle_room) {
            int32_t x = 0;
            int32_t y = 0;
            int32_t width = 0;
            int32_t height = 0;
            if (!mp::multiplayer_map_list_rect(&x, &y, &width, &height))
                return false;
            list_rect = {x, y, width, height};
            const mp::Panel* panel = mp::multiplayer_modal();
            if (panel == nullptr)
                return true;
            const mp::Control* listed = mp::panel_control(*panel, "MAPNAMES");
            const int32_t shift_x = listed != nullptr ? x - listed->x : 0;
            const int32_t shift_y = listed != nullptr ? y - listed->y : 0;
            for (int32_t index = 1; index < panel->count; ++index) {
                const mp::Control& control = panel->controls[static_cast<std::size_t>(index)];
                if (&control == listed || control.width <= 0 || control.height <= 0)
                    continue;
                others.push_back(
                    {control.x + shift_x, control.y + shift_y, control.width, control.height}
                );
            }
            return true;
        }
        const auto& gadgets = runtime_.resources_.layout.gadgets;
        if (gadgets.empty())
            return false;
        // The map picker is centred over the skirmish setup.
        const auto& root = gadgets.front().common;
        const auto origin = runtime_.panel_origin();
        const int32_t left = origin.x + (kCanvasWidth - static_cast<int32_t>(root.width)) / 2;
        const int32_t top = origin.y + (kCanvasHeight - static_cast<int32_t>(root.height)) / 2;
        bool found = false;
        for (std::size_t index = 1; index < gadgets.size(); ++index) {
            const auto& common = gadgets[index].common;
            const kit::Rect place{
                left + common.x,
                top + common.y,
                static_cast<int32_t>(common.width),
                static_cast<int32_t>(common.height)
            };
            if (place.width <= 0 || place.height <= 0)
                continue;
            if (!found && tdf_names_equal(common.name, "MAPNAMES")) {
                list_rect = place;
                found = true;
                continue;
            }
            others.push_back(place);
        }
        return found;
    }

    /// Returns where the button lies, in the layer's points: under the map
    /// list's frame, its right edge on the list's, or above the frame when
    /// the place under it meets another gadget of the picker. The button
    /// keeps its size.
    ///
    /// @param placed_on what the screen is placed on
    /// @return the place; none while it does not show
    [[nodiscard]] std::optional<kit::Rect> place_of(const LayerView& placed_on) const {
        if (placed_on.match || oa::ui::frontend_dialogs::dialog_count() != 0)
            return std::nullopt;
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        kit::Rect list_rect{};
        std::vector<kit::Rect> others;
        if (fonts == nullptr || !picker_rects(list_rect, others))
            return std::nullopt;
        const kit::Metrics& metrics = more_maps_metrics;
        const int32_t width = std::max(
            metrics.button_width,
            kit::text_width(*fonts, more_maps_font, shown(word_more_maps)) +
                metrics.prompt_button_padding
        );
        const kit::Rect list_points = frame_to_points(placed_on, list_rect);
        kit::Rect framed = list_rect;
        framed.x -= list_frame_outset;
        framed.y -= list_frame_outset;
        framed.width += list_frame_outset * 2;
        framed.height += list_frame_outset * 2;
        const kit::Rect frame_points = frame_to_points(placed_on, framed);
        const int32_t percent = std::max(placed_on.scale_percent, int32_t{100});
        // The top is the first point that draws on or below the frame's outer
        // edge. The picture's scale rounds that edge outward, and a point
        // just inside it draws on the frame.
        kit::Rect place{
            list_points.x + list_points.width - width,
            point_on_or_below(frame_window_bottom(placed_on, framed), percent),
            width,
            metrics.control_height
        };
        const bool blocked = std::any_of(others.begin(), others.end(), [&](const kit::Rect& other) {
            return meet(place, frame_to_points(placed_on, other));
        });
        if (blocked)
            place.y = frame_points.y - metrics.control_height;
        return place;
    }

    /// Opens the browser over the picker.
    void open_browser() {
        if (const std::optional<mb::Browser::Opener> shown_over = opener())
            MapBrowserScreen::open(runtime_, *shown_over);
    }

    Runtime& runtime_;    ///< the runtime whose pickers it shows over
    LayerView view_{};    ///< the view it was last laid out for
    bool hovered_{};      ///< the pointer is over the button
    bool pressed_{};      ///< a press on the button is held
    uint64_t revision_{}; ///< counts the changes to how it looks
};

void Runtime::keep_more_maps_screen() {
    OaLayer& layer = oa_layer();
    if (!MoreMapsScreen::picker(*this) || layer.find(more_maps_screen_name) != nullptr ||
        layer.screen_fonts() == nullptr)
        return;
    layer.push(std::make_unique<MoreMapsScreen>(*this));
}

} // namespace oa::app
