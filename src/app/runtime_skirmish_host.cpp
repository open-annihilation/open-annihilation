// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Skirmish menu and map-selection modal host services.
#include "oa/app/runtime.hpp"
#include "map_packs.hpp"
#include "map_picture_state.hpp"
#include "match_models.hpp"
#include "oa/data/map_pack/map_name.hpp"
#include "oa/present/colour_vision/colour_vision.hpp"
#include "oa/present/gaf_sprites.hpp"
#include "oa/present/surface.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

std::string Runtime::pack_map_shown_name(std::string_view key) {
    const auto split = oa::data::map_pack::split_pack_map_name(key);
    if (!split)
        return {};
    std::string plain(split->stem);
    std::string mark(split->id);
    if (const PackMap* map = pack_map(key)) {
        if (const std::string_view title = pack_map_title(*map); !title.empty())
            plain = title;
        if (!map->pack_name.empty())
            mark = map->pack_name;
    }
    const std::vector<std::string> names = copy_map_names();
    bool shared = false;
    for (const std::string& other : names) {
        if (other == key)
            continue;
        std::string other_plain = other;
        if (const auto other_split = oa::data::map_pack::split_pack_map_name(other)) {
            other_plain.assign(other_split->stem);
            if (const PackMap* other_map = pack_map(other);
                other_map != nullptr && !pack_map_title(*other_map).empty())
                other_plain = pack_map_title(*other_map);
        }
        if (other_plain == plain) {
            shared = true;
            break;
        }
    }
    if (!shared || mark.empty())
        return plain;
    return plain + " (" + mark + ")";
}

oa::ui::gui_layout::Gadget* Runtime::widget(std::string_view name) {
    const auto found = std::find_if(
        resources_.layout.gadgets.begin(),
        resources_.layout.gadgets.end(),
        [name](const auto& gadget) { return gadget.common.name == name; }
    );
    return found == resources_.layout.gadgets.end() ? nullptr : &*found;
}

skirmish::MenuHandle Runtime::frontend_menu() {
    return {kFrontendMenuHandle};
}

void Runtime::clear_backbuffer() {
}

skirmish::MenuHandle Runtime::load_menu(std::string_view name) {
    if (name != "SKIRMISH.GUI")
        throw std::runtime_error("unexpected skirmish layout");
    load(Screen::skirmish);
    return frontend_menu();
}

void Runtime::install_event_callback(skirmish::MenuHandle) {
}

void Runtime::load_background(std::string_view name) {
    // A bitmap that cannot be read throws; the result only says whether the
    // backdrop changed, and the surface is rebuilt either way.
    std::ignore = load_named_background(std::string(name).c_str(), false, false, false);
    // The screen keeps the main menu's palette. The skirmish setup's picture
    // holds bright green in the palette entries it does not use, which lit
    // pixels, such as the focus marker's outline, would otherwise show.
    if (main_menu_palette_)
        show_background_in(*main_menu_palette_);
    rebuild_surface();
}

void Runtime::install_input_callback() {
    typed_key_hook_ = TypedKeyHook::skirmish_players;
}

void Runtime::set_input_enabled(int32_t enabled) {
    input_enabled_ = enabled != 0;
}

void Runtime::add_menu_flags(uint32_t flags) {
    menu_flags_ |= flags;
}

int16_t Runtime::widget_count() {
    return static_cast<int16_t>(std::min<std::size_t>(
        resources_.layout.gadgets.size(),
        static_cast<std::size_t>(std::numeric_limits<int16_t>::max())
    ));
}

void Runtime::set_widget_count(int16_t count) {
    if (count >= 0 && static_cast<std::size_t>(count) < resources_.layout.gadgets.size())
        resources_.layout.gadgets.resize(static_cast<std::size_t>(count));
}

void Runtime::link_slot_sprite(oa::ui::gui_layout::Gadget& gadget, std::string_view sequence) {
    widget_sprites_.erase(gadget.common.name);
    const auto* found = gaf_sequence(resources_.sprites, sequence);
    if (found == nullptr)
        return;
    widget_sprites_[gadget.common.name] = {renderer::SpriteArchive::screen, found->name};
    const auto stage = widget_gaf_frames_.find(gadget.common.name);
    const auto frame = stage == widget_gaf_frames_.end() ? 0U : stage->second;
    if (frame >= found->frames.size())
        return;
    gadget.common.width = static_cast<int16_t>(found->frames[frame].width);
    gadget.common.height = static_cast<int16_t>(found->frames[frame].height);
}

