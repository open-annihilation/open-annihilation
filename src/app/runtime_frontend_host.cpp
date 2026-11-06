// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Frontend dispatcher, preferences, map list and main-menu host services.
#include "oa/app/runtime.hpp"
#include "device_state.hpp"
#include "map_packs.hpp"
#include "oa/app/mod_profile_loader.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/ui/decoded.hpp"
#include "oa/ui/map_browser/model.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/view_rules.hpp"
#include "oa/data/campaign/campaign_assets.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/data/campaign/map_catalog.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/platform/system.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

namespace oa::app {
namespace {

/// True when `text` begins with `prefix`, ignoring ASCII case.
bool starts_with_nocase(const char* text, const char* prefix) {
    if (text == nullptr)
        return false;
    for (; *prefix != '\0'; ++text, ++prefix) {
        if (*text == '\0')
            return false;
        const auto left = std::tolower(static_cast<unsigned char>(*text));
        const auto right = std::tolower(static_cast<unsigned char>(*prefix));
        if (left != right)
            return false;
    }
    return true;
}

/// True when `type` is a network schema, Network 1 through Network 4.
bool network_schema(const char* type) {
    static constexpr const char* kTypes[] = {"Network 1", "Network 2", "Network 3", "Network 4"};
    if (type == nullptr)
        return false;
    for (const char* known : kTypes)
        if (oa::formats::tdf::compare_nocase(type, known) == 0)
            return true;
    return false;
}

/// Start positions of one schema: the specials whose kind begins with StartPos.
int32_t start_positions(const oa::formats::tdf::Block* schema) {
    const auto* specials = oa::formats::tdf::find_child(schema, "specials");
    int32_t count = 0;
    for (uint32_t index = 0; index < oa::formats::tdf::child_count(specials); ++index) {
        const char* what = oa::formats::tdf::find_value(
            oa::formats::tdf::child_at(specials, index), "specialwhat"
        );
        if (starts_with_nocase(what, "StartPos"))
            ++count;
    }
    return count;
}

} // namespace

[[nodiscard]] std::size_t Runtime::map_visible_rows() {
    const auto* list = widget("MAPNAMES");
    if (list == nullptr)
        return 1;
    auto item_height =
        static_cast<std::size_t>(oa::formats::fnt::line_height(resources_.font)) + 1U;
    if (const auto* fields = std::get_if<oa::ui::gui_layout::ListBoxFields>(&list->fields);
        fields != nullptr && fields->item_height > 0)
        item_height = static_cast<std::size_t>(fields->item_height);
    return std::max<std::size_t>(1, static_cast<std::size_t>(list->common.height) / item_height);
}

[[nodiscard]] std::size_t Runtime::map_first_visible() {
    if (const auto first = frontend_list_first("MAPNAMES"))
        return *first;
    const auto selected = static_cast<std::size_t>(std::max<int16_t>(0, modal_map_index_));
    const auto rows = map_visible_rows();
    return selected >= rows ? selected - rows + 1U : 0U;
}

void Runtime::preview_map_index(std::size_t index) {
    if (index >= bound_map_names_.size())
        return;
    modal_map_index_ = static_cast<int16_t>(index);
    select_frontend_list_row("MAPNAMES", index);
    map_modal::preview_selection(map_modal_, *this);
    rebuild_surface();
}

bool Runtime::step_map_list(bool forward) {
    const auto focus = frontend_focus();
    if (focus < 0 || bound_map_names_.empty() ||
        !tdf_names_equal(
            resources_.layout.gadgets[static_cast<std::size_t>(focus)].common.name, "MAPNAMES"
        ))
        return false;
    if (const auto row = step_frontend_list_row("MAPNAMES", forward);
        row && *row < bound_map_names_.size())
        modal_map_index_ = static_cast<int16_t>(*row);
    map_modal::preview_selection(map_modal_, *this);
    rebuild_surface();
    return true;
}

void Runtime::select_map_row_at(float canvas_y) {
    const auto* list = widget("MAPNAMES");
    if (list == nullptr)
        return;
    if (frontend_list_first("MAPNAMES")) {
        if (const auto row = frontend_list_row_at("MAPNAMES", canvas_y))
            preview_map_index(*row);
        return;
    }
    const auto modal_offset_y =
        (kCanvasHeight - static_cast<int>(resources_.layout.gadgets.front().common.height)) / 2;
    auto item_height =
        static_cast<std::size_t>(oa::formats::fnt::line_height(resources_.font)) + 1U;
    if (const auto* fields = std::get_if<oa::ui::gui_layout::ListBoxFields>(&list->fields);
        fields != nullptr && fields->item_height > 0)
        item_height = static_cast<std::size_t>(fields->item_height);
    const auto local_y = static_cast<int32_t>(canvas_y) - modal_offset_y - list->common.y - 2;
    if (local_y < 0)
        return;
    preview_map_index(map_first_visible() + static_cast<std::size_t>(local_y) / item_height);
}

void Runtime::close_map_modal() {
    if (!map_modal_.active)
        return;
    map_modal::handle_event(
        map_modal_,
        skirmish_settings_,
        entry::Event{{kFrontendMenuHandle}, menu::destroy_event},
        *this
    );
    skirmish::setup(state_, skirmish_settings_, preferences_, skirmish_ui_, *this);
}

void Runtime::step(frontend::Step step_id, frontend::State&) {
    if (const auto* handler = step_find(&screens_, step_id)) {
        auto context = screen_context();
        handler->run(&context, handler->state);
        return;
    }
    status_ = "dispatcher reached unimplemented service " +
              std::to_string(static_cast<uint32_t>(step_id));
}

uint32_t Runtime::query(frontend::Query query, frontend::State&) {
    const auto* handler = query_find(&screens_, query);
    if (handler == nullptr)
        return 0;
    auto context = screen_context();
    return handler->run(&context, handler->state);
}

void Runtime::play_movie(frontend::State&, std::string_view filename) {
    // The mod profile names each movie's file (media.movies); an empty name
    // plays none, and the frontend goes straight on to what follows.
    if (options_.check_frontend_controls)
        check_frontend_movie_ = std::string(filename);
    const auto file = movie_file_of(mod_profile(), filename);
    if (!file.empty())
        play_movie_resource(file);
}

void Runtime::set_cursor_visible(frontend::State&, int32_t visible) {
    oa::present::set_cursor_overlay_visible(visible);
}

void Runtime::select_map_list(frontend::State&, int32_t selector_value) {
    init::select_map_list(map_list_state_, selector_value, *this);
}

void Runtime::open_new_game_panel(frontend::State&, int32_t value) {
    new_game_selection_ = value;
    if (state_.signal == frontend::signal_id::any_mission ||
        state_.pending_signal == frontend::signal_id::any_mission)
        load(Screen::any_mission);
    else
        load(Screen::new_campaign);
}

void Runtime::set_app_mode(frontend::State&, int32_t mode) {
    frontend_mode_ = mode;
    frontend_game().mode = mode;
    // The extension hears of every mode set, the same one again included.
    call_hook_or_report<&Extension::app_mode_set>(extension_, hook_error_report(), *this, mode);
}

oa::Game& Runtime::frontend_game() {
    if (extension_.frontend_game != nullptr)
        return *call_hook_or_raise<&Extension::frontend_game>(extension_);
    return *frontend_game_;
}

void Runtime::set_cursor(frontend::State&, int32_t index) {
    select_cursor_animation(static_cast<uint32_t>(index));
}

void Runtime::shut_down(frontend::State&) {
    quit_application(nullptr);
}

std::string Runtime::preference_key(std::string_view section, std::string_view key) const {
    const auto prefix =
        options_.mod_profile ? registry_key_prefix(*options_.mod_profile) : std::string();
    return prefix + std::string(section) + '|' + std::string(key);
}

void Runtime::load_preference_file() {
    preference_path_ = preference_file(options_.preferences_file);
    load_preference_values();
    // A mod's first run finds the registry values its installer would have
    // written; they are saved with the next change.
    if (options_.mod_profile && seed_registry(*options_.mod_profile, preference_values_))
        preferences_dirty_ = true;
}

void Runtime::load_preference_values() {
    if (std::filesystem::exists(preference_path_)) {
        preference_values_ = oa::platform::preferences::load(preference_path_);
        return;
    }
    // Explicit profiles start with defaults, never import installation or
    // personal settings. This keeps isolated test runs reproducible.
    if (options_.preferences_file)
        return;
    // Read-only migration of the early prototype's installation-local
    // settings. Future reads/writes use the platform location exclusively.
    const auto legacy = options_.game_dir / "open-annihilation.ini";
    if (!std::filesystem::is_regular_file(legacy))
        return;
    constexpr uintmax_t maximum_legacy_bytes = 1024U * 1024U;
    const auto size = std::filesystem::file_size(legacy);
    if (size > maximum_legacy_bytes)
        throw std::runtime_error("legacy preferences exceed size limit");
    std::ifstream input(legacy, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read legacy preferences");
    std::string data(static_cast<std::size_t>(size), '\0');
    if (!data.empty() && !input.read(data.data(), static_cast<std::streamsize>(data.size())))
        throw std::runtime_error("cannot finish reading legacy preferences");
    if (input.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("legacy preferences grew beyond size limit");
    if (input.bad())
        throw std::runtime_error("cannot finish reading legacy preferences");
    std::size_t at = 0;
    while (at < data.size()) {
        const auto end = data.find('\n', at);
        auto line = std::string_view(data).substr(
            at, end == std::string::npos ? data.size() - at : end - at
        );
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        const auto equals = line.find('=');
        if (equals != std::string_view::npos && equals != 0)
            preference_values_[std::string(line.substr(0, equals))] =
                std::string(line.substr(equals + 1));
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    oa::platform::preferences::save(preference_path_, preference_values_);
}

void Runtime::flush_preferences() {
    if (!preferences_dirty_)
        return;
    oa::platform::preferences::save(preference_path_, preference_values_);
    preferences_dirty_ = false;
}

std::map<std::string, std::string>::const_iterator
Runtime::find_preference(std::string_view section, std::string_view key) const {
    const auto wanted = preference_key(section, key);
    const auto found = preference_values_.find(wanted);
    if (found != preference_values_.end() || !options_.mod_profile)
        return found;
    // Registry value names are matched without case, so a value a mod's
    // registry seeds spelled otherwise is still found.
    const auto same = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    };
    for (auto entry = preference_values_.begin(); entry != preference_values_.end(); ++entry)
        if (entry->first.size() == wanted.size() &&
            std::equal(entry->first.begin(), entry->first.end(), wanted.begin(), same))
            return entry;
    return preference_values_.end();
}

std::optional<uint32_t> Runtime::read_number(std::string_view section, std::string_view key) {
    const auto found = find_preference(section, key);
    if (found == preference_values_.end())
        return std::nullopt;
    uint32_t value = 0;
    const auto parsed =
        std::from_chars(found->second.data(), found->second.data() + found->second.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != found->second.data() + found->second.size())
        return std::nullopt;
    return value;
}

void Runtime::write_number(std::string_view section, std::string_view key, uint32_t value) {
    preference_values_[preference_key(section, key)] = std::to_string(value);
    preferences_dirty_ = true;
}

std::optional<std::string>
Runtime::read_string(std::string_view section, std::string_view key, std::size_t capacity) {
    const auto found = find_preference(section, key);
    if (found == preference_values_.end() || found->second.size() >= capacity)
        return std::nullopt;
    return found->second;
}

void Runtime::write_string(std::string_view section, std::string_view key, std::string_view value) {
    preference_values_[preference_key(section, key)] = value;
    preferences_dirty_ = true;
}

void Runtime::audio_mode(init::AudioMode mode) {
    if (mode != init::AudioMode::unchanged)
        sound_spatial_ = mode == init::AudioMode::spatial ? 1 : 0;
}

void Runtime::mixing_buffers(uint32_t value) {
    mixing_buffers_ = value;
}

void Runtime::wave_volume(uint32_t value) {
    wave_volume_ = value;
    audio_player_.set_volume(wave_volume_, preferences_.fx_volume);
}

void Runtime::cd_volume(uint32_t value) {
    cd_volume_ = value;
}

uint32_t Runtime::nickname_override_enabled() {
    // The first of the preferences load's three overrides asks the extension
    // once for the load; the values are copied at once.
    FrontendEntry entry{};
    call_hook_or_raise<&Extension::frontend_entry>(extension_, entry);
    entry_nickname_ = entry.nickname != nullptr ? entry.nickname : "";
    entry_game_name_ = entry.game_name != nullptr ? entry.game_name : "";
    return entry_nickname_.empty() ? 0 : 1;
}

std::string Runtime::nickname_override() {
    return entry_nickname_;
}

std::string Runtime::game_name_override() {
    return entry_game_name_;
}

std::optional<std::string> Runtime::user_name() {
    for (const char* variable : {"USER", "USERNAME"})
        if (auto name = oa::platform::environment_value(variable); name && !name->empty())
            return name;
    return std::nullopt;
}

std::string Runtime::application_directory() {
    return path_to_utf8(options_.game_dir);
}

void Runtime::select_map_list(int32_t selector_value) {
    init::select_map_list(map_list_state_, selector_value, *this);
}

void Runtime::select_map_index(int32_t mode) {
    if (map_list_mode_ != 0 && mode == 0) {
        eligible_map_names_.clear();
        first_map_name_.clear();
    }
    if (eligible_map_names_.empty())
        discover_first_map();
    map_list_mode_ = mode;
    if (first_map_name_.empty())
        return;
    if (select_map(first_map_name_) != 0)
        return;
    // A map the list holds may still not be playable: a mod can ship a
    // map's OTA without its terrain, such as its settings for a downloadable
    // map the game does not have. The default is then the first listed map
    // that can be selected, and only a list with none stops the game.
    for (const std::string& name : eligible_map_names_) {
        if (name != first_map_name_ && select_map(name) != 0) {
            first_map_name_ = name;
            return;
        }
    }
    throw std::runtime_error("first eligible skirmish map cannot be selected");
}

std::string Runtime::selected_map_name() {
    return first_map_name_;
}

uint32_t Runtime::mixing_buffer_count() {
    return mixing_buffers_;
}

uint32_t Runtime::wave_out_volume() {
    return wave_volume_;
}

uint32_t Runtime::cd_audio_volume() {
    return cd_volume_;
}

uint8_t Runtime::keep_stored_password() {
    return call_hook_or_raise<&Extension::keep_stored_password>(extension_) ? 1 : 0;
}

int32_t Runtime::selector(init::MapListHandle handle) {
    const auto found = map_list_objects_.find(handle.value);
    return found == map_list_objects_.end() ? -1 : found->second;
}

void Runtime::destroy(init::MapListHandle) {
}

void Runtime::release(init::MapListHandle handle) {
    map_list_objects_.erase(handle.value);
}

init::MapListHandle Runtime::allocate() {
    return {++next_map_list_handle_};
}

init::MapListHandle Runtime::construct(init::MapListHandle handle, int32_t selector_value) {
    map_list_objects_[handle.value] = selector_value;
    return handle;
}

void Runtime::remember_base_map(const char* name, const oa::formats::tdf::Document* ota) {
    if (name == nullptr || name[0] == '\0')
        return;
    BaseMapSummary summary;
    summary.name = name;
    const auto* header =
        ota != nullptr ? oa::formats::tdf::find_child(ota->root, "GlobalHeader") : nullptr;
    if (const char* title = oa::formats::tdf::find_value(header, "missionname"))
        summary.title = title;
    if (const char* description = oa::formats::tdf::find_value(header, "missiondescription"))
        summary.description = description;
    if (const char* size = oa::formats::tdf::find_value(header, "size"))
        summary.size = size;
    // A map that names its counts is not measured by its start positions.
    const char* listed = oa::formats::tdf::find_value(header, "numplayers");
    const oa::ui::map_browser::BaseMapPlayers named = oa::ui::map_browser::base_map_players(
        listed != nullptr ? std::string_view{listed} : std::string_view{}, 0
    );
    if (named.from_numplayers) {
        summary.player_counts = named.counts;
        summary.players = named.players;
    } else {
        int32_t starts = 0;
        for (uint32_t index = 0; index < oa::formats::tdf::child_count(header); ++index) {
            const auto* schema = oa::formats::tdf::child_at(header, index);
            if (!network_schema(oa::formats::tdf::find_value(schema, "type")))
                continue;
            starts = std::max(starts, start_positions(schema));
        }
        const oa::ui::map_browser::BaseMapPlayers kept =
            oa::ui::map_browser::base_map_players({}, starts);
        summary.player_counts = kept.counts;
        summary.players = kept.players;
    }
    summary.memory_mb = oa::ui::frontend_multiplayer::map_memory_mb(
        static_cast<int32_t>(assets_.file_size("maps/" + summary.name + ".tnt"))
    );
    base_map_summaries_.push_back(std::move(summary));
}

void Runtime::discover_first_map() {
    // The list keeps the maps' file names, which the screens find the maps
    // by; a chosen map shows its translated name (campaign_localized_name).
    // The same scan records each base map's title, description, size and memory.
    base_map_summaries_.clear();
    auto files = oa::data::campaign::campaign_asset_files(assets_);
    files.translate = nullptr;
    const oa::data::campaign::MapScanHost host{
        this,
        [](void* runtime, uint32_t animation) {
            static_cast<Runtime*>(runtime)->select_cursor_animation(animation);
        },
        [](void* runtime, const char* name, const oa::formats::tdf::Document* ota) {
            static_cast<Runtime*>(runtime)->remember_base_map(name, ota);
        }
    };
    oa::data::campaign::MapList list{};
    char* names = nullptr;
    const auto count =
        oa::data::campaign::map_build_multiplayer_list(list, files, host, &names, false, false);
    const char* name = names;
    for (int32_t index = 0; name != nullptr && index < count;
         ++index, name += std::strlen(name) + 1)
        eligible_map_names_.emplace_back(name);
    if (names == nullptr)
        base_map_summaries_.clear();
    std::free(names);
    oa::data::campaign::map_clear_list_cache(list, nullptr);
    // The scan finds a mounted pack map's OTA too, under its own name; it is
    // listed with the other pack maps, so that every base map comes first.
    std::erase_if(eligible_map_names_, [this](const std::string& listed) {
        return pack_map(listed) != nullptr;
    });
    std::erase_if(base_map_summaries_, [this](const BaseMapSummary& listed) {
        return pack_map(listed.name) != nullptr;
    });
    // A map whose terrain no file holds cannot be played, and is left out. A
    // mod may ship such an OTA as its settings for a map the game does not
    // have; those are kept for a pack map of the same file name.
    const auto terrain_missing = [this](const std::string& name) {
        return assets_.provider("maps/" + name + ".tnt").kind == oa::ProviderKind::none;
    };
    mod_map_settings_.clear();
    std::string missing_names;
    for (const BaseMapSummary& summary : base_map_summaries_) {
        if (!terrain_missing(summary.name))
            continue;
        missing_names.append(missing_names.empty() ? "" : ", ").append(summary.name);
        if (mod_map_settings(summary.name) != nullptr)
            mod_map_settings_.push_back(summary);
    }
    std::erase_if(eligible_map_names_, terrain_missing);
    std::erase_if(base_map_summaries_, [&](const BaseMapSummary& listed) {
        return terrain_missing(listed.name);
    });
    if (!missing_names.empty())
        std::cerr << "open-annihilation: map list: left out, their terrain is missing: "
                  << missing_names << '\n';
    if (!eligible_map_names_.empty())
        first_map_name_ = eligible_map_names_.front();
    // Every installed pack map joins after the base maps. One that cannot
    // be played is listed greyed. None of a pack map's files is opened here
    // for a registry pack whose catalogue entry is already known.
    refresh_listed_pack_maps();
}

void Runtime::save_preferences() {
    init::save_preferences(
        state_,
        player_skirmish_settings_ ? *player_skirmish_settings_ : skirmish_settings_,
        preferences_,
        *this
    );
    flush_preferences();
}

void Runtime::keep_player_skirmish_settings() {
    init::remember_player_skirmish_settings(skirmish_settings_, player_skirmish_settings_);
}

void Runtime::release_player_skirmish_settings() {
    init::restore_player_skirmish_settings(skirmish_settings_, player_skirmish_settings_);
}

// MAINMENU callback boundary.
menu::Environment& Runtime::environment() {
    return environment_;
}

void Runtime::release_sparks() {
    menu_sparks_ = {};
}

uint32_t Runtime::button_result(menu::MenuHandle, menu::Button button) {
    // The credits open from the gadget the profile names
    // (strings.gadget.credits); a name no gadget has opens nothing.
    const std::string_view name = button == menu::Button::credits
                                      ? view_rules::credits_gadget(mod_profile())
                                      : menu::resource_name(button);
    return oa::ui::gui_input::button_result(input_menu(), name);
}

void Runtime::play_sound(menu::Sound sound, uint32_t) {
    play_menu_sound(sound);
}

void Runtime::select_cursor_animation(uint32_t index) {
    select_game_cursor(static_cast<uint8_t>(index));
}

void Runtime::prepare_multiplayer() {
    status_ = "Multiplayer preparation is not implemented here.";
}

std::string Runtime::resolve_resource(menu::ResourceRequest request) {
    return std::string(request.directory) + '/' + std::string(request.name) + '.' +
           std::string(request.extension);
}

menu::DocumentHandle Runtime::construct_document() {
    return {++next_document_};
}

uint32_t Runtime::load_document(menu::DocumentHandle handle, std::string_view path) {
    if (handle.value == 0 || handle.value != next_document_)
        return 0;
    try {
        const auto document = assets_.read(path);
        return document.bytes.empty() ? 0U : 1U;
    } catch (const std::exception&) {
        return 0;
    }
}

void Runtime::destroy_document(menu::DocumentHandle) noexcept {
}

void Runtime::reset_after_multiplayer_selection() {
    selected_ = -1;
}

uint8_t Runtime::application_flags() {
    return state_.video_context_flags;
}

namespace {

// The game discs' archives, which the game's own installation holds.
constexpr const char* campaign_disc_archive = "totala2.hpi";
constexpr const char* game_disc_archive = "totala1.hpi";

} // namespace

bool Runtime::holds_disc_archive() const {
    const auto regular = [this](const char* name) {
        const auto found = game_path(name);
        std::error_code error;
        return found && fs::is_regular_file(*found, error);
    };
    return regular(campaign_disc_archive) || regular(game_disc_archive);
}

std::optional<fs::path> Runtime::game_path(std::string_view relative) const {
    const auto lowered = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return text;
    };
    for (const auto& root : assets_.loose_roots()) {
        fs::path at = root;
        bool found = true;
        for (std::string_view rest = relative; found && !rest.empty();) {
            const auto end = rest.find('/');
            const auto part = lowered(std::string(rest.substr(0, end)));
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
            found = false;
            std::error_code error;
            for (fs::directory_iterator entry{at, error}, last; !error && entry != last;
                 entry.increment(error))
                if (lowered(path_to_utf8(entry->path().filename())) == part) {
                    at = entry->path();
                    found = true;
                    break;
                }
        }
        if (found)
            return at;
    }
    return std::nullopt;
}

uint32_t Runtime::find_disc(menu::Disc disc) {
    // A mod that skips the disc check always finds its disc.
    if (options_.mod_profile && !options_.mod_profile->layout.cd_check)
        return 1;
    if (!holds_disc_archive())
        return 1;
    const auto archive = disc == menu::Disc::campaign ? campaign_disc_archive : game_disc_archive;
    const auto found = game_path(archive);
    std::error_code error;
    return found && fs::is_regular_file(*found, error) ? 1U : 0U;
}

int16_t Runtime::shift_key_state() {
    return device_state::key_held(SDL_SCANCODE_LSHIFT) ||
                   device_state::key_held(SDL_SCANCODE_RSHIFT)
               ? static_cast<int16_t>(-1)
               : 0;
}

void Runtime::drain_input() {
    // The renderer's events are handled, not dropped: a texture that a
    // reset took must be made again.
    constexpr int render_event_batch = 8;
    std::array<SDL_Event, render_event_batch> render_events{};
    for (;;) {
        const int taken = SDL_PeepEvents(
            render_events.data(),
            render_event_batch,
            SDL_GETEVENT,
            SDL_EVENT_RENDER_TARGETS_RESET,
            SDL_EVENT_RENDER_DEVICE_LOST
        );
        for (int index = 0; index < taken; ++index)
            std::ignore = take_render_event(render_events[static_cast<std::size_t>(index)]);
        if (taken < render_event_batch)
            break;
    }
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    // The fingers whose lifts were dropped here are let go, and so are the
    // gamepads' held buttons.
    touch_screen_changed();
    pad_screen_changed();
    // The window events dropped here may have changed whether the pointer is
    // kept on the screen; the window's own state settles it.
    keep_pointer_on_screen(sdl_.window);
}

void Runtime::check_frontend_integrity() {
    // The engine keeps no image checksum to test.
}

void Runtime::show_message(menu::MessageTarget, menu::Message message) {
    show_frontend_message(
        menu::message_text(message),
        entry::localized_message_width,
        entry::message_show_ok,
        entry::message_fit_width
    );
}

void Runtime::default_event(menu::MenuHandle) {
    selected_ = -1;
}

// SINGLE.GUI callback boundary.
void Runtime::play_sound(entry::Sound sound, uint32_t) {
    play_named_sound(sound);
}

void Runtime::refresh_disc_archives() {
}

std::string Runtime::translate(entry::Message message) {
    return translate_ui(entry::message_text(message));
}

void Runtime::show_frontend_message(
    std::string_view text, int32_t width, int32_t show_ok, int32_t fit_width
) {
    auto context = screen_context();
    if (!oa::ui::frontend_dialogs::open_message_box(&context, text, width, show_ok, fit_width))
        show_unsupported(text);
}

void Runtime::open_help() {
    auto context = screen_context();
    if (!oa::ui::frontend_dialogs::open_help(&context))
        status_ = "HELP.GUI is unavailable";
}

void Runtime::show_cd_check() {
    auto context = screen_context();
    if (!oa::ui::frontend_dialogs::open_cd_check(&context))
        status_ = "CDCHECK.GUI is unavailable";
}

void Runtime::clear_event_selection(entry::MenuHandle) {
    selected_ = -1;
}

void Runtime::clear_frontend_selection() {
    selected_ = -1;
}

uint32_t Runtime::button_result(entry::MenuHandle, entry::Button button) {
    return oa::ui::gui_input::button_result(input_menu(), entry::resource_name(button));
}

int32_t Runtime::load_named_background(const char* name, bool redraw, bool apply, bool defer) {
    const auto files = oa::data::campaign::campaign_asset_files(assets_);
    oa::ui::frontend::ResourceHost host{};
    host.context = this;
    host.load_bitmap = [](void* context, const char* path, uint8_t* palette) -> oa_ref32 {
        auto& runtime = *static_cast<Runtime*>(context);
        std::unique_ptr<Image> image;
        try {
            image = std::make_unique<Image>(
                oa::ui::decoded::require(oa::decode_pcx(runtime.assets_.read(path).bytes), path)
            );
        } catch (const std::exception&) {
            return 0;
        }
        runtime.caption_bitmap(path, *image);
        if (image->palette)
            std::memcpy(palette, image->palette->data(), oa::ui::frontend::kResourcePaletteBytes);
        auto& bitmaps = runtime.named_backgrounds_.bitmaps;
        auto free = std::find(bitmaps.begin(), bitmaps.end(), nullptr);
        if (free == bitmaps.end())
            free = bitmaps.insert(bitmaps.end(), nullptr);
        *free = std::move(image);
        return static_cast<oa_ref32>(free - bitmaps.begin() + 1);
    };
    host.free_bitmap = [](void* context, oa_ref32 bitmap) {
        auto& bitmaps = static_cast<Runtime*>(context)->named_backgrounds_.bitmaps;
        if (bitmap != 0 && bitmap <= bitmaps.size())
            bitmaps[bitmap - 1].reset();
    };
    host.fatal = [](void*, const char* path) {
        throw std::runtime_error(std::string("Unable to load ") + path);
    };
    host.panel_open = [](void* context) {
        return !static_cast<Runtime*>(context)->resources_.layout.gadgets.empty();
    };
    host.set_backdrop = [](void* context, oa_ref32 bitmap) {
        auto& runtime = *static_cast<Runtime*>(context);
        const auto& bitmaps = runtime.named_backgrounds_.bitmaps;
        runtime.resources_.background =
            bitmap != 0 && bitmap <= bitmaps.size() && bitmaps[bitmap - 1] ? *bitmaps[bitmap - 1]
                                                                           : Image{};
    };
    host.apply_palette = [](void* context, const uint8_t* palette) {
        PaletteBytes entries{};
        std::memcpy(entries.data(), palette, entries.size());
        static_cast<Runtime*>(context)->resources_.background.palette = entries;
    };
    host.files = &files;
    return oa::ui::frontend::load_resource_palette(
        &named_backgrounds_.cache, &frontend_game(), host, name, redraw, apply, defer
    );
}

void Runtime::open_load_game() {
    options_parent_ = screen_ == Screen::match ? Screen::match : Screen::single_player;
    load(Screen::load_game);
}

void Runtime::open_options() {
    if (screen_ != Screen::match)
        options_parent_ = screen_;
    enter_options_panel();
}

} // namespace oa::app