void Runtime::create_slot_widget(const skirmish::SlotWidget& source) {
    if (resources_.layout.gadgets.size() >= oa::ui::gui_layout::limit::gadgets)
        throw std::runtime_error("dynamic skirmish widgets exceed the GUI's gadget capacity");
    oa::ui::gui_layout::Gadget gadget;
    gadget.common.type = source.kind == skirmish::WidgetKind::button
                             ? oa::ui::gui_layout::GadgetType::button
                             : oa::ui::gui_layout::GadgetType::hot_surface;
    gadget.common.name = source.name;
    gadget.common.x = source.x;
    gadget.common.y = source.y;
    gadget.common.width = source.width;
    gadget.common.height = source.height;
    gadget.common.active = 1;
    gadget.common.attributes = static_cast<int32_t>(source.flags);
    gadget.common.runtime_help = source.tooltip;
    if (source.kind == skirmish::WidgetKind::button) {
        oa::ui::gui_layout::ButtonFields fields;
        fields.stages = static_cast<int8_t>(source.stages);
        gadget.fields = std::move(fields);
    }
    if (source.kind == skirmish::WidgetKind::button && !source.sprite_resource.empty())
        link_slot_sprite(gadget, source.sprite_resource);
    resources_.layout.gadgets.push_back(std::move(gadget));
}

std::string Runtime::translate_ui(std::string_view text) {
    const std::string source(text);
    const char* translated = game_translation(source.c_str());
    return translated != nullptr ? std::string(translated) : source;
}

void Runtime::set_text(
    skirmish::MenuHandle, std::string_view name, std::string_view text, int32_t
) {
    auto* gadget = widget(name);
    if (gadget == nullptr)
        return;
    // Current Map on the skirmish screen, and the name under a map list. The
    // stored name stays name@package; only the words on the gadget change.
    std::string_view written = text;
    std::string replaced;
    if ((name == "MapName" || name == "MAPNAME") && oa::data::map_pack::split_pack_map_name(text)) {
        replaced = pack_map_shown_name(text);
        if (!replaced.empty())
            written = replaced;
    }
    const std::string shown = translate_ui(written);
    if (auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget->fields))
        button->text = shown;
    else if (auto* label = std::get_if<oa::ui::gui_layout::LabelFields>(&gadget->fields))
        label->text = shown;
}

void Runtime::set_enabled(std::string_view name, int32_t enabled) {
    if (auto* gadget = widget(name))
        gadget->common.active = enabled != 0 ? 1 : 0;
}

void Runtime::set_button_stage(std::string_view name, uint8_t stage) {
    widget_gaf_frames_[std::string(name)] = stage;
    widget_text_stages_[std::string(name)] = stage;
}

void Runtime::select_difficulty_label(std::string_view label, int32_t value) {
    set_button_status(label, static_cast<int16_t>(value));
}

oa::data::match_rules::AiDifficultyNames Runtime::difficulty_names() const {
    const auto* profile = mod_profile();
    return profile != nullptr ? profile->rules.ai.difficulty_names
                              : oa::data::match_rules::AiDifficultyNames{};
}

void Runtime::set_button_status(std::string_view name, int16_t value) {
    auto* gadget = widget(name);
    auto* fields = gadget != nullptr
                       ? std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget->fields)
                       : nullptr;
    if (fields == nullptr)
        return;
    fields->status = value;
    const auto group = gadget->common.association;
    if (value == 0 || group == 0)
        return;
    for (auto& other : resources_.layout.gadgets) {
        auto* other_fields = std::get_if<oa::ui::gui_layout::ButtonFields>(&other.fields);
        if (&other != gadget && other_fields != nullptr && other.common.association == group)
            other_fields->status = 0;
    }
}

void Runtime::set_side_stage(std::string_view name, uint8_t stage) {
    const std::string key(name);
    std::size_t frames = 0;
    if (const auto sprite = widget_sprites_.find(key); sprite != widget_sprites_.end()) {
        if (const auto* sequence = gaf_sequence(resources_.sprites, sprite->second.sequence))
            frames = sequence->frames.size();
    }
    auto* gadget = widget(name);
    auto* fields = gadget != nullptr
                       ? std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget->fields)
                       : nullptr;
    if (static_cast<std::size_t>(stage) < frames) {
        widget_gaf_frames_[key] = stage;
        widget_text_stages_[key] = stage;
        if (fields != nullptr)
            fields->text.clear();
        widget_caption_only_.erase(key);
        return;
    }
    widget_gaf_frames_.erase(key);
    widget_text_stages_.erase(key);
    widget_caption_only_.insert(key);
    if (fields == nullptr)
        return;
    if (stage >= side_table_.count) {
        fields->text.clear();
        return;
    }
    const oa::Side* loaded = oa::data::defs::side_at(side_table_, stage);
    fields->text.assign(loaded->name, ::strnlen(loaded->name, sizeof loaded->name));
}

void Runtime::set_tooltip(std::string_view name, std::string_view text) {
    if (auto* gadget = widget(name))
        gadget->common.runtime_help = text;
}

void Runtime::set_image(std::string_view name, skirmish::Sprite sprite, uint16_t frame) {
    if (widget(name) == nullptr)
        return;
    widget_gaf_frames_[std::string(name)] = frame;
    widget_sprites_[std::string(name)] =
        sprite == skirmish::Sprite::player_colors
            ? renderer::SpriteOverride{renderer::SpriteArchive::global, "32xlogos"}
            : renderer::SpriteOverride{renderer::SpriteArchive::screen, "ally icons"};
}

void Runtime::set_image_frame(std::string_view name, uint16_t frame) {
    if (widget(name) != nullptr)
        widget_gaf_frames_[std::string(name)] = frame;
}

std::optional<uint16_t> Runtime::team_icon_frame_count() {
    const auto found = std::find_if(
        resources_.sprites.sequences.begin(),
        resources_.sprites.sequences.end(),
        [](const auto& sequence) { return sequence.name == "ally icons"; }
    );
    if (found == resources_.sprites.sequences.end())
        return std::nullopt;
    return static_cast<uint16_t>(
        std::min<std::size_t>(found->frames.size(), std::numeric_limits<uint16_t>::max())
    );
}

void Runtime::zero_team_icon_frame_origin(uint32_t) {
}

uint16_t Runtime::color_frame_count() {
    return logo_sequence_ != nullptr ? static_cast<uint16_t>(logo_sequence_->frames.size()) : 0;
}

void Runtime::load_logo_textures() {
    logo_sequence_ = nullptr;
    logo_textures_ = {};
    const auto bytes = read("textures/logos.gaf");
    if (!bytes)
        return;
    auto parsed = oa::formats::gaf::parse(*bytes);
    if (!parsed.ok())
        return;
    logo_textures_ = std::move(*parsed.archive);
    logo_sequence_ = gaf_sequence(logo_textures_, "32xlogos");
}

void Runtime::invalidate_menu() {
    rebuild_surface();
}

void Runtime::refresh_help_text() {
    if (widget("HELPTEXT") == nullptr)
        return;
    std::string help;
    if (hovered_ && *hovered_ < resources_.layout.gadgets.size())
        help = resources_.layout.gadgets[*hovered_].common.runtime_help;
    set_text(frontend_menu(), "HELPTEXT", translate_ui(help), 0);
}

std::string Runtime::selected_widget_name(const entry::Event&) {
    if (selected_ < 0 || static_cast<std::size_t>(selected_) >= resources_.layout.gadgets.size())
        return {};
    return resources_.layout.gadgets[static_cast<std::size_t>(selected_)].common.name;
}

uint32_t Runtime::button_result(skirmish::MenuHandle, skirmish::Button button) {
    return oa::ui::gui_input::button_result(input_menu(), skirmish::resource_name(button));
}

int32_t Runtime::event_button(skirmish::MenuHandle) {
    return event_button_;
}

void Runtime::capture_input() {
}

void Runtime::play_ui_sound(std::string_view name, uint32_t) {
    if (options_.mute)
        return;
    const auto selection =
        oa::audio::game_audio::select(audio_registry_, name, false, sound_playback_state());
    std::string error;
    if (selection.status == oa::audio::game_audio::SelectionStatus::selected &&
        !sound_found_missing(selection.sound->resource) && !audio_player_.play(selection, error))
        report_unplayed_sound(selection.sound->resource, error);
}

void Runtime::open_map_selection() {
    // With no map to choose, the modal shows its own message and stays shut.
    std::ignore = map_modal::open(map_modal_, skirmish_settings_, *this);
}

int32_t Runtime::map_count() {
    return static_cast<int32_t>(eligible_map_names_.size());
}

std::vector<std::string> Runtime::copy_map_names() {
    return eligible_map_names_;
}

map_modal::MenuHandle Runtime::load_modal(std::string_view resource, uint32_t flags) {
    if (resource != "SELMAP.GUI" || flags != map_modal::modal_flags)
        throw std::runtime_error("unexpected map modal setup");
    modal_parent_surface_ = surface_;
    load(Screen::map_selection);
    return {kFrontendMenuHandle};
}

void Runtime::install_map_event_callback(map_modal::MenuHandle) {
}

void Runtime::bind_map_names(std::span<const std::string> names) {
    bound_map_names_.assign(names.begin(), names.end());
    label_bound_maps();
    fill_frontend_list("MAPNAMES", bound_map_names_.size());
}

void Runtime::install_map_selection_callback() {
}

void Runtime::set_selected_map_index(int16_t index) {
    modal_map_index_ = index;
    if (index >= 0)
        select_frontend_list_row("MAPNAMES", static_cast<std::size_t>(index));
}

int16_t Runtime::selected_map_index() {
    return modal_map_index_;
}

uint32_t Runtime::button_result(map_modal::MenuHandle, map_modal::Button button) {
    return oa::ui::gui_input::button_result(input_menu(), map_modal::resource_name(button));
}

void Runtime::set_parent_map_name(map_modal::MenuHandle, std::string_view text) {
    pending_parent_map_name_ = text;
}

bool Runtime::has_map_name_widget() {
    return widget("MAPNAME") != nullptr;
}

std::string Runtime::map_display_name() {
    if (const PackMap* installed = pack_map(selected_map_name_runtime_); installed != nullptr) {
        if (pack_map_title(*installed).empty() && selected_map_metadata_ &&
            !selected_map_metadata_->mission_name.empty() &&
            !oa::data::map_pack::split_pack_map_name(selected_map_metadata_->mission_name))
            return selected_map_metadata_->mission_name;
        const std::string shown = pack_map_shown_name(selected_map_name_runtime_);
        if (!shown.empty())
            return shown;
    }
    return selected_map_metadata_ ? selected_map_metadata_->mission_name : std::string{};
}

std::string Runtime::map_memory_requirement_text() {
    return selected_map_metadata_ ? selected_map_metadata_->memory_requirement : std::string{};
}

std::string Runtime::permitted_player_counts_text() {
    return selected_map_metadata_ ? selected_map_metadata_->permitted_player_counts : std::string{};
}

std::string Runtime::map_description() {
    if (!selected_map_metadata_)
        return {};
    const std::string& own = selected_map_metadata_->mission_description;
    std::string lowered = own;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    const char* translated = game_translation(lowered.c_str());
    return translated != nullptr && !tdf_names_equal(translated, lowered) ? std::string(translated)
                                                                          : own;
}

std::string Runtime::terrain_resource_path() {
    return "maps/" + selected_map_name_runtime_ + ".tnt";
}

void Runtime::set_modal_text(std::string_view name, std::string_view text, int32_t argument) {
    set_text(frontend_menu(), name, text, argument);
}

void Runtime::destroy_map_picture_state(MapPictureState* state) noexcept {
    delete state;
}

Runtime::MapPictureState& Runtime::map_picture_state() {
    if (!map_picture_)
        map_picture_.reset(new MapPictureState());
    return *map_picture_;
}

map_modal::PictureHandle Runtime::picture() {
    return map_picture_state().handle;
}

void Runtime::set_picture(map_modal::PictureHandle picture_value) {
    map_picture_state().handle = picture_value;
}

void Runtime::release_picture(map_modal::PictureHandle) {
    auto& picture = map_picture_state();
    picture.rgb.clear();
    picture.width = picture.height = 0;
    picture.source_width = picture.source_height = 0;
    picture.destination_x = picture.destination_y = 0;
    picture.destination_width = picture.destination_height = 0;
}

map_modal::LoadedPicture Runtime::load_picture(std::string_view terrain_path) {
    // The highlighted map's terrain is already parsed for the match; its
    // minimap comes from that parse.
    const std::string held = "maps/" + selected_map_name_runtime_ + ".tnt";
    if (!selected_tnt_ || terrain_path != held) {
        const auto bytes = assets_.read(terrain_path).bytes;
        auto parsed = oa::formats::tnt::parse(bytes);
        if (!parsed.ok())
            throw std::runtime_error("cannot parse map terrain: " + parsed.error->message);
        selected_tnt_ = std::move(*parsed.map);
    }
    auto& picture = map_picture_state();
    if (selected_tnt_->minimap) {
        const auto palette_data = assets_.read("palettes/palette.pal").bytes;
        if (palette_data.size() != oa::PaletteBytes{}.size())
            throw std::runtime_error("game palette has invalid size");
        std::copy_n(palette_data.begin(), picture.clear_rgb.size(), picture.clear_rgb.begin());
        picture.rgb.resize(selected_tnt_->minimap->palette_indices.size() * 3U);
        picture.width = selected_tnt_->minimap->width;
        picture.height = selected_tnt_->minimap->height;
        picture.source_width = picture.width;
        picture.source_height = picture.height;
        for (std::size_t i = 0; i < selected_tnt_->minimap->palette_indices.size(); ++i) {
            const auto source =
                static_cast<std::size_t>(selected_tnt_->minimap->palette_indices[i]) * 4U;
            std::copy_n(
                palette_data.begin() + static_cast<std::ptrdiff_t>(source),
                3,
                picture.rgb.begin() + static_cast<std::ptrdiff_t>(i * 3U)
            );
        }
    }
    return {
        {++picture.next_handle},
        static_cast<int32_t>(selected_tnt_->attribute_width),
        static_cast<int32_t>(selected_tnt_->attribute_height)
    };
}

map_modal::PictureSize Runtime::picture_size() {
    if (const auto* gadget = widget("MAPPIC"))
        return {gadget->common.width, gadget->common.height};
    return {};
}

void Runtime::fit_picture(
    map_modal::PictureHandle,
    int32_t widget_width,
    int32_t widget_height,
    int32_t world_width,
    int32_t world_height
) {
    const auto effective_width = world_width - 32;
    const auto effective_height = world_height - 128;
    if (effective_width <= 0 || effective_height <= 0 || widget_width <= 0 || widget_height <= 0)
        throw std::runtime_error("map preview dimensions are invalid");
    auto& picture = map_picture_state();
    if (effective_width < effective_height) {
        picture.destination_width = static_cast<int32_t>(
            static_cast<int64_t>(effective_width) * widget_width / effective_height
        );
        picture.destination_height = widget_height;
        picture.destination_x = (widget_width - picture.destination_width) / 2;
        picture.destination_y = 0;
        picture.source_width = picture.width * static_cast<std::size_t>(effective_width) /
                               static_cast<std::size_t>(effective_height);
    } else {
        picture.destination_width = widget_width;
        picture.destination_height = static_cast<int32_t>(
            static_cast<int64_t>(effective_height) * widget_height / effective_width
        );
        picture.destination_x = 0;
        picture.destination_y = (widget_height - picture.destination_height) / 2;
        picture.source_height = picture.height * static_cast<std::size_t>(effective_height) /
                                static_cast<std::size_t>(effective_width);
    }
}

int32_t Runtime::select_map(std::string_view name) {
    // A map that cannot be loaded clears the selection. Unless it is a pack
    // map refused for a reason of its own, map_refusal then says its
    // terrain does not exist, as the game's own message says at Start.
    const auto unloadable = [&] {
        selected_map_metadata_.reset();
        selected_start_markers_.clear();
        if (!pack_map_refusal(name))
            unloadable_map_name_ = name;
        return 0;
    };
    // A pack map's files are mounted before they are read, and a map that
    // does not fit is not selected; any other map shows no pack map's files.
    if (pack_map(name) != nullptr) {
        if (!prepare_pack_map(name, nullptr))
            return unloadable();
    } else {
        release_pack_map();
    }
    const auto base = "maps/" + std::string(name);
    const auto tnt_data = read(base + ".tnt");
    const auto ota_data = read(base + ".ota");
    if (!tnt_data || !ota_data)
        return unloadable();
    const std::string_view ota_text(
        reinterpret_cast<const char*>(ota_data->data()), ota_data->size()
    );
    auto parsed = oa::formats::ota::parse(ota_text);
    if (!parsed.ok())
        throw std::runtime_error("cannot parse selected map metadata: " + parsed.error->message);
    if (oa::formats::ota::select_multiplayer_schema(*parsed.metadata, 2) == nullptr)
        return unloadable();
    auto terrain = oa::formats::tnt::parse(*tnt_data);
    if (!terrain.ok())
        throw std::runtime_error("cannot parse selected map terrain: " + terrain.error->message);
    selected_map_metadata_ = std::move(*parsed.metadata);
    oa::formats::tdf::OwnedDocument scenario_document;
    oa::formats::tdf::ParseError scenario_error{};
    if (!scenario_document.parse(ota_text, &scenario_error))
        throw std::runtime_error(
            "cannot parse selected scenario definitions: " +
            oa::formats::tdf::describe(scenario_error)
        );
    selected_ota_document_ = std::move(scenario_document);
    selected_tnt_ = std::move(*terrain.map);
    selected_map_name_runtime_ = name;
    selected_start_markers_.clear();
    if (unloadable_map_name_ == name)
        unloadable_map_name_.clear();
    return 1;
}

int32_t Runtime::map_player_capacity() {
    if (!selected_map_metadata_)
        return 0;
    const auto* schema = oa::formats::ota::select_multiplayer_schema(
        *selected_map_metadata_, static_cast<int32_t>(state_.player_count)
    );
    if (schema == nullptr)
        return 0;
    selected_start_markers_.clear();
    selected_start_markers_.reserve(schema->start_positions.size());
    for (const auto& position : schema->start_positions)
        selected_start_markers_.push_back({1, position.index, position.x, position.z});
    return oa::sim::unit_spawn::count_start_positions(selected_start_markers_);
}

std::optional<std::vector<uint8_t>> Runtime::read(std::string_view path) {
    try {
        return assets_.read(path).bytes;
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).starts_with("asset not found: "))
            return std::nullopt;
        throw;
    }
}

namespace {

const oa::formats::tdf::Block*
global_header(const std::optional<oa::formats::tdf::OwnedDocument>& document) {
    if (!document)
        return nullptr;
    return oa::formats::tdf::find_child(document->root(), "GlobalHeader");
}

std::optional<std::string> tdf_text(const oa::formats::tdf::Block* section, std::string_view key) {
    const char* value = oa::formats::tdf::find_value(section, std::string(key).c_str());
    if (value == nullptr)
        return std::nullopt;
    if (std::strlen(value) >= 256)
        throw std::runtime_error("OTA scenario text exceeds 255 bytes");
    return std::string(value);
}

} // namespace

int32_t Runtime::integer(std::string_view key, int32_t fallback) {
    return oa::formats::tdf::get_int(
        global_header(selected_ota_document_), std::string(key).c_str(), fallback
    );
}

std::optional<std::string> Runtime::text(std::string_view key) {
    return tdf_text(global_header(selected_ota_document_), key);
}

const oa::formats::tdf::Block* Runtime::session_schema_section() const {
    if (session_schema_.empty())
        return nullptr;
    return oa::formats::tdf::find_child(
        global_header(selected_ota_document_), session_schema_.c_str()
    );
}

int32_t Runtime::schema_integer(std::string_view key, int32_t fallback) {
    return oa::formats::tdf::get_int(session_schema_section(), std::string(key).c_str(), fallback);
}

std::optional<std::string> Runtime::schema_text(std::string_view key) {
    return tdf_text(session_schema_section(), key);
}

uint16_t Runtime::commander_type_for_side(uint8_t side) {
    // A side this match's side data does not have places no commander. The
    // start then fails with its own message, instead of dying on the lookup.
    if (side >= side_table_.count)
        return 0;
    const auto type = oa::sim::unit_spawn::find_type_index(
        spawn_type_names_, oa::data::defs::side_at(side_table_, side)->commander
    );
    if (type == 0)
        throw std::runtime_error("side commander is absent from unit catalog");
    return type;
}

void Runtime::report_missing_start_position(int32_t index) {
    throw std::runtime_error("selected map lacks start position " + std::to_string(index));
}

namespace {

bool blip_named(const oa::present::GafSequence& sequence, const char* name) noexcept {
    std::size_t stored_length = 0;
    while (stored_length < oa::present::gaf_name_size && sequence.name[stored_length] != '\0')
        ++stored_length;
    const std::string_view stored(sequence.name, stored_length);
    const std::string_view wanted(name);
    if (stored.size() != wanted.size())
        return false;
    for (std::size_t at = 0; at < stored.size(); ++at) {
        const char left = stored[at] >= 'A' && stored[at] <= 'Z'
                              ? static_cast<char>(stored[at] - 'A' + 'a')
                              : stored[at];
        const char right = wanted[at] >= 'A' && wanted[at] <= 'Z'
                               ? static_cast<char>(wanted[at] - 'A' + 'a')
                               : wanted[at];
        if (left != right)
            return false;
    }
    return true;
}

/// One radlogo or nuclogo frame whose row-compressed bytes sit in the radar archive.
struct BlipFrame {
    uint8_t* data{};
    std::size_t size{};
    int width{};
    int height{};
    int key{-1};
    int player{};
    const char* sequence{};
    bool ready{};
};

} // namespace

void Runtime::apply_colour_vision() {
    // A paint that reaches back into the match must not paint again.
    struct Reentry {
        bool& held;

        ~Reentry() { held = false; }
    };

    static bool held = false;
    if (held)
        return;
    held = true;
    const Reentry reentry{held};

    namespace vision = oa::present::colour_vision;
    const auto mode_entry = preference_values_.find(std::string(vision::kColourVisionKey));
    const auto shape_entry = preference_values_.find(std::string(vision::kShapeCuesKey));
    const std::string_view mode_text = mode_entry == preference_values_.end()
                                           ? std::string_view{}
                                           : std::string_view{mode_entry->second};
    const std::string_view shape_text = shape_entry == preference_values_.end()
                                            ? std::string_view{}
                                            : std::string_view{shape_entry->second};
    vision::set_active(vision::parse_mode(mode_text), vision::shape_cues_preference(shape_text));
    // Health, build sites and the other marks follow the mode even when no
    // team picture is recoloured. Off, before the table exists, this leaves
    // the load as late as it is today.
    refresh_mark_colours();

    const bool model_painted =
        match_models_ != nullptr && match_models_->library.textures.team_colours_applied;
    // Off, with nothing recoloured: every frame stays as it was loaded.
    if (vision::active_mode() == vision::Mode::off && !vision::holds_originals() && !model_painted)
        return;

    oa::PaletteBytes palette{};
    bool have_palette = false;
    const auto adopt = [&](std::span<const uint8_t> bytes) {
        if (bytes.size() < palette.size())
            return;
        bool any = false;
        for (std::size_t at = 0; at < palette.size(); ++at) {
            palette[at] = bytes[at];
            any = any || bytes[at] != 0;
        }
        have_palette = any;
    };
    if (match_ != nullptr) {
        bool any = false;
        for (const uint8_t byte : match_palette_)
            any = any || byte != 0;
        if (any)
            adopt(match_palette_);
    }
    if (!have_palette && resources_.game_palette.has_value())
        adopt(*resources_.game_palette);
    if (!have_palette) {
        try {
            const auto loaded = assets_.read("palettes/palette.pal");
            adopt(loaded.bytes);
        } catch (const std::exception&) {
        }
    }
    const std::span<const uint8_t> palette_bytes =
        have_palette ? std::span<const uint8_t>(palette.data(), palette.size())
                     : std::span<const uint8_t>{};
    vision::match_player_dots(palette_bytes);
    const bool paint = vision::active_mode() != vision::Mode::off && have_palette;

    std::vector<BlipFrame> blips;
    const uint8_t* const radar_bytes = radar_state_.fx.bytes.data();
    const std::size_t radar_size = radar_state_.fx.bytes.size();
    if (radar_bytes != nullptr) {
        for (oa::present::GafSequence& sequence : radar_state_.fx.sequences) {
            const char* sequence_name = nullptr;
            if (blip_named(sequence, "radlogo"))
                sequence_name = "radlogo";
            else if (blip_named(sequence, "nuclogo"))
                sequence_name = "nuclogo";
            if (sequence_name == nullptr || sequence.frame_count != vision::kPlayerCount ||
                sequence.frames == nullptr)
                continue;
            for (int player = 0; player < vision::kPlayerCount; ++player) {
                Sprite* const sprite = sequence.frames[player].frame;
                if (sprite == nullptr || sprite->data == nullptr || sprite->child_count != 0 ||
                    sprite->encoding != OA_SPRITE_ROW_RLE || sprite->width == 0 ||
                    sprite->height == 0)
                    continue;
                auto* const data = static_cast<uint8_t*>(sprite->data);
                const auto begin = reinterpret_cast<uintptr_t>(radar_bytes);
                const auto at = reinterpret_cast<uintptr_t>(data);
                if (at < begin || at - begin >= radar_size)
                    continue;
                const auto available = radar_size - (at - begin);
                const auto size = vision::row_rle_size(
                    std::span<const uint8_t>(data, available),
                    static_cast<int>(sprite->width),
                    static_cast<int>(sprite->height)
                );
                if (size == 0)
                    continue;
                const vision::FramePaint prepared = vision::prepare_original(
                    "radar", sequence_name, player, std::span<uint8_t>(data, size), paint
                );
                BlipFrame frame;
                frame.data = data;
                frame.size = size;
                frame.width = static_cast<int>(sprite->width);
                frame.height = static_cast<int>(sprite->height);
                frame.key = static_cast<int>(sprite->key);
                frame.player = player;
                frame.sequence = sequence_name;
                frame.ready = prepared == vision::FramePaint::ready;
                blips.push_back(frame);
            }
        }
    }
    std::array<int, vision::kPlayerCount> unit_bodies{};
    std::array<int, vision::kPlayerCount> weapon_bodies{};
    unit_bodies.fill(-1);
    weapon_bodies.fill(-1);
    if (paint && !blips.empty()) {
        std::vector<std::vector<uint8_t>> decoded(blips.size());
        std::array<vision::RawFrame, vision::kPlayerCount> unit_blips{};
        std::array<vision::RawFrame, vision::kPlayerCount> weapon_blips{};
        bool any = false;
        for (std::size_t frame_at = 0; frame_at < blips.size(); ++frame_at) {
            const BlipFrame& frame = blips[frame_at];
            if (!frame.ready)
                continue;
            if (!vision::decode_row_rle(
                    std::span<const uint8_t>(frame.data, frame.size),
                    frame.width,
                    frame.height,
                    frame.key,
                    decoded[frame_at]
                ))
                continue;
            const vision::RawFrame raw{
                decoded[frame_at].data(), decoded[frame_at].size(), frame.key
            };
            auto& destination =
                std::string_view(frame.sequence) == "radlogo" ? unit_blips : weapon_blips;
            destination[static_cast<std::size_t>(frame.player)] = raw;
            any = true;
        }
        if (any) {
            unit_bodies = vision::blip_bodies(unit_blips, palette_bytes);
            weapon_bodies = vision::blip_bodies(weapon_blips, palette_bytes);
            vision::BlipPins pins{};
            for (int player = 0; player < vision::kPlayerCount; ++player) {
                pins[static_cast<std::size_t>(player)] = {
                    unit_bodies[static_cast<std::size_t>(player)],
                    weapon_bodies[static_cast<std::size_t>(player)]
                };
            }
            vision::remember_blip_pins(pins);
        }
    }

    if (match_models_ != nullptr)
        oa::present::model::recolour_team_textures(match_models_->library.textures, palette_bytes);
    vision::paint_team_archive(logo_textures_, palette_bytes, "logos");
    vision::paint_team_archive(resources_.global_sprites, palette_bytes, "menu");
    if (paint) {
        // The blip's own map sends its body to the anchor. The outline, which
        // the frames share and which is one of white's team shades, stays.
        const vision::TeamMaps unit_maps =
            vision::blip_maps(vision::active_mode(), palette_bytes, unit_bodies);
        const vision::TeamMaps weapon_maps =
            vision::blip_maps(vision::active_mode(), palette_bytes, weapon_bodies);
        for (const BlipFrame& frame : blips) {
            if (!frame.ready)
                continue;
            const bool unit_blip = std::string_view(frame.sequence) == "radlogo";
            const vision::TeamMaps& maps = unit_blip ? unit_maps : weapon_maps;
            const bool remapped = vision::remap_row_rle(
                std::span<uint8_t>(frame.data, frame.size),
                frame.width,
                frame.height,
                maps[static_cast<std::size_t>(frame.player)],
                frame.key
            );
            // A stream the walk cannot cover stays the original frame.
            if (!remapped) {
                vision::prepare_original(
                    "radar",
                    frame.sequence,
                    frame.player,
                    std::span<uint8_t>(frame.data, frame.size),
                    false
                );
            }
        }
    }
    drop_full_team_frames();
    drop_live_team_frames();
}

} // namespace oa::app
